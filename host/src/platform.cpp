// OS seams of the host (see platform.h). Credit: the split follows Zelda64Recomp's main.cpp (GPL-3.0).
#include "platform.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "sdl_include.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <timeapi.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if !defined(_WIN32)
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#include <thread>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
bool thps_window_visible(void* nswindow);  // src/macos_window.mm
#endif

namespace fs = std::filesystem;

#ifdef __APPLE__
// App Translocation: a quarantined app opened where it was unzipped runs from a randomized read-only copy
// (/private/var/folders/.../AppTranslocation/...), so "beside the app" is not where the user put portable.txt.
// Security.framework reports the original location, but the SDK has no public header for it: look the two
// functions up at run time, as Sparkle does. Empty path = not translocated (or the API is unavailable).
static fs::path translocation_original(const fs::path& app_bundle) {
    using IsFn = Boolean (*)(CFURLRef, bool*, CFErrorRef*);
    using OrigFn = CFURLRef (*)(CFURLRef, CFErrorRef*);
    static void* sec = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY);
    if (!sec) return {};
    auto is_translocated = reinterpret_cast<IsFn>(dlsym(sec, "SecTranslocateIsTranslocatedURL"));
    auto original_path = reinterpret_cast<OrigFn>(dlsym(sec, "SecTranslocateCreateOriginalPathForURL"));
    if (!is_translocated || !original_path) return {};
    const std::string s = app_bundle.string();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(s.data()), CFIndex(s.size()), true);
    if (!url) return {};
    fs::path result;
    bool translocated = false;
    if (is_translocated(url, &translocated, nullptr) && translocated) {
        if (CFURLRef orig = original_path(url, nullptr)) {
            char buf[4096];
            if (CFURLGetFileSystemRepresentation(orig, true, reinterpret_cast<UInt8*>(buf), sizeof buf)) result = buf;
            CFRelease(orig);
        }
    }
    CFRelease(url);
    return result;
}
#endif

namespace thps::platform {

#ifdef _WIN32
static bool has_std_handle(DWORD which) {
    HANDLE h = GetStdHandle(which);
    return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
}
#endif

void early_init() {
#ifdef _WIN32
    // The exe is a GUI-subsystem program (no console window on a double-click), so Windows gives it no console. Started from a
    // terminal, write to that terminal: --help, --check-rom and the logs. A pipe or file the parent already handed us (the tests,
    // `> log.txt`) is kept. The shell does not wait for a GUI program: its prompt may come back before our output.
    const bool have_out = has_std_handle(STD_OUTPUT_HANDLE), have_err = has_std_handle(STD_ERROR_HANDLE);
    if ((!have_out || !have_err) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!have_out) std::freopen("CONOUT$", "w", stdout);
        if (!have_err) std::freopen("CONOUT$", "w", stderr);
    }
    // ultramodern::sleep_milliseconds is Win32 Sleep(): ~15.6 ms granularity unless the process asks for 1 ms.
    // The OS restores it at exit (the host leaves with std::_Exit).
    timeBeginPeriod(1);
    SetConsoleOutputCP(CP_UTF8);
    // Zelda64Recomp forces WASAPI: DirectSound had trouble with sample queueing. Our audio is a queue-mode device too.
    SDL_setenv("SDL_AUDIODRIVER", "wasapi", 0);
#endif
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
    // RT64's own DPI setup (SetProcessDpiAwareness in ApplicationWindow::setup(title)) never runs when we create the window.
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
}

std::vector<std::string> command_line(int argc, char** argv) {
#ifdef _WIN32
    int n = 0;
    if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        std::vector<std::string> out;
        for (int i = 0; i < n; i++) {
            int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(len > 0 ? size_t(len - 1) : 0, '\0');
            if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
            out.push_back(std::move(s));
        }
        LocalFree(w);
        if (n > 0) return out;
    }
#endif
    return std::vector<std::string>(argv, argv + argc);
}

fs::path path_from_utf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string to_utf8(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::FILE* fopen_path(const fs::path& p, const char* mode) {
#ifdef _WIN32
    wchar_t wmode[16] = {};
    for (size_t i = 0; mode[i] && i + 1 < 16; i++) wmode[i] = wchar_t(mode[i]);
    return _wfopen(p.c_str(), wmode);
#else
    return std::fopen(p.c_str(), mode);
#endif
}

std::FILE* fopen_utf8(const std::string& utf8_path, const char* mode) { return fopen_path(path_from_utf8(utf8_path), mode); }

fs::path exe_dir(const char* argv0) {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n > 0 && n < buf.size()) {
        buf.resize(n);
        return fs::path(buf).parent_path();
    }
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        auto p = fs::canonical(buf, ec);
        if (!ec) return p.parent_path();
    }
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    return fs::absolute(argv0).parent_path();
}

