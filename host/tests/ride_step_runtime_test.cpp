// Actual ride.cpp with a synthetic host and owned words, not a game or ROM.
#include "ride_test_host.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>
#include <chrono>
#include <future>
#include <thread>

int main(int argc,char** argv) {
    if(argc!=2)return 2;
    namespace fs=std::filesystem;
    const fs::path out=fs::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(out.parent_path());
    if(!fs::create_directory(out))return 2;
    std::printf("synthetic evidence: %s\n",out.string().c_str());
    alignas(8) std::array<uint8_t,1024> ram{}; thps::rdram_base=ram.data();
    auto& run=*reinterpret_cast<uint32_t*>(ram.data()+0x10);
    auto& body=*reinterpret_cast<uint32_t*>(ram.data()+0x18);
    auto& original=*reinterpret_cast<uint32_t*>(ram.data()+0x24);
    auto& chosen=*reinterpret_cast<uint32_t*>(ram.data()+0x28);
    auto& ticks=*reinterpret_cast<uint32_t*>(ram.data()+0x2C);
    uint32_t natural_ticks=3;
    original=0x3C23D70A;
    unsigned n=0,bad=0;
    auto check=[&](bool yes,const char* name){++n;if(!yes){++bad;std::fprintf(stderr,"FAIL %s status=%s\n",name,thps::ride_status().c_str());}};
    auto status=[&](const char* value){return thps::ride_status().find(value)!=std::string::npos;};
    auto read=[&](){bool p=true;uint16_t b=0x8000;float x=.5f,y=-.25f;thps::ride_on_read(&p,&b,&x,&y);};
    auto writer=[&](){thps_ride_step_begin(ram.data(),nullptr);chosen=original;ticks=natural_ticks;thps_ride_step_end(ram.data(),nullptr);};
    auto contents=[](const fs::path& path){std::ifstream f(path);std::ostringstream s;s<<f.rdbuf();return s.str();};
    auto reset=[&](){thps::ride_quit();body=0;run=0;thps::ride_engine_live(false);thps::versions::current=&thps::test_us;};
    const auto legacy=out/"legacy.ride";
    check(thps::ride_record(legacy.string()),"legacy record arms");
    ++thps::dd::generation;body=0x80000100;run=1;writer();read();thps::ride_stop();
    check(contents(legacy).starts_with("# thps ride v2 thps2 reads=1 start=run"),"legacy header remains v2 and writes at stop");
    check(original==0x3C23D70A&&ticks==3,"legacy hooks do not change override or integer output");reset();
    const auto configured=out/"configured.ride";
    check(!thps::ride_record_step(configured.string(),"1/60"),"unsupported option refuses");
    body=0x80000100;
    check(!thps::ride_record_step(configured.string(),"1/30")&&thps::ride_error()=="step-present-body-arm","present-body arm refuses");
    body=0;check(thps::ride_record_step(configured.string(),"1/30"),"configured recording arms without body");
    ++thps::dd::generation;writer();check(original==0x3C23D70A&&status("mode=rec-armed"),"setup before body does not install");
    body=0x80000100;run=1;writer();
    check(chosen==0x3D088889&&ticks==2&&original==0x3C23D70A,"actual callbacks choose coherent float/integer base and restore nonzero word");
    check(status("step_tick_writes=1"),"configured integer output write accounted");
    read();check(status("mode=rec "),"first input activates only after writer");
    run=0;writer();check(chosen==0x3D088889&&ticks==2,"loaded pause retains coherent configured base");
    thps::ride_stop();check(status("stop_pending=1")&&status("mode=rec ")&&!fs::exists(configured),"stop waits for the next writer before closing input");
    read();check(status("reads=2")&&status("stop_pending=1"),"already configured writer retains its input after stop request");
    writer();check(chosen==0x3C23D70A&&ticks==3&&original==0x3C23D70A&&status("draining=1")&&status("step_tick_writes=2"),"boundary writer computes ordinary float/ticks without configured output writes");
    read();check(status("draining=0")&&contents(configured).starts_with("# thps ride v3 thps2 reads=2 start=run version=thps2 code=us step=1/30 timing=frame-time-v2"),"N+1 writes explicit v3 metadata");
    chosen=0;natural_ticks=4;writer();check(chosen==0x3C23D70A&&ticks==4,"following ordinary float and changed natural ticks untouched");natural_ticks=3;reset();
    check(thps::ride_play(configured.string()),"v3 playback parses");
    ++thps::dd::generation;body=0x80000100;run=1;writer();read();writer();read();writer();
    check(chosen==0x3C23D70A&&ticks==3&&status("mode=off "),"EOF boundary writer float/ticks are ordinary before input hold");
    read();check(status("mode=off ")&&status("next=2")&&thps::ride_error()=="none","complete EOF drains timing");
    chosen=0;writer();check(chosen==0x3C23D70A&&ticks==3,"post EOF ordinary float/ticks untouched");reset();
    check(thps::ride_record_step((out/"missed.ride").string(),"1/30"),"missing hook case arms");
    ++thps::dd::generation;body=0x80000100;run=1;read();
    check(thps::ride_error()=="step-missing-writer"&&!fs::exists(out/"missed.ride"),"missing paired hook fails before input/file");reset();
    thps::ride_engine_live(true);
    check(!thps::ride_play(configured.string())&&thps::ride_error()=="step-unsupported-live","live cannot silently play v3");reset();
    thps::versions::current=&thps::test_eu;
    check(!thps::ride_record_step((out/"eu.ride").string(),"1/30")&&thps::ride_error()=="step-unsupported-version","Europe refuses unverified feature");reset();
    check(thps::ride_record_step((out/"nested.ride").string(),"1/30"),"nested recovery case arms");
    ++thps::dd::generation;body=0x80000100;run=1;
    thps_ride_step_begin(ram.data(),nullptr);thps_ride_step_begin(ram.data(),nullptr);thps_ride_step_end(ram.data(),nullptr);
    check(original==0x3C23D70A&&thps::ride_error()=="step-nested-writer","missing end/nested begin restores exact original");reset();
    thps::ride_record_step((out/"wrong-value.ride").string(),"1/30");++thps::dd::generation;body=0x80000100;run=1;
    thps_ride_step_begin(ram.data(),nullptr);chosen=0;ticks=4;thps_ride_step_end(ram.data(),nullptr);
    check(original==0x3C23D70A&&ticks==4&&status("step_tick_writes=0")&&thps::ride_error()=="step-writer-value","wrong float result restores input without forcing integer output");reset();
    thps::ride_record_step((out/"wrong-end.ride").string(),"1/30");++thps::dd::generation;body=0x80000100;run=1;
    thps_ride_step_begin(ram.data(),nullptr);chosen=original;thps_ride_step_end(nullptr,nullptr);
    check(original==0x3C23D70A&&thps::ride_error()=="step-end-rdram","wrong end callback restores before explicit failure");reset();
    thps::ride_record_step((out/"aborted.ride").string(),"1/30");++thps::dd::generation;body=0x80000100;run=1;writer();read();
    thps::ride_stop();thps::ride_quit();
    check(!fs::exists(out/"aborted.ride")&&thps::ride_error()=="step-quit-before-drain","quit cannot publish an undrained v3 recording");reset();
    thps::ride_record_step((out/"cancelled-arm.ride").string(),"1/30");thps::ride_stop();
    check(status("mode=off ")&&status("stop_pending=0")&&!fs::exists(out/"cancelled-arm.ride"),"unbound arm cancels immediately");reset();
    thps::ride_record_step((out/"bound-arm.ride").string(),"1/30");++thps::dd::generation;body=0x80000100;run=1;writer();thps::ride_stop();
    check(status("mode=rec-armed ")&&status("stop_pending=1"),"bound arm retains already chosen first input");
    read();writer();read();check(contents(out/"bound-arm.ride").find("reads=1")!=std::string::npos,"bound arm stops at subsequent ordinary writer");reset();
    thps::ride_record_step((out/"replace-bound.ride").string(),"1/30");++thps::dd::generation;body=0x80000100;run=1;writer();
    check(!thps::ride_record((out/"replace-legacy.ride").string())&&thps::ride_error()=="step-stop-and-drain-first"&&status("mode=rec-armed ")&&status("error=none"),"legacy replacement refuses bound arm without poisoning session");
    check(!thps::ride_play(configured.string())&&thps::ride_error()=="step-stop-and-drain-first"&&status("mode=rec-armed ")&&status("error=none"),"play replacement refuses bound arm without poisoning session");
    read();check(!thps::ride_record_step((out/"invalid-option.ride").string(),"1/60")&&thps::ride_error()=="step-unsupported-value"&&status("error=none"),"bad replacement option is a request error only");
    thps::ride_stop();writer();read();check(contents(out/"replace-bound.ride").find("reads=1")!=std::string::npos&&status("error=none"),"valid session completes after refused replacement");reset();
    thps::ride_play(configured.string());++thps::dd::generation;body=0x80000100;run=1;writer();
    check(!thps::ride_record((out/"replace-playing.ride").string())&&thps::ride_error()=="step-stop-and-drain-first"&&status("mode=play-armed ")&&status("error=none"),"record replacement refuses bound playback arm without poisoning session");
    check(!thps::ride_play(configured.string())&&thps::ride_error()=="step-stop-and-drain-first"&&status("mode=play-armed ")&&status("error=none"),"play replacement refuses bound playback arm without poisoning session");reset();
    // A controller stop cannot publish or cancel halfway through a writer scope.
    reset();thps::ride_record_step((out/"threaded.ride").string(),"1/30");
    ++thps::dd::generation;body=0x80000100;run=1;writer();read();
    thps_ride_step_begin(ram.data(),nullptr);
    std::promise<void> entered;auto ready=entered.get_future();
    auto stop=std::async(std::launch::async,[&](){entered.set_value();thps::ride_stop();});
    ready.wait();check(stop.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout,"concurrent stop waits for paired restoration");
    chosen=original;ticks=4;thps_ride_step_end(ram.data(),nullptr);stop.get();
    check(original==0x3C23D70A&&status("installed=0")&&status("stop_pending=1"),"stop sees restored original and requests next writer boundary");
    read();writer();read();check(fs::exists(out/"threaded.ride"),"threaded stop retains chosen input then publishes at ordinary boundary");reset();
    reset();thps::ride_hold(true);thps::ride_record_step((out/"held.ride").string(),"1/30");
    ++thps::dd::generation;body=0x80000100;run=1;writer();read();thps::ride_stop();writer();
    auto held=std::async(std::launch::async,read);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!thps::ride_is_held()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    check(thps::ride_is_held()&&original==0x3C23D70A&&ticks==3&&status("installed=0")&&fs::exists(out/"held.ride"),"capture hold begins only after ordinary float/ticks and restored override/publication");
    thps::ride_hold(false);held.get();reset();
    // New-format failures must not create an apparently valid recording.
    auto invalid=[&](const char* name,const std::string& text){const auto path=out/name;std::ofstream(path)<<text;return !thps::ride_play(path.string());};
    const std::string header="# thps ride v3 thps2 reads=1 start=run version=thps2 code=us step=1/30 timing=frame-time-v2\n";
    auto old=header;old.replace(old.find("frame-time-v2"),13,"frame-time-v1");
    check(invalid("old-experimental.ride",old+"0000 0 0 1\n")&&thps::ride_error()=="step-unsupported-timing","old experimental float-only metadata explicitly refuses");
    check(invalid("nan.ride",header+"0000 nan 0 1\n"),"v3 rejects nonfinite input in actual file parser");
    check(invalid("short.ride",header),"v3 rejects count without input");
    check(invalid("extra.ride",header+"0000 0 0 1\n0000 0 0 1\n"),"v3 rejects extra input beyond declared count");
    check(invalid("duplicate.ride",header+header+"0000 0 0 1\n"),"v3 rejects duplicate timing declarations");
    auto wrong=header;wrong.replace(wrong.find("code=us"),7,"code=eu");
    check(invalid("version.ride",wrong+"0000 0 0 1\n")&&thps::ride_error()=="step-recording-version","v3 rejects selected code mismatch");reset();
    wrong=header;wrong.replace(wrong.find("start=run"),9,"start=course");
    check(invalid("course.ride",wrong+"0000 0 0 1\n")&&thps::ride_error()=="step-unsupported-boundary","v3 refuses unaudited course-start boundary");reset();
    thps::versions::tv_override=0;
    check(!thps::ride_record_step((out/"pal.ride").string(),"1/30")&&thps::ride_error()=="step-unsupported-tv","PAL override refuses");
    thps::versions::tv_override=-1;reset();
    thps::ride_record_step(out.string(),"1/30");++thps::dd::generation;body=0x80000100;run=1;writer();read();thps::ride_stop();writer();read();
    check(status("error=write")&&status("draining=0"),"write failure survives successful final drain");reset();
    thps::ride_record_step((out/"epoch-ended.ride").string(),"1/30");++thps::dd::generation;body=0x80000100;run=1;writer();read();
    ++thps::dd::generation;read();
    check(contents(out/"epoch-ended.ride").find("reads=1")!=std::string::npos&&status("draining=0"),"epoch mismatch at N+1 publishes without an extra input");reset();
    // A two-input file keeps early epoch termination distinguishable from EOF.
    auto two=header;two.replace(two.find("reads=1"),7,"reads=2");
    const auto two_path=out/"two.ride";std::ofstream(two_path)<<two<<"0000 0 0 1\n0000 0 0 1\n";
    thps::ride_play(two_path.string());++thps::dd::generation;body=0x80000100;run=1;writer();read();++thps::dd::generation;read();
    check(thps::ride_error()=="step-early-lifetime-end"&&status("next=1"),"epoch mismatch at input cannot silently complete partial v3 playback");reset();
    // Body removal suppresses timing before a frontend writer, even when run is zero.
    thps::ride_record_step((out/"ended.ride").string(),"1/30");++thps::dd::generation;body=0x80000100;run=1;writer();read();
    body=0;run=0;chosen=0;writer();
    check(chosen==0x3C23D70A&&ticks==3&&status("mode=off ")&&status("draining=1"),"body removal stops both configured outputs before frontend work");
    read();check(contents(out/"ended.ride").find("reads=1")!=std::string::npos,"lifetime-ended record contains only configured inputs");reset();
    thps::ride_play(configured.string());++thps::dd::generation;body=0x80000100;run=1;writer();
    body=0;run=0;writer();
    check(thps::ride_error()=="step-early-lifetime-end"&&status("next=0"),"early playback lifetime end is explicit failure");reset();
    thps::ride_play(configured.string());++thps::dd::generation;body=0x80000100;run=1;writer();read();writer();read();read();
    check(thps::ride_error()=="step-missing-boundary-writer","EOF cannot silently pass without the ordinary next writer");reset();
    thps::code_classes[0].ride_step_frame_ticks=0;
    check(!thps::ride_record_step((out/"missing-ticks.ride").string(),"1/30")&&thps::ride_error()=="step-missing-profile","missing integer profile fails closed");
    thps::code_classes[0].ride_step_frame_ticks=0x80000028;
    check(!thps::ride_record_step((out/"aliased-ticks.ride").string(),"1/30")&&thps::ride_error()=="step-missing-profile","aliased integer profile fails closed");
    thps::code_classes[0].ride_step_frame_ticks=0x8000002C;
    reset();
    // An input read inside an unclosed writer scope recovers instead of relocking the writer's mutex.
    thps::ride_record_step((out/"unclosed.ride").string(),"1/30");++thps::dd::generation;body=0x80000100;run=1;
    thps_ride_step_begin(ram.data(),nullptr);read();
    check(original==0x3C23D70A&&thps::ride_error()=="step-unclosed-writer"&&status("mode=off ")&&!fs::exists(out/"unclosed.ride"),"read after a missed end hook restores and fails without deadlock");
    writer();check(original==0x3C23D70A&&ticks==3,"writers after the recovery are ordinary");reset();
    // A refused playback request leaves the current recording running.
    const auto kept=out/"kept.ride";
    thps::ride_record(kept.string());++thps::dd::generation;body=0x80000100;run=1;writer();read();
    check(!thps::ride_play((out/"absent.ride").string())&&thps::ride_error()=="input-file"&&status("mode=rec ")&&!fs::exists(kept),"missing playback file keeps the recording");
    check(!thps::ride_play(configured.string())&&thps::ride_error()=="step-present-body-arm"&&status("mode=rec ")&&!fs::exists(kept),"refused v3 playback keeps the recording");
    read();thps::ride_stop();check(contents(kept).starts_with("# thps ride v2 thps2 reads=2 start=run"),"kept recording writes every read at stop");reset();
    // Playing the legacy recording in progress writes it first, then reads it back.
    const auto again=out/"again.ride";
    thps::ride_record(again.string());++thps::dd::generation;body=0x80000100;run=1;writer();read();
    check(thps::ride_play(again.string())&&status("mode=play-armed ")&&contents(again).starts_with("# thps ride v2 thps2 reads=1 start=run"),"same-path playback publishes the recording first");reset();
    check(thps::ride_set_step("1/30")&&!thps::ride_play(legacy.string())&&thps::ride_error()=="step-with-legacy-file","explicit configured mode never reinterprets v2");
    std::printf("%u checks, %u failed; synthetic host only\n",n,bad);return bad?1:0;
}
