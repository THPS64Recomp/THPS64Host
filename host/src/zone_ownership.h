// Zone ownership (v0.3; shared by the three games' draw distance, game/draw_distance.cpp). The trigger scripts hide and show course items
// by box as the skater moves; "full" clears exactly the hidden bits the culling boxes set, and never touches an item an event named or a
// state box owns. The record is per course instance. The core below reads guest memory but runs no guest code, so tests drive it with a
// synthetic RDRAM (tests/zone_ownership_test.cpp); src/zone_ownership.cpp has the hooks that run the game's functions around it.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "guest_mem.h"
#include "trigger_zones.h"

namespace thps::zones {

// What differs between the games (each game/draw_distance.cpp defines profile()).
struct Profile {
    bool nonzero_show;     // the box command's show flag: THPS1 shows on any nonzero value, THPS2/3 on 1 (classify)
    uint32_t radius_at;    // an item's bounding-sphere radius (rooms: a trigger object's reach): THPS1 +0x30, in box units; THPS2/3 +0x40
    uint32_t sphere_at;    // 0: the radius is in box units; else the sphere centre's offset (THPS2/3 +0x34), whose units the radius is in
};
const Profile& profile();

// An item's flag halfword, through the pointer at item +4: the hidden bits are 0x01 (player 1) and 0x40 (player 2).
inline uint16_t* flag_ptr(uint8_t* rdram, uint32_t arr, uint32_t i) {
    return reinterpret_cast<uint16_t*>(rdram + ((word(rdram, word(rdram, arr + 4 * i) + 4) & 0x3FFFFFE) ^ 2));
}
// Box membership by the item's centre at +0x24 (all three games; rendering uses other fields).
inline bool item_in_box(const uint8_t* rdram, uint32_t item, const BoxKey& k) {
    bool in = true;
    for (int a = 0; a < 3; a++) {
        const float c = thps::mem::f32(rdram, item + 0x24 + 4 * a);
        in = in && float(k[a]) <= c && c <= float(k[3 + a]);
    }
    return k[6] ? in : !in;
}

struct Record {
    uint32_t arr = 0, n = 0;
    uint64_t gen = 0;             // the course instance it belongs to
    std::vector<uint8_t> bits;    // per item: hidden bits (of 0x41) the zone culling set and nothing has cleared since
    std::vector<uint8_t> after;   // per item: the item's hidden bits right after the zone culling last changed them
    std::vector<uint8_t> event;   // per item: named by World_FindItem in this course (a script / goal / gap command): never unhidden
    std::vector<uint8_t> state;   // inside any state box, or touched by an unknown box: never owned by culling
};

class Ownership {
public:
    Record z;
    std::vector<uint32_t> named_since_draw;   // items World_FindItem named since the last draw of the course items
    std::vector<uint32_t> zoned_since_draw;   // indices of the items whose hidden bits the zone culling changed since then

    struct Started {
        bool started = false;
        uint32_t old_arr = 0, old_n = 0;
        size_t zoned = 0, named = 0;
    };
    // A new item list (pointer or count) or course instance starts the record again: empty, except the zone culling done since the last
    // draw of the same list (the new run's, while it loaded) and the items an event named since then. Event ownership never carries over.
    Started sync(const uint8_t* rdram, uint32_t arr, uint32_t n, uint64_t gen) {
        Started s;
        if (n == z.n && arr == z.arr && gen == z.gen) return s;
        const size_t m = n <= 8192 ? n : 0;
        Record old = std::move(z);
        z = Record{arr, n, gen, std::vector<uint8_t>(m, 0), std::vector<uint8_t>(m, 0), std::vector<uint8_t>(m, 0), std::vector<uint8_t>(m, 0)};
        if (arr == old.arr && n == old.n)
            for (uint32_t i : zoned_since_draw)
                if (i < m && i < old.bits.size()) z.bits[i] = old.bits[i], z.after[i] = old.after[i];
        for (uint32_t i = 0; i < m && !named_since_draw.empty(); i++)
            if (std::find(named_since_draw.begin(), named_since_draw.end(), word(rdram, arr + 4 * i)) != named_since_draw.end()) z.event[i] = 1;
        s.started = true, s.old_arr = old.arr, s.old_n = old.n, s.zoned = zoned_since_draw.size(), s.named = named_since_draw.size();
        return s;
    }
    void draw_begins() { named_since_draw.clear(), zoned_since_draw.clear(); }

