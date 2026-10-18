// Controller Pak (memory pak) for the THPS hosts: libultra's osPfs* file API implemented on
// top of a 32 KiB image in the standard .mpk layout (the format mupen64plus/ares/Project64
// use), one file per port in <config>/saves/thps1_pakN.mpk. THPS1 saves only to the Controller
// Pak (no EEPROM), so this is what makes career progress persist.
//
// Approach: high level. The game's calls to osPfsInitPak/FindFile/AllocateFile/ReadWriteFile/...
// are N64Recomp "runtime" names; librecomp answers them with "no pak" (librecomp/src/pak.cpp).
// Defining all ten here means the linker never pulls that object from librecomp.a. The other
// routes (sonicdcer's controller_pak branch, or Army Men's SI-level block
// emulation under the game's own PFS code) need a runtime fork or the PIF/SI layer; this needs
// neither and still writes real .mpk files (page 0 ID area, inode table + backup in pages 1-2,
// 16-entry directory in pages 3-4, data pages 5-127).
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "recomp.h"
#include "host.h"
#include "thps_hooks.h"

namespace {

constexpr int PFS_ERR_NOPACK = 1, PFS_ERR_INVALID = 5, PFS_DATA_FULL = 7, PFS_DIR_FULL = 8, PFS_ERR_EXIST = 9;
constexpr int k_ports = 2;
constexpr size_t k_size = 0x8000, k_page = 0x100, k_first_data_page = 5, k_pages = 128, k_files = 16;
constexpr uint16_t INODE_FREE = 0x0003, INODE_LAST = 0x0001;

struct Pak {
    std::array<uint8_t, k_size> img{};
    bool loaded = false;
    std::filesystem::path path;

    uint16_t inode(size_t i) const { return uint16_t(img[k_page + 2 * i] << 8 | img[k_page + 2 * i + 1]); }
    void set_inode(size_t i, uint16_t v) { img[k_page + 2 * i] = uint8_t(v >> 8); img[k_page + 2 * i + 1] = uint8_t(v); }
    uint8_t* dir(size_t n) { return &img[3 * k_page + 32 * n]; }
    static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    static uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

    void fix_inode_sum() {
        uint32_t sum = 0;
        for (size_t i = k_first_data_page; i < k_pages; i++) sum += img[k_page + 2 * i] + img[k_page + 2 * i + 1];
        img[k_page + 0] = 0;
        img[k_page + 1] = uint8_t(sum);
        std::memcpy(&img[2 * k_page], &img[k_page], k_page);  // backup copy (page 2)
    }

    void format() {
        img.fill(0);
        for (int i = 0; i < 32; i++) img[i] = uint8_t(i);  // label area (as written by emulators)
        img[0] = 0x81;
        // ID block (+3 copies at 0x60, 0x80, 0xC0): repaired, random, serial, device id 1, 1 bank.
        uint8_t id[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0x05, 0x1A, 0x5F, 0x13, 0, 0, 0, 0, 0, 0, 0, 0,
                          0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x01, 0x01, 0x00, 0, 0, 0, 0};
        uint32_t sum = 0;
        for (int i = 0; i < 28; i += 2) sum += uint32_t(id[i] << 8 | id[i + 1]);
        sum &= 0xFFFF;
        uint32_t inv = (0xFFF2 - sum) & 0xFFFF;
        id[28] = uint8_t(sum >> 8), id[29] = uint8_t(sum), id[30] = uint8_t(inv >> 8), id[31] = uint8_t(inv);
        for (size_t off : {0x20, 0x60, 0x80, 0xC0}) std::memcpy(&img[off], id, 32);
        for (size_t i = 0; i < k_pages; i++) set_inode(i, INODE_FREE);
        fix_inode_sum();
    }

