#!/usr/bin/env python3
"""Assemble the split-build app: engine + recipe + tools + SDK headers, with no ROM-derived bytes.

Dev/release-time tool (the user never runs it). Needs the engine built with -DTHPS_SPLIT_GAME_LIB=ON, the
N64Recomp/RSPRecomp executables (pinned commit), the runtime tree (for the headers the generated C includes) and the
committed recipes (recipes/<class>/, see recipes/README.md). No ROM and no tools/gen.py output: the release CI runs this.

usage: python3 -I tools/package_split.py --engine build/cmake/thps1_recomp --tools <dir with N64Recomp, RSPRecomp>
           --runtime <recomp-runtime dir> --out build/split-pkg [--class us11 --class us10] [--version 0.1.0] [--lib libSDL2-2.0.0.dylib=PATH]
           [--license NAME=PATH ...] [--zip] [--app-name "THPS1 64 Recompiled"] [--toolchain <llvm-mingw dir> (Windows)]

  --class C         a code class to include (repeat). Default: every class that has a recipe (recipes/*), i.e. every release of this game
                    the app can build: THPS1 USA v1.1 and Europe (class us11), USA v1.0 (us10); THPS2 USA (us) and Europe (eu); THPS3 USA (us).
                    One package per game, carrying all of that game's classes.
  --engine may be a universal (lipo) binary; the archive name says macOS-arm64, macOS-x86_64 or macOS-universal.
  --lib NAME=PATH   resolves a "@rpath/NAME" dependency of the engine to PATH (our own SDL2 build, tools/build_sdl2.py).
                    A dependency that is neither a system library nor given here is an error: the bundle must not need
                    Homebrew (a path under /opt/homebrew or /usr/local is refused).
  --license N=P     copies P to Contents/Resources/licenses/N.txt (upstream licence texts of what we redistribute).
                    SDL2, N64Recomp, N64ModernRuntime and RT64 default to the texts in --lib's SDL2 prefix or build/work/sdl2,
                    --tools (or its parent) and --runtime, so a local Windows package needs no --license (release.yml passes all four).

Refreshing the recipes (needs the ROM; once per change of the symbol files; then review and commit recipes/):
       python3 -I tools/package_split.py --refresh-recipes --gen build/gen --engine <engine> --rom <thps1 .z64> [--rom ...] [--class us11]
       (one --rom per release of the class: USA v1.1 and Europe both go to class us11's blob.txt)
  THPS2/3 (front-end overlays): python3 -I tools/package_split.py --refresh-recipes [--rom <dump>] [--class us|eu]
       needs no --gen or --engine: decomp/common/firstrun/gen_overlay_recipe.py writes the class's whole recipe (overlays.txt, the symbol file,
       blob.txt) from the ROM (found in roms/ by its SHA-1 when no --rom matches) and the committed symbol file. Without --class: every class.

Layout produced (macOS):  OUT/<app>.app/Contents/{MacOS/thps1_recomp, Frameworks/*.dylib, Resources/{tools,sdk,classes,licenses}, Info.plist}
                          (no portable.txt: like the N64Recomp ports, settings default to the per-user folder. Quiver writes
                          portable.txt itself when it installs (filesToAdd). --portable adds one, for local tests.)
Elsewhere: OUT/<product>.exe (THPS1-64Recomp.exe; Linux OUT/thps1_recomp) + OUT/resources/ (+ libraries next to the exe).
Signing (macOS): every Mach-O ad hoc, hardened runtime on, packaging/macos/entitlements.plist on the app (disable-library-validation, so the
library built on the user's machine loads). See the THPS1 repository's docs/internals/recomp.md and LOG Q27.
"""
import argparse
import os
import platform
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostenv  # noqa: E402
import menu_assets  # noqa: E402
import dxc_pins  # noqa: E402
import scan_bundle  # noqa: E402  (the package inventory, RUNTIME_HEADERS and TOOLCHAIN_SKIP live there: one list for packager and gate)

