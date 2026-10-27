// In-game settings menu (THPS_MENU): see ui_host.h. Structure after Zelda64Recomp's src/ui/ui_state.cpp (GPL-3.0): an RmlUi
// context created in the render hook's init, SDL events queued from the main thread and fed to RmlUi inside
// the draw hook, one RmlUi render per frame over RT64's swap chain framebuffer.
#include "ui_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "RmlUi/Core.h"
#include "RmlUi/Core/Elements/ElementTabSet.h"
#include "RmlUi_Platform_SDL.h"
#include "rt64_render_hooks.h"
#include "ultramodern/config.hpp"

#include "../draw_distance.h"
#include "../hfr.h"
#include "../host.h"
#include "../platform.h"
#include "ui_renderer.h"

namespace thps::ui {
namespace {

std::atomic<bool> g_open{false};
std::atomic<bool> g_wanted{false};
SDL_Window* g_window = nullptr;
std::filesystem::path g_assets, g_config_dir;

std::mutex g_click_mutex;
std::string g_pending_click;          // --control "ui click #id": the render thread finds the element and pushes the SDL click
std::atomic<int> g_pending_tab{-1};   // --control "ui tab N": switch the tab on the render thread
std::mutex g_queue_mutex;
std::vector<SDL_Event> g_queue;
// Render thread -> "ui state" (control channel): what the menu shows right now.
std::mutex g_info_mutex;
std::string g_focus_id;
std::atomic<int> g_active_tab{0};
std::atomic<bool> g_hint_visible{false};
std::atomic<bool> g_hint_pending{false};
// The hint's window in steady_clock ticks (set when the renderer starts; 0 = none / used up). wanted() reads it too: a window RT64 does
// not present (hidden, occluded) composites no frame, so the draw hook would never run to notice that the hint is due.
std::atomic<int64_t> g_hint_from{0}, g_hint_until{0};
int64_t clock_ticks() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

// ---- option values as the menu shows them (strings: RmlUi radio buttons compare strings) -------------------

std::string gfx_aspect() { return thps::gfx.aspect; }
std::string gfx_hud() { return thps::gfx.hud; }
std::string gfx_split() { return thps::gfx.split_wide ? "wide" : "original"; }
std::string gfx_border() { return thps::gfx.border; }
std::string gfx_draw_distance() { return thps::dd::mode(); }
std::string gfx_msaa() { return std::to_string(thps::gfx.msaa); }
std::string gfx_window() { return thps::gfx.fullscreen ? "fullscreen" : "windowed"; }
std::string gfx_res_scale() { return std::to_string(thps::gfx.res_scale); }
std::string gfx_refresh() { return thps::gfx.refresh < 0 ? "display" : thps::gfx.refresh == 0 ? "original" : std::to_string(thps::gfx.refresh); }

// A graphics option changed: gfx_config_set (validates), graphics.cfg, then ask the gfx thread to apply it
// (RT64Context::update_config runs on the renderer thread; ultramodern triggers it from set_graphics_config).
void gfx_changed(const std::string& key, const std::string& value) {
    if (!thps::gfx_config_set(key, value)) return;
    thps::gfx_config_save(g_config_dir / "graphics.cfg");
    thps::settings::mark_changed();
    ultramodern::renderer::set_graphics_config(ultramodern::renderer::get_graphics_config());
}

// ---- Controls tab: binding rows, capture, the Controllers list (render thread; ui state reads a copy) ---------------------------

struct BindRow {
    Rml::String id, label, p0, p1, k0, k1;
    int cap = 0;   // 1-4: this row's slot (pad 0, pad 1, key 0, key 1) waits for a press
    bool m0 = false, m1 = false, m2 = false, m3 = false;   // that slot differs from the layout's default (drawn marked: ctl-slot--changed)
};
struct PadRow {
    Rml::String name, player_text, pressed;
    int player = 0;
    bool pressing = false;
    int id = -1;
};
std::vector<BindRow> g_rows;
std::vector<PadRow> g_pad_rows;
std::string g_bind_status;
Rml::DataModelHandle g_controls;

constexpr int k_capture_seconds = 5;
struct Capture {
    int row = -1;            // -1 = none
    bool keyboard = false;
    int slot = 0;
    std::chrono::steady_clock::time_point until{};
    int shown_left = -1;     // seconds left as last drawn
};
Capture g_cap;
std::atomic<bool> g_capturing{false};   // handle_event: Esc / Back / B go to the capture, not to the menu
uint64_t g_rows_version = 0;
std::string g_capture_info = "-";       // "ui state" capture=...: input:pad|key:slot (g_info_mutex)
std::string g_status_copy;              // "ui bindstatus" (g_info_mutex)
std::string g_marked_copy = "-";        // "ui state" marked=...: the ids of the slots that differ from the layout (g_info_mutex)
bool g_confirm_reset = false;           // Reset all was pressed once: a second press resets, anything else cancels
Rml::String g_reset_label = "Reset all to default";

// One field slot as the row shows it.
Rml::String slot_text(const std::vector<std::string>& fields, size_t i) {
    if (i >= fields.size()) return "-";
    std::string t = thps::input_field_label(fields[i]);
    if (i == 1 && fields.size() > 2) t += " +" + std::to_string(fields.size() - 2);
    return t;
}

void rebuild_rows() {
    const int n = thps::input_count();
    g_rows.resize(size_t(n));
    const int left = g_cap.row >= 0 ? g_cap.shown_left : 0;
    const std::string waiting = g_cap.keyboard ? "Press a key (" + std::to_string(left) + ")" : "Press a button (" + std::to_string(left) + ")";
    std::string marked;
    for (int i = 0; i < n; i++) {
        BindRow& r = g_rows[size_t(i)];
        const auto pad = thps::input_slots(i, false), keys = thps::input_slots(i, true);
        r.id = thps::input_id(i);
        r.label = thps::input_label(i);
        r.p0 = slot_text(pad, 0), r.p1 = slot_text(pad, 1), r.k0 = slot_text(keys, 0), r.k1 = slot_text(keys, 1);
        const auto dpad = thps::input_default_slots(i, false), dkeys = thps::input_default_slots(i, true);
        const auto field_at = [](const std::vector<std::string>& v, size_t k) { return k < v.size() ? v[k] : std::string(); };
        r.m0 = field_at(pad, 0) != field_at(dpad, 0), r.m1 = field_at(pad, 1) != field_at(dpad, 1);
        r.m2 = field_at(keys, 0) != field_at(dkeys, 0), r.m3 = field_at(keys, 1) != field_at(dkeys, 1);
        for (const auto& [m, prefix] : {std::pair{r.m0, "bp0_"}, std::pair{r.m1, "bp1_"}, std::pair{r.m2, "bk0_"}, std::pair{r.m3, "bk1_"}}) {
            if (m) marked += (marked.empty() ? "" : ",") + std::string(prefix) + r.id;
        }
        r.cap = 0;
        if (g_cap.row == i) {
            r.cap = (g_cap.keyboard ? 3 : 1) + g_cap.slot;
            (g_cap.keyboard ? (g_cap.slot ? r.k1 : r.k0) : (g_cap.slot ? r.p1 : r.p0)) = waiting;
        }
    }
    g_rows_version = thps::input_bindings_version();
    {
        std::lock_guard lock{g_info_mutex};
        g_marked_copy = marked.empty() ? "-" : marked;
    }
    if (g_controls) g_controls.DirtyVariable("rows");
}

// Reset all asks first: the first press arms it (the button says so), a second press resets, anything else disarms it.
void cancel_reset_confirm() {
    if (!g_confirm_reset) return;
    g_confirm_reset = false;
    g_reset_label = "Reset all to default";
    if (g_controls) g_controls.DirtyVariable("reset_all_label");
}

void set_status(const std::string& s) {
    g_bind_status = s;
    {
        std::lock_guard lock{g_info_mutex};
        g_status_copy = s;
    }
    if (g_controls) g_controls.DirtyVariable("bind_status");
    std::printf("[thps-ui] controls: %s\n", s.c_str());
    std::fflush(stdout);
}

void set_capture_info() {
    std::lock_guard lock{g_info_mutex};
    g_capture_info = g_cap.row < 0 ? "-" : thps::input_id(g_cap.row) + (g_cap.keyboard ? ":key:" : ":pad:") + std::to_string(g_cap.slot);
}

void start_capture(int row, bool keyboard, int slot) {
    if (row < 0 || row >= thps::input_count() || slot < 0 || slot > 1) return;
    cancel_reset_confirm();
    g_cap.row = row, g_cap.keyboard = keyboard, g_cap.slot = slot;
    g_cap.until = std::chrono::steady_clock::now() + std::chrono::seconds(k_capture_seconds);
    g_cap.shown_left = k_capture_seconds;
    g_capturing = true;
    set_capture_info();
    set_status(thps::input_label(row) + (keyboard ? ": press a key (Esc cancels, Backspace empties the slot)"
                                                  : ": press a button, trigger or stick direction (Back or Esc cancels, Backspace empties the slot)"));
    rebuild_rows();
}

void end_capture(const std::string& status) {
    g_cap.row = -1;
    g_capturing = false;
    set_capture_info();
    set_status(status);
    rebuild_rows();
}

void capture_field(const std::string& field) {
    const auto r = thps::input_rebind(g_cap.row, g_cap.keyboard, g_cap.slot, field);
    end_capture(r.ok ? r.message : r.reply);
}

// An event while a slot waits for a press. true = taken (it never reaches RmlUi or the menu).
bool capture_event(const SDL_Event& ev) {
    if (g_cap.row < 0) return false;
    switch (ev.type) {
        case SDL_KEYDOWN: {
            if (ev.key.repeat) return true;
            const SDL_Scancode sc = ev.key.keysym.scancode;
            if (sc == SDL_SCANCODE_ESCAPE) { end_capture("Cancelled: unchanged"); return true; }
            if (sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_DELETE) { capture_field("none"); return true; }
            if (!g_cap.keyboard) return true;   // a controller slot takes controller input only
            const char* name = SDL_GetScancodeName(sc);
            // controls.cfg separates fields with commas, so a key whose SDL name has one (Comma is ",") can't be written there.
            if (!name || !*name || std::string(name).find(',') != std::string::npos) end_capture("That key can't be bound (its name has a comma): unchanged");
            else capture_field(std::string("key:") + name);
            return true;
        }
        case SDL_CONTROLLERBUTTONDOWN: {
            const int b = ev.cbutton.button;
            if (b == SDL_CONTROLLER_BUTTON_BACK || (g_cap.keyboard && b == SDL_CONTROLLER_BUTTON_B)) { end_capture("Cancelled: unchanged"); return true; }
            if (g_cap.keyboard) return true;
            const char* name = SDL_GameControllerGetStringForButton(SDL_GameControllerButton(b));
            if (name) capture_field(std::string("button:") + name);
            return true;
        }
        case SDL_CONTROLLERAXISMOTION: {
            if (g_cap.keyboard) return true;
            const int a = ev.caxis.axis, v = ev.caxis.value;
            const bool trigger = a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
            // Triggers past half way; sticks past three quarters (a resting or drifting stick never binds).
            if (trigger ? v < 16384 : std::abs(v) < 24576) return true;
            const char* name = SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis(a));
            if (name) capture_field(std::string("axis:") + name + (trigger || v > 0 ? "+" : "-"));
            return true;
        }
        case SDL_MOUSEBUTTONDOWN:
            end_capture("Cancelled: unchanged");
            return true;
        case SDL_MOUSEMOTION:
            return false;
        default:
            return true;
    }
}

void refresh_pads() {
    std::vector<PadRow> rows;
    for (const auto& p : thps::input_pads()) {
        PadRow r;
        r.name = p.name;
        r.player = p.player;
        r.player_text = p.player ? "Player " + std::to_string(p.player) : "No player yet";
        r.pressing = !p.pressed.empty();
        r.pressed = r.pressing ? p.pressed : "-";
        r.id = p.id;
        rows.push_back(std::move(r));
    }
    auto same = [](const PadRow& a, const PadRow& b) {
        return a.name == b.name && a.player == b.player && a.pressed == b.pressed && a.id == b.id;
    };
    if (rows.size() == g_pad_rows.size() && std::equal(rows.begin(), rows.end(), g_pad_rows.begin(), same)) return;
    g_pad_rows = std::move(rows);
    if (g_controls) g_controls.DirtyVariable("pads");
}

// ---- RmlUi side (render thread only) -------------------------------------------------------------------------

struct UiState {
    std::unique_ptr<SystemInterface_SDL> system_interface;
    thpsui::RmlRenderInterface_RT64 render_interface;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* menu = nullptr;
    Rml::ElementDocument* hint = nullptr;
    bool menu_shown = false;
    std::chrono::steady_clock::time_point hint_from{}, hint_until{};
    int last_w = 0, last_h = 0;
    int latest_cont_key = 0;
    std::chrono::steady_clock::time_point next_repeat{};
};
std::unique_ptr<UiState> g_ui;

// Click handlers named in the .rml (onclick="close_menu").
using Handler = void (*)(Rml::Event&);
class Listener : public Rml::EventListener {
  public:
    explicit Listener(Handler h) : h_(h) {}
    void ProcessEvent(Rml::Event& e) override { h_(e); }
    Handler h_;
};
class ListenerInstancer : public Rml::EventListenerInstancer {
  public:
    Rml::EventListener* InstanceEventListener(const Rml::String& value, Rml::Element*) override {
        auto it = listeners_.find(value);
        if (it != listeners_.end()) return &it->second;
        auto h = handlers_.find(value);
        if (h == handlers_.end()) return nullptr;
        return &listeners_.emplace(value, Listener{h->second}).first->second;
    }
    void add(const std::string& name, Handler h) { handlers_[name] = h; }

