#!/usr/bin/env python3
"""Check a directory (the split-build app, a release zip's contents) for bytes copied from the user's ROM.

Indexes 32-byte windows of the ROM (big-endian, every 4th offset) and of the decompressed code blob, then slides over
every byte offset of every file under DIR looking them up. Any copy of 35 or more consecutive ROM bytes contains one
indexed window, so it is found at any alignment. Windows with fewer than 6 distinct byte values (zero padding, fill
patterns) are ignored, and the number of ignored hits is reported.

usage: python3 -I tools/scan_bundle.py ROM.z64 BLOB.bin [BLOB2.bin ...] DIR [--sanity]     (full scan: MAINTAINER CHECK, needs the ROM, local only)
       python3 -I tools/scan_bundle.py --no-rom DIR [--max-engine-mb N] [--classes us11,us10] [--runtime DIR] [--menu-fonts DIR]
                                       [--toolchain-archive ZIP | --toolchain-ref DIR]       (the ROM-free gate: layered checks; CI runs it)
       python3 -I tools/scan_bundle.py --self-test                                          (the gate's own regression tests, no ROM)
Run it through the game's wrapper (decomp/thps<N>/recomp-host/tools/scan_bundle.py): the gate then knows the game's recipe classes
and checks the bundled recipes and NOTICE against the committed ones.
  --sanity   first plant 64 ROM bytes in a temp copy of DIR and require the scanner to find them (proves it detects)
A run of 64+ bytes that is a 16-bit counting table (char_table: k, k+1, k+2 ...; UTF-16 alphabets, code maps) is reported as
"counting table", not COPY: SDL2, LLVM, FreeType/RmlUi and the menu's fonts carry the same tables as the ROMs.
  --no-rom   the ROM-free release gate (release.yml). It cannot compare bytes with the ROM, so it is a set of LAYERED CHECKS that
             together make generated game code in the bundle unlikely; it is not a proof (the full ROM scan below stays required):
               0. the package INVENTORY: exactly the expected files for the game (engine, SDL2, tools, SDK, recipes of every class,
                  menu documents and fonts, licences; Windows: the DLLs and the toolchain), no missing and no extra file; and the
                  CONTENTS of what has a trusted source: the SDK sources, game hooks, menu, recipes and NOTICE equal the host and game
                  trees; with --runtime the SDK headers, fonts and runtime licences equal the runtime tree's; on Windows the bundled
                  llvm-mingw equals the pinned archive file for file (--toolchain-archive, SHA-256 checked against tools/mingw_check.py,
                  or --toolchain-ref, the extracted copy that `mingw_check.py --fetch-toolchain` left: then the archive it kept in
                  WORK/dl is what is hashed and compared, never the extracted files or their marker);
               1. every file is on an allowlist (no RecompiledFuncs/, no .c/.o/.a, no n_aspMain.cpp, no codeblob, no ROM);
               2. no Mach-O in the bundle defines a symbol that is a game function: func_8xxxxxxx, a "<class>_" prefixed name
                  (namespace.h), or any name from the bundled symbol file; for a PE (Windows) the same over its export table and
                  COFF symbol table (a clang-cl exe has neither beyond a few exports: its symbols are in the PDB, which is not
                  shipped, so on Windows the size budget and the full ROM scan carry more of the proof);
               3. the engine (each Mach-O slice, or the .exe) stays under its size budget: the measured size of that platform's split engine plus
                  1.5 MB (ENGINE_MEASURED_MB below; --max-engine-mb overrides). The translated game is only ~2.1 MB per class, so the budget
                  must be tighter than that. A tripwire: re-measure and raise it deliberately when engine code lands;
               3b. no binary contains the game-library marker (a string compiled only into game libraries: game-lib/game_lib.cpp);
               4. the recipe files have the expected shape (symbol file: names, addresses, sizes; blob.txt: hashes only; THPS2/3 overlays.txt:
                  `o`/`f` lines of offsets, sizes and names, one XXH3 fingerprint).
             It does NOT replace the full scan: run the ROM scan locally on the exact zip before any public release. Neither can
             prove that nothing derived reached a text or compressed file: that rests on the reviewed sources (the export inventory).
Layouts: macOS   <name>.app/Contents/{MacOS/thps<N>_recomp, Frameworks/, Resources/...}
         Windows THPS<N>-64Recomp.exe, SDL2.dll, dxil.dll, dxcompiler.dll, resources/... (package_split.py on Windows), incl.
                 resources/toolchain/ (the bundled llvm-mingw: allowed as a tree with fixed top-level names; its PE files get check 2,
                 and it may hold no generated-code file names).
Run it on the finished bundle (the unzipped zip). ROM and blob stay local; the report has counts and file names only.
"""
import os
import shutil
import sys
import tempfile

W = 32


def index_windows(data, step, out, tag):
    for i in range(0, len(data) - W + 1, step):
        out.setdefault(data[i:i + W], (tag, i))


SRC = {}


def run_length(data, i, tag, off):
    """(length, bytes before i) of the common run around data[i:i+W] and the source at `off` (extended both ways)."""
    src = SRC[tag]
    a = 0
    while i + a < len(data) and off + a < len(src) and data[i + a] == src[off + a]:
        a += 1
    b = 0
    while i - b - 1 >= 0 and off - b - 1 >= 0 and data[i - b - 1] == src[off - b - 1]:
        b += 1
    return a + b, b


def scan_file(path, idx, min_distinct=6):
    with open(path, "rb") as f:
        data = f.read()
    hits, ignored = [], 0
    if len(data) < W:
        return hits, ignored
    last = -W
    for i in range(0, len(data) - W + 1):
        w = data[i:i + W]
        t = idx.get(w)
        if t is not None:
            if len(set(w)) < min_distinct:
                ignored += 1
            elif i >= last + W:
                n, back = run_length(data, i, t[0], t[1])
                hits.append((i, t, n, back))
                last = i + n
    return hits, ignored


def char_table(tag, start, n):
    """True when a run is a 16-bit counting table: read as 16-bit units (either byte order, either alignment), between any leading
    and trailing zeros it is k, k+1, k+2 ... for at least 8 units (zeros plus a count: THPS2/3's code blobs have one). UTF-16 alphabets (0x20, 0x21 ... 0x7E), character-code maps and identity
    tables have this shape, and SDL2, LLVM, FreeType/RmlUi and the menu's fonts carry the same bytes as the games' ROMs (2026-10-12:
    every run of 64+ bytes in the Windows zips, THPS1-3, was one). Such a run carries no game content."""
    run = SRC[tag][start:start + n]
    for align in (0, 1):
        for order in ("big", "little"):
            units = [int.from_bytes(run[o:o + 2], order) for o in range(align, len(run) - 1, 2)]
            while units and units[0] == 0:
                units.pop(0)
            while units and units[-1] == 0:
                units.pop()
            if len(units) >= 8 and all(y == x + 1 for x, y in zip(units, units[1:])):
                return True
    return False


