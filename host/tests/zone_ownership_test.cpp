// Zone ownership (src/zone_ownership.h), driven by synthetic event sequences: box calls, names, course starts, RETRY.
// A synthetic RDRAM holds an item list; no ROM or generated game code is used.
#define THPS_ZONES_CORE_ONLY
#include "zone_ownership.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
int checks = 0;
void check(bool condition, const char* what) {
    checks++;
    if (!condition) { std::cerr << "zone ownership: " << what << "\n"; std::exit(1); }
}
std::vector<uint8_t> ram(8 * 1024 * 1024);
constexpr uint32_t kArr = 0x80001000, kItems = 0x80002000, kFlags = 0x80008000, kN = 4;
void put32(uint32_t p, uint32_t v) { std::memcpy(ram.data() + (p & 0x7FFFFC), &v, 4); }
void putf(uint32_t p, float f) { uint32_t v; std::memcpy(&v, &f, 4); put32(p, v); }
uint32_t item(uint32_t i) { return kItems + 0x80 * i; }
uint16_t& flags(uint32_t i) { return *thps::zones::flag_ptr(ram.data(), kArr, i); }
// Items 0..3 in a row along x (centres 0, 100, 200, 300), each with its own flag halfword.
void course(uint32_t arr = kArr) {
    std::fill(ram.begin(), ram.end(), 0);
    for (uint32_t i = 0; i < kN; i++) {
        put32(arr + 4 * i, item(i));
        put32(item(i) + 4, kFlags + 4 * i);
        putf(item(i) + 0x24, 100.0f * float(i));
    }
}
// A box around items I..J (inclusive) along x; inside = 1.
thps::zones::BoxKey box(uint32_t i, uint32_t j) { return {int32_t(100 * i) - 10, -10, -10, int32_t(100 * j) + 10, 10, 10, 1}; }
// One box call: the "game function" sets or clears BITS on items I..J.
void box_call(thps::zones::Ownership& o, uint32_t i, uint32_t j, uint16_t bits, bool hide, bool culling) {
    o.box_before(ram.data());
    for (uint32_t k = i; k <= j; k++) flags(k) = hide ? uint16_t(flags(k) | bits) : uint16_t(flags(k) & ~bits);
    o.box_after(ram.data(), box(i, j), culling);
}
// What "full" clears for item I now.
uint16_t unhide(thps::zones::Ownership& o, uint32_t i) { return o.unhide_bits(i, flags(i), false); }
}  // namespace

