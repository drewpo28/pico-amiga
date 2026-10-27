#!/usr/bin/env python3
"""Build a tiny bootable test ADF: the bootblock prints "BOOTBLOCK OK" on the
serial port and then paints COLOR00 green forever.  Booting it end to end
exercises CIA-B drive control, the MFM track encoder, disk DMA with sync,
trackdisk.device's sector decoder and the boot strap."""
import struct, sys

code = bytes.fromhex(
    "41FA0020"          # lea     msg(pc),a0
    "1018"              # loop:  move.b (a0)+,d0
    "6710"              #        beq.s  done
    "024000FF"          #        andi.w #$ff,d0
    "00400100"          #        ori.w  #$100,d0       ; stop bit
    "33C000DFF030"      #        move.w d0,$dff030     ; SERDAT
    "60EC"              #        bra.s  loop
    "33FC00F000DFF180"  # done:  move.w #$0f0,$dff180  ; COLOR00 = green
    "60F6"              #        bra.s  done
) + b"BOOTBLOCK OK\n\0"

boot = bytearray(1024)
boot[0:4] = b"DOS\0"
struct.pack_into(">I", boot, 8, 880)
boot[12:12 + len(code)] = code
s = 0
for i in range(0, 1024, 4):
    s += struct.unpack_from(">I", boot, i)[0]
    if s > 0xFFFFFFFF:
        s = (s + 1) & 0xFFFFFFFF
struct.pack_into(">I", boot, 4, (~s) & 0xFFFFFFFF)

adf = bytearray(901120)
adf[0:1024] = boot
open(sys.argv[1] if len(sys.argv) > 1 else "boot-test.adf", "wb").write(adf)
