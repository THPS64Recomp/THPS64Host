"""High-frame-rate and input-latency steps for the THPS recomp suites (Q18; stdlib only).

Steps (see scenarios.py):
  ("hfr_motion", SEC, STICK, checks)   hold STICK for SEC s in level while every composited frame
                                       is captured (--control pcap 1), then check:
        frame_motion_60hz >= checks["fm60"]  share of 60 Hz display refreshes that show a new image
                                             (0.5 for 30 fps content, 1.0 when interpolated)
        alternation <= checks["alt"]         median |d(i) - d(i+1)| / (d(i) + d(i+1)) of consecutive
                                             image changes d: ~1 when frames repeat, ~0 for even steps
        interpolating >= checks["interp"]    share of game frames RT64 generated in-between frames for
        paired >= checks["paired"]           share of transforms paired with the previous frame
        dropped <= checks["dropped"]         share of in-between frames dropped for lack of time
  ("hfr_cut",)                         pause -> RETRY: the host must log a camera cut (not interpolated)
  ("bench", SECONDS, MODE)             performance run: skate forward and turn for SECONDS of wall time with no capture; MODE off | on | naive = matrix IDs off / on / on with the first version's search
                                       (control ids), keep = leave as is, pcap = present dump of every frame, idsstat = pairing statistics running; the notes carry
                                       presented / game fps, frame-time p50 / p95 / p99 / max, frames over 20 ms and the ID walk cost (control ftime, frametime.cpp)
  ("hfr_hole", SEC)                    skate off from where the skater stands for SEC s with every composited frame captured: no presented
                                       frame may be a "hole" (the skater and half the world gone for one in-between frame: RT64 paired the
                                       camera scene with the planar-shadow scene, interpolation.cpp "projection kinds"); a hole is a frame far
                                       from the mean of its two neighbours (ratio > 1.8 and mean luma deviation > 8; holes measure 2.4-2.9 / 12-13.5,
                                       other frames <= 1.3 / 6.5). Also requires the projection-kind tags to be in use (control projids)
  ("vp_split", SEC)                    skate off from where the skater stands for SEC s: the host re-splits every combined camera load into view and
                                       projection (interpolation.cpp "view-projection split"); the route must reach loads where RT64's own split
                                       failed (camera along the world z axis: NaN, filed as orthographic) or had its depth scale off by > 1 %
                                       (THPS2 Hangar start: ~30 % of the loads), and >= 90 % of the loads must have been re-split (--hfr-vp-split 0 fails)
  ("latency", BUTTONS, N, MAX_MS)      N press-to-present measurements (--control latency) on a menu,
                                       alternating BUTTONS (e.g. "DOWN/UP"); median must be <= MAX_MS

The metric code is shared with tools/hfr_probe.py. Frames stay under build/ (ROM-derived).
"""
from __future__ import annotations

import statistics
import time
from pathlib import Path

import harness as H

KINDS = {"hfr_motion", "hfr_cut", "hfr_hole", "vp_split", "latency", "bench"}


def ids_clip() -> list[tuple]:
    """Steps for A/B clips of the matrix-ID pairing: every composited frame of ~7 s of skating (forward, a turn, an ollie, forward again)
    into <host>/build/ids/clip; THPS_IDS=0 switches the IDs off first (default on). Make the video with ffmpeg (tools/hfr_showcase.py or media/playtest-answers/hfr/)."""
    import os  # noqa: PLC0415
    clip_dir = os.path.join(os.environ["THPS_HOST"], "build", "ids", "clip")
    return [*ids_switches(), ("wait", 30),
            ("vpad", "attach"),
            ("ctl", f"pcap 1 {clip_dir}"),
            # THPS_CLIP_DUMP=N: also display-list dumps (trace + software composite) of the next N gfx tasks into <clip>-dl
            *([("ctl", f"dump {os.environ['THPS_CLIP_DUMP']} {clip_dir}-dl")] if os.environ.get("THPS_CLIP_DUMP") else []),
            ("ctl", "vpad axis lefty -32000"), ("wait", 70),
            ("ctl", "vpad axis leftx 32767"), ("wait", 50),
            ("ctl", "vpad axis leftx 0"), ("wait", 20),
            ("ctl", "vpad button a 1"), ("wait", 8), ("ctl", "vpad button a 0"), ("wait", 60),
            ("ctl", "vpad axis leftx -32767"), ("wait", 50),
            ("ctl", "pcap 0"), ("ctl", "vpad axis lefty 0"), ("ctl", "vpad axis leftx 0"), ("wait", 30)]


