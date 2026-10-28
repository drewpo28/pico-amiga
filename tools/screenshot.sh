#!/usr/bin/env bash
# Convert the framebuffer dump of screenshot.gdb (or screenshot_profi.gdb with
# --profi) to a 720x576 PNG, as pico-speccy's VS Code extension expects:
# /tmp/picospec_screen.png (or /tmp/picospec_profi_screen.png).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${1:-}" == "--profi" ]]; then
    FB=/tmp/picospec_profi_fb.bin; DIM=/tmp/picospec_profi_dim.txt; OUT=/tmp/picospec_profi_screen.png
else
    FB=/tmp/picospec_fb.bin; DIM=/tmp/picospec_dim.txt; OUT=/tmp/picospec_screen.png
fi
if [[ ! -f "$FB" || ! -f "$DIM" ]]; then
    echo "Missing dump files. Run the screenshot GDB script in an active GDB session first."
    exit 1
fi
# GDB via Cortex-Debug may wrap the log in MI noise: take the first "<num> <num>".
DIMLINE=$(grep -oE '[0-9]+ [0-9]+' "$DIM" | head -1)
read -r ROWSIZE ROWS <<< "$DIMLINE"
python3 "$SCRIPT_DIR/fb2png.py" "$FB" "$ROWSIZE" "$ROWS" "$OUT"
echo "$OUT"
