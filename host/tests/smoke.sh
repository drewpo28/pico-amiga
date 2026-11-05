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
if build-host/tmds_test > "$OUT/tmds.log"; then echo "PASS tmds tables"; else echo "FAIL tmds tables"; cat "$OUT/tmds.log"; fail=1; fi
check() {  # name, pattern, log
    if grep -aq "$2" "$3"; then echo "PASS $1"; else echo "FAIL $1 (no '$2' in $3)"; fail=1; fi
}

# 1. DiagROM: CPU, chip RAM detection, CIAs, serial, copper/bitplanes.
$BIN --rom "$ROMS/diagrom-13.bin" --frames 1500 --status 1500 --shot 1500:"$OUT/diagrom.png" > "$OUT/diagrom.log" 2>&1
check "diagrom chipmem" "CHIPMEM OK" "$OUT/diagrom.log"
check "diagrom hires rows at full resolution" "idx8=2[0-9][0-9]" "$OUT/diagrom.log"

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

# 3a. The same on a 68EC020 (Musashi's second handler set, amiga_set_cpu).
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --slow --cpu020 --adf "$OUT/boot.adf" \
     --frames 1200 > "$OUT/aros020.log" 2>&1
check "aros bootblock on a 68EC020" "BOOTBLOCK OK" "$OUT/aros020.log"
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --a1200 --adf "$OUT/boot.adf" \
     --frames 1200 > "$OUT/aros1200.log" 2>&1
check "aros bootblock on the A1200" "BOOTBLOCK OK" "$OUT/aros1200.log"

# 3b. Snapshots: saved mid-boot (disk DMA running), loaded, the same picture 500
#     frames on as without the round trip.
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --slow --adf "$OUT/boot.adf" \
     --frames 900 --state-save 400:"$OUT/state.bin" --shot 900:"$OUT/state-a.png" > /dev/null 2>&1
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --slow --adf "$OUT/boot.adf" \
     --frames 500 --state-load "$OUT/state.bin" --shot 500:"$OUT/state-b.png" > /dev/null 2>&1
if cmp -s "$OUT/state-a.png" "$OUT/state-b.png"; then echo "PASS snapshot round trip"; else echo "FAIL snapshot round trip"; fail=1; fi

# 4. AROS boot screen: 640x512 interlace, both fields kept (LOF toggles).
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --slow --frames 1000 --status 1000 \
     --shot 1000:"$OUT/aros-screen.png" > "$OUT/aros-screen.log" 2>&1
check "aros interlace rows" "lace4=2[0-9][0-9]" "$OUT/aros-screen.log"

# 5. A500+ (ECS): the chip IDs, and a super-hires screen from the upper 512 KB of
#    1 MB chip RAM (make_ecs_adf.py): $770 from output column 40 to 679, black around.
python3 host/tests/make_ecs_adf.py "$OUT/ecs.adf"
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --a500plus --adf "$OUT/ecs.adf" \
     --frames 1200 --shot 1200:"$OUT/ecs.png" > "$OUT/ecs.log" 2>&1
check "ecs chip ids" "ECS OK" "$OUT/ecs.log"
python3 - "$OUT/ecs.png" <<'PY' && echo "PASS ecs super-hires" || { echo "FAIL ecs super-hires"; fail=1; }
import sys, struct, zlib
d = open(sys.argv[1], 'rb').read()
w, h = struct.unpack('>II', d[16:24])
raw, p = b'', 8
while p < len(d):
    n, t = struct.unpack('>I4s', d[p:p + 8])
    if t == b'IDAT': raw += d[p + 8:p + 8 + n]
    p += 12 + n
raw = zlib.decompress(raw)
row = raw[300 * (1 + 3 * w) + 1:][:3 * w]              # output line 300, filter 0
px = [tuple(row[3 * x:3 * x + 3]) for x in (39, 40, 360, 679, 680)]
sys.exit(0 if px == [(0, 0, 0), (0x77, 0x77, 0), (0x77, 0x77, 0), (0x77, 0x77, 0), (0, 0, 0)] else 1)
PY

