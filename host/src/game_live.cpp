// Spike A (the THPS1 repository's docs/internals/recomp.md): translate the whole game at start-up with N64Recomp's LiveRecomp (sljit),
// instead of loading a compiled game library. Only built with -DTHPS_LIVERECOMP=ON (on top of THPS_SPLIT_GAME_LIB).
//
// What it does, in order:
//   1. code blob (native ERZ inflate) -> N64Recomp::Context from the class's symbol file (names, addresses, sizes);
//   2. functions the runtime reimplements (osXxx, memcpy...) get a one-instruction body (jr ra), and are then redirected
//      to the runtime's native implementation by patching their entry (the way librecomp patches mod functions);
//   3. [[patches.instruction]] and `ignored` from the class's N64Recomp config (the COP2 leftovers);
//   4. our [[patches.hook]] blocks: C text cannot be JIT-compiled, so each hook is a native wrapper (table below) over a
//      second JIT copy of the function ("original"), reached by patching the entry of the first;
//   5. every function through LiveGenerator into one sljit code object; the section table and the entry point are
//      registered as the class's payload, exactly where the static build links libthps1_funcs.
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <toml++/toml.hpp>

#include "recompiler/context.h"
#include "recompiler/live_recompiler.h"
#include "librecomp/sections.h"

#include "game_split.h"
#include "host.h"
#include "platform.h"
#include "thps_hooks.h"

extern "C" void thps_name_entry_update(uint8_t* rdram, recomp_context* ctx);   // THPS1's game/name_entry.cpp (a Before hook below)

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/resource.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace thps::game {
extern void* const g_import_table[];
extern const char* const g_import_names[];
extern const size_t g_import_count;
}  // namespace thps::game

extern "C" {
void cop0_status_write(recomp_context* ctx, gpr value);
gpr cop0_status_read(recomp_context* ctx);
void switch_error(const char* func, uint32_t vram, uint32_t jtbl);
void do_break(uint32_t vram);
recomp_func_t* get_function(int32_t vram);
void pause_self(uint8_t* rdram);
}
// The static RSP code, linked only in the experiment build (-DTHPS_LIVE_STATIC_RSP=ON): RSPRecomp output is C++ and has no JIT.
#ifdef THPS_LIVE_STATIC_RSP
#include "librecomp/rsp.hpp"
RspExitReason n_aspMain(uint8_t* rdram, uint32_t ucode_addr);
#endif

using namespace std::string_view_literals;
namespace fs = std::filesystem;
namespace plat = thps::platform;

