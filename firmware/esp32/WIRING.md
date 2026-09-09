# Wiring the receiver pair

Two Seeed XIAO ESP32-S3 boards, one wire and a ground between them. This is the build
the `CIELOTRACK_ROLE_WIFI` / `CIELOTRACK_SENSOR_ONLY` firmware expects; a single board on
its own needs none of this and behaves exactly as it did before the pair existed.

It is written down because it was not. The parts list on `/parts` covers the Pi hardware
in detail — the Pi 5, the Bluetooth adapter, the Alfa AWUS036ACM, the USB extensions —
and says nothing about either ESP32 build, so answering "which pin is that wire on" meant
reading a C header.

## The two boards

| role | MAC (also its USB serial number) | radio | network |
|---|---|---|---|
| **master** | `A4:CB:8F:01:23:45` | BLE, extended scanning | Wi-Fi, uploads for both |
| **sensor** | `A4:CB:8F:01:23:46` | Wi-Fi, promiscuous on channel 6 | none at all |

The MACs above are illustrative — `A4:CB:8F` is Espressif's prefix, but every board's own
suffix differs, so read yours rather than copying these. They identify themselves by USB
serial, not by port number, and the numbering changes between plug-ins. Always check
before flashing:

```bash
for d in /dev/ttyACM*; do echo -n "$d "; udevadm info -q property -n $d | grep ID_SERIAL_SHORT; done
```

The master holds both identities. Contacts the sensor hears are forwarded under the
peer's device id, so each transport still reports as its own receiver — which is the
entire reason for running two boards rather than one.

## The wire

Three conductors: data, ground, and 5 V so the pair runs off one USB supply. The header
they come off reads

```
D7 · D8 · D9 · D10 · 3V3 · GND · UUSB
```

so D9 and D10 are adjacent to each other, and GND is two pins further along past 3V3 —
**not** three in a row, which is what `peer_link.h` used to claim and what the first
version of this document repeated. UUSB sits at the end, next to GND.

```
     SENSOR  (Wi-Fi, no network)              MASTER  (BLE, uploads for both)
     ──────────────────────────               ─────────────────────────────

       D7   ○                                   D7   ○
       D8   ○                                   D8   ○
       D9   ●──── data ─────────────┐           D9   ○
       D10  ○                       └────────▶  D10  ●
       3V3  ○      (do not link)                3V3  ○
       GND  ●──── ground ──────────────────────  GND  ●
       UUSB ●──── 5 V ─────────────────────────  UUSB ●
                                                       ▲
     ● used      ○ not used                            │
                                              one USB supply, here
     One direction only for data: the sensor talks, the master listens.
     Power flows the other way — whichever board has USB in it feeds the
     other over UUSB.

     Note the gap: D10 and 3V3 sit between D9 and GND, so the conductors
     are not taken from neighbouring pins.
```

### Power

`UUSB` is the 5 V USB rail. Linking the two lets one USB supply run the pair, and each
board still regulates its own 3.3 V from it.

**Do not link `3V3` instead.** That bypasses the second board's regulator and makes one
LDO carry two ESP32-S3s. Both radios peaking together is precisely when it would sag, and
a brownout on the sensor presents as an intermittent peer link — a fault that looks like
software and is not.

Two cautions that come with tying the rails:

- **Never plug both boards into USB at once.** With UUSB linked, that connects two hosts'
  5 V rails together and can back-feed a laptop port. Flash one board at a time, with the
  other's cable out.
- **The connector must be keyed.** A reversible plug can now put 5 V onto the data pin.
  This was already the reason to avoid bare 0.1 inch jumpers; it is a harder reason now.

Worth confirming with a multimeter before relying on it: continuity from the USB
connector's 5 V to the `UUSB` pin, on both boards. This document has already had one
claim about the header that was inherited rather than measured.

| setting | value | where |
|---|---|---|
| UART | 1 | `PEER_LINK_UART_NUM` |
| sensor TX | GPIO8 — XIAO **D9** | `PEER_LINK_TX_GPIO` |
| master RX | GPIO9 — XIAO **D10** | `PEER_LINK_RX_GPIO` |
| baud | 115200 | `PEER_LINK_BAUD` |
| max line | 512 bytes | `PEER_LINK_MAX_LINE` |
| sensor status | every 60 s | `PEER_STATUS_EVERY_SECONDS` |

