"""Border crop sessions (src/border_crop.cpp, graphics setting `border = original | fill`), shared by THPS1-3.

The game draws its 3D inside an 8 px inset of the 320x240 frame; RT64 shows the whole frame, so a thin black frame surrounds the
picture: 8/320 of the width on the sides and 8/240 of the height at the top and bottom of the presented image, at every aspect
(RT64's widescreen scales the inset with the picture). The `border` step (run.py) measures that frame on a composited frame:

  ("border", NAME, "black"|"picture")   in each of the four bands (the outer W/40 columns, the outer H/30 rows, 1 px kept off each side
                                         of the band), the share of lines (columns / rows) that hold picture: more than 5 % of the line's
                                         middle 80 % brighter than luma 3. "black": every band <= 5 % (today's frame), "picture": every
                                         band >= 90 % (the 3D reaches the window's edge). The frame is kept as <out>/NAME.png.

crop_sessions(to_level, *, boot, pause) builds (crop-probe: optin, exploration: a traced 4:3 level task), from the game's own navigation steps (boot to the first in-level frame; pause = the steps
from the level into its pause menu):
  crop          16:9 (the default window), `--border fill`: the level frame has picture in all four bands; the stats count cropped tasks;
                the pause menu's bands are still (`edges`, rows too: nothing stale in the former border); the menu's Picture Border row
                switches to Original at run time (graphics.cfg border=original, the black frame is back) and to Fill again.
  crop-219      21:9 (1280x540: a window of exactly 64:27, so RT64 adds no letterbox rows of its own), `--border fill`: the level frame has picture in all four bands.
  crop-43       4:3 (`--aspect original`, 960x720), `--border fill`: the same, plus still bands on the pause menu.
  border-default  16:9, no flag (the default): the frame is black in all four bands and nothing is cropped (the setting is inert).
  two-players-169-crop  (crop_two_players: two players at 16:9 with `--border fill`, from the game's own two-player steps): both
                split views reach the window's edges (picture in all four bands), the split-screen halves are cropped (`crop_halves`) and
                RT64 still widens them as a split screen (`split` views). The views meet at the game's own 2 px divider in the middle.
                Paused, `border original` (live) brings the frame back and changes nothing else (present_cmp: 1 % to 15 % of pixels).
  crop-shots    optin: present captures of the level and pause frames at 16:9 and 21:9 with both settings (before / after evidence),
                via the same steps (the `border` checks, frames kept as crop_<aspect>_<setting>_<state>.png).
"""

WIDE = ["--res-scale", "2", "--msaa", "4", "--hidden", "--skip-intro"]


