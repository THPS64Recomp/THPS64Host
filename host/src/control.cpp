// --control: a line protocol on stdin for test runners (recomp-host/tests/run.py).
//
// Wall-clock scripted presses (--press SEC:...) break when the machine is loaded: the game
// then runs at a few fps and a 0.3 s press can fall between two controller polls. Commands
// here are counted in *game input polls* (the game polls port 1 once per game frame) and in
// *displayed frames*, so a script behaves the same at 30 fps and at 3 fps, and the runner can
// wait for a checkpoint (an RT64 frame that matches) before it sends the next input.
//
// Every command is answered with one line "[ctl] ok <cmd> ..." (or "[ctl] err <cmd> ...").
//   press BUTTONS [HOLD [GAP]]  hold BUTTONS for HOLD polls (default 3), then nothing for GAP
//                               polls (default 3); answered when the gap is over. Presses queue.
//   pressat POLL BUTTONS [HOLD [GAP]]   like press, but not before game input poll POLL (exact timing
//                               relative to an earlier press's polls=N: deterministic game states)
//   ui esc|click X Y|tab N|state  in-game settings menu (THPS_MENU builds): same events a person would send
//   shot PATH                   RT64's output of the next displayed frame -> PNG at PATH
//   dl [N]                      display-list stats of the next N gfx tasks (default 1): max/sum
//   audio                       RMS/peak of the audio queued since the previous "audio"; output underruns, starved
//                               frames, and the largest sample step at an underrun edge (edge_step; edge_raw = the hard
//                               cut it would have been without the declick), device=1 for a real SDL device
//   audiostall MS               the next audio buffer is queued MS ms late (forces an underrun: declick test)
//   audiogap N                  the next device callback plays N frames (1-256) as an underrun mid-callback, then resumes
//                               within that callback (a short underrun: declick test of the fade-out/fade-in blend)
//   stats                       counters (frames, polls, gfx tasks, elapsed time)
//   wait FRAMES                 answered after FRAMES more displayed frames
//   quit                        exit (as --seconds would); EOF on stdin does the same
//   peek ADDR [WORDS]           RDRAM words (hex) at a KSEG0 address, e.g. peek 0x800F3294 4; ADDR may be
//                               a host name of the running version: peek @gSkater, peek @gSControl+0x10
//   overlays                    games with front-end overlays: "table=16 with_code=16 funcs=552 seen=N names=a,b": the library's screen table
//                               cross-checked with its section table, and the screens the game has loaded so far
//   sym NAME                    the running version's address / offset of a host name (versions/host_syms.toml)
//   version                     id, code class, game id and osTvType of the running version
//   poke ADDR VALUE             write one RDRAM word (debugging / RE probes)
//   vpad attach|detach|button NAME 0|1|axis NAME V   a virtual SDL game controller (input.cpp); vpad2 ... = a second one
//   key NAME                     a keyboard tap (SDL key name) through the key latch (input.cpp)
//   bind INPUT INDEX FIELD      set + save a binding, as the Controls tab does (input.cpp)
//   loadrom PATH                frontend build: the launcher's Load ROM with PATH as the file picked
//   edges [N [DIR]]             border-band stability of the next N composited presents (RT64's final
//                               image, as the window shows it): mean |frame-to-frame difference| of the
//                               left/right 1/24 of the width, worst frame and worst single column;
//                               with DIR, the first 6 frames are also saved there as PNGs (ROM-derived: local)
//   cull K                      set --cull-extra K live (widescreen culling A/B on a paused frame)
//   border [original|fill]      the border setting live, not saved (A/B on a paused frame), and the crop counters
//   crash                       the game thread writes to a null pointer at its next input poll (crash.log test)
//   ramsave PATH                the whole RDRAM (as the host stores it: native-endian words) -> PATH
//                               (ROM-derived: local only, e.g. under build/)
// No game data is involved: this only moves inputs in and counters/PNGs (local) out.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#ifdef __APPLE__
#include <mach/mach.h>
#include <pthread.h>
#endif
#include <cstdlib>

