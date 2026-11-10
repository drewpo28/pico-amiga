#!/usr/bin/env python3
"""tools/m68kbench: a function's executed ARM instructions by source line.
usage: byline.py ELF PROF FUNCTION [frames] [top N]   (counts are divided by frames)"""
import subprocess
import sys
from collections import defaultdict

elf, prof, fn = sys.argv[1:4]
frames = int(sys.argv[4]) if len(sys.argv) > 4 else 1
top = int(sys.argv[5]) if len(sys.argv) > 5 else 40
cnt = {}
for l in open(prof):
    a, c = l.split()
    cnt[int(a, 16)] = int(c)
dis = subprocess.run(['arm-none-eabi-objdump', '-d', '--no-show-raw-insn', '--disassemble=' + fn, elf],
                     capture_output=True, text=True).stdout
addrs = []
for l in dis.split('\n'):
    p = l.split(':', 1)
    try:
        a = int(p[0].strip(), 16)
    except ValueError:
        continue
    if cnt.get(a): addrs.append(a)
out = subprocess.run(['arm-none-eabi-addr2line', '-e', elf] + [hex(a) for a in addrs],
                     capture_output=True, text=True).stdout.split('\n')
by = defaultdict(int)
for a, l in zip(addrs, out):
    f, _, n = l.rpartition(':')
    by[(f.split('/')[-1], n.split(' ')[0])] += cnt[a]
tot = sum(by.values())
print('%s: %d instructions a frame' % (fn, tot // frames))
src = {}
for (f, n), c in sorted(by.items(), key=lambda x: -x[1])[:top]:
    text = ''
    try:
        if f not in src:
            p = subprocess.run(['find', '.', '-name', f, '-not', '-path', './build*'], capture_output=True, text=True).stdout.split('\n')[0]
            src[f] = open(p).read().split('\n') if p else []
        text = src[f][int(n) - 1].strip()[:90]
    except (ValueError, IndexError, OSError):
        pass
    print('%8d  %s:%s  %s' % (c // frames, f, n, text))
