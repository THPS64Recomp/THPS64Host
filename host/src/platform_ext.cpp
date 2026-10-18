// See platform_ext.h.
#include "platform_ext.h"

#include <cerrno>
#include <cstring>

#include "platform.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace thps::platform {

int run_process_cancellable(const std::vector<std::string>& argv, const fs::path& log, const std::atomic<bool>* cancel) {
    if (!cancel) return run_process(argv, log);
    if (argv.empty()) return -1;
#ifdef _WIN32
    // [Windows branch: written from the API docs, not run.] The child lives in a job object so a cancel also ends what it starts.
    auto wide = [](const std::string& u) {
        int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? size_t(n - 1) : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, w.data(), n);
        return w;
    };
    std::wstring cmd;   // CommandLineToArgvW quoting, as run_process does
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
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) return -1;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    bool cancelled = false;
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 20) != WAIT_TIMEOUT) break;
        if (cancel->load()) {
            cancelled = true;
            if (job) TerminateJobObject(job, 1);
            else TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (job) CloseHandle(job);
    return cancelled ? -2 : int(code);
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
    // Own process group, so a cancel reaches everything the compiler driver started.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, args[0], &fa, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) return -1;
    int status = 0;
    bool cancelled = false;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) return -1;
        if (!cancelled && cancel->load()) {
            cancelled = true;
            kill(-pid, SIGTERM);
        }
        struct timespec ts{0, 10 * 1000 * 1000};   // 10 ms
        nanosleep(&ts, nullptr);
    }
    if (cancelled) {
        kill(-pid, SIGKILL);   // anything in the group that ignored SIGTERM
        return -2;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
}

LockResult acquire_instance_lock(const fs::path& path, std::string& error) {
#ifdef _WIN32
    // [Windows branch: written from the API docs, not run.] A handle opened with no sharing: a second open fails with a sharing
    // violation. It stays open until the process ends; the system closes it then.
    static HANDLE held = INVALID_HANDLE_VALUE;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION) return LockResult::Busy;
        error = "cannot open " + to_utf8(path) + " (Windows error " + std::to_string(e) + ")";
        return LockResult::Error;
    }
    held = h;
    return LockResult::Acquired;
#else
    static int held = -1;
    // Close-on-exec: the compiler we start must not inherit the lock (it would outlive a cancelled app).
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = "cannot open " + to_utf8(path) + ": " + std::strerror(errno);
        return LockResult::Error;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        close(fd);
        if (e == EWOULDBLOCK || e == EAGAIN) return LockResult::Busy;
        error = "cannot lock " + to_utf8(path) + ": " + std::strerror(e);
        return LockResult::Error;
    }
    std::string pid = std::to_string(long(getpid())) + "\n";
    if (ftruncate(fd, 0) == 0) {
        ssize_t w = write(fd, pid.data(), pid.size());
        (void)w;
    }
    held = fd;   // never closed: the lock lives as long as the process
    return LockResult::Acquired;
#endif
}

bool open_folder(const fs::path& dir) {
#if defined(_WIN32)
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif defined(__APPLE__)
    return run_process({"/usr/bin/open", to_utf8(dir)}, {}) == 0;
#else
    return run_process({"xdg-open", to_utf8(dir)}, {}) == 0;
#endif
}

}  // namespace thps::platform
