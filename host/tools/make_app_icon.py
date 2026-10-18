#!/usr/bin/env python3
"""Draw the app icon of one game and write .icns / .ico / .png: the game's hand-drawn SVG, or a procedural look.

Original art only: nothing from a game, no logos, no game fonts. Pure standard library for the drawing and the file formats (no
Pillow/numpy, no SVG library: build machines have only Python); `iconutil` (macOS only) turns the iconset into the .icns, and the
.ico is written by hand (PNG-compressed entries, Windows Vista+). This runs at build/package time on our machines; users get the
finished .icns / the .ico embedded in the exe and nothing runs on their side.

The game comes from its profile (game/game.toml): `icon_svg` ("icon.svg", relative to the profile) is the default look, drawn by
the small SVG renderer below (a subset; anything else is an error). Without it, `icon_variant` picks a procedural look, signed
distance shapes with the numeral from `icon_number` (default: the digits of `family`). Three procedural looks (previews: media/icons/):
  deck  - a board seen from below (kicktails, trucks, wheels, a bit of perspective) on a warm gradient, number badge  [default]
  neon  - the same board as a glowing outline on a dark tile, number badge
  big   - the number as the hero, a small board underneath
`icon_svg_small` (optional) is a simplified drawing used at SMALL_ICON_MAX_PX (48) and below.
Sizes: the .ico renders the SVG as drawn (full canvas) at Microsoft's 16-256 px list; the .icns scales it onto Apple's icon grid
(MACOS_TILE_FRACTION) and has the full iconset, 16/32/128/256/512 at @1x and @2x.

usage: python3 -I make_app_icon.py [--game-toml game/game.toml | --number N] [--variant deck|neon|big | --svg FILE]
                                   [--icns OUT.icns] [--ico OUT.ico] [--png OUT.png --size 1024 [--macos]] [--preview-sheet OUT.png]
                                   [--svg-overview OUT.png --svg A --svg B ...]
With no --game-toml/--number the game is $THPS_HOST/game/game.toml (the wrapper decomp/thpsN/recomp-host/tools/make_app_icon.py sets it).
Precedence: --svg, then --variant, then the profile's icon_svg, then its icon_variant.
"""
import argparse
import math
import multiprocessing
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib
import xml.etree.ElementTree as ET
import zlib
from array import array
from itertools import accumulate

VARIANTS = ("deck", "neon", "big")
DEFAULT_VARIANT = "deck"
ICNS_SIZES = [(16, 1), (16, 2), (32, 1), (32, 2), (128, 1), (128, 2), (256, 1), (256, 2), (512, 1), (512, 2)]   # (points, scale)
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]   # Microsoft's list for crisp scaling at every DPI (+128)
# At this size and below, a profile's `icon_svg_small` (a simplified drawing: no trucks/rim/shadow, a big number badge) replaces
# `icon_svg`. Why: the full drawing's badge is ~20 % of the tile, so its numeral is unreadable at 32 px and gone at 16 px; it reads
# from 64 px up. Both Apple and Microsoft recommend simplified artwork for the smallest sizes. macOS uses it for 16, 16@2x and 32;
# Windows for 16-48 (Explorer lists, title bar, taskbar).
SMALL_ICON_MAX_PX = 48

INK = (0.07, 0.08, 0.15)
CREAM = (0.98, 0.95, 0.86)
GREY = (0.72, 0.75, 0.82)
GREY_D = (0.45, 0.48, 0.58)

# Per game number: tile gradient (top, bottom), the deck's stripe colour, the neon colour.
PALETTES = {
    1: dict(top=(1.00, 0.74, 0.20), bot=(0.92, 0.22, 0.20), stripe=(0.08, 0.55, 0.64), neon=(1.00, 0.72, 0.25)),
    2: dict(top=(0.22, 0.88, 0.72), bot=(0.04, 0.36, 0.62), stripe=(1.00, 0.46, 0.18), neon=(0.25, 0.95, 0.80)),
    3: dict(top=(0.66, 0.48, 1.00), bot=(0.30, 0.12, 0.60), stripe=(1.00, 0.83, 0.18), neon=(0.80, 0.55, 1.00)),
}


# ---------------------------------------------------------------- shapes (signed distances, y down)

def sd_box(px, py, hx, hy, r):
    qx, qy = abs(px) - hx + r, abs(py) - hy + r
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def sd_seg(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = 0.0 if t < 0.0 else 1.0 if t > 1.0 else t
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def sd_arc(px, py, cx, cy, r, a0, sweep):
    """Distance to a circular arc (centre, radius, start angle and sweep in degrees; y down, so +angle turns clockwise)."""
    dx, dy = px - cx, py - cy
    ang = (math.degrees(math.atan2(dy, dx)) - a0) % 360.0
    if ang <= sweep:
        return abs(math.hypot(dx, dy) - r)
    e = []
    for a in (a0, a0 + sweep):
        e.append(math.hypot(px - (cx + r * math.cos(math.radians(a))), py - (cy + r * math.sin(math.radians(a)))))
    return min(e)


def arc_end(cx, cy, r, a):
    return cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))


def digit_parts(n):
    """The numeral n (1-3) as strokes in a box of height 1 centred on 0: ('s', x0, y0, x1, y1) or ('a', cx, cy, r, a0, sweep)."""
    if n == 1:
        return [("s", -0.22, -0.20, 0.06, -0.5), ("s", 0.06, -0.5, 0.06, 0.5), ("s", -0.22, 0.5, 0.34, 0.5)]
    if n == 2:
        ex, ey = arc_end(0, -0.20, 0.29, 405)
        return [("a", 0, -0.20, 0.29, 190, 215), ("s", ex, ey, -0.30, 0.5), ("s", -0.30, 0.5, 0.32, 0.5)]
    if n == 3:
        return [("a", 0, -0.235, 0.255, 200, 250), ("a", 0, 0.245, 0.265, 270, 250), ("s", -0.11, 0.0, 0.02, 0.0)]
    # 4+ : not used by the three games; show a plain bar so a new profile still gets an icon
    return [("s", 0.0, -0.5, 0.0, 0.5)]


def sd_digit(parts, x, y):
    d = 1e9
    for p in parts:
        if p[0] == "s":
            v = sd_seg(x, y, p[1], p[2], p[3], p[4])
        else:
            v = sd_arc(x, y, p[1], p[2], p[3], p[4], p[5])
        if v < d:
            d = v
    return d


# ---------------------------------------------------------------- the picture

