# Changelog

All notable changes to THPS64Host are listed here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). When a game has to change something to keep working with a
release, the release lists it under a **Games must** heading.

## [Unreleased]

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

[Unreleased]: https://github.com/THPS64Recomp/THPS64Host/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/THPS64Recomp/THPS64Host/releases/tag/v0.1.0