def scan_dir(d, idx):
    total_files = total_bytes = 0
    findings, ignored = [], 0
    for root, _, files in os.walk(d):
        for n in sorted(files):
            p = os.path.join(root, n)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            total_files += 1
            total_bytes += os.path.getsize(p)
            h, ig = scan_file(p, idx)
            ignored += ig
            if h:
                findings.append((os.path.relpath(p, d), h))
    return total_files, total_bytes, findings, ignored


# ---------------------------------------------------------------------------------------------------------------------------
# The ROM-free gate (--no-rom): layered checks. See the module docstring.

import re
import subprocess

APP = r"[^/]+\.app/Contents"
# What the resources folder holds, the same on both layouts (macOS Contents/Resources, Windows resources/). {EXE}: ".exe" on Windows.
RES_ALLOW = [
    r"(AppIcon\.icns|NOTICE|[A-Za-z0-9_.-]+\.(txt|md))",
    r"licenses/[A-Za-z0-9_.-]+",
    r"tools/(N64Recomp|RSPRecomp){EXE}",
    r"sdk/include/(recomp\.h|sse2neon\.h|librecomp/(sections\.h|rsp\.hpp|rsp_vu\.hpp|rsp_vu_impl\.hpp)|ultramodern/ultra64\.h)",
    r"sdk/src/(game_abi\.h|game_imports\.def|game_hooks\.def|thps_hooks\.h)",
    r"sdk/game/(game_lib\.cpp|game_rsp\.cpp)",
    r"classes/[a-z0-9]+/(thps[0-9]\.host\.toml|thps[0-9]\.host\.syms\.toml|blob\.txt|overlays\.txt|thps[0-9]\.aspMain\.toml)",
    # the settings menu (THPS_MENU, tools/menu_assets.py): documents, styles, icons, the OFL fonts and their licence
    # By exact name (2026-10-12; was *.rcss / *.svg / *.ttf): a new file in the menu must be added here on purpose. Keep in step with
    # decomp/common/host/ui/menu and tools/menu_assets.py (FONTS).
    r"ui/(hint\.rml|settings\.rml|recomp\.rcss|rml\.rcss|icons/X\.svg"
    r"|fonts/(LatoLatin-(Regular|Italic|Bold|BoldItalic)|NotoEmoji-Regular|promptfont)\.ttf|fonts/LICENSE\.txt)",
]
ALLOW_MAC = [re.compile("^" + p + "$") for p in [
    r"portable\.txt",
    r"[^/]+\.(txt|md)",                                  # README / NOTICE / licences beside the .app (release-docs)
    rf"{APP}/Info\.plist",
    rf"{APP}/PkgInfo",
    rf"{APP}/_CodeSignature/CodeResources",
    rf"{APP}/MacOS/thps[0-9]_recomp",
    rf"{APP}/Frameworks/libSDL2-2\.0\.0\.dylib",
] + [rf"{APP}/Resources/" + p.replace("{EXE}", "") for p in RES_ALLOW]]
ALLOW_WIN = [re.compile("^" + p + "$", re.I) for p in [
    r"portable\.txt",
    r"[^/]+\.(txt|md)",
    r"THPS[0-9]-64Recomp\.exe",                          # the engine, named after the product (game.toml `product`)
    r"(SDL2|dxil|dxcompiler)\.dll",                      # SDL2 (the build's own copy, see thps_host.cmake) and the DirectX shader compiler
] + [r"resources/" + p.replace("{EXE}", r"\.exe") for p in RES_ALLOW]]
# The bundled llvm-mingw (package_split.py --toolchain): thousands of upstream files, allowed as a tree. Its top level is fixed (the
# other targets' sysroots and its Python are trimmed), and it is checked for generated-code names and game symbols like the rest.
TOOLCHAIN_TOP = {"bin", "include", "lib", "share", "x86_64-w64-mingw32", "LICENSE.TXT", "README.md"}
# Engine size budgets (2026-10-12): each is the MEASURED size of that platform's split engine plus ENGINE_MARGIN_MB. The translated game adds
# only about 2.1 MB per code class, so a budget looser than that stops catching a game that got linked into the engine (and on Windows,
# where the exe has no symbol table, nothing else would). Re-measure and edit these numbers together when engine code lands (the test is
# exactly this gate's "engine ... MB" note, printed for every bundle). Measured on the three games' Release split engines, the largest of them:
ENGINE_MARGIN_MB = 1.5
ENGINE_MEASURED_MB = {
    "macos-arm64": 12.75,    # THPS1/2/3 split engines (12,748,144 / 12,748,704 / 12,748,624 bytes), Release, own SDL2 2.32.10, macOS 13 target, 2026-10-12
    "macos-x86_64": 14.0,    # ESTIMATE, not measured (no x86_64 build was made): 12.75 x (10.4 / 9.5), the CI x86_64:arm64 size ratio of run 38185901512
                             # (before the settings menu). The first release run prints the real size in this gate's "engine x86_64" note: correct it then.
    "windows-x64": 12.8,                   # clang-cl RelWithDebInfo split engine with the menu (Windows run 2026-10-12, WINDOWS-RESULTS-2026-10-12.md);
                                           # the debug info is in the PDB, not the exe. The mingw cross-build (tools/mingw_check.py) is a different size.
}
# A string that exists only in game libraries (game-lib/game_lib.cpp, compiled on the user's machine at first run): no binary of a release
# bundle may contain it, whichever file the translated game was linked into. Keep in step with game_lib.cpp.
GAME_LIBRARY_MARKER = b"thps-game-library-marker/1"
MACHO_MAGIC = {b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}
FUNC_RE = re.compile(r"^_*func_8[0-9A-Fa-f]{7}\b")
SYMS_LINE = re.compile(r'^\s*\{ name = "([^"]+)", vram = 0x[0-9A-Fa-f]+, size = 0x[0-9A-Fa-f]+ \},?(  # host split)?$')
SYMS_OTHER = re.compile(r'^(\[\[section\]\]|name = "[^"]+"|rom = 0x[0-9A-Fa-f]+|vram = 0x[0-9A-Fa-f]+|size = 0x[0-9A-Fa-f]+|functions = \[|\])$')
# overlays.txt (THPS2/3, decomp/common/firstrun/gen_overlay_recipe.py): layout and a symbol map of the front-end screens. Header numbers, then per screen
# one `o` line (name, blob ROM offset, size, relocation stream ROM offset, stream size) and `f` lines (function offset, size, optional forced name).
HEX = r"0x[0-9A-Fa-f]{1,8}"
OVL_LINE = re.compile(rf"^(vbase {HEX}|stride {HEX}|rom_xxh3 0x[0-9a-f]{{16}}|o [A-Za-z0-9_]+ {HEX} {HEX} {HEX} {HEX}|f {HEX} {HEX}( [A-Za-z0-9_]+)?)$")
BLOB_LINE = re.compile(r"^(xxh3\.[A-Za-z0-9._-]+ = [0-9a-f]{16}|versions = [A-Za-z0-9._, -]+)$")


