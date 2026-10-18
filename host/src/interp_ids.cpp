// Stable matrix-group IDs for RT64's frame interpolation (shared by THPS1-3; the per-game part is data in game.toml).
//
// Why: RT64 pairs each transform of a game frame with its counterpart in the previous frame so it can interpolate the
// in-between frames. Without IDs ("G_EX_ID_AUTO") it searches for the counterpart among the draw calls whose state and
// triangle count match and takes the closest ones; whatever it cannot pair is shown at the game's 30 Hz while the rest moves
// at 60 Hz (skater jitter, camera that alternates between smooth and stepped). With an ID on a transform
// (gEXMatrixGroup, RT64's extended GBI) the counterpart is the transform with the same ID in the previous frame: no search,
// no thresholds. Zelda64Recomp / Starfox64Recomp tag every actor and skeleton limb that way from patched game code.
//
// Here the tags are made by the host instead of patched game code: the display list is walked before RT64 reads it
// (the same place dl_fixes.cpp works in), each model-view matrix load gets an ID by a per-game rule, and the ID is set
// through RSP::matrixId when RT64 reaches that G_MTX (Interpreter::commandHook), exactly what a gEXMatrixGroup command
// in the list would do. No game data: addresses and matrices read at run time.
//
// This file: (1) the display-list scanner and its dump (--control idsdump), the evidence tool for finding why transforms
// do not pair; (2) the ID rules (--interp-ids).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__APPLE__) || defined(__linux__)
#include <time.h>
#endif
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hle/rt64_hfr_stats.h"
#include "hle/rt64_interpreter.h"
#include "hle/rt64_rsp.h"
#include "hle/rt64_state.h"

#include "hfr.h"
#include "host.h"

namespace thps {

void read_fixed_matrix(const uint8_t* rdram, uint32_t addr, float m[4][4]) {
    // N64 Mtx: 16 s16 integer parts, then 16 u16 fractions, row-major; RDRAM is stored as
    // native-endian 32-bit words, so the two halves of each word are swapped (j ^ 1).
    const auto* w = reinterpret_cast<const uint16_t*>(rdram + (addr & 0x7FFFFC));
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            int k = i * 4 + (j ^ 1);
            int32_t v = int32_t((uint32_t(uint16_t(w[k])) << 16) | uint16_t(w[16 + k]));
            m[i][j] = float(v) / 65536.0f;
        }
    }
}

}  // namespace thps

namespace {

constexpr int kRing = 12;

// One model-view matrix load as the walker saw it.
struct MtxRec {
    uint32_t seq = 0;        // order among the task's G_MTX commands
    uint32_t cmd = 0;        // RDRAM address of the G_MTX command
    uint32_t mtx = 0;        // RDRAM address of the matrix (segment resolved)
    uint32_t parent = 0;     // start address of the display list the command is in
    uint8_t params = 0;
    uint32_t proj = 0;       // projection matrix that was current
    uint32_t mh = 0;         // hash of the matrix words (equal in two frames = the object did not move)
    float t[3] = {0, 0, 0};  // translation (row 3)
    uint32_t vtx = 0;        // vertices loaded while this matrix was current
    uint32_t tris = 0;       // triangles drawn
    uint32_t bursts = 0;     // runs of triangle commands separated by other commands
    uint32_t statehash = 0;  // combiner / other mode / geometry mode words at the first triangle
    uint32_t sig = 0;        // draw shape: vertices, triangles, bursts and state hash (tier 2 key); set when the walk is done
    bool created = false;    // a vertex load followed: RT64 makes a world transform for it
    uint32_t xf = 0;         // index among the created transforms
};

// One projection (view * projection) load.
struct ProjRec {
    uint32_t cmd = 0, mtx = 0;
    uint8_t params = 0;
    float m[4][4] = {};
    float c[3] = {0, 0, 0}, d[3] = {0, 0, 1};   // camera centre and view direction (the gradient of clip w) when it can be solved
    bool cam = false;
    uint8_t kind = 1;          // 0 = perspective camera, 1 = orthographic (2D / HUD), 2 = singular (the planar-shadow matrix: y row zero)
    uint32_t first_xf = 0;     // index of the first world transform made after this load
};

struct TaskScan {
    std::vector<ProjRec> proj;
    std::vector<uint32_t> mtx_cmds;    // RDRAM address of every model-view G_MTX, in walk order (the hook counts them to find its record)
    std::vector<int32_t> mtx_xf;       // ...and the first world transform made under it (index into xf; -1 = none)
    uint64_t task = 0;
    uint32_t dl_address = 0;
    std::vector<MtxRec> mtx;           // every modelview G_MTX, in walk order
    std::vector<uint32_t> xf;          // indices into mtx of the ones that create a world transform
    int projs = 0;
    bool truncated = false;
    uint32_t cmds = 0;
};

std::mutex g_ring_mutex;
TaskScan g_ring[kRing];
int g_ring_head = 0;
std::atomic<bool> g_scan_enabled{false};   // walk every task (cost: ~1 ms); on for --control idsdump and for the ID rules

uint32_t hash32(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }


// clip = [x y z 1] * M. The camera centre has clip x = y = w = 0: solve the 3x3 system. false = orthographic (2D): no camera.
// Returns 0 = camera solved, 1 = orthographic, 2 = singular.
int camera_from_viewproj(const float m[4][4], float c[3], float d[3]) {
    const int cols[3] = {0, 1, 3};
    float a[3][3], b[3];
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 3; i++) a[r][i] = m[i][cols[r]];
        b[r] = -m[3][cols[r]];
    }
    auto det3 = [](const float t[3][3]) {
        return t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) + t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0]);
    };
    float det = det3(a);
    if (std::fabs(det) < 1e-12f) return std::sqrt(m[0][3] * m[0][3] + m[1][3] * m[1][3] + m[2][3] * m[2][3]) < 1e-9f ? 1 : 2;
    for (int k = 0; k < 3; k++) {
        float t[3][3];
        std::memcpy(t, a, sizeof t);
        for (int r = 0; r < 3; r++) t[r][k] = b[r];
        c[k] = det3(t) / det;
    }
    float n = std::sqrt(m[0][3] * m[0][3] + m[1][3] * m[1][3] + m[2][3] * m[2][3]);
    if (n < 1e-9f) return 1;
    for (int k = 0; k < 3; k++) d[k] = m[k][3] / n;
    return 0;
}

