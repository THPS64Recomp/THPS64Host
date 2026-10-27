#!/usr/bin/env python3
"""First-run flow tests (Q26): ROM picker/errors, build progress, missing compiler, cancel, damaged library, second instance,
read-only folder, full disk, app update, the graphics defaults in force on a fresh config. Shared by THPS1-3 (ADR 0006): the game comes from $THPS_HOST (game/game.toml, decomp/common/games.toml).
Stdlib only. Usage:

    python3 -I tests/firstrun_test.py [--binary PATH] [--only NAME ...] [--shots DIR] [--list]

Needs the SPLIT-build engine (THPS_SPLIT_GAME_LIB=ON: build/cmake-split/<game>_recomp; tools/test.sh --first-run picks it when it exists) with a
resources folder next to it (the packaged .app, or build/cmake-split/resources) and your own ROMs: $THPS<N>_ROM (the game's first release), else roms/ in a
parent directory. The game's other releases are used when present ($THPS<N>_ROM_<SUFFIX>, e.g. THPS1_ROM_US10 / THPS1_ROM_EU / THPS2_ROM_EU); a scenario
that needs a ROM you do not have is skipped. A release the app has a recipe for (resources/classes/<class>/blob.txt) is built and played
(`other_release_<suffix>`); one it has none for must get the "no recipe" page.

Games whose front end is overlays (THPS2/3: game.toml `overlays`) also run `overlay_table` (the built library holds code for all 16 screens),
`play_menus` (boot to the menus with the built library, via tests/run.py) and `overlay_refusal` (a recipe whose ROM fingerprint does not match
the ROM is refused with a normal error page); the modified-ROM case in `rom_errors` flips a byte inside the overlays.

The window is driven by THPS_UI_ACTIONS (src/firstrun_ui.h): the scenarios click buttons and take screenshots (BMP -> PNG, only our own UI).
The real Command Line Tools installer and the file manager are never started: with THPS_UI_ACTIONS set those actions are mocked and logged.
Nothing ROM-derived is written outside temp folders that are deleted at the end (a modified copy of the ROM is made for one scenario).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import tomllib
import zlib
from pathlib import Path

if not os.environ.get("THPS_HOST"):
    raise SystemExit("THPS_HOST is not set: run this through decomp/thps<N>/recomp-host/tests/firstrun_test.py (or tools/test.sh --first-run)")
HERE = Path(os.environ["THPS_HOST"]).resolve()   # the game's recomp-host directory
with open(HERE / "game" / "game.toml", "rb") as _f:
    PROFILE = tomllib.load(_f)
FAMILY = PROFILE["family"]                       # thps1 / thps2 / thps3
OVERLAYS = bool(PROFILE.get("overlays"))         # THPS2/3: the front end is overlays, built from the ROM at first run (decomp/common/firstrun)
IS_WINDOWS = os.name == "nt"
EXE_NAME = f"{FAMILY}_recomp.exe" if IS_WINDOWS else f"{FAMILY}_recomp"
LIB_EXT = "dll" if IS_WINDOWS else "dylib" if sys.platform == "darwin" else "so"
LIB_GLOB = "*." + LIB_EXT                         # the game library built at first run
LIB_PREFIX = f"{FAMILY}_game_"                    # <family>_game_<code class>.<ext>


def find_rom(names: list[str]) -> Path | None:
    for parent in [HERE] + list(HERE.parents):
        for n in names:
            for d in (parent / "roms", parent):
                if (d / n).is_file():
                    return d / n
    return None


def load_rows() -> dict[str, dict]:
    """decomp/common/games.toml rows (the `inherits` chain merged), by row id."""
    games = Path(__file__).resolve().parents[2] / "games.toml"     # the host tree's own table (decomp/common, or host/ in a game repository)
    if not games.is_file():
        games = None
    for parent in ([] if games else [HERE] + list(HERE.parents)):
        if (parent / "decomp" / "common" / "games.toml").is_file():
            games = parent / "decomp" / "common" / "games.toml"
            break
        if (parent / "common" / "games.toml").is_file():
            games = parent / "common" / "games.toml"
            break
    if games is None:
        raise SystemExit("decomp/common/games.toml not found above " + str(HERE))
    raw = tomllib.loads(games.read_text(encoding="utf-8"))
    rows: dict[str, dict] = {}

    def merged(k: str) -> dict:
        r = raw[k]
        return {**(merged(r["inherits"]) if r.get("inherits") else {}), **{x: y for x, y in r.items() if x != "inherits"}}

    for k in raw:
        if isinstance(raw[k], dict) and ("rom_sha1" in raw[k] or "inherits" in raw[k]):
            rows[k] = merged(k)
    return rows


ROWS = load_rows()


class Release:
    """One ROM version of this game (a games.toml row): the id, its code class, the ROM file the tests use (None: not available)."""

    def __init__(self, vid: str):
        r = ROWS[vid]
        self.id = vid
        self.cls = r["code_class"]
        self.display = r["display_name"]
        self.sha1 = r["rom_sha1"]
        self.suffix = "" if vid == FAMILY else vid[len(FAMILY) + 1:]            # "", "eu", "us10"
        env = os.environ.get(FAMILY.upper() + "_ROM" + ("_" + self.suffix.upper() if self.suffix else ""))
        self.rom: Path | None = Path(env) if env else find_rom(list(r.get("rom_names", [])))


RELEASES = [Release(k) for k in ROWS if k == FAMILY or k.startswith(FAMILY + "_")]
MAIN = RELEASES[0]                       # the game's first release: the one most scenarios build
assert MAIN.id == FAMILY
ROM_MAIN = MAIN.rom
OTHERS = [r for r in RELEASES if r is not MAIN]


def other_game_rom() -> tuple[Path | None, str]:
    """A ROM of another game of the family set (for "this is not <game>"): its path and the header name the app should quote."""
    for fam in ("thps1", "thps2", "thps3"):
        if fam == FAMILY or fam not in ROWS:
            continue
        r = ROWS[fam]
        p = Path(os.environ[fam.upper() + "_ROM"]) if os.environ.get(fam.upper() + "_ROM") else find_rom(list(r.get("rom_names", [])))
        if p:
            return p, r["header_name"]
    return None, ""


ROM_OTHER, OTHER_HEADER = other_game_rom()


class Skip(Exception):
    pass


def resources_dir(binary: Path) -> Path | None:
    """The app's resources folder: beside a plain executable, Contents/Resources in an .app."""
    for res in (binary.parent / "resources", binary.parent.parent / "Resources"):
        if res.is_dir():
            return res
    return None


def recipe_versions(binary: Path) -> set[str]:
    """Version ids the app has a recipe for: the `xxh3.<id>` lines of its resources/classes/*/blob.txt."""
    res = resources_dir(binary)
    if res:
        return {m.group(1) for f in res.glob("classes/*/blob.txt") for m in re.finditer(r"^xxh3\.(\S+) =", f.read_text(), re.M)}
    return set()


