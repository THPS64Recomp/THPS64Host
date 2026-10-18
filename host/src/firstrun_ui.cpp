// The first-run window (see firstrun_ui.h).
#include "firstrun_ui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <sstream>

#include "nfd.h"
#include "thps_versions.h"   // thps::game::window_title

#include "platform.h"
#include "sdl_include.h"

namespace thps::ui {
namespace {

// ------------------------------------------------------------------------------------------------ the font
// Our own 5x7 pixel font (plus two rows of descender below the baseline), ASCII 32..126. One string per glyph: rows separated by '/',
// 7 to 9 rows of 5 columns ('#' = lit). Anything above 126 draws as '?'.
struct GlyphDef {
    char ch;
    const char* rows;
};
const GlyphDef k_defs[] = {
    {' ', "...../...../...../...../...../...../....."},
    {'!', "..#../..#../..#../..#../..#../...../..#.."},
    {'"', ".#.#./.#.#./.#.#./...../...../...../....."},
    {'#', ".#.#./.#.#./#####/.#.#./#####/.#.#./.#.#."},
    {'$', "..#../.####/#.#../.###./..#.#/####./..#.."},
    {'%', "##..#/##..#/...#./..#../.#.../#..##/#..##"},
    {'&', ".##../#..#./#.#../.#.../#.#.#/#..#./.##.#"},
    {'\'', "..#../..#../.#.../...../...../...../....."},
    {'(', "...#./..#../.#.../.#.../.#.../..#../...#."},
    {')', ".#.../..#../...#./...#./...#./..#../.#..."},
    {'*', "...../#.#.#/.###./#####/.###./#.#.#/....."},
    {'+', "...../..#../..#../#####/..#../..#../....."},
    {',', "...../...../...../...../.##../.##../..#../.#..."},
    {'-', "...../...../...../#####/...../...../....."},
    {'.', "...../...../...../...../...../.##../.##.."},
    {'/', "...../....#/...#./..#../.#.../#..../....."},
    {'0', ".###./#...#/#..##/#.#.#/##..#/#...#/.###."},
    {'1', "..#../.##../..#../..#../..#../..#../.###."},
    {'2', ".###./#...#/....#/...#./..#../.#.../#####"},
    {'3', "####./....#/....#/.###./....#/....#/####."},
    {'4', "...#./..##./.#.#./#..#./#####/...#./...#."},
    {'5', "#####/#..../####./....#/....#/#...#/.###."},
    {'6', ".###./#..../#..../####./#...#/#...#/.###."},
    {'7', "#####/....#/...#./..#../.#.../.#.../.#..."},
    {'8', ".###./#...#/#...#/.###./#...#/#...#/.###."},
    {'9', ".###./#...#/#...#/.####/....#/....#/.###."},
    {':', "...../.##../.##../...../.##../.##../....."},
    {';', "...../.##../.##../...../.##../.##../..#../.#..."},
    {'<', "...#./..#../.#.../#..../.#.../..#../...#."},
    {'=', "...../...../#####/...../#####/...../....."},
    {'>', ".#.../..#../...#./....#/...#./..#../.#..."},
    {'?', ".###./#...#/....#/...#./..#../...../..#.."},
    {'@', ".###./#...#/#.###/#.#.#/#.###/#..../.###."},
    {'A', ".###./#...#/#...#/#####/#...#/#...#/#...#"},
    {'B', "####./#...#/#...#/####./#...#/#...#/####."},
    {'C', ".###./#...#/#..../#..../#..../#...#/.###."},
    {'D', "####./#...#/#...#/#...#/#...#/#...#/####."},
    {'E', "#####/#..../#..../####./#..../#..../#####"},
    {'F', "#####/#..../#..../####./#..../#..../#...."},
    {'G', ".###./#...#/#..../#.###/#...#/#...#/.###."},
    {'H', "#...#/#...#/#...#/#####/#...#/#...#/#...#"},
    {'I', ".###./..#../..#../..#../..#../..#../.###."},
    {'J', "..###/...#./...#./...#./...#./#..#./.##.."},
    {'K', "#...#/#..#./#.#../##.../#.#../#..#./#...#"},
    {'L', "#..../#..../#..../#..../#..../#..../#####"},
    {'M', "#...#/##.##/#.#.#/#.#.#/#...#/#...#/#...#"},
    {'N', "#...#/##..#/#.#.#/#..##/#...#/#...#/#...#"},
    {'O', ".###./#...#/#...#/#...#/#...#/#...#/.###."},
    {'P', "####./#...#/#...#/####./#..../#..../#...."},
    {'Q', ".###./#...#/#...#/#...#/#.#.#/#..#./.##.#"},
    {'R', "####./#...#/#...#/####./#.#../#..#./#...#"},
    {'S', ".####/#..../#..../.###./....#/....#/####."},
    {'T', "#####/..#../..#../..#../..#../..#../..#.."},
    {'U', "#...#/#...#/#...#/#...#/#...#/#...#/.###."},
    {'V', "#...#/#...#/#...#/#...#/#...#/.#.#./..#.."},
    {'W', "#...#/#...#/#...#/#.#.#/#.#.#/##.##/#...#"},
    {'X', "#...#/#...#/.#.#./..#../.#.#./#...#/#...#"},
    {'Y', "#...#/#...#/.#.#./..#../..#../..#../..#.."},
    {'Z', "#####/....#/...#./..#../.#.../#..../#####"},
    {'[', ".###./.#.../.#.../.#.../.#.../.#.../.###."},
    {'\\', "#..../.#.../..#../...#./....#/...../....."},
    {']', ".###./...#./...#./...#./...#./...#./.###."},
    {'^', "..#../.#.#./#...#/...../...../...../....."},
    {'_', "...../...../...../...../...../...../#####"},
    {'`', ".#.../..#../...#./...../...../...../....."},
    {'a', "...../...../.###./....#/.####/#...#/.####"},
    {'b', "#..../#..../#.##./##..#/#...#/#...#/####."},
    {'c', "...../...../.###./#...#/#..../#...#/.###."},
    {'d', "....#/....#/.##.#/#..##/#...#/#...#/.####"},
    {'e', "...../...../.###./#...#/#####/#..../.###."},
    {'f', "..##./.#..#/.#.../###../.#.../.#.../.#..."},
    {'g', "...../...../.####/#...#/#...#/#...#/.####/....#/.###."},
    {'h', "#..../#..../#.##./##..#/#...#/#...#/#...#"},
    {'i', "..#../...../.##../..#../..#../..#../.###."},
    {'j', "...#./...../..##./...#./...#./...#./...#./#..#./.##.."},
    {'k', "#..../#..../#..#./#.#../##.../#.#../#..#."},
    {'l', ".##../..#../..#../..#../..#../..#../.###."},
    {'m', "...../...../##.#./#.#.#/#.#.#/#...#/#...#"},
    {'n', "...../...../#.##./##..#/#...#/#...#/#...#"},
    {'o', "...../...../.###./#...#/#...#/#...#/.###."},
    {'p', "...../...../#.##./##..#/#...#/#...#/####./#..../#...."},
    {'q', "...../...../.##.#/#..##/#...#/#...#/.####/....#/....#"},
    {'r', "...../...../#.##./##..#/#..../#..../#...."},
    {'s', "...../...../.####/#..../.###./....#/####."},
    {'t', ".#.../.#.../###../.#.../.#.../.#..#/..##."},
    {'u', "...../...../#...#/#...#/#...#/#..##/.##.#"},
    {'v', "...../...../#...#/#...#/#...#/.#.#./..#.."},
    {'w', "...../...../#...#/#...#/#.#.#/#.#.#/.#.#."},
    {'x', "...../...../#...#/.#.#./..#../.#.#./#...#"},
    {'y', "...../...../#...#/#...#/#...#/#...#/.####/....#/.###."},
    {'z', "...../...../#####/...#./..#../.#.../#####"},
    {'{', "..##./.#.../.#.../#..../.#.../.#.../..##."},
    {'|', "..#../..#../..#../..#../..#../..#../..#.."},
    {'}', ".##../...#./...#./....#/...#./...#./.##.."},
    {'~', "...../...../.#..#/#.##./...../...../....."},
};
// Icons beyond ASCII: indexes 95.. in the same table.
enum Icon { IconTick = 95, IconArrow, IconDot, IconCross, GlyphCount };
const char* const k_icons[] = {
    "...../....#/...##/#.##./###../.#.../.....",   // tick
    "..#../..##./#####/..##./..#../...../.....",   // arrow
    "...../...../.##../.##../...../...../.....",   // dot
    "#...#/.#.#./..#../.#.#./#...#/...../.....",   // cross
};

constexpr int kGlyphW = 5, kGlyphH = 9, kAdvance = 6, kLine = 11;   // in font pixels

struct GlyphTable {
    uint8_t px[GlyphCount][kGlyphH][kGlyphW] = {};
    GlyphTable() {
        auto put = [&](int idx, const char* rows) {
            int r = 0, c = 0;
            for (const char* p = rows; *p && r < kGlyphH; p++) {
                if (*p == '/') { r++; c = 0; continue; }
                if (c < kGlyphW) px[idx][r][c++] = (*p == '#');
            }
        };
        for (const GlyphDef& d : k_defs) put(d.ch - 32, d.rows);
        for (int i = 0; i < 4; i++) put(IconTick + i, k_icons[i]);
        // '?' for everything outside ASCII is the table's own '?'
    }
};
const GlyphTable& glyphs() {
    static GlyphTable t;
    return t;
}

// ------------------------------------------------------------------------------------------------ colours
struct Color { uint8_t r, g, b; };
constexpr Color kBg{0x14, 0x16, 0x1b}, kPanel{0x22, 0x26, 0x30}, kBorder{0x3a, 0x41, 0x50}, kText{0xe9, 0xec, 0xf2}, kDim{0x9a, 0xa4, 0xb5},
    kAccent{0xf2, 0xa1, 0x2c}, kAccentText{0x1a, 0x14, 0x08}, kOk{0x4c, 0xc3, 0x8a}, kError{0xff, 0x6b, 0x6b}, kTrack{0x2c, 0x32, 0x3f};

Color tone_color(Tone t) {
    switch (t) {
        case Tone::Warn: return kAccent;
        case Tone::Error: return kError;
        case Tone::Ok: return kOk;
        default: return kText;
    }
}

constexpr int kW = 860, kH = 600, kMargin = 40;   // window size in points
constexpr int kBodyScale = 2, kHeadScale = 3;       // points per font pixel
constexpr int kButtonH = 44;

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Number of columns the text takes (UTF-8 sequences count as one '?').
int text_cols(const std::string& s) {
    int n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) == 0x80) continue;   // continuation byte
        n++;
    }
    return n;
}