#include "host.h"
#include "platform.h"
#include "hfr.h"
#include "versions.h"
#ifdef THPS_MENU
#include "ui/ui_host.h"
#endif

uint16_t thps_parse_buttons(const std::string& s, float& sx, float& sy);  // main.cpp

namespace {

bool sym_lookup(const std::string& name, uint32_t& out) {
    for (const auto& s : thps::sym_names) {
        if (name == s.name) { out = thps::sym().*(s.member); return true; }
    }
    return false;
}

struct CtlPress {
    uint16_t buttons;
    float x, y;
    int hold, gap;
    uint64_t at_poll = 0;
    uint32_t if_addr = 0, if_ge = 0;   // pressif: starts at the first poll where the RDRAM word at if_addr is >= if_ge (0 = no condition)
    std::string text;
};

// "watch ADDR VALUE": the game thread's port 1 poll (control_input) looks at the RDRAM word each poll and answers with the poll count at which it
// first equalled VALUE. The poll is in the game's own frame, so the answer is the exact game frame of a state change (a script polling the word from
// outside sees it 0-2 frames late, by a few ms of race against the game thread); a later "pressat" lands a deterministic number of frames after it.
struct CtlWatch {
    bool armed = false;
    uint32_t addr = 0, want = 0;
    std::string text;
};

std::mutex g_mutex;
CtlWatch g_watch;
std::deque<CtlPress> g_presses;
int g_press_polls = 0;  // polls spent on the front press

std::string g_shot_path;   // pending "shot" (under g_mutex)
int g_shot_age = 0;        // screen updates since the shot was requested

int g_dl_left = 0;         // pending "dl" tasks
uint64_t g_dl_tris_max = 0, g_dl_tris_sum = 0, g_dl_texrects_max = 0, g_dl_mtx_max = 0, g_dl_tasks = 0;

int64_t g_wait_until = -1;  // pending "wait": vi_origin_changes target

std::deque<std::string> g_main_cmds;
std::atomic<bool> g_crash{false};  // commands that need the main (SDL) thread

uint64_t g_audio_sumsq0 = 0, g_audio_samples0 = 0, g_audio_nonsilent0 = 0, g_audio_underruns0 = 0, g_audio_starved0 = 0;
uint64_t g_audio_peak_window = 0;
std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();

void reply(const std::string& s) {
    std::printf("[ctl] %s\n", s.c_str());
    std::fflush(stdout);
}

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count(); }

