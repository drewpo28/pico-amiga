#!/usr/bin/env python3
"""Build a bootable Gayle (A600) test ADF.  The bootblock prints "GAYLE OK" on
the serial port when

  * the ID at $DE1000, restarted by a write and read as eight bytes (bit 7
    of each, MSB first), is $D0;
  * the empty IDE port's status register ($DA201C) reads $7F;
  * $A0E001, a CIA-A mirror on the A500 / A500+, reads 0 (Gayle machines
    decode the CIAs at $BF0000 only);

"GAYLE NO" otherwise (an A500+ prints that).  smoke.sh checks both."""
import struct, sys

code = bytearray()
labels, fixups = {}, []

def w(*words):
    for x in words: code.extend(struct.pack(">H", x & 0xFFFF))
def l(x): w(x >> 16, x)
def label(n): labels[n] = len(code)
def br(op, n):                         # bra.s/bne.s/beq.s n
    fixups.append(("s", len(code), n)); w(op << 8)
def dbf_d2(n):
    w(0x51CA); fixups.append(("w", len(code), n)); w(0)
def lea_pc_a0(n):
    w(0x41FA); fixups.append(("w", len(code), n)); w(0)
def move_w_abs(imm, a): w(0x33FC, imm); l(a)

move_w_abs(0x7FFF, 0xDFF09A)           # INTENA: all off
move_w_abs(0x7FFF, 0xDFF096)           # DMACON: all off
w(0x13FC, 0x0000); l(0xDE1000)         # move.b #0,$DE1000: the ID from its first bit
w(0x7200)                              # moveq #0,d1
w(0x7407)                              # moveq #7,d2
label("id")
w(0x1039); l(0xDE1000)                 # move.b $DE1000,d0
w(0xD000)                              # add.b d0,d0: bit 7 into X
w(0xD301)                              # addx.b d1,d1: and into d1
dbf_d2("id")
w(0x0C01, 0x00D0)                      # cmpi.b #$d0,d1
br(0x66, "no")
w(0x1039); l(0xDA201C)                 # move.b $DA201C,d0 (IDE status)
w(0x0C00, 0x007F)                      # cmpi.b #$7f,d0
br(0x66, "no")
w(0x4A39); l(0xA0E001)                 # tst.b $A0E001
br(0x66, "no")
lea_pc_a0("ok"); br(0x60, "print")
label("no"); lea_pc_a0("nomsg")
label("print")
w(0x1018)                              # move.b (a0)+,d0
br(0x67, "forever")
w(0x0240, 0x00FF, 0x0040, 0x0100)      # andi.w #$ff,d0; ori.w #$100,d0 (stop bit)
w(0x33C0); l(0xDFF030)                 # move.w d0,SERDAT (the bench takes it at once)
br(0x60, "print")
label("forever"); br(0x60, "forever")

label("ok"); code.extend(b"GAYLE OK\n\0")
label("nomsg"); code.extend(b"GAYLE NO\n\0")

for kind, at, n in fixups:
    d = labels[n] - (at + 2) if kind == "s" else labels[n] - at
    if kind == "s":
        assert -128 <= d < 128 and d != 0, (n, d)
        code[at + 1] = d & 0xFF
    else:
        struct.pack_into(">h", code, at, d)

boot = bytearray(1024)
boot[0:4] = b"DOS\0"
struct.pack_into(">I", boot, 8, 880)
assert 12 + len(code) <= 1024
boot[12:12 + len(code)] = code
s = 0
for i in range(0, 1024, 4):
    s += struct.unpack_from(">I", boot, i)[0]
    if s > 0xFFFFFFFF:
        s = (s + 1) & 0xFFFFFFFF
struct.pack_into(">I", boot, 4, (~s) & 0xFFFFFFFF)

adf = bytearray(901120)
adf[0:1024] = boot
open(sys.argv[1] if len(sys.argv) > 1 else "gayle-test.adf", "wb").write(adf)
