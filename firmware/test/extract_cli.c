/* Runs the BLE extraction against one real advertising payload from argv.
 *
 * decode_cli takes a message that something already found. This takes what the radio
 * actually hands over — one advertiser's payload, exactly the buffer the ESP32 passes
 * to odid_ble_extract — and answers the question the conformance vectors cannot: does
 * the byte-hunting work on an advertisement that really flew?
 *
 * The vectors were written by us, from our own understanding of the format. If that
 * understanding is wrong in the same way in both the fixture and the code, they agree
 * with each other and disagree with the sky. This is how that gets caught.
 *
 *     extract_cli 0201060319000302fafa161...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../odid_ble.h"
#include "../odid_decode.h"

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: extract_cli <advertising payload as hex>\n");
        return 2;
    }
    size_t chars = strlen(argv[1]);
    if (chars % 2 != 0 || chars == 0) {
        fprintf(stderr, "hex must be an even number of characters\n");
        return 2;
    }
    size_t length = chars / 2;
    uint8_t *data = malloc(length);
    if (data == NULL) return 1;
    for (size_t i = 0; i < length; i++) {
        int hi = hex_nibble(argv[1][i * 2]), lo = hex_nibble(argv[1][i * 2 + 1]);
        if (hi < 0 || lo < 0) { fprintf(stderr, "bad hex\n"); free(data); return 2; }
        data[i] = (uint8_t)((hi << 4) | lo);
    }

    uint8_t message[ODID_MESSAGE_SIZE];
    if (!odid_ble_extract(data, length, message)) {
        printf("extracted: no\n");
        free(data);
        /* 1, not 0: a receiver that finds nothing in an advertisement known to carry
         * Remote ID has failed, and a script comparing the two sides should be able to
         * tell that from a clean miss without parsing this text. */
        return 1;
    }
    printf("extracted: yes\n");
    printf("message:   ");
    for (int i = 0; i < ODID_MESSAGE_SIZE; i++) printf("%02x", message[i]);
    printf("\n");
    printf("type:      0x%X\n", odid_message_type(message));

    switch (odid_message_type(message)) {
    case 0x0: {
        odid_basic_id_t id = odid_decode_basic_id(message);
        printf("valid:     %s\n", id.valid ? "yes" : "no");
        if (id.valid) {
            printf("uas_id:    %s\n", id.uas_id);
            printf("ua_type:   %u\n", id.ua_type);
        }
        break;
    }
    case 0x1: {
        odid_location_t loc = odid_decode_location(message);
        printf("valid:     %s\n", loc.valid ? "yes" : "no");
        if (loc.valid) {
            printf("lat:       %.7f\n", loc.lat);
            printf("lon:       %.7f\n", loc.lon);
            printf("speed:     %.1f\n", loc.speed_mps);
            printf("direction: %d\n", loc.direction_deg);
        }
        break;
    }
    case 0x4: {
        odid_system_t sys = odid_decode_system(message);
        printf("valid:     %s\n", sys.valid ? "yes" : "no");
        if (sys.valid) {
            printf("op_lat:    %.7f\n", sys.operator_lat);
            printf("op_lon:    %.7f\n", sys.operator_lon);
        }
        break;
    }
    default:
        printf("(no decoder for this message type)\n");
    }
    free(data);
    return 0;
}
