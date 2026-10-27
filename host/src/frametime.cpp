// Frame-time statistics for benchmarks (--control ftime start | stop): timestamps of every frame RT64 composites and presents and of every game
// frame (gfx task), taken by a sampler thread that polls the counters every 200 us, so no hook sits in the frame path and nothing is written to disk.
// `stop` answers: fps, p50 / p95 / p99 / max of the interval between frames and how many were over 20 ms (presents) or 40 ms (game frames), plus the cost of the
// interpolation-ID walk and tracker (interp_ids.cpp) over the same window. Used by the `idsbench` sessions; see docs/interpolation-ids.md.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hle/rt64_application.h"
#include "hle/rt64_present_queue.h"
#include "render/rt64_raster_shader_cache.h"
#include "hle/rt64_hfr_stats.h"

#include "hfr.h"
#include "host.h"

namespace {

using Clock = std::chrono::steady_clock;

std::thread g_thread;
std::atomic<bool> g_run{false};
std::mutex g_mutex;
std::vector<double> g_composited, g_presented, g_game, g_erz, g_dma;   // seconds since start (g_erz / g_dma: ERZ unpacks and raw ROM DMAs of the game, for the CSV)
Clock::time_point g_t0;
thps::IdsTiming g_ids0;
double g_load0 = 0;
uint64_t g_erz_ns0 = 0, g_dma_ns0 = 0, g_erz_n0 = 0, g_mem0 = 0;
uint64_t g_dd_hook_ns0 = 0, g_dd_hook_calls0 = 0, g_dd_task_ns0 = 0, g_gfx0 = 0;
struct Span { const char* name; double start, ms; };
std::mutex g_span_mutex;
std::vector<Span> g_spans;   // thps::ftime_span() while ftime runs (capped: a forgotten `ftime start` must not grow without bound)

void sample() {
    RT64::Application* app = thps::hfr_app();
    uint64_t c = app ? app->presentQueue->presentedFrameCount.load() : 0, w = app ? app->presentQueue->windowPresentCount.load() : 0, g = thps::stats.gfx_tasks.load();
    uint64_t e = thps::stats.erz_unpacks.load(), d = thps::stats.raw_dmas.load();
    uint64_t wl = RT64::hfrStats.workloads.load(), wus = RT64::hfrStats.renderMicros.load(), drop = RT64::hfrStats.displayFramesDropped.load();
    while (g_run.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        const double t = std::chrono::duration<double>(Clock::now() - g_t0).count();
        const uint64_t c2 = app ? app->presentQueue->presentedFrameCount.load() : 0, w2 = app ? app->presentQueue->windowPresentCount.load() : 0, g2 = thps::stats.gfx_tasks.load();
        std::lock_guard lock{g_mutex};
        if (c2 != c) g_composited.push_back(t);
        if (w2 != w) g_presented.push_back(t);
        if (g2 != g) g_game.push_back(t);
        const uint64_t e2 = thps::stats.erz_unpacks.load(), d2 = thps::stats.raw_dmas.load();
        if (e2 != e) g_erz.push_back(t);
        if (d2 != d) g_dma.push_back(t);
        c = c2, w = w2, g = g2, e = e2, d = d2;
        // RT64's workload thread (rt64-hfr patch counters): a finished workload (game frame rendered, its interpolated frames included)
        // becomes a span "workload" ending now, as long as its render time; dropped interpolated frames are counted on it.
        const uint64_t wl2 = RT64::hfrStats.workloads.load();
        if (wl2 != wl) {
            const uint64_t wus2 = RT64::hfrStats.renderMicros.load(), drop2 = RT64::hfrStats.displayFramesDropped.load();
            const double ms = double(wus2 - wus) / 1000.0 / double(wl2 - wl);
            std::lock_guard sl{g_span_mutex};
            for (uint64_t k = wl; k < wl2; k++) g_spans.push_back({drop2 != drop ? "workload_dropped" : "workload", t - ms / 1000.0 * double(wl2 - k), ms});
            wl = wl2, wus = wus2, drop = drop2;
        }
    }
}

std::string describe(const char* name, std::vector<double> ts, double seconds, double slow_ms) {
    char b[300];
    if (ts.size() < 3) { std::snprintf(b, sizeof b, " %s_n=%zu", name, ts.size()); return b; }
    std::vector<double> d;
    for (size_t i = 1; i < ts.size(); i++) d.push_back((ts[i] - ts[i - 1]) * 1000.0);
    std::sort(d.begin(), d.end());
    auto q = [&](double p) { return d[std::min(d.size() - 1, size_t(double(d.size()) * p))]; };
    size_t over = 0, over50 = 0, over100 = 0;
    for (double x : d) over += x > slow_ms, over50 += x > 50.0, over100 += x > 100.0;
    std::snprintf(b, sizeof b, " %s_fps=%.2f %s_ms_p50=%.2f %s_ms_p95=%.2f %s_ms_p99=%.2f %s_ms_max=%.1f %s_over%.0fms=%zu %s_over50ms=%zu %s_over100ms=%zu", name, double(ts.size()) / seconds, name, q(0.5), name, q(0.95), name,
                  q(0.99), name, d.back(), name, slow_ms, over, name, over50, name, over100);
    return b;
}

// Histogram of the frame intervals in 0.5 ms bins ("<bin>:<count>,...", bin = interval in half milliseconds, capped at 600 = 300 ms): lets a soak run add
// windows together and read exact percentiles (tests/soak.py). Only the non-empty bins are written.
std::string histogram(const char* name, const std::vector<double>& ts) {
    if (ts.size() < 3) return "";
    std::map<int, size_t> bins;
    for (size_t i = 1; i < ts.size(); i++) bins[std::min(600, int((ts[i] - ts[i - 1]) * 2000.0))]++;
    std::string out = std::string(" ") + name + "_hist=";
    for (auto& [bin, n] : bins) out += std::to_string(bin) + ":" + std::to_string(n) + ",";
    out.pop_back();
    return out;
}

}  // namespace

