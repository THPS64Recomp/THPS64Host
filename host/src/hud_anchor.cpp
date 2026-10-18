// HUD placement for the THPS recomp hosts (graphics setting `hud`: original | 16:9 | expand).
//
// RT64 (aspect ratio 16:9 / 21:9 / expand) draws a wider 3D view, but the game's 2D layer (the HUD) is 4:3 art: it stays in a 4:3
// pillar in the middle. Zelda64Recomp's "HUD Placement" is RT64's extended-GBI alignment: its game code brackets HUD draws with
// gEXSetRectAlign / gEXSetScissorAlign (origin LEFT / CENTER / RIGHT) and the setting only changes RT64's extAspectRatio
// (Original = 0 %, 16:9 clamp = a share, Expand = 100 % of the extra width). THPS has no such commands in its display lists and we do
// not patch game code, so this file does what those commands would do from the host, the way interpolation.cpp does for matrix groups:
// RT64's Interpreter::commandHook sees every command (with its RDRAM address) just before RT64 runs it, and calls the RDP functions
// the extended commands call (RDP::setRectAlign, the scissor origin stack).
//
// THPS1 draws all its 2D as G_TEXRECT / G_FILLRECT. Each text string is one rect with the string's bounds followed by its glyph
// rects (shadow layers included), all inside those bounds. So the host groups rects (a rect inside or touching the previous group's
// bounds joins it) and anchors a group by its first rect's centre in the 4:3 frame: left third -> LEFT, right third -> RIGHT, the rest
// stays CENTER (= no extended alignment, as today); so does everything in the bottom fifth of the frame (prompts, captions: the opening frame
// of the pause menu draws its "B GO BACK / A SELECT" before any dim rect exists). A group is anchored as a whole, so a string never splits.
// Where HUD placement applies: only in a gfx task that has a 3D camera (a projection matrix was loaded) and a 320-wide colour image
// (the in-level frame; the 640x480 front end and the loading screens have neither), and only until a rect covers most of the frame's
// width (the pause menu / goals / results overlays start with a full-frame dim rect: those stay a centred 4:3 page).
// G_FILLRECT: a fill that spans the frame (the clears, the letterbox bars) keeps the regular placement; a narrower fill inside the HUD joins the group like a
// texrect. THPS1's SPECIAL meter is a texrect frame plus a FILLRECT for the bar's fill: left in the 4:3 pillar while its frame sat at the left edge, the fill started
// part-way along the frame and ran past its end (play test 2026-10-14). `--control hud` counts them (`fills left|centre|right`); the test asserts centre=0.
// `expand` is the default (host.h kDefaultHud); "original" emits nothing: the picture is exactly what it was before this file existed, and so is any
// 4:3 (--aspect original) run, whatever the setting says.
//
// THPS2 / THPS3 (same structure, rules per game in game/game.toml hud_*): their text is glyph rects with no bounds rect first, so a group is anchored
// by the bounds of the whole group, found by the pre-scan (hud_anchor_by_bounds), and only a group hugging an edge is anchored there (hud_edge_frac):
// the score / meter / tips go LEFT, the SWITCH badge and the sliding letters RIGHT; a bail's scattering letters, trick lines and the timer stay put.
//
// No game data: coordinates read from the display list at run time; the thresholds are per-game constants (thps::game::hud_*)
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "hle/rt64_rdp.h"
#include "hle/rt64_state.h"

#include "hfr.h"
#include "host.h"

