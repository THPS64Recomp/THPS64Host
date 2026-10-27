// Graphics settings of the THPS recomp hosts (no game data).
//
// CLI-only build (default): key=value lines in <config>/graphics.cfg; command-line options
// override a key and are saved back.
// With the settings menu (THPS_WITH_RECOMPFRONTEND): recompui's Graphics tab is the source
// (<config>/graphics.json): resolution + downsampling, aspect ratio, MSAA, window mode, plus our
// own "Widescreen" target (window / 16:9 / 21:9, used when the aspect ratio is Expand).
// Command-line options then override it for one run and are not saved; a change made in the
// menu replaces them.
//
//   aspect    original | 16:9 | 21:9 | expand   (RT64 aspect ratio; expand = follow the window)
//   window    WxH                               (initial window size)
//   res_scale N                                 (internal resolution multiplier, 0 = auto)
//   msaa      0 | 2 | 4 | 8
//   refresh   original | display | N             (frame interpolation target; src/interpolation.cpp)
//   hud       original | 16:9 | expand            (HUD placement in widescreen; src/hud_anchor.cpp; default kDefaultHud = expand, host.h)
//   split     wide | original                    (two-player split screens fill the wide picture; original = both views in the 4:3 area; default wide)
//   border    original | fill                     (the game's 8 px frame around the 3D picture; src/border_crop.cpp; default original)
//   draw_distance original | 2x | 4x | full     (the game's far plane x1 / x2 / x4 / the whole course; src/draw_distance.cpp; written only when not original;
//                                               far and max, the names before 2026-10-26, are read as 2x and 4x)
//   dd_buffers bigger | original              (draw distance's matrix pools: bigger = always 2048, so the setting takes effect at once;
//                                               original = the game's own allocations; read at start-up; written only when original)
//   cfg_rev   N                                   (written by the host, not a setting: which defaults the file was saved under; see gfx_config_load)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <utility>
#include <vector>
#include <string>

#include "draw_distance.h"
#include "host.h"
#include "hfr.h"

