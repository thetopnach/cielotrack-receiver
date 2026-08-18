#!/usr/bin/env python3
"""Move a receiver's own data out of the install directory and into its state directory.

Existing receivers keep their queue, credentials, log and status file beside the code,
which works only while the service is root and root owns the checkout. Once the service
runs as its own user it cannot create files there, and the writes that fail first are
the ones that make new files — `outbox.db-wal`, `status.json.tmp`. Capture keeps
working; every upload stops. We have already lost seventeen hours to exactly that.

So this runs once, before the switch, and it is written to be interruptible. Nothing is
removed until its copy has been opened and checked, and what it replaces is set aside
rather than deleted:

    sudo ./migrate_state.py                       # what it would do
    sudo ./migrate_state.py --apply

The delicate file is `device_credentials.json`. It holds this receiver's identity, and
losing it is silent: an absent credential file is an ordinary state — a receiver nobody
has claimed yet — so the receiver would generate a new device id, register as a
stranger, and leave the operator watching a receiver that has stopped reporting. That is
why the device id is compared on both sides here, and why radio_tracker refuses to start
if it ever finds an identity left behind.
"""
import argparse
import grp
import hashlib
import json
import os
import pwd
import shutil
import sqlite3
import subprocess
import sys

DEFAULT_STATE_DIR = "/var/lib/cielotrack"
DEFAULT_USER = "cielotrack"
SERVICE = "cielotrack-receiver"

# status.json is deliberately absent: it is rewritten every minute and says nothing
# about the past, so carrying a stale copy across only creates a file whose timestamp
# lies about when this receiver was last alive.
STATE_FILES = ("device_credentials.json", "outbox.db", "amazon_drone_matches.csv")


def digest(path):
    hashed = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(65536), b""):
            hashed.update(block)
    return hashed.hexdigest()


def service_is_running(service=SERVICE):
    """True if the receiver is up. Unknown counts as running: a migration that races
    the writer is the one failure mode this cannot recover from."""
    try:
        done = subprocess.run(["systemctl", "is-active", "--quiet", service], timeout=10)
    except FileNotFoundError:
        return False          # no systemd at all — a test box or a container
    except Exception:
        return True
    return done.returncode == 0


def copy_database(source, destination):
    """Copy a SQLite database through its own backup API, so the WAL comes with it.

    A plain file copy takes the database and leaves the write-ahead log behind, which
    silently drops every detection queued since the last checkpoint — the newest ones,
    which are the ones that have not been uploaded yet.
    """
    live = sqlite3.connect(f"file:{source}?mode=ro", uri=True)
    try:
        copy = sqlite3.connect(destination)
        try:
            live.backup(copy)
        finally:
            copy.close()
    finally:
        live.close()


def outbox_rows(path):
    conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        if conn.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
            return None
        return conn.execute("SELECT COUNT(*) FROM outbox").fetchone()[0]
    except sqlite3.Error:
        return None
    finally:
        conn.close()


def device_id(path):
    try:
        with open(path) as handle:
            return json.load(handle).get("device_id")
    except (OSError, ValueError):
        return None


def verify(name, source, destination):
    """Prove the copy holds what the original held. Returns (ok, description)."""
    if name == "device_credentials.json":
        theirs, ours = device_id(source), device_id(destination)
        if not ours:
            return False, "the copy has no device id in it"
        if theirs != ours:
            return False, f"device id changed: {theirs} -> {ours}"
        return True, f"device {ours}"
    if name.endswith(".db"):
        theirs, ours = outbox_rows(source), outbox_rows(destination)
        if ours is None:
            return False, "the copy is not a readable database"
        if theirs is not None and ours < theirs:
            return False, f"rows lost: {theirs} -> {ours}"
        return True, f"{ours} queued row(s)"
    if digest(source) != digest(destination):
        return False, "the copy does not match the original"
    return True, f"{os.path.getsize(destination)} bytes"


def own(path, uid, gid):
    """Hand a file to the service user. A no-op when we are not root or there is no
    such user — the same script has to work on a test box."""
    if uid is None:
        return
    try:
        os.chown(path, uid, gid)
    except (OSError, PermissionError):
        pass


