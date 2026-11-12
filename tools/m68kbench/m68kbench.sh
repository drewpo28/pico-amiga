#!/bin/bash
# tools/m68kbench - what the emulated CPU costs on the board, counted: the
# emulation core (src/amiga + Musashi) compiled with the firmware's flags, on a
# Cortex-M33 under QEMU (mps2-an505), running an A1200 from a snapshot of the
# host bench.  A TCG plugin counts the ARM instructions each function executes;
# the run ends with the 68k instruction count and CRCs of chip RAM and the
# framebuffer, which a change to the CPU core must not move.
#
# What it is NOT: a cycle count.  No XIP / PSRAM misses, no flash wait, no core 1.
# Compare two cores with it, then take the firmware to the board.
# (pico-speccy's tools/z80bench is the model.)
#
# Needs: the firmware's arm-none-eabi-gcc on PATH, qemu-system-arm, gcc + glib-2.0
# headers (the plugin), curl (once, for qemu-plugin.h).
#
#   m68kbench.sh plugin
#   m68kbench.sh build [-DNAME=VALUE ...]         -> $WORK/bench.elf
#   m68kbench.sh snap  ROM ADF FRAME OUT.state [HOST ARGS]   the host bench's snapshot after FRAME
#   m68kbench.sh run   ROM ADF STATE FRAMES [WARM] [FETCH8,COST8] [FAST_MB] [EXT]   totals only
#   m68kbench.sh prof  ROM ADF STATE FRAMES [WARM] [FETCH8,COST8] [FAST_MB] [EXT]   by function
#
# Fast RAM: the snapshot taken with `--fast N` (HOST ARGS), FAST_MB the same N.
# EXT: a CD32's extended ROM - the machine is a CD32 (the snapshot from the host
# bench's `--cd32 --ext ... --cd ...`; the drive is empty here, so a part that
# does not read the disc).
#
# ADF / STATE "-": none (a cold boot of the ROM).  The chip bus model defaults
# to the menu's "With cache" (2,16); 0 = off (the CPU runs free).
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${M68KBENCH_REPO:-$(cd "$HERE/../.." && pwd)}"     # another tree's core: M68KBENCH_REPO
WORK="${M68KBENCH_WORK:-/tmp/m68kbench}"
mkdir -p "$WORK"
QEMU="qemu-system-arm -M mps2-an505 -nographic"
CC="${M68KBENCH_CC:-arm-none-eabi-gcc}"
ARCH="-mcpu=cortex-m33 -mthumb -march=armv8-m.main+fp+dsp -mfloat-abi=softfp"

plugin() {
    [ -f "$WORK/qemu-plugin.h" ] || curl -sSL -o "$WORK/qemu-plugin.h" \
        "https://raw.githubusercontent.com/qemu/qemu/v$(qemu-system-arm --version | head -1 | grep -oP '\d+\.\d+\.\d+' | head -1)/include/qemu/qemu-plugin.h"
    gcc -shared -fPIC -O2 -I"$WORK" $(pkg-config --cflags glib-2.0) -o "$WORK/libprof.so" "$HERE/prof.c"
}

build() {
    # The firmware's flags for the core (CMakeLists.txt + cmake/amiga_core.cmake).
    local DEF='-DAMIGA_FETCH_WINDOW=1 -DAMIGA_OVL=1 -DNDEBUG'
    local INC="-I$REPO/src/amiga -I$REPO/external/musashi -I$REPO/external/musashi/gen"
    local OBJ=()
    mkdir -p "$WORK/obj"
    for f in $(sed -n 's/^ *\${ROOT}\/\(.*\.c\)$/\1/p' "$REPO/cmake/amiga_core.cmake" | sort -u); do
        local o="$WORK/obj/$(echo "$f" | tr / _).o" w="-Wall -Wextra -Wno-unused-parameter"
        case "$f" in external/*) w="-w" ;; esac
        $CC $ARCH -g -O3 -std=gnu11 -O2 $w $DEF '-DAMIGA_HOT=__attribute__((section(".time_critical.amiga")))' \
            "$@" $INC -c "$REPO/$f" -o "$o" &
        OBJ+=("$o")
    done
    wait
    $CC $ARCH -g -O2 -std=gnu11 -Wall $INC -c "$HERE/harness.c" -o "$WORK/obj/harness.o"
    $CC $ARCH --specs=rdimon.specs -nostartfiles -T "$HERE/link.ld" "$WORK/obj/harness.o" "${OBJ[@]}" \
        -o "$WORK/bench.elf" -Wl,--start-group -lc -lrdimon -lgcc -lm -Wl,--end-group 2>&1 | grep -v "warning\|RWX" || true
    [ -f "$WORK/bench.elf" ] && arm-none-eabi-size "$WORK/bench.elf"
}

snap() {
    local rom="$1" adf="$2" n="$3" out="$4"; shift 4
    "$REPO/build-host/pico-amiga-host" --a1200 --aga --rom "$rom" --adf "$adf" --frames "$n" --state-save "$n:$out" "$@"
}

args() { local IFS='|' a; a="bench|$*"; echo "${a//,/,,}"; }   # QEMU's option syntax: a comma doubled

run() {
    local mark; mark=0x$(arm-none-eabi-nm "$WORK/bench.elf" | awk '$3=="bench_mark"{print $1}')
    $QEMU -semihosting-config enable=on,target=native,arg="$(args "$@")" \
        -kernel "$WORK/bench.elf" -plugin "$WORK/libprof.so",mark=$mark 2>&1
}

prof() {
    local out; out=$(PROF_OUT="$WORK/prof.txt" run "$@")
    echo "$out" | grep -v "^INSNS"
    python3 "$HERE/symprof.py" "$WORK/bench.elf" "$WORK/prof.txt" "$(echo "$out" | awk '$1=="M68K"{print $2}')" "${TOP:-40}"
}

cmd="$1"; shift || true
case "$cmd" in
    plugin) plugin ;;
    build)  build "$@" ;;
    snap)   snap "$@" ;;
    run)    run "$@" ;;
    prof)   prof "$@" ;;
    *) sed -n 2,24p "$0"; exit 1 ;;
esac
