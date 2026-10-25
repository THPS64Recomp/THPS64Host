"""Level-start freeze (v0.2 queue item 3): frame timestamps (`ftime`, src/frametime.cpp) over every course start of a course cycle,
so the longest presented / composited / game-frame interval of each load can be measured and checked.

load_freeze_session(name, to_select, courses, phases, end_run, ...): to_select, courses and end_run as in course_cycle_session
(tests/course_cycle.py). `phases` = the course start as (BUTTON, CHECKPOINT or [STEPS]) pairs, e.g. [("A", "career_goals"), ("A", "in_level")]:
each is timed on its own: `ftime start`, the press, WAIT game frames with no screenshot, then `ftime_max` (run.py), which writes
the timestamps (CSV `kind,seconds` plus the `span` rows of src/frametime.cpp) to $THPS_LF_DATA/<name>/<tag>_<course>_<phase>.csv
and checks the limits, and only then the checkpoint is waited for (or the steps run).
Why no checkpoint inside the window: a checkpoint polls screenshots, and each one is encoded on the gfx thread (update_screen,
~90 ms); that stall is the harness's, not the game's (first probe, 2026-10-18).
THPS_LF_SAMPLE=SECONDS adds a macOS `sample` of the host over each phase (its attach stalls the process too: read with care).
THPS_LF_TAG names the run (default "run"); THPS_LF_ARGS adds host flags.
"""
import os

from course_cycle import PLAY   # a few seconds of skating: the pause checkpoint's references were taken after it

LIMITS = {"composited": 100.0}
WAIT = 150   # game frames after the press (~5 s at 30 fps; the frame counter stops while the game thread loads)


def load_freeze_session(name, to_select, courses, phases, end_run, *, claim=(), args=(), config=None, limits=None, hidden=True, optin=False,
                        settle=30, median=None):
    """median = (STREAM, MS): instead of a per-window limit, the median over the course starts of the last phase's longest STREAM
    interval must stay under MS (ftime_median in run.py)."""
    host = os.environ.get("THPS_HOST", ".")
    data = os.path.join(os.environ.get("THPS_LF_DATA", os.path.join(host, "build", "tests", "lf")), name)
    tag = os.environ.get("THPS_LF_TAG", "run")
    sample = float(os.environ.get("THPS_LF_SAMPLE", "0") or 0)
    steps = [*to_select]
    for i, move in enumerate(courses):
        steps += [*move, ("wait", 20)]
        for k, (button, cp) in enumerate(phases):
            base = os.path.join(data, f"{tag}_{i}_{k}")
            probe = [("sample", sample, base + ".sample.txt")] if sample > 0 else []
            keep = ("start",) if median and k == len(phases) - 1 else ()
            steps += [*probe, *([("wait", 45)] if probe else []), ("ctl", "ftime start"), ("press", button), ("wait", WAIT),   # (sample attaches first)
                      ("ftime_max", base + ".csv", limits or LIMITS, *keep), *([("sample_wait",)] if probe else []),
                      *([("wait_for", cp)] if isinstance(cp, str) else cp)]
        steps += [*([("vpad", "attach"), *claim] if i == 0 else []), ("wait", settle), *PLAY, ("shot", f"played_{i}")]   # settle: START is ignored during the intro camera
        if i < len(courses) - 1:
            steps += [*end_run, ("shot", f"ended_{i}")]
    if median:
        steps.append(("ftime_median", "start", *median))
    extra = os.environ.get("THPS_LF_ARGS", "").split()
    return dict(name=name, config=config or f"{name}-config", fresh_pak=True, wav=False, optin=optin,
                args=["--skip-intro", *(["--hidden"] if hidden else []), "--unlock-levels", *args, *extra], scenarios=[
        (name.replace("-", "_"), f"{len(courses)} course starts: frame timestamps over each load phase; "
                                  + (f"the median of the starts' longest {median[0]} interval stays under {median[1]} ms" if median else
                                     f"the longest interval stays under {limits or LIMITS} ms"), steps),
    ])
