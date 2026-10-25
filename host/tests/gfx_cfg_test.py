#!/usr/bin/env python3
"""graphics.cfg defaults + migration (HUD Placement defaults to `expand`, 2026-10-14). Stdlib only, no ROM, no window: the host exits at
--check-rom right after it loaded graphics.cfg. Usage: gfx_cfg_test.py [path/to/<game>_recomp]   ($THPS_HOST names the game: run it through the game's tests/ wrapper)

Checks, each in a fresh temp config dir:
  1. a fresh config dir: graphics.cfg is written with hud=expand and cfg_rev=2;
  2. a graphics.cfg from before cfg_rev (as every earlier build saved it: hud=original, the old default) moves to hud=expand once,
     keeps its other values, and is stamped cfg_rev=2;
  3. after that, hud=original is a choice and stays (saved by the menu / --hud original): not migrated again;
  4. a stamped file with hud=16:9 stays; an old file with hud=16:9 stays too (only the old default moves);
  5. --hud original for one run is saved back with cfg_rev (so it sticks next run), and --hud original / --aspect original never touch the other keys;
  6. split (two-player split screen in widescreen): a fresh dir and a file without the key get split=wide (the default); --split original
     is saved and kept, the other keys untouched; a bad value (also the old 1|0 form) is ignored with a message.
"""
import pathlib, subprocess, sys, tempfile

import os, re, tomllib
if not os.environ.get("THPS_HOST"):
    raise SystemExit("THPS_HOST is not set: run this through decomp/thps<N>/recomp-host/tests/gfx_cfg_test.py")
HERE = pathlib.Path(os.environ["THPS_HOST"]).resolve()    # the game's recomp-host directory
with open(HERE / "game" / "game.toml", "rb") as _f:
    _GAME = tomllib.load(_f)
_EXE = f"{_GAME['family']}_recomp.exe" if sys.platform == "win32" else f"{_GAME['family']}_recomp"
EXE = sys.argv[1] if len(sys.argv) > 1 else str(HERE / "build" / "cmake" / _EXE)
_EMPTY = tempfile.NamedTemporaryFile(prefix="empty-rom-", delete=False)
_EMPTY.close()

# What a build from before the change saved (Q18 defaults + the hud line it added, default original).
OLD = """# graphics settings
aspect=21:9
window=1600x900
res_scale=3
msaa=2
fullscreen=0
hud=%s
refresh=60
"""


def run(cfg, *flags):
    p = subprocess.run([EXE, "--config", str(cfg), "--check-rom", _EMPTY.name, *flags], capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout + p.stderr


def value(cfg, key):
    m = re.search(r"(?m)^%s=(.*)$" % re.escape(key), (cfg / "graphics.cfg").read_text())
    return m.group(1) if m else None


if "frontend=1" in subprocess.run([EXE, "--features"], capture_output=True, text=True, timeout=30).stdout:
    print("skipped: the launcher build keeps its graphics settings in graphics.json (no hud there)")
    sys.exit(0)

fails = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails.append(what)


with tempfile.TemporaryDirectory(prefix="thps-gfx-") as tmp:
    tmp = pathlib.Path(tmp)

    d = tmp / "fresh"; d.mkdir()
    run(d)
    check(value(d, "hud") == "expand" and value(d, "cfg_rev") == "2", "fresh dir: hud=expand, cfg_rev=2")

    d = tmp / "old"; d.mkdir(); (d / "graphics.cfg").write_text(OLD % "original")
    rc, out = run(d)
    check(value(d, "hud") == "expand" and value(d, "cfg_rev") == "2", "pre-cfg_rev hud=original: moved to expand, stamped")
    check(out.count("moved to the new default") == 1, "pre-cfg_rev hud=original: one log line")
    check((value(d, "aspect"), value(d, "window"), value(d, "res_scale"), value(d, "msaa"), value(d, "refresh")) == ("21:9", "1600x900", "3", "2", "60"),
          "pre-cfg_rev hud=original: the other values are kept")
    rc, out = run(d)
    check("moved to the new default" not in out and value(d, "hud") == "expand", "migrated once: second start says nothing, still expand")

    # the user picks Original afterwards (menu / --hud original): stamped file, stays
    rc, out = run(d, "--hud", "original")
    check(value(d, "hud") == "original" and value(d, "cfg_rev") == "2", "--hud original: saved back with cfg_rev")
    rc, out = run(d)
    check(value(d, "hud") == "original" and "moved to the new default" not in out, "hud=original with cfg_rev stays original")

    d = tmp / "stamped"; d.mkdir(); (d / "graphics.cfg").write_text("cfg_rev=2\n" + OLD % "original")
    rc, out = run(d)
    check(value(d, "hud") == "original" and "moved to the new default" not in out, "stamped file with hud=original: untouched")

    d = tmp / "other"; d.mkdir(); (d / "graphics.cfg").write_text(OLD % "16:9")
    rc, out = run(d)
    check(value(d, "hud") == "16:9" and value(d, "cfg_rev") == "2", "pre-cfg_rev hud=16:9: kept (only the old default moves)")

    d = tmp / "nohud"; d.mkdir(); (d / "graphics.cfg").write_text("aspect=16:9\nwindow=auto\nres_scale=0\nmsaa=4\nfullscreen=0\nrefresh=display\n")
    rc, out = run(d)
    check(value(d, "hud") in (None, "expand"), "file without a hud line: no hud=original appears")
    rc, out = run(d, "--aspect", "original")
    check(value(d, "hud") in (None, "expand") and value(d, "aspect") == "original", "--aspect original: hud setting not changed (4:3 never moves the HUD by itself)")

    d = tmp / "split"; d.mkdir()
    run(d)
    check(value(d, "split") == "wide", "fresh dir: split=wide (the default)")
    rc, out = run(d, "--split", "original")
    check(value(d, "split") == "original" and value(d, "hud") == "expand", "--split original: saved, the other keys kept")
    rc, out = run(d)
    check(value(d, "split") == "original", "split=original stays on the next start")
    (d / "graphics.cfg").write_text((d / "graphics.cfg").read_text().replace("split=original", "split=0"))
    rc, out = run(d)
    check("ignoring split=0" in out, "split=0: ignored with a message (the default applies)")
    d = tmp / "nosplit"; d.mkdir(); (d / "graphics.cfg").write_text("cfg_rev=2\n" + OLD % "16:9")
    rc, out = run(d, "--msaa", "4")
    check(value(d, "split") == "wide", "file without a split line: wide")

print("\n%s" % ("ALL PASS" if not fails else "%d FAILED" % len(fails)))
sys.exit(1 if fails else 0)
