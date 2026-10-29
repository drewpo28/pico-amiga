#!/bin/bash
# Build the release firmware for every board (after pico-speccy's build_all.sh):
# one UF2 per board - HDMI or VGA is picked at boot, the HDMI back-end (HSTX or
# PIO) by the board. Each board builds in build-all/<BOARD>, logs go to
# build-all/logs, the UF2s to firmware/.
#
#   ./build_all.sh [--clean] [-j JOBS_PER_BUILD] [-p MAX_PARALLEL] [BOARDS...]
#
# Release images start at 252 MHz (CPU_MHZ, the clock of a fresh card): every
# board runs there; Options > Overclock > 504 MHz is what the emulation needs. The built-in ROMs come
# from ROMS_DIR (default debug/, cmake/roms.cmake); a missing one is left out
# and listed at the end.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_TYPE="${BUILD_TYPE:-Release}"
CPU_MHZ="${CPU_MHZ:-252}"
NPROC="$(nproc)"
MAX_PARALLEL="${MAX_PARALLEL:-3}"
JOBS_PER_BUILD="${JOBS_PER_BUILD:-$(( (NPROC + MAX_PARALLEL - 1) / MAX_PARALLEL ))}"
CLEAN=false

POSITIONAL=()
while [ $# -gt 0 ]; do
    case "$1" in
        --clean) CLEAN=true; shift ;;
        -j)      JOBS_PER_BUILD="$2"; shift 2 ;;
        -p)      MAX_PARALLEL="$2"; shift 2 ;;
        -h|--help)
            cat <<EOF
Usage: $0 [--clean] [-j JOBS_PER_BUILD] [-p MAX_PARALLEL] [BOARDS...]

Env vars: BUILD_TYPE (Release), CPU_MHZ (252), ROMS_DIR, MAX_PARALLEL, JOBS_PER_BUILD
Boards:   MURM MURM2 PICO_PC PICO_DV ZERO2 (default: all; the _W / _WS variants
          build too when named)
EOF
            exit 0 ;;
        *) POSITIONAL+=("$1"); shift ;;
    esac
done
set -- "${POSITIONAL[@]}"

# --- Pico SDK tools: plain shells (not the VS Code extension's) lack them ---
if ! command -v cmake >/dev/null 2>&1; then
    CMAKE_BIN=$(ls -d "$HOME/.pico-sdk/cmake"/*/bin 2>/dev/null | sort -V | tail -1)
    [ -n "$CMAKE_BIN" ] && export PATH="$CMAKE_BIN:$PATH"
fi
if [ -z "$PICO_TOOLCHAIN_PATH" ]; then
    TC_VER=$(grep -oP 'set\(toolchainVersion \K[^)]+' "$SCRIPT_DIR/CMakeLists.txt")
    TC_DIR="$HOME/.pico-sdk/toolchain/$TC_VER"
    if [ -d "$TC_DIR" ]; then
        export PICO_TOOLCHAIN_PATH="$TC_DIR"
        export PATH="$TC_DIR/bin:$PATH"
    fi
fi
CMAKE_MAKE_PROGRAM=$(ls -d "$HOME/.pico-sdk/ninja"/*/ninja 2>/dev/null | sort -V | tail -1)
[ -z "$CMAKE_MAKE_PROGRAM" ] && CMAKE_MAKE_PROGRAM=$(command -v ninja || true)
if [ -z "$CMAKE_MAKE_PROGRAM" ]; then echo "Error: ninja not found"; exit 1; fi

CCACHE_ARGS=""
if command -v ccache >/dev/null 2>&1; then
    CCACHE_ARGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache"
fi

ALL_BOARDS="MURM MURM2 PICO_PC PICO_DV ZERO2"
BOARDS="${*:-$ALL_BOARDS}"
VER=$(grep -oP 'set\(PORT_VERSION "\K[0-9.]+' "$SCRIPT_DIR/CMakeLists.txt")

