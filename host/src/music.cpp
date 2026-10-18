// The next song is loaded over the one still playing (THPS1-3: the same music player on top of the same MUS library).
//
// The games keep one song buffer. To change the song (a course starts, a run ends, the playlist moves on), Music_Play calls
// MusHandleStop(old song, 0) and then Music_LoadSong(player, song), which copies the new song from the cartridge into that
// buffer; MusStartSong starts it. MusHandleStop only marks the old song's channels (`stopping` = 0): the audio thread stops
// them in its next frame, AFTER it has run their events for that frame, and it reads those events from the buffer, which by
// then holds the new song. Wrong bytes there become a wave index past the sample bank's list (a wild ALWaveTable pointer:
// SIGBUS in n_alLoadParam under func_800D6930 in THPS2) or a command byte past the command table ("Failed to find
// function" under func_800D4CC0). On the console the copy goes through the game's DMA thread in 2 KB pieces and the game
// thread waits for each, so the audio thread can run its frame in between, while the old bytes are still there. Here the
// copy is done at once and the game thread does not yield before the new song is in place. Seen at every course start in
// THPS2 and THPS3 (three channels); THPS1 asks MusHandleAsk first and had no channel left reading at its loads.
//
// Fix: at Music_LoadSong's entry, every channel still reading from the buffer is told to stop in the next frame (what
// MusHandleStop(handle, 0) does) and to read no more events (its next event time is set past any channel time). The old song
// was being stopped anyway; nothing audible changes. THPS_MUSIC_GUARD=0 turns the fix off (A/B tests); the log line then
// counts the channels that will read the new song.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "host.h"
#include "thps_hooks.h"
#include "versions.h"

namespace {
// The MUS library's channel record (the same code in all three games; only the two globals move: versions/<class>/data_syms.toml).
constexpr uint32_t kChannelSize = 304;
constexpr uint32_t kChPData = 4;          // pointer to the next byte of song data (0 = channel free)
constexpr uint32_t kChStopping = 16;      // -1 = playing; N = stop after N more frames (MusHandleStop)
constexpr uint32_t kChNextEvent = 60;     // channel time of the next event (events run while it is below the channel time at +12, unsigned)
// The game's music player object (a0 of Music_LoadSong).
constexpr uint32_t kPlayerSongBuf = 84;   // the song buffer
constexpr uint32_t kPlayerSongBufSize = 92;  // its size (the largest song)

int g_guard = -1;  // -1 = not read yet

uint32_t rd(uint8_t* rdram, uint32_t addr) { return uint32_t(MEM_W(0, (gpr)(int32_t)addr)); }
void wr(uint8_t* rdram, uint32_t addr, uint32_t v) { MEM_W(0, (gpr)(int32_t)addr) = int32_t(v); }
bool in_ram(uint32_t a) { return a >= 0x80000000u && a < 0x80800000u; }
}  // namespace

extern "C" void thps_music_load_song(uint8_t* rdram, recomp_context* ctx) {
    if (g_guard < 0) {
        const char* e = std::getenv("THPS_MUSIC_GUARD");
        g_guard = (e && std::strcmp(e, "0") == 0) ? 0 : 1;
    }
    const uint32_t player = uint32_t(ctx->r4);
    const uint32_t song = uint32_t(ctx->r5);
    const uint32_t nch_addr = thps::sym().gMusNumChannels, ch_addr = thps::sym().gMusChannels;
    if (!in_ram(player) || !in_ram(nch_addr) || !in_ram(ch_addr)) return;
    const uint32_t buf = rd(rdram, player + kPlayerSongBuf), size = rd(rdram, player + kPlayerSongBufSize);
    const uint32_t channels = rd(rdram, ch_addr);
    const int32_t n = int32_t(rd(rdram, nch_addr));
    if (!in_ram(buf) || !in_ram(channels) || n <= 0 || n > 64) return;
    int reading = 0, fading = 0;
    for (int32_t i = 0; i < n; i++) {
        const uint32_t cp = channels + uint32_t(i) * kChannelSize;
        const uint32_t pdata = rd(rdram, cp + kChPData);
        if (!pdata || pdata < buf || pdata - buf >= size) continue;
        reading++;
        if (int32_t(rd(rdram, cp + kChStopping)) > 0) fading++;
        if (g_guard) {
            wr(rdram, cp + kChStopping, 0);
            wr(rdram, cp + kChNextEvent, 0xFFFFFFFFu);
        }
    }
    if (reading) {
        std::printf("[thps] music: song %u loads over %d channel(s) still reading the buffer (%d fading)%s\n", song, reading, fading,
                    g_guard ? ": stopped without further events" : " (THPS_MUSIC_GUARD=0: left as they are)");
    } else {
        std::printf("[thps] music: song %u loads\n", song);
    }
    std::fflush(stdout);
}
