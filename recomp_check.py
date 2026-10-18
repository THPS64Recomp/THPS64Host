"""Static completeness checks on the function list N64Recomp will use (no emulator needed).

  1. coverage   every word of every text range is inside a listed function, or is
                inter-object padding (zeros, or an instruction no compiler emits)
  2. jr sites   non-$ra `jr` instructions (jump-table switches and a few libultra internals)
  3. data ptrs  word-aligned pointers stored in non-text memory that land in the middle of
                a listed function (hidden function starts, e.g. libmus F-command tables;
                jump-table case labels also show up and are listed separately)
  4. jalr       count of indirect call sites

usage: python3 -I decomp/common/recomp_check.py --game thps2 recomp/thps2.syms.toml ROM.z64 main.bin
"""
import bisect
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops it
import funclist as F  # noqa: E402
import thpsgame  # noqa: E402

FUNC = re.compile(r'\{\s*name\s*=\s*"([^"]+)"\s*,\s*vram\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*size\s*=\s*(0x[0-9A-Fa-f]+)')


def main(g, syms_p, rom_p, img_p):
    boot, main_, fb, fm, _ = F.load_all(rom_p, img_p, sym_paths=(), g=g)
    funcs = sorted((int(a, 16), int(s, 16), n) for n, a, s in FUNC.findall(open(syms_p).read()))
    starts = [f[0] for f in funcs]
    ends = {f[0]: f[0] + f[1] for f in funcs}
    print(f"{len(funcs)} functions in {syms_p}")
    for img, name in ((boot, "boot"), (main_, "main")):
        gaps, uncovered = [], 0
        for lo, hi in img.text:
            a = lo
            while a < hi:
                k = bisect.bisect_right(starts, a) - 1
                if k >= 0 and starts[k] <= a < ends[starts[k]]:
                    a = ends[starts[k]]
                    continue
                # a is in a gap: padding?
                nxt = starts[bisect.bisect_right(starts, a)] if bisect.bisect_right(starts, a) < len(starts) else hi
                b = min(nxt, hi)
                words = [img.w(x) for x in range(a, b, 4)]
                pad = all(w == 0 for w in words) or F.looks_like_padding(img, a, (b + 3) & ~3) if b - a < 16 else all(w == 0 for w in words)
                if not pad:
                    uncovered += b - a
                    gaps.append((a, b))
                a = b
        print(f"{name}: text words outside any function and not padding: {uncovered // 4}"
              + (f"  e.g. {[(hex(a), hex(b)) for a, b in gaps[:8]]}" if gaps else ""))
    # jr / jalr
    jr_sw, jalr = [], 0
    for img in (boot, main_):
        for lo, hi in img.text:
            for a in range(lo, hi, 4):
                w = img.w(a)
                if w >> 26 == 0 and (w & 63) == 8 and ((w >> 21) & 31) != 31:
                    jr_sw.append(a)
                if w >> 26 == 0 and (w & 63) == 9:
                    jalr += 1
    print(f"non-$ra jr: {len(jr_sw)}; jalr sites: {jalr}")
    # data pointers into function middles
    mid = []
    for img in (boot, main_):
        for i, w in enumerate(img.words):
            a = img.vram + 4 * i
            if img.in_text(a) or w % 4 or not (g.boot_vram <= w < g.main_end):
                continue
            if not any(lo <= w < hi for lo, hi in boot.text + main_.text):
                continue
            k = bisect.bisect_right(starts, w) - 1
            if k >= 0 and starts[k] != w and w < ends[starts[k]]:
                mid.append((a, w, funcs[k][2]))
    # runs of >=2 pointers within one function are jump tables, not hidden starts
    tbl, single = [], []
    for k_, (a, w, n) in enumerate(mid):
        prev = mid[k_ - 1] if k_ else None
        nxt = mid[k_ + 1] if k_ + 1 < len(mid) else None
        near = lambda x: x is not None and x[2] == n and abs(x[0] - a) == 4
        (tbl if near(prev) or near(nxt) else single).append((a, w, n))
    print(f"data pointers into a function middle: {len(mid)} ({len(tbl)} in runs = jump-table cases, "
          f"{len(single)} singles = candidate hidden starts)")
    for a, w, n in single[:40]:
        print(f"  data 0x{a:08X} -> 0x{w:08X} inside {n}")
    if len(single) > 40:
        print(f"  ... {len(single) - 40} more")


if __name__ == "__main__":
    g, args = thpsgame.from_argv(sys.argv[1:])
    main(g, *args[:3])
