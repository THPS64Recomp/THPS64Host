// Border crop: the graphics setting `border = original | fill` (v0.2; the value was called `crop` until 2026-10-25: the picture fills
// the frame the game left black, nothing is cut off).
//
// The border. A THPS1 level frame (traced with `dltrace`, USA v1.1 Warehouse) is, in order:
//   - the host's black clear of the whole 320x240 colour buffer (dl_fixes.cpp, lores_ring_clear);
//   - the depth clear: a fill rect (8,8)-(311,231) into the Z buffer (`F64DC39C 00020020`);
//   - a black colour fill (0,8)-(319,231);
//   - the scene, scissored to (8,8)-(311,231) (`ED020020 004DC39C`; the RDP's lower-right edge is exclusive, so the picture is
//     columns 8..310, rows 8..230), and the HUD with a full-frame scissor;
//   - two black fill rects over the top and bottom 8 px bands (`F6500020 00000000`, `F65003C0 0000039C`), then the inset scissor again.
// The viewport is the WHOLE frame, give or take a pixel and a half (RT64_DEBUG_WIDESCREEN: (-0.5, 1.5)-(318.5, 240.5)): the
// projection maps the view to 320x240 and only the scissor and the clears are inset. THPS2 and THPS3 frames have the same structure
// (their background-colour fill is inset too, (8,8)-(310,230)). A TV hid that ring in its overscan; RT64 shows the whole buffer, hence the thin
// black frame (RT64's widescreen scales it with the picture: at every aspect it is 1/40 of the width at the sides, 1/30 of the height
// at the top and bottom).
//
// `fill` (this file): before RT64 reads a gfx task (rt64_context.cpp send_dl, after fix_display_list) the inset is undone in the
// display list itself, in 320-wide frames only:
//   - an 8 px inset scissor ((7..9, 7..9)-(310..313, 230..233)) becomes the whole frame (0,0)-(320,240);
//   - an inset fill rect (the depth clear; THPS2/3's background fill) becomes (0,0)-(319,239);
//   - the two black band fills become no-ops (G_RDPPIPESYNC), and so do the 1 px texrects along the frame's very edge that THPS2/3's
//     pause overlay draws (transparent as far as we measured: prim alpha 0; dropped so nothing can show there; counted with the bands);
//   - a two-player split screen's two views (THPS1: (8,8)-(160,231) and (160,8)-(311,231), each with its own viewport) have a
//     half-width inset scissor each: its outer sides (the left or the right edge, the top, the bottom) go to the frame's edges and
//     its inner side (the middle) stays where the game put it, so the views reach the window's edges and still meet in the middle;
//     a fill rect inset the same way (a per-view clear) is widened alike. THPS2 and THPS3 draw the left view's scene with the
//     full-frame inset scissor (the right view then draws over the right half), so in a split-screen task (one with half-width
//     viewports on both sides) an inset full-frame scissor set while a half-width viewport is current, and used for triangles,
//     becomes that half, not the whole frame (2D drawn under it, such as THPS3's pause banner, keeps the whole frame). Counted as `crop_halves`. RT64's split-screen rule (rt64_split_screen.h) keeps seeing two side-by-side
//     halves: it intersects each scissor with its viewport widened by the clip ratio (below), so a whole-frame scissor there
//     would no longer look like a half and the left view would stay in the 4:3 middle;
//   - gSPClipRatio(FRUSTRATIO_1) becomes ratio 2: with ratio 1 triangles are clipped exactly at the viewport, which stops 1.5 px short
//     of row 0 and column 319, so THPS2/3 pause frames kept two dark rows at the top and a dark column at the right.
// Nothing is scaled: the projection and the viewport stay the game's, so the ring now shows the picture that lies just outside the
// inset, at the same scale and position (a slightly wider view, not a zoom), at every aspect. RT64's widescreen widens a full-width
// scissor exactly as it widened the inset one (rt64-present-capture-widescreen.patch's 1/32 slack was for the inset).
// The 640-wide front-end screens (menus, loading pages) keep their frame: it belongs to their 2D page. `original` (the default)
// leaves every task as it is: today's picture, byte for byte. The game rebuilds its display lists every frame, so switching back to
// `original` at run time needs no undo (the `crop_menu_toggle` test checks the frame comes back).
#include <atomic>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "host.h"

