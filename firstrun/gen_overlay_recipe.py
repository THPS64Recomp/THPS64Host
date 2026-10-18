#!/usr/bin/env python3
"""Recipe generator for the front-end overlays of THPS2 / THPS3 (what `package_split.py --refresh-recipes` emits for them).

Dev/release-time tool (a user never runs it; needs the ROM). Writes, per code class, a ROM-free recipe directory:

  overlays.txt          the 16 screens: name, ROM offsets and sizes of blob + relocation stream, function starts/sizes/names,
                        and an XXH3-64 fingerprint of the ROM bytes they cover. Read by overlay_rom.cpp on the user's machine.
  <game>.host.syms.toml the class's N64Recomp symbol file with the host's forced names (gen_host_syms.py output): names,
                        addresses, sizes. The overlay sections are NOT in it; the first run appends them from the ROM.
  blob.txt              "xxh3.<version id> = <XXH3-64 of the main code blob>" per ROM version, "versions = ..." (THPS1's format)

What is deliberately not in the recipe: the relocation stream (6,300 site/type/addend entries per class: a raw table in ROM
order: the publishing policy ships facts, not tables of game data) and any blob byte. Function starts/sizes/names are a symbol map, like the main image's.

usage: python3 -I decomp/common/firstrun/gen_overlay_recipe.py CLASS [CLASS ...] [--rom PATH] [--out DIR] [--cli overlay_rom_cli]
       CLASS: thps2/us thps2/eu thps3/us (or: all). The ROM is found in roms/ (SHA-1 checked) unless --rom is given (one class only).
       --out DIR: write DIR/<class>/ (default decomp/<game>/recomp-host/recipes/<class>/, next to THPS1's).
Function discovery is overlay_recomp.find_functions (the static build's), so the recipe equals what the static build uses.
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
COMMON = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(COMMON))
sys.path.insert(0, COMMON)
import overlay_recomp  # noqa: E402
import overlay_relocs  # noqa: E402
import thps_unpack  # noqa: E402
import thpsgame  # noqa: E402

CLASSES = {
    "thps2/us": dict(game="thps2", syms="decomp/thps2/recomp/thps2.syms.toml", prefix="thps2",
                     force="decomp/thps2/recomp-host/versions/us/host_force.toml"),
    "thps2/eu": dict(game="thps2_eu", syms="decomp/thps2/recomp/thps2_eu.syms.toml", prefix="thps2",
                     force="decomp/thps2/recomp-host/versions/eu/host_force.toml"),
    "thps3/us": dict(game="thps3", syms="decomp/thps3/recomp/thps3.syms.toml", prefix="thps3",
                     force="decomp/thps3/recomp-host/versions/us/host_force.toml"),
}


def recipe_dir(name):
    """Where a class's committed recipe lives: decomp/<game>/recomp-host/recipes/<class>/ (the shared host's layout, as THPS1's)."""
    game, cls = name.split("/")
    return os.path.join(REPO, "decomp", game, "recomp-host", "recipes", cls)


def build_cli(workdir=None):
    """Compile overlay_rom_cli (needs clang++ and the xxHash sources from the pinned runtime, vendor/recomp-runtime)."""
    xx = os.environ.get("XXHASH_DIR") or os.path.join(REPO, "vendor", "recomp-runtime", "lib", "N64ModernRuntime", "thirdparty")
    if not os.path.isdir(os.path.join(xx, "xxHash")):
        # a worktree: the vendor/ folder lives in the main checkout
        d = REPO
        while d != os.path.dirname(d) and not os.path.isdir(os.path.join(d, "vendor", "recomp-runtime")):
            d = os.path.dirname(d)
        xx = os.path.join(d, "vendor", "recomp-runtime", "lib", "N64ModernRuntime", "thirdparty")
    out = workdir or tempfile.mkdtemp(prefix="overlay_rom_cli-")
    cc, cxx = os.environ.get("CC", "clang"), os.environ.get("CXX", "clang++")
    subprocess.run([cc, "-O2", "-c", os.path.join(xx, "xxHash", "xxhash.c"), "-o", os.path.join(out, "xxhash.o"), "-I" + xx], check=True)
    exe = os.path.join(out, "overlay_rom_cli")
    subprocess.run([cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-I" + xx, os.path.join(HERE, "overlay_rom.cpp"),
                    os.path.join(HERE, "overlay_rom_cli.cpp"), os.path.join(out, "xxhash.o"), "-o", exe], check=True)
    return exe


def sha1_file(p):
    h = hashlib.sha1()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def find_rom(g):
    d = REPO
    while True:
        for n in getattr(g, "rom_names", []):
            c = os.path.join(d, "roms", n)
            if os.path.isfile(c) and sha1_file(c) == g.rom_sha1:
                return c
        rd = os.path.join(d, "roms")   # a row without rom_names (THPS3): any file of the right size whose SHA-1 matches
        if os.path.isdir(rd):
            for n in sorted(os.listdir(rd)):
                c = os.path.join(rd, n)
                if os.path.isfile(c) and os.path.getsize(c) == g.rom_size and sha1_file(c) == g.rom_sha1:
                    return c
        if d == os.path.dirname(d):
            break
        d = os.path.dirname(d)
    sys.exit(f"no ROM for {g.name} (SHA-1 {g.rom_sha1}) in roms/: pass --rom")


