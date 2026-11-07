// Rides: record what the game reads from port 1 during a course, and play it back (v0.3 prototype, test tooling).
//
// A ride is armed for the next course generation (RETRY included). Games with gRunActive begin at its first gameplay input read,
// after the goals sheet; other games begin at the course generation. The file declares this boundary so old recordings retain
// their course-start meaning. Each line holds one port-1 read: buttons, sticks and presence. Recording ends at the next course
// or at stop/quit. Playback consumes every line before handing the pad back. Tests must also verify matching initial state and
// simulation timing; an equal input-read count alone does not prove an equal ride.
//
// --ride-record FILE / --ride-play FILE, or the control commands `ride rec FILE`, `ride play FILE`, `ride stop`, `ride` (status).
// File v2: `# thps ride v2 <family> reads=N`, then `buttons x y present` per read. Nine significant digits preserve binary32 input.
// Legacy v1 input (three fields, assumed present) remains readable. Values are input only, never guest memory.
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef THPS_RIDE_STATE_TEST
#include "../tests/ride_test_host.h"
#else
#include "draw_distance.h"
#include "host.h"
#include "platform.h"
#include "thps_hooks.h"
#endif
#include "ride_v3_format.h"
#include "ride_step_state.h"

namespace {
struct Read { uint16_t buttons; float x, y; bool present = true; };
enum class Mode { Off, RecArmed, Rec, PlayArmed, Play };
std::mutex g_mutex;
std::condition_variable g_hold_cv;
bool g_hold_enabled = false, g_hold_pending = false, g_held = false, g_hold_failed = false;
Mode g_mode = Mode::Off;
std::string g_path;
uint64_t g_gen = 0;             // the course start the ride is waiting for (armed: the one current when armed)
std::vector<Read> g_reads;
size_t g_next = 0;              // playback: the next line
uint64_t g_done = 0;            // finished ride attempts (recorded or played)
bool g_write_failed = false;
bool g_start_run = false;  // games with gRunActive start after the goals sheet, at the first run input read
std::array<uint64_t, 16> g_states{};  // local diagnostics only; never stored in an input file
// Optional local diagnostic. Facts sampled before the same input read can distinguish
// a simulation divergence from a paused menu animation or a capture-frame difference.
bool g_trace = false;
struct Trace { size_t read; uint32_t state, run, scale; uint64_t gfx; };
std::vector<Trace> g_transitions;
bool g_step_default = false, g_step_file = false, g_step_live = false;
const char* g_step_error = "none";
const char* g_request_error = "none"; // a refused controller request does not poison the running session
std::atomic<bool> g_step_enabled{false};
thps::ride_step::State g_step;
thps::ride_format::V3Header g_step_header;
uint32_t g_step_override = 0, g_step_frame_time = 0, g_step_frame_ticks = 0;
uint64_t g_step_restores = 0, g_step_tick_writes = 0;
bool g_step_installed = false;
bool g_pending_record = false, g_stop_pending = false;
uint64_t g_step_writer_events = 0, g_step_input_events = 0;
std::string g_finish_reason;
struct WriterScope {
    bool locked = false, configured = false;
    const char* scope = "none";
    uint32_t prior = 0;
    uint64_t event = 0, input_event = 0;
    size_t consumed = 0;
    int mode = 0;
    thps::ride_step::Override override;
    uint8_t* ram = nullptr;
    uint32_t frame_time = 0, frame_ticks = 0;
    uint64_t generation = 0;
    uint32_t body = 0;
};
thread_local WriterScope t_writer;
void step_hook_error(const char* message, thps::ride_step::Error error);
void restore_writer();
void unlock_writer();

void sync_step_enabled() { g_step_enabled.store(g_step_file && g_step.enabled()); }
bool valid_word(uint32_t a) { return a >= 0x80000000u && a <= 0x807FFFFCu && !(a & 3); }
uint32_t selected_raw() {
    if (!thps::versions::active()) return 0;
    uint8_t* ram = thps::rdram_base.load();
    if (!ram || !valid_word(thps::sym().gSkater)) return 0;
    uint32_t body = 0;
    std::memcpy(&body, ram + (thps::sym().gSkater - 0x80000000u), 4);
    return body;
}
uint32_t selected_body() { const uint32_t body = selected_raw(); return valid_word(body) ? body : 0; }
bool step_version_locked(const thps::VersionInfo& v) {
    if (std::strcmp(thps::game::family, "thps2") || std::strcmp(v.id, "thps2") || std::strcmp(v.code_class,"us")) {
        g_step_error = "step-unsupported-version"; return false;
    }
    if (g_step_live) { g_step_error = "step-unsupported-live"; return false; }
    if (thps::versions::tv_override == 0) { g_step_error = "step-unsupported-tv"; return false; }
    const thps::CodeClass* profile = nullptr;
    for (const auto& c : thps::code_classes) if (!std::strcmp(c.name,v.code_class)) profile = &c;
    if (!profile || !valid_word(profile->ride_step_override) || !valid_word(profile->ride_step_frame_time) ||
        !valid_word(profile->ride_step_frame_ticks) || profile->ride_step_override == profile->ride_step_frame_time ||
        profile->ride_step_override == profile->ride_step_frame_ticks || profile->ride_step_frame_time == profile->ride_step_frame_ticks) {
        g_step_error = "step-missing-profile"; return false;
    }
    if (!g_step_header.version.empty() && (g_step_header.version != v.id || g_step_header.code_class != v.code_class)) {
        g_step_error = "step-recording-version"; return false;
    }
    if (g_step_header.start != thps::ride_format::Start::Run) {
        g_step_error = "step-unsupported-boundary"; return false;
    }
    g_step_override = profile->ride_step_override;
    g_step_frame_time = profile->ride_step_frame_time;
    g_step_frame_ticks = profile->ride_step_frame_ticks;
    if (g_step_file) {
        g_step_header.family = thps::game::family;
        g_step_header.version = v.id; g_step_header.code_class = v.code_class;
    }
    return true;
}
bool request_version_locked(const thps::VersionInfo& version) {
    const char* session_error = g_step_error;
    const bool valid = step_version_locked(version);
    if (!valid) { g_request_error = g_step_error; g_step_error = session_error; }
    return valid;
}
bool can_replace_step() {
    if (g_pending_record || g_step.phase == thps::ride_step::Phase::Closing ||
        (g_step_file && (g_step.phase == thps::ride_step::Phase::Bound || g_mode == Mode::Rec || g_mode == Mode::Play))) {
        g_request_error = "step-stop-and-drain-first"; return false;
    }
    return true;
}
uint64_t g_trace_hash = 14695981039346656037ull;
uint64_t g_trace_gfx0 = 0;
size_t g_trace_reads = 0;

void release_hold_locked() {
    g_hold_pending = false;
    g_hold_cv.notify_all();
}

void wait_hold_locked(std::unique_lock<std::mutex>& lock) {
    if (!g_hold_pending) return;
    g_held = true;
    // The saved read N has already returned to the guest. Block only N+1, so its
    // preceding update/graphics work can finish while the renderer drains.
    if (!g_hold_cv.wait_for(lock, std::chrono::seconds(20), [] { return !g_hold_pending; })) {
        g_hold_pending = false;
        g_hold_failed = true;
        std::printf("[thps] ride: capture hold timed out\n");
        std::fflush(stdout);
    }
    g_held = false;
}

uint32_t named_word(const char* name) {
    uint8_t* ram = thps::rdram_base.load();
    if (ram) for (const auto& symbol : thps::sym_names) {
        if (std::strcmp(symbol.name, name) != 0) continue;
        const uint32_t addr = thps::sym().*(symbol.member);
        if (addr >= 0x80000000u && addr < 0x80800000u && !(addr & 3))
            return *reinterpret_cast<const volatile uint32_t*>(ram + (addr - 0x80000000u));
    }
    return UINT32_MAX;
}

void reset_trace() {
    const char* enabled = std::getenv("THPS_RIDE_TRACE");
    g_trace = enabled && std::strcmp(enabled, "1") == 0;
    g_transitions.clear();
    g_trace_hash = 14695981039346656037ull;
    g_trace_gfx0 = 0;
    g_trace_reads = 0;
}

bool has_run_boundary() {
    for (const auto& symbol : thps::sym_names) if (std::strcmp(symbol.name, "gRunActive") == 0) return true;
    return false;
}

bool run_ready() {
    uint8_t* ram = thps::rdram_base.load();
    if (!ram) return false;
    for (const auto& symbol : thps::sym_names) {
        if (std::strcmp(symbol.name, "gRunActive") != 0) continue;
        const uint32_t addr = thps::sym().*(symbol.member);
        if (addr < 0x80000000u || addr >= 0x80800000u || (addr & 3)) return false;
        return *reinterpret_cast<const volatile uint32_t*>(ram + (addr - 0x80000000u)) == 1;
    }
    return false;
}

void note_state() {
    uint8_t* ram = thps::rdram_base.load();
    const uint32_t addr = thps::sym().gSkater;
    if (!ram || addr < 0x80000000u || addr >= 0x80800000u || (addr & 3)) return;
    const uint32_t skater = *reinterpret_cast<const volatile uint32_t*>(ram + (addr - 0x80000000u));
    const uint64_t at = uint64_t(skater) + thps::sym().Skater_PhysState;
    if (skater < 0x80000000u || at >= 0x80800000u || (at & 3)) return;
    const uint32_t state = *reinterpret_cast<const volatile uint32_t*>(ram + (at - 0x80000000u));
    if (state < g_states.size()) g_states[state]++;
    if (g_trace && !(g_mode == Mode::Play && g_next >= g_reads.size())) {
        const uint32_t run = named_word("gRunActive"), scale = named_word("gTimeScale");
        const uint64_t gfx = thps::stats.gfx_tasks.load();
        if (!g_trace_reads) g_trace_gfx0 = gfx;
        for (const uint32_t word : {state, run, scale}) {
            // Fixed byte order keeps the diagnostic digest comparable across hosts.
            for (unsigned shift = 0; shift < 32; shift += 8) {
                g_trace_hash ^= (word >> shift) & 255;
                g_trace_hash *= 1099511628211ull;
            }
        }
        if (g_transitions.empty() || g_transitions.back().state != state || g_transitions.back().run != run || g_transitions.back().scale != scale)
            g_transitions.push_back({g_trace_reads, state, run, scale, gfx - g_trace_gfx0});
        g_trace_reads++;
    }
}

void write_record_locked(const char* why) {
        if (FILE* f = thps::platform::fopen_path(g_path, "w")) {
            if (g_step_file) std::fprintf(f, "# thps ride v3 %s reads=%zu start=run version=%s code=%s step=1/30 timing=frame-time-v2\n",
                thps::game::family, g_reads.size(), g_step_header.version.c_str(), g_step_header.code_class.c_str());
            else std::fprintf(f, "# thps ride v2 %s reads=%zu start=%s\n", thps::game::family, g_reads.size(), g_start_run ? "run" : "course");
            for (const Read& r : g_reads) std::fprintf(f, "%04X %.9g %.9g %d\n", r.buttons, double(r.x), double(r.y), int(r.present));
            bool written = !std::ferror(f);
            written = std::fclose(f) == 0 && written;
            g_write_failed = !written;
            if (written) std::printf("[thps] ride: recorded %zu reads to %s (%s)\n", g_reads.size(), g_path.c_str(), why);
            else std::printf("[thps] ride: write failed for %s\n", g_path.c_str());
        } else {
            g_write_failed = true;
            std::printf("[thps] ride: can't write %s\n", g_path.c_str());
        }
        g_done++;
}

void drain_step_locked() {
    g_step.input_boundary();
    if (g_pending_record) { write_record_locked(g_finish_reason.c_str()); g_pending_record = false; }
    sync_step_enabled();
}

void finish_locked(const char* why) {
    const bool active = g_mode == Mode::Rec || g_mode == Mode::Play;
    if (g_mode == Mode::Rec) {
        if (g_step_file) { g_pending_record = true; g_finish_reason = why; }
        else write_record_locked(why);
    } else if (g_mode == Mode::Play) {
        std::printf("[thps] ride: played %zu of %zu reads (%s)\n", g_next, g_reads.size(), why);
        g_done++;
    }
    if (g_mode == Mode::Rec || g_mode == Mode::Play) {
        std::printf("[thps] ride: physics state samples");
        for (size_t i = 0; i < g_states.size(); i++) if (g_states[i]) std::printf(" %zu:%llu", i, (unsigned long long)g_states[i]);
        std::printf("\n");
        if (g_trace) {
            std::printf("[thps] ride: trace reads=%zu hash=%016llx (state/run/timescale before each consumed input)\n", g_trace_reads, (unsigned long long)g_trace_hash);
            for (const auto& t : g_transitions)
                std::printf("[thps] ride: trace at=%zu state=%u run=%u scale=%u gfx=%llu\n", t.read, t.state, t.run, t.scale, (unsigned long long)t.gfx);
        }
    }
    std::fflush(stdout);
    g_mode = Mode::Off; g_stop_pending = false;
    if (g_step_file) { g_step.close(); sync_step_enabled(); }
    if (active && g_hold_enabled) g_hold_pending = true;
}
}  // namespace

