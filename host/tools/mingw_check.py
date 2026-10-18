#!/usr/bin/env python3
"""Compile-check the host for Windows from macOS, Linux or Windows itself, with llvm-mingw (open source, no licence prompt).

    python3 -I tools/mingw_check.py [--work DIR] [--runtime DIR] [--no-ui]
    python3 -I tools/mingw_check.py --fetch-toolchain [--work DIR]    (any OS, incl. Windows: only download + check + unpack, print the path)

--fetch-toolchain is how a Windows split build gets the compiler it bundles (resources/toolchain): the Windows-host asset of the same
pinned release. `--work build/work` puts it where the CMake split build looks by default (<runtime>/../llvm-mingw, see THPS_TOOLCHAIN_DIR).

Downloads llvm-mingw (a pinned release, SHA-256 checked), configures a stub-functions Windows build (x86_64-w64-mingw32) and
compiles every host source file with the Windows branches of main.cpp, platform.cpp, crash.cpp and rt64_context.cpp enabled, then the split build's own
files (first run: game_split.cpp, firstrun*.cpp, overlay_rom.cpp ...) from a second configuration (-DTHPS_SPLIT_GAME_LIB=ON).
It stops there on purpose: linking needs RT64's D3D12 backend, which does not compile against mingw-w64's D3D12 headers
(no dxguids.h, no D3D12_HEAP_TYPE_GPU_UPLOAD, COM struct returns); that needs the Windows SDK (docs/WINDOWS-TEST.md).
Evidence from 2026-10-11: the private development notes, "Q24 Windows port". This catches compile errors early; it is not a
substitute for building with clang-cl on Windows.

The in-game settings menu (THPS_MENU, on by default) is compiled too: RmlUi, lunasvg and FreeType (setup_runtime.py fetches their sources into the
runtime dir) and the host's src/ui. --no-ui builds -DTHPS_MENU=OFF. The shader headers DXC would write (it is a Windows program) are stubbed.

Nothing here is ROM-derived. The download is one archive of about 125 MB (macOS) or 85 MB (Linux).
"""
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostenv  # noqa: E402

HERE = hostenv.TOOLS
HOST = hostenv.host_dir()
GAME = os.path.basename(os.path.dirname(HOST))      # thps1 / thps2 / thps3
RELEASE = "20261006"
ASSETS = {   # name -> SHA-256 (GitHub's asset digest, and the file hashed after download)
    "llvm-mingw-20261006-ucrt-macos-universal.tar.xz": "a7a19b11048e89372637d26279cdb7c430ed3cf8c1e224310421312bf986ebf6",
    "llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz": "5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4",
    "llvm-mingw-20261006-ucrt-ubuntu-22.04-aarch64.tar.xz": "e77c4acfa44184f6b0c52b814d724ad63b2ecebb3c3f6f842c0846e717cbd23c",
    "llvm-mingw-20261006-ucrt-x86_64.zip": "317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a",   # Windows host (the split build bundles it)
}


def asset_for_host(windows_ok=False):
    if sys.platform == "win32" and windows_ok:
        return "llvm-mingw-20261006-ucrt-x86_64.zip"
    if sys.platform == "darwin":
        return "llvm-mingw-20261006-ucrt-macos-universal.tar.xz"
    if sys.platform.startswith("linux"):
        return "llvm-mingw-20261006-ucrt-ubuntu-22.04-" + ("aarch64" if platform.machine() in ("aarch64", "arm64") else "x86_64") + ".tar.xz"
    sys.exit("no llvm-mingw asset is pinned for this host")


