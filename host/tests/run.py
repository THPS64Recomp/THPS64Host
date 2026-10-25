#!/usr/bin/env python3
"""THPS recomp regression suite (stdlib only; shared by THPS1-3: run it through decomp/thps<N>/recomp-host/tests/run.py).

    tests/run.py [--version ID] [--rom PATH] [--record] [--only NAME[,NAME]] [--session NAME] [--rerun-failed]

Runs the scenarios of tests/scenarios.py against build/cmake/<game>_recomp --control, one host
instance per session (scenarios in a session share it), and prints a summary table. Results go
to build/tests/out/<timestamp>/ (results.json, per-session host logs, shots of each checkpoint
and a contact sheet of failures); exit status 1 if any scenario failed, 2 on setup errors.

ROM versions: --version ID (a decomp/common/games.toml row of the game's family; default = the family root, e.g. thps1 = USA v1.1)
runs the suite on that version's ROM. References live in build/tests/refs/ (the family root) or
build/tests/refs-<ID>/, and a checkpoint missing there falls through to the row's `refs_from` (Europe
and v1.0 reuse v1.1's frames; --record captures only what the chain lacks, --rerecord-cp NAME[,NAME]
re-captures named checkpoints for this version). Other state (config dirs) is per version too.

References: build/tests/refs/<checkpoint>.png. Missing ones make a scenario fail unless
--record is given, which captures them at the checkpoint's `record` timing (see scenarios.py)
and writes build/tests/refs/sheet.png for a human look. All of this is ROM-derived and stays
under build/ (gitignored).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))                    # decomp/common/host/tests: harness, hfr_steps
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))                # decomp/common: thpsgame (the version table)
import thpsgame  # noqa: E402
import harness as H  # noqa: E402  ($THPS_HOST names the game)
sys.path.append(str(H.HOST_DIR / "tests"))                                  # the game's own scenarios.py (after the shared dir: its run.py is a wrapper)
import hfr_steps  # noqa: E402
import scenarios as S  # noqa: E402

GAME = getattr(S, "GAME", {})                                                  # per-game test knobs (scenarios.py)
BASE_VERSION = H.FAMILY                                                     # thps1 / thps2 / thps3
BUILD0 = H.HOST_DIR / "build" / "tests"
BUILD = BUILD0          # config dirs (per version: set by use_version)
REFS = BUILD0 / "refs"  # this version's own reference dir
VERSION = thpsgame.load(BASE_VERSION)
RERECORD: set[str] = set()   # checkpoints to capture for this version even if a fall-through reference exists


def use_version(vid: str):
    """Select the ROM version under test: its table row, its reference dir and config dirs."""
    global VERSION, REFS, BUILD
    VERSION = thpsgame.load(vid)
    if VERSION.family != BASE_VERSION:
        raise SystemExit(f"--version {vid}: not a {BASE_VERSION} version")
    REFS = BUILD0 / ("refs" if vid == BASE_VERSION else f"refs-{vid}")
    BUILD = BUILD0 if vid == BASE_VERSION else BUILD0 / vid
    _ref_cache.clear()


def ref_chain() -> list[Path]:
    """Reference dirs to look in: the version's own, then its `refs_from` chain."""
    dirs, g, seen = [REFS], VERSION, set()
    while getattr(g, "refs_from", "") and g.refs_from not in seen:
        seen.add(g.refs_from)
        g = thpsgame.load(g.refs_from)
        dirs.append(BUILD0 / ("refs" if g.name == BASE_VERSION else f"refs-{g.name}"))
    return dirs


def find_ref(cp: str) -> Path | None:
    for d in ref_chain():
        if (d / f"{cp}.png").exists():
            return d / f"{cp}.png"
    return None


def default_rom() -> str:
    """$THPS<N>_ROM, else roms/<one of the version's rom_names> in the nearest parent dir that has it
    (the main checkout when run from a worktree under .worktrees/)."""
    if os.environ.get(BASE_VERSION.upper() + "_ROM"):
        return os.environ[BASE_VERSION.upper() + "_ROM"]
    for d in Path(__file__).resolve().parents:
        for name in VERSION.rom_names:
            if (d / "roms" / name).exists():
                return str(d / "roms" / name)
    return ""


class StepFail(Exception):
    pass


