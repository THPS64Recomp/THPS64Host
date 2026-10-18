"""Decode and sanity-check the relocation streams of the front-end overlays (THPS2/THPS3).

Stream format (from the loader's applier, THPS3 func_8009AD20; word = big-endian u32):
  0xFFFFFFFF            end of stream
  w, type = w & 3, site = w & ~3 (byte offset in the overlay blob)
    type 0  R_32    *(u32*)site += base                       (data pointer; the word holds a blob offset)
    type 1  HI16    next word = full 32-bit addend A; imm16(site) = (base + A + 0x8000) >> 16   (lui)
    type 2  LO16    next word = A; imm16(site) = (base + A) & 0xFFFF                              (addiu/ori/lw/sw...)
    type 3  26      *(u32*)site += (base & 0x0FFFFFFF) >> 2   (j/jal with a blob-relative target)
Checks: stream length matches the record, every site lies in the blob, sites of type 1 are `lui`, type 3 are j/jal.

usage: python3 -I decomp/common/overlay_relocs.py --game thps3 baserom.z64 baserom.main.bin
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops it
import overlay_table  # noqa: E402
import thpsgame  # noqa: E402


def records(g, rom, img):
    tab, n = overlay_table.find_table(g, img)
    root = struct.unpack_from(">I", rom, g.root_ptr)[0] & 0x0FFFFFFF
    cnt = struct.unpack_from(">I", rom, root)[0]
    first = root + struct.unpack_from(f">{cnt + 1}I", rom, root + 4)[-1]
    while rom[first] == 0:
        first += 1
    recs = [struct.unpack_from(">7I", img, tab - g.main_vram + 28 * k) for k in range(n)]
    delta = (recs[0][1] - 0xB0000000) - first
    out = []
    for r in recs:
        j = r[0] - g.main_vram
        name = img[j:img.index(b"\0", j)].decode("latin1")
        out.append((name, (r[1] - 0xB0000000) - delta, r[2], (r[3] - 0xB0000000) - delta, r[4]))
    return out


def main():
    g, args = thpsgame.from_argv(sys.argv[1:])
    rom, img = open(args[0], "rb").read(), open(args[1], "rb").read()
    tot = collections.Counter()
    for name, code, size, rel, rsize in records(g, rom, img):
        blob, st = rom[code:code + size], rom[rel:rel + rsize]
        i, c, bad = 0, collections.Counter(), []
        while True:
            (w,) = struct.unpack_from(">I", st, i)
            i += 4
            if w == 0xFFFFFFFF:
                break
            t, site = w & 3, w & ~3
            if t in (1, 2):
                i += 4
            c[t] += 1
            if site + 4 > size:
                bad.append(("out of blob", hex(site)))
                continue
            (ins,) = struct.unpack_from(">I", blob, site)
            op = ins >> 26
            if t == 1 and op != 0x0F:
                bad.append(("HI16 not lui", hex(site)))
            if t == 3 and op not in (2, 3):
                bad.append(("26 not j/jal", hex(site)))
        tot.update(c)
        print(f"{name:22s} blob 0x{size:6X} stream 0x{rsize:5X} ({'ends exactly' if i == rsize else f'ends at 0x{i:X}'})"
              f"  R32 {c[0]:4d} HI16 {c[1]:4d} LO16 {c[2]:4d} J26 {c[3]:4d}  anomalies {len(bad)}")
        for b in bad[:3]:
            print("    ", b)
    print("total", dict(tot))


if __name__ == "__main__":
    main()
