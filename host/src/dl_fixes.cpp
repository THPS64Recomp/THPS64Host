// Display-list fixes applied to every gfx task before RT64 reads it (rt64_context.cpp send_dl).
//
// Edge column/row (Q19, "flashing on the edges"): THPS1 sets the scissor (0,0)-(319,239) (640x480
// screens: (0,0)-(639,479)) for its per-frame colour clears and 2D overlays (the code at
// 0x80036034 builds `ED000000 004FC3BC`).
// The RDP scissor's lower-right edge is exclusive, so column 319 and row 239 of a 320x240 frame
// are never cleared, while some 2D draws use a full (0,0)-(320,240) scissor and can leave pixels
// there (e.g. trick text sliding in from the right). With three framebuffers, one buffer can keep
// a stale yellow column 319 that the others don't have: it flickers at the right edge, most
// visibly on the pause menu (the game stops redrawing the 3D). A CRT hides that column in its
// overscan; RT64 shows the whole frame. Widening that scissor to (0,0)-(320,240) lets the
// game's own clears reach the last column and row. --no-edge-fix turns it off (A/B).
//
// Margin ring of the 640x480 screens (playtest 2026-10-12, "flashing border" on the skater / course select): each frame of a
// 640x480 screen starts with a depth clear inset 8 px (a fill rect (8,8)-(631,471) drawn into the Z buffer) and draws its picture
// inside the same 8 px scissor; nothing ever clears the colour buffer's outer 8 px ring (a CRT's overscan hides it). With three
// framebuffers the ring keeps whatever earlier screens (the main menu's full-frame art) left there, a different picture in each
// buffer, so RT64's frame, which shows the whole buffer, flashed stale art around the picture. We clear the ring: right after the
// prologue switches back from the Z buffer to the colour image, two commands fill the whole colour buffer black (the prologue has
// no-op syncs there: [sync, 1-cycle mode, sync] becomes [fill colour, fill rect, 1-cycle mode]).
//
// THPS3 (playtest 2026-10-13, "often flashing strip of screen on top left side in 16x9 mode"): the same frame structure at 320x240 (depth
// clear (8,8)-(311,231), colour image back, [sync, 1-cycle, sync]); the game paints the top and bottom 8 px bands black itself at the end
// of the frame but never the left and right ones, so two of the three framebuffers show stale 3D up to ~3 game pixels left of the picture
// (lores_ring_clear in game.toml). The scene fills the colour buffer inside the inset itself, so a black start-of-frame fill costs nothing.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <algorithm>
#include <atomic>
#include "hfr.h"
#include "host.h"

namespace {
constexpr uint32_t k_scissor_w0 = 0xED000000u;     // G_SETSCISSOR, (0,0) top-left, non-interlaced
// Lower-right corners in 10.2: (319,239) -> (320,240) in game, (639,479) -> (640,480) in the
// 640x480 front end and attract screens (same pattern there).
constexpr uint32_t k_scissor_319 = 0x004FC3BCu, k_scissor_320 = 0x005003C0u;
constexpr uint32_t k_scissor_639 = 0x009FC77Cu, k_scissor_640 = 0x00A00780u;
constexpr uint32_t k_sync_w0 = 0xE7000000u;                               // G_RDPPIPESYNC (a no-op for RT64)
constexpr uint32_t k_cycle1_w0 = 0xE3000A01u, k_cycle1_w1 = 0x00100000u;  // SETOTHERMODE_H: cycle type 1 cycle
constexpr uint32_t k_fill_black = 0x00010001u;                            // RGBA5551 black, opaque, twice
}  // namespace

std::atomic<uint64_t> thps::edge_fix_patches{0};
std::atomic<uint64_t> thps::ring_clear_patches{0};

