// High frame rate for the THPS recomp hosts: RT64 frame interpolation.
//
// THPS1's logic steps at a fixed 1/30 s (gPhysicsFrameTime, 0x800F5668 in v1.1) and the scheduler swaps
// every second retrace, so the game shows 30 frames per second. With graphics.cfg `refresh`
// set to `display` or a number (--refresh), RT64 renders extra frames between two game frames by
// pairing each transform with its counterpart in the previous frame and interpolating it.
//
// What this file adds on top of RT64's automatic pairing:
//   - the game's real rate (30) is given to RT64 once per gfx task (= gEXSetRefreshRate(30));
//   - the camera (the view-projection) is tagged with an ID that changes on a camera cut, so
//     the cut is not smeared across the in-between frames (= gEXMatrixGroupSimple on the
//     projection with a new ID, Zelda64Recomp camera_transform_tagging.c's approach);
//   - on a cut, every transform of that frame is tagged G_EX_ID_IGNORE (shown as is);
//   - every projection load is tagged with an ID per kind (camera / planar shadow), so RT64 never interpolates a
//     camera scene from the shadow scene (the THPS2 one-frame "hole"; see "projection kinds" below);
//   - the combined camera matrix is split into view and projection by the host (RT64's split loses the depth range when the
//     camera looks along the world z axis; see "view-projection split" below);
//   - pairing counters (RT64 patch rt64-hfr.patch: RT64::hfrStats) for the perf log.
// Instead of writing extended-GBI commands into the game's display lists (RDRAM), the host calls
// the RT64 functions those commands would call (RSP::matrixId, State::setRefreshRate) when
// RT64 starts interpreting a gfx task, or at a command address (Interpreter::commandHook).
//
// No game data: only addresses, counters and matrices read at run time.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hle/rt64_application.h"
#include "hle/rt64_hfr_stats.h"
#include "hle/rt64_interpreter.h"
#include "hle/rt64_state.h"

#include "hfr.h"
#include "host.h"

