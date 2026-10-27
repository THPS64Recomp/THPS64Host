/* Host functions called from N64Recomp [[patches.hook]] text (config/<game>.hooks.toml). Shared by THPS1-3.
 * Included by every generated RecompiledFuncs/*.c file (recomp_include), so keep it C. */
#ifndef THPS_HOOKS_H
#define THPS_HOOKS_H
#include <stdint.h>
#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Boot_PioCopy(a0 = cart addr (osRomBase|offset), a1 = dst vram, a2 = bytes): word copy from ROM. */
void thps_boot_pio_copy(uint8_t* rdram, recomp_context* ctx);
/* Erz_UnpackMethodN(a0 = ERZ header, a1 = dst, a2 = packed size, a3 = unpacked size). */
void thps_erz_unpack(uint8_t* rdram, recomp_context* ctx, int method);
/* osPiRawStartDma(a0 = dir, a1 = devAddr, a2 = dramAddr, a3 = size); completion -> OS_EVENT_PI queue. */
void thps_pi_raw_start_dma(uint8_t* rdram, recomp_context* ctx);
/* osSetEventMesg(a0 = event, a1 = mq, a2 = msg): records OS_EVENT_PI, forwards the rest. */
void thps_set_event_mesg(uint8_t* rdram, recomp_context* ctx);

/* osAiGetStatus(): AI_STATUS_FIFO_FULL (0x80000000) while the host audio queue is full. */
void thps_ai_get_status(uint8_t* rdram, recomp_context* ctx);
// osContGetQuery (renamed): the runtime's answer + CONT_CARD_ON for ports with a Controller Pak.
void thps_cont_get_query(uint8_t* rdram, recomp_context* ctx);

/* View_ComputeCullPlanes(a0 = view) entry: in widescreen, runs the function with the frustum's
 * l/r widened and returns 1 (skip the original body); 0 = run it as is. src/widescreen.cpp */
int thps_widen_cull_planes(uint8_t* rdram, recomp_context* ctx);

/* View_SetFrustum(a0 = view, a1 = l, a2 = r, a3 = b, stack: t, n, f) entry. src/widescreen.cpp */
int thps_view_set_frustum(uint8_t* rdram, recomp_context* ctx);

/* Skip Intro (the game's game/skip_intro.cpp or the shared skip_intro_overlay.cpp). thps_skip_intro_notice is every game's boot
 * notice entry ("EXPANSION PAK FOUND"): the host presses B for the player. The legal screen / logos differ by game: THPS1
 * hooks Front_IntroUpdate (1 = return 3, done, at once); THPS2/3 hook the intro overlay's update. */
void thps_skip_intro_notice(uint8_t* rdram, recomp_context* ctx);
int thps_skip_intro_logos(uint8_t* rdram, recomp_context* ctx);      /* THPS1 */
void thps_skip_intro_update(uint8_t* rdram, recomp_context* ctx);    /* THPS2/3: the t2_intro overlay's update: skips its two minimum-time waits */

/* Unlock All Levels cheat (--unlock-levels, test aid; THPS2/3): returns 1 when the level select's "is this level available?" test should answer yes. */
int thps_unlock_level(uint8_t* rdram, recomp_context* ctx);
/* The same cheat in THPS1 (game/cheats.cpp): CourseSelect_UpdateLock(a0 = screen) entry; 1 = the course is marked available, skip the body. */
int thps_unlock_course(uint8_t* rdram, recomp_context* ctx);
/* THPS2/3 cheats (game/cheats.cpp): Cheat_IsOn(a0 = cheat index) entry; 1 = answer "on" (the host has that cheat on), 0 = ask the game. */
int thps_cheat_is_on(uint8_t* rdram, recomp_context* ctx);

/* THPS2/3 World_DrawItems(a0 = item list, a1 = view, a2 = player) entry (game/draw_distance.cpp): with draw distance "full", runs the
 * function with the items' trigger-visibility bits cleared and puts them back; when it draws more than the game would, the extra items
 * go first, farthest first, in passes of their own, so the renderer's lists never overflow; returns 1 (skip the body), 0 = run it as is. */
/* THPS1 World_DrawItems(a0 = world, a1 = view, a2 = player) entry (game/draw_distance.cpp): with a far plane longer than the game's, runs
 * the function several times (the items beyond the game's far plane in passes, farthest first, then the game's own pass) so each pass fits
 * the mesh queue; returns 1, or 0 = run it as is. */
int thps_world_draw_items(uint8_t* rdram, recomp_context* ctx);

/* World_SetVisibilityInBox entry (game/draw_distance.cpp): runs the function and records which hidden bits it set; returns 1. */
int thps_world_set_visibility_in_box(uint8_t* rdram, recomp_context* ctx);