void handle(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd.empty()) return;
    auto& s = thps::stats;
    if (cmd == "press" || cmd == "pressat" || cmd == "pressif") {
        CtlPress p{};
        std::string b;
        if (cmd == "pressat") in >> p.at_poll;
        if (cmd == "pressif") {
            // pressif ADDR GE BUTTONS [HOLD [GAP]]: like press, but it starts at the first game poll (in the game's own frame) where the RDRAM word
            // at ADDR (a host name "@Name" or an address) is >= GE as an unsigned integer. A game-state trigger for a tick counter: the game's
            // update runs a variable number of ticks per displayed frame, so a poll count is not a game time.
            std::string a, ge;
            in >> a >> ge;
            uint32_t addr = 0;
            if (!a.empty() && a[0] == '@') {
                if (!thps::versions::active() || !sym_lookup(a.substr(1), addr)) { reply("err pressif unknown name " + a); return; }
            } else {
                addr = uint32_t(std::strtoul(a.c_str(), nullptr, 0));
            }
            if (ge.empty() || addr < 0x80000000u || addr >= 0x80800000u || (addr & 3)) { reply("err pressif usage: pressif ADDR GE BUTTONS [HOLD [GAP]]"); return; }
            p.if_addr = addr;
            p.if_ge = uint32_t(std::strtoul(ge.c_str(), nullptr, 0));
        }
        in >> b;
        p.hold = 3, p.gap = 3;
        in >> p.hold >> p.gap;
        if (b.empty() || p.hold < 1 || p.gap < 0) { reply("err press usage: press BUTTONS [HOLD [GAP]]"); return; }
        p.buttons = thps_parse_buttons(b, p.x, p.y);
        p.text = b;
        std::lock_guard lock{g_mutex};
        g_presses.push_back(p);
    } else if (cmd == "shot") {
        std::string path;
        std::getline(in >> std::ws, path);
        if (path.empty()) { reply("err shot usage: shot PATH"); return; }
        std::lock_guard lock{g_mutex};
        g_shot_path = path;
        g_shot_age = 0;
    } else if (cmd == "dl") {
        int n = 1;
        in >> n;
        std::lock_guard lock{g_mutex};
        g_dl_tris_max = g_dl_tris_sum = g_dl_texrects_max = g_dl_mtx_max = g_dl_tasks = 0;
        g_dl_left = std::max(1, n);
    } else if (cmd == "wait") {
        int64_t n = 1;
        in >> n;
        std::lock_guard lock{g_mutex};
        g_wait_until = int64_t(s.vi_origin_changes.load()) + std::max<int64_t>(1, n);
    } else if (cmd == "audio") {
        uint64_t sq = s.audio_sumsq, n = s.audio_samples, nz = s.audio_nonsilent_buffers;
        uint64_t peak = s.audio_peak_ctl.exchange(0);
        double ms = n > g_audio_samples0 ? double(sq - g_audio_sumsq0) / double(n - g_audio_samples0) : 0.0;
        double rms = std::sqrt(ms) / 32768.0;
        uint64_t un = s.audio_underruns, sf = s.audio_starved_frames;
        char buf[400];
        std::snprintf(buf, sizeof buf, "ok audio samples=%" PRIu64 " rms_dbfs=%.1f peak=%" PRIu64 " nonsilent_buffers=%" PRIu64
                      " underruns=%" PRIu64 " starved_frames=%" PRIu64 " edge_step=%" PRIu64 " edge_raw=%" PRIu64 " device=%d",
                      n - g_audio_samples0, rms > 0 ? 20 * std::log10(rms) : -120.0, peak, nz - g_audio_nonsilent0,
                      un - g_audio_underruns0, sf - g_audio_starved0, s.audio_edge_step_max.exchange(0), s.audio_edge_raw_max.exchange(0),
                      int(thps::audio_device_open()));
        g_audio_sumsq0 = sq, g_audio_samples0 = n, g_audio_nonsilent0 = nz, g_audio_underruns0 = un, g_audio_starved0 = sf;
        reply(buf);
    } else if (cmd == "audiostall") {
        int ms = 0;
        in >> ms;
        s.audio_stall_ms = uint32_t(std::clamp(ms, 0, 1000));
        reply("ok audiostall ms=" + std::to_string(s.audio_stall_ms.load()));
    } else if (cmd == "audiogap") {
        int n = 0;
        in >> n;
        s.audio_gap_frames = uint32_t(std::clamp(n, 0, 256));
        reply("ok audiogap frames=" + std::to_string(s.audio_gap_frames.load()));
    } else if (cmd == "stats") {
        char buf[400];
        std::snprintf(buf, sizeof buf, "ok stats t=%.2f frames=%" PRIu64 " polls=%" PRIu64 " gfx=%" PRIu64 " audio_tasks=%" PRIu64
                      " skipped_presents=%" PRIu64 " erz=%" PRIu64 " raw_dma=%" PRIu64 " ring_clear=%" PRIu64 " edge_fix=%" PRIu64,
                      now_s(), s.vi_origin_changes.load(), s.input_polls.load(), s.gfx_tasks.load(), s.audio_tasks.load(),
                      s.presents_skipped.load(), s.erz_unpacks.load(), s.raw_dmas.load(), thps::ring_clear_patches.load(), thps::edge_fix_patches.load());
        reply(std::string(buf) + " " + thps::border_crop_report());
    } else if (cmd == "border") {   // border [original|fill]: live, for this run only (the menu row and --border save it)
        std::string v;
        in >> v;
        if (v == "original" || v == "fill") thps::border_crop_set(v == "fill");
        reply("ok border " + thps::border_crop_report());
    } else if (cmd == "inputstate") {
        reply("ok inputstate " + thps::input_state());
    } else if (cmd == "overlays") {
        reply("ok overlays " + thps::overlay_report());
    } else if (cmd == "sym") {
        // sym NAME: the active version's address / offset for a host name (versions/host_syms.toml)
        std::string n;
        in >> n;
        uint32_t v = 0;
        if (!thps::versions::active() || !sym_lookup(n, v)) { reply("err sym unknown name or no version active: " + n); return; }
        char buf[80];
        std::snprintf(buf, sizeof buf, "ok sym %s=0x%08X", n.c_str(), v);
        reply(buf);
    } else if (cmd == "cheat") {
        // cheat [KEY 0|1]: the settings menu's Cheats toggle (src/cheats.cpp: saves cheats.cfg, logs); no argument = the list that is on
        std::string k;
        int v = -1;
        in >> k >> v;
        if (!k.empty()) {
            int id = -1;
            for (int i = 0; i < thps::cheats::kCount; i++)
                if (k == thps::cheats::info(thps::cheats::Id(i)).key) id = i;
            if (id < 0 || v < 0 || !thps::cheats::available(thps::cheats::Id(id))) { reply("err cheat unknown or not in this game: " + k + " (cheat KEY 0|1)"); return; }
            thps::cheats::set(thps::cheats::Id(id), v != 0);
        }
        const std::string list = thps::cheats::active_list();
        std::string on = list.empty() ? std::string("none") : list;
        on.erase(std::remove(on.begin(), on.end(), ' '), on.end());
        const std::string yes = thps::cheats::game_stats();
        reply("ok cheat on=" + on + (yes.empty() ? "" : " yes=" + yes));
    } else if (cmd == "version") {
        const thps::VersionInfo* v = thps::versions::active();
        if (!v) { reply("err version none active"); return; }
        reply(std::string("ok version id=") + v->id + " class=" + v->code_class + " game_id=" + v->game_id + " tv=" + std::to_string(thps::versions::tv_type()));
    } else if (cmd == "peek" || cmd == "poke") {
        std::string a, v;
        in >> a >> v;
        uint32_t addr = 0;
        if (!a.empty() && a[0] == '@') {   // @Name or @Name+0xOFF
            size_t plus = a.find('+');
            if (!thps::versions::active() || !sym_lookup(a.substr(1, plus == std::string::npos ? plus : plus - 1), addr)) { reply("err " + cmd + " unknown name " + a); return; }
            if (plus != std::string::npos) addr += uint32_t(std::strtoul(a.c_str() + plus + 1, nullptr, 0));
        } else {
            addr = uint32_t(std::strtoul(a.c_str(), nullptr, 0));
        }
        uint8_t* ram = thps::rdram_base.load();
        if (!ram || addr < 0x80000000u || addr >= 0x80800000u || (addr & 3)) { reply("err " + cmd + " bad address or no RDRAM yet"); return; }
        auto* w = reinterpret_cast<uint32_t*>(ram + (addr - 0x80000000u));
        if (cmd == "poke") {
            *w = uint32_t(std::strtoul(v.c_str(), nullptr, 0));
            reply("ok poke " + a);
            return;
        }
        int n = v.empty() ? 1 : std::max(1, std::min(256, std::atoi(v.c_str())));
        std::string out = "ok peek " + a;
        char buf[16];
        for (int i = 0; i < n && addr + 4u * i < 0x80800000u; i++) {
            std::snprintf(buf, sizeof buf, " %08X", w[i]);
            out += buf;
        }
        reply(out);
    } else if (cmd == "watch") {
        // watch ADDR VALUE: reply "ok watch ADDR=VALUE poll=N" at the first game poll whose RDRAM word at ADDR (a host name "@gRunActive", or an address)
        // equals VALUE; N = the input poll count of that poll (what "pressat" counts). One watch at a time; it also fires if the word already equals VALUE.
        std::string a, v;
        in >> a >> v;
        uint32_t addr = 0;
        if (!a.empty() && a[0] == '@') {
            if (!thps::versions::active() || !sym_lookup(a.substr(1), addr)) { reply("err watch unknown name " + a); return; }
        } else {
            addr = uint32_t(std::strtoul(a.c_str(), nullptr, 0));
        }
        if (v.empty() || addr < 0x80000000u || addr >= 0x80800000u || (addr & 3)) { reply("err watch usage: watch ADDR VALUE (aligned RDRAM address or @Name)"); return; }
        std::lock_guard lock{g_mutex};
        g_watch = CtlWatch{true, addr, uint32_t(std::strtoul(v.c_str(), nullptr, 0)), a + "=" + v};
    } else if (cmd == "ramsave") {
        std::string path;
        std::getline(in >> std::ws, path);
        uint8_t* ram = thps::rdram_base.load();
        if (path.empty() || !ram) { reply("err ramsave usage: ramsave PATH (after boot)"); return; }
        FILE* f = thps::platform::fopen_utf8(path, "wb");
        if (!f) { reply("err ramsave cannot open " + path); return; }
        std::fwrite(ram, 1, 0x800000, f);
        std::fclose(f);
        char buf[80];
        std::snprintf(buf, sizeof buf, "ok ramsave frame=%" PRIu64 " path=", s.vi_origin_changes.load());
        reply(buf + path);
    } else if (cmd == "vpad" || cmd == "vpad2" || cmd == "key" || cmd == "bind" || cmd == "preset" || cmd == "loadrom" || cmd == "ui") {
        std::lock_guard lock{g_mutex};
        g_main_cmds.push_back(line);
    } else if (cmd == "edges") {
        int n = 30;
        in >> n;
        std::string dir;
        std::getline(in >> std::ws, dir);
        thps::edges_dump_dir = dir;
        thps::edges_frames_left = std::max(2, n);   // answered by rt64_context.cpp when measured
    } else if (cmd == "dump") {
        // dump N DIR: display-list dumps (textures + 2D composite) of the next N gfx tasks into DIR
        int n = 1;
        in >> n;
        std::string dir;
        std::getline(in >> std::ws, dir);
        if (dir.empty()) { reply("err dump usage: dump N DIR"); return; }
        std::filesystem::create_directories(std::filesystem::path(dir));
        if (thps::dump_next_tasks.load() > 0) { reply("err dump busy: the previous dump is still running"); return; }
        thps::ctl_dump_dir = std::filesystem::path(dir);   // before the count: the gfx thread reads it only while the count is > 0
        thps::options.dl_trace = -2;
        thps::dump_next_tasks = std::max(1, n);
        reply("ok dump " + std::to_string(n));
    } else if (cmd == "cull") {
        float k = 1.0f;
        in >> k;
        thps::options.cull_extra = k;
        reply("ok cull " + std::to_string(k));
    } else if (cmd == "crash") {
        reply("ok crash");
        g_crash = true;
    } else if (cmd == "cpu") {
        // cpu: CPU seconds (user+system) of every thread of the process, by thread name (macOS only; for the distribution
        // spike's "how much of the CPU is recompiled game code" question: the game's own threads are the ones named "Game ...").
#ifdef __APPLE__
        thread_act_array_t th;
        mach_msg_type_number_t n = 0;
        if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) { reply("err cpu task_threads failed"); return; }
        std::string out;
        double total = 0;
        std::map<std::string, double> by_name;   // threads sharing a name are summed
        for (mach_msg_type_number_t i = 0; i < n; i++) {
            thread_basic_info_data_t bi;
            mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
            if (thread_info(th[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&bi), &cnt) != KERN_SUCCESS) continue;
            double secs = bi.user_time.seconds + bi.user_time.microseconds / 1e6 + bi.system_time.seconds + bi.system_time.microseconds / 1e6;
            char name[64] = "";
            if (pthread_t pt = pthread_from_mach_thread_np(th[i])) pthread_getname_np(pt, name, sizeof name);
            for (char* c = name; *c; c++) if (*c == ' ' || *c == ';' || *c == '=') *c = '_';
            total += secs;
            by_name[name[0] ? name : "unnamed"] += secs;
        }
        for (auto& [nm, secs] : by_name) {
            char buf[160];
            std::snprintf(buf, sizeof buf, " %s=%.3f", nm.c_str(), secs);
            out += buf;
        }
        reply("ok cpu total=" + std::to_string(total) + out);