def mix(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def scale_c(c, k):
    return (min(c[0] * k, 1.0), min(c[1] * k, 1.0), min(c[2] * k, 1.0))


def smooth(t):
    t = 0.0 if t < 0.0 else 1.0 if t > 1.0 else t
    return t * t * (3.0 - 2.0 * t)


class Canvas:
    """Premultiplied accumulator: layers are laid over in order with a coverage from the signed distance."""
    __slots__ = ("r", "g", "b", "a", "k")

    def __init__(self, k):
        self.r = self.g = self.b = self.a = 0.0
        self.k = k            # pixels per unit

    def over(self, col, alpha):
        if alpha <= 0.0:
            return
        if alpha > 1.0:
            alpha = 1.0
        ia = 1.0 - alpha
        self.r = col[0] * alpha + self.r * ia
        self.g = col[1] * alpha + self.g * ia
        self.b = col[2] * alpha + self.b * ia
        self.a = alpha + self.a * ia

    def cov(self, d):
        c = 0.5 - d * self.k
        return 0.0 if c < 0.0 else 1.0 if c > 1.0 else c


def make_style(number, variant):
    pal = PALETTES.get(number) or PALETTES[(number - 1) % 3 + 1]
    st = dict(variant=variant, pal=pal)
    if variant == "neon":
        st.update(top=(0.06, 0.07, 0.17), bot=(0.17, 0.09, 0.30), deck=(0.07, 0.08, 0.18), deck_end=(0.11, 0.12, 0.26),
                  stripe=pal["neon"], line=pal["neon"], wheel=pal["neon"], badge=pal["neon"], badge_ring=(1, 1, 1),
                  digit=INK, seam=pal["neon"], glow=pal["neon"])
    else:
        st.update(top=pal["top"], bot=pal["bot"], deck=CREAM, deck_end=scale_c(CREAM, 0.9), stripe=pal["stripe"], line=INK,
                  wheel=(1.0, 0.93, 0.62), badge=INK, badge_ring=CREAM, digit=(1, 1, 1), seam=INK, glow=None)
    return st


def layout(variant):
    """Where the board and the numeral go: board centre (u, v), angle in degrees, scale; numeral centre, height, kind."""
    if variant == "big":
        return dict(bc=(0.50, 0.745), ba=-12.0, bs=0.74, nc=(0.50, 0.385), nh=0.43, kind="hero")
    return dict(bc=(0.465, 0.455), ba=-32.0, bs=1.0, nc=(0.735, 0.735), nh=0.215, kind="badge")


def shade(u, v, k, st, lo):
    """Colour at u,v in [0,1]^2: premultiplied (r, g, b, a). k = pixels per unit, lo = low-detail (tiny sizes)."""
    cv = Canvas(k)
    lay = layout(st["variant"])
    # --- the tile (rounded square with a margin, as the macOS icon grid) and its shadow
    half, rad = 0.43, 0.19
    dt = sd_box(u - 0.5, v - 0.5, half, half, rad)
    if not lo:
        ds = sd_box(u - 0.5, v - 0.5 - 0.012, half, half, rad)
        cv.over((0, 0, 0), 0.30 * smooth(1.0 - ds / 0.025) if ds > 0 else 0.30)
    tile_a = cv.cov(dt)
    if tile_a <= 0.0 and dt > 0.05:
        return (cv.r, cv.g, cv.b, cv.a)
    t = (v - 0.07) / 0.86
    col = mix(st["top"], st["bot"], min(max(t, 0.0), 1.0))
    # a soft light from the top left, and (full detail) faint rays from behind the board
    gl = max(0.0, 1.0 - math.hypot(u - 0.30, v - 0.18) / 0.65)
    col = mix(col, (1, 1, 1), 0.18 * gl * gl)
    if not lo and st["variant"] != "neon":
        ang = math.atan2(v - lay["bc"][1], u - lay["bc"][0])
        ray = smooth(abs(((ang * 12.0 / math.pi) % 1.0) - 0.5) * 6.0 - 0.8)
        col = mix(col, (1, 1, 1), 0.07 * ray)
    cv.over(col, tile_a)
    # all the rest is clipped to the tile: draw into a second canvas and lay it over with the tile coverage
    top = Canvas(k)
    draw_board(top, u, v, st, lay, lo)
    draw_number(top, u, v, st, lay, lo)
    ia = 1.0 - top.a * tile_a
    # `top` holds premultiplied colours; clip by the tile coverage
    cv.r = top.r * tile_a + cv.r * ia
    cv.g = top.g * tile_a + cv.g * ia
    cv.b = top.b * tile_a + cv.b * ia
    cv.a = top.a * tile_a + cv.a * ia
    return (cv.r, cv.g, cv.b, cv.a)


def draw_board(cv, u, v, st, lay, lo):
    variant = st["variant"]
    ang = math.radians(lay["ba"])
    ca, sa = math.cos(ang), math.sin(ang)
    sc = lay["bs"] * (1.10 if lo else 1.0)
    dx, dy = (u - lay["bc"][0]) / sc, (v - lay["bc"][1]) / sc

    def to_local(px, py):
        rx = px * ca + py * sa
        ry = -px * sa + py * ca
        f = 1.0 - 0.22 * rx / 0.40           # the +x end is farther away: slight perspective (f < 1 is nearer... see below)
        return rx, ry / f, f

    # silhouette first: nothing to do far from the board
    rx, ry, f = to_local(dx, dy)
    LH, WH = 0.40, (0.125 if lo else 0.105)
    if abs(rx) > LH + 0.12 or abs(ry) > WH + 0.14:
        return
    ow = (0.017 if lo else 0.011)                                     # ink outline width
    line = st["line"]
    neon = variant == "neon"
    ks = sc / 1.0                                                      # local distance -> unit distance
    def cov(d):                                                        # d in local units
        return cv.cov(d * f * ks)
    # glow (neon)
    if neon and not lo:
        dd = sd_box(rx, ry, LH, WH, 0.095) * f * ks
        if dd > 0:
            cv.over(st["glow"], 0.35 * math.exp(-dd / 0.022))
    # drop shadow under the board (flat look)
    if not neon and not lo:
        sx, sy = dx - 0.012 / sc, dy - 0.030 / sc
        srx = sx * ca + sy * sa
        sry = (-sx * sa + sy * ca) / (1.0 - 0.22 * srx / 0.40)
        ds = sd_box(srx, sry, LH, WH, 0.095) * f * ks
        cv.over((0.0, 0.0, 0.0), 0.28 * smooth(1.0 - ds / 0.03))
    # wheels poke out of the deck's sides (drawn first: the deck's edge overlaps them a little)
    for ax in (-0.255, 0.255):
        dwx = rx - ax
        for side in (-1, 1):
            wy = side * (WH + 0.012)
            hx, hy = (0.050, 0.036) if lo else (0.056, 0.040)
            dwheel = sd_box(dwx, ry - wy, hx, hy, 0.032)
            if dwheel < 0.05:
                if not neon and not lo:
                    cv.over(line, cov(dwheel - ow))
                cv.over(st["wheel"], cov(dwheel))
                if not lo and not neon:
                    cv.over((0.85, 0.74, 0.40), cov(sd_box(dwx, ry - wy, 0.040, 0.026, 0.020)))
    # the deck: outline, body, kicktails
    dd = sd_box(rx, ry, LH, WH, 0.095)
    iw = 0.016 if neon else ow
    cv.over(line, cov(dd))
    cv.over(st["deck"], cov(dd + iw))
    inner = dd + iw
    if inner < 0.0:
        end = abs(rx) - 0.292
        cv.over(st["deck_end"], cov(-end) * cov(inner))      # the kicked ends: another tone beyond the bend line
        if not lo:
            cv.over(st["seam"], cov(abs(end) - 0.0065) * cov(inner) * (0.9 if neon else 0.5))
        if lo:
            cv.over(st["stripe"], cov(abs(ry) - 0.032) * cov(inner) * cov(abs(rx) - 0.30))
        else:
            for sy in (-0.052, 0.052):
                cv.over(st["stripe"], cov(abs(ry - sy) - 0.011) * cov(inner) * cov(abs(rx) - 0.292))
            cv.over(st["stripe"], cov(math.hypot(rx, ry) - 0.058) * cov(inner))
            cv.over(st["deck"], cov(math.hypot(rx, ry) - 0.040) * cov(inner))
            cv.over(st["stripe"], cov(math.hypot(rx, ry) - 0.018) * cov(inner))
    # trucks: base plate on the deck + the axle hanger between the wheels
    for ax in (-0.255, 0.255):
        dwx = rx - ax
        if lo:
            dax = sd_box(dwx, ry, 0.020, WH + 0.012, 0.010)
            cv.over(line, cov(dax - ow * 0.7))
            cv.over(GREY, cov(dax))
            continue
        dax = sd_box(dwx, ry, 0.012, WH + 0.006, 0.006)
        cv.over(line, cov(dax - ow * 0.8))
        cv.over(GREY_D, cov(dax))
        dbp = sd_box(dwx, ry, 0.038, 0.070, 0.012)
        cv.over(line, cov(dbp - ow * 0.9))
        cv.over(GREY, cov(dbp))
        for by in (-0.045, 0.045):
            for bx in (-0.020, 0.020):
                cv.over(INK, cov(math.hypot(dwx - bx, ry - by) - 0.0085))
        dkp = sd_box(dwx, ry, 0.016, 0.026, 0.012)
        cv.over(GREY_D, cov(dkp))


def draw_number(cv, u, v, st, lay, lo):
    n = st["number"]
    cx, cy = lay["nc"]
    h = lay["nh"] * (1.28 if lo and lay["kind"] == "badge" else 1.0)
    if lay["kind"] == "badge":
        if lo:
            cx, cy = 0.685, 0.685
        r = h * 0.90
        d = math.hypot(u - cx, v - cy) - r
        if d > 0.03:
            return
        ring = r * 0.12
        cv.over(st["badge_ring"], cv.cov(d))
        cv.over(st["badge"], cv.cov(d + ring))
        gh = h * 1.06
        stroke = 0.20 if not lo else 0.24
        dd = sd_digit(st["parts"], (u - cx) / gh, (v - cy) / gh) * gh - stroke * gh * 0.5
        cv.over(st["digit"], cv.cov(dd))
    else:
        gh = h
        x, y = (u - cx) / gh, (v - cy) / gh
        stroke = 0.21
        if abs(x) > 0.9 or abs(y) > 0.85:
            return
        dd = sd_digit(st["parts"], x, y) * gh - stroke * gh * 0.5
        # soft shadow, ink outline, fill
        sdd = sd_digit(st["parts"], x - 0.02 / gh, y - 0.035 / gh) * gh - stroke * gh * 0.5
        if st["variant"] != "neon":
            cv.over((0, 0, 0), 0.30 * smooth(1.0 - sdd / 0.03))
        ow = 0.020 if not lo else 0.03
        cv.over(INK, cv.cov(dd - ow))
        cv.over((1, 1, 1) if st["variant"] != "neon" else st["badge"], cv.cov(dd))


# ---------------------------------------------------------------- SVG icons (a small renderer for the subset our icons use)
#
# The game icons are drawn by hand as SVG (game/icon.svg, named by `icon_svg` in game.toml). No build machine has an SVG renderer
# (Windows and CI have only Python), so this is a distance-field one, like the procedural looks above, for exactly the subset those
# files use and nothing else: anything it does not know raises SvgError instead of silently drawing something different.
#   svg       viewBox (square); width/height only as "100%"
#   g         transform (rotate / translate / scale / matrix, composed; rotation, translation and uniform scale only), clip-path, filter
#   rect      x y width height rx (rx = ry)          circle  cx cy r          line  x1 y1 x2 y2 (stroke-linecap round)
#   polygon   points                                  path    d with absolute M, L, Z only
#   paint     fill, stroke, stroke-width (hex colours or none; inherited from g); a stroked polygon/path needs stroke-linejoin="round"
#   defs      clipPath with one rect child; filter with one feDropShadow (dx dy stdDeviation flood-color flood-opacity), on a g
# Shapes are painted in document order with one analytic anti-aliased coverage each (fills from the signed distance, strokes as a
# box-filtered band around the outline), so there is no supersampling. The drop shadow is the filtered group's alpha, blurred (the
# SVG spec's three box passes), offset and tinted, painted under the group; it is computed at most SVG_MASK_MAX pixels wide and
# sampled bilinearly (a blur sigma of several pixels is smooth), which keeps the 1024 px render at a few seconds.

SVG_NS = "{http://www.w3.org/2000/svg}"
SVG_MASK_MAX = 256
# Apple's icon grid: the rounded tile fills about 824 of the 1024 canvas, the rest is transparent margin that the Dock and Finder
# size their icons for. An icon whose tile fills the whole canvas (as our SVG does, for Windows) looks too big next to the others,
# so the macOS .icns scales the whole drawing about its centre until the tile's outer edge (its stroke included) spans this fraction.
MACOS_TILE_FRACTION = 824 / 1024

_PRESENTATION = {"fill", "stroke", "stroke-width", "stroke-linecap", "stroke-linejoin"}
_SVG_ATTRS = {
    "svg": {"viewBox", "width", "height"},
    "defs": set(), "clipPath": {"id"}, "filter": {"id", "x", "y", "width", "height"},
    "feDropShadow": {"dx", "dy", "stdDeviation", "flood-color", "flood-opacity"},
    "g": _PRESENTATION | {"id", "transform", "clip-path", "filter"},
    "rect": _PRESENTATION | {"id", "transform", "clip-path", "x", "y", "width", "height", "rx"},
    "circle": _PRESENTATION | {"id", "transform", "clip-path", "cx", "cy", "r"},
    "line": _PRESENTATION | {"id", "transform", "clip-path", "x1", "y1", "x2", "y2"},
    "polygon": _PRESENTATION | {"id", "transform", "clip-path", "points"},
    "path": _PRESENTATION | {"id", "transform", "clip-path", "d"},
}
_SVG_DEFAULTS = {"fill": "#000000", "stroke": "none", "stroke-width": "1", "stroke-linecap": "butt", "stroke-linejoin": "miter"}
K_RECT, K_CIRCLE, K_POLY, K_SHADOW = 0, 1, 2, 3


class SvgError(Exception):
    pass


def _svg_num(s, what):
    try:
        return float(s)
    except (TypeError, ValueError):
        raise SvgError(f"{what}: {s!r} is not a plain number (units and percentages are not supported)") from None


def _svg_nums(s, what):
    return [_svg_num(t, what) for t in re.split(r"[\s,]+", s.strip()) if t]


def _svg_color(s):
    if s == "none":
        return None
    m = re.fullmatch(r"#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})", s or "")
    if not m:
        raise SvgError(f"colour {s!r}: only #rgb, #rrggbb and none are supported")
    h = m.group(1)
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    return tuple(int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4))


