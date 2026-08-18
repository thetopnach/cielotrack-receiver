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
CHANNEL_FILE="${CONFIG_DIR}/channel"
REJECTED_FILE="${CONFIG_DIR}/rejected"
# The same ladder radio_tracker walks, minus systemd's STATE_DIRECTORY — this runs from
# its own timer, not from the receiver's unit, so it has to work the directory out
# rather than be handed it. Getting this wrong is not loud: the health check would look
# for a status file in the old place, never find one this build wrote, and roll back
# every release forever while the receiver was perfectly healthy.
STATE_DIR="${CIELOTRACK_STATE_DIR:-}"
if [[ -z "$STATE_DIR" ]]; then
    if [[ -d /var/lib/cielotrack ]]; then STATE_DIR=/var/lib/cielotrack; else STATE_DIR="$INSTALL_DIR"; fi
fi
STATUS_FILE="${CIELOTRACK_STATUS_FILE:-${STATE_DIR}/status.json}"
# Two heartbeats plus warmup. Long enough that a healthy receiver has certainly
# reported, short enough that a broken one is rolled back inside the quiet window.
HEALTH_TIMEOUT="${CIELOTRACK_HEALTH_TIMEOUT:-180}"
CHECK_ONLY=0
[[ "${1:-}" == "--check" ]] && CHECK_ONLY=1

# Both to stderr. select_release below returns its answer on stdout and is read with a
# command substitution, so a log line written to stdout would be captured as part of the
# version string — a bug that would look like a corrupt tag name rather than a stray
# message. systemd records both streams identically, so the journal is unchanged.
log() { echo "[cielotrack-update] $*" >&2; }
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
#
# The receiver's own files are excluded. .gitignore covers them too, but this list is
# what actually decides, and an install whose data still sits beside the code would
# otherwise look permanently "modified" — so the one release that moves that data out
# would be the one release this could never install.
OURS=(':!status.json' ':!*.db*' ':!.env' ':!device_credentials.json' ':!*.csv')
if [[ -n "$(git status --porcelain -- "${OURS[@]}" 2>/dev/null)" ]]; then
    log "local modifications present — leaving this checkout alone"
    git status --short -- "${OURS[@]}" | sed 's/^/    /'
    exit 0
fi

CURRENT_COMMIT="$(git rev-parse HEAD)"
CURRENT_DESC="$(git describe --tags --always 2>/dev/null || echo "$CURRENT_COMMIT")"

log "fetching releases"
git fetch --quiet --tags --force origin || fail "could not reach the release repository"

# --- which releases this receiver accepts -------------------------------------------
# Two channels. stable takes only final releases. canary also takes prereleases, so
# something is running a release before the fleet is, and a fault that survives the
# health check below — a release that installs cleanly, reports healthy, and quietly
# hears less — has somewhere to show up while a person is watching.
#
# An absent or unreadable file means stable, because the safe channel has to be the one
# a receiver ends up on by doing nothing.
CHANNEL="stable"
if [[ -r "$CHANNEL_FILE" ]]; then
    CHANNEL="$(tr -d '[:space:]' < "$CHANNEL_FILE" | tr '[:upper:]' '[:lower:]')"
fi
case "$CHANNEL" in
    stable|canary) ;;
    *)  log "unrecognised channel '${CHANNEL}' in ${CHANNEL_FILE} — using stable"
        CHANNEL="stable" ;;
esac

# versionsort.suffix teaches git that -rc sorts below the release it precedes. Without
# it git ranks v1.2.0-rc2 *above* v1.2.0, and plain `sort -V` does the same — which
# would strand a canary on a release candidate forever once the real release shipped.
#
# The prerelease test is a shell glob rather than grep deliberately: this runs on
# whatever the operator happens to have installed, and grep is not always GNU grep.
# A release this receiver already tried and rolled back from. Without this the failed
# tag is still the newest one, so the receiver reinstalls it, fails the same check, and
# rolls back again — every night, indefinitely, restarting the service twice each time.
# Nothing raises an alarm, because from the outside it just looks like a receiver that
# keeps restarting in the small hours.
is_rejected() {
    [[ -r "$REJECTED_FILE" ]] || return 1
    local line
    while IFS= read -r line; do
        line="${line%%#*}"
        line="${line//[[:space:]]/}"
        [[ -n "$line" ]] || continue
        [[ "$line" == "$1" ]] && return 0
    done < "$REJECTED_FILE"
    return 1
}

