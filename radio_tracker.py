import subprocess
import threading
import time
import csv
import json
import os
import re
import secrets
import sqlite3
import struct
import urllib.error
import urllib.request
import uuid
from datetime import datetime, timezone
from dotenv import load_dotenv

import wifi_remote_id
from odid_decode import ODID_BLE_SERVICE_SIGNATURE, decode_message, extract_odid_message

load_dotenv()

# Merged per-drone Remote ID state. Both transports rotate/bundle message types
# (Basic ID, Location, System, ...), so fields accumulate here across captures.
drone_state = {}

# --- MASTER CONFIGURATION ---
LOG_FILE = "amazon_drone_matches.csv"

# --- 📡 HARDWARE INTERFACE ROUTING ---
def detect_ble_interface():
    """The Bluetooth adapter to capture on: BLE_INTERFACE if set, else whichever
    hciN actually exists.

    Not hardcoded to hci0, because the index is assigned at enumeration and a USB
    dongle that resets comes back as hci1 — at which point a hardcoded name makes the
    tracker exit on every start with the adapter sitting right there, working. The
    onboard radio is disabled on this Pi, so the lowest-numbered adapter is the USB
    one; set BLE_INTERFACE explicitly on a host where that isn't true."""
    override = os.environ.get("BLE_INTERFACE", "").strip()
    if override:
        return override
    try:
        found = sorted(n for n in os.listdir("/sys/class/bluetooth") if n.startswith("hci"))
    except OSError:
        found = []
    if not found:
        print("⚠️ BLE: no Bluetooth adapter found; falling back to hci0")
        return "hci0"
    return found[0]

BLE_INTERFACE = detect_ble_interface()
# Alfa AWUS036ACM (MT7612U) in monitor mode. Separate radio from the BLE dongle, so
# the two capture paths never contend for airtime. Empty disables the Wi-Fi path.
WIFI_INTERFACE = os.environ.get("WIFI_INTERFACE", "wlan1")

def parse_wifi_channels(raw):
    """Comma-separated channel list for the Wi-Fi radio to rotate across; a single
    value parks on it. Falls back to the NaN channels rather than failing to boot,
    since a typo here shouldn't take the whole tracker down."""
    channels = tuple(int(c) for c in raw.replace(",", " ").split() if c.strip().isdigit())
    if raw.strip() and not channels:
        print(f"⚠️ Wi-Fi: couldn't read WIFI_CHANNELS={raw!r}, using "
              f"{list(wifi_remote_id.CAPTURE_CHANNELS)}")
    return channels or wifi_remote_id.CAPTURE_CHANNELS

WIFI_CHANNELS = parse_wifi_channels(os.environ.get("WIFI_CHANNELS", ""))

def configured_position():
    """This unit's own coordinates from .env, reported in the heartbeat.

    Deliberately labelled as *configured* rather than as the device's location: it's
    whatever someone typed into .env, so a unit that was physically moved without
    that file being updated reports the old spot confidently. Kept separate from the
    address an operator enters centrally so the two can be compared — a disagreement
    between them is a real signal about a real unit, and averaging them into one
    "location" would destroy exactly that."""
    try:
        return {"lat": float(os.environ["BASE_LAT"]), "lon": float(os.environ["BASE_LON"]),
                "source": "configured"}
    except (KeyError, ValueError):
        return None

# --- RADIO STATE FOR HEARTBEATS ---
# What the radios are actually doing, for the fleet status view. Written by the
# capture paths as they set things up, and read by the heartbeat thread.
#
# Everything here is either measured at heartbeat time or recorded at the moment it
# happened — never a "healthy" flag the process sets once and forgets, which is
# precisely the thing that stays true while a radio quietly stops working.
radio_state = {
    "started_at": None,
    "ble_mode": None,          # "extended" | "legacy" | "failed"
    "ble_stream": None,        # the hcidump Popen handle, so liveness is the real thing
    "last_detection_at": {},   # protocol -> ISO timestamp
}

# --- COOLDOWN TRACKING VARIABLES ---
last_audio_time = 0
AUDIO_COOLDOWN = 10
sent_alert_tracker = {}
ALERT_COOLDOWN = 120

def play_audio_alert():
    """Generates an immediate localized audio chime alert."""
    subprocess.Popen(["speaker-test", "-t", "sine", "-f", "1200", "-l", "1"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# --- CENTRAL SERVER REPORTING ---
# Every detection is queued locally (outbox.db) regardless of whether this device
# has been claimed yet or the central server is reachable — local capture (CSV,
# audio chime) never depends on the cloud side working. A background thread drains
# the outbox whenever it can.
CENTRAL_SERVER_URL = os.environ.get("CENTRAL_SERVER_URL", "http://127.0.0.1:8090")
CREDENTIALS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "device_credentials.json")
OUTBOX_DB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "outbox.db")
SYNC_INTERVAL = 15
# Matches the server's expectation; it treats three missed intervals as offline.
HEARTBEAT_INTERVAL = 60
HEARTBEAT_RETRY_INTERVAL = 10
HEARTBEAT_WARMUP_SECONDS = 20
# Matches the Wi-Fi path's retry cadence.
BLE_RETRY_SECONDS = 10
OUTBOX_BATCH_SIZE = 20

def load_or_create_credentials():
    """A device's identity (device_id + bootstrap_secret) is generated once, locally,
    on first run — never issued by the server — and persisted so it survives restarts."""
    if os.path.exists(CREDENTIALS_FILE):
        with open(CREDENTIALS_FILE) as f:
            creds = json.load(f)
    else:
        creds = {"device_id": str(uuid.uuid4()), "bootstrap_secret": secrets.token_urlsafe(32), "api_key": None}
        save_credentials(creds)
    return creds

