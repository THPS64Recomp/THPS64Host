// Input latency for the THPS recomp hosts: measurement (--control "latency") and options.
//
// Measurement: the press is injected at a game input poll (like --control press), and every
// presented image (RT64 present capture, the same composite the window shows, interpolated
// frames included) is reduced to a 32x24 luma grid. Before the press, a few presents give the
// noise level of the screen (animated backgrounds); after it, the first present whose grid
// differs from the last pre-press one by more than the threshold ends the measurement:
//   ms       = time from the poll that delivered the press to that present (on the present thread)
//   presents = composited frames since the press (at 60 Hz with interpolation, 2 per game frame)
//   frames   = frames the game displayed (VI origin changes) since the press
// No game data: only timings and a grid of averages.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "versions.h"

#include "hle/rt64_application.h"

#include "hfr.h"
#include "host.h"

uint16_t thps_parse_buttons(const std::string& s, float& sx, float& sy);  // main.cpp

namespace {

constexpr int GW = 32, GH = 24;
using Grid = std::vector<float>;

enum class Phase { Idle, Baseline, Pressing, Waiting };

std::mutex g_mutex;
Phase g_phase = Phase::Idle;
uint16_t g_buttons = 0;
int g_hold = 3;
int g_polls_held = 0;
bool g_pressed = false;
std::chrono::steady_clock::time_point g_t_press;
uint64_t g_frames_at_press = 0, g_present_at_press = 0;
int g_baseline_left = 0, g_wait_left = 0;
Grid g_last;              // last pre-press grid
bool g_cursor_mode = true;  // detect the menu cursor (gold band) moving instead of any image change
float g_band = -1;          // last pre-press cursor band
float g_noise = 0;        // max consecutive difference before the press
float g_min_threshold = 12.0f;
uint32_t g_saved_every = 0, g_saved_w = 0, g_saved_h = 0;
std::atomic<RT64::Application*> g_app{nullptr};

void reply(const std::string& s) {
    std::printf("[ctl] %s\n", s.c_str());
    std::fflush(stdout);
}

Grid grid_of(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t pitch) {
    Grid g(GW * GH, 0.0f);
    std::vector<int> n(GW * GH, 0);
    for (uint32_t y = 0; y < h; y += 2) {
        const uint8_t* row = bgra + size_t(y) * pitch;
        int gy = int(y * GH / h);
        for (uint32_t x = 0; x < w; x += 2) {
            const uint8_t* p = row + x * 4;
            int gx = int(x * GW / w);
            g[gy * GW + gx] += 0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2];
            n[gy * GW + gx]++;
        }
    }
    for (int i = 0; i < GW * GH; i++) g[i] = n[i] ? g[i] / float(n[i]) : 0.0f;
    return g;
}

// Centre (fraction of the height) of the strongest band of highlighted (gold text / orange bar) menu text in the
// menu box of thps_game.h (menu_cursor_box_pct), or -1: the menu cursor (port of tests/harness.py highlight_band).
float highlight_band(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t pitch) {
    const int* bx = thps::game::menu_cursor_box_pct;
    uint32_t x0 = w * bx[0] / 100, x1 = w * bx[1] / 100, y0 = h * bx[2] / 100, y1 = h * bx[3] / 100;
    std::vector<int> prof;
    for (uint32_t y = y0; y < y1; y++) {
        const uint8_t* row = bgra + size_t(y) * pitch;
        int n = 0;
        for (uint32_t x = x0; x < x1; x++) {
            int b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
            n += ((r > 170 && g > 110 && b < 120 && r - b > 80) || (r > 140 && g > 45 && g < 100 && b < 40)) ? 1 : 0;   // gold text, or THPS3's orange bar
        }
        prof.push_back(n);
    }
    int best = 0;
    float best_c = -1;
    for (size_t y = 0; y < prof.size(); y++) {
        if (prof[y] > 3 * int(w) / 640 + 1) {
            size_t s0 = y;
            int tot = 0;
            while (y < prof.size() && prof[y] > 3 * int(w) / 640 + 1) tot += prof[y++];
            if (tot > best) best = tot, best_c = (float(y0) + float(s0 + y) / 2.0f) / float(h);
        }
    }
    return best >= int(40 * w / 640) ? best_c : -1.0f;
}

float grid_dist(const Grid& a, const Grid& b) {  // largest cell difference (a cursor move is local)
    float m = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); i++) m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}

void restore_capture() {
    if (RT64::Application* app = g_app.load()) {
        app->presentQueue->captureEvery = g_saved_every;
        app->presentQueue->captureWidth = g_saved_w;
        app->presentQueue->captureHeight = g_saved_h;
    }
}

}  // namespace

