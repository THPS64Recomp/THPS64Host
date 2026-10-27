// Fog: the graphics setting `fog = original | soft | off` (v0.2; control command `fog [original|soft|off] [START]`).
//
// The games' fog (the same engine in THPS1-3; journal 2026-10-25-fog). When a course's fog amount (a level-script value / 4096) is
// below 0.999 the materials set G_FOG and the cycle-1 blender G_RM_FOG_SHADE_A, and each frame the game writes one constant
// gSPFogPosition(996, 1000), `DB080000 7D008400`, into its top-level display list (THPS1 inline at 0x800344F8, only when fog is on;
// THPS2 func_800BBBA8 and THPS3 func_800C0060, every frame). The fog colour is the course's clear colour. The ramp is linear in NDC
// depth, so where it lies depends on the projection: with the courses' near of 10.5 it starts at ~86 % of a far of 439, ~76 % of a far
// twice that, ~62 % at 4x and ~13 % (about 2,300 units) of the Full draw distance's 18000. fm is an s16, so 996..1000 (fm 32000) is
// already the steepest ramp there is: fog can be moved closer or made lighter, never steeper. So the game's fog is thin by design: a
// haze over the last ~13 % of the view. The sky and scenery pass (near 200, far 20000) and the skater draw no fog.
//
// Before RT64 reads a gfx task (rt64_context.cpp send_dl, after the border crop) the game's own fog command, that exact word pair and
// nothing else, is rewritten:
//   original  the game's command as it is (the walk is skipped unless memory still holds an earlier edit: g_outstanding);
//   soft      the ramp runs from START x the far plane in use (kSoftStart) to the far plane, never steeper than the game's ramp, so
//             Soft only ever adds fog. As the far plane moves out (Draw Distance) the start moves with it. Where the game's ramp starts
//             nearer than START x far (long draw distances: at Full, from about 2,300 units) the game's ramp stays;
//   off       fm = fo = 0: fog alpha 0 everywhere, the plain colours.
// Finding the far plane: the games load each object's whole model-view-projection into the projection slot, so near and far can't be
// read off one matrix. With P a glFrustum and an affine model-view, column 2 of the product = -a' x column 3 in rows 0..2, with
// a' = -(f + n) / (f - n), so r = far / near = (a' - 1) / (a' + 1). The ramp's NDC start for a fraction s of the far needs only r:
// z = ((r + 1) - 2 / s) / (r - 1). The r used is the one most fogged triangles of the frame were drawn with (the camera's).
// After a mid-run Draw Distance change THPS2's world view keeps its old projection until the next course start while a few extra items
// use the new one; one fog command per frame fits one of them, the majority.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "host.h"

namespace {
// Soft's start, as a fraction of the far plane in use. 0.7 (the prototype had 0.6): a gentle haze. The sky, the scenery pass and the
// skater are never fogged, so a stronger fog tints mid-distance buildings toward the clear colour in front of a clear sky, which reads as
// fog rather than distance (THPS2 and THPS3 courses at Original, journal 2026-10-26-fog-setting).
constexpr float kSoftStart = 0.7f;

std::atomic<int> g_mode{0};   // 0 original, 1 soft, 2 off
std::atomic<float> g_start{kSoftStart};
std::atomic<uint64_t> g_tasks{0}, g_found{0}, g_edits{0};
std::atomic<bool> g_dirty{false};   // some storage may still hold a fog word of ours: Original walks the lists to put the game's back
// The storage that holds our words (review 2026-10-26, finding 11): each fog command we rewrote, with the word we wrote. An entry retires
// when that memory no longer holds the command with our word (we put the game's back, or the game wrote a new list over it); once none is
// left, nothing drawn can hold an edit and Original is the game's list as it is again (g_dirty off: no walk). More than kMaxOutstanding at
// once (not seen: two display lists, one command each) keeps g_dirty on for the run, as before. Only the gfx thread touches the vector.
constexpr size_t kMaxOutstanding = 256;
std::vector<std::pair<uint32_t, uint32_t>> g_outstanding;   // (command address, our word)
bool g_overflow = false;
std::atomic<uint32_t> g_outstanding_n{0};
std::atomic<uint64_t> g_walks{0};
std::mutex g_diag_mutex;
std::string g_diag = "-";
// The words written so far (each Soft r and start gives one; Off is 0): a fog command holding one of them is ours, if a display list is
// ever drawn again without the game writing its command anew. Only the gfx thread touches it.
std::vector<uint32_t> g_written;

constexpr uint32_t kFogW0 = 0xDB080000u, kGameFogW1 = 0x7D008400u;   // G_MOVEWORD G_MW_FOG; gSPFogPosition(996, 1000)

uint32_t soft_word(double r, float start) {
    if (!(r > 1.0001)) return kGameFogW1;
    const double st = std::max(double(start), 1.01 / r);
    const double z = ((r + 1.0) - 2.0 / st) / (r - 1.0);
    double mn = 500.0 * (z + 1.0);
    const double mx = 1000.0;
    if (mn > 996.0) mn = 996.0;   // never steeper than the game's own ramp (fm must fit an s16)
    const double fm = 128000.0 / (mx - mn), fo = (500.0 - mn) * 256.0 / (mx - mn);
    const int ifm = int(std::lround(std::min(fm, 32767.0))), ifo = int(std::lround(std::max(-32768.0, std::min(fo, 32767.0))));
    return (uint32_t(uint16_t(int16_t(ifm))) << 16) | uint16_t(int16_t(ifo));
}

bool ours(uint32_t w1) { return std::find(g_written.begin(), g_written.end(), w1) != g_written.end(); }

// Drops the entries whose memory no longer holds our edit; true when none is left (gfx thread).
bool retire_outstanding(uint8_t* rdram) {
    auto word = [&](uint32_t a) { return *reinterpret_cast<const uint32_t*>(rdram + (a & 0x7FFFFC)); };
    g_outstanding.erase(std::remove_if(g_outstanding.begin(), g_outstanding.end(),
                                       [&](const auto& e) { return word(e.first) != kFogW0 || word(e.first + 4) != e.second; }),
                        g_outstanding.end());
    g_outstanding_n = uint32_t(g_outstanding.size());
    return g_outstanding.empty() && !g_overflow;
}
}  // namespace

