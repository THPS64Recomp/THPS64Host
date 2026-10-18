"""Generate N64Recomp inputs for the front-end overlays (THPS2/THPS3): relocatable sections.

Appends the 16 overlay blobs to a copy of the game's codeblob and writes a syms.toml that holds the game's
normal sections (boot + main, copied from recomp/<game>.syms.toml) plus one relocatable section per overlay
(`.ovl_NN_<name>`, `relocs` decoded from the relocation stream, see overlay_relocs.py).

Link addresses: section k is placed at vram 0x80800000 + k * 0x20000. The base only has to (a) lie in the
0x8xxxxxxx region, because the overlays' absolute `jal`s into the main image are resolved with the section's
top nibble, and (b) not collide with boot/main; at run time the host registers each overlay at the heap address
Overlay_Load picked, so the link address is a pure label.

Function boundaries inside a blob (no symbols exist): offset 0 (the init function), every R_26 target,
every R_32 / HI16 / LO16 addend that points at a word following a `jr ra` + delay slot or at a prologue
(`addiu sp,sp,-N`), and every word after a `jr ra` + delay slot. Code ends after the last `jr ra`
delay slot (string data and jump tables follow it inside the blob).

usage: python3 -I decomp/common/overlay_syms.py --game thps3 baserom.z64 baserom.main.bin \
           build/thps3.codeblob.bin recomp/thps3.syms.toml build/thps3.ovl.codeblob.bin recomp/thps3.ovl.syms.toml
Output syms.toml has addresses/sizes/names only; the blob output is ROM-derived (build/, never commit).
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops it
import overlay_relocs  # noqa: E402
import thpsgame  # noqa: E402

JR_RA = 0x03E00008
OVL_VRAM, OVL_STRIDE = 0x80800000, 0x20000
RELOC_NAMES = {0: "R_MIPS_32", 1: "R_MIPS_HI16", 2: "R_MIPS_LO16", 3: "R_MIPS_26"}


def decode_stream(st):
    out, i = [], 0
    while True:
        (w,) = struct.unpack_from(">I", st, i)
        i += 4
        if w == 0xFFFFFFFF:
            return out
        t, site = w & 3, w & ~3
        addend = None
        if t in (1, 2):
            (addend,) = struct.unpack_from(">I", st, i)
            i += 4
        out.append((site, t, addend))


def relocate(blob, relocs, base):
    """Apply the stream as Overlay_Reloc does, for load address `base` (the link-time copy N64Recomp reads)."""
    b = bytearray(blob)
    for site, t, a in relocs:
        (w,) = struct.unpack_from(">I", b, site)
        if t == 0:
            w = (w + base) & 0xFFFFFFFF
        elif t == 1:
            w = (w & 0xFFFF0000) | (((base + a + 0x8000) >> 16) & 0xFFFF)
        elif t == 2:
            w = (w & 0xFFFF0000) | ((base + a) & 0xFFFF)
        else:
            w = (w & 0xFC000000) | ((w + ((base & 0x0FFFFFFF) >> 2)) & 0x03FFFFFF)
        struct.pack_into(">I", b, site, w)
    return bytes(b)


def branch_target(i, w):
    """Word index a conditional branch at word i jumps to, or None."""
    op = w >> 26
    if op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or (op == 1 and ((w >> 16) & 0x1F) in (0, 1, 2, 3, 0x10, 0x11, 0x12, 0x13)) \
            or (op == 0x11 and ((w >> 21) & 0x1F) == 8):
        imm = w & 0xFFFF
        return i + 1 + (imm - 0x10000 if imm & 0x8000 else imm)
    return None


def analyse(blob, relocs):
    """Function starts (byte offsets) and the end of code.

    A `jr ra` ends a function only when no branch, intra-function `j` or jump-table case of the function
    lies beyond its delay slot (the same rule as funclist.py). Jump tables are found through the LO16
    relocation that points into the blob's data and the run of R_32 relocs at that address.
    """
    words = struct.unpack_from(f">{len(blob) // 4}I", blob, 0)
    n = len(words)
    code_end_w = 0
    for i in range(n - 1):
        if words[i] == JR_RA:
            code_end_w = i + 2
    r32 = {site // 4: words[site // 4] for site, t, a in relocs if t == 0}
    lo16 = [(site // 4, a) for site, t, a in relocs if t == 2]
    called = set()     # targets of jal (definitely function starts)
    jtargets = set()   # targets of `j` that are not also called (intra-function gotos)
    for site, t, a in relocs:
        if t == 3:
            w = words[site // 4]
            tgt = (w & 0x3FFFFFF)           # word index of the target (already offset >> 2)
            (called if (w >> 26) == 3 else jtargets).add(tgt)
    jtargets -= called
    starts = [0]
    i, s = 0, 0
    maxt = 0
    case_cache = {}

    def table_cases(lo_i, hi_i):
        """Case targets (word indexes) of jump tables referenced by LO16 relocs in words lo_i..hi_i."""
        out = []
        for wi, a in lo16:
            if lo_i <= wi <= hi_i and a % 4 == 0 and a // 4 >= code_end_w:
                k = a // 4
                run = []
                while k in r32 and 0 <= r32[k] // 4 < code_end_w and r32[k] % 4 == 0:
                    run.append(r32[k] // 4)
                    k += 1
                out += run
        return out

    while i < code_end_w:
        w = words[i]
        t = branch_target(i, w)
        if t is not None and t > maxt:
            maxt = t
        if i in jtargets:
            pass
        if (w >> 26) == 2 and (i * 4) in {site for site, tt, a in relocs if tt == 3 and site == i * 4}:
            tt = w & 0x3FFFFFF
            if tt not in called and tt > maxt:
                maxt = tt
        if (w >> 26) == 0 and (w & 0x3F) == 8 and ((w >> 21) & 0x1F) != 31:       # jr $reg (not ra): a switch
            for c in table_cases(s, i):
                if c > maxt:
                    maxt = c
        nxt_start = False
        if w == JR_RA:
            end = i + 2
            if end > maxt:
                nxt_start = True
        if not nxt_start and (i + 1) in called and i + 1 > s:
            nxt_start = True     # a called function begins right after this word: close the current one
            end = i + 1
        if nxt_start:
            if end < code_end_w:
                starts.append(end)
            s, i, maxt = end, end, end
            continue
        i += 1
    return sorted({x * 4 for x in starts}), code_end_w * 4


def main():
    g, args = thpsgame.from_argv(sys.argv[1:])
    rom = open(args[0], "rb").read()
    img = open(args[1], "rb").read()
    codeblob = open(args[2], "rb").read()
    syms_in = open(args[3]).read()
    recs = overlay_relocs.records(g, rom, img)
    out_blob = bytearray(codeblob)
    out_blob += bytes(-len(out_blob) % 16)
    toml = [syms_in.rstrip("\n"), "", "# ---- front-end overlays (decomp/common/overlay_syms.py): relocatable sections at 0x80800000 + k * 0x20000 ----", ""]
    total_f = total_r = 0
    for k, (name, code, size, rel, rsize) in enumerate(recs):
        blob, st = rom[code:code + size], rom[rel:rel + rsize]
        relocs = decode_stream(st)
        starts, code_end = analyse(blob, relocs)
        base = len(out_blob)
        vb = OVL_VRAM + k * OVL_STRIDE
        # the link-time copy: N64Recomp decodes `j` targets and jump tables from the raw words, so store the
        # blob already relocated to its link address (the relocs stay in the toml for the RELOC_* macros)
        out_blob += relocate(blob, relocs, vb) + bytes(-len(blob) % 16)
        toml += ["[[section]]", f'name = ".ovl_{k:02d}_{name}"', f"rom = 0x{base:X}", f"vram = 0x{vb:08X}", f"size = 0x{size:X}", ""]
        toml.append("functions = [")
        for j, s in enumerate(starts):
            e = starts[j + 1] if j + 1 < len(starts) else code_end
            toml.append(f'    {{ name = "ovl{k:02d}_{s:05X}", vram = 0x{vb + s:08X}, size = 0x{e - s:X} }},')
        toml += ["]", "", "relocs = ["]
        for site, t, a in sorted(relocs):
            (word,) = struct.unpack_from(">I", blob, site)
            tgt = (word & 0x3FFFFFF) << 2 if t == 3 else (word if t == 0 else a)
            toml.append(f'    {{ type = "{RELOC_NAMES[t]}", vram = 0x{vb + site:08X}, target_vram = 0x{vb + tgt:08X} }},')
        toml += ["]", ""]
        total_f += len(starts)
        total_r += len(relocs)
        print(f"  {k:2d} {name:20s} blob 0x{size:X} code_end 0x{code_end:X} functions {len(starts):3d} relocs {len(relocs)}")
    os.makedirs(os.path.dirname(args[4]) or ".", exist_ok=True)
    open(args[4], "wb").write(out_blob)
    open(args[5], "w").write("\n".join(toml))
    print(f"{args[5]}: {len(recs)} overlay sections, {total_f} functions, {total_r} relocs; {args[4]}: {len(out_blob):#x} bytes")


if __name__ == "__main__":
    main()