namespace thps {

void latency_set_app(RT64::Application* app) { g_app.store(app); }

// Game thread, port 1 poll: deliver the armed press.
void latency_on_poll(uint16_t* buttons) {
    std::lock_guard lock{g_mutex};
    if (g_phase != Phase::Pressing) return;
    if (g_polls_held == 0) {
        g_t_press = std::chrono::steady_clock::now();
        g_frames_at_press = stats.vi_origin_changes.load();
        if (RT64::Application* app = g_app.load()) g_present_at_press = app->presentQueue->presentedFrameCount.load();
        g_pressed = true;
    }
    if (g_polls_held < g_hold) *buttons |= g_buttons;
    if (++g_polls_held >= g_hold) g_phase = Phase::Waiting;
}

// Present thread (RT64 capture callback): true = this capture was for the measurement (no PNG).
bool latency_on_present(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t pitch, uint64_t present_index) {
    std::lock_guard lock{g_mutex};
    if (g_phase == Phase::Idle) return false;
    if (g_cursor_mode) {
        float band = highlight_band(bgra, w, h, pitch);
        if (g_phase == Phase::Baseline || !g_pressed) {
            if (band >= 0) g_band = band;
            if (g_phase == Phase::Baseline && --g_baseline_left <= 0 && g_band >= 0) {
                g_phase = Phase::Pressing;
                g_polls_held = 0;
                g_pressed = false;
            }
            if (g_phase == Phase::Baseline && g_baseline_left < -120) {
                g_phase = Phase::Idle;
                restore_capture();
                reply("err latency no menu cursor (gold highlight) on screen");
            }
            return true;
        }
        bool moved = band >= 0 && std::fabs(band - g_band) > 0.03f;
        if (moved || --g_wait_left <= 0) {
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_t_press).count();
            char buf[300];
            if (moved) {
                std::snprintf(buf, sizeof buf, "ok latency ms=%.1f presents=%" PRIu64 " frames=%" PRIu64 " mode=cursor from=%.3f to=%.3f",
                              ms, present_index + 1 - g_present_at_press, stats.vi_origin_changes.load() - g_frames_at_press, g_band, band);
            } else {
                std::snprintf(buf, sizeof buf, "err latency cursor did not move within %.0f ms (at %.3f)", ms, g_band);
            }
            g_phase = Phase::Idle;
            restore_capture();
            reply(buf);
        }
        return true;
    }
    Grid g = grid_of(bgra, w, h, pitch);
    if (g_phase == Phase::Baseline) {
        if (!g_last.empty()) g_noise = std::max(g_noise, grid_dist(g, g_last));
        g_last = std::move(g);
        if (--g_baseline_left <= 0) {
            g_phase = Phase::Pressing;
            g_polls_held = 0;
            g_pressed = false;
        }
        return true;
    }
    if (!g_pressed) {  // Pressing, but the game hasn't polled yet: keep the reference current
        g_last = std::move(g);
        return true;
    }
    float d = grid_dist(g, g_last);
    float thr = std::max(g_min_threshold, 3.0f * g_noise);
    if (d > thr || --g_wait_left <= 0) {
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_t_press).count();
        char buf[300];
        if (d > thr) {
            std::snprintf(buf, sizeof buf, "ok latency ms=%.1f presents=%" PRIu64 " frames=%" PRIu64 " dist=%.1f noise=%.1f threshold=%.1f",
                          ms, present_index + 1 - g_present_at_press, stats.vi_origin_changes.load() - g_frames_at_press, d, g_noise, thr);
        } else {
            std::snprintf(buf, sizeof buf, "err latency no change within %.0f ms (noise %.1f, threshold %.1f)", ms, g_noise, thr);
        }
        g_phase = Phase::Idle;
        restore_capture();
        reply(buf);
    }
    return true;
}

// latency BUTTONS [HOLD [THRESHOLD]]: measure press -> first presented image where the menu
// cursor (gold highlight band) has moved; "latency_image ..." = any image change instead.
bool latency_control(const std::string& cmd, std::istringstream& in) {
    if (cmd != "latency" && cmd != "latency_image") return false;
    std::string b;
    int hold = 3;
    float thr = 12.0f;
    in >> b >> hold >> thr;
    RT64::Application* app = g_app.load();
    if (b.empty() || !app) {
        reply("err latency usage: latency BUTTONS [HOLD [THRESHOLD]] (after the renderer is up)");
        return true;
    }
    float sx = 0, sy = 0;
    std::lock_guard lock{g_mutex};
    if (g_phase != Phase::Idle) {
        reply("err latency already running");
        return true;
    }
    g_cursor_mode = cmd == "latency";
    g_band = -1;
    g_buttons = thps_parse_buttons(b, sx, sy);
    g_hold = std::max(1, hold);
    g_min_threshold = thr;
    g_noise = 0;
    g_last.clear();
    g_baseline_left = 6;
    g_wait_left = 240;
    g_saved_every = app->presentQueue->captureEvery.load();
    g_saved_w = app->presentQueue->captureWidth.load();
    g_saved_h = app->presentQueue->captureHeight.load();
    app->presentQueue->captureWidth = 320;
    app->presentQueue->captureHeight = 240;
    app->presentQueue->captureEvery = 1;
    g_phase = Phase::Baseline;
    return true;
}

}  // namespace thps
