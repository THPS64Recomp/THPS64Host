// More OS seams, for the first-run flow (src/firstrun.cpp, src/game_split.cpp). Same idea as platform.h, kept in its own file because
// platform.{h,cpp} are copied unchanged into thps2/thps3 (CI checks they are identical) and these are used by the split build only.
// macOS/Linux branches are run; the Windows branches are written from the API docs and have not been run (docs/WINDOWS-TEST.md).
#pragma once
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace thps::platform {

// platform::run_process, but pollable: when `*cancel` becomes true the child and what it started (clang's cc1, the linker) are
// terminated and -2 is returned. Otherwise like run_process: stdout and stderr appended to `log` (empty = inherit), the exit code
// is returned, -1 if the process could not be started.
int run_process_cancellable(const std::vector<std::string>& argv, const std::filesystem::path& log, const std::atomic<bool>* cancel);

// One instance per config folder: an exclusive lock on `path` (created if missing), held until the process exits (the OS releases it,
// also after a crash). macOS/Linux flock; Windows a handle opened without sharing.
// Busy: another process holds it. Error: `error` says why (read-only folder...).
enum class LockResult { Acquired, Busy, Error };
LockResult acquire_instance_lock(const std::filesystem::path& path, std::string& error);

// Shows a folder in the system's file manager (macOS `open`, Windows ShellExecute, Linux `xdg-open`). false: it could not be started.
bool open_folder(const std::filesystem::path& dir);

}  // namespace thps::platform
