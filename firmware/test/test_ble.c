/* Extraction tests. The decoder is only correct if it is handed the right bytes, and
 * this is the half that decides which bytes those are. */
#include <stdio.h>
#include <string.h>

#include "../odid_ble.h"

static int passed, failed;

static void check(const char *name, bool ok, const char *detail) {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name);
    if (!ok && detail) printf("  - %s", detail);
    printf("\n");
    ok ? passed++ : failed++;
}

int main(void) {
    uint8_t out[ODID_MESSAGE_SIZE];
    printf("BLE advertisement extraction\n\n");

    /* A realistic advertisement: flags element, then the ODID service data. */
    uint8_t advert[64];
    memset(advert, 0, sizeof advert);
    size_t at = 0;
    advert[at++] = 0x02; advert[at++] = 0x01; advert[at++] = 0x06;   /* flags element */
    advert[at++] = 0x1E; advert[at++] = 0x16;                        /* len, service data */
    advert[at++] = 0xFA; advert[at++] = 0xFF; advert[at++] = 0x0D;   /* signature */
    advert[at++] = 0x01;                                             /* message counter */
    size_t message_at = at;
    for (int i = 0; i < ODID_MESSAGE_SIZE; i++) advert[at++] = (uint8_t)(0x40 + i);

    check("a message after the signature is found",
          odid_ble_extract(advert, at, out), NULL);
    check("and it is the bytes after the counter, not the counter",
          memcmp(out, &advert[message_at], ODID_MESSAGE_SIZE) == 0, "wrong offset");

    /* The service data element is not always first, which is why this scans. */
    uint8_t offset_advert[80];
    memset(offset_advert, 0xAB, sizeof offset_advert);
    memcpy(&offset_advert[31], advert, at);
    check("the signature is found when it is not at the start",
          odid_ble_extract(offset_advert, 31 + at, out), NULL);
    check("and still yields the same message",
          memcmp(out, &advert[message_at], ODID_MESSAGE_SIZE) == 0, "wrong offset");

    /* A truncated tail must not be returned as a short message. */
    check("a truncated message is refused",
          !odid_ble_extract(advert, message_at + 10, out), "returned a partial message");

    /* A coincidental signature near the end must not stop the search. */
    uint8_t twice[128];
    memset(twice, 0x00, sizeof twice);
    twice[0] = 0xFA; twice[1] = 0xFF; twice[2] = 0x0D;      /* spurious, truncated */
    memcpy(&twice[4], advert, at);                          /* the real one follows */
    check("a spurious match does not end the search",
          odid_ble_extract(twice, 4 + at, out), "gave up on the first match");

    /* The envelope of an advertisement a radio really delivered, captured on
     * 2026-08-18: its length byte, service-data type, UUID, application code and
     * message counter, verbatim. The 25 bytes of payload are a fixture message rather
     * than the one that flew, because the real one carried the aircraft's position a
     * short distance from the receiver, and tests/test_odid_vectors.py refuses to let
     * this repository ship somebody's address.
     *
     * That trade costs nothing here. Extraction is about finding a message inside a
     * payload — element order, the length byte, where the counter sits, a full 31-byte
     * advertisement — and every one of those is real. What the message says is the
     * decoder's problem, and the vectors already hold it to that.
     *
     * The hand-built cases above stay: this frame is a tidy one, with the service data
     * element first, and they cover the shapes it happens not to have. */
    static const uint8_t from_the_air[] = {
        0x1e, 0x16, 0xfa, 0xff, 0x0d, 0xcc, 0x10, 0x00, 0x5a, 0x28, 0x00, 0x00, 0xe1,
        0xf5, 0x05, 0x00, 0xc2, 0xeb, 0x0b, 0x00, 0x00, 0x87, 0x09, 0x2f, 0x08, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t expected[ODID_MESSAGE_SIZE] = {
        0x10, 0x00, 0x5a, 0x28, 0x00, 0x00, 0xe1, 0xf5, 0x05, 0x00, 0xc2, 0xeb, 0x0b,
        0x00, 0x00, 0x87, 0x09, 0x2f, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    check("a real advertisement envelope yields its message",
          odid_ble_extract(from_the_air, sizeof from_the_air, out),
          "found nothing in a frame shaped like one that really carried Remote ID");
    check("and the message is the one it carried",
          memcmp(out, expected, ODID_MESSAGE_SIZE) == 0, "wrong bytes");

    uint8_t nothing[40];
    memset(nothing, 0x11, sizeof nothing);
    check("an advertisement with no Remote ID yields nothing",
          !odid_ble_extract(nothing, sizeof nothing, out), NULL);
    check("an empty buffer is handled", !odid_ble_extract(nothing, 0, out), NULL);
    check("a NULL buffer is handled", !odid_ble_extract(NULL, 30, out), NULL);

    printf("\n%d/%d passed\n", passed, passed + failed);
    return failed == 0 ? 0 : 1;
}
