// Native ERZ (= RNC ProPack with the magic renamed) decoder.
// Straight port of decomp/thps1/tools/erz.py (which mirrors the handwritten MIPS
// decoders at 0x80001330 = method 1 and 0x80000C40 = method 2). See that file
// for the format notes.
#include "erz.h"

#include <algorithm>
#include <iterator>
#include <cstring>

namespace thps {

static uint16_t crc_table[256];
static bool crc_table_ready = false;

uint16_t erz_crc16(const uint8_t* data, size_t len) {
    if (!crc_table_ready) {
        for (int i = 0; i < 256; i++) {
            uint16_t c = uint16_t(i);
            for (int k = 0; k < 8; k++) c = (c & 1) ? uint16_t((c >> 1) ^ 0xA001) : uint16_t(c >> 1);
            crc_table[i] = c;
        }
        crc_table_ready = true;
    }
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) crc = uint16_t((crc >> 8) ^ crc_table[(crc ^ data[i]) & 0xFF]);
    return crc;
}

namespace {

struct DecodeError {
    std::string msg;
};

// ------------------------------------------------------------------ method 1
struct Bits1 {
    const std::vector<uint8_t>& buf;
    size_t p;
    uint64_t bb;
    int left = 0;

    Bits1(const std::vector<uint8_t>& b, size_t pos) : buf(b), p(pos) { bb = word(p); }

    uint32_t word(size_t pos) const {
        uint32_t lo = pos < buf.size() ? buf[pos] : 0;
        uint32_t hi = pos + 1 < buf.size() ? buf[pos + 1] : 0;
        return lo | (hi << 8);
    }
    uint32_t peek(uint32_t mask) const { return uint32_t(bb) & mask; }
    void skip(int n) {
        left -= n;
        if (left < 0) {
            int k = left + n;
            bb >>= k;
            p += 2;
            bb |= uint64_t(word(p)) << 16;
            n -= k;
            left = 16 - n;
        }
        bb = (bb >> n) & 0xFFFFFFFFull;
    }
    uint32_t read(int n) {
        uint32_t v = uint32_t(bb) & ((1u << n) - 1);
        skip(n);
        return v;
    }
    void literals(size_t n, std::vector<uint8_t>& out) {
        if (p + n > buf.size()) throw DecodeError{"literal run past end of input"};
        out.insert(out.end(), buf.begin() + p, buf.begin() + p + n);
        p += n;
        bb = (bb & ((1ull << left) - 1)) | (uint64_t(word(p)) << left);
    }
};

struct HuffEntry {
    uint32_t mask, code;
    int len, sym;
};
// A table plus a lookup of its next kLutBits bits (load freeze, 2026-10-18: the linear search was most of the decode time). The LUT
// gives the same answer as the linear search in table order (first match wins); codes longer than kLutBits fall back to that search.
constexpr int kLutBits = 9;
struct HuffTable {
    std::vector<HuffEntry> entries;
    int16_t lut[1 << kLutBits];   // (len << 8) | sym, or -1: no entry of kLutBits bits or fewer matches
    void build_lut() {
        std::fill(std::begin(lut), std::end(lut), int16_t(-1));
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {   // backwards: an earlier entry overwrites a later one
            if (it->len > kLutBits) continue;
            for (uint32_t k = 0; k < (1u << (kLutBits - it->len)); k++) lut[it->code | (k << it->len)] = int16_t((it->len << 8) | it->sym);
        }
    }
};

static uint32_t bit_reverse(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) r |= ((v >> i) & 1u) << (n - 1 - i);
    return r;
}

static void read_table(Bits1& bits, HuffTable& table) {
    uint32_t num = bits.read(5);
    if (num == 0) return;  // MIPS leaves the previous table in place
    int lens[32];
    for (uint32_t i = 0; i < num; i++) lens[i] = int(bits.read(4));
    table.entries.clear();
    uint32_t code = 0;
    for (int L = 1; L <= 16; L++) {
        for (uint32_t sym = 0; sym < num; sym++) {
            if (lens[sym] == L) {
                table.entries.push_back({(1u << L) - 1, bit_reverse(code, L), L, int(sym)});
                code++;
            }
        }
        code <<= 1;
    }
    table.build_lut();
}

static uint32_t decode(Bits1& bits, const HuffTable& table) {
    const int16_t hit = table.lut[bits.peek((1u << kLutBits) - 1)];
    if (hit >= 0) {
        bits.skip(hit >> 8);
        const int sym = hit & 0xFF;
        if (sym < 2) return uint32_t(sym);
        return (1u << (sym - 1)) | bits.read(sym - 1);
    }
    for (const auto& e : table.entries) {
        if (bits.peek(e.mask) == e.code) {
            bits.skip(e.len);
            if (e.sym < 2) return uint32_t(e.sym);
            return (1u << (e.sym - 1)) | bits.read(e.sym - 1);
        }
    }
    throw DecodeError{"Huffman decode error"};
}

// Appends n bytes copied from `back` bytes before the end, byte by byte (overlaps intended: a run repeats).
static inline void copy_match(std::vector<uint8_t>& out, size_t back, uint32_t n) {
    const size_t src = out.size() - back;
    for (uint32_t i = 0; i < n; i++) out.push_back(out[src + i]);   // capacity reserved up front: no reallocation
}

