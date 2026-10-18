#!/usr/bin/env python3
"""Fetch the pinned N64ModernRuntime + RT64 (and optionally RecompFrontend) and apply our patches.

Works on Windows, Linux and macOS (needs git; no `patch` binary). Everything fetched here is
upstream code; the patches are ours (recomp-host/patches/). Nothing ROM-derived is involved.

usage: python3 -I tools/setup_runtime.py DEST [--patches-only] [--frontend] [--no-ui | --ui-only] [--no-patch] [--dxc | --dxc-only] [--recreate]

  DEST            becomes the RECOMP_RUNTIME_DIR (DEST/lib/N64ModernRuntime, DEST/lib/rt64, ...)
  --patches-only  fetch only what the patches touch (rt64 + its plume submodule): the CI patch-apply check
  --frontend      also fetch RecompFrontend (the unlicensed launcher build; local use only)
  --no-ui         do not fetch the in-game settings menu's libraries and fonts (see below; build with -DTHPS_MENU=OFF)
  --ui-only       fetch only those (what the CMake configure runs when they are missing); --ui is accepted and means the default
  --no-patch      fetch the pins, apply nothing
  --dxc           also fetch the Windows zip's DXC DLLs (the default on Windows); --dxc-only fetches only those (what the Windows CMake configure runs)
  --recreate      a checkout whose patches don't match the patch files (or the pin) is moved aside to <name>.stale-<time> and
                  fetched and patched again (without it, setup refuses: see "The marker" below)

The in-game settings menu (THPS_MENU, on by default) needs RmlUi, lunasvg and FreeType sources (DEST/lib) and its fonts (DEST/ui-fonts):
pinned commits / SHA-256 below. The fonts are not in git on purpose (third-party binaries stay out of the repo); they are SIL OFL 1.1 and
the release bundle carries their licence text (ui/FONTS-LICENSE.txt).

The Windows zip's dxil.dll + dxcompiler.dll are NOT RT64's contrib/dxc copies (v1.7.2212, whose dxil licence forbids redistribution) but
Microsoft's DXC v1.8.2407 release, downloaded into DEST/dxc/v1.8.2407/x64 with the archive and each file SHA-256-checked (tools/dxc_pins.py).
Nothing is committed to git. RT64's own contrib/dxc still provides the build-time dxc (headers, dxc.exe: its output is the embedded shader blobs).

Pins (as in CBFD-Recompiled V0.1.5): N64ModernRuntime cdf5abb, rt64 4337374, RecompFrontend b1a1477.
Patches, in this order: rt64-hlslpp-labs, rt64-present-capture-widescreen, rt64-hfr, rt64-metal-clear-state;
then to N64ModernRuntime: ultramodern-sp-complete-after-dl. (ultramodern-vi-rate
is not applied: it is for real PAL timing, see README.)

The marker (<repo>/.thps-patches-applied) records the pin and each applied patch's SHA-256. A rerun applies patches added to the
END of the list on top of an old checkout. Anything else (a patch whose file changed since it was applied, a different pin, a
patch removed or reordered, or the old filename-only marker, whose content is unknown) means the tree is not what the patch files
describe: setup refuses and says to recreate it (a new DEST, or --recreate). A tree with local changes and no marker is refused
too. (Astra review P9: a patch edited in place used to be reported "already applied".) When the marker matches, each applied
patch must still reverse-apply (`git apply -R --check`, last patch first, on a scratch copy of the files): a patched file edited
after setup is refused the same way.

  python3 -I tools/setup_runtime.py --self-test [RUNTIME_DIR]
                  the marker and reverse-apply checks on a synthetic repository; with RUNTIME_DIR (a set-up runtime), also on a
                  scratch copy of its rt64 checkout (APFS clone where available): intact passes, an edited patched file is refused
"""
import hashlib
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)   # -I does not put the script's directory on the path
import dxc_pins  # noqa: E402
PATCHES = os.path.join(os.path.dirname(HERE), "patches")

