// Zone ownership hooks (zone_ownership.h): the box command's World_SetVisibilityInBox and World_FindItem run the game's function, then
// tell the record what changed. Built with a game's draw distance (game/draw_distance.cpp supplies profile()). Game thread only.
//
// Culling boxes and state boxes (v0.3, docs: v03-zone-classification-research.md in the private notes). The box calls come from one
// trigger command (0x8D: u16 show, u16 inside, then boxes of six s32 until a u16 0x00FF), and the course uses them for two things:
// culling (areas hidden and shown again as the skater rides through doorways and corridors) and course state (THPS2's Hangar crashed
// ceiling, hidden until its event; its floor's light overlays, switched by a script). Which one a box is follows from what runs the
// commands that name it (src/trigger_zones.h: the course's trigger file, walked with the game's own Trig_SkipCommand). A box the table
// does not know counts as state, so the default is the game's own behaviour. The table is built once per course instance (the node
// table, its count, the course and its item array/count), at the first box call or course draw.
#include "zone_ownership.h"

#include <cmath>
#include <cstdio>

#include "draw_distance.h"
#include "host.h"
#include "thps_hooks.h"

namespace thps::zones {
namespace {
using mem::word;
Ownership g_own;
struct TrgTable {
    uint32_t tab = 0, count = 0, arr = 0, items = 0;
    uint64_t gen = ~0ull;
    bool built = false;
    std::map<BoxKey, uint8_t> cls;   // box key (min x, y, z, max x, y, z, inside) -> 1 culling, 0 state, 2 room
    uint32_t unknown = 0;            // box calls with a key not in the table (this course)
} g_trg;
// The trigger interpreter's skip (a leaf: reads the command, returns the next one, 0 at the list's end), run on a copy of the context.
uint32_t trg_skip(uint8_t* rdram, const recomp_context* ctx, uint32_t p) {
    recomp_context c = *ctx;
    c.r4 = uint64_t(int64_t(int32_t(p)));
    thps::call_guest(thps::sym().Trig_SkipCommand, rdram, &c);
    return uint32_t(c.r2);
}
}  // namespace

Ownership& own() { return g_own; }
const std::map<BoxKey, uint8_t>& classes() { return g_trg.cls; }


void sync(uint8_t* rdram, uint32_t list) {
    const uint32_t n = word(rdram, list + 16), arr = word(rdram, list + 12);
    const uint64_t gen = thps::dd::course_generation();
    const Ownership::Started s = g_own.sync(rdram, arr, n, gen);
    if (s.started && thps::dd::digest::on()) {   // tests: the new record, what it kept and what it took from the names
        const Record& z = g_own.z;
        thps::dd::digest::Hash h;
        h.u(arr), h.u(n), h.u(gen), h.u(s.old_arr), h.u(s.old_n), h.u(s.zoned), h.u(s.named);
        uint32_t kept = 0, events = 0;
        for (size_t i = 0; i < z.bits.size(); i++) h.u(z.bits[i]), h.u(z.after[i]), h.u(z.event[i]), kept += z.bits[i] != 0, events += z.event[i];
        thps::dd::digest::record('Z', 0, h, "items=" + std::to_string(z.bits.size()) + " kept=" + std::to_string(kept) + " events=" + std::to_string(events));
    }
}

void build(uint8_t* rdram, const recomp_context* ctx) {
    const auto& sy = thps::sym();
    const uint32_t tab = word(rdram, sy.gTrgNodes), count = word(rdram, sy.gTrgNodeCount);
    const uint32_t list = sy.gWorldItems, n = word(rdram, list + 16), arr = word(rdram, list + 12);
    const uint64_t gen = thps::dd::course_generation();
    if (g_trg.built && tab == g_trg.tab && count == g_trg.count && gen == g_trg.gen && arr == g_trg.arr && n == g_trg.items) return;
    g_trg = TrgTable{};
    g_trg.tab = tab, g_trg.count = count, g_trg.gen = gen, g_trg.built = true;
    g_trg.arr = arr, g_trg.items = n;
    if (!in_ram(tab) || count == 0 || count > 8192) return;
    // A touched CP's trigger object: the first course item whose record (item +4) has its checksum at +0x1C, as World_FindItem looks it
    // up; its centre (+0x24) in box units, and its bounding-sphere radius in box units.
    const Profile& pf = profile();
    std::map<uint32_t, uint32_t> by_checksum;
    float scale = 1.0f;
    if (n <= 8192) {
        std::vector<float> ratios;
        uint32_t prev = 0;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t it = word(rdram, arr + 4 * i);
            if (!in_ram(it) || !in_ram(it + 0x44) || !in_ram(word(rdram, it + 4) + 0x1F)) continue;
            by_checksum.emplace(word(rdram, word(rdram, it + 4) + 0x1C), it);
            if (pf.sphere_at && prev && ratios.size() < 256) {   // box units per sphere unit, from the distances between neighbours
                float d24 = 0.0f, ds = 0.0f;
                for (int a = 0; a < 3; a++) {
                    const float x = thps::mem::f32(rdram, it + 0x24 + 4 * a) - thps::mem::f32(rdram, prev + 0x24 + 4 * a);
                    const float y = thps::mem::f32(rdram, it + pf.sphere_at + 4 * a) - thps::mem::f32(rdram, prev + pf.sphere_at + 4 * a);
                    d24 += x * x, ds += y * y;
                }
                if (ds > 1.0f && d24 > 1.0f) ratios.push_back(std::sqrt(d24 / ds));
            }
            prev = it;
        }
        if (!ratios.empty()) std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end()), scale = ratios[ratios.size() / 2];
    }
    auto where = [&](uint32_t cs, float* c, float& r) {
        const auto f = by_checksum.find(cs);
        if (f == by_checksum.end()) return false;
        for (int a = 0; a < 3; a++) c[a] = thps::mem::f32(rdram, f->second + 0x24 + 4 * a);
        r = std::fabs(thps::mem::f32(rdram, f->second + pf.radius_at)) * (pf.sphere_at ? scale : 1.0f);
        return std::isfinite(r) && r < 1e7f;
    };
    auto c = classify(rdram, tab, count, [&](uint32_t p) { return trg_skip(rdram, ctx, p); }, pf.nonzero_show, where);
    g_trg.cls = std::move(c.boxes);
    uint32_t nc = 0, nst = 0, nroom = 0;
    for (const auto& [k, v] : g_trg.cls) (v == 1 ? nc : v == 2 ? nroom : nst)++;
    // Items whose centre the boxes of each kind contain (the evidence line; compare with the research table).
    const Ownership::Members m = g_own.mark_state_items(rdram, arr, n, g_trg.cls);
    std::printf("[thps] draw distance: zones: trigger file of %u nodes, %u box commands, %u lists unparsed: culling boxes %u (items %u), rooms %u, "
                "state boxes %u (items in them and in rooms %u), items in both %u, course items %u\n", count, c.commands, c.unparsed, nc, m.culling, nroom, nst,
                m.state, m.both, n <= 8192 ? n : 0);
    for (const auto& [k, e] : c.edge)   // every culling box's verdict: of the touched triggers that hide it, how many sit at its edge
        if (thps::verbose() || g_trg.cls[k] == 2)
            std::printf("[thps] draw distance: zones:   %s (%d, %d, %d)..(%d, %d, %d): %u of %u hiding triggers at its edge\n", g_trg.cls[k] == 2 ? "room" : "culling",
                        k[0], k[1], k[2], k[3], k[4], k[5], e[1], e[0]);
    std::fflush(stdout);
    if (thps::dd::digest::on()) {   // tests: the course's box table and the state items it marked
        thps::dd::digest::Hash h;
        h.u(tab), h.u(count), h.u(c.commands), h.u(c.unparsed);
        for (const auto& [k, v] : g_trg.cls) {
            for (int32_t x : k) h.u(uint32_t(x));
            h.u(v);
        }
        for (uint8_t s : g_own.z.state) h.u(s);
        char d[160];
        std::snprintf(d, sizeof d, "nodes=%u cmds=%u unparsed=%u culling=%u/%u state=%u/%u both=%u", count, c.commands, c.unparsed, nc, m.culling, nst,
                      m.state, m.both);
        thps::dd::digest::record('T', 0, h, d);
    }
}

