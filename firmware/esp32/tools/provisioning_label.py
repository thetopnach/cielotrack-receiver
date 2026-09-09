#!/usr/bin/env python3
"""Generate per-board WiFi-provisioning labels for CieloTrack ESP receivers.

Each board advertises as CIELO_<last-3-MAC-bytes> and gates provisioning with a
proof-of-possession derived, exactly as the firmware does, from SHA-256(fleet-salt || MAC).
This prints a label per board carrying that name, the PoP, and a QR code the Espressif
"ESP BLE Provisioning" app can scan to fill both in — so setting up a board is scan-the-
sticker, pick-your-WiFi, with nothing typed.

The MAC is a board's Wi-Fi STA MAC (its USB serial number; see firmware/esp32/WIRING.md).
The salt must match CIELOTRACK_PROV_SALT in the flashed secrets.h, or the QR won't match
the board.

    python3 provisioning_label.py --salt "our-fleet-salt" A4:CB:8F:01:23:45 A4:CB:8F:01:23:46
    python3 provisioning_label.py --salt "our-fleet-salt" --width 62 --height 40 --out labels.pdf A4CB8F012345
"""
import argparse
import hashlib
import json
import re
import sys

from reportlab.lib.units import mm
from reportlab.pdfgen import canvas
from reportlab.graphics.barcode import qr
from reportlab.graphics.shapes import Drawing
from reportlab.graphics import renderPDF


def parse_mac(text):
    hexs = re.sub(r"[^0-9a-fA-F]", "", text)
    if len(hexs) != 12:
        raise ValueError(f"'{text}' is not a 6-byte MAC")
    return bytes.fromhex(hexs)


def service_name(mac):
    # Matches provisioning.c: "CIELO_%02X%02X%02X", mac[3], mac[4], mac[5].
    return "CIELO_%02X%02X%02X" % (mac[3], mac[4], mac[5])


def proof_of_possession(salt, mac):
    # Matches provisioning.c: first 6 bytes of SHA-256(salt-bytes || mac), as lowercase hex.
    return hashlib.sha256(salt.encode() + mac).digest()[:6].hex()


def qr_payload(name, pop):
    # The format the ESP BLE Provisioning app scans. Compact separators keep the QR small.
    return json.dumps({"ver": "v1", "name": name, "pop": pop, "transport": "ble"},
                      separators=(",", ":"))


def draw_label(c, w, h, name, pop, payload):
    c.setPageSize((w, h))
    margin = 3 * mm
    # QR on the left, sized to the label height less margins.
    qr_side = min(h - 2 * margin, w * 0.38)
    widget = qr.QrCodeWidget(payload, barLevel="M")
    b = widget.getBounds()
    widget_w, widget_h = b[2] - b[0], b[3] - b[1]
    d = Drawing(qr_side, qr_side,
                transform=[qr_side / widget_w, 0, 0, qr_side / widget_h, -b[0], -b[1]])
    d.add(widget)
    renderPDF.draw(d, c, margin, (h - qr_side) / 2)

    # Text on the right.
    tx = margin + qr_side + 3 * mm
    c.setFont("Helvetica-Bold", 8)
    c.drawString(tx, h - margin - 7, "CieloTrack setup")
    c.setFont("Helvetica", 6)
    c.drawString(tx, h - margin - 16, "ESP BLE Provisioning app")
    c.setFont("Helvetica-Bold", 8)
    c.drawString(tx, h - margin - 30, "Device")
    c.setFont("Courier", 8)
    c.drawString(tx, h - margin - 40, name)
    c.setFont("Helvetica-Bold", 8)
    c.drawString(tx, h - margin - 53, "PoP")
    c.setFont("Courier", 8)
    c.drawString(tx, h - margin - 63, pop)
    c.showPage()


def main():
    ap = argparse.ArgumentParser(description="CieloTrack provisioning labels")
    ap.add_argument("macs", nargs="+", help="board Wi-Fi STA MAC(s), any separator")
    ap.add_argument("--salt", required=True, help="fleet salt (CIELOTRACK_PROV_SALT)")
    ap.add_argument("--out", default="provisioning-labels.pdf")
    ap.add_argument("--width", type=float, default=62.0, help="label width mm (default 62)")
    ap.add_argument("--height", type=float, default=40.0, help="label height mm (default 40)")
    args = ap.parse_args()

    c = canvas.Canvas(args.out)
    for text in args.macs:
        mac = parse_mac(text)
        name, pop = service_name(mac), proof_of_possession(args.salt, mac)
        draw_label(c, args.width * mm, args.height * mm, name, pop, qr_payload(name, pop))
        print(f"{text}  ->  {name}  pop={pop}", file=sys.stderr)
    c.save()
    print(f"wrote {args.out} ({len(args.macs)} label(s))", file=sys.stderr)


if __name__ == "__main__":
    main()
