// High frame rate (frame interpolation) and input latency for the THPS recomp hosts.
// src/interpolation.cpp (RT64 extended-GBI matrix groups + refresh rate),
// src/latency.cpp (input-to-display latency measurement and the low-latency options).
#pragma once

#include <cstdint>
#include <sstream>
#include <string>

namespace RT64 { struct State; struct Application; }

namespace thps {

// --control commands handled here (interpolation, latency probes). Returns false if `cmd`
// isn't one of ours; otherwise it has answered the command.
bool hfr_control(const std::string& cmd, std::istringstream& in);

// Gfx thread, send_dl: called before RT64 interprets a gfx task / after it is done.
void hfr_task_begin(RT64::State* state, uint8_t* rdram, uint32_t dl_address);
void hfr_task_end(RT64::State* state);

// Command-line flags of ours (--hfr-*): true if argv[i] was one (i advanced past its value).
bool hfr_arg(int argc, char** argv, int& i);

// HUD placement (src/hud_anchor.cpp): graphics setting `hud` = original | 16:9 | expand. Gfx thread: bracketed around RT64's
// interpretation of a gfx task, and per display-list command from the commandHook (interpolation.cpp).
bool hud_supported();                      // the game's HUD placement is implemented (game.toml hud_placement)
bool hud_active();                         // placement is on right now (hud != original, widescreen, supported; or a widened split screen)
void hud_set_mode(bool on, bool split_on); // gfx_config.cpp publish(): placement on (hud != original, widescreen, supported); split-screen placement on (widescreen)
void hud_task_begin(RT64::State* state, const uint8_t* rdram, uint32_t dl_address);
void hud_task_end(RT64::State* state);
void hud_command(RT64::State* state, uint32_t w0, uint32_t w1);
bool hud_arg(int argc, char** argv, int& i);   // --hud-debug N
std::string hud_report();                  // control "hud"
std::string split_report();                // control "split": split-screen widening counters (RT64 patch) and the 2P HUD

// Stable matrix-group IDs for RT64's pairing (src/interp_ids.cpp). Gfx thread: bracketed around RT64's interpretation of a gfx task
// (task_begin walks the display list), and per display-list command from the commandHook (interpolation.cpp).
void ids_task_begin(RT64::State* state, const uint8_t* rdram, uint32_t dl_address);
void ids_task_end(RT64::State* state);
// True = the command was handled (the caller must not tag it as a camera / transform itself).
void ids_command(RT64::State* state, uint32_t cmd_address, uint32_t w0, uint32_t w1);
void ids_cut();                                // the camera cut detector found a cut in this task
bool ids_arg(int argc, char** argv, int& i);   // --interp-ids on|off, --ids-max-step N
bool ids_control(const std::string& cmd, std::istringstream& in);
std::string ids_report();                      // control "ids"
struct IdsTiming { uint64_t tasks = 0, walk_ns = 0, walk_max_ns = 0, hook_ns = 0, cpu_ns = 0, cpu_max_ns = 0; };   // wall and thread-CPU time of the walk + tracker
IdsTiming ids_timing();                        // cumulative cost of the display-list walk + tracker and of the G_MTX hook (interp_ids.cpp)
const char* ids_budget_clock();                // the clock of the tracker's budget and of ids_cpu_us_*: "thread_cpu", or "wall" (Windows: no thread CPU clock)
RT64::Application* hfr_app();                  // the renderer (null before the first gfx task)
bool frametime_control(const std::string& cmd, std::istringstream& in);   // control "ftime start|stop" (src/frametime.cpp)
void read_fixed_matrix(const uint8_t* rdram, uint32_t addr, float m[4][4]);   // N64 Mtx (s15.16) at an RDRAM address

// One line for the perf log (pairing counters etc.); empty when interpolation is off.
std::string hfr_perf_text();

// Latency (src/latency.cpp).
void latency_set_app(RT64::Application* app);  // after RT64 setup
void latency_on_poll(uint16_t* buttons);         // game thread, port 1 poll (after control/script input)
// Present thread, RT64 capture callback: true = consumed by a latency measurement (write no PNG).
bool latency_on_present(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t pitch, uint64_t present_index);
bool latency_control(const std::string& cmd, std::istringstream& in);

}  // namespace thps
