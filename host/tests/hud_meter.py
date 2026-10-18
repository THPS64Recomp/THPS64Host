"""THPS1's SPECIAL meter on a presented frame (step `meter_inside`, tests/scenarios.py `hud-special*`).

The meter is a texrect frame (a dark blue bar with a black interior) plus a G_FILLRECT for the bar's fill (green 006300 while the meter runs down, yellow CECE00 when it is full).
The play test 2026-10-14: with HUD placement the frame sat at the picture's left edge but the fill stayed in the centred 4:3 area, so it started part-way along the frame
and ran past it. Check: find the yellow fill, then at both ends and the middle of it the pixels a little above and below must be the frame's dark blue (not the 3D scene).
"""


def _is_fill(p):
    r, g, b = p
    return b < 0x30 and g >= 0x50 and g > b + 0x40 and r <= g + 0x20


def find_fill(img):
    """(x0, y0, x1, y1) of the fill: the longest horizontal run of green / yellow pixels in the rows of the meter's bar (y ~ 57-59 of 240) and the rows it spans, or None.
    While a special runs the frame is olive too, and its lit bottom border can be a longer olive run than the fill: runs followed by the bar's black interior
    (the fill stops short of the frame's end) win over the others."""
    s = img.h / 240.0
    gap = max(3, round(1.0 * s))
    best = (False, 0, 0, 0)   # black interior after the run, run length, y, x0
    for y in range(int(img.h * 0.22), int(img.h * 0.27)):
        x = 0
        while x < img.w:
            if _is_fill(img.px(x, y)):
                x0 = x
                while x < img.w and _is_fill(img.px(x, y)):
                    x += 1
                xe = x - 1 + gap
                cand = (xe < img.w and max(img.px(xe, y)) <= 0x20, x - x0, y, x0)
                if cand[:2] > best[:2]:
                    best = cand
            else:
                x += 1
    _, n, y, x0 = best
    if n < 20:
        return None
    xm = x0 + n // 2
    y0 = y1 = y
    while _is_fill(img.px(xm, y0 - 1)):
        y0 -= 1
    while _is_fill(img.px(xm, y1 + 1)):
        y1 += 1
    return x0, y0, x0 + n - 1, y1


def is_frame(p):
    """The meter's frame and the bar's interior: dark (black interior, olive-tinted frame while a special runs) or the idle frame's blue; the 3D scene is neither."""
    r, g, b = p
    return max(r, g, b) < 0x60 or (b >= 0x20 and b >= r + 0x10)


def check_meter(img, name):
    """The fill's frame is where the 4:3 art says it is relative to the fill's start (frame x 28..104.75 of 320, fill from x 30; the bar's rows 55-63): the pixels at the frame's
    left and right border, and above and below the fill, must be the frame's, and the black interior follows the fill's end (the test fills it to ~93 %). A fill left in the centred 4:3 area while the frame sits at the picture's edge has scene there."""
    box = find_fill(img)
    if box is None:
        return None
    x0, y0, x1, y1 = box
    s = img.h / 240.0
    ym = (y0 + y1) // 2
    d = max(3, round(1.3 * s))
    probes = {"frame's left border": (x0 - max(2, round(0.7 * s)), ym), "frame's right border": (x0 + round(73.5 * s), ym)}
    for label, fx in (("near the start", x0 + round(8 * s)), ("near the frame's end", x0 + round(66 * s))):
        probes[f"above the fill, {label}"] = (fx, y0 - d)
        probes[f"below the fill, {label}"] = (fx, y1 + d)
    bad = [f"{label} at {pos} = {img.px(*pos)}" for label, pos in probes.items() if not is_frame(img.px(*pos))]
    pos = (x1 + max(3, round(1.0 * s)), ym)   # the fill stops short of the frame's end: the bar's black interior follows it
    if max(img.px(*pos)) > 0x20:
        bad.append(f"after the end of the fill at {pos} = {img.px(*pos)} (the bar's black interior expected)")
    if bad:
        raise AssertionError(f"{name}: the meter fill {box} is not inside its frame: " + "; ".join(bad))
    return f"{name}: meter fill x {x0}-{x1}, y {y0}-{y1} lies inside its frame"