namespace {
bool record_locked(const std::string& path, bool configured) {
    if (!can_replace_step()) return false;
    thps::ride_step::State candidate;
    if (configured && !candidate.arm(thps::dd::course_generation(),selected_raw())) {
        g_request_error = thps::ride_step::name(candidate.error); return false;
    }
    // Validate a known active version before replacing the prior session.
    if (configured) {
        const auto saved = g_step_header;
        g_step_header = {};
        if (const auto* v = thps::versions::active(); v && !request_version_locked(*v)) { g_step_header = saved; return false; }
    }
    finish_locked("a new ride"); release_hold_locked();
    g_path = path; g_reads.clear(); g_next = 0; g_states = {}; reset_trace();
    g_gen = thps::dd::course_generation(); g_write_failed = false;
    g_start_run = has_run_boundary();
    g_step_file = configured; g_step = candidate; g_step_error = "none"; g_step_restores = g_step_tick_writes = 0;
    g_stop_pending = false; g_step_writer_events = g_step_input_events = 0;
    if (configured) {
        g_step_header = {}; g_step_header.family = thps::game::family;
        if (const auto* v = thps::versions::active()) {
            g_step_header.version = v->id; g_step_header.code_class = v->code_class;
        }
    } else g_step_header = {};
    g_mode = Mode::RecArmed; sync_step_enabled(); return true;
}
}
bool thps::ride_record(const std::string& path) {
    std::lock_guard lock{g_mutex}; g_request_error = "none"; return record_locked(path,g_step_default);
}
bool thps::ride_record_step(const std::string& path, const std::string& step) {
    std::lock_guard lock{g_mutex};
    g_request_error = "none";
    if (step != "1/30") { g_request_error = "step-unsupported-value"; return false; }
    return record_locked(path,true);
}
bool thps::ride_set_step(const std::string& step) {
    std::lock_guard lock{g_mutex};
    g_request_error = "none";
    if (step != "1/30") { g_request_error = "step-unsupported-value"; return false; }
    g_step_default = true; return true;
}
void thps::ride_engine_live(bool live) { std::lock_guard lock{g_mutex}; g_step_live = live; }
bool thps::ride_needs_step() { std::lock_guard lock{g_mutex}; return g_step_default || g_step_file; }
bool thps::ride_validate_version(const VersionInfo& v) {
    std::lock_guard lock{g_mutex}; return !(g_step_default || g_step_file) || step_version_locked(v);
}
std::string thps::ride_error() { std::lock_guard lock{g_mutex}; return std::strcmp(g_request_error,"none") ? g_request_error : g_step_error; }