// Wraps to `max_cols` columns. A paragraph's leading spaces become the hanging indent of its continuation lines.
std::vector<std::string> wrap(const std::string& text, int max_cols) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string para;
    max_cols = std::max(8, max_cols);
    while (std::getline(in, para)) {
        if (!para.empty() && para.back() == '\r') para.pop_back();
        size_t ind = 0;
        while (ind < para.size() && para[ind] == ' ') ind++;
        const std::string indent(ind, ' ');
        std::string line = indent;
        bool fresh = true;   // nothing but the indent on the line
        std::istringstream words(para.substr(ind));
        std::string w;
        if (para.size() == ind) { out.push_back(""); continue; }
        while (words >> w) {
            while (text_cols(w) > max_cols - int(ind)) {   // a word longer than a line (a path): break it
                int room = max_cols - int(line.size()) - (fresh ? 0 : 1);
                if (room < 6) { out.push_back(line); line = indent; fresh = true; room = max_cols - int(ind); }
                if (!fresh) { line += ' '; }
                line += w.substr(0, size_t(room));
                w = w.substr(size_t(room));
                out.push_back(line);
                line = indent;
                fresh = true;
            }
            if (w.empty()) continue;
            if (!fresh && int(line.size() + 1) + text_cols(w) > max_cols) { out.push_back(line); line = indent; fresh = true; }
            if (!fresh) line += ' ';
            line += w;
            fresh = false;
        }
        out.push_back(line);
    }
    return out;
}

