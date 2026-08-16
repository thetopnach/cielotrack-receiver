"""Tests for decoding the ASTM F3411 Location message.

Altitude is the part of this worth testing. A Location message carries three different
vertical numbers — barometric altitude, geodetic altitude, and height above takeoff or
ground — and they are not three ways of saying one thing. A drone 47.5 m over a field
that is itself 172 m above sea level broadcasts both 47.5 and 219.5, and only the first
is comparable to the 400 ft limit. Conflating them made some operators appear to cruise
at ~700 ft against others' ~185 ft.

Run directly — no test framework required:

    python3 tests/test_odid_decode.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import odid_decode

results = []


def check(name, condition, detail=""):
    print(f"  {'PASS' if condition else 'FAIL'}  {name}{'  - ' + detail if detail else ''}")
    results.append(bool(condition))
    return bool(condition)


def encode_altitude(metres):
    """The spec stores altitude in 0.5 m steps with a 1000 m offset."""
    return int((metres + 1000) / 0.5)


def location_message(height_m=None, geodetic_m=None, above_ground=False,
                     lat=32.99, lon=-96.68):
    msg = bytearray(25)
    msg[0] = 0x10                          # message type 0x1, Location
    msg[1] = 0x04 if above_ground else 0x00  # bit 2 is the height type flag
    msg[2] = 90                            # direction
    msg[3] = 40                            # horizontal speed
    msg[4] = 0                             # vertical speed
    struct.pack_into('<ii', msg, 5, int(lat * 1e7), int(lon * 1e7))
    struct.pack_into('<HHH', msg, 13,
                     0,
                     encode_altitude(geodetic_m) if geodetic_m is not None else 0,
                     encode_altitude(height_m) if height_m is not None else 0)
    return bytes(msg)


def test_height_and_altitude_are_both_reported():
    """The two numbers must both survive, since the aircraft broadcast both."""
    print("\nheight and absolute altitude are both reported")
    out = odid_decode.decode_location(location_message(height_m=47.5, geodetic_m=219.5))
    ok = check("height is reported", out.get("height_m") == 47.5, str(out.get("height_m")))
    ok &= check("height reference is above takeoff", out.get("height_ref") == "takeoff",
                str(out.get("height_ref")))
    # altitude_m keeps its existing meaning deliberately: consumers already branch on
    # altitude_ref, so changing which number it holds would move the ground under them.
    ok &= check("altitude_m keeps preferring the height, as before",
                out.get("altitude_ref") == "agl" and out.get("altitude_m") == 47.5,
                f"{out.get('altitude_ref')}, {out.get('altitude_m')}")
    return ok


def test_height_type_flag_is_read():
    print("\nthe height type flag distinguishes takeoff from ground")
    above_ground = odid_decode.decode_location(
        location_message(height_m=47.5, geodetic_m=219.5, above_ground=True))
    return check("above-ground height is labelled as such",
                 above_ground.get("height_ref") == "ground", str(above_ground.get("height_ref")))


def test_absolute_only_broadcast():
    """Wing and Zipline send the 'no value' sentinel for height on moving aircraft.

    Substituting absolute altitude silently is what made them look like they cruise
    several hundred feet higher than everyone else.
    """
    print("\nan aircraft broadcasting no height")
    out = odid_decode.decode_location(location_message(geodetic_m=219.5))
    ok = check("height is absent rather than invented", out.get("height_m") == "N/A",
               str(out.get("height_m")))
    ok &= check("altitude falls back to absolute, and says so",
                out.get("altitude_ref") == "absolute" and out.get("altitude_m") == 219.5,
                f"{out.get('altitude_ref')}, {out.get('altitude_m')}")
    return ok


def test_null_island_is_treated_as_no_data():
    """0,0 is the spec's 'no position' sentinel, not a real place in the Atlantic."""
    print("\nthe no-position sentinel is not a position")
    out = odid_decode.decode_location(location_message(height_m=47.5, lat=0.0, lon=0.0))
    return check("nothing is decoded from 0,0", out == {}, str(out))


def test_position_is_decoded():
    print("\nposition is decoded")
    out = odid_decode.decode_location(location_message(height_m=47.5, lat=32.99, lon=-96.68))
    ok = check("latitude", abs(out.get("lat", 0) - 32.99) < 1e-6, str(out.get("lat")))
    ok &= check("longitude", abs(out.get("lon", 0) - (-96.68)) < 1e-6, str(out.get("lon")))
    return ok


TESTS = [
    test_height_and_altitude_are_both_reported,
    test_height_type_flag_is_read,
    test_absolute_only_broadcast,
    test_null_island_is_treated_as_no_data,
    test_position_is_decoded,
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
