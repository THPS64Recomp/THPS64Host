// Stand-alone driver for overlay_rom.{h,cpp}: the test and release-tooling front end (the host links the module itself).
//
//   overlay_rom_cli build --recipe overlays.txt --rom ROM.z64 --blob main.codeblob.bin --syms syms0.toml --out DIR
//        writes DIR/blob.bin (codeblob + overlays), DIR/syms.toml (syms + overlay sections), DIR/table.inc
//   overlay_rom_cli check --recipe overlays.txt --rom ROM.z64       only the fingerprint check; prints the actual fingerprint
//   overlay_rom_cli xxh3 FILE [BEGIN END]                           XXH3-64 of a file or a byte range (the recipe generator uses it)
// `--rom` is a big-endian image. Exit: 0 ok, 1 usage/IO, 2 the recipe or ROM was refused.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "overlay_rom.h"

using namespace thps::firstrun;

static bool read_all(const std::string& p, std::vector<uint8_t>& v) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    v.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
static bool write_all(const std::string& p, const void* d, size_t n) {
    std::ofstream o(p, std::ios::binary);
    o.write(static_cast<const char*>(d), std::streamsize(n));
    return bool(o);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: see the comment at the top of overlay_rom_cli.cpp\n"); return 1; }
    const std::string cmd = argv[1];
    std::string recipe_p, rom_p, blob_p, syms_p, out_dir;
    std::vector<std::string> pos;
    for (int i = 2; i < argc; i++) {
        auto opt = [&](const char* n, std::string& dst) {
            if (std::strcmp(argv[i], n) != 0 || i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        if (!(opt("--recipe", recipe_p) || opt("--rom", rom_p) || opt("--blob", blob_p) || opt("--syms", syms_p) || opt("--out", out_dir))) pos.push_back(argv[i]);
    }
    if (cmd == "xxh3") {
        std::vector<uint8_t> f;
        if (pos.empty() || !read_all(pos[0], f)) { std::fprintf(stderr, "cannot read file\n"); return 1; }
        size_t b = 0, e = f.size();
        if (pos.size() == 3) { b = std::strtoull(pos[1].c_str(), nullptr, 0); e = std::strtoull(pos[2].c_str(), nullptr, 0); }
        if (b > e || e > f.size()) { std::fprintf(stderr, "bad range\n"); return 1; }
        std::printf("%016llx\n", (unsigned long long)xxh3_64(f.data() + b, e - b));
        return 0;
    }
    std::vector<uint8_t> rom, recipe_text;
    if (!read_all(recipe_p, recipe_text) || !read_all(rom_p, rom)) { std::fprintf(stderr, "cannot read --recipe/--rom\n"); return 1; }
    OverlayRecipe r;
    std::string err;
    if (!parse_overlay_recipe(std::string(recipe_text.begin(), recipe_text.end()), r, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    if (cmd == "check") {
        uint64_t h;
        if (!overlay_rom_hash(r, rom, h, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
        std::printf("%016llx %s\n", (unsigned long long)h, (!r.rom_xxh3 || h == r.rom_xxh3) ? "match" : "MISMATCH");
        return (!r.rom_xxh3 || h == r.rom_xxh3) ? 0 : 2;
    }
    if (cmd == "build") {
        std::vector<uint8_t> blob, syms;
        if (!read_all(blob_p, blob) || !read_all(syms_p, syms) || out_dir.empty()) { std::fprintf(stderr, "build needs --blob, --syms, --out\n"); return 1; }
        OverlayOutput o;
        if (!build_overlays(r, rom, blob, o, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
        const std::string s = append_syms(std::string(syms.begin(), syms.end()), o);
        if (!write_all(out_dir + "/blob.bin", o.blob.data(), o.blob.size()) || !write_all(out_dir + "/syms.toml", s.data(), s.size()) ||
            !write_all(out_dir + "/table.inc", o.table_inc.data(), o.table_inc.size())) {
            std::fprintf(stderr, "cannot write into %s\n", out_dir.c_str());
            return 1;
        }
        std::printf("total: %zu overlays, %zu functions, %zu HI/LO/J relocs; blob 0x%zx bytes\n", r.overlays.size(), o.functions, o.relocs, o.blob.size());
        return 0;
    }
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 1;
}