def save_credentials(creds):
    with open(CREDENTIALS_FILE, "w") as f:
        json.dump(creds, f, indent=2)

def init_outbox_db():
    conn = sqlite3.connect(OUTBOX_DB)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS outbox (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            payload_json TEXT NOT NULL,
            created_at TEXT NOT NULL,
            sent_at TEXT
        )
    """)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS mac_identity (
            mac TEXT PRIMARY KEY,
            uas_id TEXT NOT NULL,
            ua_type TEXT,
            id_type TEXT,
            last_decoded_at REAL NOT NULL
        )
    """)
    conn.commit()
    conn.close()

# --- IDENTITY CACHE ---
# drone_state is pruned once a contact's cooldown expires, so each new sighting of the
# same aircraft starts from nothing. When a pass only yields a System message (operator
# location) we end up logging a bare MAC with every aircraft field N/A — even for a
# drone we've positively identified many times before. Remembering MAC -> identity
# across sightings recovers that.
#
# Anything recovered this way is marked inferred, never merged in as if freshly
# decoded: it's an inference from a hardware address, not something this pass actually
# heard, and an aviation log shouldn't blur that line. The TTL bounds the risk of a
# MAC being randomized or reassigned to a different aircraft.
IDENTITY_CACHE_TTL_SECONDS = 7 * 24 * 3600

def remember_identity(mac, telemetry):
    """Records a freshly *decoded* identity for this MAC. No-op for inferred values."""
    uas_id = telemetry.get("uas_id")
    if not uas_id or uas_id == "N/A":
        return
    try:
        conn = sqlite3.connect(OUTBOX_DB)
        conn.execute(
            "INSERT INTO mac_identity (mac, uas_id, ua_type, id_type, last_decoded_at) "
            "VALUES (?, ?, ?, ?, ?) ON CONFLICT(mac) DO UPDATE SET "
            "uas_id=excluded.uas_id, ua_type=excluded.ua_type, id_type=excluded.id_type, "
            "last_decoded_at=excluded.last_decoded_at",
            (mac, uas_id, telemetry.get("ua_type"), telemetry.get("id_type"), time.time()))
        conn.commit()
        conn.close()
    except Exception as e:
        print(f"⚠️ Identity cache write failed for {mac}: {e}")

def lookup_identity(macs):
    """Returns a previously decoded identity for any of these MACs, or None."""
    try:
        conn = sqlite3.connect(OUTBOX_DB)
        conn.row_factory = sqlite3.Row
        cutoff = time.time() - IDENTITY_CACHE_TTL_SECONDS
        placeholders = ','.join('?' * len(macs))
        row = conn.execute(
            f"SELECT * FROM mac_identity WHERE mac IN ({placeholders}) AND last_decoded_at >= ? "
            f"ORDER BY last_decoded_at DESC LIMIT 1",
            list(macs) + [cutoff]).fetchone()
        conn.close()
        return dict(row) if row else None
    except Exception as e:
        print(f"⚠️ Identity cache read failed: {e}")
        return None

def enqueue_detection(icao_hex, protocol, telemetry, detected_at, identity_source="decoded",
                      message_count=0, rssi_dbm=None):
    payload = {
        "detected_at": detected_at,
        "mac": icao_hex,
        "protocol": protocol,
        "uas_id": telemetry.get("uas_id"),
        "ua_type": telemetry.get("ua_type"),
        "lat": telemetry.get("lat"),
        "lon": telemetry.get("lon"),
        "altitude_m": telemetry.get("altitude_m"),
        "speed_mps": telemetry.get("speed_mps"),
        "operator_lat": telemetry.get("operator_lat"),
        "operator_lon": telemetry.get("operator_lon"),
        "operator_altitude_m": telemetry.get("operator_altitude_m"),
        "identity_source": identity_source,
        "message_count": message_count,
        "altitude_ref": telemetry.get("altitude_ref", "N/A"),
        "operator_location_type": telemetry.get("operator_location_type", "N/A"),
        "rssi_dbm": rssi_dbm,
    }
    conn = sqlite3.connect(OUTBOX_DB)
    conn.execute("INSERT INTO outbox (payload_json, created_at) VALUES (?, ?)",
                 (json.dumps(payload), detected_at))
    conn.commit()
    conn.close()

def _central_request(method, path, headers=None, body=None, timeout=10):
    url = f"{CENTRAL_SERVER_URL}{path}"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                  headers={"Content-Type": "application/json", **(headers or {})})
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return json.loads(response.read().decode())

def ensure_claimed(creds):
    """Registers the device (idempotent) and, if a human has since claimed it via
    the claim code, picks up the real API key. Never blocks/retries in a loop here —
    called once per sync cycle so a slow/unreachable server can't stall local capture."""
    if creds.get("api_key"):
        return True
    try:
        _central_request("POST", "/v1/devices/claim", body={
            "device_id": creds["device_id"], "bootstrap_secret": creds["bootstrap_secret"],
        })
        status = _central_request("GET", f"/v1/devices/{creds['device_id']}/status",
                                   headers={"X-Bootstrap-Secret": creds["bootstrap_secret"]})
        if status.get("status") == "claimed" and status.get("api_key"):
            creds["api_key"] = status["api_key"]
            save_credentials(creds)
            print(f"✅ Central server: device claimed, now reporting detections.")
            return True
        if status.get("claim_code"):
            # Points at the page rather than the API on purpose. Claiming needs a
            # signed-in session — the email was deliberately removed from the request
            # body, since accepting it let anyone holding a code bind the device to
            # any address they typed. This message used to describe that old call and
            # would have sent every new user down a path that answers "sign in first".
            print(f"🔑 Central server: unclaimed. Claim code: {status['claim_code']} "
                  f"— enter this at {CENTRAL_SERVER_URL}/receivers")
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        print(f"⚠️ Central server unreachable during claim check: {e}")
    return False

