// The first-run window: a plain SDL window drawn with SDL_Renderer and a small built-in bitmap font (no RecompFrontend: it is unlicensed;
// no SDL_ttf: no new dependency). Used before RT64 starts: ROM picker, build progress, errors. Split build only.
//
// Tests drive it without a human: THPS_UI_ACTIONS="shot=NAME|click=BUTTON|pick=PATH|drop=PATH|wait=SEC|waitpage=ID|key=enter|quit" (see Window::run),
// THPS_UI_SHOT_DIR (where `shot=` writes BMP files). With THPS_UI_ACTIONS set, external actions (the Command Line Tools installer, the file
// manager) are mocked: they are logged, not run.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "game_split.h"

namespace thps::ui {

enum class Tone { Normal, Warn, Error, Ok };

struct Button {
    std::string id;       // "choose", "quit", "retry", "cancel", "install", "open_logs"...
    std::string label;
    bool primary = false;
};

struct Page {
    std::string id;       // for the log and tests: "welcome", "building", "rom_not_<family>"...
    std::string heading;
    Tone tone = Tone::Normal;
    std::string body;     // wrapped to the window; "\n" breaks lines (two leading spaces stay as a hanging indent)
    std::string detail;   // dimmer, below the body (the tail of a compiler log)
    std::string note;     // dim line above the buttons
    std::vector<Button> buttons;
    const game::Progress* progress = nullptr;   // draws the step list, bar and elapsed time
};

struct Result {
    // The id of the button pressed, or: "$close" (window closed, Cmd-Q), "$drop" (a file was dropped: `path`), "$done" (tick() said so),
    // "$timeout" (a test script ran out and nobody pressed anything).
    std::string button;
    std::string path;
};

class Window {
  public:
    ~Window() { close(); }
    bool open(bool hidden, std::string& error);
    void close();
    bool is_open() const { return window_ != nullptr; }

    // Shows `page` until a button is pressed, a file is dropped or the window is closed. `tick` runs about 30 times a second;
    // when it returns true the call ends with "$done". May be called again with the next page.
    Result run(Page& page, const std::function<bool(Page&)>& tick = {});

    // Test scripts: a `pick=PATH` action is consumed here by whoever handles the "choose" button (instead of opening the OS dialog).
    bool take_scripted_pick(std::string& path);

  private:
    struct Impl;
    void* window_ = nullptr;
    Impl* impl_ = nullptr;
};

// The OS file dialog (nativefiledialog-extended, already linked by RT64). false: cancelled or failed (`error` empty when cancelled).
bool native_pick_rom(std::string& path, std::string& error);

// THPS_UI_ACTIONS is set: external actions are mocked, and a screen nobody answers ends with "$timeout".
bool test_mode();

}  // namespace thps::ui
