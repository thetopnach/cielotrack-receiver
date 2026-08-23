/* Finding Remote ID inside an 802.11 frame.
 *
 * The Pi has decoded Wi-Fi Remote ID for months and the firmware has not, so the risk
 * here is not that the format is unknown — it is that a second implementation of a known
 * format drifts from the first in some corner, and the only symptom is one receiver
 * hearing fewer aircraft than the other with nothing in either log to explain it.
 *
 * These cases are therefore built to match wifi_remote_id.py's extract_rid_payload
 * exactly: the same two signatures, the same offsets past each, the same willingness to
 * find the element anywhere in the frame rather than at a fixed position.
 */
#include <stdio.h>
#include <string.h>

#include "../odid_decode.h"
#include "../wifi_rid.h"

static int passed, failed;

static void check(const char *name, bool ok, const char *detail) {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name);
    if (!ok && detail) printf("  - %s", detail);
    printf("\n");
    if (ok) passed++; else failed++;
}

/* A plausible beacon: an 802.11 management header, a couple of unrelated information
 * elements, then the vendor element carrying Remote ID. Real beacons look like this —
 * the Remote ID element is never first, which is exactly why the search scans. */
static size_t build_beacon(uint8_t *out, const uint8_t *pack, size_t pack_len,
                           uint8_t counter) {
    size_t n = 0;
    out[n++] = 0x80; out[n++] = 0x00;                     /* frame control: beacon */
    out[n++] = 0x00; out[n++] = 0x00;                     /* duration */
    for (int i = 0; i < 6; i++) out[n++] = 0xFF;          /* addr1: broadcast */
    const uint8_t tx[6] = {0x60, 0x60, 0x1F, 0x06, 0x6B, 0x4D};
    memcpy(out + n, tx, 6); n += 6;                        /* addr2: the transmitter */
    memcpy(out + n, tx, 6); n += 6;                        /* addr3 */
    out[n++] = 0x00; out[n++] = 0x00;                      /* sequence control */
    for (int i = 0; i < 12; i++) out[n++] = 0x00;          /* timestamp + interval + caps */
    out[n++] = 0x00; out[n++] = 0x08;                      /* SSID element */
    memcpy(out + n, "WING_RID", 8); n += 8;
    out[n++] = 0x01; out[n++] = 0x04;                      /* supported rates */
    for (int i = 0; i < 4; i++) out[n++] = 0x82;
    out[n++] = 0xDD;                                       /* vendor specific */
    out[n++] = (uint8_t)(5 + pack_len);
    memcpy(out + n, WIFI_RID_BEACON_SIGNATURE, WIFI_RID_BEACON_SIGNATURE_LEN);
    n += WIFI_RID_BEACON_SIGNATURE_LEN;
    out[n++] = counter;
    memcpy(out + n, pack, pack_len); n += pack_len;
    return n;
}

static size_t build_pack(uint8_t *out, int count) {
    out[0] = ODID_MESSAGE_TYPE_PACKED << 4;
    out[1] = ODID_MESSAGE_SIZE;
    out[2] = (uint8_t)count;
    for (int i = 0; i < count * ODID_MESSAGE_SIZE; i++) out[3 + i] = (uint8_t)(i & 0xFF);
    return 3 + (size_t)count * ODID_MESSAGE_SIZE;
}

int main(void) {
    uint8_t pack[3 + ODID_PACK_MAX_MESSAGES * ODID_MESSAGE_SIZE];
    uint8_t frame[512];
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    size_t pack_len = build_pack(pack, 3);
    size_t frame_len = build_beacon(frame, pack, pack_len, 0x2A);

    printf("a beacon's vendor element is found wherever it sits\n");
    check("the payload is located", wifi_rid_payload(frame, frame_len, &payload, &payload_len),
          "not found");
    check("the message counter is stripped", payload_len && payload[0] == pack[0],
          "counter left in place");
    check("and what remains is the pack",
          payload_len >= pack_len && memcmp(payload, pack, pack_len) == 0, "wrong bytes");

    printf("\nand it hands straight to the pack splitter\n");
    const uint8_t *messages[ODID_PACK_MAX_MESSAGES];
    int n = odid_pack_split(payload, payload_len, messages, ODID_PACK_MAX_MESSAGES);
    check("three messages come out", n == 3, "wrong count");

    printf("\nthe transmitter is addr2, not addr1\n");
    uint8_t mac[6];
    const uint8_t expected_tx[6] = {0x60, 0x60, 0x1F, 0x06, 0x6B, 0x4D};
    check("the address is read", wifi_rid_transmitter(frame, frame_len, mac), NULL);
    /* addr1 here is the broadcast address; taking it would label every aircraft
     * ff:ff:ff:ff:ff:ff and merge the lot into one contact. */
    check("and it is the transmitter", memcmp(mac, expected_tx, 6) == 0, "wrong address");
    check("a runt frame yields no address", !wifi_rid_transmitter(frame, 12, mac), NULL);

    printf("\nNaN framing carries the same payload\n");
    uint8_t nan[256];
    size_t n_len = 0;
    for (int i = 0; i < 24; i++) nan[n_len++] = 0x11;      /* an action frame header */
    memcpy(nan + n_len, WIFI_RID_NAN_SERVICE_ID, WIFI_RID_NAN_SERVICE_ID_LEN);
    n_len += WIFI_RID_NAN_SERVICE_ID_LEN;
    for (int i = 0; i < 4; i++) nan[n_len++] = 0x00;       /* descriptor fields */
    nan[n_len++] = 0x07;                                    /* counter */
    memcpy(nan + n_len, pack, pack_len); n_len += pack_len;
    check("the payload is located", wifi_rid_payload(nan, n_len, &payload, &payload_len),
          "not found");
    check("with its counter stripped too",
          payload_len >= pack_len && memcmp(payload, pack, pack_len) == 0, "wrong offset");

    printf("\nordinary traffic yields nothing\n");
    uint8_t ordinary[128];
    memset(ordinary, 0x33, sizeof ordinary);
    check("a frame with no Remote ID", !wifi_rid_payload(ordinary, sizeof ordinary,
                                                         &payload, &payload_len), NULL);
    check("an empty buffer", !wifi_rid_payload(frame, 0, &payload, &payload_len), NULL);
    check("a NULL buffer", !wifi_rid_payload(NULL, 100, &payload, &payload_len), NULL);

    printf("\na signature at the very end promises nothing\n");
    /* The element is there but the frame stops before the counter, so there is no
     * payload — returning a pointer past the end would be a buffer overrun dressed as a
     * detection. */
    uint8_t truncated[64];
    size_t t = 0;
    for (int i = 0; i < 20; i++) truncated[t++] = 0x00;
    memcpy(truncated + t, WIFI_RID_BEACON_SIGNATURE, WIFI_RID_BEACON_SIGNATURE_LEN);
    t += WIFI_RID_BEACON_SIGNATURE_LEN;
    check("nothing is returned past the end",
          !wifi_rid_payload(truncated, t, &payload, &payload_len), "returned a pointer");

    printf("\n%d/%d passed\n", passed, passed + failed);
    return failed == 0 ? 0 : 1;
}