def sync_outbox(creds):
    conn = sqlite3.connect(OUTBOX_DB)
    conn.row_factory = sqlite3.Row
    rows = conn.execute(
        "SELECT id, payload_json FROM outbox WHERE sent_at IS NULL ORDER BY id LIMIT ?",
        (OUTBOX_BATCH_SIZE,)
    ).fetchall()
    if not rows:
        conn.close()
        return

    detections = [json.loads(r["payload_json"]) for r in rows]
    try:
        _central_request("POST", "/v1/detections/batch",
                          headers={"Authorization": f"Bearer {creds['api_key']}"},
                          body={"detections": detections})
        ids = [r["id"] for r in rows]
        conn.execute(f"UPDATE outbox SET sent_at = ? WHERE id IN ({','.join('?' * len(ids))})",
                     [datetime.now(timezone.utc).isoformat()] + ids)
        conn.commit()
        print(f"📡 Central server: synced {len(ids)} detection(s).")
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        print(f"⚠️ Central server sync failed, will retry: {e}")
    finally:
        conn.close()

def collect_radio_status():
    """Measured radio state for the heartbeat.

    Reads the hardware rather than internal beliefs wherever it can: monitor mode and
    channel come from `iw`, not from what was requested at boot, because the failure
    that matters is NetworkManager reclaiming the interface hours later while every
    internal flag still says it was configured correctly.

    `problems` is the part the fleet view acts on. A device that is reachable but has
    a dead radio would otherwise show as healthy, since it is still heartbeating."""
    problems = []

    ble_stream = radio_state["ble_stream"]
    ble_alive = ble_stream is not None and ble_stream.poll() is None
    if not ble_alive:
        problems.append("ble_stream_down")
    if radio_state["ble_mode"] == "legacy":
        # Not fatal, but it silently drops Bluetooth 5 Long Range coverage, and it
        # happens on any restart where the controller was left scanning.
        problems.append("ble_legacy_scanning")
    elif radio_state["ble_mode"] != "extended":
        problems.append("ble_not_scanning")

    wifi = {"interface": WIFI_INTERFACE or None, "mode": None, "channel": None}
    if WIFI_INTERFACE:
        try:
            out = subprocess.run(["iw", "dev", WIFI_INTERFACE, "info"],
                                 capture_output=True, text=True, timeout=10).stdout
            for line in out.splitlines():
                line = line.strip()
                if line.startswith("type "):
                    wifi["mode"] = line.split(None, 1)[1]
                elif line.startswith("channel "):
                    wifi["channel"] = int(line.split()[1])
        except Exception as e:
            wifi["error"] = str(e)
        if wifi["mode"] != "monitor":
            problems.append("wifi_not_monitor")

    try:
        conn = sqlite3.connect(OUTBOX_DB)
        pending = conn.execute("SELECT COUNT(*) FROM outbox WHERE sent_at IS NULL").fetchone()[0]
        conn.close()
    except Exception:
        pending = None

    return {
        "started_at": radio_state["started_at"],
        "ble": {"mode": radio_state["ble_mode"], "stream_alive": ble_alive},
        "wifi": wifi,
        "last_detection_at": dict(radio_state["last_detection_at"]),
        "outbox_pending": pending,
        "position": configured_position(),
        "problems": problems,
    }


def heartbeat_loop(creds):
    """Reports liveness plus measured radio state on a fixed interval.

    Separate from the detection sync on purpose: detections are event-driven and a
    quiet sky produces none for hours, so they can't distinguish a working receiver
    over an empty neighbourhood from a dead one. A heartbeat is the only signal that
    keeps arriving when nothing is flying."""
    # Let the radios finish coming up before reporting on them. Bringing up the BLE
    # adapter, negotiating extended scanning and starting the capture pipe takes a
    # few seconds, so an immediate first report describes a half-booted device and
    # flags problems that resolve themselves — the fastest way to teach an operator
    # to ignore this page. Well inside the offline threshold, so a device that dies
    # during boot still shows up as never reporting.
    time.sleep(HEARTBEAT_WARMUP_SECONDS)
    while True:
        delay = HEARTBEAT_INTERVAL
        try:
            if creds.get("api_key"):
                _central_request(
                    "POST", f"/v1/devices/{creds['device_id']}/heartbeat",
                    headers={"Authorization": f"Bearer {creds['api_key']}"},
                    body={"status": collect_radio_status()})
        except Exception as e:
            # Never fatal and never noisy: the central server being unreachable must
            # not disturb local capture, which is the part that actually matters.
            # Retry sooner than the normal cadence, because the common cause is both
            # services restarting together and this one winning the race — otherwise a
            # device looks offline for a full interval over a one-second overlap.
            delay = HEARTBEAT_RETRY_INTERVAL
            print(f"⚠️ Heartbeat failed (retrying in {delay}s): {e}")
        time.sleep(delay)


def central_sync_loop():
    """Runs in a background thread for the life of the process — local BLE capture
    in the main thread never waits on any of this."""
    init_outbox_db()
    creds = load_or_create_credentials()
    while True:
        try:
            if ensure_claimed(creds):
                sync_outbox(creds)
        except Exception as e:
            print(f"⚠️ Central sync loop error: {e}")
        time.sleep(SYNC_INTERVAL)

