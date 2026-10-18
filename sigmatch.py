"""Masked-signature function matcher between two EoR THPS N64 builds.

Seeds symbol names and function boundaries for a new game from a game whose
symbols are known (THPS1 -> THPS2, THPS2 -> THPS3, ...). No external deps.

Method
  1. For every source function (from the source game's recomp/<game>.syms.toml,
     size >= --min-size) build its *masked* word sequence: `j/jal` targets, the
     16-bit immediate of ALU-immediate ops (addi..lui) and of loads/stores are
     zeroed, so relocated addresses and moved struct offsets don't matter. Branch
     offsets and register fields stay.
  2. Index the destination image by the first 8 masked words at every word.
     A source function matches where the *whole* masked body is equal.
  tier A  body unique in the source list AND exactly one hit in the destination.
  tier O  several identical bodies: take the candidate lying between the
          destination addresses of the neighbouring tier-A anchors (link order is
          mostly preserved), only if exactly one fits.
  tier C  call-graph: for a matched pair whose `jal`s sit at the same word indexes,
          callee -> callee (iterated to a fixpoint; source not yet matched). The two
          callee bodies must still agree on >= --min-sim of their masked words, which
          is what separates real "modified function" hits from coincidences (without
          that filter about half of tier C was wrong on THPS1 -> THPS2).
  Boundaries come out of the match: dst start = hit address, size = source size
  (for tier C the callee size is the source's, flagged as unverified).

Output
  --json FILE      all matches: {src_addr, name, dst_addr, size, tier}
  --symbols FILE   splat-format lines `name = 0xADDR; // type:func size:0x.. sigmatch:A`
                   for named source functions (func_XXXXXXXX placeholders skipped)

usage:
  python3 -I decomp/common/sigmatch.py SRC_GAME SRC_ROM SRC_MAIN SRC_SYMS_TOML \\
        DST_GAME DST_ROM DST_MAIN [--min-size 0x20] [--json out.json] [--symbols out.txt]
"""
import argparse
import json
import os
import re
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops it
import thpsgame  # noqa: E402

FUNC = re.compile(r'\{\s*name\s*=\s*"([^"]+)"\s*,\s*vram\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*size\s*=\s*(0x[0-9A-Fa-f]+)')
K = 8   # words hashed for the index


def mask_word(w):
    op = w >> 26
    if op in (2, 3):
        return w & 0xFC000000
    if 0x08 <= op <= 0x0F or 0x20 <= op <= 0x2E or 0x30 <= op <= 0x3F:
        return w & 0xFFFF0000
    return w


class Region:
    def __init__(self, data, vram):
        n = len(data) // 4
        self.vram, self.n = vram, n
        self.words = struct.unpack(f">{n}I", data[:n * 4])
        self.masked = [mask_word(w) for w in self.words]

    def idx(self, a):
        return (a - self.vram) // 4

    def contains(self, a, size=4):
        return self.vram <= a and a + size <= self.vram + self.n * 4


def load_regions(g, rom_path, main_path):
    rom = open(rom_path, "rb").read()
    boot = Region(rom[g.boot_rom:g.boot_rom + g.boot_size], g.boot_vram)
    main = Region(open(main_path, "rb").read(), g.main_vram)
    return {"boot": boot, "main": main}


def parse_syms(path):
    return sorted((int(a, 16), n, int(s, 16)) for n, a, s in FUNC.findall(open(path).read()))


