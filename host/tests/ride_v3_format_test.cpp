// Standalone ROM-free checks for configured-step metadata and evidence vocabulary.
// No host/runtime linking or gameplay claims from this test.
#include "../src/ride_v3_format.h"
#include <cstdio>
#include <string>

int main() {
    using namespace thps::ride_format;
    constexpr auto canonical = "# thps ride v3 thps2 reads=1690 start=run version=thps2 code=us step=1/30 timing=frame-time-v2";
    unsigned checks = 0, failures = 0;
    auto expect = [&](bool condition, const char *name) {
        ++checks;
        if (!condition) { ++failures; std::fprintf(stderr,"FAIL %s\n",name); }
    };
    auto replace = [&](std::string_view old, std::string_view value) {
        std::string result = canonical;
        const auto at = result.find(old);
        if (at == std::string::npos) { ++failures; return std::string{}; }
        result.replace(at,old.size(),value);
        return result;
    };
    auto rejected = [&](std::string_view line, HeaderError error, const char *name) {
        const auto parsed = parse_v3(line,"thps2");
        expect(!parsed.header && parsed.error == error,name);
    };
    const auto normal = parse_v3(canonical,"thps2");
    expect(normal.header.has_value() && normal.error == HeaderError::None,"canonical declaration");
    if (!normal.header) return 1;
    expect(normal.header->family == "thps2" && normal.header->version == "thps2" &&
           normal.header->code_class == "us" && normal.header->reads == 1690 &&
           normal.header->start == Start::Run && normal.header->step == Step{1,30},"all configured metadata");
    expect(parse_v3(" \t"+std::string(canonical)+"\r\n","thps2").header.has_value(),"whitespace CRLF");
    expect(parse_v3(replace("start=run","start=course"),"thps2").header->start == Start::Course,"course metadata parsed separately from capability");
    expect(parse_v3(replace("reads=1690","reads=1"),"thps2").header->reads == 1,"minimum count");
    expect(parse_v3(replace("reads=1690","reads=1000000"),"thps2").header->reads == 1000000,"maximum count");
    for (const auto value : {"0","1000001","4294967296","18446744073709551616","-1","+1","1x","","nan"})
        rejected(replace("reads=1690",std::string("reads=")+value),HeaderError::InvalidReadCount,"invalid/overflow count");
    rejected(replace("thps2 reads=","thps3 reads="),HeaderError::FamilyMismatch,"family mismatch");
    rejected(replace("version=thps2","version="),HeaderError::Malformed,"empty version");
    rejected(replace("version=thps2","version=../thps2"),HeaderError::Malformed,"path-like version");
    rejected(replace("code=us","code=US"),HeaderError::Malformed,"noncanonical code identifier");
    rejected(replace("code=us","code="+std::string(32,'a')),HeaderError::Malformed,"oversized identifier");
    expect(parse_v3(replace("code=us","code="+std::string(31,'a')),"thps2").header.has_value(),"identifier upper bound");
    rejected(replace("start=run","start=arbitrary"),HeaderError::Malformed,"unknown boundary");
    rejected(replace("code=us","version=thps2"),HeaderError::Malformed,"duplicate key replaces required key");
    rejected(replace("code=us","codeclass=us"),HeaderError::Malformed,"unknown key");
    rejected(replace("version=thps2 code=us","code=us version=thps2"),HeaderError::Malformed,"noncanonical key order");
    rejected(std::string(canonical)+" extra=1",HeaderError::Malformed,"trailing unknown field");
    rejected(replace(" timing=frame-time-v2",""),HeaderError::Malformed,"missing declaration");
    rejected(std::string(1025,' ')+canonical,HeaderError::Malformed,"bounded header line");
    for (const auto value : {"0/30","-1/30","1/0","1/60","2/60","nan","inf","0.0333333333","1/4294967296","1/30junk"})
        rejected(replace("step=1/30",std::string("step=")+value),HeaderError::UnsupportedStep,"unsupported/nonfinite/overflow step");
    rejected(replace("timing=frame-time-v2","timing=frame-time-v3"),HeaderError::UnsupportedTiming,"unknown timing algorithm");
    rejected(replace("timing=frame-time-v2","timing=frame-time-v1"),HeaderError::UnsupportedTiming,"old experimental float-only timing explicitly refuses");
    for (const auto value : {"# thps ride v1 thps2","# thps ride v2 thps2 reads=1 start=run","0000 0 0","# comment"})
        rejected(value,HeaderError::NotV3,"legacy routes elsewhere; no reinterpretation");

    auto build = SelectedBuild{"thps2","thps2","us"};
    expect(check_v3(*normal.header,build) == CapabilityError::WriterUnverified,"capability defaults refuse");
    build.paired_writer_verified = true;
    expect(check_v3(*normal.header,build) == CapabilityError::LifetimeUnverified,"lifetime evidence required");
    build.lifetime_verified = true;
    expect(check_v3(*normal.header,build) == CapabilityError::ImportsUnverified,"actual import capability required");
    build.library_imports_verified = true;
    expect(check_v3(*normal.header,build) == CapabilityError::StartBoundaryUnverified,"start boundary evidence required");
    build.start_boundary_verified = true;
    expect(check_v3(*normal.header,build) == CapabilityError::None,"explicitly evidenced static metadata");
    build.engine = Engine::Split;
    expect(check_v3(*normal.header,build,Step{1,30}) == CapabilityError::None,"explicitly evidenced split metadata");
    expect(check_v3(*normal.header,build,Step{1,60}) == CapabilityError::ConflictingStep,"explicit option conflicts with recording");
    build.engine = Engine::Live;
    expect(check_v3(*normal.header,build) == CapabilityError::UnsupportedEngine,"live refuses even with evidence flags");
    build.engine = Engine::Static;
    build.version = "thps2_eu";
    expect(check_v3(*normal.header,build) == CapabilityError::VersionMismatch,"selected actual version mismatch");
    build.version = "thps2"; build.code_class = "eu";
    expect(check_v3(*normal.header,build) == CapabilityError::VersionMismatch,"selected actual code class mismatch");
    build.code_class = "us"; build.family = "thps3";
    expect(check_v3(*normal.header,build) == CapabilityError::VersionMismatch,"selected actual family mismatch");
    build.family = "thps2";
    auto constructed = *normal.header; constructed.step = Step{2,60};
    expect(check_v3(constructed,build) == CapabilityError::UnsupportedStep,"unparsed configuration cannot normalize silently");
    std::printf("%u checks, %u failed; metadata only\n",checks,failures);
    return failures ? 1 : 0;
}
