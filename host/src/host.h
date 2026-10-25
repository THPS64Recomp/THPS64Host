// Shared state of the THPS recomp hosts (no game data in here).
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/config.hpp"

#include "versions.h"   // thps::game (the game profile), thps::sym(), ...

union SDL_Event;

namespace thps {

struct Stats {
    std::atomic<uint64_t> frames_presented{0};
    std::atomic<uint64_t> dummy_workloads{0};
    std::atomic<uint64_t> gfx_tasks{0};
    std::atomic<uint64_t> audio_tasks{0};
    std::atomic<uint64_t> other_tasks{0};
    std::atomic<uint64_t> pio_copies{0};
    std::atomic<uint64_t> erz_unpacks{0};
    std::atomic<uint64_t> erz_errors{0};
    std::atomic<uint64_t> raw_dmas{0};
    std::atomic<uint64_t> erz_ns{0}, erz_max_ns{0}, raw_dma_ns{0};   // time the game thread spent in the ERZ unpack hook / raw ROM DMA copies (ftime)
    std::atomic<uint64_t> raw_dma_bytes{0};
    std::atomic<uint64_t> overlay_loads{0};   // front-end overlays mapped (games with overlays: hooks.cpp)
    std::atomic<uint64_t> pio_tail_fixes{0};  // chunk tails completed from the ROM (hooks.cpp)
    std::atomic<uint64_t> input_polls{0};
    std::atomic<uint64_t> input_nonzero{0};
    std::atomic<int> renderer_api{-1};
    // Frame pacing (perf): VI origin changes seen at screen updates = frames the game displayed.
    std::atomic<uint64_t> vi_origin_changes{0};
    std::atomic<uint64_t> send_dl_us{0}, send_dl_us_max{0};
    std::atomic<uint64_t> update_screen_us{0}, update_screen_us_max{0};
    std::atomic<uint64_t> presents_skipped{0};   // screen updates skipped while the window was occluded
    // RT64 present counters (rt64 patch, rt64_context.cpp): frames composited for presentation
    // (shown or not), frames actually presented to the window, present captures written.
    std::atomic<uint64_t> rt64_composited{0}, rt64_window_presents{0}, present_dumps{0};
    // Main thread sets this from platform::window_visible (macOS: NSWindow.occlusionState; Windows: minimized
    // or hidden; Linux: minimized or hidden). While false, update_screen() doesn't
    // queue presents: CAMetalLayer.nextDrawable blocks ~1 s per frame for an occluded window
    // (display asleep, other Space, covered), and RT64's workload -> present pipeline would
    // then stall send_dl and with it the game's DP-done/frame pacing.
    std::atomic<bool> window_visible{true};
    // Audio
    std::atomic<uint64_t> audio_frames_queued{0};
    std::atomic<uint64_t> audio_nonsilent_buffers{0};
    std::atomic<uint64_t> audio_peak{0};
    std::atomic<uint64_t> audio_frames_remaining_last{0};
    std::atomic<uint64_t> audio_ucode_errors{0};
    std::atomic<uint64_t> ai_fifo_full{0};
    std::atomic<uint64_t> audio_sumsq{0}, audio_samples{0};  // for --control "audio" (RMS)
    std::atomic<uint64_t> audio_peak_ctl{0};
    // Output underruns: the device (or the --mute virtual one) ran dry; frames of silence it played meanwhile.
    std::atomic<uint64_t> audio_underruns{0}, audio_starved_frames{0};
    // Largest sample step the device played around an underrun edge, and the hard cut it would have been without the declick.
    std::atomic<uint64_t> audio_edge_step_max{0}, audio_edge_raw_max{0};
    std::atomic<uint32_t> audio_stall_ms{0};   // --control "audiostall MS": the next queue_samples waits MS first (underrun test)
    std::atomic<uint32_t> audio_gap_frames{0}; // --control "audiogap N": the next device callback plays N frames as an underrun, then resumes
};
extern Stats stats;

struct Options {
    bool verbose = false;
    int dl_log = -1;                  // log a summary of the first N display lists (-1: 8 with --verbose, else 0)
    int dl_log_every = -1;            // ...then every Nth (-1: 60 with --verbose, else 0)
    std::filesystem::path dump_dir;   // texture/frame dumps (ROM-derived: keep local)
    std::string dump_frames;          // comma list of gfx task numbers to dump, e.g. "120,300"
    int dump_every = 0;               // also dump every Nth gfx task (0 = off)
    int dl_trace = -1;                // print every command of this gfx task (-2: of the tasks --control "dump" dumps)
    int rt64_dump_every = 0;          // every Nth displayed frame: PNG of RT64's output read back from RDRAM
    std::string rt64_dump_frames;     // comma list of displayed-frame numbers for the same
    bool no_pak = false;              // no Controller Pak in any port
    bool si_requeue = false;          // --si-requeue: runtime default (requeue SI messages that find the pad queue full); see main.cpp message_queue_control
    bool rumble_pak = false;          // ports hold a Rumble Pak instead of the Controller Pak
    std::filesystem::path save_dir;   // Controller Pak images (default <config>/saves)
    bool no_keyboard = false;
    bool no_pads = false;             // ignore physical game controllers (headless tests; --control vpad still works)
    bool edge_fix = true;             // widen the game's (0,0)-(319,239) scissor so its clears reach column 319 / row 239 (dl_fixes.cpp)
    bool always_present = false;      // present even while the window is occluded (old behaviour)
    bool mute = false;                // no SDL audio device (a virtual one drains in real time)
    bool null_audio = false;          // complete audio tasks without running n_aspMain (old behaviour)
    int audio_fifo_frames = 800;      // osAiGetStatus reports "FIFO full" above this queue depth
    std::filesystem::path wav_path;   // also write the game's audio output here (ROM-derived: local)
    bool control = false;             // --control: line commands on stdin (src/control.cpp)
    int present_dump_every = 0;       // every Nth composited frame: PNG of the final presented image (rt64 patch)
    int present_w = 0, present_h = 0; // present-capture size (0 = the window's drawable size)
    bool present_pair = false;        // with each present dump, also the same frame's native framebuffer from RDRAM
    bool hidden = false;              // never show the window (headless runs: composited, not presented)
    float cull_extra = 1.0f;          // extra factor on the widescreen cull widening (A/B tests; 0 disables it)
    std::string graphics_api = "auto";  // --api: auto (Metal on macOS, else D3D12 -> Vulkan) | d3d12 | vulkan | metal
};
// The default HUD placement (the one place that decides it; 2026-10-14, user decision): at the screen edges. At the default 16:9 `expand` and
// `16:9` are the same picture, at 21:9 `expand` follows the real edges; 4:3 (aspect original) never moves the HUD whatever this says.
inline constexpr const char* kDefaultHud = "expand";
// Graphics settings (src/gfx_config.cpp): <config>/graphics.cfg (CLI build; flags are saved
// back), or the settings menu's graphics.json (THPS_FRONTEND; flags override for one run).
// Defaults (Q18, measured on an M2 Pro: README "Graphics defaults"): 16:9, 1280x720, internal
// resolution = integer scale of the window, MSAA 4x, frame interpolation to the display's rate.
struct GfxConfig {
    std::string aspect = "16:9";      // original (4:3) | 16:9 | 21:9 | expand (follow the window)
    int window_w = 1280, window_h = 720;
    bool window_auto = true;          // window=auto (the default): 1280x720 at 100 % display scaling, grown with it on Windows
                                      // (platform::scale_window_size); --window / window=WxH are physical pixels, used as given
    int res_scale = 0;                // internal resolution multiplier (0 = auto: integer scale of the window)
    int downsample = 1;               // render at res_scale, show at res_scale / downsample
    int msaa = 4;                     // 0, 2, 4, 8 (Apple GPUs have no 8x: RT64 then turns MSAA off)
    bool fullscreen = false;
    int refresh = -1;                 // 0 = original (30), -1 = display, N = manual target (frame interpolation)
    int hpfb = -1;                    // high-precision framebuffer: -1 auto, 0 off, 1 on
    std::string hud = kDefaultHud;    // HUD placement (src/hud_anchor.cpp): original (centred 4:3) | 16:9 (to the 16:9 edges) | expand (to the window's edges)
    bool split_wide = true;           // two-player split screens fill the wide picture (rt64-splitscreen-widescreen.patch, src/hud_anchor.cpp); key split wide|original
    std::string border = "original";  // the game's 8 px frame around the 3D picture (src/border_crop.cpp): original | fill (3D fills the frame)
    float aspect_ratio() const;       // target aspect, 0 = original (no widening) or expand (window)
    bool widescreen() const { return aspect != "original"; }
};
extern GfxConfig gfx;
bool gfx_config_set(const std::string& key, const std::string& value);   // false = bad value
bool gfx_override(const std::string& key, const std::string& value);     // command line: set + remember
void gfx_config_load(const std::filesystem::path& file);
void gfx_config_save(const std::filesystem::path& file);
// Settings menu values -> gfx. keep_overrides: re-apply the command-line overrides on top.
void gfx_from_settings(const ultramodern::renderer::GraphicsConfig& c, const std::string& widescreen, bool keep_overrides);
std::string gfx_describe();
float gfx_widen_factor();             // displayed aspect / (4:3), >= 1 (for game-side cull/HUD hooks)
extern std::atomic<float> window_aspect;  // drawable width / height (main thread updates it)
extern std::atomic<float> target_aspect;  // 0 = 4:3, -1 = follow the window, else the target ratio

extern Options options;
// Skip Intro (game/skip_intro.cpp or skip_intro_overlay.cpp): settings::skip_intro().
void skip_intro_input(uint16_t* buttons);   // game thread, port 1 polls
void skip_intro_overlay_loaded(const char* name);   // games with overlays: called by hooks.cpp when a screen's code blob is DMA'd
std::string overlay_report();                    // games with overlays: "table=16 with_code=16 funcs=552 seen=N names=a,b" (control "overlays", hooks.cpp)
bool in_gameplay_3d();   // a 3D view was drawn within the last few frames (src/widescreen.cpp)
bool skater_in_control();   // THPS2/3 (b_is_flip_in_game): a level run is being played, not paused (the game's own flag, sym().gRunActive; src/run_state.cpp)
// THPS1 name entry (SAVE CAREER / SAVE DATA): the game reads only the D-pad there (Left/Right = cursor, Up/Down = letter), not the
// stick. thps_name_entry_update (THPS1's game/name_entry.cpp, a hook on NameEntry_Update) calls name_entry_tick() each frame the
// screen runs; while it is active, src/input.cpp turns the left stick / keyboard arrows into D-pad bits.
void name_entry_tick();
bool name_entry_active();

// Cheats (test aids, src/cheats.cpp + the game's game/cheats.cpp): off by default, <config>/cheats.cfg (the settings menu's Cheats tab)
// or one command-line flag each for one run. Not stored in the Controller Pak save (progress made with one on is saved as usual);
// named in the log and crash.log while on.
namespace cheats {
enum Id : int { UnlockLevels, PerfectBalance, SlowMotion, SkipToRestart, AlwaysSpecial, kCount };
struct Info {
    const char* key;     // cheats.cfg key, log name
    const char* flag;    // command line
    const char* title;   // menu
};
const Info& info(Id id);
bool on(Id id);                  // effective state (any thread)
bool available(Id id);           // this game has it (game_has)
void set(Id id, bool v);         // menu: change, save cheats.cfg, log
bool parse_flag(const char* arg);   // command line: --unlock-levels etc.; false = not a cheat flag
void load(const std::filesystem::path& config_dir);   // cheats.cfg; logs the cheats that are on
std::string active_list();       // "unlock_levels, slow_motion" ("" = none)
const char* crash_note();        // crash.log line while any is on, else "" (async-signal-safe read)
void tick();                     // game thread, every controller poll: game_tick(rdram)
// The game's side (game/cheats.cpp).
bool game_has(Id id);
const char* game_note(Id id);    // the menu's details line: what it does and when it takes effect
void game_tick(uint8_t* rdram);
std::string game_stats();        // tests (control "cheat"): THPS2/3 "perfect_balance:N,..." = how often the game was told a cheat is on; THPS1 ""
}

// Crash log (src/crash.cpp): fatal signals -> <config>/crash.log with the guest function names.
void crash_add_guest_function(const void* host, uint32_t vram);   // before crash_install, or follow with crash_set_jit_range (it sorts)
void crash_set_jit_range(const void* begin, const void* end);       // LiveRecomp: frames in this range are named by the nearest guest start
void crash_install(const std::filesystem::path& config_dir);
extern std::atomic<uint8_t*> rdram_base;  // the game's RDRAM (set at the entrypoint), for --control peek/ramsave
inline bool verbose() { return options.verbose; }

std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

// Input (src/input.cpp): remappable bindings (CLI build: <config>/controls.cfg; frontend build:
// recompinput, Controls tab, controls.json) + scripted --press. Ports: 1 = keyboard + the first
// controller to press a button, 2 = the next one.
void input_load_bindings(const std::filesystem::path& config_dir);  // before the settings are made
void input_init();                     // main thread, after SDL_Init
void input_event(const ::SDL_Event& ev);  // main thread (CLI build: controllers come and go)
void input_update(double t);           // main thread, every update_gfx (--press script, rumble)
void input_poll();                     // game thread (poll_input)
bool input_get(int port, uint16_t* buttons, float* x, float* y);
bool input_connected(int port);
void input_set_rumble(int port, bool on);
void input_vi();                       // VI callback
void input_add_press(double at, double dur, uint16_t buttons, float x, float y);
// --control "vpad"/"bind" (main thread): a virtual SDL controller; replace + save a binding.
std::string input_vpad(const std::string& args, int slot = 0);
std::string input_key(const std::string& args);   // --control "key NAME": a keyboard tap
std::string input_state();   // control "inputstate": what the game thread last read on port 1 (after the thps-xbox B filter), for tests
std::string input_bind(const std::string& args);
// Layout presets: "thps-xbox" (default: tricks on the face buttons) and "n64" (C buttons on the right stick).
std::string input_set_preset(const std::string& name);   // replaces + saves every binding; "ok ..." or "err ..."
std::string input_preset_name();                         // CLI build: the controls.cfg preset: "thps-xbox" | "n64" | "custom" (edited); frontend build: ""
void input_after_settings(const std::filesystem::path& config_dir);  // frontend build: move a stale generated controls.json layout
// The settings menu's Controls tab (CLI build; any thread): the inputs (controls.cfg lines), their fields per device, rebinding.
int input_count();
std::string input_id(int in);                       // controls.cfg name: "a", "c_down", "stick_left" ...
std::string input_label(int in);                    // the game action: "Ollie (C-down)"
int input_find(const std::string& id);              // -1 = none
std::vector<std::string> input_slots(int in, bool keyboard);   // that device's fields, in order ("button:a", "axis:righty+", "key:K")
std::string input_field_label(const std::string& field);       // "A", "R-stick down", "K"
uint64_t input_bindings_version();                  // changes whenever a binding does
struct RebindResult {
    bool ok = false;
    std::string reply;     // "ok rebind c_down pad 0 button:y swapped=c_up preset=custom saved=1" | "err ..."
    std::string message;   // for the menu: "Ollie (C-down): Y (Grind (C-up) is now A)"
};
RebindResult input_rebind(int in, bool keyboard, int slot, const std::string& field);   // field "none" clears the slot; saves controls.cfg
RebindResult input_reset_input(int in);             // one input back to the preset loaded / picked last
std::vector<std::string> input_default_slots(int in, bool keyboard);   // that input's fields in the preset loaded / picked last (the Controls tab marks the others)
std::string input_base_preset();                    // the preset loaded / picked last: "thps-xbox" | "n64" (frontend build: "")
// The Controllers list: open controllers, their player (port) and what they press now. Main thread updates it every frame.
struct InputPad {
    int id = -1;           // SDL joystick instance id
    std::string name;      // SDL's controller name
    int player = 0;        // 1 | 2 | 0 = no port yet (it takes the first free one when it presses a button)
    std::string pressed;   // "A + LB", "" = nothing
};
std::vector<InputPad> input_pads();
void input_assign_port(int id, int port);   // port 0 | 1; a controller already on the other port swaps; applied on the main thread
void input_swap_ports();                    // players 1 and 2 trade controllers (either may be none: keyboard-only player 1)
std::string input_ports(const std::string& args);   // --control "ports [swap | assign INDEX 1|2]" (main thread)

// Settings (src/settings.cpp in the CLI build: <config>/general.cfg; src/frontend.cpp with
// THPS_FRONTEND: recompui's General/Graphics/Sound tabs). Command-line flags override.
namespace settings {
void load(const std::filesystem::path& config_dir);   // CLI build: general.cfg (frontend: no-op)
bool skip_intro();
bool show_fps();
float volume();                        // 0..1
std::string widescreen();              // frontend: "expand" | "16:9" | "21:9" (Expand's target)
bool take_changed();                   // our own options changed since the last call (gfx thread)
#ifdef THPS_MENU
// In-game menu (src/ui/): change an option, persist general.cfg, tell the gfx thread. CLI build only.
void set_skip_intro(bool v);
void set_show_fps(bool v);
void set_volume_percent(int v);
void mark_changed();
void save();
#endif
}
extern std::atomic<int> skip_intro_override;   // -1 = settings, 0/1 = command line
extern std::atomic<int> show_fps_override;
extern std::atomic<int> volume_override;       // -1 = settings, else percent

// ROM checks (src/rom_check.cpp, src/versions.h): check_rom_file() identifies a file against the
// version table; its error is a recomp::RomValidationError value.
const char* rom_error_text(int error);

// FPS counter (src/fps_counter.cpp): presented vs game frames, every 0.5 s while Show FPS is on;
// in the window title (CLI build) or a recompui overlay (THPS_FRONTEND).
void fps_update(void* sdl_window);     // main thread

// Alt+Enter (Windows, Linux): windowed <-> fullscreen through RT64 (src/rt64_context.cpp). Main thread.
void toggle_fullscreen();

#ifdef THPS_FRONTEND
// Front end (src/frontend.cpp): RecompFrontend launcher + settings menu.
namespace frontend {
void init(void* games);                // std::vector<recomp::GameEntry>* (one per runnable version); before register_game; loads the settings
void set_callbacks(void* cfg);         // recomp::Configuration*
void after_sdl_init();                 // create_gfx: controller db, file dialogs
void set_window(void* sdl_window);     // create_window: recompui's window
void handle_events();                  // update_gfx: recompinput's SDL event loop
bool launcher_enabled();
void set_launcher_enabled(bool on);
void update();                         // main thread, every update_gfx
bool ui_wanted();                      // a menu/overlay is shown or the launcher is up (draw UI offscreen too)
void fps_text(const char* text);       // FPS overlay text (empty = hide)
bool load_rom(const std::filesystem::path& path);  // main thread: Load ROM (validate, store, message box on errors)
}
#endif


// Controller Pak (src/pak.cpp): port 1 always (unless --no-pak), port 2 with a second pad.
bool pak_present(int port);

// Audio output (src/audio.cpp).
void audio_init();
void audio_set_frequency(uint32_t freq);
void audio_queue_samples(const int16_t* samples, size_t count);
size_t audio_frames_remaining();
bool audio_device_open();         // an SDL device (not --mute / the virtual one)
void audio_shutdown();

// Display-list inspector (gfx thread): logs F3DEX2 command stats, optionally dumps
// the textures it loads and a software composite of fill/texture rectangles.
void write_png_rgba(const std::string& path, const std::vector<uint32_t>& px, int w, int h);  // px = 0xAABBGGRR
void inspect_display_list(uint8_t* rdram, uint32_t task_index, uint32_t ucode, uint32_t data_ptr, uint32_t data_size);

// Test control channel (src/control.cpp, --control).
extern std::atomic<bool> ctl_quit;
void control_start();
void control_main_thread();                                              // main thread: vpad/bind
void control_input(uint16_t* buttons, float* x, float* y);              // game thread, port 1 polls
std::string control_take_shot(bool new_frame);                           // gfx thread
void control_shot_done(const std::string& path, uint64_t frame, int w, int h, double mean);
void control_frame(uint64_t frame);                                      // gfx thread, new displayed frame
bool control_want_dl();
void control_dl_stats(uint64_t task, uint64_t tris, uint64_t texrects, uint64_t mtx);
// --control "edges N": border-band stability of the next N composited presents (rt64_context.cpp
// measures them in the present-capture callback; the reply comes from there).
// Display-list fixes before RT64 reads a gfx task (src/dl_fixes.cpp).
void fix_display_list(uint8_t* rdram, uint32_t data_ptr);
bool dl_canary_enabled();
void dl_canary_begin(uint8_t* rdram, uint32_t data_ptr);
void dl_canary_end(uint8_t* rdram, uint64_t task_index);
extern std::atomic<uint64_t> edge_fix_patches;
extern std::atomic<uint64_t> ring_clear_patches;   // 640x480 frames whose colour buffer got a black clear of its margin ring (dl_fixes.cpp)
// Border crop (src/border_crop.cpp, graphics setting `border`): with `fill` the 3D frames' 8 px inset scissor and clears are widened to the
// whole buffer before RT64 reads the task. crop_display_list runs after fix_display_list; border_crop_set is called by gfx_config.
void border_crop_set(bool on);
bool border_crop_on();
void crop_display_list(uint8_t* rdram, uint32_t data_ptr);
std::string border_crop_report();   // "crop=0|1 tasks=N scissors=N fills=N bands=N" (control "stats")
extern std::atomic<int> edges_frames_left;
extern std::string edges_dump_dir;   // set before edges_frames_left: also save the first frames as PNGs here
// --control "dump N DIR": the display-list inspector dumps the next N gfx tasks into DIR (dl_inspect.cpp).
extern std::atomic<int> dump_next_tasks;
extern std::filesystem::path ctl_dump_dir;
void control_reply(const std::string& s);

}  // namespace thps
