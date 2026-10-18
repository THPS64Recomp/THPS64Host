"""Derive the host's N64Recomp symbol file from the decomp's recomp/thps1.syms.toml.

The decomp's syms.toml is owned by the symbol track (it changes as names are
found). The host only needs a few functions to have *fixed* names so that the
N64Recomp config can hook them, plus two libultra names changed so N64Recomp
does NOT swap them for the runtime's versions (we hook them instead):

  osPiRawStartDma -> thps_osPiRawStartDma  (runtime version is a fatal stub)
  osSetEventMesg  -> thps_osSetEventMesg   (runtime ignores OS_EVENT_PI)
  osAiGetStatus   -> thps_osAiGetStatus    (runtime never reports the AI FIFO full)
  osContGetQuery  -> thps_osContGetQuery   (runtime reports CONT_CARD_ON only for a Rumble Pak)

Names are forced by VRAM, so renames upstream don't break the hooks.
Output contains addresses/sizes/names only (no ROM bytes).

usage: python3 -I decomp/common/gen_host_syms.py --force host_force/thps2.toml IN_SYMS_TOML OUT_SYMS_TOML [CODEBLOB]
       (CODEBLOB enables the prologue / .data-pointer scan)

The per-game --force file holds:
  [force]   "0xVRAM" = "name"     names forced by address (hook targets, runtime-replaced libultra)
  splits = [0xVRAM, ...]          extra function starts (run-time lookup misses)
"""
import bisect
import re
import struct
import sys
import tomllib

FORCE = {}      # vram -> name, loaded from the per-game file (host_force/<game>.toml)
SPLITS = set()  # extra function starts to split out (lookup misses found at run time)

LINE = re.compile(r'(\{\s*name\s*=\s*")([^"]+)("\s*,\s*vram\s*=\s*)(0x[0-9A-Fa-f]+)')


SIZE = re.compile(r'(size\s*=\s*)(0x[0-9A-Fa-f]+)')


SECTION = re.compile(r'^rom\s*=\s*(0x[0-9A-Fa-f]+)\s*\nvram\s*=\s*(0x[0-9A-Fa-f]+)\s*\nsize\s*=\s*(0x[0-9A-Fa-f]+)', re.M)


def parse_funcs(lines):
    funcs = []  # (line index, vram, size)
    for i, l in enumerate(lines):
        m = LINE.search(l)
        if m:
            funcs.append((i, int(m.group(4), 16), int(SIZE.search(l, m.end()).group(2), 16)))
    return funcs


def prologue_splits(lines, blob_path):
    """Starts hidden inside a listed function: (1) `jr ra` + delay slot, then `addiu sp,sp,-N`,
    with no branch from the earlier part landing at or after that point; (2) pointers in .data."""
    blob = open(blob_path, "rb").read()
    secs = [tuple(int(x, 16) for x in m.groups()) for m in SECTION.finditer("".join(lines))]

    def word(v):
        for rom, vr, sz in secs:
            if vr <= v < vr + sz:
                return struct.unpack_from(">I", blob, rom + v - vr)[0]
        return 0

    found = set()
    for _, v, sz in parse_funcs(lines):
        for p in range(v + 8, v + sz, 4):
            w = word(p)
            if word(p - 8) != 0x03E00008 or (w >> 16) != 0x27BD or not (w & 0x8000):
                continue
            crossed = False
            for q in range(v, p, 4):
                i = word(q)
                op = i >> 26
                if op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and ((i >> 16) & 0x1F) in (0, 1, 2, 3, 16, 17, 18, 19)):
                    t = q + 4 + (struct.unpack(">h", struct.pack(">H", i & 0xFFFF))[0] << 2)
                elif op == 2:
                    t = (q & 0xF0000000) | ((i & 0x3FFFFFF) << 2)
                else:
                    continue
                if p <= t < v + sz:
                    crossed = True
                    break
            if not crossed:
                found.add(p)

    # Function pointers stored in the main image's .data (after the last text section):
    # a target strictly inside a listed function, right after its `jr ra` + delay slot,
    # is a hidden start (e.g. libmus's F-command table). Jump tables live in .rodata
    # (before .text), so they are not scanned.
    funcs = sorted((v, sz) for _, v, sz in parse_funcs(lines))
    starts = [v for v, _ in funcs]
    rom_end, vram_end = max((rom + sz, vr + sz) for rom, vr, sz in secs)
    for off in range(rom_end, len(blob) - 3, 4):
        t = struct.unpack_from(">I", blob, off)[0]
        if t & 3 or not (0x80000400 <= t < vram_end):
            continue
        k = bisect.bisect_right(starts, t) - 1
        if k < 0 or not (funcs[k][0] < t < funcs[k][0] + funcs[k][1]):
            continue
        if word(t - 8) == 0x03E00008:
            found.add(t)
    return found


def apply_splits(lines, starts):
    n = 0
    for start in sorted(starts):
        funcs = parse_funcs(lines)
        if any(v == start for _, v, _ in funcs):
            continue  # upstream already has it
        parent = [f for f in funcs if f[1] < start < f[1] + f[2]]
        if not parent:
            sys.exit(f"gen_host_syms: split 0x{start:08X} is not inside any function")
        i, v, sz = parent[0]
        l = lines[i]
        m = LINE.search(l)
        sm = SIZE.search(l, m.end())
        lines[i] = l[:sm.start(2)] + f"0x{start - v:X}" + l[sm.end(2):]
        indent = l[:len(l) - len(l.lstrip())]
        lines.insert(i + 1, f'{indent}{{ name = "func_{start:08X}", vram = 0x{start:08X}, size = 0x{v + sz - start:X} }},  # host split\n')
        print(f"gen_host_syms: split {m.group(2)} 0x{v:08X}+0x{sz:X} at 0x{start:08X}")
        n += 1
    return lines


def main():
    args = sys.argv[1:]
    if "--force" not in args:
        sys.exit("gen_host_syms: --force host_force/<game>.toml is required")
    k = args.index("--force")
    with open(args[k + 1], "rb") as f:
        cfg = tomllib.load(f)
    FORCE.update({int(a, 16): n for a, n in cfg["force"].items()})
    SPLITS.update(cfg.get("splits", []))
    args = args[:k] + args[k + 2:]
    src, dst = args[0], args[1]
    blob = args[2] if len(args) > 2 else None
    seen = set()
    out = []
    with open(src) as f:
        for line in f:
            m = LINE.search(line)
            if m:
                vram = int(m.group(4), 16)
                if vram in FORCE:
                    seen.add(vram)
                    line = line[:m.start(2)] + FORCE[vram] + line[m.end(2):]
            out.append(line)
    starts = set(SPLITS)
    if blob:
        starts |= prologue_splits(out, blob)
    out = apply_splits(out, starts)
    missing = [hex(v) for v in FORCE if v not in seen]
    if missing:
        sys.exit(f"gen_host_syms: functions not found in {src}: {missing}")
    names = [LINE.search(l).group(2) for l in out if LINE.search(l)]
    dup = {n for n in names if names.count(n) > 1} if len(names) != len(set(names)) else set()
    if dup:
        sys.exit(f"gen_host_syms: duplicate names after renaming: {sorted(dup)[:10]}")
    with open(dst, "w") as f:
        f.write("# GENERATED by decomp/common/gen_host_syms.py - do not edit\n")
        f.writelines(out)
    print(f"gen_host_syms: {len(names)} functions, {len(FORCE)} forced names -> {dst}")


if __name__ == "__main__":
    main()