PINS = {
    "N64ModernRuntime": ("https://github.com/N64Recomp/N64ModernRuntime.git", "cdf5abbd5026fef5c364c676e4667c45e42b6863"),
    "rt64": ("https://github.com/rt64/rt64.git", "43373749dac9bbc1b653e6a02aed40a9e1783bed"),
    "RecompFrontend": ("https://github.com/N64Recomp/RecompFrontend.git", "b1a1477c6556aeb7ed45defbfb5924f721efebc1"),
}
# In-game settings menu (THPS_MENU; src/ui/). No submodules are needed.
UI_PINS = {
    "RmlUi": ("https://github.com/mikke89/RmlUi.git", "7a06f27db04fe5d13a5dacc19b2b4544673a4eca"),             # 6.0, as Zelda64Recomp b65c482
    "lunasvg": ("https://github.com/sammycage/lunasvg.git", "4166d0cccfc059b39d5ecfc372524375e59448f9"),     # as Zelda64Recomp b65c482
    "freetype": ("https://github.com/freetype/freetype.git", "42608f77f20749dd6ddc9e0536788eaad70ea4b5"),     # VER-2-13-3
}
UI_FONTS_REF = "b65c482ed672258eb684ab94a4e8c8aa662615ee"   # Zelda64Recomp commit the font files are taken from
UI_FONTS = {   # path in Zelda64Recomp's assets/ -> SHA-256 (all SIL OFL 1.1; the Chiaro fonts there are commercial and are NOT fetched)
    "LatoLatin-Regular.ttf": "d785334ac4e7810f571def986bbad41161f68ac385db8813f798bf04d71478e1",
    "LatoLatin-Italic.ttf": "1121daa4b0f0510074f54b046b2a25016614cca419974b992e2b8a2b2231f5da",
    "LatoLatin-Bold.ttf": "74dc638cf436cce77a0217a2a55ce7906a4cc9e4c595c6f1dcd9dd858078c1b6",
    "LatoLatin-BoldItalic.ttf": "116bd3f939070d9df8c73acb76faa8f6b582bcce3c391eb6b3695f40e5ae404a",
    "NotoEmoji-Regular.ttf": "415dc6290378574135b64c808dc640c1df7531973290c4970c51fdeb849cb0c5",
    "promptfont/promptfont.ttf": "7206e62fdb7e07fdf137e52464d7fa862f2f8dc79931fa6500e6ceb8ed1f06b9",
}
RT64_PATCHES = ["rt64-hlslpp-labs.patch", "rt64-present-capture-widescreen.patch", "rt64-hfr.patch", "rt64-metal-clear-state.patch"]
# N64ModernRuntime: "RSP done" goes to the game after RT64 has parsed the display list (see the patch's header).
NMR_PATCHES = ["ultramodern-sp-complete-after-dl.patch"]
# Submodules a --patches-only checkout needs (the patched files only).
PATCHES_ONLY_SUBMODULES = ["src/contrib/plume"]


def git(*args, cwd=None):
    print("+ git " + " ".join(args), flush=True)
    # core.longpaths: RT64's submodules nest deeply, and a Windows checkout under a long path can pass 260 characters.
    # core.autocrlf=false: Git for Windows defaults to CRLF checkouts, and then our LF patches don't apply
    # (CI run 38178976341). `-c` reaches the submodule commands too (GIT_CONFIG_PARAMETERS).
    subprocess.run(["git", *GIT_CFG, *args], cwd=cwd, check=True)


GIT_CFG = ["-c", "core.longpaths=true", "-c", "core.autocrlf=false", "-c", "core.eol=lf"]


def fetch_ui_fonts(dest):
    import urllib.request
    out = os.path.join(dest, "ui-fonts")
    for rel, want in UI_FONTS.items():
        path = os.path.join(out, rel)
        if os.path.isfile(path) and hashlib.sha256(open(path, "rb").read()).hexdigest() == want:
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        url = f"https://raw.githubusercontent.com/Zelda64Recomp/Zelda64Recomp/{UI_FONTS_REF}/assets/{rel}"
        print("+ fetch", url, flush=True)
        data = urllib.request.urlopen(url, timeout=60).read()
        got = hashlib.sha256(data).hexdigest()
        if got != want:
            sys.exit(f"{rel}: SHA-256 {got} is not the pinned {want}.\nFix: run again (a download can be cut short). If it fails "
                     f"the same way, the upstream file changed: report it, and don't edit the pin to match.")
        with open(path, "wb") as f:
            f.write(data)