    void load(int port) {
        if (loaded) return;
        path = thps::options.save_dir / (std::string(thps::game::family) + "_pak" + std::to_string(port + 1) + ".mpk");
        std::ifstream f(path, std::ios::binary);
        if (f && f.read(reinterpret_cast<char*>(img.data()), k_size) && f.gcount() == std::streamsize(k_size)) {
            std::printf("[thps] pak %d: loaded %s\n", port + 1, path.string().c_str());
        } else {
            format();
            std::printf("[thps] pak %d: new formatted pak %s\n", port + 1, path.string().c_str());
            save();
        }
        loaded = true;
    }

    void save() {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(img.data()), k_size);
            if (!f) { std::fprintf(stderr, "[thps] pak: write %s failed\n", tmp.string().c_str()); return; }
        }
        if (std::filesystem::exists(path)) {
            auto bak = path;
            bak += ".bak";
            std::filesystem::copy_file(path, bak, std::filesystem::copy_options::overwrite_existing, ec);
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) std::fprintf(stderr, "[thps] pak: rename to %s failed: %s\n", path.string().c_str(), ec.message().c_str());
    }

    bool used(size_t n) { const uint8_t* d = dir(n); return be32(d) != 0 || be16(d + 4) != 0; }
    size_t free_pages() const {
        size_t n = 0;
        for (size_t i = k_first_data_page; i < k_pages; i++) n += inode(i) == INODE_FREE;
        return n;
    }
    std::vector<size_t> chain(size_t n) {
        std::vector<size_t> pages;
        size_t p = dir(n)[7];
        while (p >= k_first_data_page && p < k_pages && pages.size() < k_pages) {
            pages.push_back(p);
            uint16_t next = inode(p);
            if (next == INODE_LAST || next == INODE_FREE) break;
            p = next & 0xFF;
        }
        return pages;
    }
};

std::mutex g_mutex;
std::array<Pak, k_ports> g_paks;

bool pak_present(int port) {
    if (thps::options.no_pak || thps::options.rumble_pak || port < 0 || port >= k_ports) return false;
    return port == 0 || thps::input_connected(port);
}

int32_t channel_of(uint8_t* rdram, gpr pfs) { return MEM_W(0x8, pfs); }

Pak* pak_for(uint8_t* rdram, gpr pfs) {
    int ch = channel_of(rdram, pfs);
    if (!pak_present(ch)) return nullptr;
    g_paks[ch].load(ch);
    return &g_paks[ch];
}

gpr stack_arg(uint8_t* rdram, recomp_context* ctx, int n) { return (gpr)(int32_t)MEM_W(0x10 + 4 * (n - 4), ctx->r29); }

void read_name(uint8_t* rdram, gpr p, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = p ? uint8_t(MEM_BU(i, p)) : 0;
}

int find(Pak& pak, uint16_t company, uint32_t game, const uint8_t* name16, const uint8_t* ext4) {
    for (size_t n = 0; n < k_files; n++) {
        const uint8_t* d = pak.dir(n);
        if (Pak::be32(d) == game && Pak::be16(d + 4) == company && !std::memcmp(d + 0x10, name16, 16) && !std::memcmp(d + 0xC, ext4, 4))
            return int(n);
    }
    return -1;
}

void note(const char* fmt, int a = 0, int b = 0, int c = 0) {
    if (thps::verbose()) { std::printf("[thps] pak: "); std::printf(fmt, a, b, c); std::printf("\n"); }
}

}  // namespace

bool thps::pak_present(int port) { return ::pak_present(port); }

