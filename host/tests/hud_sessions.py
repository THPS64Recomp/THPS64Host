"""HUD placement sessions (src/hud_anchor.cpp) for THPS2 / THPS3 (THPS1's are in its own scenarios.py; same shape).

hud_sessions(to_level, *, pause_menu, end_run, view_goals) builds, from the game's own navigation steps:
  hud          the DEFAULT (`expand`, 2026-10-14) at 16:9: the in-level HUD is anchored (hud counters: anchored tasks, left groups), the pause menu is not
  hud-original explicit `--hud original` at 16:9: nothing is anchored (the picture from before HUD placement)
  hud-43       the default at 4:3 (`--aspect original`): the setting says expand, nothing is anchored
  hud-<orig169|orig219|edge169|edge219|clamp219>   optin present captures of the same states (hud_<var>_<state>.png in the run's out dir):
               goals page, level, trick, pause, goals from the pause menu (THPS3), the end-of-run pages; run by name.
  hud-dbg      optin: `--hud-debug`: one log line per anchored group
`to_level`: steps from boot to the first in-level frame (the level's goals page -> in_level). The menu-hud session is in menu_session.py.
two_players_split_original(to_level) builds two-players-169-splitorig for all three games (split original: each HUD stays with its view).
"""

WIDE = ["--window", "1280x720", "--res-scale", "2", "--msaa", "4", "--hidden"]