OUTPUT_DIR="$SCRIPT_DIR/firmware"
BUILD_ROOT="$SCRIPT_DIR/build-all"
LOG_DIR="$BUILD_ROOT/logs"
mkdir -p "$OUTPUT_DIR" "$LOG_DIR"

echo "=== pico-amiga multi-board build, v$VER ==="
echo "Boards:     $BOARDS"
echo "Build type: $BUILD_TYPE, CPU $CPU_MHZ MHz"
echo "Parallel:   $MAX_PARALLEL boards x $JOBS_PER_BUILD jobs, ccache $([ -n "$CCACHE_ARGS" ] && echo on || echo off)"
echo ""

if $CLEAN; then
    for B in $BOARDS; do rm -rf "${BUILD_ROOT:?}/$B"; done
fi

build_one() {
    local board="$1" dir="$BUILD_ROOT/$1" log="$LOG_DIR/$1.log"
    # One build per dir at a time (a second build_all or a VS Code task).
    exec 9>"$dir.lock"
    command -v flock >/dev/null 2>&1 && flock -x 9 || true
    {
        echo "=== $board -> $dir"
        # Every option explicit: a cache keeps what an earlier configure set.
        # shellcheck disable=SC2086
        cmake -B "$dir" -S "$SCRIPT_DIR" -G Ninja -DCMAKE_MAKE_PROGRAM="$CMAKE_MAKE_PROGRAM" \
              -DBOARD="$board" -DCPU_MHZ="$CPU_MHZ" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
              ${ROMS_DIR:+-DROMS_DIR="$ROMS_DIR"} $CCACHE_ARGS \
          && cmake --build "$dir" --parallel "$JOBS_PER_BUILD"
    } >"$log" 2>&1
}
build_wrapper() {
    local start=$SECONDS
    if build_one "$1"; then echo "[OK]   $1 ($((SECONDS - start))s)"
    else echo "[FAIL] $1 ($((SECONDS - start))s)"; return 1; fi
}
export -f build_one build_wrapper
export SCRIPT_DIR BUILD_ROOT LOG_DIR BUILD_TYPE CPU_MHZ CMAKE_MAKE_PROGRAM CCACHE_ARGS JOBS_PER_BUILD ROMS_DIR

START=$SECONDS
RESULTS="$(mktemp)"
trap 'rm -f "$RESULTS"' EXIT
printf '%s\n' $BOARDS | xargs -I{} -P "$MAX_PARALLEL" bash -c 'build_wrapper "$@"' _ {} | tee "$RESULTS" || true

echo ""
echo "=== Summary ($((SECONDS - START))s) ==="
rm -f "$OUTPUT_DIR"/*.uf2
FAILED=0
for B in $BOARDS; do
    if grep -q "^\[OK\]   $B " "$RESULTS"; then
        FW=$(ls -t "$BUILD_ROOT/$B"/*-amiga-"$VER".uf2 2>/dev/null | head -1)
        if [ -n "$FW" ]; then cp "$FW" "$OUTPUT_DIR/"; echo "  OK    $B -> $(basename "$FW")"
        else echo "  !!    $B: no *-amiga-$VER.uf2 in $BUILD_ROOT/$B"; FAILED=1; fi
    else
        echo "  FAIL  $B (see $LOG_DIR/$B.log)"; FAILED=1
    fi
done

# A ROM file missing from ROMS_DIR is left out of the image with a CMake warning.
MISSING=$(grep -ahoP 'built-in ROM \K.*(?= not found)' "$LOG_DIR"/*.log 2>/dev/null | sed 's/: .*//' | sort -u || true)
if [ -n "$MISSING" ]; then
    echo ""
    echo "Built-in ROMs left out (not in ROMS_DIR):"
    echo "$MISSING" | sed 's/^/  /'
fi

echo ""
[ $FAILED = 0 ] && echo "All boards built; firmware in $OUTPUT_DIR/" || { echo "Some boards failed"; exit 1; }