std::string fmt_time(double s) {
    int t = int(s);
    char b[32];
    std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

struct Action {
    std::string name, arg;
};

}  // namespace

bool test_mode() {
    const char* a = std::getenv("THPS_UI_ACTIONS");
    return a && *a;
}

struct Window::Impl {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    double dpi = 1;
    double mouse_scale = 1;   // mouse position -> points: 1 where windows are sized in points (macOS), 1/scaling on Windows

    struct Rect { int x, y, w, h; };
    struct DrawnButton { Button b; Rect r; };
    std::vector<DrawnButton> drawn;
    int hover = -1, pressed = -1, focus = 0;
    std::string current_page_id;
    double page_started = 0;

    std::deque<Action> script;
    bool had_script = false;
    double next_action_at = 0;
    double retry_until = 0;
    std::string scripted_pick;
    bool have_pick = false;
    double idle_since = 0;

    // ---- primitives (points -> device pixels)
    int dev(double v) const { return int(std::lround(v * dpi)); }
    void color(Color c) { SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255); }
    void fill(int x, int y, int w, int h, Color c) {
        color(c);
        SDL_Rect r{dev(x), dev(y), dev(x + w) - dev(x), dev(y + h) - dev(y)};
        SDL_RenderFillRect(renderer, &r);
    }
    void frame(int x, int y, int w, int h, int t, Color c) {
        fill(x, y, w, t, c);
        fill(x, y + h - t, w, t, c);
        fill(x, y, t, h, c);
        fill(x + w - t, y, t, h, c);
    }
    // One glyph index per UTF-8 sequence.
    static int glyph_index(const unsigned char*& p) {
        unsigned char c = *p++;
        if (c >= 32 && c < 127) return c - 32;
        if (c >= 0x80) { while ((*p & 0xC0) == 0x80) p++; }
        return '?' - 32;
    }
    void glyph(int idx, double x, double y, int scale, std::vector<SDL_Rect>& rects) {
        const auto& g = glyphs().px[idx];
        for (int r = 0; r < kGlyphH; r++) {
            int c = 0;
            while (c < kGlyphW) {
                if (!g[r][c]) { c++; continue; }
                int c0 = c;
                while (c < kGlyphW && g[r][c]) c++;
                rects.push_back({dev(x + c0 * scale), dev(y + r * scale), dev(x + c * scale) - dev(x + c0 * scale), dev(y + (r + 1) * scale) - dev(y + r * scale)});
            }
        }
    }
    void text(int x, int y, int scale, Color c, const std::string& s) {
        std::vector<SDL_Rect> rects;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(s.c_str());
        int col = 0;
        while (*p) {
            int idx = glyph_index(p);
            if (idx != 0) glyph(idx, x + col * kAdvance * scale, y, scale, rects);
            col++;
        }
        color(c);
        if (!rects.empty()) SDL_RenderFillRects(renderer, rects.data(), int(rects.size()));
    }
    void icon(int which, int x, int y, int scale, Color c) {
        std::vector<SDL_Rect> rects;
        glyph(which, x, y, scale, rects);
        color(c);
        if (!rects.empty()) SDL_RenderFillRects(renderer, rects.data(), int(rects.size()));
    }
    // Draws wrapped text; returns the y below it. At most `max_lines` lines (the last is cut with "...").
    int paragraph(int x, int y, int width, int scale, Color c, const std::string& s, int max_lines) {
        auto lines = wrap(s, width / (kAdvance * scale));
        int n = int(lines.size());
        bool cut = n > max_lines;
        if (cut) n = max_lines;
        for (int i = 0; i < n; i++) {
            std::string line = lines[size_t(i)];
            if (cut && i == n - 1) line += " ...";
            text(x, y + i * kLine * scale, scale, c, line);
        }
        return y + n * kLine * scale;
    }

    // ---- one page
    void draw(const Page& page, double t_now) {
        color(kBg);
        SDL_RenderClear(renderer);
        fill(0, 0, kW, 6, page.tone == Tone::Normal ? kAccent : tone_color(page.tone));
        // buttons first: their height decides where the text may end
        const int btn_y = kH - kMargin - kButtonH;
        int y = 44;
        y = paragraph(kMargin, y, kW - 2 * kMargin, kHeadScale, tone_color(page.tone), page.heading, 2) + 18;
        const int body_bottom = btn_y - (page.note.empty() ? 20 : 20 + kLine * kBodyScale);
        auto room = [&](int from) { return std::max(1, (body_bottom - from) / (kLine * kBodyScale)); };
        if (page.progress) {
            if (!page.body.empty()) y = paragraph(kMargin, y, kW - 2 * kMargin, kBodyScale, kText, page.body, 3) + 14;
            draw_progress(page, y, t_now);
        } else {
            if (!page.body.empty()) y = paragraph(kMargin, y, kW - 2 * kMargin, kBodyScale, kText, page.body, page.detail.empty() ? room(y) : std::max(3, room(y) - 5)) + 14;
            if (!page.detail.empty()) {
                fill(kMargin - 12, y - 4, 3, std::max(1, body_bottom - y), kBorder);
                paragraph(kMargin, y, kW - 2 * kMargin, kBodyScale, kDim, page.detail, room(y));
            }
        }
        if (!page.note.empty()) text(kMargin, btn_y - 20 - kLine * kBodyScale + 10, kBodyScale, kDim, page.note);
        draw_buttons(page, btn_y);
    }

    void draw_progress(const Page& page, int y, double t_now) {
        const game::Progress& pr = *page.progress;
        int step = pr.step.load();
        for (int i = 0; i <= game::StepCount; i++) {
            const bool is_done_row = (i == game::StepCount);
            std::string label = is_done_row ? "Done: starting the game" : game::step_title(i);
            if (!is_done_row && (i == game::StepCompileC || i == game::StepCompileGlue) && i == step && pr.jobs_total.load() > 0)
                label += "  (" + std::to_string(pr.jobs_done.load()) + " of " + std::to_string(pr.jobs_total.load()) + ")";
            Color c = kDim;
            int ic = IconDot;
            if (step >= 0 && (i < step || step >= game::StepCount)) { c = kOk; ic = IconTick; }
            else if (i == step) { c = kAccent; ic = IconArrow; }
            if (is_done_row && step < game::StepCount) { c = kTrack; ic = IconDot; }
            icon(ic, kMargin, y + 2, kBodyScale, c);
            text(kMargin + 36, y, kBodyScale, (c.r == kDim.r && c.g == kDim.g && c.b == kDim.b) || (is_done_row && step < game::StepCount) ? kDim : kText, label);
            y += 28;
        }
        y += 14;
        // bar
        const int bar_w = kW - 2 * kMargin - 90;
        double f = pr.fraction();
        fill(kMargin, y, bar_w, 20, kTrack);
        fill(kMargin, y, int(bar_w * f), 20, pr.failure.load() == game::Failure::None ? kAccent : kError);
        frame(kMargin, y, bar_w, 20, 2, kBorder);
        char pct[16];
        std::snprintf(pct, sizeof pct, "%3d%%", int(f * 100));
        text(kMargin + bar_w + 14, y + 3, kBodyScale, kText, pct);
        y += 36;
        text(kMargin, y, kBodyScale, kDim, "Elapsed " + fmt_time(t_now - page_started));
    }

    void draw_buttons(const Page& page, int y) {
        drawn.clear();
        int total = 0;
        std::vector<int> widths;
        for (const Button& b : page.buttons) {
            int w = text_cols(b.label) * kAdvance * kBodyScale + 48;
            widths.push_back(w);
            total += w + 16;
        }
        int x = kW - kMargin - (total > 0 ? total - 16 : 0);
        for (size_t i = 0; i < page.buttons.size(); i++) {
            const Button& b = page.buttons[i];
            Rect r{x, y, widths[i], kButtonH};
            drawn.push_back({b, r});
            const bool hot = int(i) == hover, down = int(i) == pressed && hot;
            if (b.primary) {
                fill(r.x, r.y, r.w, r.h, down ? Color{0xc8, 0x82, 0x1c} : hot ? Color{0xff, 0xb4, 0x45} : kAccent);
                text(r.x + 24, r.y + (kButtonH - 7 * kBodyScale) / 2, kBodyScale, kAccentText, b.label);
            } else {
                fill(r.x, r.y, r.w, r.h, down ? Color{0x1a, 0x1d, 0x25} : hot ? Color{0x2d, 0x33, 0x41} : kPanel);
                frame(r.x, r.y, r.w, r.h, 2, kBorder);
                text(r.x + 24, r.y + (kButtonH - 7 * kBodyScale) / 2, kBodyScale, kText, b.label);
            }
            if (int(i) == focus) frame(r.x - 4, r.y - 4, r.w + 8, r.h + 8, 2, kText);
            x += widths[i] + 16;
        }
    }

    int points(int window_coord) const { return int(window_coord * mouse_scale); }
    int button_at(int mx, int my) const {
        for (size_t i = 0; i < drawn.size(); i++) {
            const Rect& r = drawn[i].r;
            if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h) return int(i);
        }
        return -1;
    }

    // ---- tests
    void load_script() {
        const char* a = std::getenv("THPS_UI_ACTIONS");
        if (!a || !*a) return;
        std::string s = a;
        size_t i = 0;
        while (i <= s.size()) {
            size_t j = s.find('|', i);
            std::string item = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
            if (!item.empty()) {
                size_t eq = item.find('=');
                script.push_back({item.substr(0, eq), eq == std::string::npos ? "" : item.substr(eq + 1)});
            }
            if (j == std::string::npos) break;
            i = j + 1;
        }
        had_script = true;
    }

    void save_shot(const std::string& name) {
        const char* dir = std::getenv("THPS_UI_SHOT_DIR");
        if (!dir || !*dir) { std::printf("[ui] shot %s: THPS_UI_SHOT_DIR is not set\n", name.c_str()); return; }
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!s) return;
        SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
        std::string path = std::string(dir) + "/" + name + ".bmp";
        int rc = SDL_SaveBMP(s, path.c_str());
        SDL_FreeSurface(s);
        std::printf("[ui] shot %s -> %s (%s)\n", name.c_str(), path.c_str(), rc == 0 ? "ok" : SDL_GetError());
        std::fflush(stdout);
    }

    // Returns true when the action ends run() with `out`.
    bool script_step(const Page& page, double t_now, Result& out) {
        if (script.empty() || t_now < next_action_at) return false;
        Action a = script.front();
        script.pop_front();
        std::printf("[ui] script: %s%s%s on page '%s'\n", a.name.c_str(), a.arg.empty() ? "" : "=", a.arg.c_str(), page.id.c_str());
        std::fflush(stdout);
        auto retry = [&](double limit_s) {   // the thing it waits for may appear in a moment
            if (retry_until == 0) retry_until = t_now + limit_s;
            if (t_now < retry_until) {
                script.push_front(a);
                next_action_at = t_now + 0.2;
                return true;
            }
            retry_until = 0;
            std::printf("[ui] script: gave up on %s=%s (page '%s')\n", a.name.c_str(), a.arg.c_str(), page.id.c_str());
            return false;
        };
        if (a.name == "wait") next_action_at = t_now + std::atof(a.arg.c_str());
        else if (a.name == "shot") { save_shot(a.arg); next_action_at = t_now + 0.1; }
        else if (a.name == "pick") { scripted_pick = a.arg; have_pick = true; next_action_at = t_now + 0.05; }
        else if (a.name == "waitpage") {
            if (page.id != a.arg) { retry(60); return false; }
            retry_until = 0;
            next_action_at = t_now + 0.5;   // let the page draw once
        } else if (a.name == "click") {
            for (const DrawnButton& d : drawn) {
                if (d.b.id == a.arg) { retry_until = 0; out.button = a.arg; next_action_at = t_now + 0.3; return true; }
            }
            retry(10);
        } else if (a.name == "drop") { out.button = "$drop"; out.path = a.arg; next_action_at = t_now + 0.3; return true; }
        else if (a.name == "key") {
            if (a.arg == "enter" && !drawn.empty()) { out.button = drawn[size_t(std::min<int>(focus, int(drawn.size()) - 1))].b.id; return true; }
            if (a.arg == "escape") { out.button = "$close"; return true; }
        } else if (a.name == "quit") { out.button = "$close"; return true; }
        else std::printf("[ui] script: unknown action %s\n", a.name.c_str());
        return false;
    }
};

