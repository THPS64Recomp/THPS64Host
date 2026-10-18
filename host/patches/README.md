# Runtime patches and RT64 dependencies (register)

The host builds against **pinned** upstream commits (`tools/setup_runtime.py`, `PINS`). The script fetches each pin, checks its
SHA, and applies the patches below. This file lists:
- every change we carry on top of those pins;
- every place where host code depends on RT64 internals rather than its public interface.

Read it before moving a pin. Update it whenever a patch is added, removed or upstreamed.

| Component | Pin |
|---|---|
| N64ModernRuntime | `cdf5abbd5026fef5c364c676e4667c45e42b6863` |
| RT64 | `43373749dac9bbc1b653e6a02aed40a9e1783bed` (plume `d890ac8` as its submodule) |

## Patches applied by `setup_runtime.py`

| Patch | Applies to | Size | Why | Proven by | Upstream |
|---|---|---|---|---|---|
| `rt64-hlslpp-labs.patch` | RT64 `src/common/rt64_hlslpp.h` | 3 lines | Recent macOS SDKs no longer pull in `<stdlib.h>` through `<math.h>`, so the scalar `hlsl++` path fails to compile (`labs`). Same fix as CBFD-Recompiled. | The macOS build itself. | Candidate (trivial). |
| `rt64-present-capture-widescreen.patch` | RT64 present queue, framebuffer renderer, projection processor; plume copy paths | 234 lines | Present capture for the test harness (every Nth composited frame read back; composite without a swap chain for hidden windows), plus widescreen fixes. | Every session that uses `pcap` / present checkpoints; the widescreen and HUD sessions (`ws`, `wsel`, `hud*`). | Ours. Capture is test tooling; widescreen parts could be offered piecemeal. |
| `rt64-hfr.patch` | RT64 interpreter, workload queue; new `rt64_hfr_stats.h` | 99 lines | Frame-interpolation counters (`RT64::hfrStats`) **and the interpreter command hook** (`Interpreter::commandHook`). The host sees each display-list command as RT64 parses it. All of our interpolation tagging goes through this hook. | `hfr`, `ids*`, `hole` (THPS2) sessions. | Candidate: the hook as an official extension point. |
| `rt64-metal-clear-state.patch` | plume `plume_metal.cpp` | 10 lines | Metal partial clears drew a quad but restored the old state cache, so the next draw ran with the clear pipeline and vanished. Found on THPS1's name-entry row. Metal only. | THPS1 `name-entry` session. | Candidate (plume bug, general). |
| `ultramodern-sp-complete-after-dl.patch` | N64ModernRuntime `ultramodern/src/events.cpp` | 5 lines | The runtime told the game "RSP done" before RT64 had parsed the display list. The game then rewrote its buffers while RT64 was still reading them: render-thread crashes in `processDisplayLists` / `addFramebuffer`. | Crash canary (`THPS_DL_CANARY=1`, `THPS_DL_STALL_MS`), the soak test. | Candidate (affects any game that reuses its buffers immediately). |

## Patches in this folder that are **not** applied

| Patch | Status |
|---|---|
| `ultramodern-vi-rate.patch` | **Dormant.** It adds `get_vi_rate()`/`set_vi_rate()` for real PAL 50 Hz. Nothing in the host calls them, and it isn't in `NMR_PATCHES`. Real PAL is not done (Europe runs at 60 Hz). When PAL work starts: add it to `NMR_PATCHES` and call `set_vi_rate(50)` from the game's init hook. |

## Host code that depends on RT64 internals

These aren't patches, but an RT64 update can break them. Breakage can show up as a compile error (loud), or as the code
silently stopping to apply (quiet; each item has a test for that case).

| Host code | Uses | Kind | Guard |
|---|---|---|---|
| `src/interpolation.cpp`: camera cut tagging, projection IDs (camera vs. planar-shadow kind) | `RSP::matrixId` | **Public:** RT64's extended-GBI matrix-group interface, the same one games use (Zelda64Recomp tags its camera with it). | `hfr`, `hole` |
| `src/interp_ids.cpp`: per-transform interpolation IDs | `RSP::matrixId`, `hfrStats` | Public plus our counters. | `ids*` |
| `src/interpolation.cpp`: **view-projection re-split** | `RSP::viewMatrixStack`, `projMatrixStack`, `projectionMatrixStackSize` (written after each perspective load) | **Internal.** RT64 splits the game's combined view×projection taking the depth scale from one row. That row is ~0 when the camera faces the world z axis: the split goes NaN (frame treated as orthographic, nothing pairs, a 30 Hz step) or gets a wrong depth range (far scenery clipped frame to frame). We redo the split with the best-conditioned row. `--hfr-vp-split 0` turns it off. | `vp_split` step in THPS2 `hole` (fails if no camera load was re-split) |
| `src/interpolation.cpp`, `src/hfr.h` | `Interpreter::commandHook` | Ours (from `rt64-hfr.patch`). | as above |
| `src/hud_anchor.cpp`: HUD placement in widescreen | `RDP`, `ExtendedAlignment`, `DrawAttribute::Scissor` | Mostly public extended-GBI alignment, plus RDP state reads. | `hud*` sessions |
| `src/frametime.cpp` | `RasterShaderCache` counters | Internal (read-only, diagnostics). | `ftime` output only |
| `src/rt64_context.cpp`, `src/ui/*` | `Application`, `PresentQueue` (incl. the capture fields from our patch), RmlUi render interface | Mostly public setup interface, plus our capture fields. | Every session (nothing runs without them) |

## Moving a pin (checklist)

1. Read the upstream changes around every file in the tables above. If upstream fixed one of our issues, drop the patch
   or host workaround and note it here.
2. Update `PINS` and recreate the runtime from scratch: `python3 -I tools/setup_runtime.py <new empty dir>`. Never patch an
   existing tree by hand: the script can't tell what a hand-patched tree holds.
3. Rebase any patch that no longer applies (`git apply --3way`), and regenerate it with its header kept.
   **Changing a patch's content (here or at any other time) also means recreating the tree** that has the old version
   applied: a new empty DEST, or `setup_runtime.py DEST --recreate`. The marker (`<repo>/.thps-patches-applied`) records
   the pin and each applied patch's SHA-256, and setup refuses a tree whose patches no longer match the files (or that has
   the older filename-only marker) instead of reporting "already applied". Only a patch appended to the end of the list
   is applied on top of an existing tree.
4. Build all three games, static and split. Run every "Proven by" / "Guard" session above, then the full gate.
5. Update the Pin table and this register in the same commit as the pin change.