HERE = os.path.join(hostenv.host_dir(), "tools")   # the game's wrappers (gen_versions.py, ...)
HOST = hostenv.host_dir()                          # decomp/thps<N>/recomp-host: recipes/, NOTICE, versions/
SHARED = hostenv.SHARED                            # decomp/common/host: src/, game-lib/, packaging/
FAMILY = hostenv.family(HOST)                      # thps1 / thps2 / thps3
SHORT = hostenv.game(HOST)["short_name"]
PRODUCT = hostenv.game(HOST)["product"]            # THPS1-64Recomp: the Windows exe in the package, the zip prefix
RECIPES = os.path.join(HOST, "recipes")
OVERLAYS = bool(hostenv.game(HOST).get("overlays"))   # THPS2/3: the front-end screens are overlays; a class's recipe also has overlays.txt (read at first run)
ENTITLEMENTS = os.path.join(SHARED, "packaging", "macos", "entitlements.plist")

# Names (ADR 0007; user, 2026-10-15): game.toml `app_name` ("THPS1 64 Recompiled") and `product` ("THPS1-64Recomp").
APP_NAME = hostenv.game(HOST)["app_name"]
BUNDLE_ID = f"local.{FAMILY}.recompiled"
MIN_MACOS = "13.0"   # RT64's Metal backend needs argument buffers tier 2 (macOS 13+); keep equal to CMAKE_OSX_DEPLOYMENT_TARGET in release.yml
COPYRIGHT = "Engine and tools: GPL-3.0 and MIT, see NOTICE. Contains no game code or data."

RUNTIME_HEADERS = scan_bundle.RUNTIME_HEADERS   # what the generated C and the RSP code include (clang -MM over a funcs file, game_lib.cpp, n_aspMain.cpp)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f"{' '.join(cmd)}: {r.stdout}{r.stderr}")
    return r.stdout


# ---------------------------------------------------------------- macOS: dylibs, signing

def archs(path):
    return run(["lipo", "-archs", path]).split()


def otool_deps(path):
    out = run(["otool", "-arch", archs(path)[0], "-L", path]).splitlines()[1:]
    return [l.strip().split(" (")[0] for l in out]


def otool_rpaths(path):
    """LC_RPATH entries present in every slice (install_name_tool edits all slices, so only those can be changed in one go)."""
    per = [set(re.findall(r"path (\S+) \(offset", run(["otool", "-arch", a, "-l", path]))) for a in archs(path)]
    common = set.intersection(*per)
    if any(p - common for p in per):
        sys.exit(f"{path}: the slices have different rpaths {per}: remove the build-machine rpath from each slice first (release.yml does)")
    return sorted(common)


SYSTEM_PREFIXES = ("/System/", "/usr/lib/")


