// In-game settings menu (THPS_MENU): RmlUi drawn over the game through RT64's render hooks.
//
// Derived from Zelda64Recomp's src/ui (GPL-3.0; see NOTICE): the RT64 renderer for RmlUi (ui_renderer.cpp),
// the event-queue + draw-hook structure of ui_state.cpp, and the look of the config menu (recomp.rcss, the
// tab/option classes). Everything game-, mod- and launcher-specific is left out.
//
// Threads: handle_event() runs on the main thread (SDL events); everything RmlUi runs on RT64's render
// thread inside the draw hook. The two meet in a locked event queue and a few atomics.
#pragma once
#include <filesystem>
#include <string>

#include "../sdl_include.h"

namespace thps::ui {

// Before RT64's app->setup(): registers the render hooks (RmlUi starts on the render thread).
void set_render_hooks();
// Main thread, before the window exists. `assets` = <resources>/ui; `config_dir` holds graphics.cfg / general.cfg.
void init(const std::filesystem::path& assets, const std::filesystem::path& config_dir);
void set_window(SDL_Window* window);   // create_window()

// Main thread, every SDL event, before the game sees it. true = consumed (the menu is open, or this opened/closed it).
bool handle_event(const SDL_Event& ev);

// --control "ui ..." (test runner, main thread): "ui esc" | "ui click X Y" (window points) | "ui click #id" (centre of that element) | "ui tab N" | "ui state".
// Events go through SDL_PushEvent, so they take the same path as a person's input.
std::string control(const std::string& args);

bool menu_open();      // any thread
bool input_blocked();  // the game must ignore pads and keyboard (menu open)
bool wanted();         // the render hook has something to draw (RT64 keeps drawing it while the window is hidden)

}  // namespace thps::ui
