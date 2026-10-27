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
// `expand` is the default (host.h kDefaultHud); "original" emits nothing for one player: the picture is exactly what it was before this file existed,
// and so is any 4:3 (--aspect original) run, whatever the setting says.
// Two-player split screens (split = wide, widescreen): each player's HUD block goes to its own view's edge with `original` too, the same as
// `expand`. The views fill the wide picture there, and the 4:3 spot would leave each HUD floating inside its view (2026-10-18, user decision:
// "more practical for the expanded screen"). `16:9` places them in the halves of a 16:9 picture, as for one player.
// Two-player split screens with split = original: both views stay in the 4:3 middle, so the HUD keeps the game's own placement whatever `hud` says.
// The one-player rules must not run there: by thirds of the whole frame they sent player 1's block to the window's left edge, outside its view, and
// tore player 2's apart (its SCORE label in the middle third stayed put, its digits and meter went to the right edge; play test 2026-10-25).
// The side bands stay black there too: RT64 stretched the game's whole-width frame clear (THPS2 School II: the sky colour) over them; the RT64
// patch now clears them black and the fill's colour over the 4:3 area only (RT64::splitScreenBlackBands, counted as `bands` in the split report).
//
// THPS2 / THPS3 (same structure, rules per game in game/game.toml hud_*): their text is glyph rects with no bounds rect first, so a group is anchored
// by the bounds of the whole group, found by the pre-scan (hud_anchor_by_bounds), and only a group hugging an edge is anchored there (hud_edge_frac):
// the score / meter / tips go LEFT, the SWITCH badge and the sliding letters RIGHT; a bail's scattering letters, trick lines and the timer stay put.
//
// No game data: coordinates read from the display list at run time; the thresholds are per-game constants (thps::game::hud_*)
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "hle/rt64_rdp.h"
#include "hle/rt64_state.h"
#include "render/rt64_split_screen.h"

#include "hfr.h"
#include "host.h"

