# CieloTrack Receiver

[![tests](https://github.com/thetopnach/cielotrack-receiver/actions/workflows/tests.yml/badge.svg)](https://github.com/thetopnach/cielotrack-receiver/actions/workflows/tests.yml)

Listens for drone Remote ID broadcasts over Bluetooth LE and Wi-Fi, decodes them, and
reports what it hears to [cielotrack.com](https://cielotrack.com).

Aircraft sold in the US since September 2023 are required to broadcast Remote ID — a
serial number, position, altitude, speed, and the operator's location — in the clear,
on frequencies any ordinary radio can receive. This listens for that. It transmits
nothing and needs no permission to run.

Your data stays yours: detections are visible only to your account until you choose to
share them.

## Two ways to build one

The same firmware and decoder drive two hardware builds — pick whichever suits you:

- **Raspberry Pi** — a Pi plus USB radios. Does the most and is the easiest to set up; it
  is what the rest of this README covers.
- **ESP32** — two Seeed XIAO ESP32-S3 boards on a small carrier board: cheaper, lower
  power, no OS. Build it from **[`firmware/esp32/BUILD.md`](firmware/esp32/BUILD.md)**
  (wiring in [`firmware/esp32/WIRING.md`](firmware/esp32/WIRING.md)).

Either way it is **two radios**, because one radio cannot sit in BLE extended-scan *and*
Wi-Fi monitor at the same time: the Pi uses two USB adapters (a Bluetooth dongle plus a
monitor-mode Wi-Fi adapter), the ESP build puts each radio on its own board, linked over
one wire. The Wi-Fi half is optional in both — BLE alone still hears every drone that
broadcasts over Bluetooth.

## What you need (Raspberry Pi)

| | |
|---|---|
| **Raspberry Pi** | A 4 or 5. Anything that can run 64-bit Raspberry Pi OS is fine. |
| **Bluetooth adapter** | Must support **Bluetooth 5 extended advertising**. This is the part people get wrong — a Bluetooth 4 dongle will connect happily and simply never see most drones. |
| **Wi-Fi adapter** | Optional, and worth having. Must support **monitor mode**, and must be a second adapter — monitor mode takes the interface off your network. Without one the receiver is BLE-only, which still hears every drone that broadcasts over Bluetooth; manufacturers choose their transport, so this is a real receiver, not half of one. |
| **USB 2.0 port or extension** | Not optional. See below. |

The Pi's built-in Bluetooth works, but a dongle on an extension cable placed away from
the case usually hears considerably more.

**If you keep the onboard radio enabled, the receiver prefers the USB dongle over it.**
Both take an `hci` index, and which one gets `hci0` is decided at enumeration — so
choosing "the first adapter" means a dongle unplugged for a moment can come back with
the receiver listening to the onboard radio instead. That failure is silent: extended
scanning reports active, no fault is raised, and the receiver simply stops hearing
much. `provision.sh` tells you which adapter it will use. To remove the ambiguity for
good, either pin `BLE_INTERFACE=hci0` in `.env`, or disable the onboard radio with
`dtoverlay=disable-bt` in `/boot/firmware/config.txt` and reboot.

## Install

```bash
sudo apt update && sudo apt install -y python3-pip bluez bluez-hcidump iw git
sudo git clone https://github.com/thetopnach/cielotrack-receiver.git /opt/cielotrack-receiver
cd /opt/cielotrack-receiver
sudo pip3 install -r requirements.txt --break-system-packages
sudo cp .env.example .env
```

`bluez-hcidump` is its own package on Debian 13 and later, and it is the one that
matters: without it every radio configures correctly, extended scanning starts, and
nothing is ever decoded. `provision.sh` checks for all of them before it changes
anything, so you will be told rather than left to find out from a quiet sky.

Edit `.env` and set `BASE_LAT` / `BASE_LON` to where the receiver sits, and
`WIFI_INTERFACE` to your monitor-capable adapter — or leave it empty on a BLE-only
receiver.

Then provision the host state the receiver needs but cannot create for itself — the
user it runs as, the directory its data lives in, monitor mode, protection from
NetworkManager reclaiming the adapter, re-application when it is replugged, and masking
`bluetoothd` so it stops competing for the Bluetooth controller:

```bash
sudo ./provision.sh            # everything except Wi-Fi capture
sudo ./provision.sh wlan1      # all of that, plus monitor mode on wlan1
```

The adapter is optional. Run it without one on a BLE-only receiver, or on one still
waiting for an adapter to arrive — and set `WIFI_INTERFACE=` (empty) in `.env`, or the
receiver spends the day retrying a device that is not there.

This also pins the release signing key and enables the nightly updater described under
[Updates](#updates).

Everything it writes is derived from the adapter you name, so no file needs
hand-editing. It is safe to run again after changing hardware.

Skipping this step leaves Wi-Fi capture unable to work at all: the receiver retunes the
channel, it does not create monitor mode — and, since the service runs as `cielotrack`
rather than root, leaves it with no user to run as. Then:

```bash
sudo cp cielotrack-receiver.service /etc/systemd/system/
sudo systemctl enable --now cielotrack-receiver
journalctl -u cielotrack-receiver -f
```

## Register it

On first start the receiver generates its own identity and prints a six-digit claim
code:

```
🔑 Central server: unclaimed. Claim code: 481-207 — enter this at https://cielotrack.com/receivers
```

Sign in at [cielotrack.com](https://cielotrack.com) — it emails you a one-time link,
there's no password — then enter that code on the **Receivers** page. The receiver
picks up its API key within a minute and starts reporting.

The key is handed over exactly once and then erased server-side, so it never sits in a
database waiting to be read. If a receiver loses its key, claim it again.

## Checking it works

The receiver writes `status.json` into its state directory every minute. It needs
neither the server nor a claimed device, which is the point — those are usually what you
are trying to diagnose.

```bash
sudo cat /var/lib/cielotrack/status.json
```

`sudo` because the state directory is `0750` and owned by the `cielotrack` user. It
holds the upload queue and this receiver's detection history alongside the health file,
and those are not for every account on the machine.

`radios.problems` is empty when both radios are configured as intended, and names any
fault the receiver can see in itself — including `detections_not_queued`, which means
aircraft are being heard but cannot be written to the upload queue.

`pipeline.contacts_recorded` counts detections this process has recorded, and
`pipeline.enqueue_failures` counts those it could not queue. Both are cumulative since
start, so a rising failure count next to a static recorded count is a receiver that is
hearing everything and delivering nothing.

`pipeline.frames_seen` counts every frame that reached a decoder, and
`pipeline.messages_decoded` counts the few that carried Remote ID. Together they
separate the two silences a heartbeat cannot tell apart, because Remote ID is rare and
ordinary 2.4 GHz traffic is not:

| frames_seen | messages_decoded | what it means |
|---|---|---|
| climbing | climbing | working |
| climbing | flat | radios fine, nothing flying — or a decoder fault |
| **flat at zero** | zero | **that radio has stopped** |

On a populated channel a live adapter counts hundreds of frames a second whether or not
anything is in the air, so a protocol missing from `frames_seen` entirely is a radio
that is not delivering — not a quiet sky. Check it against `radios.problems`, which
reports what the adapter says about itself; this reports what it is actually doing.

## Bluetooth / BLE Long Range setup & troubleshooting

The receiver tries to start **Bluetooth 5 extended scanning**, which is the only way to
hear drones broadcasting Remote ID over Bluetooth 5 Long Range (the Coded PHY). When the
controller rejects that, it falls back to **legacy** scanning and raises
`ble_legacy_scanning` in `radios.problems`. The log line to look for is:

```
📡 BLE: extended scanning active (1M + Coded PHY / Long Range)
```

A legacy line instead does **not** always mean a wrong or broken dongle. The recommended
Realtek adapter (RTL8761B family, which reports HCI/LMP version 5.1 — so it supports Long
Range by spec) can still fall back on a stock Raspberry Pi OS install for reasons that
have nothing to do with the chipset: the Bluetooth radio is blocked by `rfkill`, or the
Realtek firmware and BlueZ are too old to negotiate extended scanning. Both are fixable
in software, and neither shows up as an obvious error — the first operator to hit this
saw an `Operation not possible due to RF-Kill` message scroll past and a silent fall back
to legacy. Work through the steps below in order.

**1. Confirm the dongle is the adapter in use, not the Pi's onboard radio.** A stock Pi
enables its own Bluetooth radio, which takes an `hci` index of its own and usually can't
do extended advertising. The receiver prefers a USB adapter and says which one it chose
at startup:

```bash
journalctl -u cielotrack-receiver | grep "BLE:"
# 📋 BLE: 2 adapters present; using hci1 (USB) rather than hci0. Set BLE_INTERFACE to override.
hciconfig -a                       # lists every adapter; note which hciN is the USB Bus
```

If the wrong adapter is being used — or you want to remove the ambiguity for good — pin
it in `.env` and restart:

```bash
echo 'BLE_INTERFACE=hci1' | sudo tee -a /opt/cielotrack-receiver/.env
sudo systemctl restart cielotrack-receiver
```

**2. Check the dongle is Bluetooth 5 capable.** Replace `hciX` with your dongle's index:

```bash
sudo hciconfig hciX version
#   HCI Version: 5.1 (0xa)   LMP Version: 5.1 (0xa)
```

`LMP Version` 5.0 or higher means the controller supports Long Range by spec. A 4.x dongle
genuinely cannot do this and there is nothing to fix — buy a Bluetooth 5 adapter. A 5.x
dongle that still falls back is what the rest of these steps are for.

**3. Clear the rfkill block and make it stick.** This is the single most common cause, and
it is specific to how this receiver runs: `provision.sh` masks `bluetoothd` (it fights
`hcitool` over the controller), and `bluetoothd` is normally what clears the Bluetooth
`rfkill` soft-block at boot. With it masked, nothing does — so an adapter that is soft-blocked
stays blocked and every extended-scan command fails with `RF-Kill`.

```bash
rfkill list                        # look for Bluetooth → "Soft blocked: yes"
sudo rfkill unblock bluetooth
rfkill list                        # should now read "Soft blocked: no"
```

To make it survive a reboot, mask the service that re-applies it rather than fighting it
each boot:

```bash
sudo systemctl disable --now systemd-rfkill.service systemd-rfkill.socket
sudo reboot
```

**4. Update the Realtek firmware and Bluetooth stack.** Extended scanning negotiates only
with a current `linux-firmware` (the RTL8761B blobs) and a recent BlueZ. A fresh Pi OS
image is often old enough to miss both:

```bash
sudo apt update && sudo apt full-upgrade
sudo reboot
```

After the reboot, confirm the Realtek firmware actually loaded:

```bash
dmesg | grep -i rtl
# Bluetooth: hci1: RTL: examining hci_ver=0a hci_rev=... lmp_ver=0a ...
# Bluetooth: hci1: RTL: loading rtl_bt/rtl8761bu_fw.bin
# Bluetooth: hci1: RTL: fw version ...
```

A `failed to load` line here (rather than `fw version`) means the firmware package is still
missing — `sudo apt install linux-firmware` and reboot again.

**5. Verify you got extended scanning.** Two independent checks. The receiver's own view,
in `status.json`:

```bash
sudo cat /var/lib/cielotrack/status.json | grep -A2 '"ble"'
#   "ble": { "mode": "extended", "stream_alive": true }
```

`radios.ble.mode` is `"extended"` when it worked and `"legacy"` when it fell back; in
legacy, `radios.problems` also carries `ble_legacy_scanning`. And the controller's own
advertised capabilities:

```bash
sudo btmgmt info
# look for "le" in supported settings and a current controller version; a 5.x
# controller with firmware loaded is what makes extended scanning negotiable
```

**What legacy still gets you.** Legacy scanning is not a dead receiver. It hears every
drone that broadcasts standard Bluetooth 4 / 1M-PHY Remote ID, which is most of the
common ones — in this fleet, Amazon's MK30 is heard over Bluetooth and shows up fine on a
legacy-only box. What extended scanning adds is **Bluetooth 5 Long Range (Coded PHY)**
coverage: aircraft that transmit only on the Coded PHY are inaudible to a legacy scanner.
So a receiver that falls back to legacy during setup is already useful and worth leaving
running while you work through the steps above — you are extending its range, not
switching it on.

## Updates

The receiver updates itself overnight, and you should know exactly what that means
before you leave it running. If you are the one publishing releases rather than
receiving them, see [RELEASING.md](RELEASING.md).

Once a night, at a random time between 02:00 and 04:00 **local** to the receiver, it
checks for a new release. The window is when the sky is empty — Remote ID traffic here
drops to nothing between roughly 23:00 and 06:00 — so an update that goes wrong costs
the least data it can. The time is randomised so that a bad release cannot take every
receiver down in the same instant; the early updaters roll themselves back before the
later ones start.

It will only move to a **signed release tag**, never to whatever is on `main`, and it
verifies that signature against a key pinned to `/etc/cielotrack/allowed_signers` when
you installed — not against the key in the repository it is about to install, which
would prove nothing.

Afterwards it checks the receiver still works, using the same faults the fleet page
shows. If detections stop reaching the queue, or the status file cannot be written, it
puts the previous release **and the previous service unit** back and restarts. The unit
matters: it lives in `/etc`, so a plain `git pull` never updates it, and fixes to the
sandboxing or capabilities would otherwise silently never reach anyone already running.

A release that failed here is written to `/etc/cielotrack/rejected` and not tried again,
so a receiver settles on the last version that worked rather than reinstalling a broken
one every night. A *newer* release is still installed normally — a fix is never blocked
by the release it repairs. To give a rejected version another chance, delete its line.

```bash
sudo ./update.sh --check              # what tonight's run would do, changing nothing
systemctl list-timers cielotrack-update
journalctl -u cielotrack-update       # what it did, and any rollback
```

To turn it off:

```bash
sudo touch /etc/cielotrack/no-auto-update
```

### Channels

`/etc/cielotrack/channel` decides which releases a receiver accepts. It contains
`stable` unless you change it, and an absent file means stable too — the safe channel
has to be the one you end up on by doing nothing.

| | |
|---|---|
| `stable` | Final releases only. This is what you want. |
| `canary` | Also takes release candidates, so this receiver runs a release before the fleet does. |

```bash
echo canary | sudo tee /etc/cielotrack/channel
```

Running one receiver on `canary` is worth doing if you have two, and the reason is
narrow: the automatic health check can only catch faults the receiver can see in
itself. A release that installs cleanly, reports every radio healthy, and simply
*hears less* passes that check. Somebody has to look at the numbers, and a canary is
what gives them a day to do it.

A checkout with local modifications is never touched — if you are mid-debug, the
updater leaves you alone and says so.

Worth being plain about the trade: the *updater* runs as root and installs code fetched
from the internet — the receiver itself does not, and has not since v1.4.0. Signing and pinning mean a compromised mirror cannot feed you a release,
but they do not protect against the signing key itself being stolen. If that is not a
trade you want on your hardware, disable it and update by hand — the project works
exactly the same either way.

### If you installed before v1.4.0

The receiver used to run as root. It now runs as a `cielotrack` system user holding two
capabilities — `CAP_NET_RAW` and `CAP_NET_ADMIN` — which is all raw HCI and monitor mode
actually need. Its data moved with it, from the install directory to
`/var/lib/cielotrack`, because root owns the checkout and an ordinary user cannot create
files beside it.

One command does both, and it is the same one you already ran:

```bash
cd /opt/cielotrack-receiver
sudo ./provision.sh              # add your adapter name if you have one
```

It creates the user, then stops the receiver, moves `device_credentials.json`,
`outbox.db` and `amazon_drone_matches.csv` across, checks each copy against the original
before setting the original aside, and starts it again. Nothing is deleted: the
originals end up in `/var/lib/cielotrack/pre-migration/`, and you can remove them once
the receiver has run for a day.

Until you do, the nightly updater will decline v1.4.0 and say why in
`journalctl -u cielotrack-update`. That is deliberate — it is not recorded as a failed
release, and your receiver keeps running the version it has.

The identity file is the reason for the care. Losing it is silent: an absent one looks
exactly like a receiver nobody has claimed yet, so the receiver would register itself as
a new device and the one on your dashboard would simply stop reporting. Both this
migration and the receiver itself refuse to run past that state rather than guess.

### If you installed before v1.1.0

The release signing key changed in v1.1.0. Releases up to v1.0.1 were signed with a key
that also authenticated to GitHub, which meant one stolen key could both publish a
release and sign it; releases are now signed by a key that does nothing else.

Your receiver pinned the old key when you installed, and will correctly refuse releases
signed with the new one — you will see `signature on … did not verify` in
`journalctl -u cielotrack-update`. Re-pin deliberately:

```bash
cd /opt/cielotrack-receiver
git fetch --tags
git show v1.1.0:allowed_signers | sudo tee /etc/cielotrack/allowed_signers
sudo ./update.sh
```

A fresh install needs none of this.

## Tests

```bash
python3 tests/test_state_paths.py
python3 tests/test_receiver_state.py
sudo -v && bash tests/test_update.sh
```

Both also run on every push, in [`.github/workflows/tests.yml`](.github/workflows/tests.yml).

The second builds a throwaway origin, a throwaway checkout and a stub service unit, and
generates its own signing key rather than borrowing the real one — the key that can
install code on every receiver has no business in a test that runs unattended. It walks
the whole path: a good release applied, a bad one rolled back, an unsigned tag refused,
opt-out honoured, and a checkout with local edits left alone. It needs `sudo` because
the thing it is testing restarts a service, and it cleans up after itself.

No framework needed. They cover the contact state machine — where the receiver decides
whether a drone that was heard becomes a drone that was recorded.

## Things that cost us time

Written down because none of them are obvious, and each one looked like "this project
doesn't work" until we found it.

**USB 3.0 ports jam 2.4 GHz reception.** A SuperSpeed port radiates broadband noise
across the band Bluetooth and Wi-Fi both use. Plugging the Wi-Fi adapter into a USB 3
port raised our noise floor to −74 dBm and we caught *nothing* for 46 hours, while
drones flew overhead at −90 dBm. Moving it to USB 2.0 restored reception immediately.
Use USB 2.0 ports, or a USB 2.0 extension cable, for both radios.

**Bluetooth 4 dongles miss most drones.** Remote ID uses extended advertising, which
Bluetooth 4 cannot see. The tracker logs which mode it got:

```
📡 BLE: extended scanning active (1M + Coded PHY / Long Range)
```

If it says legacy instead, a Bluetooth 4 dongle genuinely can't do this — but a Bluetooth
5 dongle that still falls back usually can, and is blocked by `rfkill` or stale Realtek
firmware rather than by hardware. See [Bluetooth / BLE Long Range setup &
troubleshooting](#bluetooth--ble-long-range-setup--troubleshooting).

**Don't rotate Wi-Fi channels — but know why.** A rotating scan is away from any given
channel most of the time, so it misses beacons on the one that matters. Parking on 6 is
the default here for that reason.

The evidence behind 6 is narrower than "every beacon we decoded was on 6", which would
be circular — you mostly find beacons where you are listening. What was actually
measured: two Wing Hummingbirds produced 65 decodes across two airframes, all on channel
6 with SSID `WING_AC_RID`, and nothing on 44 or 149. Sweeping 1, 6 and 11 near Dallas on
2026-08-16 found no Remote ID vendor IE on any of them across ~29,000 frames.

So 6 is a tested default for one manufacturer and an untested one for the rest — nothing
contradicts it, and little confirms it beyond Wing. The honest summary is that Wi-Fi
Remote ID is rare here rather than that it lives on channel 6: this receiver has logged
ten Wi-Fi detections in its lifetime against a hundred and fifty-seven on Bluetooth.
Somewhere with more Wi-Fi traffic, test rather than inherit — `sudo iw dev wlan1 set
channel 1` and watch — before assuming 6 is right for the aircraft overhead.

**Antenna placement beats everything.** Moving one receiver to the other side of the
house changed what it could hear more than any software change we made. If you are
seeing less than you expect, move the antenna before you debug the code.

## Sending data from your own hardware

You don't have to run this code. Anything that can make HTTPS requests can report to
CieloTrack — register, claim, then POST detections. The full field reference is at
[cielotrack.com/build](https://cielotrack.com/build), which needs no account, and only
`detected_at` is required.

```bash
curl -X POST https://cielotrack.com/v1/detections \
  -H 'Authorization: Bearer <your-api-key>' \
  -H 'Content-Type: application/json' \
  -d '{"detected_at":"2026-01-01T00:00:00Z","uas_id":"...","lat":32.9,"lon":-96.7}'
```

Batch up to 500 at `/v1/detections/batch`. Invalid rows come back with their index and
the reason; valid rows in the same batch are still stored.

## Legal

Receiving Remote ID is receiving a public broadcast, which is what it was designed to
be. This transmits nothing and does not interfere with any aircraft. Putting a Wi-Fi
adapter into monitor mode to capture beacon frames may be treated differently where you
live — that's worth checking for your own jurisdiction.

## Licence

MIT.