#else
        reply("err cpu only on macOS");
#endif
    } else if (cmd == "ping") {
        reply("ok ping");
    } else if (cmd == "quit") {
        reply("ok quit");
        thps::ctl_quit = true;
    } else if (thps::hfr_control(cmd, in)) {
        // interpolation / latency commands (src/interpolation.cpp, src/latency.cpp)
    } else {
        reply("err " + cmd + " unknown command");
    }
}

}  // namespace

std::atomic<bool> thps::ctl_quit{false};
std::atomic<int> thps::edges_frames_left{0};
std::string thps::edges_dump_dir;
std::atomic<int> thps::dump_next_tasks{0};
std::filesystem::path thps::ctl_dump_dir;
void thps::control_reply(const std::string& s) { reply(s); }

void thps::control_start() {
    std::thread([] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();   // a Windows runner may send CRLF
            handle(line);
        }
        reply("ok eof");
        thps::ctl_quit = true;
    }).detach();
    reply("ok ready");
}

// Main thread (update_gfx): SDL-side commands.
void thps::control_main_thread() {
    std::deque<std::string> cmds;
    {
        std::lock_guard lock{g_mutex};
        cmds.swap(g_main_cmds);
    }
    for (const auto& line : cmds) {
        size_t sp = line.find(' ');
        std::string cmd = line.substr(0, sp), rest = sp == std::string::npos ? "" : line.substr(sp + 1);
        if (cmd == "ui") {
#ifdef THPS_MENU
            reply(thps::ui::control(rest));
#else
            reply("err ui: no in-game menu in this build");
#endif
            continue;
        }
        if (cmd == "loadrom") {
#ifdef THPS_FRONTEND
            reply(std::string("ok loadrom ") + (thps::frontend::load_rom(rest) ? "good" : "rejected"));
#else
            reply("err loadrom: no launcher in this build");
#endif
            continue;
        }
        reply(cmd == "vpad" ? thps::input_vpad(rest) : cmd == "vpad2" ? thps::input_vpad(rest, 1) : cmd == "key" ? thps::input_key(rest) : cmd == "preset" ? thps::input_set_preset(rest) : thps::input_bind(rest));
    }
}

