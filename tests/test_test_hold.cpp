// rt::TestHold: waits for frame 240, holds Test (in0 0x04) for 173 frames,
// says when it is done, and leaves the other inputs alone.
#include "runtime/test_hold.h"

#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
} // namespace

int main() {
    using rt::TestHold;
    {   // armed at power-on: nothing until frame 240, then 173 frames held
        TestHold h;
        h.arm();
        uint64_t held = 0, first = 0, done_at = 0;
        bool others = true;
        for (uint64_t frame = 0; frame < 1000; ++frame) {
            uint8_t in0 = 0xff & ~0x10; // start held by the player: kept
            const bool done = h.apply(frame, in0);
            if (!(in0 & 0x04)) held += 1, first = first ? first : frame;
            others &= (in0 & 0x10) == 0 && (in0 | 0x04 | 0x10) == 0xff;
            if (done) done_at = frame;
        }
        check(first == TestHold::kEarliest, "the hold starts at frame 240");
        check(held == TestHold::kFrames, "held 173 frames");
        check(done_at == TestHold::kEarliest + TestHold::kFrames - 1, "done on the last held frame");
        check(others, "other inputs untouched");
        check(!h.armed(), "disarmed when done");
    }
    {   // armed on a running game: straight away
        TestHold h;
        h.arm();
        uint8_t in0 = 0xff;
        h.apply(5000, in0);
        check(!(in0 & 0x04), "held at once on a running game");
        h.cancel();
        in0 = 0xff;
        check(!h.apply(5001, in0) && in0 == 0xff, "cancelled: let go");
    }
    {   // not armed: nothing
        TestHold h;
        uint8_t in0 = 0xff;
        check(!h.apply(5000, in0) && in0 == 0xff, "not armed: nothing held");
    }
    if (failures) return 1;
    std::printf("test_test_hold: ok\n");
    return 0;
}