  private:
    std::unordered_map<std::string, Handler> handlers_;
    std::unordered_map<std::string, Listener> listeners_;
};
ListenerInstancer g_instancer;

void close_from_ui(Rml::Event&) { g_open = false; }

// A left click at window point (x, y): motion, press, release, as SDL would deliver them (SDL_PushEvent is thread-safe).
void push_click(int x, int y) {
    SDL_Event m{};
    m.type = SDL_MOUSEMOTION;
    m.motion.x = x, m.motion.y = y;
    SDL_PushEvent(&m);
    SDL_Event d{};
    d.type = SDL_MOUSEBUTTONDOWN;
    d.button.button = SDL_BUTTON_LEFT, d.button.state = SDL_PRESSED, d.button.clicks = 1, d.button.x = x, d.button.y = y;
    SDL_PushEvent(&d);
    SDL_Event u = d;
    u.type = SDL_MOUSEBUTTONUP;
    u.button.state = SDL_RELEASED;
    SDL_PushEvent(&u);
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// string-valued option bound to getter/setter functions
void bind_string(Rml::DataModelConstructor& c, const char* name, std::string (*get)(), void (*set)(const std::string&)) {
    c.BindFunc(
        name, [get](Rml::Variant& v) { v = Rml::String(get()); }, [set](const Rml::Variant& v) { set(v.Get<Rml::String>()); });
}
void bind_onoff(Rml::DataModelConstructor& c, const char* name, bool (*get)(), void (*set)(bool)) {
    c.BindFunc(
        name, [get](Rml::Variant& v) { v = Rml::String(get() ? "On" : "Off"); },
        [set](const Rml::Variant& v) {
            set(v.Get<Rml::String>() == "On");
            thps::settings::save();
            thps::settings::mark_changed();
        });
}

std::vector<Rml::DataModelHandle> g_handles;   // render thread: every model, to refresh the radio buttons when the menu opens

void build_models(Rml::Context* ctx) {
    // Constants for the page (game name from the profile, game/game.toml).
    if (auto c = ctx->CreateDataModel("app_model")) {
        g_handles.push_back(c.GetModelHandle());
        c.BindFunc("game_title", [](Rml::Variant& v) { v = Rml::String(thps::game::rom_title_short); });
    }
    if (auto c = ctx->CreateDataModel("general_model")) {
        g_handles.push_back(c.GetModelHandle());
        bind_onoff(c, "skip_intro", [] { return thps::settings::skip_intro(); }, [](bool v) { thps::settings::set_skip_intro(v); });
        bind_onoff(c, "show_fps", [] { return thps::settings::show_fps(); }, [](bool v) { thps::settings::set_show_fps(v); });
    }
    if (auto c = ctx->CreateDataModel("controls_model")) {
        g_handles.push_back(c.GetModelHandle());
        // The layout presets of input.cpp (controls.cfg "preset = ..."); an edited controls.cfg reads "custom" and checks neither.
        bind_string(c, "preset", [] { return thps::input_preset_name(); },
                    [](const std::string& v) {
                        cancel_reset_confirm();
                        const std::string r = thps::input_set_preset(v);
                        std::printf("[thps] settings: controls preset %s -> %s\n", v.c_str(), r.c_str());
                        std::fflush(stdout);
                        if (g_controls) g_controls.DirtyVariable("preset_is_custom");
                    });
        c.BindFunc("config_dir", [](Rml::Variant& v) { v = Rml::String(thps::platform::to_utf8(g_config_dir)); });
        c.BindFunc("preset_is_custom", [](Rml::Variant& v) { v = thps::input_preset_name() == "custom"; });
        // Binding rows (rebinding) and the Controllers list.
        if (auto r = c.RegisterStruct<BindRow>()) {
            r.RegisterMember("id", &BindRow::id);
            r.RegisterMember("label", &BindRow::label);
            r.RegisterMember("p0", &BindRow::p0);
            r.RegisterMember("p1", &BindRow::p1);
            r.RegisterMember("k0", &BindRow::k0);
            r.RegisterMember("k1", &BindRow::k1);
            r.RegisterMember("cap", &BindRow::cap);
            r.RegisterMember("m0", &BindRow::m0);
            r.RegisterMember("m1", &BindRow::m1);
            r.RegisterMember("m2", &BindRow::m2);
            r.RegisterMember("m3", &BindRow::m3);
        }
        c.RegisterArray<std::vector<BindRow>>();
        if (auto r = c.RegisterStruct<PadRow>()) {
            r.RegisterMember("name", &PadRow::name);
            r.RegisterMember("player_text", &PadRow::player_text);
            r.RegisterMember("pressed", &PadRow::pressed);
            r.RegisterMember("player", &PadRow::player);
            r.RegisterMember("pressing", &PadRow::pressing);
        }
        c.RegisterArray<std::vector<PadRow>>();
        rebuild_rows();
        g_bind_status = "Choose a slot, then press the new button or key.";
        c.Bind("rows", &g_rows);
        c.Bind("pads", &g_pad_rows);
        c.Bind("bind_status", &g_bind_status);
        c.Bind("reset_all_label", &g_reset_label);
        c.BindEventCallback("reset_all", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (g_cap.row >= 0) end_capture("Cancelled: unchanged");
            const std::string base = thps::input_base_preset();
            const std::string layout = base == "n64" ? "N64 layout" : "face-button layout";
            if (!g_confirm_reset) {
                g_confirm_reset = true;
                g_reset_label = "Confirm: reset all";
                if (g_controls) g_controls.DirtyVariable("reset_all_label");
                set_status("Put every binding back to the " + layout + "? Choose Confirm to do it; anything else cancels.");
                return;
            }
            cancel_reset_confirm();
            const std::string r = thps::input_set_preset(base);
            std::printf("[thps] settings: controls reset all -> %s\n", r.c_str());
            std::fflush(stdout);
            rebuild_rows();
            if (g_controls) g_controls.DirtyVariable("preset"), g_controls.DirtyVariable("preset_is_custom");
            set_status(r.rfind("ok", 0) == 0 ? "Every binding is back to the " + layout + " (controls.cfg.bak keeps the old file)" : r);
        });
        c.BindEventCallback("rebind", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (args.size() < 3) return;
            start_capture(args[0].Get<int>(), args[1].Get<int>() != 0, args[2].Get<int>());
        });
        c.BindEventCallback("reset_row", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (args.empty()) return;
            if (g_cap.row >= 0) end_capture("Cancelled: unchanged");
            cancel_reset_confirm();
            const auto r = thps::input_reset_input(args[0].Get<int>());
            set_status(r.ok ? r.message : r.reply);
            rebuild_rows();
        });
        c.BindEventCallback("set_player", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (args.size() < 2) return;
            const int i = args[0].Get<int>(), player = args[1].Get<int>();
            if (i < 0 || i >= int(g_pad_rows.size()) || player < 1 || player > 2) return;
            cancel_reset_confirm();
            thps::input_assign_port(g_pad_rows[size_t(i)].id, player - 1);
            set_status(g_pad_rows[size_t(i)].name + " is player " + std::to_string(player));
        });
        c.BindEventCallback("swap_players", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            cancel_reset_confirm();
            thps::input_swap_ports();
            set_status("Players 1 and 2 swapped");
        });
        g_controls = c.GetModelHandle();
    }
    if (auto c = ctx->CreateDataModel("graphics_model")) {
        g_handles.push_back(c.GetModelHandle());
        bind_string(c, "aspect", gfx_aspect, [](const std::string& v) { gfx_changed("aspect", v); });
        bind_string(c, "hud", gfx_hud, [](const std::string& v) { gfx_changed("hud", v); });
        c.BindFunc("show_hud", [](Rml::Variant& v) { v = thps::hud_supported(); });
        bind_string(c, "split", gfx_split, [](const std::string& v) { gfx_changed("split", v); });
        bind_string(c, "border", gfx_border, [](const std::string& v) { gfx_changed("border", v); });
        bind_string(c, "draw_distance", gfx_draw_distance, [](const std::string& v) { gfx_changed("draw_distance", v); });   // live (src/draw_distance.cpp)
        c.BindFunc("show_dd", [](Rml::Variant& v) { v = thps::dd::supported(); });   // only where the setting changes something
        bind_string(c, "msaa", gfx_msaa, [](const std::string& v) { gfx_changed("msaa", v); });
        bind_string(c, "window_mode", gfx_window, [](const std::string& v) { gfx_changed("fullscreen", v == "fullscreen" ? "1" : "0"); });
        bind_string(c, "res_scale", gfx_res_scale, [](const std::string& v) { gfx_changed("res_scale", v); });
        bind_string(c, "refresh", gfx_refresh, [](const std::string& v) { gfx_changed("refresh", v); });
        c.BindFunc("show_msaa8", [](Rml::Variant& v) {
#ifdef __APPLE__
            v = false;   // Apple GPUs have no 8x MSAA (RT64 would turn it off)
#else
            v = true;
#endif
        });
    }
    if (auto c = ctx->CreateDataModel("sound_model")) {
        g_handles.push_back(c.GetModelHandle());
        c.BindFunc(
            "main_volume", [](Rml::Variant& v) { v = int(thps::settings::volume() * 100.0f + 0.5f); },
            [](const Rml::Variant& v) {
                thps::settings::set_volume_percent(v.Get<int>());
                thps::settings::save();
            });
    }
}

