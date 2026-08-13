"""Wi-Fi Remote ID capture: ASTM F3411 broadcasts over Wi-Fi Beacon and Wi-Fi NaN.

Complements the BLE capture in radio_tracker.py — manufacturers choose their
transport under F3411, so a drone broadcasting over Wi-Fi is invisible to a
BLE-only receiver and vice versa.

Two framings carry the same ODID payload:

  Beacon: a vendor-specific information element (element ID 0xDD) with
          OUI FA:0B:BC (ASD-STAN) and OUI type 0x0D (Direct Remote ID),
          then a 1-byte message counter, then the ODID message pack.
          Verified against opendroneid/transmitter-linux's wifi_beacon.c,
          which sets vendor_elements to "dd1EFA0BBC0D00".

  NaN:    a service descriptor carrying service ID 88:69:19:9D:92:09 (the first
          6 bytes of SHA-256("org.opendroneid.remoteid")), then the same
          1-byte counter + message pack as service info.

Both are located by scanning the frame for their signature rather than walking
the full 802.11 IE/NaN attribute trees — same approach radio_tracker.py already
uses for the BLE signature, and it's robust to the vendor IE sitting at any
offset among other IEs.

Channel note: the radio hears one channel at a time, so coverage is a scheduling
problem, and parking beats rotating whenever you know where to park.

Measured here, not assumed. Two Wing Hummingbirds delivering to this address
produced 65 decodes across two airframes, every one of them a beacon on channel 6
with SSID WING_AC_RID — nothing on 44 or 149. So the default parks on 6.

The rotation this module can do was written for a diagnosis that turned out to be
wrong: that Wing was missed because it broadcast outside our one parked channel.
It wasn't. Wing was already on channel 6 the whole time and the receiver was
already sitting on it. What we were missing was range — every earlier Wing
contact was a transit at the edge of reception (one was 2 messages in 59 ms at
-83 dBm), while the pass we finally caught was an aircraft hovering ~50 ft away
to lower a package. Rotating costs two thirds of the duty cycle on the only
channel Wing has ever used, and it costs it worst on exactly those brief
transits, so it stays available via WIFI_CHANNELS but is no longer the default.
"""
import socket
import struct
import subprocess
import threading
import time

from odid_decode import decode_odid_payload

# Vendor-specific IE signature: OUI FA:0B:BC (ASD-STAN) + type 0x0D (Direct Remote ID)
BEACON_RID_SIGNATURE = bytes([0xFA, 0x0B, 0xBC, 0x0D])
# First 6 bytes of SHA-256("org.opendroneid.remoteid")
NAN_SERVICE_ID = bytes([0x88, 0x69, 0x19, 0x9D, 0x92, 0x09])

ETH_P_ALL = 0x0003

# The three channels Wi-Fi NaN is allowed to use, all non-DFS under FCC rules so a
# monitor-mode interface can sit on any of them without waiting out a radar check.
# Kept as the sweep set for WIFI_CHANNELS when hunting an unknown transmitter.
NAN_CHANNELS = (6, 44, 149)

# Default: park on 6. Every Wing decode this receiver has ever produced landed
# here, and a parked radio gets three times the duty cycle of a three-channel
# rotation. Override with WIFI_CHANNELS to sweep — see the channel note above.
CAPTURE_CHANNELS = (6,)

# F3411 requires Location at 1 Hz or better, so a few seconds on a channel is
# enough to hear anything transmitting on it — the dwell only has to beat that
# rate, not the length of a pass.
CHANNEL_DWELL_SECONDS = 3.0

# Once Remote ID actually decodes, stop rotating and stay on that channel. Has to
# outlast radio_tracker's 10s alert grace period, or a contact gets written from
# whatever single frame happened to land before the next hop.
ACTIVE_DWELL_SECONDS = 30.0

# How long the capture socket may hear absolutely nothing before it's treated as
# dead. A monitor-mode interface on a populated 2.4 GHz channel sees beacons
# constantly — thousands a minute here — so total silence is a fault, not a quiet
# moment. Generous enough that it can't trip on an unusually idle channel.
SILENT_SOCKET_SECONDS = 60.0


