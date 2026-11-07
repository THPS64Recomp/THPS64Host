// SDL controller actions share this filter before either the settings menu or
// gameplay sees them. Device lifecycle events still reach input_event().
#pragma once

#include "sdl_include.h"

namespace thps {

inline bool controller_instance_is_virtual(SDL_JoystickID instance) {
    // SDL_JoystickIsVirtual takes a device INDEX, not an instance ID. Resolve it
    // afresh so unplugging or changing device order cannot admit a physical pad.
    bool is_virtual = false;
    SDL_LockJoysticks();
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_JoystickGetDeviceInstanceID(i) == instance) {
            is_virtual = SDL_JoystickIsVirtual(i) == SDL_TRUE;
            break;
        }
    }
    SDL_UnlockJoysticks();
    return is_virtual;
}

using ControllerVirtualQuery = bool (*)(SDL_JoystickID);

inline bool controller_action_allowed(const SDL_Event& ev, bool no_pads,
                                      ControllerVirtualQuery is_virtual = controller_instance_is_virtual) {
    if (!no_pads) return true;
    SDL_JoystickID instance;
    switch (ev.type) {
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            instance = ev.cbutton.which;
            break;
        case SDL_CONTROLLERAXISMOTION:
            instance = ev.caxis.which;
            break;
        default:
            return true;   // including attach/remove/remap bookkeeping
    }
    return instance >= 0 && is_virtual(instance);
}

}  // namespace thps
