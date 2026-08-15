"""ASTM F3411 / Open Drone ID message decoding, shared by every transport.

The message layouts below are identical regardless of how the broadcast reached
us — Bluetooth LE (radio_tracker.py) or Wi-Fi beacon/NaN (wifi_remote_id.py).
Only the framing around them differs, so this module owns the message decoding
and each transport module owns its own framing.

Field layouts and scale factors per opendroneid-core-c
(libopendroneid/opendroneid.h/.c), the reference implementation.
"""
import struct

ODID_ID_TYPE_LABELS = {0: "None", 1: "Serial Number", 2: "CAA Registration ID",
                       3: "UTM Assigned ID", 4: "Specific Session ID"}
ODID_UA_TYPE_LABELS = {
    0: "None/Undeclared", 1: "Aeroplane", 2: "Helicopter/Multirotor", 3: "Gyroplane",
    4: "Hybrid Lift", 5: "Ornithopter", 6: "Glider", 7: "Kite", 8: "Free Balloon",
    9: "Captive Balloon", 10: "Airship", 11: "Free Fall/Parachute", 12: "Rocket",
    13: "Tethered Powered Aircraft", 14: "Ground Obstacle", 15: "Other",
}

ODID_MESSAGE_SIZE = 25
ODID_MESSAGETYPE_PACKED = 0xF
ODID_PACK_MAX_MESSAGES = 9
# Service UUID 0xFFFA (little-endian) + the Open Drone ID AD application code
ODID_BLE_SERVICE_SIGNATURE = b'\xFA\xFF\x0D'

# What the System message's operator coordinate actually refers to. These are three
# different claims and the aircraft tells you which one it's making, so the value is
# meaningless without it: a Wing Hummingbird delivering here broadcasts an operator
# position 8.6 miles away, which is wrong as a launch site and unremarkable as a
# control station. Treating the coordinate as "where it took off" is an assumption
# the message never made.
ODID_OPERATOR_LOCATION_TYPE_LABELS = {0: "takeoff", 1: "live-gnss", 2: "fixed"}


def decode_basic_id(msg):
    """Decodes a 25-byte Basic ID message (type nibble 0x0): ID type, UA type, UAS ID."""
    id_type = msg[1] >> 4
    ua_type = msg[1] & 0x0F
    uas_id = msg[2:22].split(b'\x00')[0].decode('ascii', errors='ignore').strip()
    return {
        "uas_id": uas_id or "N/A",
        "id_type": ODID_ID_TYPE_LABELS.get(id_type, f"Type {id_type}"),
        "ua_type": ODID_UA_TYPE_LABELS.get(ua_type, f"Type {ua_type}"),
    }


def decode_location(msg):
    """Decodes a 25-byte Location message (type nibble 0x1): real GPS position, altitude, speed."""
    byte1 = msg[1]
    speed_mult = byte1 & 0x01
    ew_direction = (byte1 >> 1) & 0x01
    # Height Type: 0 = above takeoff, 1 = above ground. Reported rather than assumed,
    # because the two differ by the whole climb of a launch from a rooftop or a hill.
    height_type = (byte1 >> 2) & 0x01
    direction_enc = msg[2]
    speed_h_enc = msg[3]
    speed_v_enc = struct.unpack('b', msg[4:5])[0]
    lat_enc, lon_enc = struct.unpack('<ii', msg[5:13])
    alt_baro_enc, alt_geo_enc, height_enc = struct.unpack('<HHH', msg[13:19])

    lat = lat_enc / 1e7
    lon = lon_enc / 1e7
    if lat == 0 and lon == 0:
        # 0,0 is the spec's "no data" sentinel, not a real position
        return {}

    speed_mps = (255 * 0.25 + speed_h_enc * 0.75) if speed_mult else (speed_h_enc * 0.25)
    # Height is above-ground/above-takeoff — what actually matters for an FAA ceiling
    # comparison. AltitudeGeo/AltitudeBaro are absolute WGS84/barometric altitude, which
    # reads several hundred feet higher anywhere with real ground elevation.
    #
    # Not every operator broadcasts Height: over the cloud feed, Wing and Zipline send
    # the "no value" sentinel on every moving aircraft, and silently substituting
    # absolute altitude made them look like they cruise at ~700 ft against Amazon's
    # ~185 ft. Report which reference the number actually uses instead of conflating
    # them — a consumer can then decline to compare an absolute figure against the
    # 400 ft AGL limit.
    altitude_ref = "N/A"
    altitude_m = None
    if height_enc:
        altitude_m, altitude_ref = height_enc * 0.5 - 1000, "agl"
    elif alt_geo_enc or alt_baro_enc:
        altitude_m = (alt_geo_enc or alt_baro_enc) * 0.5 - 1000
        altitude_ref = "absolute"

    # Height and absolute altitude are two different measurements broadcast together,
    # not two ways of saying one thing, so reporting height separately stops the pair
    # above from having to choose between them. altitude_m keeps its existing meaning —
    # consumers already branch on altitude_ref — and height_m simply stops the other
    # number being thrown away. A drone at 47.5 m over a field 172 m above sea level
    # broadcasts both 47.5 and 219.5, and each answers a different question.
    height_m = round(height_enc * 0.5 - 1000, 1) if height_enc else None

    return {
        "lat": round(lat, 7),
        "lon": round(lon, 7),
        "altitude_m": round(altitude_m, 1) if altitude_m is not None else "N/A",
        "altitude_ref": altitude_ref,
        "height_m": height_m if height_m is not None else "N/A",
        "height_ref": ("ground" if height_type else "takeoff") if height_enc else "N/A",
        "speed_mps": round(speed_mps, 1),
        "vspeed_mps": round(speed_v_enc * 0.5, 1),
        "direction_deg": direction_enc + (180 if ew_direction else 0),
    }


