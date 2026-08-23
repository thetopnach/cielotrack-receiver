#include "wifi_rid.h"

#include <string.h>

/* ASD-STAN's OUI followed by the Direct Remote ID type. Verified against
 * opendroneid/transmitter-linux's wifi_beacon.c, which sets the vendor element to
 * "dd1EFA0BBC0D00" — the same four bytes this looks for. */
const uint8_t WIFI_RID_BEACON_SIGNATURE[WIFI_RID_BEACON_SIGNATURE_LEN] = {
    0xFA, 0x0B, 0xBC, 0x0D
};

/* First six bytes of SHA-256("org.opendroneid.remoteid"). */
const uint8_t WIFI_RID_NAN_SERVICE_ID[WIFI_RID_NAN_SERVICE_ID_LEN] = {
    0x88, 0x69, 0x19, 0x9D, 0x92, 0x09
};

static const uint8_t *find(const uint8_t *haystack, size_t haystack_len,
                           const uint8_t *needle, size_t needle_len) {
    if (haystack == NULL || needle_len == 0 || haystack_len < needle_len) return NULL;
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return haystack + i;
    }
    return NULL;
}

bool wifi_rid_payload(const uint8_t *frame, size_t length,
                      const uint8_t **payload, size_t *payload_length) {
    if (frame == NULL || payload == NULL || payload_length == NULL) return false;

    const uint8_t *hit = find(frame, length, WIFI_RID_BEACON_SIGNATURE,
                              WIFI_RID_BEACON_SIGNATURE_LEN);
    if (hit != NULL) {
        /* ...FA 0B BC 0D <counter> <pack...> */
        const uint8_t *start = hit + WIFI_RID_BEACON_SIGNATURE_LEN + 1;
        if (start >= frame + length) return false;
        *payload = start;
        *payload_length = (size_t)(frame + length - start);
        return true;
    }

    hit = find(frame, length, WIFI_RID_NAN_SERVICE_ID, WIFI_RID_NAN_SERVICE_ID_LEN);
    if (hit != NULL) {
        /* service_id[6] instance_id requestor_instance_id service_control
         * service_info_length, then service info = <counter> <pack...> */
        const uint8_t *start = hit + WIFI_RID_NAN_SERVICE_ID_LEN + 4 + 1;
        if (start >= frame + length) return false;
        *payload = start;
        *payload_length = (size_t)(frame + length - start);
        return true;
    }
    return false;
}

bool wifi_rid_transmitter(const uint8_t *frame, size_t length, uint8_t mac[6]) {
    if (frame == NULL || mac == NULL || length < 16) return false;
    memcpy(mac, frame + 10, 6);
    return true;
}