def ids_switches() -> list[tuple]:
    """THPS_IDS / THPS_IDS_TIER2 = 0 switch a part of the matrix-ID mechanism off for an A/B run (default all on)."""
    import os  # noqa: PLC0415
    on = lambda k: "on" if os.environ.get(k, "1") == "1" else "off"  # noqa: E731
    return [("ctl", "ids " + on("THPS_IDS")), ("ctl", "ids tier2 " + on("THPS_IDS_TIER2"))]


def ids_phases(n: int = 40, dump: bool = False) -> list[tuple]:
    """Steps for the matrix-ID pairing statistics (--control idsstat, src/interp_ids.cpp): from the first frame of a level run, the same
    five situations in every game; the reply of each idsstat goes into the notes. dump: also idsdumps into <host>/build/ids while skating (evidence)."""
    import os  # noqa: PLC0415
    calls = os.environ.get("THPS_IDS_CALLS") == "1"   # also RT64's call tables (needs an RT64 build with HFR_STATS_HAS_CALLS)
    dump_dir = os.path.join(os.environ["THPS_HOST"], "build", "ids") if dump else ""
    st = [("ctl", "idsscan 1"), *ids_switches(), ("wait", 20),
          ("ctl", f"idsstat {n}"),                                                   # standing at the start
          ("vpad", "attach"), ("ctl", "vpad axis lefty -32000"), ("wait", 45),
          ("ctl", f"idsstat {n}"),                                                   # skating forward
          ("ctl", "vpad axis leftx 32767"), ("wait", 20),
          ("ctl", f"idsstat {n}"),                                                   # turning
          *([("ctl", f"{'idscalls' if calls else 'idsdump'} {dump_dir}/ids_turn.txt 1")] if dump_dir else []),
          ("ctl", "vpad axis leftx 0"), ("ctl", "vpad button a 1"), ("wait", 6),
          ("ctl", f"idsstat {n // 2}"),                                              # ollie: in the air
          ("ctl", "vpad button a 0"), ("ctl", "vpad axis lefty 0"), ("wait", 40), ("ctl", "ids")]
    if dump_dir:
        d = "idscalls" if calls else "idsdump"   # idscalls: also RT64's call / candidate tables (needs an RT64 build with HFR_STATS_HAS_CALLS)
        st += [("ctl", "vpad axis lefty -32000"), ("wait", 30), ("ctl", f"{d} {dump_dir}/ids_a.txt 1"), ("wait", 4), ("ctl", f"{d} {dump_dir}/ids_b.txt 1"),
               ("wait", 4), ("ctl", f"{d} {dump_dir}/ids_c.txt 1"), ("ctl", "vpad axis lefty 0")]
    return st


# ---- metrics ---------------------------------------------------------------------------------
def luma_small(img: H.Image, gw: int = 160, gh: int = 90) -> list[float]:
    out = []
    for gy in range(gh):
        y = min(img.h - 1, int((gy + 0.5) * img.h / gh))
        for gx in range(gw):
            x = min(img.w - 1, int((gx + 0.5) * img.w / gw))
            r, g, b = img.px(x, y)
            out.append(0.299 * r + 0.587 * g + 0.114 * b)
    return out


def mad(a: list[float], b: list[float]) -> float:
    return sum(abs(x - y) for x, y in zip(a, b)) / max(len(a), 1)


