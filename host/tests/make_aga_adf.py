#!/usr/bin/env python3
"""Build a bootable AGA test ADF.  The bootblock prints "AGA OK" on the serial
port when VPOSR has Alice's ID ($23, PAL) and DENISEID reads Lisa's $F8, "AGA NO"
otherwise, then shows 8 lowres planes of 1s (BPU3) with FMODE 3 (the 64-bit
fetch: 32-colour-clock units of 4 words a plane) through a copper list that:

  * sets COLOR31 in bank 6 (BPLCON3 BANK = 6) to $0F0: palette entry 223;
  * writes $00F to it again with LOCT set: the low nibbles of the 8-bit guns,
    which the 12-bit output does not show (the entry stays $0F0);
  * sets COLOR00 in bank 0 to black (the border);
  * sets BPLCON4's BPLAM to $20: the planes' value 255 shows entry 223.

So the window is $0F0 with 8 planes, the wide fetch, Lisa's banks, LOCT and
BPLAM right, black around it.

With --ham8 the picture is hires HAM8 instead: plane 1 all 0s, planes 2-8 all 1s (one
buffer),
so every pixel is $FE - "modify red" with the data $3F - after COLOR00 = $00F at
the window's start: $F0F inside, the $00F border around it.
smoke.sh checks both."""
import struct, sys

HAM8 = "--ham8" in sys.argv
args = [a for a in sys.argv[1:] if a != "--ham8"]

code = bytearray()
labels, fixups = {}, []

def w(*words):
    for x in words: code.extend(struct.pack(">H", x & 0xFFFF))
def l(x): w(x >> 16, x)
def label(n): labels[n] = len(code)
def br(op, n):                         # bra.s/bne.s/beq.s n
    fixups.append(("s", len(code), n)); w(op << 8)
def dbf_d1(n):
    w(0x51C9); fixups.append(("w", len(code), n)); w(0)
def lea_pc_a0(n):
    w(0x41FA); fixups.append(("w", len(code), n)); w(0)
def move_w_abs(imm, a): w(0x33FC, imm); l(a)
def move_l_abs(imm, a): w(0x23FC); l(imm); l(a)
def lea_a0(a): w(0x41F9); l(a)
def fill_w(a, words, v):               # lea a,a0; move.w #words-1,d1; .f: move.w #v,(a0)+; dbf d1,.f
    lea_a0(a); w(0x323C, words - 1)
    n = "f%d" % len(code); label(n); w(0x30FC, v); dbf_d1(n)
def cop(*moves):                       # move.l #x,(a0)+ each
    for x in moves: w(0x20FC); l(x)

move_w_abs(0x7FFF, 0xDFF09A)           # INTENA: all off
move_w_abs(0x7FFF, 0xDFF096)           # DMACON: all off
w(0x3039); l(0xDFF004)                 # move.w VPOSR,d0
w(0x0240, 0x7F00)                      # andi.w #$7f00,d0
w(0x0C40, 0x2300)                      # cmpi.w #$2300,d0: Alice PAL
br(0x66, "no")
w(0x3039); l(0xDFF07C)                 # move.w DENISEID,d0
w(0x0C00, 0x00F8)                      # cmpi.b #$f8,d0: Lisa
br(0x66, "no")
lea_pc_a0("ok"); br(0x60, "print")
label("no"); lea_pc_a0("nomsg")
label("print")
w(0x1018)                              # move.b (a0)+,d0
br(0x67, "show")
w(0x0240, 0x00FF, 0x0040, 0x0100)      # andi.w #$ff,d0; ori.w #$100,d0 (stop bit)
w(0x33C0); l(0xDFF030)                 # move.w d0,SERDAT
br(0x60, "print")

label("show")
fill_w(0xD0000, 0x10000, 0xFFFF)       # 8 planes of 16 KB, all 1s
if HAM8: fill_w(0xF0000, 0x4000, 0)    # HAM8: plane 1's 32 KB of 0s
lea_a0(0xC0000)
for p in range(8):                     # BPL1PT..BPL8PT = $D0000 + 16 KB x p
    a = (0xF0000 if p == 0 else 0xD0000) if HAM8 else 0xD0000 + p * 0x4000   # HAM8: hires
    cop(((0xE0 + 4 * p) << 16) | (a >> 16), ((0xE2 + 4 * p) << 16) | (a & 0xFFFF))   # planes share the 1s
if HAM8: cop(0x01008A10,               # BPLCON0: hires, HAM, 8 planes, COLOR
             0x01020000, 0x01080000, 0x010A0000, 0x01FC0003,
             0x008E2C81, 0x00902CC1, 0x0092003C, 0x009400D0,
             0x01060000, 0x0180000F,   # COLOR00 = $00F
             0x010C0011, 0xFFFFFFFE)
cop(0x01000210,                        # BPLCON0: 8 planes (BPU3), COLOR
    0x01020000, 0x01080000, 0x010A0000,   # BPLCON1, modulos
    0x01FC0003,                        # FMODE: 64-bit bitplane fetch
    0x008E2C81, 0x00902CC1,            # DIW 320 x 256
    0x00920038, 0x009400D0,            # DDF lowres
    0x0106C000, 0x01BE00F0,            # BANK 6: COLOR31 -> entry 223 = $0F0
    0x0106C200, 0x01BE000F,            # BANK 6 + LOCT: low nibbles only, not shown
    0x01060000, 0x01800000,            # BANK 0: COLOR00 = black (border)
    0x010C2011,                        # BPLCON4: BPLAM $20 - pf value 255 shows entry 223
    0xFFFFFFFE)
move_l_abs(0xC0000, 0xDFF080)          # COP1LC
w(0x33C0); l(0xDFF088)                 # COPJMP1
move_w_abs(0x8380, 0xDFF096)           # DMACON: SET | DMAEN | BPLEN | COPEN
label("forever"); br(0x60, "forever")

label("ok"); code.extend(b"AGA OK\n\0")
label("nomsg"); code.extend(b"AGA NO\n\0")

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
open(args[0] if args else "aga-test.adf", "wb").write(adf)