# ---------------------------------------------------------------------------------------------------------------------------
# 0. The package inventory and the contents of what has a trusted source.

SHARED = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))     # decomp/common/host
# What the generated C and the RSP code include: (path under <runtime>/lib, path under sdk/include). package_split.py copies these.
RUNTIME_HEADERS = [
    ("N64ModernRuntime/N64Recomp/include/recomp.h", "recomp.h"),
    ("N64ModernRuntime/librecomp/include/librecomp/sections.h", "librecomp/sections.h"),
    ("N64ModernRuntime/librecomp/include/librecomp/rsp.hpp", "librecomp/rsp.hpp"),
    ("N64ModernRuntime/librecomp/include/librecomp/rsp_vu.hpp", "librecomp/rsp_vu.hpp"),
    ("N64ModernRuntime/librecomp/include/librecomp/rsp_vu_impl.hpp", "librecomp/rsp_vu_impl.hpp"),
    ("N64ModernRuntime/ultramodern/include/ultramodern/ultra64.h", "ultramodern/ultra64.h"),
    ("N64ModernRuntime/thirdparty/sse2neon/sse2neon.h", "sse2neon.h"),
]
SDK_SRC = {"sdk/src/game_abi.h": "src/game_abi.h", "sdk/src/game_imports.def": "src/game_imports.def", "sdk/src/thps_hooks.h": "src/thps_hooks.h",
           "sdk/game/game_lib.cpp": "game-lib/game_lib.cpp", "sdk/game/game_rsp.cpp": "game-lib/game_rsp.cpp"}   # bundle -> SHARED
LICENSES = ["SDL2", "N64Recomp", "N64ModernRuntime", "RT64"]                  # resources/licenses/<n>.txt (release.yml's --license list)
LICENSES_WIN = ["DXC-dxil", "DXC-dxcompiler"]
RUNTIME_LICENSES = {"N64ModernRuntime": "N64ModernRuntime/COPYING", "RT64": "rt64/LICENSE"}   # under <runtime>/lib
# The bundled llvm-mingw: the pinned Windows-host archive minus these top-level entries (package_split.py trims them: other targets'
# sysroots, its Python, busybox).
TOOLCHAIN_SKIP = {"aarch64-w64-mingw32", "armv7-w64-mingw32", "i686-w64-mingw32", "python", "busybox"}
FIX_PACKAGE = "rebuild the package with tools/package_split.py (as release.yml does) from a clean checkout; do not edit a package by hand"


def menu_files():
    """The settings menu's files under resources/ui (menu_assets.py stages exactly these)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import menu_assets
    menu = os.path.join(SHARED, "ui", "menu")
    out = {}
    for root, _, names in os.walk(menu):
        for n in names:
            p = os.path.join(root, n)
            out["ui/" + os.path.relpath(p, menu).replace(os.sep, "/")] = p
    for f in menu_assets.FONTS:
        out["ui/fonts/" + os.path.basename(f)] = None          # from the runtime's ui-fonts (checked with --runtime / --menu-fonts)
    out["ui/fonts/LICENSE.txt"] = os.path.join(SHARED, "ui", "FONTS-LICENSE.txt")
    return out


def game_info(family):
    """(host dir or None, overlays, recipe classes or None) from the game's profile, when run through its wrapper."""
    host = os.environ.get("THPS_HOST")
    if host and os.path.isfile(os.path.join(host, "game", "game.toml")):
        import tomllib
        with open(os.path.join(host, "game", "game.toml"), "rb") as f:
            g = tomllib.load(f)
        if g.get("family") == family:
            rec = os.path.join(host, "recipes")
            classes = sorted(d for d in os.listdir(rec) if os.path.isfile(os.path.join(rec, d, "blob.txt"))) if os.path.isdir(rec) else []
            return host, bool(g.get("overlays")), classes
    return None, family in ("thps2", "thps3"), None


def expected_files(win, family, classes, overlays, app=None):
    """-> {bundle path: trusted source path or None} of a complete package, and the optional paths."""
    exe = ".exe" if win else ""
    if win:
        top = {f"{family.upper()}-64Recomp.exe": None, "SDL2.dll": None, "dxil.dll": None, "dxcompiler.dll": None}
        res = "resources/"
    else:
        c = f"{app}/Contents/"
        top = {c + "Info.plist": None, c + "PkgInfo": None, c + "_CodeSignature/CodeResources": None, c + f"MacOS/{family}_recomp": None,
               c + "Frameworks/libSDL2-2.0.0.dylib": None, c + "Resources/AppIcon.icns": None}
        res = c + "Resources/"
    r = {"NOTICE": None, f"tools/N64Recomp{exe}": None, f"tools/RSPRecomp{exe}": None, "sdk/src/game_hooks.def": None}
    r.update({k: os.path.join(SHARED, v) for k, v in SDK_SRC.items()})
    r.update({f"sdk/include/{dst}": None for _, dst in RUNTIME_HEADERS})
    for cls in classes:
        for f in (f"{family}.host.toml", f"{family}.host.syms.toml", "blob.txt", f"{family}.aspMain.toml") + (("overlays.txt",) if overlays else ()):
            r[f"classes/{cls}/{f}"] = None
    for n in LICENSES + (LICENSES_WIN if win else []):
        r[f"licenses/{n}.txt"] = None
    r.update(menu_files())
    out = dict(top)
    out.update({res + k: v for k, v in r.items()})
    return out, {"portable.txt"}


def sha256_file(p):
    import hashlib
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def same_file(a, b):
    return os.path.getsize(a) == os.path.getsize(b) and sha256_file(a) == sha256_file(b)


