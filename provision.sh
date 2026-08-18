#!/usr/bin/env bash
#
# Provisions the host state the receiver assumes but cannot create for itself: the
# unprivileged user it runs as, the directory its data lives in, monitor mode on the
# Wi-Fi adapter, protection from NetworkManager reclaiming it, re-application when the
# adapter is replugged, and a Bluetooth stack that is not fighting us for the
# controller.
#
# The install instructions used to stop at "copy the service and start it", which
# produced a receiver whose Wi-Fi capture could never work: radio_tracker only retunes
# the channel, it does not create monitor mode. This is the missing half.
#
# Idempotent — safe to run again after changing hardware. Everything it writes is
# derived from the adapter you name, so no file needs hand-editing.
#
#   sudo ./provision.sh              # user, state, Bluetooth, updates — no Wi-Fi capture
#   sudo ./provision.sh wlan1        # all of that, plus monitor mode on wlan1
#
# The adapter is optional because a receiver without one is a real configuration, not a
# half-finished install: manufacturers choose their transport under F3411, and a
# BLE-only box still hears every drone that broadcasts over Bluetooth. Requiring the
# argument meant a receiver waiting on a Wi-Fi adapter could not be provisioned at all,
# so it also had no service user, no pinned signing key and no updates.
set -euo pipefail

IFACE="${1:-}"
INSTALL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CIELOTRACK_USER="${CIELOTRACK_USER:-cielotrack}"
STATE_DIR="${CIELOTRACK_STATE_DIR:-/var/lib/cielotrack}"
SERVICE=cielotrack-receiver

