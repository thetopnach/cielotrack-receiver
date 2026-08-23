/* Finding an Open Drone ID payload inside an 802.11 frame.
 *
 * Split from the ESP-IDF plumbing for the same reason odid_ble is: this half is pure
 * byte-hunting and can be proved on a host, where a wrong offset shows up as a failing
 * test rather than as a receiver that hears nothing for a week. The half that talks to a
 * radio is the half that cannot be tested that way, so it is kept as small as possible.
 *
 * Two framings carry the same payload, and this handles both — a receiver that
 * understood only one would miss whichever manufacturers chose the other, invisibly:
 *
 *   Beacon: a vendor-specific information element with OUI FA:0B:BC (ASD-STAN) and
 *           OUI type 0x0D, then a one-byte message counter, then a Message Pack.
 *
 *   NaN:    a service descriptor carrying the service ID that is the first six bytes of
 *           SHA-256("org.opendroneid.remoteid"), then four bytes of descriptor fields,
 *           then the same counter and pack.
 *
 * Located by scanning for the signature rather than walking the IE and attribute trees.
 * That is what odid_ble already does for BLE, and it is robust to the element sitting at
 * any offset among others — which it does, because a beacon carries whatever else the
 * transmitter felt like advertising.
 *
 * Unlike the Pi's capture there is no radiotap header here: the ESP32 hands over the
 * 802.11 frame itself and reports signal strength separately, so this starts at frame
 * control rather than after a variable-length prefix.
 */
#ifndef WIFI_RID_H
#define WIFI_RID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_RID_BEACON_SIGNATURE_LEN 4
#define WIFI_RID_NAN_SERVICE_ID_LEN   6

extern const uint8_t WIFI_RID_BEACON_SIGNATURE[WIFI_RID_BEACON_SIGNATURE_LEN];
extern const uint8_t WIFI_RID_NAN_SERVICE_ID[WIFI_RID_NAN_SERVICE_ID_LEN];

/* The ODID payload inside an 802.11 frame, with the message counter already stripped —
 * so what comes back is a Message Pack ready for odid_pack_split, or a single message.
 *
 * Points into `frame`; nothing is copied and nothing is allocated. Returns false when
 * the frame carries no Remote ID, which is almost every frame in the air.
 */
bool wifi_rid_payload(const uint8_t *frame, size_t length,
                      const uint8_t **payload, size_t *payload_length);

/* The transmitter's address: 802.11 addr2, at offset 10 after frame control (2),
 * duration (2) and addr1 (6). Written as six bytes, not a string, because the caller
 * formats it and a receiver should not be allocating text on a hot path.
 */
bool wifi_rid_transmitter(const uint8_t *frame, size_t length, uint8_t mac[6]);

#endif /* WIFI_RID_H */