def png_from_bmp(bmp: Path, png: Path):
    """SDL_SaveBMP output (32 or 24 bit, bottom-up) -> PNG (stdlib only)."""
    d = bmp.read_bytes()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    flip = h > 0
    h = abs(h)
    bytespp = bpp // 8
    stride = (w * bytespp + 3) & ~3
    rows = []
    for y in range(h):
        sy = h - 1 - y if flip else y
        row = d[off + sy * stride: off + sy * stride + w * bytespp]
        out = bytearray(b"\x00")
        for x in range(w):
            b, g, r = row[x * bytespp], row[x * bytespp + 1], row[x * bytespp + 2]
            out += bytes((r, g, b))
        rows.append(bytes(out))
    raw = b"".join(rows)

    def chunk(t: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + t + body + struct.pack(">I", zlib.crc32(t + body) & 0xFFFFFFFF)

    png.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


class Ctx:
    def __init__(self, binary: Path, shots: Path | None):
        self.binary = binary
        self.shots = shots
        self.tmp = Path(tempfile.mkdtemp(prefix="thps-firstrun-"))
        self.base_cfg: Path | None = None   # a config with a built library (made once, copied per scenario)
        self.n = 0

    def cfg(self, name: str, with_library: bool = False) -> Path:
        d = self.tmp / f"cfg-{name}"
        if d.exists():
            shutil.rmtree(d, ignore_errors=True)
        if with_library:
            shutil.copytree(self.library_config(), d)
            (d / f"{FAMILY}.lock").unlink(missing_ok=True)
        return d

    def library_config(self) -> Path:
        """A config folder holding a stored v1.1 ROM and a built library (one build for the whole run)."""
        if self.base_cfg is None:
            d = self.tmp / "cfg-base"
            rc, out = self.run(["--rom", str(ROM_MAIN), "--config", str(d), "--build-game"], ui=False)
            assert rc == 0 and "game library ready" in out, f"base build failed ({rc}):\n{out}"
            self.base_cfg = d
        return self.base_cfg

    def run(self, args: list[str], actions: str | None = None, env: dict | None = None, ui: bool = True, timeout: float = 240, shots: str | None = None,
            game_seconds: int = 0) -> tuple[int, str]:
        e = dict(os.environ)
        for k in ("THPS_GAME_LIB", "THPS_UI", "THPS_UI_ACTIONS", "THPS_MENU_HINT", "THPS_MENU_DEBUG", "THPS_FAKE_NO_COMPILER", "THPS_FAKE_FREE_MB", "THPS_BUILD_JOBS", "THPS_LIVE"):
            e.pop(k, None)
        if actions:
            e["THPS_UI_ACTIONS"] = actions
        shot_dir = self.tmp / "shots"
        shot_dir.mkdir(exist_ok=True)
        e["THPS_UI_SHOT_DIR"] = str(shot_dir)
        e.update(env or {})
        cmd = [str(self.binary)] + args
        if ui:
            cmd += ["--hidden", "--null-audio", "--mute", "--no-pads", "--no-keyboard", "--seconds", str(game_seconds or 3)]
        p = subprocess.run(cmd, env=e, cwd=HERE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", timeout=timeout)
        out = p.stdout
        self.keep_shots(shot_dir, shots)
        return p.returncode, out

    def copy_app(self, name: str) -> Path:
        """A copy of the app to modify: the engine, its resources and (Windows) the DLLs it loads from its own folder. Returns the exe."""
        src_res = resources_dir(self.binary)
        assert src_res, f"no resources folder beside {self.binary}"
        app = self.tmp / name
        app.mkdir()
        shutil.copy2(self.binary, app / EXE_NAME)
        # A packaged macOS app finds its SDL2 at @executable_path/../Frameworks: that is <tmp>/Frameworks for a copy in <tmp>/<name> (shared by the copies).
        frameworks = self.binary.parent.parent / "Frameworks"
        if sys.platform == "darwin" and frameworks.is_dir() and not (self.tmp / "Frameworks").exists():
            shutil.copytree(frameworks, self.tmp / "Frameworks", symlinks=True)
        if IS_WINDOWS:
            for dll in self.binary.parent.glob("*.dll"):
                shutil.copy2(dll, app / dll.name)
        shutil.copytree(src_res, app / "resources", symlinks=False)
        return app / EXE_NAME

    def keep_shots(self, shot_dir: Path, prefix: str | None):
        for bmp in sorted(shot_dir.glob("*.bmp")):
            if self.shots and prefix:
                self.shots.mkdir(parents=True, exist_ok=True)
                png_from_bmp(bmp, self.shots / f"{prefix}-{bmp.stem}.png")
            bmp.unlink()

    def close(self):
        for p in self.tmp.rglob("*"):
            try:
                if p.is_dir() and not p.is_symlink():
                    p.chmod(0o755)
            except OSError:
                pass
        shutil.rmtree(self.tmp, ignore_errors=True)


def pages(out: str) -> list[str]:
    return re.findall(r"^\[ui\] page (\S+):", out, re.M)


def check(cond: bool, msg: str):
    if not cond:
        raise AssertionError(msg)


# ------------------------------------------------------------------------------------------------ scenarios
def big_endian_copy(path: Path) -> bytearray:
    """The ROM image in big-endian (z64) order: .n64 is little-endian words, .v64 byte-swapped pairs."""
    data = bytearray(path.read_bytes())
    if data[:4] == bytes([0x40, 0x12, 0x37, 0x80]):
        for i in range(0, len(data) - 3, 4):
            data[i:i + 4] = data[i:i + 4][::-1]
    elif data[:4] == bytes([0x37, 0x80, 0x40, 0x12]):
        for i in range(0, len(data) - 1, 2):
            data[i:i + 2] = data[i:i + 2][::-1]
    return data


def recipe_file(binary: Path, cls: str, name: str) -> Path | None:
    res = resources_dir(binary)
    p = res / "classes" / cls / name if res else None
    return p if p and p.is_file() else None


def rom_error_scenarios(c: Ctx):
    """Each ROM problem has its own page and message; Quit ends the run with exit code 10."""
    cases = []
    if ROM_OTHER:
        cases.append((f"rom_not_{FAMILY}", ROM_OTHER, [f"not {PROFILE['title']}", OTHER_HEADER, f"SHA-1 {MAIN.sha1[:8]}"]))
    have = recipe_versions(c.binary)
    for r in OTHERS:
        if r.rom and r.id not in have:
            cases.append(("rom_no_recipe", r.rom, [r.display[r.display.rindex("(") + 1:-1], "no recipe"]))
    txt = c.tmp / "notes.txt"
    txt.write_text("this is not a ROM\n")
    cases.append(("rom_not_a_rom", txt, ["not an N64 ROM"]))
    cases.append(("rom_read_error", c.tmp / "does-not-exist.z64", ["could not be read"]))
    cases.append(("rom_read_error", c.tmp, ["could not be read"]))   # a folder
    if ROM_MAIN:
        bad = c.tmp / "modified.z64"
        data = big_endian_copy(ROM_MAIN)
        data[0x2000] ^= 0xFF
        bad.write_bytes(data)
        cases.append(("rom_wrong_version", bad, ["not a dump this app knows", "SHA-1", "modified"]))
        if OVERLAYS:
            # A byte inside a front-end screen (the ROM tail the overlay recipe covers): the app must not take it for the known dump either.
            rec = recipe_file(c.binary, MAIN.cls, "overlays.txt")
            first = re.search(r"^o \S+ (0x[0-9A-Fa-f]+) ", rec.read_text(), re.M) if rec else None
            if first:
                bad = c.tmp / "modified-overlay.z64"
                data = big_endian_copy(ROM_MAIN)
                data[int(first.group(1), 16) + 0x10] ^= 0xFF
                bad.write_bytes(data)
                cases.append(("rom_wrong_version", bad, ["not a dump this app knows", "SHA-1", "modified"]))
    for i, (page_id, rom, needles) in enumerate(cases):
        name = f"{page_id}-{i}"
        cfg = c.cfg(name)
        # window flow: the error page, then Quit
        rc, out = c.run(["--rom", str(rom), "--config", str(cfg)], actions=f"waitpage={page_id}|shot={page_id}|click=quit", shots=f"{i:02d}-{page_id}")
        check(page_id in pages(out), f"{name}: expected page {page_id}, got {pages(out)}\n{out}")
        check(rc == 10, f"{name}: exit code {rc}, expected 10 (user quit)\n{out}")
        for n in needles:
            check(n in out, f"{name}: message lacks {n!r}\n{out}")
        check(not (cfg / "game").exists() or not any((cfg / "game").glob(LIB_GLOB)), f"{name}: a game library was built for a refused ROM")
        # console flow: the same message on stderr and exit code 1
        rc, out = c.run(["--rom", str(rom), "--config", str(cfg), "--no-ui"], ui=False)
        check(rc == 1, f"{name}: console exit code {rc}, expected 1\n{out}")
        for n in needles:
            check(n in out, f"{name}: console message lacks {n!r}\n{out}")
    return f"{len(cases)} ROM problems, window and console"


def picker_bypass(c: Ctx):
    """With --rom (or a stored ROM) and a current library no window opens at all, and the game starts."""
    cfg = c.cfg("bypass", with_library=True)
    gs = 8 if IS_WINDOWS else 0   # a launch that presents nothing before --seconds exits 1; a slower Windows PC can take past 3 s (as in app_update)
    rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], shots="bypass", game_seconds=gs)
    check(rc == 0, f"exit {rc}\n{out}")
    check(not pages(out), f"a first-run page was shown: {pages(out)}\n{out}")
    check("[thps] quit" in out, out)
    # no --rom: the stored copy is used
    rc, out = c.run(["--config", str(cfg)], game_seconds=gs)
    check(rc == 0 and not pages(out), f"stored-ROM start: exit {rc}, pages {pages(out)}\n{out}")
    return "no window with --rom or a stored ROM"


