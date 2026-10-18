"""Seed a new code class from a solved one: unpack the ROM's main image, transfer names and function
boundaries with decomp/common/sigmatch.py, and write the class's N64Recomp symbol file + codeblob.

Shared by the hosts: each game's recomp-host/tools/seed_class.py calls main(argv, host, src_class) with its
recomp-host directory and the class it seeds from. Paths below are relative to that recomp-host.

  versions/<class>/symbols/{boot,main}.txt   names transferred (splat symbol_addrs format; committed:
                                             addresses, sizes, names only)
  ../recomp/<game>.syms.toml                 N64Recomp symbol file (committed)
  ../build/<game>.codeblob.bin               boot segment + inflated main image (ROM-derived, local only)
  ../build/<game>.main.bin                   the inflated main image (local only)
  ../build/<game>.sigmatch.json              every function match (addresses only; for tools/translate_data.py)

usage: <python with rabbitizer> -I tools/seed_class.py CLASS ROM --src-rom baserom.z64 --src-main baserom.main.bin
  versions/<class>/syms_fixes.toml (optional) lists hand fixes for what funclist gets wrong: `trim` (address ->
  bytes of leading linker padding to cut off a function) and `drop` (addresses of padding "functions").
  CLASS must be in versions/classes.toml (its `game` row in games.toml needs the geometry facts);
  the source is the wrapper's source class (THPS1: `us11`, decomp/thps1/recomp/thps1.syms.toml).
funclist (generic boundary discovery) can split a matched function at an inner `jr ra`; sigmatch's tier A/O
matches are whole-body matches, so afterwards every other function that starts strictly inside a matched
range is dropped and the matched function keeps its matched size (tier C sizes are unverified: not applied).
(`range` in a symbols file would do the same inside funclist, but it also forces a start at the range end,
which can land in padding.)
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import tomllib

COMMON = os.path.dirname(os.path.abspath(__file__))


def py(script, *args, cwd=None):
    r = subprocess.run([sys.executable, "-I", os.path.join(COMMON, script), *args], cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{script} failed:\n{r.stdout}{r.stderr}")
    return r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""


def main(argv, host, src_class, all_matches=False):
    """all_matches: also list the unnamed whole-body matches (tiers A and O, as func_<dst>) as known starts. A host
    whose source class's function list came from splat's glabels + funclist (THPS2) needs it: the seeding runs funclist
    alone (no splat pass for the new version), which misses the starts only splat found (THPS2 Europe: 42 of 3,354)."""
    HOST = host
    GAMEDIR = os.path.dirname(HOST)
    with open(os.path.join(HOST, "versions", "classes.toml"), "rb") as f:
        classes = tomllib.load(f)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cls")
    ap.add_argument("rom")
    ap.add_argument("--src-rom", required=True)
    ap.add_argument("--src-main", required=True)
    ap.add_argument("--src-game", default=classes[src_class]["game"])
    a = ap.parse_args(argv)
    game, syms = classes[a.cls]["game"], classes[a.cls]["syms"]
    sys.path.insert(0, COMMON)
    import thpsgame  # noqa: E402
    g = thpsgame.load(game)
    build = os.path.join(GAMEDIR, "build")
    os.makedirs(build, exist_ok=True)
    main_bin = os.path.join(build, game + ".main.bin")
    print(py("thps_unpack.py", "--game", game, a.rom, main_bin))
    with tempfile.TemporaryDirectory() as tmp:
        js, txt = os.path.join(tmp, "m.json"), os.path.join(tmp, "m.txt")
        print(py("sigmatch.py", a.src_game, a.src_rom, a.src_main, os.path.join(GAMEDIR, classes[src_class]["syms"]),
                 game, a.rom, main_bin, "--min-size", "8", "--json", js, "--symbols", txt))
        rows = json.load(open(js))
        with open(os.path.join(build, game + ".sigmatch.json"), "w") as f:   # every match, for tools/translate_data.py
            json.dump(rows, f)
        tier = {r["name"]: r["tier"] for r in rows}
        ranges = {r["dst"]: r["size"] for r in rows if r["tier"] in ("A", "O")}
        boot, main = [], []
        for line in open(txt):
            m = re.match(r"(\S+) = (0x[0-9A-F]+); // (.*)", line)
            name, addr, rest = m.group(1), int(m.group(2), 16), m.group(3)
            (boot if addr < g.main_vram else main).append(f"{name} = 0x{addr:08X}; // {rest}  tier:{tier.get(name, '?')}\n")
        if all_matches:
            named = {int(re.match(r"\S+ = (0x[0-9A-F]+);", l).group(1), 16) for l in boot + main}
            extra = 0
            for r in sorted(rows, key=lambda r: r["dst"]):
                if r["tier"] in ("A", "O") and r["dst"] not in named:
                    named.add(r["dst"])
                    extra += 1
                    (boot if r["dst"] < g.main_vram else main).append(
                        f"func_{r['dst']:08X} = 0x{r['dst']:08X}; // type:func size:0x{r['size']:X} sigmatch unnamed  tier:{r['tier']}\n")
            # Tier S (shift): a source function sigmatch could not place (a tiny body with many twins, e.g. libmus's
            # command handlers, reached through a pointer table) whose two neighbouring tier-A/O anchors moved by the
            # same delta, and whose masked body is equal at its own address + that delta.
            import bisect
            import sigmatch
            fre = re.compile(r'\{\s*name\s*=\s*"([^"]+)"\s*,\s*vram\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*size\s*=\s*(0x[0-9A-Fa-f]+)')
            src_funcs = sorted((int(v, 16), n, int(z, 16)) for n, v, z in fre.findall(open(os.path.join(GAMEDIR, classes[src_class]["syms"])).read()))
            sg = thpsgame.load(a.src_game)
            simg, sboot = open(a.src_main, "rb").read(), open(a.src_rom, "rb").read()[sg.boot_rom:sg.boot_rom + sg.boot_size]
            dimg, dboot = open(main_bin, "rb").read(), open(a.rom, "rb").read()[g.boot_rom:g.boot_rom + g.boot_size]

            def masked(d, base, v, n):
                o = v - base
                if o < 0 or o + 4 * n > len(d):
                    return None
                return [sigmatch.mask_word(int.from_bytes(d[o + 4 * k:o + 4 * k + 4], "big")) for k in range(n)]

            anchors = sorted((r["src"], r["dst"]) for r in rows if r["tier"] in ("A", "O"))
            asrc = [x[0] for x in anchors]
            matched_src = {r["src"] for r in rows}
            shifted = 0
            for v, n, z in src_funcs:
                if v in matched_src or z < 4:
                    continue
                i = bisect.bisect_left(asrc, v)
                if i == 0 or i >= len(anchors):
                    continue
                d0, d1 = anchors[i - 1][1] - anchors[i - 1][0], anchors[i][1] - anchors[i][0]
                t = v + d0
                if d0 != d1 or t in named:
                    continue
                main_side = v >= sg.main_vram
                sw = masked(simg, sg.main_vram, v, z // 4) if main_side else masked(sboot, sg.boot_vram, v, z // 4)
                dw = masked(dimg, g.main_vram, t, z // 4) if main_side else masked(dboot, g.boot_vram, t, z // 4)
                if sw is None or sw != dw:
                    continue
                named.add(t)
                shifted += 1
                nm = n if not n.startswith("func_") else f"func_{t:08X}"
                (boot if t < g.main_vram else main).append(f"{nm} = 0x{t:08X}; // type:func size:0x{z:X} shift  tier:S\n")
            print(f"symbols: {extra} unnamed matched starts added, {shifted} placed by shift (all_matches)")
    d = os.path.join(HOST, "versions", a.cls, "symbols")
    os.makedirs(d, exist_ok=True)
    hdr = f"// {g.title}: names transferred from {a.src_game} by decomp/common/sigmatch.py (tools/seed_class.py).\n// splat symbol_addrs format; addresses, sizes and names only.\n"
    open(os.path.join(d, "boot.txt"), "w").write(hdr + "".join(boot))
    open(os.path.join(d, "main.txt"), "w").write(hdr + "".join(main))
    print(f"symbols: {len(boot)} boot + {len(main)} main names -> {d}")
    # funclist reads symbols/*.txt relative to the working directory
    toml_path = os.path.join(GAMEDIR, syms)
    print(py("gen_syms.py", "--game", game, a.rom, main_bin, os.path.join(build, game + ".codeblob.bin"), toml_path,
             cwd=os.path.join(HOST, "versions", a.cls)))
    # Matched ranges win over funclist's own splits.
    ent = re.compile(r'^(\s*\{ name = ")([^"]+)(", vram = )(0x[0-9A-Fa-f]+)(, size = )(0x[0-9A-Fa-f]+)( \},)$')
    img = open(main_bin, "rb").read()
    boot = open(a.rom, "rb").read()[g.boot_rom:g.boot_rom + g.boot_size]

    def word(v):
        d, base = (img, g.main_vram) if v >= g.main_vram else (boot, g.boot_vram)
        return int.from_bytes(d[v - base:v - base + 4], "big")

    def alignment_garbage(v, sz):
        """Linker alignment padding that funclist took for a function: 4 to 12 bytes ending on a 16-byte
        boundary, starting off it, with no `jr ra` (a real stub has one)."""
        return sz <= 12 and v % 16 != 0 and (v + sz) % 16 == 0 and all(word(v + k) != 0x03E00008 for k in range(0, sz, 4))

    fixes_path = os.path.join(HOST, "versions", a.cls, "syms_fixes.toml")
    trim = {}
    if os.path.exists(fixes_path):
        with open(fixes_path, "rb") as f:
            fx = tomllib.load(f)
        trim = {int(k, 16): n for k, n in fx.get("trim", {}).items()}
        drop = {int(k, 16) for k in fx.get("drop", [])}
    else:
        drop = set()
    jal_targets = set()   # a function entered by a jal is never padding
    for d, base in ((img, g.main_vram), (boot, g.boot_vram)):
        for i in range(0, len(d) - 3, 4):
            w = int.from_bytes(d[i:i + 4], "big")
            if w >> 26 == 3:
                jal_targets.add(0x80000000 | ((w & 0x3FFFFFF) << 2))

    def garbage_word(w):
        """An instruction no compiler emits at a function start: an ALU op / load whose destination is $zero
        (linker padding words decode as such), other than the nop."""
        op, rd, rt, funct = w >> 26, (w >> 11) & 31, (w >> 16) & 31, w & 63
        if w == 0:
            return False
        if op == 0:
            return rd == 0 and (funct < 8 or 0x20 <= funct <= 0x2B) and funct not in (0x08, 0x09)
        return (0x08 <= op <= 0x0F or 0x20 <= op <= 0x26) and rt == 0

    out, dropped, resized, padding = [], 0, 0, 0
    for line in open(toml_path).read().splitlines():
        m = ent.match(line)
        if m:
            v, sz = int(m.group(4), 16), int(m.group(6), 16)
            if v in drop:
                padding += 1
                continue
            if alignment_garbage(v, sz):
                padding += 1
                continue
            lead = 0
            while not lead and v not in jal_targets and lead < sz - 4 and garbage_word(word(v + lead)):
                lead += 4   # leading words that decode as garbage (write $zero)
            if lead and v not in trim:
                trim[v] = lead   # leading padding words that decode as garbage
            if v in trim:   # leading linker padding that funclist merged into the function
                line = f"{m.group(1)}func_{v + trim[v]:08X}{m.group(3)}0x{v + trim[v]:08X}{m.group(5)}0x{sz - trim[v]:X}{m.group(7)}"
                padding += 1
                out.append(line)
                continue
            if any(r < v < r + rs for r, rs in ranges.items()):
                dropped += 1
                continue
            if v in ranges and ranges[v] != sz:
                line = f"{m.group(1)}{m.group(2)}{m.group(3)}{m.group(4)}{m.group(5)}0x{ranges[v]:X}{m.group(7)}"
                resized += 1
        out.append(line)
    open(toml_path, "w").write("\n".join(out) + "\n")
    print(f"matched ranges applied: {dropped} inner starts dropped, {resized} functions resized; {padding} alignment-padding entries dropped")
