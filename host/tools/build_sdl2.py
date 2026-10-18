#!/usr/bin/env python3
"""Build our own SDL2 from the pinned upstream source release (macOS release builds; any Unix works; Windows with Visual Studio's clang-cl).

The release app must not need Homebrew: Homebrew's `sdl2` is sdl2-compat, a shim that dlopen()s libSDL3 at run time
(Q25), and a developer's /opt/homebrew has nothing to do with a user's Mac. This script builds the real SDL2 once, into
a prefix that CMake finds with -DCMAKE_PREFIX_PATH=<prefix> (the host's find_package(SDL2) is unchanged), and
tools/package_split.py copies libSDL2-2.0.0.dylib into the .app.

Pin (docs: NOTICE credits SDL, zlib licence):  SDL2 2.32.10, source tarball SDL2-2.32.10.tar.gz from the upstream release
page https://github.com/libsdl-org/SDL/releases/tag/release-2.32.10  (tag object 5d24957, 2025-09-01).
SHA-256 5f5993c5... is the digest GitHub shows for that asset (trust on first use: checked against the release page on
2026-10-11, not against a signature; the release also carries a .sig file). The build fails on any other hash.
Bumping SDL2: change VERSION and SHA256 together, and re-run the CI smoke test.

usage: python3 -I tools/build_sdl2.py PREFIX [--work DIR] [--arch arm64|x86_64] [--min-macos 13.0]
  PREFIX   install prefix (lib/libSDL2-2.0.0.dylib, lib/cmake/SDL2, include/SDL2; Windows: bin/SDL2.dll, lib/SDL2.lib, cmake/)
Windows: configure the host with -DTHPS_OWN_SDL2=ON -DCMAKE_PREFIX_PATH=PREFIX; the build copies PREFIX's SDL2.dll beside the exe
(instead of RT64's prebuilt 2.26.3) and tools/package_split.py ships it.
  --arch   one architecture (default: the machine's); universal builds are two native builds + lipo (see release.yml)
"""
import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request

VERSION = "2.32.10"
SHA256 = "5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{VERSION}/SDL2-{VERSION}.tar.gz"


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def fetch(work):
    tgz = os.path.join(work, f"SDL2-{VERSION}.tar.gz")
    if not os.path.exists(tgz):
        print(f"download {URL}", flush=True)
        try:
            req = urllib.request.Request(URL, headers={"User-Agent": "thps1-recomp-build"})
            with urllib.request.urlopen(req, timeout=120) as r, open(tgz + ".part", "wb") as f:
                shutil.copyfileobj(r, f)
        except Exception as e:   # python.org builds on macOS can lack a certificate bundle: curl uses the system one
            print(f"urllib failed ({e}); trying curl", flush=True)
            subprocess.run(["curl", "-fsSL", "--retry", "3", "-o", tgz + ".part", URL], check=True)
        os.replace(tgz + ".part", tgz)
    h = hashlib.sha256(open(tgz, "rb").read()).hexdigest()
    if h != SHA256:
        os.remove(tgz)
        sys.exit(f"SDL2 tarball hash mismatch: got {h}, pinned {SHA256}")
    print(f"sha256 ok: {h}", flush=True)
    return tgz


def extract(tgz, work):
    src = os.path.join(work, f"SDL2-{VERSION}")
    if os.path.isdir(src):
        return src
    with tarfile.open(tgz) as t:
        for m in t.getmembers():   # refuse path escapes (the hash already pins the content; belt and braces)
            if m.name.startswith(("/", "..")) or "/../" in m.name:
                sys.exit(f"unsafe path in tarball: {m.name}")
        t.extractall(work)
    return src


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prefix")
    ap.add_argument("--work", default=None)
    ap.add_argument("--arch", default=platform.machine())
    ap.add_argument("--min-macos", default="13.0")
    a = ap.parse_args()
    prefix = os.path.abspath(a.prefix)
    work = os.path.abspath(a.work or (prefix + ".work"))
    os.makedirs(work, exist_ok=True)
    src = extract(fetch(work), work)
    build = os.path.join(work, "build")
    args = ["cmake", "-S", src, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}",
            "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TEST=OFF", "-DSDL_INSTALL_TESTS=OFF"]
    if sys.platform == "darwin":
        args += [f"-DCMAKE_OSX_ARCHITECTURES={a.arch}", f"-DCMAKE_OSX_DEPLOYMENT_TARGET={a.min_macos}"]
    if sys.platform == "win32":
        import shutil as _sh
        if not _sh.which("clang-cl"):
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import hostenv
            hostenv.load_msvc_env()   # the same Visual Studio environment the host build uses
        args += ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]   # as for the host
    run(args)
    run(["cmake", "--build", build])
    run(["cmake", "--install", build])
    shutil.copy2(os.path.join(src, "LICENSE.txt"), os.path.join(prefix, "LICENSE.txt"))   # zlib licence: shipped in the bundle
    print(f"SDL2 {VERSION} installed in {prefix}: use -DCMAKE_PREFIX_PATH={prefix}")


if __name__ == "__main__":
    main()