def bundle_dylibs(exe, frameworks, libs):
    """Copy every non-system dylib the exe needs (recursively) into Frameworks and point the install names there.

    `libs`: {"libSDL2-2.0.0.dylib": path} for "@rpath/NAME" dependencies. Absolute dependencies outside the system are
    refused when they live in Homebrew's prefixes: a release must be built against our own libraries."""
    os.makedirs(frameworks, exist_ok=True)
    seen = {}   # original dep string -> bundled path
    todo = [exe]
    while todo:
        f = todo.pop()
        for dep in otool_deps(f):
            if dep.startswith(SYSTEM_PREFIXES) or dep in seen:
                continue
            name = os.path.basename(dep)
            if dep.startswith("@rpath/"):
                if name not in libs:
                    sys.exit(f"{os.path.basename(f)} needs {dep}: pass --lib {name}=PATH (our own build; not a system library)")
                real = libs[name]
            elif dep.startswith("@"):
                continue   # @executable_path/... or @loader_path/...: already relative, nothing to resolve
            else:
                if dep.startswith(("/opt/homebrew", "/usr/local")):
                    sys.exit(f"{os.path.basename(f)} links {dep}: a release must not depend on Homebrew (build SDL2 with tools/build_sdl2.py)")
                real = os.path.realpath(dep)
            dst = os.path.join(frameworks, name)
            shutil.copy2(real, dst)
            os.chmod(dst, 0o755)
            seen[dep] = dst
            todo.append(dst)
            run(["install_name_tool", "-id", f"@rpath/{name}", dst])
    # rewrite references in the exe and in the copies
    for f in [exe] + list(seen.values()):
        for dep in otool_deps(f):
            if dep in seen:
                run(["install_name_tool", "-change", dep, f"@rpath/{os.path.basename(dep)}", f])
    # drop every absolute rpath (build machine paths), add the bundle's
    for f in [exe] + list(seen.values()):
        for rp in otool_rpaths(f):
            if not rp.startswith("@"):
                run(["install_name_tool", "-delete_rpath", rp, f])
    if "@executable_path/../Frameworks" not in otool_rpaths(exe):
        run(["install_name_tool", "-add_rpath", "@executable_path/../Frameworks", exe])
    for f in [exe] + list(seen.values()):   # nothing may still point outside the bundle
        for dep in otool_deps(f):
            if not dep.startswith(SYSTEM_PREFIXES + ("@rpath/", "@executable_path/", "@loader_path/")):
                sys.exit(f"{f}: dependency {dep} is outside the bundle after rewriting")
    return sorted(seen.values())


