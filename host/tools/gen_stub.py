#!/usr/bin/env python3
"""Generate STUB RecompiledFuncs: no ROM, no recompiled code, same link surface as tools/gen.py.

For CI and platform builds (cmake -DTHPS_STUB_FUNCS=ON): the host, N64ModernRuntime and RT64 get built and
linked with clang-cl / gcc / clang on a machine that has no ROM. The executable cannot play a game: every
recompiled function is an empty RECOMP_FUNC. (The front-end overlays of the real build are not stubbed: base
sections only.)

Inputs are public facts only: the class's symbol file (names, addresses, sizes: decomp/thps<N>/recomp/*.syms.toml,
which is committed), versions/<class>/host_force.toml (forced names, COP2 ignores) and the N64Recomp
symbol lists (which libultra names the runtime provides). Output, under build/gen-stub/ (or --out):

  thps_versions.h                       as tools/gen.py (tools/gen_versions.py header)
  <class>/RecompiledFuncs/funcs.h        extern declarations (recomp_entrypoint, every function, the runtime's *_recomp)
  <class>/RecompiledFuncs/funcs_N.c      one empty RECOMP_FUNC per function the real N64Recomp output would define
  <class>/RecompiledFuncs/recomp_overlays.inl   the section table (same shape as N64Recomp's)
  <class>/namespace.h                    tools/gen_versions.py namespace
  rsp/n_aspMain.cpp                      returns RspExitReason::Broke

  .gen-stub-stamp                       a digest of the inputs (for --if-stale)

usage: python3 -I tools/gen_stub.py --runtime DIR [--out DIR] [--if-stale] [CLASS ...]
  --runtime DIR   the runtime tree (setup_runtime.py DEST): lib/N64ModernRuntime/N64Recomp/src/symbol_lists.cpp is read
  --if-stale      do nothing when the stamp in DIR matches the inputs (CMake runs this at every stub configure, so an older
                  checkout's build/gen-stub is regenerated instead of failing with "header not found")
"""
import hashlib
import os
import re
import subprocess
import sys
import tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostenv  # noqa: E402

HOST = hostenv.host_dir()
HERE = os.path.join(HOST, "tools")                 # the game's wrappers (gen_versions.py)
GAMEDIR = os.path.dirname(HOST)
PY = [sys.executable, "-I"]
CHUNK = 400  # functions per funcs_N.c

SET_RE = re.compile(r"N64Recomp::(reimplemented_funcs|ignored_funcs|renamed_funcs)\s*\{(.*?)\n\};", re.S)


def symbol_lists_path(runtime):
    return os.path.join(runtime, "lib", "N64ModernRuntime", "N64Recomp", "src", "symbol_lists.cpp")


def inputs_digest(runtime, classes):
    """Every file the output depends on: this script, the version tables tool, each class's symbols and forced names,
    and the runtime's symbol lists."""
    shared = os.path.dirname(os.path.abspath(__file__))
    paths = [os.path.abspath(__file__), os.path.join(shared, "gen_versions.py"), os.path.join(HERE, "gen_versions.py"),
             symbol_lists_path(runtime)]
    for cls in classes:
        paths += [os.path.join(GAMEDIR, versions_tool("class-info", cls).split()[1]),
                  os.path.join(HOST, "versions", cls, "host_force.toml")]
    h = hashlib.sha256(" ".join(classes).encode())
    for p in paths:
        h.update(p.encode() + b"\0")
        if os.path.isfile(p):
            with open(p, "rb") as f:
                h.update(f.read())
    return h.hexdigest()


def symbol_lists(runtime):
    path = symbol_lists_path(runtime)
    if not os.path.exists(path):
        sys.exit(f"gen_stub: {path} not found (--runtime must hold lib/N64ModernRuntime; tools/setup_runtime.py)")
    text = open(path).read()
    out = {}
    for name, body in SET_RE.findall(text):
        lines = [l.split("//")[0] for l in body.splitlines()]
        out[name] = set(re.findall(r'"([^"]+)"', "\n".join(lines)))
    for k in ("reimplemented_funcs", "ignored_funcs", "renamed_funcs"):
        if not out.get(k):
            sys.exit(f"gen_stub: no {k} found in {path}")
    return out


def toml(path):
    with open(path, "rb") as f:
        return tomllib.load(f)


def versions_tool(*args):
    return subprocess.run(PY + [os.path.join(HERE, "gen_versions.py"), *args], check=True, capture_output=True, text=True).stdout.strip()


