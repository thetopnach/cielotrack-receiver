# Building a receiver (ESP32 carrier)

Start-to-finish for the two-board carrier: a **master** (Bluetooth LE, has Wi-Fi, uploads
for both) and a **sensor** (Wi-Fi Remote ID capture, no network). A single board is a
simpler subset — see [One board](#one-board) at the end.

Pinouts are **not** repeated here: [`WIRING.md`](WIRING.md) is the authority for the pins,
the link, and the power rail, and has been wrong once from being copied rather than
measured. This is the order to do things in, and how to configure and provision each board.

## 1. Parts
- 2 × Seeed **XIAO ESP32-S3** (the WROOM-1U variant for an external antenna).
- 2 × 2.4 GHz antennas with short (10–15 cm) pigtails.
- A **keyed** 3-pin connector for the inter-board link — never bare 0.1″ jumpers, because a
  reversible plug can put 5 V on the data pin.
- One USB-C supply (the pair runs off one; see `WIRING.md` § Power).
- The printed **carrier enclosure** (base + lid, or the `dipole` / `wallmount` variants),
  from the enclosure page on the dashboard (`/enclosure`).

## 2. Wire the pair
Follow `WIRING.md` exactly — sensor **D9 (TX) → master D10 (RX)**, **GND ↔ GND**,
**UUSB ↔ UUSB** (5 V). **Do not link 3V3** (it bypasses a regulator and browns out the
sensor, which looks like a software fault). Multimeter-check USB-5V → UUSB on both boards
before relying on it.

## 3. Configure `secrets.h` (per role)
Copy `main/secrets.h.example` to two role files, and **never edit `secrets.h` in place**
(doing so once compiled the forwarding path out of a master while it reported healthy):

- `main/secrets.ble.h` — the **master**.
- `main/secrets.wifi.h` — the **sensor** (`CIELOTRACK_ROLE_WIFI 1`, `CIELOTRACK_SENSOR_ONLY 1`).

The only values you must set:

- **`CIELOTRACK_SERVER_URL`** — your server. The build won't compile without it (a
  placeholder host on purpose, so an unconfigured board reaches nothing rather than the
  wrong fleet).
- **`CIELOTRACK_PROV_SALT`** — one secret shared across your fleet; each board's
  provisioning code is derived from it and its MAC. Changing it re-provisions every board.

**Leave `CIELOTRACK_WIFI_SSID` / `CIELOTRACK_WIFI_PASSWORD` undefined** — the network is set
from a phone (step 6), so it can't be mistyped and moving a board needs no rebuild. The
device key/id are optional too: a board asks the server for its identity and shows a claim
code.

## 4. Flash — one board at a time
The rails are tied, so **only one board may have a USB cable in at a time** (two hosts'
5 V rails must never be joined). Identify each board by USB serial (the `udevadm` loop in
`WIRING.md`), copy its role image over `secrets.h` with `--no-preserve=timestamps` and a
`touch` (see `WIRING.md` § Choosing which image — `cp -p` will silently relink the old
image), then:

```bash
idf.py build flash
```

Verify the binary is the role you meant: a master carries `advertisements seen` and is
~1360 KB; a sensor is ~770 KB (`WIRING.md` § Choosing which image).

## 5. Print a provisioning label
For each board, generate the QR label the phone app scans — its service name and
proof-of-possession, derived exactly as the firmware does from the salt and the board MAC:

```bash
pip install reportlab   # once; the tool's only dependency
python3 tools/provisioning_label.py --salt "<your CIELOTRACK_PROV_SALT>" <board Wi-Fi MAC>
```

The MAC is the board's Wi-Fi STA MAC — the same USB serial you read in step 4. Prints a
62 × 40 mm label per board (`--width` / `--height` to change) on any small thermal label
printer or on Avery sheets. It lives inside the enclosure; it is a setup credential, so keep
the salt out of anywhere public.

## 6. Provision Wi-Fi (master only)
The sensor never joins a network. On the master:

1. Power it — with no stored credentials it advertises **`CIELO_XXXXXX`** (its last three
   MAC bytes).
2. Open Espressif's **ESP BLE Provisioning** app.
3. **Scan the label's QR** (or choose `CIELO_XXXXXX` and enter the PoP by hand).
4. Pick your Wi-Fi from the scanned list and enter the password. **2.4 GHz only** — a
   5 GHz-only network won't appear.

Credentials land in NVS and the board connects on its own from then on. To move a board to
a different network later, `provisioning_forget()` reboots it back into this flow.

## 7. Register and claim
On first connection the master asks the server for its identity and logs a **claim code**.
Enter it, signed in, at `/receivers` on the dashboard to bind the receiver to your account.

## 8. Verify the pair
The peer link is the one check a merely-booting board can't fake:

- the master's `problems` list drops `peer_link_down` within a minute or two;
- both rows report the **same firmware version** (the master reports the sensor's version as
  its own — reflash both together, or neither).

Then keep the antennas ≥ 0.5 m apart and close the enclosure. See `WIRING.md` § Verifying,
§ Antennas, and § Reading a board's console (these boards only stream once a host asserts
DTR — `cat /dev/ttyACM0` reads like a dead board otherwise).

## One board
A single board needs none of the link or role configuration: leave `CIELOTRACK_SENSOR_ONLY`
and the peer defines undefined and it behaves as its own receiver with its own uplink.
Steps 3 (server URL + salt), 4 (flash), and 5–7 (label, provision, claim) apply unchanged;
skip the wiring (step 2) and the pair verification (step 8).