def fetch_pin(dest, name, submodules):
    url, sha = (PINS | UI_PINS)[name]
    d = os.path.join(dest, "lib", name)
    if os.path.isdir(os.path.join(d, ".git")):
        # A damaged .git (e.g. a half-finished delete) is not a repository: git would walk up and report the enclosing
        # checkout's HEAD. Check that the directory is its own top level first.
        top = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=d, capture_output=True, text=True).stdout.strip()
        if not top or os.path.normcase(os.path.abspath(top)) != os.path.normcase(os.path.abspath(d)):
            sys.exit(f"{d} has a damaged .git (not a repository of its own).\nFix: delete {d} and run again.")
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=d, capture_output=True, text=True).stdout.strip()
        if head != sha:
            sys.exit(f"{d} is at {head[:7]}, not the pin {sha[:7]}.\nFix: delete {d} (or use a new empty DEST) and run again.")
        print(f"{name}: already at {sha[:7]}")
        # The tree may come from a --patches-only run (one submodule only): make sure the submodules this run asks for are there.
        # Already-initialized submodules at their commit are left alone (their patched files included).
        update_submodules(d, submodules)
        return d
    os.makedirs(d, exist_ok=True)
    git("init", "-q", cwd=d)
    git("config", "core.autocrlf", "false", cwd=d)
    git("remote", "add", "origin", url, cwd=d)
    git("fetch", "-q", "--depth", "1", "origin", sha, cwd=d)
    git("checkout", "-q", "FETCH_HEAD", cwd=d)
    update_submodules(d, submodules)
    return d


def update_submodules(d, submodules):
    if submodules is True:
        git("submodule", "update", "--init", "--recursive", "--depth", "1", "--jobs", "4", cwd=d)
    elif submodules:
        git("submodule", "update", "--init", "--recursive", "--depth", "1", "--", *submodules, cwd=d)


MARKER = ".thps-patches-applied"
MARKER_HEADER = "# thps-patches v2: pin <sha>, then patch <file> <sha256> in the order applied"


def sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def read_marker(path):
    """(pin, [(patch, sha256), ...]) of a v2 marker; None for an older (filename-only) marker."""
    with open(path) as f:
        lines = [l.strip() for l in f if l.strip()]
    if not lines or lines[0] != MARKER_HEADER:
        return None
    pin, done = None, []
    for l in lines[1:]:
        parts = l.split()
        if len(parts) == 2 and parts[0] == "pin":
            pin = parts[1]
        elif len(parts) == 3 and parts[0] == "patch":
            done.append((parts[1], parts[2]))
        else:
            return None
    return pin, done


def write_marker(path, pin, done):
    with open(path, "w") as f:
        f.write(MARKER_HEADER + "\n" + f"pin {pin}\n" + "".join(f"patch {p} {h}\n" for p, h in done))


def marker_problem(repo_dir, pin, patches, want):
    """None if the checkout can take `patches` (already applied, or only later ones missing); else why not."""
    marker = os.path.join(repo_dir, MARKER)
    if not os.path.exists(marker):
        # No marker: a fresh (or --no-patch) checkout. Local changes there are of unknown origin (a hand patch, a half-applied run).
        st = subprocess.run(["git", *GIT_CFG, "status", "--porcelain", "--untracked-files=no"], cwd=repo_dir, capture_output=True,
                            text=True, check=True).stdout.strip()
        return f"it has local changes and no {MARKER} ({st.splitlines()[0]} ...)" if st else None
    m = read_marker(marker)
    if m is None:
        return f"its {MARKER} is the old filename-only kind: what those patches contained is unknown"
    mpin, done = m
    if mpin != pin:
        return f"its patches were applied to {str(mpin)[:7]}, not the pin {pin[:7]}"
    if [p for p, _ in done] != patches[:len(done)]:
        return f"its patches ({', '.join(p for p, _ in done) or 'none'}) are not the start of {', '.join(patches)}"
    for p, h in done:
        if h != want[p]:
            return f"{p} changed since it was applied (SHA-256 {h[:12]} then, {want[p][:12]} now)"
    return None


def patch_paths(path):
    """the files a patch touches (its a/ and b/ sides)"""
    out = set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith(("--- a/", "+++ b/")):
                out.add(line[6:].rstrip("\n").split("\t")[0])
    return out