// Cheats tab (src/cheats.cpp): one On/Off row per cheat the game has (has_<key>), its title and what it does / when it takes effect.
void build_cheats_model(Rml::Context* ctx) {
    auto c = ctx->CreateDataModel("cheats_model");
    if (!c) return;
    g_handles.push_back(c.GetModelHandle());
    for (int i = 0; i < thps::cheats::kCount; i++) {
        const auto id = thps::cheats::Id(i);
        const std::string key = thps::cheats::info(id).key;
        c.BindFunc("has_" + key, [id](Rml::Variant& v) { v = thps::cheats::available(id); });
        c.BindFunc("title_" + key, [id](Rml::Variant& v) { v = Rml::String(thps::cheats::info(id).title); });
        c.BindFunc("note_" + key, [id](Rml::Variant& v) { v = Rml::String(thps::cheats::game_note(id)); });
        c.BindFunc(
            key, [id](Rml::Variant& v) { v = Rml::String(thps::cheats::on(id) ? "On" : "Off"); },
            [id](const Rml::Variant& v) { thps::cheats::set(id, v.Get<Rml::String>() == "On"); });
    }
}

void init_hook(plume::RenderInterface* iface, plume::RenderDevice* device) {
    g_ui = std::make_unique<UiState>();
    UiState& ui = *g_ui;
    ui.system_interface = std::make_unique<SystemInterface_SDL>();
    ui.system_interface->SetWindow(g_window);
    ui.render_interface.init(iface, device);

    g_instancer.add("close_menu", close_from_ui);
    Rml::SetSystemInterface(ui.system_interface.get());
    Rml::SetRenderInterface(ui.render_interface.get_rml_interface());
    Rml::Factory::RegisterEventListenerInstancer(&g_instancer);
    Rml::Initialise();

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    ui.context = Rml::CreateContext("thps-ui", Rml::Vector2i(std::max(w, 1), std::max(h, 1)));

    // Faces: the OFL fonts that tools/setup_runtime.py fetched (ui/FONTS-LICENSE.txt), staged as <resources>/ui/fonts by tools/menu_assets.py.
    struct Face {
        const char* file;
        bool fallback;
    };
    const Face faces[] = {{"fonts/LatoLatin-Regular.ttf", false}, {"fonts/LatoLatin-Italic.ttf", false}, {"fonts/LatoLatin-Bold.ttf", false},
                          {"fonts/LatoLatin-BoldItalic.ttf", false}, {"fonts/NotoEmoji-Regular.ttf", true}, {"fonts/promptfont.ttf", false}};
    for (const Face& f : faces) {
        if (!Rml::LoadFontFace((g_assets / f.file).string(), f.fallback)) std::fprintf(stderr, "[thps-ui] font missing: %s\n", f.file);
    }

    build_models(ui.context);
    build_cheats_model(ui.context);
    ui.menu = ui.context->LoadDocument((g_assets / "settings.rml").string());
    if (!ui.menu) std::fprintf(stderr, "[thps-ui] cannot load %s\n", (g_assets / "settings.rml").string().c_str());
    ui.hint = ui.context->LoadDocument((g_assets / "hint.rml").string());

    // First launch: a toast for a few seconds ("Press Esc for Settings"), once per config folder
    // (<config>/ui-hint-shown; THPS_MENU_HINT=1 forces it, for tests and screenshots).
    std::error_code ec;
    const auto flag = g_config_dir / "ui-hint-shown";
    const char* force = std::getenv("THPS_MENU_HINT");
    if ((force && *force == '1') || !std::filesystem::exists(flag, ec)) {
        // from 3 s (the game is booting before that) to 15 s after RT64 came up
        ui.hint_from = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        ui.hint_until = ui.hint_from + std::chrono::seconds(12);
        if (!force) std::ofstream(flag) << "1\n";
        g_hint_pending = true;
        g_hint_from = ui.hint_from.time_since_epoch().count();
        g_hint_until = ui.hint_until.time_since_epoch().count();
    }
}

