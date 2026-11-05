#!/usr/bin/env python3
"""Sum the PC profiler's PROF lines (Debug > PC profiler, board.c) from a UART log
and name the hot code from the firmware's ELF.

  tools/pcprof.py LOG ELF [--mode off|x2|x2+8|x3|x3+8] [--from N] [--to N] [--top N]

Each second the firmware prints "PROF <samples> smp: ADDR:permille ..." (+ "PROF+"
continuation lines) after its PERF line.  The buckets are 64 bytes of code; this
weights each second by its sample count, sums by bucket and by function
(addr2line -f), and prints both, with where the code runs from (SRAM / flash).
--mode keeps the seconds whose PERF line has that A1200 load test; --from / --to
pick PERF lines by number (1-based)."""
import re, subprocess, sys
from collections import defaultdict

def addr2line():
    for p in ("/home/drew/.pico-sdk/toolchain/15_2_Rel1/bin/arm-none-eabi-addr2line",
              "arm-none-eabi-addr2line"):
        try:
            subprocess.run([p, "--version"], capture_output=True, check=True)
            return p
        except (OSError, subprocess.CalledProcessError):
            pass
    sys.exit("no arm-none-eabi-addr2line")

def mode_of(line):
    m = re.search(r"LOAD TEST cpu (x\d)( \+ 8 planes)?", line)
    return "off" if not m else m.group(1) + ("+8" if m.group(2) else "")

def region(a):
    if 0x10000000 <= a < 0x11000000: return "flash"
    if 0x20000000 <= a < 0x20082000: return "SRAM"
    return "?"

def main():
    args = sys.argv[1:]
    if len(args) < 2: sys.exit(__doc__)
    log, elf = args[0], args[1]
    opt = lambda k, d=None: args[args.index(k) + 1] if k in args else d
    want, lo, hi, top = opt("--mode"), int(opt("--from", 1)), int(opt("--to", 10**9)), int(opt("--top", 30))
    buckets = defaultdict(float)
    total = 0
    perf_n, mode, cur = 0, "off", None
    for raw in open(log, errors="replace"):
        line = raw.rstrip("\n")
        if "PERF fps" in line:
            perf_n += 1
            mode = mode_of(line)
            cur = None
            continue
        m = re.match(r".*PROF (\d+) smp:(.*)", line)
        m2 = re.match(r".*PROF\+(.*)", line) if not m else None
        if not (m or m2): continue
        if perf_n < lo or perf_n > hi or (want and mode != want):
            cur = None
            continue
        if m:
            cur = int(m.group(1))
            total += cur
            body = m.group(2)
        elif cur is None:
            continue
        else:
            body = m2.group(1)
        for a, pm in re.findall(r"([0-9A-F]{8}):(\d+)", body):
            buckets[int(a, 16)] += cur * int(pm) / 1000.0
    if not total: sys.exit("no PROF samples (Debug > PC profiler on, UART log on?)")
    addrs = sorted(buckets)
    a2l = subprocess.run([addr2line(), "-f", "-e", elf] + [hex(a) for a in addrs],
                         capture_output=True, text=True).stdout.split("\n")
    names = {a: a2l[2 * i] for i, a in enumerate(addrs)}
    funcs = defaultdict(float)
    for a, n in buckets.items(): funcs[(names[a], region(a))] += n
    named = sum(buckets.values())
    print(f"{total} samples, {100 * named / total:.0f}% in the listed buckets")
    print("\nby function:")
    for (f, r), n in sorted(funcs.items(), key=lambda x: -x[1])[:top]:
        print(f"  {100 * n / total:5.1f}%  {r:5}  {f}")
    print("\nby 64-byte bucket:")
    for a, n in sorted(buckets.items(), key=lambda x: -x[1])[:top]:
        print(f"  {100 * n / total:5.1f}%  {a:08X} {region(a):5}  {names[a]}")

main()
