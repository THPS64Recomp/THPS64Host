"""Make the front-end overlays of an EoR THPS N64 port (THPS2/THPS3) recompilable with N64Recomp as
*relocatable sections* (the Zelda64Recomp overlay model).

Each screen overlay is a code+rodata blob linked at base 0 plus a relocation stream that the game's own
loader applies when it loads the blob at a run-time base (overlay_table.py / overlay_relocs.py describe the
format; stream types: 0 R_32, 1 HI16 +addend, 2 LO16 +addend, 3 J26). For N64Recomp we need:
  * the blob as the recompiler's ROM: appended to the code blob, "pre-linked" at a fake base V_k so that
    ordinary instruction decoding (jal targets, jump tables, lui/addiu pairs) sees consistent addresses;
  * a symbols section per overlay: functions (found here) and `relocs` (HI16/LO16/R_MIPS_26 with
    target_vram = V_k + offset), which makes N64Recomp emit RELOC_HI16/LO16 against section_addresses[] and
    marks the section relocatable. R_32 words are data: the game's loader relocates them in RDRAM and the
    recompiled code reads them from there, so they need no entry.
At run time the host calls load_overlays(blob_off, real_base, size) when the overlay's bytes are DMA'd
(librecomp then maps the functions at real_base + offset and sets section_addresses).

Inputs/outputs (the blob is ROM-derived: keep it local; the .syms.toml and .inc hold offsets only):
  python3 -I decomp/common/overlay_recomp.py --game thps2 baserom.z64 baserom.main.bin \
      --blob-in build/thps2.codeblob.bin --blob-out recomp-host/build/gen/thps2.host.blob.bin \
      --syms-in recomp-host/build/gen/thps2.host.syms.toml --syms-out ... (appended copy) \
      --table-out recomp-host/build/gen/overlay_table.inc [--vbase 0x80C00000] [--force versions/<class>/host_force.toml]

Function discovery per overlay (no disassembler needed): starts = offset 0, every R_MIPS_26 target, every
HI/LO value that is a word after `jr ra`+delay, R_32 values that are, then a linear sweep where a `jr ra`
ends a function unless a branch or a jump-table case reaches beyond it. Jump tables are located from the
LO16-relocated `lw` before each indirect `jr`. Everything after the last code word is rodata.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops it
import overlay_relocs  # noqa: E402
import thpsgame  # noqa: E402

JR_RA = 0x03E00008


def parse_relocs(st):
    out, i = [], 0
    while True:
        (w,) = struct.unpack_from(">I", st, i)
        i += 4
        if w == 0xFFFFFFFF:
            return out
        t, site = w & 3, w & ~3
        if t in (1, 2):
            (a,) = struct.unpack_from(">I", st, i)
            i += 4
            out.append((t, site, a))
        else:
            out.append((t, site, None))


def branch_target(ins, pos):
    """Return the in-function forward/backward branch target of a conditional branch or j, else None."""
    op = ins >> 26
    if op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and ((ins >> 16) & 0x1F) in (0, 1, 2, 3, 16, 17, 18, 19)) \
            or (op == 17 and ((ins >> 21) & 0x1F) == 8):
        off = ins & 0xFFFF
        if off & 0x8000:
            off -= 0x10000
        return pos + 4 + off * 4
    return None


def find_functions(blob, relocs):
    size = len(blob) // 4 * 4
    n = size // 4
    w = struct.unpack_from(f">{n}I", blob, 0)
    by_site = {s: (t, a) for t, s, a in relocs}
    code_min_end = max((s for t, s, a in relocs if t in (1, 2, 3)), default=0) + 4
    # a jal target is code, even if no reloc'd instruction follows it
    code_min_end = max([code_min_end] + [((blob[s] << 24 | blob[s+1] << 16 | blob[s+2] << 8 | blob[s+3]) & 0x03FFFFFF) * 4 + 4
                                         for t, s, a in relocs if t == 3 and blob[s] >> 2 == 3])
    # R_32 table runs (jump tables / vtables): sites -> value (the word itself, base-0 offset)
    r32 = {s: w[s // 4] for t, s, a in relocs if t == 0}

    def is_after_ret(v):
        return v == 0 or (v >= 8 and w[v // 4 - 2] == JR_RA)

    def jump_table_targets(p):
        """Case labels of the jump table used by the indirect `jr` at p (a LO16 reloc within 10 words that points at an R_32 run)."""
        for q in range(p - 4, max(p - 44, -1), -4):
            r = by_site.get(q)
            if r and r[0] == 2 and r[1] in r32:  # LO16 (on the addiu or the lw) pointing at a run of R_32 words
                T = r[1]
                tg, a = [], T
                while a in r32:
                    tg.append(r32[a])
                    a += 4
                return tg
        return []

    jt_entries = set()  # case labels of every jump table (not function starts even after a `jr ra`)
    for q in range(0, size, 4):
        ins = w[q // 4]
        if ins >> 26 == 0 and (ins & 0x3F) == 8 and ((ins >> 21) & 0x1F) != 31:
            jt_entries.update(jump_table_targets(q))

    strong = {0}
    for t, s, a in relocs:
        if t == 3:
            tgt = (w[s // 4] & 0x03FFFFFF) << 2
            if tgt < size and (w[s // 4] >> 26) == 3:  # jal only (a j may be an internal jump)
                strong.add(tgt)
        elif t in (1, 2) and a < code_min_end and a % 4 == 0 and is_after_ret(a) and a not in jt_entries:  # (later string/rodata refs are not code)
            strong.add(a)
    for s, v in r32.items():
        if v < code_min_end and v % 4 == 0 and is_after_ret(v) and v not in jt_entries:
            strong.add(v)

    funcs, pos = [], 0
    while pos < size:
        while pos < size and w[pos // 4] == 0 and pos > code_min_end:
            pos += 4
        if pos >= size:
            break
        if pos > code_min_end and not (w[pos // 4] >> 16 == 0x27BD or pos in strong):
            break  # rodata starts here
        start, p, reach = pos, pos, pos
        end = None
        while p < size:
            ins = w[p // 4]
            t = branch_target(ins, p)
            if t is not None and t > p:
                reach = max(reach, t)
            if (ins >> 26) == 2:
                tgt = (ins & 0x03FFFFFF) << 2
                if p < tgt < size and tgt not in strong:
                    reach = max(reach, tgt)
            if ins >> 26 == 0 and (ins & 0x3F) == 8 and ((ins >> 21) & 0x1F) != 31:  # jr rs (not ra)
                for tg in jump_table_targets(p):
                    if p < tg < size:
                        reach = max(reach, tg)
            if ins == JR_RA and reach <= p + 4:
                end = p + 8
                break
            p += 4
            if p in strong and p > start:  # a jal target (or a function pointer) is always a function start
                end = p
                break
        if end is None:
            end = size
        funcs.append((start, end - start))
        pos = end
    return funcs, strong, code_min_end


def main():
    g, args = thpsgame.from_argv(sys.argv[1:])
    opts, pos = {}, []
    it = iter(args)
    for a in it:
        if a.startswith("--"):
            opts[a[2:]] = next(it)
        else:
            pos.append(a)
    rom, img = open(pos[0], "rb").read(), open(pos[1], "rb").read()
    vbase = int(opts.get("vbase", "0x80C00000"), 0)
    stride = 0x00080000
    blob = bytearray(open(opts["blob-in"], "rb").read())
    blob += bytes((-len(blob)) % 16)
    syms = open(opts["syms-in"]).read().rstrip("\n") + "\n"
    # --force FILE (a host's versions/<class>/host_force.toml): [overlay_force] "overlay+0xOFFSET" = "name" names
    # overlay functions for hooks. A forced name that does not land on a function start is an error.
    force = {}
    if "force" in opts:
        import tomllib
        with open(opts["force"], "rb") as f:
            for key, nm in tomllib.load(f).get("overlay_force", {}).items():
                o, off = key.split("+")
                force[(o, int(off, 16))] = nm
    used = set()
    table = []
    total = {"funcs": 0, "relocs": 0}
    for k, (name, code, size, rel, rsize) in enumerate(overlay_relocs.records(g, rom, img)):
        V = vbase + k * stride
        text = bytearray(rom[code:code + size])
        relocs = parse_relocs(rom[rel:rel + rsize])
        raw = bytes(text)
        funcs, strong, code_end = find_functions(raw, relocs)
        # pre-link at V
        for t, s, a in relocs:
            (wd,) = struct.unpack_from(">I", text, s)
            if t == 0:
                wd = (wd + V) & 0xFFFFFFFF
            elif t == 1:
                wd = (wd & 0xFFFF0000) | (((V + a + 0x8000) >> 16) & 0xFFFF)
            elif t == 2:
                wd = (wd & 0xFFFF0000) | ((V + a) & 0xFFFF)
            else:
                wd = (wd & 0xFC000000) | ((wd + ((V & 0x0FFFFFFF) >> 2)) & 0x03FFFFFF)
            struct.pack_into(">I", text, s, wd)
        short = name[3:] if name.startswith("t2_") or name.startswith("t3_") else name
        blob_off = len(blob)
        blob += text + bytes((-len(text)) % 16)
        starts = {s for s, _ in funcs}
        missing = sorted(x for x in strong if x not in starts)
        sec = [f"\n[[section]]\nname = \".ovl_{short}\"\nrom = 0x{blob_off:X}\nvram = 0x{V:08X}\nsize = 0x{size:X}\n"]
        sec.append("relocs = [\n")
        names = {0: None, 1: "R_MIPS_HI16", 2: "R_MIPS_LO16", 3: "R_MIPS_26"}
        for t, s, a in sorted(relocs, key=lambda r: r[1]):
            if t == 0:
                continue
            (wd,) = struct.unpack_from(">I", raw, s)
            tv = V + (a if t in (1, 2) else ((wd & 0x03FFFFFF) << 2))
            sec.append(f"    {{ type = \"{names[t]}\", vram = 0x{V + s:08X}, target_vram = 0x{tv:08X} }},\n")
        sec.append("]\nfunctions = [\n")
        for s, sz in funcs:
            fname = force.get((name, s), f"ovl_{short}_{s:05X}")
            used.add((name, s))
            sec.append(f"    {{ name = \"{fname}\", vram = 0x{V + s:08X}, size = 0x{sz:X} }},\n")
        sec.append("]\n")
        syms += "".join(sec)
        table.append((name, code, size, blob_off, V, len(funcs)))
        total["funcs"] += len(funcs)
        total["relocs"] += sum(1 for t, _, _ in relocs if t)
        print(f"{name:20s} blob+0x{blob_off:X} V=0x{V:08X} size 0x{size:X} code_end~0x{code_end:X} funcs {len(funcs)}"
              f" (strong starts {len(strong)}, not starts after sweep {len(missing)}{' ' + ' '.join(hex(m) for m in missing[:6]) if missing else ''})")
    bad = [f"{o}+0x{off:X} = {nm}" for (o, off), nm in force.items() if (o, off) not in used]
    if bad:
        sys.exit("overlay_recomp: [overlay_force] names that are not overlay function starts: " + ", ".join(bad))
    open(opts["blob-out"], "wb").write(blob)
    open(opts["syms-out"], "w").write(syms)
    with open(opts["table-out"], "w") as f:
        f.write("// Generated by decomp/common/overlay_recomp.py (offsets only). { name, rom text offset, size, codeblob offset, fake base }\n")
        for name, code, size, blob_off, V, nf in table:
            f.write(f'{{ "{name}", 0x{code:X}, 0x{size:X}, 0x{blob_off:X}, 0x{V:08X} }},\n')
    print(f"total: {len(table)} overlays, {total['funcs']} functions, {total['relocs']} HI/LO/J relocs; blob {len(blob):#x} bytes")


if __name__ == "__main__":
    main()