// Walk the task's display list from `dl_address` the way RT64 will (G_DL calls and branches, segment table).
void scan_task(const uint8_t* rdram, uint32_t dl_address, TaskScan& out) {
    out.mtx.clear();
    out.xf.clear();
    out.proj.clear();
    out.mtx_cmds.clear();
    out.mtx_xf.clear();
    out.projs = 0;
    out.truncated = false;
    out.dl_address = dl_address;
    auto word = [&](uint32_t a) { return *reinterpret_cast<const uint32_t*>(rdram + (a & 0x7FFFFC)); };
    uint32_t seg[16] = {0};
    auto resolve = [&](uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0xFFFFFF; };
    struct Frame { uint32_t ret; uint32_t start; };
    std::vector<Frame> stack;
    uint32_t pc = dl_address & 0xFFFFFF, cur_start = pc;
    int cur = -1;                  // record of the latest world transform (index into out.mtx)
    bool changed = false;          // a matrix was loaded since the last vertex load: RT64 makes a new world transform
    uint32_t cur_mtx = 0, cur_cmd = 0, cur_seq = 0xFFFFFFFFu, cur_proj = 0;   // the model-view matrix that is current (0 = none yet: identity)
    uint8_t cur_params = 0;
    uint32_t cur_mh = 0;
    float cur_t[3] = {0, 0, 0};
    bool in_burst = false;
    uint32_t cc0 = 0, cc1 = 0, om_h = 0, om_l = 0, geom = 0;
    uint32_t mtx_count = 0;
    for (uint32_t n = 0; n < 400000; n++) {
        if (pc >= 0x800000) { out.truncated = true; return; }
        const uint32_t w0 = word(pc), w1 = word(pc + 4);
        const uint32_t cmd_addr = pc;
        pc += 8;
        out.cmds = n;
        const uint32_t op = w0 >> 24;
        bool burst_cmd = false;
        switch (op) {
            case 0xDA: {   // G_MTX
                uint32_t params = (w0 & 0xFF) ^ 0x01;
                changed = true;
                in_burst = false;
                if (params & 0x04) {
                    out.projs++;
                    cur_proj = resolve(w1);
                    ProjRec pr;
                    pr.cmd = cmd_addr, pr.mtx = cur_proj, pr.params = uint8_t(params);
                    thps::read_fixed_matrix(rdram, cur_proj, pr.m);
                    pr.kind = uint8_t(camera_from_viewproj(pr.m, pr.c, pr.d));
                    pr.cam = pr.kind == 0;
                    pr.first_xf = uint32_t(out.xf.size());
                    out.proj.push_back(pr);
                    break;
                }
                cur_mtx = resolve(w1);
                cur_cmd = cmd_addr;
                cur_seq = mtx_count++;
                out.mtx_cmds.push_back(cmd_addr);
                out.mtx_xf.push_back(-1);
                cur_params = uint8_t(params);
                float m[4][4];
                thps::read_fixed_matrix(rdram, cur_mtx, m);
                cur_t[0] = m[3][0], cur_t[1] = m[3][1], cur_t[2] = m[3][2];
                cur_mh = 2166136261u;
                for (int k = 0; k < 16; k++) cur_mh = hash32(cur_mh, word(cur_mtx + 4 * k));
                break;
            }
            case 0x01: {   // G_VTX (F3DEX2): count in bits 12..19
                if (changed || cur < 0) {
                    MtxRec r;
                    r.seq = cur_seq;
                    r.cmd = cur_cmd;
                    r.mtx = cur_mtx;
                    r.parent = cur_start;
                    r.params = cur_params;
                    r.proj = cur_proj;
                    r.mh = cur_mh;
                    r.t[0] = cur_t[0], r.t[1] = cur_t[1], r.t[2] = cur_t[2];
                    r.created = true;
                    r.xf = uint32_t(out.xf.size());
                    if (cur_seq != 0xFFFFFFFFu && out.mtx_xf[cur_seq] < 0) out.mtx_xf[cur_seq] = int32_t(out.xf.size());
                    out.xf.push_back(uint32_t(out.mtx.size()));
                    out.mtx.push_back(r);
                    cur = int(out.mtx.size()) - 1;
                    changed = false;
                }
                out.mtx[cur].vtx += (w0 >> 12) & 0xFF;
                break;
            }
            case 0x05: case 0x06: case 0x07: {   // G_TRI1 / G_TRI2 / G_QUAD
                burst_cmd = true;
                if (cur >= 0) {
                    MtxRec& r = out.mtx[cur];
                    r.tris += op == 0x05 ? 1 : 2;
                    if (!in_burst) {
                        if (r.bursts == 0) r.statehash = hash32(hash32(hash32(hash32(hash32(2166136261u, cc0), cc1), om_h), om_l), geom);
                        r.bursts++;
                    }
                }
                in_burst = true;
                break;
            }
            case 0xFC: cc0 = w0, cc1 = w1; break;
            case 0xE3: om_h = w0 ^ (w1 * 7); break;
            case 0xE2: om_l = w0 ^ (w1 * 7); break;
            case 0xD9: geom = w0 ^ (w1 * 5); break;
            case 0xDB:   // G_MOVEWORD: segment table
                if (((w0 >> 16) & 0xFF) == 6) seg[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0xFFFFFF;
                break;
            case 0xDE:   // G_DL (push unless it is a branch)
                if (((w0 >> 16) & 0xFF) == 0) {
                    if (stack.size() > 18) { out.truncated = true; return; }
                    stack.push_back({pc, cur_start});
                }
                pc = resolve(w1);
                cur_start = pc;
                break;
            case 0xDF:   // G_ENDDL
                if (stack.empty()) return;
                pc = stack.back().ret;
                cur_start = stack.back().start;
                stack.pop_back();
                break;
            default: break;
        }
        if (!burst_cmd && op != 0x01 && op != 0xDA && op != 0xDE && op != 0xDF && op != 0xDB && op != 0xE7 && op != 0xE6 && op != 0xE9) in_burst = false;
    }
    out.truncated = true;
}

// Draw shape of each world transform (tier 2 key).
void finish_scan(TaskScan& t) {
    for (MtxRec& r : t.mtx) r.sig = hash32(hash32(hash32(hash32(2166136261u, r.vtx), r.tris), r.bursts), r.statehash);
}

// Per-task walk result for the ID rules.
void record_task(const TaskScan& t) {
    std::lock_guard lock{g_ring_mutex};
    g_ring[g_ring_head] = t;
    g_ring_head = (g_ring_head + 1) % kRing;
}

TaskScan g_cur;

// RT64's verdicts for the next matched frame, joined with the walk of the task that made it. false = no frame / no matching task.
// `prev` = the walk of the task before it (for "did this matrix change since the last frame").
struct Sample {
    std::vector<uint64_t> v;   // RT64: (address << 2) | paired | skipped << 1, one per world transform (default identity dropped)
    TaskScan cur, prev;
    bool has_prev = false;
    std::string why;
    std::string calls;         // RT64's call / candidate tables of this frame (RT64 clone with HFR_STATS_HAS_CALLS only)
};

bool sample_frame(Sample& smp) {
    g_scan_enabled = true;
    auto& h = RT64::hfrStats;
    h.collect = true;
    for (int i = 0; i < 300 && h.collect.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    {
        std::lock_guard lock{h.collectedMutex};
        smp.v = h.collected;
#ifdef HFR_STATS_HAS_CALLS
        smp.calls = h.collectedCalls;
        h.collectedCalls.clear();
#endif
    }
    if (smp.v.empty()) { smp.why = "no matched frame (needs --refresh above original)"; return false; }
    smp.v.erase(smp.v.begin());   // RT64's workload starts with a default identity transform (rt64_workload.cpp): not a G_VTX-made one
    std::vector<TaskScan> all;
    {
        std::lock_guard lock{g_ring_mutex};
        for (int k = 0; k < kRing; k++) all.push_back(g_ring[(g_ring_head + k) % kRing]);   // oldest first
    }
    int hit = -1;
    for (size_t i = 0; i < all.size(); i++) {
        const TaskScan& t = all[i];
        if (t.xf.size() != smp.v.size()) continue;
        bool same = true;
        for (size_t j = 0; j < smp.v.size() && same; j++) same = (t.mtx[t.xf[j]].mtx & 0x7FFFF8) == (uint32_t(smp.v[j] >> 2) & 0x7FFFF8);
        if (same) hit = int(i);
    }
    if (hit < 0) { smp.why = "no ring task matches RT64's " + std::to_string(smp.v.size()) + " transforms"; return false; }
    smp.cur = all[hit];
    smp.has_prev = hit > 0 && all[hit - 1].task + 1 == smp.cur.task;
    if (smp.has_prev) smp.prev = all[hit - 1];
    return true;
}

// ---- the ID rule -------------------------------------------------------------------------------------------------------
// Object identity from the display list itself: a model piece is drawn by a display list that stays at the same address from frame to
// frame (the vertex load that follows the G_MTX is in it), and the matrix says where it is. So a world transform is the same object as
// the transform of the previous game frame that was loaded by the same list (`parent`) and is nearest to where it should be (previous
// position + previous step). The matrix's own address does not work: the games allocate matrices from per-frame pools that alternate
// (measured: never the same address in consecutive frames for anything that moves). Equal ID = RT64 pairs them, whatever the draw order,
// triangle counts or scene pairing.
// Safety: a match needs the same parent list, a distance under max_step from the predicted position, and no near-tie with another
// candidate (clusters of identical pieces stay with RT64's own search). Objects whose list address changes every frame (the task's own
// list, particles) never match anything and stay untagged (RT64 pairs them as before).
struct Track {
    uint32_t id = 0, parent = 0, mh = 0, sig = 0;
    float t[3] = {0, 0, 0}, v[3] = {0, 0, 0};
};

std::atomic<bool> g_ids_on{true};            // --interp-ids off / control "ids off" for A/B
std::atomic<float> g_max_step{thps::game::interp_ids_max_step};   // world units a piece may move between two game frames and still be the same object (game.toml)
std::atomic<bool> g_reset_tracks{false};     // control "ids reset": the gfx thread clears the tracks at the next task
std::vector<Track> g_tracks;                 // previous 3D task (gfx thread only)
uint32_t g_next_id = 0x100;
std::vector<uint32_t> g_mtx_ids;             // per model-view G_MTX of the task being interpreted (G_EX_ID_AUTO = leave to RT64)
std::vector<uint32_t> g_mtx_cmds;            // ...and where each one is (the hook checks it is at the right place)
size_t g_mtx_cursor = 0;
bool g_mtx_abort = false, g_last_tagged = false;
bool g_task_cut = false;
std::atomic<uint64_t> g_budget_ns{600000};     // thread-CPU budget of the tracker per task; the rest of a task past it is left to RT64
std::atomic<bool> g_naive{false};            // selftest only: the first version's search (linear exact pass, all pairs), to show its worst case
std::atomic<bool> g_tier2{true};             // control "ids tier2 on|off": second tier (draw shape) for pieces from lists that do not repeat

std::atomic<uint64_t> g_walk_ns{0}, g_walk_max_ns{0}, g_hook_ns{0}, g_timed_tasks{0};
std::atomic<uint64_t> g_cpu_ns{0}, g_cpu_max_ns{0};
// The thread's own CPU time: separates a walk that cost CPU from one that was preempted (wall time without CPU time). 0 where there is no such clock
// (Windows: GetThreadTimes ticks at about 15.6 ms, QueryThreadCycleTime counts cycles, not ns). THPS_IDS_WALL_BUDGET: test hook, the Windows path on macOS.
#if (defined(__APPLE__) || defined(__linux__)) && !defined(THPS_IDS_WALL_BUDGET)
constexpr bool kThreadCpuClock = true;
#else
constexpr bool kThreadCpuClock = false;
#endif
uint64_t thread_cpu_ns() {
#if (defined(__APPLE__) || defined(__linux__)) && !defined(THPS_IDS_WALL_BUDGET)
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
    return 0;
}
// The tracker's budget clock: thread CPU time, or wall time where there is none. Wall time also counts preemption (a loaded machine: walks of 0.4 ms CPU
// measured at 16 ms wall), so the budget is kWallBudgetScale times larger there: above the largest real walk (1.2 ms wall), still a small part of a frame.
constexpr uint64_t kWallBudgetScale = 3;
uint64_t budget_clock_ns() {
    if (kThreadCpuClock) return thread_cpu_ns();
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
// One task's walk, kept for the histogram and the slowest-ten list (control "ids slow").
struct WalkRec {
    uint64_t task = 0, wall_ns = 0, cpu_ns = 0, scan_wall_ns = 0, scan_cpu_ns = 0;
    uint32_t xf = 0, cmds = 0, mtx = 0, projs = 0, tracks = 0, tier2_cand = 0;
    bool cut = false, tagging = false, truncated = false;
};
constexpr int kBuckets = 8;   // < 0.25, 0.5, 1, 2, 4, 8, 16 ms, more
const double kBucketMs[kBuckets - 1] = {0.25, 0.5, 1, 2, 4, 8, 16};
std::mutex g_walk_mutex;
std::vector<WalkRec> g_slow;                      // the ten slowest by wall time
uint64_t g_hist_wall[kBuckets] = {}, g_hist_cpu[kBuckets] = {};
WalkRec g_walk_cur;                               // being measured (gfx thread)
uint32_t g_tier2_cand_cur = 0;
int bucket_of(uint64_t ns) { int b = 0; while (b < kBuckets - 1 && double(ns) / 1e6 >= kBucketMs[b]) b++; return b; }
struct IdStats {
    std::atomic<uint64_t> tasks{0}, mtx{0}, tagged{0}, matched{0}, matched2{0}, bailed{0}, fresh_far{0}, ambiguous{0}, untracked{0}, mismatches{0}, cuts{0};
} g_idstats;

float speed(const Track& k) { return std::sqrt(k.v[0] * k.v[0] + k.v[1] * k.v[1] + k.v[2] * k.v[2]); }

// The matching proper: `tracks` (the previous 3D task's) become this task's, `mtx_ids` (per model-view G_MTX) get the IDs. Bounded work: the cost is
// about linear in the number of transforms (hashed exact pass, a spatial grid for the nearest search of big groups), and after `budget_ns` of this thread's
// CPU time (wall time x kWallBudgetScale where there is no thread CPU clock) the rest of the task is left to RT64's own search (counted as `bailed`).
void match_task(const TaskScan& t, std::vector<Track>& tracks, std::vector<uint32_t>& mtx_ids, bool count, uint64_t budget_ns) {
    const uint64_t c_start = budget_clock_ns();
    if (!kThreadCpuClock) budget_ns *= kWallBudgetScale;
    bool over_budget = false;
    auto check_budget = [&]() {
        if (!over_budget && budget_ns && c_start && budget_clock_ns() - c_start > budget_ns) over_budget = true;
        return over_budget;
    };
    const float max_step = g_max_step.load();
    const bool tier2 = g_tier2.load();
    const size_t n = t.xf.size();
    std::vector<int> match(n, -1);
    std::vector<char> used(tracks.size(), 0), ambiguous(n, 0), has_key(n, 0), tier2_cand(n, 0), unresolved(n, 0);
    auto dist_to = [&](size_t j, int p) {
        const MtxRec& r = t.mtx[t.xf[j]];
        const Track& k = tracks[p];
        const float dx = r.t[0] - (k.t[0] + k.v[0]), dy = r.t[1] - (k.t[1] + k.v[1]), dz = r.t[2] - (k.t[2] + k.v[2]);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    // Match the transforms `js_all` of this task (walk order) to the unused previous tracks `ps_all` (their order), all of one key (a parent list in
    // tier 1, a draw shape in tier 2). `one_max`: the largest distance for a one-to-one match; `cap`: the cap of the per-piece gate.
    auto resolve = [&](const std::vector<int>& js_all, const std::vector<int>& ps_all, float one_max, float cap) {
        // 1. The same matrix as last frame: the piece did not move (level geometry, parked props). Exact, whatever the order.
        std::vector<int> js, ps;
        if (g_naive.load() || js_all.size() * ps_all.size() <= 64) {
            for (int j : js_all) {
                const uint32_t mh = t.mtx[t.xf[j]].mh;
                bool found = false;
                for (int p : ps_all) {
                    if (used[p] || tracks[p].mh != mh) continue;
                    match[j] = p;
                    used[p] = 1;
                    found = true;
                    break;
                }
                if (!found) js.push_back(j);
            }
        } else {   // hashed: matrix hash -> previous tracks (in order)
            std::unordered_map<uint32_t, std::vector<int>> by_mh;
            by_mh.reserve(ps_all.size());
            for (int p : ps_all) by_mh[tracks[p].mh].push_back(p);
            std::unordered_map<uint32_t, size_t> next_of;
            for (int j : js_all) {
                const uint32_t mh = t.mtx[t.xf[j]].mh;
                auto it = by_mh.find(mh);
                bool found = false;
                if (it != by_mh.end()) {
                    size_t& c = next_of[mh];
                    if (c < it->second.size()) { const int p = it->second[c++]; match[j] = p; used[p] = 1; found = true; }
                }
                if (!found) js.push_back(j);
            }
        }
        for (int p : ps_all) if (!used[p]) ps.push_back(p);
        if (js.empty() || ps.empty()) return;
        if (check_budget()) { for (int j : js) unresolved[j] = 1; return; }
        // 2. What is left, moving pieces: one and one is the same object (a vehicle, a pedestrian); the same count, the k-th is the k-th (a model's
        // pieces are issued in the same order every frame and sit a few units apart: nearest alone confuses them), when each is near where it should
        // be; otherwise nearest to the predicted position, only when clearly nearer than the runner-up.
        auto gate = [&](int p) { return std::min(cap, std::max(12.0f, 4.0f * speed(tracks[p]))); };
        if (js.size() == 1 && ps.size() == 1) {
            if (dist_to(js[0], ps[0]) <= one_max) { match[js[0]] = ps[0]; used[ps[0]] = 1; }
            return;
        }
        if (js.size() == ps.size()) {
            bool ok = true;
            for (size_t k = 0; k < js.size() && ok; k++) ok = dist_to(js[k], ps[k]) <= gate(ps[k]);
            if (ok) {
                for (size_t k = 0; k < js.size(); k++) { match[js[k]] = ps[k]; used[ps[k]] = 1; }
                return;
            }
        }
        struct Pair { float d; int j, p; };
        std::vector<Pair> pairs;
        std::unordered_map<int, std::pair<float, float>> best;   // per cur: nearest, runner-up (among the candidates looked at)
        const float cell = std::max(cap, 1.0f);   // no match is farther than `cap`: only the 27 cells around a piece are looked at when the group is big
        const bool use_grid = !g_naive.load() && js.size() * ps.size() > 1024;
        auto key = [&](float x, float y, float z) {
            auto q = [&](float v) { return int64_t(std::floor(v / cell)) & 0x1FFFFF; };
            return uint64_t(q(x)) | (uint64_t(q(y)) << 21) | (uint64_t(q(z)) << 42);
        };
        std::unordered_map<uint64_t, std::vector<int>> grid;
        if (use_grid) {
            grid.reserve(ps.size() * 2);
            for (int p : ps) grid[key(tracks[p].t[0] + tracks[p].v[0], tracks[p].t[1] + tracks[p].v[1], tracks[p].t[2] + tracks[p].v[2])].push_back(p);
        }
        auto consider = [&](int j, int p, float& b1, float& b2) {
            const float d = dist_to(j, p);
            pairs.push_back({d, j, p});
            if (d < b1) b2 = b1, b1 = d;
            else if (d < b2) b2 = d;
        };
        for (int j : js) {
            float b1 = 1e30f, b2 = 1e30f;
            if (!use_grid) {
                for (int p : ps) consider(j, p, b1, b2);
            } else {
                const MtxRec& r = t.mtx[t.xf[j]];
                const int64_t cx = int64_t(std::floor(r.t[0] / cell)), cy = int64_t(std::floor(r.t[1] / cell)), cz = int64_t(std::floor(r.t[2] / cell));
                for (int64_t dx = -1; dx <= 1; dx++)
                    for (int64_t dy = -1; dy <= 1; dy++)
                        for (int64_t dz = -1; dz <= 1; dz++) {
                            auto it = grid.find(uint64_t(cx + dx & 0x1FFFFF) | (uint64_t(cy + dy & 0x1FFFFF) << 21) | (uint64_t(cz + dz & 0x1FFFFF) << 42));
                            if (it != grid.end()) for (int p : it->second) consider(j, p, b1, b2);
                        }
            }
            best[j] = {b1, b2};
            if ((!g_naive.load() && pairs.size() > 200000) || check_budget()) { for (int jj : js) if (match[jj] < 0) unresolved[jj] = 1; pairs.clear(); return; }
        }
        std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.d < b.d; });
        for (const Pair& pr : pairs) {
            if (match[pr.j] >= 0 || used[pr.p]) continue;
            if (pr.d > gate(pr.p)) continue;
            const auto [b1, b2] = best[pr.j];
            if (b2 < 2.0f * b1 + 0.5f && b2 < 1e29f) { ambiguous[pr.j] = 1; continue; }
            match[pr.j] = pr.p;
            used[pr.p] = 1;
        }
    };
    // Tier 1: by the list that holds the vertex load (model pieces: static lists).
    std::unordered_map<uint32_t, std::vector<int>> by_parent, cur_by_parent;
    for (size_t p = 0; p < tracks.size(); p++) by_parent[tracks[p].parent].push_back(int(p));
    for (size_t j = 0; j < n; j++) cur_by_parent[t.mtx[t.xf[j]].parent].push_back(int(j));
    for (auto& [parent, js] : cur_by_parent) {
        auto it = by_parent.find(parent);
        if (it == by_parent.end()) continue;
        for (int j : js) has_key[j] = 1;
        resolve(js, it->second, max_step, max_step);
    }
    // Tier 2: pieces whose list was not in the previous frame (the task's own list, which alternates between two buffers; sprites, particles, new
    // objects) against the previous frame's tracks that no list of this frame claims, by draw shape: vertices, triangles, bursts, render state.
    // Stricter: half the distance.
    uint32_t cand2 = 0;
    if (tier2 && !over_budget) {
        std::unordered_map<uint32_t, std::vector<int>> js_by_sig, ps_by_sig;
        for (size_t j = 0; j < n; j++)
            if (!has_key[j]) { js_by_sig[t.mtx[t.xf[j]].sig].push_back(int(j)); tier2_cand[j] = 1; cand2++; }
        for (size_t p = 0; p < tracks.size(); p++)
            if (!used[p] && !cur_by_parent.count(tracks[p].parent)) ps_by_sig[tracks[p].sig].push_back(int(p));
        for (auto& [sig, js] : js_by_sig) {
            auto it = ps_by_sig.find(sig);
            if (it != ps_by_sig.end()) resolve(js, it->second, max_step * 0.5f, max_step * 0.5f);
        }
    }
    if (count) g_tier2_cand_cur = cand2;
    std::vector<Track> next;
    next.reserve(n);
    std::vector<uint32_t> xf_id(n, G_EX_ID_AUTO);
    uint64_t nm = 0, nm2 = 0, nfar = 0, namb = 0, nun = 0, nbail = 0;
    for (size_t j = 0; j < n; j++) {
        const MtxRec& r = t.mtx[t.xf[j]];
        Track k;
        k.parent = r.parent;
        k.sig = r.sig;
        k.t[0] = r.t[0], k.t[1] = r.t[1], k.t[2] = r.t[2];
        k.mh = r.mh;
        if (match[j] >= 0) {
            const Track& o = tracks[match[j]];
            k.id = o.id;
            for (int c = 0; c < 3; c++) k.v[c] = r.t[c] - o.t[c];
            xf_id[j] = k.id;
            nm++;
            if (tier2_cand[j]) nm2++;
        } else {
            k.id = g_next_id++;
            if (unresolved[j] || (over_budget && !has_key[j])) nbail++;   // out of budget: RT64's own search
            else if (ambiguous[j]) namb++;                                 // tie with another candidate: RT64's own search
            else if (!has_key[j] && !tier2) nun++;                         // first sight of this list (tier 2 off): RT64's own search, tracked from the next frame
            else { xf_id[j] = k.id; nfar++; }                              // new object / nothing within the gate (a teleport): an ID with no partner
        }
        next.push_back(k);
    }
    if (count) {
        g_idstats.matched += nm, g_idstats.matched2 += nm2, g_idstats.fresh_far += nfar, g_idstats.ambiguous += namb, g_idstats.untracked += nun, g_idstats.bailed += nbail;
    }
    tracks.swap(next);
    mtx_ids.assign(t.mtx_cmds.size(), G_EX_ID_AUTO);
    for (size_t k = 0; k < t.mtx_xf.size(); k++)
        if (t.mtx_xf[k] >= 0) mtx_ids[k] = xf_id[size_t(t.mtx_xf[k])];
}

void assign_ids(const TaskScan& t, bool cut) {
    g_mtx_ids.assign(t.mtx_cmds.size(), G_EX_ID_AUTO);
    g_mtx_cmds = t.mtx_cmds;
    g_mtx_cursor = 0;
    g_mtx_abort = false;
    g_last_tagged = false;
    g_task_cut = cut;
    if (g_reset_tracks.exchange(false)) g_tracks.clear();
    if (t.projs == 0 || t.truncated) return;   // not a 3D task: keep the tracks of the last one
    if (cut) { g_tracks.clear(); return; }
    match_task(t, g_tracks, g_mtx_ids, true, g_naive.load() ? 0 : g_budget_ns.load());
}

}  // namespace

namespace thps {

void ids_task_begin(RT64::State* state, const uint8_t* rdram, uint32_t dl_address) {
    (void)state;
    const bool tagging = g_ids_on.load() && gfx.refresh != 0;   // original refresh: nothing to pair
    if (!g_scan_enabled.load() && !tagging) return;
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = thread_cpu_ns();
    g_cur.task = stats.gfx_tasks.load();
    g_tier2_cand_cur = 0;
    scan_task(rdram, dl_address, g_cur);
    finish_scan(g_cur);
    if (g_scan_enabled.load()) record_task(g_cur);   // the ring (a copy of the whole walk) is for idsdump / idsstat only
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t c1 = thread_cpu_ns();
    if (tagging) {
        g_walk_cur.tracks = uint32_t(g_tracks.size());
        assign_ids(g_cur, false);
        g_idstats.tasks++;
    } else {
        g_mtx_ids.clear();
        g_mtx_cmds.clear();
    }
    const auto t2 = std::chrono::steady_clock::now();
    const uint64_t c2 = thread_cpu_ns();
    const auto ns = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()); };
    const uint64_t wall = ns(t0, t2), cpu = c2 - c0;
    g_walk_ns += wall, g_timed_tasks++, g_cpu_ns += cpu;
    uint64_t m = g_walk_max_ns.load();
    while (wall > m && !g_walk_max_ns.compare_exchange_weak(m, wall)) {}
    m = g_cpu_max_ns.load();
    while (cpu > m && !g_cpu_max_ns.compare_exchange_weak(m, cpu)) {}
    WalkRec& r = g_walk_cur;
    r.task = g_cur.task, r.wall_ns = wall, r.cpu_ns = cpu, r.scan_wall_ns = ns(t0, t1), r.scan_cpu_ns = c1 - c0;
    r.xf = uint32_t(g_cur.xf.size()), r.cmds = g_cur.cmds, r.mtx = uint32_t(g_cur.mtx_cmds.size()), r.projs = uint32_t(g_cur.projs), r.tier2_cand = g_tier2_cand_cur;
    r.cut = false, r.tagging = tagging, r.truncated = g_cur.truncated;
}