std::string dd_item(uint8_t* rdram, uint32_t index) {
    const uint32_t list = thps::sym().gWorldItems;
    const uint32_t n = word(rdram, list + 16), arr = word(rdram, list + 12);
    if (n == 0 || n > 8192 || index >= n) return "none";
    sync(rdram, list);
    const Record& z = g_own.z;
    const uint16_t f = *flag_ptr(rdram, arr, index);
    const uint8_t zone = index < z.bits.size() ? z.bits[index] : 0, event = index < z.event.size() ? z.event[index] : 0;
    const uint16_t unhidden = thps::dd::show_all() ? uint16_t(f & (thps::dd::unhide_everything() ? 0x41 : zone)) : 0;
    uint32_t named = 0;
    for (uint8_t e : z.event) named += e;
    char buf[200];
    std::snprintf(buf, sizeof buf, "flags 0x%04X zone 0x%02X event %u drawn %d named %u gen %llu", f, zone, event, ((f & 0x41) & ~unhidden) == 0,
                  named, (unsigned long long)z.gen);
    return buf;
}

std::string dd_own(uint8_t* rdram, uint32_t index) {
    const uint32_t list = thps::sym().gWorldItems;
    const uint32_t n = word(rdram, list + 16);
    if (n == 0 || n > 8192 || index >= n) return "none";
    sync(rdram, list);
    if (index >= g_own.z.event.size()) return "none";
    g_own.z.event[index] = 1;
    return dd_item(rdram, index);
}
}  // namespace thps::zones