namespace {

std::atomic<RT64::Application*> g_app{nullptr};

// Counters at the last perf line.
uint64_t l_matched = 0, l_interp = 0, l_tf = 0, l_paired = 0, l_skip = 0, l_vp = 0, l_vpp = 0, l_disp = 0, l_drop = 0;

void reply(const std::string& s) {
    std::printf("[ctl] %s\n", s.c_str());
    std::fflush(stdout);
}

// ---- camera cuts ------------------------------------------------------------------------------
// THPS1 loads its camera as a combined view*projection matrix (G_MTX PROJECTION|LOAD; two per
// in-level frame) and every model matrix in world space. A camera cut (level start, retry,
// respawn, replay camera switches, competition intro) shows up as a jump of the camera centre
// or a sudden turn of the view direction between two game frames; the follow camera never
// moves like that. On a cut, the frame's camera and transforms are tagged G_EX_ID_IGNORE, so
// RT64 shows the new frame as is instead of sweeping between the two views.
struct Cam {
    float c[3] = {0, 0, 0};  // camera centre (world units)
    float d[3] = {0, 0, 1};  // view direction (gradient of clip w)
    bool ok = false;
};

constexpr int kMaxProj = 4;
Cam g_prev[kMaxProj], g_cur[kMaxProj];
int g_proj_in_task = 0;
bool g_cut_this_task = false;
float g_step_avg = 0;      // running average of the main camera's per-frame movement
float g_cut_jump = 0, g_cut_angle = 0;
std::atomic<uint64_t> g_cuts{0}, g_cut_frames_seen{0};
std::atomic<float> g_last_jump{0}, g_last_angle{0};
// Thresholds (--hfr-cut-jump / --hfr-cut-angle): a cut is a move of more than
// max(cut_jump_min, cut_jump_factor x the recent average step), or a turn above cut_angle degrees.
float g_cut_jump_factor = 8.0f, g_cut_jump_min = 40.0f, g_cut_angle_deg = 25.0f;
bool g_hfr_enabled = true;    // --hfr-tags 0: plain RT64 auto pairing (A/B)

// ---- projection kinds (the THPS2 "hole") ----------------------------------------------------------
// RT64 pairs a frame's scenes (groups of identical view-projections) with the previous frame's by the smallest matrix
// difference, then interpolates each scene's camera from its partner. A THPS level frame has a camera scene (plus near-identical
// variants: depth-biased decals, a far copy) and the skater's planar-shadow scene (a singular matrix: the camera with the y row
// zeroed, i.e. squashed onto the ground). When the camera moves fast (THPS2 Hangar, skating off the start), the previous frame's
// shadow matrix can be closer to this frame's camera than the previous camera is: RT64 then pairs camera <- shadow and
// shadow <- camera, and the in-between frame draws the world and the skater with a camera half way to the squashed shadow
// matrix (skater and half the world gone, a skater-shaped dark silhouette where the shadow is). Measured with an RT64
// diagnostic build (docs/interpolation-ids.md, known limits). Tagging every projection load with an ID per kind
// (gEXMatrixGroup on the projection stack) makes RT64 refuse such a pair (a tagged projection is only interpolated from one with
// the same ID): the scene is then shown with this frame's camera (one 30 Hz step) instead of the broken mix. Camera-like
// variants share one ID (pairing them with each other is harmless); 2D (orthographic) loads go back to RT64's default group.
// --hfr-proj-ids 0 / --control projids off: the old behaviour (A/B).
// The switches (this one and vpsplit below) are written by the --control thread; the command hook uses a copy sampled once
// at hfr_task_begin, so a whole gfx task sees one value (Astra review C2).
constexpr uint32_t kProjIdCamera = 0x7C0A0001u, kProjIdShadow = 0x7C0A0002u;
std::atomic<bool> g_proj_ids{true};
bool g_proj_ids_task = true;             // g_proj_ids for the current gfx task
uint32_t g_proj_id_set = G_EX_ID_AUTO;   // the projection group this task's stack top has (G_EX_ID_AUTO = RT64's default)
std::atomic<uint64_t> g_proj_tags_camera{0}, g_proj_tags_shadow{0};

// 0 = perspective camera, 1 = orthographic (no w column: 2D / HUD), 2 = singular (the planar shadow: no camera centre).
int projection_kind(const float m[4][4]) {
    if (std::fabs(m[0][3]) + std::fabs(m[1][3]) + std::fabs(m[2][3]) < 1e-9f) return 1;
    const float det = m[0][0] * (m[1][1] * m[2][3] - m[1][3] * m[2][1]) - m[0][1] * (m[1][0] * m[2][3] - m[1][3] * m[2][0]) +
                      m[0][3] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    return std::fabs(det) < 1e-9f ? 2 : 0;
}

void tag_projection(RT64::State* state, const float m[4][4]) {
    const int kind = projection_kind(m);
    const uint32_t id = kind == 0 ? kProjIdCamera : kind == 2 ? kProjIdShadow : G_EX_ID_AUTO;
    if (id == g_proj_id_set) return;
    // The values RT64 uses for an untagged camera (GameFrame::matchScene): not decomposed, every component interpolated; the
    // rest as RT64's default group. Only the ID differs, so a correctly paired scene is interpolated exactly as before.
    const bool def = id == G_EX_ID_AUTO;
    const uint8_t c = def ? G_EX_COMPONENT_AUTO : G_EX_COMPONENT_INTERPOLATE;
    state->rsp->matrixId(id, false, true, def, c, c, c, c, c, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO,
                         G_EX_COMPONENT_AUTO, G_EX_ORDER_AUTO, G_EX_ASPECT_AUTO, G_EX_EDIT_NONE, false, false);
    g_proj_id_set = id;
    if (kind == 0) g_proj_tags_camera++;
    if (kind == 2) g_proj_tags_shadow++;
}

// ---- the view-projection split (THPS2 jutter / "popping" backgrounds) -------------------------------------
// THPS loads its camera as one combined view * projection matrix. RT64 splits a combined load into a view and a projection
// (common/rt64_math.cpp matrixDecomposeViewProj) because it renders, widens (aspect) and interpolates the two separately. It takes
// the projection's depth scale p22 from the x row alone: p22 = -vp[0][2] / vp[0][3], where vp[0][3] is the x component of the
// camera's view direction. When the camera looks (nearly) along the world z axis, that component is a few 1/65536 fixed-point
// steps or exactly 0: p22 (and p32, derived from it) come out wrong, so the frame is drawn with the wrong depth range (far
// scenery and backgrounds clipped or shown frame by frame: "popping"); at exactly 0 the split is NaN, RT64 falls back to
// view = identity and files the camera as an orthographic projection, so nothing pairs with the frames around it (a 30 Hz
// step: the jutter going straight down the Hangar start ramp). The real hardware uses the combined matrix as it is.
// Fix without touching RT64: at the command after a perspective projection load (RT64 has executed the load by then, no draw
// has used it yet) the host redoes the split with p22 from the row whose w component is largest (|vp[k][3]| >= 0.577 for a unit
// direction) and writes view / projection into RT64's RSP stack top; the rest is RT64's own formula, so normal frames change
// by rounding only. --hfr-vp-split 0 / --control vpsplit off: RT64's split (A/B).
std::atomic<bool> g_vp_split{true};
bool g_vp_split_task = true;             // g_vp_split for the current gfx task
bool g_vp_pending = false;
float g_vp_m[4][4];
std::atomic<uint64_t> g_vp_loads{0}, g_vp_fixed{0}, g_vp_rt64_flat{0}, g_vp_rt64_off{0};

void vp_split_apply(RT64::State* state) {
    g_vp_pending = false;
    const float(&vp)[4][4] = g_vp_m;
    // RT64's test for a combined matrix (rt64_math.cpp isMatrixViewProj).
    if (!(std::fabs(vp[3][3]) >= 1e-6f && std::fabs(1.0f - vp[3][3]) >= 1e-6f)) return;
    int k = 0;
    for (int r = 1; r < 3; r++)
        if (std::fabs(vp[r][3]) > std::fabs(vp[k][3])) k = r;
    if (std::fabs(vp[k][3]) < 1e-3f) return;
    const double p00 = std::sqrt(double(vp[0][0]) * vp[0][0] + double(vp[1][0]) * vp[1][0] + double(vp[2][0]) * vp[2][0]);
    const double p11 = std::sqrt(double(vp[0][1]) * vp[0][1] + double(vp[1][1]) * vp[1][1] + double(vp[2][1]) * vp[2][1]);
    if (p00 < 1e-6 || p11 < 1e-6) return;
    const double p22 = -double(vp[k][2]) / vp[k][3];
    const double p32 = double(vp[3][2]) + p22 * vp[3][3];
    RT64::RSP& rsp = *state->rsp;
    const int top = rsp.projectionMatrixStackSize - 1;
    hlslpp::float4x4& v = rsp.viewMatrixStack[top];
    hlslpp::float4x4& p = rsp.projMatrixStack[top];
    if (p[3][3] != 0.0f) g_vp_rt64_flat++;   // RT64's split failed (NaN): it filed the camera as orthographic
    else if (std::fabs(float(p[2][2]) - float(p22)) > 0.01 * std::fabs(p22)) g_vp_rt64_off++;   // its depth scale is off by > 1 %
    p = hlslpp::float4x4::identity();
    p[0][0] = float(p00);
    p[1][1] = float(p11);
    p[2][2] = float(p22);
    p[2][3] = -1.0f;
    p[3][2] = float(p32);
    p[3][3] = 0.0f;
    v = hlslpp::float4x4::identity();
    for (int r = 0; r < 4; r++) {
        v[r][0] = float(vp[r][0] / p00);
        v[r][1] = float(vp[r][1] / p11);
        v[r][2] = -vp[r][3];
    }
    g_vp_fixed++;
}

bool camera_from_viewproj(const float m[4][4], Cam& cam) {
    // clip = [x y z 1] * M. The centre has clip x = y = w = 0: solve the 3x3 system.
    const int cols[3] = {0, 1, 3};
    float a[3][3], b[3];
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 3; i++) a[r][i] = m[i][cols[r]];
        b[r] = -m[3][cols[r]];
    }
    float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (std::fabs(det) < 1e-12f) return false;
    for (int k = 0; k < 3; k++) {
        float t[3][3];
        std::memcpy(t, a, sizeof t);
        for (int r = 0; r < 3; r++) t[r][k] = b[r];
        float dk = t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
                   t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0]);
        cam.c[k] = dk / det;
    }
    float n = std::sqrt(m[0][3] * m[0][3] + m[1][3] * m[1][3] + m[2][3] * m[2][3]);
    if (n < 1e-9f) return false;  // orthographic (2D): no camera
    for (int k = 0; k < 3; k++) cam.d[k] = m[k][3] / n;
    cam.ok = true;
    return true;
}

