#!/usr/bin/env python3
"""controls.cfg migration + presets (Q22). Stdlib only, no ROM, no window: the host exits at
--check-rom right after it loaded the bindings. Usage: controls_cfg_test.py [path/to/<game>_recomp]   ($THPS_HOST names the game: run it through the game's tests/ wrapper)

Checks, each in a fresh temp config dir:
  1. the pre-Q19 generated N64 layout (any field order, lines missing) is moved to thps-xbox, the old
     file is kept as controls.cfg.pre-xbox.bak, one log line, and it is not migrated again;
  2. a hand-edited file, and a file saying "preset = n64", are left alone;
  3. --controls-preset n64 / thps-xbox rewrite the file (old one kept as controls.cfg.bak) and stick;
  4. a bad preset name exits 2.
"""
import pathlib, subprocess, sys, tempfile

import os, tomllib
if not os.environ.get("THPS_HOST"):
    raise SystemExit("THPS_HOST is not set: run this through decomp/thps<N>/recomp-host/tests/controls_cfg_test.py")
HERE = pathlib.Path(os.environ["THPS_HOST"]).resolve()    # the game's recomp-host directory
with open(HERE / "game" / "game.toml", "rb") as _f:
    _GAME = tomllib.load(_f)
_EXE = f"{_GAME['family']}_recomp.exe" if sys.platform == "win32" else f"{_GAME['family']}_recomp"
EXE = sys.argv[1] if len(sys.argv) > 1 else str(HERE / "build" / "cmake" / _EXE)
# --check-rom wants a file: an empty temp file (no /dev/null on Windows; NUL is not a file std::filesystem can open).
_EMPTY = tempfile.NamedTemporaryFile(prefix="empty-rom-", delete=False)
_EMPTY.close()

LEGACY = f"""# {_GAME['short_name']} recomp controls: INPUT = FIELD, FIELD ... (keyboard fields count on port 1 only)
a = key:Space, button:a
b = key:D, button:x
z = key:S, axis:lefttrigger+
l = key:Q, button:leftshoulder
r = key:E, axis:righttrigger+
start = key:Return, key:Keypad Enter, button:start
c_up = key:I, axis:righty-, button:rightstick
c_down = key:K, axis:righty+, button:rightshoulder
c_left = key:J, axis:rightx-, button:y
c_right = key:L, axis:rightx+, button:b
dpad_up = button:dpup
dpad_down = button:dpdown
dpad_left = button:dpleft
dpad_right = button:dpright
stick_left = key:Left, axis:leftx-
stick_right = key:Right, axis:leftx+
stick_up = key:Up, axis:lefty-
stick_down = key:Down, axis:lefty+
"""


def run(cfg, *flags):
    p = subprocess.run([EXE, "--config", str(cfg), "--check-rom", _EMPTY.name, *flags], capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout + p.stderr


def field(cfg, name):
    for line in (cfg / "controls.cfg").read_text().splitlines():
        if line.startswith(name + " ="):
            return sorted(x.strip() for x in line.split("=", 1)[1].split(","))
    return None


if "frontend=1" in subprocess.run([EXE, "--features"], capture_output=True, text=True, timeout=30).stdout:
    print("skipped: the launcher build keeps its bindings in controls.json (checked by hand, LOG Q22)")
    sys.exit(0)

fails = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails.append(what)


with tempfile.TemporaryDirectory(prefix="thps1-ctl-") as tmp:
    tmp = pathlib.Path(tmp)

    d = tmp / "legacy"; d.mkdir(); (d / "controls.cfg").write_text(LEGACY)
    rc, out = run(d)
    check(out.count("moved to preset thps-xbox") == 1, "legacy file: one migration log line")
    check((d / "controls.cfg.pre-xbox.bak").read_text() == LEGACY, "legacy file: backed up byte for byte")
    check(field(d, "c_left") == sorted(["button:x", "key:J", "axis:rightx-"]), "legacy file: X is C-left now")
    check(field(d, "c_down") == sorted(["key:K", "key:Space", "button:a", "axis:righty+"]), "legacy file: A is C-down (ollie) now")
    rc, out = run(d)
    check("moved to preset" not in out, "legacy file: not migrated twice")

    # reordered fields, comments dropped, some lines missing: still the generated layout
    d = tmp / "shuffled"; d.mkdir()
    lines = []
    for l in LEGACY.splitlines():
        if l.startswith("#") or l.startswith(("dpad_", "stick_up", "start")):
            continue
        k, v = l.split(" = ")
        lines.append(k + " = " + ", ".join(reversed(v.split(", "))))
    (d / "controls.cfg").write_text("\n".join(lines) + "\n")
    rc, out = run(d)
    check("moved to preset thps-xbox" in out and (d / "controls.cfg.pre-xbox.bak").exists(), "shuffled + partial legacy file: migrated")

    # hand edit: left alone
    d = tmp / "custom"; d.mkdir()
    edited = LEGACY.replace("c_up = key:I,", "c_up = key:U, key:I,")
    (d / "controls.cfg").write_text(edited)
    rc, out = run(d)
    check("moved to preset" not in out and (d / "controls.cfg").read_text() == edited and not (d / "controls.cfg.pre-xbox.bak").exists(),
          "hand-edited file: untouched, no backup")

    # presets
    d = tmp / "preset"; d.mkdir()
    rc, out = run(d)
    check("wrote default bindings (preset thps-xbox)" in out, "fresh dir: thps-xbox written")
    rc, out = run(d, "--controls-preset", "n64")
    check("ok preset n64" in out and field(d, "b") == sorted(["key:D", "button:x"]), "--controls-preset n64: N64 layout (B = Xbox X)")
    check((d / "controls.cfg.bak").exists(), "--controls-preset n64: previous file kept as controls.cfg.bak")
    rc, out = run(d)
    check("moved to preset" not in out and field(d, "b") == sorted(["key:D", "button:x"]), "n64 preset is not migrated back")
    rc, out = run(d, "--controls-preset", "thps-xbox")
    check("ok preset thps-xbox" in out and field(d, "b") == sorted(["key:D", "button:b"]) and field(d, "c_left") == sorted(["key:J", "button:x", "axis:rightx-"]),
          "--controls-preset thps-xbox: back to the default")
    rc, out = run(d, "--controls-preset", "nonsense")
    check(rc == 2 and "thps-xbox | n64" in out, "bad preset name: exit 2")

print("\n%s" % ("ALL PASS" if not fails else "%d FAILED" % len(fails)))
sys.exit(1 if fails else 0)