class Session:
    def __init__(self, inst: H.Instance, out: Path, record: bool, slow: float, config: Path):
        self.inst, self.out, self.record, self.slow, self.config = inst, out, record, slow, config
        self.rom = ""   # the suite's ROM path (loadrom)
        self.k = 0
        self.last_img: H.Image | None = None
        self.last_shot: Path | None = None
        self.pak_hash: str | None = None
        self.notes: list[str] = []
        self.warnings: list[str] = []
        self.dists: dict[str, float] = {}
        self.ui_input_mark = 0
        self.present_every = 0   # the session's --present-dump-every (0: no captures armed)
        self.watch_polls = 0   # input poll count of the state change the last go_watch saw (a note; also bounds the press that follows)

    # -- frames -----------------------------------------------------------------------
    def shot(self) -> tuple[H.Image, dict]:
        p = self.out / f"poll_{self.k % 6}.png"
        self.k += 1
        info = self.inst.shot(p, timeout=60 * self.slow)
        img = H.read_png(p)
        self.last_img, self.last_shot = img, p
        return img, info

    def _peek(self, addr: int | str, n: int = 1) -> list[int]:
        """RDRAM words at an address, or at a host name of the running version ("@gSkater")."""
        self.inst.send(f"peek {addr if isinstance(addr, str) else f'{addr:#x}'} {n}")
        return [int(w, 16) for w in self.inst.expect("ok peek", 30).split()[3:]]

    def _captures_until_now(self, timeout: float = 30) -> list[Path]:
        """The session's present captures up to the frame composited now, once they are on disk. The host PNG-encodes them on writer
        threads that can fall seconds behind: at 120 Hz with present_every 3 (40 captures/s at 1280x720) and a loaded machine the newest
        file was ~5 s old (2026-10-15, hud-special: the newest captures were from before the meter was poked). `pcap EVERY` (same EVERY,
        so nothing changes) replies with RT64's composited-frame count, which is the index in the capture names."""
        pdir = self.out / "present"
        idx = lambda f: int(f.name.split("_")[1])   # noqa: E731
        if not self.present_every:
            return sorted(pdir.glob("present_*.png"))
        self.inst.send(f"pcap {self.present_every}")
        now = int(re.search(r"composited=(\d+)", self.inst.expect("ok pcap", 30)).group(1))
        deadline = time.monotonic() + timeout * max(self.slow, 1)
        while True:
            files = sorted(pdir.glob("present_*.png"))
            if (files and idx(files[-1]) >= now - self.present_every) or time.monotonic() > deadline:
                break
            time.sleep(0.2)
        if not files or idx(files[-1]) < now - self.present_every:
            self.warnings.append(f"present captures still behind after {timeout:.0f} s (newest {files[-1].name if files else None}, composited {now})")
        time.sleep(0.3)   # the writers are three threads: the ones just before `now` may still be in flight
        return [f for f in sorted(pdir.glob("present_*.png")) if idx(f) <= now + self.present_every]

    def _sym(self, name: str) -> int:
        """The running version's address / offset of a host name (versions/host_syms.toml)."""
        self.inst.send(f"sym {name}")
        return int(self.inst.expect("ok sym", 30).split("=")[1], 16)

    def _pressed_records(self) -> list[int]:
        w = self._peek("@gSControl", 80)
        return [i for i in range(20) if w[4 * i] >> 24]

    def keep(self, name: str):
        if self.last_shot:
            shutil.copyfile(self.last_shot, self.out / f"{self.k:04d}_{name}.png")

    def match(self, img: H.Image, cp: str) -> tuple[bool, float]:
        c = S.CHECKPOINTS[cp]
        if f"{img.w}x{img.h}" != c["size"]:
            return False, 9.0
        if not c["regions"]:   # size-only checkpoint (e.g. the attract demo)
            return True, 0.0
        ref_p = find_ref(cp)
        if ref_p is None:
            raise StepFail(f"no reference for '{cp}' ({REFS / (cp + '.png')}); run with --record first")
        ref = _ref_cache.get(cp) or _ref_cache.setdefault(cp, H.read_png(ref_p))
        worst = max(H.region_ratio(img, ref, spec) for spec in c["regions"])
        return worst <= 1.0, worst  # worst = distance / tolerance of the worst region (<= 1 = match)

    def record_ref(self, cp: str):
        REFS.mkdir(parents=True, exist_ok=True)
        img, info = self.shot()
        want = S.CHECKPOINTS[cp]["size"]
        if f"{img.w}x{img.h}" != want:
            raise StepFail(f"record '{cp}': frame is {img.w}x{img.h}, expected {want}")
        shutil.copyfile(self.last_shot, REFS / f"{cp}.png")
        _ref_cache.pop(cp, None)
        self.notes.append(f"recorded ref {cp} at frame {info['frame']}")
        self.keep(cp)

    def wait_until(self, cp: str, timeout: float, poll_frames: int = 10, on_poll=None) -> float:
        deadline = time.monotonic() + timeout * self.slow
        best = 99.0
        while time.monotonic() < deadline:
            img, _ = self.shot()
            ok, d = self.match(img, cp)
            best = min(best, d)
            if ok:
                self.dists[cp] = round(d, 3)
                self.keep(cp)
                return d
            if on_poll:
                on_poll(img)
            self.inst.wait_frames(poll_frames, timeout=60 * self.slow)
        raise StepFail(f"checkpoint '{cp}' ({S.CHECKPOINTS[cp]['what']}) not reached in {timeout * self.slow:.0f}s "
                       f"(best distance/tol {best:.2f}); last frame {self.last_shot}")

    def wait_peek(self, addr, want: int, timeout: float, on_poll=None):
        """Wait, one displayed frame at a time, until the RDRAM word at `addr` (see _peek) equals `want` (a state check; for the exact frame of
        a state change use go_watch)."""
        deadline = time.monotonic() + timeout * self.slow
        last = None
        while True:
            last = self._peek(addr)[0]
            if last == want:
                return
            if time.monotonic() > deadline:
                raise StepFail(f"{addr} never became {want} in {timeout * self.slow:.0f}s (last value {last})")
            if on_poll:
                on_poll()
            self.inst.wait_frames(1, timeout=60 * self.slow)

    def have_ref(self, cp: str) -> bool:
        if not S.CHECKPOINTS[cp]["regions"]:   # size-only checkpoints need none
            return True
        if cp in RERECORD:   # --rerecord-cp: only this version's own file counts
            return (REFS / f"{cp}.png").exists()
        return find_ref(cp) is not None

    def ui_state(self) -> dict[str, str]:
        """`ui state` (in-game settings menu) as a dict; the graphics line is under "gfx"."""
        self.inst.send("ui state")
        line = self.inst.expect("ok ui", 30)
        head, _, gfx = line.partition(" gfx: ")
        d = dict(kv.split("=", 1) for kv in head.split()[2:] if "=" in kv)
        d["gfx"] = gfx
        return d

    # -- steps ------------------------------------------------------------------------
    def step(self, st: tuple):
        kind = st[0]
        self.inst.note(f"step {st}")
        if kind == "wait_for":
            cp = st[1]
            if self.record and not self.have_ref(cp):
                how, n = S.CHECKPOINTS[cp]["record"]
                if how == "frame":
                    cur = int(self.inst.stats()["frames"])
                    if n > cur:
                        self.inst.wait_frames(n - cur, timeout=120 * self.slow)
                else:
                    self.inst.wait_frames(n, timeout=120 * self.slow)
                self.record_ref(cp)
            else:
                self.wait_until(cp, 90)
        elif kind == "go":
            _, button, from_cp, to_cp = st[:4]
            timeout = st[4] if len(st) > 4 else 60
            if self.record and not self.have_ref(to_cp):
                self.inst.press(button, timeout=60 * self.slow)
                self.inst.wait_frames(S.CHECKPOINTS[to_cp]["record"][1], timeout=120 * self.slow)
                self.record_ref(to_cp)
                return
            # Press, then poll for to_cp. Re-press only while the screen still shows from_cp
            # (dropped press) -- or, with no from_cp, every ~2 s of game frames.
            self.inst.press(button, timeout=60 * self.slow)
            last_press = int(self.inst.stats()["frames"])
            presses = 1

            def on_poll(img):
                nonlocal last_press, presses
                frames = int(self.inst.stats()["frames"])
                if frames - last_press < 45:
                    return
                still = self.match(img, from_cp)[0] if from_cp else True
                if still and presses < GAME.get("press_limit", 8):   # THPS3: START on the logo screen is ignored for its first 7 s, the demo needs another
                    self.inst.press(button, timeout=60 * self.slow)
                    last_press, presses = int(self.inst.stats()["frames"]), presses + 1
            self.wait_until(to_cp, timeout, on_poll=on_poll)
            if presses > 1:
                self.notes.append(f"{button} {from_cp}->{to_cp}: {presses} presses")
        elif kind == "menu":
            _, menu, index = st
            m = S.MENUS[menu]
            for attempt in range(3 * len(m["rows"]) + 4):
                img, _ = self.shot()
                y = H.highlight_band(img, m["y_range"])
                cur = None if y is None else min(range(len(m["rows"])), key=lambda i: abs(m["rows"][i] - y))
                if cur == index:
                    self.keep(f"{menu}_{index}")
                    return
                if cur is None:   # cursor blinking or menu still fading in
                    self.inst.wait_frames(8)
                    continue
                self.inst.press("DOWN" if (index - cur) % len(m["rows"]) <= len(m["rows"]) // 2 else "UP", 3, 3)
                self.inst.wait_frames(10)
            raise StepFail(f"menu '{menu}': could not highlight {m['items'][index]} (cursor at {cur}); {self.last_shot}")
        elif kind == "menu_at":
            _, menu, index = st
            m = S.MENUS[menu]
            seen = []
            for attempt in range(8):
                img, _ = self.shot()
                y = H.highlight_band(img, m["y_range"])
                if y is not None:
                    cur = min(range(len(m["rows"])), key=lambda i: abs(m["rows"][i] - y))
                    seen.append(cur)
                    if len(seen) >= 2:
                        break
                self.inst.wait_frames(6)
            if not seen or any(c != index for c in seen):
                raise StepFail(f"menu '{menu}': cursor at {[m['items'][c] for c in seen]}, expected {m['items'][index]}; {self.last_shot}")
            self.keep(f"{menu}_at_{index}")
            self.notes.append(f"{menu} menu cursor on {m['items'][index]}")
        elif kind == "menu_name_end":
            # name-entry grid (OPTIONS > CONTROLLER PAK > SAVE DATA): from the first letter to END (last row, 8th column)
            for b in ["DOWN"] * 4 + ["RIGHT"] * 7:
                self.inst.press(b, 3, 5)
                self.inst.wait_frames(4)
            self.inst.wait_frames(10)
        elif kind == "wait_peek":
            # ("wait_peek", ADDR, VALUE[, TIMEOUT_S]): one displayed frame at a time until the game word at ADDR (a host name "@gRunActive" or an
            # address) equals VALUE. A game-state anchor: it does not depend on how long a level load took or how fast the checkpoint polls are.
            self.wait_peek(st[1], st[2], st[3] if len(st) > 3 else 90)
        elif kind == "go_watch":
            # ("go_watch", BUTTON, FROM_CP, ADDR, VALUE[, TIMEOUT_S]): press BUTTON and wait for the game word at ADDR ("@gRunActive") to become VALUE,
            # as seen by the HOST'S GAME THREAD (control.cpp `watch`): the answer is the exact input poll of the state change, whatever the load (a
            # checkpoint poll sees it 0-10 frames late; polling the word from here, 0-2 frames late by a race of a few ms with the game thread).
            # Re-pressed every ~2 s of game frames while the screen still shows FROM_CP (a dropped press).
            _, button, from_cp, addr, want, *rest = st
            timeout = rest[0] if rest else 90
            self.inst.watch(addr, want)
            self.inst.press(button, timeout=60 * self.slow)
            last_press = int(self.inst.stats()["frames"])
            presses = 1
            deadline = time.monotonic() + timeout * self.slow
            while (polls := self.inst.wait_watch(0.05)) is None:
                if time.monotonic() > deadline:
                    raise StepFail(f"go_watch: {addr} never became {want} in {timeout * self.slow:.0f}s after {button} ({presses} presses); {self.last_shot}")
                if int(self.inst.stats()["frames"]) - last_press >= 45 and presses < GAME.get("press_limit", 8):
                    if self.match(self.shot()[0], from_cp)[0]:
                        self.inst.press(button, timeout=60 * self.slow)
                        last_press, presses = int(self.inst.stats()["frames"]), presses + 1
            self.watch_polls = polls
            self.notes.append(f"{button} {from_cp} -> {addr}={want} at poll {polls} ({presses} press{'es' if presses > 1 else ''})")
        elif kind == "press_when":
            # ("press_when", ADDR, GE, BUTTON): the host's game thread presses BUTTON at the first poll where the RDRAM word at ADDR is >= GE
            # (control.cpp `pressif`). For a game tick counter: the game's update runs a variable number of ticks per displayed frame (2, sometimes
            # 0 or 4, by real elapsed time), so the picture N polls after something differs by up to ~5 ticks from run to run (THPS3 Foundry:
            # tick counter 62 / 64 / 67 at the same poll), but the picture at a given TICK is the same in every run.
            _, addr, ge, button = st
            self.inst.note(f"pressif {addr} {ge} {button}")
            self.inst.send(f"pressif {addr} {ge} {button} 3 3")
            line = self.inst.expect("ok press", 120 * self.slow)
            self.notes.append(f"{button} when {addr} >= {ge} ({line})")
        elif kind == "press_after":
            # Press exactly N game input polls after the previous press finished (its polls=).
            _, button, n = st
            target = self.inst.last_press_polls + n
            self.inst.note(f"pressat {target} {button}")
            self.inst.send(f"pressat {target} {button} 3 3")
            line = self.inst.expect("ok press", 120 * self.slow)
            self.notes.append(f"{button} at poll {target} ({line})")
        elif kind == "press":
            self.inst.press(st[1], timeout=60 * self.slow)
            self.inst.wait_frames(10)
        elif kind == "cpu":
            # ("cpu", SECONDS): CPU time the host process (all threads) used over SECONDS of wall time, as a note: % of one core (POSIX `ps`).
            def cpu_s():
                t = subprocess.run(["ps", "-o", "time=", "-p", str(self.inst.proc.pid)], capture_output=True, text=True).stdout.strip()
                m, sec = t.split(":")
                return int(m) * 60 + float(sec)
            c0, t0 = cpu_s(), time.monotonic()
            time.sleep(st[1])
            self.notes.append(f"cpu: host used {100 * (cpu_s() - c0) / (time.monotonic() - t0):.0f} % of one core over {st[1]} s")
        elif kind == "ring_check":
            # ("ring_check", NAME, MAX_SPREAD): the frames of a ("pcap", ...) step: in every frame the picture's first column on the left and its last
            # column on the right (first/last column of the middle rows with any pixel above 3/255, i.e. not the black ring; every row looked at)
            # must lie within MAX_SPREAD pixels of the other frames' (a stale strip in the border ring of one of the three framebuffers shows
            # as a jump of 6+ px at 640 wide). Threshold 3, not 40 (2026-10-14): the world's right edge is dark in places (one frame of the
            # THPS3 level has no pixel above 36 in the last 8 columns), a bright-pixel threshold read that as the edge moving to column 613.
            files = sorted((self.out / st[1]).glob("*.png"))
            if len(files) < 10:
                raise StepFail(f"ring_check: only {len(files)} frames in {st[1]}")
            lefts, rights = [], []
            for f in files:
                img = H.read_png(f)
                y0, y1 = img.h // 10, img.h * 9 // 10
                def bright(x):
                    return any(max(img.px(x, y)) > 3 for y in range(y0, y1))
                lefts.append(next((x for x in range(img.w // 4) if bright(x)), -1))
                rights.append(next((x for x in range(img.w - 1, img.w * 3 // 4, -1) if bright(x)), -1))
            self.notes.append(f"ring_check {st[1]}: {len(files)} frames, left edge columns {sorted(set(lefts))}, right edge columns {sorted(set(rights))}")
            if max(lefts) - min(lefts) > st[2] or max(rights) - min(rights) > st[2]:
                raise StepFail(f"ring_check: the picture's edge moves between frames (left {min(lefts)}..{max(lefts)}, right {min(rights)}..{max(rights)}; max spread {st[2]})")
        elif kind == "ring_black":
            # ("ring_black", NAME, COLS, MAX_LUMA, SKIP): the frames of a ("pcap", ...) clip NAME (ignoring the first SKIP files): the outer COLS columns on each side of
            # the middle rows (20 %..80 % of the height) stay black (luma <= MAX_LUMA, every frame): stale text in the colour buffer's margin ring (THPS1 Chicago
            # competition, LOG RP) shows there as bright pixels.
            _, name, cols, max_luma, skip = st
            files = sorted((self.out / name).glob("*.png"))[skip:]
            if len(files) < 5:
                raise StepFail(f"ring_black: only {len(files)} frames in {name}")
            worst = 0
            for f in files:
                img = H.read_png(f)
                y0, y1 = img.h // 5, img.h * 4 // 5
                for x in [*range(cols), *range(img.w - cols, img.w)]:
                    for y in range(y0, y1):
                        r, g, b = img.px(x, y)[:3]
                        worst = max(worst, int(0.299 * r + 0.587 * g + 0.114 * b))
            self.notes.append(f"ring_black {name}: {len(files)} frames, brightest margin pixel (outer {cols} columns) {worst}")
            if worst > max_luma:
                raise StepFail(f"ring_black: a margin pixel has luma {worst} (> {max_luma}): stale picture in the ring")
        elif kind == "ctl":
            # ("ctl", "COMMAND ..."): send a raw --control command (exploration), the reply goes into the notes.
            self.inst.send(st[1])
            self.notes.append(self.inst.expect("ok " + st[1].split()[0], 30))
        elif kind == "poke_ptr":
            # ("poke_ptr", PTR_ADDR, OFFSET, VALUE): write VALUE at (the pointer stored at PTR_ADDR) + OFFSET (exploration: e.g. THPS1 USA v1.1's total tape
            # count, which unlocks every course at the course select; see LOG RP).
            _, ptr_addr, off, val = st
            base = self._peek(ptr_addr)[0]
            if not 0x80000000 <= base < 0x80800000:
                raise StepFail(f"poke_ptr: {ptr_addr:#x} holds {base:#x}, not a pointer")
            self.inst.cmd(f"poke {base + off:#x} {val:#x}")
            self.notes.append(f"poke_ptr: {base + off:#x} = {val:#x}")
        elif kind == "peek_ptr":
            # ("peek_ptr", PTR_ADDR, OFFSET, N[, WANT_BYTE_AT, WANT]): N words at (the pointer at PTR_ADDR) + OFFSET, into the notes; with WANT_BYTE_AT,
            # the byte at that offset (from the pointer) must equal WANT (e.g. THPS1's pause menu: entry count at +5).
            _, ptr_addr, off, n, *want = st
            base = self._peek(ptr_addr)[0]
            if not 0x80000000 <= base < 0x80800000:
                raise StepFail(f"peek_ptr: {ptr_addr} holds {base:#x}, not a pointer")
            words = self._peek(base + off, n)
            self.notes.append(f"*{ptr_addr}+{off:#x} = {base + off:#x}: " + " ".join(f"{w:08X}" for w in words))
            if want:
                at, value = want
                w = self._peek(base + (at & ~3))[0]
                b = (w >> (8 * (3 - (at & 3)))) & 0xFF
                self.notes.append(f"byte +{at:#x} = {b}")
                if b != value:
                    raise StepFail(f"peek_ptr: byte +{at:#x} of *{ptr_addr} is {b}, expected {value}")
        elif kind == "unlock_courses":
            # ("unlock_courses", BASE, VARIANT): THPS1's career keeps one 20-byte record per course at BASE (USA v1.1: 0x800DE9F4); byte +8 is the tape count the
            # course select shows. VARIANT 0: 40 tapes each; 1: also every other byte of the record 3 (a guess at the medals Downtown / Streets ask for).
            _, base, variant = st
            for i in range(10):
                a = base + i * 20 + 8
                w = self._peek(a)[0]
                self.inst.cmd(f"poke {a:#x} {((40 << 24) | (0x030303 if variant else w & 0xFFFFFF)):#x}")
                if variant:
                    self.inst.cmd(f"poke {a + 4:#x} 0x03030303")
                    self.inst.cmd(f"poke {a + 8:#x} 0x03030303")
            self.notes.append(f"unlock_courses variant {variant}: " + " ".join(f"{w:08X}" for w in self._peek(base, 15)))
        elif kind == "cheat":
            # ("cheat", KEY, 0|1): the settings menu's Cheats toggle through the control channel (src/cheats.cpp: saves cheats.cfg, logs the change).
            self.inst.send(f"cheat {st[1]} {st[2]}")
            self.notes.append(self.inst.expect("ok cheat", 30))
        elif kind == "cheat_yes":
            # ("cheat_yes", KEY, MIN): THPS2/3: the game asked Cheat_IsOn for cheat KEY and the host said "on" at least MIN times so far (control "cheat").
            self.inst.send("cheat")
            r = self.inst.expect("ok cheat", 30)
            m = re.search(rf"\b{st[1]}:(\d+)", r)
            n = int(m.group(1)) if m else -1
            self.notes.append(f"{st[1]}: the game was told 'on' {n} times")
            if n < st[2]:
                raise StepFail(f"cheat_yes: {st[1]} answered {n} times (< {st[2]}): {r}")
        elif kind == "peek_is":
            # ("peek_is", ADDR, WANT[, WHAT]): the game word at ADDR (a host name "@gCourseTable+0x88" or an address) equals WANT.
            w = self._peek(st[1])[0]
            what = st[3] if len(st) > 3 else st[1]
            self.notes.append(f"{what}: {w:#x}")
            if w != st[2]:
                raise StepFail(f"{what}: {st[1]} = {w:#x}, expected {st[2]:#x}")
        elif kind == "peek_save":
            # ("peek_save", NAME, ADDR, N): keep N game words at ADDR; ("peek_same", NAME) later checks they are unchanged (e.g. the career data
            # before and after a cheat was used: cheats must never reach what the Controller Pak save is made from).
            _, name, addr, n = st
            self.saved_words = getattr(self, "saved_words", {})
            self.saved_words[name] = (addr, n, self._peek(addr, n))
            self.notes.append(f"{name}: {n} words at {addr if isinstance(addr, str) else hex(addr)} kept")
        elif kind == "peek_same":
            addr, n, before = self.saved_words[st[1]]
            after = self._peek(addr, n)
            diff = [i for i in range(n) if before[i] != after[i]]
            if diff:
                i = diff[0]
                raise StepFail(f"{st[1]}: {len(diff)} of {n} words changed (first +{4 * i:#x}: {before[i]:08X} -> {after[i]:08X})")
            self.notes.append(f"{st[1]}: {n} words unchanged")
        elif kind == "skater_watch":
            # ("skater_watch", ROUNDS, INPUTS, FIELDS[, CHECK]): ROUNDS times: the vpad INPUTS (as in "skate": ("vpad", NAME, 0|1) | ("axis", NAME, V) |
            # ("wait", FRAMES)), then one peek of each skater FIELD (offsets into CBruce via @gSkater, or host field names). Notes: per field the
            # distinct values (up to 12) and the largest |value| (signed words). CHECK = (FIELD, KEY_FIELD, KEY_VALUE, MAX_ABS, MIN_SAMPLES): of the
            # rounds where KEY_FIELD == KEY_VALUE (e.g. the physics state of a grind), FIELD must stay within +-MAX_ABS, and there must be at least
            # MIN_SAMPLES of them (the inputs really got the skater there; with CHECK, up to 3 x ROUNDS rounds until they did).
            _, rounds, inputs, fields, *chk = st
            sk = self._peek("@gSkater")[0]
            if not 0x80000000 <= sk < 0x80800000:
                raise StepFail(f"no skater object (gSkater = {sk:#x})")
            # a field "@gName" is a game word of its own (not in the skater), e.g. "@gTimeScale"
            offs = {f: (None if isinstance(f, str) and f.startswith("@") else f if isinstance(f, int) else self._sym(f)) for f in fields}
            rows = []
            batches = 3 if chk and chk[0][0] != "some" else 1   # a rail is a matter of luck: up to 3 batches until MIN_SAMPLES rounds in the key state
            for _ in range(rounds * batches):
                if chk and chk[0][0] != "some" and len(rows) % rounds == 0 and rows:
                    f0, k0, kv0, _m, n0 = chk[0]
                    if sum(1 for r in rows if r[k0] == kv0) >= n0:
                        break
                for kind2, a, *b in inputs:
                    if kind2 == "wait":
                        self.inst.wait_frames(a)
                    else:
                        self.inst.send(f"vpad {'button' if kind2 == 'vpad' else 'axis'} {a} {b[0]}")
                        self.inst.expect("ok vpad", 30)
                rows.append({f: (lambda w: w - (1 << 32) if w & 0x80000000 else w)(self._peek(f if o is None else sk + o)[0]) for f, o in offs.items()})
            for f in fields:
                vals = sorted({r[f] for r in rows})
                name = f if isinstance(f, str) else hex(f)
                self.notes.append(f"{name}: {len(vals)} values {vals[:12]}{' ...' if len(vals) > 12 else ''}, max |v| {max(abs(v) for v in vals)}")
            for other in fields[1:]:   # where another field is not at its most common value: the first field's values then (e.g. the physics
                common = max({r[other] for r in rows}, key=lambda v: sum(1 for r in rows if r[other] == v))   # state while the rail balance moves)
                at = sorted({r[fields[0]] for r in rows if r[other] != common})
                self.notes.append(f"{other} != {common} at {fields[0]} in {at}")
            if chk and chk[0][0] == "some":   # ("some", FIELD, VALUE): at least one round had FIELD == VALUE (e.g. the step at a quarter)
                _, field, value = chk[0]
                if not any(r[field] == value for r in rows):
                    raise StepFail(f"skater_watch: {field} was never {value}")
                chk = []
            if chk:
                field, key, key_v, max_abs, min_n = chk[0]
                sel = [r[field] for r in rows if r[key] == key_v]
                self.notes.append(f"check: {len(sel)} rounds with {key} = {key_v}; {field} max |v| {max((abs(v) for v in sel), default=0)}")
                if len(sel) < min_n:
                    raise StepFail(f"skater_watch: only {len(sel)} rounds with {key} = {key_v} (need {min_n}): the inputs did not get there")
                if any(abs(v) > max_abs for v in sel):
                    raise StepFail(f"skater_watch: {field} reached {max(abs(v) for v in sel)} (> {max_abs}) while {key} = {key_v}")
        elif kind == "press_moves":
            # ("press_moves", BUTTON, True|False): press BUTTON; the picture must change (e.g. an unlocked course starts loading) or stay
            # (a locked course ignores A). Thumbnail distance > 3, as the soak's course-select check.
            _, button, want = st
            before, _ = self.shot()
            self.inst.press(button, timeout=60 * self.slow)
            self.inst.wait_frames(45, timeout=120 * self.slow)
            after, _ = self.shot()
            self.keep(f"after_{button}")
            d = H.thumb_dist(H.thumb(before), H.thumb(after))
            self.notes.append(f"{button}: picture distance {d:.1f}")
            if (d > 3.0) != want:
                raise StepFail(f"{button}: the picture {'did not change' if want else 'changed'} (distance {d:.1f}); {self.last_shot}")
        elif kind == "ctl_match":
            # ("ctl_match", "COMMAND ...", REGEX): send a --control command; its reply must match REGEX (and goes into the notes).
            self.inst.send(st[1])
            reply = self.inst.expect("ok " + st[1].split()[0], 30)
            self.notes.append(reply)
            if not re.search(st[2], reply):
                raise StepFail(f"'{st[1]}' replied {reply!r}, expected a match of /{st[2]}/")
        elif kind == "meter_inside":
            # ("meter_inside",): THPS1's SPECIAL meter on the newest present captures (hud_meter.py): the fill lies inside the frame. The fill pulses, so the
            # newest 6 captures (18 frames) are tried; one with a fill must pass, and at least one must have a fill.
            from hud_meter import check_meter
            time.sleep(0.5)
            files = self._captures_until_now()[-6:]
            if not files:
                raise StepFail(f"no present capture in {self.out / 'present'}")
            notes, found = [], False
            for f in reversed(files):
                try:
                    r = check_meter(H.read_png(f), f.name)
                except AssertionError as e:
                    self.keep("meter_fail")
                    raise StepFail(str(e))
                if r:
                    notes.append(r)
                    found = True
            if not found:
                raise StepFail("no green / yellow meter fill in the last 6 present captures (the meter was not filled, or the fill is not drawn)")
            self.notes.append(notes[0] + (f" (+{len(notes) - 1} more captures checked)" if len(notes) > 1 else ""))
        elif kind == "poke_skater":
            # ("poke_skater", FIELD, VALUE[, FIELD, VALUE ...]): write words into the skater object (CBruce via @gSkater): FIELD = an int
            # offset or a struct-field name of the version (versions/host_syms.toml [field]). E.g. THPS1's special meter (Skater_MagicTime).
            sk = self._peek("@gSkater")[0]
            if not 0x80000000 <= sk < 0x80800000:
                raise StepFail(f"no skater object (gSkater = {sk:#x})")
            args = st[1:]
            for f, v in zip(args[0::2], args[1::2]):
                off = f if isinstance(f, int) else self._sym(f)
                self.inst.send(f"poke {sk + off:#x} {v:#x}")
                self.inst.expect("ok poke", 30)
                self.notes.append(f"poke skater+{off:#x} = {v:#x}")
        elif kind == "name_is":
            # ("name_is", "ab"): THPS1's name-entry screen holds the typed name (13 ASCII slots, lower case, 0x20 = blank) at
            # gNameEntryBuf; the slots after the last letter must still be blank.
            raw = b"".join(w.to_bytes(4, "big") for w in self._peek(self._sym("gNameEntryBuf"), 4))[:13]
            typed = raw.decode("latin-1")
            self.notes.append(f"name buffer {typed!r}")
            if typed.rstrip(" ") != st[1] or typed != st[1].ljust(13):
                raise StepFail(f"name entry buffer is {typed!r}, expected {st[1].ljust(13)!r}")
        elif kind == "vstick":
            # ("vstick", "up"|"down"|"left"|"right"): push the virtual controller's left stick for 6 frames, then release. THPS1's name
            # entry only reads the D-pad: the host maps the stick (and the keyboard arrows, which feed the same stick) to it there.
            axis, value = {"up": ("lefty", -30000), "down": ("lefty", 30000), "left": ("leftx", -30000), "right": ("leftx", 30000)}[st[1]]
            self.inst.send(f"vpad axis {axis} {value}")
            self.inst.expect("ok vpad", 30)
            self.inst.wait_frames(6)
            self.inst.send(f"vpad axis {axis} 0")
            self.inst.expect("ok vpad", 30)
            self.inst.wait_frames(10)
        elif kind == "shot":
            # ("shot", NAME): grab the current RDRAM frame and keep it as <out>/NNNN_NAME.png (evidence).
            self.shot()
            self.keep(st[1])
        elif kind == "pcap":
            # ("pcap", GAME_FRAMES, NAME): keep every composited (presented) frame for GAME_FRAMES game frames under <out>/NAME/ (evidence,
            # frames at --present-size; analysed offline, e.g. a flickering strip outside the picture).
            d = self.out / st[2]
            d.mkdir(parents=True, exist_ok=True)
            c0 = int(self.inst.cmd(f"pcap 1 {d}")["composited"])
            self.inst.wait_frames(st[1], timeout=120 * self.slow)
            c1 = int(self.inst.cmd("pcap 0")["composited"])
            time.sleep(1.0)   # the PNG writer threads
            self.notes.append(f"pcap {st[2]}: {c1 - c0} presented frames in {len(list(d.glob('*.png')))} files")
        elif kind == "ctl":
            # ("ctl", COMMAND): a control command whose reply is awaited (e.g. "dltrace": the next task's commands go to the host log).
            self.inst.cmd(st[1])
        elif kind == "send":
            # ("send", CMD): a raw control command, its reply is not awaited (debugging a hung game: the reply never comes).
            # Written straight to the pipe: Instance.send() polls the host first, and a debugger attached to it (lldb reparents the
            # process while attached) makes Popen.poll() report "exited (code 0)".
            self.inst.proc.stdin.write(st[1] + "\n")
            self.inst.proc.stdin.flush()
        elif kind == "hold":
            # ("hold", FILE): debugging aid, blocks until FILE exists (attach a debugger / sample the host meanwhile; touch FILE to go on).
            while not Path(st[1]).exists():
                time.sleep(1)
        elif kind == "wait":
            self.inst.wait_frames(st[1], timeout=120 * self.slow)
        elif kind == "dl":
            _, tasks, min_tris = st
            r = self.inst.cmd(f"dl {tasks}", timeout=60 * self.slow)
            self.notes.append(f"dl: tris max {r['tris_max']} avg {r['tris_avg']}, mtx max {r['mtx_max']}")
            if int(r["tris_max"]) < min_tris:
                raise StepFail(f"dl: only {r['tris_max']} triangles per task (want >= {min_tris})")
        elif kind == "audio":
            _, frames, min_rms = st
            self.inst.cmd("audio")
            self.inst.wait_frames(frames, timeout=120 * self.slow)
            r = self.inst.cmd("audio")
            rms = float(r["rms_dbfs"])
            self.notes.append(f"audio: {r['samples']} samples, RMS {rms:.1f} dBFS, peak {r['peak']}, "
                              f"non-silent buffers {r['nonsilent_buffers']}")
            if rms < min_rms or int(r["nonsilent_buffers"]) == 0:
                raise StepFail(f"audio: RMS {rms:.1f} dBFS < {min_rms} (or no non-silent buffers)")
        elif kind == "underruns":
            # ("underruns", FRAMES, MAX): the audio output may run dry at most MAX times over FRAMES game frames
            # (a pop each time it does: src/audio.cpp). Counts the real device, or the --mute virtual one.
            _, frames, max_un = st
            self.inst.cmd("audio")
            self.inst.wait_frames(frames, timeout=120 * self.slow)
            r = self.inst.cmd("audio")
            un, sf = int(r.get("underruns", 0)), int(r.get("starved_frames", 0))
            self.notes.append(f"audio: {un} underruns, {sf} starved frames over {frames} frames, RMS {float(r['rms_dbfs']):.1f} dBFS")
            if un > max_un:
                raise StepFail(f"audio: {un} underruns ({sf} starved frames) over {frames} frames (want <= {max_un})")
        elif kind == "declick":
            # ("declick", STALL_MS, N, MAX_STEP): N forced underruns (the next audio buffer queued STALL_MS late, src/audio.cpp
            # audiostall) while music plays; the largest sample step the device played at an underrun edge must stay <= MAX_STEP
            # (a hard cut to silence is a step of the whole sample value = a click). Needs a real SDL device (control_args without --mute).
            _, stall, n, max_step = st
            r = self.inst.cmd("audio")
            if r.get("device") != "1":
                self.warnings.append("declick: no SDL audio device (virtual output); not checked")
                return
            for _ in range(n):
                self.inst.cmd(f"audiostall {stall}")
                self.inst.wait_frames(20, timeout=60 * self.slow)
            r = self.inst.cmd("audio")
            un, step, raw = int(r["underruns"]), int(r["edge_step"]), int(r["edge_raw"])
            self.notes.append(f"declick: {un} underruns from {n} stalls of {stall} ms, edge step {step} (hard cut would be {raw}), RMS {float(r['rms_dbfs']):.1f} dBFS")
            if un < 1:
                raise StepFail(f"declick: {n} stalls of {stall} ms made no underrun")
            if step > max_step:
                raise StepFail(f"declick: sample step {step} at an underrun edge > {max_step} (hard cut {raw}): a click")
        elif kind == "declick_gap":
            # ("declick_gap", GAP_FRAMES, N, MAX_STEP): N short underruns (src/audio.cpp audiogap: GAP_FRAMES of one device
            # callback play as an underrun, then the output resumes within that callback, before the held frame's fade-out
            # has finished). The resume must blend from the partly faded held value: sample step <= MAX_STEP. Needs a real
            # SDL device.
            _, gap, n, max_step = st
            r = self.inst.cmd("audio")
            if r.get("device") != "1":
                self.warnings.append("declick_gap: no SDL audio device (virtual output); not checked")
                return
            for _ in range(n):
                self.inst.cmd(f"audiogap {gap}")
                self.inst.wait_frames(10, timeout=60 * self.slow)
            r = self.inst.cmd("audio")
            un, step, raw = int(r["underruns"]), int(r["edge_step"]), int(r["edge_raw"])
            self.notes.append(f"declick_gap: {un} underruns from {n} gaps of {gap} frames, edge step {step} (hard cut would be {raw}), "
                              f"RMS {float(r['rms_dbfs']):.1f} dBFS")
            if un < n:
                raise StepFail(f"declick_gap: {n} gaps of {gap} frames made only {un} underruns")
            if step > max_step:
                raise StepFail(f"declick_gap: sample step {step} at a short underrun's edge > {max_step} (hard cut {raw}): a click")
        elif kind == "fps":
            _, seconds, min_med = st
            t0 = time.monotonic()
            n0 = len(self.inst.perf)
            while time.monotonic() - t0 < seconds + 1.5:
                self.inst.wait_frames(15, timeout=60 * self.slow)
            samples = [f for _, f in self.inst.perf[n0 + 1:]]  # first sample straddles the window start
            if not samples:
                raise StepFail("fps: no perf samples")
            med = statistics.median(samples)
            load = H.load_info()
            self.notes.append(f"fps: median {med:.1f}, min {min(samples):.1f} over {len(samples)} s; load {load['load1']:.1f}")
            if med < min_med:
                msg = f"fps: median {med:.1f} < {min_med}"
                if load["overloaded"]:
                    self.warnings.append(msg + f" (load avg {load['load1']:.1f} > 2x{load['ncpu']} CPUs: warning only)")
                else:
                    raise StepFail(msg)
        elif kind == "pak_snapshot":
            self.pak_hash = _pak_hash(self.config)
            self.notes.append(f"pak before: {self.pak_hash[:12] if self.pak_hash else 'none'}")
        elif kind == "pak_changed":
            h = _pak_hash(self.config)
            self.notes.append(f"pak after: {h[:12] if h else 'none'}")
            if not h or h == self.pak_hash:
                raise StepFail(f"pak image unchanged ({h})")
        elif kind == "pak_keep":
            # ("pak_keep", NAME): copy the Controller Pak image to <out>/NAME.mpk (the run's out dir: a later session of the same run compares with it).
            src = self.config / "saves" / f"{BASE_VERSION}_pak1.mpk"
            shutil.copyfile(src, self.out.parent / f"{st[1]}.mpk")
            self.notes.append(f"pak kept as {st[1]}.mpk ({_pak_hash(self.config)[:12]})")
        elif kind == "pak_same":
            # ("pak_same", NAME): the Controller Pak image is byte-identical to the one an earlier session kept (e.g. a career saved with the cheats on
            # vs. the same career saved with none: the cheats never reach the save).
            ref = self.out.parent / f"{st[1]}.mpk"
            if not ref.exists():
                raise StepFail(f"no {ref.name}: run the session that keeps it first (same run)")
            a, b = ref.read_bytes(), (self.config / "saves" / f"{BASE_VERSION}_pak1.mpk").read_bytes()
            if a != b:
                diff = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
                raise StepFail(f"pak differs from {ref.name}: {len(diff)} bytes (first at {diff[0] if diff else min(len(a), len(b)):#x}), sizes {len(a)} / {len(b)}")
            self.notes.append(f"pak identical to {ref.name} ({len(a)} bytes)")
        elif kind == "pak_log":
            deadline = time.monotonic() + 10 * self.slow
            while not any(re.search(st[1], e) for e in self.inst.pak_events):
                if time.monotonic() > deadline:
                    raise StepFail(f"no host pak log line matching /{st[1]}/ (saw {self.inst.pak_events[-3:]})")
                time.sleep(0.2)
            self.notes.append("pak log: " + "; ".join(e.replace("[thps] ", "") for e in self.inst.pak_events
                                                       if "loaded" not in e and "new formatted" not in e))
        elif kind == "present_wide":
            # Newest present capture (rt64 patch, --present-dump-every) after this step started:
            # expected size, and few never-drawn (exactly black) pixels in the bands outside the
            # central 4:3 area, i.e. the 3D really fills the wide frame.
            _, size, max_black = st
            pdir = self.out / "present"
            t_start = time.monotonic()
            before = set(pdir.glob("present_*.png")) if pdir.exists() else set()
            new = []
            while not new:
                if time.monotonic() - t_start > 30 * self.slow:
                    raise StepFail(f"no new present capture in {pdir}")
                time.sleep(0.5)
                new = sorted(set(pdir.glob("present_*.png")) - before)
            time.sleep(0.5)  # let the writer thread finish the file
            path = new[-1]
            img = H.read_png(path)
            if f"{img.w}x{img.h}" != size:
                raise StepFail(f"{path.name}: {img.w}x{img.h}, expected {size}")
            core = img.h * 4 // 3
            x0 = (img.w - core) // 2
            mx, my = img.w // 40, img.h // 40   # the game's own 8 px frame margins
            black = total = 0
            for y in range(my, img.h - my, 2):
                for x in list(range(mx, x0, 2)) + list(range(x0 + core, img.w - mx, 2)):
                    total += 1
                    if img.px(x, y) == (0, 0, 0):
                        black += 1
            frac = black / max(total, 1)
            # Q18: with MSAA in widescreen, never-drawn columns at the right edge showed garbage
            # (magenta 255,0,255) until new render targets were cleared (rt64-hfr.patch).
            magenta = sum(1 for y in range(0, img.h, 4) for x in range(img.w - 16, img.w) if img.px(x, y) == (255, 0, 255))
            self.notes.append(f"present {path.name}: side bands {frac:.1%} unrendered, {magenta} magenta edge pixels")
            if magenta:
                raise StepFail(f"{path.name}: {magenta} magenta (uncleared) pixels at the right edge")
            if frac > max_black:
                raise StepFail(f"{path.name}: {frac:.1%} of the side bands is unrendered (max {max_black:.0%})")
        elif kind == "present_same":
            # ("present_same", NAME_A, NAME_B, MAX_FRAC): two kept present captures (keep_present) of a paused frame differ in at most MAX_FRAC
            # of their pixels (sum of |RGB| differences > 24): an A/B switch that must not change the picture (split screen at 4:3).
            _, na, nb, max_frac = st
            a, b = H.read_png(self.out / f"{na}.png"), H.read_png(self.out / f"{nb}.png")
            if (a.w, a.h) != (b.w, b.h):
                raise StepFail(f"{na}.png {a.w}x{a.h} vs {nb}.png {b.w}x{b.h}")
            n = d = 0
            for y in range(0, a.h, 2):
                for x in range(0, a.w, 2):
                    n += 1
                    if sum(abs(i - j) for i, j in zip(a.px(x, y), b.px(x, y))) > 24:
                        d += 1
            self.notes.append(f"{na} vs {nb}: {d / n:.3%} of pixels differ")
            if d / n > max_frac:
                raise StepFail(f"{na}.png and {nb}.png differ in {d / n:.2%} of pixels (max {max_frac:.2%})")
        elif kind == "present_split":
            # ("present_split", SIZE, MIN_FRAC): two-player split screen in widescreen. Newest present capture (after this step started): the
            # expected size, and in EACH side band outside the central 4:3 area (left and right separately) at least MIN_FRAC of the columns hold
            # rendered pixels (not exactly black in more than 5 % of the column's rows), i.e. the left view reaches the left edge and the right view
            # the right edge. Column-based, because a split view can have large dark areas (the Warehouse ceiling) that a pixel count would miss.
            _, size, min_frac = st
            pdir = self.out / "present"
            t_start = time.monotonic()
            before = set(pdir.glob("present_*.png")) if pdir.exists() else set()
            new = []
            while not new:
                if time.monotonic() - t_start > 30 * self.slow:
                    raise StepFail(f"no new present capture in {pdir}")
                time.sleep(0.5)
                new = sorted(set(pdir.glob("present_*.png")) - before)
            path = self._captures_until_now()[-1]   # the frame composited now, not an older one a lagging writer just finished
            img = H.read_png(path)
            if f"{img.w}x{img.h}" != size:
                raise StepFail(f"{path.name}: {img.w}x{img.h}, expected {size}")
            core = img.h * 4 // 3
            x0 = (img.w - core) // 2
            mx, my = img.w // 40, img.h // 40
            rows = range(my, img.h - my, 4)

            def drawn(x):
                lit = sum(1 for y in rows if img.px(x, y) != (0, 0, 0))
                return lit > 0.05 * len(rows)
            bands = {"left": range(mx, x0, 2), "right": range(x0 + core, img.w - mx, 2)}
            fracs = {k: sum(1 for x in xs if drawn(x)) / max(len(xs), 1) for k, xs in bands.items()}
            self.notes.append(f"present {path.name}: side-band columns drawn left {fracs['left']:.0%} right {fracs['right']:.0%}")
            if not bands["left"]:
                raise StepFail(f"{path.name}: no side bands (a 4:3 frame?)")
            for k, f in fracs.items():
                if f < min_frac:
                    raise StepFail(f"{path.name}: only {f:.0%} of the {k} side band's columns are drawn (min {min_frac:.0%})")
        elif kind == "present_bands_clear":
            # ("present_bands_clear", SIZE, MAX_FRAC): two-player split screen with split = original. Newest present capture (as present_split):
            # the expected size, and in EACH side band outside the central 4:3 area at most MAX_FRAC of the columns are not black (a channel
            # above 16 in at least two sampled rows). The bands are black as on a TV: no HUD (each player's HUD stays with its view in the 4:3
            # middle) and no clear colour (2026-10-25 play test: THPS2's sky blue filled the bands, and player 1's block and player 2's digits
            # and meter sat at the window's edges).
            _, size, max_frac = st
            path = self._captures_until_now()[-1]
            img = H.read_png(path)
            if f"{img.w}x{img.h}" != size:
                raise StepFail(f"{path.name}: {img.w}x{img.h}, expected {size}")
            core = img.h * 4 // 3
            x0 = (img.w - core) // 2
            mx, my = img.w // 80, img.h // 40
            rows = range(my, img.h - my, 2)

            def bright(x):
                return sum(1 for y in rows if max(img.px(x, y)) > 16) >= 2
            bands = {"left": range(mx, x0 - mx), "right": range(x0 + core + mx, img.w - mx)}
            if not bands["left"]:
                raise StepFail(f"{path.name}: no side bands (a 4:3 frame?)")
            fracs = {k: sum(1 for x in xs if bright(x)) / max(len(xs), 1) for k, xs in bands.items()}
            self.notes.append(f"present {path.name}: side-band columns not black left {fracs['left']:.1%} right {fracs['right']:.1%}")
            for k, f in fracs.items():
                if f > max_frac:
                    raise StepFail(f"{path.name}: {f:.1%} of the {k} side band's columns are not black (max {max_frac:.1%}): a HUD or the clear colour outside the views?")
        elif kind == "max_frame":
            # The checkpoint just matched was reached within N displayed frames of boot.
            frame = int(self.inst.stats()["frames"])
            self.notes.append(f"reached at displayed frame <= {frame} (max {st[1]})")
            if frame > st[1]:
                raise StepFail(f"took {frame} displayed frames (max {st[1]})")
        elif kind == "log":
            # A host line starting with "[thps] <prefix>" and matching regex was printed.
            deadline = time.monotonic() + 10 * self.slow
            while not any(re.search(st[1], e) for e in self.inst.other_lines):
                if time.monotonic() > deadline:
                    raise StepFail(f"no host log line matching /{st[1]}/ (saw {self.inst.other_lines[-3:]})")
                time.sleep(0.2)
            self.notes.append("log: " + next(e for e in self.inst.other_lines if re.search(st[1], e)).replace("[thps] ", ""))
        elif kind == "fps_overlay":
            # Show FPS: the counter's own lines agree with the perf log (game fps) within tol.
            _, seconds, tol = st
            t0 = time.monotonic()
            while time.monotonic() - t0 < seconds:
                self.inst.wait_frames(15, timeout=60 * self.slow)
            # The counter logs only changes, so what it shows now is its last line.
            perf = [f for t, f in self.inst.perf[-3:]]
            if not self.inst.fps_overlay or not perf:
                raise StepFail(f"fps overlay: {len(self.inst.fps_overlay)} counter lines, {len(perf)} perf samples")
            _, shown_fps, a = self.inst.fps_overlay[-1]
            b = statistics.median(perf)
            self.notes.append(f"fps overlay shows '{shown_fps:.0f} FPS | Game {a:.0f}'; perf fps (last 3 s) median {b:.1f}; "
                              f"{len(self.inst.fps_overlay)} counter changes")
            if abs(a - b) > tol:
                raise StepFail(f"fps overlay Game {a:.1f} vs perf {b:.1f} (tol {tol})")
        elif kind == "vpad":
            r = self.inst.send(f"vpad {st[1]}") or self.inst.expect("ok vpad", 30)
            self.inst.wait_frames(5)
        elif kind == "vpress2":
            # ("vpress2", NAME): the same on the SECOND virtual controller (--control vpad2).
            self.inst.send(f"vpad2 button {st[1]} 1")
            self.inst.expect("ok vpad2", 30)
            self.inst.wait_frames(4)
            self.inst.send(f"vpad2 button {st[1]} 0")
            self.inst.expect("ok vpad2", 30)
            self.inst.wait_frames(12)
        elif kind == "vpad2":
            self.inst.send(f"vpad2 {st[1]}")
            self.inst.expect("ok vpad2", 30)
            self.inst.wait_frames(5)
        elif kind == "log_n":
            # ("log_n", REGEX, N): at least N host "[thps] input|skip intro|..." lines matching REGEX have been printed (counts, so a
            # second "takes port 1" after a re-attach is not satisfied by the first).
            deadline = time.monotonic() + 10 * self.slow
            while sum(1 for e in self.inst.other_lines if re.search(st[1], e)) < st[2]:
                if time.monotonic() > deadline:
                    raise StepFail(f"fewer than {st[2]} host log lines matching /{st[1]}/ (saw {self.inst.other_lines[-4:]})")
                time.sleep(0.2)
        elif kind == "log_none":
            # ("log_none", REGEX): no such host line (yet).
            hit = [e for e in self.inst.other_lines if re.search(st[1], e)]
            if hit:
                raise StepFail(f"unexpected host log line /{st[1]}/: {hit[-1]}")
        elif kind == "vpress":
            # A virtual SDL controller button: down for 4 displayed frames, then up.
            self.inst.send(f"vpad button {st[1]} 1")
            self.inst.expect("ok vpad", 30)
            self.inst.wait_frames(4)
            self.inst.send(f"vpad button {st[1]} 0")
            self.inst.expect("ok vpad", 30)
            self.inst.wait_frames(12)
        elif kind == "ui":
            # ("ui", "esc"[, frames]): an in-game settings menu command (--control "ui ...": esc, pad NAME, key NAME, click #id,
            # tab N; THPS_MENU builds), then `frames` displayed frames (default 12) for the render thread to act on it.
            self.inst.send("ui " + st[1])
            self.inst.expect("ok ui", 30)
            self.inst.wait_frames(st[2] if len(st) > 2 else 12)
        elif kind == "ui_state":
            # ("ui_state", "open=1 tab=0 gfx.msaa=2"[, timeout s]): `ui state` until every key=value matches (the render thread acts a frame later;
            # gfx.KEY=VALUE is a token of the graphics settings line).
            want = dict(kv.split("=", 1) for kv in st[1].split())
            deadline = time.monotonic() + (st[2] if len(st) > 2 else 10) * self.slow
            while True:
                got = self.ui_state()
                if all((f"{k[4:]}={v}" in got["gfx"].split()) if k.startswith("gfx.") else got.get(k) == v for k, v in want.items()):   # gfx.KEY: a token of the graphics line
                    self.notes.append(f"ui state: {st[1]}")
                    break
                if time.monotonic() > deadline:
                    raise StepFail(f"ui state {st[1]}: got { {k: got.get(k) for k in want} }")
                time.sleep(0.25)
        elif kind == "ui_never":
            # ("ui_never", "hint=1", seconds): `ui state` never matches key=value in that many seconds (wall clock).
            k, v = st[1].split("=", 1)
            t_end = time.monotonic() + st[2]
            while time.monotonic() < t_end:
                if self.ui_state().get(k) == v:
                    raise StepFail(f"ui state {st[1]} (it must not)")
                time.sleep(0.5)
            self.notes.append(f"ui state never {st[1]} for {st[2]} s")
        elif kind == "ui_input":
            # ("ui_input", "mark"|"same"|"more"): the game's count of non-zero input polls ("ui state" input_nz): remember it, check it is
            # unchanged (the menu blocked the pad), or went up (the pad reaches the game).
            n = int(self.ui_state()["input_nz"])
            if st[1] == "mark":
                self.ui_input_mark = n
            elif st[1] == "same" and n != self.ui_input_mark:
                raise StepFail(f"game input polls went {self.ui_input_mark} -> {n} while the menu was open")
            elif st[1] == "more" and n <= self.ui_input_mark:
                raise StepFail(f"game input polls stayed at {n}: the pad does not reach the game")
            self.notes.append(f"input_nz {st[1]}: {self.ui_input_mark} -> {n}")
        elif kind == "ui_frame":
            # ("ui_frame", NAME, (x0, y0, x1, y1), MIN, MAX|None): RT64's composited frame (UI included, as the window shows it; --control
            # `edges`) saved as <out>/NAME.png; the bright (luma > 170) pixels in the region (fractions of the frame): MIN <= count <= MAX.
            _, name, reg, lo, hi = st
            edir = self.out / f"ui_{name}"
            shutil.rmtree(edir, ignore_errors=True)
            edir.mkdir(parents=True)
            self.inst.send(f"edges 2 {edir}")
            self.inst.expect("ok edges", 60 * self.slow)
            time.sleep(0.5)
            png = sorted(edir.glob("edges_*.png"))[-1]
            img = H.read_png(png)
            shutil.copyfile(png, self.out / f"{name}.png")
            x0, y0, x1, y1 = int(reg[0] * img.w), int(reg[1] * img.h), int(reg[2] * img.w), int(reg[3] * img.h)
            n = sum(1 for y in range(y0, y1) for x in range(x0, x1)
                    if 0.299 * img.px(x, y)[0] + 0.587 * img.px(x, y)[1] + 0.114 * img.px(x, y)[2] > 170)
            self.notes.append(f"{name}.png ({img.w}x{img.h}): {n} bright pixels in {reg}")
            if n < lo or (hi is not None and n > hi):
                raise StepFail(f"{name}: {n} bright pixels in {reg}, expected {lo}..{hi}")
        elif kind == "file_exists":
            if not (self.config / st[1]).exists():
                raise StepFail(f"{st[1]} does not exist in {self.config}")
            self.notes.append(f"{st[1]} exists")
        elif kind == "bind":
            self.inst.send(f"bind {st[1]} {st[2]}")
            line = self.inst.expect("ok bind", 30)
            if "saved=1" not in line:
                raise StepFail(f"bind not saved: {line}")
        elif kind == "file_contains":
            p = self.config / st[1]
            if not p.exists() or not re.search(st[2], p.read_text()):
                raise StepFail(f"{p.name} has no line matching /{st[2]}/")
            self.notes.append(f"{st[1]}: /{st[2]}/ found")
        elif kind == "present_cmp":
            # ("present_cmp", NAME_A, NAME_B, MIN_FRAC, MAX_FRAC[, "sides"][, "mask:NAME_C"]): two kept present captures (keep_present) of a
            # paused frame differ in MIN_FRAC .. MAX_FRAC of their pixels (sum of |RGB| differences > 24). "sides": only the bands outside the
            # central 4:3 area. "mask:NAME_C": skip the pixels where A and C differ, i.e. what moves on its own (animated textures keep
            # running in a paused frame); C is a second capture of A's state. A/B checks on one frame: a switch that must change nothing
            # (MAX 0), or must change something (MIN > 0).
            _, na, nb, lo, hi, *opt = st
            sides = "sides" in opt
            nc = next((o[5:] for o in opt if o.startswith("mask:")), None)
            a, b = H.read_png(self.out / f"{na}.png"), H.read_png(self.out / f"{nb}.png")
            c = H.read_png(self.out / f"{nc}.png") if nc else None
            for o, nm in ((b, nb), (c, nc)):
                if o is not None and (a.w, a.h) != (o.w, o.h):
                    raise StepFail(f"{na}.png {a.w}x{a.h} vs {nm}.png {o.w}x{o.h}")
            core = a.h * 4 // 3
            x0 = (a.w - core) // 2
            xs = [x for x in range(0, a.w, 2) if not sides or x < x0 or x >= x0 + core]
            n = d = masked = 0
            for y in range(0, a.h, 2):
                for x in xs:
                    pa = a.px(x, y)
                    if c is not None and sum(abs(i - j) for i, j in zip(pa, c.px(x, y))) > 24:
                        masked += 1
                        continue
                    n += 1
                    if sum(abs(i - j) for i, j in zip(pa, b.px(x, y))) > 24:
                        d += 1
            frac = d / max(n, 1)
            self.notes.append(f"{na} vs {nb}{' (side bands)' if sides else ''}: {frac:.3%} of pixels differ"
                              + (f" ({masked} moving pixels masked by {nc})" if c is not None else ""))
            if not lo <= frac <= hi:
                raise StepFail(f"{na}.png vs {nb}.png: {frac:.3%} of pixels differ (expected {lo:.3%} .. {hi:.3%})")
        elif kind == "keep_present":
            # ("keep_present", NAME[, "now"]): copy the newest present capture (session present_every) to <out>/<name>.png (evidence).
            # "now": the newest up to the frame composited now, once the writer threads have caught up (_captures_until_now). On a loaded
            # machine the newest file on disk can be seconds old (2026-10-19: a paused-frame A/B got the course-select screen from before
            # the level). Not the default: the wait can take seconds, and a step after it that looks at an animation (hud-special's
            # poked meter, which fades) would then see later frames.
            time.sleep(1.0)
            if len(st) > 2 and st[2] == "now":
                ps = self._captures_until_now()
            else:
                ps = sorted((self.out / "present").glob("present_*.png"))
            if not ps:
                raise StepFail("no present capture yet")
            shutil.copyfile(ps[-1], self.out / f"{st[1]}.png")
            self.notes.append(f"{st[1]}.png = {ps[-1].name}")
        elif kind == "loadrom":
            # ("loadrom", "rom"|"rev0", regex): the launcher's Load ROM with the suite's ROM, or with a
            # synthetic 1 MiB file whose header says the game (S.GAME["rev0_header"]; no game data);
            # the host log (message text) must match regex.
            _, which, rx = st
            if which == "rom":
                path = Path(self.rom)
            else:
                path = self.out / "synthetic_rev0.z64"
                hdr = bytearray(1 << 20)
                hdr[0:4] = bytes([0x80, 0x37, 0x12, 0x40])
                name, code = S.GAME.get("rev0_header", (b"TONY HAWK PRO SKATER", b"NTFE"))
                hdr[0x20:0x34] = name.ljust(20)
                hdr[0x3B:0x3F] = code
                path.write_bytes(bytes(hdr))
            self.inst.send(f"loadrom {path}")
            line = self.inst.expect("ok loadrom", 30 * self.slow)
            time.sleep(0.5)
            text = self.inst.log_path.read_text(errors="replace")
            if not re.search(rx, text):
                raise StepFail(f"loadrom {which}: {line}; host log has no /{rx}/")
            self.notes.append(f"loadrom {which}: {line}")
        elif kind == "json_value":
            _, name, key, want = st
            p = self.config / name
            got = json.loads(p.read_text()).get(key) if p.exists() else None
            self.notes.append(f"{name}: {key} = {got!r}")
            if got != want:
                raise StepFail(f"{name}: {key} = {got!r}, want {want!r}")
        elif kind == "binding_saved":
            # ("binding_saved", input, sdl_button_index, cfg_regex): the default build saves controls.cfg,
            # the frontend build recompinput's controls.json (single-player controller profile).
            _, inp, button, rx = st
            cfg_p, json_p = self.config / "controls.cfg", self.config / "controls.json"
            if json_p.exists():
                prof = next(p for p in json.loads(json_p.read_text())["profiles"] if p["key"].startswith("controller"))
                fields = prof["mappings"][inp.upper()]
                ok = any(f["input_type"] == 3 and f["input_id"] == button for f in fields)
                self.notes.append(f"controls.json {inp.upper()}: {fields}")
            else:
                ok = cfg_p.exists() and re.search(rx, cfg_p.read_text()) is not None
                self.notes.append(f"controls.cfg: /{rx}/ {'found' if ok else 'missing'}")
            if not ok:
                raise StepFail(f"binding for {inp} not saved")
        elif kind == "audio_max":
            _, frames, max_rms = st
            self.inst.cmd("audio")
            self.inst.wait_frames(frames, timeout=120 * self.slow)
            r = self.inst.cmd("audio")
            rms = float(r["rms_dbfs"])
            self.notes.append(f"audio: RMS {rms:.1f} dBFS (max {max_rms}), {r['samples']} samples")
            if rms > max_rms or int(r["samples"]) == 0:
                raise StepFail(f"audio: RMS {rms:.1f} dBFS > {max_rms} (or no samples)")
        elif kind == "crash":
            # --control crash: the host must die with SIGSEGV and leave crash.log naming game code. ("crash", REGEX): and matching REGEX.
            log = self.config / "crash.log"
            before = log.read_text() if log.exists() else ""
            self.inst.send("crash")
            try:
                self.inst.proc.wait(timeout=30 * self.slow)
            except subprocess.TimeoutExpired:
                raise StepFail("host still running 30 s after 'crash'")
            rc = self.inst.proc.returncode
            text = log.read_text()[len(before):] if log.exists() else ""
            m = re.search(r"innermost game function: (\S+) \((0x[0-9A-F]+)\)", text)
            self.notes.append(f"exit code {rc}; crash.log +{len(text)} bytes; innermost game function "
                              f"{m.group(1) + ' ' + m.group(2) if m else 'none'}")
            # POSIX: killed by SIGSEGV (128 + 11). Windows: the unhandled-exception filter ends the process with the exception code.
            if os.name == "nt":
                died = rc in (0xC0000005, 0xC0000005 - 2**32) and "EXCEPTION_ACCESS_VIOLATION" in text
            else:
                died = rc == 128 + 11 and "SIGSEGV" in text
            if not died or not m:
                raise StepFail(f"crash: exit {rc}, report: {text[:300]!r}")
            if len(st) > 1 and not re.search(st[1], text):   # ("crash", REGEX): the report also has this (e.g. the cheats that were on)
                raise StepFail(f"crash: the report has no match of /{st[1]}/: {text[:300]!r}")
            (self.out / "crash_report.txt").write_text(text)
        elif kind == "pad_records":
            # ("pad_records", INPUT, [records]): while INPUT is held, the game's PlayStation-style pad
            # layer (SControl = @gSControl, 20 SButton records of 16 bytes: 0 Triangle/grind,
            # 1 Square/flip, 2 Circle/grab, 3 X/ollie, 4-7 L1 L2 R1 R2, 8-11 D-pad, 14 Start,
            # 15 Select) shows exactly these records pressed. INPUT = "vpad:NAME" (virtual SDL
            # controller through the default bindings) or N64 buttons for a scripted press.
            _, inp, want = st
            if inp.startswith("vpad:"):
                self.inst.send(f"vpad button {inp[5:]} 1")
                self.inst.expect("ok vpad", 30)
                self.inst.wait_frames(4)
                held = self._pressed_records()
                self.inst.send(f"vpad button {inp[5:]} 0")
                self.inst.expect("ok vpad", 30)
            else:
                self.inst.send(f"press {inp} 8 4")
                self.inst.wait_frames(4)
                held = self._pressed_records()
                self.inst.expect("ok press", 60 * self.slow)
            self.inst.wait_frames(6)
            self.notes.append(f"{inp}: pad records {held}")
            if held != list(want):
                raise StepFail(f"{inp}: game pad records {held}, expected {list(want)}")
        elif kind == "skate":
            # ("skate", INPUTS, EXPECT): on the ground, do INPUTS, then watch the skater (CBruce via
            # the game's skater pointer @gSkater; Skater_PhysState: 1 = in air; THPS1 also has per-trick counters,
            # Skater_TrickCounters, bumped by Trick_Trick: GAME["trick_counters"]). EXPECT "air": airborne within 40
            # frames; "trick": a trick counter went up (THPS1 only). INPUTS: ("vpad", NAME, 0|1) | ("axis", NAME, V) |
            # ("wait", FRAMES). Up to GAME["skate_tries"] (4) tries (the skater keeps rolling: a ramp or a wall can cut a jump short).
            # GAME["skate_warn_only"] (THPS3): the air flag is not reliable (LOG P3), a miss only warns.
            _, inputs, expect = st
            tc = GAME.get("trick_counters", True)
            sk = self._peek("@gSkater")[0]
            f_ctl, f_phys = (self._sym(n) for n in ("Skater_ControlPtr", "Skater_PhysState"))
            f_trick = self._sym("Skater_TrickCounters") if tc else 0
            if not 0x80000000 <= sk < 0x80800000 or self._peek(sk + f_ctl)[0] != self._sym("gSControl"):
                raise StepFail(f"no skater object (gSkater = {sk:#x})")
            tries = []
            for attempt in range(GAME.get("skate_tries", 4)):
                for _ in range(40):   # wait for the ground (state 0) twice in a row
                    if self._peek(sk + f_phys)[0] == 0:
                        self.inst.wait_frames(2)
                        if self._peek(sk + f_phys)[0] == 0:
                            break
                    self.inst.wait_frames(4)
                c0 = self._peek(sk + f_trick, 80) if tc else [0] * 80
                states = []
                for kind2, a, *b in inputs:
                    if kind2 == "wait":
                        self.inst.wait_frames(a)
                        states.append(self._peek(sk + f_phys)[0])
                    else:
                        self.inst.send(f"vpad {'button' if kind2 == 'vpad' else 'axis'} {a} {b[0]}")
                        self.inst.expect("ok vpad", 30)
                for _ in range(GAME.get("skate_watch", 20)):   # polls of 2 frames after the inputs
                    self.inst.wait_frames(2)
                    states.append(self._peek(sk + f_phys)[0])
                c1 = self._peek(sk + f_trick, 80) if tc else [0] * 80
                tricks = sum(max(0, b - a) for a, b in zip(c0, c1))   # counters that went up (others decay on landing)
                changed = {i: (c0[i], c1[i]) for i in range(80) if c0[i] != c1[i]}
                ok = (1 in states) if expect == "air" else tricks > 0
                tries.append(f"states {''.join(map(str, states))} tricks +{tricks}" + (f" changed {changed}" if changed else ""))
                self.inst.wait_frames(20)
                if ok:
                    break
            self.notes.append(f"skate {expect}: " + " | ".join(tries))
            if not ok:
                self.shot()   # the last frame goes into the failure sheet
                msg = f"skate: no {expect} in {len(tries)} tries ({' | '.join(tries)})"
                if GAME.get("skate_warn_only"):   # THPS3: the air flag is not reliable (LOG P3): a miss only warns
                    self.warnings.append(msg)
                else:
                    raise StepFail(msg)
        elif kind == "edges":
            # ("edges", N, MAX[, "rows"]): RT64's final image (the present capture, as the window shows it) of the
            # next N composited frames; on a still screen the border bands (outer 1/24 of the width) must not change:
            # worst single-column mean |frame-to-frame difference| <= MAX (luma). With "rows" the top and bottom bands
            # (outer 1/24 of the height) are held to the same limit (for screens whose picture is framed by black bars).
            _, n, mx, *opt = st
            edir = self.out / f"edges_{self.k:04d}"
            edir.mkdir(parents=True, exist_ok=True)
            self.inst.send(f"edges {n} {edir}")
            line = self.inst.expect("ok edges", 120 * self.slow)
            r = dict(kv.split("=", 1) for kv in line.split()[2:] if "=" in kv)
            self.notes.append("edges: " + line[len("ok edges "):])
            if float(r["col_max"]) > mx:
                raise StepFail(f"border band unstable: column {r['col_x']} changes by {r['col_max']} per frame (max {mx}); {line}")
            if "rows" in opt and float(r["row_max"]) > mx:
                raise StepFail(f"top/bottom band unstable: row {r['row_y']} changes by {r['row_max']} per frame (max {mx}); {line}")
        elif kind == "not_cp":
            img = self.last_img or self.shot()[0]
            if self.have_ref(st[1]) and self.match(img, st[1])[0]:
                raise StepFail(f"frame unexpectedly matches '{st[1]}' ({S.CHECKPOINTS[st[1]]['what']})")
        elif kind in hfr_steps.KINDS:   # high frame rate / input latency (Q18)
            hfr_steps.step(self, st)
        else:
            raise ValueError(f"unknown step {st}")


_ref_cache: dict[str, H.Image] = {}


def _pak_hash(config: Path) -> str | None:
    p = config / "saves" / f"{BASE_VERSION}_pak1.mpk"
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None


def check_refs() -> int:
    """For each checkpoint: the closest *other* references (distance/tol, must be > 1)."""
    refs = {}
    for d in reversed(ref_chain()):   # own references win over the ones they fall through to
        refs.update({p.stem: H.read_png(p) for p in d.glob("*.png") if p.stem != "sheet"})
    bad = 0
    for cp, c in S.CHECKPOINTS.items():
        if cp not in refs:
            print(f"{cp:16s} (no reference)")
            continue
        near = sorted((max(H.region_ratio(img, refs[cp], spec) for spec in c["regions"]), name)
                      for name, img in refs.items() if name != cp and f"{img.w}x{img.h}" == c["size"])
        bad += sum(1 for r, _ in near if r <= 1.2)
        print(f"{cp:16s} " + "  ".join(f"{n}:{r:.2f}" for r, n in near[:3]))
    print("OK" if not bad else f"{bad} reference pair(s) too close (<= 1.2)")
    return 1 if bad else 0


def frontend_build() -> bool:
    r = subprocess.run([str(H.BINARY), "--features"], capture_output=True, text=True, timeout=30)
    return "frontend=1" in r.stdout


def menu_build() -> bool:
    """The binary has the in-game settings menu (THPS_MENU, on by default): --features says menu=1."""
    r = subprocess.run([str(H.BINARY), "--features"], capture_output=True, text=True, timeout=30)
    return "menu=1" in r.stdout


def lock_file(f) -> None:
    """Exclusive non-blocking lock (raises OSError when held): flock on POSIX, msvcrt.locking on Windows."""
    if os.name == "nt":
        import msvcrt
        f.write(" ")
        f.flush()
        f.seek(0)
        msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        import fcntl
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)


def other_instances() -> list[str]:
    if os.name == "nt":
        prod = H.GAME_PROFILE.get("product", "") + ".exe"          # a packaged Windows engine is named after the product
        out = []
        for name in (H.EXE_NAME, prod):
            r = subprocess.run(["tasklist", "/FO", "CSV", "/NH", "/FI", f"IMAGENAME eq {name}"], capture_output=True, text=True)
            out += [l for l in r.stdout.splitlines() if name.lower() in l.lower()]
        return out
    r = subprocess.run(["pgrep", "-fl", H.FAMILY + "_recomp"], capture_output=True, text=True)
    # Only real instances: the program being <game>_recomp. A `lockf FILE <game>_recomp ...` or `sh -c '... <game>_recomp ...'` that is itself
    # waiting for the machine-wide game lock mentions the name too, and would make this run wait for a process that waits for this one.
    return [l for l in r.stdout.splitlines() if len(l.split()) > 1 and os.path.basename(l.split()[1]).startswith(H.FAMILY + "_recomp")
            and str(os.getpid()) not in l.split()[0]]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", default=BASE_VERSION, help="ROM version under test: a games.toml row of the game's family (default: the family root)")
    ap.add_argument("--rom", default=None, help="ROM path (default: $THPS<N>_ROM or roms/ up the tree)")
    ap.add_argument("--record", action="store_true", help="capture missing reference frames")
    ap.add_argument("--rerecord", action="store_true", help="delete all of this version's own references first (implies --record)")
    ap.add_argument("--rerecord-cp", default="", help="comma list of checkpoints: delete this version's own reference for them first (implies --record)")
    ap.add_argument("--only", default="", help="comma list of scenarios (their session prefix still runs)")
    ap.add_argument("--session", default="", help="comma list of sessions")
    ap.add_argument("--out", default="")
    ap.add_argument("--wait-others", type=float, default=600, help="seconds to wait for other <game>_recomp instances")
    ap.add_argument("--slow", type=float, default=0, help="timeout multiplier (default: 1, or 3 when overloaded)")
    ap.add_argument("--check-refs", action="store_true", help="only cross-check the recorded references")
    ap.add_argument("--rerun-failed", action="store_true", help="only the scenarios that failed in the last run")
    ap.add_argument("--keep", type=int, default=10, help="keep the newest N output dirs (default 10)")
    args = ap.parse_args()
    use_version(args.version)
    if args.rom is None:
        args.rom = default_rom()
    if args.check_refs:
        return check_refs()

    if not H.BINARY.exists():
        print(f"error: {H.BINARY} not built", file=sys.stderr)
        return 2
    BUILD0.mkdir(parents=True, exist_ok=True)
    lock = open(BUILD0 / ".lock", "w")
    try:
        lock_file(lock)
    except OSError:
        print("error: another test run holds build/tests/.lock", file=sys.stderr)
        return 2
    others = other_instances()
    deadline = time.monotonic() + args.wait_others
    if others:
        print("waiting for other " + H.FAMILY + "_recomp instance(s) to exit (one at a time):\n  " + "\n  ".join(o[:160] for o in others))
    while others and time.monotonic() < deadline:
        time.sleep(5)
        others = other_instances()
    if others:
        print("error: " + H.FAMILY + "_recomp still running (one instance at a time)", file=sys.stderr)
        return 2
    if args.rerecord and REFS.exists():
        shutil.rmtree(REFS)
    for cp in filter(None, args.rerecord_cp.split(",")):
        RERECORD.add(cp)
        if cp not in S.CHECKPOINTS:
            print(f"error: --rerecord-cp {cp}: no such checkpoint", file=sys.stderr)
            return 2
        (REFS / f"{cp}.png").unlink(missing_ok=True)
    record = args.record or args.rerecord or bool(args.rerecord_cp)

    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = Path(args.out) if args.out else BUILD0 / "out" / stamp
    out.mkdir(parents=True, exist_ok=True)
    load = H.load_info()
    slow = args.slow or (3.0 if load["overloaded"] else 1.0)
    print(f"uptime: {load['uptime']}")
    print(f"out: {out}  record={record}  timeout x{slow}")
    only = set(filter(None, args.only.split(",")))
    if args.rerun_failed:
        last = sorted((BUILD0 / "out").glob("*/results.json"))
        last = [p for p in last if p.parent != out]
        if not last:
            print("error: no previous results.json", file=sys.stderr)
            return 2
        prev = json.loads(last[-1].read_text())
        only = {r["name"] for r in prev["results"] if r["status"] in ("fail", "error", "skipped")}
        print(f"rerunning {sorted(only) or 'nothing'} from {last[-1]}")
        if not only:
            return 0
    sessions = set(filter(None, args.session.split(",")))

    results = []
    suite_t0 = time.monotonic()
    failed_sessions: set[str] = set()
    for sess in [*S.SESSIONS, *getattr(S, "DEBUG_SESSIONS", [])]:
        names = [n for n, _, _ in sess["scenarios"]]
        if sessions and sess["name"] not in sessions:
            continue
        if sess.get("optin") and not sessions and not (only & set(names)):   # exploration sessions: only when named (--session / --only)
            continue
        if sess in getattr(S, "DEBUG_SESSIONS", []) and sess["name"] not in sessions:
            continue   # evidence/debug sessions run only when named
        if only and not (only & set(names)):
            continue
        if sess.get("requires") == "frontend" and not frontend_build():
            for n, what, _ in sess["scenarios"]:
                results.append(dict(name=n, session=sess["name"], what=what, status="skipped", seconds=0,
                                    message="needs the frontend build (THPS<N>_BINARY=build/cmake-ui/<game>_recomp)"))
            continue
        if sess.get("requires") == "menu" and not menu_build():
            for n, what, _ in sess["scenarios"]:
                results.append(dict(name=n, session=sess["name"], what=what, status="skipped", seconds=0,
                                    message="needs the in-game settings menu (build without -DTHPS_MENU=OFF)"))
            continue
        needs = sess.get("needs", [])
        if any(n in failed_sessions for n in needs):
            for n, what, _ in sess["scenarios"]:
                results.append(dict(name=n, session=sess["name"], what=what, status="skipped", seconds=0,
                                    message=f"{', '.join(needs)} session failed"))
            continue
        config = BUILD / sess["config"]
        if sess["fresh_pak"]:
            shutil.rmtree(config / "saves", ignore_errors=True)
        for name in sess.get("fresh_files", []):   # settings files the session must create itself
            if name == f"{thpsgame.load(BASE_VERSION).game_id}.z64":     # the stored ROM copy: <game_id>.z64 of the version under test
                name = f"{VERSION.game_id}.z64"
            (config / name).unlink(missing_ok=True)
        if "ui-hint-shown" not in sess.get("fresh_files", []):
            # The first launch on a config folder shows the "Press Esc for Settings" toast over the game's picture for ~3 s, from a wall clock
            # of its own (host ui_host.cpp). The present-capture checks (edges, present_wide) would see it appear and vanish as a change
            # at a random point of a run (macOS e169_menus 2026-10-14: column 43 = the toast, 4.38 vs 4.0). Sessions that test the toast list
            # ui-hint-shown in fresh_files and get it; every other one starts as a second launch, which shows none.
            config.mkdir(parents=True, exist_ok=True)
            (config / "ui-hint-shown").write_text("1\n")
        sdir = out / sess["name"]
        sdir.mkdir(parents=True, exist_ok=True)
        extra = ["--wav", str(sdir / "audio.wav")] if sess.get("wav") else []
        extra += sess.get("args", [])
        extra += os.environ.get("THPS_TEST_EXTRA_ARGS", "").split()   # debugging: e.g. --verbose for every session
        if sess.get("present_every"):
            extra += ["--present-dump-every", str(sess["present_every"]), "--dump-dir", str(sdir / "present")]
        rom = args.rom or None  # the host keeps a copy in the config dir after the first run
        if sess.get("no_rom"):  # launcher test: start without a ROM (fresh_files removes the stored copy)
            rom = None
        t0 = time.monotonic()
        inst = None
        try:
            inst = H.Instance(config, sdir / "host.log", rom=rom, extra=extra, control_args=sess.get("control_args"))
            inst.note(f"uptime: {load['uptime']}")
        except Exception as e:  # noqa: BLE001
            for n, what, _ in sess["scenarios"]:
                results.append(dict(name=n, session=sess["name"], what=what, status="error", seconds=0, message=str(e)))
            failed_sessions.add(sess["name"])
            continue
        print(f"[{sess['name']}] host started ({time.monotonic() - t0:.1f}s)")
        ses = Session(inst, sdir, record, slow, config)
        ses.rom = args.rom
        ses.present_every = int(sess.get("present_every") or 0)
        broken = False
        last_selected = max((i for i, n in enumerate(names) if not only or n in only), default=len(names) - 1)
        for i, (name, what, steps) in enumerate(sess["scenarios"]):
            if i > last_selected:
                break
            r = dict(name=name, session=sess["name"], what=what)
            if broken:
                r.update(status="skipped", seconds=0, message="earlier scenario in session failed")
                results.append(r)
                continue
            ses.notes, ses.warnings, ses.dists = [], [], {}
            ts = time.monotonic()
            f0 = int(inst.stats()["frames"])
            try:
                for st in steps:
                    ses.step(st)
                r["status"] = "warn" if ses.warnings else "pass"
                r["message"] = "; ".join(ses.warnings)
            except (StepFail, H.HostError) as e:
                r["status"], r["message"] = "fail", str(e)
                broken = True
            except Exception as e:  # noqa: BLE001
                r["status"], r["message"] = "error", f"{type(e).__name__}: {e}"
                (sdir / f"{name}.traceback.txt").write_text(traceback.format_exc())
                broken = True
            r["seconds"] = round(time.monotonic() - ts, 1)
            try:
                r["frames"] = int(inst.stats()["frames"]) - f0
            except Exception:  # noqa: BLE001
                pass
            r["notes"], r["checkpoint_dist"] = ses.notes, ses.dists
            results.append(r)
            print(f"[{sess['name']}] {name:16s} {r['status'].upper():5s} {r['seconds']:6.1f}s  {r.get('message', '')}")
            for n in ses.notes:
                print(f"      {n}")
        perf = list(inst.perf)
        inst.quit()
        if broken:
            failed_sessions.add(sess["name"])
            polls = sorted(sdir.glob("poll_*.png"), key=lambda p: p.stat().st_mtime)
            if polls:
                H.contact_sheet(polls, sdir / "failure_sheet.png")
        else:
            for p in sdir.glob("poll_*.png"):
                p.unlink()
        if sess.get("wav") and (sdir / "audio.wav").exists() and not broken:
            # Cross-check with the WAV: the last 8 s were in-level (audio + fps scenarios).
            import wave
            with wave.open(str(sdir / "audio.wav")) as w:
                dur = w.getnframes() / w.getframerate()
            rms, loud = H.wav_rms_dbfs(sdir / "audio.wav", max(0.0, dur - 8), dur)
            for r in results:
                if r["name"] == "level_audio":
                    r.setdefault("notes", []).append(f"wav: last 8 s of {dur:.0f} s: RMS {rms:.1f} dBFS, {loud:.0%} of 50 ms windows > -50 dBFS")
                    print(f"      wav: last 8 s of {dur:.0f} s: RMS {rms:.1f} dBFS, {loud:.0%} windows > -50 dBFS")
                    if rms < -40 and r["status"] == "pass":
                        r["status"], r["message"] = "fail", f"WAV RMS {rms:.1f} dBFS"
        print(f"[{sess['name']}] session {time.monotonic() - t0:.1f}s, perf samples {len(perf)}")

    if record and REFS.exists():
        refs = sorted(REFS.glob("*.png"))
        refs = [p for p in refs if p.name != "sheet.png"]
        if refs:
            H.contact_sheet(refs, REFS / "sheet.png")
            (REFS / "sheet.txt").write_text("\n".join(f"{i}: {p.stem}" for i, p in enumerate(refs)) + "\n")
            print(f"references: {REFS / 'sheet.png'} (order in sheet.txt) -- check them by eye")

    total = round(time.monotonic() - suite_t0, 1)
    load_end = H.load_info()
    summary = dict(started=stamp, version=VERSION.name, seconds=total, uptime_start=load["uptime"], uptime_end=load_end["uptime"],
                   binary=str(H.BINARY), record=record, results=results,
                   counts={s: sum(1 for r in results if r["status"] == s) for s in ("pass", "warn", "fail", "error", "skipped")})
    (out / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    print()
    print(f"{'scenario':18s} {'session':11s} {'status':7s} {'time':>7s}  detail")
    for r in results:
        print(f"{r['name']:18s} {r['session']:11s} {r['status']:7s} {r['seconds']:6.1f}s  {r.get('message', '')[:90]}")
    print(f"total {total:.1f}s; {summary['counts']}; uptime at end: {load_end['uptime']}")
    print(f"results: {out / 'results.json'}")
    outs = sorted(p for p in (BUILD0 / "out").iterdir() if p.is_dir()) if (BUILD0 / "out").is_dir() else []   # (--out elsewhere: no build/tests/out yet)
    for old_out in outs[:-args.keep] if args.keep > 0 else []:
        shutil.rmtree(old_out, ignore_errors=True)
    bad = any(r["status"] in ("fail", "error") for r in results)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
