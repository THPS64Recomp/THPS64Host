"""Generate what the host links (ROM-derived output stays in build/gen/, never committed). Shared by THPS1-3: run it through a
game's wrapper, decomp/thps<N>/recomp-host/tools/gen.py.
  build/gen/thps_versions.h               version table + per-class SymTable + thps::game (tools/gen_versions.py)
  build/gen/<class>/RecompiledFuncs/      N64Recomp output of one code class (+ namespace.h)
  build/gen/<class>/<game>.host.blob.bin  (games with front-end overlays: THPS2/3) the class's codeblob + its relocatable screens,
                                          pre-linked (decomp/common/overlay_recomp.py); versions/<class>/overlay_table.inc is
                                          written next to the tables (offsets and names only: committed)
  build/gen/rsp/n_aspMain.cpp             audio microcode (RSPRecomp; config: gen_versions.py rsp-config of the first CLASS)

Needs, per class, the decomp's local codeblob build/<game>.codeblob.bin (decomp/common/gen_syms.py --game <game>;
docs/versions.md "How to add a version"; USA: `make extract recomp-inputs` in the game's decomp directory). A game with overlays
also needs the main image (build/<game>.main.bin, or baserom.main.bin for the family root) and the class's ROM (the overlays are
in its tail): found by the games.toml `rom_names` in a roms/ folder up the tree (SHA-1 checked), the decomp's baserom.z64, or
given as --rom CLASS=PATH.

usage: python3 -I tools/gen.py /path/to/N64Recomp [--rom CLASS=PATH ...] [CLASS ...]    (default: every class in versions/classes.toml)
       (RSPRecomp is taken from next to N64Recomp, or $RSPRECOMP; $N64RECOMP replaces the first argument)
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostenv  # noqa: E402

HOST = hostenv.host_dir()
GAME = hostenv.game(HOST)
FAMILY = GAME["family"]
OVERLAYS = bool(GAME.get("overlays"))
GAMEDIR = os.path.dirname(HOST)                    # decomp/thps<N>
COMMON = hostenv.COMMON
GEN = os.path.join(HOST, "build", "gen")
PY = [sys.executable, "-I"]
import thpsgame  # noqa: E402


def run(args, log=None, tail=0):
    """Run a command; with `log`, write its output there (and print the last `tail` lines on failure)."""
    if log is None:
        subprocess.run(args, check=True)
        return ""
    with open(log, "w") as f:
        p = subprocess.run(args, stdout=f, stderr=subprocess.STDOUT)
    text = open(log).read()
    if p.returncode:
        print("\n".join(text.splitlines()[-20:]), file=sys.stderr)
        sys.exit(f"gen: {args[0]} failed ({p.returncode}); log {log}")
    return text


def versions_tool(*args):
    return subprocess.run(PY + [os.path.join(HOST, "tools", "gen_versions.py"), *args], check=True, capture_output=True, text=True).stdout.strip()


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def find_rom(g, given):
    """The ROM of games.toml row g: --rom, the decomp's baserom.z64 (the family root), or roms/<rom_names> up the tree."""
    cands = [given] if given else []
    if g.name == g.family:
        cands.append(os.path.join(GAMEDIR, "baserom.z64"))
    d = GAMEDIR
    while True:
        for n in getattr(g, "rom_names", []):
            cands.append(os.path.join(d, "roms", n))
        up = os.path.dirname(d)
        if up == d:
            break
        d = up
    for c in cands:
        if c and os.path.isfile(c) and sha1(c) == g.rom_sha1:
            return c
    sys.exit(f"gen: no ROM for {g.name} (SHA-1 {g.rom_sha1}): pass --rom CLASS=PATH (a big-endian .z64/.n64)")


