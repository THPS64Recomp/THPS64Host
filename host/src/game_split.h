// Split build: the game code is a library built on the user's machine (the THPS1 repository's docs/internals/recomp.md).
// Only compiled with -DTHPS_SPLIT_GAME_LIB=ON.
//
//   ensure(): ROM -> [version/hash check by the caller] -> code blob (native ERZ inflate) -> N64Recomp -> RSPRecomp
//             -> system C/C++ compiler -> link -> <config>/game/thps1_game_<class>.<dylib|so|dll> -> load.
//   Nothing ROM-derived is in the engine: the library, the generated C and the blob exist only in the user's config folder.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "versions.h"

namespace thps::game {

// Finds the app's shipped resources (tools, SDK headers, per-class recipes). Call once, before runnable().
void init(const std::filesystem::path& resources, bool prebuilt_library);   // prebuilt: --game-lib / THPS_GAME_LIB given
// A recipe (classes/<cls>/) exists for this code class, i.e. the app can build its library.
bool class_available(std::string_view code_class);
// The app holds a recipe for exactly this ROM version: its class directory exists and blob.txt lists the version's code fingerprint.
// (A first-run build translates the user's own ROM, so Europe cannot borrow the USA v1.1 recipe the way the static build does.)
bool version_has_recipe(const VersionInfo& v);

// Why a build did not finish; the UI picks its screen from this.
enum class Failure {
    None,
    Cancelled,    // the user cancelled (nothing is left behind)
    NoCompiler,   // no usable C/C++ compiler on this machine
    DiskFull,     // not enough free space, or a write failed with the disk (nearly) full
    Other,
};

// The build's steps, in order (the progress screen lists them).
enum Step { StepCompiler, StepRom, StepTranslate, StepAudio, StepCompileC, StepCompileGlue, StepLink, StepCount };
const char* step_title(int step);   // for people: "Translate the game code"

// Shared between the build (a worker thread) and the UI (main thread).
struct Progress {
    std::atomic<bool> cancel{false};          // set by the UI: stop, kill the compiler, leave nothing half-written
    std::atomic<int> step{-1};                // the Step being worked on; StepCount once the library is placed
    std::atomic<int> jobs_done{0};            // within the two compile steps
    std::atomic<int> jobs_total{0};
    std::atomic<Failure> failure{Failure::None};
    double fraction() const;                  // 0..1 over the whole build (weights follow the measured times of each step)
};

struct Options {
    bool force_rebuild = false;
    bool keep_work = false;                 // keep <config>/game/work-<class>/ (generated C, logs) after a successful build
    std::filesystem::path library;          // --game-lib: use this library, never build
    bool live = false;                      // --live-game: translate in memory with LiveRecomp at start (-DTHPS_LIVERECOMP=ON)
    Progress* progress = nullptr;           // optional: the UI's view of the build, and its cancel flag
};

// <config>/first-run.log: every step with its command line and wall time, also what the UI did. Kept (the last ~1 MB).
void log_open(const std::filesystem::path& config_dir);
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Free bytes where the game library will be built (THPS_FAKE_FREE_MB=N pretends N MiB, for tests).
uint64_t free_bytes(const std::filesystem::path& dir);

// Loads the library if it is current (right ABI, recipe, tools and ROM, and the bytes match the manifest written beside it);
// registers its payload. false: it is missing, stale, damaged or will not load (nothing is built; `why` says which, for the log).
bool load_if_current(const VersionInfo& v, const std::filesystem::path& config_dir, const Options& opt, std::string& why);

// Builds the class's library if it is missing or stale (prints one "[first-run]" line per step with its wall time and logs them),
// then loads it and registers its payload (versions::payload_for). false: `error` is a message for the user; opt.progress->failure
// says what kind of failure it was.
bool ensure(const VersionInfo& v, const std::filesystem::path& rom_z64, const std::filesystem::path& config_dir, const Options& opt, std::string& error);

// Release tooling (tools/prepare_split.py, scans): the XXH3-64 of the code blob this ROM yields (a fingerprint the recipe stores,
// so a modified dump is refused before translating), or the blob itself for a local comparison. `rom_z64` is a big-endian file.
bool blob_hash(const VersionInfo& v, const std::filesystem::path& rom_z64, std::string& hash, std::string& error);
bool dump_blob(const VersionInfo& v, const std::filesystem::path& rom_z64, const std::filesystem::path& out, std::string& error);

bool make_code_blob(const VersionInfo& v, const std::filesystem::path& rom_z64, std::vector<uint8_t>& blob, std::string& error);
void set_aspmain_override(int (*fn)(uint8_t* rdram, uint32_t ucode_addr));
#ifdef THPS_LIVERECOMP
// LiveRecomp mode (src/game_live.cpp): translate the whole main image to host machine code in memory and register it.
bool live_translate(const VersionInfo& v, const std::filesystem::path& rom_z64, const std::filesystem::path& recipe_dir, std::string& error);
#endif

// Once the game runs (librecomp has allocated section_addresses): hands that array to the library, whose front-end screens (THPS2/3) read it.
void bind_library();

// The loaded library's audio microcode (src/main.cpp's get_rsp_microcode); null before ensure().
int run_aspmain(uint8_t* rdram, uint32_t ucode_addr);

}  // namespace thps::game
