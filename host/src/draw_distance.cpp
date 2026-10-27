// Extended draw distance. Off by default.
//
// Setting: graphics.cfg `draw_distance = original | 2x | 4x | full` (--draw-distance, the Graphics tab; default original). The game's
// far plane is multiplied by a factor (2x, 4x, full: out to kFullFarPlane): the game passes it to View_SetFrustum each frame for the gameplay camera, and that one
// value drives the projection's far clip, the game's cull volume and (through the projection) the fog ramp. With `original` nothing is
// changed: the View_SetFrustum hook sees factor 1 and runs the game's body as before.
//
// Which frustum calls are the gameplay camera's, and what the game's display-list and matrix buffers hold, is game-specific
// (game/draw_distance.cpp: dd_game_far, dd_game_measure). This file keeps the setting, the high-water marks and a governor: every
// gfx task reports how full the game's fixed buffers were; above kBackoff of any of them the factor in use drops (at kPanic straight to
// 1x), below kRecover it rises again. So more geometry can never fill a buffer the game cannot grow.
//
// A course start (the game tells us: note_course_start, an explicit count of course set-ups, not a change of the far plane: courses can
// share one) resets the learned ceiling and puts the factor at the setting at once, as does a new setting. The first frames, which the
// governor sees only after they are drawn, are kept safe by the games themselves: the 144 KiB display lists, the passes for the mesh
// lists and THPS2/3's display-list byte budget (game/draw_distance.cpp). (2026-10-23 had a ramp from 1x over about a second instead;
// players found the far scenery filling in after every start irritating, and with the budget it was not needed: removed the same day.)
// After a back-off the factor stays at the lowered ceiling for the course; below the ceiling it rises by kRampStep a frame while every
// buffer is under kRecover. unhide_reach() keeps the zone-culled items within (factor - 1) x the game's far while the factor is below its
// end (only after a back-off lowered the ceiling below the setting it is not: show_all() is off then).
//
// Buffers: graphics.cfg `dd_buffers = bigger | original` (--dd-buffers; default bigger, read at start-up). bigger: the games always set up
// matrix pools of 2048 and keep their own cap in force at `original`, so the setting takes effect at once (game/draw_distance.cpp).
//
// Control (tests): `dd` = stats; `dd mode original|2x|4x|full`; `dd factor X` (exploration: any factor >= 1); `dd reset` (high-water marks);
// `dd testcap PCT` (the governor treats the buffers as PCT % of their size, to exercise it); `dd showall 0|1|all|auto` (zone
// visibility on its own, for A/B tests; `all` = also the items hidden by other commands, i.e. the first version's behaviour, for comparison only);
// `dd stale PCT` / `dd feed PCT` (one feedback sample across a transition / of the current generation: see g_gen); `dd own N` (item N event-owned).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

#include "draw_distance.h"
#include "host.h"

