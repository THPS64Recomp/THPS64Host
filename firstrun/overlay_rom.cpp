// See overlay_rom.h. The algorithm is decomp/common/overlay_recomp.py's, minus function discovery (that arrives in the recipe).
#include "overlay_rom.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "xxHash/xxh3.h"

namespace thps::firstrun {
namespace {

constexpr uint32_t kStreamEnd = 0xFFFFFFFFu;

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

bool plain_name(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!(c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    return true;
}

struct Reloc {
    uint32_t type, site, addend;   // type: 0 R_32, 1 HI16, 2 LO16, 3 J26 (the game's own Overlay_Reloc stream format)
};

// Stream of big-endian words: 0xFFFFFFFF ends; w & 3 = type, w & ~3 = byte offset of the site in the blob; HI16/LO16 are followed by
// their addend word. Must end exactly at `n` bytes (every stream in the known dumps does) and lie inside the blob.
bool decode_stream(const uint8_t* p, size_t n, uint32_t blob_size, std::vector<Reloc>& out, std::string& error) {
    size_t i = 0;
    for (;;) {
        if (i + 4 > n) { error = "relocation stream runs past its end"; return false; }
        uint32_t w = be32(p + i);
        i += 4;
        if (w == kStreamEnd) break;
        Reloc r{w & 3, w & ~3u, 0};
        if (r.type == 1 || r.type == 2) {
            if (i + 4 > n) { error = "relocation stream runs past its end"; return false; }
            r.addend = be32(p + i);
            i += 4;
        }
        if (uint64_t(r.site) + 4 > blob_size) { error = fmt("relocation site 0x%X lies outside the blob", r.site); return false; }
        out.push_back(r);
    }
    if (i != n) { error = fmt("relocation stream ends at 0x%zX, not at its recorded size 0x%zX", i, n); return false; }
    return true;
}

}  // namespace

uint64_t xxh3_64(const uint8_t* data, size_t n) { return XXH3_64bits(data, n); }

// ------------------------------------------------------------------------------------------------ recipe
// Lines (blank lines and '#' comments ignored):
//   vbase 0x80C00000 | stride 0x80000 | rom_xxh3 0x<16 hex>
//   o <name> <code_rom> <size> <reloc_rom> <reloc_size>      starts an overlay (numbers: any base accepted by strtoul)
//   f <offset> <size> [<name>]                                a function of the current overlay
bool parse_overlay_recipe(const std::string& text, OverlayRecipe& out, std::string& error) {
    out = OverlayRecipe{};
    std::istringstream ss(text);
    int lineno = 0;
    auto bad = [&](const char* why) {
        error = fmt("overlay recipe line %d: %s", lineno, why);
        return false;
    };
    auto num = [](const std::string& s, uint64_t& v) {
        char* end = nullptr;
        v = std::strtoull(s.c_str(), &end, 0);
        return !s.empty() && end && *end == 0;
    };
    for (std::string line; std::getline(ss, line);) {
        lineno++;
        std::istringstream ls(line);
        std::string cmd;
        if (!(ls >> cmd) || cmd[0] == '#') continue;
        std::vector<std::string> a;
        for (std::string t; ls >> t;) a.push_back(t);
        uint64_t v[4];
        if (cmd == "vbase" || cmd == "stride" || cmd == "rom_xxh3") {
            if (a.size() != 1 || !num(a[0], v[0])) return bad("expected one number");
            if (cmd == "rom_xxh3") out.rom_xxh3 = v[0];
            else if (v[0] > 0xFFFFFFFFull) return bad("number out of range");
            else (cmd == "vbase" ? out.vbase : out.stride) = uint32_t(v[0]);
        } else if (cmd == "o") {
            if (a.size() != 5 || !plain_name(a[0])) return bad("expected: o <name> <code_rom> <size> <reloc_rom> <reloc_size>");
            for (int k = 0; k < 4; k++)
                if (!num(a[1 + k], v[k]) || v[k] > 0xFFFFFFFFull) return bad("bad number");
            OverlayDef d;
            d.name = a[0];
            d.code_rom = uint32_t(v[0]);
            d.size = uint32_t(v[1]);
            d.reloc_rom = uint32_t(v[2]);
            d.reloc_size = uint32_t(v[3]);
            out.overlays.push_back(std::move(d));
        } else if (cmd == "f") {
            if (out.overlays.empty()) return bad("function before any overlay");
            if ((a.size() != 2 && a.size() != 3) || !num(a[0], v[0]) || !num(a[1], v[1]) || v[0] > 0xFFFFFFFFull || v[1] > 0xFFFFFFFFull)
                return bad("expected: f <offset> <size> [<name>]");
            if (a.size() == 3 && !plain_name(a[2])) return bad("bad function name");
            out.overlays.back().funcs.push_back({uint32_t(v[0]), uint32_t(v[1]), a.size() == 3 ? a[2] : std::string()});
        } else {
            return bad("unknown directive");
        }
    }
    if (out.overlays.empty()) { error = "overlay recipe has no overlays"; return false; }
    for (const OverlayDef& d : out.overlays) {
        uint32_t prev_end = 0;
        for (const OverlayFunc& f : d.funcs) {
            if (f.size == 0 || f.off % 4 || f.off < prev_end || uint64_t(f.off) + f.size > d.size) {
                error = "overlay recipe: function list of " + d.name + " is not sorted, aligned and inside the blob";
                return false;
            }
            prev_end = f.off + f.size;
        }
    }
    return true;
}

bool overlay_rom_range(const OverlayRecipe& r, uint32_t& begin, uint32_t& end) {
    if (r.overlays.empty()) return false;
    uint64_t lo = ~0ull, hi = 0;
    for (const OverlayDef& d : r.overlays) {
        lo = std::min<uint64_t>({lo, d.code_rom, d.reloc_rom});
        hi = std::max<uint64_t>({hi, uint64_t(d.code_rom) + d.size, uint64_t(d.reloc_rom) + d.reloc_size});
    }
    if (hi > 0xFFFFFFFFull) return false;
    begin = uint32_t(lo);
    end = uint32_t(hi);
    return true;
}

bool overlay_rom_hash(const OverlayRecipe& r, const std::vector<uint8_t>& rom, uint64_t& hash, std::string& error) {
    uint32_t b, e;
    if (!overlay_rom_range(r, b, e) || e > rom.size()) { error = "the ROM file is too short for the front-end screens"; return false; }
    hash = XXH3_64bits(rom.data() + b, e - b);
    return true;
}

// ------------------------------------------------------------------------------------------------ build
bool build_overlays(const OverlayRecipe& r, const std::vector<uint8_t>& rom, const std::vector<uint8_t>& main_blob, OverlayOutput& out,
                    std::string& error) {
    out = OverlayOutput{};
    uint64_t h;
    if (!overlay_rom_hash(r, rom, h, error)) return false;
    if (r.rom_xxh3 && h != r.rom_xxh3) {
        error = fmt("the front-end screens in this ROM are not the ones this release was prepared for (fingerprint %016llx, expected %016llx). "
                    "The ROM is probably a modified dump.", (unsigned long long)h, (unsigned long long)r.rom_xxh3);
        return false;
    }
    static const char* const kRelocNames[4] = {nullptr, "R_MIPS_HI16", "R_MIPS_LO16", "R_MIPS_26"};

    std::vector<uint8_t>& blob = out.blob;
    blob = main_blob;
    blob.resize((blob.size() + 15) & ~size_t(15), 0);
    std::string& syms = out.syms_sections;

    for (size_t k = 0; k < r.overlays.size(); k++) {
        const OverlayDef& d = r.overlays[k];
        const uint64_t V64 = uint64_t(r.vbase) + k * uint64_t(r.stride);
        if ((V64 >> 28) != 8 || V64 + d.size > 0xFFFFFFFFull) { error = "overlay " + d.name + ": fake base is not a 0x8xxxxxxx address"; return false; }
        const uint32_t V = uint32_t(V64);
        if (uint64_t(d.code_rom) + d.size > rom.size() || uint64_t(d.reloc_rom) + d.reloc_size > rom.size()) { error = "overlay " + d.name + " lies outside the ROM"; return false; }

        std::vector<Reloc> relocs;
        std::string err;
        if (!decode_stream(&rom[d.reloc_rom], d.reloc_size, d.size, relocs, err)) { error = "overlay " + d.name + ": " + err; return false; }

        const std::vector<uint8_t> raw(rom.begin() + d.code_rom, rom.begin() + d.code_rom + d.size);
        std::vector<uint8_t> text = raw;
        // Pre-link at V: the link-time copy N64Recomp decodes (jump tables, jal targets, lui/addiu pairs). Same arithmetic as the
        // game's Overlay_Reloc for load address V; Python integers there, 64-bit here, masked the same way.
        for (const Reloc& rl : relocs) {
            uint64_t wd = be32(&text[rl.site]);
            switch (rl.type) {
                case 0: wd = (wd + V) & 0xFFFFFFFFull; break;
                case 1: wd = (wd & 0xFFFF0000ull) | (((uint64_t(V) + rl.addend + 0x8000) >> 16) & 0xFFFF); break;
                case 2: wd = (wd & 0xFFFF0000ull) | ((uint64_t(V) + rl.addend) & 0xFFFF); break;
                default: wd = (wd & 0xFC000000ull) | ((wd + ((V & 0x0FFFFFFFull) >> 2)) & 0x03FFFFFF); break;
            }
            put_be32(&text[rl.site], uint32_t(wd));
        }
        const std::string& nm = d.name;
        const std::string short_name = (nm.rfind("t2_", 0) == 0 || nm.rfind("t3_", 0) == 0) ? nm.substr(3) : nm;
        const size_t blob_off = blob.size();
        blob.insert(blob.end(), text.begin(), text.end());
        blob.resize((blob.size() + 15) & ~size_t(15), 0);

        syms += fmt("\n[[section]]\nname = \".ovl_%s\"\nrom = 0x%zX\nvram = 0x%08X\nsize = 0x%X\n", short_name.c_str(), blob_off, V, d.size);
        syms += "relocs = [\n";
        std::stable_sort(relocs.begin(), relocs.end(), [](const Reloc& a, const Reloc& b) { return a.site < b.site; });
        size_t nrel = 0;
        for (const Reloc& rl : relocs) {
            if (rl.type == 0) continue;   // R_32 words are data: the game's loader relocates them in RDRAM, the recompiled code reads them there
            nrel++;
            const uint64_t tv = uint64_t(V) + (rl.type == 3 ? uint64_t(be32(&raw[rl.site]) & 0x03FFFFFFu) << 2 : uint64_t(rl.addend));
            syms += fmt("    { type = \"%s\", vram = 0x%08X, target_vram = 0x%08llX },\n", kRelocNames[rl.type], V + rl.site, (unsigned long long)tv);
        }
        syms += "]\nfunctions = [\n";
        for (const OverlayFunc& f : d.funcs) {
            const std::string fname = f.name.empty() ? fmt("ovl_%s_%05X", short_name.c_str(), f.off) : f.name;
            syms += fmt("    { name = \"%s\", vram = 0x%08X, size = 0x%X },\n", fname.c_str(), V + f.off, f.size);
        }
        syms += "]\n";
        out.table_inc += fmt("{ \"%s\", 0x%X, 0x%X, 0x%zX, 0x%08X },\n", nm.c_str(), d.code_rom, d.size, blob_off, V);
        out.functions += d.funcs.size();
        out.relocs += nrel;
    }
    return true;
}

std::string append_syms(const std::string& base_syms, const OverlayOutput& o) {
    std::string s = base_syms;
    while (!s.empty() && s.back() == '\n') s.pop_back();
    s += "\n";
    return s + o.syms_sections;
}

}  // namespace thps::firstrun
