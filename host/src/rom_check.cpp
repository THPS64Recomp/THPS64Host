// ROM checks for the THPS hosts. librecomp's select_rom() says only *what* failed
// (recomp::RomValidationError) and only for one game; RecompFrontend's launcher then shows a generic
// text that asks for "the NTSC-U N64 version", which doesn't help here: a game has several releases
// (THPS1: USA v1.0 and v1.1 are both NTSC-U, and the Europe release is the same program as v1.1).
// Here a file is identified against the version table (src/versions.h, generated from
// decomp/common/games.toml): by hash first, then by header, and the message names what the file is and
// which versions this build runs. Used by --rom, --check-rom and the launcher's Load ROM (frontend
// build). No game data here: only header fields of the user's file are read; the table holds hashes.
#include <cstdio>
#include <string>
#include <string_view>

#include "librecomp/game.hpp"

#include "host.h"
#include "versions.h"

namespace {
const char* region_of(char c) {
    switch (c) {
        case 'E': return "USA";
        case 'P': return "Europe";
        case 'D': return "Germany";
        case 'F': return "France";
        case 'J': return "Japan";
        default: return "unknown region";
    }
}
}  // namespace

const char* thps::rom_error_text(int error) {
    // Built once per process (the game title comes from the game profile).
    static const std::string good = std::string("OK: a supported ") + thps::game::rom_title_short + " ROM.";
    static const std::string not_game = std::string("This N64 ROM isn't ") + thps::game::rom_title_short + ".";
    static const std::string wrong_version = std::string("This is ") + thps::game::rom_title_short + ", but not a version this build runs, or the ROM was modified.";
    switch (recomp::RomValidationError(error)) {
        case recomp::RomValidationError::Good: return good.c_str();
        case recomp::RomValidationError::FailedToOpen: return "The file couldn't be opened.";
        case recomp::RomValidationError::NotARom: return "This file isn't an N64 ROM (.z64, .n64 or .v64).";
        case recomp::RomValidationError::IncorrectRom: return not_game.c_str();
        case recomp::RomValidationError::NotYet: return "This game isn't supported yet.";
        case recomp::RomValidationError::IncorrectVersion: return wrong_version.c_str();
        default: return "The ROM couldn't be loaded (unknown error).";
    }
}

thps::RomCheck thps::check_rom_file(const std::filesystem::path& rom) {
    using E = recomp::RomValidationError;
    using V = RomVerdict;
    RomCheck r;
    r.id = versions::identify(rom);
    const auto& id = r.id;
    const std::string file = "\"" + rom.filename().string() + "\"";
    const std::string list = versions::supported_list();
    const std::string runs = list.empty() ? "\n\nThis app runs no ROM version." : "\n\nThis app can run:" + list;
    if (!id.read_ok) { r.verdict = V::ReadError; r.error = int(E::FailedToOpen); }
    else if (!id.is_rom) { r.verdict = V::NotARom; r.error = int(E::NotARom); }
    else if (id.match && versions::runnable(*id.match)) { r.verdict = V::Ok; r.error = int(E::Good); r.version = id.match; }
    else if (id.match) { r.verdict = V::NoRecipe; r.error = int(E::IncorrectVersion); }
    else if (id.by_header || id.looks_like_game) { r.verdict = V::WrongVersion; r.error = int(E::IncorrectVersion); }
    else { r.verdict = V::NotGame; r.error = int(E::IncorrectRom); }

    char buf[600];
    char rev[40] = "";
    if (id.is_rom && id.looks_like_game && thps::game::revision_note[0]) std::snprintf(rev, sizeof rev, thps::game::revision_note, unsigned(id.revision));
    switch (r.verdict) {
        case V::Ok:
            r.text = std::string("OK: ") + id.match->display_name + ".";
            return r;
        case V::ReadError:
            r.text = "The file " + file + " could not be read. Check that it exists, that you may open it, and that it is not on a disconnected drive.";
            return r;
        case V::NotARom:
            std::snprintf(buf, sizeof buf, "%s is not an N64 ROM. A cartridge dump is a .z64, .n64 or .v64 file of 8 to 64 MB; if you have a zip or 7z archive, extract it first.",
                          file.c_str());
            r.text = buf;
            return r;
        case V::NotGame:
            std::snprintf(buf, sizeof buf, "%s is an N64 ROM, but not %s. It is \"%s\" (game code %s, %s).", file.c_str(), thps::game::rom_title_short, id.internal_name.c_str(),
                          id.game_code, region_of(id.game_code[3]));
            r.text = buf;
            r.text += std::string("\nThis app plays ") + thps::game::about_text + "." + runs;
            return r;
        case V::WrongVersion:
            std::snprintf(buf, sizeof buf, "%s looks like %s (\"%s\", game code %s, %s, header revision %u%s), but it is not a dump this app knows: SHA-1 %s.",
                          file.c_str(), thps::game::rom_title_short, id.internal_name.c_str(), id.game_code, region_of(id.game_code[3]), unsigned(id.revision), rev, id.sha1);
            r.text = buf;
            if (id.by_header) r.text += std::string("\n\nIts header says ") + id.by_header->display_name + ", but the hash does not match any known dump of it: the file is modified or a bad dump.";
            else r.text += "\nIt is a release this app does not know (a modified or damaged dump, or another release).";
            r.text += runs;
            return r;
        case V::NoRecipe:
#ifdef THPS_SPLIT_GAME_LIB
            // The release app builds the game from recipes it ships (recipes/<class>): none for this release yet.
            r.text = file + " is " + id.match->display_name + ", a known release of the game. This version of the app cannot build it yet: "
                     "it has no recipe for that release (support is planned for a later version)." + runs;
#else
            {
                // A static build holds the code classes whose ROM was passed to tools/build_host.py; this one was left out.
                const VersionInfo* source = nullptr;
                for (const CodeClass& c : code_classes) {
                    if (std::string_view(c.name) == id.match->code_class) source = versions::find(c.source);
                }
                const std::string from = source ? source->display_name : std::string("that release");
                r.text = file + " is " + id.match->display_name + ", a known release of the game, but this copy of the app was built without its code "
                         "(code class " + id.match->code_class + ", generated from the " + from + " ROM). To include it, build again and pass that ROM "
                         "as well: tools/build_host.py --rom <ROM> --rom <ROM> (Windows: tools\\build_windows.ps1 -Rom \"<ROM>\",\"<ROM>\"); "
                         "see the README, \"Supported ROMs\"." + runs;
            }
#endif
            return r;
    }
    return r;
}
