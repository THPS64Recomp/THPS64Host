"""Input-only canonical ride fixtures. Captures and recorded inputs stay in the supplied local path.

RIDE_FILE=/absolute/path.ride RIDE_MODE=record|play selects the opt-in ride-canonical session.
RIDE_STEP=1/30 opts a new recording into v3; playback obtains timing from its file.
Record a moving route with ollies, flips and grind attempts; replay consumes every read before shooting.
A human-recorded ride can use the same playback step. Public fixture selection remains the maintainer's choice.
"""
import os
from pathlib import Path


def ride_sessions(to_run, inputs, start_after, required_states):
    path = str(Path(os.environ.get("RIDE_FILE", os.path.join(os.environ["THPS_HOST"], "build", "canonical.ride"))).resolve())
    play = os.environ.get("RIDE_MODE", "record") == "play"
    capture = os.environ.get("RIDE_CAPTURE", "present")
    if capture not in ("present", "native"):
        raise ValueError("RIDE_CAPTURE must be present or native")
    step = os.environ.get("RIDE_STEP")
    if step is not None and (step != "1/30" or play or capture != "present"):
        raise ValueError("RIDE_STEP=1/30 is for new recordings with present capture only; playback uses file metadata")
    split = to_run.index(("shot", start_after)) + 1
    # Boot may load an attract course. Arm only after selecting the intended course.
    arm = ("ride_arm", path) if play else ("ctl", f"ride rec-step {step} {path}" if step else f"ride rec {path}")
    nav = [*to_run[:split], *([("ctl", "ride hold 1")] if capture == "present" else []), arm, *to_run[split:]]
    return [dict(name="ride-canonical", config="ride-config", fresh_pak=True, wav=False, optin=True,
                 fresh_files=["graphics.cfg", "controls.cfg", "general.cfg", "cheats.cfg"],
                 args=["--skip-intro", "--hidden", "--unlock-levels", "--refresh", "60", "--draw-distance", "original"], scenarios=[
        ("ride_canonical", "canonical moving ride: complete input stream, tricks and grind attempts, paused end frame", [
            *nav,
            *([("ride_end", "ride_end", 240, os.environ.get("RIDE_REFERENCE"), capture)] if play else [
                ("vpad", "attach"), ("vpress", "a"), ("wait", 20),
                ("log", r"Virtual Controller takes port 1"),
                ("skater_watch", 60, inputs, ["Skater_PhysState", "Skater_RailBalance", "@gTimeScale"]),
                ("ctl", "vpad axis lefty 0"), ("ctl", "vpad axis leftx 0"),
                ("press", "START"), ("wait", 60), ("ride_record_end", "ride_end", 800, required_states, capture),
            ]),
        ]),
    ])]
