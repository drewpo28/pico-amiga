#!/usr/bin/env python3
"""Build a bootable IDE test ADF for the A600 (Gayle) with a bare-partition HDF
from make_hdf.py.  The bootblock drives the port directly and prints
"IDE OK" on the serial port, or "IDE NO <step>" at the first check that fails:

  A  IDENTIFY: DRQ, the model string ("PICO-AMIGA HDF", byte-swapped
     as the 68000 reads it), and the interrupt: Gayle's IRQ bit 7 and
     INTREQ PORTS set (Gayle's INT bit 7 enabled, the CPU's off)
  B  READ SECTORS, LBA 0: the virtual RDB's "RDSK"
  C  READ SECTORS, CHS 0/0/33, two sectors (LBA 32, 33): the image's "DOS\\1"
     boot block, then "MARK" (busy and a second interrupt between them)
  D  WRITE SECTORS, LBA 100: $1234 x 256, no error
  E  READ SECTORS, LBA 100: the words back

--slave: the same on the slave drive (select bit 4).
smoke.sh also checks the HDF file (--write-back): image block 68 is $1234s."""
import struct, sys

code = bytearray()
labels, fixups = {}, []

def w(*words):
    for x in words: code.extend(struct.pack(">H", x & 0xFFFF))
def l(x): w(x >> 16, x)
def label(n): labels[n] = len(code)
def br(op, n):                         # bra.w/bne.w/beq.w/bsr.w/bmi.w n
    w(op << 8); fixups.append(("w", len(code), n)); w(0)
def dbf_d1(n):
    w(0x51C9); fixups.append(("w", len(code), n)); w(0)
def lea_pc_a1(n):
    w(0x43FA); fixups.append(("w", len(code), n)); w(0)
BRA, BSR, BNE, BEQ, BMI = 0x60, 0x61, 0x66, 0x67, 0x6B

def reg_b(off, v): w(0x1B7C, v & 0xFF, off)      # move.b #v,off(a5)
def step(c): w(0x7400 | ord(c))                   # moveq #'c',d2: the step, for the message
def lea_a0(a): w(0x41F9); l(a)
def cmp_l(v, a): w(0x0CB9); l(v); l(a)            # cmpi.l #v,a
def cmp_w(v, a): w(0x0C79, v); l(a)               # cmpi.w #v,a

DATA, NSEC, SEC, LCYL, HCYL, SEL, CMD = 0, 8, 12, 16, 20, 24, 28
SLAVE = 0x10 if "--slave" in sys.argv else 0          # the same checks on the slave drive
args = [a for a in sys.argv[1:] if a != "--slave"]
BUF = 0x70000

w(0x4BF9); l(0xDA0000)                 # lea $DA0000,a5
w(0x33FC, 0x7FFF); l(0xDFF09A)         # INTENA: all off
w(0x33FC, 0x7FFF); l(0xDFF09C)         # INTREQ: all clear
w(0x13FC, 0x0080); l(0xDAA000)         # Gayle INT: IDE

step("A")
reg_b(SEL, 0xA0 | SLAVE); reg_b(CMD, 0xEC)
br(BSR, "wait")
w(0x0800, 3); br(BEQ, "no")            # btst #3,d0 (DRQ)
lea_a0(BUF); br(BSR, "read")
cmp_w(0x4950, BUF + 54)                # "PI" swapped: the model's first word
br(BNE, "no")
w(0x0839, 7); l(0xDA9000); br(BEQ, "no")   # btst #7,$DA9000
w(0x3039); l(0xDFF01E)                 # move.w INTREQR,d0
w(0x0800, 3); br(BEQ, "no")            # PORTS

step("B")
reg_b(SEL, 0xE0 | SLAVE); reg_b(SEC, 0); reg_b(LCYL, 0); reg_b(HCYL, 0); reg_b(NSEC, 1); reg_b(CMD, 0x20)
br(BSR, "wait")
lea_a0(BUF); br(BSR, "read")
cmp_l(0x5244534B, BUF); br(BNE, "no")  # "RDSK"

step("C")
reg_b(SEL, 0xA0 | SLAVE); reg_b(SEC, 33); reg_b(LCYL, 0); reg_b(HCYL, 0); reg_b(NSEC, 2); reg_b(CMD, 0x20)
br(BSR, "wait")
lea_a0(BUF); br(BSR, "read")
br(BSR, "wait")
w(0x0800, 3); br(BEQ, "no")            # DRQ for the second sector
br(BSR, "read")
cmp_l(0x444F5301, BUF); br(BNE, "no")
cmp_l(0x4D41524B, BUF + 512); br(BNE, "no")

step("D")
reg_b(SEL, 0xE0 | SLAVE); reg_b(SEC, 100); reg_b(LCYL, 0); reg_b(HCYL, 0); reg_b(NSEC, 1); reg_b(CMD, 0x30)
br(BSR, "wait")
w(0x0800, 3); br(BEQ, "no")            # DRQ
w(0x323C, 255)                         # move.w #255,d1
label("wr"); w(0x3ABC, 0x1234); dbf_d1("wr")   # move.w #$1234,(a5)
br(BSR, "wait")
w(0x0800, 0); br(BNE, "no")            # ERR

step("E")
reg_b(SEL, 0xE0 | SLAVE); reg_b(SEC, 100); reg_b(LCYL, 0); reg_b(HCYL, 0); reg_b(NSEC, 1); reg_b(CMD, 0x20)
br(BSR, "wait")
lea_a0(BUF); br(BSR, "read")
cmp_w(0x1234, BUF); br(BNE, "no")
cmp_w(0x1234, BUF + 510); br(BNE, "no")

lea_pc_a1("ok"); br(BSR, "puts")
label("forever"); br(BRA, "forever")
label("no")
lea_pc_a1("nomsg"); br(BSR, "puts")
w(0x1002); br(BSR, "putc")             # move.b d2,d0: the step
w(0x700A); br(BSR, "putc")             # moveq #10,d0
br(BRA, "forever")

label("puts")                          # the string at a1 to the serial port
w(0x1019)                              # move.b (a1)+,d0
br(BEQ, "puts_end")
br(BSR, "putc")
br(BRA, "puts")
label("puts_end"); w(0x4E75)
label("putc")
w(0x0240, 0x00FF, 0x0040, 0x0100)      # andi.w #$ff,d0; ori.w #$100,d0 (stop bit)
w(0x33C0); l(0xDFF030)                 # move.w d0,SERDAT (the bench takes it at once)
w(0x4E75)

label("wait")                          # d0 = status once not busy
w(0x102D, CMD); br(BMI, "wait")        # move.b 28(a5),d0; bmi.s
w(0x4E75)
label("read")                          # 256 words from the data port to (a0)+
w(0x323C, 255)
label("rd"); w(0x30D5); dbf_d1("rd")   # move.w (a5),(a0)+
w(0x4E75)

label("ok"); code.extend(b"IDE OK\n\0")
label("nomsg"); code.extend(b"IDE NO \0")

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
open(args[0] if args else "ide-test.adf", "wb").write(adf)