fs::path default_config_dir(const char* argv0, const char* app_name) {
    if (fs::exists("portable.txt")) return fs::current_path();
    auto exe = exe_dir(argv0);
    if (fs::exists(exe / "portable.txt")) return exe;
    if (exe.filename() == "MacOS" && exe.parent_path().filename() == "Contents") {
        auto beside_app = exe.parent_path().parent_path().parent_path();
        if (fs::exists(beside_app / "portable.txt")) return beside_app;
#ifdef __APPLE__
        // Translocated: honour a portable.txt beside the app's original location (that folder stays writable).
        auto original_app = translocation_original(exe.parent_path().parent_path());
        if (!original_app.empty() && fs::exists(original_app.parent_path() / "portable.txt")) return original_app.parent_path();
#endif
    }
#if defined(_WIN32)
    // %LOCALAPPDATA%\<app> (the catalog's convention), via the environment or the shell folder.
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA")) {
        if (*local) return fs::path(local) / app_name;
    }
    wchar_t folder[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, folder))) return fs::path(folder) / app_name;
    return fs::current_path() / app_name;
#else
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::current_path();
#if defined(__APPLE__)
    return base / "Library" / "Application Support" / app_name;
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fs::path(xdg) / app_name;
    return base / ".config" / app_name;
#endif
#endif
}

bool window_visible(void* native) {
    if (native == nullptr) return true;
#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(native);
    return IsWindowVisible(hwnd) && !IsIconic(hwnd);
#elif defined(__APPLE__)
    return thps_window_visible(native);
#else
    Uint32 flags = SDL_GetWindowFlags(static_cast<SDL_Window*>(native));
    return (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) == 0;
#endif
}

float window_scale() {
#if defined(_WIN32)
    // The process is per-monitor DPI aware (early_init), so SDL reports the display's real DPI: 96 at 100 %, 144 at 150 %.
    // The window opens centred on display 0, the primary.
    float ddpi = 0;
    if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) == 0 && ddpi >= 48 && ddpi <= 960) return ddpi / 96.0f;
#endif
    return 1.0f;
}

void scale_window_size(int& w, int& h) {
    const float s = window_scale();
    if (s == 1.0f) return;
    float fw = w * s, fh = h * s;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
        const float fit = std::min({1.0f, 0.9f * usable.w / fw, 0.9f * usable.h / fh});
        fw *= fit, fh *= fit;
    }
    w = std::max(64, int(fw + 0.5f)), h = std::max(64, int(fh + 0.5f));
}

// --- Split build ---------------------------------------------------------------------------------------

int run_process(const std::vector<std::string>& argv, const fs::path& log) {
    if (argv.empty()) return -1;
#ifdef _WIN32
    auto wide = [](const std::string& u) {
        int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? size_t(n - 1) : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, w.data(), n);
        return w;
    };
    // CreateProcess takes one command line: quote every argument the way CommandLineToArgvW will undo it.
    std::wstring cmd;
    for (const std::string& a : argv) {
        std::wstring w = wide(a);
        if (!cmd.empty()) cmd += L' ';
        bool quote = w.empty() || w.find_first_of(L" \t\"") != std::wstring::npos;
        if (!quote) { cmd += w; continue; }
        cmd += L'"';
        size_t backslashes = 0;
        for (wchar_t c : w) {
            if (c == L'\\') { backslashes++; continue; }
            if (c == L'"') cmd.append(backslashes * 2 + 1, L'\\');
            else cmd.append(backslashes, L'\\');
            backslashes = 0;
            cmd += c;
        }
        cmd.append(backslashes * 2, L'\\');
        cmd += L'"';
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = INVALID_HANDLE_VALUE;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (!log.empty()) {
        out = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out != INVALID_HANDLE_VALUE) {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = si.hStdError = out;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        }
    }
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return int(code);
#else
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (!log.empty()) {
        posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
    }
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
}

void* load_library(const fs::path& path, std::string& error) {
#ifdef _WIN32
    HMODULE h = LoadLibraryW(path.c_str());
    if (!h) error = "LoadLibrary failed, error " + std::to_string(GetLastError());
    return reinterpret_cast<void*>(h);
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        error = e ? e : "dlopen failed";
    }
    return h;
#endif
}

void* library_symbol(void* library, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

std::string library_extension() {
#if defined(_WIN32)
    return "dll";
#elif defined(__APPLE__)
    return "dylib";
#else
    return "so";
#endif
}

unsigned hardware_threads() {
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1;
}

fs::path resource_dir(const char* argv0) {
    auto exe = exe_dir(argv0);
    if (exe.filename() == "MacOS" && exe.parent_path().filename() == "Contents") return exe.parent_path() / "Resources";
    return exe / "resources";
}

}  // namespace thps::platform
