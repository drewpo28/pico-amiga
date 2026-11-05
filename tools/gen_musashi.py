#!/usr/bin/env python3
"""Regenerate external/musashi/gen/m68kops.{c,h} with CONST opcode tables.

Stock Musashi builds its opcode -> handler jump table (64K pointers = 256 KB)
and a 5 x 64 KB cycle table in RAM at m68k_init() time.  The RP2350 has 520 KB
of SRAM in total, so pico-amiga uses const tables instead (they land in flash):

    m68ki_handler_index[0x10000]  u16  opcode -> index into m68ki_handlers[]
    m68ki_handlers[]              the handlers actually referenced (illegal = 0,
                                  then tools/musashi_hot.txt's, then the rest)
    m68ki_handler_cycles[]        u8   68000 base cycles per handler (the same for
                                  every opcode of a handler, illegal aside)
    m68ki_cycles_000[0x10000]     u8   68000 base cycle counts (illegal opcodes and
                                  the exception paths)

The handlers named in tools/musashi_hot.txt are marked M68KI_HOT (SRAM on the
board, m68kconf.h); the handler table and the per-handler cycles are not const
(SRAM too), so a common instruction touches flash only for its index entry.

The handlers are `static` in m68kops.c, so the tables are appended to that
same translation unit.  The stock table builder stays in the file under
`#if !M68K_CONST_TABLES` (host builds can still flip back to it to compare).

How: build m68kmake, generate m68kops.c, then compile a small host program
that #includes the stock m68kops.c, runs m68ki_build_opcode_table() and dumps
the result, mapping every handler pointer back to its row in the static
m68k_opcode_handler_table[] (whose names this script parses from the text).

Run from anywhere:  python3 tools/gen_musashi.py   (needs a host `cc`)
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MUS = os.path.join(ROOT, "external", "musashi")
OUT = os.path.join(MUS, "gen")
CC = os.environ.get("CC", "cc")

TABGEN_C = r'''
#include <stdio.h>
#include <stdint.h>
#include "m68kops.c"   /* the STOCK generated file: static handlers + table */

unsigned int m68k_read_memory_8(unsigned int a)  { (void)a; return 0; }
unsigned int m68k_read_memory_16(unsigned int a) { (void)a; return 0; }
unsigned int m68k_read_memory_32(unsigned int a) { (void)a; return 0; }
void m68k_write_memory_8(unsigned int a, unsigned int v)  { (void)a; (void)v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { (void)a; (void)v; }
void m68k_write_memory_32(unsigned int a, unsigned int v) { (void)a; (void)v; }
void amiga_cpu_reset_instr(void) {}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "wb");
    int n = 0, op, i;
    m68ki_build_opcode_table();
    while (m68k_opcode_handler_table[n].opcode_handler) n++;
    fwrite(&n, 4, 1, f);
    for (op = 0; op < 0x10000; op++) {
        int idx = -1;
        for (i = 0; i < n; i++)
            if (m68k_opcode_handler_table[i].opcode_handler == m68ki_instruction_jump_table[op]) { idx = i; break; }
        /* -1: the builder's default (m68k_op_illegal for opcodes no row matches) */
        fwrite(&idx, 4, 1, f);
        fputc(m68ki_cycles[0][op], f);
        fputc(m68ki_cycles[2][op], f);     /* the 68020 / 68EC020's */
    }
    fclose(f);
    (void)argc;
    return 0;
}
'''


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode:
        sys.exit("failed: " + " ".join(cmd))


def main():
    os.makedirs(OUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        mk = os.path.join(tmp, "m68kmake")
        run([CC, "-O1", "-o", mk, os.path.join(MUS, "m68kmake.c")])
        run([mk, tmp, os.path.join(MUS, "m68k_in.c")], stdout=subprocess.DEVNULL)
        stock_c = os.path.join(tmp, "m68kops.c")
        stock_h = os.path.join(tmp, "m68kops.h")

        src = os.path.join(tmp, "tabgen.c")
        open(src, "w").write(TABGEN_C)
        exe = os.path.join(tmp, "tabgen")
        run([CC, "-O1", "-w", "-DM68K_CONST_TABLES=0", "-I", tmp, "-I", MUS,
             "-o", exe, src, os.path.join(MUS, "m68kcpu.c"),
             os.path.join(MUS, "softfloat", "softfloat.c"), "-lm"])
        dump = os.path.join(tmp, "tab.bin")
        run([exe, dump])

        text = open(stock_c).read()
        hdr = open(stock_h).read()
        raw = open(dump, "rb").read()

    # Row names of m68k_opcode_handler_table[], in order.
    start = text.index("static const opcode_handler_struct m68k_opcode_handler_table[] =")
    end = text.index("{0, 0, 0, {0, 0, 0, 0, 0}}", start)
    names = re.findall(r"\{(m68k_op_\w+)\s*,", text[start:end])

    n = struct.unpack_from("<i", raw, 0)[0]
    if n != len(names):
        sys.exit("table rows %d != parsed names %d" % (n, len(names)))
    hot = [l.strip() for l in open(os.path.join(ROOT, "tools", "musashi_hot.txt"))
           if l.strip() and not l.startswith("#")]
    ops = []
    for op in range(0x10000):
        idx, cyc, cyc20 = struct.unpack_from("<iBB", raw, 4 + op * 6)
        ops.append(("m68k_op_illegal" if idx < 0 else names[idx], cyc, cyc20))
    referenced = set(o[0] for o in ops)
    missing = [name for name in hot if name not in referenced]
    if missing:
        sys.exit("musashi_hot.txt names unknown handlers: " + " ".join(missing))
    used = ["m68k_op_illegal"] + hot
    for o in ops:
        if o[0] not in used:
            used.append(o[0])
    slot = {name: i for i, name in enumerate(used)}
    index = [slot[o[0]] for o in ops]
    cycles = [o[1] for o in ops]
    cycles20 = [o[2] for o in ops]

    def per_handler(k):
        h = [None] * len(used)
        for o in ops:
            i = slot[o[0]]
            if h[i] is None:
                h[i] = o[k]
            elif h[i] != o[k] and i:
                sys.exit("%s: cycles differ between its opcodes" % o[0])
        h[0] = 0                    # illegal: m68k_execute() uses the opcode's cycle table
        return [v or 0 for v in h]
    hcyc, hcyc20 = per_handler(1), per_handler(2)
    # The 68EC020's own list (tools/musashi_hot_020.txt): in SRAM for both CPUs
    # (M68KI_HOT2), the 68000 only (M68KI_HOT) or the 68EC020 only (M68KI_HOT_020).
    hot020 = [l.strip() for l in open(os.path.join(ROOT, "tools", "musashi_hot_020.txt"))
              if l.strip() and not l.startswith("#")]
    missing = [name for name in hot020 if name not in referenced]
    if missing:
        sys.exit("musashi_hot_020.txt names unknown handlers: " + " ".join(missing))
    hotset, hot2 = set(hot), set(hot020)
    def mark(name):
        if name in hotset and name in hot2: return "M68KI_HOT2 "
        if name in hotset: return "M68KI_HOT "
        if name in hot2: return "M68KI_HOT_020 "
        return ""
    text = re.sub(r"^static void (m68k_op_\w+)\(void\)$",
                  lambda m: "%sstatic void %s(void)" % (mark(m.group(1)), m.group(1)),
                  text, flags=re.M)

    # Fence the stock RAM tables + builder off the const build.
    tb = text.index("/* ========================= OPCODE TABLE BUILDER ========================= */")
    tb = text.rindex("/* ====", 0, tb)
    body = text[:tb] + "#if !M68K_CONST_TABLES\n" + text[tb:]
    # The 68000 set (this file compiled as it is) fixes its CPU type too.
    body = body.replace('#include "m68kcpu.h"', '#ifndef M68KI_FIXED_EC020\n#define M68KI_FIXED_000 1\n#endif\n#include "m68kcpu.h"', 1)
    eof = body.rindex("/* ======================================================================== */\n/* ============================== END OF FILE")
    out = [body[:eof], "#endif /* !M68K_CONST_TABLES */\n\n",
           "#if M68K_CONST_TABLES\n",
           "/* Generated by tools/gen_musashi.py - do not edit.\n",
           " * This file is compiled twice (amiga_core.cmake): as it is for the 68000 and\n",
           " * through m68kops_020.c (M68KI_FIXED_EC020) for the 68EC020, each with its CPU\n",
           " * type a constant, so neither set of handlers tests it at run time.  Both sets\n",
           " * and their cycles are const (flash); m68k_set_cpu_type() copies the running\n",
           " * one into m68ki_handlers[] / m68ki_handler_cycles[] (SRAM).  The opcode ->\n",
           " * handler index is the same for every CPU type (a handler that only a 68020\n",
           " * has raises the illegal-instruction exception on a 68000 itself). */\n",
           "#ifdef M68KI_FIXED_EC020\n#define M68KI_SET(x) x##_020\n#else\n#define M68KI_SET(x) x##_000\n#endif\n",
           "void (*const M68KI_SET(m68ki_handlers)[%d])(void) = {\n" % len(used)]
    for k in range(0, len(used), 4):
        out.append("\t" + ", ".join(used[k:k + 4]) + ",\n")
    out.append("};\n\nconst unsigned char M68KI_SET(m68ki_handler_cycles)[%d] = {\n#ifdef M68KI_FIXED_EC020\n" % len(used))
    for k in range(0, len(used), 32):
        out.append("\t" + ",".join(str(v) for v in hcyc20[k:k + 32]) + ",\n")
    out.append("#else\n")
    for k in range(0, len(used), 32):
        out.append("\t" + ",".join(str(v) for v in hcyc[k:k + 32]) + ",\n")
    out.append("#endif\n};\n\n#ifdef M68KI_FIXED_EC020\nconst unsigned char m68ki_cycles_020[0x10000] = {\n")
    for k in range(0, 0x10000, 32):
        out.append("\t" + ",".join(str(v) for v in cycles20[k:k + 32]) + ",\n")
    out.append("};\n#else\n")
    out.append("const unsigned m68ki_handler_count = %d;\n" % len(used))
    out.append("void (*m68ki_handlers[%d])(void);\nunsigned char m68ki_handler_cycles[%d];\n\n" % (len(used), len(used)))
    out.append("const unsigned short m68ki_handler_index[0x10000] = {\n")
    for k in range(0, 0x10000, 16):
        out.append("\t" + ",".join(str(v) for v in index[k:k + 16]) + ",\n")
    out.append("};\n\nconst unsigned char m68ki_cycles_000[0x10000] = {\n")
    for k in range(0, 0x10000, 32):
        out.append("\t" + ",".join(str(v) for v in cycles[k:k + 32]) + ",\n")
    out.append("};\n#endif /* M68KI_FIXED_EC020 */\n#endif /* M68K_CONST_TABLES */\n\n")
    out.append(body[eof:])
    open(os.path.join(OUT, "m68kops.c"), "w").write("".join(out))
    open(os.path.join(OUT, "m68kops.h"), "w").write(hdr)
    open(os.path.join(OUT, "m68kops_020.c"), "w").write(
        "/* Generated by tools/gen_musashi.py - do not edit.  The handlers again, for\n"
        " * the 68EC020: m68kops.c with the CPU type a constant (m68kcpu.h). */\n"
        "#define M68KI_FIXED_EC020 1\n#include \"m68kops.c\"\n")
    print("m68kops.c: %d handlers referenced of %d rows, %d hot" % (len(used), n, len(hot)))


if __name__ == "__main__":
    main()
