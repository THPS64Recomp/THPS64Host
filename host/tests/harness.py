"""Test harness for the THPS recomp hosts (stdlib only; shared by THPS1-3: $THPS_HOST names the game, see tests/run.py).

- Instance: runs build/cmake/<game>_recomp with --control and talks the line protocol of
  src/control.cpp (press/shot/dl/audio/stats/wait/quit), logging every output line.
- PNG decode (zlib) + small image features: region thumbnails, mean colour, average hash.
- WAV RMS.

Nothing ROM-derived lives in this file; shots, WAVs, references and logs are written under
recomp-host/build/tests/ (gitignored).
"""
from __future__ import annotations

import io
import os
import platform
import queue
import re
import struct
import subprocess
import threading
import time
import wave
import zlib
from array import array
from dataclasses import dataclass
from pathlib import Path

import tomllib

if not os.environ.get("THPS_HOST"):
    raise SystemExit("THPS_HOST is not set: run the tests through decomp/thps<N>/recomp-host/tests/<name>.py (or tools/test.sh)")
HOST_DIR = Path(os.environ["THPS_HOST"]).resolve()   # the game's recomp-host directory
with open(HOST_DIR / "game" / "game.toml", "rb") as _f:
    GAME_PROFILE = tomllib.load(_f)                   # game/game.toml
FAMILY = GAME_PROFILE["family"]                       # thps1 / thps2 / thps3
IS_WINDOWS = os.name == "nt"
EXE_NAME = f"{FAMILY}_recomp.exe" if IS_WINDOWS else f"{FAMILY}_recomp"


def find_binary() -> Path:
    """$THPS<N>_BINARY (e.g. the frontend build), else build/cmake/<game>_recomp[.exe], also in a multi-config subdirectory (Release/...)."""
    if os.environ.get(FAMILY.upper() + "_BINARY"):
        return Path(os.environ[FAMILY.upper() + "_BINARY"])
    base = HOST_DIR / "build" / "cmake"
    for d in (base, base / "Release", base / "RelWithDebInfo", base / "Debug"):
        if (d / EXE_NAME).exists():
            return d / EXE_NAME
    return base / EXE_NAME


BINARY = find_binary()


class HostError(RuntimeError):
    pass


# The graphics defaults the tests were written for. v0.2 made the Enhanced preset the fresh-install default (border fill, draw distance
# full: src/gfx_config.cpp), which would change what most sessions see (fog's default is original again, kept here so a folder an
# earlier v0.2 build saved with fog=soft can't change a session). So before a start, every key of these that the config folder's
# graphics.cfg lacks is added with the old default: a fresh folder starts as before, and an older folder (which never wrote
# draw_distance=original) keeps its meaning. A session's flags still win (they come after the file); a key already in the file stays (the
# menu sessions' reload checks need what an earlier run saved). Instance(pin_gfx=False) skips it (run.py: a session with
# fresh_gfx_defaults=True, to see the real defaults).
TEST_GFX_DEFAULTS = {"aspect": "16:9", "hud": "expand", "split": "wide", "border": "original", "refresh": "60", "draw_distance": "original",
                     "fog": "original"}


def pin_gfx_defaults(config: Path) -> list[str]:
    """Adds the TEST_GFX_DEFAULTS keys config/graphics.cfg lacks; returns the lines added."""
    f = config / "graphics.cfg"
    text = f.read_text() if f.exists() else ""
    have = set(re.findall(r"(?m)^([a-z_]+)=", text))
    add = [f"{k}={v}" for k, v in TEST_GFX_DEFAULTS.items() if k not in have]
    if add:
        config.mkdir(parents=True, exist_ok=True)
        f.write_text(text + ("\n" if text and not text.endswith("\n") else "") + "\n".join(add) + "\n")
    return add


