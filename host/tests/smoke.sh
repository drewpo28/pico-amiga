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

# 3t. Trace mode (Musashi's traced loop, m68krun.h): SR's T1 traces every
#     instruction, a 68020's T0 the changes of flow only (make_trace_adf.py).
python3 host/tests/make_trace_adf.py "$OUT/trace.adf"
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --slow --adf "$OUT/trace.adf" \
     --frames 900 > "$OUT/trace.log" 2>&1
check "trace on the 68000" "TRACE PA" "$OUT/trace.log"
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --slow --cpu020 --adf "$OUT/trace.adf" \
     --frames 900 > "$OUT/trace020.log" 2>&1
check "trace on the 68EC020" "TRACE PG" "$OUT/trace020.log"

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

# 5c. AGA HAM8 in hires (make_aga_adf.py --ham8): $F0F inside, the $00F border.
python3 host/tests/make_aga_adf.py "$OUT/ham8.adf" --ham8
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --a1200 --aga --adf "$OUT/ham8.adf" \
     --frames 1200 --shot 1200:"$OUT/ham8.png" > "$OUT/ham8.log" 2>&1
python3 - "$OUT/ham8.png" <<'PY' && echo "PASS aga ham8 hires" || { echo "FAIL aga ham8 hires"; fail=1; }
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
px = [tuple(row[3 * x:3 * x + 3]) for x in (20, 100, 360, 600, 700)]
sys.exit(0 if px == [(0, 0, 0xFF), (0xFF, 0, 0xFF), (0xFF, 0, 0xFF), (0xFF, 0, 0xFF), (0, 0, 0xFF)] else 1)
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
# The slave: the same checks with the image there alone, then beside a master (an
# RDB image: a read from the wrong drive finds no "DOS\\1" at block 32).
python3 host/tests/make_hdf.py "$OUT/ide-s.hdf" --blocks 8032
python3 host/tests/make_hdf.py "$OUT/ide-m.hdf" --blocks 8064 --rdb
python3 host/tests/make_ide_adf.py --slave "$OUT/ide-s.adf"
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --a600 --adf "$OUT/ide-s.adf" \
     --hdf2 "$OUT/ide-s.hdf" --write-back --frames 1200 > "$OUT/ide-s1.log" 2>&1
check "a600 ide slave alone" "IDE OK" "$OUT/ide-s1.log"
$BIN --rom "$ROMS/aros-rom.bin" --ext "$ROMS/aros-ext.bin" --a600 --adf "$OUT/ide-s.adf" \
     --hdf "$OUT/ide-m.hdf" --hdf2 "$OUT/ide-s.hdf" --write-back --frames 1200 > "$OUT/ide-s2.log" 2>&1
check "a600 ide slave with a master" "IDE OK" "$OUT/ide-s2.log"
python3 - "$OUT/ide-s.hdf" "$OUT/ide-m.hdf" <<'PY' && echo "PASS a600 ide slave write-back" || { echo "FAIL a600 ide slave write-back"; fail=1; }
import sys
s = open(sys.argv[1], 'rb').read(); m = open(sys.argv[2], 'rb').read()
sys.exit(0 if s[68 * 512:69 * 512] == b'\x12\x34' * 256 and b'\x12\x34' * 256 not in m else 1)
PY