const char* ids_budget_clock() { return kThreadCpuClock ? "thread_cpu" : "wall"; }

IdsTiming ids_timing() { return {g_timed_tasks.load(), g_walk_ns.load(), g_walk_max_ns.exchange(0), g_hook_ns.load(), g_cpu_ns.load(), g_cpu_max_ns.exchange(0)}; }

void ids_task_end(RT64::State*) {
    if (g_walk_cur.task != 0) {   // a walk was measured for this task: file it (the camera cut flag is known only now)
        g_walk_cur.cut = g_task_cut;
        std::lock_guard lock{g_walk_mutex};
        g_hist_wall[bucket_of(g_walk_cur.wall_ns)]++;
        g_hist_cpu[bucket_of(g_walk_cur.cpu_ns)]++;
        if (g_slow.size() < 10 || g_walk_cur.wall_ns > g_slow.back().wall_ns) {
            g_slow.push_back(g_walk_cur);
            std::sort(g_slow.begin(), g_slow.end(), [](const WalkRec& a, const WalkRec& b) { return a.wall_ns > b.wall_ns; });
            if (g_slow.size() > 10) g_slow.pop_back();
        }
        g_walk_cur.task = 0;
    }
    g_mtx_ids.clear();
    g_mtx_cmds.clear();
}

