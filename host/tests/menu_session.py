"""The in-game settings menu sessions of the THPS recomp regression suite (shared by THPS1-3; each game's scenarios.py appends them).

Data only; the steps (ui, ui_state, ui_never, ui_input, ui_frame, file_exists, file_contains) are in run.py. They need a build with
THPS_MENU (the default; `--features` says menu=1) and skip otherwise. No references: nothing here depends on which screen of the game
is behind the menu, only on what the menu draws and what it saves. A fresh settings folder per run.

  menu         first launch: the hint shows once and writes ui-hint-shown; Esc opens the menu over the game (composited frame), Esc closes;
               the pad is blocked from the game while it is open and reaches it after; mouse clicks change Show FPS, MSAA, Aspect, Resolution and Frame Rate and
               write general.cfg / graphics.cfg; the pad's Back opens, D-pad / LB / RB navigate, A picks the N64 layout, B closes, the
               volume slider moves (a click on its track, then the D-pad); the settings are written to controls.cfg / general.cfg.
  menu-hud     the HUD Placement radio (Graphics tab) writes graphics.cfg hud=... and the renderer gets it.
  menu-reload  a new process on the same folder: every setting persisted, and the hint does not show again.
"""

TAB_BAR = (0.05, 0.09, 0.50, 0.18)      # "General Controls Graphics Sound": bright text on the dark modal
HINT_BOX = (0.0, 0.86, 0.55, 1.0)       # bottom left: the hint toast

