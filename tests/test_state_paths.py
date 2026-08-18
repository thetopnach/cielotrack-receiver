"""Tests for running as an ordinary user: where state lives, and getting it there.

Three failures are being guarded against, and none of them is loud on its own.

  Prepending sudo when we already hold CAP_NET_RAW. Under NoNewPrivileges there is no
  setuid bit for sudo to use and no sudoers entry for a system user, so every helper
  call fails at once — but the failure is a subprocess exit code, and the receiver
  keeps running with no radios.

  Writing state into the install directory. Root owns the checkout, the service user
  does not, and what fails is only the writes that *create* files: outbox.db-wal,
  status.json.tmp. Capture, decoding and alerting all keep working. That is the
  2026-08-15 outage, seventeen hours of it.

  Losing the credential file in the move. An absent one is an ordinary state — a
  receiver nobody has claimed — so nothing reports a problem: the receiver registers
  as a stranger and the operator watches a receiver that has stopped reporting.

Run directly — no test framework required:

    python3 tests/test_state_paths.py
"""
import json
import os
import sqlite3
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
os.environ.setdefault("CENTRAL_SERVER_URL", "http://127.0.0.1:9")

import radio_tracker as rt
import migrate_state


def check(name, condition, detail=""):
    print(f"  {'PASS' if condition else 'FAIL'}  {name}{'  — ' + detail if detail else ''}")
    return bool(condition)


def with_environment(**overrides):
    """Set env vars, returning what they were, so a test can put them back."""
    previous = {}
    for name, value in overrides.items():
        previous[name] = os.environ.get(name)
        if value is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = value
    return previous


def restore(previous):
    for name, value in previous.items():
        if value is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = value


def status_file(directory, capabilities):
    """A stand-in for /proc/self/status carrying a given effective capability mask."""
    path = os.path.join(directory, "status")
    with open(path, "w") as handle:
        handle.write("Name:\tpython3\n")
        handle.write(f"CapEff:\t{capabilities:016x}\n")
        handle.write("Seccomp:\t0\n")
    return path


def seed_install(directory, device_id="dev-1234", queued=3):
    """An install of the old shape: state sitting beside the code."""
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "device_credentials.json"), "w") as handle:
        json.dump({"device_id": device_id, "bootstrap_secret": "s3cret", "api_key": "k"},
                  handle)
    os.chmod(os.path.join(directory, "device_credentials.json"), 0o600)

    outbox = os.path.join(directory, "outbox.db")
    conn = sqlite3.connect(outbox)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("CREATE TABLE outbox (id INTEGER PRIMARY KEY, payload TEXT, sent_at TEXT)")
    for i in range(queued):
        conn.execute("INSERT INTO outbox (payload, sent_at) VALUES (?, NULL)", (f"row{i}",))
    conn.commit()
    # Left open, so the newest rows are still only in the WAL — a plain file copy would
    # take the database and leave exactly the detections that have not been uploaded.
    return conn


def test_the_capability_decides_whether_sudo_is_used():
    """Keyed on euid, a service holding CAP_NET_RAW as uid 1000 prepends sudo to every
    hcitool call — and every one of them fails, silently, forever."""
    print("\nthe capability decides whether sudo is used, not the uid")
    with tempfile.TemporaryDirectory() as tmp:
        ok = check("a mask with CAP_NET_RAW reads as held",
                   rt.effective_capabilities(status_file(tmp, 0x3000)) & (1 << rt.CAP_NET_RAW))
        ok &= check("one without it does not",
                    not rt.effective_capabilities(status_file(tmp, 0x0400)) & (1 << rt.CAP_NET_RAW))
        ok &= check("an unreadable status file reads as holding nothing",
                    rt.effective_capabilities(os.path.join(tmp, "absent")) == 0)
        ok &= check("and so does a malformed one",
                    rt.effective_capabilities(__file__) == 0)

        held = rt.HAS_NET_RAW
        try:
            rt.HAS_NET_RAW = True
            ok &= check("holding it, the helper is called directly",
                        rt.privileged(["hcitool", "lescan"]) == ["hcitool", "lescan"])
            rt.HAS_NET_RAW = False
            ok &= check("without it, sudo is still there for a person at a terminal",
                        rt.privileged(["hcitool", "lescan"]) == ["sudo", "hcitool", "lescan"])
        finally:
            rt.HAS_NET_RAW = held
        return ok


def fake_adapters(directory, adapters):
    """A stand-in for /sys/class/bluetooth. `adapters` maps hciN -> subsystem name."""
    for name, subsystem in adapters.items():
        device = os.path.join(directory, name, "device")
        os.makedirs(device, exist_ok=True)
        target = os.path.join(directory, "_subsystems", subsystem)
        os.makedirs(target, exist_ok=True)
        link = os.path.join(device, "subsystem")
        if not os.path.islink(link):
            os.symlink(target, link)
    return directory