def vs_cmake_path():
    """Windows: CMake and Ninja come with Visual Studio and are not on a plain PATH. Only their folders are added (not vcvars64's
    INCLUDE/LIB: this is a mingw build). Elsewhere, or when cmake is already on PATH: nothing."""
    if sys.platform != "win32" or shutil.which("cmake"):
        return ""
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Microsoft Visual Studio", "Installer", "vswhere.exe")
    vs = subprocess.run([vswhere, "-latest", "-products", "*", "-property", "installationPath"], capture_output=True, text=True).stdout.strip()         if os.path.exists(vswhere) else ""
    ext = os.path.join(vs, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake")
    dirs = [os.path.join(ext, "CMake", "bin"), os.path.join(ext, "Ninja")]
    if not vs or not all(os.path.isdir(d) for d in dirs):
        sys.exit("cmake/ninja not found: put them on PATH, or install Visual Studio 2022's 'C++ CMake tools for Windows'")
    return os.pathsep.join(dirs) + os.pathsep


def fetch_toolchain(work, windows_ok=False):
    name = asset_for_host(windows_ok)
    dest = os.path.join(work, "llvm-mingw")
    marker = os.path.join(dest, ".extracted-" + name)
    if os.path.exists(marker):
        return next(os.path.join(dest, d) for d in os.listdir(dest) if d.startswith("llvm-mingw-"))
    os.makedirs(os.path.join(work, "dl"), exist_ok=True)
    archive = os.path.join(work, "dl", name)
    if not os.path.exists(archive):
        url = f"https://github.com/mstorsjo/llvm-mingw/releases/download/{RELEASE}/{name}"
        print(f"download {url}", flush=True)
        urllib.request.urlretrieve(url, archive)
    h = hashlib.sha256()
    with open(archive, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    digest = h.hexdigest()
    if digest != ASSETS[name]:
        os.remove(archive)
        sys.exit(f"SHA-256 mismatch for {name}: got {digest}, expected {ASSETS[name]}")
    print(f"sha256 ok: {digest}")
    hostenv.rmtree(dest)
    os.makedirs(dest)
    if name.endswith(".zip"):
        import zipfile
        with zipfile.ZipFile(archive) as z:
            root = os.path.realpath(dest)
            for n in z.namelist():   # refuse absolute names and ../ (zipfile strips most, check anyway)
                if not os.path.realpath(os.path.join(dest, n)).startswith(root + os.sep):
                    sys.exit(f"{name}: unsafe member name {n!r}")
            z.extractall(dest)
    else:
        with tarfile.open(archive) as t:
            t.extractall(dest, filter="data")
    open(marker, "w").close()
    return next(os.path.join(dest, d) for d in os.listdir(dest) if d.startswith("llvm-mingw-"))


def main(argv):
    if "--fetch-toolchain" in argv:
        argv = [a for a in argv if a != "--fetch-toolchain"]
        work = os.path.abspath(argv[argv.index("--work") + 1]) if "--work" in argv else os.path.join(HOST, "build", "work")
        os.makedirs(work, exist_ok=True)
        print(fetch_toolchain(work, windows_ok=True))
        return
    work = os.path.join(HOST, "build", "mingw-check")
    runtime = None
    menu = True
    i = 0
    while i < len(argv):
        if argv[i] == "--work" and i + 1 < len(argv):
            work = os.path.abspath(argv[i + 1]); i += 2
        elif argv[i] == "--runtime" and i + 1 < len(argv):
            runtime = os.path.abspath(argv[i + 1]); i += 2
        elif argv[i] == "--no-ui":
            menu = False; i += 1
        elif argv[i] == "--ui":   # the default
            i += 1
        else:
            sys.exit(__doc__)
    os.makedirs(work, exist_ok=True)
    tc = fetch_toolchain(work, windows_ok=True)   # on Windows: the Windows-host asset (the one the split build bundles)
    runtime = runtime or os.path.join(work, "runtime")
    if not os.path.isdir(os.path.join(runtime, "lib", "rt64", ".git")):
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "setup_runtime.py"), runtime] + ([] if menu else ["--no-ui"]), check=True)
    elif menu and not os.path.isdir(os.path.join(runtime, "lib", "RmlUi")):
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "setup_runtime.py"), runtime, "--ui-only"], check=True)
    build = os.path.join(work, "build")
    os.environ["PATH"] = vs_cmake_path() + os.environ["PATH"]   # Windows looks the program up in this process's PATH, not env's
    env = dict(os.environ, PATH=os.path.join(tc, "bin") + os.pathsep + os.environ["PATH"])
    exe = ".exe" if sys.platform == "win32" else ""   # CMake on Windows does not add it
    # forward slashes: CMake pastes the RC compiler path into generated CMake code unescaped ("Invalid character escape '\g'")
    cc, cxx, rc = (os.path.join(tc, "bin", f"x86_64-w64-mingw32-{t}{exe}").replace("\\", "/") for t in ("clang", "clang++", "windres"))
    subprocess.run(["cmake", "-S", HOST, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_SYSTEM_NAME=Windows",
                    "-DCMAKE_SYSTEM_PROCESSOR=AMD64", f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}", f"-DCMAKE_RC_COMPILER={rc}",
                    f"-DCMAKE_FIND_ROOT_PATH={tc}/x86_64-w64-mingw32", f"-DRECOMP_RUNTIME_DIR={runtime}", "-DTHPS_STUB_FUNCS=ON",
                    f"-DTHPS_GEN_DIR={os.path.join(work, 'gen-stub')}"]
                   + (["-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON",
                       # RmlUi's Math.cpp uses std::floor without <cmath>: MSVC's STL pulls it in through <algorithm>, llvm-mingw's libc++ does not
                       "-DCMAKE_CXX_FLAGS=-include cmath"] if menu else ["-DTHPS_MENU=OFF"]), check=True, env=env)
    if menu:
        # compdb lists compile commands only; the generated shader headers (DXC runs on Windows) are stubbed so the units that include them compile.
        sh = os.path.join(build, "ui", "shaders")
        os.makedirs(sh, exist_ok=True)
        for stem in ("InterfaceVS", "InterfacePS"):
            for ext, blob in (("spirv", "SPIRV"), ("dxil", "DXIL")):
                with open(os.path.join(sh, f"{stem}.hlsl.{ext}.h"), "w") as f:
                    f.write(f'#include <stddef.h>\nextern "C" {{ extern const char {stem}Blob{blob}[1]; extern const size_t {stem}Blob{blob}_size; }}\n')
    db = json.loads(subprocess.run(["ninja", "-C", build, "-t", "compdb"], check=True, capture_output=True, text=True, env=env).stdout)
    # Compare normalised paths: on Windows the compile database says C:/g/... while os.path.join gives C:\g\... (only the two
    # payload_ units, matched by name, were found before this).
    norm = lambda q: os.path.normcase(os.path.normpath(q))
    host_src = tuple(norm(os.path.join(d, "src")) + os.sep for d in (HOST, hostenv.SHARED)) + (norm(os.path.join(HOST, "game")) + os.sep,)
    ui_src = (norm(os.path.join(runtime, "lib")) + os.sep,) if menu else ()
    units = [e for e in db if norm(e["file"]).startswith(host_src) or os.path.basename(e["file"]).startswith("payload_")
             or (menu and norm(e["file"]).startswith(ui_src) and e["file"].endswith((".cpp", ".c"))
                 and any(d in e["file"].replace("\\", "/") for d in ("/RmlUi/", "/lunasvg/", "/freetype/")))]
    fails = 0
    for e in units:
        r = subprocess.run(e["command"], shell=True, cwd=e["directory"], capture_output=True, text=True, env=env)
        name = os.path.basename(e["file"])
        if r.returncode:
            fails += 1
            print("FAIL", name)
            print("\n".join(l for l in r.stderr.splitlines() if "error" in l)[:3000])
        else:
            print("ok  ", name)
    print(f"{len(units) - fails} of {len(units)} host source files compile for x86_64-w64-mingw32")
    # The split build's own files (first run: game_split.cpp, the ROM picker and build progress, overlay_rom.cpp, ...) only exist in the
    # THPS_SPLIT_GAME_LIB configuration: configure that one too (no stub functions, no generated C needed) and compile just those.
    split_build = os.path.join(work, "build-split")
    subprocess.run(["cmake", "-S", HOST, "-B", split_build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_SYSTEM_NAME=Windows",
                    "-DCMAKE_SYSTEM_PROCESSOR=AMD64", f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}", f"-DCMAKE_RC_COMPILER={rc}",
                    f"-DCMAKE_FIND_ROOT_PATH={tc}/x86_64-w64-mingw32", f"-DRECOMP_RUNTIME_DIR={runtime}", "-DTHPS_SPLIT_GAME_LIB=ON",
                    f"-DTHPS_GEN_DIR={os.path.join(work, 'gen-split')}"], check=True, env=env)
    db = json.loads(subprocess.run(["ninja", "-C", split_build, "-t", "compdb"], check=True, capture_output=True, text=True, env=env).stdout)
    split_only = {"game_split.cpp", "game_imports_table.cpp", "firstrun.cpp", "firstrun_ui.cpp", "platform_ext.cpp", "overlay_rom.cpp"}
    units = [e for e in db if os.path.basename(e["file"]) in split_only]
    sfails = 0
    for e in units:
        r = subprocess.run(e["command"], shell=True, cwd=e["directory"], capture_output=True, text=True, env=env)
        name = os.path.basename(e["file"])
        if r.returncode:
            sfails += 1
            print("FAIL", name)
            print("\n".join(l for l in r.stderr.splitlines() if "error" in l)[:3000])
        else:
            print("ok  ", name)
    print(f"{len(units) - sfails} of {len(units)} split-build source files compile for x86_64-w64-mingw32")
    if len(units) != len(split_only):
        print(f"FAIL: expected {len(split_only)} split-build sources in the compile database, found {len(units)}")
        sfails += 1
    sys.exit(1 if (fails or sfails) else 0)


if __name__ == "__main__":
    main(sys.argv[1:])
