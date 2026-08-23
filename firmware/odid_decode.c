#include "odid_decode.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Altitudes are stored in 0.5 m steps offset by 1000 m. Zero therefore means both
 * -1000 m and "no value", which is why -1000 m cannot be expressed and why zero has to
 * be read as absent. */
static double altitude_from(uint16_t encoded) {
    return encoded * 0.5 - 1000.0;
}

/* The Python side rounds before reporting, and the vectors record rounded values.
 * Matching that here keeps the two implementations comparable to the last digit rather
 * than almost-equal in a way that needs a tolerance nobody agrees on.
 *
 * nearbyint, not round. C's round() goes half away from zero; Python's round() goes
 * half to even, and horizontal speed is encoded in 0.25 m/s steps so every other value
 * lands exactly on a tie. That made the two disagree on 115 of 5030 fields — always by
 * 0.1 m/s, always on a speed, never on anything the curated vectors happened to cover.
 * A differential run over random messages is what found it; it would otherwise have
 * surfaced as two receivers reporting the same aircraft slightly differently, which
 * reads as a reception difference and is the exact confusion this port exists to
 * remove. nearbyint under the default rounding mode is round-half-to-even, and these
 * values are exact binary fractions, so the two agree bit for bit. */
static double round_to(double value, int places) {
    double scale = pow(10.0, places);
    return nearbyint(value * scale) / scale;
}

static int32_t read_i32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

odid_location_t odid_decode_location(const uint8_t *msg) {
    odid_location_t out;
    memset(&out, 0, sizeof(out));

    const uint8_t byte1 = msg[1];
    const bool speed_multiplier = (byte1 & 0x01) != 0;
    const bool east_west = (byte1 & 0x02) != 0;
    /* Bit 2 separates above-takeoff from above-ground. The two differ by the whole
     * climb of a launch from a rooftop, so it is reported rather than assumed. */
    const bool above_ground = (byte1 & 0x04) != 0;

    const uint8_t direction_enc = msg[2];
    const uint8_t speed_h_enc = msg[3];
    const int8_t speed_v_enc = (int8_t)msg[4];

    /* Signed. Reading these unsigned puts the aircraft on the wrong side of the
     * planet, and the wrong side looks entirely plausible on a map. */
    const int32_t lat_enc = read_i32(&msg[5]);
    const int32_t lon_enc = read_i32(&msg[9]);

    const uint16_t alt_baro_enc = read_u16(&msg[13]);
    const uint16_t alt_geo_enc = read_u16(&msg[15]);
    const uint16_t height_enc = read_u16(&msg[17]);

    const double lat = lat_enc / 1e7;
    const double lon = lon_enc / 1e7;
    if (lat == 0.0 && lon == 0.0) {
        return out;                      /* the spec's "no position" sentinel */
    }

    out.valid = true;
    out.lat = round_to(lat, 7);
    out.lon = round_to(lon, 7);

    /* Above 63.75 m/s the encoding changes scale. Ignoring the bit understates a fast
     * aircraft by an amount that grows with its speed. */
    out.speed_mps = round_to(speed_multiplier ? (255 * 0.25 + speed_h_enc * 0.75)
                                              : (speed_h_enc * 0.25), 1);
    out.vspeed_mps = round_to(speed_v_enc * 0.5, 1);
    out.direction_deg = direction_enc + (east_west ? 180 : 0);

    if (height_enc) {
        out.has_altitude = true;
        out.altitude_m = round_to(altitude_from(height_enc), 1);
        out.altitude_ref = ODID_ALT_AGL;
    } else if (alt_geo_enc || alt_baro_enc) {
        out.has_altitude = true;
        out.altitude_m = round_to(altitude_from(alt_geo_enc ? alt_geo_enc : alt_baro_enc), 1);
        out.altitude_ref = ODID_ALT_ABSOLUTE;
    }

    if (height_enc) {
        out.has_height = true;
        out.height_m = round_to(altitude_from(height_enc), 1);
        out.height_ref = above_ground ? ODID_HEIGHT_GROUND : ODID_HEIGHT_TAKEOFF;
    }
    return out;
}