extern "C" {

// s32 osPfsInitPak(OSMesgQueue* mq, OSPfs* pfs, int channel)
void osPfsInitPak_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{g_mutex};
    gpr pfs = ctx->r5;
    int ch = int(ctx->r6);
    MEM_W(0x4, pfs) = int32_t(ctx->r4);
    MEM_W(0x8, pfs) = ch;
    if (!::pak_present(ch)) { ctx->r2 = PFS_ERR_NOPACK; return; }
    Pak& pak = g_paks[ch];
    pak.load(ch);
    MEM_W(0x0, pfs) = 1;  // PFS_INITIALIZED
    for (int i = 0; i < 32; i++) MEM_B(0xC + i, pfs) = int8_t(pak.img[0x20 + i]);  // id
    for (int i = 0; i < 32; i++) MEM_B(0x2C + i, pfs) = int8_t(pak.img[i]);        // label
    MEM_W(0x4C, pfs) = 0;           // version
    MEM_W(0x50, pfs) = int32_t(k_files);
    MEM_W(0x54, pfs) = 1 * 8;       // inode_table (in 32-byte blocks)
    MEM_W(0x58, pfs) = 2 * 8;       // minode_table
    MEM_W(0x5C, pfs) = 3 * 8;       // dir_table
    MEM_W(0x60, pfs) = int32_t(k_first_data_page);
    MEM_B(0x64, pfs) = 1;           // banks
    MEM_B(0x65, pfs) = 0;           // activebank
    note("InitPak channel %d", ch);
    ctx->r2 = 0;
}

// s32 osPfsNumFiles(OSPfs* pfs, s32* max_files, s32* files_used)
void osPfsNumFiles_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{g_mutex};
    Pak* pak = pak_for(rdram, ctx->r4);
    int used = 0;
    if (pak) for (size_t n = 0; n < k_files; n++) used += pak->used(n);
    if (ctx->r5) MEM_W(0, ctx->r5) = pak ? int32_t(k_files) : 0;
    if (ctx->r6) MEM_W(0, ctx->r6) = used;
    note("NumFiles -> %d used", used);
    ctx->r2 = pak ? 0 : PFS_ERR_NOPACK;
}

// s32 osPfsFreeBlocks(OSPfs* pfs, s32* bytes_not_used)
void osPfsFreeBlocks_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{g_mutex};
    Pak* pak = pak_for(rdram, ctx->r4);
    if (!pak) { ctx->r2 = PFS_ERR_NOPACK; return; }
    if (ctx->r5) MEM_W(0, ctx->r5) = int32_t(pak->free_pages() * k_page);
    note("FreeBlocks -> %d bytes", int(pak->free_pages() * k_page));
    ctx->r2 = 0;
}

// s32 osPfsFindFile(OSPfs* pfs, u16 company_code, u32 game_code, u8* game_name, u8* ext_name, s32* file_no)
void osPfsFindFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{g_mutex};
    Pak* pak = pak_for(rdram, ctx->r4);
    if (!pak) { ctx->r2 = PFS_ERR_NOPACK; return; }
    uint8_t name[16], ext[4];
    read_name(rdram, ctx->r7, name, 16);
    read_name(rdram, stack_arg(rdram, ctx, 4), ext, 4);
    int n = find(*pak, uint16_t(ctx->r5), uint32_t(ctx->r6), name, ext);
    gpr out = stack_arg(rdram, ctx, 5);
    if (out) MEM_W(0, out) = n;
    note("FindFile -> %d", n);
    ctx->r2 = n >= 0 ? 0 : PFS_ERR_INVALID;
}