select_release() {
    local tag
    while IFS= read -r tag; do
        [[ -n "$tag" ]] || continue
        if [[ "$CHANNEL" == "stable" ]]; then
            case "$tag" in *-*) continue ;; esac
        fi
        # Only this exact tag is skipped, never "everything up to it" — a newer release
        # is tried on its own merits, so a fix is never blocked by the broken release it
        # replaces. That is the whole reason this is a list of versions rather than a
        # low-water mark.
        if is_rejected "$tag"; then
            log "skipping $tag: it failed on this receiver before"
            continue
        fi
        printf '%s\n' "$tag"
        return 0
    done < <(git -c versionsort.suffix=-rc tag --sort=-v:refname \
                 --list 'v[0-9]*.[0-9]*.[0-9]*')
    return 1
}

LATEST="$(select_release || true)"
[[ -n "$LATEST" ]] || { log "no release tags published yet — nothing to do"; exit 0; }
log "channel: $CHANNEL"

LATEST_COMMIT="$(git rev-parse "${LATEST}^{commit}")"
if [[ "$LATEST_COMMIT" == "$CURRENT_COMMIT" ]]; then
    log "already on $CURRENT_DESC (latest for $CHANNEL is $LATEST)"
    exit 0
fi

# Note this can move a receiver *backwards*: switching a canary to stable while it is
# running a prerelease puts it on the newest final release, which is an older commit.
# That is the point of switching back, so it is allowed rather than guarded against.

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
    # Recorded here rather than at the individual failure sites because every path that
    # reaches this function means the same thing: this release did not work on this
    # receiver. An operator who disagrees can clear the file and it will be retried.
    mkdir -p "$CONFIG_DIR"
    printf '%s  # rejected %s\n' "$LATEST" "$(date -Is 2>/dev/null || date)" \
        >> "$REJECTED_FILE"
    log "recorded $LATEST as rejected — it will not be retried; to undo, edit $REJECTED_FILE"
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

# --- can this host actually run it -------------------------------------------------
# A release may need host state a checkout cannot create for itself: the service user,
# or the receiver's data moved out of the install directory. Installing it anyway would
# be *safe* — the health check fails, the rollback puts everything back — but it would
# also record the release as rejected, which is a permanent verdict on a receiver that
# has simply not been prepared yet, and nothing would tell the operator what to do. So
# the check happens before anything is changed, and standing down is not a rejection.
unmet_prerequisite() {
    local unit="${SERVICE}.service" wanted_user state_name
    [[ -f "$unit" ]] || return 1

    wanted_user="$(sed -n 's/^[[:space:]]*User=//p' "$unit" | tail -1)"
    if [[ -n "$wanted_user" ]] && ! id -u "$wanted_user" >/dev/null 2>&1; then
        echo "it runs as '$wanted_user', and there is no such user on this host"
        echo "    sudo $INSTALL_DIR/provision.sh"
        return 0
    fi

    # Read from the unit rather than assumed, so this keeps telling the truth if the
    # directory is ever renamed.
    state_name="$(sed -n 's/^[[:space:]]*StateDirectory=//p' "$unit" | tail -1)"
    if [[ -n "$state_name" && -f "$INSTALL_DIR/device_credentials.json" \
          && ! -f "/var/lib/${state_name}/device_credentials.json" ]]; then
        echo "this receiver's identity is still in $INSTALL_DIR, where the service user cannot read it"
        echo "    sudo $INSTALL_DIR/migrate_state.py --apply"
        return 0
    fi
    return 1
}

if reason="$(unmet_prerequisite)"; then
    log "$LATEST needs something this receiver does not have yet:"
    while IFS= read -r line; do log "  $line"; done <<<"$reason"
    log "staying on $CURRENT_DESC — nothing is wrong with the release, so it is not recorded as rejected"
    git checkout --quiet --force "$CURRENT_COMMIT" || log "WARNING: could not restore the previous commit"
    exit 0
fi

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
