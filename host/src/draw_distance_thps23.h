// The THPS2/THPS3 engine's draw distance (src/draw_distance_thps23.cpp): the two games share the renderer, the buffers and the course items, and
// differ in a few offsets and sizes (this profile), in THPS3's bigger vertex pools and in THPS3's deferred-item pass (the callbacks).
// Each game's game/draw_distance.cpp defines profile() and, for THPS3, what only its symbol table has.
#pragma once
#include <cstdint>
#include <vector>

#include "draw_distance.h"
#include "recomp.h"

namespace thps::dd23 {

struct Profile {
    // The renderer (gRenderer): the mesh queue and the depth-sorted list of translucent meshes (entry counts, and where each count is kept as
    // an offset from the renderer), and the flag that says a pass is open (the batcher is filling the lists).
    uint32_t queue_count, queue_entries;
    uint32_t sort_count, sort_entries;
    uint32_t batch_on;
    // THPS3 only: the vertex pools are enlarged like the matrix pools (4096 vertices per pool), and the digest and the logs name them.
    bool big_vertex_pools;
    // THPS3 only (null for THPS2): called in an extra run's pass, after its pass end and begin, with the renderer and the first draw call's
    // a0; draws the items the run put aside (World_DrawDeferredItems) in a pass of their own. pass_log is non-null when the decision digest
    // is on: the callback pushes its marker for the flush there.
    void (*after_run)(uint8_t* rdram, recomp_context* ctx, uint64_t a0, uint32_t renderer, thps::dd::HookTimer& timer, std::vector<uint32_t>* pass_log);
    // THPS3 only (null for THPS2): adds the game's own buffer measurements to a gfx task's BufferUse (the deferred-item list's use).
    void (*measure)(thps::dd::BufferUse& use);
};
const Profile& profile();

}  // namespace thps::dd23