namespace thps::dd {
namespace {

constexpr float kFar = 2.0f, kMax = 4.0f, kFull = 64.0f;   // full: as far as kFullFarPlane (the games clamp to it)
constexpr float kBackoff = 0.85f, kPanic = 0.95f, kRecover = 0.70f;
constexpr float kRampStep = 1.15f;   // recovery below the ceiling, per frame while all buffers are under kRecover

// The default setting is `full` (the Enhanced graphics preset, v0.2: src/gfx_config.cpp); graphics.cfg, --draw-distance and --preset change it.
std::atomic<float> g_target{kFull};    // the setting's factor (or `dd factor`)
std::atomic<float> g_in_use{kFull};    // after the governor: what dd_game_far applies
std::atomic<float> g_in_use_min{64.0f}; // lowest factor the governor went to since `dd reset`
std::atomic<float> g_ceiling{64.0f};    // the governor's learned ceiling for this course: after a back-off it recovers only up to here
std::atomic<float> g_level_far{0.0f};   // the game's own far of the course in play (how far the factor can go: kFullFarPlane / this)
std::atomic<uint64_t> g_course_starts{0};
std::atomic<uint64_t> g_cuts{0}, g_cut_items{0}, g_late_cuts{0};   // World_DrawItems calls that left extra passes out for the display-list budget; their items
std::atomic<uint32_t> g_budget_cap{0}, g_budget_rest{0}, g_budget_pass{0}, g_budget_margin{0};   // `dd dlbudget` (tests; 0 = the game's own)   // note_course_start calls (course set-ups the game reported)
std::mutex g_gov_mutex;   // the governor's state changes as one: a frame's update (note_task), a course start, a new setting
// Feedback generations (review 2026-10-26, finding 6). A gfx task is measured on the gfx thread after the game thread built it, so a task
// built before a new setting or a course start can be measured after it, and its use of the buffers then read as pressure on the new
// setting (a THPS3 frame at Original near its 500 vertices, measured once Full is set, panicked Full to 1x for the course). Every
// transition (set_mode, `dd factor`, note_course_start) bumps g_gen and drops the next kSettleTasks measured tasks (the frames in flight:
// the one RT64 has, the one queued, the one being built); a measurement that saw g_gen change while it ran is dropped too.
constexpr uint32_t kSettleTasks = 3;
std::atomic<uint64_t> g_gen{0};
uint32_t g_settle = 0;                                 // under g_gov_mutex
std::atomic<uint64_t> g_stale_dropped{0}, g_settle_dropped{0};
std::atomic<uint64_t> g_tasks{0}, g_backoffs{0}, g_panics{0}, g_scaled{0}, g_mtx_capped{0};
std::atomic<uint32_t> g_dl_hw{0}, g_dl_cap{0}, g_mtx_hw{0}, g_mtx_cap{0}, g_vtx_hw{0}, g_vtx_cap{0}, g_queue_hw{0}, g_queue_cap{0}, g_blend_hw{0}, g_blend_cap{0}, g_defer_hw{0}, g_defer_cap{0}, g_reset_hw{0}, g_reset_cap{0}, g_runs_hw{0};
std::atomic<float> g_far_seen{0.0f};
std::atomic<float> g_effective{1.0f};  // what the last scaled call really used (far / the game's far): below g_in_use when the full clamp applies
std::atomic<float> g_cap_scale{1.0f};   // tests (`dd testcap PCT`): the governor acts as if the buffers were this much of their size
std::string g_mode = "full";
std::atomic<bool> g_full{true};
std::atomic<bool> g_big_buffers{true};     // dd_buffers (start-up): the games allocate the bigger matrix pools
std::atomic<int> g_showall_override{-1};   // tests (`dd showall 0|1|auto`): -1 = follow the mode
std::mutex g_mode_mutex;

float factor_for(const std::string& m) { return m == "full" ? kFull : m == "4x" ? kMax : m == "2x" ? kFar : 1.0f; }

// The names before the playtest of 2026-10-26: `far` and `max` (renamed because "Max" was less than "Full"). Still read from
// graphics.cfg, --draw-distance and `dd mode` as 2x and 4x; only the new names are written.
std::string canonical(const std::string& m) { return m == "far" ? "2x" : m == "max" ? "4x" : m; }

// A transition of the governor's inputs (g_gov_mutex held): feedback from tasks built before it is stale.
void transition_locked() {
    g_gen.fetch_add(1);
    g_settle = kSettleTasks;
}

void max_into(std::atomic<uint32_t>& a, uint32_t v) {
    uint32_t m = a.load();
    while (v > m && !a.compare_exchange_weak(m, v)) {}
}

}  // namespace

bool valid_mode(const std::string& m_in) {
    const std::string m = canonical(m_in);
    return m == "original" || m == "2x" || m == "4x" || m == "full";
}

void set_mode(const std::string& m_in) {
    if (!valid_mode(m_in)) return;
    const std::string m = canonical(m_in);
    {
        std::lock_guard lock{g_mode_mutex};
        g_mode = m;
    }
    const float f = factor_for(m);
    {
        std::lock_guard lock{g_gov_mutex};
        g_full = m == "full";
        g_target = f;
        g_ceiling = f;
        g_in_use = f;   // at once, either way
        transition_locked();
    }
    static bool logged = false;
    if (f > 1.0f || logged) {
        std::printf("[thps] draw distance: %s (far plane x%.1f%s)\n", m.c_str(), double(f), supported() ? "" : "; this game already draws its distant scenery, so nothing changes");
        std::fflush(stdout);
        logged = true;
    }
}

std::string mode() {
    std::lock_guard lock{g_mode_mutex};
    return g_mode;
}

float factor() { return supported() ? g_in_use.load() : 1.0f; }
float projection_factor() { return supported() ? std::max(1.0f, std::min(g_target.load(), g_ceiling.load())) : 1.0f; }

bool enlarge_pools() { return g_target.load() > 1.0001f; }

bool valid_buffers(const std::string& v) { return v == "bigger" || v == "original"; }
void set_buffers(const std::string& v) {
    if (valid_buffers(v)) g_big_buffers = v == "bigger";
}
std::string buffers() { return g_big_buffers.load() ? "bigger" : "original"; }
bool big_buffers() { return g_big_buffers.load(); }

bool unhide_everything() { return g_showall_override.load() == 2; }

bool show_all() {
    const int o = g_showall_override.load();
    if (o >= 0) return o >= 1;
    return g_full.load() && supported() && g_ceiling.load() >= g_target.load() * 0.999f;   // dropped for the course after any back-off
}

void note_effective(float k) { g_effective = std::max(1.0f, k); }

void note_game_far(float f) { g_level_far = f; }

void note_course_start() {
    std::lock_guard lock{g_gov_mutex};
    g_course_starts.fetch_add(1);
    g_ceiling = g_target.load();   // a new course: the setting again
    g_in_use = std::max(1.0f, g_target.load());
    transition_locked();
}

uint64_t course_generation() { return g_course_starts.load(); }

namespace {
// How far the factor in use can go now: the setting, the course's learned ceiling, and the far plane's own limit (dd_game_far clamps
// the far to kFullFarPlane, so past kFullFarPlane / the game's far a higher factor changes nothing).
float factor_end() {
    const float lf = g_level_far.load();
    const float far_limit = lf > 0.0f ? std::max(1.0f, kFullFarPlane / lf) : kFull;
    return std::min({g_target.load(), g_ceiling.load(), far_limit});
}
}  // namespace

DlBudget dl_budget() { return DlBudget{g_budget_cap.load(), g_budget_rest.load(), g_budget_pass.load(), g_budget_margin.load()}; }

void note_cut(uint32_t items, uint32_t used, uint32_t cap, bool late) {
    const uint64_t n = g_cuts.fetch_add(1);
    g_cut_items.fetch_add(items);
    if (late) g_late_cuts.fetch_add(1);
    if (thps::verbose() && (n < 20 || n % 300 == 0)) {
        std::printf("[thps] draw distance: display list budget: %u extra items left out (%s), %u of %u bytes used before them (cut %" PRIu64 ")\n",
                    items, late ? "LATE: nearer ones, a pass went past its bound" : "the farthest", used, cap, n + 1);
        std::fflush(stdout);
    }
}

float unhide_reach() {
    const float k = g_in_use.load();
    return k >= factor_end() * 0.999f ? 1e30f : std::max(0.0f, k - 1.0f);
}

namespace {
uint32_t rd_word(const uint8_t* rdram, uint32_t vaddr) { return *reinterpret_cast<const uint32_t*>(rdram + (vaddr & 0x3FFFFFC)); }
std::vector<uint32_t> heap_view_env() {
    std::vector<uint32_t> v;
    const char* e = std::getenv("THPS_DD_HEAP_FREE");
    if (!e) return v;
    std::string s(e);
    for (size_t i = 0; i < s.size();) {
        const size_t j = std::min(s.find(',', i), s.size());
        if (j > i) v.push_back(uint32_t(std::strtoul(s.substr(i, j - i).c_str(), nullptr, 0)));
        i = j + 1;
    }
    return v;
}
std::mutex g_view_mutex;
std::vector<uint32_t> g_view;          // the test view of the free list (game thread; under g_view_mutex)
std::atomic<uint32_t> g_view_fails{0};
}  // namespace

std::vector<uint32_t> heap_free_blocks(const uint8_t* rdram, uint32_t heap) {
    if (heap_view_on()) {
        std::lock_guard lock{g_view_mutex};
        return g_view;
    }
    std::vector<uint32_t> b;
    for (uint32_t p = rd_word(rdram, heap), k = 0; p && k < 100000; p = rd_word(rdram, p + 4), k++) b.push_back(rd_word(rdram, p + 20));
    return b;
}

uint32_t heap_failed_cache(const uint8_t* rdram, uint32_t heap) { return heap_view_on() ? 0xFFFFFFFFu : rd_word(rdram, heap + 16); }

int64_t first_fit_after(std::vector<uint32_t> blocks, const std::vector<uint32_t>& needs, uint32_t cache) {
    for (uint32_t n : needs) {
        if (cache != 0xFFFFFFFFu && n >= cache) return -1;
        auto it = std::find_if(blocks.begin(), blocks.end(), [&](uint32_t f) { return f >= n; });
        if (it == blocks.end()) return -1;
        *it -= n;
    }
    int64_t largest = 0;
    for (uint32_t f : blocks) largest = std::max<int64_t>(largest, f);
    return largest;
}

bool heap_view_on() {
    static const bool on = std::getenv("THPS_DD_HEAP_FREE") != nullptr;
    return on;
}

void heap_view_begin() {
    if (!heap_view_on()) return;
    std::lock_guard lock{g_view_mutex};
    g_view = heap_view_env();
}

bool heap_view_take(uint32_t need, const char* what) {
    if (!heap_view_on()) return true;
    std::lock_guard lock{g_view_mutex};
    auto it = std::find_if(g_view.begin(), g_view.end(), [&](uint32_t f) { return f >= need; });
    if (it != g_view.end()) {
        *it -= need;
        return true;
    }
    g_view_fails.fetch_add(1);
    std::printf("[thps] draw distance: WARNING heap test view: %s (%u bytes) does not fit the view's free list: the game would hang here\n", what, need);
    std::fflush(stdout);
    return false;
}

uint32_t heap_view_fails() { return g_view_fails.load(); }

bool timing() { return thps::ftime_running(); }
void note_hook_time(uint64_t ns) {
    thps::stats.dd_hook_ns.fetch_add(ns, std::memory_order_relaxed);
    thps::stats.dd_hook_calls.fetch_add(1, std::memory_order_relaxed);
    uint64_t m = thps::stats.dd_hook_max_ns.load();
    while (ns > m && !thps::stats.dd_hook_max_ns.compare_exchange_weak(m, ns)) {}
}

float far_plane(uint8_t* rdram, float f) {
    if (!supported()) return f;
    HookTimer timer;
    const float out = dd_game_far(rdram, f, std::max(1.0f, g_in_use.load()));
    if (out != f) {
        g_scaled.fetch_add(1);
        g_far_seen = out;
    }
    return out;
}

namespace {
void note_task_inner(uint8_t* rdram, uint32_t data_ptr, uint32_t data_size);
void feedback_locked(float load, bool capped, uint64_t gen, const BufferUse* u);
}

void note_task(uint8_t* rdram, uint32_t data_ptr, uint32_t data_size) {
    if (!timing()) { note_task_inner(rdram, data_ptr, data_size); return; }
    const auto t0 = std::chrono::steady_clock::now();
    note_task_inner(rdram, data_ptr, data_size);
    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    thps::stats.dd_task_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t m = thps::stats.dd_task_max_ns.load();
    while (ns > m && !thps::stats.dd_task_max_ns.compare_exchange_weak(m, ns)) {}
}

namespace {
void note_task_inner(uint8_t* rdram, uint32_t data_ptr, uint32_t data_size) {
    BufferUse u{};   // measured also where the setting changes nothing (THPS2): the high-water marks are worth having
    const uint64_t gen = g_gen.load();   // before the measurement: a transition while it runs makes it stale
    if (!dd_game_measure(rdram, data_ptr, data_size, u)) return;   // not a frame of the game's 3D pipeline
    g_tasks.fetch_add(1);
    max_into(g_dl_hw, u.dl_bytes);
    max_into(g_mtx_hw, u.mtx_used);
    max_into(g_vtx_hw, u.vtx_used);
    max_into(g_queue_hw, u.queue_used);
    max_into(g_blend_hw, u.blend_used);
    max_into(g_defer_hw, u.defer_used);
    max_into(g_reset_hw, u.reset_used);
    max_into(g_runs_hw, u.runs);
    g_dl_cap = u.dl_cap;
    g_mtx_cap = u.mtx_cap;
    g_vtx_cap = u.vtx_cap;
    g_queue_cap = u.queue_cap;
    g_blend_cap = u.blend_cap;
    g_defer_cap = u.defer_cap;
    g_reset_cap = u.reset_cap;
    if (u.mtx_capped) g_mtx_capped.fetch_add(1);
    const float target = g_target.load();
    if (target <= 1.0001f || !supported()) return;
    const float cs = g_cap_scale.load();
    const float load = std::max({u.dl_cap ? float(u.dl_bytes) / (float(u.dl_cap) * cs) : 0.0f, u.mtx_cap ? float(u.mtx_used) / (float(u.mtx_cap) * cs) : 0.0f,
                                 u.vtx_cap ? float(u.vtx_used) / (float(u.vtx_cap) * cs) : 0.0f,
                                 u.queue_cap ? float(u.queue_used) / (float(u.queue_cap) * cs) : 0.0f,
                                 u.blend_cap ? float(u.blend_used) / (float(u.blend_cap) * cs) : 0.0f,
                                 u.defer_cap ? float(u.defer_used) / (float(u.defer_cap) * cs) : 0.0f,
                                 u.reset_cap ? float(u.reset_used) / (float(u.reset_cap) * cs) : 0.0f});
    std::lock_guard lock{g_gov_mutex};
    feedback_locked(load, u.mtx_capped, gen, &u);
}

// One task's feedback (g_gov_mutex held). gen: g_gen when its measurement started.
// A back-off also lowers the ceiling to the factor it backs off to, so a course settles at the largest factor that fits instead of
// pumping the far plane in and out (the ceiling is reset when a course starts or the mode is set again). Below the ceiling the factor
// ramps up (kRampStep a frame) while every buffer is under kRecover; between kRecover and kBackoff it holds.
void feedback_locked(float load, bool capped, uint64_t gen, const BufferUse* u) {
    if (gen != g_gen.load()) {   // a transition happened while this task was measured
        g_stale_dropped.fetch_add(1);
        return;
    }
    if (g_settle > 0) {          // a task built before the last transition
        g_settle--;
        g_settle_dropped.fetch_add(1);
        return;
    }
    float k = g_in_use.load();
    const float eff = std::min(k, g_effective.load());   // what is really in use (full mode clamps the far)
    if (load >= kPanic || capped) {
        if (k > 1.0f) g_panics.fetch_add(1);
        k = std::max(1.0f, eff * 0.5f);
        g_ceiling = k;
    } else if (load >= kBackoff) {
        k = std::max(1.0f, eff * 0.8f);
        g_ceiling = k;
        g_backoffs.fetch_add(1);
    } else if (load < kRecover) {
        const float end = factor_end();
        if (k < end) k = std::min(end, std::max(1.0f, k) * kRampStep);
    }
    if (u && thps::verbose() && k < g_in_use.load() - 0.001f) {   // which buffer made the governor step back
        std::printf("[thps] draw distance: governor %.2f -> %.2f (load %.0f %%): dl %u/%u mtx %u/%u%s vtx %u/%u queue %u/%u blend %u/%u defer %u/%u reset %u/%u runs %u\n",
                    double(g_in_use.load()), double(k), double(load * 100.0f), u->dl_bytes, u->dl_cap, u->mtx_used, u->mtx_cap, u->mtx_capped ? " (capped)" : "",
                    u->vtx_used, u->vtx_cap, u->queue_used, u->queue_cap, u->blend_used, u->blend_cap, u->defer_used, u->defer_cap, u->reset_used, u->reset_cap, u->runs);
        std::fflush(stdout);
    }
    g_in_use = k;
    if (k < g_in_use_min.load()) g_in_use_min = k;
}
}  // namespace

std::string stats() {
    char b[1200];
    std::snprintf(b, sizeof b, "mode=%s buffers=%s target=%.2f in_use=%.2f in_use_min=%.2f ceiling=%.2f supported=%d far_seen=%.1f scaled_calls=%" PRIu64 " tasks=%" PRIu64
                  " course_starts=%" PRIu64 " dl_hw=%u dl_cap=%u mtx_hw=%u mtx_cap=%u vtx_hw=%u vtx_cap=%u queue_hw=%u queue_cap=%u blend_hw=%u blend_cap=%u defer_hw=%u defer_cap=%u reset_hw=%u reset_cap=%u runs_hw=%u mtx_capped=%" PRIu64 " backoffs=%" PRIu64 " panics=%" PRIu64 " cuts=%" PRIu64 " cut_items=%" PRIu64 " late_cuts=%" PRIu64 " stale_dropped=%" PRIu64 " settle_dropped=%" PRIu64,
                  mode().c_str(), buffers().c_str(), double(g_target.load()), double(g_in_use.load()), double(g_in_use_min.load()), double(g_ceiling.load()), int(supported()), double(g_far_seen.load()), g_scaled.load(),
                  g_tasks.load(), g_course_starts.load(), g_dl_hw.load(), g_dl_cap.load(), g_mtx_hw.load(), g_mtx_cap.load(), g_vtx_hw.load(), g_vtx_cap.load(), g_queue_hw.load(), g_queue_cap.load(), g_blend_hw.load(), g_blend_cap.load(), g_defer_hw.load(), g_defer_cap.load(), g_reset_hw.load(), g_reset_cap.load(), g_runs_hw.load(), g_mtx_capped.load(), g_backoffs.load(), g_panics.load(), g_cuts.load(), g_cut_items.load(), g_late_cuts.load(), g_stale_dropped.load(), g_settle_dropped.load());
    return b;
}

bool control(const std::string& cmd, std::istringstream& in) {
    if (cmd != "dd") return false;
    auto reply = [](const std::string& s) { std::printf("[ctl] %s\n", s.c_str()); std::fflush(stdout); };
    std::string sub;
    in >> sub;
    if (sub == "mode") {
        std::string m;
        in >> m;
        if (!valid_mode(m)) { reply("err dd mode original|2x|4x|full"); return true; }
        set_mode(m);
    } else if (sub == "factor") {
        float k = 1.0f;
        in >> k;
        if (!(k >= 1.0f && k <= 64.0f)) { reply("err dd factor 1..64"); return true; }
        std::lock_guard lock{g_gov_mutex};   // exploration: at once, no ramp (the paused A/B tests compare factors frame by frame)
        g_target = k;
        g_in_use = k;
        g_ceiling = k;
        transition_locked();
    } else if (sub == "showall") {
        std::string v;
        in >> v;
        g_showall_override = v == "1" ? 1 : v == "0" ? 0 : v == "all" ? 2 : -1;
    } else if (sub == "testcap") {
        float pct = 100.0f;
        in >> pct;
        if (!(pct >= 1.0f && pct <= 100.0f)) { reply("err dd testcap 1..100"); return true; }
        g_cap_scale = pct / 100.0f;
    } else if (sub == "item") {
        uint32_t i = 0;
        in >> i;
        uint8_t* ram = thps::rdram_base.load();
        reply("ok dd item " + std::to_string(i) + " " + (ram ? dd_game_item(ram, i) : std::string("none")));
        return true;
    } else if (sub == "own") {
        uint32_t i = 0;
        in >> i;
        uint8_t* ram = thps::rdram_base.load();
        reply("ok dd own " + std::to_string(i) + " " + (ram ? dd_game_own(ram, i) : std::string("none")));
        return true;
    } else if (sub == "heap") {
        uint8_t* ram = thps::rdram_base.load();
        std::string r;
        in >> r;
        reply("ok dd heap " + (ram ? dd_game_heap(ram, r == "reset") : std::string("none")));
        return true;
    } else if (sub == "dlbudget") {
        uint32_t v[4] = {0, 0, 0, 0};
        for (uint32_t& x : v) in >> x;
        g_budget_cap = v[0], g_budget_rest = v[1], g_budget_pass = v[2], g_budget_margin = v[3];
    } else if (sub == "dlprobe") {
        uint8_t* ram = thps::rdram_base.load();
        std::string r;
        in >> r;
        reply("ok dd dlprobe " + (ram ? dd_game_dlprobe(ram, r == "reset") : std::string("none")));
        return true;
    } else if (sub == "stale" || sub == "feed") {
        // Tests (finding 6), deterministic: one feedback sample of PCT % load (default 99: a panic) through the governor, in one critical section.
        // `dd stale PCT`: the setting is set again (a transition), then the sample is given twice: tagged with the generation from before the
        // transition (a task measured across it: dropped as stale) and with the new one (a task built before it: dropped while settling).
        // Neither may move the governor. `dd feed PCT`: one sample of the current generation (outside a settle it acts: the control case).
        float pct = 99.0f;
        in >> pct;
        if (!(pct >= 0.0f && pct <= 200.0f)) { reply("err dd " + sub + " 0..200"); return true; }
        if (supported()) {
            std::lock_guard lock{g_gov_mutex};
            if (sub == "stale") {
                const uint64_t before = g_gen.load();
                g_ceiling = g_target.load();
                g_in_use = std::max(1.0f, g_target.load());
                transition_locked();
                feedback_locked(pct / 100.0f, false, before, nullptr);
                feedback_locked(pct / 100.0f, false, g_gen.load(), nullptr);
            } else if (g_target.load() > 1.0001f) {
                feedback_locked(pct / 100.0f, false, g_gen.load(), nullptr);
            }
        }
    } else if (sub == "reset") {
        g_in_use_min = 64.0f, g_dl_hw = 0, g_mtx_hw = 0, g_vtx_hw = 0, g_queue_hw = 0, g_blend_hw = 0, g_defer_hw = 0, g_reset_hw = 0, g_runs_hw = 0, g_tasks = 0, g_backoffs = 0, g_panics = 0, g_scaled = 0, g_mtx_capped = 0, g_cuts = 0, g_cut_items = 0, g_late_cuts = 0, g_stale_dropped = 0, g_settle_dropped = 0;
    } else if (!sub.empty()) {
        reply("err dd [mode M | factor X | testcap PCT | showall 0|1|all|auto | item N | heap [reset] | dlbudget CAP REST PASS MARGIN | dlprobe [reset] | stale PCT | feed PCT | own N | reset]");
        return true;
    }
    reply("ok dd " + stats());
    return true;
}

}  // namespace thps::dd
