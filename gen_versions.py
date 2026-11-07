"""Version tables for a THPS recomp host (docs: docs/internals/versions.md).

Shared by the hosts: each game's recomp-host/tools/gen_versions.py is a thin wrapper that passes its
Profile (the game's recomp-host directory; the profile itself is game/game.toml) to main(). The command line and the output are the game's.
`header` also writes the profile as `thps::game` constants (names, wording, flags) for the shared host sources (decomp/common/host).

Reads
  decomp/common/games.toml                 ROM versions (facts: hashes, header, geometry), `inherits` rows
  versions/classes.toml                    code classes (one recompiled payload each)
  versions/host_syms.toml                  the names the host needs, whatever the version
  versions/<class>/host_force.toml         function names forced per class (hook targets) + COP2 patches
  versions/<class>/data_syms.toml          data addresses and struct offsets, by name
  game/game.toml                           the game profile (names, wording, flags; documented in decomp/common/host/README.md)
  config/<game>.hooks.toml                 [[patches.hook]] blocks, by function name (shared)
Writes (all generated, no ROM data: addresses, sizes, names, hashes)
  check                         validate everything (a missing name in any class is an error)
  header OUT.h                  thps::SymTable per class + thps::versions[] (host C++)
  config CLASS OUTDIR           OUTDIR/<game>.host.toml, the N64Recomp config of one class
  namespace CLASS FUNCS_DIR OUT.h | rsp-config CLASS OUT.toml BLOB CPP   `#define name CLASS_name` for every function the class's
                                generated C defines, so several payloads link into one binary
  ride-audit CLASS FUNCS_DIR    check the generated ride writer scope (a class with ride_step; others pass)
  list                          one line per version (shell scripts)

usage: python3 -I tools/gen_versions.py <command> [args]
"""
import os
import re
import sys
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import thpsgame  # noqa: E402

TV = {"pal": 0, "ntsc": 1, "mpal": 2}
STATUS = ("supported", "recognised")
# Fields that decide what code the payload was generated from: every version of a class must agree.
CLASS_FIELDS = ("boot_rom", "boot_vram", "boot_end", "main_vram", "root_ptr", "main_size", "entry",
                "game_bss_end", "boot_text", "main_text", "boot_data_vram", "main_data_vram")


class Profile:
    """What differs between the hosts: the game's profile, <host>/game/game.toml (`host` is the game's recomp-host directory;
    keys are documented in decomp/common/host/README.md). A game with `overlays = true` feeds N64Recomp a code blob with the
    front-end overlays appended (build/gen/<class>/<family>.host.blob.bin, decomp/common/overlay_recomp.py via tools/gen.py);
    otherwise the decomp's build/<root game>.codeblob.bin."""

    def __init__(self, host):
        self.host = host
        self.gamedir = os.path.dirname(host)
        self.game = toml(os.path.join(host, "game", "game.toml"))
        self.family = self.game["family"]
        self.overlays = bool(self.game.get("overlays", False))

    def blob(self, cls, root):
        if self.overlays:
            return os.path.join(self.host, "build", "gen", cls, f"{self.family}.host.blob.bin")
        return os.path.join(self.gamedir, "build", f"{root.name}.codeblob.bin")

    def blob_note(self, root):
        if self.overlays:
            return f"{self.family}.host.blob.bin (build/{root.name}.codeblob.bin + the overlays, overlay_recomp.py)"
        return f"build/{root.name}.codeblob.bin (decomp/common/gen_syms.py --game {root.name})"


def toml(path):
    with open(path, "rb") as f:
        return tomllib.load(f)


