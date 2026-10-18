// Stubs for libultra internals that recompiled boot-segment fragments call (see thps_hooks.h).
// Paks: report "no pack" like librecomp's pak.cpp. Thread/VI/timer internals: these belong to
// libultra managers that the runtime replaces, so reaching them is logged as unexpected.
#include <atomic>
#include <cstdio>
#include <cstring>
#include "thps_hooks.h"

namespace {
constexpr int PFS_ERR_NOPACK = 1;
void note(const char* name, std::atomic<int>& count) {
    if (count++ < 3) std::fprintf(stderr, "[thps] stub %s called\n", name);
}
}  // namespace

#define THPS_PAK_STUB(name)                                          \
    extern "C" void name##_recomp(uint8_t* rdram, recomp_context* ctx) { \
        static std::atomic<int> n{0};                               \
        note(#name, n);                                             \
        ctx->r2 = PFS_ERR_NOPACK;                                   \
    }
#define THPS_NOP_STUB(name)                                          \
    extern "C" void name##_recomp(uint8_t* rdram, recomp_context* ctx) { \
        static std::atomic<int> n{0};                               \
        note(#name, n);                                             \
        ctx->r2 = 0;                                                \
    }

THPS_PAK_STUB(__osCheckPackId)
THPS_PAK_STUB(__osContRamRead)
THPS_PAK_STUB(__osContRamWrite)
THPS_PAK_STUB(__osPfsRWInode)
THPS_PAK_STUB(__osPfsSelectBank)
THPS_PAK_STUB(__osRepairPackId)
THPS_NOP_STUB(__osContAddressCrc)
THPS_NOP_STUB(__osEnqueueThread)
THPS_NOP_STUB(__osPopThread)
THPS_NOP_STUB(__osTimerInterrupt)
THPS_NOP_STUB(__osViSwapContext)
THPS_NOP_STUB(__osPiCreateAccessQueue)
THPS_NOP_STUB(__osSiCreateAccessQueue)
// The body of the renamed osContGetQuery (unreachable: the hook returns first) calls this.
THPS_NOP_STUB(__osContGetInitData)

// osPfsIsPlug is implemented with the Controller Pak in src/pak.cpp.