def welcome_pick_and_drop(c: Ctx):
    """No ROM: the welcome page; a scripted pick (the OS dialog is never opened in tests) and a dropped file both lead to the build."""
    cfg = c.cfg("pick")
    acts = f"waitpage=welcome|shot=welcome|pick={ROM_MAIN}|click=choose|waitpage=building|wait=0.5|shot=building|wait=60"
    rc, out = c.run(["--config", str(cfg)], actions=acts, env={"THPS_BUILD_JOBS": "2"}, shots="pick")
    check(rc == 0, f"exit {rc}\n{out}")
    check(pages(out)[:2] == ["welcome", "building"], f"pages {pages(out)}\n{out}")
    check("[first-run] done in" in out and len(list((cfg / "game").glob(LIB_PREFIX + "*"))) == 2, out)
    # a wrong file first, then a drop of the right one
    cfg = c.cfg("drop")
    acts = (f"waitpage=welcome|drop={ROM_OTHER or c.tmp / 'x'}|waitpage={'rom_not_' + FAMILY if ROM_OTHER else 'rom_read_error'}|shot=after-wrong-file|"
            f"drop={ROM_MAIN}|waitpage=building|wait=60")
    rc, out = c.run(["--config", str(cfg)], actions=acts, shots="drop")
    check(rc == 0, f"exit {rc}\n{out}")
    p = pages(out)
    check(p[0] == "welcome" and p[-1] == "building" and len(p) == 3, f"pages {p}\n{out}")
    return "welcome -> pick -> build; welcome -> wrong drop -> right drop -> build"


def no_compiler(c: Ctx):
    """THPS_FAKE_NO_COMPILER: the no-compiler page. macOS: Install (Command Line Tools) is mocked, never run. Windows: the compiler
    is bundled, so the page says the app is incomplete and offers only Retry/Quit. Retry looks again."""
    mac = sys.platform == "darwin"
    cfg = c.cfg("nocc")
    install = "click=install|wait=0.6|shot=no_compiler-after-install|" if mac else ""
    acts = f"waitpage=no_compiler|shot=no_compiler|{install}click=retry|waitpage=no_compiler|click=quit"
    rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], actions=acts, env={"THPS_FAKE_NO_COMPILER": "1"}, shots="nocompiler")
    check(rc == 10, f"exit {rc}\n{out}")
    p = pages(out)
    check(p == ["building", "no_compiler", "building", "no_compiler"], f"pages {p}\n{out}")
    check(out.count("[ui] mock: would run: xcode-select --install") == (1 if mac else 0), "Install mocked not exactly once (macOS) / offered (elsewhere)\n" + out)
    check(not list((cfg / "game").glob(LIB_GLOB)), "a library exists although there was no compiler")
    if mac:
        check("Command Line Tools" in out, out)
    if IS_WINDOWS:
        check("compiler is missing" in out and "toolchain" in out, out)
    # console flow: exit 4 with the message
    rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg), "--no-ui"], ui=False, env={"THPS_FAKE_NO_COMPILER": "1"})
    check(rc == 4, f"console exit {rc}, expected 4\n{out}")
    check(("xcode-select --install" if mac else "compiler") in out, f"console message\n{out}")
    return "no_compiler page" + (", Install mocked" if mac else " (bundled compiler, no Install)") + ", Retry, console message"


def cancel_mid_build(c: Ctx):
    """Cancel while compiling: the compiler is stopped, nothing half-written is left, the next start builds the library."""
    cfg = c.cfg("cancel")
    acts = "waitpage=building|wait=3|shot=cancelling|click=cancel"
    rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], actions=acts, env={"THPS_BUILD_JOBS": "1"}, shots="cancel")
    check(rc == 10, f"exit {rc}\n{out}")
    check("compile generated C" in out and "[first-run] done in" not in out, out)
    game = cfg / "game"
    left = [p.name for p in game.glob("*")] if game.exists() else []
    check(not left, f"left behind after cancel: {left}")
    time.sleep(0.3)
    leftover = subprocess.run(["pgrep", "-f", str(cfg)], stdout=subprocess.PIPE, text=True).stdout.split() if not IS_WINDOWS else []
    check(not leftover, f"processes still running for the cancelled build: {leftover}")
    log = (cfg / "first-run.log").read_text()
    check("cancelled by the user" in log, log)
    # next start rebuilds and plays
    rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)])
    check(rc == 0 and "[first-run] done in" in out, f"rebuild after cancel: exit {rc}\n{out}")
    return "cancel leaves nothing; the next start rebuilds"