class Instance:
    """One running <game>_recomp --control. Only one may run at a time (see run.py lock)."""

    def __init__(self, config: Path, log_path: Path, rom: str | None = None, extra: list[str] | None = None,
                 binary: Path = BINARY, line_hook=None, control_args: list[str] | None = None, pin_gfx: bool = True,
                 env: dict[str, str] | None = None):
        pinned = pin_gfx_defaults(config) if pin_gfx else []
        self.line_hook = line_hook   # called with every host output line, on the reader thread (the soak test's monitor, tests/soak.py)
        # control_args: replaces the default "--no-keyboard --no-pads --mute" (the soak test plays through a virtual pad with real audio at volume 0)
        self.log_path = log_path
        log_path.parent.mkdir(parents=True, exist_ok=True)
        args = [str(binary), "--control", "--config", str(config)] + (["--no-keyboard", "--no-pads", "--mute"] if control_args is None else control_args)
        if rom:
            args += ["--rom", rom]
        args += extra or []
        self.args = args
        self._log = open(log_path, "w", buffering=1)
        self._log.write("# " + " ".join(args) + "\n")
        if pinned:
            self._log.write("# graphics.cfg: test defaults added: " + " ".join(pinned) + "\n")
        if env:   # a session's own environment variables (run.py `env`: e.g. THPS_DD_CANARY=1), on top of this process's
            self._log.write("# env: " + " ".join(f"{k}={v}" for k, v in env.items()) + "\n")
        self._lines: "queue.Queue[str]" = queue.Queue()
        self.perf: list[tuple[float, float]] = []   # (t, fps) from "[thps] perf" lines
        self.pak_events: list[str] = []
        self.fps_overlay: list[tuple[float, float, float]] = []   # (time, FPS, Game) from "[thps] fps overlay" lines
        self.other_lines: list[str] = []                          # "[thps] skip intro" / "[thps] input" / "[thps] cheats" ... lines
        self.last_press_polls = 0                                 # polls= of the last press (press_after)
        self.watch_polls: int | None = None                       # poll= of the last "ok watch" line (src/control.cpp watch), None until it fires
        self.watch_event = threading.Event()
        self.start_time = time.monotonic()
        # Binary pipes wrapped as text: the protocol is "\n"-terminated lines on every OS (a text-mode pipe would write
        # "\r\n" on Windows).
        self.proc = subprocess.Popen(args, cwd=HOST_DIR, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
                                     env={**os.environ, **env} if env else None)
        self.proc.stdin = io.TextIOWrapper(self.proc.stdin, encoding="utf-8", newline="\n", write_through=True)
        self.proc.stdout = io.TextIOWrapper(self.proc.stdout, encoding="utf-8", errors="replace", newline="\n")
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()
        self.expect("ok ready", timeout=30)

    _fps_re = re.compile(r"^\[thps\] fps overlay: ([\d.]+) FPS  \|  Game ([\d.]+)")
    _watch_re = re.compile(r"^\[ctl\] ok watch \S+ poll=(\d+)")
    _perf_re = re.compile(r"^\[thps\] perf t=([\d.]+)s .*? fps=([\d.]+)")

    def _read(self):
        assert self.proc.stdout
        for line in self.proc.stdout:
            line = line.rstrip("\r\n")
            self._log.write(line + "\n")
            if self.line_hook:
                try:
                    self.line_hook(line)
                except Exception:   # noqa: BLE001  (a monitor bug must not kill the reader)
                    pass
            m = self._perf_re.match(line)
            if m:
                self.perf.append((float(m.group(1)), float(m.group(2))))
            if line.startswith("[thps] pak"):
                self.pak_events.append(line)
            m = self._fps_re.match(line)
            if m:
                self.fps_overlay.append((time.monotonic() - self.start_time, float(m.group(1)), float(m.group(2))))
            if line.startswith(("[thps] start:", "[thps] skip intro", "[thps] input", "[thps] launcher", "[thps] check-rom", "[thps] gfx:", "[thps] settings:", "[thps] hfr camera cut", "[thps] cheats", "[thps] draw distance", "[thps] music")):
                self.other_lines.append(line)
            m = self._watch_re.match(line)
            if m:   # answered by the game thread at the poll where the word changed: kept aside so expect() never swallows it
                self.watch_polls = int(m.group(1))
                self.watch_event.set()
            if line.startswith("[ctl] "):
                self._lines.put(line[6:])
        rc = self.proc.wait()
        self._log.write(f"# [runner] host exited with code {rc}\n")
        self._lines.put("__eof__")

    def note(self, text: str):
        self._log.write(f"# [runner {time.monotonic() - self.start_time:7.2f}s] {text}\n")

    def alive(self) -> bool:
        return self.proc.poll() is None

    def send(self, line: str):
        if not self.alive():
            raise HostError(f"host exited (code {self.proc.returncode}); see {self.log_path}")
        assert self.proc.stdin
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def expect(self, prefix: str, timeout: float) -> str:
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise HostError(f"timeout ({timeout:.0f}s) waiting for '{prefix}'; see {self.log_path}")
            try:
                line = self._lines.get(timeout=min(left, 1.0))
            except queue.Empty:
                if not self.alive():
                    raise HostError(f"host exited (code {self.proc.returncode}) waiting for '{prefix}'")
                continue
            if line == "__eof__":
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
                raise HostError(f"host output ended (exit {self.proc.returncode}) waiting for '{prefix}'; see {self.log_path}")
            if line.startswith(prefix):
                return line
            if line.startswith("err"):
                raise HostError(line)

    def cmd(self, line: str, timeout: float = 30) -> dict[str, str]:
        name = line.split()[0]
        self.send(line)
        reply = self.expect(f"ok {name}", timeout)
        return dict(kv.split("=", 1) for kv in reply.split()[2:] if "=" in kv)

    # Convenience wrappers ---------------------------------------------------------------
    def press(self, buttons: str, hold: int = 3, gap: int = 3, timeout: float = 60):
        self.note(f"press {buttons} {hold} {gap}")
        r = self.cmd(f"press {buttons} {hold} {gap}", timeout)
        self.last_press_polls = int(r.get("polls", 0))
        return r

    def shot(self, path: Path, timeout: float = 30) -> dict[str, str]:
        path.parent.mkdir(parents=True, exist_ok=True)
        return self.cmd(f"shot {path}", timeout)

    def watch(self, addr: str, value: int):
        """Arm `watch ADDR VALUE` (host game thread: the input poll count at which the RDRAM word first equals VALUE); see wait_watch."""
        self.watch_event.clear()
        self.watch_polls = None
        self.note(f"watch {addr} {value}")
        self.send(f"watch {addr} {value}")

    def wait_watch(self, timeout: float) -> int | None:
        """The poll count the armed watch fired at, or None after `timeout` seconds."""
        if not self.watch_event.wait(timeout):
            return None
        return self.watch_polls

    def wait_frames(self, n: int, timeout: float = 120):
        return self.cmd(f"wait {n}", timeout)

    def stats(self):
        return self.cmd("stats")

    def quit(self, timeout: float = 15):
        try:
            if self.alive():
                self.send("quit")
                self.proc.wait(timeout=timeout)
        except Exception:
            pass
        finally:
            if self.alive():
                self.proc.kill()
                self.proc.wait()
            self._reader.join(timeout=5)
            self._log.close()