def motion_report(frames: list[Path], diff_eps: float = 0.5, seconds: float = 0.0) -> dict:
    """Body Harvest's frame_motion (share of consecutive presents that differ), the same on a
    60 Hz display timeline, and how even the per-present image change is."""
    grids = [luma_small(H.read_png(p)) for p in frames]
    diffs = [mad(grids[i], grids[i + 1]) for i in range(len(grids) - 1)]
    differ = [d > diff_eps for d in diffs]
    idx = [int(p.name.split("_")[1]) for p in frames]
    gaps = sum(1 for i in range(len(idx) - 1) if idx[i + 1] != idx[i] + 1)
    alt = [abs(diffs[i] - diffs[i + 1]) / max(diffs[i] + diffs[i + 1], 1e-6) for i in range(len(diffs) - 1)]
    fm60 = round(min(1.0, sum(differ) / (seconds * 60)), 3) if seconds > 0 else None
    return dict(presents=len(frames), index_gaps=gaps, frame_motion_60hz=fm60,
                frame_motion=round(sum(differ) / max(len(differ), 1), 3),
                alternation=round(statistics.median(alt), 3) if alt else None,
                diffs=[round(d, 2) for d in diffs])


def wait_pngs(d: Path, n: int, timeout: float = 60) -> list[Path]:
    """PNGs in d once at least n exist and the writers have gone quiet."""
    t0 = time.monotonic()
    last = -1
    while time.monotonic() - t0 < timeout:
        files = presents(d)
        if len(files) >= n and len(files) == last:
            return files
        last = len(files)
        time.sleep(0.5)
    return presents(d)


def presents(d: Path) -> list[Path]:
    """Present captures in d, without --present-pair's native companions."""
    return sorted(p for p in d.glob("present_*.png") if "_native_" not in p.name)


def hfr_counters(inst: H.Instance) -> dict[str, int]:
    r = inst.cmd("hfr")
    return {k: int(v) for k, v in r.items() if v.lstrip("-").isdigit()}


def kv(line: str) -> dict[str, str]:
    return dict(x.split("=", 1) for x in line.split()[2:] if "=" in x)


