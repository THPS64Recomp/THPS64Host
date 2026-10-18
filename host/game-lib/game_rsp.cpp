// The audio microcode wrapper. n_aspMain.cpp (RSPRecomp output) is compiled with -Ddmem=thps_game_dmem, so its
// DMEM is private to the library (a DLL cannot import the runtime's `dmem` array); the runtime's DMEM is copied in
// and back out around each run (4 KiB, twice per audio task).
#include <cstdint>
#include <cstring>

#include "librecomp/rsp.hpp"   // RspExitReason; declares `extern uint8_t thps_game_dmem[]` through the -D

uint8_t thps_game_dmem[0x1000];

RspExitReason n_aspMain(uint8_t* rdram, uint32_t ucode_addr);   // n_aspMain.cpp, generated

extern "C" int thps_game_aspmain(uint8_t* rdram, uint32_t ucode_addr, uint8_t* host_dmem) {
    std::memcpy(thps_game_dmem, host_dmem, sizeof thps_game_dmem);
    RspExitReason r = n_aspMain(rdram, ucode_addr);
    std::memcpy(host_dmem, thps_game_dmem, sizeof thps_game_dmem);
    return int(r);
}
