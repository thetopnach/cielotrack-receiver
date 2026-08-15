#!/usr/bin/env bash
#
# Updates the receiver to the newest signed release, and puts it back if that goes
# badly.
#
# The argument for automating this at all: a receiver that is working looks exactly
# like a receiver that is broken in the ways that matter, so operators do not go
# looking, so fixes never get installed. The argument against is that a bad release
# then reaches everyone at once, unattended, on hardware nobody is watching. This
# script exists to make the second argument survivable rather than to dismiss it:
#
#   - it only moves to signed release tags, never to whatever is on main;
#   - it verifies against a key pinned at install time, not one read from the repo it
#     is about to trust;
#   - it checks the receiver still works afterwards, using the same fault signals the
#     fleet page uses, and rolls code *and* unit back if it does not;
#   - it runs in the small hours, when the sky is empty, so a failed update costs the
#     least data it can.
#
# Run by cielotrack-update.timer. Safe to run by hand at any time:
#
#   sudo /opt/cielotrack-receiver/update.sh            # update if there is one
#   sudo /opt/cielotrack-receiver/update.sh --check    # report only, change nothing
#
set -euo pipefail

# Overridable so the update path can be exercised end to end against a throwaway
# checkout and a stub unit, rather than only ever being tested in production at 02:00.
INSTALL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE="${CIELOTRACK_SERVICE:-cielotrack-receiver}"
UNIT_PATH="${CIELOTRACK_UNIT_PATH:-/etc/systemd/system/${SERVICE}.service}"
CONFIG_DIR="${CIELOTRACK_CONFIG_DIR:-/etc/cielotrack}"
PINNED_SIGNERS="${CONFIG_DIR}/allowed_signers"
OPT_OUT="${CONFIG_DIR}/no-auto-update"
STATUS_FILE="${CIELOTRACK_STATUS_FILE:-${INSTALL_DIR}/status.json}"
# Two heartbeats plus warmup. Long enough that a healthy receiver has certainly
# reported, short enough that a broken one is rolled back inside the quiet window.
HEALTH_TIMEOUT="${CIELOTRACK_HEALTH_TIMEOUT:-180}"
CHECK_ONLY=0
[[ "${1:-}" == "--check" ]] && CHECK_ONLY=1