bool Window::open(bool hidden, std::string& error) {
    if (impl_) return true;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { error = std::string("SDL_Init failed: ") + SDL_GetError(); return false; }
    auto* im = new Impl();
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    // Windows sizes windows in physical pixels: grow with the display scaling (tests and scripted runs keep kW x kH).
    int win_w = kW, win_h = kH;
    if (!hidden && !test_mode()) thps::platform::scale_window_size(win_w, win_h);
    im->window = SDL_CreateWindow(thps::game::window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, flags);
    if (!im->window) { error = std::string("SDL_CreateWindow failed: ") + SDL_GetError(); delete im; return false; }
    SDL_GetWindowSize(im->window, &win_w, &win_h);
    im->mouse_scale = win_w > 0 ? double(kW) / win_w : 1.0;
    // A hidden window (tests) or a scripted run draws in software: no GPU context is needed for screenshots.
    const bool software = hidden || test_mode() || std::getenv("THPS_UI_SOFTWARE");
    if (!software) im->renderer = SDL_CreateRenderer(im->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!im->renderer) im->renderer = SDL_CreateRenderer(im->window, -1, SDL_RENDERER_SOFTWARE);
    if (!im->renderer) {
        error = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
        SDL_DestroyWindow(im->window);
        delete im;
        return false;
    }
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(im->renderer, &ow, &oh);
    im->dpi = ow > 0 ? double(ow) / kW : 1.0;
    im->load_script();
    window_ = im->window;
    impl_ = im;
    return true;
}

