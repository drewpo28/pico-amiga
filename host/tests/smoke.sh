#!/bin/sh
# Smoke tests for the emulation core.  Needs the host build and the test ROMs:
#   cmake -S host -B build-host && cmake --build build-host
#   host/tests/fetch_test_roms.sh roms
#   host/tests/smoke.sh [roms-dir] [out-dir]
set -e
ROMS=${1:-roms}
OUT=${2:-build-host/smoke}
BIN=build-host/pico-amiga-host
mkdir -p "$OUT"
fail=0
check() {  # name, pattern, log
    if grep -q "$2" "$3"; then echo "PASS $1"; else echo "FAIL $1 (no '$2' in $3)"; fail=1; fi
}

# 1. DiagROM: CPU, chip RAM detection, CIAs, serial, copper/bitplanes.
$BIN --rom "$ROMS/diagrom-13.bin" --frames 1500 --shot 1500:"$OUT/diagrom.png" > "$OUT/diagrom.log" 2>&1
check "diagrom chipmem" "CHIPMEM OK" "$OUT/diagrom.log"

# 2. DiagROM keyboard + Paula: '1' (audio menu), '2' (play module).
$BIN --rom "$ROMS/diagrom-13.bin" --frames 2300 --key 1500:0x01:1 --key 1505:0x01:0 \
     --key 1750:0x02:1 --key 1755:0x02:0 --wav "$OUT/diagrom.wav" --shot 2300:"$OUT/diag-module.png" \
     > "$OUT/diagrom-audio.log" 2>&1
python3 - "$OUT/diagrom.wav" <<'PY' && echo "PASS diagrom audio" || { echo "FAIL diagrom audio"; fail=1; }
import sys, struct
d = open(sys.argv[1], 'rb').read()[44:][-31300 * 4:]   # last second
v = struct.unpack('<%dh' % (len(d) // 2), d)
sys.exit(0 if max(v) > 4000 else 1)
PY

# 3. AROS boot from floppy: blitter, sprites, interlace, trackdisk/MFM, bootblock.
python3 host/tests/make_boot_adf.py "$OUT/boot.adf"
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --slow --adf "$OUT/boot.adf" \
     --frames 1200 --shot 1200:"$OUT/aros-boot.png" > "$OUT/aros.log" 2>&1
check "aros bootblock" "BOOTBLOCK OK" "$OUT/aros.log"

exit $fail