def set_channel(interface, channel):
    """Retunes a monitor-mode interface. False if the driver or the regulatory
    domain refuses the channel, so the caller can drop it from the rotation."""
    try:
        result = subprocess.run(["iw", "dev", interface, "set", "channel", str(channel)],
                                capture_output=True, text=True, timeout=10)
    except Exception as e:
        print(f"⚠️ Wi-Fi: retuning {interface} to channel {channel} failed: {e}")
        return False
    if result.returncode != 0:
        print(f"⚠️ Wi-Fi: {interface} rejected channel {channel} "
              f"({result.stderr.strip() or 'no error text'}) — dropping it from the rotation")
        return False
    return True


def channel_hopper(interface, channels, last_decode, stop_event):
    """Rotates `interface` across `channels` in its own thread.

    last_decode is a single-element list holding the time of the most recent
    decoded Remote ID frame — a mutable box rather than a return value because the
    capture loop updates it while this thread reads it. A channel the driver
    rejects is dropped rather than retried every cycle, so an adapter that can't do
    5 GHz degrades to covering whatever it can instead of stalling on each pass."""
    usable = list(channels)
    index = 0
    while usable and not stop_event.is_set():
        channel = usable[index % len(usable)]
        if not set_channel(interface, channel):
            usable.remove(channel)
            continue
        index += 1
        # Hold for the base dwell, then keep holding as long as something is still
        # decoding here — an active contact is worth more than the rotation.
        deadline = time.time() + CHANNEL_DWELL_SECONDS
        while time.time() < deadline or time.time() - last_decode[0] < ACTIVE_DWELL_SECONDS:
            if stop_event.wait(0.25):
                return
    if not usable:
        print(f"⚠️ Wi-Fi: {interface} accepted none of the channels {list(channels)} — "
              f"capture is running on whatever channel it was left on")


def parse_radiotap_length(frame):
    """Radiotap header: 1 byte version, 1 byte pad, 2 bytes little-endian length."""
    if len(frame) < 4:
        return None
    length = struct.unpack('<H', frame[2:4])[0]
    if length < 4 or length > len(frame):
        return None
    return length


# Radiotap fields appear in bit order, each aligned to its own natural size, so the
# signal byte's offset depends on which lower-numbered fields the driver included.
# (size, alignment) for every bit up to the one we want.
RADIOTAP_FIELD_SIZES = {
    0: (8, 8),   # TSFT
    1: (1, 1),   # Flags
    2: (1, 1),   # Rate
    3: (4, 2),   # Channel: u16 frequency + u16 flags
    4: (2, 2),   # FHSS
    5: (1, 1),   # Antenna signal, dBm, signed
}
RADIOTAP_DBM_ANTSIGNAL_BIT = 5


def parse_radiotap_signal(frame):
    """Received signal strength in dBm from the radiotap header, or None when the
    driver didn't supply it.

    Has to walk rather than index: the presence bitmap says which fields are there,
    each is aligned to its own size, and a set bit 31 chains another bitmap word
    ahead of the field data. Guessing a fixed offset would silently read whatever
    field happens to sit there."""
    if len(frame) < 8:
        return None
    it_len = struct.unpack('<H', frame[2:4])[0]
    if it_len > len(frame) or it_len < 8:
        return None

    present = []
    pos = 4
    while pos + 4 <= it_len:
        word = struct.unpack('<I', frame[pos:pos + 4])[0]
        present.append(word)
        pos += 4
        if not word & (1 << 31):
            break
    if not present or not present[0] & (1 << RADIOTAP_DBM_ANTSIGNAL_BIT):
        return None

    for bit in range(RADIOTAP_DBM_ANTSIGNAL_BIT + 1):
        if not present[0] & (1 << bit):
            continue
        size, alignment = RADIOTAP_FIELD_SIZES[bit]
        pos += (-pos) % alignment
        if bit == RADIOTAP_DBM_ANTSIGNAL_BIT:
            return struct.unpack('b', frame[pos:pos + 1])[0] if pos < it_len else None
        pos += size
    return None


def extract_transmitter_mac(dot11):
    """802.11 header addr2 (the transmitter) sits at offset 10 after
    frame control (2) + duration (2) + addr1 (6)."""
    if len(dot11) < 16:
        return None
    return ':'.join(f'{b:02X}' for b in dot11[10:16])