def pinned_toolchain_assets():
    """mingw_check.py's ASSETS {archive name: SHA-256}, read without importing it (it needs a game's wrapper)."""
    import ast
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "mingw_check.py"), encoding="utf-8").read()
    for node in ast.walk(ast.parse(src)):
        if isinstance(node, ast.Assign) and any(getattr(t, "id", None) == "ASSETS" for t in node.targets):
            return ast.literal_eval(node.value)
    return {}


def toolchain_reference(archive=None, ref_dir=None, pins=None):
    """-> ({path under the toolchain: SHA-256} of the pinned archive minus TOOLCHAIN_SKIP, a note) or (None, problem).
    The reference is always the ARCHIVE, authenticated by its SHA-256 pin (tools/mingw_check.py ASSETS). With --toolchain-ref (an
    extracted copy), the archive `mingw_check.py --fetch-toolchain` downloaded beside it (WORK/dl/<name>, for WORK/llvm-mingw/<dir>) is
    used: the extracted files and their .extracted-* marker are never trusted, since a file edited after extraction keeps the marker
    (Astra N4)."""
    import hashlib
    pins = {n: h for n, h in (pins if pins is not None else pinned_toolchain_assets()).items() if n.endswith(".zip")}   # the Windows-host asset
    if not archive and ref_dir:
        work = os.path.dirname(os.path.dirname(os.path.abspath(ref_dir)))
        found = [os.path.join(work, "dl", n) for n in pins if os.path.isfile(os.path.join(work, "dl", n))]
        if not found:
            return None, (f"{ref_dir}: the pinned archive it was extracted from ({', '.join(pins)}) is not in {os.path.join(work, 'dl')}; "
                          f"an extracted copy is only trusted through its archive's SHA-256 (its marker proves nothing about later edits)")
        archive = found[0]
    if not archive:
        return None, "no toolchain reference given"
    name = os.path.basename(archive)
    if name not in pins:
        return None, f"{archive} is not the pinned Windows llvm-mingw archive ({', '.join(pins)})"
    if sha256_file(archive) != pins[name]:
        return None, f"{archive}: SHA-256 differs from the pin in tools/mingw_check.py: not the pinned release"
    import zipfile
    ref = {}
    with zipfile.ZipFile(archive) as z:
        for i in z.infolist():
            if i.is_dir():
                continue
            rel = i.filename.split("/", 1)[1] if "/" in i.filename else i.filename
            if rel.split("/")[0] in TOOLCHAIN_SKIP:
                continue
            ref[rel] = hashlib.sha256(z.read(i)).hexdigest()
    return ref, f"{name} (SHA-256 pinned in mingw_check.py)" + (f", the archive of {ref_dir}" if ref_dir else "")


def compare_toolchain(files, tc_pre, ref):
    """Bundled toolchain files (bundle rel, path) against the reference {rel: sha}: missing, extra and different files."""
    problems = []
    got = {rel[len(tc_pre):]: p for rel, p in files if rel.lower().startswith(tc_pre)}
    for rel in sorted(set(got) - set(ref)):
        problems.append(f"toolchain file not in the pinned llvm-mingw archive: {tc_pre}{rel}. Fix: {FIX_PACKAGE}")
    for rel in sorted(set(ref) - set(got)):
        problems.append(f"toolchain file of the pinned archive missing from the bundle: {tc_pre}{rel}. Fix: {FIX_PACKAGE}")
    for rel in sorted(set(got) & set(ref)):
        if sha256_file(got[rel]) != ref[rel]:
            problems.append(f"toolchain file differs from the pinned llvm-mingw archive: {tc_pre}{rel}. Fix: {FIX_PACKAGE}")
    return problems


def inventory_check(files, win, opts):
    """Layer 0: the expected file set and, where a trusted source exists, the contents. -> (problems, notes)"""
    problems, notes = [], []
    rels = {rel: p for rel, p in files}
    if win:
        exe = [r for r in rels if re.fullmatch(r"THPS[0-9]-64Recomp\.exe", r, re.I)]
        family = exe[0][:5].lower() if exe else None
        app = None
    else:
        apps = sorted({r.split("/")[0] for r in rels if re.match(r"^[^/]+\.app/", r)})
        if len(apps) != 1:
            return [f"expected exactly one .app in the bundle, found {apps}. Fix: {FIX_PACKAGE}"], notes
        app = apps[0]
        eng = [r for r in rels if re.fullmatch(re.escape(app) + r"/Contents/MacOS/thps[0-9]_recomp", r)]
        family = eng[0].rsplit("/", 1)[1][:5] if eng else None
    if not family:
        return [f"no engine found (THPS<N>-64Recomp.exe or <app>/Contents/MacOS/thps<N>_recomp). Fix: {FIX_PACKAGE}"], notes
    host, overlays, classes = game_info(family)
    if opts.get("classes"):
        classes = opts["classes"]
    elif classes is None:
        classes = sorted({m.group(1) for r in rels for m in [re.search(r"/classes/([a-z0-9]+)/", r)] if m})
        notes.append("recipe classes taken from the bundle (not checked against the game's recipes: run the gate through the game's "
                     "wrapper, or pass --classes)")
    exp, optional = expected_files(win, family, classes, overlays, app)
    tc_pre = "resources/toolchain/"
    present = {r for r in rels if not (win and r.lower().startswith(tc_pre))}
    for r in sorted(set(exp) - present):
        problems.append(f"missing from the package: {r}. Fix: {FIX_PACKAGE}" + (
            " (the menu fonts come from tools/setup_runtime.py <runtime> --ui-only)" if "/ui/fonts/" in r or r.startswith("ui/fonts/") else
            " (pass every --license NAME=PATH release.yml passes)" if "/licenses/" in r else ""))
    for r in sorted(present - set(exp) - optional):
        problems.append(f"unexpected file in the package: {r}. Fix: {FIX_PACKAGE}; if the file is meant to ship, add it to "
                        f"expected_files() in tools/scan_bundle.py (and the allowlist) in a reviewed change")
    notes.append(f"inventory: {len(exp)} expected files for {family} (classes {', '.join(classes) or 'none'}"
                 + (", overlays" if overlays else "") + f"), {len(present)} present")
    # contents with a trusted source
    trusted = {r: src for r, src in exp.items() if src}
    res = (tc_pre[:-len("toolchain/")] if win else f"{app}/Contents/Resources/")
    if host:
        trusted[res + "NOTICE"] = os.path.join(host, "NOTICE")
        trusted[res + "sdk/src/game_hooks.def"] = os.path.join(host, "game", "game_hooks.def")
        for cls in classes:
            for f in (f"{family}.host.syms.toml", "blob.txt") + (("overlays.txt",) if overlays else ()):
                trusted[f"{res}classes/{cls}/{f}"] = os.path.join(host, "recipes", cls, f)
    rt = opts.get("runtime")
    fonts = opts.get("menu_fonts") or (os.path.join(rt, "ui-fonts") if rt else None)
    if rt:
        for src, dst in RUNTIME_HEADERS:
            trusted[f"{res}sdk/include/{dst}"] = os.path.join(rt, "lib", src)
        for n, src in RUNTIME_LICENSES.items():
            trusted[f"{res}licenses/{n}.txt"] = os.path.join(rt, "lib", src)
    if fonts:
        import menu_assets
        for f in menu_assets.FONTS:
            trusted[f"{res}ui/fonts/{os.path.basename(f)}"] = os.path.join(fonts, f)
    nchk = 0
    for r, src in sorted(trusted.items()):
        if r in rels and os.path.isfile(src):
            nchk += 1
            if not same_file(rels[r], src):
                problems.append(f"{r} differs from its source {src}. Fix: {FIX_PACKAGE} (from the same checkout and runtime)")
        elif r in rels:
            problems.append(f"{r}: its trusted source {src} is missing. Fix: run the gate in the checkout (and with the runtime) "
                            f"the package was built from")
    notes.append(f"contents: {nchk} files equal their sources in the host/game trees" + (" and the runtime" if rt else
                 " (SDK headers, fonts and runtime licences not compared: pass --runtime)"))
    if win:
        if opts.get("toolchain_archive") or opts.get("toolchain_ref"):
            ref, what = toolchain_reference(opts.get("toolchain_archive"), opts.get("toolchain_ref"))
            if ref is None:
                problems.append(f"{what}. Fix: pass the pinned archive (`python -I tools/mingw_check.py --fetch-toolchain` downloads and "
                                f"checks it, and keeps it in WORK/dl) with --toolchain-archive, or the extracted copy with --toolchain-ref "
                                f"while that archive is still in WORK/dl")
            else:
                problems += compare_toolchain(files, tc_pre, ref)
                notes.append(f"toolchain: {len(ref)} files compared with {what}")
        else:
            problems.append("the bundled toolchain was not compared with the pinned llvm-mingw archive. Fix: pass --toolchain-archive "
                            "<llvm-mingw-...-ucrt-x86_64.zip> or --toolchain-ref <the extracted llvm-mingw-... directory>")
    return problems, notes


