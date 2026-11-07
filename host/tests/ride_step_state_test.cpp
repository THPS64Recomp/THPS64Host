// ROM-free behavior gates for configured-ride lifetime and bit-exact restoration.
#include "../src/ride_step_state.h"
#include <cstdio>
#include <initializer_list>
int main() {
    using namespace thps::ride_step;
    unsigned n=0,bad=0;
    auto check=[&](bool ok,const char* label){++n;if(!ok){++bad;std::fprintf(stderr,"FAIL %s\n",label);}};
    State s;
    check(!s.arm(7,0x80001000)&&s.error==Error::PresentBodyArm,"present-body arming refuses");
    check(s.arm(7,0),"frontend arm");
    check(s.writer(7,0x80002000,true)==Decision::Wait,"old course does not configure");
    check(s.writer(8,0,false)==Decision::Wait,"setup generation before body remains armed");
    check(s.writer(8,0x80002000,false)==Decision::Wait,"goals before run remains armed");
    check(s.writer(8,0x80002000,true)==Decision::Apply&&s.generation==8&&s.body==0x80002000,"first eligible writer binds actual identity");
    check(s.input_ready(8,0x80002000),"first input sees prior configured writer");
    check(s.writer(8,0x80002000,false)==Decision::Apply,"loaded-course pause/statistics keeps base step");
    s.close();check(s.phase==Phase::Closing,"stop enters final-input drain");
    check(s.writer(8,0x80002000,false)==Decision::Wait,"first writer after completed final input is ordinary");
    s.input_boundary();check(!s.enabled()&&s.writer(8,0x80002000,true)==Decision::Wait,"N+1 ends timing before hold");
    check(s.arm(8,0),"next arm after old removal");
    check(s.writer(9,0x80002000,true)==Decision::Apply,"reused pointer binds new generation");
    check(s.writer(9,0,false)==Decision::LifetimeEnd&&!s.enabled(),"body removal suppresses next frontend writer");
    s.arm(9,0);s.writer(10,0x80002000,true);
    check(s.writer(11,0x80002000,true)==Decision::LifetimeEnd,"same pointer new generation ends old timing");
    s.arm(11,0);s.writer(12,0x80002000,true);
    check(s.writer(12,0x80003000,true)==Decision::LifetimeEnd,"changed selected body ends timing");
    s.arm(12,0);check(!s.input_ready(13,0x80002000)&&s.error==Error::MissingWriter,"missing hook cannot silently begin input");
    s.arm(13,0);check(s.writer(15,0x80002000,true)==Decision::Error&&s.error==Error::MissedGeneration,"missed setup epoch fails");
    s.arm(15,0);s.close();check(!s.enabled(),"stop before activation");
    s.arm(15,0);s.cancel();check(!s.enabled(),"quit/cancel suppresses future scopes");
    for(uint32_t prior:{0u,0x3C23D70Au,0x80000000u,0x7FC01234u}) {
        volatile uint32_t word=prior;
        { Override scope;check(scope.install(&word,0x3D088889u)&&word==0x3D088889u,"install configured own float bits");
          check(!scope.install(&word,0),"nested install refuses");
          check(scope.restore()&&word==prior,"exact original bits restored");
          check(!scope.restore(),"duplicate end harmless"); }
        { Override scope;scope.install(&word,0x3D088889u); }
        check(word==prior,"scope cleanup restores after error/cancel path");
    }
    std::printf("%u checks, %u failed\n",n,bad);return bad?1:0;
}