# 2D affine maps as (a, b, c, d, e, f): x' = a x + c y + e, y' = b x + d y + f
def mat_mul(p, q):
    """p after q (q is applied first)."""
    a, b, c, d, e, f = p
    A, B, C, D, E, F = q
    return (a * A + c * B, b * A + d * B, a * C + c * D, b * C + d * D, a * E + c * F + e, b * E + d * F + f)


def mat_inv(m):
    a, b, c, d, e, f = m
    det = a * d - b * c
    if abs(det) < 1e-12:
        raise SvgError("a transform collapses the drawing (zero scale)")
    return (d / det, -b / det, -c / det, a / det, (c * f - d * e) / det, (b * e - a * f) / det)


def mat_apply(m, x, y):
    return m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]


def mat_scale_of(m):
    """Linear scale of a rotation + uniform scale (+ mirror) map: distances stay distances, so a signed-distance shape can be
    evaluated in its own units and scaled. Anything else (non-uniform scale, skew) is refused rather than drawn wrongly."""
    a, b, c, d = m[:4]
    s2 = a * a + b * b
    if s2 < 1e-18 or abs(c * c + d * d - s2) > 1e-9 * s2 or abs(a * c + b * d) > 1e-9 * s2:
        raise SvgError("only rotate, translate and uniform scale transforms are supported (no skew or non-uniform scale)")
    return math.sqrt(s2)