def resolve_user(name):
    try:
        entry = pwd.getpwnam(name)
    except KeyError:
        return None, None
    try:
        group = grp.getgrnam(name).gr_gid
    except KeyError:
        group = entry.pw_gid
    return entry.pw_uid, group


def migrate(install_dir, state_dir, user=None, apply=False, files=STATE_FILES):
    """Copy each state file across, check it, then set the original aside.

    Returns (ok, [(name, verdict, detail)]) — every file is reported, including the ones
    that needed nothing, because "there was nothing to move" and "it moved" are answers
    an operator needs to be able to tell apart.
    """
    uid, gid = resolve_user(user) if user else (None, None)
    steps = []
    ok = True

    if os.path.abspath(install_dir) == os.path.abspath(state_dir):
        return False, [("--", "refused", "the state directory is the install directory")]

    if apply:
        os.makedirs(state_dir, exist_ok=True)
        own(state_dir, uid, gid)
        os.chmod(state_dir, 0o750)

    aside = os.path.join(state_dir, "pre-migration")

    for name in files:
        source = os.path.join(install_dir, name)
        destination = os.path.join(state_dir, name)

        if not os.path.exists(source):
            steps.append((name, "skipped", "nothing at the old location"))
            continue
        if os.path.exists(destination):
            # Not overwritten on purpose: the destination is what the service has been
            # writing to, so it is newer than whatever is still sitting in the install
            # directory. Copying over it would undo real work.
            steps.append((name, "kept", "already in the state directory"))
            continue
        if not apply:
            steps.append((name, "would move", f"{source} -> {destination}"))
            continue

        try:
            if name.endswith(".db"):
                copy_database(source, destination)
            else:
                shutil.copy2(source, destination)
        except (OSError, sqlite3.Error) as failure:
            steps.append((name, "FAILED", f"{type(failure).__name__}: {failure}"))
            ok = False
            continue

        good, detail = verify(name, source, destination)
        if not good:
            # Left where it is. A copy that did not verify is evidence, and the original
            # is still the working file.
            steps.append((name, "FAILED", detail))
            ok = False
            continue

        # The credential file carries its mode across explicitly; copy2 preserves it,
        # but this one is not left to a library's definition of "metadata".
        if name == "device_credentials.json":
            os.chmod(destination, 0o600)
        own(destination, uid, gid)

        os.makedirs(aside, exist_ok=True)
        own(aside, uid, gid)
        os.replace(source, os.path.join(aside, name))
        steps.append((name, "moved", detail))

    return ok, steps


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--from", dest="install_dir",
                        default=os.path.dirname(os.path.abspath(__file__)),
                        help="the install directory to move state out of")
    parser.add_argument("--state-dir", default=DEFAULT_STATE_DIR)
    parser.add_argument("--user", default=DEFAULT_USER,
                        help="the service user to give the files to")
    parser.add_argument("--apply", action="store_true",
                        help="actually move things; without it, only says what it would do")
    parser.add_argument("--force", action="store_true",
                        help="migrate even with the receiver running (it will race the writer)")
    args = parser.parse_args()

    if args.apply and service_is_running() and not args.force:
        print(f"✗ {SERVICE} is running. It is writing to these files right now.")
        print(f"  sudo systemctl stop {SERVICE}")
        return 1

    ok, steps = migrate(args.install_dir, args.state_dir, user=args.user, apply=args.apply)
    width = max(len(name) for name, _, _ in steps)
    for name, verdict, detail in steps:
        mark = "✗" if verdict == "FAILED" else " "
        print(f"{mark} {name.ljust(width)}  {verdict}  — {detail}")

    if not args.apply:
        print("\nNothing has changed. Add --apply to do it.")
        return 0 if ok else 1

    if not ok:
        print("\n✗ Some files did not move. The originals are untouched; nothing was lost.")
        return 1

    print(f"\n✓ State is in {args.state_dir}. The originals are in "
          f"{os.path.join(args.state_dir, 'pre-migration')} — delete them once the "
          f"receiver has run for a day.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
