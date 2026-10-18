// The game library's own code (ours, not ROM-derived; shipped as source and compiled on the user's machine
// together with the N64Recomp output). See src/game_abi.h and the THPS1 repository's docs/internals/recomp.md.
//
//   cc -c funcs_N.c ...            the recompiled functions (generated)
//   c++ -c game_lib.cpp            this file: the imports, the section table, the init entry point
//   c++ -c game_rsp.cpp n_aspMain.cpp
//   link -> thps1_game_<class>.dylib | .so | .dll
//
// Include paths: the generated directory (funcs.h, recomp_overlays.inl), the engine's src/ (game_abi.h,
// game_imports.def, thps_hooks.h) and the runtime's headers (recomp.h, librecomp/sections.h).
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game_abi.h"
#include "thps_hooks.h"

// The generated tables: section_table[], num_sections, overlay_sections_by_index[]; they name the functions
// through funcs.h. (N64Recomp's recomp_overlays.inl, including its own #includes.)
#include "recomp_overlays.inl"

namespace {

enum Import {
#define THPS_IMPORT(kind, name) IMP_##name,
#include "game_imports.def"
#undef THPS_IMPORT
    IMP_COUNT
};
void* g_imp[IMP_COUNT];

}  // namespace

// --- The imports: one forwarder per name -----------------------------------------------------------------
// Function pointer types by kind. The compiler turns each forwarder into a tail call.
extern "C" {
#define THPS_IMPORT(kind, name) THPS_FWD_##kind(name)
#define THPS_FWD_CTX(name) \
    void name(uint8_t* rdram, recomp_context* ctx) { reinterpret_cast<void (*)(uint8_t*, recomp_context*)>(g_imp[IMP_##name])(rdram, ctx); }
#define THPS_FWD_CTX_INT(name) \
    void name(uint8_t* rdram, recomp_context* ctx, int a) { reinterpret_cast<void (*)(uint8_t*, recomp_context*, int)>(g_imp[IMP_##name])(rdram, ctx, a); }
#define THPS_FWD_CTX_RET(name) \
    int name(uint8_t* rdram, recomp_context* ctx) { return reinterpret_cast<int (*)(uint8_t*, recomp_context*)>(g_imp[IMP_##name])(rdram, ctx); }
#define THPS_FWD_GET_FUNCTION(name) \
    recomp_func_t* name(int32_t vram) { return reinterpret_cast<recomp_func_t* (*)(int32_t)>(g_imp[IMP_##name])(vram); }
#define THPS_FWD_PAUSE_SELF(name) \
    void name(uint8_t* rdram) { reinterpret_cast<void (*)(uint8_t*)>(g_imp[IMP_##name])(rdram); }
#define THPS_FWD_SWITCH_ERROR(name) \
    void name(const char* func, uint32_t vram, uint32_t jtbl) { reinterpret_cast<void (*)(const char*, uint32_t, uint32_t)>(g_imp[IMP_##name])(func, vram, jtbl); }
#define THPS_FWD_DO_BREAK(name) \
    void name(uint32_t vram) { reinterpret_cast<void (*)(uint32_t)>(g_imp[IMP_##name])(vram); }
#include "game_imports.def"
#undef THPS_IMPORT
}

// --- section_addresses: the relocated addresses of the code sections (THPS2/3's front-end screens are relocatable; recomp.h declares it,
// librecomp defines it, the engine fills it in). The library cannot import the engine's variable, so it has its own, bound at game start.
extern "C" {
int32_t* section_addresses = nullptr;
}

// --- A marker that is in game libraries and nowhere else: tools/scan_bundle.py --no-rom refuses to find it in any binary of a release
// bundle, so a translated game that got linked into the engine (the engine is built from the same CMake file as the static dev build)
// fails the ROM-free gate even where the symbol checks prove little (a PE has no symbol table). Keep the text in sync with MARKER there.
static const char k_game_library_marker[] __attribute__((used)) = "thps-game-library-marker/1";

// --- The front-end overlay table (THPS2/3). The first run writes it from the user's ROM (src/game_split.cpp, firstrun/overlay_rom.cpp):
// rows of { name, rom text offset, size, code blob offset, fake base }.
#ifdef THPS_HAS_OVERLAYS
static const ThpsOverlay k_overlays[] = {
#include "thps_overlay_table.inc"
};
#endif

// --- The audio microcode (game_rsp.cpp) -----------------------------------------------------------------
extern "C" int thps_game_aspmain(uint8_t* rdram, uint32_t ucode_addr, uint8_t* host_dmem);

// --- Entry point ---------------------------------------------------------------------------------------
#ifndef THPS_CODE_CLASS
#error "compile with -DTHPS_CODE_CLASS=\"us11\""
#endif

static const ThpsGameInfo k_info = {
    THPS_GAME_ABI,
    sizeof(ThpsGameInfo),
    THPS_CODE_CLASS,
    section_table,
    num_sections,
    overlay_sections_by_index,
    sizeof(overlay_sections_by_index) / sizeof(overlay_sections_by_index[0]),
    recomp_entrypoint,
    thps_game_aspmain,
    &section_addresses,
#ifdef THPS_HAS_OVERLAYS
    k_overlays,
    sizeof(k_overlays) / sizeof(k_overlays[0]),
#else
    nullptr,
    0,
#endif
};

#if defined(_WIN32)
#define THPS_EXPORT extern "C" __declspec(dllexport)
#else
#define THPS_EXPORT extern "C" __attribute__((visibility("default")))
#endif

THPS_EXPORT const ThpsGameInfo* thps_game_init(const ThpsHostApi* api) {
    if (!api || api->abi != THPS_GAME_ABI || api->num_imports != IMP_COUNT || !api->imports) return nullptr;
    std::memcpy(g_imp, api->imports, sizeof g_imp);
    return &k_info;
}