def damaged_library(c: Ctx):
    """A library that is cut short, flipped, stampless, or built for another ABI/recipe is rebuilt, a good one is not."""
    ok = 0
    for what in ("truncate", "flip", "no_stamp", "other_inputs", "old_abi", "missing"):
        cfg = c.cfg("damaged-" + what, with_library=True)
        lib = next((cfg / "game").glob(LIB_PREFIX + "*." + LIB_EXT))
        stamp = Path(str(lib) + ".stamp")
        if what == "truncate":
            lib.write_bytes(lib.read_bytes()[:100_000])
        elif what == "flip":
            b = bytearray(lib.read_bytes())
            b[len(b) // 2] ^= 0x55
            lib.write_bytes(b)
        elif what == "no_stamp":
            stamp.unlink()
        elif what == "other_inputs":
            stamp.write_text(re.sub(r"res=[0-9a-f]+", "res=0000000000000000", stamp.read_text()))
        elif what == "old_abi":
            stamp.write_text(re.sub(r"abi=\d+", "abi=0", stamp.read_text()))
        elif what == "missing":
            lib.unlink()
        rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], actions="waitpage=building|wait=60", shots=None)
        check(rc == 0 and "[first-run] done in" in out and pages(out) == ["building"], f"{what}: not rebuilt (exit {rc}, pages {pages(out)})\n{out}")
        why = {"truncate": "manifest", "flip": "manifest", "no_stamp": "no manifest", "other_inputs": "other inputs", "old_abi": "other inputs", "missing": "no game library"}[what]
        check(why in out, f"{what}: expected reason containing {why!r}\n{out}")
        ok += 1
    # an undamaged library is loaded without any window or build
    cfg = c.cfg("damaged-none", with_library=True)
    rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)])
    check(rc == 0 and "[first-run]" not in out and not pages(out), f"undamaged library was rebuilt\n{out}")
    return f"{ok} kinds of damage rebuilt, undamaged library untouched"


def app_update(c: Ctx):
    """An app update that changes the recipe/tools/SDK rebuilds the library; a changed NOTICE does not."""
    # Windows: a freshly written exe is slow to its first frame on its first launches (measured 2026-10-14: 1.8 s, then 4.5 s, until a run
    # lasts past ~5 s), and a launch that presents nothing before --seconds exits 1. Wait long enough for the first frame there.
    gs = 8 if IS_WINDOWS else 0
    exe = c.copy_app("app")
    app = exe.parent
    cfg = c.cfg("update", with_library=True)
    old = c.binary
    c.binary = exe
    try:
        rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], game_seconds=gs)
        check(rc == 0 and "[first-run]" not in out, f"same app, library current expected\n{out}")
        notice = app / "resources" / "NOTICE"
        notice.write_text(notice.read_text() + "\nunrelated change\n")
        rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], game_seconds=gs)
        check(rc == 0 and "[first-run]" not in out, f"a NOTICE change must not rebuild\n{out}")
        for sub in ("classes", "sdk", "tools"):    # Finder drops a .DS_Store into any folder someone browses (Show Package Contents)
            (app / "resources" / sub / ".DS_Store").write_bytes(b"finder litter")
        rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], game_seconds=gs)
        check(rc == 0 and "[first-run]" not in out, f"a .DS_Store in the resources must not rebuild\n{out}")
        sdk_file = next((app / "resources" / "sdk" / "game").glob("*.cpp"))
        sdk_file.write_text(sdk_file.read_text() + "\n// recipe change\n")
        rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], actions="waitpage=building|wait=60", game_seconds=gs)
        check(rc == 0 and "[first-run] done in" in out and "other inputs" in out, f"SDK change did not rebuild\n{out}")
    finally:
        c.binary = old
    return "recipe/SDK change rebuilds; NOTICE change and a stray .DS_Store do not"


def second_instance(c: Ctx):
    """One instance per config folder: the second says so (window or console) and exits; the first is not disturbed."""
    cfg = c.cfg("lock", with_library=True)
    first = subprocess.Popen([str(c.binary), "--rom", str(ROM_MAIN), "--config", str(cfg), "--hidden", "--null-audio", "--mute", "--no-pads", "--no-keyboard", "--seconds", "14"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=HERE)
    try:
        time.sleep(4)
        check(first.poll() is None, "the first instance exited early")
        rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], actions="waitpage=already_running|shot=already_running|click=quit", shots="lock")
        check(rc == 6 and "already_running" in pages(out), f"window flow: exit {rc}, pages {pages(out)}\n{out}")
        check("already open" in out, out)
        rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg), "--no-ui"], ui=False)
        check(rc == 6 and "Already running" in out, f"console flow: exit {rc}\n{out}")
        out1, _ = first.communicate(timeout=60)
        check(first.returncode == 0, f"the first instance ended with {first.returncode}\n{out1}")
    finally:
        if first.poll() is None:
            first.kill()
    # the lock is released when the first one is gone
    rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)])
    check(rc == 0, f"start after the first instance ended: exit {rc}\n{out}")
    return "second instance refused (window and console), lock released at exit"


def read_only_folder(c: Ctx):
    if IS_WINDOWS or os.geteuid() == 0:
        return "skipped (needs a non-root POSIX user)"
    cfg = c.tmp / "cfg-readonly"
    cfg.mkdir()
    cfg.chmod(stat.S_IRUSR | stat.S_IXUSR)
    try:
        rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], actions="waitpage=folder_not_writable|shot=folder_not_writable|click=quit", shots="readonly")
    finally:
        cfg.chmod(0o755)
    check(rc == 7 and "folder_not_writable" in pages(out), f"exit {rc}, pages {pages(out)}\n{out}")
    check("cannot be written" in out or "cannot open" in out, out)
    # a folder that cannot even be created
    blocker = c.tmp / "blocker"
    blocker.write_text("a file where a folder should be")
    rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(blocker / "sub")], actions="waitpage=fatal|shot=folder_cannot_be_created|click=quit", shots="readonly")
    check(rc == 7 and "cannot be created" in out, f"exit {rc}\n{out}")
    return "read-only folder and uncreatable folder: clear page, exit 7"


def disk_full(c: Ctx):
    """THPS_FAKE_FREE_MB models a full disk (a real one cannot be made safely): the build refuses with a clear page; Retry looks again."""
    cfg = c.cfg("full")
    acts = "waitpage=disk_full|shot=disk_full|click=retry|waitpage=disk_full|click=quit"
    rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], actions=acts, env={"THPS_FAKE_FREE_MB": "20"}, shots="diskfull")
    check(rc == 10 and pages(out).count("disk_full") == 2, f"exit {rc}, pages {pages(out)}\n{out}")
    check("Not enough free disk space" in out, out)
    check(not list((cfg / "game").glob(LIB_GLOB)), "a library was built on a full disk")
    # the ROM copy cannot be stored on a (nearly) full disk either
    return "build refused on a full disk, Retry re-checks"