static void unpack1(const std::vector<uint8_t>& buf, uint32_t usize, std::vector<uint8_t>& out) {
    Bits1 bits(buf, 0);
    bits.read(2);  // lock / key flags
    HuffTable raw, dist, length;   // an empty table (never read) matches nothing, as before
    raw.build_lut(), dist.build_lut(), length.build_lut();
    while (out.size() < usize) {
        read_table(bits, raw);
        read_table(bits, dist);
        read_table(bits, length);
        uint32_t count = (bits.read(16) - 1) & 0xFFFF;
        while (true) {
            uint32_t n = decode(bits, raw);
            if (n) bits.literals(n, out);
            if (count == 0) break;
            count--;
            uint32_t d = decode(bits, dist) + 1;
            n = decode(bits, length) + 2;
            if (d > out.size()) throw DecodeError{"match before start"};
            copy_match(out, d, n);
        }
    }
}

// ------------------------------------------------------------------ method 2
struct Bits2 {
    const std::vector<uint8_t>& buf;
    size_t p;
    uint32_t bb = 0;
    int n = 0;
    Bits2(const std::vector<uint8_t>& b, size_t pos) : buf(b), p(pos) {}
    uint32_t byte() {
        if (p >= buf.size()) throw DecodeError{"method 2 read past end"};
        return buf[p++];
    }
    uint32_t bit() {
        if (n == 0) {
            bb = byte();
            n = 8;
        }
        n--;
        uint32_t v = (bb >> 7) & 1;
        bb = (bb << 1) & 0xFF;
        return v;
    }
};

static uint32_t offset2(Bits2& b) {
    uint32_t off = 0;
    if (b.bit()) {
        off = b.bit();
        if (b.bit()) {
            off = (off << 1) | b.bit() | 4;
            if (!b.bit()) off = (off << 1) | b.bit();
        } else if (off == 0) {
            off = 2 + b.bit();
        }
    }
    return (off << 8) + b.byte() + 1;
}

static void unpack2(const std::vector<uint8_t>& buf, uint32_t usize, std::vector<uint8_t>& out) {
    Bits2 b(buf, 0);
    b.bit();
    b.bit();  // lock / key flags
    while (true) {  // like the MIPS: stop only at an end-of-block marker + 0 bit
        if (out.size() > size_t(usize) + 0x10000) throw DecodeError{"runaway method-2 stream"};
        if (!b.bit()) {
            out.push_back(uint8_t(b.byte()));
            continue;
        }
        uint32_t n, off;
        if (!b.bit()) {
            n = 4 + b.bit();
            if (b.bit()) {
                n = ((n - 1) << 1) + b.bit();
                if (n == 9) {  // literal run
                    uint32_t a = b.bit(), c = b.bit(), d = b.bit(), e = b.bit();
                    n = (((a << 3) | (c << 2) | (d << 1) | e) + 3) * 4;
                    if (b.p + n > buf.size()) throw DecodeError{"method 2 literal run past end"};
                    out.insert(out.end(), buf.begin() + b.p, buf.begin() + b.p + n);
                    b.p += n;
                    continue;
                }
            }
            off = offset2(b);
        } else if (!b.bit()) {
            n = 2;
            off = b.byte() + 1;
        } else {
            if (!b.bit()) {
                n = 3;
                off = offset2(b);
            } else {
                n = b.byte() + 8;
                if (n == 8) {  // end of block; next bit = more blocks follow
                    if (!b.bit()) break;
                    continue;
                }
                off = offset2(b);
            }
        }
        if (off > out.size()) throw DecodeError{"match before start"};
        copy_match(out, off, n);
    }
}

}  // namespace

bool erz_decompress(const uint8_t* chunk, size_t avail, std::vector<uint8_t>& out, ErzInfo& info, std::string& err, bool check_crc) {
    out.clear();
    if (avail < 18) {
        err = "short header";
        return false;
    }
    if (!((chunk[0] == 'E' && chunk[1] == 'R' && chunk[2] == 'Z') || (chunk[0] == 'R' && chunk[1] == 'N' && chunk[2] == 'C'))) {
        err = "bad magic";
        return false;
    }
    auto be32 = [&](size_t o) { return (uint32_t(chunk[o]) << 24) | (uint32_t(chunk[o + 1]) << 16) | (uint32_t(chunk[o + 2]) << 8) | chunk[o + 3]; };
    auto be16 = [&](size_t o) { return uint16_t((chunk[o] << 8) | chunk[o + 1]); };
    info.method = chunk[3];
    info.usize = be32(4);
    info.psize = be32(8);
    info.ucrc = be16(12);
    info.pcrc = be16(14);
    if (18 + size_t(info.psize) > avail) {
        err = "packed size exceeds input";
        return false;
    }
    std::vector<uint8_t> src(chunk + 18, chunk + 18 + info.psize);
    if (check_crc) info.pcrc_ok = erz_crc16(src.data(), src.size()) == info.pcrc;
    out.reserve(size_t(info.usize) + 0x1000);
    src.resize(src.size() + 8, 0);  // the bit reader's lookahead may run past the end
    try {
        if (info.method == 1) {
            unpack1(src, info.usize, out);
        } else if (info.method == 2) {
            unpack2(src, info.usize, out);
        } else {
            err = "unknown method " + std::to_string(info.method);
            return false;
        }
    } catch (const DecodeError& e) {
        err = e.msg;
        return false;
    }
    if (out.size() < info.usize) {
        err = "short output";
        return false;
    }
    if (check_crc) info.ucrc_ok = erz_crc16(out.data(), info.usize) == info.ucrc;
    return true;
}

}  // namespace thps
