// Strict opt-in configured-step metadata. Legacy input files keep their own parser.
#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace thps::ride_format {
struct Step {
    uint32_t numerator = 1, denominator = 30;
    bool operator==(const Step &) const = default;
};
enum class Start { Course, Run };
struct V3Header {
    std::string family, version, code_class;
    uint32_t reads = 0;
    Start start = Start::Run;
    Step step;
};
enum class HeaderError {
    None, NotV3, Malformed, FamilyMismatch, InvalidReadCount,
    UnsupportedStep, UnsupportedTiming
};
struct ParsedHeader {
    std::optional<V3Header> header;
    HeaderError error = HeaderError::Malformed;
};

inline bool identifier(std::string_view value) {
    if (value.empty() || value.size() > 31) return false;
    for (char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

// New-format parser only. A caller must route legacy v1/v2 through their exact
// existing parser, without applying fixed-step metadata or policy to them.
// Canonical declaration:
// # thps ride v3 thps2 reads=1690 start=run version=thps2 code=us step=1/30 timing=frame-time-v2
inline ParsedHeader parse_v3(std::string_view line, std::string_view expected_family) {
    auto fail = [](HeaderError error) { return ParsedHeader{std::nullopt, error}; };
    auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    if (line.size() > 1024) return fail(HeaderError::Malformed);
    std::array<std::string_view,11> fields;
    std::size_t count = 0, cursor = 0;
    while (cursor < line.size()) {
        while (cursor < line.size() && whitespace(line[cursor])) ++cursor;
        if (cursor == line.size()) break;
        const std::size_t begin = cursor;
        while (cursor < line.size() && !whitespace(line[cursor])) ++cursor;
        if (count == fields.size()) return fail(HeaderError::Malformed);
        fields[count++] = line.substr(begin, cursor - begin);
    }
    if (count < 4 || fields[0] != "#" || fields[1] != "thps" || fields[2] != "ride" || fields[3] != "v3")
        return fail(HeaderError::NotV3);
    if (count != fields.size()) return fail(HeaderError::Malformed);
    if (!identifier(fields[4])) return fail(HeaderError::Malformed);
    if (fields[4] != expected_family) return fail(HeaderError::FamilyMismatch);
    auto value = [&](std::size_t index, std::string_view prefix) -> std::optional<std::string_view> {
        return fields[index].starts_with(prefix) ? std::optional(fields[index].substr(prefix.size())) : std::nullopt;
    };
    const auto reads = value(5,"reads="), start = value(6,"start="), version = value(7,"version="),
               code = value(8,"code="), step = value(9,"step="), timing = value(10,"timing=");
    if (!reads || !start || !version || !code || !step || !timing ||
        !identifier(*version) || !identifier(*code)) return fail(HeaderError::Malformed);
    uint32_t read_count = 0;
    const auto converted = std::from_chars(reads->data(), reads->data() + reads->size(), read_count,10);
    if (converted.ec != std::errc{} || converted.ptr != reads->data() + reads->size() ||
        read_count == 0 || read_count > 1000000) return fail(HeaderError::InvalidReadCount);
    if (*start != "run" && *start != "course") return fail(HeaderError::Malformed);
    // Deliberately small initial configuration set. No float parser, implicit
    // normalization, zero/nonfinite/overflow or arbitrary game-speed control.
    if (*step != "1/30") return fail(HeaderError::UnsupportedStep);
    if (*timing != "frame-time-v2") return fail(HeaderError::UnsupportedTiming);
    return {V3Header{std::string(fields[4]),std::string(*version),std::string(*code),read_count,
                     *start == "run" ? Start::Run : Start::Course,Step{1,30}},HeaderError::None};
}

// Evidence vocabulary used by standalone metadata tests. Runtime support is
// deliberately narrower and checked against the selected class paired-writer profile.
enum class Engine { Static, Split, Live };
struct SelectedBuild {
    std::string_view family, version, code_class;
    Engine engine = Engine::Static;
    bool paired_writer_verified = false;
    bool lifetime_verified = false;
    bool library_imports_verified = false;
    bool start_boundary_verified = false;
};
enum class CapabilityError {
    None, VersionMismatch, UnsupportedEngine, UnsupportedStep, WriterUnverified,
    LifetimeUnverified, ImportsUnverified, StartBoundaryUnverified, ConflictingStep
};
inline CapabilityError check_v3(const V3Header &header, const SelectedBuild &build,
                               std::optional<Step> explicit_step = std::nullopt) {
    if (header.family != build.family || header.version != build.version || header.code_class != build.code_class)
        return CapabilityError::VersionMismatch;
    if (header.step != Step{1,30}) return CapabilityError::UnsupportedStep;
    if (build.engine == Engine::Live) return CapabilityError::UnsupportedEngine;
    if (!build.paired_writer_verified) return CapabilityError::WriterUnverified;
    if (!build.lifetime_verified) return CapabilityError::LifetimeUnverified;
    if (!build.library_imports_verified) return CapabilityError::ImportsUnverified;
    if (!build.start_boundary_verified) return CapabilityError::StartBoundaryUnverified;
    if (explicit_step && *explicit_step != header.step) return CapabilityError::ConflictingStep;
    return CapabilityError::None;
}
} // namespace thps::ride_format
