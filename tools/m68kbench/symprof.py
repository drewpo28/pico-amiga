#!/usr/bin/env python3
"""tools/m68kbench: fold prof.c's per-address counts by symbol.
usage: symprof.py ELF PROF [M68K instructions] [top N]
Groups: the dispatch loop (m68k_execute, m68ki_run_*), the opcode handlers,
memory access (the m68kf_* accessors and mem.c's callbacks), the rest of
Musashi, the chipset; then the top N functions."""
import bisect
import subprocess
import sys

elf, prof = sys.argv[1], sys.argv[2]
m68k = int(sys.argv[3]) if len(sys.argv) > 3 else 0
top = int(sys.argv[4]) if len(sys.argv) > 4 else 40
nm = subprocess.run(['arm-none-eabi-nm', '-n', '--defined-only', elf], capture_output=True, text=True).stdout.split('\n')
syms = []
for l in nm:
    p = l.split(' ', 2)
    if len(p) == 3 and p[1] in 'TtWw' and not p[2].startswith('$'):
        syms.append((int(p[0], 16) & ~1, p[2]))
syms.sort()
addrs = [a for a, _ in syms]
agg = {}
tot = 0
for l in open(prof):
    a, c = l.split()
    a = int(a, 16); c = int(c); tot += c
    i = bisect.bisect_right(addrs, a) - 1
    n = syms[i][1] if i >= 0 else '?'
    agg[n] = agg.get(n, 0) + c


def group(n):
    if n.startswith('m68k_op_'): return 'handlers'
    if n in ('m68k_execute', 'm68ki_loop') or n.startswith('m68ki_run'): return 'dispatch loop'
    if n.startswith(('m68k_read_memory', 'm68k_write_memory', 'm68kf_', 'm68k_fast_')) or n == 'm68ki_fetch_slow16': return 'memory access'
    if n.startswith(('m68ki_', 'm68k_')): return 'musashi other'
    if n == 'amiga_crc32': return 'bench'
    return 'chipset'


groups = {}
for n, c in agg.items():
    groups[group(n)] = groups.get(group(n), 0) + c
cpu = sum(c for n, c in groups.items() if n not in ('chipset', 'bench'))
print('ARM instructions %d' % tot + (', CPU emulation %.1f per 68k instruction (%d)' % (cpu / m68k, m68k) if m68k else ''))
for n, c in sorted(groups.items(), key=lambda x: -x[1]):
    print('%6.2f%% %12d %-14s' % (100 * c / tot, c, n) + ('  %6.1f / instr' % (c / m68k) if m68k else ''))
print()
for n, c in sorted(agg.items(), key=lambda x: -x[1])[:top]:
    print('%6.2f%% %12d %s' % (100 * c / tot, c, n) + ('  %6.2f' % (c / m68k) if m68k else ''))