def hud_sessions(to_level, *, pause_menu, end_run, view_goals=None, boot=(), counters=r"groups left=[1-9]\d* "):
    sessions = [
        dict(name="hud", config="hud-config", fresh_pak=True, wav=False, present_every=30,
             args=["--aspect", "16:9", *WIDE, "--skip-intro"], scenarios=[
            ("hud_edges", "default HUD placement (expand) at 16:9: the in-level HUD is anchored (score / special meter at the left edge); the pause menu is not", [
                *boot, *to_level,
                ("wait", 60),
                ("ctl_match", "hud", r"mode=expand supported=1 tasks_anchored=[1-9]\d* " + counters + r".* overlay_tasks=0 "),
                ("keep_present", "hud_edges_level"),
                ("go", "START", "in_level", "pause"),
                ("wait", 30),
                ("ctl_match", "hud", r"overlay_tasks=[1-9]"),
                ("keep_present", "hud_edges_pause"),
            ]),
        ]),
        dict(name="hud-original", config="hud-original-config", fresh_pak=True, wav=False, present_every=30,
             args=["--aspect", "16:9", *WIDE, "--skip-intro", "--hud", "original"], scenarios=[
            ("hud_original_inert", "explicit --hud original at 16:9: nothing is anchored", [
                *boot, *to_level,
                ("wait", 60),
                ("ctl_match", "hud", r"mode=original supported=1 tasks_anchored=0 groups left=0 centre=0 right=0 overlay_tasks=0"),
                ("keep_present", "hud_original_level"),
            ]),
        ]),
        dict(name="hud-43", config="hud-43-config", fresh_pak=True, wav=False,
             args=["--aspect", "original", "--window", "960x720", "--hidden", "--skip-intro"], scenarios=[
            ("hud_default_43", "default HUD placement at 4:3 (--aspect original): the setting says expand, nothing is anchored", [
                *boot, *to_level,
                ("wait", 60),
                ("ctl_match", "hud", r"mode=expand supported=1 tasks_anchored=0 groups left=0 centre=0 right=0 overlay_tasks=0"),
            ]),
        ]),
    ]
    for var, asp, win, hud in (("orig169", "16:9", "1280x720", "original"), ("orig219", "21:9", "1280x540", "original"),
                               ("edge169", "16:9", "1280x720", "expand"), ("edge219", "21:9", "1280x540", "expand"),
                               ("clamp219", "21:9", "1280x540", "16:9")):
        sessions.append(dict(
            name=f"hud-{var}", config=f"hud-{var}-config", fresh_pak=True, wav=False, present_every=6, optin=True,
            args=["--aspect", asp, "--window", win, "--res-scale", "2", "--msaa", "4", "--hidden", "--skip-intro", "--hud", hud], scenarios=[
            (f"hud_{var}", f"HUD placement {hud} at {asp}: level, trick, pause, goals, end of run (present captures)", [
                *boot,
                *to_level[:-1],
                ("wait", 20), ("keep_present", f"hud_{var}_goals"),
                to_level[-1],
                ("wait", 90), ("keep_present", f"hud_{var}_level"),
                ("vpad", "attach"), ("vpress", "rightstick"), ("wait", 120),
                ("ctl", "vpad axis lefty -32000"),
                *[step for i in range(4) for step in (("vpress", "a"), ("wait", 22), ("vpress", "x"), ("wait", 4), ("keep_present", f"hud_{var}_trick{i}a"), ("wait", 8),
                                                       ("vpress", "b"), ("wait", 6), ("keep_present", f"hud_{var}_trick{i}b"), ("wait", 20), ("keep_present", f"hud_{var}_trick{i}c"),
                                                       ("wait", 30), ("ctl", f"vpad axis leftx {-32000 if i % 2 else 32000}"))],
                ("ctl", "vpad axis lefty 0"), ("ctl", "vpad axis leftx 0"),
                ("wait", 60), ("keep_present", f"hud_{var}_land"),
                ("go", "START", "in_level", "pause"),
                ("wait", 30), ("keep_present", f"hud_{var}_pause"),
                *([("menu", "pause", view_goals), ("press", "A"), ("wait", 40), ("keep_present", f"hud_{var}_viewgoals"),
                   ("press", "B"), ("wait", 40)] if view_goals is not None else []),
                ("menu", "pause", end_run), ("press", "A"), ("wait", 20), ("keep_present", f"hud_{var}_stats"),
                ("wait", 60),
                ("press", "A"), ("wait", 60), ("keep_present", f"hud_{var}_endmenu"),
                ("press", "DOWN"), ("wait", 20), ("press", "A"), ("wait", 60), ("keep_present", f"hud_{var}_endgoals"),
                ("press", "B"), ("wait", 40), ("keep_present", f"hud_{var}_endmenu2"),
            ]),
        ]))
    # The run ends by itself (2:00 timer): "time's up" and the run-over pages, not from the pause menu.
    sessions.append(dict(
        name="hud-timeup", config="hud-timeup-config", fresh_pak=True, wav=False, present_every=30, optin=True,
        args=["--aspect", "16:9", "--window", "1280x720", "--res-scale", "2", "--msaa", "4", "--hidden", "--skip-intro", "--hud", "expand", "--hud-debug", "100000"], scenarios=[
        ("hud_timeup", "the timer runs out: time's up and the run-over pages (present captures)", [
            *boot, *to_level,
            ("wait", 3300),
            *[step for i in range(14) for step in (("wait", 45), ("keep_present", f"timeup_{i:02d}"))],
            ("press", "A"), ("wait", 60), ("keep_present", "timeup_A1"),
            ("press", "A"), ("wait", 60), ("keep_present", "timeup_A2"),
        ]),
    ]))
    # THPS3 shows "HEELFLIP + OVER THE HALFPIPE" style combo lines, drawn as glyph rects only (the first glyph lies left of the string's centre):
    # the line must stay centred. The keys press at once after the level starts (the skater rolls toward the halfpipe gap).
    sessions.append(dict(
        name="hud-combo", config="hud-combo-config", fresh_pak=True, wav=False, present_every=3, optin=True,
        args=["--aspect", "16:9", "--window", "1280x720", "--res-scale", "1", "--msaa", "0", "--hidden", "--skip-intro", "--hud", "expand", "--hud-debug", "100000"], scenarios=[
        ("hud_combo", "combo line over a gap (present captures)", [
            *boot, *to_level,
            ("wait", 90),
            ("vpad", "attach"),
            ("skate", [("vpad", "a", 1), ("wait", 10), ("vpad", "a", 0), ("wait", 3),
                       ("axis", "leftx", 32767), ("vpad", "x", 1), ("wait", 4), ("vpad", "x", 0), ("axis", "leftx", 0)], "air"),
            ("wait", 400),
        ]),
    ]))
    # Skate around for a minute (forward stick, ollie / flip / grab / grind in a loop): every kind of trick line and gap text shows up in host.log
    # (--hud-debug lists the groups whose anchor the whole-group rule changed against the first-rect rule).
    sessions.append(dict(
        name="hud-skate", config="hud-skate-config", fresh_pak=True, wav=False, optin=True, present_every=3,
        args=["--aspect", "16:9", "--window", "1280x720", "--present-size", "640x360", "--res-scale", "1", "--msaa", "0", "--hidden", "--skip-intro", "--hud", "expand", "--hud-debug", "1000000"], scenarios=[
        ("hud_skate", "skate around for a minute (host.log)", [
            *boot, *to_level,
            ("wait", 120),
            ("vpad", "attach"), ("vpress", "rightstick"),
            ("ctl", "vpad axis lefty -32000"),
            *[step for i in range(16) for step in (("vpress", "a"), ("wait", 25), ("vpress", "x"), ("wait", 15), ("vpress", "b"), ("wait", 40), ("vpress", "y"), ("wait", 30),
                                                    ("ctl", f"vpad axis leftx {-32000 if i % 2 else 32000}"))],
            ("ctl", "vpad axis lefty 0"),
        ]),
    ]))
    sessions.append(dict(
        name="hud-dbg", config="hud-dbg-config", fresh_pak=True, wav=False, optin=True, present_every=3,
        args=["--aspect", "16:9", "--window", "1280x720", "--res-scale", "1", "--msaa", "0", "--hidden", "--skip-intro", "--hud", "expand", "--hud-debug", "100000"], scenarios=[
        ("hud_dbg", "debug: one log line per anchored group (host.log)", [
            *boot, *to_level,
            ("wait", 90),
            ("keep_present", "dbg_level"),
            ("vpad", "attach"), ("vpress", "rightstick"), ("wait", 120),
            ("skate", [("vpad", "a", 1), ("wait", 10), ("vpad", "a", 0), ("wait", 3),
                       ("axis", "leftx", 32767), ("vpad", "x", 1), ("wait", 4), ("vpad", "x", 0), ("axis", "leftx", 0)], "air"),
            ("wait", 30),
            ("go", "START", "in_level", "pause"),
            ("wait", 30),
            ("keep_present", "dbg_pause"),
            ("menu", "pause", end_run), ("press", "A"), ("wait", 20), ("keep_present", "dbg_end0"),
            ("wait", 60), ("keep_present", "dbg_end1"),
            ("press", "A"), ("wait", 60), ("keep_present", "dbg_end2"),
            ("press", "A"), ("wait", 60), ("keep_present", "dbg_end3"),
        ]),
    ]))
    return sessions


