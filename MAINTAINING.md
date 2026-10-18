# Maintaining the THPS64Recomp repositories

How the checks work, who runs which, and how a release is made. It covers the four repositories:
THPS64Host (this one) and the three games,
[THPS1-64Recomp](https://github.com/THPS64Recomp/THPS1-64Recomp),
[THPS2-64Recomp](https://github.com/THPS64Recomp/THPS2-64Recomp) and
[THPS3-64Recomp](https://github.com/THPS64Recomp/THPS3-64Recomp).

**The rule:** contributors run nothing special. CI checks every pull request on its own. The checks that need the game
(test suites, ROM scans, the provenance review, the release manifest, the push guard) are the maintainer's job.

## Who does what

| Who | Does | Needs |
|---|---|---|
| **Contributor** | Opens a pull request. Can run `tools/check.sh` first, to see what CI will say. | A clone. No ROM. |
| **CI** (GitHub Actions) | Builds every pull request on Linux, Windows and macOS with stand-in game code, and runs the ROM-free checks. | Nothing. It never has a ROM. |
| **Maintainer** | Reviews, runs the game's test suite before merging, pushes through the guard, makes releases. | Their own dumps of every game, and the maintainer tools. |

The maintainer tools (the export, the push guard, the release manifest) aren't in these repositories. They run in a
maintainer's checkout, next to the local ROM dumps they scan against.

## Every check

<!-- table-ok: the list of checks; no game data -->
| Check | Protects against | Who | When | Command | When it fails |
|---|---|---|---|---|---|
| **CI build and host pin** | A change that doesn't build on one OS; a game on an unreleased host | CI | Every PR and push to `main` | Runs on its own. Locally: `tools/check.sh` | An `::error` on the PR says what failed and how to fix it. See [Common failures](#common-failures-and-their-fixes). |
| **Host CI** | A host change that breaks one of the games | CI | Every THPS64Host PR | Runs on its own (builds each game's `main`, or a branch named like yours) | The failing game and OS show in the job name. Fix the host, or make the game change first ([Breaking host changes](#breaking-host-changes)). |
| **setup_runtime patch hashes** | A runtime tree that isn't the pinned upstream plus exactly our patches | Everyone, and CI | Every build | `python3 -I host/tools/setup_runtime.py DIR` | "changed since it was applied" or "no longer reverse-applies". Fix: a new empty `DIR`, or `--recreate`. |
| **Export and file inventory** (provenance review) | A file that is ROM-derived, decompiled or translated from the game | Maintainer | Each new or renamed file | `python3 -I tools/export_public.py --verify CHECKOUT --tree T --update-inventory` | "new file not in the export inventory". Fix: review the path, then commit the inventory. |
| **ROM-byte scan** | Bytes copied from a dump into any file | Maintainer | Every export, `--verify` and push | Part of the export and the guard | A file and offset, or a missing dump. Fix: remove the bytes, or add the dump. |
| **Doc, reference and link lints** | Disassembly in docs, long value tables, private paths, broken links | Maintainer | Every export, `--verify` and push | Part of the export and the guard | A file and line. Fix the doc. |
| **Push guard** (pre-push hook) | Anything above reaching GitHub, in any commit of the push; agent trailers in messages; a game pinned to an untagged host | Maintainer | Every `git push` | `tools/public_guard.sh CHECKOUT --ref BRANCH` | Each line says what failed and how to fix it. Never push with `--no-verify`. |
| **Bundle gate** | Generated game code, or a ROM, inside a release zip | CI (ROM-free part), maintainer (ROM scan) | Each release | `scan_bundle.py --no-rom DIR` (CI); `scan_bundle.py DUMP BLOB DIR --sanity` (each dump) | A file in the zip. Fix the packaging, rebuild, and rescan. |
| **Release manifest** | A release whose zips, commits or tags differ from what was tested and approved | Maintainer | Each release | `release_manifest.py init`, `approve`, `verify --publish` | `verify` names the field that differs. Rebuild, record again, and get a new approval. |
| **Test tiers** | Gameplay, graphics and audio regressions CI can't see | Maintainer | Before merging; before each release | `decomp/<game>/recomp-host/tools/test.sh` | Failing scenarios and their pictures under `build/tests/out/`. |

### The test tiers

| Tier | When | What |
|---|---|---|
| 1. Targeted | Every change | The test sessions for the area you touched (`test.sh --only NAME`). |
| 2. Batch | Before merging a PR that changes behaviour | The game's full suite and `test.sh --first-run`. All releases (`--version ID`) if version-specific code changed. |
| 3. Release | Each release | The full matrix on macOS and Windows, a soak (`test.sh --soak`), the ROM scans of the zips, and hand-play. |

A pull request from outside gets its tier 2 run by a maintainer before it's merged.

## tools/check.sh

One command for each repository. It prints each step, a `Fix:` line for each failure, and ends with "Ready for a PR."

- **`tools/check.sh`** (contributor mode, no ROM): the host pin, the version tables, a build with stand-in game
  functions (in `build/cmake-check`, so your real build is left alone), and the ROM-free checks CI runs (`--features`,
  `--check-rom` of an empty file exits 3, the controls.cfg test). In THPS64Host it checks each game cloned next to it
  (`../THPS1-64Recomp`, ...) against your host, as the host CI does, with one shared runtime (`build/runtime`, or
  `THPS_RUNTIME_DIR`). It warns about each game that isn't cloned there, with the clone command, and its last line names
  the games it checked and the ones it didn't: "Ready for a PR (checked: THPS1; not checked: THPS2, THPS3)." With no
  games next to it, it checks that our patches apply.
- **`tools/check.sh --maintainer`**: also a real build from your dumps, the split engine, the full suite and the first-run
  test (tier 2), then the push guard. It refuses to start when it can't find your dumps.
  - Dumps: the nearest `roms/` folder at or above the checkout, or `THPS_ROMS=DIR`.
  - The guard: the one the checkout's pre-push hook calls, or `THPS_GUARD=PATH`.
- **Windows:** run it in Git Bash.

## The release flow

### A game release

1. **Finish the changelog.** All PRs for the release are merged. Rename `## [Unreleased]` to `## [X.Y.Z] - <date, e.g. 2026-11-02>`
   and add an empty `## [Unreleased]` above it. The release notes come from this section, so write it for players.
2. **Start the manifest.** Pick the build commit on `main`, then:
   `release_manifest.py --manifest FILE init --scope thpsN --version X.Y.Z --build-sha SHA`.
3. **Test it.** Tier 3 at that commit.
4. **macOS zip.** Run the game's `release` workflow from the build commit (Actions > release > Run workflow). It checks the
   host pin, the changelog section and the tag, builds, runs the ROM-free bundle gate, and makes a **draft** release.
5. **Windows zip.** On Windows, from the same commit: a split build (`build_host.py --split --own-sdl2`), then
   `package_split.py --zip` and the bundle gate (the game's `decomp/thpsN/recomp-host/tools/` has all three). Upload the zip
   to the draft.
6. **Scan and record.** Download the zips from the draft (always the downloaded copies). Run the full ROM scan against every
   dump. Then `release_manifest.py --manifest FILE set-run ...`, `add-zip ...` and `verify`.
7. **Approve.** `release_manifest.py --manifest FILE summary`, then
   `approve --by NAME --zips DIR --remote https://github.com/THPS64Recomp`. Any later change to a zip or a commit clears the
   approval.
8. **Tag.** `git tag -a vX.Y.Z -m vX.Y.Z BUILD_SHA`, then push the tag (the guard checks it). The workflow never makes tags.
9. **Final gate, then publish.**
   `release_manifest.py --manifest FILE verify --publish --remote https://github.com/THPS64Recomp --zips DIR`.
   Publish the draft only when it passes. A `-preview` version is a prerelease.

A published release is never changed. A fix is a new version.

### A host release

1. Merge the host PR. Its CI has already built all three games against it.
2. Date the changelog section, then push an annotated tag at the merge commit: `git tag -a vX.Y.Z -m vX.Y.Z SHA`.
3. In each game, one small PR moves `decomp/common` to the new tag **and** changes the `@vX.Y.Z` and `host-tag:` lines in
   `.github/workflows/ci.yml` and `release.yml` to the same tag. Run tier 2 for that game, then merge.
4. Move all three games, even if only one needed the change, so they stay on the same host.

### Breaking host changes

If the games must change too, either land the host change so it accepts both the old and the new game side first, or use
**same-named branches**: a branch in each affected game with the same name as the host branch. The host CI builds those
branches instead of `main`, so the host PR can pass. Never move or delete a published tag.

## Common failures and their fixes

- **"new file not in the export inventory"** (export, `--verify`, guard). A new file needs a provenance review. Check that
  it's our own work, not ROM-derived or translated from the game. Then run the export with `--update-inventory`, read the
  diff, and commit the inventory.
- **"Host pin not a release"** (CI). `decomp/common` points at a commit that no annotated `vX.Y.Z` host tag names.
  Fix: `git -C decomp/common fetch --tags && git -C decomp/common checkout vX.Y.Z && git add decomp/common`.
- **"Workflow and submodule differ"** (CI). `decomp/common` and the workflows name different host tags. Fix: change every
  `@v...` and `host-tag:` in `.github/workflows/` to the submodule's tag, in the same commit.
- **"No CHANGELOG section"** or **"CHANGELOG section undated"** (release workflow). Fix: rename `## [Unreleased]` to
  `## [X.Y.Z] - <date, e.g. 2026-11-02>`, commit, and run the workflow again.
- **"changed since it was applied"** or **"no longer reverse-applies"** (`setup_runtime.py`). The runtime folder doesn't
  match the patch files, often after a patch was edited. Fix: run it into a new empty folder, or add `--recreate`. Edits
  to patched files belong in the patch files.
- **"the ... dump is not under .../roms"** (export, guard). The ROM scan needs every dump the repository supports, or bytes
  from the missing one would pass. Fix: add that dump to your local ROM dumps, or point `--data-root` at the folder that
  has them.
- **"carries an agent trailer"** (guard). A commit message has a `Co-Authored-By` line for an AI tool. Fix: reword the
  commit (`git commit --amend`, or a rebase for an older one).
- **"Tag names another commit"** or **"Draft targets another commit"** (release workflow). Fix: run the workflow from the
  tag's or the draft's commit, or delete the stale draft. Never move a tag.

## For agents

An agent helping a maintainer should follow this file and the README of the repository it works in.
- **No ROM-derived data in a commit**, ever: no dumps, extracted code or data, generated C, test pictures or captures.
- **Plain commit messages,** with no agent trailers (`Co-Authored-By` for a tool, "Generated with ..."). The guard
  refuses them.
- **Run `tools/check.sh` before saying a change is ready.** The ROM-based steps are the maintainer's to run.
- **Don't push, tag or publish** unless the maintainer asks for that step.