namespace {
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_tasks{0}, g_scissors{0}, g_fills{0}, g_bands{0}, g_clips{0}, g_halves{0};
std::atomic<bool> g_logged{false};

constexpr uint32_t k_scissor_full_w0 = 0xED000000u, k_scissor_full_w1 = 0x005003C0u;   // (0,0)-(320,240), 10.2 fixed point
constexpr uint32_t k_fill_full_w0 = 0xF64FC3BCu, k_fill_full_w1 = 0x00000000u;         // (0,0)-(319,239), inclusive
constexpr uint32_t k_sync_w0 = 0xE7000000u;                                              // G_RDPPIPESYNC: a no-op for RT64

bool in_range(uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v <= hi; }
// An 8 px inset rect of a 320x240 frame (whole pixels): top-left 7..9, right edge 310..313, bottom edge 230..233.
bool inset(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    return in_range(ulx, 7, 9) && in_range(uly, 7, 9) && in_range(lrx, 310, 313) && in_range(lry, 230, 233);
}
// One view of a side-by-side split screen, inset like the full frame on its outer sides: 1 = the left half (its right edge near the
// middle, 158..162), 2 = the right half (its left edge near the middle), 0 = neither.
int inset_half(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    if (!in_range(uly, 7, 9) || !in_range(lry, 230, 233)) return 0;
    if (in_range(ulx, 7, 9) && in_range(lrx, 158, 162)) return 1;
    if (in_range(ulx, 158, 162) && in_range(lrx, 310, 313)) return 2;
    return 0;
}
}  // namespace

void thps::border_crop_set(bool on) {
    if (g_on.exchange(on) != on && verbose()) {
        std::printf("[thps] border: %s\n", on ? "fill" : "original");
        std::fflush(stdout);
    }
}

bool thps::border_crop_on() { return g_on.load(); }

std::string thps::border_crop_report() {
    char buf[260];
    std::snprintf(buf, sizeof buf, "crop=%d crop_tasks=%llu crop_scissors=%llu crop_fills=%llu crop_bands=%llu crop_clips=%llu crop_halves=%llu",
                  int(g_on.load()), (unsigned long long)g_tasks.load(), (unsigned long long)g_scissors.load(), (unsigned long long)g_fills.load(),
                  (unsigned long long)g_bands.load(), (unsigned long long)g_clips.load(), (unsigned long long)g_halves.load());
    return buf;
}