void deinit_hook() {
    if (!g_ui) return;
    g_handles.clear();
    Rml::Shutdown();
    g_ui->render_interface.reset();
    g_ui.reset();
}

int cont_button_to_key(const SDL_ControllerButtonEvent& b) {
    switch (b.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return SDLK_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return SDLK_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return SDLK_LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return SDLK_RIGHT;
        case SDL_CONTROLLER_BUTTON_A: return SDLK_RETURN;
        default: return 0;
    }
}

// A pad button "press" on the focused element: radio buttons, tabs and buttons take a click (Enter alone does nothing on a radio).
bool activate_focus(UiState& ui) {
    Rml::Element* f = ui.context->GetFocusElement();
    if (!f || f == ui.context->GetRootElement() || f->GetTagName() == "body") return false;
    if (f->GetTagName() == "input" && f->GetAttribute<Rml::String>("type", "") == "range") return false;   // a slider moves with left / right
    f->Click();
    return true;
}

// Activate tab `index` and move the focus to its header: the focus must not stay on an element of the panel that was just hidden.
void set_tab(UiState& ui, int index) {
    static const char* const ids[] = {"tab_general", "tab_controls", "tab_graphics", "tab_sound", "tab_cheats"};
    auto* tabs = ui.menu ? rmlui_dynamic_cast<Rml::ElementTabSet*>(ui.menu->GetElementById("config_tabset")) : nullptr;
    if (!tabs || index < 0 || index >= tabs->GetNumTabs()) return;
    tabs->SetActiveTab(index);
    if (index < int(sizeof ids / sizeof *ids))
        if (Rml::Element* t = ui.menu->GetElementById(ids[index])) t->Focus();
}

