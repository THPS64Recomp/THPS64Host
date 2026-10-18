// First-run support for the front-end overlays of THPS2 / THPS3.
//
// The static build turns the 16 relocatable front-end screens into N64Recomp input with Python
// (decomp/common/overlay_recomp.py). A user's machine has no Python, so this module does the ROM-reading half in C++:
//
//   recipe (shipped, ROM-free)   overlay names, ROM offsets and sizes, function starts/sizes/names, a fingerprint
//   + the user's ROM             the code blobs and their relocation streams (never shipped)
//   + the class's main codeblob  (the inflated main image the first run already makes)
//   -> blob      the main codeblob with every overlay appended, "pre-linked" at a fake base (N64Recomp's ROM input)
//   -> syms      one relocatable `[[section]]` per overlay (functions from the recipe, relocs decoded from the ROM)
//   -> table     the `{ name, rom, size, blob offset, fake base }` rows the host's overlay loader hook needs
//
// Output is byte-identical to overlay_recomp.py's (tests: decomp/common/firstrun/test_overlay_rom.py). Standard library
// only, so it can be compiled into the host, a game-library build step, or the stand-alone CLI (overlay_rom_cli.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace thps::firstrun {

struct OverlayFunc {
    uint32_t off = 0;     // byte offset in the overlay blob
    uint32_t size = 0;
    std::string name;     // empty: the default "ovl_<screen>_<OFFSET:05X>"; set for names the host hooks by (e.g. Ovl_IntroUpdate)
};

struct OverlayDef {
    std::string name;                // the screen's name in the game's overlay table ("t2_menuwheel")
    uint32_t code_rom = 0;           // ROM offset of the code+rodata blob (the table's link-time address, already moved to the real asset root)
    uint32_t size = 0;
    uint32_t reloc_rom = 0;          // ROM offset of the relocation stream
    uint32_t reloc_size = 0;
    std::vector<OverlayFunc> funcs;  // sorted by offset
};

struct OverlayRecipe {
    uint32_t vbase = 0x80C00000;     // fake link base of overlay 0; overlay k sits at vbase + k * stride
    uint32_t stride = 0x00080000;
    uint64_t rom_xxh3 = 0;           // XXH3-64 of ROM[first blob .. end of last stream); 0 = do not check
    std::vector<OverlayDef> overlays;
};

// Parses the recipe text (overlays.txt, see the generator tools/gen_overlay_recipe.py for the format).
bool parse_overlay_recipe(const std::string& text, OverlayRecipe& out, std::string& error);

// The ROM byte range the recipe's blobs and streams cover: [begin, end).
bool overlay_rom_range(const OverlayRecipe& r, uint32_t& begin, uint32_t& end);
// XXH3-64 of that range (a fingerprint, stored in the recipe; false: the ROM is too short).
bool overlay_rom_hash(const OverlayRecipe& r, const std::vector<uint8_t>& rom, uint64_t& hash, std::string& error);

struct OverlayOutput {
    std::vector<uint8_t> blob;   // the class's main codeblob + the pre-linked overlays (16-byte aligned, as overlay_recomp.py)
    std::string syms_sections;   // text to append to the class's syms.toml (see append_syms)
    std::string table_inc;       // the overlay table rows, one `{ ... },` line each (no header comment)
    size_t functions = 0, relocs = 0;
};

// `rom` is the big-endian image. `main_blob` is the class's code blob WITHOUT overlays (what make_blob in game_split.cpp makes).
// Checks the recipe's ROM fingerprint first, so a modified dump is refused with a message instead of failing inside N64Recomp.
bool build_overlays(const OverlayRecipe& r, const std::vector<uint8_t>& rom, const std::vector<uint8_t>& main_blob, OverlayOutput& out,
                    std::string& error);

// base syms.toml + the overlay sections, as overlay_recomp.py joins them (trailing newlines of `base` are normalised to one).
std::string append_syms(const std::string& base_syms, const OverlayOutput& o);

// XXH3-64 of a byte range (the same function the host's first run uses for its fingerprints).
uint64_t xxh3_64(const uint8_t* data, size_t n);

}  // namespace thps::firstrun
