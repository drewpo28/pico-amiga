# Reference screenshots

`diagrom-d655a1c/` — DiagROM 1.3 menus as the HDMI scanout showed them at
commit d655a1c (720x576; hires lines still averaged into 360 lowres columns).
Kept as the "before" for the native-hires / interlace framebuffer change.

Regenerate a set from the current tree with

    host/tests/diagrom_screens.sh roms/diagrom-13.bin <out-dir>
