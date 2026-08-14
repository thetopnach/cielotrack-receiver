"""Regression tests for the contact state machine in radio_tracker.

Both cases here were live bugs that silently suppressed detections, found in an
external review on 2026-08-14. Neither needed hardware to reproduce, which is the
argument for these tests existing: the state machine is where the receiver decides
whether a drone that was heard becomes a drone that was recorded, and nothing else
checks it.

Run directly — no test framework required:

    python3 tests/test_receiver_state.py
"""
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
os.environ.setdefault("CENTRAL_SERVER_URL", "http://127.0.0.1:9")

import radio_tracker as rt


def isolate():
    """Point every side effect somewhere harmless and clear all shared state.

    The module keeps its contact state in module-level dicts, so a test that does not
    clear them inherits whatever the previous one left behind — which is exactly the
    class of bug being tested for.
    """
    rt.LOG_FILE = os.path.join(tempfile.mkdtemp(), "detections.csv")
    rt.play_audio_alert = lambda *a, **k: None
    # The real name of the outbox writer; stubbing the wrong one let the test
    # write to the production outbox.db, which is root-owned.
    rt.enqueue_detection = lambda *a, **k: None
    rt.remember_identity = lambda *a, **k: None
    rt.lookup_identity = lambda *a, **k: None
    for store in (rt.drone_state, rt.key_to_macs, rt.key_to_msg_count, rt.key_to_rssi,
                  rt.transport_mac_to_key, rt.pending_alert_timers,
                  rt.pending_alert_protocols, rt.sent_alert_tracker):
        store.clear()

    written = []
    original = rt.log_and_alert

    def spy(*args, **kwargs):
        written.append(args)
        return original(*args, **kwargs)

    rt.log_and_alert = spy
    return written


def check(name, condition, detail=""):
    print(f"  {'PASS' if condition else 'FAIL'}  {name}{'  — ' + detail if detail else ''}")
    return bool(condition)


def test_rekey_during_grace_period():
    """A Location-first contact must still be recorded when Basic ID arrives after it.

    The timer is created with the current key in its callback arguments. When Basic ID
    arrives the contact is re-keyed from transport MAC to UAS ID, and carrying the live
    timer across left it firing against a key whose state had just been moved: the
    detection vanished, and the dead timer stayed registered under the new key so every
    later packet from that aircraft saw "already pending" and returned.
    """
    print("re-key during the grace period")
    written = isolate()
    rt.PENDING_ALERT_GRACE_SECONDS = 0.4
    mac = "AA:BB:CC:DD:EE:FF"

    rt.register_detection(mac, {"lat": 10.0, "lon": 20.0}, "BLE", 1, -80)
    rt.register_detection(mac, {"uas_id": "RID-123"}, "BLE", 1, -80)
    time.sleep(0.9)

    ok = check("the detection is recorded", len(written) == 1, f"{len(written)} written")
    ok &= check("no timer is left registered", not rt.pending_alert_timers,
                f"{list(rt.pending_alert_timers)}")

    # The real damage was here: a stranded timer suppressed the aircraft permanently.
    rt.sent_alert_tracker.clear()
    rt.register_detection(mac, {"lat": 10.1, "lon": 20.1}, "BLE", 1, -80)
    time.sleep(0.9)
    ok &= check("a later pass is still recorded", len(written) == 2, f"{len(written)} written")
    return ok


def test_expired_cooldown_does_not_suppress():
    """An expired cooldown entry must not keep suppressing its aircraft.

    Expiry only ever happened inside log_and_alert, which a suppressed key can never
    reach — being suppressed is what stops a timer being scheduled. So on a quiet
    receiver seeing one aircraft repeatedly, a two-minute cooldown lasted indefinitely.
    """
    print("expired cooldown")
    written = isolate()
    rt.PENDING_ALERT_GRACE_SECONDS = 0.2
    rt.ALERT_COOLDOWN = 2
    rt.sent_alert_tracker["RID-999"] = time.time() - 3600

    rt.register_detection("BB:BB:BB:BB:BB:BB", {"uas_id": "RID-999", "lat": 1.0, "lon": 2.0},
                          "BLE", 1, -70)
    time.sleep(0.6)

    ok = check("the detection is recorded", len(written) == 1, f"{len(written)} written")
    # Not absent — replaced. Recording the detection starts a fresh cooldown, which is
    # the correct end state; what must not survive is the hour-old timestamp.
    stamp = rt.sent_alert_tracker.get("RID-999")
    ok &= check("the stale timestamp was replaced, not kept",
                stamp is not None and time.time() - stamp < 60,
                f"age {time.time() - stamp:.0f}s" if stamp else "entry missing entirely")
    return ok


def test_cooldown_still_suppresses_while_live():
    """The cooldown must keep working — the fix above must not simply disable it."""
    print("live cooldown still suppresses")
    written = isolate()
    rt.PENDING_ALERT_GRACE_SECONDS = 0.2
    rt.ALERT_COOLDOWN = 120
    rt.sent_alert_tracker["RID-555"] = time.time()

    rt.register_detection("CC:CC:CC:CC:CC:CC", {"uas_id": "RID-555", "lat": 1.0, "lon": 2.0},
                          "BLE", 1, -70)
    time.sleep(0.5)
    return check("nothing is recorded inside the window", not written, f"{len(written)} written")


def test_both_transports_are_credited():
    """A contact heard on BLE and Wi-Fi records both, and only once."""
    print("dual transport")
    written = isolate()
    rt.PENDING_ALERT_GRACE_SECONDS = 0.5

    rt.register_detection("DD:DD:DD:DD:DD:DD", {"uas_id": "RID-777"}, "BLE", 1, -70)
    rt.register_detection("EE:EE:EE:EE:EE:EE", {"uas_id": "RID-777", "lat": 5.0, "lon": 6.0},
                          "Wi-Fi", 4, -60)
    time.sleep(1.0)

    ok = check("recorded exactly once", len(written) == 1, f"{len(written)} written")
    protocol = written[0][1] if written and len(written[0]) > 1 else ""
    ok &= check("both transports credited", "BLE" in str(protocol) and "Wi-Fi" in str(protocol),
                str(protocol))
    return ok


if __name__ == "__main__":
    results = [
        test_rekey_during_grace_period(),
        test_expired_cooldown_does_not_suppress(),
        test_cooldown_still_suppresses_while_live(),
        test_both_transports_are_credited(),
    ]
    print(f"\n{sum(results)}/{len(results)} passed")
    sys.exit(0 if all(results) else 1)
