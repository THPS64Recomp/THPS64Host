// SDL_syswm.h under both include layouts (see sdl_include.h). Only Windows (HWND) and macOS (NSWindow)
// need the native handle; on Linux RT64 takes the SDL_Window itself (RT64_SDL_WINDOW_VULKAN).
#pragma once
#include "sdl_include.h"
#if __has_include(<SDL2/SDL_syswm.h>)
#include <SDL2/SDL_syswm.h>
#else
#include <SDL_syswm.h>
#endif
