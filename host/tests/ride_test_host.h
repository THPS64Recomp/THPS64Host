// Own synthetic host interface for ROM-free tests of the actual ride.cpp.
#pragma once
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <string>
#include "../src/ride.h"
struct recomp_context {};
extern "C" void thps_ride_step_begin(uint8_t*,recomp_context*);
extern "C" void thps_ride_step_end(uint8_t*,recomp_context*);
namespace thps {
namespace game { inline constexpr const char* family="thps2"; }
struct VersionInfo { const char* id; const char* code_class; };
inline const VersionInfo test_us{"thps2","us"},test_eu{"thps2_eu","eu"};
struct CodeClass { const char* name; uint32_t ride_step_override,ride_step_frame_time,ride_step_frame_ticks; };
inline CodeClass code_classes[]={{"us",0x80000024,0x80000028,0x8000002C},{"eu",0,0,0}};
struct SymTable { uint32_t gSkater,gRunActive,gTimeScale,Skater_PhysState; };
inline const SymTable test_symbols{0x80000018,0x80000010,0x80000014,4};
struct NamedSym { const char* name; uint32_t SymTable::*member; };
inline const NamedSym sym_names[]={{"gSkater",&SymTable::gSkater},{"gRunActive",&SymTable::gRunActive},{"gTimeScale",&SymTable::gTimeScale}};
inline const SymTable& sym(){return test_symbols;}
namespace versions {
inline const VersionInfo* current=&test_us;
inline int tv_override=-1;
inline const VersionInfo* active(){return current;}
}
inline std::atomic<uint8_t*> rdram_base{nullptr};
struct Stats { std::atomic<uint64_t> gfx_tasks{0}; };
inline Stats stats;
namespace dd { inline uint64_t generation=0;inline uint64_t course_generation(){return generation;} }
namespace platform { inline FILE* fopen_path(const std::string& path,const char* mode){return std::fopen(path.c_str(),mode);} }
}
