#!/usr/bin/env python3
"""Build a bootable ECS test ADF.  The bootblock prints "ECS OK" on the serial
port when VPOSR has the ECS Agnus ID ($20) and DENISEID reads $FC, "ECS NO"
otherwise, then shows a super-hires screen from the upper 512 KB of a 1 MB
chip RAM:

  * the copper list at $C0000, one bitplane at $D0000 (DDF $38/$D8, as
    KS 2.04 sets for a super-hires Workbench), filled with $5555:
    every pixel pair is (0, 1), which selects COLOR04 = $C30 - the left pixel
    $F00 (the top bits), the right one $0F0 (the low bits); averaged into one
    output pixel, $770;
  * at $40000 / $50000, where those addresses land with 19-bit DMA pointers or
    512 KB of chip RAM, an empty copper list and a $FFFF plane (COLOR15 = 0):
    a black screen.

So the middle of the picture is $770 only with ECS super-hires, 21-bit copper
and bitplane pointers and 1 MB of chip RAM.  smoke.sh checks it."""
import struct, sys

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
w(0x0C40, 0x2000)                      # cmpi.w #$2000,d0
br(0x66, "no")
w(0x3039); l(0xDFF07C)                 # move.w DENISEID,d0
w(0x0C00, 0x00FC)                      # cmpi.b #$fc,d0
br(0x66, "no")
lea_pc_a0("ok"); br(0x60, "print")
label("no"); lea_pc_a0("nomsg")
label("print")
w(0x1018)                              # move.b (a0)+,d0
br(0x67, "shres")
w(0x0240, 0x00FF, 0x0040, 0x0100)      # andi.w #$ff,d0; ori.w #$100,d0 (stop bit)
w(0x33C0); l(0xDFF030)                 # move.w d0,SERDAT
br(0x60, "print")

label("shres")
# The aliases first: with 1 MB of chip RAM and 21-bit pointers they are not read.
lea_a0(0x40000); cop(0xFFFFFFFE)
fill_w(0x50000, 168 // 2 * 256, 0xFFFF)
fill_w(0xD0000, 168 // 2 * 256, 0x5555)   # 1344 x 256, one plane
lea_a0(0xC0000)
cop(0x00E0000D, 0x00E20000,            # BPL1PT = $D0000
    0x01001240,                        # BPLCON0: 1 plane, COLOR, SHRES
    0x01020000, 0x01080000,            # BPLCON1, BPL1MOD
    0x008E2C81, 0x00902CC1,            # DIW 320 x 256 lowres
    0x00920038, 0x009400D8,            # DDF as Workbench's: 21 units of 4 words, from DIW x $75
    0x01800000, 0x01880C30, 0x019E0000,   # COLOR00, COLOR04, COLOR15
    0xFFFFFFFE)
move_l_abs(0xC0000, 0xDFF080)          # COP1LC
w(0x33C0); l(0xDFF088)                 # COPJMP1
move_w_abs(0x8380, 0xDFF096)           # DMACON: SET | DMAEN | BPLEN | COPEN
label("forever"); br(0x60, "forever")

label("ok"); code.extend(b"ECS OK\n\0")
label("nomsg"); code.extend(b"ECS NO\n\0")

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
open(sys.argv[1] if len(sys.argv) > 1 else "ecs-test.adf", "wb").write(adf)