    // The course's box table: items inside a state box are state-owned for the course. Returns the evidence counts.
    struct Members { uint32_t culling = 0, state = 0, both = 0; };
    Members mark_state_items(const uint8_t* rdram, uint32_t arr, uint32_t n, const std::map<BoxKey, uint8_t>& cls) {
        Members c;
        if (n > 8192) return c;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t it = word(rdram, arr + 4 * i);
            if (!in_ram(it)) continue;
            bool cu = false, st = false;
            for (const auto& [k, v] : cls) {   // a room (2) is left to the game like a state box
                if (v == 1 && !cu) cu = item_in_box(rdram, it, k);
                if (v != 1 && !st) st = item_in_box(rdram, it, k);
            }
            c.culling += cu, c.state += st, c.both += cu && st;
            // Membership, not call order, decides ownership. A later culling call must not reclaim a state item.
            if (st && i < z.state.size()) z.state[i] = 1, z.bits[i] = 0;
        }
        return c;
    }

    // One box call: the flags before the game's function, then what it changed. Culling boxes record the bits they set and forget the ones
    // they clear; a state box (or one the table does not know) takes the items it touched or contains for the course.
    std::vector<uint16_t> before;
    void box_before(uint8_t* rdram) {
        const uint32_t n = uint32_t(z.bits.size());
        before.resize(n);
        for (uint32_t i = 0; i < n; i++) before[i] = *flag_ptr(rdram, z.arr, i);
    }
    void box_after(uint8_t* rdram, const BoxKey& key, bool culling) {
        const uint32_t n = uint32_t(z.bits.size());
        for (uint32_t i = 0; i < n; i++) {
            const uint16_t after = *flag_ptr(rdram, z.arr, i), changed = uint16_t((before[i] ^ after) & 0x41);
            if (!culling) {
                if (changed || item_in_box(rdram, word(rdram, z.arr + 4 * i), key)) z.state[i] = 1, z.bits[i] = 0;
                continue;
            }
            if (z.state[i]) { z.bits[i] = 0; continue; }
            if (changed) {
                z.bits[i] = uint8_t((z.bits[i] & ~changed) | (after & changed));
                if (zoned_since_draw.size() < 65536) zoned_since_draw.push_back(i);
                z.after[i] = uint8_t(after & 0x41);
            }
        }
    }

    // World_FindItem found ITEM in the course list: noted for a new record (name), then event-owned in this one (mark_event: its index,
    // ~0u when the record has no such item). The hook syncs the record between the two, so a course start in between still takes it.
    void name(uint32_t item) {
        if (named_since_draw.size() < 8192) named_since_draw.push_back(item);
    }
    uint32_t mark_event(const uint8_t* rdram, uint32_t item) {
        for (uint32_t i = 0; i < z.event.size(); i++)
            if (word(rdram, z.arr + 4 * i) == item) { z.event[i] = 1; return i; }
        return ~0u;
    }

    // What "full" clears of item I's flags F at a draw. A record goes stale as soon as anything else changed the bits since the zone culling
    // did (an event hid or showed the item: the game's own state governs from then on), and an item an event named is never unhidden.
    // everything: `dd showall all` (tests), every hidden bit.
    uint16_t unhide_bits(uint32_t i, uint16_t f, bool everything) {
        if (z.bits[i] && ((f & 0x41) != z.after[i] || z.event[i] || z.state[i])) z.bits[i] = 0;
        return uint16_t(f & (everything ? 0x41 : z.bits[i]));
    }
};

// The game thread's ownership and the course's box table (src/zone_ownership.cpp, built with a game/draw_distance.cpp).
Ownership& own();
const std::map<BoxKey, uint8_t>& classes();
}  // namespace thps::zones

#ifndef THPS_ZONES_CORE_ONLY   // the unit test builds the core alone, without the runtime's headers
#include "recomp.h"
namespace thps::zones {
// The course items' list (count at +16, pointers at +12): the record for its current course instance, and the course's box table, built
// once per instance at the first box call or course draw (the adapters call these where they did before; the order is the game's).
void sync(uint8_t* rdram, uint32_t list);
void build(uint8_t* rdram, const recomp_context* ctx);
// `dd item N` / `dd own N` (tests): an item's flags and ownership; "drawn": whether its hidden bits are all ones "full" may clear, by
// the record as it stands (stale bits and a new `own` take effect at the next draw). `own` makes it event-owned as World_FindItem would.
std::string dd_item(uint8_t* rdram, uint32_t index);
std::string dd_own(uint8_t* rdram, uint32_t index);
}  // namespace thps::zones
#endif
