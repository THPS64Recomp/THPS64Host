#!/usr/bin/env python3
"""High-frame-rate / latency probes for the THPS1 recomp host (stdlib only, uses tests/).

    tools/hfr_probe.py [--args "--refresh 60 ..."] [--out DIR] ACTION [ACTION ...]

Boots <game>_recomp --control --skip-intro --hidden (plus --args), drives the menus to the
Warehouse in Free Skate (tests/scenarios.py checkpoints, so it needs the suite's references),
then runs the actions in order:

  trace                 print every command of one in-level gfx task into the host log
  motion:SEC[:STICK]    hold STICK (default SUP) for SEC s while every composited frame is captured
                        (pcap 1) into OUT/motion_N/; then report frame_motion and the step series
  shots:N[:EVERY]       N present captures, one every EVERY composited frames, into OUT/shots_N/
  perf:SEC              median fps / presented/s / hfr pairing line over SEC s
  latency:BUTTON[:N]    N press-to-present measurements at the main menu (before the level!)
  menu_latency:N        go to the main menu only and measure DOWN/UP latency N times
  wait:FRAMES           let the game run
  cam                   camera-cut detector state (hfrcam)
  retry                 pause -> RETRY (restarts the run: a camera cut)
  dbg[:N]               N lists of the next frame's transforms: paired / unpaired / skipped

Results go to OUT (default build/hfr/<time>/: ROM-derived frames, local only) and a JSON
summary is printed at the end. Only one host may run at a time (pgrep <game>_recomp first).
"""
from __future__ import annotations

import argparse
import json
import shlex
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hostenv  # noqa: E402

HERE = Path(hostenv.host_dir())                    # the game's recomp-host: scenarios.py, build/
sys.path.insert(0, str(Path(hostenv.SHARED) / "tests"))   # harness, hfr_steps, run
sys.path.append(str(HERE / "tests"))                       # scenarios.py (its run.py is a wrapper: keep it behind the shared one)
import harness as H  # noqa: E402
import run as R  # noqa: E402
import scenarios as S  # noqa: E402


# Metrics: tests/hfr_steps.py (shared with the suite's hfr_motion step).
from hfr_steps import motion_report, wait_pngs  # noqa: E402