def build_failure(c: Ctx):
    """A compiler that fails: the page names the step and shows the end of the log; the log folder button is mocked."""
    cfg = c.cfg("fail")
    # A resources copy whose glue source does not compile.
    exe = c.copy_app("app-broken")
    glue = exe.parent / "resources" / "sdk" / "game" / "game_rsp.cpp"
    glue.write_text(glue.read_text() + "\nthis is not valid C++;\n")
    old = c.binary
    c.binary = exe
    try:
        acts = "waitpage=build_failed|shot=build_failed|click=open_logs|wait=0.4|click=quit"
        rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], actions=acts, shots="failure")
    finally:
        c.binary = old
    check(rc == 10 and "build_failed" in pages(out), f"exit {rc}, pages {pages(out)}\n{out}")
    check("[ui] mock: would open the folder" in out, "Open log folder was not mocked\n" + out)
    check("cxx job" in out and "error" in out, out)
    check(not list((cfg / "game").glob(LIB_GLOB)), "a library exists after a failed build")
    check((cfg / "first-run.log").exists() and "FAILED" in (cfg / "first-run.log").read_text(), "first-run.log has no FAILED line")
    return "compile error page with log tail; open-folder mocked; no library left"


def other_release(r: "Release"):
    """A release of the game other than the first (its own recipe or a second fingerprint of the class's): the console build makes
    <family>_game_<class>, and that release then plays from the stored ROM (Europe runs at NTSC timing, ADR 0004)."""
    def scenario(c: Ctx):
        if r.id not in recipe_versions(c.binary):
            raise Skip(f"this app has no recipe for {r.display} (rom_errors checks its no-recipe page)")
        cfg = c.cfg("rel-" + r.suffix)
        rc, out = c.run(["--rom", str(r.rom), "--config", str(cfg), "--build-game"], ui=False)
        check(rc == 0 and "game library ready" in out, f"build: exit {rc}\n{out}")
        want = f"{LIB_PREFIX}{r.cls}.{LIB_EXT}"
        check([p.name for p in (cfg / "game").glob(LIB_GLOB)] == [want], f"libraries: {list((cfg / 'game').iterdir())}")
        rc, out = c.run(["--config", str(cfg), "--verbose"], game_seconds=16 if OVERLAYS else 5)   # the first front-end screen (an overlay) loads at ~10 s
        check(rc == 0 and "[thps] quit" in out and not pages(out), f"play: exit {rc}, pages {pages(out)}\n{out}")
        check(f"({r.id}), code class {r.cls}, osTvType 1" in out and "Failed to find function" not in out, out)
        if OVERLAYS:
            check("overlay " in out, "no front-end overlay was loaded while playing the built library\n" + out)
        return f"built {want} from the {r.display} ROM, then played it from the stored ROM (osTvType NTSC)"
    return scenario


def first_run_log(c: Ctx):
    cfg = c.cfg("log")
    rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg), "--build-game"], ui=False)
    check(rc == 0, out)
    log = (cfg / "first-run.log").read_text()
    for needle in ("run: ", "clang", "N64Recomp", "RSPRecomp", "-> exit 0 in", "link library: ", "done in"):
        check(needle in log, f"first-run.log lacks {needle!r}:\n{log[:3000]}")
    return f"first-run.log: {len(log.splitlines())} lines with commands and times"


# Overlay games (THPS2/3): the screens are built from the ROM at first run (decomp/common/firstrun). The screen table is read back from
# the running game with the control protocol (`overlays`, src/control.cpp) and compared with the recipe.
class Control:
    """The engine's --control line protocol (src/control.cpp) over stdin/stdout, enough for a status query."""

    def __init__(self, binary: Path, args: list[str], env: dict):
        self.proc = subprocess.Popen([str(binary), "--control", "--hidden", "--null-audio", "--mute", "--no-pads", "--no-keyboard"] + args, env=env, cwd=HERE,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        self.lines: list[str] = []
        self.cv = threading.Condition()
        threading.Thread(target=self._read, daemon=True).start()
        self.expect("ok ready", 60)

    def _read(self):
        for line in self.proc.stdout:
            with self.cv:
                self.lines.append(line.rstrip("\r\n"))
                self.cv.notify_all()
        with self.cv:
            self.lines.append("__eof__")
            self.cv.notify_all()

    def expect(self, prefix: str, timeout: float) -> str:
        end = time.monotonic() + timeout
        seen = 0
        with self.cv:
            while True:
                for i in range(seen, len(self.lines)):
                    if self.lines[i].startswith("[ctl] " + prefix):
                        return self.lines[i][6:]
                    if self.lines[i] == "__eof__":
                        raise AssertionError(f"engine ended while waiting for {prefix!r}:\n" + "\n".join(self.lines[-20:]))
                seen = len(self.lines)
                left = end - time.monotonic()
                if left <= 0:
                    raise AssertionError(f"timeout waiting for {prefix!r}:\n" + "\n".join(self.lines[-20:]))
                self.cv.wait(left)

    def ask(self, cmd: str, timeout: float = 60) -> str:
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()
        return self.expect("ok " + cmd.split()[0], timeout)

    def close(self):
        try:
            self.proc.stdin.write("quit\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=30)
        except Exception:   # noqa: BLE001
            self.proc.kill()


def overlay_recipe_counts(binary: Path, cls: str) -> tuple[int, int]:
    """(screens, functions) in the class's overlays.txt as shipped in the app."""
    rec = recipe_file(binary, cls, "overlays.txt")
    assert rec, f"the app has no overlays.txt for class {cls}"
    text = rec.read_text()
    return len(re.findall(r"^o ", text, re.M)), len(re.findall(r"^f ", text, re.M))


def overlay_table(c: Ctx):
    """The library built from the ROM holds code for every front-end screen: the table the game library carries (rows built from the ROM) is
    checked against the section table (recompiled screens), and both against the shipped recipe: all 16 screens, every function."""
    cfg = c.cfg("ovl-table", with_library=True)
    n, nfun = overlay_recipe_counts(c.binary, MAIN.cls)
    e = dict(os.environ)
    for k in ("THPS_GAME_LIB", "THPS_UI", "THPS_UI_ACTIONS"):
        e.pop(k, None)
    ctl = Control(c.binary, ["--rom", str(ROM_MAIN), "--config", str(cfg), "--verbose"], e)
    try:
        ctl.ask("wait 60")
        reply = ctl.ask("overlays")
    finally:
        ctl.close()
    m = re.search(r"table=(\d+) with_code=(\d+) funcs=(\d+) seen=(\d+)", reply)
    check(m is not None, f"unexpected reply {reply!r}")
    table, with_code, funcs, seen = (int(x) for x in m.groups())
    check(table == n and with_code == n, f"{table} screens in the library's table, {with_code} with code; the recipe has {n}: {reply}")
    check(funcs == nfun, f"{funcs} functions in the library's screens; the recipe lists {nfun}: {reply}")
    return f"{with_code}/{n} front-end screens have code in the built library ({funcs} functions, as the recipe lists); {seen} loaded during boot"


def play_menus(c: Ctx):
    """Boot the library that first run built and reach the menus (the suite's boot_title and main_menu scenarios, tests/run.py): the title
    screens and the main menu are overlays, drawn by code that was translated from this machine's ROM."""
    refs = HERE / "build" / "tests" / "refs"
    if not refs.is_dir():
        raise Skip(f"no reference frames in {refs} (tools/test.sh --record, or copy media/test-refs/{FAMILY}/refs*)")
    cfg = c.cfg("play-menus", with_library=True)
    lib = next((cfg / "game").glob(LIB_GLOB))
    e = dict(os.environ)
    e.update({"THPS_GAME_LIB": str(lib), f"{FAMILY.upper()}_BINARY": str(c.binary), "THPS_HOST": str(HERE)})
    out_dir = c.tmp / "menus-out"
    p = subprocess.run([sys.executable, "-I", str(HERE / "tests" / "run.py"), "--only", "boot_title,main_menu", "--out", str(out_dir), "--rom", str(ROM_MAIN)],
                       env=e, cwd=HERE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", timeout=900)
    tail = "\n".join(p.stdout.splitlines()[-25:])
    check(p.returncode == 0, f"run.py --only boot_title,main_menu: exit {p.returncode}\n{tail}")
    results = json.loads((out_dir / "results.json").read_text())["results"]
    got = {r["name"]: r["status"] for r in results}
    check(set(got) == {"boot_title", "main_menu"} and all(v in ("pass", "warn") for v in got.values()), f"scenarios: {got}\n{tail}")
    return "the built library boots to the title screens and the main menu: " + ", ".join(f"{n} {st}" for n, st in got.items())


def overlay_refusal(c: Ctx):
    """The ROM fingerprint of the front-end screens is checked before anything is translated. The recipe of a copy of the app is changed so
    that the fingerprint no longer matches the (unmodified) ROM, which is what a modified dump of a known version would look like: the build
    must stop with a normal error page, and no library is left."""
    exe = c.copy_app("app-overlay")
    rec = exe.parent / "resources" / "classes" / MAIN.cls / "overlays.txt"
    text = rec.read_text()
    new, n = re.subn(r"^(rom_xxh3 0x)([0-9a-f]{16})$", lambda m: m.group(1) + ("0" if m.group(2)[0] != "0" else "1") + m.group(2)[1:], text, flags=re.M)
    check(n == 1, "the recipe has no rom_xxh3 line")
    rec.write_text(new)
    cfg = c.cfg("ovl-refusal")
    old = c.binary
    c.binary = exe
    try:
        rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], actions="waitpage=build_failed|shot=build_failed|click=quit", shots="overlay-refusal")
        check(rc == 10 and "build_failed" in pages(out), f"exit {rc}, pages {pages(out)}\n{out}")
        check("front-end screens are not what this release was prepared for" in out and "modified dump" in out, out)
        check(not list((cfg / "game").glob(LIB_GLOB)), "a library was built for a refused ROM")
        rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg), "--build-game"], ui=False)
        check(rc != 0 and "front-end screens are not what this release was prepared for" in out, f"console flow: exit {rc}\n{out}")
    finally:
        c.binary = old
    return "a recipe fingerprint that does not match the ROM: build_failed page and console error, no library"


