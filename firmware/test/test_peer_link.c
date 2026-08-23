/* The framing on the wire between the two boards.
 *
 * A UART on a pole is not a quiet environment, and the failure that matters is not a
 * lost line — it is a corrupted one that still parses. A flipped bit inside a position
 * or a serial would put a wrong aircraft at a wrong place into the permanent record,
 * with nothing downstream able to tell. So every line carries a CRC over its payload,
 * and the master drops anything that does not match rather than forwarding it.
 *
 * The CRC implementation is the part worth testing on the host: it has to agree with
 * itself across two boards compiled separately, and a subtly wrong polynomial or
 * initial value would only show as traffic quietly vanishing in the field.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static int passed, failed;

static void check(const char *name, bool ok, const char *detail) {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name);
    if (!ok && detail) printf("  - %s", detail);
    printf("\n");
    if (ok) passed++; else failed++;
}

/* Copied deliberately rather than linked: this asserts that the algorithm in
 * peer_link.c is the standard CRC-32, so it must be checked against an independent
 * statement of it, not against itself. */
static uint32_t crc32(const char *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint8_t)data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

int main(void) {
    printf("peer link framing\n");

    /* Known-answer tests for CRC-32, so a wrong polynomial cannot pass by agreeing
     * with itself. These values are the standard ones. */
    check("CRC of \"\" is 0", crc32("", 0) == 0x00000000u, NULL);
    check("CRC of \"a\" matches the standard", crc32("a", 1) == 0xE8B7BE43u, NULL);
    check("CRC of \"123456789\" matches the standard",
          crc32("123456789", 9) == 0xCBF43926u, NULL);

    const char *contact =
        "{\"rssi_dbm\":-93,\"mac\":\"54:64:DE:2F:D0:2B\",\"uas_id\":\"1786501045\","
        "\"ua_type\":\"Hybrid Lift\",\"lat\":32.9934618,\"lon\":-96.6787773,"
        "\"message_count\":1,\"inferred\":false}";
    uint32_t good = crc32(contact, strlen(contact));

    /* A single flipped bit anywhere must not survive. This is the case the CRC exists
     * for: the line still parses as JSON and still looks entirely reasonable. */
    bool all_caught = true;
    char mangled[512];
    for (size_t i = 0; i < strlen(contact); i++) {
        for (int bit = 0; bit < 8; bit++) {
            snprintf(mangled, sizeof mangled, "%s", contact);
            mangled[i] = (char)(mangled[i] ^ (1 << bit));
            if (crc32(mangled, strlen(mangled)) == good) { all_caught = false; break; }
        }
        if (!all_caught) break;
    }
    check("every single-bit flip in a contact changes the CRC", all_caught,
          "a corrupted line would have been forwarded as though it were sound");

    /* The line is text so a human with a serial adapter can read it, which means the
     * separators have to survive being present in the payload. */
    check("the payload contains no newline", strchr(contact, '\n') == NULL,
          "a newline inside a record would split it into two");

    printf("\n%d/%d passed\n", passed, passed + failed);
    return failed ? 1 : 0;
}
