/* Decoding ASTM F3411 / Open Drone ID messages.
 *
 * A C port of odid_decode.py, kept deliberately close to it. The two run on different
 * hardware and their output is compared in the field, so a disagreement about what a
 * message says would be indistinguishable from a difference in what each one heard.
 * tests/odid_vectors.json is the contract that stops that: both implementations are
 * checked against the same cases.
 *
 * No allocation, no dependencies beyond stdint/stdbool, nothing that needs an RTOS.
 * It is host-testable precisely because it knows nothing about radios.
 */
#ifndef ODID_DECODE_H
#define ODID_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ODID_MESSAGE_SIZE 25

/* A Message Pack carries several messages in one frame. Wi-Fi almost always uses them —
 * one capture yields Basic ID, Location and System together, where BLE rotates through
 * the types one broadcast at a time. Nine is the spec's ceiling.
 *
 * The BLE-only firmware never needed this; the Wi-Fi receiver cannot work without it,
 * because on Wi-Fi the pack is the normal case rather than an option. */
#define ODID_PACK_MAX_MESSAGES 9
#define ODID_MESSAGE_TYPE_PACKED 0xF

/* Every message type shares this: a field is either present or it is not, and absent
 * must never be represented by a plausible-looking zero. The Python side says "N/A";
 * here a has_* flag carries the same meaning without inventing a sentinel value that
 * could be mistaken for a reading. */

typedef enum {
    ODID_HEIGHT_TAKEOFF = 0,   /* above the takeoff point */
    ODID_HEIGHT_GROUND  = 1,   /* above the ground below the aircraft */
} odid_height_ref_t;

typedef enum {
    ODID_ALT_NONE = 0,
    ODID_ALT_AGL,              /* height above ground/takeoff, comparable to the limit */
    ODID_ALT_ABSOLUTE,         /* WGS84 or barometric, includes ground elevation */
} odid_altitude_ref_t;

typedef struct {
    bool valid;                /* false when the message carries no usable position */
    double lat;
    double lon;

    bool has_altitude;
    double altitude_m;
    odid_altitude_ref_t altitude_ref;

    /* Height is a separate measurement from altitude, not another name for it. An
     * aircraft broadcasts both and they answer different questions. */
    bool has_height;
    double height_m;
    odid_height_ref_t height_ref;

    double speed_mps;
    double vspeed_mps;
    int direction_deg;
} odid_location_t;

typedef struct {
    bool valid;
    char uas_id[21];           /* 20 bytes on the wire, always NUL terminated here */
    uint8_t id_type;
    uint8_t ua_type;
} odid_basic_id_t;

typedef enum {
    ODID_OPERATOR_TAKEOFF = 0,
    ODID_OPERATOR_LIVE_GNSS = 1,
    ODID_OPERATOR_FIXED = 2,
    ODID_OPERATOR_RESERVED = 3,
} odid_operator_location_t;

typedef struct {
    bool valid;
    double operator_lat;
    double operator_lon;
    bool has_operator_altitude;
    double operator_altitude_m;
    odid_operator_location_t operator_location_type;
} odid_system_t;

/* Each returns with .valid false when the message carries nothing usable — most often
 * the spec's 0,0 "no position" sentinel, which is not a place in the Atlantic. */
odid_location_t odid_decode_location(const uint8_t *msg);
odid_basic_id_t odid_decode_basic_id(const uint8_t *msg);
odid_system_t   odid_decode_system(const uint8_t *msg);

/* Splits a Message Pack into the messages it contains. Returns how many, or 0 if this
 * is not a valid pack.
 *
 * `messages` is filled with pointers *into* `data`, not copies: the caller decodes each
 * with the functions above while the buffer is still alive, which on a receiver is the
 * span of one frame. Nothing is allocated.
 *
 * Every message is handed back, including types this decoder has no opinion about —
 * Auth, Self ID, Operator ID. Deciding which are interesting belongs to the caller, and
 * a splitter that silently dropped them would make a pack of nine look like a pack of
 * three with no way to tell the difference from a malformed frame. */
int odid_pack_split(const uint8_t *data, size_t length,
                    const uint8_t **messages, int max_messages);

/* Message type is the high nibble of byte 0. */
static inline uint8_t odid_message_type(const uint8_t *msg) { return msg[0] >> 4; }


/* The airframe class as a name, matching odid_decode.py's ODID_UA_TYPE_LABELS exactly.
 *
 * Without this the boards reported ua_type as a bare number while every other receiver
 * in the fleet reported a label, so one row read "4" and its neighbour "Hybrid Lift" for
 * the same aircraft. Two conventions out of one decoder is precisely what compiling the
 * Pi's decoder into the firmware was meant to prevent, so the table lives here beside it
 * and a differential test holds the two in step.
 *
 * Never NULL: an unknown value returns "Type N" rather than nothing, because a missing
 * label and a label meaning "the aircraft declared nothing" are different claims. */
const char *odid_ua_type_label(uint8_t ua_type);

#endif /* ODID_DECODE_H */
