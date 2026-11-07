"""Exercise actual ride harness branches using a synthetic controller protocol.

AST-loading only the two methods avoids profile imports or engine startup.
"""
import ast
import importlib.util
import os
from pathlib import Path
import re
import tempfile
import time
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
tree = ast.parse((HERE / "run.py").read_text())
session = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == "Session")
methods = [node for node in session.body if isinstance(node, ast.FunctionDef) and node.name in ("step", "_ride_completed")]
assert len(methods) == 2
module = ast.Module(body=[ast.ClassDef(name="ActualMethods", bases=[], keywords=[], body=methods, decorator_list=[])], type_ignores=[])
ast.fix_missing_locations(module)


class StepFail(Exception):
    pass


namespace = dict(StepFail=StepFail, Path=Path, re=re, time=time)
exec(compile(module, str(HERE / "run.py"), "exec"), namespace)
ActualMethods = namespace["ActualMethods"]
checks = 0


def check(value, message):
    global checks
    checks += 1
    if not value:
        raise AssertionError(message)


class Controller:
    def __init__(self, final, log):
        self.final = final
        self.log_path = log
        self.fenced = False
        self.commands = []

    def note(self, text):
        pass

    def wait_frames(self, count):
        pass

    def cmd(self, command):
        self.commands.append(command)
        if command == "ride stop":
            return dict(self.final, error="none", draining="1")
        if command == "ride":
            return self.final if self.fenced else dict(self.final, error="none", draining="1")
        if command == "ride hold 0":
            return {}
        raise AssertionError(command)


class Fixture(ActualMethods):
    def __init__(self, final, log):
        self.inst = Controller(final, log)
        self.notes = []
        self.slow = 1

    def _ride_capture_start(self):
        pass

    def _ride_present_end(self):
        self.inst.fenced = True

    def keep(self, name):
        pass


complete = dict(mode="off", reads="2", next="2", error="none", step="1/30",
                step_writers="3", step_restores="3", step_tick_writes="3", installed="0", draining="0", stop_pending="0")
with tempfile.TemporaryDirectory(prefix="thps-own-harness-") as tmp:
    log = Path(tmp) / "host.log"
    log.write_text("[thps] ride: physics state samples 0:2\n")
    fixture = Fixture(complete, log)
    fixture.step(("ride_record_end", "end", 2, {0: 2}, "present"))
    check(fixture.inst.commands == ["ride stop", "ride"], "record status re-read after fence")
    for changes in (dict(error="write"), dict(draining="1"), dict(installed="1"),
                    dict(step_restores="2"), dict(step_tick_writes="2"), dict(step_writers="0"), dict(stop_pending="1")):
        fixture = Fixture(dict(complete, **changes), log)
        try:
            fixture.step(("ride_record_end", "end", 2, {}, "present"))
        except StepFail:
            check(True, "late record error or incomplete restoration fails")
        else:
            check(False, "late record error or incomplete restoration fails")
    for changes in (dict(error="step-writer-value"), dict(next="1"), dict(step_restores="2")):
        fixture = Fixture(dict(complete, **changes), log)
        fixture.inst.fenced = True
        try:
            fixture.step(("ride_end", "end", 1, None, "present"))
        except StepFail:
            check(fixture.inst.commands[-1] == "ride hold 0", "failed replay releases hold")
        else:
            check(False, "replay error must fail even at numerical EOF")
    fixture = Fixture(dict(mode="off", reads="2", next="2", error="none"), log)
    fixture.inst.fenced = True
    fixture.step(("ride_end", "end", 1, None, "present"))
    check(any("2/2" in note for note in fixture.notes), "legacy completion needs no new status fields")

spec = importlib.util.spec_from_file_location("own_ride_sessions", HERE / "ride_sessions.py")
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)
nav = [("shot", "select"), ("wait", 1)]
own = str(Path("/own/test.ride").resolve())   # the fixture resolves RIDE_FILE: on Windows this is a drive path


def build(extra):
    with patch.dict(os.environ, dict(THPS_HOST="/own", RIDE_FILE="/own/test.ride", **extra), clear=True):
        return fixtures.ride_sessions(nav, [], "select", {})[0]["scenarios"][0][2]


check(("ctl", f"ride rec {own}") in build({}), "legacy recording construction unchanged")
check(("ctl", f"ride rec-step 1/30 {own}") in build(dict(RIDE_STEP="1/30")), "configured recording opt-in")
check(("ride_arm", own) in build(dict(RIDE_MODE="play")), "playback uses file metadata")
for extra in (dict(RIDE_STEP="1/60"), dict(RIDE_STEP="1/30", RIDE_MODE="play"),
              dict(RIDE_STEP="1/30", RIDE_CAPTURE="native")):
    try:
        build(extra)
    except ValueError:
        check(True, "unsupported fixture override fails before engine")
    else:
        check(False, "unsupported fixture override fails before engine")

print(f"{checks} checks, 0 failed; synthetic controller only")