def test_the_usb_adapter_wins_over_the_boards_own_radio():
    """The failure this prevents was invisible. On a stock Raspberry Pi the onboard
    radio is enabled and takes an hci index, so picking the lowest index is decided by
    enumeration order — unplug the dongle for a moment and the receiver comes back
    listening on the onboard radio. Extended scanning reports active, no fault is
    raised, and the only symptom is hearing almost nothing."""
    print("\nthe USB adapter wins over the board's own radio")
    saved = with_environment(BLE_INTERFACE=None)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            # The shape that broke the first externally-provisioned receiver: the
            # onboard radio enumerated first.
            both = fake_adapters(os.path.join(tmp, "both"),
                                 {"hci0": "usb", "hci1": "serial"})
            ok = check("with the dongle first, the dongle is chosen",
                       rt.detect_ble_interface(both) == "hci0")

            swapped = fake_adapters(os.path.join(tmp, "swapped"),
                                    {"hci0": "serial", "hci1": "usb"})
            ok &= check("and with the onboard radio first, still the dongle",
                        rt.detect_ble_interface(swapped) == "hci1")

            two_usb = fake_adapters(os.path.join(tmp, "two_usb"),
                                    {"hci0": "usb", "hci1": "usb"})
            ok &= check("two dongles falls back to the lowest index",
                        rt.detect_ble_interface(two_usb) == "hci0")

            onboard = fake_adapters(os.path.join(tmp, "onboard"), {"hci0": "serial"})
            ok &= check("an onboard-only host still works",
                        rt.detect_ble_interface(onboard) == "hci0")

            ok &= check("no adapters at all falls back to hci0",
                        rt.detect_ble_interface(os.path.join(tmp, "absent")) == "hci0")

            # The override is what a host deliberately using its onboard radio needs.
            os.environ["BLE_INTERFACE"] = "hci1"
            ok &= check("an explicit BLE_INTERFACE beats all of it",
                        rt.detect_ble_interface(both) == "hci1")
            return ok
    finally:
        restore(saved)


def test_the_state_directory_is_resolved_in_order():
    """systemd hands the directory over in STATE_DIRECTORY. Anything run outside the
    unit — the updater, a migration, someone at a terminal — has to reach the same
    answer, or two halves of the same receiver read different files."""
    print("\nthe state directory is resolved in a fixed order")
    saved = with_environment(CIELOTRACK_STATE_DIR=None, STATE_DIRECTORY=None)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            named, systemd = os.path.join(tmp, "named"), os.path.join(tmp, "systemd")
            os.environ["CIELOTRACK_STATE_DIR"] = named
            os.environ["STATE_DIRECTORY"] = systemd
            ok = check("an explicit directory wins", rt.state_directory() == named)

            del os.environ["CIELOTRACK_STATE_DIR"]
            ok &= check("otherwise systemd's", rt.state_directory() == systemd)

            # systemd passes a colon-separated list when a unit names several.
            os.environ["STATE_DIRECTORY"] = f"{systemd}:{tmp}/other"
            ok &= check("and only the first of its list", rt.state_directory() == systemd)

            del os.environ["STATE_DIRECTORY"]
            expected = (rt.DEFAULT_STATE_DIR if os.path.isdir(rt.DEFAULT_STATE_DIR)
                        else rt.INSTALL_DIR)
            ok &= check("with neither set, it falls back without inventing a directory",
                        rt.state_directory() == expected, expected)
            return ok
    finally:
        restore(saved)