def _svg_transform(s):
    m, pos = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0), 0
    s = s.strip()
    while pos < len(s):
        t = re.compile(r"\s*([a-zA-Z]+)\s*\(([^)]*)\)\s*,?\s*").match(s, pos)
        if not t:
            raise SvgError(f"transform {s!r}: cannot parse from {s[pos:]!r}")
        pos, name, v = t.end(), t.group(1), _svg_nums(t.group(2), "transform")
        if name == "rotate" and len(v) in (1, 3):
            r = math.radians(v[0])
            cx, cy = (v[1], v[2]) if len(v) == 3 else (0.0, 0.0)
            c, sn = math.cos(r), math.sin(r)
            n = (c, sn, -sn, c, cx - c * cx + sn * cy, cy - sn * cx - c * cy)
        elif name == "translate" and len(v) in (1, 2):
            n = (1.0, 0.0, 0.0, 1.0, v[0], v[1] if len(v) == 2 else 0.0)
        elif name == "scale" and len(v) in (1, 2):
            n = (v[0], 0.0, 0.0, v[1] if len(v) == 2 else v[0], 0.0, 0.0)
        elif name == "matrix" and len(v) == 6:
            n = tuple(v)
        else:
            raise SvgError(f"transform {name}({t.group(2)}): not supported (rotate, translate, scale, matrix only)")
        m = mat_mul(m, n)
    return m


class SvgShape:
    """One painted shape in document order. geom by kind: rect (cx, cy, hx, hy, r), circle (cx, cy, r), poly [(points, closed)].
    ctm maps its user space to the viewBox; clips are (ctm, (cx, cy, hx, hy, r)) rounded boxes; bbox is the fill geometry's (x0, y0, x1, y1)."""
    __slots__ = ("kind", "geom", "ctm", "fill", "stroke", "sw", "clips", "bbox")

    def __init__(self, kind, geom, ctm, paint, clips, bbox):
        self.kind, self.geom, self.ctm, self.clips, self.bbox = kind, geom, ctm, clips, bbox
        self.fill, self.stroke, self.sw = paint["fill"], paint["stroke"], paint["stroke-width"]


class SvgShadow:
    """An feDropShadow on a group: the group's shapes (painted right after this), its transform, and the filter's numbers."""
    __slots__ = ("shapes", "ctm", "dx", "dy", "std", "color", "opacity", "region")

    def __init__(self, shapes, ctm, fe, region):
        self.shapes, self.ctm, self.region = shapes, ctm, region
        self.dx, self.dy, self.std, self.color, self.opacity = fe


class SvgScene:
    def __init__(self, vb, ops):
        self.vb, self.ops = vb, ops          # vb = (minx, miny, size)

    def tile_fraction(self):
        """Extent of the first painted shape (the tile, stroke included) as a fraction of the viewBox."""
        for op in self.ops:
            if isinstance(op, SvgShape):
                x0, y0, x1, y1 = _shape_bounds(op, op.ctm, op.sw / 2.0 if op.stroke else 0.0)
                return max(x1 - x0, y1 - y0) / self.vb[2]
        raise SvgError("nothing is drawn")


def _check_attrs(el, tag):
    bad = sorted(set(el.attrib) - _SVG_ATTRS[tag])
    if bad:
        raise SvgError(f"<{tag}>: attribute {bad[0]!r} is not supported")


def _local(el):
    if not el.tag.startswith(SVG_NS):
        raise SvgError(f"element {el.tag!r} is not in the SVG namespace")
    tag = el.tag[len(SVG_NS):]
    if tag not in _SVG_ATTRS:
        raise SvgError(f"<{tag}> is not supported (supported: {', '.join(sorted(_SVG_ATTRS))})")
    _check_attrs(el, tag)
    return tag


def _url_ref(s, what):
    m = re.fullmatch(r"url\(#([^)\s]+)\)", s.strip())
    if not m:
        raise SvgError(f"{what}: {s!r} is not url(#id)")
    return m.group(1)


def _svg_frac(s, default, what):
    """A filter-region number: a fraction of the object's bounding box, or a percentage of it."""
    if s is None:
        return default
    s = s.strip()
    return _svg_num(s[:-1], what) / 100.0 if s.endswith("%") else _svg_num(s, what)


def parse_svg(path):
    """The icon SVG as an SvgScene, or SvgError."""
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as e:
        raise SvgError(f"cannot read the SVG: {e}") from None
    if _local(root) != "svg":
        raise SvgError("the root element is not <svg>")
    vb = _svg_nums(root.get("viewBox", ""), "viewBox")
    if len(vb) != 4 or vb[2] <= 0 or abs(vb[2] - vb[3]) > 1e-9:
        raise SvgError("viewBox must be 'minx miny size size' (square)")
    for k in ("width", "height"):
        if root.get(k) not in (None, "100%"):
            raise SvgError(f"<svg {k}> must be absent or 100% (the icon is scaled to the requested size)")
    clips, filters = {}, {}
    for el in root:
        if _local(el) != "defs":
            continue
        for d in el:
            tag = _local(d)
            if tag == "clipPath":
                kids = list(d)
                if len(kids) != 1 or _local(kids[0]) != "rect":
                    raise SvgError("<clipPath> must hold exactly one <rect>")
                clips[d.get("id")] = kids[0]
            elif tag == "filter":
                kids = list(d)
                if len(kids) != 1 or _local(kids[0]) != "feDropShadow":
                    raise SvgError("<filter> must hold exactly one <feDropShadow>")
                filters[d.get("id")] = d
            else:
                raise SvgError(f"<{tag}> inside <defs> is not supported")
    ops = []
    for el in root:
        if _local(el) != "defs":
            _walk(el, (1.0, 0.0, 0.0, 1.0, 0.0, 0.0), dict(_SVG_DEFAULTS), [], ops, clips, filters, False)
    if not any(isinstance(o, SvgShape) for o in ops):
        raise SvgError("nothing is drawn")
    return SvgScene((vb[0], vb[1], vb[2]), ops)


def _box_geom(el, what):
    x, y = _svg_num(el.get("x", "0"), what), _svg_num(el.get("y", "0"), what)
    w, h = _svg_num(el.get("width"), what), _svg_num(el.get("height"), what)
    if w <= 0 or h <= 0:
        raise SvgError(f"{what}: width and height must be positive")
    r = min(_svg_num(el.get("rx", "0"), what), w / 2.0, h / 2.0)
    return x, y, w, h, max(r, 0.0)