void switch_tab(UiState& ui, int delta) {
    auto* tabs = ui.menu ? rmlui_dynamic_cast<Rml::ElementTabSet*>(ui.menu->GetElementById("config_tabset")) : nullptr;
    if (!tabs) return;
    const int n = tabs->GetNumTabs();
    if (n > 0) set_tab(ui, (tabs->GetActiveTab() + delta + n) % n);
}

// RmlUi's spatial navigation looks only inside the focused element's nearest scroll container and never enters another one, so
// in the Controls tab (a scrolling list) Up / Down could neither go from the tab bar into the list nor back out of it; and from a
// row of radio buttons whose labels wrap differently it took the row's other radio for "below". When an Up / Down press left the
// focus where it was or in the same row (same parent), search the whole visible menu again, rows first: the distance along the
// direction plus a tenth of the sideways gap (RmlUi weighs the gap 10000x, which skips a row whose buttons sit to one side), and
// scroll the element found into view.
void collect_focusable(Rml::Element* e, std::vector<Rml::Element*>& out) {
    for (int i = 0; i < e->GetNumChildren(true); i++) {   // with non-DOM children: a tabset keeps its tabs and panels there
        Rml::Element* c = e->GetChild(i);
        if (!c->IsVisible()) continue;
        const auto& cv = c->GetComputedValues();
        if (cv.focus() == Rml::Style::Focus::None) continue;
        if (cv.tab_index() == Rml::Style::TabIndex::Auto) out.push_back(c);
        collect_focusable(c, out);
    }
}

bool nav_fallback(UiState& ui, Rml::Element* from, bool down) {
    if (!from || !ui.menu || from == ui.context->GetRootElement() || from->GetTagName() == "body") return false;
    std::vector<Rml::Element*> all;
    collect_focusable(ui.menu, all);
    auto box = [](Rml::Element* e) {
        const Rml::Vector2f p = e->GetAbsoluteOffset(Rml::BoxArea::Border);
        return std::pair{p, p + e->GetBox().GetSize(Rml::BoxArea::Border)};
    };
    const auto [a0, a1] = box(from);
    Rml::Element* best = nullptr;
    float best_h = 0;
    for (Rml::Element* e : all) {
        if (e == from || e->GetParentNode() == from->GetParentNode()) continue;
        const auto [b0, b1] = box(e);
        const float main = down ? b0.y - a1.y : a0.y - b1.y;
        if (main < 0) continue;
        const float h = main + 0.1f * (std::max(0.0f, b0.x - a1.x) + std::max(0.0f, a0.x - b1.x));
        if (!best || h < best_h) best = e, best_h = h;
    }
    if (!best) return false;
    best->Focus(true);
    best->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest));
    return true;
}

