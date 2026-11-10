#!/usr/bin/env python3
"""tools/m68kbench: a function's disassembly with prof.c's execution counts.
usage: annotate.py ELF PROF FUNCTION [M68K instructions]"""
import subprocess
import sys

elf, prof, fn = sys.argv[1:4]
m68k = int(sys.argv[4]) if len(sys.argv) > 4 else 0
cnt = {}
for l in open(prof):
    a, c = l.split()
    cnt[int(a, 16)] = int(c)
dis = subprocess.run(['arm-none-eabi-objdump', '-d', '--no-show-raw-insn', '--disassemble=' + fn, elf],
                     capture_output=True, text=True).stdout
for l in dis.split('\n'):
    p = l.split(':', 1)
    try:
        a = int(p[0].strip(), 16)
    except ValueError:
        continue
    c = cnt.get(a, 0)
    print(('%9.2f' % (c / m68k) if m68k else '%10d' % c) if c else ' ' * (9 if m68k else 10), l)
