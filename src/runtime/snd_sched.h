// Event schedule for the recompiled sound 68000, counted in completed
// instructions (the same model as the i960's rt::Lockstep). Recompiled code
// calls boundary() before each instruction; when the count reaches the next
// event the due callbacks run and, in free run, a pending interrupt the mask
// allows is taken. The lockstep checker instead takes interrupts where MAME's
// log says, from its own callbacks.
#pragma once

#include "runtime/snd_cpu.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>

namespace snd {

class Sched {
public:
    explicit Sched(Cpu68k &c) : c_(c) {}

    uint64_t count = 0;              // completed instructions
    uint64_t end_count = UINT64_MAX; // recompiled code returns once this is reached
    uint64_t next_count = UINT64_MAX;
    bool auto_irq = true;            // free run: take c.irq_line when the mask allows

    bool boundary() {
        if (count < next_count && count < end_count) return false;
        return apply();
    }
    bool finished() const { return count >= end_count; }

    // Run fn once `at` instructions have completed (at >= count).
    void at(uint64_t when, std::function<void()> fn) {
        events_.emplace(std::max(when, count), std::move(fn));
        next_count = std::min(next_count, events_.begin()->first);
    }
    // The interrupt line or mask changed: re-check at the next boundary.
    void poke() { next_count = std::min(next_count, count + 1); }

private:
    bool apply() {
        const uint32_t pc = c_.pc;
        while (!events_.empty() && events_.begin()->first <= count) {
            auto fn = std::move(events_.begin()->second);
            events_.erase(events_.begin());
            fn();
        }
        if (auto_irq && c_.irq_line > c_.mask()) c_.take_irq(c_.irq_line);
        next_count = events_.empty() ? UINT64_MAX : events_.begin()->first;
        return c_.pc != pc || finished();
    }

    Cpu68k &c_;
    std::multimap<uint64_t, std::function<void()>> events_;
};

} // namespace snd