def two_players_split_original(to_level, *, win="1280x720"):
    """two-players-169-splitorig: two players at 16:9 with `--split original` and the default HUD placement (`expand`), from the game's own
    two-player steps (boot to the split-screen level). Both views stay in the 4:3 middle, so each player's HUD keeps the game's own placement:
    the `split` counters count those tasks (`kept`) and place no group (q0-q4, centred all 0), and the side bands are black
    (`present_bands_clear`: no HUD block at the window's edges, no clear colour; `bands` counts the whole-width fills the RT64 patch kept out
    of them). The 2026-10-25 play test found the one-player rules running there (player 1's block at the window's left edge, player 2's SCORE
    label in the middle and its digits and meter at the right edge) and, in THPS2, the sky colour filling the bands. Shared by THPS1-3."""
    return dict(name="two-players-169-splitorig", config="two-players-169-splitorig-config", fresh_pak=True, wav=False, present_every=10,
                args=["--skip-intro", "--hidden", "--aspect", "16:9", "--window", win, "--res-scale", "2", "--split", "original", "--hud", "expand"],
                scenarios=[
        ("two_players_169_splitorig", "two players at 16:9, split original, hud expand: black side bands, each HUD block with its view in the 4:3 middle", [
            *to_level,
            ("wait", 60),
            ("present_bands_clear", win, 0.02),
            ("keep_present", "two_players_169_splitorig", "now"),
            ("ctl_match", "split", r"wide=0 .* groups q0=0 q1=0 q2=0 q3=0 q4=0 centred=0 kept=[1-9]\d* bands=\d+"),
            ("ctl_match", "hud", r"mode=expand "),
        ]),
    ])