// A navigation key (pad D-pad, stick, arrows) for RmlUi, with the fallback above for Up / Down.
void nav_key_down(UiState& ui, int sdl_key) {
    Rml::Element* before = ui.context->GetFocusElement();
    ui.context->ProcessKeyDown(RmlSDL::ConvertKey(sdl_key), 0);
    if ((sdl_key != SDLK_UP && sdl_key != SDLK_DOWN) || !before) return;
    Rml::Element* after = ui.context->GetFocusElement();
    if (after != before && !(after && after->GetParentNode() == before->GetParentNode())) return;
    if (!nav_fallback(ui, before, sdl_key == SDLK_DOWN) && after != before) before->Focus(true);   // nothing further: stay put
}

void pad_button_down(UiState& ui, int button) {
    using clock = std::chrono::steady_clock;
    if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) return switch_tab(ui, -1);
    if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) return switch_tab(ui, +1);
    if (button == SDL_CONTROLLER_BUTTON_A) { activate_focus(ui); return; }
    SDL_ControllerButtonEvent b{};
    b.button = Uint8(button);
    const int k = cont_button_to_key(b);
    if (!k) return;
    nav_key_down(ui, k);
    ui.latest_cont_key = k;
    ui.next_repeat = clock::now() + std::chrono::milliseconds(500);
}

void pad_button_up(UiState& ui, int button) {
    SDL_ControllerButtonEvent b{};
    b.button = Uint8(button);
    const int k = cont_button_to_key(b);
    if (k && k == ui.latest_cont_key) {
        ui.context->ProcessKeyUp(RmlSDL::ConvertKey(k), 0);
        ui.latest_cont_key = 0;
    }
}

// The left stick navigates like the d-pad (with hysteresis so a drifting stick does not).
int stick_dir_button(int axis, int value, int& state) {
    if (value > 16000) state = +1; else if (value < -16000) state = -1; else if (std::abs(value) < 8000) state = 0;
    return state;
}

// A left press on a range input (the volume slider) beside its 2dp track line: RmlUi moves the knob only for a press on the
// slidertrack element itself, so a click next to the thin line did nothing. Set the value from the pointer's x along the track
// and send the same change event RmlUi's own slider sends (the data binding reads its "value"). Presses on the track or the knob
// are RmlUi's (the hover element is then the track or the bar, not the input).
void range_click(UiState& ui, float px) {
    Rml::Element* el = ui.context->GetHoverElement();
    if (!el || el->GetTagName() != "input" || el->GetAttribute<Rml::String>("type", "") != "range") return;
    Rml::Element* track = nullptr;
    for (int i = 0; i < el->GetNumChildren(true); i++)
        if (el->GetChild(i)->GetTagName() == "slidertrack") track = el->GetChild(i);
    if (!track) return;
    const float left = track->GetAbsoluteOffset(Rml::BoxArea::Content).x, width = track->GetBox().GetSize(Rml::BoxArea::Content).x;
    if (width <= 0) return;
    const float lo = el->GetAttribute<float>("min", 0.0f), hi = el->GetAttribute<float>("max", 100.0f), step = el->GetAttribute<float>("step", 1.0f);
    float v = lo + std::clamp((px - left) / width, 0.0f, 1.0f) * (hi - lo);
    if (step > 0) v = lo + std::round((v - lo) / step) * step;
    if (v == el->GetAttribute<float>("value", lo)) return;
    el->SetAttribute("value", v);
    Rml::Dictionary parameters;
    parameters["value"] = v;
    el->DispatchEvent(Rml::EventId::Change, parameters);
}

