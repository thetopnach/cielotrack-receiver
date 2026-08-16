"""Checks the decoder against the shipped conformance vectors, and the vectors
against a wrong decoder.

The second half is the part that matters. Vectors exist so a second implementation —
the ESP32 in C, or someone's own tracker — can be checked without hardware. Vectors
that a plausibly-wrong implementation would also pass are worse than none, because
they produce confidence rather than coverage. So this builds a decoder making the four
mistakes people actually make with this format, and requires the fixture to reject it.

Run directly — no test framework required:

    python3 tests/test_odid_vectors.py
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

import odid_decode

FIXTURE = os.path.join(HERE, "odid_vectors.json")
results = []


def check(name, condition, detail=""):
    """Detail is shown only on failure. Several of these read as failure messages, and
    printing them next to PASS makes the output argue with itself."""
    print(f"  {'PASS' if condition else 'FAIL'}  {name}"
          f"{'  - ' + detail if detail and not condition else ''}")
    results.append(bool(condition))
    return bool(condition)


def load():
    with open(FIXTURE) as handle:
        return json.load(handle)


def close_enough(expected, actual):
    if isinstance(expected, float) and isinstance(actual, (int, float)):
        return abs(expected - actual) < 1e-6
    return expected == actual


def run(decoder_name, message, decoders):
    return decoders[decoder_name](message)


REAL = {
    "location": odid_decode.decode_location,
    "basic_id": odid_decode.decode_basic_id,
    "system": odid_decode.decode_system,
}


def naive_location(msg):
    """A decoder making the four mistakes this format invites.

    Unsigned coordinates, the height-type bit ignored, the speed multiplier ignored,
    and 0,0 treated as a real position. Every one of these produces plausible-looking
    output — which is exactly why a fixture has to catch them.
    """
    byte1 = msg[1]
    direction_enc = msg[2]
    speed_h_enc = msg[3]
    speed_v_enc = struct.unpack("b", msg[4:5])[0]
    lat_enc, lon_enc = struct.unpack("<II", msg[5:13])      # unsigned: mistake 1
    alt_baro_enc, alt_geo_enc, height_enc = struct.unpack("<HHH", msg[13:19])
    altitude_m, altitude_ref = None, "N/A"
    if height_enc:
        altitude_m, altitude_ref = height_enc * 0.5 - 1000, "agl"
    elif alt_geo_enc or alt_baro_enc:
        altitude_m = (alt_geo_enc or alt_baro_enc) * 0.5 - 1000
        altitude_ref = "absolute"
    return {                                                 # no 0,0 check: mistake 4
        "lat": round(lat_enc / 1e7, 7),
        "lon": round(lon_enc / 1e7, 7),
        "altitude_m": round(altitude_m, 1) if altitude_m is not None else "N/A",
        "altitude_ref": altitude_ref,
        "height_m": round(height_enc * 0.5 - 1000, 1) if height_enc else "N/A",
        "height_ref": "takeoff" if height_enc else "N/A",    # flag ignored: mistake 2
        "speed_mps": round(speed_h_enc * 0.25, 1),           # multiplier ignored: 3
        "vspeed_mps": round(speed_v_enc * 0.5, 1),
        "direction_deg": direction_enc + (180 if (byte1 >> 1) & 0x01 else 0),
    }


NAIVE = dict(REAL, location=naive_location)


def disagreements(case, decoders):
    message = bytes.fromhex(case["message_hex"])
    decoded = run(case["decoder"], message, decoders)
    if case["expect"] == {}:
        return [] if decoded == {} else [f"expected nothing, got {decoded}"]
    bad = []
    for field, expected in case["expect"].items():
        actual = decoded.get(field)
        if not close_enough(expected, actual):
            bad.append(f"{field}: expected {expected!r}, got {actual!r}")
    return bad


def test_the_fixture_is_well_formed():
    print("\nthe fixture itself")
    document = load()
    cases = document.get("cases", [])
    ok = check(f"it has cases ({len(cases)})", len(cases) >= 10)
    ok &= check("every case explains why it exists",
                all(c.get("why") for c in cases),
                str([c["name"] for c in cases if not c.get("why")]))
    ok &= check("every message is a 25-byte message",
                all(len(bytes.fromhex(c["message_hex"])) == 25 for c in cases))
    ok &= check("every case names a known decoder",
                all(c["decoder"] in REAL for c in cases))
    ok &= check("it carries no real coordinates",
                not any(str(c["expect"].get("lat", "")).startswith("32.9")
                        for c in cases),
                "a fixture in a public repo must not carry someone's address")
    return ok


def test_the_shipped_decoder_conforms():
    print("\nthe shipped decoder against the vectors")
    ok = True
    for case in load()["cases"]:
        bad = disagreements(case, REAL)
        ok &= check(case["name"], not bad, "; ".join(bad))
    return ok


def test_the_vectors_reject_a_wrong_decoder():
    """Each classic mistake must be caught by at least one vector."""
    print("\nthe vectors against a plausibly-wrong decoder")
    caught = {}
    for case in load()["cases"]:
        if case["decoder"] != "location":
            continue
        if disagreements(case, NAIVE):
            caught[case["name"]] = True

    ok = check(f"the wrong decoder is rejected by {len(caught)} vectors", bool(caught))
    # And specifically, each mistake has a vector aimed at it.
    for mistake, name in [
        ("signed coordinates", "southern and western hemispheres"),
        ("height type flag", "height type flag says above ground"),
        ("speed multiplier", "the speed multiplier changes the scale"),
        ("no-position sentinel", "null island is the no-position sentinel"),
    ]:
        ok &= check(f"a vector catches: {mistake}", name in caught,
                    f"'{name}' did not reject the wrong decoder")
    return ok


TESTS = [
    test_the_fixture_is_well_formed,
    test_the_shipped_decoder_conforms,
    test_the_vectors_reject_a_wrong_decoder,
]


if __name__ == "__main__":
    if len(sys.argv) > 1:
        wanted = {n.lstrip("-").replace("-", "_") for n in sys.argv[1:]}
        chosen = [t for t in TESTS if t.__name__ in wanted
                  or t.__name__.removeprefix("test_") in wanted]
        if not chosen:
            print(f"no test matches {sorted(wanted)}; known tests:")
            for t in TESTS:
                print(f"  {t.__name__.removeprefix('test_')}")
            sys.exit(2)
    else:
        chosen = TESTS
    outcomes = [t() for t in chosen]
    print(f"\n{sum(outcomes)}/{len(outcomes)} passed")
    sys.exit(0 if all(outcomes) else 1)
