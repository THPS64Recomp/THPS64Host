// The engine's side of src/game_abi.h: the addresses of everything the game library imports, in
// game_imports.def order. A separate file with no includes, because the names are declared here with a
// bare `void()` type (recomp.h and thps_hooks.h declare them with their real signatures; the library
// calls them with those, through the forwarders in game-lib/game_lib.cpp).
#include <cstddef>

extern "C" {
#define THPS_IMPORT(kind, name) void name();
#include "game_imports.def"
#undef THPS_IMPORT
}

namespace thps::game {
extern void* const g_import_table[];
extern const size_t g_import_count;

void* const g_import_table[] = {
#define THPS_IMPORT(kind, name) reinterpret_cast<void*>(&name),
#include "game_imports.def"
#undef THPS_IMPORT
};
const size_t g_import_count = sizeof(g_import_table) / sizeof(g_import_table[0]);

// The same names as strings (LiveRecomp mode looks the runtime's native implementations up by name).
extern const char* const g_import_names[];
const char* const g_import_names[] = {
#define THPS_IMPORT(kind, name) #name,
#include "game_imports.def"
#undef THPS_IMPORT
};
}  // namespace thps::game
