// ROM version identification and the active version / code class (src/versions.h).
#include "versions.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "xxHash/xxh3.h"

#include "host.h"
#ifdef THPS_SPLIT_GAME_LIB
#include "game_split.h"
#endif

namespace thps {
namespace {
std::vector<const Payload*>& registry() {
    static std::vector<const Payload*> r;   // filled by static registrars: construct on first use
    return r;
}
const VersionInfo* g_active = nullptr;
const SymTable* g_sym = nullptr;
}  // namespace

PayloadRegistrar::PayloadRegistrar(const Payload& p) { registry().push_back(&p); }
void register_payload(const Payload& p) { registry().push_back(&p); }

const Payload* payload_for(std::string_view code_class) {
    for (const Payload* p : registry()) {
        if (code_class == p->code_class) return p;
    }
    return nullptr;
}

const VersionInfo& version() { return *g_active; }
const SymTable& sym() { return *g_sym; }

namespace versions {
int tv_override = -1;

namespace {
// Byte order of an N64 ROM from its first word: z64 80 37 12 40, v64 37 80 40 12, n64 40 12 37 80.
// Returns the index xor that maps a byte position to its z64 position, or -1.
int byte_order_xor(const uint8_t* h) {
    if (h[0] == 0x80 && h[1] == 0x37) return 0;
    if (h[0] == 0x37 && h[1] == 0x80) return 1;
    if (h[0] == 0x40 && h[1] == 0x12) return 3;
    return -1;
}

std::string trimmed(const char* p, size_t n) {
    std::string s(p, n);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}
}  // namespace

// SHA-1 (FIPS 180-4), only to show a user the hash that ROM databases list for their file.
static void sha1_hex(const uint8_t* data, size_t len, char out[41]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    };
    size_t full = len / 64;
    for (size_t i = 0; i < full; i++) block(data + 64 * i);
    uint8_t tail[128] = {};
    size_t rest = len - full * 64;
    std::memcpy(tail, data + full * 64, rest);
    tail[rest] = 0x80;
    size_t tl = rest + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = uint64_t(len) * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = uint8_t(bits >> (8 * i));
    for (size_t i = 0; i < tl; i += 64) block(tail + i);
    for (int i = 0; i < 5; i++) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
}

Identity identify(const std::filesystem::path& path) {
    Identity id;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return id;
    // N64 cartridges are at most 64 MiB: anything bigger is not one (and is not read into memory).
    const uintmax_t size = std::filesystem::file_size(path, ec);
    std::ifstream in(path, std::ios::binary);
    if (ec || !in) return id;
    if (size > (80u << 20)) { id.read_ok = true; return id; }
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad() || raw.size() != size) return id;
    id.read_ok = true;
    if (raw.size() < 0x40) return id;
    int x = byte_order_xor(raw.data());
    if (x < 0) return id;
    id.is_rom = true;
    raw.resize((raw.size() + 3) & ~size_t(3));
    if (x != 0) {
        for (size_t i = 0; i < raw.size(); i += 4) {
            uint8_t t[4] = {raw[i], raw[i + 1], raw[i + 2], raw[i + 3]};
            for (int k = 0; k < 4; k++) raw[i + (k ^ x)] = t[k];
        }
    }
    id.xxh3 = XXH3_64bits(raw.data(), raw.size());
    sha1_hex(raw.data(), raw.size(), id.sha1);
    id.internal_name = trimmed(reinterpret_cast<const char*>(raw.data() + 0x20), 20);
    std::memcpy(id.game_code, raw.data() + 0x3B, 4);
    id.revision = raw[0x3F];
    id.looks_like_game = id.internal_name.rfind(game::header_prefix, 0) == 0;
    for (const VersionInfo& v : thps::all_versions) {
        if (v.xxh3 == id.xxh3) id.match = &v;
        if (!id.by_header && id.internal_name == v.internal_name && std::memcmp(id.game_code, v.game_code, 4) == 0 && id.revision == v.revision) id.by_header = &v;
    }
    return id;
}

const VersionInfo* find(std::string_view s) {
    for (const VersionInfo& v : thps::all_versions) {
        if (s == v.id || s == v.game_id) return &v;
    }
    return nullptr;
}

#ifdef THPS_SPLIT_GAME_LIB
// Split build: runnable = the app holds a recipe for this very version (or already loaded its code class's library).
bool runnable(const VersionInfo& v) { return v.status == Status::Supported && game::version_has_recipe(v); }
#else
bool runnable(const VersionInfo& v) { return v.status == Status::Supported && payload_for(v.code_class) != nullptr; }
#endif

const char* region_name(const VersionInfo& v) {
    switch (v.game_code[3]) {
        case 'E': return "USA";
        case 'P': return "Europe";
        case 'D': return "Germany";
        case 'F': return "France";
        case 'J': return "Japan";
        default: return "unknown region";
    }
}

std::string describe(const VersionInfo& v) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s (SHA-1 %s, header \"%s\", game code %s, revision %u)", v.display_name, v.sha1, v.internal_name, v.game_code,
                  unsigned(v.revision));
    return buf;
}

std::string supported_list() {
    std::string s;
    for (const VersionInfo& v : thps::all_versions) {
        if (!runnable(v)) continue;
        s += std::string("\n  ") + v.display_name + ", SHA-1 " + v.sha1;
    }
    return s;
}

void activate(const VersionInfo& v) {
    const Payload* p = payload_for(v.code_class);
    if (!p) {
        std::fprintf(stderr, "[thps] no payload for code class %s linked into this build (tools/gen.py N64Recomp %s)\n", v.code_class, v.code_class);
        std::exit(2);
    }
    const SymTable* sym = nullptr;
    for (const CodeClass& c : code_classes) {
        if (std::strcmp(c.name, v.code_class) == 0) sym = c.sym;
    }
    if (!sym) {
        std::fprintf(stderr, "[thps] no symbol table for code class %s\n", v.code_class);
        std::exit(2);
    }
    if (g_active && g_active != &v && std::strcmp(g_active->code_class, v.code_class) != 0) {
        std::fprintf(stderr, "[thps] activate(%s): code class %s is already active; one class per process\n", v.id, g_active->code_class);
        std::exit(2);
    }
    if (!g_active) {
        for (size_t i = 0; i < p->num_sections; i++) {
            const SectionTableEntry& s = p->sections[i];
            for (size_t f = 0; f < s.num_funcs; f++) {
                thps::crash_add_guest_function(reinterpret_cast<const void*>(s.funcs[f].func), s.ram_addr + s.funcs[f].offset);
            }
        }
        // Also when there is no JIT range ({null, null}): it sorts the table. crash_install() ran earlier (main.cpp) and sorted an
        // empty one; unsorted, the crash log's binary search misses game frames (seen with us10, whose registration order is not
        // the link order; us11 was sorted by luck).
        thps::crash_set_jit_range(p->jit_begin, p->jit_end);
        // total_num_sections too (as THPS2/3 and Zelda64Recomp do): librecomp's init_overlays callocs section_addresses with it and
        // writes one int32 per code section. Left 0, that was calloc(0) plus 16 bytes of writes: harmless in macOS's 16-byte
        // minimum block, a heap-header overwrite (0xC0000374 at boot, ~50% of runs) on Windows.
        recomp::overlays::register_overlays({p->sections, p->num_sections, p->num_sections}, {p->overlay_sections_by_index, p->num_overlay_sections});
        g_sym = sym;
    }
    g_active = &v;
}

const VersionInfo* active() { return g_active; }

int tv_type() { return tv_override >= 0 ? tv_override : g_active->tv_default; }

}  // namespace versions
}  // namespace thps
