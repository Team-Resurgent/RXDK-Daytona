// Optional host-side timing. Counters never control guest execution.
#pragma once
#include <cstdint>
#include <initializer_list>

namespace rt {
struct FrameProfile {
    uint64_t total = 0, geometry = 0, video = 0, sound = 0;
    uint64_t core() const {
        // i960, synchronous TGP work and scheduling, excluding the stages below.
        uint64_t rest = total;
        for (uint64_t part : {geometry, video, sound})
            rest = part < rest ? rest - part : 0;
        return rest;
    }
};

class FrameProfiler {
public:
    using Clock = uint64_t (*)(); // Monotonic host ticks; all users use one clock.
    struct Scope {
        Scope(Clock clock, uint64_t &counter)
            : clock_(clock), counter_(counter), start_(clock ? clock() : 0) {}
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
        ~Scope() {
            if (!clock_) return;
            const uint64_t end = clock_();
            if (end >= start_) counter_ += end - start_;
        }
    private:
        Clock clock_;
        uint64_t &counter_;
        uint64_t start_;
    };
    void set_clock(Clock clock) { clock_ = clock; reset(); }
    void reset() { frame = {}; }
    Scope measure(uint64_t &counter) { return Scope(clock_, counter); }
    FrameProfile frame;
private:
    Clock clock_ = nullptr; // Desktop callers retain their existing behavior.
};
} // namespace rt
