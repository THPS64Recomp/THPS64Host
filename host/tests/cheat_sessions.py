"""THPS2/3 cheat sessions (the game's own cheats through the Cheat_IsOn hook, game/cheats.cpp). Shared by both games' scenarios.py.

cheats-save-off: OPTIONS > CONTROLLER PAK > SAVE DATA (name "A") on a fresh pak with no cheat; the pak is kept.
cheats-on: every cheat on the command line. The same save is byte-identical (the hook writes no game memory); then in the first level:
  Slow-Nic: the physics step (gTimeScale) is a quarter (64) on some frames of the trick inputs; Always Special: the game asked and was told
  "on"; the pause menu: the game asked for Skip To Restart and was told "on" (the entry shows: evidence frame). Perfect Balance is asked
  only on a rail (not reached reliably by the scripted inputs): its answer count is a note.
"""


def cheat_sessions(boot, to_level, to_pak_save, trick):
    """boot: to the main menu; to_level: main menu -> the level is running; to_pak_save: main menu -> SAVE DATA written (save_ok);
    trick: vpad inputs of one round of skating."""
    cheats = ["--perfect-balance", "--slow-motion", "--always-special", "--skip-to-restart", "--unlock-levels"]
    return [
        dict(name="cheats-save-off", config="cheats-save-off-config", fresh_files=["cheats.cfg"], fresh_pak=True, wav=False, args=["--skip-intro"], scenarios=[
            ("cheats_save_off", "no cheat: SAVE DATA on a fresh pak; the pak is kept for the comparison", [
                *boot, *to_pak_save, ("pak_keep", "cheats_off"),
            ]),
        ]),
        dict(name="cheats-on", config="cheats-on-config", fresh_files=["cheats.cfg"], fresh_pak=True, wav=False, needs=["cheats-save-off"], args=["--skip-intro"] + cheats, scenarios=[
            ("cheats_on_save", "every cheat on: the same SAVE DATA is byte-identical to the one without cheats", [
                ("log", r"cheats on: unlock_levels, perfect_balance, slow_motion, skip_to_restart, always_special"),
                *boot, *to_pak_save, ("pak_same", "cheats_off"),
            ]),
        ]),
        dict(name="cheats-on-level", config="cheats-on-level-config", fresh_files=["cheats.cfg"], fresh_pak=True, wav=False, args=["--skip-intro"] + cheats, scenarios=[
            ("cheats_on_level", "in the first level: Slow-Nic quarters the step on trick frames; Always Special and Skip To Restart are asked and 'on'", [
                *boot, *to_level, ("wait", 30),
                ("vpad", "attach"),
                ("skater_watch", 60, trick, ["Skater_PhysState", "@gTimeScale"], ("some", "@gTimeScale", 64)),
                ("ctl", "vpad axis lefty 0"), ("ctl", "vpad axis leftx 0"),
                ("cheat_yes", "slow_motion", 1),
                ("cheat_yes", "always_special", 1),
                ("cheat_yes", "perfect_balance", 0),
                ("press", "START"), ("wait", 40), ("shot", "pause_menu"),
                ("cheat_yes", "skip_to_restart", 1),
            ]),
        ]),
    ]
