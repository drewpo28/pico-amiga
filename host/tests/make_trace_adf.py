#!/usr/bin/env python3
"""Build a bootable test ADF for the CPU's trace mode: the bootblock enters
supervisor mode, points the trace vector at a counter and runs the same dozen
instructions twice - with SR's T1 set (every instruction traced: 15), then with
T0 (a 68020's trace on change of flow: 6 there, 0 on a 68000, which has no T0) -
and prints "TRACE " and the two counts as letters ('A' + n) on the serial port:
"TRACE PA" on a 68000, "TRACE PG" on a 68EC020.  Musashi runs traced instructions in
a loop of their own (m68krun.h); this is its test."""
import struct, sys

def body(set_bits, clr_mask, reg):
    return bytes.fromhex(
        "7%s00" % ("E" if reg == 7 else "C") +   # moveq  #0,d7 / d6
        "007C%04X" % set_bits +                  # ori.w  #T,sr          ; not traced itself
        "7001"                                   # moveq  #1,d0
        "5280"                                   # addq.l #1,d0
        "6002"                                   # bra.s  +2
        "4E71"                                   # nop                   ; skipped
        "7205"                                   # moveq  #5,d1
        "5381"                                   # l: subq.l #1,d1
        "66FC"                                   # bne.s  l
        "027C%04X" % clr_mask)                   # andi.w #~T,sr         ; traced: it began with T set

sup = (bytes.fromhex("41FA0000"                  # lea    handler(pc),a0  (displacement patched below)
                     "2F380024"                  # move.l $24.w,-(sp)
                     "21C80024")                 # move.l a0,$24.w
       + body(0x8000, 0x7FFF, 7)
       + bytes.fromhex("41FA0000"                # lea    handler6(pc),a0
                       "21C80024")               # move.l a0,$24.w
       + body(0x4000, 0xBFFF, 6)
       + bytes.fromhex("21DF0024"                # move.l (sp)+,$24.w
                       "4E73"))                  # rte
h7 = len(sup)
sup += bytes.fromhex("52874E73")                 # handler:  addq.l #1,d7 ; rte
h6 = len(sup)
sup += bytes.fromhex("52864E73")                 # handler6: addq.l #1,d6 ; rte
sup = bytearray(sup)
struct.pack_into(">h", sup, 2, h7 - 2)
lea6 = 12 + len(body(0, 0, 7))
struct.pack_into(">h", sup, lea6 + 2, h6 - (lea6 + 2))

msg = b"TRACE \0\0"
main = bytearray(bytes.fromhex(
    "4BFA0000"          # lea     super(pc),a5
    "4EAEFFE2"          # jsr     -30(a6)              ; exec Supervisor()
    "41FA0000"          # lea     msg(pc),a0
    "7000"              # loop:  moveq  #0,d0
    "1018"              #        move.b (a0)+,d0
    "6706"              #        beq.s  counts
    "6100000E"          #        bsr.w  put
    "60F4"              #        bra.s  loop
    "7041D087"          # counts: moveq #'A',d0 ; add.l d7,d0
    "61000006"          #        bsr.w  put            (two bytes further each time)
    "7041D086"          #        moveq  #'A',d0 ; add.l d6,d0
    "6102"              #        bsr.s  put
    "60FE"              # hang:  bra.s  hang
    "00400100"          # put:   ori.w  #$100,d0       ; stop bit
    "33C000DFF030"      #        move.w d0,$dff030     ; SERDAT
    "323C1000"          #        move.w #$1000,d1      ; let the byte out
    "51C9FFFE"          # wait:  dbf    d1,wait
    "4E75"))            #        rts
# Fix the displacements of the three PC-relative operands and the two bsr.w.
put = main.index(bytes.fromhex("00400100"))
sup_at = len(main)
msg_at = sup_at + len(sup)
struct.pack_into(">h", main, 2, sup_at - 2)
struct.pack_into(">h", main, 10, msg_at - 10)
bsr1 = main.index(bytes.fromhex("6100000E"))
struct.pack_into(">h", main, bsr1 + 2, put - (bsr1 + 2))
bsr2 = main.index(bytes.fromhex("61000006"))
struct.pack_into(">h", main, bsr2 + 2, put - (bsr2 + 2))
bsr3 = main.index(bytes.fromhex("6102"), bsr2)
main[bsr3 + 1] = put - (bsr3 + 2)
code = bytes(main) + bytes(sup) + msg

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
open(sys.argv[1] if len(sys.argv) > 1 else "trace-test.adf", "wb").write(adf)
