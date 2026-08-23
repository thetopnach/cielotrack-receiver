/* Splits a Message Pack from argv and prints how many messages it holds, one hex message
 * per line. Exists so the C and the Python can be run against the same input and
 * compared — the conformance vectors cover the cases someone thought of, and a
 * differential run over random bytes covers the ones nobody did.
 *
 * Validity is the interesting half. Two implementations that decode identically but
 * disagree about which frames to accept produce one receiver quietly hearing less than
 * its sibling, with nothing in either log to say so.
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
    if (argc != 2) { fprintf(stderr, "usage: pack_cli <hex>\n"); return 2; }
    size_t chars = strlen(argv[1]);
    if (chars % 2) { fprintf(stderr, "odd hex length\n"); return 2; }
    size_t length = chars / 2;
    uint8_t *data = malloc(length ? length : 1);
    if (!data) return 1;
    for (size_t i = 0; i < length; i++) {
        int hi = hex_nibble(argv[1][i * 2]), lo = hex_nibble(argv[1][i * 2 + 1]);
        if (hi < 0 || lo < 0) { fprintf(stderr, "bad hex\n"); free(data); return 2; }
        data[i] = (uint8_t)((hi << 4) | lo);
    }

    const uint8_t *messages[ODID_PACK_MAX_MESSAGES];
    int n = odid_pack_split(data, length, messages, ODID_PACK_MAX_MESSAGES);
    printf("%d\n", n);
    for (int i = 0; i < n; i++) {
        for (int b = 0; b < ODID_MESSAGE_SIZE; b++) printf("%02x", messages[i][b]);
        printf("\n");
    }
    free(data);
    return 0;
}