namespace thps {

// graphics.cfg revision: 1 = no cfg_rev line (every build before 2026-10-14), 2 = HUD Placement defaults to `expand`.
// Builds before rev 2 saved `hud=original` on their first run (it was the default then), so a file without cfg_rev that says
// `hud=original` was never a choice, as far as we can tell: it moves to the new default once (gfx_config_load). A file that has cfg_rev
// is left alone, so `original` chosen afterwards (menu, --hud original) stays.
static constexpr int kCfgRev = 2;

GfxConfig gfx;
std::atomic<float> window_aspect{4.0f / 3.0f};
std::atomic<float> target_aspect{0.0f};

static std::vector<std::pair<std::string, std::string>> g_overrides;

float gfx_widen_factor() {
    float t = target_aspect.load();
    float a = t < 0 ? window_aspect.load() : t;
    float f = a / (4.0f / 3.0f);
    return f > 1.0f ? f : 1.0f;
}

float GfxConfig::aspect_ratio() const {
    if (aspect == "16:9") return 16.0f / 9.0f;
    if (aspect == "21:9") return 64.0f / 27.0f;  // the "21:9" of 2560x1080 / 3440x1440 monitors
    return 0.0f;
}

static bool parse_wxh(const std::string& v, int& w, int& h) {
    size_t x = v.find('x');
    if (x == std::string::npos) return false;
    int a = std::atoi(v.substr(0, x).c_str()), b = std::atoi(v.substr(x + 1).c_str());
    if (a < 64 || b < 64 || a > 16384 || b > 16384) return false;
    w = a, h = b;
    return true;
}

static void publish() {
    target_aspect = gfx.aspect == "expand" ? -1.0f : gfx.aspect_ratio();
    hud_set_mode(gfx.hud != "original" && gfx.aspect != "original", gfx.aspect != "original");
    border_crop_set(gfx.border == "fill");
}

bool gfx_config_set(const std::string& key, const std::string& value) {
    if (key == "aspect") {
        if (value != "original" && value != "16:9" && value != "21:9" && value != "expand") return false;
        gfx.aspect = value;
    } else if (key == "window") {
        if (value == "auto") {
            gfx.window_w = 1280, gfx.window_h = 720, gfx.window_auto = true;
            return true;
        }
        if (!parse_wxh(value, gfx.window_w, gfx.window_h)) return false;
        gfx.window_auto = false;
        return true;
    } else if (key == "res_scale") {
        int n = std::atoi(value.c_str());
        if (n < 0 || n > 16) return false;
        gfx.res_scale = n;
        gfx.downsample = 1;
    } else if (key == "msaa") {
        int n = std::atoi(value.c_str());
        if (n != 0 && n != 2 && n != 4 && n != 8) return false;
        gfx.msaa = n;
    } else if (key == "refresh") {
        if (value == "original" || value == "0") gfx.refresh = 0;
        else if (value == "display") gfx.refresh = -1;
        else {
            int n = std::atoi(value.c_str());
            if (n < 30 || n > 1000) return false;
            gfx.refresh = n;
        }
    } else if (key == "hud") {
        if (value != "original" && value != "16:9" && value != "expand") return false;
        gfx.hud = value;
    } else if (key == "split") {
        if (value != "wide" && value != "original") return false;
        gfx.split_wide = value == "wide";
    } else if (key == "border") {
        if (value != "original" && value != "fill") return false;   // `crop` was its name before v0.2 shipped: no shim
        gfx.border = value;
    } else if (key == "draw_distance") {
        if (!dd::valid_mode(value)) return false;
        dd::set_mode(value);
        return true;
    } else if (key == "dd_buffers") {
        if (!dd::valid_buffers(value)) return false;
        dd::set_buffers(value);
        return true;
    } else if (key == "fullscreen") {
        gfx.fullscreen = value == "1";
    } else {
        return false;
    }
    publish();
    return true;
}

bool gfx_override(const std::string& key, const std::string& value) {
    GfxConfig saved = gfx;
    if (!gfx_config_set(key, value)) {
        gfx = saved;
        publish();
        return false;
    }
    g_overrides.emplace_back(key, value);
    return true;
}

void gfx_from_settings(const ultramodern::renderer::GraphicsConfig& c, const std::string& widescreen, bool keep_overrides) {
    using namespace ultramodern::renderer;
    int ds = c.ds_option > 1 ? c.ds_option : 1;
    switch (c.res_option) {
        case Resolution::Original: gfx.res_scale = ds, gfx.downsample = ds; break;
        case Resolution::Original2x: gfx.res_scale = 2 * ds, gfx.downsample = ds; break;
        default: gfx.res_scale = 0, gfx.downsample = 1; break;
    }
    gfx.aspect = c.ar_option == AspectRatio::Original ? "original" : (widescreen.empty() ? "expand" : widescreen);
    gfx.msaa = c.msaa_option == Antialiasing::MSAA8X ? 8 : c.msaa_option == Antialiasing::MSAA4X ? 4 : c.msaa_option == Antialiasing::MSAA2X ? 2 : 0;
    gfx.fullscreen = c.wm_option == WindowMode::Fullscreen;
    gfx.refresh = c.rr_option == RefreshRate::Display ? -1 : c.rr_option == RefreshRate::Manual ? c.rr_manual_value : 0;
    gfx.hpfb = c.hpfb_option == HighPrecisionFramebuffer::On ? 1 : c.hpfb_option == HighPrecisionFramebuffer::Off ? 0 : -1;
    if (keep_overrides) {
        for (auto& [k, v] : g_overrides) gfx_config_set(k, v);
    } else {
        g_overrides.clear();
    }
    publish();
}

void gfx_config_load(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string line;
    std::vector<std::pair<std::string, std::string>> kv;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        kv.emplace_back(k, v);
    }
    in.close();   // gfx_config_save below renames over this file: Windows refuses while it is open
    int rev = 1;
    for (auto& [k, v] : kv) {
        if (k == "cfg_rev") rev = std::atoi(v.c_str());
    }
    // A graphics.cfg written before Q18 (no refresh= line) that holds exactly the old defaults
    // (the host saved them on first start) is moved to the new defaults once, like controls.cfg
    // in Q19. Files with any value changed by the user are kept as they are.
    static const std::vector<std::pair<std::string, std::string>> legacy = {
        {"aspect", "original"}, {"window", "960x720"}, {"res_scale", "0"}, {"msaa", "0"}, {"fullscreen", "0"}};
    if (kv == legacy) {
        gfx_config_save(file);
        std::printf("[thps] gfx: %s had the old defaults (4:3, 960x720, no MSAA); updated to the Q18 defaults\n",
                    file.string().c_str());
        return;
    }
    bool migrate = rev < kCfgRev;
    for (auto& [k, v] : kv) {
        if (k == "cfg_rev") continue;
        if (k == "draw_distance" && (v == "far" || v == "max")) migrate = true;   // the old names: rewritten as 2x / 4x (src/draw_distance.cpp)
        if (migrate && k == "hud" && v == "original") {
            std::printf("[thps] gfx: %s has hud=original from before HUD Placement defaulted to %s; moved to the new default (choose Original again to keep it)\n",
                        file.string().c_str(), kDefaultHud);
            v = kDefaultHud;
        }
        if (!gfx_config_set(k, v)) std::fprintf(stderr, "[thps] %s: ignoring %s=%s\n", file.string().c_str(), k.c_str(), v.c_str());
    }
    if (migrate) gfx_config_save(file);   // stamp cfg_rev: from now on hud=original is the user's own choice; new names
}