def _walk(el, ctm, inh, clips_in, ops, clip_defs, filter_defs, in_filter):
    tag = _local(el)
    if tag in ("defs", "clipPath", "filter", "feDropShadow", "svg"):
        raise SvgError(f"<{tag}> is not supported here")
    if "transform" in el.attrib:
        ctm = mat_mul(ctm, _svg_transform(el.get("transform")))
    paint = dict(inh)
    for k in _PRESENTATION & set(el.attrib):
        paint[k] = el.get(k)
    clips = list(clips_in)
    if "clip-path" in el.attrib:
        ref = _url_ref(el.get("clip-path"), "clip-path")
        if ref not in clip_defs:
            raise SvgError(f"clip-path references unknown #{ref}")
        rc = clip_defs[ref]
        x, y, w, h, r = _box_geom(rc, "clipPath rect")
        if set(rc.attrib) - {"id", "x", "y", "width", "height", "rx"}:
            raise SvgError("a <clipPath> rect takes only x, y, width, height and rx")
        clips.append((ctm, (x + w / 2.0, y + h / 2.0, w / 2.0, h / 2.0, r)))
    if tag == "g":
        flt = None
        if "filter" in el.attrib:
            if in_filter:
                raise SvgError("nested filters are not supported")
            ref = _url_ref(el.get("filter"), "filter")
            if ref not in filter_defs:
                raise SvgError(f"filter references unknown #{ref}")
            flt = filter_defs[ref]
        start = len(ops)
        for ch in el:
            _walk(ch, ctm, paint, clips, ops, clip_defs, filter_defs, in_filter or flt is not None)
        if flt is not None:
            ops.insert(start, _make_shadow(flt, ops[start:], ctm))
        return
    if "filter" in el.attrib:
        raise SvgError(f"<{tag} filter>: a filter is supported only on a <g>")
    fill, stroke = _svg_color(paint["fill"]), _svg_color(paint["stroke"])
    sw = _svg_num(paint["stroke-width"], "stroke-width")
    if stroke is not None and sw <= 0:
        raise SvgError("stroke-width must be positive")
    pp = {"fill": fill, "stroke": stroke, "stroke-width": sw}
    if paint["stroke-linecap"] not in ("butt", "round") or paint["stroke-linejoin"] not in ("miter", "round"):
        raise SvgError("stroke-linecap / stroke-linejoin: only butt|round and miter|round are supported")
    mat_scale_of(ctm)
    round_join = paint["stroke-linejoin"] == "round"
    if tag == "rect":
        x, y, w, h, r = _box_geom(el, "rect")
        if r > 0:
            ops.append(SvgShape(K_RECT, (x + w / 2.0, y + h / 2.0, w / 2.0, h / 2.0, r), ctm, pp, clips, (x, y, x + w, y + h)))
            return
        sub = [([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], True)]
    elif tag == "circle":
        cx, cy, r = (_svg_num(el.get(k, "0"), "circle") for k in ("cx", "cy", "r"))
        if r <= 0:
            raise SvgError("circle: r must be positive")
        ops.append(SvgShape(K_CIRCLE, (cx, cy, r), ctm, pp, clips, (cx - r, cy - r, cx + r, cy + r)))
        return
    elif tag == "line":
        if stroke is None:
            return
        if paint["stroke-linecap"] != "round":
            raise SvgError("<line>: only stroke-linecap=\"round\" is supported")
        pp["fill"] = None
        x1, y1, x2, y2 = (_svg_num(el.get(k, "0"), "line") for k in ("x1", "y1", "x2", "y2"))
        sub = [([(x1, y1), (x2, y2)], False)]
    elif tag == "polygon":
        v = _svg_nums(el.get("points", ""), "points")
        if len(v) < 6 or len(v) % 2:
            raise SvgError("polygon: points needs at least three x,y pairs")
        sub = [(list(zip(v[0::2], v[1::2])), True)]
    else:
        sub = _svg_path(el.get("d", ""))
    if stroke is not None and not round_join and any(len(p) > 2 or c for p, c in sub):
        raise SvgError(f"<{tag}> with a stroke needs stroke-linejoin=\"round\" (miter joins are not supported)")
    if stroke is not None and any(not c for p, c in sub) and paint["stroke-linecap"] != "round":
        raise SvgError(f"<{tag}> open subpath with a stroke needs stroke-linecap=\"round\"")
    xs = [p[0] for pts, c in sub for p in pts]
    ys = [p[1] for pts, c in sub for p in pts]
    ops.append(SvgShape(K_POLY, sub, ctm, pp, clips, (min(xs), min(ys), max(xs), max(ys))))


def _svg_path(d):
    toks = re.findall(r"[A-Za-z]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?|\S", d.replace(",", " "))
    sub, cur, cmd, i = [], None, None, 0
    while i < len(toks):
        t = toks[i]
        if t.isalpha():
            if t not in "MLZ":
                raise SvgError(f"path: command {t!r} is not supported (absolute M, L, Z only)")
            cmd = t
            i += 1
            if cmd == "Z":
                if cur is None:
                    raise SvgError("path: Z before M")
                sub.append((cur, True))
                cur, cmd = None, "Z"
            continue
        if cmd in (None, "Z") or i + 1 >= len(toks):
            raise SvgError(f"path: a coordinate {t!r} without a pending M/L (after Z a new M is required)")
        pt = (_svg_num(toks[i], "path"), _svg_num(toks[i + 1], "path"))
        i += 2
        if cmd == "M":
            if cur is not None:
                sub.append((cur, False))
            cur, cmd = [pt], "L"          # further pairs after an M are implicit L
        else:
            cur.append(pt)
    if cur is not None:
        sub.append((cur, False))
    if not sub:
        raise SvgError("path: empty")
    return sub


def _shape_bounds(shape, m, margin):
    """Bounding box of the shape's fill geometry grown by margin (in its own units), mapped through m."""
    x0, y0, x1, y1 = shape.bbox
    pts = [mat_apply(m, x, y) for x in (x0 - margin, x1 + margin) for y in (y0 - margin, y1 + margin)]
    return min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts)


def _make_shadow(flt, shapes, ctm):
    fe = list(flt)[0]
    std = _svg_nums(fe.get("stdDeviation", "2"), "stdDeviation")
    if len(std) not in (1, 2) or std[0] != std[-1] or std[0] < 0:
        raise SvgError("feDropShadow stdDeviation: one non-negative number (or two equal ones)")
    color = _svg_color(fe.get("flood-color", "#000000"))
    if color is None:
        raise SvgError("feDropShadow flood-color: a colour is needed")
    mat_scale_of(ctm)
    # the filter region: a box around the group's bounding box (fill geometry, in the group's own space)
    inv = mat_inv(ctm)
    xs, ys = [], []
    for s in shapes:
        b = _shape_bounds(s, mat_mul(inv, s.ctm), 0.0)
        xs += [b[0], b[2]]
        ys += [b[1], b[3]]
    bx0, by0, bx1, by1 = min(xs), min(ys), max(xs), max(ys)
    fx, fy = _svg_frac(flt.get("x"), -0.1, "filter x"), _svg_frac(flt.get("y"), -0.1, "filter y")
    fw, fh = _svg_frac(flt.get("width"), 1.2, "filter width"), _svg_frac(flt.get("height"), 1.2, "filter height")
    rx0, ry0 = bx0 + fx * (bx1 - bx0), by0 + fy * (by1 - by0)
    rw, rh = fw * (bx1 - bx0), fh * (by1 - by0)
    region = (ctm, (rx0 + rw / 2.0, ry0 + rh / 2.0, rw / 2.0, rh / 2.0, 0.0))
    return SvgShadow(list(shapes), ctm, (_svg_num(fe.get("dx", "2"), "dx"), _svg_num(fe.get("dy", "2"), "dy"), std[0], color,
                                         _svg_num(fe.get("flood-opacity", "1"), "flood-opacity")), region)


# ---- placing the scene on a pixel grid

class PreparedOp:
    """A shape ready to evaluate at a pixel size: inv maps device pixels to the shape's own units (scale s the other way)."""
    __slots__ = ("kind", "geom", "inv", "s", "x0", "y0", "x1", "y1", "fill", "stroke", "hw", "clips", "fedges", "sedges",
                 "color", "opacity", "mask", "mw", "mx0", "my0", "mcols", "mrows", "k", "ox", "oy")


def _edges(sub, closed_all):
    out = []
    for pts, closed in sub:
        n = len(pts)
        for i in range(n if (closed or closed_all) else n - 1):
            ax, ay = pts[i]
            bx, by = pts[(i + 1) % n]
            dx, dy = bx - ax, by - ay
            if dx or dy:
                out.append((ax, ay, bx, by, dx, dy, 1.0 / (dx * dx + dy * dy)))
    return out


def _prep_clip(clip, g):
    cm, (cx, cy, hx, hy, r) = clip
    m = mat_mul(g, cm)
    return (mat_inv(m), mat_scale_of(m), cx, cy, hx, hy, r)


def _prep_shape(sh, g):
    m = mat_mul(g, sh.ctm)
    s = mat_scale_of(m)
    hw = sh.sw / 2.0 if sh.stroke else 0.0
    op = PreparedOp()
    op.kind, op.geom, op.inv, op.s = sh.kind, sh.geom, mat_inv(m), s
    op.x0, op.y0, op.x1, op.y1 = _shape_bounds(sh, m, hw + 1.0 / s)
    op.fill = sh.fill
    op.stroke = sh.stroke
    op.hw = hw * s
    op.clips = [_prep_clip(c, g) for c in sh.clips]
    op.fedges = op.sedges = None
    if sh.kind == K_POLY:
        op.fedges = _edges(sh.geom, True)
        op.sedges = op.fedges if all(c for p, c in sh.geom) else _edges(sh.geom, False)
    return op