void draw_hook(plume::RenderCommandList* list, plume::RenderFramebuffer* swap_chain_fb) {
    if (!g_ui || !g_ui->context) return;
    UiState& ui = *g_ui;
    using clock = std::chrono::steady_clock;

    const bool open = g_open.load();
    if (open != ui.menu_shown) {
        ui.menu_shown = open;
        if (ui.menu) {
            if (open) {
                cancel_reset_confirm();
                rebuild_rows();
                refresh_pads();
                for (auto& h : g_handles) h.DirtyAllVariables();   // values may have changed since (cfg edits, --control)
                ui.menu->Show();
                set_tab(ui, 0);   // always opens on General, with the focus on the tab bar (pad / keyboard navigation starts there)
            } else {
                if (g_cap.row >= 0) end_capture("Cancelled: unchanged");
                ui.menu->Hide();
            }
        }
        if (ui.latest_cont_key) {
            ui.context->ProcessKeyUp(RmlSDL::ConvertKey(ui.latest_cont_key), 0);
            ui.latest_cont_key = 0;
        }
        if (open && ui.hint) ui.hint->Hide();
        if (open) {
            ui.hint_until = clock::time_point{};
            g_hint_until = 0;
        }
    }

    std::string click_id;
    {
        std::lock_guard lock{g_click_mutex};
        click_id.swap(g_pending_click);
    }
    if (!click_id.empty() && ui.menu) {
        if (Rml::Element* el = ui.menu->GetElementById(click_id)) {
            // A person scrolls to it first (the Controls tab's list is longer than the window).
            el->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest));
            // Element centre in context pixels -> window points -> real SDL events (same path as a mouse).
            const Rml::Vector2f c = el->GetAbsoluteOffset(Rml::BoxArea::Border) + el->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
            int ww2 = 1, wh2 = 1, pw2 = 1, ph2 = 1;
            SDL_GetWindowSize(g_window, &ww2, &wh2);
            SDL_GetWindowSizeInPixels(g_window, &pw2, &ph2);
            const int x = int(c.x * float(ww2) / float(pw2)), y = int(c.y * float(wh2) / float(ph2));
            std::printf("[thps-ui] click #%s at %d,%d (window points)\n", click_id.c_str(), x, y);
            std::fflush(stdout);
            push_click(x, y);
        } else {
            std::printf("[thps-ui] click: no element #%s\n", click_id.c_str());
            std::fflush(stdout);
        }
    }
    if (int t = g_pending_tab.exchange(-1); t >= 0 && ui.menu) set_tab(ui, t);

    std::vector<SDL_Event> events;
    {
        std::lock_guard lock{g_queue_mutex};
        events.swap(g_queue);
    }
    int ww = 1, wh = 1, pw = 1, ph = 1;
    SDL_GetWindowSize(g_window, &ww, &wh);
    SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    const float sx = ww > 0 ? float(pw) / float(ww) : 1.0f, sy = wh > 0 ? float(ph) / float(wh) : 1.0f;
    static int stick_x = 0, stick_y = 0;
    for (SDL_Event& ev : events) {
        if (!open) continue;
        if (capture_event(ev)) continue;   // a binding slot waits for a press
        if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
            pad_button_down(ui, ev.cbutton.button);
            continue;
        }
        if (ev.type == SDL_CONTROLLERBUTTONUP) {
            pad_button_up(ui, ev.cbutton.button);
            continue;
        }
        if (ev.type == SDL_CONTROLLERAXISMOTION) {
            const bool x_axis = ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX, y_axis = ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY;
            if (!x_axis && !y_axis) continue;
            int& st = x_axis ? stick_x : stick_y;
            const int before = st;
            stick_dir_button(ev.caxis.axis, ev.caxis.value, st);
            if (st == before) continue;
            const int pos = x_axis ? SDL_CONTROLLER_BUTTON_DPAD_RIGHT : SDL_CONTROLLER_BUTTON_DPAD_DOWN;
            const int neg = x_axis ? SDL_CONTROLLER_BUTTON_DPAD_LEFT : SDL_CONTROLLER_BUTTON_DPAD_UP;
            if (before != 0) pad_button_up(ui, before > 0 ? pos : neg);
            if (st != 0) pad_button_down(ui, st > 0 ? pos : neg);
            continue;
        }
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat && (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER || ev.key.keysym.sym == SDLK_SPACE)) {
            if (activate_focus(ui)) continue;   // Enter / Space on a radio button, tab or button (RmlUi alone only does it for some)
        }
        if (ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_UP || ev.key.keysym.sym == SDLK_DOWN) && !(ev.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI))) {
            nav_key_down(ui, ev.key.keysym.sym);   // arrows: as the D-pad (with the scroll-container fallback)
            continue;
        }
        // The SDL backend takes window points; the context is in pixels (Retina: 2x).
        if (ev.type == SDL_MOUSEMOTION) { ev.motion.x = int(ev.motion.x * sx); ev.motion.y = int(ev.motion.y * sy); }
        RmlSDL::InputEventHandler(ui.context, ev);
        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) range_click(ui, float(ev.button.x) * sx);
        if (ev.type == SDL_MOUSEBUTTONDOWN && std::getenv("THPS_MENU_DEBUG")) {
            Rml::Element* hv = ui.context->GetHoverElement();
            const Rml::Vector2i mp = ui.context->GetDimensions();
            Rml::Element* at = ui.context->GetElementAtPoint(Rml::Vector2f(float(ev.button.x) * sx, float(ev.button.y) * sy));
            std::printf("[thps-ui] mouse down at %d,%d (scale %.2f,%.2f, context %dx%d, sdl window %dx%d, pixels %dx%d): hover=%s#%s, at-point=%s#%s\n",
                        ev.button.x, ev.button.y, sx, sy, mp.x, mp.y, ww, wh, pw, ph, hv ? hv->GetTagName().c_str() : "(none)",
                        hv ? hv->GetId().c_str() : "", at ? at->GetTagName().c_str() : "(none)", at ? at->GetId().c_str() : "");
            std::fflush(stdout);
        }
    }
    if (open) {
        if (g_cap.row >= 0) {
            const auto now = clock::now();
            if (now >= g_cap.until) end_capture("No input in " + std::to_string(k_capture_seconds) + " seconds: unchanged");
            else if (int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(g_cap.until - now).count() / 1000) + 1; left != g_cap.shown_left) {
                g_cap.shown_left = left;
                rebuild_rows();
            }
        }
        if (thps::input_bindings_version() != g_rows_version) {   // a rebind, a preset, --control bind
            rebuild_rows();
            if (g_controls) g_controls.DirtyVariable("preset"), g_controls.DirtyVariable("preset_is_custom");
        }
        refresh_pads();
    }
    if (open && ui.latest_cont_key && clock::now() >= ui.next_repeat) {
        nav_key_down(ui, ui.latest_cont_key);
        ui.next_repeat += std::chrono::milliseconds(50);
    }

    // One-time hint toast: shown for a few seconds from the first drawn frame (see hint.rml), never while the menu is up.
    const bool hint_on = !open && ui.hint && clock::now() >= ui.hint_from && ui.hint_until > clock::now();
    if (ui.hint) {
        if (hint_on && !ui.hint->IsVisible()) ui.hint->Show();
        if (!hint_on && ui.hint->IsVisible()) ui.hint->Hide();
    }
    g_hint_visible = hint_on;
    g_wanted = open || hint_on;
    if (open && ui.menu) {   // for "ui state"
        std::lock_guard lock{g_info_mutex};
        Rml::Element* f = ui.context->GetFocusElement();
        g_focus_id = f ? f->GetId() : "";
        if (auto* tabs = rmlui_dynamic_cast<Rml::ElementTabSet*>(ui.menu->GetElementById("config_tabset"))) g_active_tab = tabs->GetActiveTab();
    }
    if (!open && !hint_on) return;

    const int w = int(swap_chain_fb->getWidth()), h = int(swap_chain_fb->getHeight());
    ui.context->SetDensityIndependentPixelRatio(float(h) / 1080.0f);  // 1080 lines = the design height, as in Zelda64Recomp
    if (w != ui.last_w || h != ui.last_h) {
        ui.context->SetDimensions({w, h});
        ui.last_w = w, ui.last_h = h;
    }
    ui.render_interface.start(list, w, h);
    ui.context->Update();
    ui.context->Render();
    ui.render_interface.end(list, swap_chain_fb);
}

}  // namespace

void set_render_hooks() { RT64::SetRenderHooks(init_hook, draw_hook, deinit_hook); }

void set_window(SDL_Window* window) { g_window = window; }

void init(const std::filesystem::path& assets, const std::filesystem::path& config_dir) {
    g_assets = assets;
    g_config_dir = config_dir;
}

// "ui state": the cheats that are on, comma-joined without spaces ("-" = none), e.g. cheats=unlock_levels,slow_motion
std::string cheats_state() {
    std::string s = thps::cheats::active_list();
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    return s.empty() ? "-" : s;
}