odid_basic_id_t odid_decode_basic_id(const uint8_t *msg) {
    odid_basic_id_t out;
    memset(&out, 0, sizeof(out));
    out.id_type = msg[1] >> 4;
    out.ua_type = msg[1] & 0x0F;

    /* 20 bytes, NUL padded. Trailing spaces are stripped the way the Python does, so a
     * serial cannot differ between implementations by invisible whitespace. */
    char raw[21];
    memcpy(raw, &msg[2], 20);
    raw[20] = '\0';
    size_t length = strlen(raw);
    while (length > 0 && (raw[length - 1] == ' ' || raw[length - 1] == '\t')) {
        raw[--length] = '\0';
    }
    size_t start = 0;
    while (start < length && (raw[start] == ' ' || raw[start] == '\t')) {
        start++;
    }
    memmove(out.uas_id, raw + start, length - start + 1);
    out.valid = out.uas_id[0] != '\0';
    /* A serial is ASCII by definition, so a control byte in this field means the frame
     * is not a Basic ID — something else was read at this offset, and its type nibble is
     * no more trustworthy than its id. Heard for real on 2026-08-17: a Wi-Fi frame that
     * decoded to "|U(\x0e\nhT'-", type Glider, four metres away. The Python decoder
     * rejects it and this must agree, or the same frame becomes an aircraft on one
     * implementation and nothing on the other. */
    for (size_t i = 0; out.uas_id[i] != '\0'; i++) {
        unsigned char character = (unsigned char)out.uas_id[i];
        if (character < 0x20 || character == 0x7F) {
            memset(out.uas_id, 0, sizeof(out.uas_id));
            out.valid = false;
            break;
        }
    }
    return out;
}

odid_system_t odid_decode_system(const uint8_t *msg) {
    odid_system_t out;
    memset(&out, 0, sizeof(out));

    const int32_t op_lat_enc = read_i32(&msg[2]);
    const int32_t op_lon_enc = read_i32(&msg[6]);
    const double op_lat = op_lat_enc / 1e7;
    const double op_lon = op_lon_enc / 1e7;
    if (op_lat == 0.0 && op_lon == 0.0) {
        return out;                      /* same sentinel as the aircraft position */
    }

    out.valid = true;
    out.operator_lat = round_to(op_lat, 7);
    out.operator_lon = round_to(op_lon, 7);

    const uint16_t op_alt_enc = read_u16(&msg[18]);
    out.has_operator_altitude = true;
    out.operator_altitude_m = round_to(altitude_from(op_alt_enc), 1);

    /* Low two bits. 3 is reserved by the spec and is surfaced rather than folded into
     * a known value, so an unexpected encoding cannot masquerade as a real one. */
    out.operator_location_type = (odid_operator_location_t)(msg[1] & 0x03);
    return out;
}


int odid_pack_split(const uint8_t *data, size_t length,
                    const uint8_t **messages, int max_messages) {
    /* Every check here mirrors decode_message_pack in odid_decode.py, in the same order
     * and for the same reasons. Two implementations of one format is already a risk;
     * two implementations that disagree about what counts as valid is the version of
     * that risk which produces a receiver quietly discarding frames its sibling accepts.
     */
    if (data == NULL || messages == NULL || max_messages <= 0) return 0;
    if (length < 3) return 0;
    if ((data[0] >> 4) != ODID_MESSAGE_TYPE_PACKED) return 0;

    uint8_t single_size = data[1];
    uint8_t count = data[2];
    /* SingleMessageSize is always 25 in the spec. Trusting the field instead of checking
     * it would let a malformed frame walk the pointer wherever it liked. */
    if (single_size != ODID_MESSAGE_SIZE) return 0;
    if (count < 1 || count > ODID_PACK_MAX_MESSAGES) return 0;
    if (length < (size_t)3 + (size_t)count * single_size) return 0;

    int found = 0;
    for (int i = 0; i < count && found < max_messages; i++) {
        messages[found++] = data + 3 + (size_t)i * single_size;
    }
    return found;
}

/* Generated from the same table odid_decode.py uses; tests/test_c_matches_python.py
 * fails if they drift. */
const char *odid_ua_type_label(uint8_t ua_type) {
    switch (ua_type) {
    case 0: return "None/Undeclared";
    case 1: return "Aeroplane";
    case 2: return "Helicopter/Multirotor";
    case 3: return "Gyroplane";
    case 4: return "Hybrid Lift";
    case 5: return "Ornithopter";
    case 6: return "Glider";
    case 7: return "Kite";
    case 8: return "Free Balloon";
    case 9: return "Captive Balloon";
    case 10: return "Airship";
    case 11: return "Free Fall/Parachute";
    case 12: return "Rocket";
    case 13: return "Tethered Powered Aircraft";
    case 14: return "Ground Obstacle";
    case 15: return "Other";
    default: break;
    }
    /* Static buffer: the spec defines 0-15 and every one of them is named above, so this
     * is only reachable if a future revision adds a value. Reporting "Type 16" keeps the
     * number visible instead of discarding it. */
    static char other[12];
    snprintf(other, sizeof other, "Type %u", ua_type);
    return other;
}
