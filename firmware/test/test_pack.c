/* Message Packs: the framing Wi-Fi Remote ID actually uses.
 *
 * BLE rotates one message per broadcast, so the BLE-only firmware never needed this.
 * Wi-Fi puts up to nine in a single frame, which is why one Wi-Fi capture yields Basic
 * ID, Location and System together — and why a Wi-Fi receiver without pack support
 * decodes nothing at all rather than decoding less.
 *
 * The validation mirrors decode_message_pack in odid_decode.py check for check. Two
 * implementations of one format is a risk already; two that disagree about what counts
 * as valid is the version that has one receiver silently discarding frames its sibling
 * accepts, with nothing to show for it but a lower count.
 */
#include <stdio.h>
#include <string.h>

#include "../odid_decode.h"
#include "odid_vectors.h"

static int passed, failed;

static void check(const char *name, bool ok, const char *detail) {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name);
    if (!ok && detail) printf("  - %s", detail);
    printf("\n");
    if (ok) passed++; else failed++;
}

/* Builds a pack around `count` copies of real messages from the conformance fixture, so
 * what is being split is what aircraft actually broadcast rather than bytes invented
 * for the test. */
static size_t build_pack(uint8_t *out, size_t capacity, int count) {
    size_t needed = 3 + (size_t)count * ODID_MESSAGE_SIZE;
    if (capacity < needed) return 0;
    out[0] = ODID_MESSAGE_TYPE_PACKED << 4;
    out[1] = ODID_MESSAGE_SIZE;
    out[2] = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        memcpy(out + 3 + (size_t)i * ODID_MESSAGE_SIZE,
               odid_vectors[i % (int)(sizeof odid_vectors / sizeof odid_vectors[0])].message, ODID_MESSAGE_SIZE);
    }
    return needed;
}

int main(void) {
    const uint8_t *messages[ODID_PACK_MAX_MESSAGES];
    uint8_t pack[3 + ODID_PACK_MAX_MESSAGES * ODID_MESSAGE_SIZE + 8];

    printf("a pack is split into the messages it carries\n");
    size_t length = build_pack(pack, sizeof pack, 3);
    int n = odid_pack_split(pack, length, messages, ODID_PACK_MAX_MESSAGES);
    check("three messages are found", n == 3, "wrong count");
    check("each points at its own message",
          n == 3 && messages[0] == pack + 3 &&
          messages[1] == pack + 3 + ODID_MESSAGE_SIZE &&
          messages[2] == pack + 3 + 2 * ODID_MESSAGE_SIZE, "wrong offsets");
    check("and the bytes are the fixture's own",
          n == 3 && memcmp(messages[0], odid_vectors[0].message, ODID_MESSAGE_SIZE) == 0,
          "content differs");

    printf("\nthe messages still decode through the ordinary path\n");
    /* The point of returning pointers rather than a merged result: a pack is a container,
     * and what is inside it is decoded by exactly the same functions a BLE message is. */
    length = build_pack(pack, sizeof pack, 1);
    n = odid_pack_split(pack, length, messages, ODID_PACK_MAX_MESSAGES);
    bool decoded = false;
    if (n == 1) {
        uint8_t type = odid_message_type(messages[0]);
        if (type == 0x1) decoded = odid_decode_location(messages[0]).valid;
        else if (type == 0x0) decoded = odid_decode_basic_id(messages[0]).valid;
        else if (type == 0x4) decoded = odid_decode_system(messages[0]).valid;
        else decoded = true;   /* a type this decoder has no opinion about is still carried */
    }
    check("a message out of a pack decodes like any other", decoded, "did not decode");

    printf("\nthe ceiling holds\n");
    length = build_pack(pack, sizeof pack, ODID_PACK_MAX_MESSAGES);
    n = odid_pack_split(pack, length, messages, ODID_PACK_MAX_MESSAGES);
    check("nine is accepted", n == ODID_PACK_MAX_MESSAGES, "wrong count");
    pack[2] = ODID_PACK_MAX_MESSAGES + 1;
    check("ten is refused", odid_pack_split(pack, length, messages,
                                            ODID_PACK_MAX_MESSAGES) == 0, "accepted");
    pack[2] = 0;
    check("zero is refused", odid_pack_split(pack, length, messages,
                                             ODID_PACK_MAX_MESSAGES) == 0, "accepted");

    printf("\nnothing malformed walks off the end\n");
    length = build_pack(pack, sizeof pack, 4);
    /* The count says four; the buffer holds three. Trusting the field here is how a
     * malformed frame reads memory it was never given. */
    check("a count longer than the buffer is refused",
          odid_pack_split(pack, length - ODID_MESSAGE_SIZE, messages,
                          ODID_PACK_MAX_MESSAGES) == 0, "accepted a short buffer");
    pack[1] = 24;
    check("a message size that is not 25 is refused",
          odid_pack_split(pack, length, messages, ODID_PACK_MAX_MESSAGES) == 0, "accepted");
    pack[1] = ODID_MESSAGE_SIZE;
    pack[0] = 0x10;   /* a Location message, not a pack */
    check("a single message is not mistaken for a pack",
          odid_pack_split(pack, length, messages, ODID_PACK_MAX_MESSAGES) == 0, "accepted");
    check("an empty buffer is handled",
          odid_pack_split(pack, 0, messages, ODID_PACK_MAX_MESSAGES) == 0, NULL);
    check("a NULL buffer is handled",
          odid_pack_split(NULL, 30, messages, ODID_PACK_MAX_MESSAGES) == 0, NULL);

    printf("\na caller with less room than the pack needs\n");
    pack[0] = ODID_MESSAGE_TYPE_PACKED << 4;
    length = build_pack(pack, sizeof pack, 5);
    n = odid_pack_split(pack, length, messages, 2);
    check("takes what it asked for, not what it cannot hold", n == 2, "overran the caller");

    printf("\n%d/%d passed\n", passed, passed + failed);
    return failed == 0 ? 0 : 1;
}
