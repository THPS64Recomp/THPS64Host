// Display-list inspector for the THPS hosts (runs on the gfx thread, before RT64).
//
// Why: `screencapture` can't capture the RT64 window here (no Screen Recording permission),
// so this gives independent evidence of what the game draws:
//   * per-task F3DEX2 command stats (vertices, triangles, texture/fill rectangles, texture
//     loads, color image) in the log;
//   * optionally, for chosen gfx tasks, PNGs of every texture the list samples (decoded from
//     RDRAM using the SETTIMG/SETTILE/LOAD* state) and a software composite of the 2D part
//     of the frame (fill + texture rectangles, nearest sampling; triangles are not drawn).
// Dumps are ROM-derived: they go to --dump-dir (default ignored by git) and stay local.
#include <algorithm>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <string>
#include <vector>

#include "miniz.h"
#include "host.h"
#include "platform.h"

namespace {

struct Rdram {
    const uint8_t* m;
    uint32_t w(uint32_t p) const { return *reinterpret_cast<const uint32_t*>(m + (p & 0x3FFFFFC)); }
    uint8_t b(uint32_t p) const { return m[(p & 0x3FFFFFF) ^ 3]; }
    uint16_t h(uint32_t p) const { return *reinterpret_cast<const uint16_t*>(m + ((p & 0x3FFFFFE) ^ 2)); }
};

struct TileDesc {
    uint32_t fmt = 0, siz = 0, line = 0, tmem = 0, palette = 0;
    uint32_t uls = 0, ult = 0, lrs = 0, lrt = 0;  // 10.2
};

// What a LOAD* put into TMEM at a given address (we sample from RDRAM, not TMEM).
struct TmemSrc {
    uint32_t addr = 0, fmt = 0, siz = 0, img_width = 0;  // SETTIMG state at load time
    uint32_t uls = 0, ult = 0;                           // texel origin (LOADTILE)
    bool block = false;
    uint32_t texels = 0, dxt = 0;
};

struct TexUse {
    uint32_t addr, fmt, siz, w, h, row_texels, pal_addr;
    bool odd_swap;
    bool operator<(const TexUse& o) const {
        return std::tie(addr, fmt, siz, w, h, pal_addr, odd_swap) < std::tie(o.addr, o.fmt, o.siz, o.w, o.h, o.pal_addr, o.odd_swap);
    }
};

const char* fmt_name(uint32_t f) {
    static const char* n[] = {"RGBA", "YUV", "CI", "IA", "I", "?5", "?6", "?7"};
    return n[f & 7];
}
const int siz_bits[4] = {4, 8, 16, 32};

uint32_t rgba5551(uint16_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31, a = c & 1;
    return (r * 255 / 31) | ((g * 255 / 31) << 8) | ((b * 255 / 31) << 16) | ((a ? 255u : 0u) << 24);
}

// Decode one texel at texel index `i` of an image at `addr` (RDRAM, physical).
// `swap` = 4 for odd rows of a LOADBLOCK with dxt == 0: the game pre-interleaved those rows
// in RDRAM (TMEM stores odd rows with their 32-bit words swapped), so undo it here.
uint32_t texel(const Rdram& r, uint32_t addr, uint32_t fmt, uint32_t siz, uint32_t i, uint32_t pal, bool tlut_ia, uint32_t swap = 0) {
    switch (siz) {
        case 0: {  // 4-bit
            uint8_t byte = r.b((addr + i / 2) ^ swap);
            uint32_t v = (i & 1) ? (byte & 0xF) : (byte >> 4);
            if (fmt == 2) {  // CI4
                uint16_t c = r.h(pal + v * 2);
                if (tlut_ia) { uint32_t I = c >> 8, A = c & 0xFF; return I | (I << 8) | (I << 16) | (A << 24); }
                return rgba5551(c);
            }
            if (fmt == 3) {  // IA4
                uint32_t I = (v >> 1) * 255 / 7, A = (v & 1) ? 255 : 0;
                return I | (I << 8) | (I << 16) | (A << 24);
            }
            uint32_t I = v * 17;  // I4
            return I | (I << 8) | (I << 16) | (I << 24);
        }
        case 1: {  // 8-bit
            uint8_t v = r.b((addr + i) ^ swap);
            if (fmt == 2) {
                uint16_t c = r.h(pal + v * 2);
                if (tlut_ia) { uint32_t I = c >> 8, A = c & 0xFF; return I | (I << 8) | (I << 16) | (A << 24); }
                return rgba5551(c);
            }
            if (fmt == 3) {
                uint32_t I = (v >> 4) * 17, A = (v & 0xF) * 17;
                return I | (I << 8) | (I << 16) | (A << 24);
            }
            return v | (v << 8) | (v << 16) | (uint32_t(v) << 24);
        }
        case 2: {  // 16-bit
            uint16_t c = r.h((addr + i * 2) ^ swap);
            if (fmt == 3) { uint32_t I = c >> 8, A = c & 0xFF; return I | (I << 8) | (I << 16) | (A << 24); }
            return rgba5551(c);
        }
        default: {  // 32-bit RGBA
            uint32_t c = r.w(addr + i * 4);
            return (c >> 24) | (((c >> 16) & 0xFF) << 8) | (((c >> 8) & 0xFF) << 16) | ((c & 0xFF) << 24);
        }
    }
}

void write_png(const std::string& path, const std::vector<uint32_t>& px, int w, int h) {
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(px.data(), w, h, 4, &len, 6, MZ_FALSE);
    if (!png) return;
    if (FILE* f = thps::platform::fopen_utf8(path, "wb")) {
        std::fwrite(png, 1, len, f);
        std::fclose(f);
    }
    mz_free(png);
}

// `ctl`: the dump was asked for by --control "dump N DIR" (into thps::ctl_dump_dir, written before dump_next_tasks was set).
bool want_dump(uint32_t task, bool& ctl) {
    const auto& o = thps::options;
    ctl = false;
    if (thps::dump_next_tasks.load() > 0 && thps::dump_next_tasks.fetch_sub(1) > 0) return ctl = true;
    if (o.dump_dir.empty()) return false;
    if (o.dump_every > 0 && task % uint32_t(o.dump_every) == 0) return true;
    std::stringstream ss(o.dump_frames);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty() && uint32_t(std::stoul(item)) == task) return true;
    }
    return false;
}