def self_test():
    """The inventory and toolchain layers on synthetic bundles (no ROM, no build)."""
    fails = []

    def check(ok, what):
        print(f"  {'ok  ' if ok else 'FAIL'} {what}")
        if not ok:
            fails.append(what)

    tmp = tempfile.mkdtemp(prefix="scan-bundle-selftest-")
    saved = os.environ.pop("THPS_HOST", None)
    try:
        def make(win, classes=("us11", "us10")):
            d = tempfile.mkdtemp(dir=tmp)
            exp, _ = expected_files(win, "thps1", list(classes), False, "THPS1 64 Recompiled.app")
            for r, src in exp.items():
                p = os.path.join(d, r)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                if src and os.path.isfile(src):
                    shutil.copy2(src, p)
                else:
                    open(p, "w").write("x")
            return d

        def walk(d):
            out = []
            for root, _, names in os.walk(d):
                for n in names:
                    p = os.path.join(root, n)
                    out.append((os.path.relpath(p, d).replace(os.sep, "/"), p))
            return out

        opts = {"classes": ["us10", "us11"]}
        d = make(False)
        pr, _ = inventory_check(walk(d), False, opts)
        check(not pr, f"a complete macOS package passes the inventory ({pr[:2]})")
        os.remove(os.path.join(d, "THPS1 64 Recompiled.app/Contents/Resources/ui/fonts/promptfont.ttf"))
        pr, _ = inventory_check(walk(d), False, opts)
        check(any("missing from the package" in x and "promptfont" in x for x in pr), "a missing menu font fails")
        open(os.path.join(d, "THPS1 64 Recompiled.app/Contents/Resources/sdk/src/extra.c"), "w").write("x")
        pr, _ = inventory_check(walk(d), False, opts)
        check(any("unexpected file" in x for x in pr), "an unexpected file fails")
        d = make(False)
        os.remove(os.path.join(d, "THPS1 64 Recompiled.app/Contents/Resources/classes/us10/blob.txt"))
        pr, _ = inventory_check(walk(d), False, opts)
        check(any("classes/us10/blob.txt" in x for x in pr), "a missing recipe file of an expected class fails")
        d = make(False)
        open(os.path.join(d, "THPS1 64 Recompiled.app/Contents/Resources/sdk/game/game_lib.cpp"), "a").write("// changed\n")
        pr, _ = inventory_check(walk(d), False, opts)
        check(any("differs from its source" in x for x in pr), "an SDK source that differs from the host tree fails")
        d = make(True)
        tc = os.path.join(d, "resources", "toolchain")
        os.makedirs(os.path.join(tc, "bin"))
        open(os.path.join(tc, "bin", "clang.exe"), "w").write("clang")
        pr, _ = inventory_check(walk(d), True, opts)
        check(any("not compared with the pinned" in x for x in pr), "Windows: a toolchain not compared with the pinned archive fails")
        ref = {"bin/clang.exe": sha256_file(os.path.join(tc, "bin", "clang.exe"))}
        check(not compare_toolchain(walk(d), "resources/toolchain/", ref), "Windows: a toolchain equal to the reference passes")
        open(os.path.join(tc, "bin", "extra.exe"), "w").write("x")
        open(os.path.join(tc, "bin", "clang.exe"), "a").write("patched")
        pr = compare_toolchain(walk(d), "resources/toolchain/", ref)
        check(any("not in the pinned" in x for x in pr) and any("differs" in x for x in pr), "Windows: an extra or changed toolchain file fails")
        r, why = toolchain_reference(ref_dir=tc)
        check(r is None and "is not in" in why, "Windows: an extracted toolchain reference without its pinned archive is refused")
        # N4: an extracted reference whose header was edited after extraction (marker kept) must not vouch for itself
        import zipfile
        work = os.path.join(tmp, "work")
        name = "llvm-mingw-test-ucrt-x86_64.zip"
        os.makedirs(os.path.join(work, "dl"))
        arc = os.path.join(work, "dl", name)
        with zipfile.ZipFile(arc, "w") as z:
            z.writestr("llvm-mingw-test/bin/clang.exe", "clang")
            z.writestr("llvm-mingw-test/include/stdio.h", "int printf(const char *, ...);\n")
        pins = {name: sha256_file(arc)}
        ext = os.path.join(work, "llvm-mingw")
        with zipfile.ZipFile(arc) as z:
            z.extractall(ext)
        open(os.path.join(ext, ".extracted-" + name), "w").close()
        ref_dir = os.path.join(ext, "llvm-mingw-test")
        d = make(True)
        shutil.copytree(ref_dir, os.path.join(d, "resources", "toolchain"))
        r, why = toolchain_reference(ref_dir=ref_dir, pins=pins)
        check(r is not None and not compare_toolchain(walk(d), "resources/toolchain/", r),
              f"Windows: a bundle equal to the pinned archive passes through --toolchain-ref ({why})")
        with open(os.path.join(ref_dir, "include", "stdio.h"), "a") as f:
            f.write("/* edited after extraction */\n")
        d = make(True)
        shutil.copytree(ref_dir, os.path.join(d, "resources", "toolchain"))
        r, why = toolchain_reference(ref_dir=ref_dir, pins=pins)
        pr = compare_toolchain(walk(d), "resources/toolchain/", r) if r else [why]
        check(any("include/stdio.h" in x and "differs" in x for x in pr),
              "Windows: a header edited in the extracted reference (marker kept) fails: the archive is the reference")
        with open(arc, "ab") as f:
            f.write(b"x")
        r, why = toolchain_reference(ref_dir=ref_dir, pins=pins)
        check(r is None and "SHA-256 differs" in why, "Windows: an archive that no longer matches its pin is refused")
    finally:
        if saved is not None:
            os.environ["THPS_HOST"] = saved
        shutil.rmtree(tmp, ignore_errors=True)
    if fails:
        print(f"SELF-TEST FAILED: {len(fails)} case(s). Fix: tools/scan_bundle.py")
        return 1
    print("self-test passed")
    return 0