bool thps::ride_play(const std::string& path) {
    std::lock_guard lock{g_mutex}; g_request_error = "none";
    if (!can_replace_step()) return false;
    // Validate the file before replacing the current session, like record_locked. The one exception is playing the
    // legacy recording in progress: it is written first so the file read back is that recording (same path spelling only).
    if (g_mode == Mode::Rec && !g_step_file && path == g_path) { finish_locked("a new ride"); release_hold_locked(); }
    FILE* f = thps::platform::fopen_path(path, "r");
    if (!f) { g_request_error = "input-file"; return false; }
    std::vector<Read> reads;
    char line[256];
    bool valid = true;
    int format = 1;
    size_t declared = 0;
    bool have_count = false;
    std::optional<thps::ride_format::V3Header> step_header;
    bool start_run = false;  // legacy files start at the course's graphics setup
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#') {
            if (std::strncmp(line,"# thps ride v3 ",15) == 0) {
                const auto parsed = thps::ride_format::parse_v3(line,thps::game::family);
                if (parsed.error == thps::ride_format::HeaderError::UnsupportedTiming) g_request_error = "step-unsupported-timing";
                if (have_count || !reads.empty() || !parsed.header) { valid = false; break; }
                step_header = *parsed.header; declared = step_header->reads;
                start_run = step_header->start == thps::ride_format::Start::Run;
                format = 3; have_count = true;
            } else if (std::strncmp(line, "# thps ride v2 ", 15) == 0) {
                char family[32] = {};
                int consumed = 0;
                if (have_count || !reads.empty() || std::sscanf(line, "# thps ride v2 %31s reads=%zu %n", family, &declared, &consumed) != 2 ||
                    family != std::string(thps::game::family)) valid = false;
                if (line[consumed] != '\0') {
                    char start[16] = {};
                    int end = 0;
                    if (std::sscanf(line + consumed, "start=%15s %n", start, &end) != 1 || line[consumed + end] != '\0') valid = false;
                    if (std::strcmp(start, "run") == 0) start_run = true;
                    else if (std::strcmp(start, "course") != 0) valid = false;
                }
                format = 2;
                have_count = true;
            } else if (std::strncmp(line, "# thps ride v", 13) == 0 && std::strncmp(line, "# thps ride v1 ", 15) != 0) {
                valid = false;
            }
            continue;
        }
        unsigned b = 0;
        float x = 0, y = 0;
        int present = 1, consumed = 0;
        const int fields = format >= 2 ? std::sscanf(line, "%x %f %f %d %n", &b, &x, &y, &present, &consumed)
                                       : std::sscanf(line, "%x %f %f %n", &b, &x, &y, &consumed);
        if (fields != (format >= 2 ? 4 : 3) || line[consumed] != '\0' || b > 0xffff ||
            !std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1 || std::abs(y) > 1 ||
            (present != 0 && present != 1) || reads.size() >= 1000000) {
            valid = false;
            break;
        }
        reads.push_back({uint16_t(b), x, y, bool(present)});
    }
    valid = valid && !std::ferror(f) && !reads.empty() && (!have_count || declared == reads.size()) && (!start_run || has_run_boundary());
    std::fclose(f);
    if (!valid) {
        std::printf("[thps] ride: invalid or empty input file %s\n", path.c_str());
        if (!std::strcmp(g_request_error,"none")) g_request_error = "input-file";
        return false;
    }
    if (g_step_default && !step_header) { g_request_error = "step-with-legacy-file"; return false; }
    thps::ride_step::State candidate;
    if (step_header) {
        if (!candidate.arm(thps::dd::course_generation(),selected_raw())) {
            g_request_error = thps::ride_step::name(candidate.error); return false;
        }
        const auto saved = g_step_header;
        g_step_header = *step_header;
        if (const auto* v = thps::versions::active(); v && !request_version_locked(*v)) { g_step_header = saved; return false; }
        // finish_locked below only writes a legacy recording, which does not read the v3 header.
    }
    finish_locked("a new ride");
    release_hold_locked();
    g_step_file = step_header.has_value(); g_step = candidate; g_step_error = "none"; g_step_restores = g_step_tick_writes = 0;
    g_stop_pending = false; g_step_writer_events = g_step_input_events = 0;
    if (!step_header) g_step_header = {};
    sync_step_enabled();
    g_reads = std::move(reads);
    g_path = path, g_next = 0, g_states = {};
    reset_trace();
    g_gen = thps::dd::course_generation();
    g_write_failed = false;
    g_start_run = start_run;
    g_mode = Mode::PlayArmed;
    return true;
}

