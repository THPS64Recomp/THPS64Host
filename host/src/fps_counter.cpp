// Show FPS (show_fps=1 in general.cfg / the Graphics tab, or --show-fps): two rates, averaged
// over half a second, like CBFD-Recompiled's counter:
// - FPS: frames RT64 composited for presentation (rt64 patch presentedFrameCount; would include
//   interpolated frames). A drop here is the Mac falling behind.
// - Game: frames the game itself displayed (VI origin changes; the games run at up to 30). A drop
//   here with FPS steady is the game's own slowdown, as on the N64.
// CLI build: shown in the window title. Frontend build: a recompui overlay in the top-right
// corner (frontend.cpp). Either way each new text is logged ("[thps] fps overlay: ..."), so tests
// can compare it with the perf lines.
#include <chrono>
#include <cstdio>
#include <string>

#define SDL_MAIN_HANDLED
#include "sdl_include.h"

#include "host.h"
#include "versions.h"

namespace {
using clock_t_ = std::chrono::steady_clock;
clock_t_::time_point g_last{};
uint64_t g_last_presented = 0, g_last_game = 0;
bool g_shown = false;
const char* k_title = thps::game::window_title;
}  // namespace

void thps::fps_update(void* sdl_window) {
    auto* w = static_cast<SDL_Window*>(sdl_window);
    if (!settings::show_fps()) {
        if (g_shown) {
            g_shown = false;
#ifdef THPS_FRONTEND
            frontend::fps_text("");
#else
            if (w) SDL_SetWindowTitle(w, k_title);
#endif
        }
        g_last = {};
        return;
    }
    auto now = clock_t_::now();
    uint64_t presented = stats.rt64_composited.load(), game = stats.vi_origin_changes.load();
    if (g_last == clock_t_::time_point{}) {
        g_last = now, g_last_presented = presented, g_last_game = game;
        return;
    }
    if (now - g_last < std::chrono::milliseconds(500)) return;
    double s = std::chrono::duration<double>(now - g_last).count();
    char text[64];
    std::snprintf(text, sizeof text, "%.0f FPS  |  Game %.0f", (presented - g_last_presented) / s, (game - g_last_game) / s);
    g_last = now, g_last_presented = presented, g_last_game = game;
    g_shown = true;
#ifdef THPS_FRONTEND
    frontend::fps_text(text);
#else
    if (w) SDL_SetWindowTitle(w, (std::string(k_title) + "  -  " + text).c_str());
#endif
    static std::string last;
    if (last != text) {
        last = text;
        if (thps::options.verbose || thps::options.control) std::printf("[thps] fps overlay: %s\n", text);
        std::fflush(stdout);
    }
}
