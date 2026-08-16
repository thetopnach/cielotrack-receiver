"""Builds the language-neutral conformance vectors in tests/odid_vectors.json.

Why these exist: the decoder is about to have a second implementation, in C on an
ESP32, and a field comparison between two receivers is meaningless if they disagree
about what a message says. Vectors let the second implementation be checked on a desk,
against exactly the cases the first one is checked against, before any hardware exists.
They are also useful to anyone writing their own tracker against our API.

How to trust them. Each case states the intended field values *independently* — a
drone at 47.5 m above takeoff, a geodetic altitude of 219.5 m, and so on — encodes a
message from those values per the spec's own scaling, and states what a decoder ought
to return. This script then checks odid_decode agrees before writing anything out. So a
vector records agreement between an independent statement of intent and the shipped
decoder, not merely whatever the decoder happens to do.

    python3 tests/build_odid_vectors.py          # verify, and rewrite the fixture
    python3 tests/build_odid_vectors.py --check  # verify only, change nothing
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import odid_decode

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "odid_vectors.json")

# The spec stores altitudes in 0.5 m steps offset by 1000 m, so 0 means -1000 m and is
# also used as "no value". That collision is why a height of exactly -1000 m cannot be
# expressed, and why zero has to mean absent.
def alt(metres):
    return int((metres + 1000) / 0.5)


def location(lat, lon, *, height_m=None, geodetic_m=None, barometric_m=None,
             above_ground=False, direction=90, west=False, speed_raw=40,
             speed_multiplier=False, vspeed_raw=0):
    msg = bytearray(25)
    msg[0] = 0x10                                    # message type 0x1
    byte1 = 0
    if speed_multiplier:
        byte1 |= 0x01
    if west:
        byte1 |= 0x02
    if above_ground:
        byte1 |= 0x04                                # height type: above ground
    msg[1] = byte1
    msg[2] = direction
    msg[3] = speed_raw
    struct.pack_into("<b", msg, 4, vspeed_raw)
    struct.pack_into("<ii", msg, 5, int(round(lat * 1e7)), int(round(lon * 1e7)))
    struct.pack_into("<HHH", msg, 13,
                     alt(barometric_m) if barometric_m is not None else 0,
                     alt(geodetic_m) if geodetic_m is not None else 0,
                     alt(height_m) if height_m is not None else 0)
    return bytes(msg)


def basic_id(uas_id, id_type=1, ua_type=2):
    msg = bytearray(25)
    msg[0] = 0x00                                    # message type 0x0
    msg[1] = (id_type << 4) | ua_type
    encoded = uas_id.encode("ascii")
    msg[2:2 + len(encoded)] = encoded
    return bytes(msg)


def system(op_lat, op_lon, op_alt_m=None, location_type=1):
    msg = bytearray(25)
    msg[0] = 0x40                                    # message type 0x4
    msg[1] = location_type & 0x03
    struct.pack_into("<ii", msg, 2, int(round(op_lat * 1e7)), int(round(op_lon * 1e7)))
    struct.pack_into("<H", msg, 18, alt(op_alt_m) if op_alt_m is not None else 0)
    return bytes(msg)


# Coordinates here are deliberately arbitrary and not anywhere in particular: a fixture
# that ships in a public repository should not carry someone's address.
LAT, LON = 10.0, 20.0

CASES = [
    {
        "name": "height and geodetic altitude are both present",
        "why": "An aircraft broadcasts both. Reporting one and discarding the other "
               "loses the number that answers the other question.",
        "decoder": "location",
        "message": location(LAT, LON, height_m=47.5, geodetic_m=219.5),
        "expect": {"lat": 10.0, "lon": 20.0,
                   "altitude_m": 47.5, "altitude_ref": "agl",
                   "height_m": 47.5, "height_ref": "takeoff",
                   "speed_mps": 10.0, "vspeed_mps": 0.0, "direction_deg": 90},
    },
    {
        "name": "height type flag says above ground",
        "why": "Bit 2 of byte 1 separates above-takeoff from above-ground, and the two "
               "differ by the whole climb of a launch from a rooftop.",
        "decoder": "location",
        "message": location(LAT, LON, height_m=47.5, geodetic_m=219.5, above_ground=True),
        "expect": {"lat": 10.0, "lon": 20.0,
                   "altitude_m": 47.5, "altitude_ref": "agl",
                   "height_m": 47.5, "height_ref": "ground",
                   "speed_mps": 10.0, "vspeed_mps": 0.0, "direction_deg": 90},
    },
    {
        "name": "no height broadcast, geodetic only",
        "why": "Wing and Zipline send the no-value sentinel for height on moving "
               "aircraft. Substituting absolute altitude silently made them look like "
               "they cruise hundreds of feet higher than everyone else.",
        "decoder": "location",
        "message": location(LAT, LON, geodetic_m=219.5),
        "expect": {"lat": 10.0, "lon": 20.0,
                   "altitude_m": 219.5, "altitude_ref": "absolute",
                   "height_m": "N/A", "height_ref": "N/A",
                   "speed_mps": 10.0, "vspeed_mps": 0.0, "direction_deg": 90},
    },
    {
        "name": "barometric altitude only",
        "why": "Falls back to barometric when geodetic is absent, and still calls it "
               "absolute rather than implying it is height above ground.",
        "decoder": "location",
        "message": location(LAT, LON, barometric_m=180.0),
        "expect": {"lat": 10.0, "lon": 20.0,
                   "altitude_m": 180.0, "altitude_ref": "absolute",
                   "height_m": "N/A", "height_ref": "N/A",
                   "speed_mps": 10.0, "vspeed_mps": 0.0, "direction_deg": 90},
    },
    {
        "name": "no altitude of any kind",
        "why": "A position with no vertical information must not report a made-up one.",
        "decoder": "location",
        "message": location(LAT, LON),
        "expect": {"lat": 10.0, "lon": 20.0,
                   "altitude_m": "N/A", "altitude_ref": "N/A",
                   "height_m": "N/A", "height_ref": "N/A",
                   "speed_mps": 10.0, "vspeed_mps": 0.0, "direction_deg": 90},
    },
    {
        "name": "below sea level is a real altitude",
        "why": "Death Valley and the Dead Sea exist. A negative geodetic altitude is "
               "data, not an error.",
        "decoder": "location",
        "message": location(LAT, LON, geodetic_m=-60.0),
        "expect": {"lat": 10.0, "lon": 20.0,
                   "altitude_m": -60.0, "altitude_ref": "absolute",
                   "height_m": "N/A", "height_ref": "N/A",
                   "speed_mps": 10.0, "vspeed_mps": 0.0, "direction_deg": 90},
    },
    {
        "name": "westerly heading adds 180 degrees",
        "why": "Direction is stored in one byte, so the east/west bit carries the "
               "other half of the circle.",
        "decoder": "location",
        "message": location(LAT, LON, height_m=30.0, direction=45, west=True),
        "expect": {"lat": 10.0, "lon": 20.0,
                   "altitude_m": 30.0, "altitude_ref": "agl",
                   "height_m": 30.0, "height_ref": "takeoff",
                   "speed_mps": 10.0, "vspeed_mps": 0.0, "direction_deg": 225},
    },
    {
        "name": "the speed multiplier changes the scale",
        "why": "Above 63.75 m/s the encoding switches scale. Ignoring the bit "
               "understates a fast aircraft by a factor that grows with its speed.",
        "decoder": "location",
        "message": location(LAT, LON, height_m=30.0, speed_raw=40, speed_multiplier=True),
        "expect": {"lat": 10.0, "lon": 20.0,
                   "altitude_m": 30.0, "altitude_ref": "agl",
                   "height_m": 30.0, "height_ref": "takeoff",
                   "speed_mps": 93.8, "vspeed_mps": 0.0, "direction_deg": 90},
    },
    {
        "name": "descending aircraft has negative vertical speed",
        "why": "Vertical speed is signed. Reading it unsigned turns a descent into a "
               "climb of about 64 m/s.",
        "decoder": "location",
        "message": location(LAT, LON, height_m=30.0, vspeed_raw=-6),
        "expect": {"lat": 10.0, "lon": 20.0,
                   "altitude_m": 30.0, "altitude_ref": "agl",
                   "height_m": 30.0, "height_ref": "takeoff",
                   "speed_mps": 10.0, "vspeed_mps": -3.0, "direction_deg": 90},
    },
    {
        "name": "null island is the no-position sentinel",
        "why": "0,0 means the aircraft has no position fix, not that it is in the "
               "Atlantic. Plotting it puts phantom aircraft off the coast of Africa.",
        "decoder": "location",
        "message": location(0.0, 0.0, height_m=47.5),
        "expect": {},
    },
    {
        "name": "southern and western hemispheres",
        "why": "Coordinates are signed 32-bit. Reading them unsigned puts an aircraft "
               "on the wrong side of the planet.",
        "decoder": "location",
        "message": location(-33.8688, -151.2093, height_m=100.0),
        "expect": {"lat": -33.8688, "lon": -151.2093,
                   "altitude_m": 100.0, "altitude_ref": "agl",
                   "height_m": 100.0, "height_ref": "takeoff",
                   "speed_mps": 10.0, "vspeed_mps": 0.0, "direction_deg": 90},
    },
    {
        "name": "basic id carries the serial and airframe class",
        "why": "This is the only message that says which aircraft the rest belongs to.",
        "decoder": "basic_id",
        "message": basic_id("1786501045", id_type=1, ua_type=2),
        "expect": {"uas_id": "1786501045"},
    },
    {
        "name": "basic id with no serial",
        "why": "An all-zero id field is absent, not an empty-string aircraft.",
        "decoder": "basic_id",
        "message": basic_id("", id_type=0, ua_type=0),
        "expect": {"uas_id": "N/A"},
    },
    {
        "name": "system message locates the operator",
        "why": "The operator position is a different thing from the aircraft position, "
               "and the location type says what it actually means.",
        "decoder": "system",
        "message": system(10.5, 20.5, op_alt_m=158.0, location_type=1),
        "expect": {"operator_lat": 10.5, "operator_lon": 20.5,
                   "operator_altitude_m": 158.0},
    },
    {
        "name": "system message with no operator position",
        "why": "Same 0,0 sentinel as the aircraft position. A delivery drone flying "
               "from a nest reports this often.",
        "decoder": "system",
        "message": system(0.0, 0.0, op_alt_m=158.0),
        "expect": {},
    },
]

DECODERS = {
    "location": odid_decode.decode_location,
    "basic_id": odid_decode.decode_basic_id,
    "system": odid_decode.decode_system,
}


def close_enough(expected, actual):
    if isinstance(expected, float) and isinstance(actual, (int, float)):
        return abs(expected - actual) < 1e-6
    return expected == actual


def verify(case):
    """Returns a list of disagreements between the stated intent and the decoder."""
    decoded = DECODERS[case["decoder"]](case["message"])
    problems = []
    if case["expect"] == {}:
        if decoded != {}:
            problems.append(f"expected nothing decoded, got {decoded}")
        return problems
    for field, expected in case["expect"].items():
        actual = decoded.get(field)
        if not close_enough(expected, actual):
            problems.append(f"{field}: expected {expected!r}, decoder gave {actual!r}")
    return problems


def main():
    check_only = "--check" in sys.argv
    failures = 0
    out = []
    for case in CASES:
        problems = verify(case)
        status = "ok" if not problems else "MISMATCH"
        print(f"  {status:<9} {case['name']}")
        for problem in problems:
            print(f"              {problem}")
            failures += 1
        out.append({
            "name": case["name"],
            "why": case["why"],
            "decoder": case["decoder"],
            "message_hex": case["message"].hex(),
            "expect": case["expect"],
        })

    print(f"\n  {len(CASES)} cases, {failures} disagreements")
    if failures:
        print("  refusing to write the fixture while the decoder disagrees with it")
        return 1
    if check_only:
        print("  --check given, fixture not rewritten")
        return 0

    document = {
        "note": ("Conformance vectors for ASTM F3411 / Open Drone ID message decoding. "
                 "Each case is a 25-byte message in hex and the fields a decoder should "
                 "produce from it. Intended for checking a second implementation — an "
                 "ESP32 in C, or your own tracker — against the same cases the Python "
                 "decoder is checked against. Absent values are the string \"N/A\"; an "
                 "empty expect object means the message carries no usable data."),
        "generated_by": "tests/build_odid_vectors.py",
        "cases": out,
    }
    with open(FIXTURE, "w") as handle:
        json.dump(document, handle, indent=2)
        handle.write("\n")
    print(f"  wrote {FIXTURE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
