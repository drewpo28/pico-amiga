#!/usr/bin/env bash
# Convert screenshot.gdb's framebuffer dump to a 720x576 PNG,
# /tmp/pico-amiga_screen.png (tools/pico-amiga-tools.json names the files).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FB=/tmp/pico-amiga_fb.bin
DIM=/tmp/pico-amiga_dim.txt
OUT=/tmp/pico-amiga_screen.png
if [[ ! -f "$FB" || ! -f "$DIM" ]]; then
    echo "Missing dump files. Run the screenshot GDB script in an active GDB session first."
    exit 1
fi
# GDB via Cortex-Debug may wrap the log in MI noise: take the first "<num> <num>".
DIMLINE=$(grep -oE '[0-9]+ [0-9]+' "$DIM" | head -1)
read -r ROWSIZE ROWS <<< "$DIMLINE"
python3 "$SCRIPT_DIR/fb2png.py" "$FB" "$ROWSIZE" "$ROWS" "$OUT"
echo "$OUT"
