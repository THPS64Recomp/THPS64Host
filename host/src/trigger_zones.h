// Our runtime trigger-box classifier. Reads the loaded trigger graph; contains no game data.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>
#include "guest_mem.h"

namespace thps::zones {
using BoxKey = std::array<int32_t, 7>;
struct Classification {
    std::map<BoxKey, uint8_t> boxes; // 1: culling, 0: course state, 2: room (left to the game, as state)
    std::map<BoxKey, std::array<uint16_t, 2>> edge;   // culling and room boxes: touched hide triggers found, and how many touch the box
    uint32_t commands = 0, unparsed = 0;
};
// Room zones. Most culling boxes are hidden by triggers far from them: the course ahead or behind, out of sight at the original draw
// distance, which "full" draws. A room is hidden by triggers at its own edge, as the skater steps out of its doors; seen from outside
// the game draws something else in its place (THPS1 School's gym: a stand-in in the doorway), so drawing the room as well breaks the
// picture, and the walls hide it anyway. A culling box is a room when at least two thirds of the touched command points whose pulses
// reach a command hiding it have a trigger object within kRoomEdge units of the box. At least two: a room is hidden from each of its
// doors, while one trigger at a box's edge is also what a corridor stretch looks like as the skater leaves it (many THPS2 boxes,
// Hangar's among them). Every course of the three games, at load: THPS1 School's gym 7 of 9 and 12 of 13, Streets one 6-item box 4
// of 4, THPS2 one box each in School II and Skatestreet 2 of 2; no other box above half. WHERE(checksum, centre, radius) gives a
// touched CP's trigger object, in box units; without it no box is a room.
constexpr float kRoomEdge = 300.0f;
struct NoWhere {
    bool operator()(uint32_t, float*, float&) const { return false; }
};
inline float box_distance(const BoxKey& k, const float* c) {
    float d = 0.0f;
    for (int a = 0; a < 3; a++) {
        const float lo = float(k[a]) - c[a], hi = c[a] - float(k[3 + a]);
        const float o = std::max(0.0f, std::max(lo, hi));
        d += o * o;
    }
    return std::sqrt(d);
}
using mem::in_ram;
using mem::word;
using mem::hword;
// Node types the games define: 255 ends the table, and a pulse only reads the position of 500/501
// (Trig_SendPulseToNode). Any other type is data we don't understand, so it fails closed.
inline bool known_type(uint16_t t) {
    return (t >= 1 && t <= 15) || t == 255 || t == 500 || t == 501 || t == 1000 || t == 1001;
}
// A touch-triggered hide and show, reachable without a baddy, means culling. All other boxes are state.
// The adapter supplies the game's own skip-command function and its show-flag convention.
template<class Skip, class Where = NoWhere>
Classification classify(const uint8_t* rdram, uint32_t tab, uint32_t count, Skip skip, bool nonzero_show = false, Where where = {}) {
    Classification result;
    if (!in_ram(tab) || count == 0 || count > 8192) return result;
    if (!in_ram(tab + 4 * count - 1)) { result.unparsed = count; return result; }   // a table running past RDRAM: nothing is trusted
    struct Box { std::array<int32_t, 7> key; bool show; };
    struct Node { uint32_t at = 0; uint16_t type = 0; uint32_t cs = 0; std::vector<uint16_t> links; bool pulses = false; std::vector<Box> boxes; };
    std::vector<Node> ns(count);
    for (uint32_t i = 0; i < count; i++) {
        ns[i].at = word(rdram, tab + 4 * i);
        if (!in_ram(ns[i].at) || !in_ram(ns[i].at + 1)) { result.unparsed = count; return result; }
    }
    uint32_t& unparsed = result.unparsed;
    for (uint32_t i = 0; i < count; i++) {
        Node& nd = ns[i];
        const uint32_t o = nd.at;
        nd.type = hword(rdram, o);
        if (!known_type(nd.type)) { unparsed++; continue; }
        // where the link count is (in halfwords from the node), by type (Trig_GetLinksPointer)
        int lt = 0;
        switch (nd.type) {
            case 1: lt = 3; break;
            case 5: lt = 2; break;
            case 2: case 3: case 6: case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 1000: case 1001: lt = 1; break;
            default: break;
        }
        if (lt) {
            const uint32_t lp = o + 2 * lt;
            if (!in_ram(lp + 1)) { unparsed++; continue; }   // the link count itself past RDRAM
            const uint32_t nl = hword(rdram, lp);
            if (nl > 1024 || !in_ram(lp + 2 * nl + 1)) { unparsed++; continue; }
            for (uint32_t k = 0; k < nl; k++) nd.links.push_back(hword(rdram, lp + 2 + 2 * k));
        }
        uint32_t cl = 0;
        if (nd.type == 4 || nd.type == 15) {
            cl = o + 2;
        } else if (nd.type == 6) {
            uint32_t p = o + 2;
            p += 2 * hword(rdram, p) + 2;
            if (p & 2) p += 2;
            if (!in_ram(p + 3)) { unparsed++; continue; }
            nd.cs = word(rdram, p);
            cl = p + 4;
        } else if (nd.type == 8) {
            uint32_t p = (o + 2 * hword(rdram, o + 2) + 7) & ~3u;
            p += 12 + 6;   // position (3 x s32), angles (3 x s16), then the name
            if (!in_ram(p + 256)) { unparsed++; continue; }
            uint32_t e = p;
            while (e < p + 256 && rdram[(e & 0x3FFFFFF) ^ 3]) e++;
            // A capped name is incomplete parsing, not the start of a command list.
            if (e == p + 256) { unparsed++; continue; }
            e++;
            cl = e + (e & 1);
        }
        if (!cl) continue;
        bool ended = false, bad = false;
        // Every read stays inside RDRAM and inside the command the game's skip sized.
        for (uint32_t p = cl, k = 0; k < 5000 && in_ram(p + 1); k++) {
            const uint16_t op = hword(rdram, p);
            if (op == 0xFFFF) { ended = true; break; }
            const uint32_t nx2 = skip(p);
            if (nx2 <= p || nx2 > p + 0x4000 || !in_ram(nx2 - 1)) break;
            if (op == 0x03) nd.pulses = true;
            if (op == 0x8D) {
                const uint16_t show = hword(rdram, p + 2), ins = hword(rdram, p + 4);
                for (uint32_t r = p + 6; r < nx2 && hword(rdram, r) != 0xFF;) {
                    r = (r + 3) & ~3u;
                    if (r + 0x18 > nx2) { bad = true; break; }
                    Box b;
                    for (int a = 0; a < 6; a++) b.key[a] = int32_t(word(rdram, r + 4 * a));
                    b.key[6] = ins;
                    b.show = nonzero_show ? show != 0 : show == 1 || show == 3;
                    nd.boxes.push_back(b);
                    r += 0x18;
                }
                if (bad) break;
            }
            p = nx2;
        }
        unparsed += !ended;
    }
    // Pulse edges, then what the touched command points reach without passing a baddy.
    std::vector<std::vector<uint32_t>> out(count);
    for (uint32_t i = 0; i < count; i++) {
        const Node& nd = ns[i];
        if (nd.pulses || nd.type == 5)
            for (uint16_t l : nd.links) out[i].push_back(l);
    }
    std::vector<uint8_t> reach(count, 0);
    std::vector<uint32_t> st;
    for (uint32_t i = 0; i < count; i++)
        if (ns[i].type == 6 && ns[i].cs) reach[i] = 1, st.push_back(i);
    while (!st.empty()) {
        const uint32_t i = st.back();
        st.pop_back();
        for (uint32_t j : out[i])
            if (j < count && !reach[j] && ns[j].type != 1 && ns[j].type != 7) reach[j] = 1, st.push_back(j);
    }
    std::map<std::array<int32_t, 7>, uint8_t> seen;   // bit 1: hidden from a touch, bit 2: shown from a touch, bit 4: named at all
    uint32_t cmds = 0;
    for (uint32_t i = 0; i < count; i++)
        for (const Box& b : ns[i].boxes) {
            uint8_t& m = seen[b.key];
            m |= 4;
            if (reach[i]) m |= b.show ? 2 : 1;
            cmds++;
        }
    result.commands = cmds;
    for (const auto& [key, mask] : seen)
        result.boxes[key] = result.unparsed == 0 && (mask & 3) == 3;
    // Rooms: per touched CP, the culling boxes its pulses hide (the same walk, from that CP alone), and how near its trigger object is.
    std::map<BoxKey, std::vector<uint32_t>> hidden_by;   // culling box -> the nodes with a command hiding it
    for (uint32_t i = 0; i < count; i++)
        for (const Box& b : ns[i].boxes)
            if (!b.show && result.boxes[b.key] == 1) hidden_by[b.key].push_back(i);
    std::vector<uint32_t> mark(count, ~0u);
    for (uint32_t r = 0; r < count && !hidden_by.empty(); r++) {
        if (ns[r].type != 6 || !ns[r].cs) continue;
        float c[3], rad = 0.0f;
        if (!where(ns[r].cs, c, rad)) continue;   // no trigger object found: the CP doesn't count either way
        mark[r] = r, st.push_back(r);
        while (!st.empty()) {
            const uint32_t i = st.back();
            st.pop_back();
            for (uint32_t j : out[i])
                if (j < count && mark[j] != r && ns[j].type != 1 && ns[j].type != 7) mark[j] = r, st.push_back(j);
        }
        for (const auto& [key, nodes] : hidden_by)
            if (std::any_of(nodes.begin(), nodes.end(), [&](uint32_t n) { return mark[n] == r; })) {
                auto& e = result.edge[key];
                e[0]++, e[1] += box_distance(key, c) - rad <= kRoomEdge;
            }
    }
    for (const auto& [key, e] : result.edge)
        if (e[0] >= 2 && 3 * e[1] >= 2 * e[0]) result.boxes[key] = 2;
    return result;
}
} // namespace thps::zones