// Game thread, port 1 poll: apply the front queued press.
void thps::control_input(uint16_t* buttons, float* x, float* y) {
    if (g_crash.load()) {
        volatile int* null_ptr = nullptr;
        *null_ptr = 1;  // --control crash: test the crash log (src/crash.cpp)
    }
    std::string watched;
    {
        std::lock_guard lock{g_mutex};
        if (g_watch.armed) {
            const uint8_t* ram = thps::rdram_base.load();
            if (ram && *reinterpret_cast<const uint32_t*>(ram + (g_watch.addr - 0x80000000u)) == g_watch.want) {
                g_watch.armed = false;
                char buf[160];
                std::snprintf(buf, sizeof buf, "ok watch %s poll=%" PRIu64, g_watch.text.c_str(), thps::stats.input_polls.load());
                watched = buf;
            }
        }
    }
    if (!watched.empty()) reply(watched);
    std::string done;
    {
        std::lock_guard lock{g_mutex};
        if (g_presses.empty()) return;
        CtlPress& p = g_presses.front();
        if (g_press_polls == 0 && thps::stats.input_polls.load() + 1 < p.at_poll) return;  // pressat: not yet
        if (g_press_polls == 0 && p.if_addr) {   // pressif: not yet
            const uint8_t* ram = thps::rdram_base.load();
            if (!ram || *reinterpret_cast<const uint32_t*>(ram + (p.if_addr - 0x80000000u)) < p.if_ge) return;
        }
        if (g_press_polls < p.hold) {
            *buttons |= p.buttons;
            if (p.x != 0) *x = p.x;
            if (p.y != 0) *y = p.y;
        }
        if (++g_press_polls >= p.hold + p.gap) {
            done = p.text;
            g_press_polls = 0;
            g_presses.pop_front();
        }
    }
    if (!done.empty()) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "ok press %s polls=%" PRIu64, done.c_str(), thps::stats.input_polls.load());
        reply(buf);
    }
}

