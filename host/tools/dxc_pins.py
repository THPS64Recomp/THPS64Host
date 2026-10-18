#!/usr/bin/env python3
"""The DirectX Shader Compiler DLLs the Windows build and zip ship, pinned: DXC v1.8.2407 (Microsoft's release).

Shared by setup_runtime.py (fetch), thps_host.cmake (via setup_runtime.py --dxc-only), package_split.py and scan_bundle.py (refuse
anything else). Nothing here is ROM-derived; the DLLs are never committed to git.

Why this release: the v1.7.2212 `dxil.dll` that RT64's contrib/dxc carries has a pre-release, time-limited licence whose clause 4(e)
forbids distribution. v1.8.2407's `LICENSE-MS.txt` has a "DISTRIBUTABLE CODE" section (decomp/thps1/docs/WINDOWS-RESULTS-2026-10-13.md,
packaging/windows/licenses/README.md). Not legal advice.

The two DLLs are a PAIR: a new dxil.dll with an old dxcompiler.dll aborts at the first shader link (Windows, 2026-10-13), so every
check here looks at both, by SHA-256 (stricter than a version resource, and the two files cannot be mixed from different builds).

usage: python3 -I tools/dxc_pins.py fetch DEST       download + verify into DEST/dxc/v1.8.2407/ (x64/dxil.dll, x64/dxcompiler.dll, LICENSE-*.txt)
       python3 -I tools/dxc_pins.py verify DIR       DIR holds dxil.dll and dxcompiler.dll: exit 1 unless both are the pinned pair
"""
import hashlib
import os
import shutil
import sys
import tempfile
import zipfile

TAG = "v1.8.2407"
ASSET = "dxc_2024_07_31.zip"
URL = f"https://github.com/microsoft/DirectXShaderCompiler/releases/download/{TAG}/{ASSET}"
ARCHIVE_SHA256 = "e2627f004f0f9424d8c71ea1314d04f38c5a5096884ae9217f1f18bd320267b5"
ARCHIVE_SIZE = 25833008
# Same bytes as renderbag/dxc-bin 0cffb90 (bin/x64), checked 2026-10-13.
MEMBERS = {   # member of the zip -> SHA-256 of the extracted file
    "bin/x64/dxil.dll": "e038a9755387fc396a9a5f393d292b9318cf6ac7e520e9283bbc3088c5f8ac93",          # 1.8.2407, validator
    "bin/x64/dxcompiler.dll": "1220478c87af97551d2342906c5a08d888ee9fa26f6f99b6072d674801f492d1",     # 1.8.2407.7
    "LICENSE-MS.txt": "734f72f239fe7b07b4c7203f294c1a7ce27095687278bab7e56d630d7c672963",           # applies to dxil.dll (the zip's ReleaseNotes.md)
    "LICENSE-LLVM.txt": "729615317e28dd03907e46f0fc3b5e88f7853cee61d1a1471d2749335516b46f",         # "all other files": dxcompiler.dll
}
DLL_SHA256 = {"dxil.dll": MEMBERS["bin/x64/dxil.dll"], "dxcompiler.dll": MEMBERS["bin/x64/dxcompiler.dll"]}
LICENSE_SHA256 = {"DXC-dxil.txt": MEMBERS["LICENSE-MS.txt"], "DXC-dxcompiler.txt": MEMBERS["LICENSE-LLVM.txt"]}   # the repo's copies are the zip's bytes
FETCHED_SHA256 = {os.path.basename(m): sha for m, sha in MEMBERS.items()}   # what fetch() leaves in DEST/dxc/<tag>/x64


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check(dirpath, pins):
    """Problems (a list of strings, empty when ok) for the files of `pins` (name -> SHA-256) directly in dirpath."""
    problems = []
    for name, want in pins.items():
        p = os.path.join(dirpath, name)
        if not os.path.isfile(p):
            problems.append(f"{name} is missing from {dirpath}")
            continue
        got = sha256_file(p)
        if got != want:
            problems.append(f"{name}: SHA-256 {got} is not the pinned DXC {TAG} file ({want}); dxil.dll and dxcompiler.dll must both be "
                            f"DXC {TAG}'s (mixing releases aborts at the first shader link, and RT64's contrib/dxc copies are v1.7.2212)")
    return problems


def verify_pair(dirpath):
    return check(dirpath, DLL_SHA256)


def dest_dir(dest):
    return os.path.join(dest, "dxc", TAG)


def pair_dir(dest):
    """Where the pinned DLLs (and the licence texts) are after fetch()."""
    return os.path.join(dest_dir(dest), "x64")


def fetch(dest):
    """Download the release zip, verify it and the files, unpack into DEST/dxc/<tag>/x64. Returns that directory.
    The download is untrusted until its SHA-256 matches: it is read into memory, hashed, and only then opened; the unpack goes into a
    fresh empty temp directory and only the pinned members are written, by fixed names (never the archive's own paths)."""
    out = pair_dir(dest)
    if not check(out, FETCHED_SHA256):
        print(f"dxc: already at {TAG} ({out})")
        return out
    import urllib.request
    print(f"+ fetch {URL}", flush=True)
    data = urllib.request.urlopen(URL, timeout=120).read()
    got = hashlib.sha256(data).hexdigest()
    if got != ARCHIVE_SHA256 or len(data) != ARCHIVE_SIZE:
        sys.exit(f"dxc: {ASSET} is {len(data)} bytes, SHA-256 {got}; pinned {ARCHIVE_SIZE} bytes, {ARCHIVE_SHA256}" + "\nFix: run again (a download can be cut short). If it fails the same way, report it; don't edit the pin to match.")
    tmp = tempfile.mkdtemp(prefix="dxc-")   # fresh and empty
    try:
        zpath = os.path.join(tmp, ASSET)
        with open(zpath, "wb") as f:
            f.write(data)
        stage = os.path.join(tmp, "x")
        os.makedirs(stage)
        with zipfile.ZipFile(zpath) as z:
            names = set(z.namelist())
            for member, sha in MEMBERS.items():
                if member not in names:
                    sys.exit(f"dxc: {member} is not in {ASSET}")
                blob = z.read(member)
                if hashlib.sha256(blob).hexdigest() != sha:
                    sys.exit(f"dxc: {member} in {ASSET} does not match its pinned SHA-256 {sha}" + "\nFix: run again (a download can be cut short). If it fails the same way, report it; don't edit the pin to match.")
                with open(os.path.join(stage, os.path.basename(member)), "wb") as f:
                    f.write(blob)
        os.makedirs(out, exist_ok=True)
        for n in os.listdir(stage):
            shutil.move(os.path.join(stage, n), os.path.join(out, n))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    problems = check(out, FETCHED_SHA256)
    if problems:
        sys.exit("dxc: " + "; ".join(problems) + f"\nFix: delete {out} and run again.")
    print(f"dxc: {TAG} fetched into {out}")
    return out


def main(argv):
    if len(argv) == 2 and argv[0] == "fetch":
        fetch(os.path.abspath(argv[1]))
    elif len(argv) == 2 and argv[0] == "verify":
        problems = verify_pair(argv[1])
        for p in problems:
            print("dxc: " + p)
        print("dxc: pair ok" if not problems else "dxc: MISMATCH")
        sys.exit(1 if problems else 0)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