def gen_class(cls, out_root, lists):
    game, syms_rel = versions_tool("class-info", cls).split()
    syms = toml(os.path.join(GAMEDIR, syms_rel))
    force = toml(os.path.join(HOST, "versions", cls, "host_force.toml"))
    forced = {int(a, 16): n for a, n in force["force"].items()}
    cop2_ignored = set(force.get("cop2", {}).get("ignored", []))
    import thpsgame
    boot_vram = thpsgame.load(game).boot_vram

    d = os.path.join(out_root, cls)
    rf = os.path.join(d, "RecompiledFuncs")
    os.makedirs(rf, exist_ok=True)
    for f in os.listdir(rf):
        os.remove(os.path.join(rf, f))

    declared, defined, sections = [], [], []
    for sec in syms["section"]:
        entries = []
        for fn in sec.get("functions", []):
            vram, size, name = fn["vram"], fn["size"], forced.get(fn["vram"], fn["name"])
            if name in cop2_ignored:
                continue            # the real config ignores it: no definition, no table entry
            if vram == boot_vram:
                name, provided = "recomp_entrypoint", False
            elif name in lists["reimplemented_funcs"]:
                name, provided = name + "_recomp", True     # the runtime defines it
            elif name in lists["ignored_funcs"]:
                continue                                    # N64Recomp drops it from the output and the table
            elif name in lists["renamed_funcs"]:
                name, provided = name + "_recomp", False
            else:
                provided = False
            declared.append(name)
            if not provided:
                defined.append(name)
            entries.append((name, vram - sec["vram"], 0 if provided else size))
        if entries:
            sections.append((sec, entries))
    if not sections:
        sys.exit(f"gen_stub: class {cls}: no functions in {syms_rel}")

    with open(os.path.join(rf, "funcs.h"), "w") as f:
        f.write('#include "recomp.h"\n#include "thps_hooks.h"\n\n#ifdef __cplusplus\nextern "C" {\n#endif\n\n')
        seen = set()
        for n in declared:
            if n not in seen:
                seen.add(n)
                f.write(f"void {n}(uint8_t* rdram, recomp_context* ctx);\n")
        f.write("\n#ifdef __cplusplus\n}\n#endif\n")
    seen = set()
    uniq = [n for n in defined if not (n in seen or seen.add(n))]
    for i in range(0, len(uniq), CHUNK):
        with open(os.path.join(rf, f"funcs_{i // CHUNK}.c"), "w") as f:
            f.write('#include "recomp.h"\n#include "thps_hooks.h"\n#include "funcs.h"\n\n')
            for n in uniq[i:i + CHUNK]:
                f.write(f"RECOMP_FUNC void {n}(uint8_t* rdram, recomp_context* ctx) {{\n    (void)rdram;\n    (void)ctx;\n}}\n")
    with open(os.path.join(rf, "recomp_overlays.inl"), "w") as f:
        f.write('#include "recomp.h"\n#include "thps_hooks.h"\n#include "funcs.h"\n#include "librecomp/sections.h"\n\n')
        for i, (sec, entries) in enumerate(sections):
            var = f"section_{i}_{re.sub(r'[^A-Za-z0-9_]', '', sec['name'])}_funcs"
            f.write(f"static FuncEntry {var}[] = {{\n")
            for n, off, size in entries:
                f.write(f"    {{ .func = {n}, .offset = 0x{off:08X}, .rom_size = 0x{size:08X} }},\n")
            f.write("};\n")
        f.write("static SectionTableEntry section_table[] = {\n")
        for i, (sec, entries) in enumerate(sections):
            var = f"section_{i}_{re.sub(r'[^A-Za-z0-9_]', '', sec['name'])}_funcs"
            f.write(f"    {{ .rom_addr = 0x{sec['rom']:08X}, .ram_addr = 0x{sec['vram']:08X}, .size = 0x{sec['size']:08X}, .funcs = {var}, "
                    f".num_funcs = ARRLEN({var}), .relocs = nullptr, .num_relocs = 0, .index = {i} }},\n")
        f.write(f"}};\nconst size_t num_sections = {len(sections)};\nstatic int overlay_sections_by_index[] = {{\n    -1,\n}};\n")
    subprocess.run(PY + [os.path.join(HERE, "gen_versions.py"), "namespace", cls, rf, os.path.join(d, "namespace.h")], check=True)
    print(f"gen_stub: class {cls}: {len(uniq)} stub functions, {len(declared) - len(uniq)} provided by the runtime, {len(sections)} sections")


def main(argv):
    runtime, out, classes, if_stale = None, os.path.join(HOST, "build", "gen-stub"), [], False
    i = 0
    while i < len(argv):
        if argv[i] == "--if-stale":
            if_stale = True; i += 1
        elif argv[i] == "--runtime" and i + 1 < len(argv):
            runtime = os.path.abspath(argv[i + 1]); i += 2
        elif argv[i] == "--out" and i + 1 < len(argv):
            out = os.path.abspath(argv[i + 1]); i += 2
        elif argv[i].startswith("--"):
            sys.exit(__doc__)
        else:
            classes.append(argv[i]); i += 1
    if not runtime:
        sys.exit(__doc__)
    versions_tool("check")
    classes = classes or versions_tool("classes").split()
    stamp, digest = os.path.join(out, ".gen-stub-stamp"), inputs_digest(runtime, classes)
    if if_stale and os.path.isfile(stamp) and open(stamp).read().strip() == digest:
        print(f"gen_stub: {out} is up to date")
        return
    if os.path.isfile(stamp) or os.path.isfile(os.path.join(out, "thps_versions.h")):
        print(f"gen_stub: regenerating {out} (its inputs changed)")
    lists = symbol_lists(runtime)
    os.makedirs(os.path.join(out, "rsp"), exist_ok=True)
    versions_tool("header", os.path.join(out, "thps_versions.h"))
    for cls in classes:
        gen_class(cls, out, lists)
    with open(os.path.join(out, "rsp", "n_aspMain.cpp"), "w") as f:
        f.write('// STUB (tools/gen_stub.py): no audio microcode without the ROM.\n#include "librecomp/rsp.hpp"\n'
                "RspExitReason n_aspMain(uint8_t*, [[maybe_unused]] uint32_t ucode_addr) { return RspExitReason::Broke; }\n")
    with open(stamp, "w") as f:
        f.write(digest + "\n")
    print(f"gen_stub: wrote {out}")


if __name__ == "__main__":
    main(sys.argv[1:])
