#!/usr/bin/env python3
"""Soak test for a THPS recomp host (stdlib only; shared by THPS1-3: run it through decomp/thps<N>/recomp-host/tools/test.sh --soak [MINUTES]).

    tests/soak.py [--minutes M] [--version ID] [--out DIR] [--launch-minutes N] [--window-s S] [--seed N] [--static]
    tests/soak.py --summary soak.jsonl [more.jsonl ...]       print the table of one or more finished (or running) soak logs

Answers "how often does it crash, hang or hitch in real play?", which a gate rerun cannot. It plays the game through the --control protocol (the
same runner the regression suite uses: tests/run.py's Session, virtual pad, checkpoints), in the normal user configuration: a visible window, the
defaults (60 Hz interpolation, IDs on, HUD default), real audio output at volume 0, no present capture, no display-list tracing. Per launch it loops:

  - career runs and FREE SKATE runs on the levels the career has open, with tricks (stick, ollie, flip, grab, grind) from a virtual pad;
  - pause -> RETRY, pause -> END RUN, a run that times out, QUIT / LEVEL SELECT from the end-of-run menu;
  - menu cycling: OPTIONS and its screens, EXTRAS, the in-game settings menu (open, every tab, close);
and relaunches every --launch-minutes, alternating warm (same app, same folder) and cold (a fresh copy of the app at a new path, the library the
first run built reused). Every launch records the time from process start to the first game frame.

Everything is appended as JSON lines to <out>/soak.jsonl (events: start, first_run_build, launch, phase, window, hitch, shaders, mem, crash, hang,
navfail, activity, end). `--summary` prints the table. Crashes keep the signal and the crash.log stack; a hang (no presented frame for more than
5 s while the game should be running) keeps a `sample` of the process (macOS) and is killed; both are recorded, the host relaunched, the run goes on.
A script desync (a checkpoint never reached, i.e. a bug of this test or of the game's menus, the host still alive) is a `navfail`, not a crash.

What is measured (and how, without patching RT64):
  - frame times: --control `ftime` windows (presented = what the window got, composited = RT64's output, game = gfx tasks), rolled every
    --window-s seconds and at each phase change, so every window belongs to one phase / level; a 0.5 ms histogram per window lets the
    summary add them up to exact percentiles and counts over 20 / 50 / 100 ms per level;
  - shader-compile hitches: RT64 compiles its specialised shaders on background threads (idle priority) and draws with the ubershader meanwhile,
    so a new effect should not stall; RT64 exposes no timing. `ftime shaders` (a read-only counter dump) shows when a shader is first asked for
    (submitted) and when its pipeline is ready (compiled). The summary counts the 50 ms+ presented intervals and the one-second `send_dl`
    spikes (host `[thps] perf` line) and sorts each by what happened with them: a new shader submitted, a compile backlog, a phase start
    (level load), nothing;
  - RSS (and on macOS the physical footprint) of the host process, sampled every 30 s and at each window; machine load average with every sample.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import traceback
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
for _d in (HERE, HERE.parents[1]):
    if str(_d) not in sys.path:
        sys.path.insert(0, str(_d))


# ---------------------------------------------------------------------------------------
# Summary (no game needed: reads the JSON lines)

def hist_pct(h: dict[int, int], p: float) -> float:
    """Percentile (ms) of a histogram of 0.5 ms bins (bin = interval in half milliseconds)."""
    total = sum(h.values())
    if not total:
        return 0.0
    target, cum = p * total, 0
    for b in sorted(h):
        cum += h[b]
        if cum >= target:
            return (b + 0.5) / 2
    return (max(h) + 0.5) / 2


def hist_over(h: dict[int, int], ms: float) -> int:
    return sum(n for b, n in h.items() if b / 2 > ms)


def fmt_dur(s: float) -> str:
    return f"{int(s // 3600)}h{int(s % 3600 // 60):02d}m" if s >= 3600 else f"{int(s // 60)}m{int(s % 60):02d}s"


def load_events(paths: list[str]) -> list[dict]:
    ev = []
    for p in paths:
        for line in Path(p).read_text(errors="replace").splitlines():
            try:
                r = json.loads(line)
            except ValueError:
                continue   # a line cut off by a kill
            ev.append(r)
    return ev


def summarize(paths: list[str]) -> str:
    ev = load_events(paths)
    if not ev:
        return "no events"
    out: list[str] = []
    starts = [e for e in ev if e["ev"] == "start"]
    ends = [e for e in ev if e["ev"] == "end"]
    windows = [e for e in ev if e["ev"] == "window"]
    launches = [e for e in ev if e["ev"] == "launch"]
    crashes = [e for e in ev if e["ev"] == "crash"]
    hangs = [e for e in ev if e["ev"] == "hang"]
    navfails = [e for e in ev if e["ev"] == "navfail"]
    hitches = [e for e in ev if e["ev"] == "hitch"]
    mems = [e for e in ev if e["ev"] == "mem"]
    played = sum(w.get("secs", 0) for w in windows)
    hidden = [w for w in windows if w.get("vis") is not None and w["vis"] < 0.5]
    for s in starts:
        out.append(f"soak: {s.get('game')} {s.get('version')}  {s.get('platform')}  engine {s.get('engine')}  head {s.get('head')}  started {s.get('wall_date')}")
        out.append(f"      machine: {s.get('machine')}; load at start {s.get('load')}")
    wall = sum(e.get("seconds", 0) for e in ends) or (max(e["t"] for e in ev) if ev else 0)
    out.append(f"ran {fmt_dur(wall)} wall, {fmt_dur(played)} of measured frames in {len(windows)} windows, {len(launches)} launches"
               f" ({sum(1 for l in launches if l.get('kind') == 'cold')} cold, {sum(1 for l in launches if l.get('kind') == 'warm')} warm,"
               f" {sum(1 for l in launches if l.get('kind') == 'first')} first)")
    out.append(f"window on screen (presenting) in {len(windows) - len(hidden)} of {len(windows)} windows"
               + (f"; {sum(w['secs'] for w in hidden):.0f} s were OCCLUDED (presents skipped: the numbers below include them, so they are NOT a visible-window measurement)" if hidden else ""))
    loads = [w["load"][0] for w in windows if w.get("load")]
    if loads:
        out.append(f"machine load (1 min avg) over the run: median {sorted(loads)[len(loads) // 2]:.1f}, max {max(loads):.1f}")
    out.append("")
    out.append(f"CRASHES: {len(crashes)}   HANGS: {len(hangs)}   script desyncs (navfail): {len(navfails)}")
    for c in crashes:
        out.append(f"  crash at {fmt_dur(c['t'])} in {c.get('phase')} ({c.get('level')}), launch {c.get('launch')}, signal {c.get('signal')} exit {c.get('exit')}")
        for l in (c.get("stack") or [])[:14]:
            out.append("      " + l)
    for h in hangs:
        out.append(f"  hang at {fmt_dur(h['t'])} in {h.get('phase')} ({h.get('level')}), launch {h.get('launch')}: {h.get('kind')}, {h.get('gap_s')} s without a presented frame; sample: {h.get('sample')}")
    for n in navfails[:12]:
        out.append(f"  navfail at {fmt_dur(n['t'])} in {n.get('phase')}: {str(n.get('msg'))[:160]}")
    if len(navfails) > 12:
        out.append(f"  ... {len(navfails) - 12} more")
    out.append("")

    # Frame times per level / phase kind
    groups: dict[str, dict] = defaultdict(lambda: {"secs": 0.0, "pres": Counter(), "game": Counter(), "n": 0, "max": 0.0, "o20": 0, "o50": 0, "o100": 0, "nohist": 0})
    for w in windows:
        key = w.get("level") or w.get("phase") or "?"
        g = groups[key]
        g["secs"] += w.get("secs", 0)
        g["n"] += 1
        p = w.get("pres", {})
        if "hist" in w:
            g["pres"].update({int(b): n for b, n in w["hist"].items()})
        else:
            g["nohist"] += 1
        g["max"] = max(g["max"], p.get("max", 0))
        g["o20"] += p.get("o20", 0)
        g["o50"] += p.get("o50", 0)
        g["o100"] += p.get("o100", 0)
    if groups:
        out.append("FRAME TIMES (presented intervals; ms)")
        out.append(f"  {'where':26s} {'time':>8s} {'fps':>6s} {'p50':>6s} {'p95':>6s} {'p99':>7s} {'max':>7s} {'>20ms':>7s} {'>50ms':>6s} {'>100ms':>6s}")
        for key, g in sorted(groups.items(), key=lambda kv: -kv[1]["secs"]):
            n = sum(g["pres"].values())
            fps = n / g["secs"] if g["secs"] else 0
            out.append(f"  {key[:26]:26s} {fmt_dur(g['secs']):>8s} {fps:6.1f} {hist_pct(g['pres'], .5):6.1f} {hist_pct(g['pres'], .95):6.1f} {hist_pct(g['pres'], .99):7.1f} "
                       f"{g['max']:7.1f} {g['o20']:7d} {g['o50']:6d} {g['o100']:6d}")
        allp: Counter = Counter()
        for g in groups.values():
            allp.update(g["pres"])
        tot = sum(allp.values())
        if tot:
            out.append(f"  {'ALL':26s} {fmt_dur(played):>8s} {tot / played if played else 0:6.1f} {hist_pct(allp, .5):6.1f} {hist_pct(allp, .95):6.1f} {hist_pct(allp, .99):7.1f} "
                       f"{max(g['max'] for g in groups.values()):7.1f} {sum(g['o20'] for g in groups.values()):7d} {sum(g['o50'] for g in groups.values()):6d} {sum(g['o100'] for g in groups.values()):6d}")
        out.append("")

    # Hitches: windows with intervals over 50 ms, and the per-second send_dl spikes, sorted by what was going on
    def kind_of(w: dict) -> str:
        sh = w.get("shaders") or {}
        if sh.get("new", 0) > 0:
            return "A: a new shader was submitted in the window"
        if w.get("age", 99) < 15:
            return "B: within 15 s of a phase start (level load), no new shader"
        if sh.get("backlog_end", 0) > 0 or sh.get("backlog_start", 0) > 0:
            return "C: later, shaders still compiling in the background"
        return "D: steady state, nothing new, nothing compiling"
    big = [w for w in windows if w.get("pres", {}).get("o50", 0) > 0]
    n50 = sum(w["pres"]["o50"] for w in big)
    n100 = sum(w["pres"].get("o100", 0) for w in big)
    out.append(f"HITCHES: {n50} presented intervals over 50 ms ({n100} over 100 ms) in {len(big)} windows of {len(windows)}; "
               f"{hours(played, n50)} per hour of play")
    by_kind: dict[str, list] = defaultdict(list)
    for w in big:
        by_kind[kind_of(w)].append(w)
    for k, ws in sorted(by_kind.items()):
        out.append(f"  {k:62s}: {sum(w['pres']['o50'] for w in ws):4d} intervals >50 ms in {len(ws)} windows, worst {max(w['pres']['max'] for w in ws):.0f} ms")
    if big:
        worst = sorted(big, key=lambda w: -w["pres"]["max"])[:8]
        out.append("  worst windows (at, phase/level, age in phase, longest interval, new shaders):")
        for w in worst:
            out.append(f"    {fmt_dur(w['t']):>8s}  {w.get('phase')}/{w.get('level')}  age {w.get('age')} s  {w['pres']['max']:.0f} ms  new shaders {(w.get('shaders') or {}).get('new', '-')}")
    sd = [h for h in hitches if h.get("send_dl_ms", 0) >= 20]
    if hitches:
        near = sum(1 for h in sd if h.get("new_shaders"))
        out.append(f"  one-second perf lines with send_dl max >= 20 ms: {len(sd)} (of which {near} with a new shader submitted within 2 s); "
                   f">= 50 ms: {sum(1 for h in sd if h['send_dl_ms'] >= 50)}; worst {max((h['send_dl_ms'] for h in sd), default=0):.0f} ms")
    sh_lines = [e for e in ev if e["ev"] == "shaders"]
    new_ev = [e for e in sh_lines if e.get("new", 0) > 0 and e.get("age", 99) is not None]
    if new_ev:
        def window_at(e: dict) -> dict | None:
            for w in windows:
                if w.get("launch") == e.get("launch") and w["t"] - w.get("secs", 0) - 0.5 <= e["t"] <= w["t"] + 0.5:
                    return w
            return None
        hit50 = hit20 = inwin = 0
        for e in new_ev:
            w = window_at(e)
            if w:
                inwin += 1
                hit50 += w["pres"].get("max", 0) >= 50
                hit20 += w["pres"].get("max", 0) >= 20
        sd_near = sum(1 for e in new_ev if any(abs(h["t"] - e["t"]) <= 2.0 and h.get("send_dl_ms", 0) >= 20 for h in hitches))
        out.append(f"  first appearances: {len(new_ev)} moments when RT64 asked for new shaders ({sum(e['new'] for e in new_ev)} shaders); the window holding the moment had an "
                   f"interval >= 50 ms in {hit50} of {inwin} (>= 20 ms in {hit20}); a send_dl second >= 20 ms within 2 s of {sd_near}")
    if sh_lines:
        last = sh_lines[-1]
        out.append(f"  RT64 specialised shaders: {last.get('submitted')} submitted, {last.get('compiled')} compiled by the end; "
                   f"compile backlog peaked at {max(e.get('queued', 0) + e.get('submitted', 0) - e.get('compiled', 0) for e in sh_lines)}; "
                   f"new shaders after minute 10: {sum(1 for e in sh_lines if e.get('new', 0) > 0 and e['t'] > 600)}")
    out.append("")

    # Memory
    if mems:
        out.append("MEMORY (host process RSS per launch; footprint = macOS physical footprint)")
        by_launch: dict[int, list] = defaultdict(list)
        for m in mems:
            by_launch[m.get("launch", 0)].append(m)
        out.append(f"  {'launch':>6s} {'kind':5s} {'life':>8s} {'RSS first':>10s} {'RSS last':>9s} {'max':>7s} {'slope/h':>9s}  footprint first..last")
        kinds = {l.get("launch"): l.get("kind") for l in launches}
        for lid, ms in sorted(by_launch.items()):
            rs = [(m["age"], m["rss_mb"]) for m in ms if m.get("rss_mb")]
            if len(rs) < 2:
                continue
            slope = linreg(rs) * 3600
            fp = [m["foot_mb"] for m in ms if m.get("foot_mb")]
            out.append(f"  {lid:6d} {str(kinds.get(lid, '?')):5s} {fmt_dur(rs[-1][0]):>8s} {rs[0][1]:9.0f}M {rs[-1][1]:8.0f}M {max(r for _, r in rs):6.0f}M {slope:+8.0f}M  "
                       + (f"{fp[0]:.0f}M..{fp[-1]:.0f}M" if fp else "-"))
        out.append("")

    # Launches
    if launches:
        out.append("LAUNCHES (process start -> first game frame)")
        for kind in ("first", "cold", "warm"):
            ls = [l for l in launches if l.get("kind") == kind and l.get("first_frame_s") is not None]
            if ls:
                v = sorted(l["first_frame_s"] for l in ls)
                out.append(f"  {kind:5s} n={len(v):2d}  median {v[len(v) // 2]:.2f} s  min {v[0]:.2f}  max {v[-1]:.2f}   (control ready: median {sorted(l['ready_s'] for l in ls)[len(ls) // 2]:.2f} s)")
        fb = [e for e in ev if e["ev"] == "first_run_build"]
        for b in fb:
            out.append(f"  first-run build of the game library: {b.get('seconds')} s")
        out.append("")

    acts = [e for e in ev if e["ev"] == "activity"]
    if acts:
        c = Counter(a.get("kind") for a in acts)
        out.append("ACTIVITIES: " + ", ".join(f"{k} x{n}" for k, n in c.most_common()))
        lv = Counter(a.get("level") for a in acts if a.get("level"))
        out.append("  levels played: " + ", ".join(f"{k} x{n}" for k, n in lv.most_common()))
    return "\n".join(out)


def hours(secs: float, n: int) -> str:
    return f"{n / (secs / 3600):.1f}" if secs else "-"


def linreg(pts: list[tuple[float, float]]) -> float:
    """Least-squares slope (y per x)."""
    n = len(pts)
    mx, my = sum(x for x, _ in pts) / n, sum(y for _, y in pts) / n
    den = sum((x - mx) ** 2 for x, _ in pts)
    return sum((x - mx) * (y - my) for x, y in pts) / den if den else 0.0


if "--summary" in sys.argv:   # no host needed (no THPS_HOST)
    i = sys.argv.index("--summary")
    print(summarize(sys.argv[i + 1:]))
    sys.exit(0)


# ---------------------------------------------------------------------------------------
# The test

import harness as H   # noqa: E402  ($THPS_HOST names the game)
import run as R       # noqa: E402  (the suite's Session / checkpoints; importing runs nothing)

S = R.S
HANG_S = 5.0            # no presented frame for this long, game supposed to be running = hang
GAME_STALL_S = 15.0     # no game frame (gfx task) this long while presents go on = hang of the game thread
CTL_TIMEOUT = 90.0      # a control reply that takes longer: the host is wedged


class Log:
    def __init__(self, path: Path):
        self.f = open(path, "a", buffering=1)
        self.t0 = time.monotonic()
        self.lock = threading.Lock()

    def emit(self, ev: str, **kw):
        rec = {"ev": ev, "t": round(time.monotonic() - self.t0, 2)}
        rec.update(kw)
        with self.lock:
            self.f.write(json.dumps(rec, separators=(",", ":")) + "\n")


def load_avg() -> list[float]:
    return [round(x, 2) for x in os.getloadavg()] if hasattr(os, "getloadavg") else [0.0, 0.0, 0.0]


def rss_mb(pid: int) -> float | None:
    try:
        if os.name == "nt":
            r = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"], capture_output=True, text=True, timeout=20).stdout
            m = re.findall(r'"([\d.,\s]+) K"', r)
            return int(re.sub(r"\D", "", m[-1])) / 1024 if m else None
        r = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True, timeout=10).stdout.strip()
        return int(r) / 1024 if r else None
    except Exception:   # noqa: BLE001
        return None


def footprint_mb(pid: int) -> float | None:
    """macOS physical footprint (counts compressed pages, which RSS does not)."""
    if sys.platform != "darwin":
        return None
    try:
        r = subprocess.run(["footprint", "-p", str(pid)], capture_output=True, text=True, timeout=30).stdout
        m = re.search(r"Footprint:\s*([\d.]+)\s*([KMG]B)", r)
        if not m:
            m = re.search(r"phys_footprint:\s*([\d.]+)\s*([KMG]B)", r)
        if m:
            return float(m.group(1)) * {"KB": 1 / 1024, "MB": 1, "GB": 1024}[m.group(2)]
    except Exception:   # noqa: BLE001
        pass
    return None


def activate_window(pid: int) -> bool:
    """macOS: a process started from a script has its window behind the front app's (the window is then "occluded": the host skips its presents,
    see the private development notes "presenting to an occluded window"), a person starting the game gets it in front. Bring the host's window to
    the front through System Events (the same thing a click on its Dock icon does). False when that is not possible."""
    if sys.platform != "darwin":
        return False
    try:
        r = subprocess.run(["osascript", "-e", f'tell application "System Events" to set frontmost of (first process whose unix id is {pid}) to true'],
                           capture_output=True, text=True, timeout=20)
        return r.returncode == 0
    except Exception:   # noqa: BLE001
        return False


def parse_hist(s: str) -> dict[int, int]:
    return {int(a): int(b) for a, b in (kv.split(":") for kv in s.split(",") if ":" in kv)} if s else {}


class Monitor:
    """Watches one host process from its output lines: the per-second `[thps] perf` line gives presented / game frames per second and the longest
    send_dl / update_screen of that second. A thread declares a hang when nothing was presented for HANG_S while armed."""

    PERF = re.compile(r"^\[thps\] perf t=([\d.]+)s skipped_presents=(\d+) fps=([\d.]+) presented/s=([\d.]+) window/s=([\d.]+) presented_total=(\d+) .*?"
                      r"gfx/s=([\d.]+) .*?send_dl avg ([\d.]+) ms max ([\d.]+) \| update_screen avg ([\d.]+) ms max ([\d.]+)")

    def __init__(self, soak: "Soak"):
        self.soak = soak
        self.armed = False
        self.last_line = time.monotonic()
        self.last_progress = time.monotonic()     # last evidence of a presented frame
        self.last_game = time.monotonic()
        self.presented_total = -1
        self.skipped = -1
        self.recent: list[str] = []                # last perf lines, kept with a hang report
        self.hung: dict | None = None
        self.stop = threading.Event()
        self.pid = 0
        self.proc: subprocess.Popen | None = None
        self.last_new_shader_t = -99.0
        self.win_secs = 0                          # perf seconds since the last frame-time window closed, and how many had the window presenting
        self.vis_secs = 0
        self.dark_run = 0                          # consecutive perf seconds with the window not presenting
        self.last_activate = -99.0
        self.activations = 0
        self.thread = threading.Thread(target=self._watch, daemon=True)
        self.thread.start()

    def on_line(self, line: str):
        now = time.monotonic()
        self.last_line = now
        m = self.PERF.match(line)
        if not m:
            return
        t, skipped, fps, pres_s, win_s, total, gfx_s, sd_avg, sd_max, us_avg, us_max = m.groups()
        total, skipped = int(total), int(skipped)
        if total != self.presented_total or skipped != self.skipped:
            self.last_progress = now
        self.presented_total, self.skipped = total, skipped
        if float(gfx_s) > 0:
            self.last_game = now
        self.win_secs += 1
        self.vis_secs += float(win_s) >= 5.0      # window/s ~ 0 = the window is not on screen (occluded, other Space): presents are skipped
        self.dark_run = 0 if float(win_s) >= 5.0 else self.dark_run + 1
        self.recent = (self.recent + [line[:200]])[-6:]
        sd, us = float(sd_max), float(us_max)
        ctx = self.soak.ctx
        if self.armed and float(win_s) >= 5.0 and (sd >= 20 or us >= 20) and ctx.get("phase") not in (None, "boot"):   # (an occluded window's update_screen numbers are artefacts)
            self.soak.log.emit("hitch", launch=ctx.get("launch"), phase=ctx.get("phase"), level=ctx.get("level"), age=round(now - ctx.get("t0", now), 1),
                               send_dl_ms=sd, update_screen_ms=us, fps=float(fps), presented_s=float(pres_s), gfx_s=float(gfx_s),
                               new_shaders=(now - self.last_new_shader_t) < 2.0)

    def _watch(self):
        while not self.stop.wait(0.5):
            if not self.armed or self.hung or self.proc is None or self.proc.poll() is not None:
                continue
            now = time.monotonic()
            if self.dark_run >= 3 and now - self.last_activate > 60 and self.soak.a.activate:
                self.last_activate = now
                self.activations += 1
                self.dark_run = 0
                ok = activate_window(self.pid)
                self.soak.log.emit("activate", launch=self.soak.ctx.get("launch"), phase=self.soak.ctx.get("phase"), ok=ok, n=self.activations)
            gap = now - self.last_progress
            kind = None
            if gap > HANG_S or now - self.last_line > HANG_S:
                kind, gap = ("no_present" if now - self.last_line <= HANG_S else "no_output"), max(gap, now - self.last_line)
            elif now - self.last_game > GAME_STALL_S:
                kind, gap = "game_stall", now - self.last_game
            if kind:
                self.hung = {"kind": kind, "gap_s": round(gap, 1)}
                self.soak.on_hang(self, kind, gap)


class SoakInstance(H.Instance):
    """H.Instance that keeps `ftime` windows running (rolled every window_s, at phase changes and on request) and polls the RT64 shader counters
    about once a second, both from `send`, i.e. between the runner's own commands, so no other thread talks to the host."""

    def __init__(self, soak: "Soak", *a, **kw):
        self.soak = soak
        self._in_tick = False
        self._active = False          # ticks start with start_windows() (after the first frame)
        self._no_shaders = False      # a binary without `ftime shaders`
        self._ftime_on = False
        self._win_t0 = 0.0
        self._win_ctx: dict = {}
        self._win_shaders: tuple | None = None
        self._last_poll = 0.0
        self._last_mem = 0.0
        self._last_foot = 0.0
        self._shaders_seen: tuple | None = None
        super().__init__(*a, **kw)

    def start_windows(self):
        self._active = True
        self._tick(force_start=True)

    # -- the window machinery ----------------------------------------------------------
    def _raw(self, line: str, timeout: float = 20) -> dict[str, str]:
        name = line.split()[0]
        H.Instance.send(self, line)
        reply = self.expect(f"ok {name}", timeout)
        return dict(kv.split("=", 1) for kv in reply.split()[2:] if "=" in kv)

    def poll_shaders(self, now: float):
        if self._no_shaders:
            return None
        try:
            r = self._raw("ftime shaders")
        except H.HostError as e:
            if "renderer not up" in str(e):
                return None
            if "ftime start|stop" in str(e):   # a binary from before `ftime shaders`
                self._no_shaders = True
                return None
            raise
        cur = (int(r["submitted"]), int(r["compiled"]), int(r["queued"]))
        if cur != self._shaders_seen:
            new = cur[0] - (self._shaders_seen[0] if self._shaders_seen else 0)
            if new > 0:
                self.soak.mon.last_new_shader_t = now
            c = self.soak.ctx
            self.soak.log.emit("shaders", launch=c.get("launch"), phase=c.get("phase"), level=c.get("level"), age=round(now - c.get("t0", now), 1),
                               submitted=cur[0], compiled=cur[1], queued=cur[2], new=new)
            self._shaders_seen = cur
        return cur

    def _stop_window(self, now: float):
        r = self._raw("ftime stop")
        self._ftime_on = False
        secs = float(r.get("seconds", 0))
        win = {k: v for k, v in r.items()}
        def side(name: str) -> dict:
            if f"{name}_fps" not in win:
                return {}
            return {"n": int(win.get(f"{name}_n", 0)) or None, "fps": float(win[f"{name}_fps"]), "p50": float(win[f"{name}_ms_p50"]), "p95": float(win[f"{name}_ms_p95"]),
                    "p99": float(win[f"{name}_ms_p99"]), "max": float(win[f"{name}_ms_max"]),
                    "o20": int(win.get(f"{name}_over20ms", win.get(f"{name}_over40ms", 0))), "o50": int(win.get(f"{name}_over50ms", 0)),
                    "o100": int(win.get(f"{name}_over100ms", 0))}
        pres, game = side("presented"), side("game")
        ctx = self._win_ctx
        end_sh = self._shaders_seen or (0, 0, 0)
        st_sh = self._win_shaders or end_sh
        rec = dict(launch=ctx.get("launch"), phase=ctx.get("phase"), level=ctx.get("level"), age=round(max(0.0, self._win_t0 - ctx.get("t0", self._win_t0)), 1), secs=round(secs, 1),
                   pres=pres, game=game, comp_fps=float(win.get("composited_fps", 0)),
                   shaders={"submitted": end_sh[0], "compiled": end_sh[1], "new": end_sh[0] - st_sh[0], "backlog_start": st_sh[0] - st_sh[1],
                            "backlog_end": end_sh[0] - end_sh[1]},
                   load=load_avg())
        if "presented_hist" in win:
            rec["hist"] = parse_hist(win["presented_hist"])
        if "game_hist" in win:
            rec["ghist"] = parse_hist(win["game_hist"])
        rec["ids_walk_us"] = win.get("ids_walk_us_avg")
        mon = self.soak.mon
        rec["vis"] = round(mon.vis_secs / mon.win_secs, 2) if mon.win_secs else None
        rec["activations"] = mon.activations
        mon.win_secs = mon.vis_secs = 0
        pid = self.proc.pid
        rec["rss_mb"] = round(rss_mb(pid) or 0, 1) or None
        self.soak.log.emit("window", **rec)

    def _tick(self, force_start: bool = False, force_roll: bool = False):
        if self._in_tick or not self.alive():
            return
        self._in_tick = True
        try:
            now = time.monotonic()
            if self._ftime_on and (force_roll or now - self._win_t0 >= self.soak.a.window_s):
                self._stop_window(now)
            if now - self._last_poll >= 1.0 or force_start or force_roll:
                self._last_poll = now
                self.poll_shaders(now)
            if not self._ftime_on:
                self._raw("ftime start")
                self._ftime_on = True
                self._win_t0 = time.monotonic()
                self._win_ctx = dict(self.soak.ctx)
                self._win_shaders = self._shaders_seen
            if now - self._last_mem >= 30.0:
                self._last_mem = now
                c = self.soak.ctx
                m = dict(launch=c.get("launch"), age=round(now - self.soak.launch_t0, 1), rss_mb=round(rss_mb(self.proc.pid) or 0, 1) or None, load=load_avg(),
                         phase=c.get("phase"))
                if now - self._last_foot >= 120.0:
                    self._last_foot = now
                    fp = footprint_mb(self.proc.pid)
                    if fp:
                        m["foot_mb"] = round(fp, 1)
                if self.soak.a.uptime_text:
                    m["uptime"] = H.load_info()["uptime"]
                self.soak.log.emit("mem", **m)
        finally:
            self._in_tick = False

    def roll(self):
        """Close the current window (phase change) and open the next one."""
        self._tick(force_roll=True)

    def send(self, line: str):
        if self._active and not self._in_tick and not line.startswith(("quit", "ftime")):
            self._tick()
        super().send(line)

    def close_windows(self):
        self._active = False
        if self._ftime_on and self.alive():
            try:
                self._in_tick = True
                self._stop_window(time.monotonic())
            except Exception:   # noqa: BLE001
                pass
            finally:
                self._in_tick = False


