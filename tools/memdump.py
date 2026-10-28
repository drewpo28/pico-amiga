#!/usr/bin/env python3
"""Turn memdump.gdb's dumps into /tmp/picospec_dump.log (pico-speccy's VS Code
extension opens it): the CPU registers and SR, the custom chip state, the code
around the PC (disassembled with build-host/m68kdis when it is built, else a
hex dump) and the copper list from COP1LC.  The binaries stay in /tmp for the
host tools: picoamiga_chip.bin, picoamiga_slow.bin, picoamiga_pc.bin.
"""
import os
import subprocess
import sys

STATE = "/tmp/picoamiga_state.txt"
CHIP = "/tmp/picoamiga_chip.bin"
PCBIN = "/tmp/picoamiga_pc.bin"
OUT = "/tmp/picospec_dump.log"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIS = os.path.join(ROOT, "build-host", "m68kdis")

# Custom register names by offset, for the copper list.
REGS = {
    0x02E: "COPCON", 0x040: "BLTCON0", 0x042: "BLTCON1", 0x058: "BLTSIZE", 0x080: "COP1LCH", 0x082: "COP1LCL",
    0x084: "COP2LCH", 0x086: "COP2LCL", 0x088: "COPJMP1", 0x08A: "COPJMP2", 0x08E: "DIWSTRT", 0x090: "DIWSTOP",
    0x092: "DDFSTRT", 0x094: "DDFSTOP", 0x096: "DMACON", 0x09A: "INTENA", 0x09C: "INTREQ", 0x09E: "ADKCON",
    0x100: "BPLCON0", 0x102: "BPLCON1", 0x104: "BPLCON2", 0x108: "BPL1MOD", 0x10A: "BPL2MOD",
}
for i in range(6):
    REGS[0x0E0 + 4 * i] = "BPL%dPTH" % (i + 1)
    REGS[0x0E2 + 4 * i] = "BPL%dPTL" % (i + 1)
for i in range(8):
    REGS[0x120 + 4 * i] = "SPR%dPTH" % i
    REGS[0x122 + 4 * i] = "SPR%dPTL" % i
for i in range(32):
    REGS[0x180 + 2 * i] = "COLOR%02d" % i


def state_lines():
    out = []
    for raw in open(STATE, errors="replace"):
        line = raw.rstrip("\n")
        # Cortex-Debug may log in MI form: ~"text\n"
        if line.startswith('~"') and line.endswith('"'):
            line = line[2:-1].replace("\\n", "").replace('\\"', '"')
        if line:
            out.append(line)
    return out


def kv(lines):
    d = {}
    for line in lines:
        parts = line.split()
        if len(parts) >= 2:
            d.setdefault(parts[0], parts[1:])
    return d


def sr_text(d):
    f = dict(zip(d["FLAGS"][0::2], d["FLAGS"][1::2]))
    h = lambda k: int(f[k], 16)
    sr = (h("t1") & 0x8000) | (h("s") << 11) | (h("int") & 0x700)
    sr |= ((h("x") & 0x100) >> 4) | ((h("n") & 0x80) >> 4) | ((1 if h("notz") == 0 else 0) << 2)
    sr |= ((h("v") & 0x80) >> 6) | ((h("c") & 0x100) >> 8)
    flags = "".join(c if sr & b else "-" for c, b in (("T", 0x8000), ("S", 0x2000), ("X", 16), ("N", 8), ("Z", 4), ("V", 2), ("C", 1)))
    return "SR %04X  %s  IPL mask %d" % (sr, flags, (sr >> 8) & 7)


def copper_list(chip, lc, count=200):
    out = []
    mask = len(chip) - 1
    pc = lc
    for _ in range(count):
        a = pc & mask & ~1
        if a + 4 > len(chip):
            break
        ir1 = chip[a] << 8 | chip[a + 1]
        ir2 = chip[a + 2] << 8 | chip[a + 3]
        if not ir1 & 1:
            reg = ir1 & 0x1FE
            out.append("%06X  MOVE %-8s = %04X" % (pc, REGS.get(reg, "$%03X" % reg), ir2))
        else:
            kind = "SKIP" if ir2 & 1 else "WAIT"
            out.append("%06X  %s v %02X h %02X  (mask v %02X h %02X)" % (pc, kind, ir1 >> 8, ir1 & 0xFE, (ir2 >> 8) & 0x7F, ir2 & 0xFE))
            if ir1 == 0xFFFF and ir2 == 0xFFFE:
                break
        pc += 4
    return out


def code_around_pc(pc):
    base = (pc - 64) & 0xFFFFFF
    if os.path.exists(DIS):
        r = subprocess.run([DIS, PCBIN, "%X" % base, "%X" % ((pc - 32) & 0xFFFFFF), "%X" % ((pc + 96) & 0xFFFFFF)],
                           capture_output=True, text=True)
        if r.returncode == 0:
            return [("=> " if l.startswith("%06X" % pc) else "   ") + l for l in r.stdout.splitlines()]
    data = open(PCBIN, "rb").read()
    out = ["(build-host/m68kdis not built: cmake --build build-host --target m68kdis)"]
    for i in range(0, len(data), 16):
        out.append("%06X  %s" % (base + i, " ".join("%02X%02X" % (data[j], data[j + 1]) for j in range(i, min(i + 16, len(data)), 2))))
    return out


def main():
    lines = state_lines()
    d = kv(lines)
    chip = open(CHIP, "rb").read()
    pc = int(d["PC"][0], 16)
    log = ["pico-amiga memory dump", ""]
    log += ["PC %06X  (previous %s)" % (pc, d["PPC"][0]), sr_text(d)]
    log += ["D0-D7 " + " ".join(d["D"]), "A0-A7 " + " ".join(d["A"]), "USP %s  SSP %s" % (d["USP"][0], d["SSP"][0]), ""]
    log += ["--- state (memdump.gdb)"] + lines + [""]
    log += ["--- code around PC"] + code_around_pc(pc) + [""]
    lc = int(d["COP1LC"][0], 16)
    log += ["--- copper list from COP1LC %06X" % lc] + copper_list(chip, lc) + [""]
    log += ["--- files", CHIP + " (chip RAM, %d KB)" % (len(chip) // 1024)]
    if os.path.exists("/tmp/picoamiga_slow.bin"):
        log.append("/tmp/picoamiga_slow.bin (slow RAM)")
    open(OUT, "w").write("\n".join(log) + "\n")
    print(OUT)


if __name__ == "__main__":
    sys.exit(main())
