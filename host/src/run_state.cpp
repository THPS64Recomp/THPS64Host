// "Is the skater under the player's control right now?", for the games that read N64 B as flip while skating (THPS2, THPS3;
// game/game.toml `b_is_flip_in_game`). input.cpp uses it to decide whether the east face button's N64 B may be dropped.
//
// The first version asked "is a 3D view being drawn" (widescreen.cpp, in_gameplay_3d). That is also true on every menu that
// draws a 3D scene or skater: Create-a-Skater, the pause menu, the end-of-run and name-entry screens over the level. Their
// "B = back" never arrived. The game's own state answers it: sym().gRunActive is a word that is 1 only while a level run is
// being played (unpaused) and 0 in the front end, on the goals sheet, in the pause menu and on every screen shown over the
// level (the private development notes, "B for back"). Reads RDRAM the way --control peek does: the word is stored as the game wrote it,
// the game thread writes it, this one only reads it.
#include <cstdint>

#include "host.h"
#include "versions.h"

bool thps::skater_in_control() {
    uint8_t* ram = thps::rdram_base.load();
    const uint32_t addr = thps::sym().gRunActive;
    if (!ram || addr < 0x80000000u || addr >= 0x80800000u || (addr & 3)) return false;
    return *reinterpret_cast<const volatile uint32_t*>(ram + (addr - 0x80000000u)) != 0;
}