def jal_slots(reg, a, size):
    """[(word index within the function, target)] for every jal."""
    out = []
    i0 = reg.idx(a)
    for k in range(size // 4):
        w = reg.words[i0 + k]
        if w >> 26 == 3:
            out.append((k, 0x80000000 | ((w & 0x03FFFFFF) << 2)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src_game"); ap.add_argument("src_rom"); ap.add_argument("src_main"); ap.add_argument("src_syms")
    ap.add_argument("dst_game"); ap.add_argument("dst_rom"); ap.add_argument("dst_main")
    ap.add_argument("--min-size", type=lambda x: int(x, 0), default=0x20)
    ap.add_argument("--json"); ap.add_argument("--symbols")
    ap.add_argument("--no-propagate", action="store_true")
    ap.add_argument("--dst-syms", help="destination syms.toml: enables tier F (fuzzy)")
    ap.add_argument("--fuzzy-min", type=float, default=0.55)
    ap.add_argument("--min-sim", type=float, default=0.5,
                    help="tier C only: fraction of identical masked words between the two bodies")
    a = ap.parse_args()

    sg, dg = thpsgame.load(a.src_game), thpsgame.load(a.dst_game)
    sreg = load_regions(sg, a.src_rom, a.src_main)
    dreg = load_regions(dg, a.dst_rom, a.dst_main)
    funcs = parse_syms(a.src_syms)

    def region_of(reg_map, g, addr):
        return "main" if addr >= g.main_vram else "boot"

    # destination index (per region): hash of first K masked words -> [index]
    dindex = {}
    for rn, r in dreg.items():
        d = defaultdict(list)
        for i in range(r.n - K):
            d[tuple(r.masked[i:i + K])].append(i)
        dindex[rn] = d

    # source bodies
    body, groups = {}, defaultdict(list)
    for addr, name, size in funcs:
        rn = region_of(sreg, sg, addr)
        r = sreg[rn]
        if size < a.min_size or not r.contains(addr, size):
            continue
        i0 = r.idx(addr)
        b = tuple(r.masked[i0:i0 + size // 4])
        body[addr] = (rn, name, size, b)
        groups[(rn, b)].append(addr)

    n_src = {rn: sum(1 for v in body.values() if v[0] == rn) for rn in ("boot", "main")}

    short_idx = {}     # (region, n words) -> {tuple: [index]} for bodies shorter than K words

    def dst_hits(rn, b):
        r, hits = dreg[rn], []
        if len(b) < K:
            key = (rn, len(b))
            if key not in short_idx:
                d = defaultdict(list)
                for i in range(r.n - len(b) + 1):
                    d[tuple(r.masked[i:i + len(b)])].append(i)
                short_idx[key] = d
            return [r.vram + 4 * i for i in short_idx[key].get(b, ())]
        for i in dindex[rn].get(b[:K], ()):
            if tuple(r.masked[i:i + len(b)]) == b:
                hits.append(dreg[rn].vram + 4 * i)
        return hits

    match = {}      # src addr -> (dst addr, tier)
    amb = []        # (src addr, [candidates])
    for addr, (rn, name, size, b) in body.items():
        hits = dst_hits(rn, b)
        if len(groups[(rn, b)]) == 1 and len(hits) == 1:
            match[addr] = (hits[0], "A")
        elif hits:
            amb.append((addr, hits))

    # drop A matches that collide on one destination address
    cnt = defaultdict(int)
    for s, (d, t) in match.items():
        cnt[d] += 1
    for s in [s for s, (d, t) in match.items() if cnt[d] > 1]:
        del match[s]

    # tier O: order-based resolution of identical bodies
    sorted_all = sorted(body)
    anchors = sorted((s, d) for s, (d, t) in match.items())
    import bisect
    ak = [s for s, _ in anchors]
    for addr, hits in sorted(amb):
        k = bisect.bisect_left(ak, addr)
        lo = anchors[k - 1][1] if k > 0 else -1
        hi = anchors[k][1] if k < len(anchors) else 1 << 40
        cand = [h for h in hits if lo < h < hi]
        taken = {d for d, _ in match.values()}
        cand = [h for h in cand if h not in taken]
        # identical sibling sources between the same anchors must not claim the same slot
        sibs = [s for s, hs in amb if bisect.bisect_left(ak, s) == k and body[s][3] == body[addr][3]]
        if len(cand) == 1 and len(sibs) == 1:
            match[addr] = (cand[0], "O")

    # tier C: callee propagation along matched pairs
    by_addr = {f[0]: f for f in funcs}
    if not a.no_propagate:
        changed = True
        while changed:
            changed = False
            taken = {d for d, _ in match.values()}
            for s, (d, t) in list(match.items()):
                rn, name, size, b = body[s]
                ss = jal_slots(sreg[rn], s, size)
                ds = jal_slots(dreg[rn], d, size)
                if len(ss) != len(ds) or [k for k, _ in ss] != [k for k, _ in ds]:
                    continue
                for (k, st), (_, dt) in zip(ss, ds):
                    ent = by_addr.get(st)
                    if st in match or ent is None or dt in taken:
                        continue
                    srn = region_of(sreg, sg, st)
                    drn = "main" if dt >= dg.main_vram else "boot"
                    if srn != drn or not dreg[drn].contains(dt, 8):
                        continue
                    n = min(ent[2] // 4, dreg[drn].n - dreg[drn].idx(dt))
                    sw = sreg[srn].masked[sreg[srn].idx(st):][:n]
                    dw = dreg[drn].masked[dreg[drn].idx(dt):][:n]
                    if sum(1 for p, q in zip(sw, dw) if p == q) < a.min_sim * n:
                        continue
                    match[st] = (dt, "C")
                    body.setdefault(st, (srn, ent[1], ent[2], ()))
                    taken.add(dt)
                    changed = True


    # tier F: fuzzy containment of 4-word shingles, destination boundaries from --dst-syms
    if a.dst_syms:
        SH = 4
        dfuncs = parse_syms(a.dst_syms)
        def shingles(reg, addr, size):
            i0 = reg.idx(addr)
            m = reg.masked[i0:i0 + size // 4]
            return {tuple(m[k:k + SH]) for k in range(len(m) - SH + 1)}
        taken_dst = {d for d, _ in match.values()}
        dsh = {}
        inv = defaultdict(list)
        for da, dn, dsz in dfuncs:
            drn = region_of(dreg, dg, da)
            if dsz < a.min_size or da in taken_dst or not dreg[drn].contains(da, dsz):
                continue
            sh = shingles(dreg[drn], da, dsz)
            dsh[da] = (sh, dsz)
            for t in sh:
                inv[t].append(da)
        # drop shingles that are everywhere (compiler idioms)
        common = {t for t, v in inv.items() if len(v) > 25}
        cands = {}
        for addr, (rn, name, size, b) in body.items():
            if addr in match:
                continue
            ssh = shingles(sreg[rn], addr, size) - common
            if len(ssh) < 6:
                continue
            votes = defaultdict(int)
            for t in ssh:
                for da in inv.get(t, ()):
                    votes[da] += 1
            if not votes:
                continue
            rank = sorted(votes.items(), key=lambda kv: -kv[1])
            (d1, v1) = rank[0]
            v2 = rank[1][1] if len(rank) > 1 else 0
            dsz = dsh[d1][1]
            if v1 >= a.fuzzy_min * len(ssh) and v1 >= 1.5 * v2 and 0.5 <= dsz / size <= 2.0 \
                    and region_of(dreg, dg, d1) == rn:
                cands[addr] = (d1, v1 / len(ssh))
        back = defaultdict(list)
        for s_, (d_, sc_) in cands.items():
            back[d_].append((sc_, s_))
        for s_, (d_, sc_) in cands.items():
            if len(back[d_]) == 1:               # mutual: one source claims this destination
                match[s_] = (d_, "F")

    # report
    tiers = defaultdict(int)
    for s, (d, t) in match.items():
        tiers[t] += 1
    named = lambda s: not body[s][1].startswith("func_")
    print(f"source functions >= {a.min_size:#x}: boot {n_src['boot']}, main {n_src['main']}")
    for rn in ("boot", "main"):
        ms = [s for s in match if body[s][0] == rn]
        print(f"  {rn}: matched {len(ms)} (A {sum(1 for s in ms if match[s][1]=='A')}, "
              f"O {sum(1 for s in ms if match[s][1]=='O')}, C {sum(1 for s in ms if match[s][1]=='C')}, "
              f"F {sum(1 for s in ms if match[s][1]=='F')}); "
              f"named among them {sum(1 for s in ms if named(s))}")
    if a.json:
        json.dump([{"src": s, "name": body[s][1], "dst": d, "size": body[s][2], "tier": t}
                   for s, (d, t) in sorted(match.items())], open(a.json, "w"), indent=0)
    if a.symbols:
        with open(a.symbols, "w") as f:
            for s, (d, t) in sorted(match.items(), key=lambda kv: kv[1][0]):
                if named(s):
                    f.write(f"{body[s][1]} = 0x{d:08X}; // type:func size:0x{body[s][2]:X} sigmatch\n")


if __name__ == "__main__":
    main()