def applied_intact(repo_dir, applied, patches_dir=None):
    """None if the applied patches still reverse-apply, last first, to the working files (nothing was edited after setup); else why not.
    Works on a scratch copy of the touched files (patches inside submodules included), so the checkout is never changed (Astra P9)."""
    import shutil
    import tempfile
    patches_dir = patches_dir or PATCHES
    tmp = tempfile.mkdtemp(prefix="setup-runtime-check-")
    try:
        work = os.path.join(tmp, "w")
        for p in applied:
            for rel in patch_paths(os.path.join(patches_dir, p)):
                src, dst = os.path.join(repo_dir, rel), os.path.join(work, rel)
                if os.path.isfile(src) and not os.path.exists(dst):
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copy2(src, dst)
        os.makedirs(work, exist_ok=True)
        env = dict(os.environ, GIT_CEILING_DIRECTORIES=tmp)   # `git apply` outside any repository: a plain patch tool here
        for p in reversed(applied):
            path = os.path.join(patches_dir, p)
            r = subprocess.run(["git", *GIT_CFG, "apply", "-R", "--check", path], cwd=work, capture_output=True, text=True, env=env)
            if r.returncode:
                first = (r.stderr.strip().splitlines() or ["?"])[0]
                return (f"{p} is recorded as applied but no longer reverse-applies to the working files ({first}): a patched file was "
                        f"edited after setup")
            subprocess.run(["git", *GIT_CFG, "apply", "-R", path], cwd=work, check=True, capture_output=True, env=env)
        return None
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def apply_patches(repo_dir, pin, patches, recreate=None, patches_dir=None):
    """Apply `patches` (in order) to the checkout at `pin`, recording each one's SHA-256 in the marker.
    A checkout the marker can't vouch for is refused, or with `recreate` (a function that refetches it) recreated."""
    patches_dir = patches_dir or PATCHES
    want = {p: sha256_file(os.path.join(patches_dir, p)) for p in patches}
    why = marker_problem(repo_dir, pin, patches, want)
    if why is None and os.path.exists(os.path.join(repo_dir, MARKER)):
        why = applied_intact(repo_dir, [p for p, _ in read_marker(os.path.join(repo_dir, MARKER))[1]], patches_dir)
    if why:
        if recreate is None:
            sys.exit(f"{repo_dir}: {why}.\nFix: recreate it: run setup into a new empty DEST, or rerun with --recreate "
                     f"(moves this checkout aside and fetches it again). Local edits to patched files belong in the patch files.")
        stale = f"{repo_dir}.stale-{time.strftime('%Y%m%d-%H%M%S')}"
        print(f"{repo_dir}: {why}: moving it to {stale} (delete that when you no longer need it)", flush=True)
        os.rename(repo_dir, stale)
        repo_dir = recreate()
    marker = os.path.join(repo_dir, MARKER)
    done = read_marker(marker)[1] if os.path.exists(marker) else []
    if len(done) == len(patches):
        print(f"patches: already applied ({os.path.basename(repo_dir)}: {', '.join(patches)}, SHA-256 checked)")
        return
    for p in patches[len(done):]:
        path = os.path.join(patches_dir, p)
        # --check first so a drifted pin fails with the patch's name, not halfway through.
        subprocess.run(["git", *GIT_CFG, "apply", "--check", path], cwd=repo_dir, check=True)
        git("apply", path, cwd=repo_dir)
        print(f"patch applied: {p}")
        done.append((p, want[p]))
        write_marker(marker, pin, done)


