// General settings of the THPS recomp hosts (no game data).
//
// CLI build (default): <config>/general.cfg, key=value lines, written with the defaults on the
// first run:
//   skip_intro = 0 | 1    boot straight to the title (src/skip_intro.cpp)
//   show_fps   = 0 | 1    frame counter in the window title (src/fps_counter.cpp)
//   volume     = 0..100   master volume, percent (src/audio.cpp)
// Frontend build (THPS_WITH_RECOMPFRONTEND): the same options live in recompui's General,
// Graphics and Sound tabs (src/frontend.cpp) instead.
// Command-line flags (--skip-intro/--no-skip-intro, --show-fps/--no-show-fps, --volume N)
// override them for one run.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "host.h"

std::atomic<int> thps::skip_intro_override{-1};
std::atomic<int> thps::show_fps_override{-1};
std::atomic<int> thps::volume_override{-1};

#ifndef THPS_FRONTEND
namespace {
std::atomic<bool> g_skip_intro{false}, g_show_fps{false};
std::atomic<int> g_volume{100};
std::atomic<bool> g_changed{false};          // THPS_MENU: an option changed in the in-game menu (see take_changed)
std::filesystem::path g_general_cfg;         // where save() writes
}  // namespace

void thps::settings::load(const std::filesystem::path& config_dir) {
    auto file = config_dir / "general.cfg";
    g_general_cfg = file;
    std::ifstream in(file);
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            while (!k.empty() && k.back() == ' ') k.pop_back();
            int n = std::atoi(v.c_str());
            if (k == "skip_intro") g_skip_intro = n != 0;
            else if (k == "show_fps") g_show_fps = n != 0;
            else if (k == "volume") g_volume = std::clamp(n, 0, 100);
            else std::fprintf(stderr, "[thps] %s: ignoring %s\n", file.string().c_str(), line.c_str());
        }
        return;
    }
    std::ofstream out(file);
    out << "# " << thps::game::short_name << " recomp host settings (command line: --skip-intro, --show-fps, --volume N override for one run)\n"
           "# skip_intro: 1 = boot straight to the title (no Expansion Pak notice, legal screen or logos)\n"
           "skip_intro = 0\n"
           "# show_fps: 1 = frames per second in the window title (presented frames | game frames)\n"
           "show_fps = 0\n"
           "# volume: master volume, 0..100\n"
           "volume = 100\n";
}

bool thps::settings::skip_intro() {
    int o = skip_intro_override.load();
    return o >= 0 ? o != 0 : g_skip_intro.load();
}

bool thps::settings::show_fps() {
    int o = show_fps_override.load();
    return o >= 0 ? o != 0 : g_show_fps.load();
}

float thps::settings::volume() {
    int o = volume_override.load();
    return float(o >= 0 ? o : g_volume.load()) / 100.0f;
}

std::string thps::settings::widescreen() { return "expand"; }
bool thps::settings::take_changed() { return g_changed.exchange(false); }

// The in-game menu (src/ui/, THPS_MENU) writes through these; the file keeps the same three keys and comments as load() makes.
void thps::settings::set_skip_intro(bool v) { g_skip_intro = v; }
void thps::settings::set_show_fps(bool v) { g_show_fps = v; }
void thps::settings::set_volume_percent(int v) { g_volume = std::clamp(v, 0, 100); }
void thps::settings::mark_changed() { g_changed = true; }
void thps::settings::save() {
    if (g_general_cfg.empty()) return;
    auto tmp = g_general_cfg;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        out << "# " << thps::game::short_name << " recomp host settings (command line: --skip-intro, --show-fps, --volume N override for one run)\n"
               "# skip_intro: 1 = boot straight to the title (no Expansion Pak notice, legal screen or logos)\n"
            << "skip_intro = " << (g_skip_intro.load() ? 1 : 0) << "\n"
               "# show_fps: 1 = frames per second in the window title (presented frames | game frames)\n"
            << "show_fps = " << (g_show_fps.load() ? 1 : 0) << "\n"
               "# volume: master volume, 0..100\n"
            << "volume = " << g_volume.load() << "\n";
    }
    std::error_code ec;
    std::filesystem::rename(tmp, g_general_cfg, ec);
}
#endif