// Gfx thread, update_screen: returns the path for a pending shot (once). A frozen picture
// (no VI origin change, e.g. a loading screen) still gets dumped after ~1 s of retraces.
std::string thps::control_take_shot(bool new_frame) {
    std::string path;
    {
        std::lock_guard lock{g_mutex};
        if (g_shot_path.empty()) return {};
        if (!new_frame && ++g_shot_age < 60) return {};
        path.swap(g_shot_path);
    }
    return path;
}

void thps::control_shot_done(const std::string& path, uint64_t frame, int w, int h, double mean) {
    char buf[600];
    std::snprintf(buf, sizeof buf, "ok shot frame=%" PRIu64 " size=%dx%d mean=%.1f path=%s", frame, w, h, mean, path.c_str());
    reply(buf);
}

// Gfx thread, update_screen on a new displayed frame: answers a pending "wait".
void thps::control_frame(uint64_t frame) {
    bool fire = false;
    {
        std::lock_guard lock{g_mutex};
        if (g_wait_until >= 0 && int64_t(frame) + 1 >= g_wait_until) { fire = true; g_wait_until = -1; }
    }
    if (fire) {
        char buf[80];
        std::snprintf(buf, sizeof buf, "ok wait frames=%" PRIu64, frame + 1);
        reply(buf);
    }
}

bool thps::control_want_dl() {
    std::lock_guard lock{g_mutex};
    return g_dl_left > 0;
}

void thps::control_dl_stats(uint64_t task, uint64_t tris, uint64_t texrects, uint64_t mtx) {
    std::string out;
    {
        std::lock_guard lock{g_mutex};
        if (g_dl_left <= 0) return;
        g_dl_tasks++;
        g_dl_tris_max = std::max(g_dl_tris_max, tris);
        g_dl_tris_sum += tris;
        g_dl_texrects_max = std::max(g_dl_texrects_max, texrects);
        g_dl_mtx_max = std::max(g_dl_mtx_max, mtx);
        if (--g_dl_left == 0) {
            char buf[240];
            std::snprintf(buf, sizeof buf, "ok dl tasks=%" PRIu64 " last_task=%" PRIu64 " tris_max=%" PRIu64 " tris_avg=%.0f texrects_max=%" PRIu64 " mtx_max=%" PRIu64,
                          g_dl_tasks, task, g_dl_tris_max, double(g_dl_tris_sum) / double(g_dl_tasks), g_dl_texrects_max, g_dl_mtx_max);
            out = buf;
        }
    }
    if (!out.empty()) reply(out);
}