# ---------------------------------------------------------------------------------------
# PNG (stdlib): enough for the host's own writer and ordinary 8-bit RGB/RGBA files.

@dataclass
class Image:
    w: int
    h: int
    rgb: bytes  # w*h*3

    def px(self, x: int, y: int) -> tuple[int, int, int]:
        i = 3 * (y * self.w + x)
        return self.rgb[i], self.rgb[i + 1], self.rgb[i + 2]


def read_png(path: Path) -> Image:
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, w = 8, [], 0
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError(f"{path}: unsupported PNG (depth {depth}, type {ctype})")
            bpp = 4 if ctype == 6 else 3
        elif typ == b"IDAT":
            idat.append(body)
        elif typ == b"IEND":
            break
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if bpp == 4:
        rgb = bytearray(w * h * 3)
        rgb[0::3] = out[0::4]
        rgb[1::3] = out[1::4]
        rgb[2::3] = out[2::4]
        return Image(w, h, bytes(rgb))
    return Image(w, h, bytes(out))


def thumb(img: Image, region=(0.0, 0.0, 1.0, 1.0), gw: int = 16, gh: int = 12) -> list[tuple[float, float, float]]:
    """Box-averaged RGB grid of a region given in fractions of the image (resolution-independent)."""
    x0, y0 = int(region[0] * img.w), int(region[1] * img.h)
    x1, y1 = max(x0 + 1, int(region[2] * img.w)), max(y0 + 1, int(region[3] * img.h))
    cells = []
    for gy in range(gh):
        ya, yb = y0 + (y1 - y0) * gy // gh, y0 + (y1 - y0) * (gy + 1) // gh
        for gx in range(gw):
            xa, xb = x0 + (x1 - x0) * gx // gw, x0 + (x1 - x0) * (gx + 1) // gw
            r = g = b = n = 0
            # Subsample every 2nd pixel of big cells: plenty for a 16x12 grid, 4x faster in pure Python.
            step = 2 if (xb - xa) >= 6 and (yb - ya) >= 6 else 1
            for y in range(ya, max(ya + 1, yb), step):
                row = 3 * y * img.w
                for x in range(xa, max(xa + 1, xb), step):
                    i = row + 3 * x
                    r += img.rgb[i]; g += img.rgb[i + 1]; b += img.rgb[i + 2]; n += 1
            cells.append((r / n, g / n, b / n))
    return cells