def overlay_recipe_missing(c: Ctx):
    """A class directory without overlays.txt is not a recipe for an overlay game: the ROM gets the 'no recipe' page (version_has_recipe /
    class_available also require the overlay recipe), and nothing is built."""
    exe = c.copy_app("app-no-overlay-recipe")
    (exe.parent / "resources" / "classes" / MAIN.cls / "overlays.txt").unlink()
    cfg = c.cfg("ovl-missing")
    old = c.binary
    c.binary = exe
    try:
        if len(list((exe.parent / "resources" / "classes").iterdir())) == 1:
            # The game's only class (THPS3): the app then runs no ROM version at all, and says so before any window opens.
            rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], ui=False)
            check(rc == 1 and "no runnable ROM version" in out, f"exit {rc}\n{out}")
            what = "the app has no runnable version (exit 1)"
        else:
            rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], actions="waitpage=rom_no_recipe|shot=no_recipe|click=quit", shots="overlay-recipe-missing")
            check(rc == 10 and "rom_no_recipe" in pages(out), f"exit {rc}, pages {pages(out)}\n{out}")
            check("no recipe" in out, out)
            what = "the 'no recipe' page"
        check(not (cfg / "game").exists() or not list((cfg / "game").glob(LIB_GLOB)), "a library was built without a recipe")
    finally:
        c.binary = old
    return f"a class without overlays.txt: {what}, no build"


def menu_hint(c: Ctx):
    """The in-game settings menu (THPS_MENU, on by default): the game that starts after the first-run build has the menu's assets (fonts,
    documents) and arms the one-time "Press Esc for Settings" hint: <config>/ui-hint-shown is written when the renderer starts."""
    feats = subprocess.run([str(c.binary), "--features"], stdout=subprocess.PIPE, text=True).stdout
    if "menu=1" not in feats:
        raise Skip("this app was built without the in-game settings menu (-DTHPS_MENU=OFF)")
    cfg = c.cfg("menu", with_library=True)
    (cfg / "ui-hint-shown").unlink(missing_ok=True)
    rc, out = c.run(["--config", str(cfg), "--verbose"], game_seconds=5)
    check(rc == 0 and "[thps] quit" in out and not pages(out), f"play: exit {rc}, pages {pages(out)}\n{out}")
    check("[thps-ui]" not in out, f"the menu reported a problem (assets missing from resources/ui?):\n{out}")
    check((cfg / "ui-hint-shown").exists(), "no ui-hint-shown: the menu's renderer hook did not start")
    return "game started after the first-run build; the menu's assets load and the one-time Settings hint is armed (ui-hint-shown)"


def fresh_defaults(c: Ctx):
    """The split build's first start on a config with no graphics.cfg (as after the first-run build): Enhanced is saved AND in force (the
    start line's "live:" part: border fill, HUD placement, 16:9; fog original, the game's list untouched), and it is still in force while the game runs
    (review 2026-10-26, finding 2: the defaults were written but not applied until the second start)."""
    cfg = c.cfg("defaults", with_library=True)
    (cfg / "graphics.cfg").unlink(missing_ok=True)
    e = dict(os.environ)
    for k in ("THPS_GAME_LIB", "THPS_UI", "THPS_UI_ACTIONS"):
        e.pop(k, None)
    ctl = Control(c.binary, ["--config", str(cfg), "--skip-intro"], e)
    try:
        start = next((l for l in ctl.lines if l.startswith("[thps] start:")), "")
        ctl.ask("wait 240")
        fog = ctl.ask("fog")
        stats = ctl.ask("stats")
    finally:
        ctl.close()
    check(re.search(r"live: crop=1 fog=original hud=1 aspect=1\.778", start) is not None, f"start line: {start!r}")
    check(" preset=enhanced" in start, f"start line: {start!r}")
    text = (cfg / "graphics.cfg").read_text() if (cfg / "graphics.cfg").exists() else ""
    check(re.search(r"(?m)^border=fill$", text) and re.search(r"(?m)^fog=original$", text), f"graphics.cfg: {text!r}")
    check(re.match(r"ok fog fog=original .*edits=0 .*walks=0", fog) is not None, f"fog: {fog!r}")   # Original never walks the lists
    m = re.search(r"crop=(\d) crop_tasks=(\d+)", stats)
    check(m is not None and m.group(1) == "1", f"stats: {stats!r}")
    return f"no graphics.cfg: Enhanced saved and in force ({start[start.find('live:'):]}); after 240 frames fog original (no walk), crop=1 ({m.group(2)} tasks filled)"


