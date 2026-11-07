// Synthetic trigger graphs. No ROM or generated game code is used.
#include "trigger_zones.h"
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
void check(bool condition) {
    if (!condition) { std::cerr << "trigger zones: graph check failed\n"; std::exit(1); }
}
std::vector<uint8_t> ram(8 * 1024 * 1024);
constexpr uint32_t table = 0x80000100, root = 0x80000200, hide = 0x80000300, show = 0x80000400;
void put16(uint32_t p, uint16_t value) { std::memcpy(ram.data() + ((p & 0x7FFFFE) ^ 2), &value, 2); }
void put32(uint32_t p, uint32_t value) { std::memcpy(ram.data() + (p & 0x7FFFFC), &value, 4); }
void box(uint32_t node, bool visible) {
    put16(node, 6); put16(node + 2, 0); put32(node + 4, 0);
    const uint32_t p = node + 8;
    put16(p, 0x8D); put16(p + 2, visible); put16(p + 4, 1);
    // Arbitrary coordinates, shared by hide and show.
    for (unsigned i = 0; i < 6; ++i) put32(p + 8 + 4 * i, i + 10);
    put16(p + 32, 0xFF); put16(p + 34, 0xFFFF);
}
void fixture() {
    std::fill(ram.begin(), ram.end(), 0);
    put32(table, root); put32(table + 4, hide); put32(table + 8, show);
    put16(root, 6); put16(root + 2, 2); put16(root + 4, 1); put16(root + 6, 2);
    put32(root + 8, 123); put16(root + 12, 3); put16(root + 14, 0xFFFF);
    box(hide, false); box(show, true);
}
uint32_t skip(uint32_t p) {
    switch (thps::zones::hword(ram.data(), p)) {
        case 3: return p + 2;
        case 0x8D: return p + 34;
        default: return 0;
    }
}
auto classify(bool nonzero = false) { return thps::zones::classify(ram.data(), table, 3, skip, nonzero); }
}
int main() {
    fixture();
    auto result = classify();
    check(result.commands == 2 && result.unparsed == 0 && result.boxes.size() == 1 && result.boxes.begin()->second);

    // A show not reachable from a touch leaves course state with the game.
    put16(root + 2, 1); // only the hide node remains linked
    put32(root + 8, 123); // with one link, checksum is still at +8
    put16(root + 12, 3); put16(root + 14, 0xFFFF);
    result = classify();
    check(!result.boxes.begin()->second);

    // Baddy nodes are not touch roots, even if their links name both box nodes.
    fixture(); put16(root, 1); put16(root + 6, 2); put16(root + 8, 1); put16(root + 10, 2);
    result = classify();
    check(result.unparsed == 0 && !result.boxes.begin()->second);

    // A partial command list must not permit any culling override.
    fixture(); put16(root + 14, 0x7777);
    result = classify();
    check(result.unparsed == 1 && !result.boxes.begin()->second);

    // THPS1 treats all nonzero show flags as show; THPS2/3 use only 1 or 3.
    fixture(); put16(show + 10, 2);
    check(!classify(false).boxes.begin()->second);
    check(classify(true).boxes.begin()->second);
    // A restart name that reaches its scan cap must invalidate the classification.
    // A terminator at the guessed command start used to conceal the incomplete name.
    fixture();
    constexpr uint32_t restart = 0x80000500, name = restart + 22;
    put32(table + 12, restart); put16(restart, 8); put16(restart + 2, 0);
    for (uint32_t i = 0; i < 256; i++) ram[((name + i) & 0x7FFFFF) ^ 3] = 'x';
    put16(name + 258, 0xFFFF);
    result = thps::zones::classify(ram.data(), table, 4, skip);
    check(result.commands == 2 && result.unparsed == 1 && !result.boxes.begin()->second);

    // Position nodes (501) and the table end (255) carry no commands and keep the result.
    fixture();
    put32(table + 12, 0x80000500); put16(0x80000500, 501);
    put32(table + 16, 0x80000600); put16(0x80000600, 255);
    result = thps::zones::classify(ram.data(), table, 5, skip);
    check(result.unparsed == 0 && result.boxes.begin()->second);
    // A node type the games don't define fails closed instead of being skipped.
    put16(0x80000500, 77);
    result = thps::zones::classify(ram.data(), table, 5, skip);
    check(result.unparsed == 1 && !result.boxes.begin()->second);

    // A box list without its terminator must not read boxes past the command's end.
    fixture(); put16(show + 8 + 32, 0x1234);
    result = classify();
    check(result.unparsed == 1 && result.boxes.size() == 1 && !result.boxes.begin()->second);

    // A command list that runs to the end of RDRAM stops there and is incomplete.
    fixture();
    constexpr uint32_t tail = 0x807FFFF8;
    put32(table + 12, tail); put16(tail, 4);
    for (uint32_t p = tail + 2; p < 0x80800000; p += 2) put16(p, 3);
    result = thps::zones::classify(ram.data(), table, 4, skip);
    check(result.unparsed == 1 && !result.boxes.begin()->second);

    // A node table that runs past RDRAM is not read at all.
    fixture();
    result = thps::zones::classify(ram.data(), 0x807FFFF8, 4, skip);
    check(result.unparsed == 4 && result.boxes.empty());

    // A node whose link count would be read past RDRAM is incomplete.
    fixture();
    constexpr uint32_t last = 0x807FFFFE;
    put32(table + 12, last); put16(last, 2);
    result = thps::zones::classify(ram.data(), table, 4, skip);
    check(result.unparsed == 1 && !result.boxes.begin()->second);

    // Rooms: a second touched CP (checksum 456) also pulses both box nodes. Both trigger objects at the box's edge: a room (2), which
    // is not culling; one of the two far away: one of two at the edge is under two thirds, so culling. No positions: never a room.
    fixture();
    constexpr uint32_t root2 = 0x80000500;
    put32(table + 12, root2);
    put16(root2, 6); put16(root2 + 2, 2); put16(root2 + 4, 1); put16(root2 + 6, 2);
    put32(root2 + 8, 456); put16(root2 + 12, 3); put16(root2 + 14, 0xFFFF);
    float far_x = 12.0f;
    auto where = [&](uint32_t cs, float* c, float& r) {
        if (cs != 123 && cs != 456) return false;
        c[0] = cs == 456 ? far_x : 12.0f, c[1] = 13.0f, c[2] = 14.0f, r = 1.0f;
        return true;
    };
    result = thps::zones::classify(ram.data(), table, 4, skip, false, where);
    check(result.unparsed == 0 && result.boxes.begin()->second == 2 && result.edge.begin()->second == (std::array<uint16_t, 2>{2, 2}));
    far_x = 5000.0f;
    result = thps::zones::classify(ram.data(), table, 4, skip, false, where);
    check(result.boxes.begin()->second == 1 && result.edge.begin()->second == (std::array<uint16_t, 2>{2, 1}));
    result = thps::zones::classify(ram.data(), table, 4, skip);
    check(result.boxes.begin()->second == 1 && result.edge.empty());

    std::cout << "trigger zones: fifteen synthetic graph checks pass\n";
}
