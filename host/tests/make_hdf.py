#!/usr/bin/env python3
"""Build an Amiga HDF holding one empty FFS (DOS\\1) partition.

  make_hdf.py out.hdf [--mb N] [--name NAME] [--rdb]

Without --rdb: a bare partition (WinUAE's partition HDF: 32 blocks per track,
one surface, 2 reserved blocks), as the IDE drive (src/amiga/ide.c) puts a
virtual RDB in front of.  With --rdb: a whole drive, RDSK at block 0, PART
at block 1 (DH0, bootable), the partition from cylinder 1 of a 1 x 32
geometry.  Block 1 of the partition (its second reserved block, unused by
the filesystem) starts with "MARK": smoke.sh's IDE test reads it."""
import struct, sys

def be(*v): return struct.pack(">%dI" % len(v), *[x & 0xFFFFFFFF for x in v])

def checksum(block, at):                    # longs add up to 0, the sum stored at `at`
    struct.pack_into(">I", block, at, 0)
    s = sum(struct.unpack(">128I", block)) & 0xFFFFFFFF
    struct.pack_into(">I", block, at, (-s) & 0xFFFFFFFF)

def ffs(blocks, name):
    """An empty FFS volume of `blocks` 512-byte blocks, 2 reserved."""
    img = bytearray(blocks * 512)
    img[0:4] = b"DOS\x01"
    img[512:516] = b"MARK"
    reserved = 2
    root = (blocks - 1 + reserved) // 2
    nbits = blocks - reserved
    nbm = (nbits + 127 * 32 - 1) // (127 * 32)
    assert nbm <= 25, "no bitmap extension blocks: keep it under ~50 MB"
    bm0 = root + 1
    used = {root} | {bm0 + i for i in range(nbm)}
    for i in range(nbm):
        b = bytearray(512)
        for j in range(127):
            word = 0
            for k in range(32):
                n = i * 127 * 32 + j * 32 + k
                if n < nbits and (n + reserved) not in used:
                    word |= 1 << k
            struct.pack_into(">I", b, 4 + 4 * j, word)
        checksum(b, 0)
        img[(bm0 + i) * 512:(bm0 + i + 1) * 512] = b
    r = bytearray(512)
    struct.pack_into(">I", r, 0x000, 2)                     # T_HEADER
    struct.pack_into(">I", r, 0x00C, 72)                    # hash table size
    struct.pack_into(">I", r, 0x138, 0xFFFFFFFF)            # bitmap valid
    for i in range(nbm):
        struct.pack_into(">I", r, 0x13C + 4 * i, bm0 + i)
    days = 17000                                            # 2016-07-18
    for off in (0x1A4, 0x1D8, 0x1E4):
        struct.pack_into(">III", r, off, days, 0, 0)
    n = name.encode()[:30]
    r[0x1B0] = len(n)
    r[0x1B1:0x1B1 + len(n)] = n
    struct.pack_into(">I", r, 0x1FC, 1)                     # ST_ROOT
    checksum(r, 0x014)
    img[root * 512:(root + 1) * 512] = r
    return img

def rdb_blocks(cyls, high_cyl, dos_type):
    rdsk = bytearray(512)
    rdsk[0:4] = b"RDSK"
    struct.pack_into(">IIII", rdsk, 4, 64, 0, 7, 512)
    struct.pack_into(">I", rdsk, 20, 0x02)
    struct.pack_into(">IIII", rdsk, 24, 0xFFFFFFFF, 1, 0xFFFFFFFF, 0xFFFFFFFF)
    for i in range(6): struct.pack_into(">I", rdsk, 40 + 4 * i, 0xFFFFFFFF)
    struct.pack_into(">IIIII", rdsk, 64, cyls, 32, 1, 1, cyls)
    struct.pack_into(">III", rdsk, 96, cyls, cyls, 3)
    struct.pack_into(">IIIII", rdsk, 128, 0, 31, 1, cyls - 1, 32)
    struct.pack_into(">I", rdsk, 152, 1)
    rdsk[160:188] = b"TESTHDF " + b"RDB IMAGE       " + b"1.0 "
    checksum_rdb(rdsk)
    part = bytearray(512)
    part[0:4] = b"PART"
    struct.pack_into(">IIIII", part, 4, 64, 0, 7, 0xFFFFFFFF, 1)
    part[36] = 3; part[37:40] = b"DH0"
    env = [16, 128, 0, 1, 1, 32, 2, 0, 0, 1, high_cyl, 30, 0, 0x1FE00, 0x7FFFFFFE, 0, dos_type]
    struct.pack_into(">17I", part, 128, *[x & 0xFFFFFFFF for x in env])
    checksum_rdb(part)
    return rdsk, part

def checksum_rdb(b):                         # the first 64 longs add up to 0
    struct.pack_into(">I", b, 8, 0)
    s = sum(struct.unpack(">64I", b[:256])) & 0xFFFFFFFF
    struct.pack_into(">I", b, 8, (-s) & 0xFFFFFFFF)

def main():
    args = sys.argv[1:]
    out = args[0]
    mb = int(args[args.index("--mb") + 1]) if "--mb" in args else 4
    name = args[args.index("--name") + 1] if "--name" in args else "TestHD"
    blocks = mb * 2048
    if "--rdb" in args:
        cyls = blocks // 32
        part = ffs((cyls - 1) * 32, name)
        rdsk, pb = rdb_blocks(cyls, cyls - 1, 0x444F5301)
        img = bytearray(32 * 512)
        img[0:512] = rdsk
        img[512:1024] = pb
        img += part
    else:
        img = ffs(blocks, name)
    open(out, "wb").write(img)

main()