bool thps::fog_valid(const std::string& v) { return v == "original" || v == "soft" || v == "off"; }

void thps::fog_set(const std::string& v, float start) {
    g_mode = v == "soft" ? 1 : v == "off" ? 2 : 0;
    if (start > 0.0f && start < 1.0f) g_start = start;
}

std::string thps::fog_mode() {
    static const char* names[] = {"original", "soft", "off"};
    return names[std::clamp(g_mode.load(), 0, 2)];
}

std::string thps::fog_report() {
    static const char* names[] = {"original", "soft", "off"};
    char buf[200];
    std::snprintf(buf, sizeof buf, "fog=%s start=%.2f tasks=%llu found=%llu edits=%llu dirty=%d outstanding=%u walks=%llu", names[std::clamp(g_mode.load(), 0, 2)],
                  double(g_start.load()), (unsigned long long)g_tasks.load(), (unsigned long long)g_found.load(),
                  (unsigned long long)g_edits.load(), int(g_dirty.load()), g_outstanding_n.load(), (unsigned long long)g_walks.load());
    std::lock_guard lock{g_diag_mutex};
    return std::string(buf) + " last: " + g_diag;
}

void thps::fog_display_list(uint8_t* rdram, uint32_t data_ptr) {
    if (rdram == nullptr) return;
    const int m = g_mode.load();
    if (m == 0 && !g_dirty.load()) return;   // Original: the game's list as it is
    if (m == 0 && retire_outstanding(rdram)) {   // Original, and every edit has been overwritten since: the fast path again
        g_dirty = false;
        return;
    }
    if (m != 0) retire_outstanding(rdram);   // Soft / Off: forget what the game has written over (bounds the list)
    g_walks++;
    auto word = [&](uint32_t a) -> uint32_t& { return *reinterpret_cast<uint32_t*>(rdram + (a & 0x7FFFFC)); };
    uint32_t seg[16] = {0};
    auto resolve = [&](uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0xFFFFFF; };
    auto mtx = [&](uint32_t a, int k) {   // element k (row-major) of an N64 fixed-point matrix: 16 s16 integer parts, then 16 u16 fractions
        const uint32_t wi = word(a + (k & ~1) * 2), wf = word(a + 32 + (k & ~1) * 2);
        const int16_t i = int16_t((k & 1) ? (wi & 0xFFFF) : (wi >> 16));
        const uint16_t fr = uint16_t((k & 1) ? (wf & 0xFFFF) : (wf >> 16));
        return double(i) + double(fr) / 65536.0;
    };
    struct Proj { double r; uint32_t fog_tris, tris; };
    std::vector<Proj> projs;
    std::vector<uint32_t> fog_cmds;   // addresses of the game's fog commands (or of ours from an earlier edit)
    uint32_t geom = 0;
    std::vector<uint32_t> stack;
    uint32_t pc = data_ptr & 0xFFFFFF;
    for (int cmds = 0; cmds < 200000; cmds++) {
        if (pc >= 0x800000) break;
        const uint32_t w0 = word(pc), w1 = word(pc + 4);
        pc += 8;
        bool done = false;
        switch (w0 >> 24) {
            case 0xDB:
                if (((w0 >> 16) & 0xFF) == 6) seg[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0xFFFFFF;
                if (w0 == kFogW0 && (w1 == kGameFogW1 || ours(w1))) fog_cmds.push_back(pc - 8);
                break;
            case 0xDA:   // G_MTX (F3DEX2): params in the low byte (G_MTX_PROJECTION = 4)
                if (m == 1 && (w0 & 0x04)) {
                    const uint32_t a = resolve(w1);
                    int bi = 0;
                    double best = 0;
                    for (int i = 0; i < 3; i++) if (std::fabs(mtx(a, 4 * i + 3)) > best) best = std::fabs(mtx(a, 4 * i + 3)), bi = i;
                    double r = 0;
                    if (best > 1e-4) {
                        const double ap = -mtx(a, 4 * bi + 2) / mtx(a, 4 * bi + 3);
                        if (std::fabs(ap + 1.0) > 1e-9) r = (ap - 1.0) / (ap + 1.0);
                    }
                    projs.push_back({r, 0, 0});
                }
                break;
            case 0xD9:   // G_GEOMETRYMODE: clear (AND w0's low 24 bits), then set w1
                geom = (geom & (w0 | 0xFF000000u)) | w1;
                break;
            case 0x05: case 0x06: case 0x07:   // G_TRI1, G_TRI2, G_QUAD
                if (!projs.empty()) {
                    const uint32_t t = (w0 >> 24) == 0x05 ? 1 : 2;
                    projs.back().tris += t;
                    if (geom & 0x00010000u) projs.back().fog_tris += t;   // G_FOG
                }
                break;
            case 0xDE:   // G_DL: call (push) or branch
                if (((w0 >> 16) & 0xFF) == 0) {
                    if (stack.size() > 18) { done = true; break; }
                    stack.push_back(pc);
                }
                pc = resolve(w1);
                break;
            case 0xDF:   // G_ENDDL
                if (stack.empty()) { done = true; break; }
                pc = stack.back();
                stack.pop_back();
                break;
            default: break;
        }
        if (done) break;
    }
    if (fog_cmds.empty()) return;   // not a course frame with fog
    g_tasks++;
    g_found += fog_cmds.size();
    uint32_t w1 = kGameFogW1;
    double r_main = 0;
    if (m == 1) {
        // The camera's r: the one most fogged triangles were drawn with.
        std::vector<std::pair<double, uint32_t>> groups;
        for (const Proj& p : projs) {
            if (!p.fog_tris || !(p.r > 1.0001)) continue;
            bool hit = false;
            for (auto& g : groups) if (std::fabs(g.first - p.r) < 0.01 * p.r) { g.second += p.fog_tris; hit = true; break; }
            if (!hit) groups.emplace_back(p.r, p.fog_tris);
        }
        uint32_t best = 0;
        for (auto& g : groups) if (g.second > best) best = g.second, r_main = g.first;
        if (!(r_main > 1.0001)) return;   // no fogged triangle this frame: nothing to fit
        w1 = soft_word(r_main, g_start.load());
    } else if (m == 2) {
        w1 = 0;
    }
    if (w1 != kGameFogW1 && !ours(w1)) {
        if (g_written.size() >= 64) g_written.erase(g_written.begin());
        g_written.push_back(w1);
    }
    for (uint32_t a : fog_cmds) {
        if (word(a + 4) != w1) {
            word(a + 4) = w1;
            g_edits++;
        }
        auto e = std::find_if(g_outstanding.begin(), g_outstanding.end(), [&](const auto& x) { return x.first == a; });
        if (w1 == kGameFogW1) {
            if (e != g_outstanding.end()) g_outstanding.erase(e);
        } else if (e != g_outstanding.end()) {
            e->second = w1;
        } else if (g_outstanding.size() < kMaxOutstanding) {
            g_outstanding.emplace_back(a, w1);
        } else {
            g_overflow = true;
        }
    }
    if (w1 != kGameFogW1) g_dirty = true;
    else if (retire_outstanding(rdram)) g_dirty = false;   // Original: everything put back
    g_outstanding_n = uint32_t(g_outstanding.size());
    char b[96];
    std::snprintf(b, sizeof b, "r=%.1f cmds=%zu word=%08X", r_main, fog_cmds.size(), w1);
    std::lock_guard lock{g_diag_mutex};
    g_diag = b;
}