def class_extensions(force):
    """Validate optional own class hooks and the paired configured-ride profile."""
    hooks = force.get("config", {}).get("hook", [])
    for hook in hooks:
        if set(hook) != {"func", "before_vram", "text"}:
            raise ValueError("class hooks require func, before_vram and text")
        if not isinstance(hook["func"], str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", hook["func"]):
            raise ValueError("class hook func must be a C identifier")
        address = hook["before_vram"]
        if type(address) is not int or not 0x80000000 <= address <= 0x807FFFFC or address % 4:
            raise ValueError("class hook before_vram must be an aligned RDRAM address")
        if not isinstance(hook["text"], str) or not hook["text"]:
            raise ValueError("class hook text must be nonempty")
    calls = {name: [h for h in hooks if name in h["text"]]
             for name in ("thps_ride_step_begin", "thps_ride_step_end")}
    step = force.get("ride_step")
    if step is None:
        if any(calls.values()):
            raise ValueError("ride writer hooks require the class ride_step profile")
        return {}, hooks
    if set(step) != {"override", "frame_time", "frame_ticks"}:
        raise ValueError("ride_step requires override, frame_time and frame_ticks only")
    for value in step.values():
        if type(value) is not int or not 0x80000000 <= value <= 0x807FFFFC or value % 4:
            raise ValueError("ride_step words must be aligned RDRAM addresses")
    if len(set(step.values())) != 3:
        raise ValueError("ride_step input and both outputs must be distinct words")
    for name, matching in calls.items():
        if len(matching) != 1 or matching[0]["text"] != f"{name}(rdram, ctx);":
            raise ValueError("ride_step requires exactly one unmodified begin and end hook")
    begin, end = calls["thps_ride_step_begin"][0], calls["thps_ride_step_end"][0]
    if begin["func"] != end["func"] or begin["before_vram"] >= end["before_vram"]:
        raise ValueError("ride_step hooks must be ordered in one function")
    return step, hooks


class Model:
    def __init__(self, prof):
        self.prof = prof
        host = prof.host
        self.versions = thpsgame.versions(prof.family)
        self.classes = toml(os.path.join(host, "versions", "classes.toml"))
        self.syms = toml(os.path.join(host, "versions", "host_syms.toml"))
        self.per_class = {}
        errors = []
        funcs, addr, field = self.syms["funcs"], self.syms["addr"], self.syms["field"]
        ovl_funcs = self.syms.get("overlay_funcs", [])
        allnames = funcs + addr + field + ovl_funcs
        dup = sorted({n for n in allnames if allnames.count(n) > 1})
        if dup:
            errors.append(f"host_syms.toml: names listed twice: {dup}")
        for n in allnames:
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", n):
                errors.append(f"host_syms.toml: {n!r} is not a C identifier")
        # A "recognised" version may name a class that is not built yet; a "supported" one may not.
        used = {v.code_class for v in self.versions if v.status == "supported"}
        for c in sorted(set(self.classes) | used):
            if c not in self.classes:
                errors.append(f"classes.toml has no class {c!r} (used by a supported version)")
                continue
            d = os.path.join(host, "versions", c)
            try:
                force = toml(os.path.join(d, "host_force.toml"))
                data = toml(os.path.join(d, "data_syms.toml"))
            except FileNotFoundError as e:
                errors.append(f"class {c}: missing {e.filename}")
                continue
            try:
                class_extensions(force)
            except (TypeError, ValueError) as e:
                errors.append(f"class {c}: {e}")
            by_name = {}
            for a, n in force["force"].items():
                if n in by_name:
                    errors.append(f"class {c}: host_force.toml forces {n!r} twice")
                by_name[n] = int(a, 16)
            # Functions inside the relocatable front-end overlays (THPS2/3): "overlay+0xOFFSET" = "name", named by
            # overlay_recomp.py --force. Hooks only: their run-time address is the overlay's load base + offset, so the
            # SymTable holds none (host_syms.toml `overlay_funcs`).
            ovl = {}
            for a, n in force.get("overlay_force", {}).items():
                if n in by_name or n in ovl:
                    errors.append(f"class {c}: host_force.toml forces {n!r} twice")
                if not re.fullmatch(r"[A-Za-z0-9_]+\+0x[0-9A-Fa-f]+", a):
                    errors.append(f"class {c}: [overlay_force] key {a!r} is not OVERLAY+0xOFFSET")
                ovl[n] = a
            for n in ovl_funcs:
                if n not in ovl:
                    errors.append(f"class {c}: overlay function {n!r} (host_syms.toml) is not in host_force.toml [overlay_force]")
            for n in ovl:
                if n not in ovl_funcs:
                    errors.append(f"class {c}: host_force.toml [overlay_force] has {n!r}, which host_syms.toml overlay_funcs does not list")
            for n in funcs:
                if n not in by_name:
                    errors.append(f"class {c}: function {n!r} (host_syms.toml) is not in host_force.toml")
            for n in by_name:
                if n not in funcs:
                    errors.append(f"class {c}: host_force.toml forces {n!r}, which host_syms.toml does not list")
            for kind, names in (("addr", addr), ("field", field)):
                tbl = data.get(kind, {})
                for n in names:
                    if n not in tbl:
                        errors.append(f"class {c}: {kind} {n!r} (host_syms.toml) is not in data_syms.toml")
                    else:
                        by_name[n] = tbl[n]
                for n in tbl:
                    if n not in names:
                        errors.append(f"class {c}: data_syms.toml [{kind}] has {n!r}, which host_syms.toml does not list")
            root = thpsgame.load(self.classes[c]["game"])
            for v in self.versions:
                if v.code_class != c:
                    continue
                for f in CLASS_FIELDS:
                    if getattr(v, f) != getattr(root, f):
                        errors.append(f"version {v.name} (class {c}) has {f} {getattr(v, f)!r}, "
                                      f"but the class root {root.name} has {getattr(root, f)!r}: that is a different code class")
            self.per_class[c] = dict(by_name=by_name, force=force, root=root, ovl=ovl)
        for v in self.versions:
            if v.status not in STATUS:
                errors.append(f"version {v.name}: status {v.status!r} not in {STATUS}")
            if v.tv_default not in TV:
                errors.append(f"version {v.name}: tv_default {v.tv_default!r} not in {sorted(TV)}")
            for k in ("rom_sha1", "rom_md5", "rom_xxh3", "game_id", "display_name", "header_name", "game_code", "revision", "rom_size"):
                if not hasattr(v, k):
                    errors.append(f"version {v.name}: missing {k}")
            refs = getattr(v, "refs_from", "")
            if refs and refs not in [x.name for x in self.versions]:
                errors.append(f"version {v.name}: refs_from {refs!r} is not a {prof.family} version")
        for key in ("game_id", "rom_sha1", "rom_xxh3", "rom_md5"):
            vals = [getattr(v, key, None) for v in self.versions]
            if len(set(vals)) != len(vals):
                errors.append(f"games.toml: two versions share {key}")
        self.errors = errors

    def fail_on_errors(self):
        if self.errors:
            sys.exit("gen_versions: " + str(len(self.errors)) + " problem(s):\n  " + "\n  ".join(self.errors)
                     + "\nFix: make the tables named above agree (games.toml, versions/<class>/, host_syms.toml); "
                     "docs/internals/versions.md describes each one.")


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


GAME_KEYS = ("family", "short_name", "app_name", "product", "title", "window_title", "config_dir", "mod_game_id", "about", "header_prefix",
             "revision_note", "menu_cursor_box_pct")


def emit_game(g):
    """namespace thps::game: the game profile (game/game.toml) as constants (names, flags)."""
    miss = [k for k in GAME_KEYS if k not in g]
    if miss:
        sys.exit(f"gen_versions: game/game.toml misses {miss}")
    box = g["menu_cursor_box_pct"]
    return ["namespace game {",
            "// The game profile (game/game.toml).",
            f"inline constexpr const char* family = {cstr(g['family'])};            // games.toml family; exe is <family>_recomp",
            f"inline constexpr const char* short_name = {cstr(g['short_name'])};",
            f"inline constexpr const char* rom_title_short = {cstr(g['title'])};",
            f"inline constexpr const char* window_title = {cstr(g['window_title'])};",
            f"inline constexpr const char* config_dir_name = {cstr(g['config_dir'])};",
            f"inline constexpr const char* mod_game_id = {cstr(g['mod_game_id'])};   // librecomp's --game name",
            f"inline constexpr const char* about_text = {cstr(g['about'])};",
            f"inline constexpr const char* header_prefix = {cstr(g['header_prefix'])};   // ROM header name starts with this: some version of this game",
            f"inline constexpr const char* revision_note = {cstr(g['revision_note'])};   // printf format of the header revision, or empty",
            f"inline constexpr int menu_cursor_box_pct[4] = {{{', '.join(str(int(x)) for x in box)}}};   // latency.cpp: x0, x1, y0, y1",
            f"inline constexpr bool has_overlays = {'true' if g.get('overlays') else 'false'};   // front-end overlays in the ROM tail (hooks.cpp)",
            f"inline constexpr bool b_is_flip_in_game = {'true' if g.get('b_is_flip_in_game') else 'false'};   // N64 B also flips while skating (input.cpp)",
            f"inline constexpr bool p2_menu_mirror = {'true' if g.get('p2_menu_mirror') else 'false'};   // port 2 carries port 1's input in menus while no second pad is attached: the game's PLAYER 2 CONTROLS screen reads only port 2 (input.cpp)",
            f"inline constexpr bool hires_ring_clear = {'true' if g.get('hires_ring_clear') else 'false'};   // clear the margin ring of 640x480 screens (dl_fixes.cpp)",
            f"inline constexpr bool lores_ring_clear = {'true' if g.get('lores_ring_clear') else 'false'};   // clear the margin ring of 320x240 3D frames (dl_fixes.cpp)",
            f"inline constexpr bool hud_ring_clear = {'true' if g.get('hud_ring_clear') else 'false'};   // with HUD placement on, clear the margin ring of 320x240 3D frames too (dl_fixes.cpp)",
            f"inline constexpr bool hud_placement = {'true' if g.get('hud_placement') else 'false'};   // the HUD Placement setting works in this game (hud_anchor.cpp)",
            f"inline constexpr bool hud_anchor_by_bounds = {'true' if g.get('hud_anchor_by_bounds') else 'false'};   // hud_anchor.cpp: a group's anchor comes from the whole group's bounds (pre-scanned), not its first rect: for strings drawn as glyph rects only",
            f"inline constexpr float hud_left_frac = {float(g.get('hud_left_frac', 1/3)):.6f}f;   // hud_anchor.cpp: a rect group centred left of this share of the 4:3 frame width anchors LEFT",
            f"inline constexpr float hud_edge_frac = {float(g.get('hud_edge_frac', 0)):.6f}f;   // > 0: only a group hugging an edge (within this share of the width) is anchored there, the rest stays centred (replaces the thirds rule)",
            f"inline constexpr float hud_right_frac = {float(g.get('hud_right_frac', 2/3)):.6f}f;   // ... right of this share anchors RIGHT",
            f"inline constexpr float hud_bottom_frac = {float(g.get('hud_bottom_frac', 0.78)):.6f}f;   // ... a group whose top is below this share of the 4:3 frame height stays centred (prompts, captions)",
            f"inline constexpr float hud_overlay_frac = {float(g.get('hud_overlay_frac', 0.85)):.6f}f;   // ... a rect at least this share of the frame width is a menu page: the rest of the task stays centred",
            f"inline constexpr float interp_ids_max_step = {float(g.get('interp_ids_max_step', 100.0))!r}f;   // interp_ids.cpp: world units a piece may move between two game frames and still be the same object",
            "}  // namespace game", ""]


def emit_header(m, out):
    funcs, addr, field = m.syms["funcs"], m.syms["addr"], m.syms["field"]
    names = funcs + addr + field
    L = ["// GENERATED by recomp-host/tools/gen_versions.py - do not edit. Facts only (addresses, sizes, names, hashes).",
         f"// Sources: decomp/common/games.toml, versions/. See decomp/{m.prof.family}/docs/versions.md.",
         "#pragma once", "#include <cstddef>", "#include <cstdint>", "", "namespace thps {", "",
         "enum class Status { Supported, Recognised };", ""] + emit_game(m.prof.game) + [
         "// Every name the host needs (versions/host_syms.toml), at one code class's addresses / offsets.",
         "struct SymTable {"]
    for n in funcs:
        L.append(f"    uint32_t {n};")
    L.append("    // data addresses")
    for n in addr:
        L.append(f"    uint32_t {n};")
    L.append("    // struct field offsets")
    for n in field:
        L.append(f"    uint32_t {n};")
    L += ["};", "", "// name -> member, for the control channel (`peek @gSkater`, `sym NAME`).", "struct NamedSym {", "    const char* name;", "    uint32_t SymTable::* member;", "};", "inline constexpr NamedSym sym_names[] = {"]
    for n in names:
        L.append(f"    {{ {cstr(n)}, &SymTable::{n} }},")
    L += ["};", ""]
    for c in sorted(m.per_class):
        bn = m.per_class[c]["by_name"]
        L.append(f"inline constexpr SymTable sym_{c} = {{")
        for n in names:
            L.append(f"    .{n} = " + (f"0x{bn[n]:08X}u," if n in funcs or n in addr else f"0x{bn[n]:X}u,"))
        L.append("};")
    L += ["", "struct CodeClass {", "    const char* name;", "    const SymTable* sym;",
          "    const char* source;   // the games.toml row whose ROM the payload is generated from (versions/classes.toml `game`)",
          "    uint32_t ride_step_override, ride_step_frame_time, ride_step_frame_ticks; // optional audited paired-writer profile; zero is unsupported", "};",
          "inline constexpr CodeClass code_classes[] = {"]
    for c in sorted(m.per_class):
        step, _ = class_extensions(m.per_class[c]["force"])
        L.append(f"    {{ {cstr(c)}, &sym_{c}, {cstr(m.classes[c]['game'])}, " +
                 f"0x{int(step.get('override', 0)):08X}u, 0x{int(step.get('frame_time', 0)):08X}u, 0x{int(step.get('frame_ticks', 0)):08X}u }},")
    L += ["};", "", "// One ROM version (games.toml row).", "struct VersionInfo {",
          "    const char* id;             // games.toml row",
          "    const char* game_id;        // recomp::GameEntry::game_id; the stored ROM is <game_id>.z64",
          "    const char* display_name;",
          "    const char* internal_name;  // ROM header name (20 bytes)",
          "    const char* title;",
          "    char game_code[5];",
          "    uint8_t revision;",
          "    const char* sha1;",
          "    const char* md5;",
          "    uint64_t xxh3;              // XXH3-64 of the big-endian ROM (what librecomp's select_rom checks)",
          "    uint32_t rom_size;",
          "    const char* code_class;",
          "    int tv_default;             // osTvType: 0 PAL, 1 NTSC, 2 MPAL",
          "    Status status;",
          "    const char* refs_from;      // test references fall through to this version (or \"\")",
          "    // Geometry of the code blob the class was built from (== the ROM's boot loader).",
          "    uint32_t boot_vram, main_vram, main_size, main_blob_rom, chunk_table, entry;",
          "};", "inline constexpr VersionInfo all_versions[] = {"]
    for v in m.versions:
        L.append("    { " + ", ".join([
            cstr(v.name), cstr(v.game_id), cstr(v.display_name), cstr(v.header_name), cstr(v.title),
            "{" + ", ".join(f"'{ch}'" for ch in v.game_code) + ", 0}",
            str(v.revision), cstr(v.rom_sha1), cstr(v.rom_md5), f"0x{int(v.rom_xxh3, 16):016X}ull", f"0x{v.rom_size:X}u",
            cstr(v.code_class), str(TV[v.tv_default]), "Status::" + v.status.capitalize(), cstr(getattr(v, "refs_from", "")),
            f"0x{v.boot_vram:08X}u", f"0x{v.main_vram:08X}u", f"0x{v.main_size:X}u", f"0x{v.root_ptr:X}u",
            f"0x{v.table:X}u", f"0x{v.entry:08X}u"]) + " },")
    L += ["};", "inline constexpr size_t num_versions = sizeof(all_versions) / sizeof(all_versions[0]);", "",
          "}  // namespace thps", ""]
    text = "\n".join(L)
    if os.path.exists(out) and open(out).read() == text:
        return   # unchanged: keep the timestamp (CMake refreshes this header at every configure in the stub and split modes)
    with open(out, "w") as f:
        f.write(text)


def rel(path, start):
    # Forward slashes: the result goes into a TOML basic string, where Windows' "\" starts an escape ("..\build" -> "\b").
    return os.path.relpath(path, start).replace(os.sep, "/")


def emit_config(m, cls, outdir):
    prof = m.prof
    g = prof.family
    pc = m.per_class[cls]
    root = pc["root"]
    cop2 = pc["force"].get("cop2", {})
    os.makedirs(outdir, exist_ok=True)
    blob = prof.blob(cls, root)
    L = [f"# GENERATED by recomp-host/tools/gen_versions.py (code class {cls}) - do not edit.",
         f"# Inputs: {prof.blob_note(root)}, the class's host symbol file",
         f"# (gen_host_syms.py), config/{g}.hooks.toml. Output (ROM-derived C, local only): RecompiledFuncs/.",
         "[input]",
         f"entrypoint = 0x{root.boot_vram:08X}",
         f'symbols_file_path = "{g}.host.syms.toml"',
         f'rom_file_path = "{rel(blob, outdir)}"',
         'output_func_path = "RecompiledFuncs"',
         f'recomp_include = "#include \\"recomp.h\\"\\n#include \\"thps_hooks.h\\""',
         "", "[patches]"]
    # [config] (optional): functions N64Recomp must skip, and instruction words it must replace, that a class needs
    # because ANOTHER ROM version shares the class (THPS1 us11 also builds from the European ROM at first run).
    extra = pc["force"].get("config", {})
    ignored = list(cop2.get("ignored", [])) + list(extra.get("ignored", []))
    if ignored:
        L.append("ignored = [" + ", ".join(cstr(n) for n in ignored) + "]")
    for ins in extra.get("instruction", []):
        L += ["", "[[patches.instruction]]", f"func = {cstr(ins['func'])}", f"vram = 0x{ins['vram']:08X}", f"value = 0x{ins['value']:08X}"]
    for func, vrams in cop2.get("patch", {}).items():
        for v in vrams:
            L += ["", "[[patches.instruction]]", f"func = {cstr(func)}", f"vram = 0x{v:08X}", "value = 0x00000000"]
    # Address-specific hooks belong to their audited class, never the shared family file.
    _, hooks = class_extensions(pc["force"])
    for hook in hooks:
        L += ["", "[[patches.hook]]", f"func = {cstr(hook['func'])}",
              f"before_vram = 0x{int(hook['before_vram']):08X}", f"text = {cstr(hook['text'])}"]
    L.append("")
    with open(os.path.join(prof.host, "config", f"{g}.hooks.toml")) as f:
        L.append(f.read())
    with open(os.path.join(outdir, f"{g}.host.toml"), "w") as f:
        f.write("\n".join(L))


def emit_rsp_config(m, cls, out, blob, cpp):
    """The RSP program is the same in every version, but its place in the code blob is not: a re-linked class
    (THPS1 us10, THPS2 eu) has n_aspMain elsewhere. Recompute text_offset from games.toml `ucode` and fill in the
    rom/output paths. Template: <host>/config/<family>.aspMain.toml (ported from the Windows session, 1ff5d1f..a0f6d69)."""
    root = m.per_class[cls]["root"]
    asp = [u for u in root.ucode if u["name"] == "n_aspMain"]
    if len(asp) != 1:
        sys.exit(f"gen_versions: games.toml row {root.name} has no n_aspMain in `ucode`")
    vram, size = asp[0]["vram"], asp[0]["size"]
    off = root.main_blob_off + vram - root.main_vram if vram >= root.main_vram else 0x1000 + vram - root.boot_vram
    tmpl = os.path.join(m.prof.host, "config", f"{m.prof.family}.aspMain.toml")
    with open(tmpl) as f:
        cfg = f.read()
    m_size = re.search(r"^text_size = (0x[0-9A-Fa-f]+)$", cfg, re.M)
    if not m_size or int(m_size.group(1), 16) != size:
        sys.exit(f"gen_versions: {tmpl} text_size differs from games.toml {root.name} n_aspMain size 0x{size:X}")
    for pat, val in ((r"^text_offset = .*$", f"text_offset = 0x{off:X}"),
                     (r'^rom_file_path = ".*"$', f'rom_file_path = "{blob}"'),
                     (r'^output_file_path = ".*"$', f'output_file_path = "{cpp}"')):
        cfg, n = re.subn(pat, lambda _: val, cfg, flags=re.M)
        if n != 1:
            sys.exit(f"gen_versions: {tmpl}: expected one line matching {pat}")
    with open(out, "w") as f:
        f.write(f"# GENERATED by gen_versions.py rsp-config {cls}: n_aspMain 0x{vram:08X} = blob offset 0x{off:X}"
                f" ({root.name}). Template: config/{m.prof.family}.aspMain.toml.\n" + cfg)


DEF = re.compile(r"^(?:RECOMP_FUNC\s+)?void\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*uint8_t\s*\*\s*rdram\s*,\s*recomp_context\s*\*\s*ctx\s*\)\s*\{", re.M)


def emit_namespace(cls, funcs_dir, out):
    names = set()
    for fn in sorted(os.listdir(funcs_dir)):
        if re.fullmatch(r"funcs_\d+\.c", fn):
            with open(os.path.join(funcs_dir, fn)) as f:
                names.update(DEF.findall(f.read()))
    if not names:
        sys.exit(f"gen_versions: no function definitions found in {funcs_dir}")
    with open(out, "w") as f:
        f.write(f"// GENERATED by recomp-host/tools/gen_versions.py: code class {cls} ({len(names)} functions).\n")
        f.write("// Force-included when compiling this class's generated C, so several classes can be linked\n")
        f.write("// into one executable. Names the runtime or the host provide (osXxx_recomp, thps_*) are not renamed.\n")
        for n in sorted(names):
            f.write(f"#define {n} {cls}_{n}\n")
    print(f"gen_versions: namespace {cls}: {len(names)} functions -> {out}")


RIDE_BEGIN, RIDE_END = "thps_ride_step_begin(rdram, ctx);", "thps_ride_step_end(rdram, ctx);"
LABEL = re.compile(r"^[ \t]*([A-Za-z_]\w*):[ \t]*$", re.M)
GOTO = re.compile(r"\bgoto\s+([A-Za-z_]\w*)\s*;")


def ride_scope(text, func, begin, end):
    """Problems with the ride writer scope in generated C (empty: sound). begin_hook holds the host's mutex until end_hook, so
    every path from the begin call must reach the end call, and nothing may enter between them or call out."""
    defs = list(DEF.finditer(text))
    starts = [d for d in defs if d.group(1) == func]
    if len(starts) != 1:
        return [f"{func} is defined {len(starts)} times"]
    after = [d.start() for d in defs if d.start() > starts[0].start()]
    body = text[starts[0].end():after[0] if after else len(text)]
    if text.count(RIDE_BEGIN) != 1 or text.count(RIDE_END) != 1 or body.count(RIDE_BEGIN) != 1 or body.count(RIDE_END) != 1:
        return [f"expected one begin and one end call, both in {func}"]
    b, e = body.index(RIDE_BEGIN), body.index(RIDE_END)
    if b > e:
        return ["the end call comes before the begin call"]
    problems = []
    def at(addr, start):   # the guest instruction annotation that must follow a call, with only blanks between
        m = re.compile(rf"\s*// 0x{addr:08X}:").match(body, start)
        return m is not None
    if not at(begin, b + len(RIDE_BEGIN)):
        problems.append(f"the begin call is not immediately before 0x{begin:08X}")
    if not at(end, e + len(RIDE_END)):
        problems.append(f"the end call is not immediately before 0x{end:08X}")
    for addr, call in ((begin, b), (end, e)):   # a branch to the hooked address must not skip the call
        label = re.search(rf"^[ \t]*L_{addr:08X}:[ \t]*$", body, re.M)
        if label and not (label.end() <= call and not body[label.end():call].strip()):
            problems.append(f"label L_{addr:08X} does not lead straight into its call")
    scope = body[b + len(RIDE_BEGIN):e]
    inside = set(LABEL.findall(scope))
    for target in GOTO.findall(scope):
        if target not in inside:
            problems.append(f"goto {target} leaves the scope")
    outside = body[:b] + body[e:]
    for target in GOTO.findall(outside):
        if target in inside:
            problems.append(f"goto {target} enters the scope without the begin call")
    if re.search(r"\breturn\b|\bswitch\b|LOOKUP_FUNC|\b\w+\(rdram, ctx\)", scope):
        problems.append("a return, switch or call inside the scope")
    addrs = sorted({int(a, 16) for a in re.findall(r"// 0x([0-9A-Fa-f]{8}):", scope)})
    if addrs != list(range(begin, end, 4)):
        problems.append(f"the scope is not exactly the instructions 0x{begin:08X}..0x{end - 4:08X}")
    return problems


def ride_audit(m, cls, funcs_dir):
    step, hooks = class_extensions(m.per_class[cls]["force"])
    if not step:
        print(f"gen_versions: ride-audit {cls}: no ride_step profile")
        return
    begin = next(h for h in hooks if h["text"] == RIDE_BEGIN)
    end = next(h for h in hooks if h["text"] == RIDE_END)
    text = ""
    for fn in sorted(os.listdir(funcs_dir)):
        if re.fullmatch(r"funcs_\d+\.c", fn):
            with open(os.path.join(funcs_dir, fn)) as f:
                text += f.read() + "\n"
    problems = ride_scope(text, begin["func"], begin["before_vram"], end["before_vram"])
    if problems:
        sys.exit(f"gen_versions: ride-audit {cls}: the generated ride writer scope in {begin['func']} is unsafe:\n  "
                 + "\n  ".join(problems))
    print(f"gen_versions: ride-audit {cls}: {begin['func']} 0x{begin['before_vram']:08X}..0x{end['before_vram']:08X} ok")


def main(argv, prof):
    if not argv:
        sys.exit(__doc__)
    m = Model(prof)
    cmd = argv[0]
    if cmd == "check":
        m.fail_on_errors()
        emit_game(prof.game)   # exits if the profile misses a key
        print(f"gen_versions: ok: {len(m.versions)} versions, classes {sorted(m.per_class)}, "
              f"{len(m.syms['funcs']) + len(m.syms['addr']) + len(m.syms['field'])} host names")
        return
    m.fail_on_errors()
    if cmd == "header":
        emit_header(m, argv[1])
    elif cmd == "config":
        emit_config(m, argv[1], argv[2])
    elif cmd == "namespace":
        emit_namespace(argv[1], argv[2], argv[3])
    elif cmd == "ride-audit":
        ride_audit(m, argv[1], argv[2])
    elif cmd == "rsp-config":
        emit_rsp_config(m, argv[1], argv[2], argv[3], argv[4])
    elif cmd == "list":
        for v in m.versions:
            print(v.name, v.code_class, v.status, v.game_id, sep="\t")
    elif cmd == "class-info":
        c = argv[1]
        print(m.classes[c]["game"], m.classes[c]["syms"])
    elif cmd == "classes":
        print(" ".join(sorted(m.per_class)))
    else:
        sys.exit(__doc__)