// The camera cut detector (interpolation.cpp) found a cut in this task: no IDs for it, and the tracks start over.
void ids_cut() {
    g_task_cut = true;
    g_tracks.clear();
    g_idstats.cuts++;
}

void ids_command(RT64::State* state, uint32_t cmd_address, uint32_t w0, uint32_t w1) {
    (void)w1;
    if ((w0 >> 24) != 0xDA || g_mtx_cmds.empty()) return;
    const uint32_t params = (w0 & 0xFF) ^ 0x01;
    if (params & 0x04) return;   // projection: the camera is mapped by RT64 itself (cuts: interpolation.cpp)
    const auto h0 = std::chrono::steady_clock::now();
    struct HookTimer { std::chrono::steady_clock::time_point t0; ~HookTimer() { g_hook_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()); } } hook_timer{h0};
    const size_t k = g_mtx_cursor++;
    if (g_mtx_abort) return;
    if (k >= g_mtx_cmds.size() || g_mtx_cmds[k] != cmd_address) {   // RT64 walks the list differently from the scan: leave the rest of the task to RT64
        g_mtx_abort = true;
        g_idstats.mismatches++;
        return;
    }
    if (g_task_cut) return;      // the cut handler already tagged the model stack G_EX_ID_IGNORE for the rest of the task
    const uint32_t id = g_mtx_ids[k];
    if (id == G_EX_ID_AUTO && !g_last_tagged) return;   // the stack's top is still the default group
    g_idstats.mtx++;
    if (id == G_EX_ID_AUTO) {
        // back to the default group (what RT64 uses for untagged transforms: decomposed, every component automatic, automatic ordering)
        state->rsp->matrixId(G_EX_ID_AUTO, false, false, true, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO,
                             G_EX_COMPONENT_AUTO, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_ORDER_AUTO,
                             G_EX_ASPECT_AUTO, G_EX_EDIT_NONE, false, false);
        g_last_tagged = false;
    } else {
        // the same group with an ID and linear ordering: paired with the transform that has this ID in the previous game frame
        state->rsp->matrixId(id, false, false, true, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO,
                             G_EX_COMPONENT_AUTO, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_ORDER_LINEAR,
                             G_EX_ASPECT_AUTO, G_EX_EDIT_NONE, false, false);
        g_last_tagged = true;
        g_idstats.tagged++;
    }
}

