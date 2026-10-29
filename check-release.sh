#!/bin/bash
# Pre-release checks, no board needed (after pico-speccy's check-release.sh):
#
#  1. The heap of each board's image: what the linker leaves between the end of
#     .bss (`end`) and __HeapLimit is the whole C/C++ heap - the menu's strings,
#     the file browser, a disk's info. With 5.4 KB the DF0 browser panicked "Out
#     of memory" (CLAUDE.md, Measured), hence the thresholds.
#  2. The host tests: tmds_test (the PIO HDMI words and every mode's layout) and
#     host/tests/smoke.sh (needs the test ROMs in roms/: host/tests/fetch_test_roms.sh).
#
#   ./check-release.sh            after ./build_all.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if ! command -v arm-none-eabi-nm >/dev/null 2>&1; then
    TC_VER=$(grep -oP 'set\(toolchainVersion \K[^)]+' CMakeLists.txt)
    TC_DIR="$HOME/.pico-sdk/toolchain/$TC_VER/bin"
    [ -d "$TC_DIR" ] || TC_DIR=$(ls -d "$HOME/.pico-sdk/toolchain"/*/bin 2>/dev/null | sort -V | tail -1)
    [ -n "$TC_DIR" ] && export PATH="$TC_DIR:$PATH"
fi
command -v arm-none-eabi-nm >/dev/null 2>&1 || { echo "Error: arm-none-eabi-nm not found"; exit 1; }

VER=$(grep -oP 'set\(PORT_VERSION "\K[0-9.]+' CMakeLists.txt)
WARN_BELOW=12288
FAIL_BELOW=8192
EXIT=0

printf "pico-amiga pre-release check - v%s\n\n" "$VER"
printf "%-9s %-26s %8s  %s\n" "BOARD" "IMAGE" "HEAP" "STATUS"
for dir in build-all/*/; do
    B=$(basename "$dir")
    [ "$B" = logs ] && continue
    ELF=$(ls -t "$dir"*-amiga-*.elf 2>/dev/null | head -1)
    if [ -z "$ELF" ]; then printf "%-9s %-26s %8s  %s\n" "$B" "-" "-" "NOT BUILT"; continue; fi
    END=$(arm-none-eabi-nm "$ELF" | awk '$3 == "end" { print $1 }')
    LIM=$(arm-none-eabi-nm "$ELF" | awk '$3 == "__HeapLimit" { print $1 }')
    HEAP=$(( 16#$LIM - 16#$END ))
    STATUS=OK
    if [ $HEAP -lt $FAIL_BELOW ]; then STATUS=FAIL; EXIT=1
    elif [ $HEAP -lt $WARN_BELOW ]; then STATUS=WARN; fi
    [[ "$(basename "$ELF")" == *"-$VER.elf" ]] || STATUS="$STATUS (stale: not v$VER)"
    printf "%-9s %-26s %6d K  %s\n" "$B" "$(basename "$ELF")" $((HEAP / 1024)) "$STATUS"
done
echo ""
echo "HEAP = __HeapLimit - end: the C/C++ heap (WARN below $((WARN_BELOW / 1024)) KB, FAIL below $((FAIL_BELOW / 1024)) KB)"
echo ""

echo "=== host tests ==="
cmake -S host -B build-host >/dev/null && cmake --build build-host -j >/dev/null
./build-host/tmds_test || EXIT=1
./build-host/tmds_test_zero2 || EXIT=1
if [ -f roms/aros-rom.bin ] && [ -f roms/diagrom-13.bin ]; then
    host/tests/smoke.sh roms || EXIT=1
else
    echo "smoke: skipped - no test ROMs in roms/ (host/tests/fetch_test_roms.sh)"
fi

echo ""
[ $EXIT = 0 ] && echo "All checks passed." || echo "Some checks FAILED."
exit $EXIT