using thps::read_fixed_matrix;

void command_hook(void*, RT64::State* state, uint32_t cmd_address, uint32_t w0, uint32_t w1) {
    if (g_vp_pending) vp_split_apply(state);           // the projection loaded by the previous command (view-projection split)
    thps::hud_command(state, w0, w1);                  // HUD placement (src/hud_anchor.cpp)
    thps::ids_command(state, cmd_address, w0, w1);     // matrix-group IDs (src/interp_ids.cpp)
    if ((w0 >> 24) != 0xDA) return;                    // G_MTX
    uint32_t params = (w0 & 0xFF) ^ 0x01;               // F3DEX2 stores params ^ G_MTX_PUSH
    if (!(params & 0x04) || !(params & 0x02)) return;   // projection load only
    float m[4][4];
    read_fixed_matrix(state->RDRAM, state->rsp->fromSegmentedMasked(w1), m);
    if (projection_kind(m) != 1) {
        g_vp_loads++;
        if (g_vp_split_task) std::memcpy(g_vp_m, m, sizeof m), g_vp_pending = true;
    }
    // Before the cut check below: a cut found at this load overrides the tag with G_EX_ID_IGNORE, and after a cut the task's
    // projections stay ignored.
    if (g_proj_ids_task && g_hfr_enabled && thps::gfx.refresh != 0 && !g_cut_this_task) tag_projection(state, m);
    int k = g_proj_in_task++;
    if (k >= kMaxProj) return;
    Cam cam;
    if (!camera_from_viewproj(m, cam)) return;
    g_cur[k] = cam;
    const Cam& p = g_prev[k];
    if (!p.ok || k != 0) return;                        // judge cuts on the first (main) camera
    float dx = cam.c[0] - p.c[0], dy = cam.c[1] - p.c[1], dz = cam.c[2] - p.c[2];
    float jump = std::sqrt(dx * dx + dy * dy + dz * dz);
    float dot = std::clamp(cam.d[0] * p.d[0] + cam.d[1] * p.d[1] + cam.d[2] * p.d[2], -1.0f, 1.0f);
    float angle = std::acos(dot) * 57.29578f;
    g_last_jump = jump, g_last_angle = angle;
    g_cut_frames_seen++;
    bool cut = (jump > std::max(g_cut_jump_min, g_cut_jump_factor * g_step_avg)) || angle > g_cut_angle_deg;
    if (!cut) {
        g_step_avg = g_step_avg == 0 ? jump : 0.9f * g_step_avg + 0.1f * jump;
        return;
    }
    g_cut_this_task = true;
    g_cut_jump = jump, g_cut_angle = angle;
    thps::ids_cut();
    if (g_hfr_enabled && thps::gfx.refresh != 0) {
        // gEXMatrixGroupNoInterpolate on the projection and on the model stack's base, both without
        // push: the rest of this frame is shown as it is.
        state->rsp->matrixId(G_EX_ID_IGNORE, false, true, false, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
                             G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
                             G_EX_COMPONENT_SKIP, G_EX_ORDER_LINEAR, G_EX_ASPECT_AUTO, G_EX_EDIT_NONE, false, false);
        state->rsp->matrixId(G_EX_ID_IGNORE, false, false, false, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
                             G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
                             G_EX_COMPONENT_SKIP, G_EX_ORDER_LINEAR, G_EX_ASPECT_AUTO, G_EX_EDIT_NONE, false, false);
    }
}

}  // namespace

