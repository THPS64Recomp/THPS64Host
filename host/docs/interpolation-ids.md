# Matrix-group IDs for RT64 frame interpolation (`src/interp_ids.cpp`)

Shared by the THPS1, THPS2 and THPS3 hosts. On by default whenever frame interpolation is on (`refresh` above `original`); `--interp-ids off`
(or `--control ids off`) gives RT64's own automatic pairing, for A/B.

## 1. The problem

The games run their logic at 30 Hz. With `refresh = display` (or a number above 30) RT64 renders extra frames between two game frames: it pairs each
transform of the new frame with its counterpart of the previous frame and interpolates (position, rotation, ...) between them. Whatever it cannot
pair is drawn as it is in every in-between frame, i.e. at 30 Hz, while the rest moves at 60 Hz. In a THPS3 level that showed as

* skater jitter: the skater (and most props) stepped at 30 Hz against a smooth background;
* "pops": every few presented frames an in-between frame showed a different part of the room;
* in THPS1 and THPS2 the same, less often.

`--refresh original` has none of it, so it is ours, not the game's.

## 2. Root cause (measured, all three games, one engine)

Tools: `--control idsdump` (RT64's verdict, paired / unpaired / skipped, for every world transform of a frame, joined with a walk of the display list that
made it), `--control idscalls` (RT64's own call and candidate tables; needs an RT64 build with the call-table diagnostics, see section 8), and the
`idsstat` statistics (section 8).

1. **Three projection variants per frame.** Each frame loads the view-projection 7-10 times, in three families: (A) the world camera; (B) the same
   camera with a depth bias (a decal projection: `m22` and `m32` differ by about 0.002 and 0.3-0.6 in THPS1/3; THPS2 has two such variants); (C) a
   flattening matrix whose y row is zero: the planar shadow, which is the skater's own mesh drawn a second time through it (573 triangles, 45 world
   transforms in THPS3).
2. **RT64 pairs whole scenes by matrix distance.** `GameFrame::matchScenes` groups a frame's projections into scenes (identical view and projection) and pairs
   them with the previous frame's scenes by the smallest L1 difference of view + projection. A and B differ by less than the camera moves between two
   game frames, so in many frames the world scene (317 calls) was paired with the previous frame's decal scene (5 calls) and the reverse
   (`SCENE curProjs=4 prevProjs=2 curCalls=317 prevCalls=5`).
3. **Transforms are paired inside the scene pair.** With the scenes paired right, 313 of 321 THPS3 transforms paired (97 %, skater included); with them
   crossed 54 of 349 (only the skater scene, which pairs with itself). A crossed pair is also where the pops come from: static level pieces are
   paired with the wrong pieces and the in-between frame draws them half way between two places.
4. **Not the cause:** draw order, triangle counts, call hashes (the twins were there, 1.5-3 units apart), per-frame allocation as such. Matrix addresses
   are useless as identity, though: the games allocate matrices from per-frame pools that alternate, and a moving piece never has the same matrix address
   in two consecutive frames (measured).

Before / after of the pairing, `idsstat` (section 8), "moving" = a drawing transform whose matrix differs from every matrix of the previous game frame
(a static piece looks the same paired or not), four phases: standing, skating, turning, ollie:

| game | IDs off: moving paired | tier 1 (parent list, the first version) | tier 1 + tier 2 (default) | all transforms paired: off / tier 1 / tier 1 + 2 |
|---|---|---|---|---|
| THPS1 | 93 / 81 / 89 / 95 % | 98 / 99 / 99 / 97 % | 100 / 100 / 99 / 97 % | 65-89 / 98-99 / 99-100 % |
| THPS2 | 75-84 / 87 / 85 / 92 % | 92 / 91 / 91 / 97 % | 98 / 88 / 90 / 97 % | 87-96 / 95-98 / 96-99 % |
| THPS3 | 41 / 42 / 71 / 83 % | 94 / 87 / 91 / 93 % | 96 / 90 / 96 / 91 % | 15-91 / 95-98 / 97-99 % |