void thps::ride_stop() {
    std::lock_guard lock{g_mutex};
    if (g_step_file && (g_mode == Mode::Rec || g_mode == Mode::Play ||
        ((g_mode == Mode::RecArmed || g_mode == Mode::PlayArmed) && g_step.phase == thps::ride_step::Phase::Bound))) {
        // Finish before the NEXT writer's override load. An already chosen
        // frame still gets its input recorded/consumed before that boundary.
        g_stop_pending = true; return;
    }
    if (g_mode == Mode::Off) release_hold_locked();
    finish_locked("stopped");
}

void thps::ride_hold(bool enabled) {
    std::lock_guard lock{g_mutex};
    g_hold_enabled = enabled;
    g_hold_failed = false;
    if (!enabled) release_hold_locked();
}

bool thps::ride_is_held() {
    std::lock_guard lock{g_mutex};
    return g_held && !g_hold_failed;
}

std::string thps::ride_status() {
    std::lock_guard lock{g_mutex};
    static const char* names[] = {"off", "rec-armed", "rec", "play-armed", "play"};
    char buf[1024];
    const char* hold = g_hold_failed ? "timeout" : g_held ? "active" : g_hold_pending ? "pending" : g_hold_enabled ? "armed" : "off";
    std::snprintf(buf, sizeof buf, "mode=%s reads=%zu next=%zu done=%llu course_gen=%llu error=%s start=%s hold=%s step=%s step_gen=%llu step_body=%08x step_writers=%llu step_restores=%llu step_tick_writes=%llu installed=%d draining=%d stop_pending=%d writer_events=%llu input_events=%llu request=%s", names[int(g_mode)], g_reads.size(), g_next,
                  (unsigned long long)g_done, (unsigned long long)thps::dd::course_generation(), g_write_failed ? "write" : g_step_error, g_start_run ? "run" : "course", hold, g_step_file ? "1/30" : "legacy",
                  (unsigned long long)g_step.generation, g_step.body, (unsigned long long)g_step.writers, (unsigned long long)g_step_restores, (unsigned long long)g_step_tick_writes, int(g_step_installed), int(g_pending_record), int(g_stop_pending),
                  (unsigned long long)g_step_writer_events, (unsigned long long)g_step_input_events,g_request_error);
    return std::string(buf) + " path=" + g_path;
}

