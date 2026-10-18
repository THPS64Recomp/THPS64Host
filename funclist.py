"""Function list (start + size) for the boot segment and the main image.

Sources of function starts, merged:
  1. splat/spimdisasm `glabel`s in asm/ (jal targets, its own heuristics)
  2. jal targets from *both* images (main calls boot libultra and vice versa)
  3. code pointers stored in data (vtables, ctor list, callback tables),
     excluding splat `jlabel`s (jump-table cases)
  4. linear sweep: a `jr ra` with no branch or jump-table label beyond it ends
     a function; inter-object padding (zero or garbage) up to the next 16-byte
     boundary is skipped
  5. symbols/*.txt entries marked `type:func` (hand-curated, always win)
Sizes run to the last `jr ra` + delay slot before the next start (padding is
not part of a function).

Importable (see tools/gen_syms.py); CLI prints a summary.
usage: python decomp/common/funclist.py --game thps2 baserom.z64 baserom.main.bin

Per-game ranges come from games.toml via configure(g) (the module globals below
are THPS1's defaults until configured).
"""
import glob
import os
import re
import struct
import sys

import rabbitizer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops it
import thpsgame  # noqa: E402

JR_RA = 0x03E00008

BOOT_VRAM, BOOT_ROM, BOOT_END = 0x80000400, 0x1000, 0x80012F10
MAIN_VRAM = 0x80016AC0
# text ranges (vram); RSP ucode .text is excluded
BOOT_TEXT = [(0x80000400, 0x80000468), (0x80000870, 0x80011C10)]
MAIN_TEXT = [(0x800223C0, 0x80097C40), (0x8009B0A0, 0x800C0E20)]
CODE_HI = 0x800C0E20     # end of the last main text range (garbage-jump check)


def configure(g):
    """Point the module at a game from games.toml."""
    global BOOT_VRAM, BOOT_ROM, BOOT_END, MAIN_VRAM, BOOT_TEXT, MAIN_TEXT, CODE_HI
    BOOT_VRAM, BOOT_ROM, BOOT_END = g.boot_vram, g.boot_rom, g.boot_end
    MAIN_VRAM, BOOT_TEXT, MAIN_TEXT, CODE_HI = g.main_vram, g.boot_text, g.main_text, g.text_hi


