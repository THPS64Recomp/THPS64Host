"""Course cycle (src/music.cpp): several courses back to back in one host, each ended from the pause menu (END RUN) and back to the
course / level select. THPS1-3 share the music player: every course start copies the course's song over the menu song while the menu
song's channels, only marked to stop, still read that buffer. Without the Music_LoadSong hook those channels ran one more audio frame
of events from the new song's bytes (a wild wave pointer or command: the THPS2 crash after a few courses, 2026-10-17). The session
checks that the hook stopped them at every course start and that the last course still plays music.

course_cycle_session(name, to_select, courses, start, end_run, ...):
  to_select  steps from boot to the select screen (cursor on the first course)
  courses    one step list per course: moves the cursor from the previous course to this one ([] for the first)
  start      select screen -> the course is played (ends when the run is live)
  end_run    in the course -> pause -> END RUN -> back on the select screen
"""

# ~6 s of skating: forward, the stick swept left / right, an ollie every second (the virtual pad must be attached first).
PLAY = [("ctl", "vpad axis lefty -32767"),
        *[x for k in range(6) for x in (("ctl", f"vpad axis leftx {(-32767, 0, 32767, 0)[k % 4]}"),
                                        ("ctl", "vpad button a 1"), ("wait", 8), ("ctl", "vpad button a 0"), ("wait", 22))],
        ("ctl", "vpad axis lefty 0"), ("ctl", "vpad axis leftx 0")]

GUARD_LINE = r"music: song \d+ loads over \d+ channel\(s\) still reading the buffer .*stopped without further events"
LOAD_LINE = r"music: song \d+ loads"   # any song load (THPS1 stops its old song before the load: nothing to guard)


def course_cycle_session(name, to_select, courses, start, end_run, *, claim=(), args=(), config=None, expect=GUARD_LINE):
    """claim: steps after the first course has started that give the virtual pad port 1 (THPS3: a press).
    expect: the host line counted at every course start (by default: the guard stopped the old song's channels)."""
    steps = [*to_select]
    for i, move in enumerate(courses):
        steps += [*move, ("shot", f"select_{i}"), *start, *([("vpad", "attach"), *claim] if i == 0 else []),
                  ("wait", 30), *PLAY, ("shot", f"played_{i}"),
                  ("log_n", expect, i + 1)]
        if i < len(courses) - 1:
            steps += [*end_run, ("shot", f"ended_{i}")]
    return dict(name=name, config=config or f"{name}-config", fresh_pak=True, wav=True,
                args=["--skip-intro", "--hidden", "--unlock-levels", *args], scenarios=[
        (name.replace("-", "_"), f"{len(courses)} courses back to back (pause -> END RUN -> select): the old song's channels are stopped "
                                  "before each course's song is loaded over them; no crash, the last course has music", [
            *steps,
            ("audio", 150, -40.0),
        ]),
    ])