def process_cpu_seconds(pid: int) -> float:
    """Cumulative CPU time (user+system, all threads) of a process, from `ps -o time=` (10 ms resolution; macOS/Linux)."""
    import subprocess
    out = subprocess.run(["ps", "-o", "time=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    secs = 0.0
    for part in out.split(":"):
        secs = secs * 60 + float(part)
    return secs


def thread_cpu(inst) -> dict:
    """Per-thread CPU seconds by thread name from the host's `cpu` control command (macOS)."""
    try:
        kv = inst.cmd("cpu")
    except Exception:
        return {}
    return {k: float(v) for k, v in kv.items() if k != "total"}


# ---- driver -------------------------------------------------------------------------------
def to_level(sess: R.Session):
    for st in [("wait_for", "title"), ("go", "START", "title", "main_menu"), ("menu", "main", 3), *S.TO_LEVEL_SELECT,
               ("go", "A", "level_select", "tips_ready"), ("go", "A", "tips_ready", "in_level")]:
        sess.step(st)


def to_menu(sess: R.Session):
    for st in [("wait_for", "title"), ("go", "START", "title", "main_menu")]:
        sess.step(st)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--args", default="", help="extra host arguments")
    ap.add_argument("--out", default="", help="output dir (default build/hfr/<time>)")
    ap.add_argument("--config", default="", help="config dir (default OUT/config)")
    ap.add_argument("--rom", default=R.default_rom())
    ap.add_argument("--no-skip-intro", action="store_true")
    ap.add_argument("--visible", action="store_true", help="don't pass --hidden")
    ap.add_argument("actions", nargs="+")
    a = ap.parse_args()
    if R.other_instances():
        print("error: another <game>_recomp is running:", R.other_instances(), file=sys.stderr)
        return 2
    out = Path(a.out) if a.out else HERE / "build" / "hfr" / time.strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    config = Path(a.config) if a.config else out / "config"
    config.mkdir(parents=True, exist_ok=True)
    extra = ([] if a.no_skip_intro else ["--skip-intro"]) + ([] if a.visible else ["--hidden"]) + shlex.split(a.args)
    inst = H.Instance(config, out / "host.log", rom=a.rom, extra=extra)
    sess = R.Session(inst, out, record=False, slow=3.0 if H.load_info()["overloaded"] else 1.0, config=config)
    results: dict = dict(args=extra, load_start=H.load_info())
    in_level = False
    try:
        for k, act in enumerate(a.actions):
            parts = act.split(":")
            name = parts[0]
            if name == "menu_latency":
                to_menu(sess)
                n = int(parts[1]) if len(parts) > 1 else 10
                ms, pres, frames = [], [], []
                for i in range(n):
                    inst.wait_frames(20)
                    line = inst.send("latency " + ("DOWN" if i % 2 == 0 else "UP")) or inst.expect("ok latency", 60)
                    kv = dict(x.split("=", 1) for x in line.split()[2:] if "=" in x)
                    ms.append(float(kv["ms"])); pres.append(int(kv["presents"])); frames.append(int(kv["frames"]))
                results[f"{k}_menu_latency"] = dict(ms=ms, presents=pres, frames=frames, median_ms=statistics.median(ms),
                                                    median_presents=statistics.median(pres), median_frames=statistics.median(frames))
                print("menu_latency", results[f"{k}_menu_latency"])
                continue
            if not in_level and name in ("trace", "motion", "shots", "perf", "dbg", "cam", "retry"):
                to_level(sess)
                in_level = True
                inst.wait_frames(30)
            if name == "wait":
                inst.wait_frames(int(parts[1]))
            elif name == "dbg":
                for _ in range(int(parts[1]) if len(parts) > 1 else 1):
                    inst.send("hfrdbg")
                    line = inst.expect("ok hfrdbg", 30)
                    print(line)
                    results.setdefault(f"{k}_dbg", []).append(line)
                    inst.wait_frames(15)
            elif name == "cam":
                print(inst.cmd("hfrcam"))
            elif name == "retry":
                # pause -> RETRY: the run restarts (a camera cut back to the start)
                for st in [("go", "START", "in_level", "pause"), ("menu", "pause", 1), ("go", "A", "pause", "in_level")]:
                    sess.step(st)
            elif name == "trace":
                print(inst.cmd("dltrace"))
                inst.wait_frames(6)
            elif name == "perf":
                sec = float(parts[1])
                n0 = len(inst.perf)
                h0 = inst.cmd("hfr")
                cpu0 = process_cpu_seconds(inst.proc.pid)
                th0 = thread_cpu(inst)
                t0 = time.monotonic()
                while time.monotonic() - t0 < sec + 1.5:
                    inst.wait_frames(15)
                h1 = inst.cmd("hfr")
                cpu1 = process_cpu_seconds(inst.proc.pid)
                th1 = thread_cpu(inst)
                dt = time.monotonic() - t0
                fps = [f for _, f in inst.perf[n0 + 1:]]
                d = {kk: int(h1[kk]) - int(h0[kk]) for kk in h1 if kk in h0 and kk != "refresh"}
                results[f"{k}_perf"] = dict(median_fps=statistics.median(fps) if fps else None, min_fps=min(fps) if fps else None,
                                            fps=fps, presented_per_s=round(d["rendered"] / dt, 1),
                                            render_ms_per_game_frame=round(d["render_us"] / max(d["workloads"], 1) / 1000, 2),
                                            dropped=d["dropped"], interpolating=d["interpolating"], matched=d["matched"],
                                            paired_pct=round(100 * d["paired"] / max(d["transforms"], 1), 1),
                                            load=H.load_info()["load1"],
                                            cpu_s=round(cpu1 - cpu0, 2),   # whole-process CPU (all threads) over the window, from ps
                                            cpu_ms_per_game_frame=round(1000 * (cpu1 - cpu0) / max(d["workloads"], 1), 2),
                                            game_frames=d["workloads"],
                                            thread_cpu_ms_per_game_frame={k2: round(1000 * (th1[k2] - th0.get(k2, 0)) / max(d["workloads"], 1), 2)
                                                                          for k2 in th1 if th1[k2] - th0.get(k2, 0) > 0.05})
                print("perf", results[f"{k}_perf"])
            elif name in ("motion", "shots"):
                d = out / f"{name}_{k}"
                d.mkdir(exist_ok=True)
                if name == "motion":
                    sec = float(parts[1])
                    stick = parts[2] if len(parts) > 2 else "SUP"
                    polls = int(sec * 30)
                    h0 = inst.cmd("hfr")
                    inst.cmd(f"pcap 1 {d}")
                    inst.send(f"press {stick} {polls} 0")
                    inst.expect(f"ok press {stick}", 120)
                    inst.cmd("pcap 0")
                    h1 = inst.cmd("hfr")
                    files = wait_pngs(d, 1)
                    rep = motion_report(files, seconds=polls / 30.0)
                    rep["hfr_delta"] = {kk: int(h1[kk]) - int(h0[kk]) for kk in h1 if kk in h0 and h1[kk].lstrip("-").isdigit()}
                    rep["dir"] = str(d)
                    results[f"{k}_motion"] = rep
                    print("motion", {kk: v for kk, v in rep.items() if kk not in ("diffs", "shift_x", "shift_y")})
                else:
                    n = int(parts[1])
                    every = int(parts[2]) if len(parts) > 2 else 30
                    c0 = int(inst.cmd(f"pcap {every} {d}")["composited"])
                    while True:   # stop capturing as soon as N frames were composited
                        inst.wait_frames(1)
                        st = inst.cmd(f"pcap {every}")
                        if int(st["composited"]) - c0 >= n * every:
                            break
                    inst.cmd("pcap 0")
                    files = wait_pngs(d, n, timeout=60)
                    results[f"{k}_shots"] = dict(dir=str(d), n=len(files))
            else:
                raise SystemExit(f"unknown action {act}")
    finally:
        results["load_end"] = H.load_info()
        inst.quit()
        (out / "results.json").write_text(json.dumps(results, indent=1))
    print(json.dumps({k: v for k, v in results.items() if not isinstance(v, dict) or "diffs" not in v}, indent=1)[:4000])
    print("out:", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
