// Skip Intro for the games whose legal text + logo screen is a front-end overlay (THPS2, THPS3; game/game.toml
// `overlays`). skip_intro=1 in general.cfg / the General tab, or --skip-intro / --no-skip-intro: boot straight
// to the main menu. The boot has two parts (the private development notes H4):
// - the "EXPANSION PAK FOUND. OK" message box (opened by MsgBox_SetText, which stores the text pointer
//   at box+0x4B0). Skip Intro presses B for the player while that one text is up (never for the
//   Controller Pak error boxes, which share the screen).
// - the legal text + logo screen is the overlay t2_intro; its update function (Ovl_IntroUpdate) is hooked
//   (thps_skip_intro_update) to skip the 9.3 s legal text, which leaves the logo/title screen.
// Per-version facts come from the version tables (thps::sym()): the two notice texts (gTextExpansionFound /
// gTextExpansionNotFound) and the intro screen's field offsets (Intro_Timer, Intro_State).
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "thps_hooks.h"
#include "host.h"
#include "versions.h"

namespace {
constexpr int k_hold_polls = 2, k_period_polls = 4;

std::atomic<bool> g_notice_up{false}, g_intro_active{false};
std::atomic<int> g_poll{0};
std::atomic<bool> g_logged_notice{false};
}  // namespace

// Game thread, entry of the intro screen's update (Ovl_IntroUpdate(screen)). Screen fields: timer (float) at
// sym().Intro_Timer (+0x250 in THPS2, +0x290 in THPS3), state at Intro_State. State 0 = legal text for 9.3 s, then state 1 = the logo / title screen, which
// waits for START (accepted after 7 s; no START for ~20 s -> the attract demo). With Skip Intro on, state
// 0 ends at once, so the game boots straight to the title. (Tried and dropped: also skipping the 7 s and the
// fade of state 1, or pressing START there: both returned straight into the attract demo, LOG H4.)
extern "C" void thps_skip_intro_update(uint8_t* rdram, recomp_context* ctx) {
    if (!thps::settings::skip_intro()) return;
    const int32_t screen = int32_t(ctx->r4);
    const int32_t timer_off = int32_t(thps::sym().Intro_Timer), state_off = int32_t(thps::sym().Intro_State);   // signed, like the literals before: MEM_W sign-extends the sum
    if (MEM_W(state_off, screen) != 0) return;
    float t;
    std::memcpy(&t, &MEM_W(timer_off, screen), 4);
    const float want = 9.4f;
    if (t < want) {
        int32_t w;
        std::memcpy(&w, &want, 4);
        MEM_W(timer_off, screen) = w;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::printf("[thps] skip intro: legal screen skipped (frame %" PRIu64 ")\n", thps::stats.vi_origin_changes.load());
            std::fflush(stdout);
        }
    }
}

// Game thread, entry of MsgBox_SetText(box, text, ...): a message box got its text.
extern "C" void thps_skip_intro_notice(uint8_t*, recomp_context* ctx) {
    uint32_t text = uint32_t(ctx->r5);
    if (text == thps::sym().gTextExpansionFound || text == thps::sym().gTextExpansionNotFound) g_notice_up = true;
}

// hooks.cpp, when an overlay's code blob is DMA'd: the intro screen starts the input phase, any other ends both.
void thps::skip_intro_overlay_loaded(const char* name) {
    bool intro = std::strcmp(name, "t2_intro") == 0;
    if (intro) g_notice_up = false;
    (void)g_intro_active;
}

// Game thread, port 1 poll (main.cpp get_input).
void thps::skip_intro_input(uint16_t* buttons) {
    if (!settings::skip_intro()) return;
    bool notice = g_notice_up.load();
    if (!notice) return;
    int n = g_poll.fetch_add(1) % k_period_polls;
    if (n >= k_hold_polls) return;
    if (notice) {
        *buttons |= 0x4000;  // B
        if (!g_logged_notice.exchange(true)) {
            std::printf("[thps] skip intro: pressing B on the Expansion Pak notice (frame %" PRIu64 ")\n", stats.vi_origin_changes.load());
            std::fflush(stdout);
        }
    }
}
