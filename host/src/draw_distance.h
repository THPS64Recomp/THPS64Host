// Extended draw distance (src/draw_distance.cpp; the game's side in game/draw_distance.cpp). Off by default.
#pragma once
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace thps::dd {

// How full the game's fixed per-frame buffers were for one gfx task (game/draw_distance.cpp fills it).
struct BufferUse {
    uint32_t dl_bytes = 0, dl_cap = 0;     // top-level display list: bytes written / buffer size
    uint32_t mtx_used = 0, mtx_cap = 0;    // matrix pool: matrices handed out / pool size (0 = the game has none we know)
    uint32_t vtx_used = 0, vtx_cap = 0;    // vertex pool (THPS2/3): vertices handed out / pool size (0 = none)
    uint32_t queue_used = 0, queue_cap = 0;  // mesh queue (THPS1 512, THPS2 512, THPS3 845): entries one pass used / its size (0 = none); past it the game drops meshes
    uint32_t blend_used = 0, blend_cap = 0;  // the queue's second list (THPS1: blended / special-mode parts, 64; THPS2/3: the depth-sorted translucent meshes, 96)
    uint32_t defer_used = 0, defer_cap = 0;  // THPS3: course items World_DrawItems puts aside for a pass of their own (64 slots, which the game does not check)
    uint32_t reset_used = 0, reset_cap = 0;  // THPS1: the per-frame list of matrix pointers the frame end clears (about 2000 slots, which the game does not check)
    uint32_t runs = 0;                     // most passes of the course items in one frame (draw-distance runs; 1 = as the game does)
    bool mtx_capped = false;               // a pool ran out this frame (the game's own fallback was used)
};

bool valid_mode(const std::string& m);     // original | 2x | 4x | full (also far, max: the old names of 2x, 4x)
// full: the far plane as far as kFullFarPlane (game/draw_distance.cpp clamps every mode to it): past any course's bounds.
// 18000, not 20000: the sky / scenery frustums use far 20000, and the hooks tell the gameplay camera's calls from the others by their far.
inline constexpr float kFullFarPlane = 18000.0f;
void set_mode(const std::string& m);       // graphics.cfg / --draw-distance / the control command
std::string mode();
// "full" and the governor is not holding it back: the zone-culled course items are drawn too (all three games) (game/draw_distance.cpp).
bool show_all();
bool unhide_everything();   // tests (`dd showall all`): unhide every hidden item, not only the zone-culled ones
// The matrix pools (game/draw_distance.cpp). With `dd_buffers = bigger` (graphics.cfg / --dd-buffers; the default) the games always set up
// pools of 2048 matrices (THPS1 once at start-up, THPS2/3 at each course start), whatever the setting, and keep the game's own cap (500 /
// 420) in force while the setting is `original`: so switching the setting takes effect at once. `original` = the game's own allocations,
// byte for byte (the governor then keeps the far plane within the game's pools). Read at start-up only: a change applies at the next start.
bool valid_buffers(const std::string& v);   // bigger | original
void set_buffers(const std::string& v);     // graphics.cfg / --dd-buffers (start-up only)
std::string buffers();
bool big_buffers();                         // allocate the bigger pools (dd_buffers = bigger)
// The setting wants more than the game's own distance now: the bigger pools' cap is in force (when they were allocated).
bool enlarge_pools();
float factor();                            // far-plane factor in use (1 = the game's own): after the governor
// The setting's factor as the governor allows it (its ceiling, lowered by a back-off). THPS2/3 give their far plane this
// one, since their world view takes a new far only at a course start or a camera change, and apply factor() to the items' cull instead.
float projection_factor();
// View_SetFrustum hook (src/widescreen.cpp, game thread): the far value to use for this call (f itself when off or not the gameplay camera).
float far_plane(uint8_t* rdram, float f);
// send_dl (gfx thread), every gfx task: high-water marks and the governor.
void note_task(uint8_t* rdram, uint32_t data_ptr, uint32_t data_size);
std::string stats();
bool control(const std::string& cmd, std::istringstream& in);   // control "dd ..."

// The game's side (game/draw_distance.cpp). dd_game_far reports the factor it really used (after the full clamp) with note_effective.
void note_effective(float k);
void note_game_far(float f);   // the game's own far for the course in play, every call (how far the factor can go)
// A course starts (THPS2/3: the course gfx set-up, also at RETRY; THPS1: the course items drawn again after a pause in drawing them, or
// another item list): the factor is the setting again and the governor's ceiling is reset. Game thread.
void note_course_start();
// note_course_start calls so far: the course instance in play. The games' zone records (which items the zone culling hid, which an event
// owns) belong to one instance and start again when this changes (a RETRY can rebuild the item list at the same address and count).
uint64_t course_generation();
// "full": zone-culled items are drawn within unhide_reach() x the game's far beyond the camera (a huge value once the factor is at its end,
// as it is from a course start; factor - 1 while it rises again below the ceiling).
float unhide_reach();
bool supported();
// f = the far argument of a View_SetFrustum call, k > 1: the far to use (f when this call is not the gameplay camera's).
float dd_game_far(uint8_t* rdram, float f, float k);
// false = not a frame of the game's 3D pipeline (nothing to measure).
bool dd_game_measure(uint8_t* rdram, uint32_t data_ptr, uint32_t data_size, BufferUse& u);
// Tests (`dd item N`): course item N's hidden bits, what the zone culling recorded, whether an event owns it, and whether "full" draws it
// now ("flags 0x0041 zone 0x00 event 1 drawn 0").
std::string dd_game_item(uint8_t* rdram, uint32_t index);
// Tests (`dd own N`): course item N is event-owned from now on, as if an event had named it (World_FindItem); "none" where unsupported.
std::string dd_game_own(uint8_t* rdram, uint32_t index);
// Tests (`dd heap [reset]`): the game heap the matrix pools come from: failed-size cache, free bytes, largest free block, and the
// low-water marks since the start or the last reset (least free bytes and smallest largest block seen at an allocation, the smallest
// margin between a request and the largest free block, failed requests); "none" before the first allocation. reset: start the marks again.
std::string dd_game_heap(uint8_t* rdram, bool reset);
// The display-list byte budget (THPS2/3, game/draw_distance.cpp): before each extra pass of World_DrawItems the game side checks
// used + pass bound + rest-of-frame bound + margin <= the buffer, and when the buffer cannot hold every extra pass it keeps the nearest
// ones it can and leaves the farthest out: the nearest pass keeps its bound reserved from the first extra pass on (the passes go far to
// near), so a cut of the nearest items once passes were drawn (`late`) happens only if a pass exceeded its bound.
// dl_budget() gives the test overrides (`dd dlbudget CAP REST PASS MARGIN`, bytes; 0 = the game's own value; CAP below the buffer forces
// cuts); note_cut counts what was left out (`dd`: cuts, cut_items, late_cuts; --verbose logs them).
struct DlBudget { uint32_t cap = 0, rest = 0, pass = 0, margin = 0; };
DlBudget dl_budget();
void note_cut(uint32_t items, uint32_t used, uint32_t cap, bool late = false);   // game thread: one World_DrawItems call left `items` extra items out
// Tests (`dd dlprobe [reset]`; THPS2/3, debug env THPS_DD_DLPROBE / THPS_DD_CANARY): the display-list bytes per part of a frame and the
// guard checks after the lists (game/draw_distance.cpp); "none" where the game has no such measurement.
std::string dd_game_dlprobe(uint8_t* rdram, bool reset);

// The bigger buffers come from the game's heap, whose allocator (THPS1 Heap_AllocUnlocked, THPS2/3 Mem_Alloc: first fit over a free list)
// does not survive a failed request: its failure path takes the heap's lock again and the game hangs. So the games plan a set-up's requests
// before enlarging any of them (game/draw_distance.cpp, review 2026-10-26, finding 1): the native requests must fit, and an enlarged plan
// must fit in order and leave a largest free block of at least plan_floor(): the native plan's, or kHeapReserve when that is more, so the
// requests still to come after the set-up find a block as big as the biggest one the games make then.
// Free list (all three games): head at heap +0, next at block +4, free bytes at block +20; heap +16 = the last failed size (-1 = none).
std::vector<uint32_t> heap_free_blocks(const uint8_t* rdram, uint32_t heap);   // the free bytes of each block, in list order (or the test view)
uint32_t heap_failed_cache(const uint8_t* rdram, uint32_t heap);              // heap +16 (0xFFFFFFFF with the test view)
// Places `needs` (sizes as the allocator rounds them) in order, each in the first block that holds it; returns the largest free block left,
// or -1 when one of them does not fit or is at or above the failed-size cache (the allocator then fails at once).
int64_t first_fit_after(std::vector<uint32_t> blocks, const std::vector<uint32_t>& needs, uint32_t cache);
// 768 KiB: the biggest single requests the games make after their set-ups are course loads of 587,000 (THPS2) and 600,320 bytes (THPS3;
// THPS1 154,160), measured with `dd heap` on every course (2026-10-22), with a quarter to spare. (A floor of the native plan's largest
// block alone refused every enlargement in normal play: THPS1's pools and THPS2/3's lists come from the largest block, 1.5-2 MB.)
inline constexpr int64_t kHeapReserve = 768 * 1024;
// The largest free block an enlarged plan must leave, given what the native plan leaves (-1: the native plan does not fit).
inline int64_t plan_floor(int64_t native_largest) { return native_largest < kHeapReserve ? native_largest : kHeapReserve; }
// Tests: THPS_DD_HEAP_FREE=A[,B..] (bytes, decimal or 0x hex): the planning sees a free list of these blocks instead of the real one, from
// each set-up's start (heap_view_begin), and every request the set-up makes is taken from it (heap_view_take: false and a WARNING when the
// view could not have satisfied it, i.e. the real allocator would have hung). `dd heap` reports view_fail.
bool heap_view_on();
void heap_view_begin();
bool heap_view_take(uint32_t need, const char* what);
uint32_t heap_view_fails();

// ftime (`ftime start` .. `ftime stop`, src/frametime.cpp: dd_hook_us_per_task): a hook's own time on the game thread, without the game
// code it calls through guest(). When ftime is off it costs one relaxed load per hook call.
bool timing();
void note_hook_time(uint64_t ns);
class HookTimer {
    using Clock = std::chrono::steady_clock;
    bool on_;
    Clock::time_point t0_{};
    uint64_t guest_ns_ = 0;
public:
    HookTimer() : on_(timing()) { if (on_) t0_ = Clock::now(); }
    HookTimer(const HookTimer&) = delete;
    HookTimer& operator=(const HookTimer&) = delete;
    template <class F> void guest(F&& f) {
        if (!on_) { f(); return; }
        const auto t = Clock::now();
        f();
        guest_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count());
    }
    ~HookTimer() {
        if (on_) note_hook_time(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0_).count()) - guest_ns_);
    }
};

}  // namespace thps::dd