void thps::fix_display_list(uint8_t* rdram, uint32_t data_ptr) {
    if (!options.edge_fix || rdram == nullptr) return;
    auto word = [&](uint32_t a) -> uint32_t& { return *reinterpret_cast<uint32_t*>(rdram + (a & 0x7FFFFC)); };
    uint32_t seg[16] = {0};
    auto resolve = [&](uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0xFFFFFF; };
    std::vector<uint32_t> stack;
    uint32_t pc = data_ptr & 0xFFFFFF;
    uint32_t zimg = 0, cimg = 0, full_scissor_w1 = 0;  // depth image, colour image, last scissor that starts at (0,0)
    bool zfilled = false;                              // a fill rect went into the depth image since the last colour image
    for (int cmds = 0; cmds < 200000; cmds++) {
        if (pc >= 0x800000) return;
        uint32_t& w0 = word(pc);
        uint32_t& w1 = word(pc + 4);
        pc += 8;
        switch (w0 >> 24) {
            case 0xFE: zimg = w1 & 0xFFFFFF; break;  // G_SETZIMG
            case 0xFF: {                             // G_SETCIMG
                const uint32_t img = w1 & 0xFFFFFF;
                const bool to_z = zimg != 0 && img == zimg;   // the depth clear draws through a "colour image" at the Z buffer
                // Back at the colour image after a depth clear of a 640-wide frame: clear the whole colour buffer too.
                const uint32_t cimg_w = w0 & 0xFFF;   // width - 1
                if (((thps::game::hires_ring_clear && cimg_w == 639) || ((thps::game::lores_ring_clear || (thps::game::hud_ring_clear && thps::hud_active())) && cimg_w == 319)) && !to_z && zfilled && full_scissor_w1 != 0 && pc + 24 <= 0x800000 && word(pc) == k_sync_w0 &&
                    word(pc + 8) == k_cycle1_w0 && word(pc + 12) == k_cycle1_w1 && word(pc + 16) == k_sync_w0) {
                    const uint32_t xh = ((full_scissor_w1 >> 12) & 0xFFF) - 4, yh = (full_scissor_w1 & 0xFFF) - 4;   // fill rects are inclusive
                    word(pc) = 0xF7000000u;                       word(pc + 4) = k_fill_black;      // G_SETFILLCOLOR
                    word(pc + 8) = 0xF6000000u | (xh << 12) | yh; word(pc + 12) = 0;                // G_FILLRECT (0,0)-(xh,yh)
                    word(pc + 16) = k_cycle1_w0;                  word(pc + 20) = k_cycle1_w1;      // back to 1 cycle
                    ring_clear_patches++;
                }
                if (!to_z) zfilled = false;
                cimg = img;
                break;
            }
            case 0xF6:  // G_FILLRECT
                if (zimg != 0 && cimg == zimg) zfilled = true;
                break;
            case 0xED:  // G_SETSCISSOR
                if (w0 == k_scissor_w0 && (w1 == k_scissor_319 || w1 == k_scissor_639)) {
                    w1 = w1 == k_scissor_319 ? k_scissor_320 : k_scissor_640;
                    edge_fix_patches++;
                }
                if (w0 == k_scissor_w0) full_scissor_w1 = w1;
                break;
            case 0xDB:  // G_MOVEWORD: segment table
                if (((w0 >> 16) & 0xFF) == 6) seg[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0xFFFFFF;
                break;
            case 0xDE:  // G_DL (push unless it is a branch)
                if (((w0 >> 16) & 0xFF) == 0) {
                    if (stack.size() > 18) return;
                    stack.push_back(pc);
                }
                pc = resolve(w1);
                break;
            case 0xDF:  // G_ENDDL
                if (stack.empty()) return;
                pc = stack.back();
                stack.pop_back();
                break;
            default: break;
        }
    }
}

// ---- DL canary (debug; THPS_DL_CANARY=1): did the game rewrite a display list while RT64 was still parsing it? ----
// ultramodern tells the game "RSP done" before send_dl starts (events.cpp: sp_complete() precedes send_dl), and a game may reuse
// the input list once the RSP is done. This walks the list exactly like fix_display_list (all nested G_DLs), keeps every command,
// and after RT64 is finished compares them with RDRAM.
namespace {
struct CanaryCmd { uint32_t addr, w0, w1; };
struct CanaryData { uint32_t addr, len; uint8_t kind; size_t off; };   // kind: 'V' vertices, 'M' matrix, 'L' movemem (light / viewport)
thread_local std::vector<CanaryCmd> g_canary;
thread_local std::vector<CanaryData> g_canary_data;
thread_local std::vector<uint8_t> g_canary_bytes;
thread_local uint32_t g_canary_dl = 0;
std::atomic<uint64_t> g_canary_tasks{0}, g_canary_hits{0};
}  // namespace

bool thps::dl_canary_enabled() {
    static const bool on = std::getenv("THPS_DL_CANARY") != nullptr;
    return on;
}

void thps::dl_canary_begin(uint8_t* rdram, uint32_t data_ptr) {
    g_canary.clear();
    g_canary_data.clear();
    g_canary_bytes.clear();
    g_canary_dl = data_ptr & 0xFFFFFF;
    auto word = [&](uint32_t a) -> uint32_t { return *reinterpret_cast<uint32_t*>(rdram + (a & 0x7FFFFC)); };
    uint32_t seg[16] = {0};
    auto resolve = [&](uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0xFFFFFF; };
    std::vector<uint32_t> stack;
    uint32_t pc = g_canary_dl;
    for (int cmds = 0; cmds < 200000; cmds++) {
        if (pc >= 0x800000) return;
        const uint32_t w0 = word(pc), w1 = word(pc + 4);
        g_canary.push_back({pc, w0, w1});
        pc += 8;
        auto keep = [&](uint32_t a, uint32_t len, uint8_t kind) {
            a = resolve(a);
            if (a + len > 0x800000 || len == 0 || len > 4096) return;
            g_canary_data.push_back({a, len, kind, g_canary_bytes.size()});
            g_canary_bytes.insert(g_canary_bytes.end(), rdram + a, rdram + a + len);
        };
        switch (w0 >> 24) {
            case 0x01: keep(w1, ((w0 >> 12) & 0xFF) * 16, 'V'); break;                 // G_VTX (F3DEX2)
            case 0xDA: keep(w1, 64, 'M'); break;                                       // G_MTX
            case 0xDC: keep(w1, ((w0 >> 19) & 0x1F) * 8 + 8, 'L'); break;              // G_MOVEMEM
            case 0xDB: if (((w0 >> 16) & 0xFF) == 6) seg[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0xFFFFFF; break;
            case 0xDE:
                if (((w0 >> 16) & 0xFF) == 0) { if (stack.size() > 18) return; stack.push_back(pc); }
                pc = resolve(w1);
                break;
            case 0xDF:
                if (stack.empty()) return;
                pc = stack.back(); stack.pop_back();
                break;
            default: break;
        }
    }
}

void thps::dl_canary_end(uint8_t* rdram, uint64_t task_index) {
    auto word = [&](uint32_t a) -> uint32_t { return *reinterpret_cast<uint32_t*>(rdram + (a & 0x7FFFFC)); };
    uint32_t changed = 0, first = 0, lo = 0xFFFFFFFF, hi = 0;
    for (const CanaryCmd& c : g_canary) {
        lo = std::min(lo, c.addr); hi = std::max(hi, c.addr);
        if (word(c.addr) != c.w0 || word(c.addr + 4) != c.w1) {
            if (!changed) first = c.addr;
            if (changed < 4 && task_index < 8) std::printf("[canary]   0x%06X: %08X %08X -> %08X %08X\n", c.addr, c.w0, c.w1, word(c.addr), word(c.addr + 4));
            changed++;
        }
    }
    uint32_t dchg[3] = {0, 0, 0}, dtot[3] = {0, 0, 0}, dfirst = 0;
    for (const CanaryData& d : g_canary_data) {
        const int k = d.kind == 'V' ? 0 : d.kind == 'M' ? 1 : 2;
        dtot[k]++;
        if (std::memcmp(rdram + d.addr, g_canary_bytes.data() + d.off, d.len) != 0) { if (!dchg[0] && !dchg[1] && !dchg[2]) dfirst = d.addr; dchg[k]++; }
    }
    uint64_t n = ++g_canary_tasks;
    if (dchg[0] + dchg[1] + dchg[2]) {
        static std::atomic<uint64_t> data_hits{0};
        uint64_t h = ++data_hits;
        std::printf("[canary] task %llu: DATA rewritten while RT64 was parsing: vertices %u/%u, matrices %u/%u, movemem %u/%u (first at 0x%06X)  [%llu data hits / %llu tasks]\n",
                    (unsigned long long)task_index, dchg[0], dtot[0], dchg[1], dtot[1], dchg[2], dtot[2], dfirst, (unsigned long long)h, (unsigned long long)n);
        std::fflush(stdout);
    }
    if (changed) {
        uint64_t h = ++g_canary_hits;
        std::printf("[canary] task %llu: DL 0x%06X (%zu cmds, 0x%06X-0x%06X): %u commands rewritten while RT64 was parsing (first at 0x%06X)  [%llu/%llu tasks]\n",
                    (unsigned long long)task_index, g_canary_dl, g_canary.size(), lo, hi, changed, first, (unsigned long long)h, (unsigned long long)n);
        std::fflush(stdout);
    } else if (n <= 16 || (n % 600) == 0) {
        std::printf("[canary] task %llu: DL 0x%06X (%zu cmds, 0x%06X-0x%06X) intact  [%llu/%llu tasks]\n", (unsigned long long)task_index, g_canary_dl,
                    g_canary.size(), lo, hi, (unsigned long long)g_canary_hits.load(), (unsigned long long)n);
        std::fflush(stdout);
    }
}
