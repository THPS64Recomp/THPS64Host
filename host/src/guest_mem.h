// Guest RAM access shared by the games' adapters and the trigger classifier. RDRAM is stored as host-order words,
// so a halfword sits at (a ^ 2) and a byte at (a ^ 3); addresses are masked to 26 bits (a 64 MB window; the games use the first 8 MB, in_ram).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace thps::mem {
inline bool in_ram(uint32_t a) { return a >= 0x80000000u && a < 0x80800000u; }
inline uint32_t word(const uint8_t* rdram, uint32_t vaddr) { return *reinterpret_cast<const uint32_t*>(rdram + (vaddr & 0x3FFFFFC)); }
inline void put_word(uint8_t* rdram, uint32_t vaddr, uint32_t v) { *reinterpret_cast<uint32_t*>(rdram + (vaddr & 0x3FFFFFC)) = v; }
inline float f32(const uint8_t* rdram, uint32_t vaddr) {
    uint32_t w = word(rdram, vaddr);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}
inline float fword(const uint8_t* rdram, uint32_t a) { return f32(rdram, a); }
inline uint16_t hword(const uint8_t* ram, uint32_t a) {
    return *reinterpret_cast<const uint16_t*>(ram + ((a & 0x3FFFFFE) ^ 2));
}
inline uint16_t* hword_ptr(uint8_t* rdram, uint32_t a) {
    return reinterpret_cast<uint16_t*>(rdram + ((a & 0x3FFFFFE) ^ 2));
}
inline void max_into(std::atomic<uint32_t>& a, uint32_t v) {
    uint32_t m = a.load();
    while (v > m && !a.compare_exchange_weak(m, v)) {}
}
// The game heap's free list: head at +0, next at +4, free bytes at +20; +16 = the last failed size (-1 = none).
struct HeapStats { uint32_t failed, free_bytes, largest, blocks; };
inline HeapStats heap_stats(const uint8_t* rdram, uint32_t heap) {
    HeapStats h{word(rdram, heap + 16), 0, 0, 0};
    for (uint32_t b = word(rdram, heap), k = 0; b && k < 100000; b = word(rdram, b + 4), k++) {
        const uint32_t f = word(rdram, b + 20);
        h.free_bytes += f, h.largest = std::max(h.largest, f), h.blocks++;
    }
    return h;
}
}  // namespace thps::mem
