// Widescreen, game side. RT64 (aspect 16:9 / 21:9 / expand, src/rt64_context.cpp) renders a
// wider frame and widens the game's perspective projection; this file widens what the game
// itself culls against (its view-space frustum planes), so geometry outside the original 4:3
// frustum is still submitted. Only the cull planes: the frustum itself feeds the camera.
//
// View object fields used (layout read from View_SetFrustum and View_ComputeCullPlanes; the offset of the frustum is
// thps::sym().View_Frustum, 0x160 in every known version):
//   +0 l, +4 r, +8 b, +0xC t, +0x10 near, +0x14 far   (floats, glFrustum args, from View_Frustum)
//   +0x188.. side planes (4 x vec3), +0x1B8 near plane, +0x218.. far-plane corners
// The recompiled View_ComputeCullPlanes is called by its (version-specific) address: thps::call_guest.
#include <atomic>
#include <cstdio>
#include <cstring>

#include "recomp.h"
#include "thps_hooks.h"
#include "host.h"
#include "versions.h"

namespace {

float load_f32(uint8_t* rdram, uint32_t addr) {
    int32_t w = MEM_W(0, (gpr)(int32_t)addr);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

void store_f32(uint8_t* rdram, uint32_t addr, float f) {
    int32_t w;
    std::memcpy(&w, &f, 4);
    MEM_W(0, (gpr)(int32_t)addr) = w;
}

std::atomic<uint64_t> g_widened{0};
std::atomic<uint64_t> g_last_3d_frame{0};

std::atomic<uint64_t> g_frustums_widened{0};

}  // namespace

// View_ComputeCullPlanes runs for every 3D view each frame (skating, pause screen over it); front-end
// screens never call it.
bool thps::in_gameplay_3d() {
    uint64_t last = g_last_3d_frame.load();
    return last != 0 && thps::stats.vi_origin_changes.load() <= last + 4;
}

extern "C" int thps_widen_cull_planes(uint8_t* rdram, recomp_context* ctx) {
    g_last_3d_frame = thps::stats.vi_origin_changes.load() + 1;
    thread_local bool inside = false;
    if (inside) return 0;
    const float k = thps::gfx_widen_factor() * thps::options.cull_extra;
    if (k <= 1.0001f) return 0;
    const uint32_t view = uint32_t(ctx->r4);
    const uint32_t fr = view + thps::sym().View_Frustum;
    const float l = load_f32(rdram, fr), r = load_f32(rdram, fr + 4);
    store_f32(rdram, fr, l * k);
    store_f32(rdram, fr + 4, r * k);
    inside = true;
    thps::call_guest(thps::sym().View_ComputeCullPlanes, rdram, ctx);
    inside = false;
    store_f32(rdram, fr, l);
    store_f32(rdram, fr + 4, r);
    if (g_widened.fetch_add(1) == 0) {
        std::printf("[thps] widescreen: cull planes widened x%.3f (view 0x%08X, frustum l %.3f r %.3f b %.3f t %.3f n %.3f f %.1f)\n", k, view, l, r,
                    load_f32(rdram, fr + 8), load_f32(rdram, fr + 0xC), load_f32(rdram, fr + 0x10), load_f32(rdram, fr + 0x14));
        std::fflush(stdout);
    }
    return 1;
}

extern "C" int thps_view_set_frustum(uint8_t* rdram, recomp_context* ctx) {
    thread_local bool inside = false;
    if (inside) return 0;
    static std::atomic<uint64_t> calls{0};
    auto f32 = [](uint64_t r) { float f; uint32_t w = uint32_t(r); std::memcpy(&f, &w, 4); return f; };
    if (thps::verbose() && calls.fetch_add(1) < 40) {
        const uint32_t sp = uint32_t(ctx->r29);
        std::printf("[thps] View_SetFrustum view 0x%08X l %.3f r %.3f b %.3f t %.3f n %.3f f %.1f\n", uint32_t(ctx->r4), f32(ctx->r5),
                    f32(ctx->r6), f32(ctx->r7), load_f32(rdram, sp + 0x10), load_f32(rdram, sp + 0x14), load_f32(rdram, sp + 0x18));
        std::fflush(stdout);
    }
    // Not widened any more (Q17). Widening this frustum (its +0x120 matrix) changed where the
    // game puts the camera: on one paused Warehouse frame at 21:9 the camera sat visibly closer to
    // the skater than at 4:3 with it, and where it sits at 4:3 without it (same native frame). The game's camera
    // code evidently uses this frustum too. Widening only the cull planes (above) gives the same
    // filled side bands without touching the camera. See the private development notes (Q17).
    return 0;
}