def pe_symbols(path):
    """(is_pe, exported names, COFF symbol names) of a PE image, read with the standard library."""
    import struct
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ" or len(data) < 0x40:
        return False, [], []
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return False, [], []
    nsect, _, symptr, nsyms, optsize = struct.unpack_from("<HIIIH", data, pe + 6)
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic not in (0x10B, 0x20B):
        return True, [], []
    ddir = opt + (112 if magic == 0x20B else 96)
    sects = []
    for i in range(nsect):
        o = opt + optsize + 40 * i
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", data, o + 8)
        sects.append((va, max(vsize, rsize), rptr))

    def off(rva):
        for va, sz, rp in sects:
            if va <= rva < va + sz:
                return rva - va + rp
        return None

    def cstr(o):
        if o is None or o >= len(data):
            return ""
        e = data.find(b"\0", o)
        return data[o:e if e >= 0 else len(data)].decode("latin-1")

    exports = []
    erva = struct.unpack_from("<I", data, ddir)[0]
    eo = off(erva) if erva else None
    if eo is not None:
        nnames, _, names_rva = struct.unpack_from("<III", data, eo + 24)
        no = off(names_rva)
        for i in range(nnames if no is not None else 0):
            exports.append(cstr(off(struct.unpack_from("<I", data, no + 4 * i)[0])))
    coff = []
    if symptr and nsyms and symptr + 18 * nsyms <= len(data):
        strtab = symptr + 18 * nsyms
        i = 0
        while i < nsyms:
            raw = data[symptr + 18 * i: symptr + 18 * i + 8]
            naux = data[symptr + 18 * i + 17]
            if raw[:4] == b"\0\0\0\0":
                coff.append(cstr(strtab + struct.unpack_from("<I", raw, 4)[0]))
            else:
                coff.append(raw.rstrip(b"\0").decode("latin-1"))
            i += 1 + naux
    return True, exports, coff


def tool(*cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"{' '.join(cmd)}: {r.stderr.strip()}")
    return r.stdout


def macho_archs(path):
    try:
        return tool("lipo", "-archs", path).split()
    except Exception:
        return []


def symbol_names(path, arch, defined_only):
    cmd = ["nm", "-j", "-arch", arch] + (["-U"] if defined_only else []) + [path]
    return [l.strip() for l in tool(*cmd).splitlines() if l.strip() and not l.startswith(path)]


def slice_size(path, arch, tmp):
    if len(macho_archs(path)) == 1:   # a thin binary (an arm64-only package): lipo -thin only takes fat files
        return os.path.getsize(path)
    out = os.path.join(tmp, f"{os.path.basename(path)}.{arch}")
    tool("lipo", path, "-thin", arch, "-output", out)
    n = os.path.getsize(out)
    os.remove(out)
    return n