// Game thread, at each port-1 read (get_input), with what the game is about to get: the pad, the scripted and --control input.
// A replay also reports the pad as present (a recording made with the pad unplugged reads zeros).
void thps::ride_on_read(bool* present, uint16_t* buttons, float* x, float* y) {
    if (t_writer.locked) {
        // This thread's writer scope never reached its end hook and still owns g_mutex: locking again would deadlock.
        // Restore its word, fail the session and release the scope, as a nested begin does.
        restore_writer();
        step_hook_error("step-unclosed-writer",thps::ride_step::Error::NestedWriter);
        unlock_writer();
    }
    std::unique_lock lock{g_mutex};
    if (g_step_file) {
        if (g_mode != Mode::Off || g_pending_record || g_step.enabled()) ++g_step_input_events;
        if (g_trace && (g_pending_record || g_step.phase == thps::ride_step::Phase::Closing)) {
            std::printf("[thps] ride: step boundary input_event=%llu writer_event=%llu saved=%zu next=%zu stop_pending=%d\n",
                (unsigned long long)g_step_input_events,(unsigned long long)g_step_writer_events,g_reads.size(),g_next,int(g_stop_pending));
            std::fflush(stdout);
        }
        // N has returned to the guest; this is N+1, after its preceding update.
        drain_step_locked();
    }
    wait_hold_locked(lock);
    if (g_mode == Mode::Off) return;
    const uint64_t gen = thps::dd::course_generation();
    if ((g_mode == Mode::Rec || g_mode == Mode::Play) && gen != g_gen) {
        if (g_step_file && g_mode == Mode::Play && g_next < g_reads.size()) { g_step_error = "step-early-lifetime-end"; g_request_error = "none"; }
        finish_locked("the next course start");
        // This call is already N+1: generation ended the saved input's update.
        if (g_step_file) { drain_step_locked(); wait_hold_locked(lock); return; }
    }
    if (g_step_file && gen != g_gen && (g_mode == Mode::RecArmed || g_mode == Mode::PlayArmed) && run_ready()) {
        if (!g_step.input_ready(gen,selected_body())) {
            g_step_error = thps::ride_step::name(g_step.error); g_request_error = "none";
            std::printf("[thps] ride: %s before first input\n",g_step_error);
            g_mode = Mode::Off; sync_step_enabled(); return;
        }
    }
    if (g_mode == Mode::RecArmed && gen != g_gen && (!g_start_run || run_ready())) {
        g_mode = Mode::Rec, g_gen = gen;
        std::printf("[thps] ride: recording from course start %llu (%s boundary)\n", (unsigned long long)gen, g_start_run ? "run" : "course");
        std::fflush(stdout);
    }
    if (g_mode == Mode::PlayArmed && gen != g_gen && (!g_start_run || run_ready())) {
        g_mode = Mode::Play, g_gen = gen;
        std::printf("[thps] ride: playing %zu reads from course start %llu (%s boundary)\n", g_reads.size(), (unsigned long long)gen, g_start_run ? "run" : "course");
        std::fflush(stdout);
    }
    if (g_step_file && (g_mode == Mode::Rec || g_mode == Mode::Play) && g_trace) {
        const size_t consumed = g_mode == Mode::Play ? g_next : g_reads.size();
        if (consumed < 20 || (g_mode == Mode::Play && consumed == g_reads.size())) {
            std::printf("[thps] ride: step input_event=%llu writer_event=%llu ordinal=%zu mode=%d gen=%llu body=%08x eof=%d\n",
                (unsigned long long)g_step_input_events,(unsigned long long)g_step_writer_events,consumed + 1,int(g_mode),
                (unsigned long long)gen,selected_body(),int(g_mode == Mode::Play && consumed == g_reads.size()));
            std::fflush(stdout);
        }
    }
    if (g_mode == Mode::Rec || g_mode == Mode::Play) note_state();
    if (g_mode == Mode::Rec) {
        g_reads.push_back({*buttons, *x, *y, *present});
    } else if (g_mode == Mode::Play) {
        if (g_next >= g_reads.size()) {
            if (g_step_file) {
                // The next ordinary writer must precede EOF's input boundary.
                // Otherwise its chosen frame could leak a configured step after hold.
                g_step_error = "step-missing-boundary-writer"; g_request_error = "none";
                g_step.fail(thps::ride_step::Error::MissingWriter); g_mode = Mode::Off;
                g_stop_pending = false; sync_step_enabled(); release_hold_locked();
                std::printf("[thps] ride: %s\n",g_step_error); std::fflush(stdout); return;
            }
            finish_locked("the end of the ride"); wait_hold_locked(lock); return;
        }
        const Read& r = g_reads[g_next++];
        *present = r.present, *buttons = r.buttons, *x = r.x, *y = r.y;
    }
}

