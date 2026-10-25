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
                        const std::string r = thps::input_set_preset(v);
                        std::printf("[thps] settings: controls preset %s -> %s\n", v.c_str(), r.c_str());
                        std::fflush(stdout);
                    });
        c.BindFunc("config_dir", [](Rml::Variant& v) { v = Rml::String(thps::platform::to_utf8(g_config_dir)); });
        c.BindFunc("preset_is_custom", [](Rml::Variant& v) { v = thps::input_preset_name() == "custom"; });
    }
    if (auto c = ctx->CreateDataModel("graphics_model")) {
        g_handles.push_back(c.GetModelHandle());
        bind_string(c, "aspect", gfx_aspect, [](const std::string& v) { gfx_changed("aspect", v); });
        bind_string(c, "hud", gfx_hud, [](const std::string& v) { gfx_changed("hud", v); });
        c.BindFunc("show_hud", [](Rml::Variant& v) { v = thps::hud_supported(); });
        bind_string(c, "split", gfx_split, [](const std::string& v) { gfx_changed("split", v); });
        bind_string(c, "border", gfx_border, [](const std::string& v) { gfx_changed("border", v); });
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

void pad_button_down(UiState& ui, int button) {
    using clock = std::chrono::steady_clock;
    if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) return switch_tab(ui, -1);
    if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) return switch_tab(ui, +1);
    if (button == SDL_CONTROLLER_BUTTON_A) { activate_focus(ui); return; }
    SDL_ControllerButtonEvent b{};
    b.button = Uint8(button);
    const int k = cont_button_to_key(b);
    if (!k) return;
    ui.context->ProcessKeyDown(RmlSDL::ConvertKey(k), 0);
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
                for (auto& h : g_handles) h.DirtyAllVariables();   // values may have changed since (cfg edits, --control)
                ui.menu->Show();
                set_tab(ui, 0);   // always opens on General, with the focus on the tab bar (pad / keyboard navigation starts there)
            } else {
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
    if (open && ui.latest_cont_key && clock::now() >= ui.next_repeat) {
        ui.context->ProcessKeyDown(RmlSDL::ConvertKey(ui.latest_cont_key), 0);
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
            {"rb", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}};
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
        return "err ui pad back|start|up|down|left|right|a|b|lb|rb";
    }
    if (cmd == "key") {   // a keyboard key press (arrows, return, tab, space)
        std::string name;
        in >> name;
        static const std::pair<const char*, std::pair<int, int>> names[] = {
            {"up", {SDLK_UP, SDL_SCANCODE_UP}}, {"down", {SDLK_DOWN, SDL_SCANCODE_DOWN}}, {"left", {SDLK_LEFT, SDL_SCANCODE_LEFT}},
            {"right", {SDLK_RIGHT, SDL_SCANCODE_RIGHT}}, {"return", {SDLK_RETURN, SDL_SCANCODE_RETURN}}, {"tab", {SDLK_TAB, SDL_SCANCODE_TAB}},
            {"space", {SDLK_SPACE, SDL_SCANCODE_SPACE}}};
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
        return "err ui key up|down|left|right|return|tab|space";
    }
    if (cmd == "state") {
        std::string focus;
        {
            std::lock_guard lock{g_info_mutex};
            focus = g_focus_id;
        }
        char buf[768];
        std::snprintf(buf, sizeof buf, "ok ui open=%d tab=%d focus=%s hint=%d hint_scheduled=%d input_nz=%llu skip_intro=%d show_fps=%d volume=%.0f preset=%s cheats=%s gfx: %s",
                      int(g_open.load()), g_active_tab.load(), focus.empty() ? "-" : focus.c_str(), int(g_hint_visible.load()),
                      int(g_hint_pending.load()), (unsigned long long)thps::stats.input_nonzero.load(), int(thps::settings::skip_intro()), int(thps::settings::show_fps()),
                      thps::settings::volume() * 100.0f, thps::input_preset_name().c_str(), cheats_state().c_str(), thps::gfx_describe().c_str());
        return buf;
    }
    return "err ui usage: ui esc | click X Y | click #ELEMENT_ID | tab N | pad NAME | key NAME | state";
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
