/* Decodes one hex message from argv and prints its fields, one per line.
 *
 * Exists so the C and the Python can be run against the same random input and
 * compared. The conformance vectors cover the cases someone thought of; a differential
 * run over random bytes covers the ones nobody did, which is where a port quietly
 * diverges — a sign bit, a rounding rule, an off-by-one offset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../odid_decode.h"

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 2 || strlen(argv[1]) != ODID_MESSAGE_SIZE * 2) {
        fprintf(stderr, "usage: decode_cli <50 hex chars>\n");
        return 2;
    }
    uint8_t msg[ODID_MESSAGE_SIZE];
    for (int i = 0; i < ODID_MESSAGE_SIZE; i++) {
        int hi = hex_nibble(argv[1][i * 2]), lo = hex_nibble(argv[1][i * 2 + 1]);
        if (hi < 0 || lo < 0) { fprintf(stderr, "bad hex\n"); return 2; }
        msg[i] = (uint8_t)((hi << 4) | lo);
    }

    switch (odid_message_type(msg)) {
    case 0x1: {
        odid_location_t l = odid_decode_location(msg);
        if (!l.valid) { printf("empty\n"); break; }
        printf("lat %.7f\nlon %.7f\n", l.lat, l.lon);
        if (l.has_altitude)
            printf("altitude_m %.1f\naltitude_ref %s\n", l.altitude_m,
                   l.altitude_ref == ODID_ALT_AGL ? "agl" : "absolute");
        else printf("altitude_m N/A\naltitude_ref N/A\n");
        if (l.has_height)
            printf("height_m %.1f\nheight_ref %s\n", l.height_m,
                   l.height_ref == ODID_HEIGHT_GROUND ? "ground" : "takeoff");
        else printf("height_m N/A\nheight_ref N/A\n");
        printf("speed_mps %.1f\nvspeed_mps %.1f\ndirection_deg %d\n",
               l.speed_mps, l.vspeed_mps, l.direction_deg);
        break;
    }
    case 0x0: {
        odid_basic_id_t b = odid_decode_basic_id(msg);
        printf("uas_id %s\n", b.valid ? b.uas_id : "N/A");
        break;
    }
    case 0x4: {
        odid_system_t s = odid_decode_system(msg);
        if (!s.valid) { printf("empty\n"); break; }
        printf("operator_lat %.7f\noperator_lon %.7f\noperator_altitude_m %.1f\n",
               s.operator_lat, s.operator_lon, s.operator_altitude_m);
        break;
    }
    default:
        printf("unsupported\n");
    }
    return 0;
}
