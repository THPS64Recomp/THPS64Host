"""ROM-free class isolation and invalid-profile tests for the actual generator."""
import copy
import importlib.util
from pathlib import Path
import tempfile
import tomllib
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ride_test_gen_versions", ROOT / "gen_versions.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

checks = 0


def check(value, message):
    global checks
    checks += 1
    if not value:
        raise AssertionError(message)


profile = {
    "ride_step": {"override": 0x80000020, "frame_time": 0x80000024, "frame_ticks": 0x80000028},
    "config": {"hook": [
        {"func": "owned_writer", "before_vram": 0x80000100, "text": "thps_ride_step_begin(rdram, ctx);"},
        {"func": "owned_writer", "before_vram": 0x80000110, "text": "thps_ride_step_end(rdram, ctx);"},
    ]},
}
check(gen.class_extensions({}) == ({}, []), "ordinary class stays unsupported")
check(gen.class_extensions(profile)[0] == profile["ride_step"], "complete paired profile validates")


def rejects(mutate, message):
    fixture = copy.deepcopy(profile)
    mutate(fixture)
    try:
        gen.class_extensions(fixture)
    except (ValueError, TypeError):
        check(True, message)
    else:
        check(False, message)


rejects(lambda p: p.pop("ride_step"), "hooks without profile refuse")
rejects(lambda p: p["config"]["hook"].pop(), "profile without end refuses")
rejects(lambda p: p["config"]["hook"].append(p["config"]["hook"][0]), "duplicate begin refuses")
rejects(lambda p: p["ride_step"].update(frame_time=0x80000020), "aliased input/output refuses")
rejects(lambda p: p["ride_step"].pop("frame_ticks"), "missing integer output refuses")
rejects(lambda p: p["ride_step"].update(frame_ticks=0x80000024), "aliased float/integer output refuses")
rejects(lambda p: p["ride_step"].update(frame_ticks=0x80000020), "aliased input/integer output refuses")
rejects(lambda p: p["ride_step"].update(override=0x80000021), "unaligned word refuses")
rejects(lambda p: p["ride_step"].update(override=0x80800000), "outside RDRAM refuses")
rejects(lambda p: p["ride_step"].update(extra=1), "unknown timing field refuses")
rejects(lambda p: p["config"]["hook"][1].update(func="another_writer"), "different functions refuse")
rejects(lambda p: p["config"]["hook"][1].update(before_vram=0x80000100), "reversed/equal pair refuses")
rejects(lambda p: p["config"]["hook"][0].update(text="thps_ride_step_begin(rdram, ctx); extra();"), "modified callback refuses")
rejects(lambda p: p["config"]["hook"][0].update(before_vram=0x80000101), "unaligned hook refuses")
rejects(lambda p: p["config"]["hook"][0].update(func="bad name"), "invalid function name refuses")
rejects(lambda p: p["config"]["hook"][0].update(extra=True), "unknown hook field refuses")

with tempfile.TemporaryDirectory(prefix="thps-own-profile-") as tmp:
    root = Path(tmp)
    (root / "config").mkdir()
    (root / "config/thps2.hooks.toml").write_text("# own empty family hooks\n")
    game = {key: "own-test" for key in gen.GAME_KEYS}
    game.update(family="thps2", menu_cursor_box_pct=[0, 0, 0, 0])
    prof = SimpleNamespace(host=str(root), family="thps2", game=game,
                           blob=lambda cls, version: str(root / "unused.bin"),
                           blob_note=lambda version: "own fixture: no ROM read")
    version = SimpleNamespace(boot_vram=0x80000400)
    model = SimpleNamespace(prof=prof, syms={"funcs": [], "addr": [], "field": []}, versions=[],
                            classes={"us": {"game": "own-us"}, "eu": {"game": "own-eu"}},
                            per_class={"us": {"force": profile, "root": version, "by_name": {}},
                                       "eu": {"force": {}, "root": version, "by_name": {}}})
    for cls in ("us", "eu"):
        out = root / cls
        gen.emit_config(model, cls, str(out))
        generated = tomllib.loads((out / "thps2.host.toml").read_text())
        hooks = generated["patches"].get("hook", [])
        check(hooks == (profile["config"]["hook"] if cls == "us" else []), f"{cls} emits only its own hooks")
    header = root / "versions.h"
    gen.emit_header(model, str(header))
    text = header.read_text()
    check('{ "us", &sym_us, "own-us", 0x80000020u, 0x80000024u, 0x80000028u }' in text, "supported metadata emitted")
    check('{ "eu", &sym_eu, "own-eu", 0x00000000u, 0x00000000u, 0x00000000u }' in text, "unsupported metadata stays zero")

# The generated writer scope (own synthetic C in N64Recomp's shape: annotations, labels, gotos).
scope = """RECOMP_FUNC void owned_writer(uint8_t* rdram, recomp_context* ctx) {
    // 0x800000FC: nop
    thps_ride_step_begin(rdram, ctx);
    // 0x80000100: beq         $a0, $zero, L_8000010C
    if (ctx->r4 == 0) {
        // 0x80000104: nop
            goto L_8000010C;
    }
    // 0x80000104: nop
    // 0x80000108: nop
L_8000010C:
    // 0x8000010C: nop
L_80000110:
    thps_ride_step_end(rdram, ctx);
    // 0x80000110: jr          $ra
    return;
}
RECOMP_FUNC void other(uint8_t* rdram, recomp_context* ctx) {
    // 0x80000200: nop
}
"""
END = "    thps_ride_step_end(rdram, ctx);\n"
check(gen.ride_scope(scope, "owned_writer", 0x80000100, 0x80000110) == [], "paired scope passes")


def unsafe(text, message):
    check(gen.ride_scope(text, "owned_writer", 0x80000100, 0x80000110) != [], message)


unsafe(scope.replace(END, ""), "missing end refuses")
unsafe(scope.replace("L_80000110:\n" + END, END + "L_80000110:\n"), "end skipped by its own label refuses")
unsafe(scope.replace(END, "").replace("L_8000010C:\n", "L_8000010C:\n" + END), "end before the last instruction refuses")
unsafe(scope.replace("    // 0x80000108: nop\n", "    // 0x80000108: nop\n    func_80000200(rdram, ctx);\n"), "call inside refuses")
unsafe(scope.replace("    // 0x80000108: nop\n", "    // 0x80000108: nop\n    return;\n"), "return inside refuses")
unsafe(scope.replace("goto L_8000010C;", "goto L_80000200;"), "goto out of the scope refuses")
unsafe(scope.replace("    // 0x800000FC: nop\n", "    // 0x800000FC: nop\n    goto L_8000010C;\n"), "goto into the scope refuses")
unsafe(scope + scope.replace("owned_writer", "copy"), "a second hook pair refuses")

print(f"{checks} checks, 0 failed; own profiles only")