def xxh3(cli, path, begin=None, end=None):
    args = [cli, "xxh3", path] + ([hex(begin), hex(end)] if begin is not None else [])
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout.strip()


def recipe_text(g, spec, rom, img, cli, rom_path, force_path):
    force = {}
    with open(force_path, "rb") as f:
        for key, nm in tomllib.load(f).get("overlay_force", {}).items():
            o, off = key.split("+")
            force[(o, int(off, 16))] = nm
    recs = overlay_relocs.records(g, rom, img)
    for k in range(len(recs) - 1):   # the 16 (blob, stream) pairs tile the ROM tail; the tool keeps explicit offsets anyway
        if recs[k][3] + recs[k][4] != recs[k + 1][1] or recs[k][1] + recs[k][2] != recs[k][3]:
            sys.exit(f"{g.name}: overlay {recs[k][0]} is not contiguous with its stream/successor")
    begin, end = recs[0][1], recs[-1][3] + recs[-1][4]
    h = xxh3(cli, rom_path, begin, end)
    out = [f"# Front-end overlay recipe: {g.title}, game row {g.name} (ROM SHA-1 {g.rom_sha1}).",
           "# Generated by decomp/common/firstrun/gen_overlay_recipe.py. Facts only: offsets, sizes, names. No ROM bytes, no relocation entries.",
           "#   o <name> <blob ROM offset> <size> <relocation stream ROM offset> <stream size>",
           "#   f <function offset in blob> <size> [<forced name>]   (unnamed: ovl_<screen>_<OFFSET>)",
           "vbase 0x80C00000", "stride 0x80000", f"rom_xxh3 0x{h}"]
    used, nfun = set(), 0
    for name, code, size, rel, rsize in recs:
        relocs = overlay_recomp.parse_relocs(rom[rel:rel + rsize])
        funcs, _, _ = overlay_recomp.find_functions(bytes(rom[code:code + size]), relocs)
        out.append(f"o {name} 0x{code:X} 0x{size:X} 0x{rel:X} 0x{rsize:X}")
        for s, sz in funcs:
            fn = force.get((name, s))
            used.add((name, s))
            out.append(f"f 0x{s:X} 0x{sz:X}" + (f" {fn}" if fn else ""))
        nfun += len(funcs)
    bad = [k for k in force if k not in used]
    if bad:
        sys.exit(f"{g.name}: [overlay_force] names that are not overlay function starts: {bad}")
    return "\n".join(out) + "\n", nfun


def generate(name, rom_path=None, out=None, cli=None):
    """Write the recipe of one class (`name`: "thps2/us" ...) into `out`/<name> (default: recipe_dir(name)). Needs the ROM (found in roms/
    by its SHA-1 unless `rom_path` is given). Returns (directory, number of overlay functions, main blob XXH3)."""
    spec = CLASSES[name]
    g = thpsgame.load(spec["game"])
    rom_path = rom_path or find_rom(g)
    rom = open(rom_path, "rb").read()
    if hashlib.sha1(rom).hexdigest() != g.rom_sha1:
        sys.exit(f"{rom_path}: not the {g.title} dump (SHA-1)")
    img = thps_unpack.unpack(rom, g)
    blob = rom[:g.main_blob_off] + img
    force_path = os.path.join(REPO, spec["force"])
    text, nfun = recipe_text(g, spec, rom, img, cli, rom_path, force_path)
    d = os.path.join(out, name) if out else recipe_dir(name)
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "overlays.txt"), "w").write(text)
    with tempfile.TemporaryDirectory(prefix="ovlrecipe-") as tmp:
        bp = os.path.join(tmp, "codeblob.bin")
        open(bp, "wb").write(blob)
        subprocess.run([sys.executable, "-I", os.path.join(COMMON, "gen_host_syms.py"), "--force", force_path,
                        os.path.join(REPO, spec["syms"]), os.path.join(d, f"{spec['prefix']}.host.syms.toml"), bp],
                       check=True, capture_output=True, text=True)
        main_hash = xxh3(cli, bp)
    # THPS1's blob.txt format: one fingerprint per ROM version that runs this class, then the list.
    open(os.path.join(d, "blob.txt"), "w").write(f"xxh3.{g.name} = {main_hash}\nversions = {g.name}\n")
    return d, nfun, main_hash


def main(argv):
    rom_arg = out_arg = cli = None
    names = []
    it = iter(argv)
    for a in it:
        if a == "--rom":
            rom_arg = next(it)
        elif a == "--out":
            out_arg = next(it)
        elif a == "--cli":
            cli = next(it)
        else:
            names.append(a)
    if names == ["all"]:
        names = list(CLASSES)
    if not names or any(n not in CLASSES for n in names) or (rom_arg and len(names) != 1):
        sys.exit(__doc__)
    cli = cli or build_cli()
    for n in names:
        d, nfun, main_hash = generate(n, rom_arg, out_arg, cli)
        print(f"recipe {n}: {nfun} overlay functions, main blob xxh3 {main_hash}, {d}")


if __name__ == "__main__":
    main(sys.argv[1:])
