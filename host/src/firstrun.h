// The path from "the user started the app" to "the game library is loaded": one instance per config folder, the ROM (stored, --rom, the OS file
// picker or a file dropped on the window), the hash check with its distinct messages, the build with a progress window, and every error with
// a way forward. Split build only. Flow and screens: the private development notes "Q26 first-run UI".
#pragma once
#include <filesystem>
#include <string>

#include "game_split.h"
#include "versions.h"

namespace thps::firstrun {

struct Params {
    std::filesystem::path config_dir;
    std::filesystem::path rom_arg;       // --rom (may be empty)
    std::string version_arg;             // --version (may be empty)
    game::Options game_opt;
    bool ui = true;                      // false: console only (--control, --no-ui, --build-game): errors go to stderr and exit codes
    bool hidden = false;                 // --hidden: the window is created hidden (tests)
};

// Exit codes of a start that does not reach the game.
enum ExitCode { kReady = 0, kNoRom = 1, kBuildFailed = 4, kAlreadyRunning = 6, kFolderNotWritable = 7, kUserQuit = 10, kScriptEnded = 11 };

// Takes the instance lock, finds a usable ROM, makes sure the game library is built and loaded. 0: ready (`chosen` is the version).
// Otherwise a process exit code; the reason was shown in the window or printed.
int prepare(const Params& p, const VersionInfo*& chosen);

// A start-up problem before prepare() can run (the config folder cannot be created): shows it (window or stderr) and returns the exit code.
int show_fatal(const std::string& heading, const std::string& message, bool ui, bool hidden, int code);

}  // namespace thps::firstrun
