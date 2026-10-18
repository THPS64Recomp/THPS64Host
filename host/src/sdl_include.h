// SDL2 under both include layouts: <SDL2/SDL.h> (Homebrew, vcpkg, distro packages) and the flat
// <SDL.h> of the SDL2 2.26.3 bundled with RT64 on Windows (contrib/mupen64plus-win32-deps).
#pragma once
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif
