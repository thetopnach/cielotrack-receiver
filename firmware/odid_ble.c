#include "odid_ble.h"

#include <string.h>

const uint8_t ODID_BLE_SIGNATURE[ODID_BLE_SIGNATURE_LEN] = { 0xFA, 0xFF, 0x0D };

bool odid_ble_extract(const uint8_t *data, size_t length, uint8_t out[ODID_MESSAGE_SIZE]) {
    if (data == NULL || length < ODID_BLE_SIGNATURE_LEN) {
        return false;
    }
    for (size_t i = 0; i + ODID_BLE_SIGNATURE_LEN <= length; i++) {
        if (memcmp(&data[i], ODID_BLE_SIGNATURE, ODID_BLE_SIGNATURE_LEN) != 0) {
            continue;
        }
        /* The signature is followed by a one-byte message counter, then the message. */
        const size_t start = i + ODID_BLE_SIGNATURE_LEN + 1;
        if (start + ODID_MESSAGE_SIZE <= length) {
            memcpy(out, &data[start], ODID_MESSAGE_SIZE);
            return true;
        }
        /* Truncated tail. Keep looking rather than giving up: this match may have been
         * coincidental bytes, and a real one could follow. */
    }
    return false;
}
