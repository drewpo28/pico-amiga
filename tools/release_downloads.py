#!/usr/bin/env python3
"""Print the "Downloads" table for a GitHub release body (after pico-speccy's).

Usage: release_downloads.py REPO TAG FILE.uf2 [FILE.uf2 ...]

One row per board, the UF2 linked to its release asset. Every image drives
HDMI or VGA, whichever is connected at boot; the column says how its HDMI is
made (the HSTX serializer or PIO). A board tag missing from the table below
still gets a row, named after the raw tag.
"""
import os
import re
import sys

# firmware tag -> (board, HDMI back-end); row order = this order
BOARDS = [
    ("m1p2", "Murmulator 1.x", "PIO"),
    ("m2p2", "Murmulator 2.0", "HSTX"),
    ("PCp2", "Olimex RP2040-PICO-PC + Pico 2", "HSTX"),
    ("DVp2", "Pimoroni Pico DV Demo Base + Pico Plus 2", "PIO"),
    ("z0p2", "Waveshare RP2350-PiZero", "PIO"),
]
MODULES = {"": "", "w": " (Pico 2 W)", "ws": " (RP2350B-Plus-W)"}
NAME_RE = re.compile(r"^(?P<tag>[A-Za-z0-9]+?)(?P<mod>ws|w)?-amiga-(?P<ver>\d+\.\d+\.\d+)\.uf2$")


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    repo, tag, files = sys.argv[1], sys.argv[2], [os.path.basename(f) for f in sys.argv[3:]]
    known = {t: (name, hdmi) for t, name, hdmi in BOARDS}
    order = {t: i for i, (t, _, _) in enumerate(BOARDS)}
    rows = []
    for f in files:
        m = NAME_RE.match(f)
        if not m:
            continue
        t, mod = m.group("tag"), m.group("mod") or ""
        name, hdmi = known.get(t, (t, "?"))
        rows.append((order.get(t, len(order)), mod, name + MODULES.get(mod, ""), hdmi, f))
    rows.sort()
    print("## Downloads\n")
    print("| Board | HDMI / VGA | Firmware |")
    print("|---|---|---|")
    for _, _, name, hdmi, f in rows:
        print(f"| {name} | {hdmi} | [{f}](https://github.com/{repo}/releases/download/{tag}/{f}) |")
    print("\nEvery image also drives VGA (detected at boot). Copy the UF2 to the board in")
    print("BOOTSEL mode; the SD card layout and keys are in the README.\n")


if __name__ == "__main__":
    main()