# 5b. AGA (make_aga_adf.py): Alice / Lisa IDs, 8 planes with FMODE 3 (64-bit fetch),
#     Lisa's palette banks, LOCT and BPLAM: $0F0 from output column 40 to 679.
python3 host/tests/make_aga_adf.py "$OUT/aga.adf"
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --a1200 --aga --adf "$OUT/aga.adf" \
     --frames 1200 --shot 1200:"$OUT/aga.png" > "$OUT/aga.log" 2>&1
check "aga chip ids" "AGA OK" "$OUT/aga.log"
python3 - "$OUT/aga.png" <<'PY' && echo "PASS aga 8 planes, fmode 3, palette" || { echo "FAIL aga 8 planes, fmode 3, palette"; fail=1; }
import sys, struct, zlib
d = open(sys.argv[1], 'rb').read()
w, h = struct.unpack('>II', d[16:24])
raw, p = b'', 8
while p < len(d):
    n, t = struct.unpack('>I4s', d[p:p + 8])
    if t == b'IDAT': raw += d[p + 8:p + 8 + n]
    p += 12 + n
raw = zlib.decompress(raw)
row = raw[300 * (1 + 3 * w) + 1:][:3 * w]              # output line 300, filter 0
px = [tuple(row[3 * x:3 * x + 3]) for x in (39, 40, 360, 679, 680)]
sys.exit(0 if px == [(0, 0, 0), (0, 0xFF, 0), (0, 0xFF, 0), (0, 0xFF, 0), (0, 0, 0)] else 1)
PY

# 6. A600 (Gayle, make_gayle_adf.py): its ID $D0, the empty IDE port, no CIA
#    mirror at $A00000; the A500+ has none of them.  And a snapshot round trip
#    on the A600 (the GAYL block, MACH's Gayle).
python3 host/tests/make_gayle_adf.py "$OUT/gayle.adf"
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --a600 --adf "$OUT/gayle.adf" \
     --frames 1200 > "$OUT/gayle.log" 2>&1
check "a600 gayle" "GAYLE OK" "$OUT/gayle.log"
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --a500plus --adf "$OUT/gayle.adf" \
     --frames 1200 > "$OUT/gayle-plus.log" 2>&1
check "a500+ has no gayle" "GAYLE NO" "$OUT/gayle-plus.log"
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --a600 --adf "$OUT/boot.adf" \
     --frames 900 --state-save 400:"$OUT/state600.bin" --shot 900:"$OUT/state600-a.png" > /dev/null 2>&1
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --a600 --adf "$OUT/boot.adf" \
     --frames 500 --state-load "$OUT/state600.bin" --shot 500:"$OUT/state600-b.png" > /dev/null 2>&1
if cmp -s "$OUT/state600-a.png" "$OUT/state600-b.png"; then echo "PASS a600 snapshot round trip"; else echo "FAIL a600 snapshot round trip"; fail=1; fi

# 7. A600 IDE (ide.c) with a bare-partition HDF (make_hdf.py: the drive's virtual
#    RDB in front of it): make_ide_adf.py's bootblock runs IDENTIFY, reads by LBA
#    and CHS, two sectors in one command, writes a sector and reads it back, and
#    checks the interrupt through Gayle; --write-back puts the write in the file.
python3 host/tests/make_hdf.py "$OUT/ide.hdf" --blocks 8032   # + the virtual RDB's 32: 8 x 16 x 63
python3 host/tests/make_ide_adf.py "$OUT/ide.adf"
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --a600 --adf "$OUT/ide.adf" \
     --hdf "$OUT/ide.hdf" --write-back --frames 1200 > "$OUT/ide.log" 2>&1
check "a600 ide" "IDE OK" "$OUT/ide.log"
python3 - "$OUT/ide.hdf" <<'PY' && echo "PASS a600 ide write-back" || { echo "FAIL a600 ide write-back"; fail=1; }
import sys
d = open(sys.argv[1], 'rb').read()
sys.exit(0 if d[68 * 512:69 * 512] == b'\x12\x34' * 256 else 1)
PY

exit $fail
