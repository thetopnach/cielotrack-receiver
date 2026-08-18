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
| **Wi-Fi adapter** | Optional, and worth having. Must support **monitor mode**, and must be a second adapter — monitor mode takes the interface off your network. Without one the receiver is BLE-only, which still hears every drone that broadcasts over Bluetooth; manufacturers choose their transport, so this is a real receiver, not half of one. |
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
cat /var/lib/cielotrack/status.json
```

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

If it says legacy instead, the adapter or driver doesn't support what's needed.

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