# HCI LE Meta subevents carrying advertisements. Both are handled because the two
# report formats differ:
#
#   04 3E <len> 02 <n> <report>...              legacy
#   04 3E <len> 0D <n> <report>...              extended
#
# Extended reports are what a Bluetooth 5 controller emits once extended scanning is
# enabled — including for legacy advertisers — so rejecting them (as this did
# originally) silently discards *everything* in that mode, and blinds the receiver to
# Bluetooth 5 Long Range Remote ID entirely.
#
# <n> is num_reports: a single event routinely batches several advertisers' reports,
# each with its own address and its own advertising data. Per report:
#
#   0x02 legacy:   event_type(1) addr_type(1) address(6)
#                  | data_length(1) | data | rssi(1)
#   0x0D extended: event_type(2) addr_type(1) address(6) primary_phy(1)
#                  secondary_phy(1) sid(1) tx_power(1) rssi(1)
#                  periodic_adv_interval(2) direct_addr_type(1) direct_addr(6)
#                  | data_length(1) | data
#
# Reports are variable-length, so report N is only reachable by walking reports
# 0..N-1 — the fixed offset this used to index by always returned the *first*
# report's address, which attributed a later report's Remote ID payload to an
# unrelated device's MAC. Sizes below are (address offset within the report, bytes
# before data_length, bytes after data, RSSI offset).
#
# The two formats put RSSI in different places: extended carries it in the header at
# a fixed offset, legacy carries it in the trailer byte after the variable-length
# data, so its position is only known once that report's length has been read. None
# means "the trailer byte".
ADV_REPORT_LAYOUTS = {0x02: (2, 8, 1, None), 0x0D: (3, 23, 0, 13)}

# Spec value for "RSSI not available" — a real reading is negative, so letting 127
# through would look like an impossibly strong signal.
RSSI_UNAVAILABLE = 127

def hcidump_event_bytes(packet_str):
    """hcidump -R prints one HCI event as '> ' followed by hex byte tokens, wrapped
    across indented continuation lines. Stops at the first token that isn't a hex
    byte, since anything past it would shift every offset that follows."""
    raw = bytearray()
    for token in packet_str.replace('>', '').split():
        if not re.fullmatch(r'[0-9A-Fa-f]{2}', token):
            break
        raw.append(int(token, 16))
    return bytes(raw)

def parse_adv_reports(event):
    """Walks an HCI LE Advertising Report event and returns [(address, adv_data,
    rssi_dbm)] for every report it carries, each address and signal level paired with
    the data from that same report. rssi_dbm is None when the controller reported it
    as unavailable. Empty for anything that isn't a legacy (0x02) or extended (0x0D)
    report event."""
    if len(event) < 5 or event[0] != 0x04 or event[1] != 0x3E:
        return []
    layout = ADV_REPORT_LAYOUTS.get(event[3])
    if layout is None:
        return []
    addr_offset, header_len, trailer_len, rssi_offset = layout
    # event[2] is the HCI parameter length; ignore anything the dump ran on past it.
    event = event[:3 + event[2]]

    reports = []
    pos = 5
    for _ in range(event[4]):
        if pos + header_len >= len(event):
            break
        data_start = pos + header_len + 1
        data_end = data_start + event[pos + header_len]
        addr = event[pos + addr_offset:pos + addr_offset + 6]
        # Header-relative for extended, the trailer byte after this report's data for
        # legacy — which is only locatable now that data_end is known.
        rssi_at = pos + rssi_offset if rssi_offset is not None else data_end
        rssi = None
        if rssi_at < len(event):
            raw = struct.unpack('b', event[rssi_at:rssi_at + 1])[0]
            rssi = None if raw == RSSI_UNAVAILABLE else raw
        reports.append((
            ':'.join(f'{b:02X}' for b in reversed(addr)) if len(addr) == 6 else None,
            event[data_start:data_end],
            rssi,
        ))
        if data_end + trailer_len > len(event):
            # Event truncated mid-report: what we just took is all there is of it,
            # and where the next report would start is no longer knowable.
            break
        pos = data_end + trailer_len
    return reports

# ASTM F3411 broadcasts rotate through several message types (Basic ID, Location,
# System, Self ID, ...) roughly once every 1-3s each. Logging the instant the FIRST
# one decodes means the CSV row only ever reflects whichever type happened to show
# up first — checked across a real capture session, only 2 of 18 logged hits ever
# had both a UAS ID and a position. Instead, wait a short grace period after the
# first decoded field to let more message types roll in and enrich the same
# drone_state entry before actually writing/alerting.
PENDING_ALERT_GRACE_SECONDS = 10
pending_alert_timers = {}
pending_alert_protocols = {}
pending_alert_lock = threading.Lock()

# A drone's Wi-Fi MAC differs from its BLE MAC, so the same aircraft picked up on
# both transports would otherwise be logged twice as two unrelated contacts. Once a
# Basic ID message reveals the UAS ID (a real identity, transport-independent), key
# that drone's state by it instead and remember the mapping so later packets from
# either MAC land on the same entry.
transport_mac_to_key = {}
# Real transport MAC(s) per state key. Kept separate from the telemetry dict so the
# CSV/central-server "mac" field still carries actual hardware addresses even after a
# drone's state has been re-keyed to its UAS ID — a drone seen on both transports has
# two of them, and both are worth recording.
key_to_macs = {}
# Running tally of ODID messages behind each contact, kept out of the telemetry dict
# so it never gets mistaken for a decoded field.
key_to_msg_count = {}
# Strongest signal seen for each contact, same reasoning — it's a property of
# reception, not something the aircraft broadcast, so it must not sit in telemetry.
# Peak rather than latest: signal tracks distance, so the maximum over a pass is the
# closest approach, which is the figure that's comparable between antenna placements.
# A pass that ends far away would otherwise report a weak signal that says nothing
# about the receiver.
key_to_rssi = {}

