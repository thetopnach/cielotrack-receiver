"""The claim request carries a contact address only when the operator opted in.

CONTACT_EMAIL is an optional reminder address: when set, the receiver includes it in its
registration so the server can email the claim code; when blank it must be absent from the
body entirely, so a receiver that gave no address is never mailed. The address is never a
claim credential — this only checks what leaves the box.

Run directly — no test framework required:

    python3 tests/test_receiver_claim.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
os.environ.setdefault("CENTRAL_SERVER_URL", "http://127.0.0.1:9")

import radio_tracker as rt


def check(name, condition, detail=""):
    print(f"  {'PASS' if condition else 'FAIL'}  {name}{'  — ' + detail if detail else ''}")
    return bool(condition)


def capture_claim_body(contact_email):
    """Run one ensure_claimed cycle with the network stubbed, return the POST body."""
    creds = {"device_id": "dev-1", "bootstrap_secret": "sekret", "api_key": None}
    calls = []

    def fake_request(method, path, body=None, headers=None):
        calls.append((method, path, body))
        # Reply as an unclaimed device so ensure_claimed prints the code and stops,
        # without trying to persist an api_key.
        return {"status": "unclaimed", "claim_code": "123-456"}

    original_request, original_email = rt._central_request, rt.CONTACT_EMAIL
    try:
        rt._central_request = fake_request
        rt.CONTACT_EMAIL = contact_email
        rt.ensure_claimed(creds)
    finally:
        rt._central_request, rt.CONTACT_EMAIL = original_request, original_email

    post = next(b for m, p, b in calls if m == "POST" and p.endswith("/claim"))
    return post


def test_address_is_sent_when_set():
    body = capture_claim_body("pilot@example.com")
    return (check("device identity is always sent",
                  body.get("device_id") == "dev-1" and body.get("bootstrap_secret") == "sekret")
            & check("the opted-in address rides along", body.get("contact_email") == "pilot@example.com"))


def test_address_is_absent_when_unset():
    # CONTACT_EMAIL is env_or(...).strip() at load, so "unset" and whitespace both arrive
    # here as "" — the empty string is what ensure_claimed must omit from the body.
    empty = capture_claim_body("")
    return check("no contact_email key when unset", "contact_email" not in empty, str(empty))


TESTS = [
    test_address_is_sent_when_set,
    test_address_is_absent_when_unset,
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