/* World_FindItem(a0 = list, a1 = name) entry (game/draw_distance.cpp): runs it and marks the found item as event-owned; returns 1. */
int thps_world_find_item(uint8_t* rdram, recomp_context* ctx);

/* Mesh_QueueEmit entry (game/draw_distance.cpp): notes how full the renderer's lists of one pass are (the queue is full here; past its
 * size the game drops meshes). THPS1: a0 = the mesh queue (512 entries). THPS2/3: a0 = the renderer, with its mesh queue (THPS2 512,
 * THPS3 845 entries) and its depth-sorted list of translucent meshes (96). Reads only; the body always runs. */
void thps_mesh_queue_emit(uint8_t* rdram, recomp_context* ctx);

/* THPS3 World_DrawDeferredItems(a0 = item list) entry (game/draw_distance.cpp): notes how many course items World_DrawItems put on its
 * list of items drawn after the others (64 slots, which the game does not check). Reads only; the body always runs. */
void thps_world_draw_deferred(uint8_t* rdram, recomp_context* ctx);

/* Mem_Alloc(a0 = heap, a1 = size, a2 = align) entry (THPS1: Heap_Alloc; game/draw_distance.cpp): enlarges the matrix-pool request of
 * the gfx set-up (THPS2/3: each course start; THPS1: start-up) when draw distance is on; every other call is left alone. */
int thps_mem_alloc(uint8_t* rdram, recomp_context* ctx);

/* THPS2/3 Gfx_SetupCourse() entry (game/draw_distance.cpp): the course gfx set-up (each course start and RETRY), which allocates the two
 * display lists, the matrix pools and the vertex pools. Runs the body with thps_mem_alloc told that the display-list requests are this
 * set-up's (dd_buffers = bigger: at least 144 KiB each); returns 1. */
int thps_gfx_setup_course(uint8_t* rdram, recomp_context* ctx);

/* THPS1 Gfx_AllocMtx() entry (game/draw_distance.cpp): the per-frame matrix allocator, whose cap (500) is an immediate. With the pools
 * enlarged at start-up it hands out matrices up to the bigger cap (v0) and returns 1; otherwise 0, and the game's body runs. */
int thps_gfx_alloc_mtx(uint8_t* rdram, recomp_context* ctx);

/* THPS1 Gfx_FrameEnd() entry (game/draw_distance.cpp): notes how many entries the frame put on the list of matrix pointers the frame end
 * clears (about 2000 slots, which the game does not check). Reads only; the body always runs. */
void thps_gfx_frame_end(uint8_t* rdram, recomp_context* ctx);
/* Music_LoadSong(a0 = music player, a1 = song) entry: the old song's channels still reading the song buffer stop without
 * reading it again before the next song is copied over it. src/music.cpp */
void thps_music_load_song(uint8_t* rdram, recomp_context* ctx);

/* libc memset(a0 = dst, a1 = byte, a2 = len) / memcpy(a0 = dst, a1 = src, a2 = len) entry: done natively, v0 = dst; returns 1 (skip the
 * original body), or 0 for a range outside RDRAM (the original runs). src/hooks.cpp */
int thps_memset(uint8_t* rdram, recomp_context* ctx);
int thps_memcpy(uint8_t* rdram, recomp_context* ctx);

/* Diagnostic at GameEntry: verifies the main image the boot loader inflated. */
void thps_on_game_entry(uint8_t* rdram, recomp_context* ctx);

/* libultra internals that N64Recomp treats as "ignored" (not emitted, not declared) but
 * that some recompiled, un-named libultra fragments in the boot segment still call.
 * Defined in src/ultra_stubs.cpp (log once, no pak / no-op). */
#define THPS_ULTRA_STUBS(X) \
    X(__osCheckPackId) X(__osContAddressCrc) X(__osContRamRead) X(__osContRamWrite) \
    X(__osPfsRWInode) X(__osPfsSelectBank) X(__osRepairPackId) \
    X(__osEnqueueThread) X(__osPopThread) X(__osTimerInterrupt) X(__osViSwapContext) \
    X(__osPiCreateAccessQueue) X(__osSiCreateAccessQueue) X(osPfsIsPlug) X(__osContGetInitData)
#define THPS_DECLARE_STUB(name) void name##_recomp(uint8_t* rdram, recomp_context* ctx);
THPS_ULTRA_STUBS(THPS_DECLARE_STUB)
/* Implemented by librecomp (pak.cpp) but not declared in funcs.h. */
void osPfsNumFiles_recomp(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif
#endif