// The box hook: runs the game's function and notes, per item, the hidden bits that call changed, for culling boxes only. A state box (or
// one the table does not know) takes permanent ownership for this course. Culling never reclaims a state item, in either call order.
extern "C" int thps_world_set_visibility_in_box(uint8_t* rdram, recomp_context* ctx) {
    using namespace thps::zones;
    thread_local bool inside = false;
    if (inside) return 0;
    sync(rdram, thps::sym().gWorldItems);
    build(rdram, ctx);
    // a0 / a1: the box's min and max (floats holding the command's integers), a3: the test (1 = centre inside, 0 = outside)
    BoxKey key{};
    for (int a = 0; a < 3; a++) {
        key[a] = int32_t(std::lround(thps::mem::f32(rdram, uint32_t(ctx->r4) + 4 * a)));
        key[3 + a] = int32_t(std::lround(thps::mem::f32(rdram, uint32_t(ctx->r5) + 4 * a)));
    }
    key[6] = int32_t(uint32_t(ctx->r7) & 0xFFFF);
    const auto found = g_trg.cls.find(key);
    const bool culling = found != g_trg.cls.end() && found->second == 1;
    if (found == g_trg.cls.end() && g_trg.unknown++ < 5) {
        std::printf("[thps] draw distance: zones: a box call the trigger table does not know (left to the game): (%d, %d, %d)..(%d, %d, %d) inside %d\n",
                    key[0], key[1], key[2], key[3], key[4], key[5], key[6]);
        std::fflush(stdout);
    }
    Ownership& o = g_own;
    o.box_before(rdram);
    inside = true;
    thps::call_guest(thps::sym().World_SetVisibilityInBox, rdram, ctx);
    inside = false;
    o.box_after(rdram, key, culling);
    if (thps::dd::digest::on()) {   // tests: the box, its class and every item's record after it
        thps::dd::digest::Hash h;
        for (int32_t v : key) h.u(uint32_t(v));
        h.u(culling), h.u(found != g_trg.cls.end());
        for (uint32_t i = 0; i < o.z.bits.size(); i++) h.u(*flag_ptr(rdram, o.z.arr, i)), h.u(o.z.bits[i]), h.u(o.z.after[i]), h.u(o.z.state[i]);
        thps::dd::digest::record('B', 0, h, culling ? "culling" : found == g_trg.cls.end() ? "unknown" : found->second == 2 ? "room" : "state");
    }
    return 1;
}

// Event ownership. Whatever looks an item up by name (the scripts' per-item show / hide / kill commands, goal and gap code) owns it from
// then on, for this course: "full" never unhides it, even if the zone culling had also hidden it. Conservative on purpose: a secret
// revealed or hidden by an event stays under the game's own control.
extern "C" int thps_world_find_item(uint8_t* rdram, recomp_context* ctx) {
    using namespace thps::zones;
    thread_local bool inside = false;
    if (inside) return 0;
    const uint32_t list = uint32_t(ctx->r4);
    inside = true;
    thps::call_guest(thps::sym().World_FindItem, rdram, ctx);
    inside = false;
    const uint32_t item = uint32_t(ctx->r2);
    if (item && list == thps::sym().gWorldItems) {
        g_own.name(item);
        sync(rdram, list);
        g_own.mark_event(rdram, item);
    }
    // tests: which item a script named. The index is the item's place in the course list for any searched list (THPS1's phase A record
    // asked for the course list first; the same on every recorded ride, where only course lists were searched with a course item found).
    if (thps::dd::digest::on()) {
        thps::dd::digest::Hash h;
        h.u(list), h.u(item);
        uint32_t index = ~0u;
        for (uint32_t i = 0; item && i < g_own.z.event.size(); i++)
            if (word(rdram, g_own.z.arr + 4 * i) == item) { index = i; break; }
        h.u(index);
        thps::dd::digest::record('F', 0, h, "index=" + std::to_string(int32_t(index)));
    }
    return 1;
}