// s32 osPfsAllocateFile(OSPfs* pfs, u16 company_code, u32 game_code, u8* game_name, u8* ext_name, int length, s32* file_no)
void osPfsAllocateFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{g_mutex};
    Pak* pak = pak_for(rdram, ctx->r4);
    if (!pak) { ctx->r2 = PFS_ERR_NOPACK; return; }
    uint16_t company = uint16_t(ctx->r5);
    uint32_t game = uint32_t(ctx->r6);
    uint8_t name[16], ext[4];
    read_name(rdram, ctx->r7, name, 16);
    read_name(rdram, stack_arg(rdram, ctx, 4), ext, 4);
    int32_t length = MEM_W(0, ctx->r29 + 0x14);
    gpr out = stack_arg(rdram, ctx, 6);
    if (company == 0 || game == 0 || length <= 0) { ctx->r2 = PFS_ERR_INVALID; return; }
    if (find(*pak, company, game, name, ext) >= 0) { ctx->r2 = PFS_ERR_EXIST; return; }
    size_t need = (size_t(length) + k_page - 1) / k_page;
    int slot = -1;
    for (size_t n = 0; n < k_files && slot < 0; n++) if (!pak->used(n)) slot = int(n);
    if (slot < 0) { ctx->r2 = PFS_DIR_FULL; return; }
    if (pak->free_pages() < need) { ctx->r2 = PFS_DATA_FULL; return; }
    std::vector<size_t> pages;
    for (size_t i = k_first_data_page; i < k_pages && pages.size() < need; i++) if (pak->inode(i) == INODE_FREE) pages.push_back(i);
    for (size_t i = 0; i < pages.size(); i++) pak->set_inode(pages[i], i + 1 < pages.size() ? uint16_t(pages[i + 1]) : INODE_LAST);
    for (size_t p : pages) std::memset(&pak->img[p * k_page], 0, k_page);
    pak->fix_inode_sum();
    uint8_t* d = pak->dir(size_t(slot));
    std::memset(d, 0, 32);
    d[0] = uint8_t(game >> 24), d[1] = uint8_t(game >> 16), d[2] = uint8_t(game >> 8), d[3] = uint8_t(game);
    d[4] = uint8_t(company >> 8), d[5] = uint8_t(company);
    d[6] = 0, d[7] = uint8_t(pages[0]);
    d[8] = 0x02;  // status: in use
    std::memcpy(d + 0xC, ext, 4);
    std::memcpy(d + 0x10, name, 16);
    pak->save();
    if (out) MEM_W(0, out) = slot;
    std::printf("[thps] pak %d: allocated note %d (%zu pages)\n", channel_of(rdram, ctx->r4) + 1, slot, need);
    std::fflush(stdout);
    ctx->r2 = 0;
}

// s32 osPfsDeleteFile(OSPfs* pfs, u16 company_code, u32 game_code, u8* game_name, u8* ext_name)
void osPfsDeleteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{g_mutex};
    Pak* pak = pak_for(rdram, ctx->r4);
    if (!pak) { ctx->r2 = PFS_ERR_NOPACK; return; }
    uint8_t name[16], ext[4];
    read_name(rdram, ctx->r7, name, 16);
    read_name(rdram, stack_arg(rdram, ctx, 4), ext, 4);
    int n = find(*pak, uint16_t(ctx->r5), uint32_t(ctx->r6), name, ext);
    if (n < 0) { ctx->r2 = PFS_ERR_INVALID; return; }
    for (size_t p : pak->chain(size_t(n))) pak->set_inode(p, INODE_FREE);
    pak->fix_inode_sum();
    std::memset(pak->dir(size_t(n)), 0, 32);
    pak->save();
    std::printf("[thps] pak %d: deleted note %d\n", channel_of(rdram, ctx->r4) + 1, n);
    ctx->r2 = 0;
}

// s32 osPfsReadWriteFile(OSPfs* pfs, s32 file_no, u8 flag, int offset, int size_in_bytes, u8* data_buffer)
void osPfsReadWriteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{g_mutex};
    Pak* pak = pak_for(rdram, ctx->r4);
    if (!pak) { ctx->r2 = PFS_ERR_NOPACK; return; }
    int n = int(ctx->r5);
    bool write = (ctx->r6 & 0xFF) == 1;
    int32_t offset = int32_t(ctx->r7);
    int32_t size = MEM_W(0, ctx->r29 + 0x10);
    gpr buf = stack_arg(rdram, ctx, 5);
    note(write ? "ReadWriteFile write note %d offset %d size %d" : "ReadWriteFile read note %d offset %d size %d", n, offset, size);
    if (n < 0 || n >= int(k_files) || !pak->used(size_t(n)) || offset < 0 || size < 0 || (offset & 31) || (size & 31)) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    auto pages = pak->chain(size_t(n));
    if (size_t(offset) + size_t(size) > pages.size() * k_page) { ctx->r2 = PFS_ERR_INVALID; return; }
    for (int32_t i = 0; i < size; i++) {
        size_t pos = size_t(offset + i);
        uint8_t& b = pak->img[pages[pos / k_page] * k_page + pos % k_page];
        if (write) b = uint8_t(MEM_BU(i, buf));
        else MEM_B(i, buf) = int8_t(b);
    }
    if (write) pak->save();
    std::printf("[thps] pak %d: %s %d bytes at %d of note %d\n", channel_of(rdram, ctx->r4) + 1, write ? "wrote" : "read", size, offset, n);
    std::fflush(stdout);
    ctx->r2 = 0;
}

