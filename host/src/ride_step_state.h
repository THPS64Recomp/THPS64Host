// Own configured-ride lifetime state. ROM-free: no addresses, guest writes or IO.
#pragma once
#include <cstdint>
namespace thps::ride_step {
enum class Phase { Off, Armed, Bound, Closing, Failed };
enum class Decision { Wait, Apply, LifetimeEnd, Error };
enum class Error { None, PresentBodyArm, MissedGeneration, MissingWriter, Identity, NestedWriter, WriterValue, Unsupported };
inline const char* name(Error error) {
    switch (error) {
    case Error::None:return "none";
    case Error::PresentBodyArm:return "step-present-body-arm";
    case Error::MissedGeneration:return "step-missed-generation";
    case Error::MissingWriter:return "step-missing-writer";
    case Error::Identity:return "step-identity";
    case Error::NestedWriter:return "step-nested-writer";
    case Error::WriterValue:return "step-writer-value";
    case Error::Unsupported:return "step-unsupported";
    }
    return "step-error";
}
struct State {
    Phase phase = Phase::Off;
    Error error = Error::None;
    uint64_t armed_generation = 0, generation = 0, writers = 0;
    uint32_t body = 0;
    bool arm(uint64_t current_generation, uint32_t current_body) {
        *this = {};
        if (current_body) { fail(Error::PresentBodyArm); return false; }
        armed_generation = current_generation; phase = Phase::Armed; return true;
    }
    void fail(Error why) { error = why; phase = Phase::Failed; }
    bool enabled() const { return phase == Phase::Armed || phase == Phase::Bound || phase == Phase::Closing; }
    Decision writer(uint64_t current_generation, uint32_t selected, bool run) {
        if (phase == Phase::Off) return Decision::Wait;
        if (phase == Phase::Failed) return Decision::Error;
        if (phase == Phase::Closing) return Decision::Wait; // next writer is ordinary; N+1 still drains/holds
        if (phase == Phase::Armed) {
            if (current_generation == armed_generation) return Decision::Wait;
            if (current_generation < armed_generation || current_generation - armed_generation != 1) {
                fail(Error::MissedGeneration); return Decision::Error;
            }
            if (!selected || !run) return Decision::Wait;
            generation = current_generation; body = selected; phase = Phase::Bound;
        } else if (current_generation != generation || !selected || selected != body) {
            phase = Phase::Off; return Decision::LifetimeEnd;
        }
        ++writers; return Decision::Apply;
    }
    bool input_ready(uint64_t current_generation, uint32_t selected) {
        if (phase == Phase::Bound && writers && generation == current_generation && body == selected) return true;
        fail(phase == Phase::Armed ? Error::MissingWriter : Error::Identity); return false;
    }
    void close() {
        if (phase == Phase::Bound) phase = Phase::Closing;
        else if (phase == Phase::Armed) phase = Phase::Off;
    }
    void input_boundary() { if (phase == Phase::Closing) phase = Phase::Off; }
    void cancel() { phase = Phase::Off; }
};
// Restores raw bits, including nonzero overrides, signed zero and NaN payloads.
// The runtime owns synchronization; this object never holds a lock across IO.
struct Override {
    volatile uint32_t* word = nullptr;
    uint32_t saved = 0;
    bool install(volatile uint32_t* target, uint32_t configured) {
        if (word || !target) return false;
        word = target; saved = *word; *word = configured; return true;
    }
    bool restore() {
        if (!word) return false;
        *word = saved; word = nullptr; return true;
    }
    ~Override() { restore(); }
    Override() = default;
    Override(const Override&) = delete;
    Override& operator=(const Override&) = delete;
};
} // namespace thps::ride_step