void Window::close() {
    if (!impl_) return;
    if (impl_->renderer) SDL_DestroyRenderer(impl_->renderer);
    if (impl_->window) SDL_DestroyWindow(impl_->window);
    delete impl_;
    impl_ = nullptr;
    window_ = nullptr;
    // SDL_INIT_VIDEO stays initialised: the game's own window (create_gfx) comes next.
}

bool Window::take_scripted_pick(std::string& path) {
    if (!impl_ || !impl_->have_pick) return false;
    path = impl_->scripted_pick;
    impl_->have_pick = false;
    return true;
}

Result Window::run(Page& page, const std::function<bool(Page&)>& tick) {
    Impl& im = *impl_;
    Result out;
    if (im.current_page_id != page.id) {   // the same page shown again after a button that keeps it (Install, Open log folder) is not a new page
        std::printf("[ui] page %s: %s\n", page.id.c_str(), page.heading.c_str());
        std::fflush(stdout);
    }
    im.current_page_id = page.id;
    im.page_started = now_s();
    im.hover = im.pressed = -1;
    im.focus = 0;
    for (size_t i = 0; i < page.buttons.size(); i++) {
        if (page.buttons[i].primary) im.focus = int(i);
    }
    im.idle_since = now_s();
    const double idle_limit = std::getenv("THPS_UI_IDLE_S") ? std::atof(std::getenv("THPS_UI_IDLE_S")) : 20.0;
    std::string last_note;
    for (;;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT: out.button = "$close"; return out;
                case SDL_WINDOWEVENT:
                    if (ev.window.event == SDL_WINDOWEVENT_CLOSE) { out.button = "$close"; return out; }
                    break;
                case SDL_DROPFILE:
                    if (ev.drop.file) {
                        out.button = "$drop";
                        out.path = ev.drop.file;
                        SDL_free(ev.drop.file);
                        return out;
                    }
                    break;
                case SDL_MOUSEMOTION: im.hover = im.button_at(im.points(ev.motion.x), im.points(ev.motion.y)); break;
                case SDL_MOUSEBUTTONDOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) im.pressed = im.button_at(im.points(ev.button.x), im.points(ev.button.y));
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        int up = im.button_at(im.points(ev.button.x), im.points(ev.button.y));
                        if (up >= 0 && up == im.pressed) { out.button = im.drawn[size_t(up)].b.id; return out; }
                        im.pressed = -1;
                    }
                    break;
                case SDL_KEYDOWN: {
                    SDL_Keycode k = ev.key.keysym.sym;
                    const int n = int(page.buttons.size());
                    if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && n > 0) { out.button = page.buttons[size_t(im.focus % n)].id; return out; }
                    if (k == SDLK_ESCAPE) {
                        for (const Button& b : page.buttons) {
                            if (b.id == "quit" || b.id == "cancel") { out.button = b.id; return out; }
                        }
                    }
                    if ((k == SDLK_TAB || k == SDLK_RIGHT || k == SDLK_LEFT) && n > 0) {
                        bool back = k == SDLK_LEFT || (ev.key.keysym.mod & KMOD_SHIFT);
                        im.focus = (im.focus + (back ? n - 1 : 1)) % n;
                    }
                    break;
                }
                default: break;
            }
        }
        if (tick && tick(page)) { out.button = "$done"; return out; }
        const double t = now_s();
        im.draw(page, t);
        if (im.script_step(page, t, out)) return out;
        if (im.had_script && im.script.empty() && !page.progress && t - im.idle_since > idle_limit) {
            std::printf("[ui] script: nothing left to do on page '%s' (waited %.0f s)\n", page.id.c_str(), idle_limit);
            std::fflush(stdout);
            out.button = "$timeout";
            return out;
        }
        if (!im.script.empty()) im.idle_since = t;
        SDL_RenderPresent(im.renderer);
        SDL_Delay(25);
    }
}

bool native_pick_rom(std::string& path, std::string& error) {
    error.clear();
    if (NFD_Init() != NFD_OKAY) { error = NFD_GetError() ? NFD_GetError() : "the file dialog could not start"; return false; }
    nfdu8filteritem_t filters[1] = {{"N64 ROM", "z64,n64,v64"}};
    nfdu8char_t* out = nullptr;
    nfdresult_t r = NFD_OpenDialogU8(&out, filters, 1, nullptr);
    bool ok = false;
    if (r == NFD_OKAY && out) {
        path = out;
        NFD_FreePathU8(out);
        ok = true;
    } else if (r == NFD_ERROR) {
        error = NFD_GetError() ? NFD_GetError() : "the file dialog failed";
    }
    NFD_Quit();
    return ok;
}

}  // namespace thps::ui