# ------------------------------------------------------------------------------------------------ relaunch: no build, saves persist
# "Does it compile every time I open the app?" (user, 2026-10-12). It must not: the first launch builds <config>/game/<game>_game_<class>.<ext> + .stamp,
# every later launch (same app, a moved or renamed copy of it, a launch through LaunchServices) loads it. And a Controller Pak save made in one launch
# is still there in the next. The pak scenarios are the regression suite's own (tests/scenarios.py sessions `pak` and `pak-reload`), run here through
# its Session on an app copy with a fresh config, so they need the suite's reference frames (build/tests/refs).
def suite_runner():
    """tests/run.py of the shared host as a module (importing it runs nothing): its Session executes the scenario steps."""
    here = Path(__file__).resolve().parent
    for d in (here, here.parents[1]):
        if str(d) not in sys.path:
            sys.path.insert(0, str(d))
    import run as R   # noqa: E402
    return R


def library_state(cfg: Path) -> dict:
    libs = list((cfg / "game").glob(LIB_PREFIX + "*." + LIB_EXT))
    check(len(libs) == 1, f"expected one game library in {cfg / 'game'}, found {libs}")
    lib = libs[0]
    stamp = Path(str(lib) + ".stamp")
    check(stamp.is_file(), f"no stamp next to {lib}")
    return {"lib": lib.name, "lib_bytes": lib.read_bytes(), "lib_mtime": lib.stat().st_mtime_ns, "stamp": stamp.read_text(), "stamp_mtime": stamp.stat().st_mtime_ns}


def builds_in_log(cfg: Path) -> tuple[int, int]:
    """(builds started, 'library is current' lines) over everything first-run.log has recorded."""
    log = (cfg / "first-run.log").read_text(errors="replace")
    return len(re.findall(r"^\[ *[\d.]+\] building the game library", log, re.M)), len(re.findall(r"^\[ *[\d.]+\] the game library is current", log, re.M))


def play_session(R, exe: Path, cfg: Path, sess_name: str, out: Path, rom: str | None) -> Path:
    """One host process of the suite's session `sess_name` (all its scenarios, in order) on `cfg`. Returns its host log."""
    sess = next((s for s in R.S.SESSIONS if s["name"] == sess_name), None)
    if sess is None:
        raise Skip(f"the game's scenarios have no {sess_name!r} session")
    out.mkdir(parents=True, exist_ok=True)
    inst = R.H.Instance(cfg, out / "host.log", rom=rom, extra=list(sess.get("args", [])), binary=exe)
    ses = R.Session(inst, out, False, 3.0 if R.H.load_info()["overloaded"] else 1.0, cfg)
    ses.rom = rom or ""
    try:
        for name, what, steps in sess["scenarios"]:
            ses.notes, ses.warnings, ses.dists = [], [], {}
            try:
                for st in steps:
                    ses.step(st)
            except Exception as e:   # noqa: BLE001
                raise AssertionError(f"scenario {name} ({what}) failed: {type(e).__name__}: {e}") from e
    finally:
        inst.quit()
    return out / "host.log"


def relaunch_saves(c: Ctx):
    """Build once; career run + SAVE in launch 1 (writes the Controller Pak); launch 2 (a copy of the app at another path, as App Translocation
    makes one) and launch 3 start without any build and see the saved note."""
    R = suite_runner()
    if not R.REFS.is_dir() or not list(R.REFS.glob("*.png")):
        raise Skip(f"the suite's reference frames are missing ({R.REFS}); run tools/test.sh --record once")
    exe = c.copy_app("relaunch-app")
    old = c.binary
    c.binary = exe
    try:
        cfg = c.cfg("relaunch")
        # launch 0: the first run, through the window, as a user's first start
        rc, out = c.run(["--rom", str(ROM_MAIN), "--config", str(cfg)], actions="waitpage=building|wait=0.5|shot=building|wait=60", shots="relaunch-first-launch")
        check(rc == 0 and "[first-run] done in" in out and pages(out) == ["building"], f"first launch did not build once (exit {rc}, pages {pages(out)})\n{out}")
        built = library_state(cfg)
        check(builds_in_log(cfg) == (1, 0), f"after the first launch: {builds_in_log(cfg)} (builds, current)")
        # launch 1: play, save a career. No --rom: the stored copy is used. No build.
        pak = cfg / "saves" / f"{FAMILY}_pak1.mpk"
        log1 = play_session(R, exe, cfg, "pak", c.tmp / "relaunch-out" / "play1", None)
        check("[first-run]" not in log1.read_text(errors="replace"), "launch 1 built something")
        check(pak.is_file() and pak.stat().st_size == 32768, f"no 32 KiB Controller Pak at {pak} after SAVE CAREER")
        saved = pak.read_bytes()
        # launch 2: the app at another path (a translocated app runs from a random one). Same config, same library, the save is listed and loads.
        moved = c.copy_app("relaunch-app-elsewhere-" + "x" * 7)
        c.binary = moved
        log2 = play_session(R, moved, cfg, "pak-reload", c.tmp / "relaunch-out" / "play2", None)
        text2 = log2.read_text(errors="replace")
        check("[first-run]" not in text2, "launch 2 (app moved) built something")
        check(re.search(r"pak 1: loaded .*" + re.escape(pak.name), text2), f"launch 2 did not load {pak.name}")
        # launch 3: the window path again (no --hidden would also be fine): nothing opens, nothing builds
        rc, out = c.run(["--config", str(cfg)])
        check(rc == 0 and "[first-run]" not in out and not pages(out), f"launch 3: exit {rc}, pages {pages(out)}\n{out}")
        after = library_state(cfg)
        check(after == built, "the game library or its stamp changed between launches (it must not be touched after the first build)")
        check(builds_in_log(cfg)[0] == 1, f"first-run.log shows {builds_in_log(cfg)[0]} builds, expected exactly 1")
        check(builds_in_log(cfg)[1] >= 3, f"first-run.log shows {builds_in_log(cfg)[1]} 'library is current' lines, expected one per later launch (3)")
        check(pak.read_bytes() == saved, "the Controller Pak changed across a relaunch that only listed and loaded the note")
    finally:
        c.binary = old
    return "1 build, 3 relaunches (one from a moved app) without a build, library+stamp untouched, Controller Pak note saved and read back"


def make_bundle(c: Ctx, dest: Path) -> Path:
    """A minimal .app around the engine and its resources (macOS), ad-hoc signed. If the engine already is inside an .app, that app is copied."""
    if c.binary.parent.name == "MacOS" and c.binary.parent.parent.name == "Contents":
        shutil.copytree(c.binary.parent.parent.parent, dest, symlinks=True)
        return dest / "Contents" / "MacOS" / c.binary.name
    res = resources_dir(c.binary)
    assert res, f"no resources folder beside {c.binary}"
    (dest / "Contents" / "MacOS").mkdir(parents=True)
    shutil.copy2(c.binary, dest / "Contents" / "MacOS" / EXE_NAME)
    shutil.copytree(res, dest / "Contents" / "Resources", symlinks=False)
    (dest / "Contents" / "Info.plist").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
        f'<plist version="1.0"><dict><key>CFBundleExecutable</key><string>{EXE_NAME}</string><key>CFBundleIdentifier</key><string>local.{FAMILY}.relaunchtest</string>'
        f'<key>CFBundleName</key><string>{FAMILY} relaunch test</string><key>CFBundlePackageType</key><string>APPL</string>'
        '<key>CFBundleVersion</key><string>1</string><key>LSMinimumSystemVersion</key><string>13.0</string></dict></plist>\n')
    subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(dest)], check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return dest / "Contents" / "MacOS" / EXE_NAME


