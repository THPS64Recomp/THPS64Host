// Native ERZ (= RNC ProPack, magic "ERZ") decoder, ported from decomp/thps1/tools/erz.py.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace thps {
struct ErzInfo {
    int method = 0;
    uint32_t usize = 0, psize = 0;
    uint16_t ucrc = 0, pcrc = 0;
    bool pcrc_ok = false, ucrc_ok = false;
};
uint16_t erz_crc16(const uint8_t* data, size_t len);
// `chunk` points at the 18-byte header; `avail` bytes are readable.
// Returns false (with `err`) on a malformed stream. `out` may exceed usize
// (the handwritten decoders also overrun to the end of the last block).
bool erz_decompress(const uint8_t* chunk, size_t avail, std::vector<uint8_t>& out, ErzInfo& info, std::string& err);
}
