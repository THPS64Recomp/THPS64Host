// The game's side of the draw distance setting, for a game that doesn't supply one (no game/draw_distance.cpp).
// The setting is accepted and saved, and changes nothing: dd_game_far keeps the game's own far value.
#include "draw_distance.h"

bool thps::dd::supported() { return false; }
float thps::dd::dd_game_far(uint8_t*, float f, float) { return f; }
bool thps::dd::dd_game_measure(uint8_t*, uint32_t, uint32_t, BufferUse&) { return false; }
std::string thps::dd::dd_game_item(uint8_t*, uint32_t) { return "none"; }
std::string thps::dd::dd_game_own(uint8_t*, uint32_t) { return "none"; }
std::string thps::dd::dd_game_heap(uint8_t*, bool) { return "none"; }
std::string thps::dd::dd_game_dlprobe(uint8_t*, bool) { return "none"; }