def structural_gate(argv):
    args, opts = [], {}
    valued = {"--max-engine-mb", "--classes", "--runtime", "--menu-fonts", "--toolchain-archive", "--toolchain-ref"}
    it = iter(argv)
    for a in it:
        if a in valued:
            opts[a[2:].replace("-", "_")] = next(it, None)
        elif not a.startswith("--"):
            args.append(a)
    if len(args) != 1:
        sys.exit(__doc__)
    max_engine_mb = float(opts["max_engine_mb"]) if opts.get("max_engine_mb") else None
    if opts.get("classes"):
        opts["classes"] = sorted(c for c in opts["classes"].split(",") if c)
    d = args[0]
    problems, notes = [], []

    files = []
    for root, _, names in os.walk(d):
        for n in sorted(names):
            p = os.path.join(root, n)
            rel = os.path.relpath(p, d).replace(os.sep, "/")
            if os.path.islink(p):
                continue   # codesign leaves none we care about; symlinks carry no bytes
            files.append((rel, p))
    win = any(re.fullmatch(r"THPS[0-9]-64Recomp\.exe", rel, re.I) for rel, _ in files)
    allow = ALLOW_WIN if win else ALLOW_MAC
    user_limit = max_engine_mb
    def engine_limit(key):
        return user_limit if user_limit is not None else ENGINE_MEASURED_MB[key] + ENGINE_MARGIN_MB
    tc_pre = "resources/toolchain/"
    print(f"ROM-free gate (layered checks; contributors and CI can run it) over {len(files)} files under {d} ({'Windows' if win else 'macOS'} layout)")

    # 0. inventory and contents
    p0, n0 = inventory_check(files, win, opts)
    problems += p0
    notes += n0

    # 1. allowlist
    ntc = 0
    for rel, _ in files:
        if win and rel.lower().startswith(tc_pre):
            ntc += 1
            if rel[len(tc_pre):].split("/")[0] not in TOOLCHAIN_TOP:
                problems.append(f"not an llvm-mingw top-level entry: {rel}")
            continue
        if not any(r.match(rel) for r in allow):
            problems.append(f"not on the allowlist: {rel}")
    if win:
        notes.append(f"{ntc} files in the bundled toolchain (resources/toolchain)")
        if not any(rel.lower() == tc_pre + "bin/clang.exe" for rel, _ in files):
            problems.append("no resources/toolchain/bin/clang.exe: the first run could not build (package_split.py --toolchain)")
        # The shader-compiler DLLs must be the pinned DXC v1.8.2407 pair, with Microsoft's texts for that release (tools/dxc_pins.py).
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import dxc_pins
        top = {rel.lower(): p for rel, p in files}
        for n, want in dxc_pins.DLL_SHA256.items():
            p = top.get(n)
            if p is None:
                problems.append(f"no {n} beside the exe: the bundle needs DXC {dxc_pins.TAG}'s dxil.dll and dxcompiler.dll")
            elif dxc_pins.sha256_file(p) != want:
                problems.append(f"{n} is not the pinned DXC {dxc_pins.TAG} file (SHA-256 {want[:12]}...): dxil.dll and dxcompiler.dll must be the {dxc_pins.TAG} pair")
        for n, want in dxc_pins.LICENSE_SHA256.items():
            p = top.get("resources/licenses/" + n.lower())
            if p is None:
                problems.append(f"resources/licenses/{n} is missing")
            elif dxc_pins.sha256_file(p) != want:
                problems.append(f"resources/licenses/{n} is not Microsoft's DXC {dxc_pins.TAG} text")
        notes.append(f"DXC {dxc_pins.TAG} dxil.dll + dxcompiler.dll pair and licence texts checked")
    for rel, _ in files:
        low = rel.lower()
        in_tc = win and low.startswith(tc_pre)
        if "recompiledfuncs" in low or "codeblob" in low or "n_aspmain" in low or low.endswith((".z64", ".n64", ".v64", ".rom")) \
                or re.search(r"(^|/)(funcs_[0-9]+|thps[0-9]_game_[a-z0-9]+|game_rsp|overlay_[a-z0-9_]+)\.(c|o|obj|dll|dylib|so)$", low) \
                or (not in_tc and low.endswith((".c", ".o", ".a", ".bin", ".obj", ".lib", ".pdb"))):
            problems.append(f"generated-code or ROM-like file: {rel}")

    # 2/3. Mach-O files
    classes = sorted({m.group(1) for rel, _ in files for m in [re.search(r"/classes/([a-z0-9]+)/", rel)] if m})
    prefixes = sorted(set(classes) | {"us10", "us11"})
    prefix_re = re.compile(r"^_*(" + "|".join(prefixes) + r")_[A-Za-z]")
    game_names = set()
    for rel, p in files:
        if re.search(r"thps[0-9]\.host\.syms\.toml$", rel):
            for line in open(p, encoding="utf-8"):
                m = SYMS_LINE.match(line.rstrip("\n"))
                if m:
                    game_names.add(m.group(1))
    # libultra API names (osCreateThread, osViSetMode ...) are in the symbol file because the game links its own libultra, but the engine
    # provides them natively (ultramodern): they are declared in the bundled ultra64.h. Everything else in the symbol file must be absent.
    api = set()
    for rel, p in files:
        if rel.endswith("/sdk/include/ultramodern/ultra64.h"):
            api |= set(re.findall(r"\b(_{0,2}os[A-Za-z0-9_]+)\s*\(", open(p, encoding="utf-8", errors="replace").read()))
    if not api:
        problems.append("the bundle has no sdk/include/ultramodern/ultra64.h to tell runtime libultra names from game functions")
    game_names -= api
    notes.append(f"{len(game_names)} game function names (symbol file minus {len(api)} runtime libultra names) loaded as the 'must not be defined' set")
    tmp = tempfile.mkdtemp(prefix="gate-")
    macho = npe = 0
    try:
        for rel, p in files:
            with open(p, "rb") as f:
                magic = f.read(4)
                if magic[:2] == b"MZ" or magic in MACHO_MAGIC:
                    f.seek(0)
                    if GAME_LIBRARY_MARKER in f.read():
                        problems.append(f"{rel}: contains the game-library marker: a game library, or code linked from one, is in the bundle")
            if magic[:2] == b"MZ":
                is_pe, exports, coff = pe_symbols(p)
                if not is_pe:
                    continue
                npe += 1
                names = exports + coff
                bad_func = [s for s in names if FUNC_RE.match(s)]
                bad_pre = [s for s in names if prefix_re.match(s)]
                # The bundled toolchain's runtime DLLs export C library names (memcpy, strlen ...) that the game's own libc also has in the
                # symbol file: for those upstream files only the func_8xxxxxxx and class-prefix checks apply.
                bad_name = [] if rel.lower().startswith(tc_pre) else sorted({s for s in names if s.lstrip("_") in game_names})
                for label, bad in (("func_8xxxxxxx symbols", bad_func), ("class-prefixed (namespace.h) symbols", bad_pre), ("symbols named like game functions", bad_name)):
                    if bad:
                        problems.append(f"{rel}: {len(bad)} {label}, e.g. {sorted(bad)[:3]}")
                if re.fullmatch(r"THPS[0-9]-64Recomp\.exe", rel, re.I):
                    mb, lim = os.path.getsize(p) / 1e6, engine_limit("windows-x64")
                    notes.append(f"engine x86_64 (PE): {mb:.1f} MB, {len(exports)} exports, {len(coff)} COFF symbols (budget {lim:g} MB)")
                    if mb > lim:
                        problems.append(f"{rel}: {mb:.1f} MB is over the {lim:g} MB engine budget")
                continue
            if magic not in MACHO_MAGIC:
                continue
            archs = macho_archs(p)
            if not archs:
                problems.append(f"{rel}: Mach-O but lipo cannot read its architectures")
                continue
            macho += 1
            for a in archs:
                allsyms = symbol_names(p, a, False)
                defined = set(symbol_names(p, a, True))
                bad_func = [s for s in allsyms if FUNC_RE.match(s)]
                bad_pre = [s for s in defined if prefix_re.match(s)]
                bad_name = sorted(s for s in defined if s.lstrip("_") in game_names)
                for label, bad in (("func_8xxxxxxx symbols", bad_func), ("class-prefixed (namespace.h) symbols", bad_pre), ("symbols named like game functions", bad_name)):
                    if bad:
                        problems.append(f"{rel} [{a}]: {len(bad)} {label}, e.g. {sorted(bad)[:3]}")
                if re.search(r"/MacOS/thps[0-9]_recomp$", rel):
                    mb, lim = slice_size(p, a, tmp) / 1e6, engine_limit(f"macos-{a}")
                    notes.append(f"engine {a}: {mb:.1f} MB, {len(defined)} defined symbols (budget {lim:g} MB)")
                    if mb > lim:
                        problems.append(f"{rel} [{a}]: {mb:.1f} MB is over the {lim:g} MB engine budget")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if not win and not any(re.search(r"/MacOS/thps[0-9]_recomp$", rel) for rel, _ in files):
        problems.append("no engine binary (Contents/MacOS/thps<N>_recomp) in the bundle")
    notes.append(f"{npe} PE files checked" if win else f"{macho} Mach-O files checked")

    # 4. recipe shape
    for rel, p in files:
        if re.search(r"/thps[0-9]\.host\.syms\.toml$", rel):
            n = 0
            for i, line in enumerate(open(p, encoding="utf-8"), 1):
                line = line.rstrip("\n")
                if not line.strip() or line.startswith("#"):
                    continue
                n += 1
                if not (SYMS_LINE.match(line) or SYMS_OTHER.match(line)):
                    problems.append(f"{rel}:{i}: unexpected line in a symbol file (names, addresses and sizes only): {line[:60]!r}")
                    break
            if os.path.getsize(p) > 400_000:
                problems.append(f"{rel}: {os.path.getsize(p)} bytes is larger than a symbol file should be")
        elif rel.endswith("/overlays.txt"):
            for i, line in enumerate(open(p, encoding="utf-8"), 1):
                line = line.rstrip("\n")
                if not line.strip() or line.startswith("#"):
                    continue
                if not OVL_LINE.match(line):
                    problems.append(f"{rel}:{i}: unexpected line in an overlay recipe (offsets, sizes and names only): {line[:60]!r}")
                    break
            if os.path.getsize(p) > 40_000:
                problems.append(f"{rel}: {os.path.getsize(p)} bytes is larger than an overlay recipe should be")
        elif rel.endswith("/blob.txt"):
            for i, line in enumerate(open(p, encoding="utf-8"), 1):
                if line.strip() and not BLOB_LINE.match(line.strip()):
                    problems.append(f"{rel}:{i}: blob.txt holds hashes and version ids only: {line.strip()[:60]!r}")
                    break
        elif re.search(r"/thps[0-9]\.(host|aspMain)\.toml$", rel):
            if 'rom_file_path = "codeblob.bin"' not in open(p, encoding="utf-8").read():
                problems.append(f"{rel}: expected the relative rom_file_path = \"codeblob.bin\"")

    for n in notes:
        print("  " + n)
    if problems:
        print("GATE FAILED (each line: what failed, and how to fix it):")
        for pr in problems:
            print("  - " + pr + ("" if "Fix:" in pr else f". Fix: {FIX_PACKAGE}"))
        return 1
    print("gate passed: layered checks (inventory and contents, allowlist, symbols, engine size budget, game-library marker, "
          "recipe shape) found no generated game code. This is not a byte comparison: a maintainer runs the full ROM scan on the "
          "exact zip before any public release (it needs the ROMs)")
    return 0


