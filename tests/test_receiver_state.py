"""Regression tests for the contact state machine in radio_tracker.

Both cases here were live bugs that silently suppressed detections, found in an
external review on 2026-08-14. Neither needed hardware to reproduce, which is the
argument for these tests existing: the state machine is where the receiver decides
whether a drone that was heard becomes a drone that was recorded, and nothing else
checks it.

Run directly — no test framework required:

    python3 tests/test_receiver_state.py
"""
import json
import os
import sys
import tempfile
import threading
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
                  rt.pending_alert_protocols, rt.sent_alert_tracker,
                  # getattr so this file still runs against a build without it, and the
                  # assertion reports the bug rather than dying on an AttributeError.
                  getattr(rt, "key_to_position_at", {})):
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


def test_concurrent_transports_do_not_corrupt_state():
    """Two capture threads hammering the same aircraft must not lose or duplicate it.

    BLE and Wi-Fi run on separate threads and mutate the same maps, and the re-key from
    transport MAC to UAS ID is several dictionary operations that have to happen
    together. Interleaving them used to be possible: the GIL makes each operation
    atomic and does nothing for a sequence of them.
    """
    print("concurrent transports")
    written = isolate()
    rt.PENDING_ALERT_GRACE_SECONDS = 0.6
    errors = []

    def hammer(mac, protocol, extra):
        try:
            for _ in range(40):
                rt.register_detection(mac, dict(extra), protocol, 1, -70)
                rt.register_detection(mac, {"uas_id": "RID-CONC"}, protocol, 1, -70)
        except Exception as exc:              # a race would surface as KeyError here
            errors.append(exc)

    threads = [
        threading.Thread(target=hammer, args=("11:11:11:11:11:11", "BLE", {"lat": 1.0})),
        threading.Thread(target=hammer, args=("22:22:22:22:22:22", "Wi-Fi", {"lon": 2.0})),
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    time.sleep(1.2)

    ok = check("no exception escaped a capture thread", not errors, str(errors[:1]))
    ok &= check("the contact was recorded", len(written) >= 1, f"{len(written)} written")
    ok &= check("state converged on one key", list(rt.drone_state) in ([], ["RID-CONC"]),
                str(list(rt.drone_state)))
    ok &= check("no timers left behind", not rt.pending_alert_timers,
                str(list(rt.pending_alert_timers)))
    return ok


def test_write_failures_are_reported_then_clear():
    """A receiver that cannot write must say so, once, for as long as it is failing.

    This is the failure that hid a 17-hour outage on 2026-08-15: capture, decoding and
    the heartbeat all kept working while every detection failed to reach the outbox, so
    every other signal stayed green — including outbox_pending, which read 0 precisely
    because nothing could be enqueued. Only a counter that moves when a write fails can
    see it, and it has to stop reporting once the writes succeed again, or the warning
    becomes background noise an operator learns to scroll past.
    """
    print("\ntest_write_failures_are_reported_then_clear")
    # Only the failure logic is under test; the radio probes shell out to iw and hcitool.
    rt.pipeline_counters["enqueue_failures"] = 0
    rt.pipeline_counters["status_write_failures"] = 0
    rt.reported_failures.update(enqueue_failures=0, status_write_failures=0)
    original_position = rt.configured_position
    rt.configured_position = lambda: None
    rt.radio_state["ble_stream"] = None
    rt.radio_state["ble_mode"] = "extended"
    original_wifi = rt.WIFI_INTERFACE
    rt.WIFI_INTERFACE = None

    def problems():
        return rt.collect_radio_status()["problems"]

    try:
        quiet = problems()
        ok = check("a healthy receiver reports no write faults",
                   "detections_not_queued" not in quiet, str(quiet))

        rt.pipeline_counters["enqueue_failures"] = 3
        first = problems()
        ok &= check("a failing enqueue is reported",
                    "detections_not_queued" in first, str(first))

        rt.pipeline_counters["enqueue_failures"] = 9
        during = problems()
        ok &= check("it keeps being reported while it keeps failing",
                    "detections_not_queued" in during, str(during))

        recovered = problems()
        ok &= check("it clears once writes succeed again",
                    "detections_not_queued" not in recovered, str(recovered))

        rt.pipeline_counters["status_write_failures"] = 1
        status_fault = rt.collect_radio_status()
        ok &= check("an unwritable status file is its own fault",
                    "status_file_unwritable" in status_fault["problems"],
                    str(status_fault["problems"]))
        ok &= check("the counts travel with the heartbeat",
                    status_fault.get("enqueue_failures") == 9
                    and status_fault.get("status_write_failures") == 1,
                    f"{status_fault.get('enqueue_failures')}, "
                    f"{status_fault.get('status_write_failures')}")
        return ok
    finally:
        rt.configured_position = original_position
        rt.WIFI_INTERFACE = original_wifi


def test_rejected_releases_are_reported():
    """A release this receiver refused has to reach the heartbeat.

    The receiver is healthy after a rollback — it is running the last version that
    worked — so this is deliberately not a fault and must not make the device look
    degraded. It is news about the release, and it is only actionable centrally: one
    receiver refusing a version is a curiosity, thirty refusing the same one is the
    only warning that a release should be withdrawn.
    """
    print("\ntest_rejected_releases_are_reported")
    original_position = rt.configured_position
    original_wifi = rt.WIFI_INTERFACE
    original_rejected = rt.REJECTED_FILE
    rt.configured_position = lambda: None
    rt.WIFI_INTERFACE = None
    rt.radio_state["ble_stream"] = None
    rt.radio_state["ble_mode"] = "extended"
    directory = tempfile.mkdtemp()
    rt.REJECTED_FILE = os.path.join(directory, "rejected")

    try:
        status = rt.collect_radio_status()
        ok = check("no file means nothing rejected", status.get("rejected_releases") == [],
                   str(status.get("rejected_releases")))

        with open(rt.REJECTED_FILE, "w") as handle:
            handle.write("v1.3.0  # rejected 2026-08-16T02:14:03-05:00\n")
            handle.write("\n")
            handle.write("v1.4.0 # rejected later\n")
        status = rt.collect_radio_status()
        ok &= check("both versions are reported, comments stripped",
                    status.get("rejected_releases") == ["v1.3.0", "v1.4.0"],
                    str(status.get("rejected_releases")))
        ok &= check("a rejected release is not reported as a fault",
                    "detections_not_queued" not in status["problems"]
                    and all("reject" not in p for p in status["problems"]),
                    str(status["problems"]))

        rt.REJECTED_FILE = os.path.join(directory, "does-not-exist")
        status = rt.collect_radio_status()
        ok &= check("an unreadable file is not an error",
                    status.get("rejected_releases") == [],
                    str(status.get("rejected_releases")))
        return ok
    finally:
        rt.configured_position = original_position
        rt.WIFI_INTERFACE = original_wifi
        rt.REJECTED_FILE = original_rejected


def test_frames_are_counted_whether_or_not_they_decode():
    """The counter has to separate a dead radio from an empty sky.

    It used to be incremented only after a Remote ID message had already been
    extracted, which made frames_seen an exact copy of messages_decoded: both silences
    read as zero. On 2026-08-15 a receiver sat at zero for seventeen hours while every
    other signal reported healthy. Remote ID is rare and ordinary 2.4 GHz traffic is
    not, so a live radio must show frames climbing even when nothing is flying.
    """
    print("\nframes are counted whether or not they decode")
    rt.pipeline_counters["frames_seen"].clear()
    rt.pipeline_counters["messages_decoded"].clear()
    rt.pipeline_counters["last_frame_at"].clear()

    for _ in range(201):
        rt.count_frame("Wi-Fi")                    # every frame the radio delivers
    rt.count_decoded("Wi-Fi")                      # one of them carried Remote ID

    seen = rt.pipeline_counters["frames_seen"].get("Wi-Fi")
    decoded = rt.pipeline_counters["messages_decoded"].get("Wi-Fi")
    ok = check("a frame that decodes nothing is still counted", seen == 201, str(seen))
    ok &= check("only the decoding one counts as decoded", decoded == 1, str(decoded))
    ok &= check("a decoded frame is not counted twice as a frame", seen == 201, str(seen))
    ok &= check("the two counters can now disagree, which is the point",
                seen != decoded, f"{seen} vs {decoded}")

    # A radio that has stopped is the case this must still report honestly.
    rt.pipeline_counters["frames_seen"].clear()
    ok &= check("a silent radio reports no frames at all",
                rt.pipeline_counters["frames_seen"].get("Wi-Fi") is None)
    return ok


def test_the_status_file_still_publishes_iso_timestamps():
    """last_frame_at is an epoch float on the hot path and must not leak that shape.

    strftime on every frame is real work several hundred times a second for a value
    nobody reads until the status file is written. Anything parsing status.json still
    expects the ISO string it has always had.
    """
    print("\nthe status file keeps its published shape")
    directory = tempfile.mkdtemp()
    original = rt.STATUS_FILE
    rt.STATUS_FILE = os.path.join(directory, "status.json")
    rt.pipeline_counters["last_frame_at"].clear()
    rt.count_frame("BLE")
    rt.count_decoded("BLE")
    try:
        rt.write_status_file({"problems": []})
        with open(rt.STATUS_FILE) as handle:
            published = json.load(handle)
        stamp = published["pipeline"]["last_frame_at"].get("BLE")
        ok = check("last_frame_at is published as a string", isinstance(stamp, str), repr(stamp))
        ok &= check("and it looks like an ISO timestamp",
                    isinstance(stamp, str) and stamp.endswith("Z") and "T" in stamp,
                    repr(stamp))
        ok &= check("frames_seen is published too",
                    published["pipeline"]["frames_seen"].get("BLE") == 1,
                    str(published["pipeline"]["frames_seen"]))
        return ok
    finally:
        rt.STATUS_FILE = original


def test_the_row_is_stamped_when_its_position_was_heard():
    """A contact is logged once per cooldown, carrying the telemetry it accumulated over
    the whole pass — so its position is the last one heard, not the first.

    Stamping that with the contact's first frame paired a coordinate from the end of a
    pass with a clock reading from the start of it: the row claimed the aircraft was
    somewhere it would not reach for several more seconds.

    Invisible on a receiver working alone, because nothing contradicts it. It showed up
    where a second receiver reported the same passes per advertisement — those rows sat
    40-68% of the way through by time but 50-138% of the way by position, two of them
    past the far end of the other receiver's track. The drawn line jumped forward to the
    stray row and back, twice per row.
    """
    print("a row is stamped when its position was heard")
    from datetime import datetime, timezone
    isolate()
    # The grace period has to outlast the movement below, or the row is written before
    # the second position ever arrives and the test proves nothing.
    rt.PENDING_ALERT_GRACE_SECONDS = 1.0
    rt.ALERT_COOLDOWN = 30
    mac = "CC:F9:57:9E:72:99"

    stamps = []
    rt.enqueue_detection = lambda _mac, _proto, _tel, detected_at, *a, **k: stamps.append(detected_at)

    started = time.time()
    rt.register_detection(mac, {"uas_id": "1786501099", "lat": 33.000, "lon": -96.650},
                          "BLE", 1, -88)
    time.sleep(0.4)
    rt.register_detection(mac, {"uas_id": "1786501099", "lat": 33.010, "lon": -96.660},
                          "BLE", 1, -87)
    moved_at = time.time()
    time.sleep(1.0)

    ok = check("the pass is recorded once", len(stamps) == 1, f"{len(stamps)} rows")
    if not stamps:
        return False
    stamped = datetime.strptime(stamps[0], '%Y-%m-%dT%H:%M:%S.%fZ').replace(tzinfo=timezone.utc)
    ok &= check("it is not stamped when the contact began",
                stamped.timestamp() - started > 0.3,
                f"{stamped.timestamp() - started:.2f}s after the first frame")
    ok &= check("it is stamped when the position it carries was heard",
                abs(stamped.timestamp() - moved_at) < 0.35,
                f"{abs(stamped.timestamp() - moved_at):.2f}s from the last position")
    return ok


TESTS = [
    test_the_row_is_stamped_when_its_position_was_heard,
    test_rekey_during_grace_period,
    test_expired_cooldown_does_not_suppress,
    test_cooldown_still_suppresses_while_live,
    test_both_transports_are_credited,
    test_concurrent_transports_do_not_corrupt_state,
    test_write_failures_are_reported_then_clear,
    test_rejected_releases_are_reported,
    test_frames_are_counted_whether_or_not_they_decode,
    test_the_status_file_still_publishes_iso_timestamps,
]


if __name__ == "__main__":
    # Naming one runs just that one. CI runs them as separate steps so a failure says
    # which test failed in the step name alone — the run log needs credentials to
    # download, and "exit code 1" with no visible assertion is not a bug report.
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
