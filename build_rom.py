"""Splice a main image (built from asm) back into a ROM as ERZ chunks.

Input ROM: the ROM linked from the boot asm + bins (build/thps1.z64). Its
original compressed chunks are *discarded*; the main image is cut into 64 KiB
pieces, each packed with tools/erz_pack.py, and a fresh chunk table is written
(4-byte-aligned spans, last chunk unpadded, then 0x00 fill to a 2-aligned asset
root, as in the retail ROM). If the packed size changes, the asset tree is moved
and its root pointer (the word at root_ptr, read by boot main) is updated, and the
header CRCs are recomputed (CIC-6102 algorithm).

usage: python -I decomp/common/build_rom.py --game thps2 RNC_BIN IN.z64 MAIN.bin OUT.z64

Per-game constants (asset-root word offset, chunk table, stride, ERZ method) come
from games.toml. The asset packs are copied from IN.z64 untouched, so per-piece
quirks (e.g. THPS2/3 pack 8's 8-byte piece, which rnc_propack cannot reproduce)
do not matter here; repack_check.py covers them.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops it
import erz_pack  # noqa: E402
import thpsgame  # noqa: E402


def n64_crc_6102(rom: bytes):
    seed = 0xF8CA4DDC
    t1 = t2 = t3 = t4 = t5 = t6 = seed
    M = 0xFFFFFFFF
    for i in range(0x1000, 0x101000, 4):
        d = struct.unpack_from(">I", rom, i)[0]
        if (t6 + d) & M < t6:
            t4 = (t4 + 1) & M
        t6 = (t6 + d) & M
        t3 ^= d
        r = ((d << (d & 0x1F)) | (d >> (32 - (d & 0x1F)))) & M if d & 0x1F else d
        t5 = (t5 + r) & M
        t2 = t2 ^ r if t2 > d else t2 ^ (t6 ^ d)
        t1 = (t1 + (t5 ^ d)) & M
    return (t6 ^ t4 ^ t3) & M, (t5 ^ t2 ^ t1) & M


def main(g, rnc, in_rom, main_bin, out_rom):
    ROOT_PTR, TABLE, STRIDE = g.root_ptr, g.table, g.stride
    rom = bytearray(open(in_rom, "rb").read())
    img = open(main_bin, "rb").read()
    old_root = struct.unpack_from(">I", rom, ROOT_PTR)[0] & 0x0FFFFFFF
    n_old = struct.unpack_from(">I", rom, TABLE)[0]
    # Everything from the asset root to the end of the ROM is carried over verbatim
    # (trailing zero fill is re-created). THPS2/3 keep a front-end code overlay *after*
    # the asset tree's last container (see the private development notes), so cutting at the
    # tree's end would drop it.
    assets = bytes(rom[old_root:]).rstrip(b"\0")

    pieces = [img[i:i + STRIDE] for i in range(0, len(img), STRIDE)]
    packed = [erz_pack.pack(rnc, p, g.code_method) for p in pieces]
    n = len(packed)
    hdr = 4 + 4 * (n + 1)
    offs, pos, blob = [], hdr, bytearray()
    for i, p in enumerate(packed):
        offs.append(pos)
        span = len(p) if i == n - 1 else (len(p) + 3) & ~3
        blob += p + b"\0" * (span - len(p))
        pos += span
    offs.append(pos)
    table = struct.pack(f">I{n + 1}I", n, *offs)
    body = table + blob
    root = TABLE + len(body)
    root += root & 1                       # assets are 2-byte aligned (PI DMA)
    out = bytearray(rom[:ROOT_PTR])
    out += struct.pack(">I", 0xB0000000 | root) + body
    # The pad byte before the asset root is stale buffer data in THPS3 (0xCD, not 0): keep the
    # original bytes when the layout did not change, so the header CRC (which covers it) matches.
    pad = bytes(rom[len(out):root]) if root == old_root else b"\0" * (root - len(out))
    out += pad + assets
    out += b"\0" * (len(rom) - len(out))
    if len(out) != len(rom):
        raise SystemExit("ROM grew past its original size")
    if n != n_old or root != old_root:
        print(f"note: layout changed (chunks {n_old}->{n}, asset root "
              f"{old_root:#x}->{root:#x}); not byte-identical")
    c1, c2 = n64_crc_6102(out)
    struct.pack_into(">II", out, 0x10, c1, c2)
    open(out_rom, "wb").write(out)
    print(f"{out_rom}: {n} ERZ chunks, {len(blob):#x} bytes packed, asset root {root:#x}")


if __name__ == "__main__":
    g, args = thpsgame.from_argv(sys.argv[1:])
    main(g, *args[:4])