def resolve_state_key(transport_mac):
    return transport_mac_to_key.get(transport_mac, transport_mac)

def merge_state(transport_mac, decoded):
    """Merges newly decoded fields into this drone's accumulated state, re-keying
    from transport MAC to UAS ID the first time we learn it. Returns the key."""
    key = resolve_state_key(transport_mac)
    state = drone_state.setdefault(key, {})
    state.update(decoded)
    key_to_macs.setdefault(key, set()).add(transport_mac)

    uas_id = state.get("uas_id")
    if uas_id and uas_id != "N/A" and key != uas_id:
        # Promote to a UAS-ID key, folding in anything already accumulated under
        # the other transport's MAC (or this one's, before the ID was known).
        merged = drone_state.pop(key, {})
        target = drone_state.setdefault(uas_id, {})
        target.update(merged)
        key_to_macs.setdefault(uas_id, set()).update(key_to_macs.pop(key, set()))
        key_to_msg_count[uas_id] = key_to_msg_count.get(uas_id, 0) + key_to_msg_count.pop(key, 0)
        carried = key_to_rssi.pop(key, None)
        if carried is not None:
            existing = key_to_rssi.get(uas_id)
            key_to_rssi[uas_id] = carried if existing is None else max(existing, carried)
        transport_mac_to_key[transport_mac] = uas_id
        # A live timer is never carried across a re-key. It was constructed with the
        # old key baked into its callback arguments, so moving the object left it
        # firing finalize_pending_alert() on a key whose state had just been popped:
        # the detection was silently dropped, and because the dead timer stayed
        # registered under the new key, every later packet from that aircraft saw
        # "already pending" and returned. One aircraft could be suppressed for the
        # life of the process.
        #
        # Cancelled instead, and left absent from pending_alert_timers so the caller
        # schedules a fresh one against the canonical key. That restarts the grace
        # period, which is the right behaviour anyway: the Basic ID that triggered the
        # re-key is new evidence worth waiting a moment on.
        with pending_alert_lock:
            pending_alert_protocols.setdefault(uas_id, set()).update(
                pending_alert_protocols.pop(key, set()))
            stale = pending_alert_timers.pop(key, None)
            if stale is not None:
                stale.cancel()
        return uas_id
    return key

def finalize_pending_alert(key):
    with pending_alert_lock:
        pending_alert_timers.pop(key, None)
        protocols = pending_alert_protocols.pop(key, set())
    state = drone_state.get(key)
    if not state:
        return

    # Log the real hardware address(es) and every transport that contributed,
    # not the internal dedup key — that key is the UAS ID once identified.
    mac_set = key_to_macs.get(key, {key})
    macs = " + ".join(sorted(mac_set))
    protocol = " + ".join(sorted(protocols)) or "Remote ID"

    identity_source = "decoded"
    if state.get("uas_id") and state["uas_id"] != "N/A":
        for mac in mac_set:
            remember_identity(mac, state)
    else:
        # Nothing identifying decoded this pass — fall back to what this MAC told us
        # on an earlier pass, clearly marked as inferred rather than heard just now.
        remembered = lookup_identity(mac_set)
        if remembered:
            state = dict(state)
            state["uas_id"] = remembered["uas_id"]
            state["ua_type"] = remembered["ua_type"]
            state["id_type"] = remembered["id_type"]
            identity_source = "inferred-from-mac"

    log_and_alert(macs, protocol, state, dedup_key=key, identity_source=identity_source,
                  message_count=key_to_msg_count.get(key, 0),
                  rssi_dbm=key_to_rssi.get(key))

def register_detection(transport_mac, decoded, protocol, message_count=1, rssi_dbm=None):
    """Shared entry point for every transport: merge what decoded, then schedule a
    single grace-period alert so more message types can roll in first.

    message_count is how many ODID messages this capture contributed — 1 for a BLE
    advertisement, up to 9 for a Wi-Fi Message Pack — accumulated per contact as a
    rough confidence signal (a hit backed by one message is far weaker evidence
    than one backed by fifteen).

    rssi_dbm is this capture's received signal strength, kept as the peak across the
    contact. Unlike everything else here it describes the receiver, not the aircraft,
    which is exactly why it's useful: it's the only measurement that says whether an
    antenna change helped, without waiting weeks for hit counts to diverge."""
    if not decoded:
        # Wait for at least one of the message types we decode before scheduling
        # anything — a hit shouldn't get logged as a bare MAC with every field N/A
        # just because the first packet we saw was an undecoded type.
        return

    key = merge_state(transport_mac, decoded)
    key_to_msg_count[key] = key_to_msg_count.get(key, 0) + message_count
    if rssi_dbm is not None:
        previous = key_to_rssi.get(key)
        key_to_rssi[key] = rssi_dbm if previous is None else max(previous, rssi_dbm)

    # Drop this key's cooldown entry once it has served its time. Testing presence
    # alone meant an expired entry suppressed the aircraft indefinitely: the only code
    # that expired entries lived in log_and_alert, which a suppressed key can never
    # reach, because being suppressed is exactly what stops a timer being scheduled.
    # On a quiet receiver seeing one aircraft repeatedly, the two-minute cooldown
    # became permanent.
    sent_at = sent_alert_tracker.get(key)
    if sent_at is not None and time.time() - sent_at >= ALERT_COOLDOWN:
        sent_alert_tracker.pop(key, None)

    with pending_alert_lock:
        # Record every transport that contributed, even when a timer is already
        # pending — a drone heard on both BLE and Wi-Fi should say so.
        pending_alert_protocols.setdefault(key, set()).add(protocol)
        if key in sent_alert_tracker or key in pending_alert_timers:
            # Already alerted this cooldown window, or already waiting on a grace-
            # period timer that will pick up whatever more has decoded by the time
            # it fires — either way, this packet's contribution to state is enough.
            return
        timer = threading.Timer(PENDING_ALERT_GRACE_SECONDS, finalize_pending_alert, args=(key,))
        timer.daemon = True
        pending_alert_timers[key] = timer
        timer.start()

