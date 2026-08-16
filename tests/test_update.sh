#!/usr/bin/env bash
#
# Exercises update.sh against a throwaway origin, a throwaway checkout and a stub
# systemd unit. Nothing here touches /etc/cielotrack or the live receiver.
#
# Needs sudo, because the thing under test restarts a service and writes a unit
# file. Run from the repository root:
#
#     sudo -v && bash tests/test_update.sh
#
# It generates a throwaway signing key of its own rather than borrowing the real
# release key. That keeps the key that can install code on every receiver out of a
# test that runs unattended, and it is what lets this run on a CI machine that has no
# keys at all.
set -uo pipefail

BASE="$(mktemp -d)"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ORIGIN="$BASE/origin.git"
WORK="$BASE/work"
INSTALL="$BASE/install"
CONFIG="$BASE/config"
UNIT="$BASE/stub.service"
KEY="$BASE/signing_key"
ssh-keygen -q -t ed25519 -N '' -C "cielotrack update self-test" -f "$KEY"

pass=0; fail=0
check() {
  if [[ "$2" == "$3" ]]; then echo "  PASS  $1"; pass=$((pass+1));
  else echo "  FAIL  $1 — expected [$3], got [$2]"; fail=$((fail+1)); fi
}
contains() {
  if grep -qF "$3" <<<"$2"; then echo "  PASS  $1"; pass=$((pass+1));
  else echo "  FAIL  $1 — output lacked [$3]"; fail=$((fail+1)); fi
}
lacks() {
  if grep -qF "$3" <<<"$2"; then echo "  FAIL  $1 — output contained [$3]"; fail=$((fail+1));
  else echo "  PASS  $1"; pass=$((pass+1)); fi
}

mkdir -p "$CONFIG"
git init --quiet --bare "$ORIGIN"
git init --quiet "$WORK"
cd "$WORK" || exit 1
git config user.name Test; git config user.email drone@station.local
git config gpg.format ssh; git config user.signingkey "$KEY"
cp "$SRC/update.sh" .; chmod +x update.sh
echo "v1 payload" > payload.txt
cat > stubunit.service <<'EOF'
[Service]
ExecStart=/bin/sleep infinity
EOF
git add -A; git commit --quiet -m "v1"
git tag -s v1.0.0 -m "release 1.0.0"
git remote add origin "$ORIGIN"; git push --quiet origin HEAD:main --tags

git clone --quiet "$ORIGIN" "$INSTALL"
cd "$INSTALL" || exit 1; git checkout --quiet v1.0.0

# The pinned key, as provision.sh would have written it.
echo "drone@station.local $(cut -d' ' -f1-2 "$KEY.pub")" > "$CONFIG/allowed_signers"

ENVVARS=(CIELOTRACK_CONFIG_DIR="$CONFIG"
         CIELOTRACK_SERVICE="cielotrack-selftest"
         CIELOTRACK_UNIT_PATH="$UNIT"
         CIELOTRACK_STATUS_FILE="$INSTALL/status.json"
         CIELOTRACK_HEALTH_TIMEOUT=40)

echo "already current"
out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" ./update.sh 2>&1)"
contains "reports it is already on the latest release" "$out" "already on"

echo
echo "a newer signed release"
cd "$WORK" || exit 1
echo "v2 payload" > payload.txt
git add -A; git commit --quiet -m "v2"
git tag -s v1.1.0 -m "release 1.1.0"
git push --quiet origin HEAD:main --tags

# A stub unit that stays active, standing in for the receiver.
sudo tee /etc/systemd/system/cielotrack-selftest.service >/dev/null <<EOF
[Unit]
Description=CieloTrack update self-test stub
[Service]
Type=simple
ExecStart=/bin/bash -c 'while true; do sleep 5; done'
EOF
sudo systemctl daemon-reload

out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" bash -c '
  ( while true; do sleep 2; echo "{\"radios\":{\"problems\":[]}}" > '"$INSTALL"'/status.json; done ) &
  writer=$!
  ./update.sh 2>&1
  kill $writer 2>/dev/null')"
contains "verifies the signature" "$out" "signature on v1.1.0 verified"
contains "updates and confirms health" "$out" "verified healthy"
check "checkout moved to the new release" "$(cd "$INSTALL" && git describe --tags)" "v1.1.0"
check "payload actually updated" "$(cat "$INSTALL/payload.txt")" "v2 payload"

echo
echo "a release that breaks the receiver rolls back"
cd "$WORK" || exit 1
echo "v3 payload" > payload.txt
git add -A; git commit --quiet -m "v3"
git tag -s v1.2.0 -m "release 1.2.0"
git push --quiet origin HEAD:main --tags