// ---- minimal F3DEX2 geometry for the software composite (dumped tasks only) ----
struct Mtx4 {
    float m[4][4];
};
Mtx4 mul(const Mtx4& a, const Mtx4& b) {
    Mtx4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += a.m[i][k] * b.m[k][j];
            r.m[i][j] = v;
        }
    return r;
}
Mtx4 identity() {
    Mtx4 r{};
    for (int i = 0; i < 4; i++) r.m[i][i] = 1;
    return r;
}
// N64 Mtx: 16 s16 integer parts then 16 u16 fractions, row-major.
Mtx4 read_mtx(const Rdram& r, uint32_t a) {
    Mtx4 m{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int16_t ip = int16_t(r.h(a + (i * 4 + j) * 2));
            uint16_t fp = r.h(a + 32 + (i * 4 + j) * 2);
            m.m[i][j] = float(ip) + float(fp) / 65536.0f;
        }
    return m;
}
struct Vtx {
    float x, y, z, w;  // clip space
    float sx, sy, sz;  // screen
    float s, t;        // texels (before tile offset)
    float r, g, b;
    bool ok;
};
}  // namespace

void thps::inspect_display_list(uint8_t* rdram, uint32_t task, uint32_t ucode, uint32_t data_ptr, uint32_t data_size) {
    const auto& o = thps::options;
    bool log = int(task) < o.dl_log || (o.dl_log_every > 0 && task % uint32_t(o.dl_log_every) == 0);
    bool ctl = o.control && thps::control_want_dl();
    bool dump_via_ctl = false;
    bool dump = want_dump(task, dump_via_ctl);
    bool trace = (o.dl_trace >= 0 && int(task) >= o.dl_trace && int(task) < o.dl_trace + o.dl_trace_n) || (dump && o.dl_trace == -2);   // -2: --control dump traces what it dumps
    if (!log && !dump && !trace && !ctl) return;
    const std::filesystem::path dump_dir = dump_via_ctl ? thps::ctl_dump_dir : o.dump_dir;

    Rdram r{rdram};
    uint32_t seg[16] = {0};
    auto resolve = [&](uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0xFFFFFF; };

    // State
    TileDesc tiles[8];
    std::map<uint32_t, TmemSrc> tmem;  // tmem word address -> source
    uint32_t timg_addr = 0, timg_fmt = 0, timg_siz = 0, timg_w = 0;
    uint32_t cimg_addr = 0, cimg_w = 0, cimg_siz = 0;
    uint32_t omh = 0, oml = 0, fill_color = 0;
    uint32_t pal_addr = 0;  // RDRAM address of the last LOADTLUT source

    // Stats
    uint64_t cmds = 0, vtx = 0, tris = 0, texrects = 0, fillrects = 0, loads = 0, mtx = 0, dls = 0;
    std::set<uint32_t> timgs;
    std::set<TexUse> uses;
    bool bad = false;

    // Software composite of the 2D part (only when dumping).
    int cw = 0, ch = 240;
    std::vector<uint32_t> canvas;
    auto tex_use_for_tile = [&](uint32_t t, TexUse& u) -> bool {
        const TileDesc& td = tiles[t & 7];
        auto it = tmem.find(td.tmem);
        if (it == tmem.end()) return false;
        const TmemSrc& s = it->second;
        u.fmt = td.fmt;
        u.siz = td.siz;
        u.w = ((td.lrs - td.uls) >> 2) + 1;
        u.h = ((td.lrt - td.ult) >> 2) + 1;
        if (s.block) {
            // LOADBLOCK: rows are `line` 64-bit words long in TMEM.
            uint32_t bits = siz_bits[td.siz & 3];
            u.row_texels = td.line ? td.line * 64 / bits : u.w;
            u.addr = s.addr;
            u.odd_swap = (s.dxt == 0 && u.h > 1 && td.siz != 3);
        } else {
            // SETTIMG width is in units of the *load* size (4-bit textures are loaded as 8/16-bit).
            u.row_texels = s.img_width * siz_bits[s.siz & 3] / siz_bits[td.siz & 3];
            uint32_t bits = siz_bits[s.siz & 3];
            u.addr = s.addr + ((s.ult >> 2) * s.img_width + (s.uls >> 2)) * bits / 8;
        }
        u.pal_addr = (td.fmt == 2) ? pal_addr + (td.siz == 0 ? td.palette * 32 : 0) : 0;
        if (u.w > 1024 || u.h > 1024 || u.w == 0 || u.h == 0) return false;
        return true;
    };
    std::vector<float> zbuf;
    Mtx4 proj = identity(), mv = identity();
    std::vector<Mtx4> mv_stack;
    Vtx vbuf[64] = {};
    float vp_scale[2] = {160, 120}, vp_trans[2] = {160, 120};
    uint32_t geom = 0, tex_on = 0, tex_tile = 0;
    float tex_sscale = 1, tex_tscale = 1;
    uint64_t tris_drawn = 0;

    auto draw_tri = [&](const Vtx& a, const Vtx& b, const Vtx& c) {
        if (canvas.empty() || !a.ok || !b.ok || !c.ok) return;
        float area = (b.sx - a.sx) * (c.sy - a.sy) - (b.sy - a.sy) * (c.sx - a.sx);
        if (area == 0) return;
        if (zbuf.size() != canvas.size()) zbuf.assign(canvas.size(), 1e30f);
        TexUse u{};
        bool textured = tex_on && tex_use_for_tile(tex_tile, u);
        bool tlut_ia = ((omh >> 14) & 3) == 3;
        const TileDesc& td = tiles[tex_tile & 7];
        int minx = std::max(0, int(std::floor(std::min({a.sx, b.sx, c.sx}))));
        int maxx = std::min(cw - 1, int(std::ceil(std::max({a.sx, b.sx, c.sx}))));
        int miny = std::max(0, int(std::floor(std::min({a.sy, b.sy, c.sy}))));
        int maxy = std::min(ch - 1, int(std::ceil(std::max({a.sy, b.sy, c.sy}))));
        if (maxx - minx > 4 * cw || maxy - miny > 4 * ch) return;
        tris_drawn++;
        for (int y = miny; y <= maxy; y++) {
            for (int x = minx; x <= maxx; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = ((b.sx - px) * (c.sy - py) - (b.sy - py) * (c.sx - px)) / area;
                float w1 = ((c.sx - px) * (a.sy - py) - (c.sy - py) * (a.sx - px)) / area;
                float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0 * a.sz + w1 * b.sz + w2 * c.sz;
                size_t idx = size_t(y) * cw + x;
                if ((geom & 1) && z > zbuf[idx]) continue;  // G_ZBUFFER
                float rr = w0 * a.r + w1 * b.r + w2 * c.r, gg = w0 * a.g + w1 * b.g + w2 * c.g, bb = w0 * a.b + w1 * b.b + w2 * c.b;
                uint32_t col;
                if (textured) {
                    // perspective-correct s,t
                    float iw = w0 / a.w + w1 / b.w + w2 / c.w;
                    float ss = (w0 * a.s / a.w + w1 * b.s / b.w + w2 * c.s / c.w) / iw - float(td.uls >> 2);
                    float tt = (w0 * a.t / a.w + w1 * b.t / b.w + w2 * c.t / c.w) / iw - float(td.ult >> 2);
                    int si = int(std::floor(ss)) % int(u.w), ti = int(std::floor(tt)) % int(u.h);
                    if (si < 0) si += u.w;
                    if (ti < 0) ti += u.h;
                    uint32_t tc = texel(r, u.addr, u.fmt, u.siz, uint32_t(ti) * u.row_texels + uint32_t(si), u.pal_addr, tlut_ia,
                                        (u.odd_swap && (ti & 1)) ? 4u : 0u);
                    if ((tc >> 24) < 32) continue;  // alpha-tested foliage/decals
                    auto ch8 = [&](int sh, float m) { return uint32_t(std::min(255.0f, ((tc >> sh) & 0xFF) * m)) << sh; };
                    col = ch8(0, rr) | ch8(8, gg) | ch8(16, bb);
                } else {
                    col = uint32_t(std::min(255.0f, rr * 255)) | (uint32_t(std::min(255.0f, gg * 255)) << 8) | (uint32_t(std::min(255.0f, bb * 255)) << 16);
                }
                canvas[idx] = col | 0xFF000000;
                zbuf[idx] = z;
            }
        }
    };


    std::vector<uint32_t> stack;
    uint32_t pc = data_ptr & 0xFFFFFF;
    const uint64_t max_cmds = 200000;
    while (cmds < max_cmds) {
        uint32_t w0 = r.w(pc), w1 = r.w(pc + 4);
        if (trace) std::printf("[trace] %06X: %08X %08X\n", pc, w0, w1);
        pc += 8;
        cmds++;
        uint8_t op = w0 >> 24;
        switch (op) {
            case 0x01: {
                uint32_t n = (w0 >> 12) & 0xFF;
                vtx += n;
                if (canvas.empty()) break;
                int v0 = int((w0 >> 1) & 0x7F) - int(n);
                uint32_t a = resolve(w1);
                Mtx4 mvp = mul(mv, proj);
                bool lit = geom & 0x00020000;
                for (uint32_t k = 0; k < n && v0 + int(k) < 64 && v0 + int(k) >= 0; k++) {
                    uint32_t va = a + k * 16;
                    float x = int16_t(r.h(va)), y = int16_t(r.h(va + 2)), z = int16_t(r.h(va + 4));
                    Vtx& v = vbuf[v0 + k];
                    v.x = x * mvp.m[0][0] + y * mvp.m[1][0] + z * mvp.m[2][0] + mvp.m[3][0];
                    v.y = x * mvp.m[0][1] + y * mvp.m[1][1] + z * mvp.m[2][1] + mvp.m[3][1];
                    v.z = x * mvp.m[0][2] + y * mvp.m[1][2] + z * mvp.m[2][2] + mvp.m[3][2];
                    v.w = x * mvp.m[0][3] + y * mvp.m[1][3] + z * mvp.m[2][3] + mvp.m[3][3];
                    v.ok = v.w > 0.01f;
                    if (v.ok) {
                        v.sx = vp_trans[0] + v.x / v.w * vp_scale[0];
                        v.sy = vp_trans[1] - v.y / v.w * vp_scale[1];
                        v.sz = v.z / v.w;
                    }
                    v.s = int16_t(r.h(va + 8)) * tex_sscale / 32.0f;
                    v.t = int16_t(r.h(va + 10)) * tex_tscale / 32.0f;
                    if (lit) {
                        // no light state tracked: shade by normal.y (+ ambient) as a neutral gray
                        float ny = int8_t(r.b(va + 13)) / 127.0f;
                        float l = 0.55f + 0.45f * std::fabs(ny);
                        v.r = v.g = v.b = l;
                    } else {
                        v.r = r.b(va + 12) / 255.0f;
                        v.g = r.b(va + 13) / 255.0f;
                        v.b = r.b(va + 14) / 255.0f;
                    }
                }
                break;
            }
            case 0x05:
                tris += 1;
                draw_tri(vbuf[((w0 >> 16) & 0xFF) / 2 & 63], vbuf[((w0 >> 8) & 0xFF) / 2 & 63], vbuf[(w0 & 0xFF) / 2 & 63]);
                break;
            case 0x06: case 0x07:
                tris += 2;
                draw_tri(vbuf[((w0 >> 16) & 0xFF) / 2 & 63], vbuf[((w0 >> 8) & 0xFF) / 2 & 63], vbuf[(w0 & 0xFF) / 2 & 63]);
                draw_tri(vbuf[((w1 >> 16) & 0xFF) / 2 & 63], vbuf[((w1 >> 8) & 0xFF) / 2 & 63], vbuf[(w1 & 0xFF) / 2 & 63]);
                break;
            case 0xDA: {
                mtx++;
                uint32_t prm = (w0 & 0xFF) ^ 1;  // F3DEX2 stores param ^ G_MTX_PUSH
                Mtx4 m = read_mtx(r, resolve(w1));
                if (prm & 4) {
                    if (trace) {   // the matrix itself: what RT64 groups into scenes and pairs between frames
                        std::printf("[trace]   proj%s", (prm & 2) ? "" : " mul");
                        for (int i = 0; i < 4; i++)
                            for (int j = 0; j < 4; j++) std::printf(" %.6g", m.m[i][j]);
                        std::printf("\n");
                    }
                    proj = (prm & 2) ? m : mul(m, proj);
                } else {
                    if ((prm & 1) && mv_stack.size() < 32) mv_stack.push_back(mv);
                    mv = (prm & 2) ? m : mul(m, mv);
                }
                break;
            }
            case 0xD8: {  // G_POPMTX
                uint32_t n = w1 / 64;
                while (n-- && !mv_stack.empty()) { mv = mv_stack.back(); mv_stack.pop_back(); }
                break;
            }
            case 0xD9: geom = (geom & (w0 & 0xFFFFFF)) | w1; break;
            case 0xDC:
                if ((w0 & 0xFF) == 8) {  // G_MV_VIEWPORT
                    uint32_t a = resolve(w1);
                    vp_scale[0] = int16_t(r.h(a)) / 4.0f;
                    vp_scale[1] = int16_t(r.h(a + 2)) / 4.0f;
                    vp_trans[0] = int16_t(r.h(a + 8)) / 4.0f;
                    vp_trans[1] = int16_t(r.h(a + 10)) / 4.0f;
                }
                break;
            case 0xDB:
                if (((w0 >> 16) & 0xFF) == 6) seg[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0xFFFFFF;
                break;
            case 0xDE:
                dls++;
                if (((w0 >> 16) & 0xFF) == 0) {
                    if (stack.size() > 18) { bad = true; break; }
                    stack.push_back(pc);
                }
                pc = resolve(w1);
                break;
            case 0xDF:
                if (stack.empty()) goto done;
                pc = stack.back();
                stack.pop_back();
                break;
            case 0xE2: {  // SETOTHERMODE_L
                uint32_t len = (w0 & 0xFF) + 1, shift = 32 - ((w0 >> 8) & 0xFF) - len;
                uint32_t mask = uint32_t(((1ull << len) - 1) << shift);
                oml = (oml & ~mask) | (w1 & mask);
                break;
            }
            case 0xE3: {  // SETOTHERMODE_H
                uint32_t len = (w0 & 0xFF) + 1, shift = 32 - ((w0 >> 8) & 0xFF) - len;
                uint32_t mask = uint32_t(((1ull << len) - 1) << shift);
                omh = (omh & ~mask) | (w1 & mask);
                break;
            }
            case 0xEF: omh = w0 & 0xFFFFFF; oml = w1; break;  // RDPSETOTHERMODE
            case 0xFF:
                cimg_addr = w1 & 0xFFFFFF;  // segmented in theory; THPS uses physical
                cimg_addr = resolve(w1);
                cimg_w = (w0 & 0xFFF) + 1;
                cimg_siz = (w0 >> 19) & 3;
                if (dump && canvas.empty() && cimg_w >= 64 && cimg_w <= 1024) {
                    cw = int(cimg_w);
                    ch = cw * 3 / 4;
                    canvas.assign(size_t(cw) * ch, 0xFF000000);
                }
                break;
            case 0xFD:
                timg_fmt = (w0 >> 21) & 7;
                timg_siz = (w0 >> 19) & 3;
                timg_w = (w0 & 0xFFF) + 1;
                timg_addr = resolve(w1);
                timgs.insert(timg_addr);
                break;
            case 0xF5: {
                TileDesc& t = tiles[(w1 >> 24) & 7];
                t.fmt = (w0 >> 21) & 7;
                t.siz = (w0 >> 19) & 3;
                t.line = (w0 >> 9) & 0x1FF;
                t.tmem = w0 & 0x1FF;
                t.palette = (w1 >> 20) & 0xF;
                break;
            }
            case 0xF2: {
                TileDesc& t = tiles[(w1 >> 24) & 7];
                t.uls = (w0 >> 12) & 0xFFF;
                t.ult = w0 & 0xFFF;
                t.lrs = (w1 >> 12) & 0xFFF;
                t.lrt = w1 & 0xFFF;
                break;
            }
            case 0xF3: case 0xF4: {
                loads++;
                TileDesc& t = tiles[(w1 >> 24) & 7];
                TmemSrc s;
                s.addr = timg_addr;
                s.fmt = timg_fmt;
                s.siz = timg_siz;
                s.img_width = timg_w;
                s.block = (op == 0xF3);
                s.uls = (w0 >> 12) & 0xFFF;
                s.ult = w0 & 0xFFF;
                s.texels = s.block ? ((w1 >> 12) & 0xFFF) + 1 : 0;
                s.dxt = s.block ? (w1 & 0xFFF) : 0;
                if (!s.block) {
                    t.uls = s.uls; t.ult = s.ult;
                    t.lrs = (w1 >> 12) & 0xFFF; t.lrt = w1 & 0xFFF;
                }
                tmem[t.tmem] = s;
                break;
            }
            case 0xF0:  // LOADTLUT
                loads++;
                pal_addr = timg_addr;
                break;
            case 0xF7: fill_color = w1; break;
            case 0xF6: {
                fillrects++;
                if (!canvas.empty()) {
                    int xh = int((w0 >> 12) & 0xFFF) >> 2, yh = int(w0 & 0xFFF) >> 2;
                    int xl = int((w1 >> 12) & 0xFFF) >> 2, yl = int(w1 & 0xFFF) >> 2;
                    uint32_t cyc = (omh >> 20) & 3;
                    if (cyc == 3 /* FILL */ && cimg_siz == 2) {
                        uint32_t c = rgba5551(uint16_t(fill_color & 0xFFFF)) | 0xFF000000;
                        for (int y = std::max(yl, 0); y <= std::min(yh, ch - 1); y++)
                            for (int x = std::max(xl, 0); x <= std::min(xh, cw - 1); x++) canvas[size_t(y) * cw + x] = c;
                    }
                }
                break;
            }
            case 0xE4: case 0xE5: {
                texrects++;
                uint32_t h1 = r.w(pc + 4), h2 = r.w(pc + 12);  // E1 RDPHALF_1, F1 RDPHALF_2
                if (trace) std::printf("[trace]   texrect halves %08X %08X\n", h1, h2);
                pc += 16;
                TexUse u{};
                uint32_t tile = (w1 >> 24) & 7;
                if (tex_use_for_tile(tile, u)) {
                    uses.insert(u);
                    if (!canvas.empty()) {
                        int xh = int((w0 >> 12) & 0xFFF), yh = int(w0 & 0xFFF);
                        int xl = int((w1 >> 12) & 0xFFF), yl = int(w1 & 0xFFF);
                        int16_t s0 = int16_t(h1 >> 16), t0 = int16_t(h1 & 0xFFFF);
                        int16_t dsdx = int16_t(h2 >> 16), dtdy = int16_t(h2 & 0xFFFF);
                        uint32_t cyc = (omh >> 20) & 3;
                        bool copy = (cyc == 2);
                        if (copy) { dsdx /= 4; xh += 4; yh += 4; }
                        bool tlut_ia = ((omh >> 14) & 3) == 3;
                        const TileDesc& td = tiles[tile];
                        for (int y = std::max(yl >> 2, 0); y < std::min(yh >> 2, ch); y++) {
                            for (int x = std::max(xl >> 2, 0); x < std::min(xh >> 2, cw); x++) {
                                int64_t s = (int64_t(s0) * 32 + int64_t(x - (xl >> 2)) * dsdx) >> 10;  // texels
                                int64_t t = (int64_t(t0) * 32 + int64_t(y - (yl >> 2)) * dtdy) >> 10;
                                s -= td.uls >> 2;
                                t -= td.ult >> 2;
                                if (s < 0 || t < 0 || s >= u.w || t >= u.h) continue;
                                uint32_t c = texel(r, u.addr, u.fmt, u.siz, uint32_t(t) * u.row_texels + uint32_t(s), u.pal_addr, tlut_ia,
                                                   (u.odd_swap && (t & 1)) ? 4u : 0u);
                                uint32_t a = c >> 24;
                                if (a == 0) continue;
                                if (a < 255) {
                                    uint32_t d = canvas[size_t(y) * cw + x];
                                    auto mix = [&](int sh) { return ((((c >> sh) & 0xFF) * a + ((d >> sh) & 0xFF) * (255 - a)) / 255) << sh; };
                                    c = mix(0) | mix(8) | mix(16) | 0xFF000000;
                                }
                                canvas[size_t(y) * cw + x] = c | 0xFF000000;
                            }
                        }
                    }
                }
                break;
            }
            case 0xD7:  // G_TEXTURE
                tex_on = w0 & 0xFF;
                tex_tile = (w0 >> 8) & 7;
                tex_sscale = (w1 >> 16) / 65536.0f;
                tex_tscale = (w1 & 0xFFFF) / 65536.0f;
                if (tex_sscale == 0) tex_sscale = 1;
                if (tex_tscale == 0) tex_tscale = 1;
                break;
            default: break;
        }
        if (bad) break;
    }
done:
    // Textures sampled by triangles: record whatever sits in tile 0 at the end too.
    {
        TexUse u{};
        if (tris && tex_use_for_tile(0, u)) uses.insert(u);
    }

    if (ctl) thps::control_dl_stats(task, tris, texrects, mtx);
    if (log || dump || trace) std::printf("[dl] task %u ucode 0x%08X dl 0x%06X: cmds=%" PRIu64 " dls=%" PRIu64 " mtx=%" PRIu64 " vtx=%" PRIu64
                " tris=%" PRIu64 " texrects=%" PRIu64 " fillrects=%" PRIu64 " loads=%" PRIu64 " timgs=%zu cimg=0x%06X w=%u%s\n",
                task, ucode, data_ptr & 0xFFFFFF, cmds, dls, mtx, vtx, tris, texrects, fillrects, loads, timgs.size(), cimg_addr, cimg_w,
                cmds >= max_cmds ? " (TRUNCATED)" : (bad ? " (BAD)" : ""));
    if (log && !uses.empty()) {
        std::printf("[dl]   textures:");
        int k = 0;
        for (const auto& u : uses) {
            if (k++ == 12) { std::printf(" ..."); break; }
            std::printf(" %s%d@%06X:%ux%u", fmt_name(u.fmt), siz_bits[u.siz & 3], u.addr, u.w, u.h);
        }
        std::printf("\n");
    }
    std::fflush(stdout);

    if (dump) {
        char name[256];
        bool tlut_ia = ((omh >> 14) & 3) == 3;
        for (const auto& u : uses) {
            std::vector<uint32_t> px(size_t(u.w) * u.h);
            for (uint32_t y = 0; y < u.h; y++)
                for (uint32_t x = 0; x < u.w; x++) px[size_t(y) * u.w + x] = texel(r, u.addr, u.fmt, u.siz, y * u.row_texels + x, u.pal_addr, tlut_ia, (u.odd_swap && (y & 1)) ? 4u : 0u);
            std::snprintf(name, sizeof name, "task%05u_tex_%06X_%s%d_%ux%u.png", task, u.addr, fmt_name(u.fmt), siz_bits[u.siz & 3], u.w, u.h);
            write_png(thps::platform::to_utf8(dump_dir / name), px, int(u.w), int(u.h));
        }
        if (!canvas.empty()) {
            std::snprintf(name, sizeof name, "task%05u_composite2d_%dx%d.png", task, cw, ch);
            write_png(thps::platform::to_utf8(dump_dir / name), canvas, cw, ch);
        }
        // Also dump the color image straight from RDRAM (only meaningful if RT64 wrote it back).
        if (cimg_w >= 64 && cimg_w <= 1024 && cimg_siz == 2) {
            uint32_t cimg_h = cimg_w * 3 / 4;
            std::vector<uint32_t> px(size_t(cimg_w) * cimg_h);
            for (uint32_t i = 0; i < px.size(); i++) px[i] = rgba5551(r.h(cimg_addr + i * 2)) | 0xFF000000;
            std::snprintf(name, sizeof name, "task%05u_rdram_cimg_%06X.png", task, cimg_addr);
            write_png(thps::platform::to_utf8(dump_dir / name), px, int(cimg_w), int(cimg_h));
        }
        std::printf("[dl]   dumped %zu textures%s (%" PRIu64 " tris drawn) to %s\n", uses.size(), canvas.empty() ? "" : " + composite", tris_drawn,
                    dump_dir.string().c_str());
        std::fflush(stdout);
    }
}

void thps::write_png_rgba(const std::string& path, const std::vector<uint32_t>& px, int w, int h) { write_png(path, px, w, h); }