def thumb_dist(a, b) -> float:
    """Mean absolute RGB difference between two thumbnails (0..255)."""
    return sum(abs(p - q) for ca, cb in zip(a, b) for p, q in zip(ca, cb)) / (3 * len(a))


def ahash(img: Image, region=(0.0, 0.0, 1.0, 1.0)) -> int:
    cells = [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in thumb(img, region, 8, 8)]
    mean = sum(cells) / len(cells)
    return sum(1 << i for i, v in enumerate(cells) if v > mean)


def hamming(a: int, b: int) -> int:
    return bin(a ^ b).count("1")


def mean_rgb(img: Image, region=(0.0, 0.0, 1.0, 1.0)) -> tuple[float, float, float]:
    cells = thumb(img, region, 4, 4)
    return tuple(sum(c[i] for c in cells) / len(cells) for i in range(3))


# ---------------------------------------------------------------------------------------

def wav_rms_dbfs(path: Path, start_s: float = 0.0, end_s: float | None = None) -> tuple[float, float]:
    """(RMS dBFS, fraction of 50 ms windows above -50 dBFS) of a 16-bit WAV section."""
    import math
    with wave.open(str(path)) as w:
        rate, ch = w.getframerate(), w.getnchannels()
        n = w.getnframes()
        a = int(start_s * rate)
        b = n if end_s is None else min(n, int(end_s * rate))
        w.setpos(min(a, n))
        samples = array("h", w.readframes(max(0, b - a)))
    if not samples:
        return -120.0, 0.0
    win = int(0.05 * rate) * ch
    loud = total = 0
    sq_all = 0.0
    for i in range(0, len(samples) - win + 1, win):
        sq = sum(s * s for s in samples[i:i + win])
        sq_all += sq
        total += 1
        if sq / win > (32768 * 10 ** (-50 / 20)) ** 2:
            loud += 1
    rms = math.sqrt(sq_all / max(1, total * win)) / 32768
    return (20 * math.log10(rms) if rms > 0 else -120.0), loud / max(1, total)


def load_info() -> dict:
    ncpu = os.cpu_count() or 1
    if hasattr(os, "getloadavg"):
        la = os.getloadavg()
        up = subprocess.run(["uptime"], capture_output=True, text=True).stdout.strip()
        return {"uptime": up, "load": f"{la[0]:.1f}", "load1": la[0], "load5": la[1], "ncpu": ncpu, "overloaded": la[0] > 2 * ncpu}
    # Windows has neither: say so, and record the machine instead (no load warning). "load" is the text the notes print.
    return {"uptime": f"load n/a ({platform.platform()}, {platform.processor() or platform.machine()}, {ncpu} CPUs)", "load": "n/a",
            "load1": 0.0, "load5": 0.0, "ncpu": ncpu, "overloaded": False}