def self_test(runtime=None):
    import shutil
    import tempfile
    fails = []

    def check(ok, what):
        print(f"  {'ok  ' if ok else 'FAIL'} {what}")
        if not ok:
            fails.append(what)

    def refused(fn):
        try:
            fn()
        except SystemExit as e:
            return str(e)
        return None

    tmp = tempfile.mkdtemp(prefix="setup-runtime-selftest-")
    quiet = dict(capture_output=True, check=True)
    try:
        print("== self-test: a synthetic repository, two patches over the same lines")
        repo, pdir = os.path.join(tmp, "repo"), os.path.join(tmp, "patches")
        os.makedirs(pdir)
        subprocess.run(["git", "init", "-q", repo], **quiet)
        with open(os.path.join(repo, "a.txt"), "w") as f:
            f.write("".join(f"line {i}\n" for i in range(20)))
        subprocess.run(["git", "-C", repo, "add", "a.txt"], **quiet)
        subprocess.run(["git", "-C", repo, "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgSign=false",
                        "commit", "-q", "-m", "base"], **quiet)
        pin = subprocess.run(["git", "-C", repo, "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
        for name, (old, new) in (("one.patch", ("line 10\n", "line 10 patched\n")), ("two.patch", ("line 10 patched\n", "line 10 twice\n"))):
            text = open(os.path.join(repo, "a.txt")).read()
            with open(os.path.join(repo, "a.txt"), "w") as f:
                f.write(text.replace(old, new))
            diff = subprocess.run(["git", "-C", repo, "diff"], capture_output=True, text=True, check=True).stdout
            with open(os.path.join(pdir, name), "w") as f:
                f.write(diff)
            subprocess.run(["git", "-C", repo, "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgSign=false",
                            "commit", "-q", "-am", name], **quiet)
        subprocess.run(["git", "-C", repo, "checkout", "-q", pin], **quiet)
        pl = ["one.patch", "two.patch"]
        check(refused(lambda: apply_patches(repo, pin, pl, patches_dir=pdir)) is None, "fresh checkout: both patches apply")
        check(refused(lambda: apply_patches(repo, pin, pl, patches_dir=pdir)) is None,
              "rerun: already applied (the marker matches and both reverse-apply, last first)")
        with open(os.path.join(repo, "a.txt"), "a") as f:
            f.write("an edit far from the patches\n")
        check(refused(lambda: apply_patches(repo, pin, pl, patches_dir=pdir)) is None, "an edit outside the patched lines is tolerated")
        text = open(os.path.join(repo, "a.txt")).read()
        with open(os.path.join(repo, "a.txt"), "w") as f:
            f.write(text.replace("line 10 twice\n", "line 10 hand-edited\n"))
        why = refused(lambda: apply_patches(repo, pin, pl, patches_dir=pdir))
        check(bool(why) and "two.patch" in why and "edited after setup" in why and "Fix:" in why,
              "a patched line edited after setup is refused, naming the patch")
        if runtime:
            src = os.path.join(os.path.abspath(runtime), "lib", "rt64")
            print(f"== self-test: a scratch copy of {src} (never the original)")
            dst = os.path.join(tmp, "rt64")
            if sys.platform == "darwin":
                subprocess.run(["cp", "-c", "-R", src, dst], check=True)      # APFS clone: instant, no extra space
            else:
                shutil.copytree(src, dst, symlinks=True)
            check(refused(lambda: apply_patches(dst, PINS["rt64"][1], RT64_PATCHES)) is None,
                  "the runtime's rt64: marker matches and every patch reverse-applies")
            victim = sorted(patch_paths(os.path.join(PATCHES, RT64_PATCHES[-1])))[0]
            with open(os.path.join(dst, victim), "r+", encoding="utf-8", errors="surrogateescape") as f:
                lines = f.read().split("\n")
                diff = open(os.path.join(PATCHES, RT64_PATCHES[-1]), encoding="utf-8", errors="replace").read().split("\n")
                added = next(l[1:] for l in diff if l.startswith("+") and not l.startswith("+++") and l[1:].strip())
                i = lines.index(added)
                lines[i] = added + " // edited after setup"
                f.seek(0)
                f.write("\n".join(lines))
                f.truncate()
            why = refused(lambda: apply_patches(dst, PINS["rt64"][1], RT64_PATCHES))
            check(bool(why) and RT64_PATCHES[-1] in why, f"the runtime's rt64 with a patched line of {victim} edited is refused")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if fails:
        print(f"SELF-TEST FAILED: {len(fails)} case(s). Fix: tools/setup_runtime.py")
        return 1
    print("self-test passed")
    return 0


def main(argv):
    if argv[:1] == ["--self-test"]:
        sys.exit(self_test(argv[1] if len(argv) > 1 else None))
    args = [a for a in argv if not a.startswith("--")]
    flags = {a for a in argv if a.startswith("--")}
    if len(args) != 1 or flags - {"--patches-only", "--frontend", "--no-patch", "--ui", "--no-ui", "--ui-only", "--dxc", "--dxc-only", "--recreate"}:
        sys.exit(__doc__)
    dest = os.path.abspath(args[0])
    only = "--patches-only" in flags
    ui_only = "--ui-only" in flags
    dxc_only = "--dxc-only" in flags
    if dxc_only:
        dxc_pins.fetch(dest)
        return
    rt64_subs = PATCHES_ONLY_SUBMODULES if only else True
    nmr_subs = [] if only else True   # --patches-only: the patch touches the repository itself, no submodules
    if not ui_only:
        rt64 = fetch_pin(dest, "rt64", rt64_subs)
        nmr = fetch_pin(dest, "N64ModernRuntime", nmr_subs)
        if not only:
            if "--frontend" in flags:
                fetch_pin(dest, "RecompFrontend", True)
    if ui_only or (not only and "--no-ui" not in flags):
        for name in UI_PINS:
            fetch_pin(dest, name, False)
        fetch_ui_fonts(dest)
    if not ui_only and "--no-patch" not in flags:
        recreate = "--recreate" in flags
        apply_patches(rt64, PINS["rt64"][1], RT64_PATCHES, (lambda: fetch_pin(dest, "rt64", rt64_subs)) if recreate else None)
        apply_patches(nmr, PINS["N64ModernRuntime"][1], NMR_PATCHES,
                      (lambda: fetch_pin(dest, "N64ModernRuntime", nmr_subs)) if recreate else None)
    if "--dxc" in flags or (os.name == "nt" and not only and not ui_only):
        dxc_pins.fetch(dest)
    print(f"runtime ready: -DRECOMP_RUNTIME_DIR={dest}")


if __name__ == "__main__":
    main(sys.argv[1:])
