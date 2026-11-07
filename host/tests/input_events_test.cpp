// ROM-free regression for the production pre-menu SDL action gate. The device
// query is injected so a physical device is covered without requiring hardware.
#include "input_events.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
int calls = 0;
SDL_JoystickID queried = -99;
bool virtual_query(SDL_JoystickID id) {
    ++calls;
    queried = id;
    return id == 42;
}
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
}

int main() {
    SDL_SetMainReady();   // SDL_MAIN_HANDLED: no SDL2main startup wrapper
    for (Uint32 type : {Uint32(SDL_CONTROLLERBUTTONDOWN), Uint32(SDL_CONTROLLERBUTTONUP), Uint32(SDL_CONTROLLERAXISMOTION)}) {
        SDL_Event ev{};
        ev.type = type;
        auto set_instance = [&](SDL_JoystickID id) {
            if (type == SDL_CONTROLLERAXISMOTION) ev.caxis.which = id;
            else ev.cbutton.which = id;
        };
        set_instance(7);   // physical controller, regardless of gameplay port
        calls = 0;
        require(!thps::controller_action_allowed(ev, true, virtual_query), "physical action blocked with --no-pads");
        require(calls == 1 && queried == 7, "action resolves the event's instance ID");
        calls = 0;
        require(thps::controller_action_allowed(ev, false, virtual_query), "ordinary physical-controller behavior retained");
        require(calls == 0, "ordinary behavior needs no virtual lookup");
        set_instance(42);
        require(thps::controller_action_allowed(ev, true, virtual_query), "virtual controller action admitted");
        set_instance(999);
        require(!thps::controller_action_allowed(ev, true, virtual_query), "unknown or unplugged action rejected");
        for (SDL_JoystickID invalid : {SDL_JoystickID(-1), SDL_JoystickID(-2), SDL_JoystickID(-99)}) {
            set_instance(invalid);
            calls = 0;
            require(!thps::controller_action_allowed(ev, true, virtual_query), "negative non-command instance rejected");
            require(calls == 0, "negative instance is not queried as a device");
        }
    }
    for (Uint32 type : {Uint32(SDL_CONTROLLERDEVICEADDED), Uint32(SDL_CONTROLLERDEVICEREMOVED),
                        Uint32(SDL_CONTROLLERDEVICEREMAPPED), Uint32(SDL_KEYDOWN), Uint32(SDL_MOUSEBUTTONDOWN), Uint32(SDL_QUIT)}) {
        SDL_Event ev{};
        ev.type = type;
        calls = 0;
        require(thps::controller_action_allowed(ev, true, virtual_query), "non-action event retains its existing path");
        require(calls == 0, "non-action event does not interpret unrelated union fields");
    }
    // Exercise the real instance-ID -> current-device-index query as well as the
    // injected physical/unknown cases above. This opens no game or window.
    require(SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) == 0, "SDL joystick/event subsystems initialize");
    const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 0);
    require(index >= 0, "own virtual joystick attaches");
    const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(index);
    require(instance >= 0, "actual SDL virtual instance is nonnegative");
    SDL_Event actual{};
    actual.type = SDL_CONTROLLERBUTTONDOWN;
    actual.cbutton.which = instance;
    require(thps::controller_action_allowed(actual, true), "actual SDL virtual instance allowed");
    require(SDL_JoystickDetachVirtual(index) == 0, "own virtual joystick detaches");
    require(!thps::controller_action_allowed(actual, true), "detached instance fails closed");
    // SDL2-compat rewrites a pushed negative instance to -1. Regardless of the
    // SDL implementation's transport choice, no negative action is hardware.
    SDL_Event pushed{};
    pushed.type = SDL_CONTROLLERBUTTONDOWN; pushed.cbutton.which = -2;
    pushed.cbutton.button = SDL_CONTROLLER_BUTTON_DPAD_DOWN; pushed.cbutton.state = SDL_PRESSED;
    require(SDL_PushEvent(&pushed) == 1, "own negative-ID event pushes");
    bool found = false;
    while (SDL_PollEvent(&actual)) {
        if (actual.type == SDL_CONTROLLERBUTTONDOWN) { found = true; break; }
    }
    require(found, "actual push/poll transport receives the controller action");
    require(actual.cbutton.which < 0, "transported synthetic ID remains outside hardware identity");
    require(!thps::controller_action_allowed(actual, true), "transported negative-ID action rejected");
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_EVENTS);
    std::printf("%d input-event checks passed\n", checks);
}