def main(argv):
    roms, rest = {}, []
    it = iter(argv)
    for a in it:
        if a == "--rom":
            k, v = next(it).split("=", 1)
            roms[k] = v
        else:
            rest.append(a)
    n64recomp = rest[0] if rest else os.environ.get("N64RECOMP", "N64Recomp")
    n64recomp = shutil.which(n64recomp) or n64recomp
    rsprecomp = os.environ.get("RSPRECOMP") or os.path.join(os.path.dirname(os.path.abspath(n64recomp)), "RSPRecomp")
    versions_tool("check")
    classes = rest[1:] or versions_tool("classes").split()
    os.makedirs(GEN, exist_ok=True)
    versions_tool("header", os.path.join(GEN, "thps_versions.h"))
    for cls in classes:
        game, syms = versions_tool("class-info", cls).split()
        g = thpsgame.load(game)
        syms = os.path.join(GAMEDIR, syms)
        blob = os.path.join(GAMEDIR, "build", game + ".codeblob.bin")
        if not os.path.exists(blob):
            sys.exit(f"missing {blob}: run decomp/common/gen_syms.py --game {game} (docs/versions.md)")
        force = os.path.join(HOST, "versions", cls, "host_force.toml")
        print(f"== code class {cls} (game {game})")
        d = os.path.join(GEN, cls)
        os.makedirs(d, exist_ok=True)
        # The class's symbol file with the host's forced names (the symbol track's syms.toml changes under us).
        host_syms = os.path.join(d, FAMILY + (".host.syms0.toml" if OVERLAYS else ".host.syms.toml"))
        out = subprocess.run(PY + [os.path.join(COMMON, "gen_host_syms.py"), "--force", force, syms, host_syms, blob],
                             check=True, capture_output=True, text=True).stdout
        print("\n".join(l for l in out.splitlines() if "host split" not in l and "split " not in l[:30]))
        if OVERLAYS:
            # Front-end overlays (16 relocatable screens in the ROM tail): sections + pre-linked blob appended to the code blob.
            main_bin = os.path.join(GAMEDIR, "baserom.main.bin") if g.name == g.family else os.path.join(GAMEDIR, "build", game + ".main.bin")
            if not os.path.exists(main_bin):
                sys.exit(f"missing {main_bin}: thps_unpack.py (docs/versions.md; USA: make extract recomp-inputs)")
            rom = find_rom(g, roms.get(cls))
            out = subprocess.run(PY + [os.path.join(COMMON, "overlay_recomp.py"), "--game", game, rom, main_bin,
                                       "--blob-in", blob, "--blob-out", os.path.join(d, FAMILY + ".host.blob.bin"),
                                       "--syms-in", host_syms, "--syms-out", os.path.join(d, FAMILY + ".host.syms.toml"),
                                       "--table-out", os.path.join(HOST, "versions", cls, "overlay_table.inc"), "--force", force],
                                 capture_output=True, text=True)
            if out.returncode:
                sys.exit(f"gen: overlay_recomp.py failed:\n{out.stdout}{out.stderr}")
            print(out.stdout.strip().splitlines()[-1])
        run(PY + [os.path.join(HOST, "tools", "gen_versions.py"), "config", cls, d])
        # Keep the previous output: files that come out byte-identical keep their old mtime, so a re-run
        # after adding a host split recompiles only the files that changed.
        rf, old = os.path.join(d, "RecompiledFuncs"), os.path.join(d, "RecompiledFuncs.old")
        shutil.rmtree(old, ignore_errors=True)
        if os.path.isdir(rf):
            os.rename(rf, old)
        log = os.path.join(d, "n64recomp.log")
        text = run([n64recomp, os.path.join(d, FAMILY + ".host.toml")], log=log)
        print("\n".join(text.splitlines()[-2:]))
        kept = 0
        if os.path.isdir(old):
            for fn in os.listdir(rf):
                o, n = os.path.join(old, fn), os.path.join(rf, fn)
                if os.path.isfile(o) and open(o, "rb").read() == open(n, "rb").read():
                    shutil.copy2(o, n)
                    kept += 1
            shutil.rmtree(old)
        print("warnings:", sum(1 for l in text.splitlines() if l.startswith("[Warn]")), " files:", len(os.listdir(rf)), " unchanged kept:", kept)
        run(PY + [os.path.join(HOST, "tools", "gen_versions.py"), "namespace", cls, rf, os.path.join(d, "namespace.h")])
    # Audio microcode (n_aspMain). The RSP program is the same for every version (docs/versions.md), so it is
    # generated once, from the first class's blob (so any one class's ROM is enough); the classes' copies must agree.
    rsp_dir = os.path.join(GEN, "rsp")
    os.makedirs(rsp_dir, exist_ok=True)
    for f in os.listdir(rsp_dir):
        if re.fullmatch(r"aspMain\.\w+\.toml", f):
            os.remove(os.path.join(rsp_dir, f))
    texts = {}
    for cls in classes:
        cfg = os.path.join(rsp_dir, f"aspMain.{cls}.toml")
        blob = os.path.join(GAMEDIR, "build", versions_tool("class-info", cls).split()[0] + ".codeblob.bin")
        versions_tool("rsp-config", cls, cfg, os.path.relpath(blob, rsp_dir).replace(os.sep, "/"), "n_aspMain.cpp")
        off, size = (int(re.search(rf"^{k} = (0x[0-9A-Fa-f]+)$", open(cfg).read(), re.M).group(1), 16) for k in ("text_offset", "text_size"))
        with open(blob, "rb") as f:
            f.seek(off)
            texts[cls] = (cfg, f.read(size))
    first = classes[0]
    for cls, (_, text) in texts.items():
        if text != texts[first][1]:
            sys.exit(f"gen: the n_aspMain microcode of class {cls} differs from {first}'s: one generated n_aspMain.cpp cannot serve both")
    run([rsprecomp, texts[first][0]], log=os.path.join(rsp_dir, "rsprecomp.log"))
    n = len(open(os.path.join(GEN, "rsp", "n_aspMain.cpp")).read().splitlines())
    print(f"rsp: {n} lines of n_aspMain.cpp")


if __name__ == "__main__":
    main(sys.argv[1:])
