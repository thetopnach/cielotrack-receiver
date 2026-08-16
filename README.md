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

## What you need

| | |
|---|---|
| **Raspberry Pi** | A 4 or 5. Anything that can run 64-bit Raspberry Pi OS is fine. |
| **Bluetooth adapter** | Must support **Bluetooth 5 extended advertising**. This is the part people get wrong — a Bluetooth 4 dongle will connect happily and simply never see most drones. |
| **Wi-Fi adapter** | Must support **monitor mode**, and must be a second adapter — monitor mode takes the interface off your network. |
| **USB 2.0 port or extension** | Not optional. See below. |

The Pi's built-in Bluetooth works, but a dongle on an extension cable placed away from
the case usually hears considerably more.

## Install

```bash
sudo apt update && sudo apt install -y python3-pip bluez aircrack-ng git
sudo git clone https://github.com/thetopnach/cielotrack-receiver.git /opt/cielotrack-receiver
cd /opt/cielotrack-receiver
sudo pip3 install -r requirements.txt --break-system-packages
sudo cp .env.example .env
```

Edit `.env` and set `BASE_LAT` / `BASE_LON` to where the receiver sits, and
`WIFI_INTERFACE` to your monitor-capable adapter.

Then provision the host state the receiver needs but cannot create for itself —
monitor mode, protection from NetworkManager reclaiming the adapter, re-application
when it is replugged, and masking `bluetoothd` so it stops competing for the Bluetooth
controller:

```bash
sudo ./provision.sh            # lists your candidate adapters
sudo ./provision.sh wlan1      # provisions that one
```

This also pins the release signing key and enables the nightly updater described under
[Updates](#updates).

Everything it writes is derived from the adapter you name, so no file needs
hand-editing. It is safe to run again after changing hardware.

Skipping this step leaves Wi-Fi capture unable to work at all: the receiver retunes the
channel, it does not create monitor mode. Then:

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

The receiver writes `status.json` beside itself every minute. It needs neither the
server nor a claimed device, which is the point — those are usually what you are trying
to diagnose.

```bash
cat /opt/cielotrack-receiver/status.json
```

`radios.problems` is empty when both radios are configured as intended, and names any
fault the receiver can see in itself — including `detections_not_queued`, which means
aircraft are being heard but cannot be written to the upload queue.

`pipeline.contacts_recorded` counts detections this process has recorded, and
`pipeline.enqueue_failures` counts those it could not queue. Both are cumulative since
start, so a rising failure count next to a static recorded count is a receiver that is
hearing everything and delivering nothing.

`pipeline.frames_seen` counts frames that reached a decoder, but note that today it is
only incremented once a Remote ID message has already been extracted, so it stays at
zero over an empty sky and cannot by itself tell you a radio has died. Use
`radios.problems` and the heartbeat for that.

## Updates

The receiver updates itself overnight, and you should know exactly what that means
before you leave it running.

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

```bash
sudo ./update.sh --check              # what tonight's run would do, changing nothing
systemctl list-timers cielotrack-update
journalctl -u cielotrack-update       # what it did, and any rollback
```

To turn it off:

```bash
sudo touch /etc/cielotrack/no-auto-update
```

A checkout with local modifications is never touched — if you are mid-debug, the
updater leaves you alone and says so.

Worth being plain about the trade: this runs as root and installs code fetched from
the internet. Signing and pinning mean a compromised mirror cannot feed you a release,
but they do not protect against the signing key itself being stolen. If that is not a
trade you want on your hardware, disable it and update by hand — the project works
exactly the same either way.

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

If it says legacy instead, the adapter or driver doesn't support what's needed.

**Don't rotate Wi-Fi channels.** It's tempting to scan the band. In practice every
Remote ID beacon we have decoded arrived on channel 6, and a rotating scan spends most
of its time listening to empty channels while missing beacons on the one that matters.
Parking on 6 measurably outperformed rotating.

**Antenna placement beats everything.** Moving one receiver to the other side of the
house changed what it could hear more than any software change we made. If you are
seeing less than you expect, move the antenna before you debug the code.

## Sending data from your own hardware

You don't have to run this code. Anything that can make HTTPS requests can report to
CieloTrack — register, claim, then POST detections. The full field reference is on the
**Receivers** page once you sign in, and only `detected_at` is required.

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
