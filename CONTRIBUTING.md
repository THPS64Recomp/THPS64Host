# Contributing to THPS64Host

Thanks for helping. This repository is the shared host of the three games. Bug reports from players go to the game's
repository first; we move them here when the cause is in the host.

## Host or game?

- **Here:** anything that all games run: the engine in `host/src/`, the settings menu, the build (`host/cmake/`,
  `host/tools/`), our patches to N64ModernRuntime and RT64 (`host/patches/`), the test harness, and the per-game tools
  and facts table at the root.
- **In a game repository:** what only that game needs: its profile (`decomp/<game>/recomp-host/game/game.toml`), hooks,
  version tables, symbols, recipes and test scenarios. If only one game needs a change, it usually belongs there.

## Before you open a pull request

Clone the three games next to this repository and run `tools/check.sh` here. It builds each game against your branch with
no ROM, as CI does, and its last line names the games it checked. A host change reaches all three games, so check all
three. The README's "Changing the host" has the clone commands.

If you have your own dumps, also run the tests for the area you changed in each game it reaches. If you don't, that's
fine: a maintainer runs the full suites.

Add a line under `## [Unreleased]` in [CHANGELOG.md](CHANGELOG.md). If a game has to change something to keep working,
say so under a **Games must** heading.

## What happens to your PR

1. CI builds the `main` branch of all three games against your branch on Linux, Windows and macOS, without a ROM.
2. A maintainer reviews it and runs the games' full test suites locally before merging.
3. After the merge, a maintainer tags a host release, and each game moves its submodule to the new tag.

How the checks and releases work: [MAINTAINING.md](MAINTAINING.md).

## AI-assisted and hand-written contributions

Both are welcome. Say how the change was made (by hand, or with which tool), include tests or say which tests you ran,
and read and understand what you submit. You're responsible for it, however it was written.

**Don't forward our patches, or AI-written changes, to the upstream projects.** N64Recomp, N64ModernRuntime and RT64 don't
accept contributions written with LLMs. Upstream bugs go up as reports written by a maintainer, with a small repro.

Never commit ROMs or anything made from them. By contributing, you agree that your contribution is licensed under
GPL-3.0-only ([LICENSE](LICENSE)).