The XIAO has two 7-pin headers, fourteen pins in total, eleven of them labelled D0–D10.
The link uses three per board — data, ground and 5 V — so a 3-pin connector is exactly
right; there is no reason to match the header's pin count.

All of it lives in `main/peer_link.h`. Change it there, not here, and update this table.

### Why not D6/D7

They are GPIO43/44 — the UART0 console, which is emitting the log stream. Wiring the link
to those would feed every log line into the peer's parser. D2 is out too: it is a
strapping pin sampled at boot. Of the eleven pins the XIAO exposes, D9 and D10 are free,
adjacent, and share the header with GND.

An earlier version of this asked for GPIO17 and GPIO18, which the XIAO does not break out
at all.

### What crosses it

Newline-delimited JSON with a CRC over the JSON only. A UART between two boards on a pole
is not a quiet environment, and a corrupted line that still parses would put a wrong
serial or a wrong position into the record — worse than losing the line outright.

Contacts carry no timestamp. The sensor has no clock and no way to set one; the master
stamps on arrival, within milliseconds of the decode, which is the more trustworthy of the
two numbers anyway.

## Choosing which image to build

Two files, one copied over `secrets.h`:

```bash
cp --no-preserve=timestamps main/secrets.ble.h  main/secrets.h && touch main/secrets.h   # master
cp --no-preserve=timestamps main/secrets.wifi.h main/secrets.h && touch main/secrets.h   # sensor
```

**Do not edit `secrets.h` in place.** Doing that once removed the peer configuration from
a master build, compiling out the forwarding path entirely — and the master went on
reporting itself perfectly healthy while everything the sensor heard went nowhere.

`--no-preserve=timestamps` and the `touch` are not decoration. `cp -p` keeps the source
mtime, ninja sees no change, and `idf.py build` relinks the image you were trying to
replace. That produced a *sensor* binary while the tree said master.

Check the binary rather than trusting the build:

```bash
strings build/cielotrack_receiver.bin | grep -q 'advertisements seen' && echo master || echo sensor
```

A master image is roughly 1,360 KB; a sensor image is roughly 770 KB, because the network
stack and BLE are compiled out of it.

The sensor variant sets `CIELOTRACK_ROLE_WIFI 1` and `CIELOTRACK_SENSOR_ONLY 1`, and sets
its device id and key to the **peer** pair — so a build with the role flags wrong reports
as the Wi-Fi receiver rather than impersonating the master.

Both variants are gitignored (`esp32/main/secrets.*.h`). They carry live credentials.

## Reading a board's console

`cat /dev/ttyACM0` returns nothing on these boards and reads exactly like a dead one. The
USB-Serial/JTAG console only streams once a host asserts DTR:

```python
s = serial.Serial("/dev/ttyACM0", 115200, timeout=1)
s.dtr = True; s.rts = False
```

What each role prints:

| role | line |
|---|---|
| master | `cielotrack: N advertisements seen, N carried Remote ID, ...` |
| sensor | `wifi_rid: N frames, N decoded, N dropped (sensor; link to master ...)` |
| master, at boot | `sensor link up on GPIO8/TX GPIO9/RX at 115200 baud` |

That last line names the pins the firmware actually opened, which beats reading this
document if the two ever disagree.

## Antennas

- Short pigtail, 10–15 cm. The 1.13 mm coax is lossy at 2.4 GHz.
- Keep the two antennas apart, ideally 0.5 m or more. The master transmits during each
  upload window while it is also the board doing the listening.
- Check for a 0-ohm jumper selecting trace-versus-IPEX; on a true WROOM-1U there is none.

## Verifying the pair after any of this

The peer link is the one test that cannot be faked by a board that merely boots:

- the master's `problems` list drops `peer_link_down` within a minute or two
- both rows report the same firmware version, because the master reports the sensor's
  version as its own build — which is a claim the master cannot really make, and the
  reason to reflash both together or neither

A sensor that is alive but silent is normal. Wi-Fi Remote ID is rare here: across two
weeks the sensor's entire contribution was one aircraft on one afternoon. Silence from it
is not evidence of a fault, and a watch waiting for its next detection may wait days.
