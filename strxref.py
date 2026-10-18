"""Find a string in the main image and the functions that reference it (lui/addiu|ori|lw pairs).

usage: python3 -I decomp/common/strxref.py --game thps3 baserom.main.bin recomp/thps3.syms.toml "needle" [more needles...]
Prints each string occurrence (vram) and every function whose code forms its address
(`lui rt,%hi` followed within 8 instructions by `addiu/ori/lw/sw/lh.. rt,%lo`) - a cheap anchor for
hand-naming (e.g. a unique debug string -> the function using it). Pure stdlib.
"""
import bisect
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops it
import thpsgame  # noqa: E402

FUNC = re.compile(r'\{\s*name\s*=\s*"([^"]+)"\s*,\s*vram\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*size\s*=\s*(0x[0-9A-Fa-f]+)')


def sx(v):
    return v - 0x10000 if v & 0x8000 else v


def refs(img, base, lo, hi, targets):
    """Yield (pc, target) for hi/lo pairs forming any address in `targets`."""
    n = (hi - lo) // 4
    ws = struct.unpack_from(f">{n}I", img, lo - base)
    out = []
    for i, w in enumerate(ws):
        if w >> 26 != 0x0F:
            continue
        rt, imm = (w >> 16) & 31, w & 0xFFFF
        for j in range(i + 1, min(i + 9, n)):
            v = ws[j]
            op, rs, r2, im = v >> 26, (v >> 21) & 31, (v >> 16) & 31, v & 0xFFFF
            if rs != rt:
                continue
            if op in (0x09, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B, 0x31, 0x35, 0x39, 0x3D, 0x0D):
                a = ((imm << 16) + (sx(im) if op != 0x0D else im)) & 0xFFFFFFFF
                if a in targets:
                    out.append((lo + 4 * j, a))
            if r2 == rt and op in (0x09, 0x0D):
                break
    return out


def main():
    g, args = thpsgame.from_argv(sys.argv[1:])
    img = open(args[0], "rb").read()
    funcs = sorted((int(a, 16), n, int(s, 16)) for n, a, s in FUNC.findall(open(args[1]).read()))
    starts = [f[0] for f in funcs]
    for needle in args[2:]:
        nb = needle.encode()
        pos, hits = 0, []
        while (pos := img.find(nb, pos)) >= 0:
            start = pos
            while start > 0 and 0x20 <= img[start - 1] < 0x7F and pos - start < 64:
                start -= 1
            hits.append(g.main_vram + start)
            pos += 1
        print(f"== {needle!r}: {len(hits)} occurrence(s)")
        for h in hits:
            s = img[h - g.main_vram:img.index(b"\0", h - g.main_vram)].decode("latin1")
            users = {}
            for lo, hi in g.main_text:
                for pc, a in refs(img, g.main_vram, lo, hi, {h}):
                    k = bisect.bisect_right(starts, pc) - 1
                    users.setdefault(funcs[k][:2], []).append(pc)
            print(f"  0x{h:08X} {s[:70]!r}")
            for (a, n), pcs in users.items():
                print(f"      used by 0x{a:08X} {n} at " + ", ".join(f"0x{p:08X}" for p in pcs))


if __name__ == "__main__":
    main()