def is_macho(path):
    with open(path, "rb") as f:
        return f.read(4) in (b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


def sign(app):
    """Ad-hoc, hardened runtime. Inside-out: every nested Mach-O first (no --deep), then the app with the entitlements."""
    for root, _, files in os.walk(app):
        for n in sorted(files):
            p = os.path.join(root, n)
            if "/Contents/MacOS/" in p or os.path.islink(p) or not is_macho(p):
                continue
            run(["codesign", "--force", "--sign", "-", "--options", "runtime", "--timestamp=none", p])
    run(["codesign", "--force", "--sign", "-", "--options", "runtime", "--timestamp=none", "--entitlements", ENTITLEMENTS, app])
    run(["codesign", "--verify", "--deep", "--strict", app])


# ---------------------------------------------------------------- recipes

def refresh_overlay_recipes(a):
    """THPS2/3: gen_overlay_recipe.py makes the class's whole recipe (overlays.txt, the host symbol file, blob.txt) from the ROM."""
    sys.path.insert(0, os.path.join(hostenv.COMMON, "firstrun"))
    import hashlib
    import gen_overlay_recipe as G
    keys = [f"{FAMILY}/{c}" for c in a.classes] if a.classes else [k for k in G.CLASSES if k.startswith(FAMILY + "/")]
    bad = [k for k in keys if k not in G.CLASSES]
    if bad:
        sys.exit(f"no overlay recipe generator entry for {bad} (gen_overlay_recipe.py CLASSES)")
    cli = G.build_cli()
    for key in keys:
        sha = G.thpsgame.load(G.CLASSES[key]["game"]).rom_sha1
        rom = None
        for r in a.rom:   # the --rom of that class, by SHA-1 (the generator verifies it again)
            if hashlib.sha1(open(r, "rb").read()).hexdigest() == sha:
                rom = r
        d, nfun, h = G.generate(key, rom, None, cli)
        print(f"recipe {key}: {nfun} overlay functions, main blob xxh3 {h}, {d}")
    print("review `git diff recipes/` and commit")


def refresh_recipes(a, classes):
    """ROM-side step: copy the generated host symbol file and compute the per-version code-blob fingerprints.
    Without --class, every recipe class that one of the --rom files belongs to is refreshed."""
    if OVERLAYS:
        return refresh_overlay_recipes(a)
    for cls in classes:
        gdir = os.path.join(a.gen, cls)
        if not a.classes and not os.path.isdir(gdir):
            print(f"recipe {cls}: skipped (no {gdir})")
            continue
        cdir = os.path.join(RECIPES, cls)
        os.makedirs(cdir, exist_ok=True)
        shutil.copy2(os.path.join(gdir, f"{FAMILY}.host.syms.toml"), cdir)
        env = dict(os.environ, THPS_GAME_LIB="none")
        lines = []
        tmpcfg = tempfile.mkdtemp(prefix="blobhash-")
        try:
            for rom in a.rom:
                h = run([a.engine, "--blob-hash", rom, "--config", tmpcfg], env=env).strip().splitlines()[-1].split()
                if h[2] == cls:   # hash, version id, code class
                    lines.append(f"xxh3.{h[1]} = {h[0]}\n")
        finally:
            shutil.rmtree(tmpcfg, ignore_errors=True)
        if not lines and not a.classes:
            print(f"recipe {cls}: skipped (no --rom of that class)")
            continue
        assert lines, f"no ROM of class {cls}"
        vers = ", ".join(l.split(" = ")[0].split(".", 1)[1] for l in lines)
        open(os.path.join(cdir, "blob.txt"), "w").write("".join(lines) + f"versions = {vers}\n")
        print(f"recipe {cls}: {len(lines)} version(s) in {cdir}")
    print("review `git diff recipes/` and commit")


def write_class_recipe(cls, cdir):
    """thps1.host.toml from the committed tables (ROM-free), plus the committed symbol file and fingerprints."""
    src = os.path.join(RECIPES, cls)
    files = (f"{FAMILY}.host.syms.toml", "blob.txt") + (("overlays.txt",) if OVERLAYS else ())   # overlays.txt: the front-end screens' layout and function map
    for f in files:
        if not os.path.exists(os.path.join(src, f)):
            sys.exit(f"{os.path.join(src, f)} is missing: run --refresh-recipes (needs the ROM) and commit it")
    os.makedirs(cdir)
    tmp = tempfile.mkdtemp(prefix="cfg-")
    try:
        run([sys.executable, "-I", os.path.join(HERE, "gen_versions.py"), "config", cls, tmp])
        cfg = open(os.path.join(tmp, f"{FAMILY}.host.toml")).read()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    cfg, n = re.subn(r'^rom_file_path = ".*"$', 'rom_file_path = "codeblob.bin"', cfg, flags=re.M)
    assert n == 1
    open(os.path.join(cdir, f"{FAMILY}.host.toml"), "w").write(cfg)
    # The audio microcode's RSPRecomp config: per class, because n_aspMain sits elsewhere in a re-linked class's blob (us10).
    run([sys.executable, "-I", os.path.join(HERE, "gen_versions.py"), "rsp-config", cls, os.path.join(cdir, f"{FAMILY}.aspMain.toml"),
         "codeblob.bin", "n_aspMain.cpp"])
    for f in files:
        shutil.copy2(os.path.join(src, f), cdir)


def default_licenses(a):
    """The four upstream licence texts every package carries (the bundle inventory requires them): fill in each one not
    passed with --license from where the build left it. One still missing is reported here; the inventory gate refuses it."""
    given = {l.partition("=")[0] for l in a.license}
    sdl = [os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(l.partition("=")[2]))), "LICENSE.txt") for l in a.lib if "SDL2" in l]
    tools = os.path.abspath(a.tools)
    candidates = {
        "SDL2": sdl + [os.path.join(HOST, "build", "work", "sdl2", "LICENSE.txt")],
        "N64Recomp": [os.path.join(tools, "LICENSE"), os.path.join(os.path.dirname(tools), "LICENSE")],
        "N64ModernRuntime": [os.path.join(a.runtime, "lib", "N64ModernRuntime", "COPYING")],
        "RT64": [os.path.join(a.runtime, "lib", "rt64", "LICENSE")],
    }
    for name, paths in candidates.items():
        if name in given:
            continue
        found = next((p for p in paths if os.path.isfile(p)), None)
        if found:
            a.license.append(f"{name}={found}")
            print(f"licence {name}: {found}")
        else:
            print(f"licence {name}: not found (looked in {', '.join(paths)}). Fix: pass --license {name}=PATH")