Read the tier 2 gain with care: it is a few points at most (THPS1 +1-2, THPS3 +2-5 in three phases, THPS2 +6 standing but -3 turning), because what it adds are
sprites and effects, and in the turning phase new objects entering view (which have no partner by definition and are counted as unpaired). Frame-neighbour
check of the clips (`interpstat`, share of presented frames above 1.0, skater box): THPS3 7.4 % (off) -> 0.2 %; THPS2 0.2-1.0 -> 0.4 %; THPS1 2.5 -> 0 %; the whole
frame 2.5 % -> 0 % in THPS3, unchanged (0.4 % / 0 %) elsewhere. Tier 2 does not move those numbers.

## 3. How other recomps do it

RT64's extended GBI gives a game the tools: matrix groups with an ID (`gEXMatrixGroup` and variants, push / pop, per-component interpolate or skip flags,
linear or automatic ordering), explicit view / projection matrices, `G_EX_ID_IGNORE` to skip interpolation for a transform.

* **Zelda64Recomp (Majora's Mask)**, read from its `patches/`: actor and skeleton limbs are tagged from patched game code with
  `ACTOR_TRANSFORM_ID_START + actor slot x limb count + limb` (a second range for post-draw matrices); skybox, particles, effects, UI each have a fixed
  ID range (`transform_ids.h`); the camera is one fixed ID, `gEXMatrixGroupSimple`, with a per-frame skip decided from the camera's own movement
  (predicted vs real eye / at, thresholds picked by testing, a few per-scene overrides); an actor that spawns or teleports is flagged skip for a frame.
* **Starfox64Recomp** does the same for its actors and limbs.

What we adopted: IDs by object identity; skip for one frame on a camera cut (`hfr camera cut` detector, which predates this); per-component AUTO flags
so RT64's own velocity check still refuses a jump; a fixed ID only where the identity is fixed.
What we did not: IDs from patched or hooked game code. It needs one hooked function per object type per game and these games draw everything from
display lists, which already carry the identity (section 4). Also rejected: tagging by matrix address (pools alternate), a bare nearest-neighbour or
draw-order rule (a model's pieces sit a few units apart, so nearest-neighbour unpairs the skater; ordinal pairing across a changing view set flew
THPS2's walls in the first prototype), and an RT64 patch for ID-aware scene pairing (section 7).

## 4. The mechanism

The host walks the gfx task's display list before RT64 reads it (the place `dl_fixes.cpp` works in), following `G_DL` calls and branches and the segment
table. It records, for every world transform RT64 will make (RT64 makes one at the first `G_VTX` after a matrix load, also after a projection load; the
workload's first, identity transform is not one of them):

* `parent`: the display list that holds the vertex load. A model piece is drawn by a static list at a fixed address, so this is its identity;
* the matrix translation, the matrix hash, and the draw shape (vertices, triangles, bursts of triangle commands, a hash of combiner / other mode /
  geometry mode): `sig`.

A **tracker** gives each transform of the 3D task the ID of the previous 3D task's transform that is the same piece. Tier 1, by `parent`:

1. same parent and identical matrix: the piece did not move (level geometry, parked props); exact, whatever the order;
2. what is left of that parent: one and one is the same object (a vehicle, a pedestrian); the same count, the k-th is the k-th (a model issues its pieces in
   the same order every frame), if each is within max(12, 4 x its last step) of where it should be; otherwise the nearest to the predicted position
   (previous position + previous step), only when clearly nearer than the runner-up (a near-tie is ambiguous and stays with RT64).

Tier 2, for transforms whose parent was not in the previous frame: the task's own list (it alternates between two buffers, so sprites and
effects drawn from it never repeat an address), new objects, particles. They are matched, with the same rules and half the distance, against the previous
frame's tracks that no list of this frame claims, by `sig`. A transform that matches nothing gets a fresh ID with no partner (it is new, or it
teleported: shown as is) unless it was ambiguous, then it stays automatic.

The ID reaches RT64 through `Interpreter::commandHook` (the existing patch in `rt64-hfr.patch`): when RT64 is about to run a model-view `G_MTX`, the host calls
`RSP::matrixId`, which is exactly what a `gEXMatrixGroup` command in the list would call. The scan counts model-view `G_MTX` commands in walk order
and checks the command address of each hook call against its own record; on a mismatch it stops tagging for the rest of the task and RT64 does what it did
before. Tagged transforms have linear ordering, decomposed, every component AUTO (what RT64 uses for untagged ones), so only the pairing changes.
Pairs by ID do not depend on scene pairing: the crossed scenes no longer matter for the world and the skater.

The camera needs nothing: RT64 maps all real projections of a frame whichever scenes they were paired with (the A and B views are identical; only the depth term
differs), shadow matrix included, see section 7.

## 5. Per-game data

One number in `game/game.toml`: `interp_ids_max_step` (world units a piece may move between two game frames and still be the same object; 100 in all
three; measured skater step 1.5-4, a respawn is several hundred). `--ids-max-step N` overrides it for a run. Nothing else is per game: the parent list,
the matrix and the draw shape are read from the display list. What would be per game if a game needed it: a list of display-list address ranges that are
rebuilt every frame (tier 2 finds them by themselves, by "this list was not there last frame").

## 6. Guards (what keeps a wrong pair from flying)

* A match needs the same parent (tier 1) or the same draw shape and an unclaimed parent (tier 2), a distance from the *predicted* position under the gate
  (max(12, 4 x last step), capped at `max_step`; tier 2 at half), and no near-tie.
* Static pieces are matched by identical matrix first, so a changed view set cannot pair a wall with another wall (this was the first prototype's bug).
* RT64's own check stays on (position AUTO: a jump of 5+ units that is 10 times the previous step is not interpolated).
* A camera cut (jump or turn beyond the detector's thresholds) tags every transform of that frame IGNORE and clears the tracks.
* A scan that disagrees with RT64's walk, or a task with no projection, tags nothing.
* Bounded work: the tracker uses a hashed exact pass and a spatial grid for the nearest search of big groups, and after 0.6 ms of its thread's CPU time (`ids budget`) the rest of a task is left to RT64's own search (`bailed` counter), so a huge scene cannot make a frame late.
  Where there is no thread CPU clock (Windows: `GetThreadTimes` ticks at about 15.6 ms, `QueryThreadCycleTime` counts cycles), the budget runs on wall time
  (steady clock) and is 3 times larger (1.8 ms by default): wall time also counts preemption (walks of 0.4 ms CPU were measured at up to 16 ms wall on a loaded
  machine, section 10.1), and the largest real walk is 1.2 ms wall, so the larger budget keeps ordinary scenes whole; a preempted walk that is cut only leaves its
  tail to RT64's own search. `ids` reports `budget_us` (effective) and `budget_clock=thread_cpu|wall`; `ftime stop` reports `ids_budget_clock`.
  Build with `-DTHPS_IDS_WALL_BUDGET` to run the wall-clock path on macOS / Linux (test hook).
* Nothing is tagged when `refresh` is `original`.

## 7. Things tried and dropped

* **An RT64 patch so IDs decide `matchScenes`** (and so the camera could be tagged). Dropped: not needed. Every real projection of a frame is already mapped
  (`V` lines of `idscalls`: `mapped=1` for the world, decal and shadow projections; the one unmapped entry is RT64's default identity entry 0, which is
  what the counters' "camera paired 86 %" was counting), and tagging the projection loads with IDs (one for perspective cameras, one for the singular
  matrix) changed nothing measurable. No RT64 change is part of this work (no `rt64-interp-ids.patch`, nothing in `setup_runtime.py`).
* **The skater's shadow.** It was suspected of being half a camera step off the skater in the in-between frames. It is not: the singular matrix is mapped, RT64
  interpolates the projection as a whole matrix (view and projection are lerped separately; with the failed decomposition the view is the identity and the
  whole matrix is the projection), and its 45 transforms are tagged like the skater's. Frames show the shadow following the feet.

## 8. Tools

Control commands (`--control`, one per line, reply `[ctl] ok ...`):

| command | what |
|---|---|
| `ids [on\|off\|tier2 on\|off\|step N\|reset]` | switch the mechanism or its second tier, change the gate, clear the tracks; the reply is the counters: tasks, hooks, tagged, matched, matched2 (tier 2), fresh_far, ambiguous, untracked, mismatches (scan vs RT64 walk), cuts |
| `ids slow` / `ids slowreset` | the histogram (wall and thread-CPU time, buckets <0.25 / 0.5 / 1 / 2 / 4 / 8 / 16 ms / more) and the ten slowest walks since the reset, each with its CPU time, scan share, transform / command / matrix counts, tracks, tier-2 candidates and whether a camera cut was seen; `ids budget US` sets the tracker's CPU budget (x3 on the wall clock); `ids naive on\|off` and `ids selftest N` are test aids (section 10.1) |
| `idsscan 0\|1` | walk every task (the two below turn it on) |
| `idsstat N [FILE]` | pairing statistics over N sampled matched frames: all / moving transforms paired, worst unpaired per frame, camera paired |
| `idsdump FILE [1]` | RT64's verdict per transform joined with the walk (matrix, translation, vertices, triangles, state hash, parent list, matrix hash) of the same task; `1` also dumps the task before |
| `idscalls FILE [1]` | the same plus RT64's call and candidate tables (scenes, calls with hash and triangle counts, transform candidates, per-projection `mapped`). Needs an RT64 built with `HFR_STATS_HAS_CALLS` (diagnostic patch kept in the clone used for this work, not part of the repo's patches) |
| `ftime start` / `ftime stop [FILE]` | (FILE: every timestamp as CSV `kind,seconds`) frame-time statistics between the two (presented / composited / game frames: fps, p50 / p95 / p99 / max interval, frames over 20 ms / 40 ms) and the cost of the ID walk + tracker and hook over the window |

Flags: `--interp-ids on|off`, `--ids-max-step N`. Test sessions (`tools/test.sh --session NAME`; THPS1/2: opt-in, THPS3: debug sessions):

* `idsprobe`: boots to a level, runs `idsstat` while standing, skating, turning and in an ollie (`tests/hfr_steps.ids_phases`) and `idsdump`s into `build/ids/`;
* `idsbench` / `idsbench-orig`: the performance runs of section 10 (`THPS_BENCH_ARGS="--always-present --refresh 60"` adds host flags; the second session runs `--refresh original`);
* `idsclip`: every composited frame of about 8.6 s of skating into `build/ids/clip` (`tests/hfr_steps.ids_clip`).

Environment for A/B runs: `THPS_IDS=0` (mechanism off), `THPS_IDS_TIER2=0` (tier 1 only), `THPS_IDS_CALLS=1` (idscalls instead of idsdump).
Make a side-by-side clip with ffmpeg (`hstack` of two `idsclip` outputs), and judge with a frame-vs-neighbours metric: for every presented frame the mean
absolute difference to the mean of its two neighbours, divided by the neighbours' difference; pops and wrong pairs are the frames above 1.
`hfr_motion` in each suite checks the paired share of all transforms (90 / 88 / 85 %, measured 98 / 97 / 97 %).

## 9. Known limits

* A piece is tagged from its first frame in view, but a piece whose parent list changes every frame and whose draw shape is shared by many neighbours
  (clusters of identical sprites) is ambiguous and stays with RT64's search, which can still pair wrongly (the cases are few: `ambiguous` counter).
* An object that teleports less than the gate in one frame is paired and interpolated across the jump for one in-between frame unless RT64's velocity check
  refuses it (it does for any jump over 5 units that is 10 times the last step).
* Transforms whose model-view matrix is built with push / multiply modes would need the walk to emulate the stack; the THPS games load every matrix.
* Not verified: Windows (the code is plain C++; `mingw_check.py` compiles it).
* The camera is still paired by RT64's scene search (`matchScenes`, matrix distance). Projection loads carry an ID per kind (camera /
  planar shadow, `interpolation.cpp` "projection kinds", 2026-10-15), so a camera paired with the previous shadow scene (the THPS2
  one-frame hole) is shown uninterpolated for that frame instead of half way to the shadow matrix; letting the IDs decide the scene
  pairing itself needs an RT64 patch (not made).
* RT64 splits the game's combined view * projection load into a view and a projection (`matrixDecomposeViewProj`) with the depth
  scale taken from the x row alone; with the camera looking (nearly) along the world z axis that row is ~0, so the depth range came
  out wrong or the split was NaN (the camera then counted as orthographic: no pairing, a 30 Hz step). The host re-splits every
  perspective projection load with the best-conditioned row (`interpolation.cpp` "view-projection split", 2026-10-15;
  `--hfr-vp-split 0` / `--control vpsplit off` for A/B). This applies at every refresh setting: RT64 also renders with the split.

## 10. Performance (does it cost frames?)

Method (`idsbench` sessions, THPS1/2: opt-in, THPS3: debug; `tests/hfr_steps` step `bench`): a **visible window** (no `--hidden`) with `--always-present --refresh 60`
(a window that is not on screen is not presented to at all and the compositor's frame pacing is then not real, see below), no capture, no dltrace; in a level,
skating forward and turning on a fixed wall-clock script for 20 s per block; blocks interleaved IDs off / on / off / on / off / on (`--control ids off|on`) in one run,
so the machine's load affects both sides; a `--refresh original` run of the same script as the baseline. Timestamps come from `--control ftime start|stop`
(`src/frametime.cpp`): a sampler thread polls the presented / composited / game-frame counters every 200 us, nothing sits in the frame path. The cost of
the display-list walk + tracker is timed inside the host with a steady clock (`ids_walk_us_*`, wall time, and `ids_cpu_us_*`, the thread's own CPU time from `CLOCK_THREAD_CPUTIME_ID`; 0 on Windows, where `ids_budget_clock=wall`), the `G_MTX` hook separately (`ids_hook_us_avg`).
Machine: M2 Pro, `uptime` load average 2.2-4.6 during the runs (another worker's builds; the game launches are serialised with `lockf`, so no other game ran).

Three blocks of 20 s each (60 s), IDs off vs on:

| game | IDs | presented fps | p50 / p95 / p99 frame time (ms) | presented frames over 20 ms (of ~3600) | game fps | walk + tracker per game frame (avg, worst) | hook per frame |
|---|---|---|---|---|---|---|---|
| THPS1 | off | 59.92 | 16.67 / 16.85 / 16.94 | 5 (worst 52.7 ms) | 29.99 | - | - |
| THPS1 | on | 59.97 | 16.67 / 16.85 / 16.90 | 2 (worst 39.2 ms) | 30.00 | 0.13 ms, 2.3 ms | 11 us |
| THPS2 | off | 60.00 | 16.67 / 16.85 / 16.89 | 2 | 30.01 | - | - |
| THPS2 | on | 59.88 | 16.67 / 16.86 / 16.91 | 3 (worst 117 ms, one frame) | 29.98 | 0.10 ms, 3.0 ms | 8 us |
| THPS3 | off | 59.98 | 16.67 / 16.85 / 16.90 | 1 | 29.98 | - | - |
| THPS3 | on | 59.99 | 16.67 / 16.85 / 16.89 | 0 | 30.02 | 0.10 ms, 0.7 ms | 9 us |

* Off and on are indistinguishable (60.0 fps, p99 16.9 ms = the 60 Hz frame; the handful of frames over 20 ms are one-off stalls that occur with IDs off as well).
* The IDs cost about 0.10-0.13 ms per game frame (walk + tracker) plus about 10 us in the hook, 0.3-0.4 % of the 33 ms game frame, against the budget of 0.5 ms; the worst
  single frames (0.7-3 ms, one in several hundred) coincide with load spikes. No optimisation was needed. What was done anyway: the walk is skipped entirely at
  `--refresh original` (nothing to pair; it had cost 36 us per frame there), and the walk's result is copied into the idsdump / idsstat ring only while one of those is in use.
* `--refresh original` baseline (THPS1 / THPS2, same script, taken before the window state below set in): 30.0 presented, 30.0 game fps, 33.3 / 35.9 / 36.9 ms (p50 / p95 / p99). THPS3: 30.0 game fps, 33.3 ms frame interval in the earlier run without `--always-present` (no presented-frame numbers: see the stall note). The game frame interval shows the same
  p95 of 36 ms in every mode (30 Hz game frames, sampler and scheduler jitter), so that spread is not the interpolation.
* **Debug modes.** The present dump (`--control pcap 1`: every composited frame encoded to PNG, as `hfr_motion`, `idsclip` and the HUD sessions do) is the one that
  chugs: presented 18-21 fps, game 11-13 fps, p99 about 1 s per stalled frame, and for the next 20 s after it stopped (the writers' backlog) 55-58 fps with a 1 s stall. The
  first version of `idsstat` and `idsdump` (`--control`, a walk and a copy per matched frame) changed nothing measurable (60.0 fps, p99 16.9 ms, game 30.0 fps). `dltrace` is one frame.
  So a visible window that "chugs" while tests run is a present-dump scenario (the suites use them for a few seconds each: `hfr_motion` waits for the writers
  to finish before it goes on, the `bench` step too); `bench` and the plain suites do not capture. A window that is not on screen (covered, other Space, display asleep) is not
  presented to by default, and `--always-present` on such a window blocks the host about 1 s per frame (CAMetalLayer): the `--refresh original` runs of THPS2 / THPS3 / THPS1 made
  at about 11:20 on the benchmark machine (repeated four times, the window then not on screen or the display asleep: not determined) gave 11-14 fps with 1 s stalls in all three games, IDs not involved (the walk is off at original refresh). The `--refresh 60` runs of THPS1/2/3 made before that (11:05-11:21) paced perfectly. A benchmark of presents is only meaningful while the window is really on screen.
* What the user may have seen as "chugging background" without any of this: with the IDs off, in about half of the frames of a THPS3 level the world steps at 30 Hz against a skater that moves at 60 Hz
  (section 2). That is what the IDs remove.

### 10.1 Slow walks: preemption, not cost (measured 2026-10-14)

A first benchmark on a busy machine (load 6-13) showed worst-case walks of 16-18 ms wall time and more frames over 20 ms with the IDs on in THPS1. The walk now
also records the thread's CPU time and keeps a histogram and the ten slowest walks with what the task looked like (`ids slow`). Under load 15-20 (a build running):

* walk wall time 3 / 8 / 10 / 16.8 ms, but the **same walks used 0.2-0.4 ms of CPU** (for example task 3791: wall 16 779 us, CPU 396 us, 445 transforms, 51 tier-2 candidates, no cut,
  7 projections; task 2596: wall 6 577 us, CPU 194 us, 186 transforms); CPU histogram of a 20 s block: 563 walks under 0.25 ms, 40 under 0.5 ms, none above;
* so the slow walks were the thread being descheduled (load average 15-20 on a 10-core machine), at random points in the scan and in the tracker, not work. In the benchmark the slowest ten
  were always ordinary level tasks (150-480 transforms, `cut=0`, `projs=7`).
* Level load, front end and camera cut (session `idsload`: a boot through the menus and loading screens to the level, then RETRY = a camera cut and a level restart; hidden window, load ~4): every
  walk before the level (menus, loading screens) is under 0.25 ms; the slowest walks of the whole run are level tasks of the largest scenes (THPS3 330-370 transforms: 0.42 ms CPU / 1.2 ms wall;
  THPS1 620-650 transforms: 0.55 ms CPU / 1.2 ms wall); the cut frame itself is cheap (the tracks are cleared, nothing is matched) and the frames after it are ordinary.

Real cost over all benchmark blocks of the three games (about 600 walks per block, 27 blocks): average 0.09-0.14 ms CPU, **largest single walk 0.38 ms CPU in the benchmark blocks, 0.55 ms in the largest scene of THPS1 at a level restart** (target < 1 ms), 0.1-0.9 ms wall
at load 3.4-6.4. The walk runs on the gfx thread (`send_dl`), the one that hands the display list to RT64 and completes the game's graphics task; it does not present (RT64's present
and workload threads do), so a late walk delays the next workload like any other preemption of that thread, it cannot stall a present that is already queued.

The algorithm did have a bad worst case that real scenes (up to 650 transforms) do not reach: the first version compared every moving piece with every candidate of its group and sorted all pairs.
`--control ids selftest N` times the tracker alone on synthetic tasks (N = 2000 transforms, thread CPU time, unbounded / bounded / bounded with the budget):

| synthetic task (2000 transforms) | first version | hashed + grid | + 0.6 ms budget |
|---|---|---|---|
| all moved, one list | 1.57 ms | 0.55 ms | 0.38 ms |
| one list, 7 fewer pieces (no ordinal shortcut) | 1.33 ms | 0.31 ms | 0.29 ms |
| 50 lists, scattered | 5.23 ms | 2.74 ms | 0.81 ms |
| static | 0.40 ms | 0.29 ms | 0.30 ms |

so the cost is now about linear and capped at about 0.8 ms of CPU for any task (500 transforms: 0.04-0.46 ms in all cases). On real scenes the first version and the bounded one are the same
(THPS1-3 `idsbench` with `naive` blocks: average 0.09-0.13 ms CPU either way).

Interleaved off / on (bounded) / on (first version), three blocks of 20 s each per mode, visible window, `--always-present --refresh 60`, load 3.4-9.7 (quiet half of it
3.4-5.4), game launches serialised with `lockf`:

<!-- table-ok: our own benchmark measurements, no game data -->
| game | mode | presented frames over 20 ms (of ~3600) | p99 (ms) | worst frame (ms) | walk avg wall / CPU (us) | walk worst wall / CPU (us) |
|---|---|---|---|---|---|---|
| THPS1 | off | 34 | 23.2 | 190 | - | - |
| THPS1 | on | 0 | 16.89 | 17.1 | 137 / 134 | 422 / 346 |
| THPS1 | first version | 13 | 18.1 | 137 | 130 / 124 | 4650 / 393 |
| THPS2 | off | 2 | 16.89 | 81 | - | - |
| THPS2 | on | 11 | 17.75 | 53 | 103 / 100 | 923 / 301 |
| THPS2 | first version | 18 | 21.7 | 104 | 93 / 91 | 565 / 271 |
| THPS3 | off | 2 | 16.89 | 25 | - | - |
| THPS3 | on | 0 | 16.89 | 17.9 | 117 / 113 | 722 / 380 |
| THPS3 | first version | 13 | 18.7 | 251 | 107 / 100 | 2354 / 259 |

The frames over 20 ms and the worst frames do not follow the mode: they appear in the IDs-off blocks as well (THPS1 off: 34, 190 ms), come in bursts when the machine is busy (the THPS2 "on" count is
one block at load 5.4; the same game's later blocks at load 3.4-4.4 had 0 in every mode), and one-off stalls of 80-250 ms occur in all modes. In the quiet blocks (load <= 4.5) every mode had 0-1 frames over 20 ms and
p99 16.9 ms. So there is no measurable effect of the IDs on presents; the CPU cost is 0.1 ms per game frame on average and under 0.4 ms at worst.