namespace thps {

RT64::Application* hfr_app() { return g_app.load(); }

void hfr_task_begin(RT64::State* state, uint8_t* rdram, uint32_t dl_address) {
    g_app.store(state->ext.app);
    state->ext.interpreter->commandHook = &command_hook;
    g_proj_in_task = 0;
    g_cut_this_task = false;
    g_vp_pending = false;
    g_proj_ids_task = g_proj_ids.load(std::memory_order_relaxed);
    g_vp_split_task = g_vp_split.load(std::memory_order_relaxed);
    g_proj_id_set = G_EX_ID_AUTO;   // RT64 resets its matrix-group stacks for every task (RSP::clearExtended)
    hud_task_begin(state, rdram, dl_address);
    ids_task_begin(state, rdram, dl_address);
    if (gfx.refresh != 0) {
        // gEXSetRefreshRate(30): the game's own rate, so RT64 doesn't infer it from VI history
        // (which would follow the 60 Hz retraces during loading screens and fades).
        state->setRefreshRate(30);
    }
}

void hfr_task_end(RT64::State* state) {
    hud_task_end(state);
    ids_task_end(state);
    // Only tasks that drew a 3D camera update the reference (2D-only tasks keep the last one).
    if (g_proj_in_task > 0) {
        for (int k = 0; k < kMaxProj; k++) g_prev[k] = k < g_proj_in_task ? g_cur[k] : Cam{};
    }
    if (g_cut_this_task) {
        g_cuts++;
        // Quiet terminal (Q19): logged with --verbose, or --control (the test runner reads it).
        if (options.verbose || options.control) std::printf("[thps] hfr camera cut #%" PRIu64 " (gfx task %" PRIu64 "): moved %.1f (avg step %.1f), turned %.1f deg%s\n",
                    g_cuts.load(), stats.gfx_tasks.load(), g_cut_jump, g_step_avg, g_cut_angle,
                    (g_hfr_enabled && gfx.refresh != 0) ? ": not interpolated" : "");
        if (options.verbose || options.control) std::fflush(stdout);
    }
}

// --hfr-tags 0|1, --hfr-cut-jump FACTOR[:MIN], --hfr-cut-angle DEG (main.cpp passes unknown flags here).
bool hfr_arg(int argc, char** argv, int& i) {
    auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    if (arg("--hfr-tags")) {
        g_hfr_enabled = std::atoi(argv[++i]) != 0;
    } else if (arg("--hfr-cut-jump")) {
        std::string v = argv[++i];
        g_cut_jump_factor = float(std::atof(v.c_str()));
        size_t c = v.find(':');
        if (c != std::string::npos) g_cut_jump_min = float(std::atof(v.c_str() + c + 1));
    } else if (arg("--hfr-proj-ids")) {
        g_proj_ids = std::atoi(argv[++i]) != 0;
    } else if (arg("--hfr-vp-split")) {
        g_vp_split = std::atoi(argv[++i]) != 0;
    } else if (arg("--hfr-cut-angle")) {
        g_cut_angle_deg = float(std::atof(argv[++i]));
    } else {
        return ids_arg(argc, argv, i) || hud_arg(argc, argv, i);
    }
    return true;
}

std::string hfr_perf_text() {
    auto& h = RT64::hfrStats;
    static uint64_t l_wl = 0, l_us = 0;
    uint64_t m = h.framesMatched, in = h.framesInterpolating, tf = h.transforms, pa = h.transformsPaired, sk = h.transformsSkipped;
    uint64_t vp = h.viewProjs, vpp = h.viewProjsPaired, di = h.displayFrames, dr = h.displayFramesDropped;
    uint64_t wl = h.workloads, us = h.renderMicros, us_max = h.renderMicrosMax.exchange(0);
    if (wl == l_wl) return {};
    uint64_t dm = m - l_matched, dtf = tf - l_tf, dwl = wl - l_wl;
    char buf[400];
    std::snprintf(buf, sizeof buf,
                  "[thps] hfr render avg %.2f ms max %.2f ms per game frame | matched=%" PRIu64 " interpolating=%" PRIu64
                  " transforms/frame=%.1f paired=%.1f%% skipped=%" PRIu64 " unpaired=%" PRIu64 " camera_paired=%" PRIu64 "/%" PRIu64
                  " rendered=%" PRIu64 " dropped=%" PRIu64,
                  double(us - l_us) / 1000.0 / double(dwl), double(us_max) / 1000.0, dm, in - l_interp,
                  dm ? double(dtf) / double(dm) : 0.0, dtf ? 100.0 * double(pa - l_paired) / double(dtf) : 0.0, sk - l_skip,
                  dtf - (pa - l_paired) - (sk - l_skip), vpp - l_vpp, vp - l_vp, di - l_disp, dr - l_drop);
    l_matched = m, l_interp = in, l_tf = tf, l_paired = pa, l_skip = sk, l_vp = vp, l_vpp = vpp, l_disp = di, l_drop = dr;
    l_wl = wl, l_us = us;
    return buf;
}

// --control commands:
//   dltrace                print every command of the next gfx task (dl_inspect's trace)
//   pcap EVERY [DIR]       present captures every EVERY composited frames (0 = off) into DIR
//                          (default: --dump-dir); PNGs at --present-size
//   hud                    HUD placement counters (src/hud_anchor.cpp)
//   hfr                    the cumulative pairing counters
//   projids [on|off]       the projection-kind IDs (switch for A/B) and how many camera / shadow loads were tagged
//   vpsplit [on|off]       the host's view-projection split (switch for A/B) and its counters
bool hfr_control(const std::string& cmd, std::istringstream& in) {
    if (cmd == "hud") {
        reply("ok hud " + hud_report());
        return true;
    }
    if (cmd == "projids") {   // projids [on|off]: the projection-kind IDs (above), and how many loads were tagged
        std::string v;
        in >> v;
        if (v == "on" || v == "off") g_proj_ids = v == "on";
        reply("ok projids state=" + std::string(g_proj_ids.load() ? "on" : "off") + " camera=" + std::to_string(g_proj_tags_camera.load()) +
              " shadow=" + std::to_string(g_proj_tags_shadow.load()));
        return true;
    }
    if (cmd == "vpsplit") {   // vpsplit [on|off]: the host's view-projection split (above); loads seen, split, and RT64 NaN fallbacks replaced
        std::string v;
        in >> v;
        if (v == "on" || v == "off") g_vp_split = v == "on";
        reply("ok vpsplit state=" + std::string(g_vp_split.load() ? "on" : "off") + " loads=" + std::to_string(g_vp_loads.load()) +
              " split=" + std::to_string(g_vp_fixed.load()) + " rt64_flat=" + std::to_string(g_vp_rt64_flat.load()) + " rt64_off=" + std::to_string(g_vp_rt64_off.load()));
        return true;
    }
    if (cmd == "dltrace") {
        int t = int(stats.gfx_tasks.load()) + 2;
        options.dl_trace = t;
        reply("ok dltrace task=" + std::to_string(t));
        return true;
    }
    if (cmd == "pcap") {
        int every = 0;
        std::string dir;
        in >> every;
        std::getline(in >> std::ws, dir);
        RT64::Application* app = g_app.load();
        if (!app) { reply("err pcap no renderer yet"); return true; }
        if (!dir.empty()) {
            std::filesystem::create_directories(dir);
            options.dump_dir = dir;
        }
        if (every > 0 && options.dump_dir.empty()) { reply("err pcap no directory (pcap EVERY DIR or --dump-dir)"); return true; }
        app->presentQueue->captureEvery = uint32_t(std::max(every, 0));
        char buf[200];
        std::snprintf(buf, sizeof buf, "ok pcap every=%d composited=%" PRIu64, every, uint64_t(app->presentQueue->presentedFrameCount.load()));
        reply(buf);
        return true;
    }
    if (cmd == "hfrdbg") {
        // The next matched frame's world transforms: address + P (paired) / S (skipped) / U (unpaired),
        // run-length compressed over consecutive matrices of a 0x40 stride.
        auto& h = RT64::hfrStats;
        h.collect = true;
        for (int i = 0; i < 200 && h.collect.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::vector<uint64_t> v;
        {
            std::lock_guard lock{h.collectedMutex};
            v = h.collected;
        }
        std::string out = "ok hfrdbg n=" + std::to_string(v.size());
        char buf[64];
        auto kind = [](uint64_t e) { return (e & 2) ? 'S' : (e & 1) ? 'P' : 'U'; };
        for (size_t i = 0; i < v.size();) {
            uint32_t a = uint32_t(v[i] >> 2);
            char c = kind(v[i]);
            size_t j = i + 1;
            while (j < v.size() && uint32_t(v[j] >> 2) == a + 0x40 * (j - i) && kind(v[j]) == c) j++;
            std::snprintf(buf, sizeof buf, " %06X%s%c", a, j - i > 1 ? ("x" + std::to_string(j - i)).c_str() : "", c);
            out += buf;
            i = j;
        }
        reply(out);
        return true;
    }
    if (cmd == "hfrcam") {
        char buf[300];
        std::snprintf(buf, sizeof buf, "ok hfrcam cuts=%" PRIu64 " frames=%" PRIu64 " last_jump=%.2f last_angle=%.2f avg_step=%.2f cam=%.1f,%.1f,%.1f dir=%.3f,%.3f,%.3f",
                      g_cuts.load(), g_cut_frames_seen.load(), g_last_jump.load(), g_last_angle.load(), g_step_avg, g_prev[0].c[0],
                      g_prev[0].c[1], g_prev[0].c[2], g_prev[0].d[0], g_prev[0].d[1], g_prev[0].d[2]);
        reply(buf);
        return true;
    }
    if (cmd == "hfr") {
        auto& h = RT64::hfrStats;
        char buf[400];
        std::snprintf(buf, sizeof buf,
                      "ok hfr matched=%" PRIu64 " interpolating=%" PRIu64 " transforms=%" PRIu64 " paired=%" PRIu64 " skipped=%" PRIu64
                      " viewprojs=%" PRIu64 " camera_paired=%" PRIu64 " rendered=%" PRIu64 " dropped=%" PRIu64 " workloads=%" PRIu64 " render_us=%" PRIu64 " refresh=%d",
                      h.framesMatched.load(), h.framesInterpolating.load(), h.transforms.load(), h.transformsPaired.load(),
                      h.transformsSkipped.load(), h.viewProjs.load(), h.viewProjsPaired.load(), h.displayFrames.load(),
                      h.displayFramesDropped.load(), h.workloads.load(), h.renderMicros.load(), gfx.refresh);
        reply(buf);
        return true;
    }
    return ids_control(cmd, in) || frametime_control(cmd, in) || latency_control(cmd, in);
}

}  // namespace thps
