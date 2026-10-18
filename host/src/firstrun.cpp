// The first-run flow (see firstrun.h).
#include "firstrun.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "librecomp/game.hpp"

#include "firstrun_ui.h"
#include "host.h"
#include "versions.h"
#include "platform.h"
#include "platform_ext.h"

namespace fs = std::filesystem;
namespace plat = thps::platform;

namespace thps::firstrun {
namespace {

std::u8string game_id_of(const VersionInfo& v) { return std::u8string(reinterpret_cast<const char8_t*>(v.game_id)); }

// "first line" / "the rest" of a multi-line message.
void split_first_line(const std::string& s, std::string& first, std::string& rest) {
    size_t nl = s.find('\n');
    first = s.substr(0, nl);
    rest = nl == std::string::npos ? "" : s.substr(nl + 1);
    while (!rest.empty() && (rest.back() == '\n' || rest.back() == ' ')) rest.pop_back();
}

// The window, opened only when something has to be shown (a start with a stored ROM and a current library never opens it).
struct Ui {
    const Params& p;
    ui::Window win;
    bool failed = false;
    explicit Ui(const Params& params) : p(params) {}
    bool interactive() {
        if (!p.ui) return false;
        if (win.is_open()) return true;
        if (failed) return false;
        std::string e;
        if (!win.open(p.hidden, e)) {
            failed = true;
            std::fprintf(stderr, "[thps] cannot open the first-run window (%s); continuing in the console\n", e.c_str());
            return false;
        }
        return true;
    }
};

ui::Page page(const char* id, const std::string& heading, ui::Tone tone, const std::string& body) {
    ui::Page pg;
    pg.id = id;
    pg.heading = heading;
    pg.tone = tone;
    pg.body = body;
    return pg;
}

// A page with only a Quit button (the end of the road).
int fatal_page(Ui& ui, const char* id, const std::string& heading, const std::string& body, int code) {
    std::fprintf(stderr, "[thps] %s: %s\n", heading.c_str(), body.c_str());
    game::logf("%s: %s", heading.c_str(), body.c_str());
    if (!ui.interactive()) return code;
    ui::Page pg = page(id, heading, ui::Tone::Error, body);
    pg.buttons = {{"quit", "Quit", true}};
    ui::Result r = ui.win.run(pg);
    ui.win.close();
    return r.button == "$timeout" ? kScriptEnded : code;
}

bool mock_external() {
    return ui::test_mode() || std::getenv("THPS_FAKE_NO_COMPILER") != nullptr || std::getenv("THPS_MOCK_INSTALL") != nullptr;
}

void open_log_folder(const fs::path& dir) {
    if (ui::test_mode()) {
        std::printf("[ui] mock: would open the folder %s\n", plat::to_utf8(dir).c_str());
        std::fflush(stdout);
        return;
    }
    if (!plat::open_folder(dir)) std::fprintf(stderr, "[thps] could not open %s\n", plat::to_utf8(dir).c_str());
}

// macOS: Apple's own installer dialog, started by the user's click. Never run in tests (mocked).
void install_command_line_tools() {
#if defined(__APPLE__)
    if (mock_external()) {
        std::printf("[ui] mock: would run: xcode-select --install\n");
        std::fflush(stdout);
        game::logf("mock install: xcode-select --install");
        return;
    }
    game::logf("running xcode-select --install (the user clicked Install)");
    plat::run_process({"/usr/bin/xcode-select", "--install"}, {});
#endif
}

const char* verdict_id(RomVerdict v) {
    switch (v) {
        case RomVerdict::ReadError: return "rom_read_error";
        case RomVerdict::NotARom: return "rom_not_a_rom";
        case RomVerdict::NotGame: { static const std::string id = std::string("rom_not_") + game::family; return id.c_str(); }
        case RomVerdict::WrongVersion: return "rom_wrong_version";
        case RomVerdict::NoRecipe: return "rom_no_recipe";
        default: return "rom_ok";
    }
}
const char* verdict_heading(RomVerdict v) {
    switch (v) {
        case RomVerdict::ReadError: return "That file could not be read";
        case RomVerdict::NotARom: return "That is not an N64 ROM";
        case RomVerdict::NotGame: return "That is a different game";
        case RomVerdict::WrongVersion: return "That version is not supported";
        case RomVerdict::NoRecipe: return "That version is not supported yet";
        default: return "";
    }
}

enum class Offer { Quit, File, None };

// The welcome / "that ROM does not work" page. Returns the file the user picked or dropped.
Offer ask_for_rom(Ui& ui, const std::string& heading, const char* id, const std::string& notice, fs::path& out, int& exit_code) {
    ui::Page pg;
    if (notice.empty()) {
        pg = page("welcome", "Choose your game", ui::Tone::Normal,
                  std::string("To play, this app needs your own copy of ") + game::rom_title_short + " for the Nintendo 64: a ROM file (.z64, .n64 or .v64) dumped from a cartridge "
                  "you own.\n\nThe file stays on this computer; nothing is uploaded. The first time, the app turns it into a game library for your machine, "
                  "which takes a few seconds to a minute.");
        pg.note = "Drag the ROM file onto this window, or click Choose ROM.";
        pg.buttons = {{"choose", "Choose ROM...", true}, {"quit", "Quit", false}};
    } else {
        pg = page(id, heading, ui::Tone::Error, notice);
        pg.note = "Drag another file onto this window, or click Choose another ROM.";
        pg.buttons = {{"choose", "Choose another ROM...", true}, {"quit", "Quit", false}};
    }
    for (;;) {
        ui::Result r = ui.win.run(pg);
        if (r.button == "$drop") { out = plat::path_from_utf8(r.path); game::logf("ROM dropped: %s", r.path.c_str()); return Offer::File; }
        if (r.button == "choose") {
            std::string path, err;
            if (ui.win.take_scripted_pick(path)) {
                out = plat::path_from_utf8(path);
                game::logf("ROM picked (scripted): %s", path.c_str());
                return Offer::File;
            }
            if (ui::test_mode()) {
                std::printf("[ui] test mode: no scripted pick=; the file dialog is not opened\n");
                std::fflush(stdout);
                continue;
            }
            if (ui::native_pick_rom(path, err)) {
                out = plat::path_from_utf8(path);
                game::logf("ROM picked: %s", path.c_str());
                return Offer::File;
            }
            if (!err.empty()) {
                pg.note = "The file dialog failed (" + err + "). Drag the ROM file onto this window instead.";
            }
            continue;
        }
        exit_code = r.button == "$timeout" ? kScriptEnded : kUserQuit;
        game::logf("first-run screen closed by the user (%s)", r.button.c_str());
        return Offer::Quit;
    }
}

// Stage 1: a ROM this app can build. 0 = chosen is set.
int find_rom(Ui& ui, const Params& p, const VersionInfo*& chosen) {
    const VersionInfo* forced = p.version_arg.empty() ? nullptr : versions::find(p.version_arg);
    fs::path offered = p.rom_arg;
    std::string notice, heading;
    const char* notice_id = "";
    for (;;) {
        if (!offered.empty()) {
            RomCheck ck = check_rom_file(offered);
            game::logf("ROM offered: %s -> %s", plat::to_utf8(offered).c_str(), verdict_id(ck.verdict));
            if (ck.verdict == RomVerdict::Ok && forced && ck.version != forced) {
                notice = std::string("That ROM is ") + ck.version->display_name + ", but --version " + p.version_arg + " asks for " + forced->display_name + ".";
                heading = "That is another version";
                notice_id = "rom_other_version";
            } else if (ck.verdict == RomVerdict::Ok) {
                recomp::RomValidationError e = recomp::select_rom(offered, game_id_of(*ck.version));   // stores <game_id>.z64 in the config folder
                recomp::check_all_stored_roms();
                if (e == recomp::RomValidationError::Good && recomp::is_rom_valid(game_id_of(*ck.version))) {
                    chosen = ck.version;
                    return kReady;
                }
                const uint64_t free = game::free_bytes(p.config_dir);
                heading = free < (64ull << 20) ? "Not enough disk space" : "A copy of the ROM could not be saved";
                notice_id = free < (64ull << 20) ? "rom_disk_full" : "rom_not_saved";
                notice = std::string("The ROM is fine, but the app could not keep a copy of it in ") + plat::to_utf8(p.config_dir) +
                         (free < (64ull << 20) ? ": only " + std::to_string(free >> 20) + " MB are free there. Free some space and choose the file again."
                                               : ". Check that the folder is writable and the disk is not full, or start the app with --config FOLDER to use another place.");
            } else {
                notice = ck.text;
                heading = verdict_heading(ck.verdict);
                notice_id = verdict_id(ck.verdict);
            }
            std::fprintf(stderr, "[thps] %s\n", notice.c_str());
            game::logf("%s", notice.c_str());
            if (!ui.interactive()) return kNoRom;
            offered.clear();
        } else {
            recomp::check_all_stored_roms();
            for (const VersionInfo& v : all_versions) {
                if (forced && forced != &v) continue;
                if (versions::runnable(v) && recomp::is_rom_valid(game_id_of(v))) { chosen = &v; return kReady; }
            }
            if (forced) {
                std::fprintf(stderr, "[thps] no stored ROM for %s; pass --rom /path/to/rom.z64 (.n64/.v64 also accepted)\n", forced->display_name);
                if (!ui.interactive()) return kNoRom;
            } else if (!ui.interactive()) {
                std::fprintf(stderr, "[thps] no stored ROM; pass --rom /path/to/%s.z64 (.n64/.v64 also accepted)\n[thps] this build runs:%s\n",
                             game::family, versions::supported_list().c_str());
                return kNoRom;
            }
        }
        int code = kUserQuit;
        Offer o = ask_for_rom(ui, heading, notice_id, notice, offered, code);
        if (o == Offer::Quit) return code;
    }
}

// Stage 2: the game library is current and loaded. 0 = ready.
int build_library(Ui& ui, const Params& p, const VersionInfo& v) {
    const fs::path stored_rom = p.config_dir / (std::string(v.game_id) + ".z64");
    game::Options opt = p.game_opt;
    std::string why;
    if (opt.live) {   // --live-game: translated in memory by ensure(); no window
        std::string err;
        if (!game::ensure(v, stored_rom, p.config_dir, opt, err)) { std::fprintf(stderr, "[thps] cannot prepare the game for %s:\n%s\n", v.display_name, err.c_str()); return kBuildFailed; }
        return kReady;
    }
    bool compiler_help_shown = false;
    for (;;) {
        if (game::load_if_current(v, p.config_dir, opt, why)) {
            if (opt.library.empty() && !opt.live) game::logf("the game library is current: no build needed");
            return kReady;
        }
        game::logf("the game library has to be built: %s", why.c_str());
        if (!opt.library.empty()) {   // --game-lib: never build
            std::fprintf(stderr, "[thps] cannot prepare the game for %s:\n%s\n", v.display_name, why.c_str());
            return kBuildFailed;
        }
        game::Progress prog;
        std::string err;
        bool ok = false;
        std::atomic<bool> finished{false};
        game::Options bo = opt;
        bo.progress = &prog;
        if (!ui.interactive()) {
            bo.progress = nullptr;
            ok = game::ensure(v, stored_rom, p.config_dir, bo, err);
            if (!ok) { std::fprintf(stderr, "[thps] cannot prepare the game for %s:\n%s\n", v.display_name, err.c_str()); return kBuildFailed; }
            return kReady;
        }
        std::thread worker([&] {
            ok = game::ensure(v, stored_rom, p.config_dir, bo, err);
            finished = true;
        });
        ui::Page pg = page("building", std::string("Preparing ") + game::rom_title_short + " from your cartridge dump...", ui::Tone::Normal,
                           "This happens once and takes a few seconds to a minute. You can cancel; the next start begins again.");
        pg.progress = &prog;
        pg.buttons = {{"cancel", "Cancel", false}};
        ui::Result r = ui.win.run(pg, [&](ui::Page&) { return finished.load(); });
        if (r.button != "$done") {
            // Cancel, Esc or the close button: stop the compiler and wait for the build to clean up.
            prog.cancel = true;
            game::logf("the user cancelled the build");
            pg.heading = "Cancelling...";
            pg.body = "Stopping the compiler and removing what was built so far.";
            pg.buttons.clear();
            ui.win.run(pg, [&](ui::Page&) { return finished.load(); });
        }
        worker.join();
        if (prog.cancel.load()) {
            game::logf("exiting after cancel");
            ui.win.close();
            return r.button == "$timeout" ? kScriptEnded : kUserQuit;
        }
        if (ok) return kReady;

        // ---- a failure: a page with a way forward
        const game::Failure f = prog.failure.load();
        std::fprintf(stderr, "[thps] cannot prepare the game for %s:\n%s\n", v.display_name, err.c_str());
        ui::Page ep;
        if (f == game::Failure::NoCompiler) {
#if defined(_WIN32)
            // The compiler ships with the app (resources/toolchain): nothing to download, the installation is incomplete.
            ep = page("no_compiler", "The app's compiler is missing", ui::Tone::Warn, "");
#else
            ep = page("no_compiler", "One more download is needed", ui::Tone::Warn, "");
#endif
#if defined(__APPLE__)
            ep.body = "Apple's Command Line Tools provide the compiler this app uses, once, to build the game from your ROM. They are free (about 1 GB) and "
                      "come from Apple.\n\nClick Install. macOS asks you to confirm and downloads them. When it says it is finished, come back here and click Retry.";
            ep.buttons = {{"install", "Install...", !compiler_help_shown}, {"retry", "Retry", compiler_help_shown}, {"quit", "Quit", false}};
#elif defined(_WIN32)
            ep.body = err + "\n\nUnzip the app again (all of it, including the resources folder next to the program), then click Retry.";
            ep.buttons = {{"retry", "Retry", true}, {"quit", "Quit", false}};
#else
            ep.body = err + "\n\nInstall it, then click Retry.";
            ep.buttons = {{"retry", "Retry", true}, {"quit", "Quit", false}};
#endif
        } else if (f == game::Failure::DiskFull) {
            ep = page("disk_full", "Not enough disk space", ui::Tone::Error, err);
            ep.buttons = {{"retry", "Retry", true}, {"quit", "Quit", false}};
        } else {
            std::string first, rest;
            split_first_line(err, first, rest);
            ep = page("build_failed", "The game could not be prepared", ui::Tone::Error, first);
            ep.detail = rest;
            ep.note = "The full log is first-run.log in the log folder.";
            ep.buttons = {{"open_logs", "Open log folder", false}, {"retry", "Retry", true}, {"quit", "Quit", false}};
        }
        for (;;) {
            ui::Result er = ui.win.run(ep);
            if (er.button == "install") {
                install_command_line_tools();
                compiler_help_shown = true;
                ep.note = "The installer is open. Click Retry when it has finished.";
                for (ui::Button& b : ep.buttons) b.primary = b.id == "retry";
                continue;
            }
            if (er.button == "open_logs") { open_log_folder(p.config_dir); continue; }
            if (er.button == "retry") { opt.force_rebuild = false; game::logf("Retry"); break; }
            ui.win.close();
            return er.button == "$timeout" ? kScriptEnded : kUserQuit;
        }
    }
}

}  // namespace

int show_fatal(const std::string& heading, const std::string& message, bool ui_on, bool hidden, int code) {
    Params p;
    p.ui = ui_on;
    p.hidden = hidden;
    Ui ui(p);
    return fatal_page(ui, "fatal", heading, message, code);
}

int prepare(const Params& p, const VersionInfo*& chosen) {
    Ui ui(p);

    // One instance per config folder. The lock file is also the first write into the folder, so it tells a read-only one.
    std::string err;
    switch (plat::acquire_instance_lock(p.config_dir / (std::string(game::family) + ".lock"), err)) {
        case plat::LockResult::Acquired: break;
        case plat::LockResult::Busy:
            return fatal_page(ui, "already_running", "Already running",
                              std::string(game::rom_title_short) + " is already open (or is still preparing the game) with this settings folder:\n  " + plat::to_utf8(p.config_dir) +
                                  "\n\nOnly one copy can use the same folder at a time. Switch to the other window, or close it and start this one again.",
                              kAlreadyRunning);
        case plat::LockResult::Error:
            return fatal_page(ui, "folder_not_writable", "The settings folder cannot be written",
                              err + "\n\nThe app keeps your settings, saves and the game it builds from your ROM in:\n  " + plat::to_utf8(p.config_dir) +
                                  "\nMake sure you may write there (a read-only or full disk is the usual cause), or start the app with --config FOLDER to use another place.",
                              kFolderNotWritable);
    }
    game::log_open(p.config_dir);
    game::logf("start: config %s, ui %s%s", plat::to_utf8(p.config_dir).c_str(), p.ui ? "window" : "console", ui::test_mode() ? ", scripted" : "");

    int rc = find_rom(ui, p, chosen);
    if (rc != kReady) return rc;
    game::logf("ROM: %s", chosen->display_name);
    rc = build_library(ui, p, *chosen);
    if (ui.win.is_open()) ui.win.close();
    return rc;
}

}  // namespace thps::firstrun
