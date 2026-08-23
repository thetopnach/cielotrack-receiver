"""Runs the C and Python decoders over random messages and requires them to agree.

The conformance vectors cover the cases someone thought of. This covers the ones
nobody did, which is where a port quietly diverges: a sign bit, a rounding rule, an
offset off by one. Every one of those produces output that looks entirely reasonable
on its own and only shows up as two receivers disagreeing about the same aircraft —
by which point it looks like a reception difference, which is exactly the confusion
this whole exercise exists to remove.

Needs the C binary built first:

    make -C firmware test && python3 tests/test_c_matches_python.py
"""
import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

import odid_decode

CLI = os.path.join(HERE, "..", "firmware", "build", "decode_cli")
ROUNDS = int(os.environ.get("FUZZ_ROUNDS", "3000"))


def c_decode(message):
    result = subprocess.run([CLI, message.hex()], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip())
    fields = {}
    for line in result.stdout.splitlines():
        if line == "empty":
            return {}
        if line in ("unsupported",):
            return None
        key, _, value = line.partition(" ")
        fields[key] = value
    return fields


def python_decode(message):
    kind = message[0] >> 4
    if kind == 0x1:
        out = odid_decode.decode_location(message)
    elif kind == 0x0:
        out = odid_decode.decode_basic_id(message)
    elif kind == 0x4:
        out = odid_decode.decode_system(message)
    else:
        return None
    return out


def normalise(value):
    """Both sides print numbers; compare them as numbers so 10.0 and 10 agree."""
    if isinstance(value, (int, float)):
        return f"{float(value):.7f}"
    text = str(value)
    try:
        return f"{float(text):.7f}"
    except ValueError:
        return text


COMPARED = {
    0x1: ("lat", "lon", "altitude_m", "altitude_ref", "height_m", "height_ref",
          "speed_mps", "vspeed_mps", "direction_deg"),
    0x0: ("uas_id",),
    0x4: ("operator_lat", "operator_lon", "operator_altitude_m"),
}


def random_message(rng):
    kind = rng.choice([0x0, 0x1, 0x1, 0x1, 0x4])      # weighted to Location
    body = bytearray(rng.randbytes(25))
    body[0] = kind << 4
    if kind == 0x0:
        # Serials are ASCII on the wire; random bytes would only test the decoder's
        # tolerance for garbage, not its agreement about real ids.
        text = "".join(rng.choice("0123456789ABCDEF") for _ in range(rng.randint(0, 20)))
        raw = text.encode()
        body[2:22] = raw + b"\x00" * (20 - len(raw))
    return bytes(body)


def label_mismatches():
    """The airframe class as a name, not a number.

    The boards reported ua_type as a bare enum value while every other receiver in the
    fleet reported a label, so one row read "4" and its neighbour "Hybrid Lift" for the
    same aircraft. Two conventions out of one decoder is the exact failure that
    compiling the Pi's decoder into the firmware was meant to prevent, and nothing was
    checking it.
    """
    from odid_decode import ODID_UA_TYPE_LABELS

    binary = os.path.join(HERE, "..", "firmware", "build", "label_cli")
    if not os.path.exists(binary):
        return [f"{binary} not built — run: make -C firmware test"]

    out = subprocess.run([binary], capture_output=True, text=True, check=True).stdout
    c_labels = {}
    for line in out.strip().splitlines():
        value, label = line.split("\t", 1)
        c_labels[int(value)] = label

    problems = []
    for value, expected in ODID_UA_TYPE_LABELS.items():
        if c_labels.get(value) != expected:
            problems.append(f"{value}: C={c_labels.get(value)!r} Python={expected!r}")

    # The spec defines 0-15; anything above has to keep the number visible rather than
    # being folded silently into a named class.
    for value in (16, 200, 255):
        if c_labels.get(value) != f"Type {value}":
            problems.append(f"{value}: C={c_labels.get(value)!r} expected 'Type {value}'")
    return problems


def main():
    if not os.path.exists(CLI):
        print(f"  {CLI} not built — run: make -C firmware test")
        return 2

    rng = random.Random(20260816)
    disagreements = []
    compared = 0

    for _ in range(ROUNDS):
        message = random_message(rng)
        kind = message[0] >> 4
        expected = python_decode(message)
        actual = c_decode(message)
        if expected is None or actual is None:
            continue
        if not expected:
            if actual:
                disagreements.append((message.hex(), "python decoded nothing, C decoded",
                                      str(actual)[:60]))
            continue
        if not actual:
            disagreements.append((message.hex(), "C decoded nothing, python decoded",
                                  str(expected)[:60]))
            continue
        for field in COMPARED[kind]:
            if field not in expected:
                continue
            compared += 1
            want, got = normalise(expected[field]), normalise(actual.get(field, "<missing>"))
            if want != got:
                disagreements.append((message.hex(), field, f"python {want} vs C {got}"))

    print(f"  {ROUNDS} random messages, {compared} field comparisons")
    if disagreements:
        print(f"  {len(disagreements)} disagreements; first few:")
        for hexed, field, detail in disagreements[:5]:
            print(f"    {hexed}")
            print(f"      {field}: {detail}")
        return 1
    print("  the C and Python decoders agree on every field of every message")

    problems = label_mismatches()
    if problems:
        print(f"  {len(problems)} UA type label disagreement(s):")
        for problem in problems[:5]:
            print(f"    {problem}")
        return 1
    print("  the C and Python UA type labels are identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())

