/* Finding an Open Drone ID message inside a BLE advertisement.
 *
 * Split from odid_decode so the byte-hunting is testable on a host too. This is the
 * half that reads a radio's buffer, and it is where a fixed offset or an off-by-one
 * quietly hands back the wrong aircraft's telemetry — the Python side already carries
 * a comment about exactly that, because scanning a whole HCI event rather than one
 * advertising report paired payloads with the wrong advertiser.
 */
#ifndef ODID_BLE_H
#define ODID_BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "odid_decode.h"

/* ASTM Remote ID service UUID (0xFFFA, little endian) followed by the Open Drone ID
 * application code. Searched for rather than assumed at a fixed offset: the service
 * data element is not always first in the advertisement. */
#define ODID_BLE_SIGNATURE_LEN 3
extern const uint8_t ODID_BLE_SIGNATURE[ODID_BLE_SIGNATURE_LEN];

/* Copies the 25-byte message out of one advertising report's data.
 *
 * `data` must be a single advertiser's payload, never a whole HCI event: an event can
 * batch reports from several advertisers, and taking the first match across all of
 * them attributes one aircraft's telemetry to another's address.
 *
 * Returns false when there is no complete message. A truncated tail after a signature
 * match does not end the search, because that match may itself have been coincidental.
 */
bool odid_ble_extract(const uint8_t *data, size_t length, uint8_t out[ODID_MESSAGE_SIZE]);

#endif /* ODID_BLE_H */