// --interp-ids on|off, --ids-tier2 on|off, --ids-max-step UNITS
bool ids_arg(int argc, char** argv, int& i) {
    auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    if (arg("--interp-ids")) {
        std::string v = argv[++i];
        g_ids_on = v == "on" || v == "1" || v == "true";
        return true;
    }
    if (arg("--ids-tier2")) {
        std::string v = argv[++i];
        g_tier2 = v == "on" || v == "1" || v == "true";
        return true;
    }
    if (arg("--ids-max-step")) {
        g_max_step = float(std::atof(argv[++i]));
        return true;
    }
    return false;
}

std::string ids_report() {
    char b[700];
    std::snprintf(b, sizeof b, "on=%d max_step=%.0f tasks=%" PRIu64 " mtx_hooks=%" PRIu64 " tagged=%" PRIu64 " matched=%" PRIu64 " fresh_far=%" PRIu64 " matched2=%" PRIu64 " tier2=%d ambiguous=%" PRIu64
                               " bailed=%" PRIu64 " untracked=%" PRIu64 " mismatches=%" PRIu64 " cuts=%" PRIu64 " budget_us=%.0f budget_clock=%s",
                  int(g_ids_on.load()), g_max_step.load(), g_idstats.tasks.load(), g_idstats.mtx.load(), g_idstats.tagged.load(), g_idstats.matched.load(),
                  g_idstats.fresh_far.load(), g_idstats.matched2.load(), int(g_tier2.load()), g_idstats.ambiguous.load(), g_idstats.bailed.load(), g_idstats.untracked.load(), g_idstats.mismatches.load(), g_idstats.cuts.load(),
                  double(g_budget_ns.load() * (kThreadCpuClock ? 1 : kWallBudgetScale)) / 1e3, ids_budget_clock());
    return b;
}