def test_a_blank_setting_means_unset():
    """The bug the first canary found, ninety seconds after it was switched on.

    .env.example ships keys present and blank — `STATUS_FILE=` is there as a hint about
    what can be overridden — and load_dotenv puts that empty string into the
    environment. os.environ.get(name, default) then hands back "", not the default, so
    the receiver spent the day writing "" and ".tmp" into a directory it did not own:

        Could not write : PermissionError: [Errno 13] Permission denied: '.tmp'

    Which is worse than it looks. status.json is what update.sh reads to decide whether
    a release worked, so a receiver that cannot write one rolls back every release it is
    offered and records each of them as rejected."""
    print("\na blank setting means unset, not empty")
    saved = with_environment(STATUS_FILE=None, CIELOTRACK_STATE_DIR=None)
    try:
        os.environ["STATUS_FILE"] = ""
        ok = check("an empty value falls through to the default",
                   rt.env_or("STATUS_FILE", "/var/lib/cielotrack/status.json")
                   == "/var/lib/cielotrack/status.json")
        os.environ["STATUS_FILE"] = "   "
        ok &= check("and so does one that is only whitespace",
                    rt.env_or("STATUS_FILE", "/default") == "/default")
        os.environ["STATUS_FILE"] = "/somewhere/else.json"
        ok &= check("a real value still wins",
                    rt.env_or("STATUS_FILE", "/default") == "/somewhere/else.json")
        del os.environ["STATUS_FILE"]
        ok &= check("an absent variable is unchanged behaviour",
                    rt.env_or("STATUS_FILE", "/default") == "/default")

        # The whole point: a stock .env must not move the file out of the state
        # directory, and must never produce a path that is empty or relative.
        os.environ["STATUS_FILE"] = ""
        os.environ["CIELOTRACK_STATE_DIR"] = ""
        os.environ["STATE_DIRECTORY"] = "/var/lib/cielotrack"
        resolved = rt.env_or("STATUS_FILE",
                             os.path.join(rt.state_directory(), "status.json"))
        ok &= check("so a stock .env resolves inside the state directory",
                    resolved == "/var/lib/cielotrack/status.json", resolved)
        return ok
    finally:
        os.environ.pop("STATE_DIRECTORY", None)
        restore(saved)


def test_an_identity_left_behind_stops_the_receiver():
    """The one failure that costs something irreversible. Registering again is easy,
    quiet, and leaves the operator's claimed receiver dark."""
    print("\nan identity left behind stops the receiver rather than being replaced")
    with tempfile.TemporaryDirectory() as tmp:
        install, state = os.path.join(tmp, "opt"), os.path.join(tmp, "var")
        os.makedirs(install)
        os.makedirs(state)
        saved = (rt.INSTALL_DIR, rt.STATE_DIR, rt.CREDENTIALS_FILE)
        try:
            rt.INSTALL_DIR, rt.STATE_DIR = install, state
            rt.CREDENTIALS_FILE = os.path.join(state, "device_credentials.json")

            ok = check("nothing anywhere is not a problem — that is a new receiver",
                       rt.stranded_credentials() is None)

            legacy = os.path.join(install, "device_credentials.json")
            with open(legacy, "w") as handle:
                json.dump({"device_id": "dev-abc"}, handle)
            ok &= check("an identity in the old place, and none in the new, is",
                        rt.stranded_credentials() == legacy)

            failed = False
            try:
                rt.load_or_create_credentials()
            except SystemExit:
                failed = True
            ok &= check("so it refuses to generate a replacement", failed)
            ok &= check("and leaves the original where it is", os.path.exists(legacy))

            with open(rt.CREDENTIALS_FILE, "w") as handle:
                json.dump({"device_id": "dev-abc"}, handle)
            ok &= check("once it has been moved across, it starts",
                        rt.stranded_credentials() is None)

            # An install that never moved is the ordinary case, not a fault.
            rt.STATE_DIR = install
            rt.CREDENTIALS_FILE = legacy
            ok &= check("and an unmigrated install is not a fault at all",
                        rt.stranded_credentials() is None)
            return ok
        finally:
            rt.INSTALL_DIR, rt.STATE_DIR, rt.CREDENTIALS_FILE = saved


def test_the_migration_carries_everything_across():
    print("\nthe migration carries the identity and the queue across")
    with tempfile.TemporaryDirectory() as tmp:
        install, state = os.path.join(tmp, "opt"), os.path.join(tmp, "var")
        writer = seed_install(install, device_id="dev-9f2c", queued=4)
        with open(os.path.join(install, "amazon_drone_matches.csv"), "w") as handle:
            handle.write("timestamp,uas_id\n2026-08-18T00:00:00Z,1596F\n")

        ok, steps = migrate_state.migrate(install, state, apply=False)
        moved = [name for name, verdict, _ in steps if verdict == "would move"]
        ok = check("a dry run says what it would do", ok and len(moved) == 3, str(moved))
        ok &= check("and does not do it", not os.path.exists(os.path.join(state, "outbox.db")))

        applied, steps = migrate_state.migrate(install, state, apply=True)
        writer.close()
        ok &= check("applied, every file moves", applied and
                    all(v == "moved" for _, v, _ in steps), str(steps))

        credentials = os.path.join(state, "device_credentials.json")
        with open(credentials) as handle:
            ok &= check("the device id is the same one",
                        json.load(handle)["device_id"] == "dev-9f2c")
        ok &= check("and it is still readable only by its owner",
                    oct(os.stat(credentials).st_mode)[-3:] == "600",
                    oct(os.stat(credentials).st_mode)[-3:])

        # The rows that only exist in the WAL are the newest — the ones not yet uploaded.
        conn = sqlite3.connect(os.path.join(state, "outbox.db"))
        rows = conn.execute("SELECT COUNT(*) FROM outbox").fetchone()[0]
        conn.close()
        ok &= check("the queue arrives whole, write-ahead log included", rows == 4, str(rows))

        ok &= check("the originals are set aside, not deleted",
                    os.path.exists(os.path.join(state, "pre-migration",
                                                "device_credentials.json")))
        ok &= check("and are gone from the install directory",
                    not os.path.exists(os.path.join(install, "device_credentials.json")))
        return ok