class Soak:
    # level names of THPS1's SELECT COURSE screen, in menu order (the career opens them one by one; only open ones are reachable)
    def __init__(self, a: argparse.Namespace):
        self.a = a
        self.out = Path(a.out)
        self.out.mkdir(parents=True, exist_ok=True)
        self.log = Log(self.out / "soak.jsonl")
        self.rng = random.Random(a.seed)
        self.ctx: dict = {"launch": 0, "phase": None, "level": None, "t0": time.monotonic()}
        self.launch_t0 = time.monotonic()
        self.launch_n = 0
        self.inst: SoakInstance | None = None
        self.mon: Monitor = Monitor(self)
        self.ses: R.Session | None = None
        self.deadline = 0.0
        self.apps: list[Path] = []
        self.split = a.split
        self.expected_exit = False
        self.navfails = 0
        self.crashes = 0
        self.hangs = 0
        self.vpad_ok = False
        self.rom = R.default_rom()
        self.cfg = H.HOST_DIR / "build" / "tests" / f"soak-config{'' if a.version == R.BASE_VERSION else '-' + a.version}"
        self.tmp = Path(a.tmp)

    # -- context ---------------------------------------------------------------------
    def phase(self, name: str, level: str | None = None):
        """Enter a phase: the frame-time window rolls, so every window is in exactly one phase / level."""
        if self.inst and self.inst.alive() and not self.mon.hung:
            self.inst.roll()
        self.ctx.update(phase=name, level=level, t0=time.monotonic(), launch=self.launch_n)
        if self.inst:
            self.inst._win_ctx = dict(self.ctx)

    # -- crash / hang -------------------------------------------------------------------
    def on_hang(self, mon: Monitor, kind: str, gap: float):
        """Called by the monitor thread: record, sample the process (macOS), kill it so the runner's blocked command returns."""
        self.hangs += 1
        c = self.ctx
        sample = None
        pid = mon.proc.pid if mon.proc else 0
        if sys.platform == "darwin" and pid:
            sample = str(self.out / f"hang-{self.hangs}-sample.txt")
            try:
                subprocess.run(["sample", str(pid), "3", "-file", sample], capture_output=True, timeout=30)
            except Exception:   # noqa: BLE001
                sample = None
        self.log.emit("hang", launch=c.get("launch"), phase=c.get("phase"), level=c.get("level"), age=round(time.monotonic() - c.get("t0", 0), 1), kind=kind,
                      gap_s=round(gap, 1), recent=mon.recent, sample=sample, rss_mb=round(rss_mb(pid) or 0, 1) if pid else None, load=load_avg())
        if self.inst:
            shutil.copyfile(self.inst.log_path, self.out / f"hang-{self.hangs}-host.log")
        try:
            if mon.proc and mon.proc.poll() is None:
                mon.proc.kill()
        except Exception:   # noqa: BLE001
            pass

    def collect_exit(self, why: str) -> dict:
        """The host is gone without us asking: crash (signal, crash.log stack) or an unexpected clean exit."""
        inst = self.inst
        assert inst
        try:
            rc = inst.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            rc = None
        sig = None
        if rc is not None:
            sig = rc - 128 if 128 < rc < 160 else (-rc if rc < 0 else None)
        stack: list[str] = []
        cl = self.cfg / "crash.log"
        if cl.exists():
            text = cl.read_text(errors="replace").splitlines()
            stack = text[-60:]
            keep = self.out / f"crash-{self.crashes + 1}.log"
            keep.write_text("\n".join(text) + "\n")
            cl.unlink(missing_ok=True)
        self.crashes += 1
        c = self.ctx
        signame = signal.Signals(sig).name if sig else (f"0x{rc & 0xFFFFFFFF:08X}" if os.name == "nt" and rc else None)   # Windows: the exception code
        shutil.copyfile(inst.log_path, self.out / f"crash-{self.crashes}-host.log")
        self.log.emit("crash", launch=c.get("launch"), phase=c.get("phase"), level=c.get("level"), age=round(time.monotonic() - c.get("t0", 0), 1),
                      exit=rc, signal=signame, why=why, stack=stack[-40:], recent=self.mon.recent, rss_mb=None, load=load_avg())
        return {"exit": rc, "signal": signame}

    # -- launch ----------------------------------------------------------------------------
    def is_split(self) -> bool:
        return self.split

    def fresh_app_copy(self) -> Path:
        """A copy of the engine and its resources at a new path (what a user gets from a fresh unzip): macOS then verifies the binary again."""
        src = H.BINARY
        res = next((d for d in (src.parent / "resources", src.parent.parent / "Resources") if d.is_dir()), None)
        n = len(self.apps) + 1
        app = self.tmp / f"app-{n}"
        if app.exists():
            shutil.rmtree(app, ignore_errors=True)
        app.mkdir(parents=True)
        shutil.copy2(src, app / src.name)
        if res is not None:
            shutil.copytree(res, app / "resources", symlinks=False)
        if os.name == "nt":
            for dll in src.parent.glob("*.dll"):
                shutil.copy2(dll, app / dll.name)
        elif sys.platform == "darwin" and (src.parent.parent / "Frameworks").is_dir():
            fw = self.tmp / "Frameworks"
            if not fw.exists():
                shutil.copytree(src.parent.parent / "Frameworks", fw, symlinks=True)
        self.apps.append(app)
        for old in self.apps[:-2]:   # keep the last two
            shutil.rmtree(old, ignore_errors=True)
        return app / src.name

    def prepare_config(self):
        shutil.rmtree(self.cfg, ignore_errors=True)
        if self.split:
            t0 = time.monotonic()
            p = subprocess.run([str(H.BINARY), "--rom", self.rom, "--config", str(self.cfg), "--build-game"], capture_output=True, text=True, errors="replace", timeout=900)
            dt = time.monotonic() - t0
            self.log.emit("first_run_build", seconds=round(dt, 1), rc=p.returncode, load=load_avg(), tail=p.stdout.strip().splitlines()[-3:])
            if p.returncode != 0:
                raise SystemExit(f"first-run build failed ({p.returncode}):\n{p.stdout[-2000:]}\n{p.stderr[-1000:]}")

    def launch(self, kind: str):
        """Start the host, wait for the first game frame. kind: first | cold | warm."""
        self.launch_n += 1
        self.mon.stop.set()
        self.mon = Monitor(self)
        self.ctx.update(launch=self.launch_n, phase="boot", level=None, t0=time.monotonic())
        (self.cfg / "crash.log").unlink(missing_ok=True)
        binary = H.BINARY
        if self.split and kind == "cold":
            binary = self.fresh_app_copy()
        elif not self.split and kind == "cold":
            binary = self.fresh_app_copy()
        skip = kind == "warm" and self.a.skip_intro_warm
        extra = ["--volume", "0"] + (["--skip-intro"] if skip else [])
        log_path = self.out / f"host-{self.launch_n}.log"
        t0 = time.monotonic()
        self.launch_t0 = t0
        self.expected_exit = False
        slow = 3.0 if H.load_info()["overloaded"] else 1.0
        try:
            inst = SoakInstance(self, self.cfg, log_path, rom=None if self.split else self.rom, extra=extra, binary=binary, line_hook=self.mon.on_line,
                                control_args=["--no-keyboard", "--no-pads"])
        except Exception as e:   # noqa: BLE001
            self.log.emit("navfail", launch=self.launch_n, phase="launch", msg=f"host did not start: {e}")
            self.navfails += 1
            raise
        self.inst = inst
        self.mon.proc = inst.proc
        self.mon.pid = inst.proc.pid
        ready = time.monotonic() - t0
        first = None
        deadline = t0 + 60 * slow
        while time.monotonic() < deadline:
            r = inst.cmd("stats")
            if int(r["frames"]) >= 1:
                first = time.monotonic() - t0
                break
            time.sleep(0.02)
        self.mon.last_progress = self.mon.last_game = self.mon.last_line = time.monotonic()
        self.mon.armed = first is not None
        act = activate_window(inst.proc.pid) if (first is not None and self.a.activate) else None   # macOS: window in front, as when a person starts the game
        self.log.emit("launch", launch=self.launch_n, kind=kind, ready_s=round(ready, 2), first_frame_s=None if first is None else round(first, 2), pid=inst.proc.pid,
                      app=str(binary), skip_intro=skip, activated=act, load=load_avg(), rss_mb=round(rss_mb(inst.proc.pid) or 0, 1) or None)
        if first is None:
            raise R.StepFail("no game frame within 60 s of launch")
        self.ses = R.Session(inst, self.out / "polls", False, slow, self.cfg)
        self.ses.rom = self.rom
        self.vpad_ok = False
        inst.start_windows()
        self.phase("boot")

    def shutdown(self, reason: str = "planned"):
        inst = self.inst
        if not inst:
            return
        self.expected_exit = True
        self.mon.armed = False
        try:
            inst.close_windows()
            inst.quit()
        finally:
            self.mon.stop.set()
            if inst.proc.returncode not in (0, None) and reason == "planned":
                self.log.emit("navfail", launch=self.launch_n, phase="quit", msg=f"host exit code {inst.proc.returncode} on quit")
            self.inst = None
            self.ses = None
            if self.a.keep_logs != "all":
                (self.out / f"host-{self.launch_n}.log").unlink(missing_ok=True)
            shutil.rmtree(self.out / "polls", ignore_errors=True)

    # -- steps --------------------------------------------------------------------------------
    def step(self, *steps):
        assert self.ses
        for st in steps:
            self.ses.step(st)

    def frames(self, n: int):
        assert self.inst
        self.inst.wait_frames(n, timeout=120 * self.ses.slow)

    def press(self, b: str, wait: int = 12):
        assert self.inst
        self.inst.press(b, 3, 3, timeout=60 * self.ses.slow)
        self.frames(wait)

    def cp_now(self, cands: list[str]) -> str | None:
        assert self.ses
        img, _ = self.ses.shot()
        for cp in cands:
            ok, _d = self.ses.match(img, cp)
            if ok:
                return cp
        return None

    def seen(self, cands: list[str], timeout: float = 30) -> str | None:
        end = time.monotonic() + timeout * self.ses.slow
        while time.monotonic() < end:
            cp = self.cp_now(cands)
            if cp:
                return cp
            self.frames(10)
        return None

    def vpad(self, cmd: str):
        assert self.inst
        self.inst.send("vpad " + cmd)
        self.inst.expect("ok vpad", 30)

    def attach_pad(self):
        if self.vpad_ok:
            return
        self.vpad("attach")
        self.frames(5)
        self.vpad("button leftshoulder 1")
        self.frames(3)
        self.vpad("button leftshoulder 0")
        self.frames(3)
        self.vpad_ok = True

    # -- game flow ----------------------------------------------------------------------------
    def boot_to_menu(self, skip: bool):
        self.phase("boot")
        if skip:
            self.step(("wait_for", "title"), ("go", "START", "title", "main_menu"))
        else:
            self.step(("wait_for", "logo"), ("go", "START", None, "title"), ("go", "START", "title", "main_menu"))
        self.phase("menu", "main_menu")

    def ensure_main_menu(self):
        cp = self.cp_now(["main_menu"])
        if cp:
            return
        raise R.StepFail("not on the main menu")

    def activity(self, kind: str, fn, **kw):
        t0 = time.monotonic()
        res = "ok"
        try:
            out = fn()
            if isinstance(out, str):
                res = out
        except BaseException as e:
            res = f"failed: {type(e).__name__}"
            raise
        finally:
            self.log.emit("activity", launch=self.launch_n, kind=kind, secs=round(time.monotonic() - t0, 1), result=res, **kw)

    def run(self):
        a = self.a
        t_start = time.monotonic()
        self.deadline = t_start + a.minutes * 60
        load0 = H.load_info()
        self.log.emit("start", game=H.FAMILY, version=a.version, platform=sys.platform, engine="split" if self.split else "static", binary=str(H.BINARY),
                      head=subprocess.run(["git", "-C", str(H.HOST_DIR), "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip(),
                      machine=load0["uptime"][:200], load=load_avg(), args=vars(a), wall_date=time.strftime("%Y-%m-%d %H:%M:%S"), ncpu=load0["ncpu"])
        self.prepare_config()
        prev_crashed = False
        i = 0
        quick_fails = 0
        while time.monotonic() < self.deadline:
            kind = "first" if i == 0 else "warm" if prev_crashed else ("cold" if i % 2 == 1 else "warm")
            i += 1
            prev_crashed = False
            t_launch = time.monotonic()
            try:
                self.launch(kind)
                life_end = min(self.deadline, time.monotonic() + a.launch_minutes * 60)
                if not a.stub_flow:
                    self.boot_to_menu(skip=kind == "warm" and a.skip_intro_warm)
                self.play_launch(life_end)
                self.shutdown()
                quick_fails = 0
            except (R.StepFail, H.HostError, OSError) as e:
                prev_crashed = True
                quick_fails = quick_fails + 1 if time.monotonic() - t_launch < 90 else 0
                self.handle_failure(e)
                if quick_fails >= 5:
                    self.log.emit("abort", why="5 launches in a row failed within 90 s", last=str(e)[:300])
                    break
            except Exception as e:   # noqa: BLE001  (a bug in this script: keep going, but say so)
                prev_crashed = True
                self.log.emit("navfail", launch=self.launch_n, phase=self.ctx.get("phase"), msg="script error: " + traceback.format_exc()[-600:])
                self.navfails += 1
                self.handle_failure(e, internal=True)
        self.mon.stop.set()
        self.log.emit("end", seconds=round(time.monotonic() - t_start, 1), launches=self.launch_n, crashes=self.crashes, hangs=self.hangs, navfails=self.navfails,
                      load=load_avg())
        for app in self.apps:
            shutil.rmtree(app, ignore_errors=True)

    def handle_failure(self, e: BaseException, internal: bool = False):
        """Classify a failure of the runner: hang (the monitor already recorded it), crash, wedged control channel, or a script desync."""
        inst = self.inst
        c = self.ctx
        if inst is None:
            time.sleep(2)
            return
        if self.mon.hung:
            pass   # on_hang recorded and killed it
        elif not inst.alive():
            self.collect_exit(str(e))
        elif isinstance(e, H.HostError) and "timeout" in str(e):
            # alive but silent: a wedged control thread or game; same treatment as a hang
            self.mon.hung = {"kind": "control_timeout", "gap_s": CTL_TIMEOUT}
            self.on_hang(self.mon, "control_timeout", CTL_TIMEOUT)
        else:
            self.navfails += 1
            if not internal:
                self.log.emit("navfail", launch=self.launch_n, phase=c.get("phase"), level=c.get("level"), msg=str(e)[:400])
            if self.ses and self.ses.last_shot and self.ses.last_shot.exists() and self.navfails <= 15:
                shutil.copyfile(self.ses.last_shot, self.out / f"navfail-{self.navfails}.png")
        try:
            self.mon.armed = False
            if inst.alive():
                inst.proc.kill()
            inst.close_windows = lambda: None
            inst.quit()
        except Exception:   # noqa: BLE001
            pass
        self.mon.stop.set()
        self.inst = None
        self.ses = None

    # -- generic play ---------------------------------------------------------------------
    def stick(self, x: int, y: int):
        self.vpad(f"axis leftx {x}")
        self.vpad(f"axis lefty {y}")

    def tap(self, button: str, hold: int = 5, after: int = 3):
        self.vpad(f"button {button} 1")
        self.frames(hold)
        self.vpad(f"button {button} 0")
        self.frames(after)

    def trick(self):
        """One random skating move on the virtual pad (Xbox layout: A = ollie, X = flip, B = grab, Y = grind; the stick pushes / steers)."""
        r = self.rng.random()
        dx = self.rng.choice((-30000, -15000, 0, 15000, 30000))
        if r < 0.30:                                   # push / steer
            self.stick(dx, -30000)
            self.frames(self.rng.randint(25, 80))
        elif r < 0.50:                                 # ollie + flip trick
            self.stick(dx, -30000)
            self.tap("a", 8, 3)
            self.vpad("button x 1")
            self.frames(5)
            self.vpad("button x 0")
            self.frames(self.rng.randint(15, 30))
        elif r < 0.68:                                 # ollie + grab
            self.stick(dx, -30000)
            self.tap("a", 8, 3)
            self.vpad("button b 1")
            self.frames(self.rng.randint(8, 16))
            self.vpad("button b 0")
            self.frames(self.rng.randint(15, 30))
        elif r < 0.84:                                 # ollie + grind attempt (a rail is a matter of luck)
            self.stick(dx, -30000)
            self.tap("a", 8, 3)
            self.vpad("button y 1")
            self.frames(self.rng.randint(10, 40))
            self.vpad("button y 0")
            self.frames(10)
        elif r < 0.92:                                 # hard turn
            self.stick(self.rng.choice((-30000, 30000)), -15000)
            self.frames(self.rng.randint(10, 25))
        else:                                          # stop and ollie in place
            self.stick(0, 0)
            self.tap("a", 6, 20)

    def play(self, secs: float, check_every: float = 20.0) -> str:
        """Skate for `secs` of wall time with random moves. Returns "ok", or "timeup" when the run ended by itself (the career's two minutes:
        the statistics screen showed up) -- checked every `check_every` s with a frame grab."""
        self.attach_pad()
        t_end = min(time.monotonic() + secs, self.deadline)
        last_check = time.monotonic()
        try:
            while time.monotonic() < t_end:
                self.trick()
                if time.monotonic() - last_check >= check_every:
                    last_check = time.monotonic()
                    if self.cp_now(["statistics"]) == "statistics":
                        return "timeup"
        finally:
            if self.inst and self.inst.alive() and not self.mon.hung:
                try:
                    self.stick(0, 0)
                except (H.HostError, OSError):
                    pass
        return "ok"

    # -- what a launch does ---------------------------------------------------------------
    def play_launch(self, life_end: float):
        """The game's own flows (menus, levels, endings) live in the game's tests/soak_game.py."""
        if self.a.stub_flow:   # harness self-test: no menus, just skate (a fake host in tests of the harness itself)
            while time.monotonic() < life_end:
                self.phase("career", "Stub")
                self.play(8)
                self.phase("menu", "stub-menu")
                self.frames(30)
            return
        import soak_game   # noqa: PLC0415  (decomp/thps<N>/recomp-host/tests, on sys.path through the suite's scenarios import)
        soak_game.play_launch(self, life_end)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--minutes", type=float, default=120.0, help="how long to soak (default 120)")
    ap.add_argument("--version", default=R.BASE_VERSION, help="ROM version: a games.toml row of the game's family (default: the family root)")
    ap.add_argument("--out", default="", help="output dir (default build/tests/soak/<timestamp>)")
    ap.add_argument("--launch-minutes", type=float, default=10.0, help="relaunch the host every N minutes, alternating warm and cold (default 10)")
    ap.add_argument("--window-s", type=float, default=10.0, help="frame-time window length (default 10 s)")
    ap.add_argument("--seed", type=int, default=0, help="random seed of the trick / level / ending choices (default: from the clock)")
    ap.add_argument("--static", action="store_true", help="the static engine (build/cmake) instead of the split engine (build/cmake-split, what a release ships)")
    ap.add_argument("--no-skip-intro", dest="skip_intro_warm", action="store_false", help="do not use --skip-intro on warm launches (cold launches never do)")
    ap.add_argument("--keep-logs", choices=["incidents", "all"], default="incidents", help="host logs kept: only those of crashes / hangs, or all")
    ap.add_argument("--only", default="", help="comma list of activities to run (career, freeskate, menus; default all), for trying the flows out")
    ap.add_argument("--no-activate", dest="activate", action="store_false", help="do not bring the host's window to the front when it is occluded (macOS)")
    ap.add_argument("--stub-flow", action="store_true", help=argparse.SUPPRESS)   # self-test of this script against a fake host
    ap.add_argument("--uptime-text", action="store_true", help="the full `uptime` line in every mem sample")
    ap.add_argument("--wait-others", type=float, default=600, help="seconds to wait for other <game>_recomp instances")
    a = ap.parse_args()
    R.use_version(a.version)
    a.split = (not a.static) and any("split" in d.name for d in (H.BINARY.parent, H.BINARY.parent.parent))   # build/cmake-split[/Release]/<game>_recomp
    if not a.seed:
        a.seed = int(time.time()) % 100000
    if not H.BINARY.exists():
        print(f"error: {H.BINARY} not built", file=sys.stderr)
        return 2
    rom = R.default_rom()
    if not rom or not Path(rom).exists():
        print(f"error: no ROM for {a.version} (set ${H.FAMILY.upper()}_ROM or put it in roms/)", file=sys.stderr)
        return 2
    others = R.other_instances()
    deadline = time.monotonic() + a.wait_others
    while others and time.monotonic() < deadline:
        time.sleep(5)
        others = R.other_instances()
    if others:
        print("error: " + H.FAMILY + "_recomp still running (one instance at a time)", file=sys.stderr)
        return 2
    stamp = time.strftime("%Y%m%d-%H%M%S")
    a.out = a.out or str(H.HOST_DIR / "build" / "tests" / "soak" / f"{stamp}-{a.version}")
    import tempfile
    a.tmp = tempfile.mkdtemp(prefix="thps-soak-")
    print(f"soak: {H.FAMILY} {a.version}, {a.minutes:g} min, engine {'split' if a.split else 'static'} {H.BINARY}\nout: {a.out}\nuptime: {H.load_info()['uptime']}", flush=True)
    soak = Soak(a)
    try:
        soak.run()
    finally:
        shutil.rmtree(a.tmp, ignore_errors=True)
    print(summarize([str(Path(a.out) / "soak.jsonl")]))
    return 1 if (soak.crashes or soak.hangs) else 0


if __name__ == "__main__":
    sys.exit(main())
