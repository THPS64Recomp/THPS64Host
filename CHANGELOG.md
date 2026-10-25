# Changelog

All notable changes to THPS64Host are listed here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). When a game has to change something to keep working with a
release, the release lists it under a **Games must** heading.

## [Unreleased]

### Added
- Tests: `present_cmp`, a step that compares two screenshots taken in the same test. It can check the whole picture or
  only the widescreen side bands, and it can ignore spots that animate on their own.

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
