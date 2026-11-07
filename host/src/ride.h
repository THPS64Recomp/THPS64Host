// Rides: controller files and optional audited configured timing.
#pragma once
#include <string>
#include <cstdint>
namespace thps {
struct VersionInfo;
bool ride_record(const std::string& path);
bool ride_record_step(const std::string& path, const std::string& step);
bool ride_set_step(const std::string& step); // CLI default for new recordings; v1/v2 playback explicitly refuses it
void ride_engine_live(bool live);
bool ride_needs_step();
bool ride_validate_version(const VersionInfo& version);
std::string ride_error();
bool ride_play(const std::string& path);
void ride_stop();
void ride_hold(bool enabled);   // test capture fence: stop at the next read after the complete ride; false always releases it
bool ride_is_held();
void ride_quit();
std::string ride_status();
uint64_t ride_position();   // inputs played so far (0 when not playing): the draw distance digest aligns its records to it
void ride_on_read(bool* present, uint16_t* buttons, float* x, float* y);   // game thread, each port 1 read (get_input)
}