log() { echo "[cielotrack-update] $*"; }
fail() { echo "[cielotrack-update] ERROR: $*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || fail "must run as root (it restarts the service and writes to /etc)"

if [[ -e "$OPT_OUT" && $CHECK_ONLY -eq 0 ]]; then
    log "automatic updates disabled by $OPT_OUT — nothing done"
    exit 0
fi

cd "$INSTALL_DIR"
git rev-parse --git-dir >/dev/null 2>&1 || fail "$INSTALL_DIR is not a git checkout; update by hand"

# Refuse to touch a checkout with local edits. Someone is mid-debug, and silently
# stashing their work in the middle of the night is not ours to do.
if [[ -n "$(git status --porcelain -- ':!status.json' ':!*.db*' ':!.env' 2>/dev/null)" ]]; then
    log "local modifications present — leaving this checkout alone"
    git status --short -- ':!status.json' ':!*.db*' ':!.env' | sed 's/^/    /'
    exit 0
fi

CURRENT_COMMIT="$(git rev-parse HEAD)"
CURRENT_DESC="$(git describe --tags --always 2>/dev/null || echo "$CURRENT_COMMIT")"

log "fetching releases"
git fetch --quiet --tags --force origin || fail "could not reach the release repository"

# Highest version-sorted tag of the form vN.N.N. Release candidates and anything else
# are ignored, so tagging an experiment cannot roll it out to the fleet.
LATEST="$(git tag --list 'v[0-9]*.[0-9]*.[0-9]*' | sort -V | tail -1)"
[[ -n "$LATEST" ]] || { log "no release tags published yet — nothing to do"; exit 0; }

LATEST_COMMIT="$(git rev-parse "${LATEST}^{commit}")"
if [[ "$LATEST_COMMIT" == "$CURRENT_COMMIT" ]]; then
    log "already on $CURRENT_DESC (latest release $LATEST)"
    exit 0
fi

log "current: $CURRENT_DESC"
log "latest : $LATEST"

# --- signature ---------------------------------------------------------------------
# Verified against the key pinned at install time. Reading the allowed signers out of
# the repository being updated would make the signature decorative: anyone able to
# publish a tag could publish the key that vouches for it.
[[ -r "$PINNED_SIGNERS" ]] || fail "no pinned signing key at $PINNED_SIGNERS — run provision.sh, or update by hand"

if ! git -c gpg.ssh.allowedSignersFile="$PINNED_SIGNERS" tag --verify "$LATEST" >/dev/null 2>&1; then
    fail "signature on $LATEST did not verify against $PINNED_SIGNERS — refusing to update"
fi
log "signature on $LATEST verified"

if [[ $CHECK_ONLY -eq 1 ]]; then
    log "update available: $CURRENT_DESC -> $LATEST (check only, nothing changed)"
    exit 0
fi

# --- rollback point ----------------------------------------------------------------
ROLLBACK_UNIT=""
if [[ -f "$UNIT_PATH" ]]; then
    ROLLBACK_UNIT="$(mktemp)"
    cp "$UNIT_PATH" "$ROLLBACK_UNIT"
fi

restore() {
    log "rolling back to $CURRENT_DESC"
    git checkout --quiet --force "$CURRENT_COMMIT" || log "WARNING: could not restore the previous commit"
    if [[ -n "$ROLLBACK_UNIT" ]]; then
        cp "$ROLLBACK_UNIT" "$UNIT_PATH"
        systemctl daemon-reload
    fi
    systemctl restart "$SERVICE" || log "WARNING: service did not restart after rollback"
    log "rolled back; this receiver stays on $CURRENT_DESC until the next release"
}

cleanup() { [[ -n "$ROLLBACK_UNIT" ]] && rm -f "$ROLLBACK_UNIT"; }
trap cleanup EXIT

# --- apply -------------------------------------------------------------------------
log "checking out $LATEST"
git checkout --quiet --force "$LATEST" || fail "could not check out $LATEST"

if [[ -f requirements.txt ]] && ! git diff --quiet "$CURRENT_COMMIT" HEAD -- requirements.txt; then
    log "dependencies changed, installing"
    if ! pip3 install -r requirements.txt --break-system-packages --quiet; then
        log "dependency install failed"
        restore
        exit 1
    fi
fi

# The unit lives in /etc, so a checkout alone never updates it — which is how a fix to
# the sandboxing or the capability set silently fails to reach anyone already running.
if [[ -f "${SERVICE}.service" ]]; then
    if ! cmp -s "${SERVICE}.service" "$UNIT_PATH"; then
        log "service unit changed:"
        diff -u "$UNIT_PATH" "${SERVICE}.service" 2>/dev/null | sed 's/^/    /' || true
        cp "${SERVICE}.service" "$UNIT_PATH"
        systemctl daemon-reload
    fi
fi

log "restarting $SERVICE"
RESTART_AT="$(date +%s)"
if ! systemctl restart "$SERVICE"; then
    log "service failed to restart"
    restore
    exit 1
fi

# --- did it actually work ----------------------------------------------------------
# "Active" is not enough, and today's outage is why: the service ran perfectly for 17
# hours while every detection failed to be written. So this waits for a heartbeat the
# new build produced and reads the same faults the fleet page shows.
log "verifying (up to ${HEALTH_TIMEOUT}s)"
deadline=$((RESTART_AT + HEALTH_TIMEOUT))
healthy=0
reason="no status file was written after the restart"

while [[ "$(date +%s)" -lt "$deadline" ]]; do
    sleep 10
    if ! systemctl is-active --quiet "$SERVICE"; then
        reason="service stopped after the update"
        break
    fi
    [[ -f "$STATUS_FILE" ]] || continue
    # Only trust a status file this build wrote.
    written="$(stat -c %Y "$STATUS_FILE" 2>/dev/null || echo 0)"
    [[ "$written" -ge "$RESTART_AT" ]] || continue

    verdict="$(python3 - "$STATUS_FILE" <<'PY'
import json, sys
try:
    status = json.load(open(sys.argv[1]))
except Exception as e:
    print(f"unreadable status file ({type(e).__name__})"); raise SystemExit(0)
radios = status.get("radios", {})
problems = radios.get("problems") or []
# Faults that mean this build cannot do its job. A radio problem is deliberately not
# in here: an adapter unplugged overnight is not the update's fault, and rolling back
# would not fix it.
fatal = [p for p in problems if p in ("detections_not_queued", "status_file_unwritable")]
if fatal:
    print("faults after update: " + ", ".join(fatal))
else:
    print("OK")
PY
)"
    if [[ "$verdict" == "OK" ]]; then
        healthy=1
        break
    fi
    reason="$verdict"
    break
done

if [[ $healthy -ne 1 ]]; then
    log "update verification failed: $reason"
    restore
    exit 1
fi

NEW_DESC="$(git describe --tags --always 2>/dev/null || echo "$LATEST")"
log "updated $CURRENT_DESC -> $NEW_DESC and verified healthy"