def border_check(img):
    """Share of lines holding picture in each outer band of a composited frame: dict left/right/top/bottom -> 0..1."""
    w, h = img.w, img.h
    bw, bh = max(3, w // 40), max(3, h // 30)

    def lit(x, y):
        r, g, b = img.px(x, y)[:3]
        return 0.299 * r + 0.587 * g + 0.114 * b > 3

    ys = range(h // 10, h * 9 // 10, 2)
    xs = range(w // 10, w * 9 // 10, 2)

    def col(x):
        return sum(1 for y in ys if lit(x, y)) > 0.05 * len(ys)

    def row(y):
        return sum(1 for x in xs if lit(x, y)) > 0.05 * len(xs)
    bands = {
        "left": [col(x) for x in range(1, bw - 1)],
        "right": [col(x) for x in range(w - bw + 1, w - 1)],
        "top": [row(y) for y in range(1, bh - 1)],
        "bottom": [row(y) for y in range(h - bh + 1, h - 1)],
    }
    return {k: sum(v) / len(v) for k, v in bands.items()}


def _level(name, aspect_args, *, boot, to_level, setting, extra=()):
    return [
        *boot, *to_level,
        ("wait", 60),
        ("ctl_match", "stats", r"crop=1 crop_tasks=[1-9]" if setting == "fill" else r"crop=0 crop_tasks=0 "),
        ("border", f"{name}_level", "picture" if setting == "fill" else "black"),
        *extra,
    ]


def crop_sessions(to_level, *, boot=(), pause=()):
    crop = ["--border", "fill"]
    sessions = [
        dict(name="crop-probe", config="crop-probe-config", fresh_pak=True, wav=False, present_every=30, optin=True,
             args=["--aspect", "original", "--window", "960x720", *WIDE], scenarios=[
            ("crop_probe", "exploration: 4:3 level and pause frames, one traced gfx task each (dltrace), present captures", [
                *boot, *to_level,
                ("wait", 60),
                ("ctl", "dltrace"),
                ("wait", 10),
                ("keep_present", "probe_level"),
                ("pcap", 6, "probe_clip"),
                *pause,
                ("wait", 30),
                ("ctl", "dltrace"),
                ("wait", 10),
                ("keep_present", "probe_pause"),
            ]),
        ]),
        dict(name="crop", config="crop-config", fresh_pak=True, wav=False, requires="menu",
             fresh_files=["graphics.cfg"],
             args=["--aspect", "16:9", "--window", "1280x720", *WIDE, *crop], scenarios=[
            ("crop_level_169", "16:9 --border fill: the level picture reaches all four edges (no black frame); tasks are cropped", [
                *_level("crop169", [], boot=boot, to_level=to_level, setting="fill"),
            ]),
            ("crop_pause_169", "16:9 --border fill: the pause menu's bands are still (rows too: nothing stale where the frame was)", [
                *pause,
                ("wait", 20),
                ("edges", 45, 4.0, "rows"),
                ("border", "crop169_pause", "picture"),
            ]),
            ("crop_menu_toggle", "Graphics > Picture Border: Original brings the black frame back (graphics.cfg border=original), Fill removes it", [
                ("ui", "esc"),
                ("ui_state", "open=1"),
                ("ui", "tab 2"),
                ("ui", "click #lbl_border_original"),
                ("ui_state", "gfx.border=original"),
                ("file_contains", "graphics.cfg", r"(?m)^border=original$"),
                ("ui", "esc"),
                ("ui_state", "open=0"),
                ("wait", 20),
                ("border", "crop169_toggled_original", "black"),
                ("ui", "esc"),
                ("ui_state", "open=1"),
                ("ui", "tab 2"),
                ("ui", "click #lbl_border_fill"),
                ("ui_state", "gfx.border=fill"),
                ("file_contains", "graphics.cfg", r"(?m)^border=fill$"),
                ("ui", "esc"),
                ("ui_state", "open=0"),
                ("wait", 20),
                ("border", "crop169_toggled_crop", "picture"),
            ]),
        ]),
        dict(name="crop-219", config="crop-219-config", fresh_pak=True, wav=False,
             args=["--aspect", "21:9", "--window", "1280x540", *WIDE, *crop], scenarios=[
            ("crop_level_219", "21:9 --border fill: the level picture reaches all four edges", [
                *_level("crop219", [], boot=boot, to_level=to_level, setting="fill"),
            ]),
        ]),
        dict(name="crop-43", config="crop-43-config", fresh_pak=True, wav=False,
             args=["--aspect", "original", "--window", "960x720", *WIDE, *crop], scenarios=[
            ("crop_level_43", "4:3 --border fill: the level picture reaches all four edges", [
                *_level("crop43", [], boot=boot, to_level=to_level, setting="fill"),
            ]),
            ("crop_pause_43", "4:3 --border fill: the pause menu's bands are still (rows too)", [
                *pause,
                ("wait", 20),
                ("edges", 45, 4.0, "rows"),
                ("border", "crop43_pause", "picture"),
            ]),
        ]),
        dict(name="border-default", config="border-default-config", fresh_pak=True, wav=False, fresh_files=["graphics.cfg"],
             args=["--aspect", "16:9", "--window", "1280x720", *WIDE], scenarios=[
            ("border_default", "the default (border=original, nothing on the command line): the black frame is there and no task is cropped", [
                *_level("orig169", [], boot=boot, to_level=to_level, setting="original"),
                ("file_contains", "graphics.cfg", r"(?m)^border=original$"),
            ]),
        ]),
    ]
    # Before / after evidence (optin): the level and the pause menu, both settings, 16:9 and 21:9.
    for asp, win in (("169", "1920x1080"), ("219", "1920x810")):
        for setting in ("original", "fill"):
            sessions.append(dict(
                name=f"crop-shots-{asp}-{setting}", config=f"crop-shots-{asp}-{setting}-config", fresh_pak=True, wav=False, optin=True,
                args=["--aspect", "16:9" if asp == "169" else "21:9", "--window", win, *WIDE, "--border", setting], scenarios=[
                (f"crop_shots_{asp}_{setting}", f"evidence: {asp} border={setting}, level and pause", [
                    *_level(f"crop_{asp}_{setting}", [], boot=boot, to_level=to_level, setting=setting),
                    *pause,
                    ("wait", 20),
                    ("border", f"crop_{asp}_{setting}_pause", "picture" if setting == "fill" else "black"),
                ]),
            ]))
    return sessions


def crop_two_players(to_level, pause):
    """Two players at 16:9 with `--border fill` (to_level: the game's steps from boot to both views in the level; pause: its steps into
    the paused split screen): each split view's own 8 px inset scissor goes to the frame's edges too (src/border_crop.cpp,
    `crop_halves`), so the split screen has no black frame either. On the paused frame, `border original` (live) brings the frame
    back and changes nothing else: the two pictures differ only in the former frame (at most 15 % of the pixels; the frame is
    about 11 %), so both views keep their place and their widening."""
    return dict(name="two-players-169-crop", config="two-players-169-crop-config", fresh_pak=True, wav=False, present_every=10,
                args=["--skip-intro", "--hidden", "--aspect", "16:9", "--window", "1280x720", "--res-scale", "2", "--border", "fill"], scenarios=[
        ("two_players_169_crop", "two players at 16:9, --border fill: both split views reach the window's edges; original brings the frame back", [
            *to_level,
            ("wait", 30),
            *pause,
            ("wait", 60),                                                     # the pause fade is over
            ("border", "two_players_169_crop", "picture"),
            ("ctl_match", "stats", r"crop=1 crop_tasks=[1-9].* crop_halves=[1-9]"),
            ("ctl_match", "split", r" views=[1-9]"),
            ("keep_present", "two_players_169_crop_on", "now"),
            ("ctl", "border original"), ("wait", 15),
            ("border", "two_players_169_crop_off", "black"),
            ("keep_present", "two_players_169_crop_off", "now"),
            ("present_cmp", "two_players_169_crop_on", "two_players_169_crop_off", 0.01, 0.15),
            ("ctl", "border fill"),
        ]),
    ])
