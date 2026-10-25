// The boundary between the engine (host + N64ModernRuntime + RT64: no ROM-derived bytes) and the game
// library (the recompiled code, built on the user's machine from their ROM). Design and rationale:
// the THPS1 repository's docs/internals/recomp.md. Only built with -DTHPS_SPLIT_GAME_LIB=ON.
//
// The library never links against the engine. The engine hands it a table of function pointers, in the
// order of src/game_imports.def; the library defines the imported names itself as one-line forwarders
// (game_lib.cpp), so the same code builds as a Mach-O dylib, an ELF .so or a Windows DLL (a DLL cannot have
// unresolved imports) without export tables or import libraries.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "recomp.h"
#include "librecomp/sections.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bump when ThpsHostApi, ThpsGameInfo or game_imports.def change. A library built for another ABI is rebuilt.
// 2: ThpsGameInfo carries the class's front-end overlay table (THPS2/3: built from the user's ROM at first run).
// 3: game_imports.def gains thps_music_load_song (src/music.cpp).
// 4: game_imports.def gains thps_memset and thps_memcpy (src/hooks.cpp).
#define THPS_GAME_ABI 4

struct ThpsHostApi {
    uint32_t abi;           // THPS_GAME_ABI
    uint32_t num_imports;   // entries in `imports`
    void* const* imports;   // function pointers, in game_imports.def order
};

// One front-end screen of a class (games with overlays; thps::OverlayInfo has the same fields): ROM offset and size of its code blob,
// offset of its pre-linked copy in the class's code blob, its fake link base. Rows come from overlay_rom.cpp's table (first run).
struct ThpsOverlay {
    const char* name;
    uint32_t rom, size, blob_off, fake_base;
};

struct ThpsGameInfo {
    uint32_t abi;
    uint32_t struct_size;
    const char* code_class;                 // "us11"
    SectionTableEntry* sections;            // recomp_overlays.inl's section_table
    size_t num_sections;
    int* overlay_sections_by_index;
    size_t num_overlay_sections;
    void (*entrypoint)(uint8_t* rdram, recomp_context* ctx);   // the class's Boot_Entry
    // The audio microcode (RSPRecomp output). `host_dmem` is the runtime's 4 KiB DMEM: it is copied in and
    // back out around the call (the library keeps a private DMEM because a DLL cannot import the runtime's array).
    int (*aspmain)(uint8_t* rdram, uint32_t ucode_addr, uint8_t* host_dmem);   // returns a RspExitReason
    // The library's own `section_addresses` pointer (the recompiled screens read it to find where the game relocated them; a DLL cannot import
    // librecomp's variable). The engine stores its array here when the game starts (thps::game::bind_library, after librecomp's init_overlays).
    int32_t** section_addresses_slot;
    const struct ThpsOverlay* overlays;   // the class's front-end screens (src/hooks.cpp maps them when the game DMAs them); null for a game without
    size_t num_overlays;
};

// The one symbol the engine looks up in the library.
typedef const struct ThpsGameInfo* (*thps_game_init_fn)(const struct ThpsHostApi* api);
#define THPS_GAME_INIT_SYMBOL "thps_game_init"

#ifdef __cplusplus
}
#endif