def extract_rid_payload(dot11):
    """Finds the ODID payload (counter + message pack) inside an 802.11 frame,
    whether it arrived as a Beacon vendor IE or a NaN service descriptor.
    Returns the payload with the 1-byte message counter already stripped."""
    idx = dot11.find(BEACON_RID_SIGNATURE)
    if idx != -1:
        # ...FA 0B BC 0D <message counter> <message pack...>
        return dot11[idx + len(BEACON_RID_SIGNATURE) + 1:]

    idx = dot11.find(NAN_SERVICE_ID)
    if idx != -1:
        # service_id[6] instance_id requestor_instance_id service_control
        # service_info_length, then service info = <counter> <message pack...>
        after = idx + len(NAN_SERVICE_ID)
        if len(dot11) > after + 4:
            return dot11[after + 4 + 1:]
    return None


def decode_wifi_frame(frame):
    """Decodes one raw monitor-mode frame. Returns (transmitter_mac, telemetry,
    message_count, rssi_dbm) if it carried Remote ID, else None. message_count is per
    ODID message, not per frame — a Message Pack contributes all of its messages.
    rssi_dbm is None when the driver didn't report signal strength."""
    radiotap_len = parse_radiotap_length(frame)
    if radiotap_len is None:
        return None
    dot11 = frame[radiotap_len:]

    payload = extract_rid_payload(dot11)
    if not payload:
        return None

    telemetry, message_count = decode_odid_payload(payload)
    if not telemetry:
        return None

    mac = extract_transmitter_mac(dot11)
    if not mac:
        return None
    return mac, telemetry, message_count, parse_radiotap_signal(frame)


def _stale_reason(interface, bound_index, last_frame):
    """Why this socket should be abandoned, or None to keep using it.

    Checked on the recv timeout rather than on a timer: it costs nothing there, and
    the timeout is exactly the moment the socket has proved it isn't delivering."""
    try:
        current = socket.if_nametoindex(interface)
    except OSError:
        return f"{interface} no longer exists"
    if current != bound_index:
        return f"{interface} re-enumerated, index {bound_index} -> {current}"
    if time.time() - last_frame > SILENT_SOCKET_SECONDS:
        return f"no frames for {SILENT_SOCKET_SECONDS:.0f}s"
    return None


def capture(interface, on_detection, stop_event=None, channels=CAPTURE_CHANNELS):
    """Blocking capture loop. Calls on_detection(mac, telemetry, protocol, count) for
    every frame carrying Remote ID. Intended to run in its own thread.

    Rotates `interface` across `channels` while it runs; pass a single channel to
    park on it instead. The hopper is owned by this call and stopped when it
    returns, so a caller that restarts capture() after an error doesn't leave the
    old thread retuning the adapter underneath the new one.

    Returns when the socket goes stale, so the caller can rebind. An AF_PACKET
    socket is bound to an interface *index*, not a name, and that index changes when
    a USB adapter is replugged — the old socket then stays open and simply never
    delivers another frame. Nothing raises, so an error-only retry loop waits
    forever on a socket that is quietly dead. Moving the Alfa between USB ports did
    exactly this, and the status page still showed "monitor ch6" with no problems
    while capture had stopped entirely."""
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    sock.bind((interface, 0))
    sock.settimeout(1.0)
    # Index this socket is actually bound to, to compare against later.
    bound_index = socket.if_nametoindex(interface)

    last_frame = time.time()
    last_decode = [0.0]
    hop_stop = threading.Event()
    if len(channels) > 1:
        threading.Thread(target=channel_hopper,
                         args=(interface, channels, last_decode, hop_stop),
                         daemon=True).start()
    elif channels:
        set_channel(interface, channels[0])

    try:
        while stop_event is None or not stop_event.is_set():
            try:
                frame = sock.recv(4096)
            except socket.timeout:
                reason = _stale_reason(interface, bound_index, last_frame)
                if reason:
                    print(f"⚠️ Wi-Fi: capture socket on {interface} is stale ({reason}) "
                          f"— rebinding")
                    return
                continue
            last_frame = time.time()
            result = decode_wifi_frame(frame)
            if result:
                mac, telemetry, message_count, rssi_dbm = result
                last_decode[0] = time.time()
                on_detection(mac, telemetry, "Wi-Fi", message_count, rssi_dbm)
    finally:
        hop_stop.set()
        sock.close()