def relaunch_open(c: Ctx):
    """macOS: the real way a user starts it. `open` (LaunchServices) on an .app with portable.txt beside it: the first launch builds into the folder
    beside the app, the second launch and a launch of the same app renamed (another path) do not. (A quarantined app would run from an App
    Translocation path; Gatekeeper's 'Open Anyway' is a person's click, so the test cannot make one. The stamp holds no path, so the result is the same.)"""
    if sys.platform != "darwin":
        raise Skip("macOS only (LaunchServices `open`)")
    base = c.tmp / "open"
    base.mkdir()
    (base / "portable.txt").touch()
    exe = make_bundle(c, base / "Relaunch Test.app")
    app = base / "Relaunch Test.app"
    if subprocess.run([str(exe), "--features"], stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode != 0:
        raise Skip("the bundled copy of the engine does not start (dependencies outside the bundle?)")
    n = [0]

    def launch(path: Path, extra: list[str]) -> str:
        n[0] += 1
        out = base / f"open{n[0]}.out"
        out.write_text("")
        e = {k: v for k, v in os.environ.items() if k in ("PATH", "HOME")}
        r = subprocess.run(["open", "-W", "-n", str(path), "--stdout", str(out), "--stderr", str(out), "--args", "--hidden", "--null-audio", "--mute", "--no-pads",
                            "--no-keyboard", "--seconds", "3"] + extra, env=e, timeout=300, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        check(r.returncode == 0, f"open {path.name} failed ({r.returncode}): {r.stdout}")
        return out.read_text(errors="replace")

    o1 = launch(app, ["--rom", str(ROM_MAIN)])
    check("[first-run] done in" in o1, f"launch 1 (open, --rom) did not build:\n{o1[-2000:]}")
    check((base / "game").is_dir() and (base / "first-run.log").is_file() and (base / "saves").is_dir(), "the portable folder beside the app was not used for the config")
    built = library_state(base)
    o2 = launch(app, [])
    check("[first-run]" not in o2 and "[ui] page" not in o2, f"launch 2 (open again) built or opened a window:\n{o2[:1500]}")
    renamed = base / "Renamed Elsewhere.app"
    app.rename(renamed)
    o3 = launch(renamed, [])
    check("[first-run]" not in o3 and "[ui] page" not in o3, f"launch 3 (renamed app) built something:\n{o3[:1500]}")
    check(library_state(base) == built, "the game library or its stamp changed")
    builds, current = builds_in_log(base)
    check(builds == 1 and current >= 2, f"first-run.log: {builds} builds, {current} 'current' lines (expected 1 and 2)")
    return "open: 1 build, then 2 launches (one from a renamed app) without a build, config beside the app"


SCENARIOS = [
    ("rom_errors", rom_error_scenarios, [ROM_MAIN]),
    ("picker_bypass", picker_bypass, [ROM_MAIN]),
    ("welcome_pick_and_drop", welcome_pick_and_drop, [ROM_MAIN]),
    ("no_compiler", no_compiler, [ROM_MAIN]),
    ("cancel", cancel_mid_build, [ROM_MAIN]),
    ("damaged_library", damaged_library, [ROM_MAIN]),
    ("app_update", app_update, [ROM_MAIN]),
    ("second_instance", second_instance, [ROM_MAIN]),
    ("read_only_folder", read_only_folder, [ROM_MAIN]),
    ("disk_full", disk_full, [ROM_MAIN]),
    ("build_failure", build_failure, [ROM_MAIN]),
    ("first_run_log", first_run_log, [ROM_MAIN]),
    ("menu_hint", menu_hint, [ROM_MAIN]),
    ("fresh_defaults", fresh_defaults, [ROM_MAIN]),
    ("relaunch_open", relaunch_open, [ROM_MAIN]),
    ("relaunch_saves", relaunch_saves, [ROM_MAIN]),
]
if OVERLAYS:
    SCENARIOS += [("overlay_table", overlay_table, [ROM_MAIN]), ("play_menus", play_menus, [ROM_MAIN]), ("overlay_refusal", overlay_refusal, [ROM_MAIN]),
                  ("overlay_recipe_missing", overlay_recipe_missing, [ROM_MAIN])]
for _r in OTHERS:
    SCENARIOS.append((f"other_release_{_r.suffix}" if _r.suffix != "us10" else "other_release", other_release(_r), [_r.rom]))


def main():
    ap = argparse.ArgumentParser()
    split_default = HERE / "build" / "cmake-split" / EXE_NAME
    # The split engine (build/cmake-split) when it exists: the static build (build/cmake) has no first run, so it is not a useful default here.
    ap.add_argument("--binary", default=os.environ.get(FAMILY.upper() + "_BINARY") or str(split_default if split_default.exists() else HERE / "build" / "cmake" / EXE_NAME))
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--shots", default=os.environ.get("FIRSTRUN_SHOTS"), help="folder for PNG screenshots of the first-run window")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list:
        print("\n".join(n for n, _, _ in SCENARIOS))
        return 0
    if subprocess.run([a.binary, "--features"], stdout=subprocess.PIPE, text=True).returncode != 0:
        print("cannot run", a.binary)
        return 2
    # --game-lib exists only in the split engine (THPS_SPLIT_GAME_LIB=ON); the static build answers "unknown argument".
    if subprocess.run([a.binary, "--game-lib", "x", "--features"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True).returncode != 0:
        print(f"{a.binary} is not a split-build engine (-DTHPS_SPLIT_GAME_LIB=ON): the first run exists only there. Build build/cmake-split and pass it with "
              f"--binary or ${FAMILY.upper()}_BINARY.")
        return 2
    pg = subprocess.run(["pgrep", "-fl", f"{FAMILY}_recomp"], stdout=subprocess.PIPE, text=True).stdout if not IS_WINDOWS else ""
    mine = [l for l in pg.splitlines() if "--control" not in l and "pgrep" not in l]
    if mine:
        print(f"another {FAMILY}_recomp is running (one at a time); wait for it:\n" + "\n".join(mine))
        return 2
    ctx = Ctx(Path(a.binary), Path(a.shots) if a.shots else None)
    fails = 0
    t_all = time.monotonic()
    try:
        for name, fn, needs in SCENARIOS:
            if a.only and name not in a.only:
                continue
            if any(n is None for n in needs):
                print(f"SKIP  {name}: a ROM it needs is missing")
                continue
            t0 = time.monotonic()
            try:
                msg = fn(ctx)
                print(f"PASS  {name} ({time.monotonic() - t0:.1f} s): {msg}")
            except Skip as e:
                print(f"SKIP  {name}: {e}")
            except Exception as e:   # noqa: BLE001
                fails += 1
                print(f"FAIL  {name} ({time.monotonic() - t0:.1f} s): {e}")
    finally:
        ctx.close()
    print(f"{'FAILED' if fails else 'ok'}: {fails} failed, {time.monotonic() - t_all:.0f} s")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
