# Changelog

All notable changes to THPS64Host are listed here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). When a game has to change something to keep working with a
release, the release lists it under a **Games must** heading.

## [Unreleased]

## [0.2.0] - 2026-10-27

Split screen, Picture Border, rebinding, draw distance and the Original and Enhanced presets, used by the v0.2 releases of
all three games.

### Added
- Tests: `present_cmp`, a step that compares two screenshots taken in the same test. It can check the whole picture or
  only the widescreen side bands, and it can ignore spots that animate on their own.
- Two-player split screens in widescreen: a new RT64 patch (`rt64-splitscreen-widescreen.patch`) widens a pair of side-by-side
  views like a full-width one, and HUD placement puts each player's HUD block at its own view's edge (with HUD Placement
  `original` too: the views fill the picture there). 4:3 is unchanged. The runtime needs the new patch: `setup_runtime.py`
  applies it on top of an existing tree.
- Graphics setting `split = wide | original` (default `wide`), the menu row Graphics > Two-Player Split Screen and
  `--split wide|original`: `original` keeps both views in the 4:3 area with black side bands (the RT64 patch keeps the
  game's whole-width frame clear out of them), and each player's HUD stays where the game draws it, inside its view, whatever
  HUD Placement says. The control command `split [on|off]` switches it for one run (A/B) and
  prints the counters (`kept`: split-screen frames left at the game's own HUD placement; `bands`: fills kept out of the side
  bands). Test step `present_bands_clear`
  and the shared session `two-players-169-splitorig` (`tests/hud_sessions.py`).
- Graphics setting `border` (`original` | `fill`, default `original`), the menu row Graphics > Picture Border and `--border`:
  `fill` removes the thin black frame around the 3D picture (the game's 8 px inset) by widening the inset scissor and clears in
  the display list (`src/border_crop.cpp`), in a two-player split screen each view's too (`crop_halves`); no zoom, no RT64
  change. The picture fills the frame the game left black: nothing is cut off. The control command `border original|fill`
  switches it for one run. Test step `border` and the shared `tests/crop_sessions.py` (`crop_two_players` for the split
  screen).
- Settings menu, Controls tab: rebinding. Each N64 input has a row with two controller slots, two keyboard slots and a
  Default button; choosing a slot takes the next button, trigger, stick direction or key (Esc / Back cancel, Backspace
  empties the slot, 5 s timeout). A button another action already uses is swapped with it, except that menu accept and
  back (N64 A, B) may share a button with a trick, as the presets do. Changes work at once and are saved to
  `controls.cfg` (`preset = custom`; the first change of a session keeps the old file as `controls.cfg.bak`). A slot whose
  binding differs from the layout's default is drawn in yellow (`ui state` lists them as `marked=`), and a Reset all to
  default button puts every binding back to the layout loaded or picked last, after a second press to confirm.
- Settings menu, Controls tab: a Controllers list with each controller's player and the buttons it presses right now,
  Player 1 / Player 2 buttons and Swap players 1 and 2. One controller can be player 2 while the keyboard alone is
  player 1. The choice lasts until you quit.
- Tests: `menu-controls`, `menu-controls-reload` and `menu-ports` sessions (virtual pads), the `ui_nav`, `ui_status` and
  `file_lacks` steps, and `--control "ports [swap | assign N P]"`.
- Native `memset` and `memcpy` for the games' C library (`thps_memset` / `thps_memcpy`, hooked by name), byte-for-byte
  the same as the MIPS loops. They halve the game-thread stall at a level start. A game uses them by adding the
  `memset_recomp` and `memcpy_recomp` hooks to its hooks file.
- `ftime` trace spans (ERZ, DMA, the mem hooks, `send_dl`, the audio task, RT64's workload thread) in `ftime stop FILE`,
  the test steps `sample`, `sample_wait` and `ftime_max`, and `tests/load_freeze.py` (sessions `lf-probe`, `lf-check`).
- Draw distance (`draw_distance` in `graphics.cfg`, `--draw-distance`, a Graphics-tab row: `original`, `2x`, `4x`,
  `full`): the gameplay camera's far plane x2, x4 or out to the whole course, with a governor that steps back while one
  of the game's per-frame buffers is over 85 % full (the display list, the matrix and vertex pools, and the renderer's
  mesh lists). The course items the setting adds are drawn first, farthest first, in passes small enough for the
  renderer's fixed lists, and the game's own set last, so a long distance never drops near meshes. The games always set
  up bigger matrix pools (THPS2/3 at a course start, THPS1 at start-up) and keep their own cap in force at `original`,
  so the setting applies without a restart. THPS1 applies a change at once. In THPS2/3 a shorter distance applies at
  once; a longer one reaches the far scenery after a RETRY, the next course or a camera change. THPS2/3 also give their
  course display lists 144 KiB each instead of 72. In THPS2/3 an extra pass is drawn only while the display list still
  has room for it and for the rest of the frame (a backstop; `dd` counts its cuts). Graphics setting `dd_buffers =
  bigger | original` (`--dd-buffers`, default `bigger`, not in the menu): `original` keeps the games' own allocations
  exactly. It is read at start-up, so a change needs a restart. Games supply the hooks in `game/draw_distance.cpp`. A
  game without it still builds, and the setting then changes nothing.
- Control command `dltrace N`: trace the display lists of the next N frames. `dltrace` alone still traces one.
- Graphics setting `fog = original | soft | off` (default `original`) in `graphics.cfg` and `--fog`. It is not in the
  menu or a preset: in playtesting it made little visible difference, and at Full draw distance Soft equals the game's
  fog. The games' fog is thin by design: they write one fixed fog command each frame with the steepest ramp the N64
  allows, over about the last 13 % of the view at the game's draw distance. `soft` starts the ramp at 70 % of the far
  plane in use, so distant scenery fades in instead of popping in; it is never steeper than the game's ramp, so it only
  adds fog, and at long draw distances (Full) the game's own ramp stays. `off` turns it off, which shows a long draw
  distance most clearly. Done in the display list (`src/fog.cpp`): only the game's exact fog command is rewritten, so
  menus, the HUD and the sky are unaffected, and no RT64 change is needed. The control command `fog original|soft|off
  [START]` switches it for one run and prints the counters.

### Changed
- Frame Rate (`refresh` in `graphics.cfg`, `--refresh`, the Graphics tab) is now `original` (30) or `60`, default `60`; `display`
  and 120 are gone, because 120 Hz (what "Match display" gave on a ProMotion Mac) paced badly. `graphics.cfg` revision 3: a file
  from before it with `refresh=display` or another rate moves to 60 once, with a log line. Measurements can still ask for another
  rate with `THPS_TEST_REFRESH=display|N` (environment, not saved).
- Graphics presets: a Preset row at the top of the Graphics tab, `--preset original|enhanced`. Original is the N64 look
  (aspect, HUD, split and border `original`, frame rate 30, draw distance `original`); Enhanced is 16:9, HUD `expand`,
  split `wide`, border `fill`, 60 and draw distance `full`. Choosing one sets and saves those six rows; the menu
  shows Custom when they match neither. The preset is read back from the values, never stored. **Enhanced is the new default**
  for a fresh config: `border` now defaults to `fill` and `draw_distance` to `full`, and `draw_distance` is
  always written. An existing
  `graphics.cfg` keeps every value it has; a key it lacks takes the new default. Tests: the harness adds the old defaults
  (`harness.TEST_GFX_DEFAULTS`: 16:9, expand, wide, border original, 60, draw distance original, fog original) for each key a session's
  `graphics.cfg` lacks, so sessions and reference pictures keep their meaning; `gfx_cfg_test` checks the presets, and the
  `menu-preset` session clicks them.
- The ERZ (RNC) decoder is about twice as fast (a lookup table for the Huffman codes; CRCs checked only in verbose
  mode). Its output is unchanged.
- The game library ABI is now 5 (4: the import table gains `thps_memset` and `thps_memcpy`; 5: the games import the
  draw-distance hooks). A library built by an earlier release is rebuilt once at the next start.
- Tests: a game can list the checkpoints whose screen differs on another release (`OWN_REFS` in its `tests/scenarios.py`).
  That release then never uses the first release's picture for them, and `test.sh --version ID --record` records its own.
  When the game ignores the first press on the way to such a screen, the recording presses again, as a normal run does.
- Tests: `test.sh` and `tests/run.py` check the ROM before starting: that the file exists, and that it is the release under
  test. `--version thps1_eu` also reads `THPS1_ROM_EU` (and so on), as the first-run test does. In Git Bash a
  `/c/...` path in `THPS<N>_ROM` is converted for the game.
- Tests: on Windows the notes say "load n/a" instead of a load of 0.0.
- `build_host.py` without `--runtime` reuses the runtime folder of your last build, so `tools/check.sh` doesn't set up a
  second one. Its closing hint names `tools/test.sh`, and `test.sh --first-run` says how to build the split engine.

### Fixed
- THPS2/3: the east-button split (flip while skating, back in menus) turned off as soon as `controls.cfg` stopped
  matching a preset, so Xbox B flipped and grabbed at once. It now follows the bindings.
- A `controls.cfg` whose `preset =` line names a preset but whose bindings were edited by hand loads as `custom`.
- Pad navigation in the settings menu reaches elements inside a scrolling list.
- Tests: the `menu` step checks the screen every 8 frames instead of every 9. The games rotate three framebuffers,
  so a check every 9 frames always read the same one, and when that one copy was out of date (THPS2 Europe, NY City,
  draw distance Full) the test missed a pause menu that was on screen. It failed about one run in three.
- Tests: the first-run test `picker_bypass` gives the game 8 s on Windows, as `app_update` does. On a slower PC the game
  could still be starting at the 3 s limit, and the run then exits 1.

### Games must
- Rebuild their game library: `THPS_GAME_ABI` is 5 (the draw-distance hooks in `game_hooks.def`).

## [0.1.0] - 2026-10-18

The first release, used by THPS1 64: Recompiled v0.1.0 and the THPS2 and THPS3 v0.1.0-preview releases.

### Added
- The engine for the three games: start-up, hooks, input (keyboard and SDL controllers, two players, hot-plugging),
  audio, Controller Pak saves, widescreen with HUD placement, frame interpolation with per-transform IDs, ROM checks and
  the version tables, crash reports.
- The first-start build: the split engine builds the game library from the user's dump with the bundled N64Recomp and
  RSPRecomp and the system compiler (macOS) or a bundled llvm-mingw (Windows).
- The in-game settings menu (RmlUi): general, controls, graphics, sound and cheats.
- Cheats for testing, through the games' own flags and level select. The cheats aren't stored in the save; progress made
  with one on is saved as usual.
- Pinned N64ModernRuntime `cdf5abb`, RT64 `4337374` and N64Recomp `ffb39cd`, with our patches and their register in
  `host/patches/README.md`.
- The test harness (pad-driven sessions with picture checkpoints), the first-start tests and the soak test.
- Packaging for macOS (universal, ad hoc signed) and Windows x64, and the ROM-byte scanner for release bundles.

### Fixed
- The view-projection split is redone with the best-conditioned row when the camera faces along the world's z axis
  (RT64 lost the depth range there: jutter, far scenery popping and flashing logos).
- Audio output underruns fade out and back in instead of cutting to silence (short pops while paused or quiet).
- The runtime reported the graphics task done before RT64 had parsed its display list, which crashed RT64's parser
  (`ultramodern-sp-complete-after-dl.patch`).
- Windows: the settings migration was never saved, and the frame-interpolation time budget never applied.

[Unreleased]: https://github.com/THPS64Recomp/THPS64Host/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/THPS64Recomp/THPS64Host/releases/tag/v0.2.0
[0.1.0]: https://github.com/THPS64Recomp/THPS64Host/releases/tag/v0.1.0