def check_packet_for_remote_id(packet_str):
    """Checks one buffered HCI event for the ASTM F3411 Remote ID BLE signature
    (AD Type 0x16, Service UUID 0xFFFA little-endian, AD Application Code 0x0D).

    An event can batch several advertisers' reports, so each is decoded on its own
    and registered against its own advertiser's address. Registering more than one
    detection per event matters both ways: pairing a payload with the wrong MAC
    merges telemetry into an unrelated contact (drone_state is keyed by transport MAC
    until a Basic ID reveals the UAS ID), and decoding only the first payload drops
    messages from exactly the brief contacts where every message counts."""
    # Matched against the parsed bytes, not the dump text: hcidump wraps an event
    # every 20 bytes, so a text search for the signature misses every event unlucky
    # enough to have a line break fall inside it.
    event = hcidump_event_bytes(packet_str)
    if ODID_BLE_SERVICE_SIGNATURE not in event:
        return

    reports = parse_adv_reports(event)
    if not reports:
        # Not a report event this knows how to walk — scan the whole buffer, which is
        # what every event got before reports were walked individually.
        reports = [(None, event, None)]

    for address, adv_data, rssi_dbm in reports:
        msg = extract_odid_message(adv_data)
        if msg:
            register_detection(address or "BLE-REMOTE-ID-UNIT", decode_message(msg),
                               "BLE", 1, rssi_dbm=rssi_dbm)

def command_complete_status(output, ocf):
    """Status byte from the Command Complete event matching this command's opcode.

    Must actually find the event rather than reading the last byte printed. hcitool
    dumps every HCI event it sees while waiting, and with extended scanning enabled
    the controller is emitting a torrent of advertising reports — so "last token of
    stdout" lands on a byte of somebody's advertisement. That misread a successful
    command as status 0x38 and demoted the radio to legacy scanning, losing Long
    Range coverage, while the same command run by hand returned 0x00.

    Command Complete (0x0e) payload is: num_allowed_cmd_packets, opcode low byte,
    opcode high byte, status. Opcode is (OGF << 10) | OCF, and every command here is
    OGF 0x08, so matching the opcode is what ties the status to *this* command."""
    opcode = (0x08 << 10) | int(ocf, 16)
    want = (f"{opcode & 0xFF:02X}", f"{opcode >> 8:02X}")
    lines = output.splitlines()
    for i, line in enumerate(lines):
        if "HCI Event: 0x0e" not in line or i + 1 >= len(lines):
            continue
        parts = lines[i + 1].split()
        if len(parts) >= 4 and (parts[1].upper(), parts[2].upper()) == want:
            return parts[3].upper()
    return None


def hci_command(ocf_bytes, description, warn=True):
    """Runs a raw HCI command and reports whether the controller accepted it.
    Status 00 is success.

    warn=False for commands whose failure is expected and harmless, so a routine
    no-op doesn't read like a hardware fault in the log."""
    try:
        out = subprocess.run(["sudo", "hcitool", "-i", BLE_INTERFACE, "cmd", "0x08"] + ocf_bytes,
                             capture_output=True, text=True, timeout=10).stdout
        status = command_complete_status(out, ocf_bytes[0])
        ok = status == "00"
        if not ok and warn:
            print(f"⚠️ BLE: {description} rejected by controller "
                  f"(status {status or 'no Command Complete seen'})")
        return ok
    except Exception as e:
        print(f"⚠️ BLE: {description} failed: {e}")
        return False