# 8. LHA archives as hard disks (whdboot.c, lha.c, ffs.c) and the Zorro II fast
#    RAM board: only with the owner's files (debug/: the archives, Kickstarts and
#    WHDLoad are not in git), skipped otherwise.  Every archive unpacks with its
#    CRCs right; HeroQuest boots through WHDLoad on an A600 (KS 3.1) to its title;
#    UFO CD32 (1.5 MB ExpMem) runs from the fast RAM on an A1200 with 4 MB.
D=debug
KS="$D/Kickstarts/PUAE Bios"
if ls "$D"/LHA/*.lha > /dev/null 2>&1; then
    for a in "$D"/LHA/*.lha; do
        if build-host/lhatest "$a" > "$OUT/lha.log" 2>&1; then echo "PASS lha $(basename "$a")"; else echo "FAIL lha $(basename "$a")"; fail=1; fi
    done
fi
if [ -f "$D/LHA/HeroQuest_v1.3a_0127.lha" ] && [ -f "$KS/kick40063.A600" ] && [ -d "$D/WHDLoad" ]; then
    $BIN --rom "$KS/kick40063.A600" --a600 --lha "$D/LHA/HeroQuest_v1.3a_0127.lha" --whd "$D/WHDLoad" \
         --lha-out "$OUT/hq.hdf" --frames 3000 --status 3000 --shot 3000:"$OUT/hq.png" > "$OUT/hq.log" 2>&1
    if python3 host/tests/ffs_check.py "$OUT/hq.hdf" > "$OUT/hq-ffs.log" 2>&1; then echo "PASS whdload volume"; else echo "FAIL whdload volume"; fail=1; fi
    check "whdload heroquest a600" "frame 3000 .*dmacon=07DF" "$OUT/hq.log"
fi
if [ -f "$D/LHA/UFOEnemyUnknown_v1.1_CD32.lha" ] && [ -f "$KS/kick40068.A1200" ] && [ -d "$D/WHDLoad" ]; then
    mkdir -p "$OUT/whd" "$OUT/whd/Devs/Kickstarts"
    cp -r "$D/WHDLoad/." "$OUT/whd/" && cp "$KS/kick40068.A1200" "$OUT/whd/Devs/Kickstarts/"
    $BIN --rom "$KS/kick40068.A1200" --a1200 --aga --fast 4 --lha "$D/LHA/UFOEnemyUnknown_v1.1_CD32.lha" \
         --whd "$OUT/whd" --frames 2000 --status 2000 > "$OUT/ufo.log" 2>&1
    check "whdload ufo a1200 fast ram" "frame 2000 .*pc=[2-9][0-9A-F]\{5\} " "$OUT/ufo.log"
fi

# 9. The CD32 (akiko.c, cdimage.c): only with the owner's files too (its Kickstart
#    and extended ROM, a disc).  Cannon Fodder's sheet (a MODE1/2048 and a
#    MODE2/2336 track, a file each) parses, the disc boots into the game's code,
#    and a snapshot taken on the way continues to the same picture.
CD="$D/CD32/Cannon Fodder (1993)(Virgin)[!][Amiga-CD32].cue"
if [ -f "$CD" ] && [ -f "$KS/kick40060.CD32" ] && [ -f "$KS/kick40060.CD32.ext" ]; then
    $BIN --rom "$KS/kick40060.CD32" --ext "$KS/kick40060.CD32.ext" --cd32 --cd "$CD" --frames 2500 --status 2500 \
         --state-save 1500:"$OUT/cd32.state" --shot 2500:"$OUT/cd32-a.png" > "$OUT/cd32.log" 2>&1
    check "cd32 cue sheet" "track 2 MODE2/2336, sectors 27459+12450" "$OUT/cd32.log"
    check "cd32 boots the disc" "frame 2500 .*pc=0[0-9A-F]\{5\} " "$OUT/cd32.log"
    $BIN --rom "$KS/kick40060.CD32" --ext "$KS/kick40060.CD32.ext" --cd32 --cd "$CD" --frames 1000 \
         --state-load "$OUT/cd32.state" --shot 1000:"$OUT/cd32-b.png" > /dev/null 2>&1
    if cmp -s "$OUT/cd32-a.png" "$OUT/cd32-b.png"; then echo "PASS cd32 snapshot round trip"; else echo "FAIL cd32 snapshot round trip"; fail=1; fi
    # CD audio: a disc of two AUDIO tracks holding a 440 Hz tone (left at full
    # level, right inverted at half); the red button starts it, and the bench's
    # WAV then has the tone at half level - 440 cycles a second, L + 2R = 0.
    mkdir -p "$OUT/cda"
    python3 - "$OUT/cda" <<'PY'
import sys, struct, math
o = sys.argv[1]
d = bytearray()
for i in range(44100 * 40):
    v = int(12000 * math.sin(2 * math.pi * 440 * i / 44100))
    d += struct.pack('<hh', v, -v // 2)
open(o + '/a.bin', 'wb').write(d[:len(d) // 2352 * 2352])
open(o + '/a.cue', 'w').write('FILE "a.bin" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:20:00\n')
PY
    $BIN --rom "$KS/kick40060.CD32" --ext "$KS/kick40060.CD32.ext" --cd32 --cd "$OUT/cda/a.cue" --frames 1700 \
         --pad 1500:32 --pad 1520:0 --wav "$OUT/cda/a.wav" > /dev/null 2>&1
    python3 - "$OUT/cda/a.wav" <<'PY' && echo "PASS cd32 cd audio" || { echo "FAIL cd32 cd audio"; fail=1; }
import sys, wave, struct
w = wave.open(sys.argv[1]); r = w.getframerate(); n = w.getnframes()
d = struct.unpack('<%dh' % (2 * n), w.readframes(n))
L = d[2 * 32 * r:2 * 33 * r:2]; R = d[2 * 32 * r + 1:2 * 33 * r:2]
zc = sum(1 for i in range(1, len(L)) if (L[i - 1] < 0) != (L[i] < 0)) // 2
ok = 438 <= zc <= 442 and 11900 <= max(L) - min(L) <= 12100 and max(abs(a + 2 * b) for a, b in zip(L, R)) <= 2
sys.exit(0 if ok else 1)
PY
fi

exit $fail
