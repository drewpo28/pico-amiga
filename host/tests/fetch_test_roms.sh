#!/bin/sh
# Fetch the freely distributable test ROMs (AROS Kickstart replacement and
# DiagROM) from the vAmiga repository into ./roms (or $1).
set -e
DEST=${1:-roms}
TMP=$(mktemp -d)
git clone -q --depth 1 --filter=blob:none --no-checkout https://github.com/dirkwhoffmann/vAmiga "$TMP/v"
cd "$TMP/v"
git sparse-checkout init --no-cone
git sparse-checkout set 'Resources/Assets.xcassets/Binary/aros-svn55696-*' \
                        'Resources/Assets.xcassets/Binary/diagrom-13.dataset/*'
git checkout -q
cd - >/dev/null
mkdir -p "$DEST"
cp "$TMP"/v/Resources/Assets.xcassets/Binary/aros-svn55696-rom.dataset/*.bin "$DEST/aros-rom.bin"
cp "$TMP"/v/Resources/Assets.xcassets/Binary/aros-svn55696-ext.dataset/*.bin "$DEST/aros-ext.bin"
cp "$TMP"/v/Resources/Assets.xcassets/Binary/diagrom-13.dataset/*.bin "$DEST/diagrom-13.bin"
rm -rf "$TMP"
ls -l "$DEST"