void thps::ride_quit() {
    std::lock_guard lock{g_mutex};
    g_hold_enabled = false;
    release_hold_locked();
    if (g_step_file && (g_mode == Mode::Rec || g_mode == Mode::Play || g_pending_record)) {
        g_step_error = "step-quit-before-drain"; g_request_error = "none"; g_pending_record = false;
        std::printf("[thps] ride: %s (active v3 recording is not written)\n",g_step_error);
        g_mode = Mode::Off;
    }
    finish_locked("quit"); g_step.cancel(); sync_step_enabled();
}

namespace {
uint32_t configured_step_bits() {
    const float seconds = 1.0f / 30.0f;
    uint32_t bits; std::memcpy(&bits,&seconds,4); return bits;
}
void step_hook_error(const char* message, thps::ride_step::Error error) {
    g_step.fail(error); g_step_error = message; g_request_error = "none"; g_mode = Mode::Off;
    g_pending_record = false; g_stop_pending = false; sync_step_enabled(); release_hold_locked();
    std::printf("[thps] ride: %s\n",message); std::fflush(stdout);
}
void restore_writer() {
    if (t_writer.override.restore()) { ++g_step_restores; g_step_installed = false; }
}
void unlock_writer() {
    restore_writer(); t_writer.locked = false; g_mutex.unlock();
}
}
extern "C" void thps_ride_step_begin(uint8_t* ram, recomp_context*) {
    try {
        if (t_writer.locked) {
            restore_writer();
            step_hook_error("step-nested-writer",thps::ride_step::Error::NestedWriter);
            unlock_writer(); return;
        }
        if (!g_step_enabled.load()) return;
        g_mutex.lock(); t_writer.locked = true; t_writer.configured = false;
        if (!g_step_file || !g_step.enabled()) { unlock_writer(); return; }
        if (!ram || ram != thps::rdram_base.load() || !valid_word(g_step_override) || !valid_word(g_step_frame_time) || !valid_word(g_step_frame_ticks)) {
            step_hook_error("step-invalid-profile",thps::ride_step::Error::Unsupported); unlock_writer(); return;
        }
        const uint64_t gen = thps::dd::course_generation();
        const uint32_t body = selected_body();
        t_writer.ram = ram; t_writer.frame_time = g_step_frame_time; t_writer.frame_ticks = g_step_frame_ticks;
        t_writer.generation = gen; t_writer.body = body;
        t_writer.event = ++g_step_writer_events; t_writer.input_event = g_step_input_events;
        t_writer.mode = int(g_mode);
        t_writer.consumed = g_mode == Mode::Play ? g_next : g_mode == Mode::Rec ? g_reads.size() : 0;
        auto* word = reinterpret_cast<volatile uint32_t*>(ram + (g_step_override - 0x80000000u));
        t_writer.prior = *word;
        const bool eof = g_mode == Mode::Play && g_next == g_reads.size();
        if (g_stop_pending || eof) {
            t_writer.scope = eof ? "ordinary-eof" : "ordinary-stop";
            finish_locked(eof ? "the end of the ride" : "stopped at the next writer");
            // Keep only an observation/mutex scope through this ordinary writer.
            // Do not install an override or require a configured chosen value.
            return;
        }
        const auto decision = g_step.writer(gen,body,run_ready());
        if (decision == thps::ride_step::Decision::Wait) { unlock_writer(); return; }
        if (decision == thps::ride_step::Decision::Error) {
            step_hook_error(thps::ride_step::name(g_step.error),g_step.error); unlock_writer(); return;
        }
        if (decision == thps::ride_step::Decision::LifetimeEnd) {
            if (((g_mode == Mode::Play || g_mode == Mode::PlayArmed) && g_next < g_reads.size()) || g_mode == Mode::RecArmed)
                { g_step_error = "step-early-lifetime-end"; g_request_error = "none"; }
            t_writer.scope = "ordinary-lifetime";
            finish_locked(g_mode == Mode::Play && g_next == g_reads.size() ? "the end of the ride" : "the bound body lifetime ended");
            sync_step_enabled(); return;
        }
        t_writer.scope = "configured";
        if (!t_writer.override.install(word,configured_step_bits())) {
            step_hook_error("step-nested-override",thps::ride_step::Error::NestedWriter); unlock_writer(); return;
        }
        t_writer.configured = true; g_step_installed = true;
        // The audited guest block has no external guest calls or waits. Keep
        // the mutex until its common successor restores this exact raw word.
    } catch (...) {
        if (t_writer.locked) {
            restore_writer();
            step_hook_error("step-hook-exception",thps::ride_step::Error::Unsupported);
            unlock_writer();
        }
    }
}
extern "C" void thps_ride_step_end(uint8_t* ram, recomp_context*) {
    if (!t_writer.locked) return;
    try {
        uint32_t native_chosen = 0, chosen = 0, native_ticks = 0, effective_ticks = 0;
        const bool matching_ram = ram == t_writer.ram;
        if (matching_ram) {
            std::memcpy(&native_chosen,ram + (t_writer.frame_time - 0x80000000u),4);
            auto* ticks = reinterpret_cast<volatile uint32_t*>(ram + (t_writer.frame_ticks - 0x80000000u));
            native_ticks = effective_ticks = *ticks;
            // The game has now produced BOTH outputs. Configure the integer
            // output for this update; the next ordinary writer owns its reset.
            // No history/counter is changed and ordinary scopes write neither.
            if (t_writer.configured && native_chosen == configured_step_bits()) {
                *ticks = 2;
            }
            std::memcpy(&chosen,ram + (t_writer.frame_time - 0x80000000u),4);
            effective_ticks = *ticks;
            if (t_writer.configured && native_chosen == configured_step_bits() && effective_ticks == 2) ++g_step_tick_writes;
        }
        restore_writer(); // restore BEFORE diagnostics, errors or unlock
        if (!matching_ram) step_hook_error("step-end-rdram",thps::ride_step::Error::Unsupported);
        else if (t_writer.configured && (native_chosen != configured_step_bits() || chosen != configured_step_bits() || effective_ticks != 2))
            step_hook_error("step-writer-value",thps::ride_step::Error::WriterValue);
        if (!t_writer.configured || g_step.writers == 1 || (g_trace && g_step.writers <= 20)) {
            const auto* word = reinterpret_cast<const volatile uint32_t*>(t_writer.ram + (g_step_override - 0x80000000u));
            std::printf("[thps] ride: step writer=%llu writer_event=%llu input_event=%llu arm_gen=%llu gen=%llu body=%08x run=%u mode=%d consumed=%zu scope=%s native_chosen=%08x native_ticks=%u chosen=%08x ticks=%u saved=%08x restored=%08x version=%s class=%s\n",
                (unsigned long long)g_step.writers,(unsigned long long)t_writer.event,(unsigned long long)t_writer.input_event,
                (unsigned long long)g_step.armed_generation,(unsigned long long)t_writer.generation,t_writer.body,
                named_word("gRunActive"),t_writer.mode,t_writer.consumed,t_writer.scope,native_chosen,native_ticks,chosen,effective_ticks,t_writer.prior,uint32_t(*word),
                g_step_header.version.c_str(),g_step_header.code_class.c_str());
            std::fflush(stdout);
        }
        unlock_writer();
    } catch (...) {
        restore_writer();
        step_hook_error("step-end-exception",thps::ride_step::Error::Unsupported);
        unlock_writer();
    }
}
