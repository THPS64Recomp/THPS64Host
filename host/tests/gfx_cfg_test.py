#!/usr/bin/env python3
"""graphics.cfg defaults + migration (HUD Placement defaults to `expand`, 2026-10-14). Stdlib only, no ROM, no window: the host exits at
--check-rom right after it loaded graphics.cfg. Usage: gfx_cfg_test.py [path/to/<game>_recomp]   ($THPS_HOST names the game: run it through the game's tests/ wrapper)

Checks, each in a fresh temp config dir:
  1. a fresh config dir: graphics.cfg is written with hud=expand, refresh=60 and cfg_rev=3;
  2. a graphics.cfg from before cfg_rev (as every earlier build saved it: hud=original, the old default) moves to hud=expand once,
     keeps its other values, and is stamped cfg_rev=3;
  3. after that, hud=original is a choice and stays (saved by the menu / --hud original): not migrated again;
  4. a stamped file with hud=16:9 stays; an old file with hud=16:9 stays too (only the old default moves);
  5. --hud original for one run is saved back with cfg_rev (so it sticks next run), and --hud original / --aspect original never touch the other keys;
  6. split (two-player split screen in widescreen): a fresh dir and a file without the key get split=wide (the default); --split original
     is saved and kept, the other keys untouched; a bad value (also the old 1|0 form) is ignored with a message.
  7. border (src/border_crop.cpp): a fresh dir and a file without the key get border=fill (the default, from the Enhanced preset); --border fill is saved; a bad value
     (also `crop`, the value's name before v0.2 was released: no shim) is ignored with a message.
  9. refresh (Frame Rate, cfg_rev 3: Original (30) or 60, default 60): a file before rev 3 with refresh=display, -1 or 120 moves to 60 once
     with one log line; refresh=original stays; a rev-3 file with refresh=120 is ignored with a message (60 applies); --refresh 120 is refused.
  8. dd_buffers (src/draw_distance.cpp): a fresh dir has no dd_buffers line (the default, bigger, is not written); --dd-buffers original is
     saved and kept, the other keys untouched; --dd-buffers bigger removes the line again; a bad value is ignored with a message.
  10. presets (Original / Enhanced, v0.2): a fresh dir gets Enhanced (16:9, expand, wide, fill, 60, full); --preset original sets
     the six rows and leaves MSAA and fog alone; a later flag makes it custom; --preset custom is refused; an edited file reads custom; an
     older file (no border, no draw_distance, no fog) keeps its values and takes the new defaults for the missing keys.
  11. fog (src/fog.cpp; not in a preset or the menu): a fresh dir gets fog=original; --fog off and --fog soft are saved, the other keys
     untouched, and the preset still reads Enhanced; a bad value is ignored with a message (the default applies).
  12. the state in force (the start line's "live:" part): a fresh dir, an empty file and the legacy-defaults migration start with Enhanced
     applied to the hooks (border fill, HUD placement, 16:9; fog original), not only saved; a file at Original starts with none of it; a
     file with fog=soft starts with Soft in force.
  13. draw_distance names (2026-10-26: original | 2x | 4x | full): a file with draw_distance=far / max reads as 2x / 4x and is rewritten
     with the new name; --draw-distance max is accepted (4x); a bad value is ignored with a message.
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
    check(value(d, "hud") == "expand" and value(d, "cfg_rev") == "3", "fresh dir: hud=expand, cfg_rev=3")

    d = tmp / "old"; d.mkdir(); (d / "graphics.cfg").write_text(OLD % "original")
    rc, out = run(d)
    check(value(d, "hud") == "expand" and value(d, "cfg_rev") == "3", "pre-cfg_rev hud=original: moved to expand, stamped")
    check(out.count("moved to the new default") == 1, "pre-cfg_rev hud=original: one log line")
    check((value(d, "aspect"), value(d, "window"), value(d, "res_scale"), value(d, "msaa"), value(d, "refresh")) == ("21:9", "1600x900", "3", "2", "60"),
          "pre-cfg_rev hud=original: the other values are kept")
    rc, out = run(d)
    check("moved to the new default" not in out and value(d, "hud") == "expand", "migrated once: second start says nothing, still expand")

    # the user picks Original afterwards (menu / --hud original): stamped file, stays
    rc, out = run(d, "--hud", "original")
    check(value(d, "hud") == "original" and value(d, "cfg_rev") == "3", "--hud original: saved back with cfg_rev")
    rc, out = run(d)
    check(value(d, "hud") == "original" and "moved to the new default" not in out, "hud=original with cfg_rev stays original")

    d = tmp / "stamped"; d.mkdir(); (d / "graphics.cfg").write_text("cfg_rev=2\n" + OLD % "original")
    rc, out = run(d)
    check(value(d, "hud") == "original" and "moved to the new default" not in out, "stamped file with hud=original: untouched")

    d = tmp / "other"; d.mkdir(); (d / "graphics.cfg").write_text(OLD % "16:9")
    rc, out = run(d)
    check(value(d, "hud") == "16:9" and value(d, "cfg_rev") == "3", "pre-cfg_rev hud=16:9: kept (only the old default moves)")

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
    d = tmp / "border"; d.mkdir()
    run(d)
    check(value(d, "border") == "fill", "fresh dir: border=fill (the default, Enhanced)")
    rc, out = run(d, "--border", "original")
    check(value(d, "border") == "original" and value(d, "hud") == "expand", "--border original: saved, the other keys kept")
    rc, out = run(d, "--border", "fill")
    check(value(d, "border") == "fill", "--border fill: saved")
    (d / "graphics.cfg").write_text((d / "graphics.cfg").read_text().replace("border=fill", "border=sideways"))
    rc, out = run(d)
    check("ignoring border=sideways" in out, "border=sideways: ignored (the default applies)")
    (d / "graphics.cfg").write_text(re.sub(r"(?m)^border=.*\n", "", (d / "graphics.cfg").read_text()) + "border=crop\n")
    rc, out = run(d)
    check("ignoring border=crop" in out and " border=fill " in out, "border=crop (the unreleased old name): ignored with a message, no shim (the default applies)")
    d = tmp / "noborder"; d.mkdir(); (d / "graphics.cfg").write_text("cfg_rev=2\n" + OLD % "16:9")
    rc, out = run(d)
    check((value(d, "border") is None and " border=fill " in out) or value(d, "border") == "fill", "file without a border line: fill (the default since the Enhanced preset)")

    d = tmp / "fresh"
    check(value(d, "refresh") == "60", "fresh dir: refresh=60 (the default)")
    for old in ("display", "-1", "120"):
        d = tmp / f"rr{old}"; d.mkdir(); (d / "graphics.cfg").write_text("cfg_rev=2\n" + OLD.replace("refresh=60", f"refresh={old}") % "16:9")
        rc, out = run(d)
        check(value(d, "refresh") == "60" and value(d, "cfg_rev") == "3" and out.count("Frame Rate is now") == 1, f"rev-2 refresh={old}: moved to 60 once, stamped")
        rc, out = run(d)
        check(value(d, "refresh") == "60" and "Frame Rate is now" not in out, f"rev-2 refresh={old}: second start says nothing")
    d = tmp / "rrorig"; d.mkdir(); (d / "graphics.cfg").write_text(OLD.replace("refresh=60", "refresh=original") % "16:9")
    rc, out = run(d)
    check(value(d, "refresh") == "original" and "Frame Rate is now" not in out, "pre-cfg_rev refresh=original: kept")
    d = tmp / "rr120new"; d.mkdir(); (d / "graphics.cfg").write_text("cfg_rev=3\n" + OLD.replace("refresh=60", "refresh=120") % "16:9")
    rc, out = run(d)
    check("ignoring refresh=120" in out and value(d, "refresh") in ("120", "60"), "rev-3 refresh=120: ignored with a message")
    d = tmp / "rrflag"; d.mkdir()
    rc, out = run(d, "--refresh", "120")
    check(rc != 0 and "bad --refresh 120" in out, "--refresh 120: refused (bad --refresh)")

    d = tmp / "ddbuf"; d.mkdir()
    run(d)
    check(value(d, "dd_buffers") is None, "fresh dir: no dd_buffers line (the default, bigger)")
    rc, out = run(d, "--dd-buffers", "original")
    check(value(d, "dd_buffers") == "original" and value(d, "hud") == "expand", "--dd-buffers original: saved, the other keys kept")
    rc, out = run(d)
    check(value(d, "dd_buffers") == "original", "dd_buffers=original stays on the next start")
    rc, out = run(d, "--dd-buffers", "bigger")
    check(value(d, "dd_buffers") is None, "--dd-buffers bigger: the line goes (the default)")
    (d / "graphics.cfg").write_text((d / "graphics.cfg").read_text() + "dd_buffers=huge\n")
    rc, out = run(d)
    check("ignoring dd_buffers=huge" in out, "dd_buffers=huge: ignored with a message (the default applies)")

    # 10. Graphics presets (v0.2): Original and Enhanced set aspect, hud, split, border, refresh and draw_distance; anything else reads as custom.
    six = ("aspect", "hud", "split", "border", "refresh", "draw_distance")
    enhanced = ("16:9", "expand", "wide", "fill", "60", "full")
    original = ("original", "original", "original", "original", "original", "original")
    d = tmp / "pfresh"; d.mkdir()
    rc, out = run(d)
    check(tuple(value(d, k) for k in six) == enhanced and " preset=enhanced" in out and value(d, "fog") == "original",
          "fresh dir: the Enhanced preset (16:9, expand, wide, fill, 60, full), saved; fog original")
    rc, out = run(d, "--fog", "off")
    rc, out = run(d, "--preset", "original")
    check(tuple(value(d, k) for k in six) == original and " preset=original" in out and value(d, "msaa") == "4" and value(d, "fog") == "off",
          "--preset original: the six rows, MSAA and fog untouched")
    rc, out = run(d, "--fog", "original")
    rc, out = run(d)
    check(" preset=original" in out, "preset original stays on the next start (from the values)")
    rc, out = run(d, "--preset", "original", "--refresh", "60")
    check(value(d, "refresh") == "60" and value(d, "aspect") == "original" and " preset=custom" in out, "--preset original --refresh 60: a later flag changes one row -> custom")
    rc, out = run(d, "--preset", "enhanced")
    check(tuple(value(d, k) for k in six) == enhanced and " preset=enhanced" in out, "--preset enhanced: the six rows back")
    rc, out = run(d, "--preset", "custom")
    check(rc != 0 and "bad --preset custom" in out, "--preset custom: refused (Custom is shown, not chosen)")
    (d / "graphics.cfg").write_text(re.sub(r"(?m)^border=.*$", "border=original", (d / "graphics.cfg").read_text()))
    rc, out = run(d)
    check(" preset=custom" in out and value(d, "draw_distance") == "full", "an edited file (border=original, the rest Enhanced): custom, values kept")
    # An older file (v0.1 / early v0.2: no border, no draw_distance line) keeps every value it has; the missing keys take the new defaults.
    d = tmp / "pold"; d.mkdir(); (d / "graphics.cfg").write_text("cfg_rev=3\n" + OLD % "original")
    rc, out = run(d, "--msaa", "2")
    check((value(d, "aspect"), value(d, "window"), value(d, "res_scale"), value(d, "hud"), value(d, "refresh")) == ("21:9", "1600x900", "3", "original", "60"),
          "older file: every value it has is kept")
    check(value(d, "border") == "fill" and value(d, "draw_distance") == "full" and value(d, "fog") == "original" and " preset=custom" in out,
          "older file: missing border / draw_distance / fog take the defaults (fill, full, original); preset custom")

    # 11. Fog (src/fog.cpp): original | soft | off, default original; graphics.cfg and --fog only (not in a preset or the menu).
    d = tmp / "fog"; d.mkdir()
    run(d)
    check(value(d, "fog") == "original", "fresh dir: fog=original (the default)")
    rc, out = run(d, "--fog", "off")
    check(value(d, "fog") == "off" and value(d, "border") == "fill" and " fog=off " in out, "--fog off: saved, the other keys kept")
    rc, out = run(d)
    check(value(d, "fog") == "off", "fog=off stays on the next start")
    rc, out = run(d, "--fog", "soft")
    check(value(d, "fog") == "soft" and " preset=enhanced" in out, "--fog soft: saved; the preset still reads enhanced (fog is not part of it)")
    (d / "graphics.cfg").write_text((d / "graphics.cfg").read_text().replace("fog=soft", "fog=thick"))
    rc, out = run(d)
    check("ignoring fog=thick" in out and " fog=original " in out, "fog=thick: ignored with a message (the default applies)")
    rc, out = run(d, "--fog", "thick")
    check(rc != 0 and "bad --fog thick" in out, "--fog thick: refused (bad --fog)")

    # 12. What is in force, not only what is saved (review 2026-10-26, finding 2): the start line's "live:" part is what gfx_config handed the
    # hooks. A fresh dir (no graphics.cfg: no setter runs while loading), an empty file, and the legacy-defaults migration (an early return) must
    # all start with Enhanced in force: border fill, HUD placement, 16:9 (fog original); a file saying Original must start with all of it off,
    # and a file saying fog=soft with Soft in force.
    live = lambda out: (re.search(r"live: crop=(\d) fog=(\S+) hud=(\d) aspect=([\d.-]+)", out) or [None] * 5)
    d = tmp / "live-fresh"; d.mkdir()
    rc, out = run(d)
    m = live(out)
    check(m[0] is not None and (m[1], m[2], m[3], m[4]) == ("1", "original", "1", "1.778"), "fresh dir (no graphics.cfg): Enhanced in force at start (crop=1 fog=original hud=1 aspect=1.778): %s" % (m[0],))
    rc, out = run(d)
    m2 = live(out)
    check(m2[0] is not None and m2[0] == m[0], "fresh dir, second start (the file written by the first): the same state in force")
    d = tmp / "live-empty"; d.mkdir(); (d / "graphics.cfg").write_text("# nothing\n")
    rc, out = run(d)
    m = live(out)
    check(m[0] is not None and (m[1], m[2], m[3]) == ("1", "original", "1"), "an empty graphics.cfg: Enhanced in force at start: %s" % (m[0],))
    d = tmp / "live-legacy"; d.mkdir(); (d / "graphics.cfg").write_text("aspect=original\nwindow=960x720\nres_scale=0\nmsaa=0\nfullscreen=0\n")
    rc, out = run(d)
    m = live(out)
    check("had the old defaults" in out and m[0] is not None and (m[1], m[2], m[3], m[4]) == ("1", "original", "1", "1.778"),
          "the legacy-defaults migration (early return): the new defaults in force at start: %s" % (m[0],))
    d = tmp / "live-orig"; d.mkdir(); run(d, "--preset", "original")
    rc, out = run(d)
    m = live(out)
    check(m[0] is not None and (m[1], m[2], m[3], m[4]) == ("0", "original", "0", "0.000"), "a file at Original: nothing in force: %s" % (m[0],))
    d = tmp / "live-fog"; d.mkdir(); run(d, "--fog", "soft")
    rc, out = run(d)
    m = live(out)
    check(m[0] is not None and m[2] == "soft", "a file with fog=soft: Soft in force at start: %s" % (m[0],))

    # 13. Draw distance names: far and max (before 2026-10-26) are read as 2x and 4x and written with the new names.
    for old, new in (("far", "2x"), ("max", "4x")):
        d = tmp / ("dd-" + old); d.mkdir(); run(d)
        (d / "graphics.cfg").write_text(re.sub(r"(?m)^draw_distance=.*$", "draw_distance=" + old, (d / "graphics.cfg").read_text()))
        rc, out = run(d)
        check(value(d, "draw_distance") == new and (" draw_distance=%s " % new) in out, "draw_distance=%s in a file: read as %s, rewritten as %s" % (old, new, new))
    d = tmp / "dd-flag"; d.mkdir()
    rc, out = run(d, "--draw-distance", "max")
    check(value(d, "draw_distance") == "4x", "--draw-distance max: accepted, saved as 4x")
    rc, out = run(d, "--draw-distance", "2x")
    check(value(d, "draw_distance") == "2x", "--draw-distance 2x: saved")
    (d / "graphics.cfg").write_text(re.sub(r"(?m)^draw_distance=.*$", "draw_distance=8x", (d / "graphics.cfg").read_text()))
    rc, out = run(d)
    check("ignoring draw_distance=8x" in out, "draw_distance=8x: ignored with a message")

print("\n%s" % ("ALL PASS" if not fails else "%d FAILED" % len(fails)))
sys.exit(1 if fails else 0)
