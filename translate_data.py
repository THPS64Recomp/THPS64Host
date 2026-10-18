"""Translate a solved class's data addresses to a new class through sigmatch-matched function pairs.

For every matched pair (same masked body in both images) it pairs the `lui rX, hi` + load/store/addiu/ori
that build an address in the source function with the same instructions in the destination function, and
counts votes: source address -> destination address. Prints the best candidates for each name in
versions/<src class>/data_syms.toml [addr]; a name with no votes (or two close candidates) needs a
look at the code. Struct field offsets are not translated: compare the immediates in matched pairs.

Shared by the hosts: each game's recomp-host/tools/translate_data.py calls main(argv, host, default_src_class).
The source class's image is the decomp's baserom.main.bin when its game is the family root, else
build/<game>.main.bin.

usage: python3 -I tools/translate_data.py DST_CLASS [SRC_CLASS]   (default: the wrapper's, THPS1 us11)
Needs build/<dst game>.main.bin and build/<dst game>.sigmatch.json (tools/seed_class.py).
"""
import json
import os
import struct
import sys
import tomllib
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import thpsgame  # noqa: E402

MEM_OPS = (0x09, 0x0D, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B, 0x31, 0x35, 0x39, 0x3D)


def sx(v):
    return v - 0x10000 if v & 0x8000 else v


def main(argv, host, default_src):
    if not argv:
        sys.exit(__doc__)
    dst_cls, src_cls = argv[0], (argv[1] if len(argv) > 1 else default_src)
    HOST, GAMEDIR = host, os.path.dirname(host)
    with open(os.path.join(HOST, "versions", "classes.toml"), "rb") as f:
        classes = tomllib.load(f)
    sg, dg = thpsgame.load(classes[src_cls]["game"]), thpsgame.load(classes[dst_cls]["game"])
    src_main = os.path.join(GAMEDIR, "baserom.main.bin") if sg.name == sg.family else os.path.join(GAMEDIR, "build", sg.name + ".main.bin")
    src = open(src_main, "rb").read()
    dst = open(os.path.join(GAMEDIR, "build", dg.name + ".main.bin"), "rb").read()
    rows = json.load(open(os.path.join(GAMEDIR, "build", dg.name + ".sigmatch.json")))
    names = tomllib.load(open(os.path.join(HOST, "versions", src_cls, "data_syms.toml"), "rb"))["addr"]

    def w(d, base, a):
        return struct.unpack_from(">I", d, a - base)[0]

    votes = defaultdict(Counter)
    for r in rows:
        s, d, sz = r["src"], r["dst"], r["size"]
        if s < sg.main_vram:
            continue
        n = sz // 4
        for i in range(n):
            sw, dw = w(src, sg.main_vram, s + 4 * i), w(dst, dg.main_vram, d + 4 * i)
            if sw >> 26 != 0x0F or dw >> 26 != 0x0F or (sw >> 16) & 31 != (dw >> 16) & 31:
                continue
            reg = (sw >> 16) & 31
            for j in range(i + 1, min(n, i + 10)):
                sj, dj = w(src, sg.main_vram, s + 4 * j), w(dst, dg.main_vram, d + 4 * j)
                op = sj >> 26
                if (sj >> 21) & 31 != reg or op not in MEM_OPS or dj >> 26 != op or (dj >> 21) & 31 != reg:
                    continue
                lo = (lambda x: x & 0xFFFF) if op == 0x0D else (lambda x: sx(x & 0xFFFF))
                sa = (((sw & 0xFFFF) << 16) + lo(sj)) & 0xFFFFFFFF
                da = (((dw & 0xFFFF) << 16) + lo(dj)) & 0xFFFFFFFF
                votes[sa][da] += 1
    for name, a in names.items():
        v = votes.get(a)
        print(f"{name:24s} 0x{a:08X} -> " + (", ".join(f"0x{k:08X} x{c}" for k, c in v.most_common(3)) if v else "no votes"))