def test_the_migration_is_safe_to_run_twice():
    """provision.sh may be run again at any time, and an interrupted migration has to be
    finishable. Neither may overwrite what the service has been writing since."""
    print("\nthe migration is safe to run again")
    with tempfile.TemporaryDirectory() as tmp:
        install, state = os.path.join(tmp, "opt"), os.path.join(tmp, "var")
        seed_install(install, device_id="dev-first").close()

        migrate_state.migrate(install, state, apply=True)
        # The receiver has run since, and its queue has moved on.
        conn = sqlite3.connect(os.path.join(state, "outbox.db"))
        conn.execute("INSERT INTO outbox (payload, sent_at) VALUES ('later', NULL)")
        conn.commit()
        conn.close()
        # An old copy reappears in the install directory, as a rollback would leave it.
        seed_install(install, device_id="dev-stale", queued=1).close()

        ok, steps = migrate_state.migrate(install, state, apply=True)
        verdicts = {name: verdict for name, verdict, _ in steps}
        ok = check("a second run reports the files as already there", ok and
                   verdicts.get("device_credentials.json") == "kept", str(verdicts))

        with open(os.path.join(state, "device_credentials.json")) as handle:
            ok &= check("the live identity is not overwritten by the stale one",
                        json.load(handle)["device_id"] == "dev-first")
        conn = sqlite3.connect(os.path.join(state, "outbox.db"))
        rows = conn.execute("SELECT COUNT(*) FROM outbox").fetchone()[0]
        conn.close()
        ok &= check("nor is the queue rolled back", rows == 4, str(rows))

        refused, steps = migrate_state.migrate(install, install, apply=True)
        ok &= check("and migrating a directory onto itself is refused", not refused,
                    str(steps))
        return ok


def test_a_copy_that_does_not_verify_leaves_the_original():
    """Verification is the whole point of copying rather than moving. If it cannot be
    trusted, the original has to still be there."""
    print("\na copy that does not verify leaves the original alone")
    with tempfile.TemporaryDirectory() as tmp:
        install, state = os.path.join(tmp, "opt"), os.path.join(tmp, "var")
        seed_install(install, device_id="dev-real").close()

        # A copy that lands with a different identity is the failure that matters, so
        # the check itself is exercised directly.
        os.makedirs(state)
        other = os.path.join(state, "other.json")
        with open(other, "w") as handle:
            json.dump({"device_id": "dev-someone-else"}, handle)
        good, detail = migrate_state.verify(
            "device_credentials.json",
            os.path.join(install, "device_credentials.json"), other)
        ok = check("a copy with a different device id fails its check", not good, detail)

        with open(other, "w") as handle:
            handle.write("{not json")
        good, detail = migrate_state.verify(
            "device_credentials.json",
            os.path.join(install, "device_credentials.json"), other)
        ok &= check("so does one that will not parse", not good, detail)

        junk = os.path.join(state, "junk.db")
        with open(junk, "wb") as handle:
            handle.write(b"not a database" * 50)
        good, detail = migrate_state.verify(
            "outbox.db", os.path.join(install, "outbox.db"), junk)
        ok &= check("and a queue that is not a database", not good, detail)
        return ok


TESTS = [
    test_the_capability_decides_whether_sudo_is_used,
    test_the_usb_adapter_wins_over_the_boards_own_radio,
    test_the_state_directory_is_resolved_in_order,
    test_a_blank_setting_means_unset,
    test_an_identity_left_behind_stops_the_receiver,
    test_the_migration_carries_everything_across,
    test_the_migration_is_safe_to_run_twice,
    test_a_copy_that_does_not_verify_leaves_the_original,
]


if __name__ == "__main__":
    if len(sys.argv) > 1:
        wanted = {name.lstrip("-").replace("-", "_") for name in sys.argv[1:]}
        chosen = [t for t in TESTS if t.__name__ in wanted or
                  t.__name__.removeprefix("test_") in wanted]
        if not chosen:
            print(f"no test matches {sorted(wanted)}; known tests:")
            for t in TESTS:
                print(f"  {t.__name__.removeprefix('test_')}")
            sys.exit(2)
    else:
        chosen = TESTS

    results = [t() for t in chosen]
    print(f"\n{sum(results)}/{len(results)} passed")
    sys.exit(0 if all(results) else 1)