class Image:
    def __init__(self, data, vram, text):
        self.data, self.vram, self.text = data, vram, text
        self.words = struct.unpack(f">{len(data) // 4}I", data[:len(data) // 4 * 4])

    def w(self, a):
        return self.words[(a - self.vram) // 4]

    def in_text(self, a):
        return any(lo <= a < hi for lo, hi in self.text)

    def contains(self, a):
        return self.vram <= a < self.vram + len(self.data)


def is_prologue(w):
    return (w >> 16) == 0x27BD and (w & 0x8000)  # addiu sp,sp,-N


def branch_target(w, a):
    op = w >> 26
    off = w & 0xFFFF
    off = off - 0x10000 if off & 0x8000 else off
    if op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or (op == 1 and ((w >> 16) & 31) in (0, 1, 2, 3, 0x10, 0x11, 0x12, 0x13)):
        return a + 4 + 4 * off
    if op == 0x11 and ((w >> 21) & 31) == 8:     # bc1f/bc1t(l)
        return a + 4 + 4 * off
    if op == 2:                                   # j
        return (a & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
    return None


# opcodes neither GCC 2.x (-mgp32) game code nor libultra emits at a function
# start; seeing one in a pre-alignment gap means garbage padding
NEVER_EMITTED = {"ll", "sc", "lld", "scd", "ldl", "ldr", "sdl", "sdr", "ld", "sd",
                 "lwc2", "swc2", "ldc2", "sdc2", "pref", "sync", "syscall",
                 "teq", "tne", "tge", "tgeu", "tlt", "tltu", "teqi", "tnei", "tgei",
                 "tgeiu", "tlti", "tltiu", "daddi", "daddiu", "dadd", "daddu",
                 "dsub", "dsubu", "dmult", "dmultu", "ddiv", "ddivu", "dsll", "dsrl",
                 "dsra", "dsll32", "dsrl32", "dsra32", "dsllv", "dsrlv", "dsrav",
                 "mfc2", "mtc2", "cfc2", "ctc2", "eret", "tlbp", "tlbr", "tlbwi",
                 "tlbwr", "cache", "lwu",
                 "bc2f", "bc2t"}


def looks_like_padding(img, n, al):
    """Words [n, al) before a 16-byte boundary: inter-object padding, or the
    start of a real function? This game's linker filled padding with garbage."""
    gap = [img.w(x) for x in range(n, al, 4)]
    if is_prologue(gap[0]):
        return False
    if all(w == 0 for w in gap):
        return True
    if len(gap) >= 2 and gap[-2] == JR_RA:
        return False                       # tiny real function
    for k, w in enumerate(gap):
        a = n + 4 * k
        ins = rabbitizer.Instruction(w, a)
        if not ins.isValid():
            return True
        mn = ins.getOpcodeName() if hasattr(ins, "getOpcodeName") else ins.disassemble().split()[0]
        if mn in NEVER_EMITTED:
            return True
        rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        if 26 in (rs, rt) or 27 in (rs, rt):   # $k0/$k1: never compiler output
            return True
        op = w >> 26
        if op in (2, 3):
            t = (a & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if not (BOOT_VRAM <= t < CODE_HI):
                return True
        t = branch_target(w, a)
        if t is not None and op not in (2, 3) and abs(t - a) > 0x4000:
            return True
    return False


def splat_labels(asm_glob):
    g, j = set(), set()
    for path in glob.glob(asm_glob, recursive=True):
        for line in open(path):
            # alabels are spimdisasm's guesses from `j` targets: in this game
            # they are mostly intra-function jumps, so they are not used
            m = re.match(r"\s*(glabel)\s+\w*?([0-9A-F]{8})\b", line)
            if m:
                g.add(int(m.group(2), 16))
            m = re.match(r"\s*jlabel\s+\.L([0-9A-F]{8})", line)
            if m:
                j.add(int(m.group(1), 16))
    return g, j


def curated(paths):
    """name -> addr for `name = 0xADDR; // type:func` lines (and all syms).
    A `size:0x..` on a curated function makes its whole range one function."""
    funcs, syms = {}, {}
    curated.sizes = {}
    for p in paths:
        try:
            lines = open(p).read().splitlines()
        except FileNotFoundError:
            continue
        for line in lines:
            m = re.match(r"\s*([A-Za-z_][\w.$]*)\s*=\s*(0x[0-9A-Fa-f]+)\s*;(.*)", line)
            if not m:
                continue
            name, addr, rest = m.group(1), int(m.group(2), 16), m.group(3)
            syms[addr] = name
            if "type:func" in rest:
                funcs[addr] = name
                sz = re.search(r"size:(0x[0-9A-Fa-f]+)", rest)
                if sz and "range" in rest:
                    curated.sizes[addr] = int(sz.group(1), 16)
    return funcs, syms


def discover(img, others, glabels, jlabels, forced):
    starts = {a for a in glabels | forced if img.in_text(a)}
    strong = {a for a in forced if img.in_text(a)}
    labels = {a for a in jlabels if img.in_text(a)}
    for src in [img] + others:
        for i, w in enumerate(src.words):
            a = src.vram + 4 * i
            if w >> 26 == 3 and src.in_text(a):
                t = 0x80000000 | ((w & 0x03FFFFFF) << 2)
                if img.in_text(t):
                    starts.add(t)
                    strong.add(t)
    for lo, _ in img.text:
        starts.add(lo)
        strong.add(lo)
    # jump tables: runs of >= 2 consecutive data words pointing into one
    # small text window -> case labels, never function starts
    run = []
    for i, w in enumerate(img.words + (0,)):
        a = img.vram + 4 * i
        if not img.in_text(a) and img.in_text(w) and w % 4 == 0:
            run.append(w)
            continue
        if len(run) >= 2 and max(run) - min(run) < 0x2000:
            labels |= set(run)
        run = []
    for i, w in enumerate(img.words):
        a = img.vram + 4 * i
        if not img.in_text(a) and img.in_text(w) and w % 4 == 0 and w not in labels:
            prev = img.w(w - 8) if img.in_text(w - 8) else JR_RA
            after_pad = w % 16 == 0 and any(img.in_text(w - k) and img.w(w - k) == JR_RA
                                            for k in (12, 16, 20))
            if prev == JR_RA or is_prologue(img.w(w)) or after_pad:
                starts.add(w)
                strong.add(w)
    # function pointers built in code: lui rX,%hi(f) ; addiu rY,rX,%lo(f)
    hi16 = {}
    for i, w in enumerate(img.words):
        a = img.vram + 4 * i
        if not img.in_text(a):
            continue
        op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 0x0F:
            hi16[rt] = ((w & 0xFFFF) << 16, a)
        elif op == 0x09 and rs in hi16 and a - hi16[rs][1] <= 32:
            imm = w & 0xFFFF
            t = (hi16[rs][0] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
            if img.in_text(t) and t % 4 == 0 and t not in labels:
                prev = img.w(t - 8) if img.in_text(t - 8) else JR_RA
                after_pad = t % 16 == 0 and any(img.in_text(t - k) and img.w(t - k) == JR_RA
                                                for k in (12, 16, 20))
                if prev == JR_RA or is_prologue(img.w(t)) or after_pad:
                    starts.add(t)
                    strong.add(t)
    for lo, hi in img.text:
        starts.add(lo)
        a, maxt = lo, lo
        while a < hi - 4:
            if a in strong:
                maxt = a        # nothing branches across a known function start
            w = img.w(a)
            t = branch_target(w, a)
            tail = w >> 26 == 2 and t is not None and img.in_text(t) and (
                t in strong or is_prologue(img.w(t)))
            if tail and t not in starts and lo <= t < hi:
                starts.add(t)                    # `j` to another function (tail call)
            if t is not None and lo <= t < hi and not tail and t - a < 0x4000:
                maxt = max(maxt, t)
            if w == JR_RA and maxt <= a + 4 and (a + 8) not in labels:
                n = a + 8
                al = (n + 15) & ~15
                if n % 16 and al <= hi and n not in strong and \
                        not any(x in labels for x in range(n, al, 4)) and \
                        looks_like_padding(img, n, al):
                    n = al
                if n < hi:
                    starts.add(n)
                maxt = n
                a = n
                continue
            a += 4
    # merge pass: a weak start (splat glabel / sweep) that some earlier
    # function branches across is not a real start
    changed = True
    while changed:
        changed = False
        ss = sorted(s for s in starts if img.in_text(s))
        for k, st in enumerate(ss[:-1]):
            nxt = ss[k + 1]
            if nxt in strong:
                continue
            end = ss[k + 2] if k + 2 < len(ss) else nxt + 0x10000
            body_end = nxt
            if nxt % 16 == 0:                      # drop inter-object padding
                for back in range(8, 24, 4):
                    if nxt - back >= st and img.w(nxt - back) == JR_RA:
                        body_end = nxt - back + 8
                        break
            for a in range(st, body_end, 4):
                t = branch_target(img.w(a), a)
                if t is None or (img.w(a) >> 26 == 2 and t in strong):
                    continue
                if nxt <= t < end and t - a < 0x4000:
                    starts.discard(nxt)
                    changed = True
                    break
    for a, sz in getattr(curated, "sizes", {}).items():   # hand-sized ranges win
        if img.in_text(a):
            starts = {s for s in starts if not a < s < a + sz} | {a, a + sz}
    starts = sorted(s for s in starts if img.in_text(s))
    funcs = {}
    for k, s in enumerate(starts):
        hi = next(h for l, h in img.text if l <= s < h)
        e = min(starts[k + 1] if k + 1 < len(starts) else hi, hi)
        last = None
        for a in range(s, e, 4):
            if img.w(a) == JR_RA:
                last = a + 8
        size = (last - s) if last and e - last < 16 and last <= e else e - s
        funcs[s] = max(size, 4)
    return funcs


def load_all(rom_path, main_path, asm_root="asm", sym_paths=("symbols/boot.txt", "symbols/main.txt"), g=None):
    if g is not None:
        configure(g)
    rom = open(rom_path, "rb").read()
    boot = Image(rom[BOOT_ROM:BOOT_ROM + (BOOT_END - BOOT_VRAM)], BOOT_VRAM, BOOT_TEXT)
    main = Image(open(main_path, "rb").read(), MAIN_VRAM, MAIN_TEXT)
    g, j = splat_labels(f"{asm_root}/**/*.s")
    forced, syms = curated(sym_paths)
    fb = discover(boot, [main], g, j, set(forced))
    fm = discover(main, [boot], g, j, set(forced))
    return boot, main, fb, fm, syms


if __name__ == "__main__":
    g, args = thpsgame.from_argv(sys.argv[1:])
    boot, main, fb, fm, syms = load_all(args[0], args[1], g=g)
    print(f"boot: {len(fb)} functions ({sum(fb.values()):#x} bytes)")
    print(f"main: {len(fm)} functions ({sum(fm.values()):#x} bytes)")
    named = sum(1 for a in list(fb) + list(fm) if a in syms)
    print(f"named: {named}")