namespace thps {

bool ftime_running() { return g_run.load(std::memory_order_relaxed); }

void ftime_span(const char* name, std::chrono::steady_clock::time_point t0) {
    if (!g_run.load(std::memory_order_relaxed)) return;
    const auto now = Clock::now();
    const Span sp{name, std::chrono::duration<double>(t0 - g_t0).count(), std::chrono::duration<double, std::milli>(now - t0).count()};
    std::lock_guard lock{g_span_mutex};
    if (g_spans.size() < 2000000) g_spans.push_back(sp);
}

bool frametime_control(const std::string& cmd, std::istringstream& in) {
    if (cmd != "ftime") return false;
    std::string sub;
    in >> sub;
    auto reply = [](const std::string& s) { std::printf("[ctl] %s\n", s.c_str()); std::fflush(stdout); };
    if (sub == "start") {
        if (g_run.exchange(true)) { reply("err ftime already running"); return true; }
        { std::lock_guard lock{g_mutex}; g_composited.clear(); g_presented.clear(); g_game.clear(); g_erz.clear(); g_dma.clear(); }
        { std::lock_guard lock{g_span_mutex}; g_spans.clear(); }
        g_ids0 = ids_timing();
        g_erz_ns0 = thps::stats.erz_ns.load(), g_dma_ns0 = thps::stats.raw_dma_ns.load(), g_erz_n0 = thps::stats.erz_unpacks.load(), g_mem0 = thps::stats.mem_bytes.load();
        thps::stats.erz_max_ns = 0;
        g_dd_hook_ns0 = thps::stats.dd_hook_ns.load(), g_dd_hook_calls0 = thps::stats.dd_hook_calls.load(), g_dd_task_ns0 = thps::stats.dd_task_ns.load();
        g_gfx0 = thps::stats.gfx_tasks.load();
        thps::stats.dd_hook_max_ns = 0, thps::stats.dd_task_max_ns = 0;
        g_t0 = Clock::now();
        g_thread = std::thread(sample);
        reply("ok ftime started");
    } else if (sub == "stop") {
        std::string file;   // optional: write every timestamp (kind,seconds) for offline pacing analysis
        std::getline(in >> std::ws, file);
        if (!g_run.exchange(false)) { reply("err ftime not running"); return true; }
        g_thread.join();
        const double seconds = std::chrono::duration<double>(Clock::now() - g_t0).count();
        const IdsTiming t = ids_timing();
        std::vector<double> c, p, g, erz, dma;
        { std::lock_guard lock{g_mutex}; c = g_composited; p = g_presented; g = g_game; erz = g_erz; dma = g_dma; }
        std::vector<Span> spans;
        { std::lock_guard lock{g_span_mutex}; spans.swap(g_spans); }
        if (!file.empty()) {
            if (FILE* f = std::fopen(file.c_str(), "w")) {
                std::fputs("kind,seconds\n", f);
                for (double x : p) std::fprintf(f, "presented,%.6f\n", x);
                for (double x : c) std::fprintf(f, "composited,%.6f\n", x);
                for (double x : g) std::fprintf(f, "game,%.6f\n", x);
                for (double x : erz) std::fprintf(f, "erz,%.6f\n", x);
                for (double x : dma) std::fprintf(f, "dma,%.6f\n", x);
                for (const Span& sp : spans) std::fprintf(f, "span,%s,%.6f,%.3f\n", sp.name, sp.start, sp.ms);
                std::fclose(f);
            }
        }
        const uint64_t tasks = t.tasks - g_ids0.tasks;
        char b[300];
        std::snprintf(b, sizeof b, "ok ftime seconds=%.2f", seconds);
        std::string out = b;
        out += describe("presented", p, seconds, 20.0) + describe("composited", c, seconds, 20.0) + describe("game", g, seconds, 40.0);
        out += histogram("presented", p) + histogram("game", g);
        std::snprintf(b, sizeof b, " ids_tasks=%" PRIu64 " ids_walk_us_avg=%.1f ids_walk_us_max=%.1f ids_cpu_us_avg=%.1f ids_cpu_us_max=%.1f ids_hook_us_avg=%.1f ids_budget_clock=%s", tasks, tasks ? double(t.walk_ns - g_ids0.walk_ns) / 1000.0 / double(tasks) : 0.0,
                      double(t.walk_max_ns) / 1000.0, tasks ? double(t.cpu_ns - g_ids0.cpu_ns) / 1000.0 / double(tasks) : 0.0, double(t.cpu_max_ns) / 1000.0, tasks ? double(t.hook_ns - g_ids0.hook_ns) / 1000.0 / double(tasks) : 0.0, ids_budget_clock());
        out += b;
        std::snprintf(b, sizeof b, " erz_n=%" PRIu64 " erz_ms=%.1f erz_ms_max=%.2f dma_ms=%.1f mem_kb=%" PRIu64, thps::stats.erz_unpacks.load() - g_erz_n0, double(thps::stats.erz_ns.load() - g_erz_ns0) / 1e6,
                      double(thps::stats.erz_max_ns.load()) / 1e6, double(thps::stats.raw_dma_ns.load() - g_dma_ns0) / 1e6,
                      (thps::stats.mem_bytes.load() - g_mem0) / 1024);
        out += b;
        // Draw distance's own time per gfx task (game-thread hooks without the game's drawing they call; the gfx thread's note_task)
        const uint64_t gt = thps::stats.gfx_tasks.load() - g_gfx0;
        std::snprintf(b, sizeof b, " dd_hook_us_per_task=%.2f dd_hook_us_max=%.1f dd_hook_calls=%" PRIu64 " dd_task_us_avg=%.2f dd_task_us_max=%.1f",
                      gt ? double(thps::stats.dd_hook_ns.load() - g_dd_hook_ns0) / 1000.0 / double(gt) : 0.0, double(thps::stats.dd_hook_max_ns.load()) / 1000.0,
                      thps::stats.dd_hook_calls.load() - g_dd_hook_calls0, gt ? double(thps::stats.dd_task_ns.load() - g_dd_task_ns0) / 1000.0 / double(gt) : 0.0,
                      double(thps::stats.dd_task_max_ns.load()) / 1000.0);
        reply(out + b);
    } else if (sub == "shaders") {
        // RT64's specialised-shader cache (read only; a soak run polls it to see when new shaders appear): `submitted` = distinct shaders RT64 has
        // asked for so far, `compiled` = the ones whose pipeline is ready (until then the ubershader draws them), `queued` = waiting for a compile thread.
        RT64::Application* app = thps::hfr_app();
        if (!app || !app->rasterShaderCache) { reply("err ftime shaders: renderer not up"); return true; }
        RT64::RasterShaderCache& rc = *app->rasterShaderCache;
        size_t submitted, queued;
        { std::lock_guard lock{rc.submissionMutex}; submitted = rc.shaderHashes.size(); }
        { std::lock_guard lock{rc.descQueueMutex}; queued = rc.descQueue.size(); }
        char b[160];
        std::snprintf(b, sizeof b, "ok ftime shaders submitted=%zu compiled=%u queued=%zu threads=%u", submitted, rc.shaderCount(), queued, rc.threadCount);
        reply(b);
    } else {
        reply("err ftime start|stop|shaders");
    }
    return true;
}

}  // namespace thps