def main():
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    if "--no-rom" in sys.argv:
        sys.exit(structural_gate(sys.argv[1:]))
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 3:
        sys.exit(__doc__)
    rom_p, blob_ps, d = args[0], args[1:-1], args[-1]
    rom = open(rom_p, "rb").read()
    idx = {}
    SRC["rom"] = rom
    index_windows(rom, 4, idx, "rom")
    nblob = 0
    for i, bp in enumerate(blob_ps):
        blob = open(bp, "rb").read()
        nblob += len(blob)
        tag = "blob" if i == 0 else f"blob{i + 1}"
        SRC[tag] = blob
        index_windows(blob, 4, idx, tag)
    print(f"indexed {len(idx)} windows ({len(rom)} ROM bytes, {nblob} bytes in {len(blob_ps)} blob(s), every 4th offset, {W}-byte windows)")
    if "--sanity" in sys.argv:
        tmp = tempfile.mkdtemp(prefix="scan-sanity-")
        try:
            shutil.copytree(d, os.path.join(tmp, "b"), symlinks=True)
            with open(os.path.join(tmp, "b", "planted.bin"), "wb") as f:
                f.write(b"\x00" * 7 + rom[0x20000:0x20000 + 64] + b"\x01" * 5)
            nf, nb, fnd, ig = scan_dir(os.path.join(tmp, "b"), idx)
            ok = any(name == "planted.bin" and any(h[2] >= 64 for h in hs) for name, hs in fnd)
            print(f"sanity: planted 64 ROM bytes at an odd offset -> {'FOUND' if ok else 'NOT FOUND (scanner broken)'}")
            if not ok:
                sys.exit(2)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    nf, nb, findings, ignored = scan_dir(d, idx)
    print(f"scanned {nf} files, {nb} bytes under {d}")
    print(f"ignored low-entropy matches: {ignored}")
    long_hits, tables = [], 0
    for name, hits in findings:
        for i, t, n, back in hits:
            if n >= 64 and char_table(t[0], t[1] - back, n):
                tag = "counting table"
                tables += 1
            else:
                tag = "COPY" if n >= 64 else "short coincidence"
            print(f"  {tag}: {name} offset {i - back}: {n}-byte run equal to {t[0]} offset {t[1] - back}")
            if tag == "COPY":
                long_hits.append((name, i, n))
    if long_hits:
        print(f"{len(long_hits)} run(s) of 64 or more ROM-derived bytes that are not counting tables: see COPY above. Fix: the named "
              f"files hold bytes of the ROM; find what put them there (a generated file, a converted asset) and rebuild without it. "
              f"Never publish this package (maintainer check: needs the ROM)")
        sys.exit(1)
    print("no run of 64 or more ROM-derived bytes found" + (f" apart from {tables} 16-bit counting tables (UTF-16 alphabets, code maps; "
          "the same bytes are in SDL2, LLVM and the fonts)" if tables else "") + (" (short coincidences above are common tables, e.g. UTF-16 alphabets)" if findings else ""))


if __name__ == "__main__":
    main()