// s32 osPfsFileState(OSPfs* pfs, s32 file_no, OSPfsState* state)
void osPfsFileState_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{g_mutex};
    Pak* pak = pak_for(rdram, ctx->r4);
    if (!pak) { ctx->r2 = PFS_ERR_NOPACK; return; }
    int n = int(ctx->r5);
    note("FileState %d -> %d", n, (n < 0 || n >= int(k_files) || !pak->used(size_t(n))) ? -1 : 0);
    if (n < 0 || n >= int(k_files) || !pak->used(size_t(n))) { ctx->r2 = PFS_ERR_INVALID; return; }
    gpr st = ctx->r6;
    const uint8_t* d = pak->dir(size_t(n));
    MEM_W(0x0, st) = int32_t(pak->chain(size_t(n)).size() * k_page);
    MEM_W(0x4, st) = int32_t(Pak::be32(d));
    MEM_H(0x8, st) = int16_t(Pak::be16(d + 4));
    for (int i = 0; i < 4; i++) MEM_B(0xA + i, st) = int8_t(d[0xC + i]);
    for (int i = 0; i < 16; i++) MEM_B(0xE + i, st) = int8_t(d[0x10 + i]);
    ctx->r2 = 0;
}

// s32 osPfsChecker(OSPfs* pfs), s32 osPfsRepairId(OSPfs* pfs): the image is always consistent.
void osPfsChecker_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{g_mutex};
    ctx->r2 = pak_for(rdram, ctx->r4) ? 0 : PFS_ERR_NOPACK;
}
void osPfsRepairId_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{g_mutex};
    ctx->r2 = pak_for(rdram, ctx->r4) ? 0 : PFS_ERR_NOPACK;
}

// s32 osPfsIsPlug(OSMesgQueue* mq, u8* bitpattern): one bit per port with a pak inserted.
void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint8_t bits = 0;
    for (int p = 0; p < k_ports; p++) if (::pak_present(p)) bits |= uint8_t(1 << p);
    if (ctx->r5) MEM_B(0, ctx->r5) = int8_t(bits);
    note("IsPlug -> 0x%x", bits);
    ctx->r2 = 0;
}

// void osContGetQuery(OSContStatus* data): OSContStatus = {u16 type; u8 status; u8 errno}.
// librecomp's version fills type/status/errno from get_connected_device_info, where "pak" can
// only be a Rumble Pak; a real Controller Pak also sets CONT_CARD_ON (bit 0) in status. The
// pak manager's note list closes itself every frame without it (func_800A2438 in 1.1).
void osContGetQuery_recomp(uint8_t* rdram, recomp_context* ctx);
void thps_cont_get_query(uint8_t* rdram, recomp_context* ctx) {
    gpr data = ctx->r4;
    osContGetQuery_recomp(rdram, ctx);
    for (int p = 0; p < k_ports; p++) {
        if (::pak_present(p) && MEM_BU(4 * p + 3, data) == 0) MEM_B(4 * p + 2, data) = int8_t(MEM_BU(4 * p + 2, data) | 0x01);
    }
}

}  // extern "C"