candidate_adapters() {
    for path in /sys/class/net/*; do
        name="$(basename "$path")"
        [[ "$name" == "lo" || "$name" == eth* ]] && continue
        [[ -d "$path/wireless" || -d "$path/phy80211" ]] || continue
        driver="$(basename "$(readlink -f "$path/device/driver" 2>/dev/null)" 2>/dev/null || echo unknown)"
        echo "  $name  (driver: $driver)"
    done
}

if [[ $EUID -ne 0 ]]; then
    echo "This writes to /etc, so it needs root: sudo $0 $IFACE" >&2
    exit 1
fi

if [[ -n "$IFACE" && ! -d "/sys/class/net/$IFACE" ]]; then
    echo "No interface called $IFACE. Candidates on this host:" >&2
    candidate_adapters >&2
    exit 1
fi

MAC=""; VENDOR=""; PRODUCT=""
if [[ -n "$IFACE" ]]; then
    MAC="$(cat "/sys/class/net/$IFACE/address")"
    DEVPATH="$(readlink -f "/sys/class/net/$IFACE/device" || true)"
    for _ in 1 2 3 4 5; do
        [[ -z "$DEVPATH" || "$DEVPATH" == "/" ]] && break
        if [[ -r "$DEVPATH/idVendor" && -r "$DEVPATH/idProduct" ]]; then
            VENDOR="$(cat "$DEVPATH/idVendor")"; PRODUCT="$(cat "$DEVPATH/idProduct")"
            break
        fi
        DEVPATH="$(dirname "$DEVPATH")"
    done
    echo "Provisioning $IFACE"
    echo "  MAC:      $MAC"
    echo "  USB ids:  ${VENDOR:-unknown}:${PRODUCT:-unknown}"
else
    echo "Provisioning host state only — no Wi-Fi adapter named"
fi
echo

# 0. The user the receiver runs as. Everything below that writes files needs it to
#    exist first, and the service unit refers to it by name.
if id -u "$CIELOTRACK_USER" >/dev/null 2>&1; then
    echo "  user $CIELOTRACK_USER already exists"
else
    useradd --system --no-create-home --home-dir "$STATE_DIR" \
            --shell /usr/sbin/nologin "$CIELOTRACK_USER"
    echo "  created the $CIELOTRACK_USER system user (no login, no home)"
fi

# systemd creates this from StateDirectory= too, but doing it here means the migration
# below has somewhere to put things before the service has ever run.
install -d -o "$CIELOTRACK_USER" -g "$CIELOTRACK_USER" -m 0750 "$STATE_DIR"
echo "  state directory: $STATE_DIR"

# 0b. An install that predates the service user keeps its queue and its identity beside
#     the code, where the new user cannot write. Moving it is not optional, and doing it
#     silently is not acceptable either — this is the receiver's identity.
if [[ -x "$INSTALL_DIR/migrate_state.py" ]] \
   && [[ -f "$INSTALL_DIR/device_credentials.json" || -f "$INSTALL_DIR/outbox.db" ]] \
   && [[ ! -f "$STATE_DIR/device_credentials.json" ]]; then
    echo
    echo "  This install keeps its data beside the code. Moving it into $STATE_DIR:"
    WAS_RUNNING=0
    if systemctl is-active --quiet "$SERVICE" 2>/dev/null; then
        WAS_RUNNING=1
        systemctl stop "$SERVICE"
        echo "    stopped $SERVICE for the move"
    fi
    if "$INSTALL_DIR/migrate_state.py" --from "$INSTALL_DIR" --state-dir "$STATE_DIR" \
                                       --user "$CIELOTRACK_USER" --apply 2>&1 | sed 's/^/    /'; then
        :
    else
        echo "  ✗ the move did not finish. Nothing was deleted; the originals are still" >&2
        echo "    in $INSTALL_DIR. Fix the reason above and run this again." >&2
        exit 1
    fi
    [[ "$WAS_RUNNING" -eq 1 ]] && systemctl start "$SERVICE" || true
fi
echo

# Sections 1-3 configure a capture adapter, so they are skipped entirely when there is
# not one yet. What they leave behind — the template unit, the udev rule — is derived
# from the adapter, so writing it now with nothing to point at would only produce a
# rule matching no device.
if [[ -n "$IFACE" ]]; then

# 1. Keep NetworkManager off the capture adapter. Without this it periodically tries to
#    manage the interface, which knocks it out of monitor mode at unpredictable times.
if [[ -d /etc/NetworkManager ]]; then
    mkdir -p /etc/NetworkManager/conf.d
    cat > /etc/NetworkManager/conf.d/99-cielotrack-unmanaged.conf <<EOF
# The capture adapter is ours, not NetworkManager's. Matched by MAC so renaming the
# interface cannot silently hand it back.
[keyfile]
unmanaged-devices=mac:$MAC
EOF
    echo "  wrote /etc/NetworkManager/conf.d/99-cielotrack-unmanaged.conf"
    systemctl reload NetworkManager 2>/dev/null || true
fi

# 2. Establish monitor mode at boot. A template unit so a second adapter is one
#    `systemctl enable` away rather than a second copy of this file.
cat > /etc/systemd/system/cielotrack-monitor@.service <<'EOF'
[Unit]
Description=Put %i into monitor mode for Wi-Fi Remote ID capture
# Must be up before the receiver opens a raw socket on it.
Before=cielotrack-receiver.service
After=network-online.target NetworkManager.service
Wants=network-online.target

[Service]
Type=oneshot
# Deliberately NOT RemainAfterExit. With it, systemd treats this as permanently
# satisfied after boot, so an adapter that is unplugged and comes back as a plain
# managed interface never has monitor mode re-applied — and Wi-Fi capture stays dead
# with nothing reporting why. Letting it go inactive lets the udev rule start it again.
RemainAfterExit=no
ExecStart=/usr/bin/nmcli device set %i managed no
ExecStart=/usr/sbin/ip link set %i down
ExecStart=/usr/sbin/iw dev %i set type monitor
ExecStart=/usr/sbin/ip link set %i up
# Channel 6 matches the receiver's default so the adapter is already correct in the
# window before capture starts; radio_tracker owns retuning from there.
ExecStart=/usr/sbin/iw dev %i set channel 6

[Install]
WantedBy=multi-user.target
EOF
echo "  wrote /etc/systemd/system/cielotrack-monitor@.service"

# 3. Re-apply it whenever the adapter appears. Unplugging it, moving it to another
#    port, or a powered hub blinking all bring it back as an ordinary managed
#    interface, and nothing else notices.
if [[ -n "$VENDOR" && -n "$PRODUCT" ]]; then
    cat > /etc/udev/rules.d/99-cielotrack-monitor.rules <<EOF
# SYSTEMD_WANTS rather than RUN+=systemctl: starting a unit with systemctl from inside
# a udev rule can deadlock, because udev waits on the command while systemd waits on
# udev to finish processing the device.
ACTION=="add", SUBSYSTEM=="net", ATTRS{idVendor}=="$VENDOR", ATTRS{idProduct}=="$PRODUCT", \\
  TAG+="systemd", ENV{SYSTEMD_WANTS}+="cielotrack-monitor@\$name.service"
EOF
    echo "  wrote /etc/udev/rules.d/99-cielotrack-monitor.rules"
    udevadm control --reload-rules
else
    echo "  skipped the udev rule: could not read USB ids for $IFACE"
    echo "    (monitor mode will still be set at boot, just not after a replug)"
fi

fi   # end of the adapter-only sections

# 4. bluetoothd competes for the controller and will re-enable scanning underneath us,
#    which shows up as extended scanning being refused for no visible reason.
if systemctl list-unit-files bluetooth.service >/dev/null 2>&1; then
    if [[ "$(systemctl is-enabled bluetooth 2>/dev/null)" != "masked" ]]; then
        systemctl stop bluetooth 2>/dev/null || true
        systemctl mask bluetooth
        echo "  masked bluetooth.service (it competes for the HCI controller)"
        echo "    to undo: sudo systemctl unmask bluetooth"
    else
        echo "  bluetooth.service already masked"
    fi
fi

# 5. Nightly updates. Pinning the signing key here, at the moment a person has chosen
#    to install this, is what makes the automatic part defensible: from now on the
#    updater trusts this copy and not whatever key the repository happens to contain.
mkdir -p /etc/cielotrack
if [[ -f "$INSTALL_DIR/allowed_signers" ]]; then
    if [[ -f /etc/cielotrack/allowed_signers ]] \
       && ! cmp -s "$INSTALL_DIR/allowed_signers" /etc/cielotrack/allowed_signers; then
        echo "  NOTE: the release signing key in this checkout differs from the pinned one."
        echo "        Leaving the pinned key alone — a key that can replace itself is not a pin."
        echo "        If this is a genuine key rotation, replace it deliberately:"
        echo "          sudo cp $INSTALL_DIR/allowed_signers /etc/cielotrack/allowed_signers"
    else
        cp "$INSTALL_DIR/allowed_signers" /etc/cielotrack/allowed_signers
        echo "  pinned the release signing key to /etc/cielotrack/allowed_signers"
    fi
fi

# Written rather than left absent so the channel is discoverable — an operator looking
# for "which releases does this box take" finds a file, not a default buried in a script.
# Absent means stable anyway, so this changes nothing for anyone who deletes it.
if [[ ! -f /etc/cielotrack/channel ]]; then
    echo stable > /etc/cielotrack/channel
    echo "  release channel: stable (/etc/cielotrack/channel)"
    echo "    to take prereleases first: echo canary | sudo tee /etc/cielotrack/channel"
else
    echo "  release channel: $(cat /etc/cielotrack/channel) (unchanged)"
fi

if [[ -f "$INSTALL_DIR/cielotrack-update.service" ]]; then
    sed "s#^ExecStart=.*#ExecStart=$INSTALL_DIR/update.sh#" \
        "$INSTALL_DIR/cielotrack-update.service" > /etc/systemd/system/cielotrack-update.service
    cp "$INSTALL_DIR/cielotrack-update.timer" /etc/systemd/system/cielotrack-update.timer
    chmod +x "$INSTALL_DIR/update.sh" 2>/dev/null || true
    systemctl daemon-reload
    systemctl enable --now cielotrack-update.timer
    echo "  nightly updates enabled (02:00-04:00 local, signed releases only)"
    echo "    to disable: sudo touch /etc/cielotrack/no-auto-update"
fi

systemctl daemon-reload
if [[ -n "$IFACE" ]]; then
    systemctl enable --now "cielotrack-monitor@$IFACE.service"
fi

echo
echo "Done. Verify:"
if [[ -n "$IFACE" ]]; then
    echo "  iw dev $IFACE info | grep -E 'type|channel'     # expect: monitor, channel 6"
fi
echo "  systemctl status $SERVICE"
echo "  cat $STATE_DIR/status.json                      # radios.problems should be []"
echo "  sudo $INSTALL_DIR/update.sh --check             # what a nightly run would do"
echo "  systemctl list-timers cielotrack-update         # when it next runs"

if [[ -z "$IFACE" ]]; then
    echo
    echo "No Wi-Fi capture on this receiver. Set WIFI_INTERFACE= (empty) in your .env,"
    echo "or radio_tracker spends the day retrying an adapter that is not there."
    echo
    echo "Candidate adapters, for when you have one:"
    candidates="$(candidate_adapters)"
    if [[ -n "$candidates" ]]; then
        echo "$candidates"
        echo "  then: sudo $0 <name>"
    else
        echo "  (none attached)"
    fi
fi