std::string control(const std::string& args) {
    std::istringstream in(args);
    std::string cmd;
    in >> cmd;
    if (cmd == "esc") {
        SDL_Event e{};
        e.type = SDL_KEYDOWN;
        e.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
        e.key.keysym.sym = SDLK_ESCAPE;
        SDL_PushEvent(&e);
        return "ok ui esc";
    }
    if (cmd == "click") {
        std::string a;
        in >> a;
        if (!a.empty() && a[0] == '#') {
            std::lock_guard lock{g_click_mutex};
            g_pending_click = a.substr(1);
            return "ok ui click " + a;
        }
        int x = std::atoi(a.c_str()), y = 0;
        in >> y;
        push_click(x, y);
        return "ok ui click";
    }
    if (cmd == "tab") {
        int t = 0;
        in >> t;
        g_pending_tab = t;
        return "ok ui tab";
    }
    if (cmd == "pad") {   // a controller button press (SDL_CONTROLLERBUTTON events: the path a real pad takes)
        std::string name;
        in >> name;
        static const std::pair<const char*, int> names[] = {
            {"back", SDL_CONTROLLER_BUTTON_BACK}, {"start", SDL_CONTROLLER_BUTTON_START}, {"up", SDL_CONTROLLER_BUTTON_DPAD_UP},
            {"down", SDL_CONTROLLER_BUTTON_DPAD_DOWN}, {"left", SDL_CONTROLLER_BUTTON_DPAD_LEFT}, {"right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
            {"a", SDL_CONTROLLER_BUTTON_A}, {"b", SDL_CONTROLLER_BUTTON_B}, {"lb", SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
            {"rb", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}, {"x", SDL_CONTROLLER_BUTTON_X}, {"y", SDL_CONTROLLER_BUTTON_Y}};
        for (const auto& [n, b] : names) {
            if (name != n) continue;
            SDL_Event d{};
            d.type = SDL_CONTROLLERBUTTONDOWN;
            d.cbutton.button = Uint8(b);
            d.cbutton.state = SDL_PRESSED;
            SDL_PushEvent(&d);
            SDL_Event u = d;
            u.type = SDL_CONTROLLERBUTTONUP;
            u.cbutton.state = SDL_RELEASED;
            SDL_PushEvent(&u);
            return "ok ui pad " + name;
        }
        return "err ui pad back|start|up|down|left|right|a|b|x|y|lb|rb";
    }
    if (cmd == "key") {   // a keyboard key press (arrows, return, tab, space)
        std::string name;
        in >> name;
        static const std::pair<const char*, std::pair<int, int>> names[] = {
            {"up", {SDLK_UP, SDL_SCANCODE_UP}}, {"down", {SDLK_DOWN, SDL_SCANCODE_DOWN}}, {"left", {SDLK_LEFT, SDL_SCANCODE_LEFT}},
            {"right", {SDLK_RIGHT, SDL_SCANCODE_RIGHT}}, {"return", {SDLK_RETURN, SDL_SCANCODE_RETURN}}, {"tab", {SDLK_TAB, SDL_SCANCODE_TAB}},
            {"space", {SDLK_SPACE, SDL_SCANCODE_SPACE}}, {"backspace", {SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE}}};
        for (const auto& [n, k] : names) {
            if (name != n) continue;
            SDL_Event d{};
            d.type = SDL_KEYDOWN;
            d.key.keysym.sym = k.first;
            d.key.keysym.scancode = SDL_Scancode(k.second);
            SDL_PushEvent(&d);
            SDL_Event u = d;
            u.type = SDL_KEYUP;
            SDL_PushEvent(&u);
            return "ok ui key " + name;
        }
        return "err ui key up|down|left|right|return|tab|space|backspace";
    }
    if (cmd == "state") {
        std::string focus;
        {
            std::lock_guard lock{g_info_mutex};
            focus = g_focus_id;
        }
        std::string capture, marked;
        {
            std::lock_guard lock{g_info_mutex};
            capture = g_capture_info;
            marked = g_marked_copy;
        }
        char buf[1400];
        std::snprintf(buf, sizeof buf, "ok ui open=%d tab=%d focus=%s capture=%s marked=%s pads=%zu hint=%d hint_scheduled=%d input_nz=%llu skip_intro=%d show_fps=%d volume=%.0f preset=%s cheats=%s gfx: %s",
                      int(g_open.load()), g_active_tab.load(), focus.empty() ? "-" : focus.c_str(), capture.c_str(), marked.c_str(), thps::input_pads().size(), int(g_hint_visible.load()),
                      int(g_hint_pending.load()), (unsigned long long)thps::stats.input_nonzero.load(), int(thps::settings::skip_intro()), int(thps::settings::show_fps()),
                      thps::settings::volume() * 100.0f, thps::input_preset_name().c_str(), cheats_state().c_str(), thps::gfx_describe().c_str());
        return buf;
    }
    if (cmd == "bindstatus") {   // the Controls tab's status line (the last rebind's result)
        std::lock_guard lock{g_info_mutex};
        return "ok ui bindstatus " + (g_status_copy.empty() ? std::string("-") : g_status_copy);
    }
    return "err ui usage: ui esc | click X Y | click #ELEMENT_ID | tab N | pad NAME | key NAME | state | bindstatus";
}

bool menu_open() { return g_open.load(); }
bool input_blocked() { return g_open.load(); }
bool wanted() {
    const int64_t now = clock_ticks(), until = g_hint_until.load();
    return g_wanted.load() || g_open.load() || (until != 0 && now >= g_hint_from.load() && now < until);
}

bool handle_event(const SDL_Event& ev) {
    // Esc / the pad's Back button toggles the menu; B (and Esc) close it.
    const bool esc = ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE;
    const bool back = ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK;
    const bool pad_b = ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_B;
    if (g_open.load() && g_capturing.load() && (esc || back || pad_b)) {   // a binding slot waits: these cancel it or bind (B)
        std::lock_guard lock{g_queue_mutex};
        if (g_queue.size() < 256) g_queue.push_back(ev);
        return true;
    }
    if ((esc && !ev.key.repeat) || back || (pad_b && g_open.load())) {
        g_open = (pad_b ? false : !g_open.load());
        return true;
    }
    if (esc) return true;
    if (!g_open.load()) return false;
    switch (ev.type) {
        case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
        case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: case SDL_CONTROLLERAXISMOTION: {
            std::lock_guard lock{g_queue_mutex};
            if (g_queue.size() < 256) g_queue.push_back(ev);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace thps::ui
