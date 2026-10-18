"""Locate and print the front-end overlay table of an EoR THPS N64 port (THPS2/THPS3).

The loader (THPS3 func_8009AAE0, THPS2 func_80096E30) looks a screen name up in a table of 28-byte records:
  +0x00 name pointer          +0x04 ROM address of the code blob (0xB0xxxxxx or 0xB1xxxxxx, link-time layout)
  +0x08 blob size             +0x0C ROM address of the relocation stream
  +0x10 relocation size       +0x14, +0x18 cache-op range (0 in every record seen)
Link-time ROM addresses are relative to a link-time asset root 0xB00FF7A0 (THPS3: lui 0xB010; addiu -0x860) and are moved at load
by (link_root - real asset root word at 0x80011C7C); this tool derives that delta from the first record,
which must be the first byte after the asset tree + alignment, and checks every record against the ROM.

usage: python3 -I decomp/common/overlay_table.py --game thps3 baserom.z64 baserom.main.bin
Addresses/sizes only on stdout; nothing ROM-derived is written.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops it
import thpsgame  # noqa: E402


def find_table(g, img):
    """Scan the main image for >= 8 consecutive 28-byte records: (name ptr, 0xB0 rom, size, 0xB0 rom, size, 0, 0)."""
    n = len(img) // 4
    w = struct.unpack_from(f">{n}I", img, 0)
    lo, hi = g.main_vram, g.main_vram + len(img)
    best = None
    for i in range(n - 7 * 8):
        run = 0
        while i + 7 * (run + 1) <= n:
            r = w[i + 7 * run:i + 7 * run + 7]
            if not (lo <= r[0] < hi and r[1] >> 24 in (0xB0, 0xB1) and r[3] >> 24 in (0xB0, 0xB1) and r[5] == 0 and r[6] == 0
                    and 0 < r[2] < 0x100000 and 0 < r[4] < 0x100000):
                break
            run += 1
        if run >= 8 and (best is None or run > best[1]):
            best = (i, run)
    return best and (g.main_vram + best[0] * 4, best[1])


def main():
    g, args = thpsgame.from_argv(sys.argv[1:])
    rom = open(args[0], "rb").read()
    img = open(args[1], "rb").read()
    found = find_table(g, img)
    if not found:
        raise SystemExit("no overlay table found")
    tab, n = found
    root_word = struct.unpack_from(">I", rom, g.root_ptr)[0]
    print(f"{g.name}: overlay table at 0x{tab:08X}, {n} records, asset root word 0x{root_word:08X}")
    off = tab - g.main_vram
    recs = [struct.unpack_from(">7I", img, off + 28 * k) for k in range(n)]

    def name(p):
        j = p - g.main_vram
        return img[j:img.index(b"\0", j)].decode("latin1")

    # first blob = first non-zero byte after the asset tree
    root = root_word & 0x0FFFFFFF
    cnt = struct.unpack_from(">I", rom, root)[0]
    tree_end = root + struct.unpack_from(f">{cnt + 1}I", rom, root + 4)[-1]
    first = tree_end
    while rom[first] == 0:
        first += 1
    delta = (recs[0][1] - 0xB0000000) - first
    print(f"first blob at ROM 0x{first:X} (tree end 0x{tree_end:X}); link-time root = 0x{(root + delta):X}  delta 0x{delta:X}")
    end = 0
    for k, r in enumerate(recs):
        code, size, rel, rsize = (r[1] - 0xB0000000) - delta, r[2], (r[3] - 0xB0000000) - delta, r[4]
        end = rel + rsize
        print(f"{k:2d} {name(r[0]):22s} code 0x{code:06X}+0x{size:X}  reloc 0x{rel:06X}+0x{rsize:X}"
              f"  {'contiguous' if rel == code + size else 'GAP'}")
    print(f"last record ends at ROM 0x{end:X}; last non-zero ROM byte ends at 0x{len(rom.rstrip(bytes(1))):X}")


if __name__ == "__main__":
    main()