def write_png(path: Path, img: Image):
    raw = b"".join(b"\x00" + img.rgb[3 * y * img.w:3 * (y + 1) * img.w] for y in range(img.h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", img.w, img.h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def contact_sheet(paths: list[Path], out: Path, cell_w: int = 160, cols: int = 6):
    """Tile downscaled frames (nearest) into one PNG, for eyeballing references and failures."""
    cell_h = cell_w * 3 // 4
    rows = (len(paths) + cols - 1) // cols
    W, H = cols * cell_w, rows * cell_h
    buf = bytearray(W * H * 3)
    for k, p in enumerate(paths):
        img = read_png(p)
        ox, oy = (k % cols) * cell_w, (k // cols) * cell_h
        for y in range(cell_h - 2):
            sy = y * img.h // (cell_h - 2)
            for x in range(cell_w - 2):
                sx = x * img.w // (cell_w - 2)
                i = 3 * (sy * img.w + sx)
                j = 3 * ((oy + y) * W + ox + x)
                buf[j:j + 3] = img.rgb[i:i + 3]
    write_png(out, Image(W, H, bytes(buf)))


def luma_grid(img: Image, region, gw: int, gh: int) -> list[float]:
    return [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in thumb(img, region, gw, gh)]


def norm_dist(a: list[float], b: list[float]) -> float:
    """Distance between two luma grids after normalising each to zero mean / unit std:
    insensitive to fades and brightness, 0 = same pattern, ~1.1 = unrelated. A flat grid
    (black frame) never matches."""
    import math
    def norm(v):
        m = sum(v) / len(v)
        sd = math.sqrt(sum((x - m) ** 2 for x in v) / len(v))
        return None if sd < 3 else [(x - m) / sd for x in v]
    na, nb = norm(a), norm(b)
    if na is None or nb is None:
        return 9.0
    return sum(abs(x - y) for x, y in zip(na, nb)) / len(na)


def gold(r: int, g: int, b: int) -> bool:
    """Highlighted menu entry (gold text and the red cursor arrows, or THPS3's orange bar); not white/blue text."""
    # THPS3's highlighted row is an orange bar (about 168, 65, 16) behind white text; THPS1/2 used gold text.
    return (r > 170 and g > 110 and b < 120 and r - b > 80) or (r > 140 and 45 < g < 100 and b < 40)


def highlight_band(img: Image, y_range=(0.2, 0.85), x_range=(0.3, 0.7)) -> float | None:
    """Centre (fraction of height) of the strongest band of highlighted text, or None."""
    x0, x1 = int(x_range[0] * img.w), int(x_range[1] * img.w)
    y0, y1 = int(y_range[0] * img.h), int(y_range[1] * img.h)
    prof = []
    for y in range(y0, y1):
        row = 3 * y * img.w
        rgb = img.rgb
        prof.append(sum(1 for x in range(x0, x1) if gold(rgb[row + 3 * x], rgb[row + 3 * x + 1], rgb[row + 3 * x + 2])))
    best, best_c, y = 0, None, 0
    while y < len(prof):
        if prof[y] > 3:
            s, tot = y, 0
            while y < len(prof) and prof[y] > 3:
                tot += prof[y]
                y += 1
            if tot > best:
                best, best_c = tot, (y0 + (s + y) / 2) / img.h
        y += 1
    return best_c if best >= 40 * img.w / 640 else None


def mask_score(img: Image, ref: Image, region, luma_min: float = 150) -> float:
    """Template match for overlays drawn on a moving background (HUD text): the fraction of the
    reference's bright pixels (luma >= luma_min) inside `region` that have (nearly) the same
    colour in img. 1.0 = all overlay pixels present."""
    if (img.w, img.h) != (ref.w, ref.h):
        return 0.0
    x0, y0 = int(region[0] * img.w), int(region[1] * img.h)
    x1, y1 = int(region[2] * img.w), int(region[3] * img.h)
    n = hit = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = ref.px(x, y)
            if 0.299 * r + 0.587 * g + 0.114 * b < luma_min:
                continue
            n += 1
            r2, g2, b2 = img.px(x, y)
            if abs(r - r2) + abs(g - g2) + abs(b - b2) < 60:
                hit += 1
    return hit / n if n else 0.0


def region_ratio(img: Image, ref: Image, spec) -> float:
    """distance/tolerance for one checkpoint region spec (<= 1 = match):
    (region, gw, gh, tol) = normalised luma grid; (region, "mask", luma_min, min_frac) = mask_score."""
    if spec[1] == "mask":
        region, _, luma_min, min_frac = spec
        s = mask_score(img, ref, region, luma_min)
        return (1 - s) / (1 - min_frac)
    if spec[1] == "color":
        # (region, "color", (rmin, rmax, gmin, gmax, bmin, bmax), min_frac): at least min_frac of the region's pixels have
        # a colour inside the box (a fixed-colour HUD element on a moving background); ratio <= 1 = match.
        region, _, (r0, r1, g0, g1, b0, b1), min_frac = spec
        x0, y0, x1, y1 = int(region[0] * img.w), int(region[1] * img.h), int(region[2] * img.w), int(region[3] * img.h)
        n = hit = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                r, g, b = img.px(x, y)
                n += 1
                if r0 <= r <= r1 and g0 <= g <= g1 and b0 <= b <= b1:
                    hit += 1
        frac = hit / n if n else 0.0
        return min(9.0, min_frac / max(frac, 1e-9))
    region, gw, gh, tol = spec
    return norm_dist(luma_grid(img, region, gw, gh), luma_grid(ref, region, gw, gh)) / tol