// --control commands:
//   idsscan 0|1         walk every gfx task's display list (needed by the two below; on by itself once one of them has run)
//   idsdump FILE [N]    write the world transforms of the next matched frame with RT64's verdict (P paired / S skipped / U unpaired)
//                       joined with the display-list walk of the task that produced them (matrix address, translation, vertices,
//                       triangles, triangle bursts, state hash, enclosing list); N = also the previous task without verdicts
//   idsstat N [FILE]    pairing statistics over N sampled matched frames. "moving" = a world transform that draws triangles with a
//                       matrix that differs from every matrix of the previous game frame (the only ones whose pairing changes anything
//                       on screen: a static piece looks the same whether it is paired or not); camera = view-projections paired
bool ids_control(const std::string& cmd, std::istringstream& in) {
    auto reply = [](const std::string& s) { std::printf("[ctl] %s\n", s.c_str()); std::fflush(stdout); };
    if (cmd == "ids") {
        std::string sub;
        in >> sub;
        if (sub == "on" || sub == "off") g_ids_on = sub == "on";
        else if (sub == "tier2") { std::string v; in >> v; g_tier2 = v != "off" && v != "0"; }
        else if (sub == "step") { float v = 100; in >> v; g_max_step = v; }
        else if (sub == "reset") { g_reset_tracks = true; }
        else if (sub == "slow") {   // the ten slowest walks (wall time), the histograms
            std::lock_guard lock{g_walk_mutex};
            std::string h = "ok ids slow hist_ms(<.25 <.5 <1 <2 <4 <8 <16 more) wall=";
            char b[300];
            for (int i = 0; i < kBuckets; i++) { std::snprintf(b, sizeof b, "%s%" PRIu64, i ? "/" : "", g_hist_wall[i]); h += b; }
            h += " cpu=";
            for (int i = 0; i < kBuckets; i++) { std::snprintf(b, sizeof b, "%s%" PRIu64, i ? "/" : "", g_hist_cpu[i]); h += b; }
            for (const WalkRec& r : g_slow) {
                std::snprintf(b, sizeof b, " | task=%" PRIu64 " wall=%.0fus cpu=%.0fus scan=%.0f/%.0fus xf=%u cmds=%u mtx=%u projs=%u tracks=%u tier2=%u cut=%d tag=%d trunc=%d", r.task, double(r.wall_ns) / 1e3,
                              double(r.cpu_ns) / 1e3, double(r.scan_wall_ns) / 1e3, double(r.scan_cpu_ns) / 1e3, r.xf, r.cmds, r.mtx, r.projs, r.tracks, r.tier2_cand, int(r.cut), int(r.tagging), int(r.truncated));
                h += b;
            }
            reply(h);
            return true;
        }
        else if (sub == "naive") { std::string v; in >> v; g_naive = v == "on" || v == "1"; }   // test aid: the first version's unbounded search
        else if (sub == "budget") { double us = 600; in >> us; g_budget_ns = uint64_t(us * 1000.0); }
        else if (sub == "selftest") {   // the tracker alone, on synthetic tasks of N transforms (control thread, own state: safe while a game runs); worst cases, no budget
            int n = 2000;
            in >> n;
            std::string out = "ok ids selftest n=" + std::to_string(n);
            auto make = [&](int count, float spread, int parents, float shift, uint32_t seed) {
                TaskScan t;
                t.projs = 1;
                uint32_t x = seed * 2654435761u + 1;
                auto rnd = [&]() { x = x * 1664525u + 1013904223u; return float(x >> 8) / float(1 << 24); };
                for (int i = 0; i < count; i++) {
                    MtxRec r;
                    r.parent = 0x1000 + uint32_t(i % parents) * 0x100;
                    r.t[0] = rnd() * spread + shift, r.t[1] = rnd() * spread, r.t[2] = rnd() * spread;
                    r.mh = hash32(uint32_t(i), uint32_t(shift * 10.0f));
                    r.sig = uint32_t(i % 3);
                    r.vtx = 4, r.tris = 2;
                    r.created = true;
                    r.xf = uint32_t(t.xf.size());
                    t.xf.push_back(uint32_t(t.mtx.size()));
                    t.mtx_xf.push_back(int32_t(t.mtx.size()));
                    t.mtx_cmds.push_back(0x100000 + uint32_t(i) * 8);
                    t.mtx.push_back(r);
                }
                return t;
            };
            struct Case { const char* name; int c0, c1; float spread, shift; int parents; };
            const Case cases[] = {
                {"one_parent_scattered", n, n, 400.0f, 7.0f, 1},     // every piece moved, one list: nearest search over the whole group
                {"one_parent_fewer", n, n - 7, 400.0f, 0.5f, 1},      // counts differ: no ordinal shortcut
                {"tier2_one_shape", n, n, 400.0f, 7.0f, 1000000},     // all lists new: tier 2 by shape (3 shapes)
                {"many_parents", n, n, 400.0f, 1.0f, 50},
                {"static", n, n, 400.0f, 0.0f, 3},
            };
            for (const Case& c : cases) {
                for (int mode = 0; mode < 3; mode++) {   // 0: first version (naive, unbounded), 1: bounded search, no budget, 2: bounded + budget
                    const int budget = mode == 2;
                    g_naive = mode == 0;
                    std::vector<Track> tracks;
                    std::vector<uint32_t> ids;
                    TaskScan a = make(c.c0, c.spread, c.parents, 0.0f, 1), b = make(c.c1, c.spread, c.parents, c.shift, c.shift == 0.0f ? 1 : 2);
                    match_task(a, tracks, ids, false, 0);
                    const auto w0 = std::chrono::steady_clock::now();
                    const uint64_t c0 = thread_cpu_ns();
                    match_task(b, tracks, ids, false, budget ? g_budget_ns.load() : 0);
                    const uint64_t c1 = thread_cpu_ns();
                    char bb[160];
                    std::snprintf(bb, sizeof bb, " | %s/%s wall=%.0fus cpu=%.0fus", c.name, mode == 0 ? "naive" : mode == 1 ? "grid" : "grid+budget", double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - w0).count()) / 1e3, double(c1 - c0) / 1e3);
                    out += bb;
                }
            }
            g_naive = false;
            reply(out);
            return true;
        }
        else if (sub == "slowreset") { std::lock_guard lock{g_walk_mutex}; g_slow.clear(); std::memset(g_hist_wall, 0, sizeof g_hist_wall); std::memset(g_hist_cpu, 0, sizeof g_hist_cpu); }
        reply("ok ids " + ids_report());
        return true;
    }
    if (cmd == "idsscan") {
        int on = 1;
        in >> on;
        g_scan_enabled = on != 0;
        reply("ok idsscan " + std::to_string(on));
        return true;
    }
    if (cmd == "idsstat") {
        int n = 30;
        std::string file;
        in >> n >> file;
        auto& h = RT64::hfrStats;
        const uint64_t vp0 = h.viewProjs, vpp0 = h.viewProjsPaired;
        uint64_t frames = 0, xf = 0, drawing = 0, moving = 0, mov_p = 0, mov_s = 0, all_p = 0, all_s = 0, fails = 0;
        uint64_t worst_unpaired = 0;
        std::ofstream f;
        if (!file.empty()) f.open(file);
        for (int i = 0; i < n; i++) {
            Sample smp;
            if (!sample_frame(smp)) { fails++; continue; }
            if (!smp.has_prev) { fails++; continue; }
            std::unordered_map<uint32_t, int> prev_mh;
            for (auto& r : smp.prev.mtx) prev_mh[r.mh]++;
            uint64_t fm = 0, fp = 0, fu = 0;
            for (size_t j = 0; j < smp.cur.xf.size(); j++) {
                const MtxRec& r = smp.cur.mtx[smp.cur.xf[j]];
                const bool p = smp.v[j] & 1, sk = smp.v[j] & 2;
                xf++;
                all_p += p, all_s += sk;
                if (r.tris == 0) continue;
                drawing++;
                if (prev_mh.count(r.mh)) continue;
                moving++, fm++;
                mov_p += p, mov_s += sk;
                fp += p;
                fu += !p && !sk;
            }
            frames++;
            worst_unpaired = std::max(worst_unpaired, fu);
            if (f) {
                char b[200];
                std::snprintf(b, sizeof b, "task %" PRIu64 " transforms=%zu moving=%" PRIu64 " paired=%" PRIu64 " unpaired=%" PRIu64 "\n", smp.cur.task, smp.cur.xf.size(), fm, fp, fu);
                f << b;
            }
        }
        const uint64_t vp = h.viewProjs - vp0, vpp = h.viewProjsPaired - vpp0;
        char b[400];
        std::snprintf(b, sizeof b,
                      "ok idsstat frames=%" PRIu64 " failed=%" PRIu64 " transforms=%" PRIu64 " drawing=%" PRIu64 " all_paired=%.1f moving=%" PRIu64
                      " moving_paired=%.1f moving_skipped=%" PRIu64 " moving_per_frame=%.1f worst_unpaired=%" PRIu64 " camera_paired=%.1f",
                      frames, fails, xf, drawing, xf ? 100.0 * double(all_p) / double(xf) : 0.0, moving, moving ? 100.0 * double(mov_p) / double(moving) : 0.0, mov_s,
                      frames ? double(moving) / double(frames) : 0.0, worst_unpaired, vp ? 100.0 * double(vpp) / double(vp) : 0.0);
        reply(b);
        return true;
    }
    if (cmd != "idsdump" && cmd != "idscalls") return false;
    const bool want_calls = cmd == "idscalls";   // needs the RT64 clone with call tables (HFR_STATS_HAS_CALLS)
    std::string file;
    int extra = 0;
    in >> file >> extra;
    if (file.empty()) { reply("err idsdump FILE [N]"); return true; }
    Sample smp;
    std::ofstream f(file);
    char buf[300];
    bool ok = false;
    for (int tries = 0; tries < (want_calls ? 30 : 1) && !ok; tries++) {
        ok = sample_frame(smp);
        if (ok && want_calls && smp.calls.empty()) ok = false, smp.why = "RT64 gave no call tables";
    }
    if (!ok) {
        f << "# " << smp.why << "\n";
        reply("err " + cmd + " " + smp.why);
        return true;
    }
    auto dump = [&](const TaskScan& t, const std::vector<uint64_t>* verdict) {
        std::snprintf(buf, sizeof buf, "# task %" PRIu64 " dl=%06X transforms=%zu projs=%d cmds=%u%s\n", t.task, t.dl_address,
                      t.xf.size(), t.projs, t.cmds, t.truncated ? " TRUNCATED" : "");
        f << buf;
        for (size_t k = 0; k < t.proj.size(); k++) {
            const ProjRec& pr = t.proj[k];
            std::snprintf(buf, sizeof buf, "# PROJ %zu cmd=%06X mtx=%06X first_xf=%u cam=%d c=%.2f,%.2f,%.2f d=%.4f,%.4f,%.4f m00=%.4f m11=%.4f m22=%.4f m23=%.4f m32=%.4f m33=%.4f\n", k, pr.cmd, pr.mtx,
                          pr.first_xf, int(pr.cam), pr.c[0], pr.c[1], pr.c[2], pr.d[0], pr.d[1], pr.d[2], pr.m[0][0], pr.m[1][1], pr.m[2][2], pr.m[2][3], pr.m[3][2], pr.m[3][3]);
            f << buf;
            for (int i = 0; i < 4; i++) {
                std::snprintf(buf, sizeof buf, "#    %10.4f %10.4f %10.4f %10.4f\n", pr.m[i][0], pr.m[i][1], pr.m[i][2], pr.m[i][3]);
                f << buf;
            }
        }
        for (size_t j = 0; j < t.xf.size(); j++) {
            const MtxRec& r = t.mtx[t.xf[j]];
            char kind = '-';
            if (verdict) kind = ((*verdict)[j] & 2) ? 'S' : ((*verdict)[j] & 1) ? 'P' : 'U';
            std::snprintf(buf, sizeof buf, "%c %4zu mtx=%06X cmd=%06X dl=%06X t=%9.2f,%9.2f,%9.2f vtx=%4u tris=%4u bursts=%2u st=%08X mh=%08X\n", kind, j, r.mtx, r.cmd,
                          r.parent, r.t[0], r.t[1], r.t[2], r.vtx, r.tris, r.bursts, r.statehash, r.mh);
            f << buf;
        }
    };
    if (extra > 0 && smp.has_prev) dump(smp.prev, nullptr);
    dump(smp.cur, &smp.v);
    if (want_calls) f << "# ---- RT64 call tables of this frame ----\n" << smp.calls;
    unsigned p = 0, s = 0;
    for (auto e : smp.v) { if (e & 2) s++; else if (e & 1) p++; }
    std::snprintf(buf, sizeof buf, "ok %s task=%" PRIu64 " transforms=%zu paired=%u skipped=%u", cmd.c_str(), smp.cur.task, smp.v.size(), p, s);
    reply(buf);
    return true;
}

}  // namespace thps
