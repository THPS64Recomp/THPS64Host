#!/usr/bin/env python3
"""One command: your ROM -> generated C -> the native host of a THPS game. Windows, Linux and macOS. Shared by THPS1-3: run it
through the game's wrapper, decomp/thps<N>/recomp-host/tools/build_host.py (the examples name THPS1).

    python tools/build_host.py --rom "C:\\roms\\Tony Hawk's Pro Skater (U) (v1.1).z64"
    python tools/build_host.py --rom "<USA v1.1 dump>" --rom "<USA v1.0 dump>"   # one binary that runs both (and Europe)
    python tools/build_host.py --stub          # no ROM: builds the host with stub functions (toolchain check, cannot play)
    python tools/build_host.py --split         # no ROM: the split engine (first run builds the game from the user's ROM) -> build/cmake-split
    python tools/build_host.py --split --own-sdl2   # ... linked against our own SDL2 (tools/build_sdl2.py, pinned), as a release is
    python tools/build_host.py --stub --runtime ~/thps64-runtime   # several games: share one runtime folder (also: $THPS_RUNTIME_DIR)

Everything ROM-derived (the inflated code image, the code blob, the recompiled C) is generated on THIS machine from
YOUR dump and stays under decomp/thps<N>/build/ and decomp/thps<N>/recomp-host/build/ (gitignored; never upload it).

Steps (each is skipped when its output exists; --clean redoes everything):
  1. the pinned N64ModernRuntime + RT64 and our RT64 patches   (tools/setup_runtime.py)  -> <work>/runtime, or --runtime DIR
  2. N64Recomp + RSPRecomp at the pinned commit, built here    (cmake)                   -> <work>/N64Recomp
  3. a Python venv with `rabbitizer`                                                      -> <work>/venv
  4. per code class: ROM -> main image -> code blob   (decomp/common/thps_unpack.py, gen_syms.py)
  5. tools/gen.py -> recompiled C (build/gen/<class>) and the audio microcode
  6. cmake configure + build of the host                                                  -> recomp-host/build/cmake

Needs: git, Python 3.11+, CMake 3.20+, Ninja, and a compiler: on Windows Visual Studio 2022 with "C++ Clang Compiler for
Windows" and "C++ CMake tools" (this script finds it and loads the build environment), elsewhere clang or gcc.
Accepts .z64 (big-endian), .n64 (little-endian) and .v64 (byte-swapped) dumps.

Which releases the binary runs: the code classes of versions/classes.toml whose source ROM you pass (repeat --rom).
  USA v1.1 -> class us11, which also runs the European release (the same program; Europe alone cannot be built from)
  USA v1.0 -> class us10 (a re-linked build: its own payload; its symbol file is committed, the ROM gives the code)
A class whose ROM is missing is skipped (and removed from build/gen if an earlier run made it), so the binary runs exactly
what this run was given; at least one class must be buildable. A v1.0-only build works (the audio microcode is taken from
whichever class is built first). Adding a class (docs/versions.md) needs no change here.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostenv  # noqa: E402

HERE = os.path.join(hostenv.host_dir(), "tools")   # the game's wrappers (gen.py, ...); setup_runtime.py is shared
HOST = hostenv.host_dir()
GAME = hostenv.game(HOST)
FAMILY = GAME["family"]                            # thps1 / thps2 / thps3
OVERLAYS = bool(GAME.get("overlays"))              # THPS2/3: the front-end screens are overlays from the ROM tail (gen.py needs the ROM)
GAMEDIR = os.path.dirname(HOST)                    # decomp/thps<N>
COMMON = hostenv.COMMON
IS_WIN = os.name == "nt"
sys.path.insert(0, COMMON)   # thpsgame (decomp/common/games.toml)
N64RECOMP_URL = "https://github.com/N64Recomp/N64Recomp.git"
N64RECOMP_PIN = "ffb39cdad1da5de07eaaa48bd1db4a89a7986771"   # ffb39cd
RABBITIZER = "rabbitizer==1.16.2"


def say(msg):
    print(f"\n=== {msg}", flush=True)


def run(cmd, cwd=None, env=None):
    print("+ " + " ".join(f'"{c}"' if " " in str(c) else str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, check=True)


def exe(name):
    return name + (".exe" if IS_WIN else "")


# ---------------------------------------------------------------- toolchain

def load_msvc_env():
    hostenv.load_msvc_env()


def check_tools():
    if IS_WIN and not shutil.which("clang-cl"):
        load_msvc_env()
    missing = [t for t in ("git", "cmake", "ninja") if not shutil.which(t)]
    if IS_WIN and not shutil.which("clang-cl"):
        missing.append("clang-cl (Visual Studio Installer: 'C++ Clang Compiler for Windows')")
    if missing:
        sys.exit("missing tools: " + ", ".join(missing) + "\nFix: install them (BUILDING.md, \"Dependencies\"), open a new terminal, "
                 "and run again.")
    if sys.version_info < (3, 11):
        sys.exit("Python 3.11+ is required (tomllib).\nFix: install Python 3.11 or newer and run this with it.")


def compiler_args():
    if IS_WIN:
        return ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
    cxx = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    return [f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}"] if cc and cxx else []


# ---------------------------------------------------------------- steps

def shared_runtime_ready(d):
    """True when d is a runtime folder set up for THIS host: both checkouts carry a marker naming the current pin and every patch,
    with the patch files' current SHA-256, and the menu's fonts are there. (A runtime made for another host release is not.)"""
    sys.path.insert(0, hostenv.TOOLS)
    import setup_runtime as sr
    for name, patches in (("rt64", sr.RT64_PATCHES), ("N64ModernRuntime", sr.NMR_PATCHES)):
        marker = os.path.join(d, "lib", name, sr.MARKER)
        m = os.path.isfile(marker) and sr.read_marker(marker)
        want = [(p, sr.sha256_file(os.path.join(sr.PATCHES, p))) for p in patches]
        if not m or m[0] != sr.PINS[name][1] or m[1] != want:
            return False
    return os.path.isdir(os.path.join(d, "ui-fonts"))


def runtime_of_last_build():
    """The runtime folder an earlier build of this game used (RECOMP_RUNTIME_DIR in its CMake cache), when it is set up for this
    host. tools/check.sh builds in its own folder without --runtime: this lets it reuse your build's runtime instead of fetching
    another one (about 850 MB)."""
    for b in ("cmake", "cmake-split"):
        try:
            with open(os.path.join(HOST, "build", b, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
                d = next((line.split("=", 1)[1].strip() for line in f if line.startswith("RECOMP_RUNTIME_DIR:")), "")
        except OSError:
            continue
        if d and shared_runtime_ready(d):
            return d
    return None


def step_runtime(work, shared=None):
    say("1/6 pinned N64ModernRuntime + RT64 + our patches")
    if shared:
        # --runtime DIR: one runtime folder for several games (or checkouts). Set it up once; later builds reuse it as it is.
        if shared_runtime_ready(shared):
            print(f"reusing the shared runtime {shared} (its markers match this host's pins and patches)")
            return shared
        run([sys.executable, "-I", os.path.join(hostenv.TOOLS, "setup_runtime.py"), shared])
        return shared
    d = os.path.join(work, "runtime")
    run([sys.executable, "-I", os.path.join(hostenv.TOOLS, "setup_runtime.py"), d])
    return d


def step_recomp_tools(work):
    say("2/6 N64Recomp + RSPRecomp")
    d = os.path.join(work, "N64Recomp")
    n64, rsp = (os.path.join(d, "build", exe(n)) for n in ("N64Recomp", "RSPRecomp"))
    if os.path.exists(n64) and os.path.exists(rsp):
        print("already built")
        return n64
    if not os.path.isdir(os.path.join(d, ".git")):
        os.makedirs(d, exist_ok=True)
        run(["git", "init", "-q"], cwd=d)
        run(["git", "remote", "add", "origin", N64RECOMP_URL], cwd=d)
        run(["git", "fetch", "-q", "--depth", "1", "origin", N64RECOMP_PIN], cwd=d)
        run(["git", "checkout", "-q", "FETCH_HEAD"], cwd=d)
        run(["git", "submodule", "update", "--init", "--recursive", "--depth", "1", "--jobs", "4"], cwd=d)
    run(["cmake", "-S", d, "-B", os.path.join(d, "build"), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", *compiler_args()])
    run(["cmake", "--build", os.path.join(d, "build"), "--target", "N64RecompCLI", "RSPRecomp"])
    return n64


def step_venv(work):
    say("3/6 Python venv with rabbitizer")
    v = os.path.join(work, "venv")
    py = os.path.join(v, "Scripts" if IS_WIN else "bin", exe("python"))
    if not os.path.exists(py):
        run([sys.executable, "-m", "venv", v])
    probe = subprocess.run([py, "-c", "import rabbitizer"], capture_output=True)
    if probe.returncode:
        run([py, "-m", "pip", "install", "--quiet", RABBITIZER])
    return py


def to_z64(data):
    """.z64 (80 37 12 40), .n64 little-endian (40 12 37 80) or .v64 byte-swapped (37 80 40 12) -> big-endian."""
    magic = data[:4]
    if magic == bytes.fromhex("80371240"):
        return data
    if magic == bytes.fromhex("40123780"):    # little-endian words
        b = bytearray(data)
        for i in range(0, len(b) - 3, 4):
            b[i:i + 4] = b[i:i + 4][::-1]
        return bytes(b)
    if magic == bytes.fromhex("37804012"):    # byte-swapped halfwords
        b = bytearray(data)
        for i in range(0, len(b) - 1, 2):
            b[i], b[i + 1] = b[i + 1], b[i]
        return bytes(b)
    return None


def identify(rom_paths, work):
    """-> {game row name: z64 path} for every dump we recognise (by SHA-1 of the big-endian ROM)."""
    import thpsgame
    versions = {v.rom_sha1: v for v in thpsgame.versions(FAMILY)}
    found = {}
    os.makedirs(os.path.join(work, "roms"), exist_ok=True)
    for p in rom_paths:
        data = to_z64(open(p, "rb").read())
        if data is None:
            print(f"{p}: not an N64 ROM dump (unknown first bytes); skipped")
            continue
        sha1 = hashlib.sha1(data).hexdigest()
        v = versions.get(sha1)
        if not v:
            print(f"{p}: SHA-1 {sha1} is not a {GAME["short_name"]} release this build knows; skipped")
            continue
        out = os.path.join(work, "roms", f"{v.name}.z64")
        with open(out, "wb") as f:
            f.write(data)
        print(f"{p}: {v.display_name} ({v.name}, code class {v.code_class})")
        found[v.name] = out
    return found


def step_generate(py, n64recomp, roms, rsprecomp=None):
    """Every code class of versions/classes.toml whose source ROM was passed; a class without its ROM is left out of the build."""
    say("4/6 ROM -> code blob, per code class")
    import tomllib
    import thpsgame
    classes = tomllib.load(open(os.path.join(HOST, "versions", "classes.toml"), "rb"))
    todo = []
    for cls, c in classes.items():
        game = c["game"]
        runs = ", ".join(v.display_name for v in thpsgame.versions(FAMILY) if v.code_class == cls)
        if game in roms:
            todo.append((cls, game))
            print(f"code class {cls}: generated from {thpsgame.load(game).display_name} (runs {runs})")
        else:
            print(f"code class {cls}: skipped, it is generated from {thpsgame.load(game).display_name} and that ROM was not passed "
                  f"(this build will not run {runs})")
    if not todo:
        sys.exit("none of the ROMs passed is the source of a code class: pass the dump of a release that is a class source with --rom "
                 "(README, \"Supported ROM versions\"; for THPS1 a European dump alone builds nothing, it runs on USA v1.1's code).")
    os.makedirs(os.path.join(GAMEDIR, "build"), exist_ok=True)
    rom_args = []
    for cls, game in todo:
        main_bin = os.path.join(GAMEDIR, "build", f"{game}.main.bin")
        blob = os.path.join(GAMEDIR, "build", f"{game}.codeblob.bin")
        scratch_syms = os.path.join(GAMEDIR, "build", f"{game}.regenerated.syms.toml")   # the tracked recomp/*.syms.toml stays as it is
        # funclist (inside gen_syms.py) reads symbols/*.txt from the working directory: a seeded class keeps its own names
        # in versions/<class>/symbols (tools/seed_class.py). The blob itself is ROM bytes only and does not depend on them.
        syms_dir = os.path.join(HOST, "versions", cls) if os.path.isdir(os.path.join(HOST, "versions", cls, "symbols")) else GAMEDIR
        if OVERLAYS and game == FAMILY:
            # decomp/common/overlay_recomp.py and tools/gen.py want the family's ROM and image next to the decomp (gitignored).
            shutil.copyfile(roms[game], os.path.join(GAMEDIR, "baserom.z64"))
            main_bin = os.path.join(GAMEDIR, "baserom.main.bin")
        rom_args += ["--rom", f"{cls}={roms[game]}"]
        run([py, "-I", os.path.join(COMMON, "thps_unpack.py"), "--game", game, roms[game], main_bin])
        run([py, "-I", os.path.join(COMMON, "gen_syms.py"), "--game", game, roms[game], main_bin, blob, scratch_syms], cwd=syms_dir)
    # One build holds exactly the classes generated now: CMake links every build/gen/<class>/, so a class left over from an
    # earlier run with more ROMs would otherwise stay in (with generated C that no longer matches the tables).
    gen = os.path.join(HOST, "build", "gen")
    keep = {c for c, _ in todo}
    if os.path.isdir(gen):
        for d in sorted(os.listdir(gen)):
            if d not in keep and os.path.isdir(os.path.join(gen, d, "RecompiledFuncs")):
                print(f"removing build/gen/{d}: code class {d} is not part of this build")
                shutil.rmtree(os.path.join(gen, d))
    say("5/6 tools/gen.py: recompiled C + audio microcode")
    env = dict(os.environ, RSPRECOMP=rsprecomp) if rsprecomp else None
    run([py, "-I", os.path.join(HERE, "gen.py"), n64recomp, *(rom_args if OVERLAYS else []), *[c for c, _ in todo]], cwd=HOST, env=env)


def step_toolchain(work):
    """Windows split build: the pinned llvm-mingw the first run compiles with (CMake bundles it into resources/toolchain)."""
    say("llvm-mingw for the first run (pinned, SHA-256 checked)")
    run([sys.executable, "-I", os.path.join(hostenv.TOOLS, "mingw_check.py"), "--fetch-toolchain", "--work", work])


def step_sdl2(work):
    """Our own SDL2 (tools/build_sdl2.py: pinned source release, SHA-256 checked), as a release links it."""
    prefix = os.path.join(work, "sdl2")
    say("our own SDL2 (tools/build_sdl2.py)")
    if os.path.exists(os.path.join(prefix, "LICENSE.txt")):
        print("already built")
    else:
        run([sys.executable, "-I", os.path.join(hostenv.TOOLS, "build_sdl2.py"), prefix, "--work", os.path.join(work, "sdl2-work")])
    return ["-DTHPS_OWN_SDL2=ON", f"-DCMAKE_PREFIX_PATH={prefix}"]


def default_build_dir(split=False):
    return os.path.join(HOST, "build", "cmake-split" if split else "cmake")


def step_cmake(runtime, stub, build_type, jobs, split=False, extra=(), build=None):
    say("6/6 CMake: host" + (" (stub functions)" if stub else " (split engine)" if split else ""))
    build = build or default_build_dir(split)
    # THPS_HOST_ROOT is a CMake cache variable: name the host tree this run uses, so a build dir follows $THPS_HOST_ROOT.
    args = ["cmake", "-S", HOST, "-B", build, "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={build_type}", f"-DRECOMP_RUNTIME_DIR={runtime}",
            f"-DTHPS_HOST_ROOT={COMMON}", *compiler_args()]
    if split:
        args += ["-DTHPS_SPLIT_GAME_LIB=ON", "-DTHPS_STUB_FUNCS=OFF"]
    else:
        args.append("-DTHPS_STUB_FUNCS=ON" if stub else "-DTHPS_STUB_FUNCS=OFF")
    args += list(extra)
    run(args)
    run(["cmake", "--build", build] + (["-j", str(jobs)] if jobs else []))
    return os.path.join(build, exe(f"{FAMILY}_recomp"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", action="append", default=[], help="your ROM dump (.z64/.n64/.v64); repeat for several releases")
    ap.add_argument("--stub", action="store_true", help="no ROM: build with stub functions (a toolchain check; cannot play)")
    ap.add_argument("--work", default=os.path.join(HOST, "build", "work"), help="where the runtime, N64Recomp and the venv go (default: recomp-host/build/work)")
    ap.add_argument("--build-type", default="RelWithDebInfo", choices=["Release", "RelWithDebInfo", "Debug"])
    ap.add_argument("-j", "--jobs", type=int, default=0)
    ap.add_argument("--clean", action="store_true", help="delete the work dir and the build first")
    ap.add_argument("--split", action="store_true", help="no ROM: build the split engine (build/cmake-split; the first run builds the game)")
    ap.add_argument("--own-sdl2", action="store_true", help="link our own SDL2 (tools/build_sdl2.py into <work>/sdl2), as a release does")
    ap.add_argument("--runtime", metavar="DIR", default=os.environ.get("THPS_RUNTIME_DIR") or None,
                    help="share one runtime folder (N64ModernRuntime, RT64, the menu's libraries) between games or checkouts: "
                    "set up there on first use, reused afterwards (default: $THPS_RUNTIME_DIR, else <work>/runtime)")
    ap.add_argument("--build-dir", help="the CMake build dir (default: build/cmake, or build/cmake-split with --split); "
                    "tools/check.sh uses build/cmake-check, so its stub build never replaces your real one")
    args = ap.parse_args()
    if not args.rom and not args.stub and not args.split:
        ap.error("pass --rom PATH (or --stub, or --split)")
    if args.split and (args.rom or args.stub):
        ap.error("--split takes no ROM (the first run reads the user's) and no --stub")
    work = os.path.abspath(args.work)
    if args.clean:
        hostenv.rmtree(work)   # not shutil.rmtree(ignore_errors=True): that keeps git's read-only files on Windows
        hostenv.rmtree(os.path.abspath(args.build_dir) if args.build_dir else default_build_dir(args.split))
    os.makedirs(work, exist_ok=True)
    check_tools()
    shared = args.runtime and os.path.abspath(os.path.expanduser(args.runtime))
    if not shared and not os.path.isdir(os.path.join(work, "runtime")):
        shared = runtime_of_last_build()
        if shared:
            print(f"no --runtime given: reusing the runtime of your last build, {shared} (set THPS_RUNTIME_DIR or --runtime to choose one)")
    runtime = step_runtime(work, shared)
    extra = step_sdl2(work) if args.own_sdl2 else []
    if args.split:
        step_recomp_tools(work)   # bundled into the dev resources/ (THPS_TOOLS_DIR defaults to <work>/N64Recomp/build)
        if IS_WIN:
            step_toolchain(work)
        binary = step_cmake(runtime, False, args.build_type, args.jobs, split=True, extra=extra, build=args.build_dir and os.path.abspath(args.build_dir))
        say("done")
        print(f"split engine: {binary}")
        print(f'first run: "{binary}" --rom "<your ROM>"   |   tests: decomp/{FAMILY}/recomp-host/tools/test.sh --first-run')
        return
    if not args.stub:
        n64recomp = step_recomp_tools(work)
        py = step_venv(work)
        roms = identify(args.rom, work)
        if not roms:
            sys.exit("no usable ROM")
        step_generate(py, n64recomp, roms)
    binary = step_cmake(runtime, args.stub, args.build_type, args.jobs, extra=extra, build=args.build_dir and os.path.abspath(args.build_dir))
    say("done")
    print(f"host: {binary}")
    if not args.stub:
        print(f'play:  "{binary}" --rom "<your ROM>"     (the ROM is stored in the config folder, later runs need no --rom)')
        print(f"tests: decomp/{FAMILY}/recomp-host/tools/test.sh --record   (the first time on a machine; on Windows run it in Git Bash."
              ' See BUILDING.md, "Tests")')


if __name__ == "__main__":
    main()
