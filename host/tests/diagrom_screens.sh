#!/bin/sh
# Reference screenshots of DiagROM 1.3 menus (720x576, as the HDMI scanout shows
# them), for before/after comparisons of the video path.
#   host/tests/diagrom_screens.sh [rom] [out-dir] [host-binary]
ROM=${1:-roms/diagrom-13.bin}
OUT=${2:-build-host/screens}
BIN=${3:-build-host/pico-amiga-host}
mkdir -p "$OUT"
shot() {  # name, frames, keys...   key = raw Amiga keycode pressed at frame 1500
    name=$1; frames=$2; shift 2
    args=""; f=1500
    for k in "$@"; do args="$args --key $f:$k:1 --key $((f + 5)):$k:0"; f=$((f + 250)); done
    $BIN --rom "$ROM" --frames "$frames" $args --shot "$frames:$OUT/$name.png" > /dev/null 2>&1
    echo "$OUT/$name.png"
}
shot 00-main-menu    1500
shot 01-systeminfo   1900 0x0A
shot 02-audiotests   1800 0x01
shot 03-memorytests  1800 0x02
shot 04-irq-cia      1800 0x03
shot 05-graphictests 1800 0x04
shot 06-graphics-1   2200 0x04 0x01
shot 06-graphics-2   2200 0x04 0x02
shot 06-graphics-5   2200 0x04 0x05
shot 07-porttests    1800 0x05
shot 08-drivetests   1800 0x06
shot 09-keyboard     1800 0x07
shot 10-other        1800 0x08
shot 11-setup        1800 0x21
shot 12-about        1800 0x20