def fill_resources(res, a, classes):
    """The app's resources/ folder (macOS: Contents/Resources): the bundled N64Recomp/RSPRecomp, the compiler (Windows), the SDK the
    game library is built from, the per-class recipes and the notices. A release package and a dev build (--dev-resources) share this."""
    # tools (upstream executables, built from the pinned N64Recomp)
    os.makedirs(os.path.join(res, "tools"))
    for t in ("N64Recomp", "RSPRecomp"):
        src = os.path.join(a.tools, t + (".exe" if os.name == "nt" else ""))
        shutil.copy2(src, os.path.join(res, "tools"))
        os.chmod(os.path.join(res, "tools", os.path.basename(src)), 0o755)

    # Windows: the bundled compiler for the first run (game_split.cpp looks for resources/toolchain/bin/clang.exe). An llvm-mingw
    # release directory (tools/mingw_check.py pins the release); the other targets' sysroots and its Python are left out.
    # A dev build keeps an existing copy (main() moves it back after clearing resources/): copying ~470 MB on every build is too slow.
    if a.toolchain and not os.path.isdir(os.path.join(res, "toolchain")):
        # busybox (the Windows asset's ~150 POSIX tools, wget and ftpput among them) is not needed to compile and link the game library.
        skip = scan_bundle.TOOLCHAIN_SKIP
        shutil.copytree(a.toolchain, os.path.join(res, "toolchain"), ignore=lambda d, names: [n for n in names if d == a.toolchain and n in skip])

    # SDK: the headers the generated C and our glue include, and our glue
    for rel, dst in RUNTIME_HEADERS:
        d = os.path.join(res, "sdk", "include", dst)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(os.path.join(a.runtime, "lib", rel), d)
    os.makedirs(os.path.join(res, "sdk", "src"))
    for f in ("game_abi.h", "game_imports.def", "thps_hooks.h"):
        shutil.copy2(os.path.join(SHARED, "src", f), os.path.join(res, "sdk", "src", f))
    shutil.copy2(os.path.join(HOST, "game", "game_hooks.def"), os.path.join(res, "sdk", "src", "game_hooks.def"))
    os.makedirs(os.path.join(res, "sdk", "game"))
    for f in ("game_lib.cpp", "game_rsp.cpp"):
        shutil.copy2(os.path.join(SHARED, "game-lib", f), os.path.join(res, "sdk", "game", f))

    # per-class recipes: the N64Recomp config (generated here, ROM-free), the committed symbol file (names, addresses, sizes) and fingerprints
    for cls in classes:
        write_class_recipe(cls, os.path.join(res, "classes", cls))

    # notices
    shutil.copy2(os.path.join(HOST, "NOTICE"), os.path.join(res, "NOTICE"))
    for l in a.license:
        n, _, p = l.partition("=")
        os.makedirs(os.path.join(res, "licenses"), exist_ok=True)
        shutil.copy2(p, os.path.join(res, "licenses", n + ".txt"))
    if os.name == "nt":
        # dxil.dll and dxcompiler.dll ship beside the exe: Microsoft's licence texts for them (packaging/windows/licenses/README.md)
        win_lic = os.path.join(SHARED, "packaging", "windows", "licenses")
        os.makedirs(os.path.join(res, "licenses"), exist_ok=True)
        # They must be Microsoft's v1.8.2407 texts, byte for byte (the DLLs shipped are that release's, checked below).
        bad = dxc_pins.check(win_lic, dxc_pins.LICENSE_SHA256)
        if bad:
            sys.exit("package_split: DXC licence texts do not match the pinned release: " + "; ".join(bad))
        for f in ("DXC-dxil.txt", "DXC-dxcompiler.txt"):
            shutil.copy2(os.path.join(win_lic, f), os.path.join(res, "licenses", f))

    # the in-game settings menu (THPS_MENU, on by default): documents, styles and the OFL fonts that setup_runtime.py fetched into <runtime>/ui-fonts
    fonts = a.menu_fonts or os.path.join(a.runtime, "ui-fonts")
    # Missing fonts are an error, not a warning: the default build has the menu (THPS_MENU=ON), and without them it shows no text.
    if not os.path.isdir(fonts):
        sys.exit(f"package_split: the settings menu's fonts are missing ({fonts}): the package would show no menu text.\n"
                 f"  how to fix: run tools/setup_runtime.py <runtime dir> --ui-only (it fetches them into <runtime>/ui-fonts), or pass --menu-fonts DIR")
    err = menu_assets.stage(os.path.join(res, "ui"), fonts)
    if err:
        sys.exit(f"package_split: {err}\n  how to fix: run tools/setup_runtime.py <runtime dir> --ui-only, or pass --menu-fonts DIR")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", default=None)
    ap.add_argument("--tools", help="directory with N64Recomp and RSPRecomp")
    ap.add_argument("--runtime", help="the runtime tree (lib/N64ModernRuntime)")
    ap.add_argument("--out")
    ap.add_argument("--class", dest="classes", action="append", default=[])
    ap.add_argument("--version", default="0.0.0-dev", help="release version: file names and Info.plist (CFBundleShortVersionString gets the numeric prefix)")
    ap.add_argument("--app-name", default=APP_NAME)
    ap.add_argument("--lib", action="append", default=[], metavar="NAME=PATH")
    ap.add_argument("--license", action="append", default=[], metavar="NAME=PATH")
    ap.add_argument("--toolchain", help="Windows: an llvm-mingw release directory, copied (trimmed) to resources/toolchain")
    ap.add_argument("--menu-fonts", help="the settings menu's font folder (default: <runtime>/ui-fonts, made by setup_runtime.py)")
    ap.add_argument("--zip", action="store_true")
    ap.add_argument("--portable", action="store_true",
                    help="also write portable.txt beside the app (local tests; release zips leave it to Quiver)")
    ap.add_argument("--dev-resources", metavar="DIR",
                    help="only write the resources folder (tools, sdk, classes, NOTICE) to DIR: for a dev build, whose engine looks for resources/ beside the "
                         "executable (the CMake split build runs this; needs --tools and --runtime)")
    ap.add_argument("--refresh-recipes", action="store_true")
    ap.add_argument("--gen", help="--refresh-recipes: tools/gen.py output (build/gen)")
    ap.add_argument("--rom", action="append", default=[], help="--refresh-recipes: big-endian .z64 of a version of the class (fingerprint only); repeat for each version")
    a = ap.parse_args()
    # Default: every class with a recipe, so a release carries all THPS1 versions (v0.1 ships every version).
    classes = a.classes or sorted(d for d in (os.listdir(RECIPES) if os.path.isdir(RECIPES) else []) if os.path.isfile(os.path.join(RECIPES, d, "blob.txt")))
    if not (a.engine or a.dev_resources or (a.refresh_recipes and OVERLAYS)):
        ap.error("--engine is required")
    if a.refresh_recipes:
        if not OVERLAYS and not (a.gen and a.rom):
            sys.exit("--refresh-recipes needs --gen and --rom")
        return refresh_recipes(a, classes)
    if a.dev_resources:
        if not (a.tools and a.runtime):
            sys.exit("--dev-resources needs --tools and --runtime")
        res = os.path.abspath(a.dev_resources)
        # Windows: the bundled compiler (--toolchain) survives a rebuild when it came from the same llvm-mingw directory (a stamp beside it).
        keep = res + ".toolchain-keep"
        stamp = os.path.join(res, ".toolchain-source")
        src = os.path.abspath(a.toolchain) if a.toolchain else ""
        hostenv.rmtree(keep)
        if src and os.path.isdir(os.path.join(res, "toolchain")) and os.path.isfile(stamp) and open(stamp).read() == src:
            os.replace(os.path.join(res, "toolchain"), keep)
        hostenv.rmtree(res)
        os.makedirs(res)
        if os.path.isdir(keep):
            os.replace(keep, os.path.join(res, "toolchain"))
        fill_resources(res, a, classes)
        if src:
            open(stamp, "w").write(src)
        print(f"dev resources: {res} (classes {', '.join(classes)})")
        return
    if not (a.tools and a.runtime and a.out):
        sys.exit("--tools, --runtime and --out are required")
    if os.name == "nt" and not a.toolchain:
        # The Windows app compiles the game at first run with the llvm-mingw it carries (resources/toolchain). Default: the copy
        # `tools/mingw_check.py --fetch-toolchain` puts beside the runtime (where the CMake split build looks too).
        import glob
        # Also the game's build/work: with a shared runtime elsewhere ($THPS_RUNTIME_DIR), the toolchain stays in build/work.
        found = [f for d in (os.path.dirname(os.path.abspath(a.runtime)), os.path.join(HOST, "build", "work"))
                 for f in sorted(glob.glob(os.path.join(d, "llvm-mingw", "llvm-mingw-*", "bin", "clang.exe")))]
        if not found:
            sys.exit("a Windows package needs the first-run compiler: pass --toolchain <llvm-mingw dir> "
                     "(python -I tools/mingw_check.py --fetch-toolchain --work build/work fetches the pinned one)")
        a.toolchain = os.path.dirname(os.path.dirname(found[0]))
        print(f"toolchain: {a.toolchain}")
    default_licenses(a)
    mac = platform.system() == "Darwin"
    # macOS: the build target's name inside the .app (CFBundleExecutable); Windows: the product name, the file a user starts.
    exe_name = f"{PRODUCT}.exe" if os.name == "nt" else f"{FAMILY}_recomp"
    libs = {}
    for l in a.lib:
        n, _, p = l.partition("=")
        libs[n] = os.path.abspath(p)
        assert os.path.exists(libs[n]), libs[n]

    out = os.path.abspath(a.out)
    shutil.rmtree(out, ignore_errors=True)
    if mac:
        app = os.path.join(out, a.app_name + ".app")
        res = os.path.join(app, "Contents", "Resources")
        macos = os.path.join(app, "Contents", "MacOS")
    else:
        app, res, macos = out, os.path.join(out, "resources"), out
    os.makedirs(res)
    os.makedirs(macos, exist_ok=True)

    # engine
    exe = os.path.join(macos, exe_name)
    shutil.copy2(a.engine, exe)
    os.chmod(exe, 0o755)   # lipo -create writes 0644 (run 38186922914: "Permission denied" on the unzipped app)
    if os.name == "nt":
        # The DLLs the engine's build put beside it (dxil.dll, dxcompiler.dll, SDL2.dll): Windows loads them from the exe's folder.
        engine_dir = os.path.dirname(os.path.abspath(a.engine))
        for n in sorted(os.listdir(engine_dir)):
            if n.lower().endswith(".dll"):
                shutil.copy2(os.path.join(engine_dir, n), os.path.join(macos, n))
        # dxil.dll and dxcompiler.dll must be the pinned DXC v1.8.2407 PAIR (tools/dxc_pins.py): RT64's contrib/dxc copies are v1.7.2212,
        # whose dxil.dll licence forbids redistribution, and a mixed pair aborts at the first shader link. Refuse anything else.
        bad = dxc_pins.verify_pair(macos)
        if bad:
            shutil.rmtree(out, ignore_errors=True)
            sys.exit("package_split: refusing to package: " + "; ".join(bad))
    dylibs = []
    arch_label = ""
    if mac:
        ar = sorted(archs(exe))
        arch_label = "universal" if len(ar) > 1 else ar[0]
        dylibs = bundle_dylibs(exe, os.path.join(app, "Contents", "Frameworks"), libs)
        short = re.match(r"\d+(\.\d+){0,2}", a.version)
        short = short.group(0) if short else "0.0.0"
        with open(os.path.join(app, "Contents", "Info.plist"), "wb") as f:
            plistlib.dump({
                "CFBundleName": a.app_name, "CFBundleDisplayName": a.app_name,
                "CFBundleIdentifier": BUNDLE_ID, "CFBundleExecutable": exe_name,
                "CFBundlePackageType": "APPL", "CFBundleInfoDictionaryVersion": "6.0",
                "CFBundleShortVersionString": short, "CFBundleVersion": a.version,
                "CFBundleIconFile": "AppIcon",
                "LSMinimumSystemVersion": MIN_MACOS, "NSHighResolutionCapable": True,
                "LSApplicationCategoryType": "public.app-category.games",
                "NSHumanReadableCopyright": COPYRIGHT,
            }, f)
        with open(os.path.join(app, "Contents", "PkgInfo"), "w") as f:
            f.write("APPL????")
        run([sys.executable, "-I", os.path.join(hostenv.TOOLS, "make_app_icon.py"), "--game-toml",
             os.path.join(hostenv.host_dir(), "game", "game.toml"), "--icns", os.path.join(res, "AppIcon.icns")])

    fill_resources(res, a, classes)

    # No portable.txt by default (ADR 0005 addendum): a browser-downloaded app opened in place is translocated by
    # macOS, so "beside the app" is a read-only random copy. Settings live in the per-user folder, as in the
    # N64Recomp ports; Quiver adds portable.txt itself (its installs are not quarantined).
    if a.portable:
        open(os.path.join(out, "portable.txt"), "w").write("")

    if mac:
        sign(app)
    if mac or os.name == "nt":
        # The package inventory (scan_bundle.py layer 0): every expected file, nothing else, and the contents of what has a trusted
        # source, before a zip is made. The release gate repeats it on the unzipped zip.
        files = []
        for root, _, names in os.walk(out):
            for n in names:
                p = os.path.join(root, n)
                if not os.path.islink(p):
                    files.append((os.path.relpath(p, out).replace(os.sep, "/"), p))
        bad, notes = scan_bundle.inventory_check(files, not mac, {"classes": sorted(classes), "runtime": os.path.abspath(a.runtime),
                                                                  "menu_fonts": a.menu_fonts, "toolchain_ref": a.toolchain})
        for n in notes:
            print("  " + n)
        if bad:
            sys.exit("package_split: the package is incomplete or has unexpected files (nothing was zipped):\n  - " + "\n  - ".join(bad))
    machine = {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "arm64", "aarch64": "arm64"}.get(platform.machine().lower(), platform.machine())
    # The family convention (Zelda64Recomp, BanjoRecomp, ...): <Name>-vX.Y.Z-<OS>[-arch].zip.
    if not mac:
        machine = {"x86_64": "x64"}.get(machine, machine)
    name = f"{PRODUCT}-v{a.version}-{platform.system().replace('Darwin', 'macOS')}-{arch_label or machine}"
    if a.zip:
        dst = os.path.join(os.path.dirname(out), name + ".zip")
        if os.path.exists(dst):
            os.remove(dst)
        if mac:
            # ditto, not zip: keeps the signature's symlinks and modes. --norsrc: no __MACOSX/ AppleDouble entries (Quiver unzips as is).
            run(["ditto", "-c", "-k", "--norsrc", out, dst])
        else:
            # zipfile, not `zip`: Windows has no zip command. Entries are relative to OUT (the exe at the top), '/'-separated.
            import zipfile
            with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
                for root, dirs, files in os.walk(out):
                    dirs.sort()
                    for n in sorted(files):
                        p = os.path.join(root, n)
                        z.write(p, os.path.relpath(p, out).replace(os.sep, "/"))
        import hashlib
        h = hashlib.sha256()
        with open(dst, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        with open(os.path.join(os.path.dirname(out), "SHA256SUMS"), "w", newline="\n") as f:
            f.write(f"{h.hexdigest()}  {os.path.basename(dst)}\n")   # shasum's format
        print(f"zip: {dst} ({os.path.getsize(dst)} bytes)")
    print(f"packaged {app}")
    print("dylibs:", [os.path.basename(l) for l in dylibs])


if __name__ == "__main__":
    main()
