"""The C and the Python must agree about what a Message Pack is.

Not just about decoding — about *validity*. Two implementations that decode identically
but disagree on which frames to accept produce one receiver quietly hearing less than
its sibling, with nothing in either log to say so. On Wi-Fi that matters more than on
BLE, because the pack is the normal framing rather than an option: a receiver that
rejects a pack rejects everything in it.

Random input on purpose. The conformance vectors cover the cases someone thought of;
this covers the ones nobody did — a count of zero, a size field that lies, a buffer that
stops mid-message.

Run directly — no test framework required:

    python3 tests/test_pack_matches_python.py
"""
import os
import random
import subprocess
import sys

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, REPO)

from odid_decode import (ODID_MESSAGE_SIZE, ODID_PACK_MAX_MESSAGES,
                         ODID_MESSAGETYPE_PACKED, decode_message_pack)

PACK_CLI = os.path.join(REPO, "firmware", "build", "pack_cli")


def check(name, condition, detail=""):
    print(f"  {'PASS' if condition else 'FAIL'}  {name}{'  — ' + detail if detail else ''}")
    return bool(condition)


def c_split(data):
    """(count, [message hex, ...]) as the C sees it."""
    out = subprocess.run([PACK_CLI, data.hex()], capture_output=True, text=True)
    if out.returncode != 0:
        return None, []
    lines = out.stdout.split()
    return int(lines[0]), lines[1:]


def python_count(data):
    """How many messages Python considers the pack to hold, by its own validity rules.

    decode_message_pack returns how many *decoded*, which is a different question — a
    pack of nine Auth messages decodes none of them and is still a valid pack of nine.
    So validity is read from the same fields the C checks.
    """
    if len(data) < 3 or (data[0] >> 4) != ODID_MESSAGETYPE_PACKED:
        return 0
    size, count = data[1], data[2]
    if size != ODID_MESSAGE_SIZE or not (1 <= count <= ODID_PACK_MAX_MESSAGES):
        return 0
    if len(data) < 3 + count * size:
        return 0
    return count


def test_they_agree_on_random_input():
    print("\nthe two agree on random input")
    if not os.path.exists(PACK_CLI):
        return check("pack_cli is built", False, "run: make -C firmware test")
    random.seed(20260819)
    disagreements = []
    for _ in range(2000):
        shape = random.random()
        if shape < 0.5:
            # A plausible pack, sometimes with a field nudged out of range.
            count = random.randint(0, ODID_PACK_MAX_MESSAGES + 2)
            size = ODID_MESSAGE_SIZE if random.random() < 0.85 else random.randint(0, 40)
            body = bytes(random.getrandbits(8) for _ in range(count * ODID_MESSAGE_SIZE))
            data = bytes([ODID_MESSAGETYPE_PACKED << 4, size, count]) + body
            if random.random() < 0.2:            # truncate it somewhere
                data = data[:random.randint(0, len(data))]
        else:
            data = bytes(random.getrandbits(8) for _ in range(random.randint(0, 260)))
        theirs = python_count(data)
        ours, _ = c_split(data)
        if ours != theirs:
            disagreements.append((data.hex()[:60], theirs, ours))
    ok = check(f"2,000 inputs, {len(disagreements)} disagreements", not disagreements,
               str(disagreements[:2]))
    return ok


def test_the_messages_come_out_identical():
    print("\nand hand back the same messages")
    if not os.path.exists(PACK_CLI):
        return False
    random.seed(1)
    ok = True
    for _ in range(200):
        count = random.randint(1, ODID_PACK_MAX_MESSAGES)
        body = bytes(random.getrandbits(8) for _ in range(count * ODID_MESSAGE_SIZE))
        data = bytes([ODID_MESSAGETYPE_PACKED << 4, ODID_MESSAGE_SIZE, count]) + body
        n, messages = c_split(data)
        expected = [body[i * ODID_MESSAGE_SIZE:(i + 1) * ODID_MESSAGE_SIZE].hex()
                    for i in range(count)]
        if n != count or messages != expected:
            ok = False
            break
    return check("200 packs split into exactly the bytes they contained", ok)


def test_a_real_message_survives_the_round_trip():
    """A pack built from the conformance fixture must decode to the same fields the
    fixture asserts for that message on its own — being inside a pack changes nothing
    about what a message says."""
    print("\na real message decodes the same inside a pack as outside")
    import json
    cases = json.load(open(os.path.join(REPO, "tests", "odid_vectors.json")))["cases"]
    case = next(c for c in cases if c["decoder"] == "location" and c["expect"])
    message = bytes.fromhex(case["message_hex"])
    data = bytes([ODID_MESSAGETYPE_PACKED << 4, ODID_MESSAGE_SIZE, 1]) + message
    n, messages = c_split(data)
    ok = check("the pack holds one message", n == 1, str(n))
    ok &= check("and it is the fixture's message", messages == [message.hex()])
    merged, decoded = decode_message_pack(data)
    ok &= check("Python decodes it too", decoded == 1, str(decoded))
    for field, expected in case["expect"].items():
        if isinstance(expected, (int, float)) and not isinstance(expected, bool):
            ok &= check(f"  {field}", abs(merged.get(field, 1e9) - expected) < 1e-6,
                        f"{merged.get(field)!r} vs {expected!r}")
    return ok


TESTS = [
    test_they_agree_on_random_input,
    test_the_messages_come_out_identical,
    test_a_real_message_survives_the_round_trip,
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
