// Cheats (test aids) of the THPS recomp hosts: the shared part. Off by default; they exist to make play testing faster (reach a late
// level, practise a line in slow motion), not to change the game. Rules: the cheats themselves are never stored in the Controller Pak
// save (progress made while one is on is saved as usual), the game's own cheat flags before raw RAM writes, named in the log and in
// crash.log while on.
//
// Storage: <config>/cheats.cfg, key = 0 | 1 lines (same shape as general.cfg), written by the settings menu's Cheats tab (and the
// --control "cheat" command) only, through a unique temp file and a rename. No file = every cheat off. set() is serialized (the UI
// thread and the --control thread can both call it): state change, cheats.cfg and the crash.log note in one order (Astra review C3). `cfg_rev` stamps the file's layout (1 = the first) for a later migration, as graphics.cfg does.
// Command line: --unlock-levels, --perfect-balance, --slow-motion, --skip-to-restart, --always-special turn one on for this run
// (not saved); a change in the menu replaces that and is saved.
// Which cheats a game has, what each one writes and when it takes effect: the game's own game/cheats.cpp (game_has, game_note,
// game_tick and its hook functions).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include "host.h"

namespace thps::cheats {
namespace {
constexpr int kCfgRev = 1;
const Info kInfo[kCount] = {
    {"unlock_levels", "--unlock-levels", "Unlock All Levels"},
    {"perfect_balance", "--perfect-balance", "Perfect Balance"},
    {"slow_motion", "--slow-motion", "Slow Motion"},
    {"skip_to_restart", "--skip-to-restart", "Skip to Restart"},
    {"always_special", "--always-special", "Always Special"},
};
std::atomic<uint32_t> g_on{0};      // effective state, bit per Id
std::atomic<uint32_t> g_saved{0};   // what cheats.cfg holds
std::filesystem::path g_file;
std::mutex g_mutex;                 // set() / load(): state change -> cheats.cfg -> crash.log note, one caller at a time
// crash.log line (crash.cpp reads it from a signal handler): two buffers, the index of the current one is atomic.
char g_note[2][256];
std::atomic<int> g_note_index{0};

uint32_t bit(Id id) { return 1u << unsigned(id); }

void update_note() {   // caller holds g_mutex (one writer: the buffer the reader does not use)
    const int next = 1 - g_note_index.load();
    const std::string list = active_list();
    if (list.empty()) g_note[next][0] = 0;
    else std::snprintf(g_note[next], sizeof g_note[next], "cheats on (test aids, not in the save): %s\n", list.c_str());
    g_note_index = next;
}

// cheats.cfg <- g_saved. Caller holds g_mutex. A unique temp file in the same directory, then a rename over the old file.
void save() {
    if (g_file.empty()) return;
    static std::atomic<uint64_t> seq{0};
    auto tmp = g_file;
    tmp += ".tmp." + std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())) + "." + std::to_string(seq++);
    bool ok;
    {
        std::ofstream out(tmp);
        out << "# " << thps::game::short_name << " recomp host cheats: test aids, off by default. Cheats are not stored in the Controller Pak\n"
               "# save (progress made with one on is saved as usual). 1 = on. The settings menu's Cheats tab writes this file; --unlock-levels,\n"
               "# --perfect-balance, --slow-motion, --skip-to-restart and --always-special turn one on for one run.\n"
            << "cfg_rev = " << kCfgRev << "\n";
        const uint32_t v = g_saved.load();
        for (int i = 0; i < kCount; i++) {
            if (game_has(Id(i))) out << kInfo[i].key << " = " << ((v >> i) & 1) << "\n";
        }
        out.flush();
        ok = bool(out);
    }
    std::error_code ec;
    if (!ok) {
        std::fprintf(stderr, "[thps] cheats: can't write %s; %s not updated\n", tmp.string().c_str(), g_file.string().c_str());
        std::filesystem::remove(tmp, ec);
        return;
    }
    std::filesystem::rename(tmp, g_file, ec);
    if (ec) {
        std::fprintf(stderr, "[thps] cheats: can't replace %s (%s); the change holds for this run only\n", g_file.string().c_str(), ec.message().c_str());
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
    }
}
}  // namespace

const Info& info(Id id) { return kInfo[id]; }
bool on(Id id) { return (g_on.load() & bit(id)) != 0; }
bool available(Id id) { return game_has(id); }

std::string active_list() {
    std::string s;
    const uint32_t v = g_on.load();
    for (int i = 0; i < kCount; i++) {
        if (!(v & (1u << i)) || !game_has(Id(i))) continue;
        if (!s.empty()) s += ", ";
        s += kInfo[i].key;
    }
    return s;
}

const char* crash_note() { return g_note[g_note_index.load()]; }

bool parse_flag(const char* arg) {
    for (int i = 0; i < kCount; i++) {
        if (std::strcmp(arg, kInfo[i].flag) != 0) continue;
        g_on |= 1u << i;
        return true;
    }
    return false;
}

void load(const std::filesystem::path& config_dir) {
    std::lock_guard lock{g_mutex};
    g_file = config_dir / "cheats.cfg";
    std::ifstream in(g_file);
    std::string line;
    uint32_t saved = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        if (k == "cfg_rev") continue;
        bool known = false;
        for (int i = 0; i < kCount; i++) {
            if (k != kInfo[i].key) continue;
            known = true;
            if (std::atoi(v.c_str()) != 0) saved |= 1u << i;
        }
        if (!known) std::fprintf(stderr, "[thps] %s: ignoring %s\n", g_file.string().c_str(), line.c_str());
    }
    g_saved = saved;
    for (int i = 0; i < kCount; i++) {
        if ((g_on.load() & (1u << i)) && !game_has(Id(i))) {
            std::fprintf(stderr, "[thps] %s: %s has no such cheat; ignored\n", kInfo[i].flag, thps::game::short_name);
            g_on &= ~(1u << i);
        }
    }
    g_on |= saved;
    update_note();
    const std::string list = active_list();
    if (!list.empty()) {
        std::printf("[thps] cheats on: %s (test aids; not stored in the Controller Pak save)\n", list.c_str());
        std::fflush(stdout);
    }
}

void set(Id id, bool v) {
    std::lock_guard lock{g_mutex};
    if (!game_has(id) || on(id) == v) return;
    if (v) g_on |= bit(id), g_saved |= bit(id);
    else g_on &= ~bit(id), g_saved &= ~bit(id);
    save();
    update_note();
    const std::string list = active_list();
    std::printf("[thps] cheats: %s %s (now on: %s)\n", kInfo[id].key, v ? "on" : "off", list.empty() ? "none" : list.c_str());
    std::fflush(stdout);
}

void tick() {
    if (uint8_t* ram = rdram_base.load()) game_tick(ram);
}

}  // namespace thps::cheats