def _scene_matrix(scene, px, fit):
    """viewBox -> pixel grid, then the whole drawing scaled by fit about the centre."""
    mx, my, size = scene.vb
    k, c = px / size, px / 2.0
    m = mat_mul((1.0, 0.0, 0.0, 1.0, c, c), mat_mul((fit, 0.0, 0.0, fit, 0.0, 0.0), (1.0, 0.0, 0.0, 1.0, -c, -c)))
    return mat_mul(m, mat_mul((k, 0.0, 0.0, k, 0.0, 0.0), (1.0, 0.0, 0.0, 1.0, -mx, -my)))


def _box_blur(line, lo, hi):
    if not any(line):
        return line
    n = len(line)
    p = [0.0]
    p += accumulate(line)
    inv = 1.0 / (lo + hi + 1)
    return [(p[min(n, i + hi + 1)] - p[max(0, i - lo)]) * inv for i in range(n)]


def _gauss_passes(sigma):
    """The SVG spec's three box blurs approximating a gaussian: (lo, hi) windows, [] for no blur."""
    d = int(math.floor(sigma * 3.0 * math.sqrt(2.0 * math.pi) / 4.0 + 0.5))
    if d < 1:
        return []
    if d % 2:
        return [((d - 1) // 2, (d - 1) // 2)] * 3
    return [(d // 2, d // 2 - 1), (d // 2 - 1, d // 2), (d // 2, d // 2)]


def _prep_shadow(sd, g, px, pool):
    """Blurred alpha of the group at <= SVG_MASK_MAX pixels, cropped to its non-zero part, as a ready-to-sample PreparedOp."""
    m = min(px, SVG_MASK_MAX)
    q = m / px
    gm = mat_mul((q, 0.0, 0.0, q, 0.0, 0.0), g)
    ops = [_prep_shape(s, gm) for s in sd.shapes]
    jobs = [(j, min(m, j + 16), m, ops, True) for j in range(0, m, 16)]
    rows = [r for part in (pool.map(render_rows_svg, jobs) if pool and m >= 128 else map(render_rows_svg, jobs)) for r in part]
    # blur: horizontal passes per row, then vertical passes per column
    sigma = sd.std * mat_scale_of(sd.ctm) * mat_scale_of(gm)
    for lo, hi in _gauss_passes(sigma):
        rows = [_box_blur(r, lo, hi) for r in rows]
    cols = list(zip(*rows))
    for lo, hi in _gauss_passes(sigma):
        cols = [_box_blur(list(c), lo, hi) for c in cols]
    rows = [list(r) for r in zip(*cols)]
    live = [j for j, r in enumerate(rows) if any(v > 1e-4 for v in r)]
    op = PreparedOp()
    op.kind = K_SHADOW
    op.color, op.opacity = sd.color, sd.opacity
    # the offset is in the group's own user space: rotate it with the group (in pixels at the final size)
    gg = mat_mul(g, sd.ctm)
    op.ox, op.oy = gg[0] * sd.dx + gg[2] * sd.dy, gg[1] * sd.dx + gg[3] * sd.dy
    op.k = q
    if not live:
        op.x0 = op.y0 = 1.0
        op.x1 = op.y1 = 0.0
        op.mask, op.mw, op.mx0, op.my0, op.mcols, op.mrows, op.clips = [], 0, 0, 0, 0, 0, []
        return op
    cx = [i for i in range(m) if any(rows[j][i] > 1e-4 for j in live)]
    op.my0, op.mrows = live[0], live[-1] - live[0] + 1
    op.mx0, op.mcols = cx[0], cx[-1] - cx[0] + 1
    op.mw = op.mcols
    op.mask = array("f", (rows[j][i] for j in range(op.my0, op.my0 + op.mrows) for i in range(op.mx0, op.mx0 + op.mcols)))
    op.x0, op.x1 = (op.mx0 - 1) / q + op.ox, (op.mx0 + op.mcols + 1) / q + op.ox
    op.y0, op.y1 = (op.my0 - 1) / q + op.oy, (op.my0 + op.mrows + 1) / q + op.oy
    op.clips = [_prep_clip(sd.region, g)]
    return op


def prepare_scene(scene, px, fit, pool):
    g = _scene_matrix(scene, px, fit)
    out = []
    for op in scene.ops:
        out.append(_prep_shadow(op, g, px, pool) if isinstance(op, SvgShadow) else _prep_shape(op, g))
    return out


# ---- evaluating it

def _clip_cov(c, x, y):
    inv, s, cx, cy, hx, hy, r = c
    lx = inv[0] * x + inv[2] * y + inv[4]
    ly = inv[1] * x + inv[3] * y + inv[5]
    v = 0.5 - sd_box(lx - cx, ly - cy, hx, hy, r) * s
    return 0.0 if v < 0.0 else 1.0 if v > 1.0 else v


def _poly_dist(edges, x, y):
    best = 1e30
    for ax, ay, bx, by, dx, dy, il in edges:
        t = ((x - ax) * dx + (y - ay) * dy) * il
        t = 0.0 if t < 0.0 else 1.0 if t > 1.0 else t
        ex, ey = x - ax - t * dx, y - ay - t * dy
        d2 = ex * ex + ey * ey
        if d2 < best:
            best = d2
    return math.sqrt(best)


def _poly_dist_inside(edges, x, y):
    """Distance to the outline and whether (x, y) is inside it (non-zero winding)."""
    best, wn = 1e30, 0
    for ax, ay, bx, by, dx, dy, il in edges:
        t = ((x - ax) * dx + (y - ay) * dy) * il
        t = 0.0 if t < 0.0 else 1.0 if t > 1.0 else t
        ex, ey = x - ax - t * dx, y - ay - t * dy
        d2 = ex * ex + ey * ey
        if d2 < best:
            best = d2
        left = dx * (y - ay) - (x - ax) * dy
        if ay <= y:
            if by > y and left > 0.0:
                wn += 1
        elif by <= y and left < 0.0:
            wn -= 1
    return math.sqrt(best), wn != 0


def _op_cover(o, x, y):
    """(fill coverage, stroke coverage) of a shape at device point x, y."""
    inv = o.inv
    lx = inv[0] * x + inv[2] * y + inv[4]
    ly = inv[1] * x + inv[3] * y + inv[5]
    kind, g, s = o.kind, o.geom, o.s
    d = None                        # signed distance to the fill's edge in pixels (negative inside), when a fill needs it
    if kind == K_RECT:
        d = sd_box(lx - g[0], ly - g[1], g[2], g[3], g[4]) * s
        du = abs(d)
    elif kind == K_CIRCLE:
        d = (math.hypot(lx - g[0], ly - g[1]) - g[2]) * s
        du = abs(d)
    else:
        if o.fill is not None:
            du, inside = _poly_dist_inside(o.fedges, lx, ly)
            d = (-du if inside else du) * s
        if o.stroke is not None:
            du = abs(d) if d is not None and o.sedges is o.fedges else _poly_dist(o.sedges, lx, ly) * s
    fc = sc = 0.0
    if o.fill is not None:
        fc = 0.5 - d
        fc = 0.0 if fc < 0.0 else 1.0 if fc > 1.0 else fc
    if o.stroke is not None:
        lo, hi, hw = du - 0.5, du + 0.5, o.hw
        if lo < -hw:
            lo = -hw
        if hi > hw:
            hi = hw
        sc = hi - lo if hi > lo else 0.0
    for c in o.clips:
        if fc > 0.0 or sc > 0.0:
            k = _clip_cov(c, x, y)
            fc *= k
            sc *= k
    return fc, sc


def _shadow_alpha(o, x, y):
    f = (x - o.ox) * o.k - 0.5 - o.mx0
    g = (y - o.oy) * o.k - 0.5 - o.my0
    i0, j0 = math.floor(f), math.floor(g)
    tx, ty = f - i0, g - j0
    w, mask, nc, nr = o.mw, o.mask, o.mcols, o.mrows
    v = 0.0
    for dj, wy in ((0, 1.0 - ty), (1, ty)):
        j = j0 + dj
        if 0 <= j < nr:
            for di, wx in ((0, 1.0 - tx), (1, tx)):
                i = i0 + di
                if 0 <= i < nc:
                    v += mask[j * w + i] * wx * wy
    return v * o.opacity


def render_rows_svg(args):
    """Rows j0..j1 of the prepared scene: premultiplied floats [r, g, b, a, ...] per row, or just alpha rows (alpha_only)."""
    j0, j1, px, ops, alpha_only = args
    out = []
    for j in range(j0, j1):
        y = j + 0.5
        act = [o for o in ops if o.y0 <= y <= o.y1]
        row = []
        if not act:
            out.append([0.0] * px if alpha_only else bytes(1 + 4 * px))
            continue
        xlo, xhi = min(o.x0 for o in act), max(o.x1 for o in act)
        png = bytearray([0])           # PNG filter type 0
        for i in range(px):
            x = i + 0.5
            r = g = b = a = 0.0
            if xlo <= x <= xhi:
                for o in act:
                    if x < o.x0 or x > o.x1:
                        continue
                    if o.kind == K_SHADOW:
                        al = _shadow_alpha(o, x, y)
                        if al > 0.0:
                            for c in o.clips:
                                al *= _clip_cov(c, x, y)
                        layers = ((o.color, al),)
                    else:
                        fc, sc = _op_cover(o, x, y)
                        layers = ((o.fill, fc), (o.stroke, sc))
                    for col, al in layers:
                        if al > 0.0:
                            if al > 1.0:
                                al = 1.0
                            ia = 1.0 - al
                            r = col[0] * al + r * ia
                            g = col[1] * al + g * ia
                            b = col[2] * al + b * ia
                            a = al + a * ia
            if alpha_only:
                row.append(a)
            elif a > 0.0:
                png += bytes((min(255, int(255.0 * r / a + 0.5)), min(255, int(255.0 * g / a + 0.5)),
                              min(255, int(255.0 * b / a + 0.5)), min(255, int(255.0 * a + 0.5))))
            else:
                png += bytes(4)
        out.append(row if alpha_only else bytes(png))
    return out


def render_svg_png(px, scene, pool, macos):
    fit = MACOS_TILE_FRACTION / scene.tile_fraction() if macos else 1.0
    ops = prepare_scene(scene, px, fit, pool)
    chunk = max(1, px // 40)
    jobs = [(j, min(px, j + chunk), px, ops, False) for j in range(0, px, chunk)]
    parts = pool.map(render_rows_svg, jobs) if pool and px >= 128 else map(render_rows_svg, jobs)
    return png_bytes(px, [r for part in parts for r in part])


# ---------------------------------------------------------------- rasterising

def render_rows(args):
    j0, j1, px, aa, st, lo = args
    out = []
    n = aa * aa
    k = float(px)
    for j in range(j0, j1):
        row = bytearray([0])   # PNG filter type 0
        for i in range(px):
            ar = ag = ab = aa_ = 0.0
            for sj in range(aa):
                for si in range(aa):
                    r, g, b, a = shade((i + (si + 0.5) / aa) / px, (j + (sj + 0.5) / aa) / px, k, st, lo)
                    ar += r
                    ag += g
                    ab += b
                    aa_ += a
            if aa_ > 0.0:
                row += bytes(min(255, int(255.0 * c / aa_ + 0.5)) for c in (ar, ag, ab)) + bytes([min(255, int(255.0 * aa_ / n + 0.5))])
            else:
                row += bytes(4)
        out.append(bytes(row))
    return out


def png_bytes(px, rows):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", px, px, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


def render_png(px, st, pool, macos=False):
    """One PNG at px. macos: an SVG icon is shrunk onto Apple's icon grid (see MACOS_TILE_FRACTION); the procedural looks already are."""
    if st.get("svg"):
        small = st.get("svg_small") if px <= SMALL_ICON_MAX_PX else None
        return render_svg_png(px, small or st["svg"], pool, macos)
    lo = px <= 48
    aa = 4 if px <= 48 else 3 if px <= 128 else 2 if px <= 256 else 1
    chunk = max(1, px // 40)
    jobs = [(j, min(px, j + chunk), px, aa, st, lo) for j in range(0, px, chunk)]
    rows = []
    for part in pool.map(render_rows, jobs):
        rows.extend(part)
    return png_bytes(px, rows)


def decode_rows(data):
    """Our own PNGs back to RGBA rows (filter 0 only: that is all render_png writes)."""
    px = struct.unpack(">I", data[16:20])[0]
    i, idat = 8, b""
    while i < len(data):
        n = struct.unpack(">I", data[i:i + 4])[0]
        if data[i + 4:i + 8] == b"IDAT":
            idat += data[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(idat)
    return px, [raw[j * (px * 4 + 1) + 1:(j + 1) * (px * 4 + 1)] for j in range(px)]


def preview_sheet(path, st, pool, macos=False):
    """One PNG to judge legibility: 16/32/64/128/256 at 1:1 on light and dark, and 16/32 enlarged 5x (nearest)."""
    sizes = (16, 32, 64, 128, 256)
    imgs = {s: decode_rows(render_png(s, st, pool, macos)) for s in sizes}
    W = sum(s + 16 for s in sizes) + 16
    H = 2 * (256 + 16) + 16 + 160 + 16
    canvas = [[(235, 235, 240)] * W for _ in range(H)]

    def put(img, x0, y0, bg, scale=1):
        px, rows = img
        for j in range(px * scale):
            row = rows[j // scale]
            for i in range(px * scale):
                o = (i // scale) * 4
                a = row[o + 3] / 255.0
                canvas[y0 + j][x0 + i] = tuple(int(row[o + c] * a + bg[c] * (1 - a) + 0.5) for c in range(3))
    for band, bg in ((0, (235, 235, 240)), (1, (28, 28, 34))):
        y0 = 16 + band * (256 + 16)
        for yy in range(y0 - 8, y0 + 256 + 8):
            for xx in range(W):
                canvas[yy][xx] = bg
        x = 16
        for s in sizes:
            put(imgs[s], x, y0, bg)
            x += s + 16
    y0 = 2 * (256 + 16) + 16
    x = 16
    for s in (16, 32):
        put(imgs[s], x, y0, (235, 235, 240), 5)
        x += s * 5 + 16
    write_rgb_png(path, canvas)


def write_rgb_png(path, canvas):
    H, W = len(canvas), len(canvas[0])
    raw = b"".join(b"\x00" + bytes(c for p in row for c in p) for row in canvas)

    def chunk(tag, d):
        c = struct.pack(">I", len(d)) + tag + d
        return c + struct.pack(">I", zlib.crc32(tag + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def overview(path, pool, size=256):
    """Every look for every game number on one sheet (rows: variants, columns: games 1-3)."""
    pad = 24
    W, H = 3 * size + 4 * pad, len(VARIANTS) * size + (len(VARIANTS) + 1) * pad
    canvas = [[(240, 240, 244)] * W for _ in range(H)]
    for r, v in enumerate(VARIANTS):
        for c, n in enumerate((1, 2, 3)):
            st = make_style(n, v)
            st["number"], st["parts"] = n, digit_parts(n)
            px, rows = decode_rows(render_png(size, st, pool))
            x0, y0 = pad + c * (size + pad), pad + r * (size + pad)
            for j in range(px):
                row = rows[j]
                for i in range(px):
                    a = row[i * 4 + 3] / 255.0
                    canvas[y0 + j][x0 + i] = tuple(int(row[i * 4 + k] * a + canvas[y0 + j][x0 + i][k] * (1 - a) + 0.5) for k in range(3))
    write_rgb_png(path, canvas)


def write_ico(path, pngs):
    """pngs: {size: png bytes}. PNG-compressed entries (Windows Vista and later; 256 px entries are always PNG)."""
    sizes = sorted(pngs)
    head = struct.pack("<HHH", 0, 1, len(sizes))
    off = 6 + 16 * len(sizes)
    ents = b""
    body = b""
    for s in sizes:
        d = pngs[s]
        ents += struct.pack("<BBBBHHII", s if s < 256 else 0, s if s < 256 else 0, 0, 0, 1, 32, len(d), off + len(body))
        body += d
    with open(path, "wb") as f:
        f.write(head + ents + body)


def svg_overview(path, svgs, pool, size=384):
    """The SVG icons side by side: as drawn on a light ground (Windows), and on the macOS grid on a dark one."""
    pad = 24
    W, H = len(svgs) * size + (len(svgs) + 1) * pad, 2 * size + 3 * pad
    canvas = [[(240, 240, 244) if y < size + 2 * pad - pad // 2 else (30, 30, 36) for _ in range(W)] for y in range(H)]
    for r, mac in enumerate((False, True)):
        for c, svg in enumerate(svgs):
            px, rows = decode_rows(render_png(size, {"svg": svg}, pool, mac))
            x0, y0 = pad + c * (size + pad), pad + r * (size + pad)
            for j in range(px):
                row = rows[j]
                for i in range(px):
                    a = row[i * 4 + 3] / 255.0
                    bg = canvas[y0 + j][x0 + i]
                    canvas[y0 + j][x0 + i] = tuple(int(row[i * 4 + k] * a + bg[k] * (1 - a) + 0.5) for k in range(3))
    write_rgb_png(path, canvas)


def load_style(args):
    number, variant, svg = args.number, args.variant, (args.svg or [None])[0]
    svg_small = None   # only from the profile: an --svg on the command line previews that one file at every size
    if args.game_toml or number is None:
        tp = args.game_toml or os.path.join(os.environ.get("THPS_HOST", ""), "game", "game.toml")
        if not os.path.isfile(tp):
            sys.exit(f"no game profile at {tp!r}: pass --game-toml or --number, or run through the game's wrapper")
        with open(tp, "rb") as f:
            g = tomllib.load(f)
        if number is None:
            number = g.get("icon_number")
            if number is None:
                m = re.search(r"(\d+)$", g.get("family", ""))
                number = int(m.group(1)) if m else 1
        if svg is None and variant is None and g.get("icon_svg"):
            svg = os.path.join(os.path.dirname(os.path.abspath(tp)), g["icon_svg"])
            if g.get("icon_svg_small"):
                svg_small = os.path.join(os.path.dirname(os.path.abspath(tp)), g["icon_svg_small"])
        variant = variant or g.get("icon_variant", DEFAULT_VARIANT)
    if svg:
        st = {"svg_path": svg}
        for key, path in (("svg", svg), ("svg_small", svg_small)):
            if path:
                try:
                    st[key] = parse_svg(path)
                except SvgError as e:
                    sys.exit(f"{path}: {e}")
        return st
    variant = variant or DEFAULT_VARIANT
    if variant not in VARIANTS:
        sys.exit(f"unknown variant {variant!r}; one of {', '.join(VARIANTS)}")
    st = make_style(int(number), variant)
    st["number"] = int(number)
    st["parts"] = digit_parts(int(number))
    return st


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--game-toml")
    ap.add_argument("--number", type=int)
    ap.add_argument("--variant", choices=VARIANTS, help="a procedural look (also overrides the profile's icon_svg, for previews)")
    ap.add_argument("--svg", action="append", help="draw this SVG instead of the profile's icon (repeat for --svg-overview)")
    ap.add_argument("--macos", action="store_true", help="--png / --preview-sheet: shrink an SVG icon onto the macOS icon grid (the .icns always is)")
    ap.add_argument("--svg-overview", help="write one sheet with every --svg icon: as drawn (Windows) and on the macOS grid")
    ap.add_argument("--icns")
    ap.add_argument("--ico")
    ap.add_argument("--png")
    ap.add_argument("--size", type=int, default=1024)
    ap.add_argument("--preview-sheet", help="write a legibility sheet (16..256 px at 1:1 on light/dark, 16/32 enlarged)")
    ap.add_argument("--overview", help="write one sheet with every look for games 1-3")
    ap.add_argument("--also-sizes", help="comma list: write NAME-<size>.png beside --png (legibility checks)")
    a = ap.parse_args()
    if not (a.icns or a.ico or a.png or a.preview_sheet or a.overview or a.svg_overview):
        ap.error("nothing to write: give --icns, --ico or --png")
    st = load_style(a) if (a.png or a.icns or a.ico or a.preview_sheet) else None
    with multiprocessing.Pool() as pool:
        if a.png:
            os.makedirs(os.path.dirname(os.path.abspath(a.png)), exist_ok=True)
            with open(a.png, "wb") as f:
                f.write(render_png(a.size, st, pool, a.macos))
            print(f"wrote {a.png}")
            for s in (a.also_sizes or "").split(","):
                if s.strip():
                    p = re.sub(r"\.png$", f"-{int(s)}.png", a.png)
                    with open(p, "wb") as f:
                        f.write(render_png(int(s), st, pool, a.macos))
        if a.preview_sheet:
            os.makedirs(os.path.dirname(os.path.abspath(a.preview_sheet)), exist_ok=True)
            preview_sheet(a.preview_sheet, st, pool, a.macos)
            print(f"wrote {a.preview_sheet}")
        if a.overview:
            os.makedirs(os.path.dirname(os.path.abspath(a.overview)), exist_ok=True)
            overview(a.overview, pool)
            print(f"wrote {a.overview}")
        if a.svg_overview:
            try:
                svgs = [parse_svg(f) for f in a.svg or []]
            except SvgError as e:
                sys.exit(f"--svg: {e}")
            os.makedirs(os.path.dirname(os.path.abspath(a.svg_overview)), exist_ok=True)
            svg_overview(a.svg_overview, svgs, pool)
            print(f"wrote {a.svg_overview}")
        cache = {}

        def get(px, macos=False):
            if (px, macos) not in cache:
                cache[(px, macos)] = render_png(px, st, pool, macos)
            return cache[(px, macos)]

        if a.icns:
            out = os.path.abspath(a.icns)
            tmp = tempfile.mkdtemp(prefix="icon-")
            try:
                iconset = os.path.join(tmp, "AppIcon.iconset")
                os.makedirs(iconset)
                for pts, scale in ICNS_SIZES:
                    name = f"icon_{pts}x{pts}{'@2x' if scale == 2 else ''}.png"
                    with open(os.path.join(iconset, name), "wb") as f:
                        f.write(get(pts * scale, True))
                os.makedirs(os.path.dirname(out), exist_ok=True)
                subprocess.run(["iconutil", "-c", "icns", iconset, "-o", out], check=True)
                print(f"wrote {out} ({os.path.getsize(out)} bytes)")
            finally:
                shutil.rmtree(tmp, ignore_errors=True)
        if a.ico:
            os.makedirs(os.path.dirname(os.path.abspath(a.ico)), exist_ok=True)
            write_ico(a.ico, {s: get(s) for s in ICO_SIZES})
            print(f"wrote {a.ico} ({os.path.getsize(a.ico)} bytes)")


if __name__ == "__main__":
    main()