namespace thps::game {
namespace {

using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// cop0 Status is not used by the game's own code (libultra's use of it is reimplemented), but the generator wants the callbacks.
void live_cop0_write(recomp_context*, gpr) {}
gpr live_cop0_read(recomp_context*) { return 0; }

// ----------------------------------------------------------------------------- patching executable memory
void patch_entry(void* target, void* replacement) {
    uint8_t code[16];
    size_t n = 0;
#if defined(__x86_64__) || defined(_M_X64)
    code[n++] = 0x48; code[n++] = 0xB8;                       // movabs rax, imm64
    std::memcpy(code + n, &replacement, 8); n += 8;
    code[n++] = 0xFF; code[n++] = 0xE0;                       // jmp rax
#elif defined(__aarch64__)
    static const uint8_t ldr_x2_8_br_x2[] = {0x42, 0x00, 0x00, 0x58, 0x40, 0x00, 0x1F, 0xD6};   // ldr x2, #8 ; br x2
    std::memcpy(code, ldr_x2_8_br_x2, 8); n = 8;
    std::memcpy(code + n, &replacement, 8); n += 8;
#else
#error "unsupported architecture"
#endif
#if defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(0);          // sljit's executable memory is MAP_JIT: W^X is per thread
    std::memcpy(target, code, n);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(target, n);
#else
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t start = reinterpret_cast<uintptr_t>(target) & ~uintptr_t(page - 1);
    mprotect(reinterpret_cast<void*>(start), size_t(page) * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    std::memcpy(target, code, n);
    mprotect(reinterpret_cast<void*>(start), size_t(page) * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char*>(target), reinterpret_cast<char*>(target) + n);
#endif
}

// ----------------------------------------------------------------------------- hooks (config/thps1.hooks.toml, as native code)
// The text of each [[patches.hook]] in the static build is the body below, run at the function's entry. kind:
//   Replace  "...; return;"            the function is gone (native code runs instead)
//   Before   "...;"                    native code, then the original
//   If       "if (hook()) return;"     native code decides whether the original runs
using NativeFn = void (*)(uint8_t*, recomp_context*);
struct HookDef {
    const char* func;
    enum Kind { Replace, Before, If, IfSetR2Is3 } kind;
    NativeFn fn;                 // Replace / Before
    int (*cond)(uint8_t*, recomp_context*);   // If / IfSetR2Is3
};
void erz1(uint8_t* r, recomp_context* c) { thps_erz_unpack(r, c, 1); }
void erz2(uint8_t* r, recomp_context* c) { thps_erz_unpack(r, c, 2); }
const HookDef k_hooks[] = {
    {"Boot_PioCopy", HookDef::Replace, thps_boot_pio_copy, nullptr},
    {"Erz_UnpackMethod1", HookDef::Replace, erz1, nullptr},
    {"Erz_UnpackMethod2", HookDef::Replace, erz2, nullptr},
    {"thps_osPiRawStartDma", HookDef::Replace, thps_pi_raw_start_dma, nullptr},
    {"thps_osSetEventMesg", HookDef::Replace, thps_set_event_mesg, nullptr},
    {"GameEntry", HookDef::Before, thps_on_game_entry, nullptr},
    {"thps_osAiGetStatus", HookDef::Replace, thps_ai_get_status, nullptr},
    {"thps_osContGetQuery", HookDef::Replace, thps_cont_get_query, nullptr},
    {"View_ComputeCullPlanes", HookDef::If, nullptr, thps_widen_cull_planes},
    {"View_SetFrustum", HookDef::If, nullptr, thps_view_set_frustum},
    {"Front_IntroUpdate", HookDef::IfSetR2Is3, nullptr, thps_skip_intro_logos},
    {"Front_NoticeUpdate", HookDef::Before, thps_skip_intro_notice, nullptr},
    {"NameEntry_Update", HookDef::Before, thps_name_entry_update, nullptr},
};
constexpr int k_num_hooks = sizeof(k_hooks) / sizeof(k_hooks[0]);

struct HookRuntime {
    recomp_func_t* original = nullptr;
};
HookRuntime g_hook_rt[k_num_hooks];

template <int N>
void hook_wrapper(uint8_t* rdram, recomp_context* ctx) {
    const HookDef& h = k_hooks[N];
    switch (h.kind) {
        case HookDef::Replace: h.fn(rdram, ctx); return;
        case HookDef::Before: h.fn(rdram, ctx); break;
        case HookDef::If: if (h.cond(rdram, ctx)) return; break;
        case HookDef::IfSetR2Is3: if (h.cond(rdram, ctx)) { ctx->r2 = 3; return; } break;
    }
    g_hook_rt[N].original(rdram, ctx);
}
template <int... I>
constexpr std::array<NativeFn, sizeof...(I)> make_wrappers(std::integer_sequence<int, I...>) { return {hook_wrapper<I>...}; }
const std::array<NativeFn, k_num_hooks> k_wrappers = make_wrappers(std::make_integer_sequence<int, k_num_hooks>{});

// ----------------------------------------------------------------------------- the translation
N64Recomp::LiveGeneratorOutput g_output;   // owns the JIT code for the life of the process
std::vector<FuncEntry> g_func_entries[8];
std::vector<SectionTableEntry> g_sections;
std::vector<int> g_overlay_table{-1};
int32_t g_section_addrs[16];

long rss_kb() {
#if defined(__APPLE__)
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss / 1024;   // bytes on macOS
#else
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
#endif
}

}  // namespace

bool live_translate(const VersionInfo& v, const fs::path& rom_z64, const fs::path& recipe_dir, std::string& error) {
    auto total = Clock::now();
    const long rss0 = rss_kb();
    N64Recomp::live_recompiler_init();   // librecomp does this at recomp::start; the rabbitizer pseudo-instruction switches matter here

    // 1. Context
    auto t = Clock::now();
    std::vector<uint8_t> blob;
    if (!make_code_blob(v, rom_z64, blob, error)) return false;
    N64Recomp::Context ctx;
    if (!N64Recomp::Context::from_symbol_file(recipe_dir / (std::string(thps::game::family) + ".host.syms.toml"), std::move(blob), ctx, true)) {
        error = "cannot read " + plat::to_utf8(recipe_dir / (std::string(thps::game::family) + ".host.syms.toml"));
        return false;
    }
    std::printf("[live] context: %zu functions, %zu sections: %.2f s\n", ctx.functions.size(), ctx.sections.size(), since(t));

    // 2. runtime-provided functions: one-instruction bodies, redirected after generation
    std::unordered_map<std::string, void*> native;
    for (size_t i = 0; i < g_import_count; i++) native.emplace(g_import_names[i], g_import_table[i]);
    constexpr uint32_t k_jr_ra = 0x03E00008;   // big-endian words are stored byte-swapped in Function::words (see context.h byteswap)
    auto stub_words = []() { return std::vector<uint32_t>{byteswap(0x03E00008u), 0u}; };
    (void)k_jr_ra;
    struct Redirect { size_t func_index; void* to; const char* why; };
    std::vector<Redirect> redirects;
    std::unordered_map<std::string, size_t> by_name;
    for (size_t i = 0; i < ctx.functions.size(); i++) by_name[ctx.functions[i].name] = i;
    size_t n_reimpl = 0, n_ignored_nonnative = 0;
    std::vector<uint32_t> orig_size(ctx.functions.size());
    for (size_t i = 0; i < ctx.functions.size(); i++) orig_size[i] = uint32_t(ctx.functions[i].words.size() * 4);
    for (size_t i = 0; i < ctx.functions.size(); i++) {
        N64Recomp::Function& f = ctx.functions[i];
        bool reimpl = N64Recomp::reimplemented_funcs.contains(f.name);
        bool ign = N64Recomp::ignored_funcs.contains(f.name);
        bool ren = N64Recomp::renamed_funcs.contains(f.name);
        if (reimpl || ign) {
            auto it = native.find(f.name + "_recomp");
            f.words = stub_words();
            if (it != native.end()) { redirects.push_back({i, it->second, "runtime"}); n_reimpl++; }
            else n_ignored_nonnative++;
        } else if (ren) {
            // The game's own copy of a libc function (memcpy, strlen...): compiled as is, under a different name in the static build.
        }
    }

    // 3. patches from the class's N64Recomp config
    try {
        toml::table cfg = toml::parse_file(plat::to_utf8(recipe_dir / (std::string(thps::game::family) + ".host.toml")));
        if (auto* ig = cfg["patches"]["ignored"].as_array()) {
            for (auto& e : *ig) {
                auto it = by_name.find(std::string(e.value_or(""sv)));
                if (it != by_name.end()) ctx.functions[it->second].words = stub_words();
            }
        }
        if (auto* ins = cfg["patches"]["instruction"].as_array()) {
            for (auto& e : *ins) {
                auto* tb = e.as_table();
                auto it = by_name.find(std::string((*tb)["func"].value_or(""sv)));
                if (it == by_name.end()) { error = "instruction patch for an unknown function"; return false; }
                N64Recomp::Function& f = ctx.functions[it->second];
                uint32_t vram = (*tb)["vram"].value_or(0u), value = (*tb)["value"].value_or(0u);
                if (vram < f.vram || vram >= f.vram + f.words.size() * 4) { error = "instruction patch outside " + f.name; return false; }
                f.words[(vram - f.vram) / 4] = byteswap(value);
            }
        }
    } catch (const toml::parse_error& e) {
        error = std::string("cannot parse the class config: ") + e.what();
        return false;
    }

    // 4. hooks: a second copy of the function for the wrapper to call
    struct HookBind { int hook; size_t func_index; size_t orig_index; };
    std::vector<HookBind> binds;
    for (int h = 0; h < k_num_hooks; h++) {
        auto it = by_name.find(k_hooks[h].func);
        if (it == by_name.end()) { error = std::string("hook target not found: ") + k_hooks[h].func; return false; }
        size_t orig = (size_t)-1;
        if (k_hooks[h].kind != HookDef::Replace) {
            N64Recomp::Function copy = ctx.functions[it->second];
            copy.name += "__original";
            orig = ctx.functions.size();
            ctx.functions.push_back(std::move(copy));   // not entered in functions_by_vram / section_functions: nothing calls it but the wrapper
        }
        binds.push_back({h, it->second, orig});
    }

    // The static C ends every function with the closing brace: control that runs off the end (Boot_Entry ends in `jal Boot_Main`,
    // which returns) returns to the caller. LiveGenerator emits no return there, so execution would fall into the next function's
    // code (found by running it: Boot_Main was entered a second time). Append `jr ra; nop` to every function.
    for (auto& f : ctx.functions) {
        if (f.words.empty()) continue;
        f.words.push_back(byteswap(0x03E00008u));
        f.words.push_back(0u);
    }

    // 5. generate
    for (size_t s = 0; s < ctx.sections.size() && s < 16; s++) g_section_addrs[s] = int32_t(ctx.sections[s].ram_addr);
    N64Recomp::LiveGeneratorInputs in{};
    in.base_event_index = 0;
    in.cop0_status_write = live_cop0_write;
    in.cop0_status_read = live_cop0_read;
    in.switch_error = switch_error;
    in.do_break = do_break;
    in.get_function = get_function;
    in.syscall_handler = nullptr;
    in.pause_self = pause_self;
    in.trigger_event = nullptr;
    in.reference_section_addresses = g_section_addrs;
    in.local_section_addresses = g_section_addrs;
    in.run_hook = nullptr;
    N64Recomp::LiveGenerator gen{ctx.functions.size(), in};
    std::vector<std::vector<uint32_t>> statics(ctx.sections.size());
    t = Clock::now();
    std::ostringstream sink;
    for (size_t i = 0; i < ctx.functions.size(); i++) {
        if (ctx.functions[i].words.empty()) continue;   // zero-sized symbols
        if (!N64Recomp::recompile_function_live(gen, ctx, i, sink, statics, false)) {
            error = "LiveRecomp could not translate " + ctx.functions[i].name;
            return false;
        }
    }
    double t_translate = since(t);
    t = Clock::now();
    g_output = gen.finish();
    if (!g_output.good) { error = "LiveRecomp failed while linking the generated code (a call to a function that was not generated)"; return false; }
    double t_finish = since(t);
    std::printf("[live] translate %zu functions: %.2f s; link+emit: %.2f s; code %.2f MB\n", ctx.functions.size(), t_translate, t_finish,
                double(g_output.code_size) / 1048576.0);

    // 6. redirect: runtime implementations, hooks
    t = Clock::now();
    const bool dbg_no_redirect = std::getenv("THPS_LIVE_NO_REDIRECT") != nullptr, dbg_no_hooks = std::getenv("THPS_LIVE_NO_HOOKS") != nullptr;
    if (!dbg_no_redirect)
        for (const Redirect& r : redirects) patch_entry(reinterpret_cast<void*>(g_output.functions[r.func_index]), r.to);
    for (const HookBind& b : binds) {
        if (dbg_no_hooks) break;
        if (b.orig_index != (size_t)-1) g_hook_rt[b.hook].original = g_output.functions[b.orig_index];
        patch_entry(reinterpret_cast<void*>(g_output.functions[b.func_index]), reinterpret_cast<void*>(k_wrappers[b.hook]));
    }
    std::printf("[live] redirected %zu runtime functions (%zu ignored without a native twin), %d hooks: %.3f s\n", n_reimpl, n_ignored_nonnative,
                k_num_hooks, since(t));

    // 7. payload
    g_sections.clear();
    recomp_func_t* entry = nullptr;
    size_t out_idx = 0;
    for (size_t s = 0; s < ctx.sections.size(); s++) {
        const auto& sec = ctx.sections[s];
        if (ctx.section_functions[s].empty()) continue;
        std::vector<FuncEntry>& fe = g_func_entries[out_idx++];
        for (size_t fi : ctx.section_functions[s]) {
            const N64Recomp::Function& f = ctx.functions[fi];
            if (orig_size[fi] == 0) continue;
            fe.push_back(FuncEntry{g_output.functions[fi], f.rom - sec.rom_addr, orig_size[fi]});
            if (f.rom == 0x1000 && f.vram == v.boot_vram) entry = g_output.functions[fi];
        }
        g_sections.push_back(SectionTableEntry{sec.rom_addr, sec.ram_addr, sec.size, fe.data(), fe.size(), nullptr, 0, s});
    }
    if (!entry) { error = "no entry point (Boot_Entry) found"; return false; }
    static Payload payload{};
    payload = Payload{v.code_class, g_sections.data(), g_sections.size(), g_overlay_table.data(), g_overlay_table.size(), entry, nullptr, 0,
                      g_output.code, static_cast<const uint8_t*>(g_output.code) + g_output.code_size};
    register_payload(payload);
#ifdef THPS_LIVE_STATIC_RSP
    set_aspmain_override([](uint8_t* rdram, uint32_t ucode) -> int { return int(n_aspMain(rdram, ucode)); });
#endif
    std::printf("[live] ready: %.2f s total; peak RSS grew %ld MB\n", since(total), (rss_kb() - rss0) / 1024);
    std::fflush(stdout);
    return true;
}

}  // namespace thps::game