# ---- steps -------------------------------------------------------------------------------------
def step(sess, st: tuple):
    """sess = run.Session (inst, out, notes, warnings, slow, step())."""
    from run import StepFail  # noqa: PLC0415 (circular import at module level)
    inst = sess.inst
    kind = st[0]
    if kind == "hfr_motion":
        _, seconds, stick, checks = st
        d = sess.out / "hfr_motion"
        d.mkdir(parents=True, exist_ok=True)
        for p in d.glob("present_*.png"):
            p.unlink()
        polls = int(seconds * 30)
        h0 = hfr_counters(inst)
        c0 = int(inst.cmd(f"pcap 1 {d}")["composited"])
        inst.send(f"press {stick} {polls} 0")
        inst.expect(f"ok press {stick}", 120 * sess.slow)
        c1 = int(inst.cmd("pcap 0")["composited"])
        h1 = hfr_counters(inst)
        files = wait_pngs(d, max(c1 - c0 - 2, 1), timeout=60 * sess.slow)
        if len(files) < 10:
            raise StepFail(f"hfr_motion: only {len(files)} present captures in {d}")
        rep = motion_report(files, seconds=polls / 30.0)
        dh = {k: h1[k] - h0.get(k, 0) for k in h1}
        matched = max(dh.get("matched", 0), 1)
        interp = dh.get("interpolating", 0) / matched
        paired = dh.get("paired", 0) / max(dh.get("transforms", 0), 1)
        dropped = dh.get("dropped", 0) / max(dh.get("rendered", 0), 1)
        sess.notes.append(f"hfr: {rep['presents']} presents in {seconds:.0f} s (gaps {rep['index_gaps']}), frame_motion "
                          f"{rep['frame_motion']} (60 Hz timeline {rep['frame_motion_60hz']}), alternation {rep['alternation']}; "
                          f"RT64: interpolating {interp:.0%} of {matched} game frames, paired {paired:.0%} of "
                          f"{dh.get('transforms', 0)} transforms, dropped {dropped:.1%}, render "
                          f"{dh.get('render_us', 0) / max(dh.get('workloads', 1), 1) / 1000:.2f} ms/game frame")
        bad = []
        if rep["frame_motion_60hz"] is None or rep["frame_motion_60hz"] < checks["fm60"]:
            bad.append(f"frame_motion_60hz {rep['frame_motion_60hz']} < {checks['fm60']}")
        if rep["alternation"] is None or rep["alternation"] > checks["alt"]:
            bad.append(f"alternation {rep['alternation']} > {checks['alt']}")
        if interp < checks["interp"]:
            bad.append(f"interpolating {interp:.0%} < {checks['interp']:.0%}")
        if paired < checks["paired"]:
            bad.append(f"paired {paired:.0%} < {checks['paired']:.0%}")
        if dropped > checks["dropped"]:
            msg = f"dropped {dropped:.1%} > {checks['dropped']:.0%}"
            if H.load_info()["overloaded"]:
                sess.warnings.append(msg + " (machine overloaded: warning only)")
            else:
                bad.append(msg)
        if bad:
            raise StepFail("hfr_motion: " + "; ".join(bad))
    elif kind == "bench":
        _, seconds, mode = st
        if mode in ("off", "on", "naive"):
            inst.cmd("ids " + ("on" if mode == "naive" else mode))
            inst.cmd("ids naive " + ("on" if mode == "naive" else "off"))   # naive = the first version's unbounded search (A/B of the bounded tracker)
        if mode == "pcap":
            d = sess.out / "bench_pcap"
            d.mkdir(parents=True, exist_ok=True)
            inst.cmd(f"pcap 1 {d}")
        inst.cmd("ids slowreset")
        inst.cmd("ftime start")
        t0 = time.monotonic()
        if mode == "idsstat":
            inst.cmd("vpad axis lefty -32767")
            inst.cmd("vpad axis leftx 0")
            inst.cmd("idsstat 100", timeout=120 * sess.slow)   # samples consecutive matched frames, each waiting for the next one
        else:
            pattern = [(0, -32767, 5.0), (32767, -32767, 2.5), (0, -32767, 3.0), (-32767, -32767, 2.5)]
            while time.monotonic() - t0 < seconds:
                for x, y, dur in pattern:
                    inst.cmd(f"vpad axis leftx {x}")
                    inst.cmd(f"vpad axis lefty {y}")
                    end = time.monotonic() + dur
                    while time.monotonic() < end and time.monotonic() - t0 < seconds:
                        time.sleep(0.05)
        r = inst.cmd("ftime stop", timeout=30)
        if mode == "pcap":
            inst.cmd("pcap 0")
            wait_pngs(sess.out / "bench_pcap", 1, timeout=90 * sess.slow)   # let the PNG writers finish: their backlog would slow the next block
            for p in (sess.out / "bench_pcap").glob("*.png"):
                p.unlink()
        inst.cmd("vpad axis leftx 0")
        inst.cmd("vpad axis lefty 0")
        load = H.load_info()
        inst.send("ids slow")
        sess.notes.append("benchslow " + mode + " " + inst.expect("ok ids slow", 30))
        sess.notes.append("bench " + mode + " load=" + load["load"] + " " + " ".join(f"{k}={v}" for k, v in r.items()))
    elif kind == "hfr_hole":
        _, seconds = st
        d = sess.out / "hfr_hole"
        d.mkdir(parents=True, exist_ok=True)
        for p in d.glob("present_*.png"):
            p.unlink()
        t0 = inst.cmd("projids")
        inst.cmd("vpad attach")
        c0 = int(inst.cmd(f"pcap 1 {d}")["composited"])
        inst.cmd("vpad axis lefty -32000")   # the hole showed ~20 game frames after skating off the Hangar start (camera catching up)
        inst.wait_frames(int(seconds * 30))
        c1 = int(inst.cmd("pcap 0")["composited"])
        inst.cmd("vpad axis lefty 0")
        t1 = inst.cmd("projids")
        files = wait_pngs(d, max(c1 - c0 - 2, 1), timeout=60 * sess.slow)
        if len(files) < 30:
            raise StepFail(f"hfr_hole: only {len(files)} present captures in {d}")
        L = [luma_small(H.read_png(p), 96, 72) for p in files]
        worst, holes = (0.0, 0.0, ""), []
        for i in range(1, len(L) - 1):
            dev = mad(L[i], [(a + b) / 2 for a, b in zip(L[i - 1], L[i + 1])])
            r = dev / max(mad(L[i - 1], L[i + 1]), 0.5)
            if dev > 4 and r > worst[0]:
                worst = (r, dev, files[i].name)
            if r > 1.8 and dev > 8:
                holes.append(f"{files[i].name} (ratio {r:.2f}, deviation {dev:.1f})")
        cam = int(t1.get("camera", 0)) - int(t0.get("camera", 0))
        shadow = int(t1.get("shadow", 0)) - int(t0.get("shadow", 0))
        sess.notes.append(f"hfr_hole: {len(files)} presents, worst frame {worst[2]} ratio {worst[0]:.2f} deviation {worst[1]:.1f}; "
                          f"projection tags {t1.get('state', '?')}: camera={cam} shadow={shadow}")
        if not holes:   # keep the captures of a failure only (they are ~80 MB a run)
            for p in files:
                p.unlink()
        bad = []
        if holes:
            bad.append(f"{len(holes)} hole frame(s): " + ", ".join(holes[:4]) + f" (captures kept in {d})")
        if t1.get("state") == "on" and (cam == 0 or shadow == 0):
            bad.append(f"projection kinds not tagged (camera={cam} shadow={shadow})")
        if bad:
            raise StepFail("hfr_hole: " + "; ".join(bad))
    elif kind == "vp_split":
        _, seconds = st
        def counters():
            r = inst.cmd("vpsplit")
            return r.get("state", "?"), {k: int(r.get(k, 0)) for k in ("loads", "split", "rt64_flat", "rt64_off")}
        _, c0 = counters()
        inst.cmd("vpad attach")
        inst.cmd("vpad axis lefty -32000")
        inst.wait_frames(int(seconds * 30))
        inst.cmd("vpad axis lefty 0")
        state, c1 = counters()
        d = {k: c1[k] - c0[k] for k in c1}
        sess.notes.append("vp_split: " + state + " " + " ".join(f"{k}={v}" for k, v in d.items()))
        bad = []
        if d["loads"] < 30:
            bad.append(f"only {d['loads']} projection loads seen (state {state})")
        elif d["split"] < 0.9 * d["loads"]:
            bad.append(f"{d['split']} of {d['loads']} camera loads re-split (state {state})")
        if state == "on" and d["rt64_flat"] + d["rt64_off"] == 0:
            bad.append("the route never reached a load RT64 splits badly (camera not along the z axis?)")
        if bad:
            raise StepFail("vp_split: " + "; ".join(bad))
    elif kind == "hfr_cut":
        n0 = sum(1 for l in inst.other_lines if "camera cut" in l)
        for s in [("go", "START", "in_level", "pause"), ("menu", "pause", 1), ("go", "A", "pause", "in_level")]:
            sess.step(s)
        deadline = time.monotonic() + 10 * sess.slow
        while sum(1 for l in inst.other_lines if "camera cut" in l) <= n0:
            if time.monotonic() > deadline:
                raise StepFail("hfr_cut: RETRY restarted the run but the host logged no camera cut")
            time.sleep(0.2)
        line = [l for l in inst.other_lines if "camera cut" in l][-1]
        sess.notes.append(line.replace("[thps] ", ""))
        if "not interpolated" not in line:
            raise StepFail(f"hfr_cut: cut was interpolated: {line}")
    elif kind == "latency":
        _, buttons, n, max_ms = st
        names = buttons.split("/")
        ms, presents, frames = [], [], []
        for i in range(n):
            inst.wait_frames(12)
            inst.send(f"latency {names[i % len(names)]}")
            line = inst.expect("ok latency", 60 * sess.slow)
            r = kv(line)
            ms.append(float(r["ms"]))
            presents.append(int(r["presents"]))
            frames.append(int(r["frames"]))
        med = statistics.median(ms)
        sess.notes.append(f"latency {buttons}: median {med:.1f} ms (min {min(ms):.1f}, max {max(ms):.1f}), "
                          f"presents {statistics.median(presents):g}, game frames {statistics.median(frames):g}; all ms {ms}")
        if med > max_ms:
            msg = f"latency: median {med:.1f} ms > {max_ms} ms"
            if H.load_info()["overloaded"]:
                sess.warnings.append(msg + " (machine overloaded: warning only)")
            else:
                raise StepFail(msg)
    else:
        raise ValueError(f"unknown step {st}")