def start_ble_scanning():
    """Enables Bluetooth 5 extended scanning across both 1M and Coded (Long Range)
    PHYs, falling back to legacy scanning on controllers that don't support it.

    ASTM F3411 permits Bluetooth 5 Long Range as a transmit method, and a drone using
    it is invisible to legacy `hcitool lescan` — which is what this used to do, so
    anything transmitting on the Coded PHY went unheard.

    This was originally written to explain why no Wing aircraft was ever captured
    locally despite the co-located DroneSight receiver seeing them regularly. That
    was the wrong diagnosis: Wing doesn't broadcast over Bluetooth here at all. Every
    Wing track that receiver has logged carries a `wifi:` correlation ID and every
    Amazon track a `ble:` one, so the Wing gap was always a Wi-Fi coverage problem
    (see wifi_remote_id.py). Extended scanning is still correct — it's the only way
    to hear Long Range at all — it just isn't what was missing.

    Note a BT5 controller reports *all* advertising as extended (0x0D) once extended
    scanning is on, including legacy advertisers, so this doesn't lose legacy coverage
    — but the parser has to accept 0x0D, which it now does."""
    # Clear whatever the last run left behind, since scanning is controller state and
    # outlives the process that started it — systemd's SIGTERM kills this one without
    # unwinding to stop_ble_scanning().
    #
    # Both kinds have to go. A leftover *legacy* scanner is the nastier case: the
    # fallback below launches `hcitool lescan` as a background process that survives
    # the restart, and legacy scanning makes the controller reject LE Set Extended
    # Scan Enable with Command Disallowed — so one fallback to legacy traps every
    # subsequent start in legacy forever. Killing the process is deliberately how
    # that's cleared rather than a raw legacy-disable HCI command, which was observed
    # to wedge this adapter badly enough that it re-enumerated.
    os.system("sudo killall hcitool > /dev/null 2>&1")
    hci_command(["0x0042", "00", "00", "00", "00", "00", "00"],
                "extended scan disable", warn=False)
    # LE Set Extended Scan Parameters: own_addr=public, no filter policy,
    # PHYs = 1M | Coded (0x05), then passive scan + interval/window per PHY.
    params_ok = hci_command(
        ["0x0041", "00", "00", "05", "00", "10", "00", "10", "00", "00", "10", "00", "10", "00"],
        "extended scan parameters")
    # LE Set Extended Scan Enable: enable, keep duplicates, no duration/period limit.
    enable_ok = params_ok and hci_command(
        ["0x0042", "01", "00", "00", "00", "00", "00"], "extended scan enable")

    if enable_ok:
        radio_state["ble_mode"] = "extended"
        print("📡 BLE: extended scanning active (1M + Coded PHY / Long Range)")
        return
    radio_state["ble_mode"] = "legacy"
    print("📡 BLE: falling back to legacy scanning (no Bluetooth 5 Long Range coverage)")
    os.system(f"sudo hcitool -i {BLE_INTERFACE} lescan --duplicates --passive > /dev/null 2>&1 &")

def stop_ble_scanning():
    hci_command(["0x0042", "00", "00", "00", "00", "00", "00"], "extended scan disable")

def parse_ble_remote_id():
    """Forces hci0 into a permanent, unbroken streaming mode to catch fast-moving targets."""
    # bluetoothd is masked (it fights hcitool over scan parameters), which also means
    # nothing else brings the adapter up after boot — do it ourselves before scanning.
    os.system(f"sudo hciconfig {BLE_INTERFACE} up > /dev/null 2>&1")
    time.sleep(1.0)

    start_ble_scanning()

    # Launch a continuous system stream pipe task
    cmd = ["sudo", "hcidump", "-i", BLE_INTERFACE, "-R"]
    try:
        # HARDWARE WARM-UP CUSHION: Gives the StarTech USB adapter ample time to bind before stream capture
        time.sleep(2.0)  
        
        # FIXED: Pass unbuffered raw streaming parameters to keep the pipe open permanently
        process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        # Held so the heartbeat can poll the actual process rather than trust a flag.
        radio_state["ble_stream"] = process

        # FIXED: Robust direct stream byte loop prevents EOF dropout crashes
        # hcidump -R starts each HCI event with "> " and wraps its bytes across
        # indented continuation lines, so a signature can straddle a line break —
        # buffer the full event before searching it.
        packet_buffer = ""
        while True:
            line = process.stdout.readline()
            if not line:
                if process.poll() is not None:
                    stderr_output = process.stderr.read().decode('utf-8', errors='ignore').strip()
                    raise RuntimeError(f"hcidump exited (code {process.returncode}): {stderr_output or 'no output'}")
                # Catch empty radio buffer states safely and wait for next active burst frame
                time.sleep(0.1)
                continue

            # Decodes raw incoming buffer packets into string profiles
            line_str = line.decode('utf-8', errors='ignore')

            if line_str.startswith('>'):
                if packet_buffer:
                    check_packet_for_remote_id(packet_buffer)
                packet_buffer = line_str
            else:
                packet_buffer += line_str

    except Exception as e:
        print(f"⚠️ Radio Stream Hiccup: {e}")
    finally:
        if 'process' in locals():
            process.terminate()
            process.wait()
        # Extended scanning is controller state, not a process — killing hcidump
        # doesn't stop it, so turn it off explicitly.
        stop_ble_scanning()
        os.system("sudo killall hcidump hcitool > /dev/null 2>&1")