namespace {

enum Anchor : int { kCentre = 0, kLeft = 1, kRight = 2 };

std::atomic<bool> g_enabled{false};        // one-player placement: hud != original, widescreen, supported
std::atomic<bool> g_split_enabled{false};  // split-screen placement: widescreen, supported (any hud setting; the split setting is checked per task)
std::atomic<bool> g_split_active{false};   // the last HUD task was a widened split screen (hud_active: THPS2's ring clear)
std::atomic<uint64_t> g_split_kept{0};      // split-screen HUD tasks with split = original: the game's own placement (no alignment at all)
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
Anchor classify(float x0, float x1, float top, float W, float H) {
    if (top >= thps::game::hud_bottom_frac * H) return kCentre;   // the bottom band (menu prompts like "B GO BACK / A SELECT", captions) stays put
    if (thps::game::hud_edge_frac > 0.0f) {
        const bool l = x0 < W * thps::game::hud_edge_frac, r = x1 > W * (1.0f - thps::game::hud_edge_frac);
        return l && !r ? kLeft : r && !l ? kRight : kCentre;
    }
    const float cx = (x0 + x1) * 0.5f;
    return cx < W * thps::game::hud_left_frac ? kLeft : cx > W * thps::game::hud_right_frac ? kRight : kCentre;
}

// Where a group goes: an extended-GBI origin (G_EX_ORIGIN_LEFT 0 .. G_EX_ORIGIN_RIGHT 0x1000, linear in the wide frame) or kNoOrigin (the
// regular centred 4:3 placement). One player: LEFT -> the left edge, RIGHT -> the right edge, CENTRE -> no origin.
// Split screen (two side-by-side views, each widened to half of the wide frame by the RT64 patch, rt64_split_screen.h): each view is its own
// frame. A player's HUD is a compact block (THPS1: SCORE label, the digits right-aligned to the end of the SPECIAL meter below), much wider
// relative to a half than to the whole frame, so per-string thirds would tear it apart (label at the edge, digits and meter in the middle;
// first try, 2026-10-18). So the pre-scan joins the groups of a half that touch or nearly touch (8 px) into blocks, and a block is placed
// within its half: its left side within a quarter of the half's width of the half's left edge and its centre left of the half's middle ->
// the half's left edge; mirrored for the right edge; otherwise the half's centre (the trick text, also in the bottom band: the band means
// "centred in its view" here). The half's left / centre / right are the points 0 / 1/4 / 1/2 (left half) or 1/2 / 3/4 / 1 (right half) of
// the wide frame (origin = that share of G_EX_ORIGIN_RIGHT). A block across the split line (the timer, the divider) stays centred.
constexpr int kNoOrigin = -1;

int origin_of(Anchor a) { return a == kLeft ? G_EX_ORIGIN_LEFT : a == kRight ? G_EX_ORIGIN_RIGHT : kNoOrigin; }

constexpr float kSplitEdge = 0.25f;   // share of a half's width: a block starting this close to a half's edge belongs to that edge

int split_origin(float x0, float x1, float top, float W, Anchor* kind) {
    const float mid = W * 0.5f, H = W * 0.75f;
    *kind = kCentre;
    if (x0 < mid - 2.0f && x1 > mid + 2.0f) return kNoOrigin;   // across the split line
    const bool right_half = (x0 + x1) * 0.5f >= mid;
    const float off = right_half ? mid : 0.0f, h0 = x0 - off, h1 = x1 - off, hc = (h0 + h1) * 0.5f;
    if (top < thps::game::hud_bottom_frac * H) {
        if (h0 < kSplitEdge * mid && hc < mid * 0.5f) *kind = kLeft;
        else if (h1 > (1.0f - kSplitEdge) * mid && hc > mid * 0.5f) *kind = kRight;
    }
    const int base = right_half ? G_EX_ORIGIN_CENTER : G_EX_ORIGIN_LEFT;
    return base + (*kind == kLeft ? 0 : *kind == kCentre ? G_EX_ORIGIN_RIGHT / 4 : G_EX_ORIGIN_CENTER);
}

// A group the pre-scan found: its first rect's command words (the key the run-time pass recognises it by) and its anchor.
struct PlannedGroup {
    uint32_t w0, w1;
    Anchor anchor;
    int origin;     // where it goes (origin_of / split_origin)
    float x0, x1;   // the group's bounds (debug output)
};

// One gfx task.
struct TaskState {
    bool in3d = false;       // a projection matrix was loaded: the in-level frame
    bool overlay = false;    // a (nearly) full-width rect was drawn: a menu page, stays centred
    bool split = false;      // two side-by-side 3D views (a two-player split screen) that RT64 widens: each half is its own frame
    bool kept = false;       // a split screen that RT64 does not widen (split = original): no placement in this task
    int cur = kNoOrigin;     // the alignment (origin) the RDP has now
    Grouper grp;
    Anchor ganchor = kCentre;
    int gorigin = kNoOrigin;
    std::vector<PlannedGroup> plan;   // hud_anchor_by_bounds: the anchors the pre-scan chose, in drawing order
    size_t plan_next = 0;
};
TaskState g_t;
std::atomic<uint64_t> g_tasks_anchored{0}, g_groups[3] = {{0}, {0}, {0}}, g_overlay_tasks{0};
std::atomic<uint64_t> g_last_task_groups[3] = {{0}, {0}, {0}};
std::atomic<uint64_t> g_fills[3] = {{0}, {0}, {0}};   // G_FILLRECTs that went through the grouping (a meter's fill), by anchor: the test asserts the fill follows its frame
uint64_t g_task_groups[3] = {0, 0, 0};
std::atomic<uint64_t> g_split_tasks{0};               // HUD tasks with a split screen (pre-scan: half-width viewports at both sides)
std::atomic<uint64_t> g_split_groups[5] = {{0}, {0}, {0}, {0}, {0}};   // ... their groups by origin: quarter points 0 .. 4 of the wide frame
std::atomic<uint64_t> g_split_centred{0};             // ... groups at the regular placement (across the split line, the bottom band)

void apply(RT64::State* st, int origin) {
    if (origin == g_t.cur) return;
    g_t.cur = origin;
    RT64::RDP* rdp = st->rdp.get();
    RT64::ExtendedAlignment al;   // NONE / NONE: the regular (4:3, centred) placement
    RT64::ExtendedAlignment sc;
    if (origin != kNoOrigin) {
        // RT64 moves a rect by origin / RIGHT of the colour image's width (RDP::movedFromOrigin), then places it relative to that point of the
        // wide frame: undo the move, so the 4:3 coordinates keep their distance from the origin's point (LEFT: no offset; RIGHT: the whole width).
        al.leftOrigin = al.rightOrigin = uint16_t(origin);
        al.leftOffset = al.rightOffset = -int32_t(int64_t(rdp->colorImage.width) * 4 * origin / G_EX_ORIGIN_RIGHT);
    }
    if (origin != kNoOrigin) {   // the scissor spans the whole (extended) frame: left edge .. right edge, numbers unchanged
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
bool plan_task(const uint8_t* rdram, uint32_t data_ptr, std::vector<PlannedGroup>* plan, bool* split) {
    *split = false;
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
    bool halves[2] = {false, false};   // a half-width, (nearly) full-height viewport at the left / the right side of a 320-wide frame
    auto finish = [&](bool overlay) {
        *split = halves[0] && halves[1];
        if (plan == nullptr || overlay) return;
        // Split screen: blocks of nearby groups in the same half (union-find), placed as a whole (split_origin on the block's bounds).
        std::vector<size_t> parent(groups.size());
        for (size_t i = 0; i < parent.size(); i++) parent[i] = i;
        auto root = [&](size_t i) { while (parent[i] != i) i = parent[i] = parent[parent[i]]; return i; };
        auto half = [](const Raw& g) { return g.x0 < 158.0f && g.x1 > 162.0f ? -1 : (g.x0 + g.x1) * 0.5f >= 160.0f ? 1 : 0; };
        if (*split) {
            for (size_t i = 0; i < groups.size(); i++) {
                for (size_t j = i + 1; j < groups.size(); j++) {
                    const Raw &a = groups[i], &b = groups[j];
                    if (half(a) < 0 || half(a) != half(b)) continue;
                    if (b.x0 <= a.x1 + 8.0f && a.x0 <= b.x1 + 8.0f && b.y0 <= a.y1 + 8.0f && a.y0 <= b.y1 + 8.0f) parent[root(j)] = root(i);
                }
            }
        }
        std::vector<Raw> block(groups.size(), Raw{0, 0, 1e9f, -1e9f, 1e9f, -1e9f});
        for (size_t i = 0; i < groups.size(); i++) {
            Raw& b = block[root(i)];
            const Raw& g = groups[i];
            b.x0 = std::min(b.x0, g.x0), b.x1 = std::max(b.x1, g.x1), b.y0 = std::min(b.y0, g.y0), b.y1 = std::max(b.y1, g.y1);
        }
        for (size_t i = 0; i < groups.size(); i++) {
            const Raw& g = groups[i];
            Anchor a = classify(g.x0, g.x1, g.y0, 320.0f, 240.0f);
            const Raw& b = block[root(i)];
            const int o = *split ? split_origin(b.x0, b.x1, b.y0, 320.0f, &a) : origin_of(a);
            plan->push_back({g.w0, g.w1, a, o, *split ? b.x0 : g.x0, *split ? b.x1 : g.x1});
        }
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
            case 0xDC:   // G_MOVEMEM G_MV_VIEWPORT (index 8): Vp = vscale[4], vtrans[4] (shorts, quarter pixels; words native-endian in the host's RDRAM)
                if ((w0 & 0xFF) == 8 && width == 320) {
                    const uint32_t a = resolve(w1);
                    const float sx = std::fabs(float(int16_t(word(a) >> 16)) / 4.0f), sy = std::fabs(float(int16_t(word(a) & 0xFFFF)) / 4.0f);
                    const float tx = float(int16_t(word(a + 8) >> 16)) / 4.0f;
                    const float l = tx - sx, r = tx + sx, slack = 320.0f / 32.0f;
                    if (std::fabs((r - l) - 160.0f) <= slack && 2.0f * sy >= 0.75f * 240.0f) {
                        if (l <= slack) halves[0] = true;
                        else if (r >= 320.0f - slack) halves[1] = true;
                    }
                }
                break;
            case 0xF6:     // G_FILLRECT: the same grouping as a texrect, but a wide fill (a clear) is no overlay and no group
            case 0xE4: {   // G_TEXRECT
                const float x0 = float((w1 >> 12) & 0xFFF) / 4.0f, y0 = float(w1 & 0xFFF) / 4.0f;
                const float x1 = float((w0 >> 12) & 0xFFF) / 4.0f, y1 = float(w0 & 0xFFF) / 4.0f;
                if (width <= 320 && x1 - x0 >= thps::game::hud_overlay_frac * float(width)) {
                    if ((w0 >> 24) == 0xE4) {
                        finish(true);
                        return true;
                    }
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

bool hud_active() { return g_enabled.load() || g_split_active.load(); }
bool hud_placement_on() { return g_enabled.load(); }

void hud_set_mode(bool on, bool split_on) {
    g_enabled = on && hud_supported();
    g_split_enabled = split_on && hud_supported();
}

void hud_task_begin(RT64::State* st, const uint8_t* rdram, uint32_t dl_address) {
    g_t = TaskState{};
    for (uint64_t& n : g_task_groups) n = 0;
    RT64::splitScreenWide = gfx.split_wide;   // the RT64 patch's switch (rt64_split_screen.h); inert at 4:3
    RT64::splitScreenBlackBands = true;       // split = original: black side bands around the two 4:3 views (the game's frame clear stays in the 4:3 area)
    if (!g_enabled.load() && !g_split_enabled.load()) {
        g_split_active = false;
        return;
    }
    // A menu page: the whole task stays a centred 4:3 page. The plan: anchor-by-bounds groups, and every group of a split screen.
    bool split = false;
    std::vector<PlannedGroup> plan;
    const bool overlay = plan_task(rdram, dl_address, &plan, &split);
    g_split_active = split && gfx.split_wide && g_split_enabled.load();
    if (split && !g_split_active.load()) {   // split = original: the views stay in the 4:3 middle, and so does their HUD (any hud setting)
        g_t.kept = true;
        g_split_kept++;
        return;
    }
    if (!g_enabled.load() && !g_split_active.load()) return;   // hud = original, one player: no placement at all (the picture as before)
    g_t.overlay = overlay;
    g_t.split = g_split_active.load();
    if (game::hud_anchor_by_bounds || g_t.split) g_t.plan = std::move(plan);
    if (g_t.split) g_split_tasks++;
    st->rdp->setRectAlign(RT64::ExtendedAlignment());
    st->rdp->setScissorAlign(RT64::ExtendedAlignment());
}

void hud_task_end(RT64::State* st) {
    if ((!g_enabled.load() || g_t.kept) && g_t.cur == kNoOrigin) return;
    st->rdp->setRectAlign(RT64::ExtendedAlignment());
    st->rdp->setScissorAlign(RT64::ExtendedAlignment());
    g_t.cur = kNoOrigin;
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
    if ((!g_enabled.load() && !g_t.split) || g_t.kept) return;
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
        apply(st, kNoOrigin);
        return;
    }
    const float x0 = float((w1 >> 12) & 0xFFF) / 4.0f, y0 = float(w1 & 0xFFF) / 4.0f;     // upper left
    const float x1 = float((w0 >> 12) & 0xFFF) / 4.0f, y1 = float(w0 & 0xFFF) / 4.0f;     // lower right
    if (x1 - x0 >= game::hud_overlay_frac * W) {   // a full-frame dim / page: everything after it is a menu (a full-width fill is a clear or a bar: centre)
        if (op == 0xE4) g_t.overlay = true;
        apply(st, kNoOrigin);
        return;
    }
    if (g_rects.load() > 0) {
        std::printf("[thps] hud %s task %" PRIu64 " %.2f-%.2f x %.2f-%.2f\n", op == 0xF6 ? "fillrect" : "rect", stats.gfx_tasks.load(), x0, x1, y0, y1);
        std::fflush(stdout);
    }
    if (g_t.grp.feed(x0, y0, x1, y1)) {
        g_t.ganchor = classify(x0, x1, y0, W, W * 0.75f);   // by the first rect ...
        g_t.gorigin = g_t.split ? split_origin(x0, x1, y0, W, &g_t.ganchor) : origin_of(g_t.ganchor);
        if (game::hud_anchor_by_bounds || g_t.split) {    // ... or, when the pre-scan planned this group (same first rect), by the whole group's bounds
            for (size_t j = g_t.plan_next; j < g_t.plan.size(); j++) {
                if (g_t.plan[j].w0 == w0 && g_t.plan[j].w1 == w1) {
                    if (g_debug.load() > 0 && g_t.plan[j].anchor != g_t.ganchor) {
                        std::printf("[thps] hud task %" PRIu64 " group %.2f-%.2f x %.2f-%.2f first rect says %s, whole group (%.2f-%.2f) says %s\n", stats.gfx_tasks.load(), x0, x1, y0, y1,
                                    g_t.ganchor == kLeft ? "left" : g_t.ganchor == kRight ? "right" : "centre", g_t.plan[j].x0, g_t.plan[j].x1,
                                    g_t.plan[j].anchor == kLeft ? "left" : g_t.plan[j].anchor == kRight ? "right" : "centre");
                        std::fflush(stdout);
                    }
                    g_t.ganchor = g_t.plan[j].anchor;
                    g_t.gorigin = g_t.plan[j].origin;
                    g_t.plan_next = j + 1;
                    break;
                }
            }
        }
        if (g_force.load() >= 0) g_t.ganchor = Anchor(g_force.load()), g_t.gorigin = origin_of(g_t.ganchor);
        g_task_groups[g_t.ganchor]++;
        g_groups[g_t.ganchor]++;
        if (g_t.split) {
            if (g_t.gorigin == kNoOrigin) g_split_centred++;
            else g_split_groups[std::min(g_t.gorigin / (G_EX_ORIGIN_RIGHT / 4), 4)]++;
        }
        if (g_debug.load() > 0 && g_groups[0] + g_groups[1] + g_groups[2] <= uint64_t(g_debug.load())) {   // (one line per group)
            std::printf("[thps] hud task %" PRIu64 " group %.2f-%.2f x %.2f-%.2f -> %s%s origin %d\n", stats.gfx_tasks.load(), x0, x1, y0, y1, g_t.split ? "split " : "",
                        g_t.ganchor == kLeft ? "left" : g_t.ganchor == kRight ? "right" : "centre", g_t.gorigin);
            std::fflush(stdout);
        }
    }
    if (op == 0xF6) g_fills[g_t.ganchor]++;
    apply(st, g_t.gorigin);
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
    char buf[400];
    std::snprintf(buf, sizeof buf, "mode=%s supported=%d tasks_anchored=%" PRIu64 " groups left=%" PRIu64 " centre=%" PRIu64 " right=%" PRIu64
                  " overlay_tasks=%" PRIu64 " last_task left=%" PRIu64 " centre=%" PRIu64 " right=%" PRIu64 " fills left=%" PRIu64 " centre=%" PRIu64 " right=%" PRIu64
                  " enabled=%d split_enabled=%d",
                  gfx.hud.c_str(), int(hud_supported()), g_tasks_anchored.load(), g_groups[kLeft].load(), g_groups[kCentre].load(), g_groups[kRight].load(),
                  g_overlay_tasks.load(), g_last_task_groups[kLeft].load(), g_last_task_groups[kCentre].load(), g_last_task_groups[kRight].load(),
                  g_fills[kLeft].load(), g_fills[kCentre].load(), g_fills[kRight].load(), int(g_enabled.load()), int(g_split_enabled.load()));
    return buf;
}

std::string split_report() {
    char buf[300];
    std::snprintf(buf, sizeof buf, "wide=%d views=%" PRIu64 " fills=%" PRIu64 " hud_tasks=%" PRIu64 " groups q0=%" PRIu64 " q1=%" PRIu64 " q2=%" PRIu64
                  " q3=%" PRIu64 " q4=%" PRIu64 " centred=%" PRIu64 " kept=%" PRIu64 " bands=%" PRIu64,
                  int(gfx.split_wide), RT64::splitScreenViews.load(), RT64::splitScreenFills.load(), g_split_tasks.load(), g_split_groups[0].load(),
                  g_split_groups[1].load(), g_split_groups[2].load(), g_split_groups[3].load(), g_split_groups[4].load(), g_split_centred.load(), g_split_kept.load(), RT64::splitScreenBands.load());
    return buf;
}

}  // namespace thps
