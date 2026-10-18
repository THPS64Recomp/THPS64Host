// OS seams of the host: everything that differs between Windows, Linux and macOS lives behind
// these functions or in an #ifdef in main.cpp (window handle), crash.cpp and rt64_context.cpp.
// Structure modelled on Zelda64Recomp's src/main/main.cpp (GPL-3.0, https://github.com/Zelda64Recomp/Zelda64Recomp):
// _WIN32 / __linux__ / __APPLE__ only at the window and renderer handle, window flags, DPI, paths, timer
// resolution and crash handling; SDL for everything else. This file is identical in thps1/2/3.
#pragma once
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

struct SDL_Window;

namespace thps::platform {

// Very first thing in main(), before SDL_Init: Windows 1 ms timer resolution (ultramodern sleeps with
// Sleep(1)), UTF-8 console output, WASAPI as the default audio driver, per-monitor DPI awareness (SDL hint).
void early_init();

// The command line as UTF-8 strings. On Windows argv is the ANSI code page, so this re-reads it with
// CommandLineToArgvW; elsewhere it is argv unchanged.
std::vector<std::string> command_line(int argc, char** argv);

// UTF-8 <-> std::filesystem::path (on Windows path(std::string) would assume the ANSI code page).
std::filesystem::path path_from_utf8(const std::string& s);
std::string to_utf8(const std::filesystem::path& p);

// fopen with a path that may hold non-ASCII characters (Windows: _wfopen). The string form takes UTF-8.
std::FILE* fopen_path(const std::filesystem::path& p, const char* mode);
std::FILE* fopen_utf8(const std::string& utf8_path, const char* mode);

// The executable's directory (portable.txt, assets).
std::filesystem::path exe_dir(const char* argv0);

// --config, else portable.txt (cwd, next to the executable, next to an .app), else the per-user data folder:
// macOS ~/Library/Application Support/<app>, Windows %LOCALAPPDATA%\<app>, Linux $XDG_CONFIG_HOME or ~/.config/<app>.
std::filesystem::path default_config_dir(const char* argv0, const char* app_name);

// True when any part of the window is on screen (macOS: occlusionState; Windows: not minimized and visible;
// Linux: not minimized or hidden). `native` is main.cpp's native window handle (NSWindow*, HWND or SDL_Window*).
bool window_visible(void* native);

// Display scaling for a new window (after SDL_Init(SDL_INIT_VIDEO)). Windows sizes windows in physical pixels, so a window meant
// to be w x h at 100 % opens at w*s x h*s (s = the primary display's scaling, 1.5 at 150 %), shrunk (aspect kept) to fit 90 % of
// the display's usable area. macOS sizes windows in points and Linux leaves it to the desktop: s = 1, sizes unchanged.
float window_scale();
void scale_window_size(int& w, int& h);

// --- Split build (engine + separately built game library; the THPS1 repository's docs/internals/recomp.md) ---

// Runs argv[0] with the rest as arguments and waits. stdout and stderr go to `log` (appended; empty = inherit).
// Returns the exit code, or -1 if the process could not be started. No shell is involved (no quoting traps).
int run_process(const std::vector<std::string>& argv, const std::filesystem::path& log);

// Dynamic libraries: dlopen(RTLD_NOW | RTLD_LOCAL) / LoadLibraryW. `error` gets the system message on failure.
void* load_library(const std::filesystem::path& path, std::string& error);
void* library_symbol(void* library, const char* name);
std::string library_extension();   // "dylib", "so" or "dll"

// Logical processors (>= 1), for the parallel compile of the generated C.
unsigned hardware_threads();

// Where the app keeps files it ships next to the executable: Contents/Resources in a macOS .app, else
// <exe dir>/resources.
std::filesystem::path resource_dir(const char* argv0);

}  // namespace thps::platform