int main() {
    using thps::zones::Ownership;
    {   // A culling box hides items 1..2 for player 1: "full" clears exactly that bit; a show forgets it.
        course();
        Ownership o;
        o.sync(ram.data(), kArr, kN, 1);
        box_call(o, 1, 2, 0x01, true, true);
        check(unhide(o, 1) == 0x01 && unhide(o, 2) == 0x01 && unhide(o, 0) == 0, "culling hide is owned");
        box_call(o, 1, 1, 0x01, false, true);
        check(unhide(o, 1) == 0 && unhide(o, 2) == 0x01, "culling show forgets");
    }
    {   // Both players' bits are owned separately.
        course();
        Ownership o;
        o.sync(ram.data(), kArr, kN, 1);
        box_call(o, 1, 1, 0x41, true, true);
        check(unhide(o, 1) == 0x41, "both players");
        box_call(o, 1, 1, 0x40, false, true);
        check(unhide(o, 1) == 0x01, "player 2 shown");
    }
    {   // An item an event names is never unhidden, even when the culling hid it too.
        course();
        Ownership o;
        o.sync(ram.data(), kArr, kN, 1);
        box_call(o, 2, 2, 0x01, true, true);
        o.name(item(2));
        check(o.mark_event(ram.data(), item(2)) == 2, "named item found");
        check(unhide(o, 2) == 0 && o.z.bits[2] == 0, "event-owned");
    }
    {   // A state box owns what it touches for the course; a later culling call does not reclaim it.
        course();
        Ownership o;
        o.sync(ram.data(), kArr, kN, 1);
        box_call(o, 1, 1, 0x01, true, false);
        check(o.z.state[1] == 1 && unhide(o, 1) == 0, "state box owns");
        box_call(o, 1, 1, 0x01, false, true);
        box_call(o, 1, 1, 0x01, true, true);
        check(unhide(o, 1) == 0, "culling does not reclaim a state item");
    }
    {   // Membership in a state box of the course's table marks the item before any call.
        course();
        Ownership o;
        o.sync(ram.data(), kArr, kN, 1);
        std::map<thps::zones::BoxKey, uint8_t> cls{{box(0, 0), 0}, {box(3, 3), 1}};
        const auto m = o.mark_state_items(ram.data(), kArr, kN, cls);
        check(m.state == 1 && m.culling == 1 && o.z.state[0] == 1 && o.z.state[3] == 0, "table membership");
    }
    {   // Another writer changed the bits after the culling did: the record is stale and dropped.
        course();
        Ownership o;
        o.sync(ram.data(), kArr, kN, 1);
        box_call(o, 1, 1, 0x01, true, true);
        flags(1) = uint16_t(flags(1) | 0x40);   // an event hid it for player 2 as well
        check(unhide(o, 1) == 0 && o.z.bits[1] == 0, "stale record dropped");
    }
    {   // Original -> Full: box calls before any draw are kept, so the first draw at Full already knows them.
        course();
        Ownership o;
        o.sync(ram.data(), kArr, kN, 1);
        box_call(o, 0, 3, 0x01, true, true);
        box_call(o, 0, 0, 0x01, false, true);
        o.draw_begins();
        check(unhide(o, 0) == 0 && unhide(o, 3) == 0x01, "history before the first draw");
    }
    {   // A course change (same list address, new course instance): ownership starts again; only the culling done since the last draw
        // (the new instance's, while it loaded) and the names since then carry over.
        course();
        Ownership o;
        o.sync(ram.data(), kArr, kN, 1);
        box_call(o, 1, 1, 0x01, true, true);   // the old course's culling
        o.name(item(2));
        o.mark_event(ram.data(), item(2));     // the old course's event
        o.draw_begins();                       // a draw: the since-draw lists start again
        box_call(o, 3, 3, 0x01, true, true);   // the new instance's set-up culling, before its first draw
        o.name(item(0));                       // and its set-up script naming item 0
        const auto s = o.sync(ram.data(), kArr, kN, 2);
        check(s.started && s.zoned == 1 && s.named == 1, "new instance");
        check(o.z.bits[1] == 0 && o.z.event[2] == 0, "old ownership not carried over");
        check(o.z.bits[3] == 0x01 && o.z.event[0] == 1, "load-time culling and names carried over");
    }
    {   // RETRY: the same list again with no draw in between keeps nothing of the old record but the since-draw lists.
        course();
        Ownership o;
        o.sync(ram.data(), kArr, kN, 1);
        o.draw_begins();
        o.name(item(1));
        o.mark_event(ram.data(), item(1));
        o.sync(ram.data(), kArr, kN, 2);
        check(o.z.event[1] == 1, "a name since the last draw survives RETRY");
        o.draw_begins();
        o.sync(ram.data(), kArr, kN, 3);
        check(o.z.event[1] == 0, "a name before the last draw does not");
    }
    {   // A new list (another address) keeps no culling at all, only names since the draw.
        course();
        Ownership o;
        o.sync(ram.data(), kArr, kN, 1);
        o.draw_begins();
        box_call(o, 2, 2, 0x01, true, true);
        o.name(item(3));
        constexpr uint32_t kArr2 = 0x80003000;
        for (uint32_t i = 0; i < kN; i++) put32(kArr2 + 4 * i, item(i));
        o.sync(ram.data(), kArr2, kN, 1);
        check(o.z.bits[2] == 0 && o.z.event[3] == 1, "new list");
    }
    {   // `dd showall all` (tests) clears every hidden bit, owned or not.
        course();
        Ownership o;
        o.sync(ram.data(), kArr, kN, 1);
        flags(0) = 0x41;
        check(o.unhide_bits(0, flags(0), true) == 0x41 && unhide(o, 0) == 0, "showall all");
    }
    std::cout << "zone ownership: " << checks << " synthetic checks pass\n";
}