MENU_SESSIONS = [
    dict(name="menu", config="menu-config", fresh_pak=True, wav=False, requires="menu",
         fresh_files=["general.cfg", "graphics.cfg", "controls.cfg", "controls.cfg.bak", "controls.preset", "ui-hint-shown"],
         args=["--skip-intro", "--hidden", "--window", "1280x720"], scenarios=[
        ("menu_hint", "first launch: the hint toast shows (composited frame) once, then goes; ui-hint-shown is written", [
            ("ui_state", "open=0 hint_scheduled=1"),
            ("file_exists", "ui-hint-shown"),
            ("ui_state", "hint=1", 40),
            ("ui_frame", "menu_hint", HINT_BOX, 150, None),
            ("ui_state", "hint=0", 40),
        ]),
        ("menu_open_close", "Esc opens the menu over the game (composited frame), Esc closes it", [
            ("ui_frame", "menu_closed", TAB_BAR, 0, None),
            ("ui", "esc"),
            ("ui_state", "open=1 tab=0 focus=tab_general"),
            ("ui_frame", "menu_general", TAB_BAR, 250, None),
            ("ui", "esc"),
            ("ui_state", "open=0"),
        ]),
        ("menu_blocks_pad", "the menu blocks the controller from the game; after Esc it reaches it", [
            ("vpad", "attach"),
            ("ui", "esc"),
            ("ui_state", "open=1"),
            ("ui_input", "mark"),
            ("vpress", "start"), ("vpress", "a"),
            ("ui_input", "same"),
            ("ui", "esc"),
            ("ui_state", "open=0"),
            ("vpress", "start"), ("vpress", "a"),
            ("ui_input", "more"),
        ]),
        ("menu_settings_click", "clicks: Show FPS on, MSAA 2x, Aspect 21:9 -> general.cfg / graphics.cfg and the renderer", [
            ("ui", "esc"),
            ("ui_state", "open=1"),
            ("ui", "click #lbl_fps_On"),
            ("ui_state", "show_fps=1"),
            ("file_contains", "general.cfg", r"(?m)^show_fps = 1$"),
            ("ui", "tab 2"),
            ("ui", "click #lbl_ms_2"),
            ("ui", "click #lbl_ar_219"),
            ("ui", "click #lbl_rs_2"),
            ("ui", "click #lbl_rr_60"),
            ("ui_state", "gfx.msaa=2 gfx.aspect=21:9 gfx.res_scale=2 gfx.refresh=60"),
            ("file_contains", "graphics.cfg", r"(?m)^msaa=2$"),
            ("file_contains", "graphics.cfg", r"(?m)^aspect=21:9$"),
            ("file_contains", "graphics.cfg", r"(?m)^res_scale=2$"),
            ("file_contains", "graphics.cfg", r"(?m)^refresh=60$"),
            ("log", r"gfx: aspect=21:9 .* res_scale=2 .* msaa=2 .* refresh=60"),
            ("ui_frame", "menu_graphics", TAB_BAR, 250, None),
        ]),
        ("menu_pad", "pad: Back toggles, RB / D-pad / A pick the N64 layout (controls.cfg), a click on the volume track and the D-pad move it, B closes", [
            ("ui", "esc"),
            ("ui_state", "open=0"),
            ("ui", "pad back"),
            ("ui_state", "open=1 tab=0"),
            ("ui", "pad back"),
            ("ui_state", "open=0"),
            ("ui", "pad back"),
            ("ui", "pad rb"),
            ("ui_state", "tab=1"),
            ("ui_frame", "menu_controls", TAB_BAR, 250, None),
            ("ui", "pad down"),                      # from the tab bar into the panel: one of the two layout radios
            ("ui", "pad right"),
            ("ui_state", "focus=cl_n64"),
            ("ui", "pad a"),
            ("ui_state", "preset=n64"),
            ("file_contains", "controls.cfg", r"(?m)^preset = n64$"),
            ("ui", "pad rb"), ("ui", "pad rb"),
            ("ui_state", "tab=3"),
            ("ui", "pad down"),
            ("ui_state", "focus=main_volume_input"),
            ("ui", "click #main_volume_input"),      # a click on the track (its middle) jumps the knob there
            ("ui_state", "volume=50"),
            ("ui", "pad left"), ("ui", "pad left"), ("ui", "pad left"),
            ("ui_state", "volume=47"),
            ("file_contains", "general.cfg", r"(?m)^volume = 47$"),
            ("ui_frame", "menu_sound", TAB_BAR, 250, None),
            ("ui", "pad b"),
            ("ui_state", "open=0"),
        ]),
    ]),
    dict(name="menu-reload", config="menu-config", fresh_pak=False, wav=False, requires="menu", needs=["menu"],
         args=["--skip-intro", "--hidden", "--window", "1280x720"], scenarios=[
        ("menu_persist", "a new process on the same folder: the settings persisted; the hint does not show again", [
            ("ui_state", "hint_scheduled=0 show_fps=1 preset=n64 volume=47 gfx.msaa=2 gfx.aspect=21:9 gfx.res_scale=2 gfx.refresh=60"),
            ("ui_never", "hint=1", 20),
            ("ui", "esc"),
            ("ui_state", "open=1"),
            ("ui_frame", "menu_reloaded", TAB_BAR, 250, None),
            ("ui", "esc"),
        ]),
    ]),
    # The Cheats tab (src/cheats.cpp; tab 4): every game has Unlock All Levels. A click turns it on: cheats.cfg, the log line, "ui state"; a new
    # process keeps it (and logs it at start); a crash report names it. Off again at the end, so the folder is left as it was found.
    dict(name="menu-cheats", config="menu-cheats-config", fresh_pak=True, wav=False, requires="menu",
         fresh_files=["general.cfg", "graphics.cfg", "controls.cfg", "controls.cfg.bak", "controls.preset", "ui-hint-shown", "cheats.cfg"],
         args=["--skip-intro", "--hidden", "--window", "1280x720"], scenarios=[
        ("menu_cheats_click", "Cheats tab: Unlock All Levels On -> cheats.cfg, a log line, ui state; nothing is on before", [
            ("ui_state", "cheats=-"),
            ("ui", "esc"),
            ("ui_state", "open=1"),
            ("ui", "tab 4"),
            ("ui_state", "tab=4"),
            ("ui_frame", "menu_cheats", TAB_BAR, 250, None),
            ("ui", "click #lbl_ch_ul_On"),
            ("ui_state", "cheats=unlock_levels"),
            ("file_contains", "cheats.cfg", r"(?m)^unlock_levels = 1$"),
            ("log", r"cheats: unlock_levels on \(now on: unlock_levels\)"),
            ("ui", "esc"),
        ]),
    ]),
    dict(name="menu-cheats-reload", config="menu-cheats-config", fresh_pak=False, wav=False, requires="menu", needs=["menu-cheats"],
         args=["--skip-intro", "--hidden", "--window", "1280x720"], scenarios=[
        ("menu_cheats_persist", "a new process: the cheat is still on and logged at start; Off in the tab saves it", [
            ("log", r"cheats on: unlock_levels"),
            ("ui_state", "cheats=unlock_levels"),
            ("ui", "esc"),
            ("ui", "tab 4"),
            ("ui", "click #lbl_ch_ul_Off"),
            ("ui_state", "cheats=-"),
            ("file_contains", "cheats.cfg", r"(?m)^unlock_levels = 0$"),
            ("log", r"cheats: unlock_levels off \(now on: none\)"),
            ("ui", "esc"),
        ]),
    ]),
    dict(name="cheats-crash", config="cheats-crash-config", fresh_files=["cheats.cfg"], fresh_pak=True, wav=False, args=["--skip-intro", "--hidden", "--unlock-levels"], scenarios=[
        ("cheats_crash_log", "--unlock-levels: the crash report names the cheats that were on", [
            ("wait", 30),
            ("crash", r"cheats on \(test aids, not in the save\): unlock_levels"),
        ]),
    ]),
    # The menu's HUD Placement radio (graphics.cfg `hud`; every game has the row, game.toml hud_placement) writes graphics.cfg and reaches the renderer.
    dict(name="menu-hud", config="menu-hud-config", fresh_pak=True, wav=False, requires="menu",
         fresh_files=["general.cfg", "graphics.cfg", "controls.cfg", "controls.cfg.bak", "controls.preset", "ui-hint-shown"],
         args=["--skip-intro", "--hidden", "--window", "1280x720"], scenarios=[
        ("menu_hud_click", "HUD Placement (Graphics tab): 16:9 edges, then Screen edges -> graphics.cfg hud=... and the renderer", [
            ("ui", "esc"),
            ("ui_state", "open=1"),
            ("ui", "tab 2"),
            ("ui", "click #lbl_hud_169"),
            ("file_contains", "graphics.cfg", r"(?m)^hud=16:9$"),
            ("log", r"gfx: aspect=16:9 .* hud=16:9"),
            ("ui", "click #lbl_hud_expand"),
            ("file_contains", "graphics.cfg", r"(?m)^hud=expand$"),
            ("log", r"gfx: aspect=16:9 .* hud=expand"),
            ("ui", "click #lbl_hud_original"),
            ("file_contains", "graphics.cfg", r"(?m)^hud=original$"),
            ("ui", "esc"),
        ]),
    ]),
]
