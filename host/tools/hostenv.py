"""Where the shared host tools find the game they run for.

The tools in this directory are shared by THPS1-3. Each game's recomp-host/tools/ has a thin wrapper of the same name
that sets $THPS_HOST (the game's recomp-host directory) and runs the shared file (`run_shared`). A tool reads the game
from there: host_dir(), game() (game/game.toml), family(). The wrapper finds this tree with the game's host_root.py
($THPS_HOST_ROOT, else decomp/common: a directory here, the THPS64Host submodule in a game repository); the shared tools
find everything of their own from this file's location, never from the game's.
"""
import os
import runpy
import shutil
import stat
import subprocess
import sys
import tomllib

TOOLS = os.path.dirname(os.path.abspath(__file__))     # decomp/common/host/tools
SHARED = os.path.dirname(TOOLS)                        # decomp/common/host
COMMON = os.path.dirname(SHARED)                       # decomp/common: the host tree's root (THPS64Host)
sys.path.insert(0, COMMON)                             # thpsgame, gen_versions, ... (decomp/common)


def host_dir():
    h = os.environ.get("THPS_HOST")
    if not h:
        sys.exit("THPS_HOST is not set: run this tool through the game's wrapper, decomp/thps<N>/recomp-host/tools/<name>")
    return os.path.abspath(h)


def game(host=None):
    with open(os.path.join(host or host_dir(), "game", "game.toml"), "rb") as f:
        return tomllib.load(f)


def family(host=None):
    return game(host)["family"]


def rmtree(path):
    """shutil.rmtree that also deletes read-only files. Git marks its object files read-only, and on Windows
    shutil.rmtree(ignore_errors=True) then silently keeps them: a half-deleted .git survives, and `git rev-parse` in it
    walks up to the enclosing repository (seen 2026-10-12: build_host.py --clean, then "rt64 is at <our HEAD>")."""
    def unlock(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    if os.path.lexists(path):
        if sys.version_info >= (3, 12):
            shutil.rmtree(path, onexc=unlock)
        else:
            shutil.rmtree(path, onerror=unlock)


def load_msvc_env():
    """Windows: run vcvars64.bat of the newest Visual Studio with the C++ tools and import its environment."""
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = os.path.join(pf86, "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.exists(vswhere):
        sys.exit("Visual Studio not found (no vswhere.exe).\nFix: install Visual Studio 2022 with the components in BUILDING.md, \"Dependencies\".")
    out = subprocess.run([vswhere, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                          "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
    if not out:
        sys.exit("Visual Studio has no 'MSVC x64/x86 build tools' component.\nFix: install the 'Desktop development with C++' workload.")
    vcvars = os.path.join(out, "VC", "Auxiliary", "Build", "vcvars64.bat")
    env_dump = subprocess.run(f'cmd /c ""{vcvars}" >nul && set"', capture_output=True, text=True, shell=True).stdout
    for line in env_dump.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            os.environ[k] = v
    llvm = os.path.join(out, "VC", "Tools", "Llvm", "x64", "bin")
    if os.path.isdir(llvm):
        os.environ["PATH"] = llvm + os.pathsep + os.environ["PATH"]
    print(f"Visual Studio: {out}")


def run_shared(wrapper_file, name=None):
    """For a game's wrapper: run the shared tool of the same name (or `name`) for the game that owns the wrapper."""
    wrapper = os.path.abspath(wrapper_file)
    host = os.path.dirname(os.path.dirname(wrapper))
    os.environ["THPS_HOST"] = host
    os.environ["THPS_HOST_ROOT"] = COMMON              # the host tree (decomp/common, or an override) for child processes
    shared = os.path.join(SHARED, os.path.basename(os.path.dirname(wrapper)), name or os.path.basename(wrapper))
    sys.path.insert(0, os.path.dirname(shared))
    sys.argv[0] = shared
    runpy.run_path(shared, run_name="__main__")
