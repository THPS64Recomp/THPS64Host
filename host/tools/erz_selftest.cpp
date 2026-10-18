// Checks the native ERZ decoder (src/erz.cpp) against the user's ROM: inflates the
// 14 main-image chunks the way Boot_Main does and compares with the decomp's
// baserom.main.bin (optional). Prints only sizes/CRC results, never data.
// usage: erz_selftest ROM.z64 [baserom.main.bin]
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "../src/erz.h"

static std::vector<uint8_t> slurp(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s ROM.z64 [main.bin]\n", argv[0]); return 2; }
    auto rom = slurp(argv[1]);
    if (rom.size() < 0x20000 || rom[0] != 0x80) { std::fprintf(stderr, "need a big-endian .z64\n"); return 2; }
    auto be32 = [&](size_t o) { return (uint32_t(rom[o]) << 24) | (uint32_t(rom[o + 1]) << 16) | (uint32_t(rom[o + 2]) << 8) | rom[o + 3]; };
    const size_t table = 0x13B14;
    uint32_t n = be32(table);
    std::vector<uint8_t> img;
    int bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        size_t a = table + be32(table + 4 + 4 * i);
        size_t span = be32(table + 8 + 4 * i) - be32(table + 4 + 4 * i);
        std::vector<uint8_t> out;
        thps::ErzInfo info;
        std::string err;
        if (rom[a] == 'E' && rom[a + 1] == 'R' && rom[a + 2] == 'Z') {
            if (!thps::erz_decompress(&rom[a], rom.size() - a, out, info, err)) { std::printf("chunk %u: %s\n", i, err.c_str()); return 1; }
            out.resize(info.usize);
        } else {
            out.assign(rom.begin() + a, rom.begin() + a + span);
        }
        std::printf("chunk %2u: rom 0x%06zX method %d usize 0x%05X pcrc %s ucrc %s\n", i, a, info.method, info.usize,
                    info.pcrc_ok ? "ok" : "BAD", info.ucrc_ok ? "ok" : "BAD");
        bad += !(info.pcrc_ok && info.ucrc_ok);
        img.insert(img.end(), out.begin(), out.end());
    }
    std::printf("image: 0x%zX bytes\n", img.size());
    if (argc > 2) {
        auto ref = slurp(argv[2]);
        bool same = ref == img;
        std::printf("vs %s: %s\n", argv[2], same ? "IDENTICAL" : "DIFFERENT");
        bad += !same;
    }
    return bad ? 1 : 0;
}
