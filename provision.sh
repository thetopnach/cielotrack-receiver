#!/usr/bin/env bash
#
# Provisions the host state the receiver assumes but cannot create for itself:
# monitor mode on the Wi-Fi adapter, protection from NetworkManager reclaiming it,
# re-application when the adapter is replugged, and a Bluetooth stack that is not
# fighting us for the controller.
#
# The install instructions used to stop at "copy the service and start it", which
# produced a receiver whose Wi-Fi capture could never work: radio_tracker only retunes
# the channel, it does not create monitor mode. This is the missing half.
#
# Idempotent — safe to run again after changing hardware. Everything it writes is
# derived from the adapter you name, so no file needs hand-editing.
#
#   sudo ./provision.sh wlan1
#
set -euo pipefail

IFACE="${1:-}"
INSTALL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "$IFACE" ]]; then
    echo "usage: sudo $0 <wifi-interface>"
    echo
    echo "Candidate interfaces on this host:"
    for path in /sys/class/net/*; do
        name="$(basename "$path")"
        [[ "$name" == "lo" || "$name" == eth* ]] && continue
        [[ -d "$path/wireless" || -d "$path/phy80211" ]] || continue
        driver="$(basename "$(readlink -f "$path/device/driver" 2>/dev/null)" 2>/dev/null || echo unknown)"
        echo "  $name  (driver: $driver)"
    done
    exit 2
fi

if [[ $EUID -ne 0 ]]; then
    echo "This writes to /etc, so it needs root: sudo $0 $IFACE" >&2
    exit 1
fi

if [[ ! -d "/sys/class/net/$IFACE" ]]; then
    echo "No interface called $IFACE. Run without arguments to list candidates." >&2
    exit 1
fi

MAC="$(cat "/sys/class/net/$IFACE/address")"
DEVPATH="$(readlink -f "/sys/class/net/$IFACE/device" || true)"
VENDOR=""; PRODUCT=""
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
echo

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
systemctl enable --now "cielotrack-monitor@$IFACE.service"

echo
echo "Done. Verify:"
echo "  iw dev $IFACE info | grep -E 'type|channel'     # expect: monitor, channel 6"
echo "  systemctl status cielotrack-receiver"
echo "  cat $INSTALL_DIR/status.json                    # radios.problems should be []"
echo "  sudo $INSTALL_DIR/update.sh --check             # what a nightly run would do"
echo "  systemctl list-timers cielotrack-update         # when it next runs"