def decode_system(msg):
    """Decodes a 25-byte System message (type nibble 0x4): where the operator is, and
    what the aircraft means by "operator".

    Byte 1's low 2 bits are OperatorLocationType — takeoff point, live pilot GNSS, or
    a declared fixed location. Decoding the coordinate without it produced a position
    the dashboard then labelled a launch site, which is only one of the three things
    it can be, and demonstrably not the right one for BVLOS delivery: one remote pilot
    supervises aircraft flying from nests they're nowhere near."""
    op_lat_enc, op_lon_enc = struct.unpack('<ii', msg[2:10])
    op_lat = op_lat_enc / 1e7
    op_lon = op_lon_enc / 1e7
    if op_lat == 0 and op_lon == 0:
        # 0,0 is the spec's "no data" sentinel, not a real position
        return {}

    location_type = msg[1] & 0x03
    op_alt_enc = struct.unpack('<H', msg[18:20])[0]
    return {
        "operator_lat": round(op_lat, 7),
        "operator_lon": round(op_lon, 7),
        "operator_altitude_m": round(op_alt_enc * 0.5 - 1000, 1),
        # 3 is reserved by the spec; surface it rather than folding it into a real
        # value, so an unexpected encoding can't masquerade as a known one.
        "operator_location_type": ODID_OPERATOR_LOCATION_TYPE_LABELS.get(
            location_type, f"reserved-{location_type}"),
    }


def decode_message(msg):
    """Dispatches one 25-byte ODID message to the right decoder by its type nibble.
    Returns {} for message types we don't decode (Auth, Self ID, Operator ID)."""
    if len(msg) != ODID_MESSAGE_SIZE:
        return {}
    message_type = msg[0] >> 4
    if message_type == 0:
        return decode_basic_id(msg)
    if message_type == 1:
        return decode_location(msg)
    if message_type == 4:
        return decode_system(msg)
    return {}


def decode_message_pack(data):
    """Decodes a Message Pack (type nibble 0xF), merging every message it contains.

    Wi-Fi almost always uses these — a single frame can carry up to 9 messages, so
    one capture yields Basic ID + Location + System together, rather than BLE's
    one-message-per-broadcast rotation. Layout: byte 0 type/version, byte 1
    SingleMessageSize (always 25), byte 2 MsgPackSize (count), then count*25 bytes.

    Returns (merged_fields, decoded_count) where decoded_count counts only the
    messages that actually yielded data — message types we don't decode (Auth,
    Self ID, Operator ID) don't inflate it. ({}, 0) if this isn't a valid pack."""
    if len(data) < 3 or (data[0] >> 4) != ODID_MESSAGETYPE_PACKED:
        return {}, 0
    single_size, count = data[1], data[2]
    if single_size != ODID_MESSAGE_SIZE or not (1 <= count <= ODID_PACK_MAX_MESSAGES):
        return {}, 0
    if len(data) < 3 + count * single_size:
        return {}, 0

    merged = {}
    decoded_count = 0
    for i in range(count):
        start = 3 + i * single_size
        fields = decode_message(data[start:start + single_size])
        if fields:
            merged.update(fields)
            decoded_count += 1
    return merged, decoded_count


def decode_odid_payload(data):
    """Decodes an ODID payload that may be either a single 25-byte message or a
    Message Pack, whichever the transmitter used.
    Returns (merged_fields, decoded_count)."""
    if not data:
        return {}, 0
    if (data[0] >> 4) == ODID_MESSAGETYPE_PACKED:
        return decode_message_pack(data)
    fields = decode_message(data[:ODID_MESSAGE_SIZE])
    return fields, (1 if fields else 0)


def extract_odid_message(data):
    """BLE framing: locates the 25-byte Open Drone ID message following the
    FA FF 0D (ASTM Remote ID service UUID + Open Drone ID app code) signature,
    skipping the trailing 1-byte message counter.

    Scans for the signature rather than indexing a fixed offset because the service
    data IE isn't always first in the advertisement. `data` must be the advertising
    data of a *single* advertising report: one HCI event can batch reports from
    several advertisers, so scanning a whole event would hand back a message that
    belongs to some other aircraft than the address the caller paired it with."""
    start = 0
    while True:
        i = data.find(ODID_BLE_SERVICE_SIGNATURE, start)
        if i < 0:
            return None
        msg = data[i + 4:i + 4 + ODID_MESSAGE_SIZE]
        if len(msg) == ODID_MESSAGE_SIZE:
            return msg
        # Truncated tail — an earlier signature match can still be spurious, so keep
        # looking rather than giving up on the report.
        start = i + 1
