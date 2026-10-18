// Split build: build, load and register the game library (src/game_split.h, src/game_abi.h).
#include "game_split.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "xxHash/xxh3.h"

#include "erz.h"
#include "game_abi.h"
#include "host.h"
#include "platform.h"
#include "platform_ext.h"
#include "overlay_rom.h"   // decomp/common/firstrun: the front-end overlays of THPS2/3, read from the user's ROM

extern uint8_t dmem[];   // librecomp's RSP DMEM (src/rsp.cpp)

namespace thps::game {
extern void* const g_import_table[];
extern const size_t g_import_count;
}  // namespace thps::game

namespace fs = std::filesystem;
namespace plat = thps::platform;

namespace thps::game {
namespace {

fs::path g_res;
const ThpsGameInfo* g_info = nullptr;

// ---------------------------------------------------------------------------------------------- utilities
using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

Progress* g_prog = nullptr;   // the build in progress (ensure() sets it), or null

// <config>/first-run.log (see log_open)
std::mutex g_log_mutex;
std::FILE* g_log_file = nullptr;
Clock::time_point g_log_t0 = Clock::now();

void log_v(const char* fmt, va_list ap) {
    std::lock_guard lock(g_log_mutex);
    if (!g_log_file) return;
    std::fprintf(g_log_file, "[%8.3f] ", since(g_log_t0));
    std::vfprintf(g_log_file, fmt, ap);
    std::fputc('\n', g_log_file);
    std::fflush(g_log_file);
}

// One line to stdout (the console flow, tests) and to first-run.log.
void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::printf("%s\n", buf);
    std::fflush(stdout);
    logf("%s", buf);
}

bool cancelled() { return g_prog && g_prog->cancel.load(); }
void set_failure(Failure f) {
    if (g_prog) g_prog->failure = f;
}

struct Step_ {
    const char* name;      // console and timing-line name (the spike's scripts read these)
    const char* title;     // for people
    double weight;         // share of the build's wall time, from the measured run (Q25: 0.01 0.06 0.24 0.01 2.68 1.41 0.08 s, rounded up for the small ones)
};
const Step_ k_steps[StepCount] = {
    {"find compiler", "Find the compiler", 0.02},
    {"read ROM, inflate main image", "Check your ROM", 0.03},
    {"translate MIPS to C (N64Recomp)", "Translate the game code", 0.08},
    {"translate audio microcode (RSPRecomp)", "Translate the audio code", 0.01},
    {"compile generated C", "Compile the game code", 0.60},
    {"compile library glue + audio microcode", "Compile the glue and audio code", 0.22},
    {"link library", "Link the game library", 0.04},
};

struct Step {
    const char* name;
    double seconds;
};
std::vector<Step> g_steps;

// One progress line per step, with its wall time once it is done; the UI reads the same state from Progress.
struct Timed {
    Clock::time_point t0 = Clock::now();
    const char* name;
    explicit Timed(int step, const std::string& detail = "") : name(k_steps[step].name) {
        if (g_prog) {
            g_prog->jobs_done = 0;
            g_prog->jobs_total = 0;
            g_prog->step = step;
        }
        say("[first-run] %s%s%s ...", name, detail.empty() ? "" : ": ", detail.c_str());
    }
    void done() {
        double s = since(t0);
        g_steps.push_back({name, s});
        say("[first-run] %s: %.2f s", name, s);
    }
};

bool read_file(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
std::string read_text(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
bool write_file(const fs::path& p, const void* data, size_t n) {
    std::FILE* f = plat::fopen_path(p, "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, n, f) == n;
    std::fclose(f);
    return ok;
}
std::string tail_of(const fs::path& log, size_t lines = 12) {
    std::string t = read_text(log), out;
    std::vector<std::string> v;
    std::istringstream ss(t);
    for (std::string l; std::getline(ss, l);) v.push_back(l);
    size_t from = v.size() > lines ? v.size() - lines : 0;
    for (size_t i = from; i < v.size(); i++) out += "    " + v[i] + "\n";
    return out;
}
std::string hex64(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}
uint32_t be32(const std::vector<uint8_t>& r, size_t o) { return (uint32_t(r[o]) << 24) | (uint32_t(r[o + 1]) << 16) | (uint32_t(r[o + 2]) << 8) | r[o + 3]; }

// "key = value" lines of a recipe's blob.txt.
std::string kv(const std::string& text, const std::string& key) {
    std::istringstream ss(text);
    for (std::string l; std::getline(ss, l);) {
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
            return s.substr(i);
        };
        if (trim(l.substr(0, eq)) == key) return trim(l.substr(eq + 1));
    }
    return "";
}

// A hash of everything that decides what the build produces: the recipes, the translators, the SDK headers and the glue sources.
// Other files in the resources folder (NOTICE, icons...) do not change the library and must not trigger a rebuild after an update.
uint64_t hash_tree(const fs::path& dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const char* sub : {"classes", "sdk", "tools"}) {
        for (auto it = fs::recursive_directory_iterator(dir / sub, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            // Not hidden files: Finder drops a .DS_Store into any folder someone looks at ("Show Package Contents"), and that must not
            // make the next start rebuild the game.
            if (it->is_regular_file() && it->path().filename().string().rfind('.', 0) != 0) files.push_back(it->path());
        }
        ec.clear();
    }
    std::sort(files.begin(), files.end());
    std::string acc;
    for (const fs::path& f : files) {
        std::vector<uint8_t> b;
        read_file(f, b);
        acc += plat::to_utf8(fs::relative(f, dir)) + ":" + hex64(XXH3_64bits(b.data(), b.size())) + "\n";
    }
    return XXH3_64bits(acc.data(), acc.size());
}

// ---------------------------------------------------------------------------------------------- toolchain
struct Toolchain {
    std::string cc, cxx;
    std::vector<std::string> common;   // flags for both compilers (sysroot, target)
    std::vector<std::string> ldflags;
    std::string description;
};

// Never runs an installer. macOS: `xcode-select -p` is the one probe that does not pop up Apple's install dialog
// when the tools are missing (running /usr/bin/clang does). THPS_FAKE_NO_COMPILER=1 simulates "missing" for tests.
bool find_toolchain(Toolchain& tc, const fs::path& work, std::string& problem) {
    const bool fake_missing = std::getenv("THPS_FAKE_NO_COMPILER") != nullptr;
#if defined(__APPLE__)
    fs::path log = work / "toolchain.log";
    std::error_code ec;
    fs::remove(log, ec);
    int rc = fake_missing ? 2 : plat::run_process({"/usr/bin/xcode-select", "-p"}, log);
    std::string dev = read_text(log);
    while (!dev.empty() && (dev.back() == '\n' || dev.back() == '\r')) dev.pop_back();
    if (rc != 0 || dev.empty() || !fs::exists(dev)) {
        set_failure(Failure::NoCompiler);
        problem =
            "Apple's developer tools (the Command Line Tools) are not installed. They contain the compiler this app uses to turn your ROM\n"
            "into a game library once. Install them with Apple's own prompt by running\n"
            "    xcode-select --install\n"
            "in Terminal (a system dialog asks for confirmation; it downloads about 1 GB), then start the app again.";
        return false;
    }
    // Safe now: the tools exist, so these do not open the installer.
    fs::remove(log, ec);
    if (plat::run_process({"/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-path"}, log) != 0) {
        set_failure(Failure::NoCompiler);
        problem = "xcrun could not find the macOS SDK (xcrun --show-sdk-path failed). Reinstall the Command Line Tools.";
        return false;
    }
    std::string sdk = read_text(log);
    while (!sdk.empty() && (sdk.back() == '\n' || sdk.back() == '\r')) sdk.pop_back();
    tc.cc = "/usr/bin/clang";
    tc.cxx = "/usr/bin/clang++";
    // The architecture of this process, not the compiler's default (a Rosetta-run x86_64 app must get an x86_64 library).
#if defined(__aarch64__)
    tc.common = {"-arch", "arm64", "-isysroot", sdk};
#else
    tc.common = {"-arch", "x86_64", "-isysroot", sdk};
#endif
#ifdef THPS_MACOS_MIN
    tc.common.push_back("-mmacosx-version-min=" THPS_MACOS_MIN);
#endif
    tc.description = "Apple clang from " + dev;
    return true;
#elif defined(_WIN32)
    // Bundled llvm-mingw (clang + lld, UCRT), as WiiCompiled does: <resources>/toolchain/bin/clang.exe.
    (void)work;
    fs::path bin = g_res / "toolchain" / "bin";
    if (fake_missing || !fs::exists(bin / "clang.exe")) {
        set_failure(Failure::NoCompiler);
        problem = "The bundled compiler (resources/toolchain/bin/clang.exe) is missing: reinstall the app.";
        return false;
    }
    tc.cc = plat::to_utf8(bin / "clang.exe");
    tc.cxx = plat::to_utf8(bin / "clang++.exe");
    tc.common = {"-target", "x86_64-w64-mingw32"};
    tc.description = "bundled llvm-mingw";
    return true;
#else
    // Linux: the distribution's clang (or gcc), found on PATH.
    (void)work;
    const char* path = std::getenv("PATH");
    auto find = [&](const char* name) -> std::string {
        std::stringstream ss(path ? path : "");
        for (std::string d; std::getline(ss, d, ':');) {
            if (!d.empty() && fs::exists(fs::path(d) / name)) return (fs::path(d) / name).string();
        }
        return "";
    };
    tc.cc = find("clang");
    tc.cxx = find("clang++");
    if (tc.cc.empty() || tc.cxx.empty()) { tc.cc = find("gcc"); tc.cxx = find("g++"); }
    if (fake_missing || tc.cc.empty() || tc.cxx.empty()) {
        set_failure(Failure::NoCompiler);
        problem = "No C/C++ compiler was found. Install one (Debian/Ubuntu: sudo apt install clang; Fedora: sudo dnf install clang) and start the app again.";
        return false;
    }
    tc.description = tc.cc;
    return true;
#endif
}

// Starts a program, logs the command line and its wall time to first-run.log. -2: cancelled by the user.
int run_logged(const std::vector<std::string>& argv, const fs::path& output_log) {
    std::string cmd;
    for (const std::string& a : argv) cmd += (cmd.empty() ? "" : " ") + (a.find(' ') != std::string::npos ? "\"" + a + "\"" : a);
    logf("run: %s", cmd.c_str());
    auto t0 = Clock::now();
    int rc = plat::run_process_cancellable(argv, output_log, g_prog ? &g_prog->cancel : nullptr);
    logf("  -> exit %d in %.2f s (output: %s)", rc, since(t0), output_log.empty() ? "inherited" : plat::to_utf8(output_log).c_str());
    return rc;
}

// Runs the jobs on all cores; stops handing out work after the first failure or a cancel.
bool run_parallel(const std::vector<std::vector<std::string>>& jobs, const fs::path& logdir, const std::string& what, std::string& error) {
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex m;
    unsigned hw = plat::hardware_threads();
    if (const char* j = std::getenv("THPS_BUILD_JOBS"); j && std::atoi(j) > 0) hw = unsigned(std::atoi(j));   // model a smaller machine / limit load
    unsigned n = std::min<unsigned>(hw, unsigned(jobs.size()));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::max(1u, n); t++) {
        pool.emplace_back([&] {
            for (;;) {
                size_t i = next++;
                if (i >= jobs.size() || failed || cancelled()) return;
                fs::path log = logdir / (what + "-" + std::to_string(i) + ".log");
                int rc = run_logged(jobs[i], log);
                if (rc == -2) return;   // cancelled: reported below
                if (rc != 0) {
                    std::lock_guard lock(m);
                    if (!failed.exchange(true)) {
                        error = what + " job " + std::to_string(i) + " failed (exit " + std::to_string(rc) + "):\n" + tail_of(log);
                        logf("%s", error.c_str());
                    }
                } else if (g_prog) {
                    g_prog->jobs_done++;
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    return !failed;
}

// ---------------------------------------------------------------------------------------------- the build
std::string lib_name(const std::string& cls) { return std::string(thps::game::family) + "_game_" + cls + "." + plat::library_extension(); }
// The recipe's files carry the game family in their names (<family>.host.toml, .host.syms.toml, .aspMain.toml: game/game.toml).
fs::path host_toml() { return std::string(family) + ".host.toml"; }
fs::path host_syms() { return std::string(family) + ".host.syms.toml"; }
fs::path asp_toml() { return std::string(family) + ".aspMain.toml"; }
fs::path stamp_path(const fs::path& lib) { return fs::path(lib.string() + ".stamp"); }

// The code blob N64Recomp reads (decomp/common/gen_syms.py): ROM[0 .. main_blob_rom) = header + IPL3 + boot segment,
// then the main image: the ROM's chunk table, each chunk ERZ-inflated, at 0x10000 strides.
bool make_blob(const VersionInfo& v, const std::vector<uint8_t>& rom, std::vector<uint8_t>& blob, std::string& error) {
    if (rom.size() < v.chunk_table + 8 || rom.size() < v.main_blob_rom) { error = "the ROM file is too short"; return false; }
    blob.assign(rom.begin(), rom.begin() + v.main_blob_rom);
    blob.resize(size_t(v.main_blob_rom) + v.main_size, 0);
    uint32_t n = be32(rom, v.chunk_table);
    if (n == 0 || n > 64) { error = "the ROM's chunk table is not what this version should have (count " + std::to_string(n) + ")"; return false; }
    for (uint32_t i = 0; i < n; i++) {
        size_t a = size_t(v.chunk_table) + be32(rom, v.chunk_table + 4 + 4 * i);
        if (a >= rom.size()) { error = "chunk " + std::to_string(i) + " lies outside the ROM"; return false; }
        std::vector<uint8_t> out;
        ErzInfo info;
        std::string err;
        if (!erz_decompress(&rom[a], rom.size() - a, out, info, err)) { error = "chunk " + std::to_string(i) + ": " + err; return false; }
        size_t at = size_t(v.main_blob_rom) + size_t(i) * 0x10000;
        size_t len = std::min<size_t>(info.usize, blob.size() > at ? blob.size() - at : 0);
        std::memcpy(&blob[at], out.data(), len);
    }
    return true;
}

// Returns false with `error` set; a cancel sets progress->failure = Cancelled (and the caller removes the work folder).
bool build_impl(const VersionInfo& v, const fs::path& rom_z64, const fs::path& game_dir, const fs::path& lib_path, const std::string& stamp, bool keep_work,
                std::string& error) {
    const std::string cls = v.code_class;
    const fs::path recipe = g_res / "classes" / cls;
    const fs::path sdk = g_res / "sdk";
    const fs::path work = game_dir / ("work-" + cls);
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work / "logs", ec);
    if (!ec) fs::create_directories(work / "obj", ec);
    if (ec) { error = "cannot create the build folder " + plat::to_utf8(work) + ": " + ec.message(); return false; }
    g_steps.clear();
    auto total = Clock::now();

    // 1. The compiler
    Toolchain tc;
    {
        Timed t(StepCompiler);
        if (!find_toolchain(tc, work, error)) return false;
        say("[first-run]   using %s", tc.description.c_str());
        t.done();
    }
    if (cancelled()) return false;

    // 2. The ROM -> code blob, checked against the fingerprint the recipe was prepared with
    {
        Timed t(StepRom);
        std::vector<uint8_t> rom, blob;
        if (!read_file(rom_z64, rom)) { error = "cannot read " + plat::to_utf8(rom_z64); return false; }
        if (!make_blob(v, rom, blob, error)) return false;
        // The recipe lists the fingerprint of each ROM version it was prepared with (blob.txt: "xxh3.<version id> = ...").
        // A class can serve more than one version when their code blobs translate with the same symbol file: USA v1.1 and the
        // European release (us11) differ in 230 words, but only padding and one instruction, which versions/us11/host_force.toml
        // [config] handles (functions to skip, an instruction to patch). A version whose code does not fit the class's symbols
        // (USA v1.0) has its own class and recipe.
        std::string expect = kv(read_text(recipe / "blob.txt"), std::string("xxh3.") + v.id);
        std::string got = hex64(XXH3_64bits(blob.data(), blob.size()));
        if (expect.empty()) {
            error = std::string("this release has no recipe for ") + v.display_name + " yet (it can build: " + kv(read_text(recipe / "blob.txt"), "versions") + ").";
            return false;
        }
        if (expect != got) {
            error = "this ROM's code is not the code this release was prepared for (" + std::string(v.id) + ": code blob " + got + ", expected " + expect +
                    "). The ROM is probably a modified dump.";
            return false;
        }
        if (has_overlays) {
            // THPS2/3: the front-end screens are relocatable overlays in the ROM tail. The recipe (overlays.txt) holds the layout, the function
            // map and a fingerprint of the ROM bytes they cover; the code and the relocation streams come from the user's ROM
            // (decomp/common/firstrun/overlay_rom.h has the design). The result is what the static build's
            // overlay_recomp.py makes: the code blob with every screen appended (N64Recomp's ROM input), the symbol file with a section per
            // screen, and the screens' table for the game library.
            firstrun::OverlayRecipe orec;
            std::string oerr;
            const std::string otext = read_text(recipe / "overlays.txt");
            if (otext.empty()) { error = "this release has no front-end overlay recipe for " + std::string(v.display_name) + " (resources/classes/" + cls + "/overlays.txt)"; return false; }
            if (!firstrun::parse_overlay_recipe(otext, orec, oerr)) { error = "the overlay recipe shipped with this app is damaged: " + oerr; return false; }
            firstrun::OverlayOutput ovl;
            if (!firstrun::build_overlays(orec, rom, blob, ovl, oerr)) {
                error = "this ROM's front-end screens are not what this release was prepared for: " + oerr;
                return false;
            }
            say("[first-run]   %zu front-end screens, %zu functions, %zu relocations from the ROM", orec.overlays.size(), ovl.functions, ovl.relocs);
            const std::string syms = firstrun::append_syms(read_text(recipe / host_syms()), ovl);
            if (!write_file(work / host_syms(), syms.data(), syms.size()) || !write_file(work / "thps_overlay_table.inc", ovl.table_inc.data(), ovl.table_inc.size())) {
                error = "cannot write the overlay files into " + plat::to_utf8(work);
                return false;
            }
            blob = std::move(ovl.blob);
        }
        if (!write_file(work / "codeblob.bin", blob.data(), blob.size())) { error = "cannot write the code blob into " + plat::to_utf8(work); return false; }
        t.done();
    }
    if (cancelled()) return false;

    // 3. N64Recomp and RSPRecomp (bundled executables)
    const std::string exe = plat::library_extension() == "dll" ? ".exe" : "";
    {
        Timed t(StepTranslate);
        fs::copy_file(recipe / host_toml(), work / host_toml(), ec);
        if (!has_overlays) fs::copy_file(recipe / host_syms(), work / host_syms(), ec);   // with overlays the ROM step wrote it (symbols + the screens')
        int rc = run_logged({plat::to_utf8(g_res / "tools" / ("N64Recomp" + exe)), plat::to_utf8(work / host_toml())}, work / "logs" / "n64recomp.log");
        if (rc == -2) return false;
        if (rc != 0 || !fs::exists(work / "RecompiledFuncs" / "funcs.h")) {
            error = "N64Recomp failed (exit " + std::to_string(rc) + "):\n" + tail_of(work / "logs" / "n64recomp.log");
            return false;
        }
        t.done();
    }
    {
        Timed t(StepAudio);
        fs::copy_file(recipe / asp_toml(), work / asp_toml(), ec);   // per class: n_aspMain's place in the blob differs
        int rc = run_logged({plat::to_utf8(g_res / "tools" / ("RSPRecomp" + exe)), plat::to_utf8(work / asp_toml())}, work / "logs" / "rsprecomp.log");
        if (rc == -2) return false;
        if (rc != 0 || !fs::exists(work / "n_aspMain.cpp")) {
            error = "RSPRecomp failed (exit " + std::to_string(rc) + "):\n" + tail_of(work / "logs" / "rsprecomp.log");
            return false;
        }
        t.done();
    }

    // 4. Compile
    const std::string w = plat::to_utf8(work);
    const std::string inc_sdk = "-I" + plat::to_utf8(sdk / "include");
    const std::string inc_src = "-I" + plat::to_utf8(sdk / "src");
    const std::string inc_gen = "-I" + w + "/RecompiledFuncs";
    std::vector<fs::path> c_files;
    for (auto& e : fs::directory_iterator(work / "RecompiledFuncs", ec)) {
        std::string n = e.path().filename().string();
        if (n.rfind("funcs_", 0) == 0 && e.path().extension() == ".c") c_files.push_back(e.path());
    }
    std::sort(c_files.begin(), c_files.end());
    std::vector<std::string> objs;
    {
        const char* jobs_env = std::getenv("THPS_BUILD_JOBS");
        Timed t(StepCompileC, std::to_string(c_files.size()) + " files on " + std::to_string(jobs_env && std::atoi(jobs_env) > 0 ? unsigned(std::atoi(jobs_env)) : plat::hardware_threads()) + " threads");
        std::vector<std::vector<std::string>> jobs;
        for (const fs::path& f : c_files) {
            std::string o = w + "/obj/" + f.stem().string() + ".o";
            objs.push_back(o);
            std::vector<std::string> j{tc.cc};
            j.insert(j.end(), tc.common.begin(), tc.common.end());
            for (const char* a : {"-std=gnu11", "-O2", "-w", "-DNDEBUG", "-fno-strict-aliasing", "-fPIC", "-fvisibility=hidden"}) j.push_back(a);
            j.insert(j.end(), {inc_sdk, inc_src, inc_gen, "-c", plat::to_utf8(f), "-o", o});
            jobs.push_back(std::move(j));
        }
        if (g_prog) g_prog->jobs_total = int(jobs.size());
        if (!run_parallel(jobs, work / "logs", "cc", error)) return false;
        if (cancelled()) return false;
        t.done();
    }
    {
        Timed t(StepCompileGlue);
        auto cxx = [&](const std::string& src, const std::string& obj, std::vector<std::string> extra) {
            std::vector<std::string> j{tc.cxx};
            j.insert(j.end(), tc.common.begin(), tc.common.end());
            for (const char* a : {"-std=c++20", "-O2", "-w", "-DNDEBUG", "-fPIC", "-fvisibility=hidden"}) j.push_back(a);
            j.insert(j.end(), {inc_sdk, inc_src, inc_gen});
            j.insert(j.end(), extra.begin(), extra.end());
            j.insert(j.end(), {"-c", src, "-o", obj});
            objs.push_back(obj);
            return j;
        };
        std::vector<std::vector<std::string>> jobs;
        std::vector<std::string> lib_flags{std::string("-DTHPS_CODE_CLASS=\"") + cls + "\""};
        if (has_overlays) lib_flags.insert(lib_flags.end(), {"-DTHPS_HAS_OVERLAYS=1", "-I" + w});   // game_lib.cpp includes <work>/thps_overlay_table.inc
        jobs.push_back(cxx(plat::to_utf8(sdk / "game" / "game_lib.cpp"), w + "/obj/game_lib.o", lib_flags));
        // x86-64: librecomp's rsp_vu_impl.hpp needs SSSE3/SSE4.1 above the SSE2 baseline (as in CMakeLists.txt for n_aspMain).
#if defined(__x86_64__) || defined(_M_X64)
        const std::vector<std::string> rsp_flags{"-Ddmem=thps_game_dmem", "-msse4.1"};
#else
        const std::vector<std::string> rsp_flags{"-Ddmem=thps_game_dmem"};
#endif
        jobs.push_back(cxx(plat::to_utf8(sdk / "game" / "game_rsp.cpp"), w + "/obj/game_rsp.o", rsp_flags));
        jobs.push_back(cxx(w + "/n_aspMain.cpp", w + "/obj/n_aspMain.o", rsp_flags));
        if (g_prog) g_prog->jobs_total = int(jobs.size());
        if (!run_parallel(jobs, work / "logs", "cxx", error)) return false;
        if (cancelled()) return false;
        t.done();
    }

    // 5. Link
    {
        Timed t(StepLink);
        std::vector<std::string> j{tc.cxx};
        j.insert(j.end(), tc.common.begin(), tc.common.end());
#if defined(__APPLE__)
        j.push_back("-dynamiclib");
#else
        j.push_back("-shared");
#endif
        j.insert(j.end(), {"-o", w + "/" + lib_name(cls)});
        j.insert(j.end(), objs.begin(), objs.end());
        int rc = run_logged(j, work / "logs" / "link.log");
        if (rc == -2) return false;
        if (rc != 0) { error = "linking the game library failed (exit " + std::to_string(rc) + "):\n" + tail_of(work / "logs" / "link.log"); return false; }
#if defined(__APPLE__)
        // ld64 signs arm64 output ad hoc already; this makes it explicit (and covers an x86_64 host).
        run_logged({"/usr/bin/codesign", "--force", "--sign", "-", w + "/" + lib_name(cls)}, work / "logs" / "codesign.log");
#endif
        // Commit. A cancel is not looked at from here on (this takes milliseconds). The stamp goes first-out, last-in, so a library
        // is only ever trusted when its stamp exists, names the current inputs and carries the hash of exactly these bytes.
        fs::remove(stamp_path(lib_path), ec);
        fs::remove(lib_path, ec);
        fs::rename(work / lib_name(cls), lib_path, ec);
        if (ec) { fs::copy_file(work / lib_name(cls), lib_path, fs::copy_options::overwrite_existing, ec); }
        if (ec) { error = "cannot place the library at " + plat::to_utf8(lib_path) + ": " + ec.message(); return false; }
        std::vector<uint8_t> placed;
        if (!read_file(lib_path, placed) || placed.empty()) { error = "cannot read back the library at " + plat::to_utf8(lib_path); return false; }
        const std::string full = stamp + "lib=" + hex64(XXH3_64bits(placed.data(), placed.size())) + " size=" + std::to_string(placed.size()) + "\n";
        fs::path tmp = stamp_path(lib_path);
        tmp += ".tmp";
        if (!write_file(tmp, full.data(), full.size())) { error = "cannot write " + plat::to_utf8(tmp); return false; }
        fs::rename(tmp, stamp_path(lib_path), ec);
        if (ec) { error = "cannot write " + plat::to_utf8(stamp_path(lib_path)) + ": " + ec.message(); return false; }
        t.done();
    }
    if (g_prog) g_prog->step = StepCount;
    say("[first-run] done in %.1f s; library %s (%.1f MB)", since(total), plat::to_utf8(lib_path).c_str(), double(fs::file_size(lib_path, ec)) / 1048576.0);
    {
        // One machine-readable line for the notes and tests.
        std::string s = "[first-run] timings:";
        for (const Step& st : g_steps) { char b[96]; std::snprintf(b, sizeof b, " \"%s\"=%.2f", st.name, st.seconds); s += b; }
        say("%s", s.c_str());
    }
    if (!keep_work) fs::remove_all(work, ec);
    return true;
}

// build_impl + what to do with a failure: the user's cancel (clean up, nothing left behind), a full disk, an unaccepted Xcode licence.
// Windows: a file the stopped compiler still has open, or one a virus scanner is reading (a fresh .o), cannot be deleted for a
// moment after the cancel (the first-run test `cancel` found work-<class> left behind once). Retry for up to 2 s.
static void remove_all_retry(const fs::path& p) {
    std::error_code ec;
    for (int i = 0; i < 40; i++) {
        ec.clear();
        fs::remove_all(p, ec);
        if (!fs::exists(p, ec)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    logf("could not remove %s: %s", plat::to_utf8(p).c_str(), ec.message().c_str());
}

bool build(const VersionInfo& v, const fs::path& rom_z64, const fs::path& game_dir, const fs::path& lib_path, const std::string& stamp, bool keep_work,
           std::string& error) {
    const fs::path work = game_dir / (std::string("work-") + v.code_class);
    std::error_code ec;
    bool ok = build_impl(v, rom_z64, game_dir, lib_path, stamp, keep_work, error);
    if (ok) return true;
    if (cancelled()) {
        // Remove the work folder and any library in the making. The library itself is only replaced in the commit step above,
        // and a stamp that does not match it makes the next launch rebuild.
        remove_all_retry(work);
        error = "cancelled";
        set_failure(Failure::Cancelled);
        logf("cancelled by the user; work folder removed");
        return false;
    }
    const uint64_t free = free_bytes(game_dir);
    const bool full_text = error.find("No space left on device") != std::string::npos || error.find("not enough space on the disk") != std::string::npos ||
                           error.find("ENOSPC") != std::string::npos;
    if (full_text || free < (24ull << 20)) {
        set_failure(Failure::DiskFull);
        error = "The disk is full: only " + std::to_string(free >> 20) + " MB are free where the game is built (" + plat::to_utf8(game_dir) +
                "). Free some space and try again.\n" + error;
    } else if (error.find("license") != std::string::npos || error.find("licence") != std::string::npos) {
        error = "Xcode's licence has not been accepted yet. Open Terminal, run  sudo xcodebuild -license accept  and try again.\n" + error;
    }
    if (g_prog && g_prog->failure.load() == Failure::None) set_failure(Failure::Other);
    logf("FAILED: %s", error.c_str());
    return false;
}

// ---------------------------------------------------------------------------------------------- loading
bool load_lib(const fs::path& lib, const std::string& cls, std::string& error) {
    std::string err;
    void* h = plat::load_library(lib, err);
    if (!h) { error = "cannot load " + plat::to_utf8(lib) + ": " + err; return false; }
    auto init = reinterpret_cast<thps_game_init_fn>(plat::library_symbol(h, THPS_GAME_INIT_SYMBOL));
    if (!init) { error = plat::to_utf8(lib) + " has no " THPS_GAME_INIT_SYMBOL; return false; }
    ThpsHostApi api{THPS_GAME_ABI, uint32_t(g_import_count), g_import_table};
    const ThpsGameInfo* info = init(&api);
    if (!info || info->abi != THPS_GAME_ABI || info->struct_size != sizeof(ThpsGameInfo)) {
        error = plat::to_utf8(lib) + " was built for another engine (ABI mismatch)";
        return false;
    }
    if (cls != info->code_class) { error = plat::to_utf8(lib) + " is for code class " + info->code_class + ", not " + cls; return false; }
    g_info = info;
    static Payload payload{};
    static std::vector<OverlayInfo> overlays;   // the library's table (THPS2/3), same fields as ThpsOverlay; the name strings live in the loaded library
    overlays.clear();
    for (size_t i = 0; info->overlays && i < info->num_overlays; i++) {
        const ThpsOverlay& o = info->overlays[i];
        overlays.push_back(OverlayInfo{o.name, o.rom, o.size, o.blob_off, o.fake_base});
    }
    payload = Payload{info->code_class, info->sections, info->num_sections, info->overlay_sections_by_index, info->num_overlay_sections, info->entrypoint,
                      overlays.empty() ? nullptr : overlays.data(), overlays.size()};
    register_payload(payload);
    return true;
}

// What a library was built from: the engine ABI, the code class, the recipe/tools/SDK shipped in this app, the ROM.
// After a build the stamp file gets one more line, "lib=<XXH3 of the library> size=<bytes>" (the manifest), so a library that was cut
// short, edited or damaged on disk is told from a good one before it is loaded.
std::string expected_stamp(const VersionInfo& v) {
    return std::string("abi=") + std::to_string(THPS_GAME_ABI) + " class=" + v.code_class + " res=" + hex64(hash_tree(g_res)) + " rom=" + hex64(v.xxh3) + "\n";
}

// Empty if the library at `lib` is current, else why not.
std::string library_problem(const fs::path& lib, const std::string& base) {
    std::error_code ec;
    if (!fs::exists(lib, ec)) return "no game library yet";
    std::string have = read_text(stamp_path(lib));
    if (have.empty()) return "no manifest next to the game library";
    if (have.compare(0, base.size(), base) != 0) return "the game library was built from other inputs (app update, other ROM or recipe)";
    std::vector<uint8_t> bytes;
    if (!read_file(lib, bytes)) return "the game library cannot be read";
    const std::string manifest = "lib=" + hex64(XXH3_64bits(bytes.data(), bytes.size())) + " size=" + std::to_string(bytes.size()) + "\n";
    if (have.substr(base.size()) != manifest) return "the game library does not match its manifest (damaged or incomplete)";
    return "";
}

}  // namespace

const char* step_title(int step) { return step >= 0 && step < StepCount ? k_steps[step].title : ""; }

double Progress::fraction() const {
    int st = step.load();
    if (st < 0) return 0;
    if (st >= StepCount) return 1;
    double done = 0;
    for (int i = 0; i < st; i++) done += k_steps[i].weight;
    int total = jobs_total.load();
    double within = total > 0 ? std::min(1.0, double(jobs_done.load()) / total) : 0;
    double sum = 0;
    for (int i = 0; i < StepCount; i++) sum += k_steps[i].weight;
    return std::min(0.999, (done + within * k_steps[st].weight) / sum);
}

void log_open(const fs::path& config_dir) {
    std::lock_guard lock(g_log_mutex);
    if (g_log_file) return;
    const fs::path path = config_dir / "first-run.log";
    std::error_code ec;
    if (fs::exists(path, ec) && fs::file_size(path, ec) > (1u << 20)) {
        fs::rename(path, config_dir / "first-run.log.old", ec);
    }
    g_log_file = plat::fopen_path(path, "ab");
    g_log_t0 = Clock::now();
    if (!g_log_file) return;
    std::time_t now = std::time(nullptr);
    char when[64];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(g_log_file, "\n=== %s  engine ABI %d  (times are seconds since this start) ===\n", when, int(THPS_GAME_ABI));
    std::fflush(g_log_file);
}

void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v(fmt, ap);
    va_end(ap);
}

uint64_t free_bytes(const fs::path& dir) {
    if (const char* f = std::getenv("THPS_FAKE_FREE_MB"); f && *f) return uint64_t(std::atoll(f)) << 20;
    std::error_code ec;
    fs::path d = dir;
    while (!d.empty() && !fs::exists(d, ec) && d.has_parent_path() && d.parent_path() != d) d = d.parent_path();
    auto sp = fs::space(d, ec);
    return ec ? ~0ull : uint64_t(sp.available);   // unknown: do not block
}

bool g_prebuilt = false;
void init(const fs::path& resources, bool prebuilt_library) { g_res = resources; g_prebuilt = prebuilt_library; }

// Everything a class's recipe needs to build: the N64Recomp config, and for a game with front-end overlays (THPS2/3) the overlay recipe.
bool recipe_files_present(const fs::path& dir) {
    std::error_code ec;
    return fs::exists(dir / host_toml(), ec) && (!has_overlays || fs::exists(dir / "overlays.txt", ec));
}

bool class_available(std::string_view cls) {
    return g_prebuilt || (!g_res.empty() && recipe_files_present(g_res / "classes" / std::string(cls)));
}

bool version_has_recipe(const VersionInfo& v) {
    if (g_prebuilt) return true;   // a library was handed in (--game-lib): it decides, as in the static build
    if (g_res.empty() || !recipe_files_present(g_res / "classes" / v.code_class)) return false;
    return !kv(read_text(g_res / "classes" / v.code_class / "blob.txt"), std::string("xxh3.") + v.id).empty();
}

bool load_if_current(const VersionInfo& v, const fs::path& config_dir, const Options& opt, std::string& why) {
    const std::string cls = v.code_class;
    if (!opt.library.empty()) return load_lib(opt.library, cls, why);
    if (opt.force_rebuild) { why = "a rebuild was asked for"; return false; }
    if (!class_available(cls)) { why = "this app has no recipe for code class " + cls; return false; }
    const fs::path lib = config_dir / "game" / lib_name(cls);
    why = library_problem(lib, expected_stamp(v));
    if (!why.empty()) return false;
    if (!load_lib(lib, cls, why)) return false;   // e.g. an OS update changed the signing rules: the caller rebuilds
    return true;
}

bool ensure(const VersionInfo& v, const fs::path& rom_z64, const fs::path& config_dir, const Options& opt, std::string& error) {
    const std::string cls = v.code_class;
#ifdef THPS_LIVERECOMP
    if (opt.live) return live_translate(v, rom_z64, g_res / "classes" / cls, error);
#endif
    g_prog = opt.progress;
    struct ResetProgress { ~ResetProgress() { g_prog = nullptr; } } reset_progress;
    if (g_prog) {
        g_prog->failure = Failure::None;
        g_prog->step = -1;
    }
    std::string why;
    if (load_if_current(v, config_dir, opt, why)) {
        if (thps::verbose()) std::printf("[first-run] library is current\n");
        return true;
    }
    if (!opt.library.empty()) { error = why; set_failure(Failure::Other); return false; }
    if (!class_available(cls)) { error = "this app has no recipe for code class " + cls + " (resources/classes/" + cls + ")"; set_failure(Failure::Other); return false; }
    const fs::path game_dir = config_dir / "game";
    std::error_code ec;
    fs::create_directories(game_dir, ec);
    if (ec) {
        error = "cannot create " + plat::to_utf8(game_dir) + ": " + ec.message() + ". The folder where the app keeps its files must be writable.";
        set_failure(Failure::Other);
        return false;
    }
    // Room for the generated C (17 MB), its objects, the code blob, the library and the stored ROM copy.
    const uint64_t kNeed = 100ull << 20, have = free_bytes(game_dir);
    if (have < kNeed) {
        error = "Not enough free disk space: the game needs about " + std::to_string(kNeed >> 20) + " MB while it is being built, but only " + std::to_string(have >> 20) +
                " MB are free where the app keeps its files (" + plat::to_utf8(game_dir) + "). Free some space and try again.";
        set_failure(Failure::DiskFull);
        logf("%s", error.c_str());
        return false;
    }
    const fs::path lib = game_dir / lib_name(cls);
    const std::string stamp = expected_stamp(v);
    logf("building the game library for %s (%s): %s", v.display_name, v.id, why.c_str());
    say("[first-run] building the game library for %s (one time, a few seconds to a minute; it needs your ROM and the system compiler): %s", v.display_name, why.c_str());
    if (!build(v, rom_z64, game_dir, lib, stamp, opt.keep_work, error)) return false;
    if (!load_lib(lib, cls, error)) {
        set_failure(Failure::Other);
        return false;
    }
    return true;
}

bool make_code_blob(const VersionInfo& v, const fs::path& rom_z64, std::vector<uint8_t>& blob, std::string& error) {
    std::vector<uint8_t> rom;
    if (!read_file(rom_z64, rom)) { error = "cannot read " + plat::to_utf8(rom_z64); return false; }
    return make_blob(v, rom, blob, error);
}

bool blob_hash(const VersionInfo& v, const fs::path& rom_z64, std::string& hash, std::string& error) {
    std::vector<uint8_t> rom, blob;
    if (!read_file(rom_z64, rom)) { error = "cannot read " + plat::to_utf8(rom_z64); return false; }
    if (!make_blob(v, rom, blob, error)) return false;
    hash = hex64(XXH3_64bits(blob.data(), blob.size()));
    return true;
}

bool dump_blob(const VersionInfo& v, const fs::path& rom_z64, const fs::path& out, std::string& error) {
    std::vector<uint8_t> rom, blob;
    if (!read_file(rom_z64, rom)) { error = "cannot read " + plat::to_utf8(rom_z64); return false; }
    if (!make_blob(v, rom, blob, error)) return false;
    if (!write_file(out, blob.data(), blob.size())) { error = "cannot write " + plat::to_utf8(out); return false; }
    return true;
}

void bind_library() {
    // The recompiled front-end screens read section_addresses[] (where the game relocated them); the library has its own variable.
    if (g_info && g_info->section_addresses_slot) *g_info->section_addresses_slot = ::section_addresses;
}

static int (*g_aspmain_override)(uint8_t*, uint32_t) = nullptr;
void set_aspmain_override(int (*fn)(uint8_t*, uint32_t)) { g_aspmain_override = fn; }

int run_aspmain(uint8_t* rdram, uint32_t ucode_addr) {
    if (g_aspmain_override) return g_aspmain_override(rdram, ucode_addr);
    return g_info ? g_info->aspmain(rdram, ucode_addr, dmem) : 0;
}

}  // namespace thps::game