def log_and_alert(icao_hex, protocol, telemetry=None, dedup_key=None, identity_source="decoded",
                  message_count=0, rssi_dbm=None):
    """Saves verified hits to CSV and plays a local audio chime.

    icao_hex is the real transport MAC(s), for display/logging. dedup_key is the
    internal identity used for cooldown bookkeeping — the same drone's UAS ID once
    known, so a drone seen on both BLE and Wi-Fi shares one cooldown rather than
    alerting once per transport. They differ, so don't collapse them.

    identity_source records whether uas_id/ua_type were actually decoded on this pass
    ("decoded") or recovered from an earlier sighting of the same MAC
    ("inferred-from-mac"), so downstream never presents an inference as a measurement."""
    global last_audio_time
    telemetry = telemetry or {}
    dedup_key = dedup_key or icao_hex
    # UTC ISO format matches the DroneSight cloud timestamps so the dashboard can
    # convert both consistently to the viewer's local time.
    timestamp = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3] + 'Z'

    current_time = time.time()
    if dedup_key in sent_alert_tracker:
        if current_time - sent_alert_tracker[dedup_key] < ALERT_COOLDOWN:
            return

    # Drop entries that have already cleared their cooldown so the trackers don't grow forever
    for tracked_key in [h for h, t in sent_alert_tracker.items() if current_time - t >= ALERT_COOLDOWN]:
        del sent_alert_tracker[tracked_key]
        drone_state.pop(tracked_key, None)
        key_to_msg_count.pop(tracked_key, None)
        key_to_rssi.pop(tracked_key, None)
        for mac in key_to_macs.pop(tracked_key, set()):
            transport_mac_to_key.pop(mac, None)

    uas_id = telemetry.get("uas_id", "N/A")
    ua_type = telemetry.get("ua_type", "N/A")
    lat = telemetry.get("lat", "N/A")
    lon = telemetry.get("lon", "N/A")
    altitude_m = telemetry.get("altitude_m", "N/A")
    speed_mps = telemetry.get("speed_mps", "N/A")
    operator_lat = telemetry.get("operator_lat", "N/A")
    operator_lon = telemetry.get("operator_lon", "N/A")
    operator_altitude_m = telemetry.get("operator_altitude_m", "N/A")
    altitude_ref = telemetry.get("altitude_ref", "N/A")
    operator_location_type = telemetry.get("operator_location_type", "N/A")
    rssi = "N/A" if rssi_dbm is None else rssi_dbm

    with open(LOG_FILE, mode='a', newline='') as f:
        writer = csv.writer(f)
        # Appended, never reordered — older rows are shorter and readers index by
        # position, so a new column has to land on the end.
        writer.writerow([timestamp, icao_hex, protocol, uas_id, ua_type, lat, lon, altitude_m, speed_mps,
                          operator_lat, operator_lon, operator_altitude_m, identity_source,
                          message_count, altitude_ref, operator_location_type, rssi])

    # Queued locally regardless of central server reachability/claim status — the
    # background sync thread drains this whenever it can, see central_sync_loop().
    enqueue_detection(icao_hex, protocol, telemetry, timestamp, identity_source, message_count,
                      rssi_dbm)

    inferred_note = " [identity inferred from earlier sighting]" if identity_source != "decoded" else ""
    signal_note = "" if rssi_dbm is None else f" [{rssi_dbm} dBm]"
    print(f"🎯 [MATCH] {protocol} Alert! Target: {icao_hex} ({ua_type}) "
          f"[{message_count} msg]{signal_note}{inferred_note}")

    if time.time() - last_audio_time > AUDIO_COOLDOWN:
        last_audio_time = time.time()
        play_audio_alert()

    sent_alert_tracker[dedup_key] = current_time
    for single in protocol.split("+"):
        radio_state["last_detection_at"][single.strip()] = timestamp

def wifi_capture_loop():
    """Wi-Fi Remote ID capture, in its own thread. Restarts on error rather than
    dying silently — a Wi-Fi problem must never take down BLE capture."""
    while True:
        try:
            wifi_remote_id.capture(
                WIFI_INTERFACE,
                lambda mac, telemetry, protocol, count, rssi: register_detection(
                    mac, telemetry, protocol, count, rssi_dbm=rssi),
                channels=WIFI_CHANNELS,
            )
        except Exception as e:
            print(f"⚠️ Wi-Fi capture error on {WIFI_INTERFACE} (retrying in 10s): {e}")
        time.sleep(10)

# --- INITIALIZATION ENGINE ---
# Guarded so importing this module is safe. Without it, `import radio_tracker` — to
# reuse a decoder in a test or a one-off script — silently starts a *second* tracker:
# claims the HCI socket, opens its own hcidump, and on the way out runs
# `killall hcidump`, which kills the real service's capture pipe too. That happened
# twice while working on this file, the second time taking the running service down
# with it. The decoders are worth importing on their own; starting the radios is what
# should require actually running this.
def ble_capture_loop():
    """Keeps BLE capture running across adapter disappearances.

    Unplugging the dongle ends the hcidump stream, which used to return straight out
    of main() — the process exited cleanly, so systemd's Restart=on-failure saw
    nothing to restart and the tracker simply stopped. Swapping a USB cable killed
    capture until someone noticed by hand.

    Re-detects the adapter on each attempt rather than reusing the name: a dongle
    that is replugged frequently comes back on a different hciN, so retrying the old
    one would fail forever with working hardware attached."""
    global BLE_INTERFACE
    while True:
        BLE_INTERFACE = detect_ble_interface()
        try:
            parse_ble_remote_id()
        except Exception as e:
            print(f"⚠️ BLE capture error on {BLE_INTERFACE}: {e}")
        print(f"⚠️ BLE: capture on {BLE_INTERFACE} stopped; retrying in "
              f"{BLE_RETRY_SECONDS}s")
        time.sleep(BLE_RETRY_SECONDS)


def main():
    print("🛰️ Booting High-Performance Tri-Core BLE Core...")
    print(f"📋 BLE interface: {BLE_INTERFACE}")
    radio_state["started_at"] = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3] + 'Z'

    threading.Thread(target=central_sync_loop, daemon=True).start()
    threading.Thread(target=heartbeat_loop, args=(load_or_create_credentials(),),
                     daemon=True).start()

    if WIFI_INTERFACE:
        print(f"📶 Wi-Fi Remote ID interface: {WIFI_INTERFACE} (monitor mode, "
              f"channels {', '.join(str(c) for c in WIFI_CHANNELS)})")
        threading.Thread(target=wifi_capture_loop, daemon=True).start()

    try:
        # Retries rather than returning, so a replugged adapter is picked back up.
        ble_capture_loop()
    except KeyboardInterrupt:
        print("\n[!] Shutting down drone monitoring loop cleanly.")
        stop_ble_scanning()
        os.system("sudo killall hcidump hcitool > /dev/null 2>&1")


if __name__ == "__main__":
    main()