void thps::crop_display_list(uint8_t* rdram, uint32_t data_ptr) {
    if (!g_on.load() || rdram == nullptr) return;
    auto word = [&](uint32_t a) -> uint32_t& { return *reinterpret_cast<uint32_t*>(rdram + (a & 0x7FFFFC)); };
    uint32_t seg[16] = {0};
    auto resolve = [&](uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0xFFFFFF; };
    std::vector<uint32_t> stack;
    uint32_t pc = data_ptr & 0xFFFFFF;
    uint32_t cimg_w = 0;   // width - 1 of the colour image (the depth clear draws through a "colour image" at the Z buffer: same width)
    uint64_t scissors = 0, fills = 0, bands = 0, outlines = 0, clips = 0, halves = 0;
    struct Edit { uint32_t addr, w0, w1; int half; };   // half: the half-width viewport current at an inset full-frame scissor (1 left, 2 right)
    std::vector<Edit> edits;   // applied only if the task has the inset 3D scissor (a 2D page's own black bands are left alone)
    int vp_half = 0;                  // the current viewport: 1 = the left half of the frame, 2 = the right half, 0 = anything else
    bool vp_seen[2] = {false, false};
    size_t pending = SIZE_MAX;        // the edit of the last inset full-frame scissor set under a half-width viewport ...
    bool pending_tris = false;        // ... and whether triangles were drawn under it (a 3D view: keep its half) or not (2D: whole frame)
    auto settle = [&] {
        if (pending != SIZE_MAX && !pending_tris) edits[pending].half = 0;
        pending = SIZE_MAX;
    };
    for (int cmds = 0; cmds < 200000; cmds++) {
        if (pc >= 0x800000) break;
        const uint32_t w0 = word(pc), w1 = word(pc + 4);
        pc += 8;
        bool done = false;
        switch (w0 >> 24) {
            case 0xFF: cimg_w = w0 & 0xFFF; break;   // G_SETCIMG
            case 0xED: {                              // G_SETSCISSOR (mode bits in w1 24..27: 0 = non-interlaced)
                settle();
                if (cimg_w != 319 || (w1 >> 24) != 0) break;
                const uint32_t ulx = ((w0 >> 12) & 0xFFF) >> 2, uly = (w0 & 0xFFF) >> 2, lrx = ((w1 >> 12) & 0xFFF) >> 2, lry = (w1 & 0xFFF) >> 2;
                if (inset(ulx, uly, lrx, lry)) {
                    edits.push_back({pc - 8, k_scissor_full_w0, k_scissor_full_w1, vp_half});
                    if (vp_half) pending = edits.size() - 1, pending_tris = false;
                    scissors++;
                } else if (const int h = inset_half(ulx, uly, lrx, lry)) {
                    // the middle (the left half's right edge, the right half's left edge) keeps the game's value; the rest goes to the frame's edges
                    if (h == 1) edits.push_back({pc - 8, 0xED000000u, (w1 & 0xFFF000u) | 0x3C0u, 0});
                    else edits.push_back({pc - 8, 0xED000000u | (w0 & 0xFFF000u), k_scissor_full_w1, 0});
                    halves++;
                }
                break;
            }
            case 0xF6: {                              // G_FILLRECT: w0 = lrx, lry; w1 = ulx, uly (10.2, inclusive in fill mode)
                if (cimg_w != 319) break;
                const uint32_t lrx = ((w0 >> 12) & 0xFFF) >> 2, lry = (w0 & 0xFFF) >> 2, ulx = ((w1 >> 12) & 0xFFF) >> 2, uly = (w1 & 0xFFF) >> 2;
                if (inset(ulx, uly, lrx + 1, lry + 1)) {
                    edits.push_back({pc - 8, k_fill_full_w0, k_fill_full_w1, 0});
                    fills++;
                } else if (const int h = inset_half(ulx, uly, lrx + 1, lry + 1)) {   // a per-view clear of a split screen
                    if (h == 1) edits.push_back({pc - 8, (w0 & 0xFFFFF000u) | 0x3BCu, 0, 0});
                    else edits.push_back({pc - 8, k_fill_full_w0, w1 & 0xFFF000u, 0});
                    halves++;
                } else if (ulx == 0 && lrx >= 319 && ((uly == 0 && in_range(lry, 7, 9)) || (in_range(uly, 230, 232) && lry >= 239))) {
                    edits.push_back({pc - 8, k_sync_w0, 0, 0});   // the black top / bottom band painted over the finished picture
                    bands++;
                }
                break;
            }
            case 0xE4: {                              // G_TEXRECT (w0 = lrx, lry; w1 = ulx, uly; the RDPHALF_1/2 words follow)
                if (cimg_w != 319) break;
                const uint32_t lrx = ((w0 >> 12) & 0xFFF) >> 2, lry = (w0 & 0xFFF) >> 2, ulx = ((w1 >> 12) & 0xFFF) >> 2, uly = (w1 & 0xFFF) >> 2;
                // THPS2's pause overlay ends with a 1 px dark outline at the frame's very edge ((0,0)-(321,1), (319,0)-(321,241), ...),
                // drawn where the inset hid it: with the frame cropped it would show as a dark line at the top and the right.
                const bool row = lrx >= 320 && ulx == 0 && lry - uly <= 2 && (uly == 0 || uly >= 238);
                const bool col = lry >= 240 && uly == 0 && lrx - ulx <= 2 && (ulx == 0 || ulx >= 318);
                if (row || col) {
                    edits.push_back({pc - 8, k_sync_w0, 0, 0});   // the halves after it only set RDP words: harmless on their own
                    outlines++;
                }
                break;
            }
            case 0xDB:  // G_MOVEWORD: segment table; clip ratio
                if (((w0 >> 16) & 0xFF) == 6) seg[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0xFFFFFF;
                // gSPClipRatio(FRUSTRATIO_1) (G_MW_CLIP: +x/+y ratio 1 at offsets 4 / 0xC, -1 at 0x14 / 0x1C): triangles are clipped
                // exactly at the viewport, and the game's viewport is (-0.5, 1.5)-(318.5, 240.5), so rows 0-1 and column 319 would stay
                // empty once the scissor no longer hides them. Ratio 2 moves the clip out to a guard band; inside the viewport nothing changes.
                if (cimg_w == 319 && ((w0 >> 16) & 0xFF) == 4) {
                    const uint32_t off = w0 & 0xFFFF;
                    if ((off == 0x04 || off == 0x0C) && w1 == 1) { edits.push_back({pc - 8, w0, 2, 0}); clips++; }
                    if ((off == 0x14 || off == 0x1C) && w1 == 0xFFFF) { edits.push_back({pc - 8, w0, 0xFFFE, 0}); clips++; }
                }
                break;
            case 0xDC:  // G_MOVEMEM G_MV_VIEWPORT (index 8): vscale, vtrans (shorts, quarter pixels; words native-endian in the host's RDRAM)
                if ((w0 & 0xFF) == 8 && cimg_w == 319) {
                    const uint32_t a = resolve(w1);
                    const int sx = std::abs(int(int16_t(word(a) >> 16))), sy = std::abs(int(int16_t(word(a) & 0xFFFF)));
                    const int tx = int16_t(word(a + 8) >> 16);
                    const int l = (tx - sx) / 4, r = (tx + sx) / 4;   // whole pixels; a half-width, (nearly) full-height viewport at one side
                    vp_half = 0;
                    if (std::abs((r - l) - 160) <= 10 && 2 * sy / 4 >= 180) vp_half = l <= 10 ? 1 : r >= 310 ? 2 : 0;
                    if (vp_half) vp_seen[vp_half - 1] = true;
                }
                break;
            case 0x05: case 0x06: case 0x07:   // G_TRI1, G_TRI2, G_QUAD (F3DEX2)
                pending_tris = true;
                break;
            case 0xDE:  // G_DL (push unless it is a branch)
                if (((w0 >> 16) & 0xFF) == 0) {
                    if (stack.size() > 18) { done = true; break; }
                    stack.push_back(pc);
                }
                pc = resolve(w1);
                break;
            case 0xDF:  // G_ENDDL
                if (stack.empty()) { done = true; break; }
                pc = stack.back();
                stack.pop_back();
                break;
            default: break;
        }
        if (done) break;
    }
    settle();
    if (scissors == 0 && halves == 0) return;   // a frame without an inset 3D scissor (front end, 2D pages) is left alone
    const bool split = vp_seen[0] && vp_seen[1];   // two side-by-side views (a lone half-width viewport, a model on a menu page, is not)
    for (const Edit& e : edits) {
        if (split && e.half == 1) word(e.addr) = 0xED000000u, word(e.addr + 4) = 0x002803C0u;          // (0,0)-(160,240)
        else if (split && e.half == 2) word(e.addr) = 0xED280000u, word(e.addr + 4) = k_scissor_full_w1;   // (160,0)-(320,240)
        else word(e.addr) = e.w0, word(e.addr + 4) = e.w1;
        if (split && e.half) halves++, scissors--;
    }
    g_tasks++;
    g_scissors += scissors;
    g_fills += fills;
    g_bands += bands + outlines;
    g_clips += clips;
    g_halves += halves;
    if (!g_logged.exchange(true)) {
        std::printf("[thps] border fill: first task filled (%llu inset scissors, %llu inset fills, %llu black bands)\n", (unsigned long long)scissors,
                    (unsigned long long)fills, (unsigned long long)bands);
        std::fflush(stdout);
    }
}