# This time the status file reports the fault the fleet page would show.
out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" bash -c '
  ( while true; do sleep 2; echo "{\"radios\":{\"problems\":[\"detections_not_queued\"]}}" > '"$INSTALL"'/status.json; done ) &
  writer=$!
  ./update.sh 2>&1
  kill $writer 2>/dev/null')"
contains "detects the fault" "$out" "detections_not_queued"
contains "rolls back" "$out" "rolling back"
check "checkout restored to the previous release" "$(cd "$INSTALL" && git describe --tags)" "v1.1.0"
check "payload restored" "$(cat "$INSTALL/payload.txt")" "v2 payload"

echo
echo "a rolled-back release is not retried forever"
# The rollback test above left v1.2.0 installed-and-rejected. Before this existed the
# failed tag was still the newest, so the receiver reinstalled it, failed the same
# check and rolled back again — nightly, indefinitely.
out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" ./update.sh --check 2>&1)"
contains "the failed release is skipped" "$out" "skipping v1.2.0"
lacks "and is not offered as an update" "$out" "update available"

# A newer release must still be tried, or a fix is blocked by the release it repairs.
cd "$WORK" || exit 1
echo "fixed payload" > payload.txt
git add -A; git commit --quiet -m "the fix"
git tag -s v1.2.1 -m "fixes the bad release"
git push --quiet origin HEAD:main --tags
out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" ./update.sh --check 2>&1)"
contains "a newer release is still offered" "$out" "v1.2.1"

# Clearing the record is how an operator says "try it again".
sudo rm -f "$CONFIG/rejected"
out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" ./update.sh --check 2>&1)"
lacks "clearing the record makes it retryable" "$out" "skipping v1.2.0"

echo
echo "release channels"
# A prerelease must not reach a stable receiver. Before channels existed the tag glob
# matched v1.3.0-rc1 as readily as v1.3.0, so tagging a candidate shipped it to
# everybody — the exact thing a canary is supposed to prevent.
cd "$WORK" || exit 1
echo "rc payload" > payload.txt
git add -A; git commit --quiet -m "rc"
git tag -s v1.3.0-rc1 -m "release candidate"
git push --quiet origin HEAD:main --tags

echo stable > "$CONFIG/channel"
out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" ./update.sh --check 2>&1)"
lacks "stable does not choose a prerelease" "$out" "v1.3.0-rc1"

echo canary > "$CONFIG/channel"
out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" ./update.sh --check 2>&1)"
contains "canary sees the prerelease" "$out" "v1.3.0-rc1"

# Once the real release exists the canary must move to it, not sit on the candidate.
# git and sort -V both rank v1.3.0-rc1 above v1.3.0 unless told otherwise, so this is
# the assertion that catches a canary stranded on a release candidate forever.
cd "$WORK" || exit 1
git tag -s v1.3.0 -m "the real thing"
git push --quiet origin HEAD:main --tags
out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" ./update.sh --check 2>&1)"
contains "canary prefers the final release over its candidate" "$out" "latest : v1.3.0"

echo "nonsense" > "$CONFIG/channel"
out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" ./update.sh --check 2>&1)"
contains "an unreadable channel falls back to stable" "$out" "using stable"

rm -f "$CONFIG/channel"
out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" ./update.sh --check 2>&1)"
contains "no channel file means stable" "$out" "channel: stable"

echo
echo "an unsigned release is refused"
cd "$WORK" || exit 1
echo "v4 payload" > payload.txt
git add -A; git commit --quiet -m "v4"
git tag -a v1.4.0 -m "unsigned release"      # deliberately not signed
git push --quiet origin HEAD:main --tags
out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" ./update.sh 2>&1)"
contains "refuses an unsigned tag" "$out" "did not verify"
check "stayed on the last good release" "$(cd "$INSTALL" && git describe --tags)" "v1.1.0"

echo
echo "opt-out is honoured"
sudo touch "$CONFIG/no-auto-update"
out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" ./update.sh 2>&1)"
contains "does nothing when opted out" "$out" "disabled by"
sudo rm -f "$CONFIG/no-auto-update"

echo
echo "local modifications are left alone"
# root owns the checkout after an update has run, as it would in a real /opt install.
sudo tee -a "$INSTALL/payload.txt" >/dev/null <<<"operator debugging"
out="$(cd "$INSTALL" && sudo "${ENVVARS[@]}" ./update.sh 2>&1)"
contains "refuses to touch a dirty checkout" "$out" "local modifications"

sudo systemctl stop cielotrack-selftest 2>/dev/null
sudo rm -f /etc/systemd/system/cielotrack-selftest.service
sudo systemctl daemon-reload
sudo rm -rf "$BASE"
echo
echo "$pass passed, $fail failed"
exit $(( fail > 0 ))
