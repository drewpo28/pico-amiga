#!/usr/bin/env python3
"""The hottest Musashi handlers from the bench's handler histograms, as a hot list
for tools/gen_musashi.py (tools/musashi_hot.txt / musashi_hot_020.txt).

  tools/musashi_hist.py [--top N] [--bytes B --elf ELF] HIST...

A HIST file is what `pico-amiga-host --hist file` writes: one "index count"
line per handler run (the index into m68ki_handlers[], as gen/m68kops.c lays
them out).  Each workload is normalised to 1 and the workloads summed, so a
long run does not outweigh a short one.  --top N prints the N hottest names;
--bytes B with --elf prints as many as fit B bytes of SRAM, taking each
handler's size from the ELF (arm-none-eabi-nm -S; a handler compiled twice is
sized by its 68EC020 copy, the larger)."""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def handler_names():
    """Index -> handler name, from the generated m68ki_handlers_000[] initializer."""
    text = open(os.path.join(ROOT, "external", "musashi", "gen", "m68kops.c")).read()
    m = re.search(r"m68ki_handlers\)\[\d+\]\)\(void\) = \{(.*?)\};", text, re.S)
    if not m:
        sys.exit("gen/m68kops.c: no handler table (run tools/gen_musashi.py)")
    return [n.strip() for n in m.group(1).replace("\n", " ").split(",") if n.strip()]


def sizes(elf):
    out = subprocess.run(["arm-none-eabi-nm", "-S", elf], capture_output=True, text=True).stdout
    sz = {}
    for line in out.split("\n"):
        p = line.split()
        if len(p) == 4 and p[3].startswith("m68k_op_"):
            sz[p[3]] = max(sz.get(p[3], 0), int(p[1], 16))
    return sz


def main():
    args = sys.argv[1:]
    opt = lambda k, d=None: args[args.index(k) + 1] if k in args else d
    top, budget, elf = int(opt("--top", 0)), int(opt("--bytes", 0)), opt("--elf")
    files = [a for i, a in enumerate(args) if not a.startswith("--") and (i == 0 or not args[i - 1].startswith("--"))]
    if not files:
        sys.exit(__doc__)
    names = handler_names()
    score = [0.0] * len(names)
    for f in files:
        counts = {}
        for line in open(f):
            p = line.split()
            if len(p) == 2:
                counts[int(p[0])] = counts.get(int(p[0]), 0) + int(p[1])
        total = sum(counts.values()) or 1
        for i, c in counts.items():
            if 0 < i < len(score):               # 0 = illegal
                score[i] += c / total
    order = sorted(range(1, len(names)), key=lambda i: -score[i])
    sz = sizes(elf) if budget and elf else {}
    used, kept = 0, []
    for i in order:
        if score[i] <= 0:
            break
        if budget:
            s = sz.get(names[i], 0)
            if used + s > budget:
                continue
            used += s
        kept.append(i)
        if top and len(kept) >= top:
            break
    covered = sum(score[i] for i in kept) / len(files)
    print("# %d handlers, %.1f%% of the instructions run%s" % (len(kept), 100 * covered,
          ", %d bytes" % used if budget else ""))
    for i in kept:
        print(names[i])


if __name__ == "__main__":
    main()