namespace {

enum Anchor : int { kCentre = 0, kLeft = 1, kRight = 2 };

std::atomic<bool> g_enabled{false};
std::atomic<int> g_debug{0};   // --hud-debug N: log the first N groups
std::atomic<int> g_rects{0};   // --hud-rects N: log every rect (and the group it joined) of the first N tasks that have a HUD
std::atomic<int> g_force{-1};  // --hud-force left|right|centre: every group gets this anchor (test hook: THPS1's HUD has nothing at the right)

// Rect grouping, shared by the run-time classification and the pre-scan: a rect that overlaps the group's rows and lies inside (or within 6 px
// of) its columns joins the group; the group's bounds grow with it.
struct Grouper {
    bool group = false;
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    bool feed(float rx0, float ry0, float rx1, float ry1) {   // true: this rect starts a new group
        const bool joins = group && ry1 >= y0 - 3 && ry0 <= y1 + 3 && rx1 >= x0 - 6 && rx0 <= x1 + 6;
        if (joins) {
            if (rx0 < x0) x0 = rx0;
            if (rx1 > x1) x1 = rx1;
            if (ry0 < y0) y0 = ry0;
            if (ry1 > y1) y1 = ry1;
            return false;
        }
        group = true;
        x0 = rx0, x1 = rx1, y0 = ry0, y1 = ry1;
        return true;
    }
};

// The anchor of a group spanning x0..x1, top y (4:3 frame of width W): per-game rules (game.toml hud_*).
// hud_edge_frac = 0: by the group's centre (left / right third of the frame, hud_left_frac / hud_right_frac).
// hud_edge_frac > 0: only a group that hugs an edge is anchored there (its left side within that share of the width of the left edge, or its right side
// within that share of the right one, not both); everything else stays centred. Text drawn glyph by glyph (a bail's letters fly apart, each its own
// group) must not jump to an edge because one letter crossed a third of the frame.
Anchor classify(float x0, float x1, float top, float W) {
    if (top >= thps::game::hud_bottom_frac * (W * 0.75f)) return kCentre;   // the bottom band (menu prompts like "B GO BACK / A SELECT", captions) stays put
    if (thps::game::hud_edge_frac > 0.0f) {
        const bool l = x0 < W * thps::game::hud_edge_frac, r = x1 > W * (1.0f - thps::game::hud_edge_frac);
        return l && !r ? kLeft : r && !l ? kRight : kCentre;
    }
    const float cx = (x0 + x1) * 0.5f;
    return cx < W * thps::game::hud_left_frac ? kLeft : cx > W * thps::game::hud_right_frac ? kRight : kCentre;
}

// A group the pre-scan found: its first rect's command words (the key the run-time pass recognises it by) and its anchor.
struct PlannedGroup {
    uint32_t w0, w1;
    Anchor anchor;
    float x0, x1;   // the group's bounds (debug output)
};

// One gfx task.
struct TaskState {
    bool in3d = false;       // a projection matrix was loaded: the in-level frame
    bool overlay = false;    // a (nearly) full-width rect was drawn: a menu page, stays centred
    Anchor cur = kCentre;    // the alignment the RDP has now
    Grouper grp;
    Anchor ganchor = kCentre;
    std::vector<PlannedGroup> plan;   // hud_anchor_by_bounds: the anchors the pre-scan chose, in drawing order
    size_t plan_next = 0;
};
TaskState g_t;
std::atomic<uint64_t> g_tasks_anchored{0}, g_groups[3] = {{0}, {0}, {0}}, g_overlay_tasks{0};
std::atomic<uint64_t> g_last_task_groups[3] = {{0}, {0}, {0}};
std::atomic<uint64_t> g_fills[3] = {{0}, {0}, {0}};   // G_FILLRECTs that went through the grouping (a meter's fill), by anchor: the test asserts the fill follows its frame
uint64_t g_task_groups[3] = {0, 0, 0};

void apply(RT64::State* st, Anchor a) {
    if (a == g_t.cur) return;
    g_t.cur = a;
    RT64::RDP* rdp = st->rdp.get();
    RT64::ExtendedAlignment al;   // NONE / NONE: the regular (4:3, centred) placement
    RT64::ExtendedAlignment sc;
    if (a == kLeft) {
        al.leftOrigin = al.rightOrigin = G_EX_ORIGIN_LEFT;
    } else if (a == kRight) {
        al.leftOrigin = al.rightOrigin = G_EX_ORIGIN_RIGHT;
        al.leftOffset = al.rightOffset = -int32_t(rdp->colorImage.width) * 4;   // RIGHT is the right edge: undo the shift for 4:3 coordinates
    }
    if (a != kCentre) {   // the scissor spans the whole (extended) frame: left edge .. right edge, numbers unchanged
        sc.leftOrigin = G_EX_ORIGIN_LEFT;
        sc.rightOrigin = G_EX_ORIGIN_RIGHT;
        sc.rightOffset = -int32_t(rdp->colorImage.width) * 4;
    }
    rdp->setRectAlign(al);
    rdp->setScissorAlign(sc);   // what the game's next G_SETSCISSOR does (it re-sets the scissor in the middle of the HUD) ...
    const int top = rdp->scissorStackSize - 1;   // ... and the scissor that is set now
    if (top >= 0) {
        rdp->extended.scissorLeftOriginStack[top] = sc.leftOrigin;
        rdp->extended.scissorRightOriginStack[top] = sc.rightOrigin;
        st->updateDrawStatusAttribute(RT64::DrawAttribute::Scissor);
    }
}

// Pre-scan of a gfx task: walks the display list the way dl_fixes.cpp does (segment table, G_DL calls) before RT64 starts and returns whether
// it draws a (nearly) full-width rect anywhere. A pause menu's first frame draws its text before its dim rect shows up in the same list (and
// the opening frame of the pause menu has none), so the decision has to be made for the whole task: the menu's buttons must not jump to the
// edges for a frame. With hud_anchor_by_bounds the same walk also groups the HUD rects (the run-time conditions: a projection matrix loaded,
// a 320-wide colour image) and plans each group's anchor from its final bounds, so a string drawn as glyph rects only (no bounds rect first)
// is anchored by where the whole string lies, not by its first glyph.
bool plan_task(const uint8_t* rdram, uint32_t data_ptr, std::vector<PlannedGroup>* plan) {
    if (rdram == nullptr) return false;
    auto word = [&](uint32_t a) { return *reinterpret_cast<const uint32_t*>(rdram + (a & 0x7FFFFC)); };
    uint32_t seg[16] = {0};
    auto resolve = [&](uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0xFFFFFF; };
    uint32_t stack[20];
    int sp = 0;
    uint32_t pc = data_ptr & 0xFFFFFF, width = 320;
    bool in3d = false;
    Grouper grp;
    struct Raw { uint32_t w0, w1; float x0, x1, y0, y1; };
    std::vector<Raw> groups;   // first rect and (growing) bounds of each group
    auto finish = [&](bool overlay) {
        if (plan == nullptr || overlay) return;
        for (const Raw& g : groups) plan->push_back({g.w0, g.w1, classify(g.x0, g.x1, g.y0, 320.0f), g.x0, g.x1});
    };
    for (int cmds = 0; cmds < 200000; cmds++) {
        if (pc >= 0x800000) break;
        const uint32_t w0 = word(pc), w1 = word(pc + 4);
        pc += 8;
        bool done = false;
        switch (w0 >> 24) {
            case 0xFF: width = (w0 & 0xFFF) + 1; break;   // G_SETCIMG: width - 1
            case 0xDB:   // G_MOVEWORD: segment table
                if (((w0 >> 16) & 0xFF) == 6) seg[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0xFFFFFF;
                break;
            case 0xDE:   // G_DL (push unless it is a branch)
                if (((w0 >> 16) & 0xFF) == 0) {
                    if (sp >= 18) { done = true; break; }
                    stack[sp++] = pc;
                }
                pc = resolve(w1);
                break;
            case 0xDF:   // G_ENDDL
                if (sp == 0) { done = true; break; }
                pc = stack[--sp];
                break;
            case 0xDA:   // G_MTX: projection load
                if (((w0 & 0xFF) ^ 0x01) & 0x04) in3d = true;
                break;
            case 0xF6:     // G_FILLRECT: the same grouping as a texrect, but a wide fill (a clear) is no overlay and no group
            case 0xE4: {   // G_TEXRECT
                const float x0 = float((w1 >> 12) & 0xFFF) / 4.0f, y0 = float(w1 & 0xFFF) / 4.0f;
                const float x1 = float((w0 >> 12) & 0xFFF) / 4.0f, y1 = float(w0 & 0xFFF) / 4.0f;
                if (width <= 320 && x1 - x0 >= thps::game::hud_overlay_frac * float(width)) {
                    if ((w0 >> 24) == 0xE4) return true;
                    break;
                }
                if (plan != nullptr && in3d && width == 320) {
                    if (grp.feed(x0, y0, x1, y1)) groups.push_back({w0, w1, x0, x1, y0, y1});
                    else groups.back() = {groups.back().w0, groups.back().w1, grp.x0, grp.x1, grp.y0, grp.y1};
                }
                break;
            }
            default: break;
        }
        if (done) break;
    }
    finish(false);
    return false;
}

}  // namespace

namespace thps {

bool hud_supported() { return game::hud_placement; }

bool hud_active() { return g_enabled.load(); }

void hud_set_mode(bool on) { g_enabled = on && hud_supported(); }

void hud_task_begin(RT64::State* st, const uint8_t* rdram, uint32_t dl_address) {
    g_t = TaskState{};
    for (uint64_t& n : g_task_groups) n = 0;
    if (!g_enabled.load()) return;
    g_t.overlay = plan_task(rdram, dl_address, game::hud_anchor_by_bounds ? &g_t.plan : nullptr);   // a menu page: the whole task stays a centred 4:3 page
    st->rdp->setRectAlign(RT64::ExtendedAlignment());
    st->rdp->setScissorAlign(RT64::ExtendedAlignment());
}

void hud_task_end(RT64::State* st) {
    if (!g_enabled.load() && g_t.cur == kCentre) return;
    st->rdp->setRectAlign(RT64::ExtendedAlignment());
    st->rdp->setScissorAlign(RT64::ExtendedAlignment());
    g_t.cur = kCentre;
    if (g_task_groups[kLeft] + g_task_groups[kRight] > 0) {
        g_tasks_anchored++;
        for (int i = 0; i < 3; i++) g_last_task_groups[i] = g_task_groups[i];
    }
    if (g_t.overlay) {
        g_overlay_tasks++;
        if (g_debug.load() > 0) {
            std::printf("[thps] hud task %" PRIu64 " overlay\n", stats.gfx_tasks.load());
            std::fflush(stdout);
        }
    }
}

void hud_command(RT64::State* st, uint32_t w0, uint32_t w1) {
    if (!g_enabled.load()) return;
    const uint32_t op = w0 >> 24;
    if (op == 0xDA) {   // G_MTX: F3DEX2 stores params ^ G_MTX_PUSH; projection load = 0x06
        if (((w0 & 0xFF) ^ 0x01) & 0x04) g_t.in3d = true;
        return;
    }
    // G_TEXRECT and G_FILLRECT. A fill that spans the frame (the clears, the letterbox bars) keeps the regular placement; a narrower fill inside the
    // HUD is part of an element and joins the group like a texrect: THPS1's SPECIAL meter is a texrect frame plus a FILLRECT for the bar's fill, and a
    // fill left in the 4:3 pillar while its frame sits at the left edge starts part-way along the frame and runs past its end (2026-10-14 play test).
    if (op != 0xE4 && op != 0xF6) return;
    const float W = float(st->rdp->colorImage.width);
    if (!g_t.in3d || g_t.overlay || W != 320.0f) {
        apply(st, kCentre);
        return;
    }
    const float x0 = float((w1 >> 12) & 0xFFF) / 4.0f, y0 = float(w1 & 0xFFF) / 4.0f;     // upper left
    const float x1 = float((w0 >> 12) & 0xFFF) / 4.0f, y1 = float(w0 & 0xFFF) / 4.0f;     // lower right
    if (x1 - x0 >= game::hud_overlay_frac * W) {   // a full-frame dim / page: everything after it is a menu (a full-width fill is a clear or a bar: centre)
        if (op == 0xE4) g_t.overlay = true;
        apply(st, kCentre);
        return;
    }
    if (g_rects.load() > 0) {
        std::printf("[thps] hud %s task %" PRIu64 " %.2f-%.2f x %.2f-%.2f\n", op == 0xF6 ? "fillrect" : "rect", stats.gfx_tasks.load(), x0, x1, y0, y1);
        std::fflush(stdout);
    }
    if (g_t.grp.feed(x0, y0, x1, y1)) {
        g_t.ganchor = classify(x0, x1, y0, W);   // by the first rect ...
        if (game::hud_anchor_by_bounds) {    // ... or, when the pre-scan planned this group (same first rect), by the whole group's bounds
            for (size_t j = g_t.plan_next; j < g_t.plan.size(); j++) {
                if (g_t.plan[j].w0 == w0 && g_t.plan[j].w1 == w1) {
                    if (g_debug.load() > 0 && g_t.plan[j].anchor != g_t.ganchor) {
                        std::printf("[thps] hud task %" PRIu64 " group %.2f-%.2f x %.2f-%.2f first rect says %s, whole group (%.2f-%.2f) says %s\n", stats.gfx_tasks.load(), x0, x1, y0, y1,
                                    g_t.ganchor == kLeft ? "left" : g_t.ganchor == kRight ? "right" : "centre", g_t.plan[j].x0, g_t.plan[j].x1,
                                    g_t.plan[j].anchor == kLeft ? "left" : g_t.plan[j].anchor == kRight ? "right" : "centre");
                        std::fflush(stdout);
                    }
                    g_t.ganchor = g_t.plan[j].anchor;
                    g_t.plan_next = j + 1;
                    break;
                }
            }
        }
        if (g_force.load() >= 0) g_t.ganchor = Anchor(g_force.load());
        g_task_groups[g_t.ganchor]++;
        g_groups[g_t.ganchor]++;
        if (g_debug.load() > 0 && g_groups[0] + g_groups[1] + g_groups[2] <= uint64_t(g_debug.load())) {   // (one line per group)
            std::printf("[thps] hud task %" PRIu64 " group %.2f-%.2f x %.2f-%.2f -> %s\n", stats.gfx_tasks.load(), x0, x1, y0, y1, g_t.ganchor == kLeft ? "left" : g_t.ganchor == kRight ? "right" : "centre");
            std::fflush(stdout);
        }
    }
    if (op == 0xF6) g_fills[g_t.ganchor]++;
    apply(st, g_t.ganchor);
}

bool hud_arg(int argc, char** argv, int& i) {
    if (std::string_view(argv[i]) == "--hud-debug" && i + 1 < argc) {
        g_debug = std::atoi(argv[++i]);
        return true;
    }
    if (std::string_view(argv[i]) == "--hud-rects" && i + 1 < argc) {
        g_rects = std::atoi(argv[++i]);
        return true;
    }
    if (std::string_view(argv[i]) == "--hud-force" && i + 1 < argc) {
        const std::string_view v = argv[++i];
        g_force = v == "left" ? kLeft : v == "right" ? kRight : v == "centre" ? kCentre : -1;
        return true;
    }
    return false;
}

std::string hud_report() {
    char buf[300];
    std::snprintf(buf, sizeof buf, "mode=%s supported=%d tasks_anchored=%" PRIu64 " groups left=%" PRIu64 " centre=%" PRIu64 " right=%" PRIu64
                  " overlay_tasks=%" PRIu64 " last_task left=%" PRIu64 " centre=%" PRIu64 " right=%" PRIu64 " fills left=%" PRIu64 " centre=%" PRIu64 " right=%" PRIu64,
                  gfx.hud.c_str(), int(hud_supported()), g_tasks_anchored.load(), g_groups[kLeft].load(), g_groups[kCentre].load(), g_groups[kRight].load(),
                  g_overlay_tasks.load(), g_last_task_groups[kLeft].load(), g_last_task_groups[kCentre].load(), g_last_task_groups[kRight].load(),
                  g_fills[kLeft].load(), g_fills[kCentre].load(), g_fills[kRight].load());
    return buf;
}

}  // namespace thps