void gfx_config_save(const std::filesystem::path& file) {
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        out << "# " << thps::game::short_name << " recomp host graphics settings (" << thps::game::family << "_recomp --aspect/--window/--res-scale/--msaa/--fullscreen/--refresh/--hud/--split)\n";
        out << "cfg_rev=" << kCfgRev << "\n";
        out << "aspect=" << gfx.aspect << "\n";
        out << "window=" << (gfx.window_auto ? std::string("auto") : std::to_string(gfx.window_w) + "x" + std::to_string(gfx.window_h)) << "\n";
        out << "res_scale=" << gfx.res_scale << "\n";
        out << "msaa=" << gfx.msaa << "\n";
        out << "fullscreen=" << (gfx.fullscreen ? 1 : 0) << "\n";
        if (hud_supported()) out << "hud=" << gfx.hud << "\n";
        out << "split=" << (gfx.split_wide ? "wide" : "original") << "\n";
        out << "border=" << gfx.border << "\n";
        if (dd::mode() != "original") out << "draw_distance=" << dd::mode() << "\n";
        if (dd::buffers() != "bigger") out << "dd_buffers=" << dd::buffers() << "\n";
        out << "refresh=" << (gfx.refresh < 0 ? std::string("display") : gfx.refresh == 0 ? std::string("original") : std::to_string(gfx.refresh)) << "\n";
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
}

std::string gfx_describe() {
    char buf[352];
    std::snprintf(buf, sizeof buf, "aspect=%s window=%dx%d%s res_scale=%d downsample=%d msaa=%d fullscreen=%d refresh=%d hpfb=%d hud=%s split=%s border=%s draw_distance=%s dd_buffers=%s",
                  gfx.aspect.c_str(), gfx.window_w, gfx.window_h, gfx.window_auto ? "(auto)" : "", gfx.res_scale, gfx.downsample, gfx.msaa, int(gfx.fullscreen),
                  gfx.refresh, gfx.hpfb, gfx.hud.c_str(), gfx.split_wide ? "wide" : "original", gfx.border.c_str(), dd::mode().c_str(), dd::buffers().c_str());
    return buf;
}

}  // namespace thps
