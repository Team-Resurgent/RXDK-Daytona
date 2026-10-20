// The cabinet's Test button held for the player, for frontends without one
// (mobile, consoles): the launcher's "Hold Test button" opens the game's test
// menu (coin settings, game type, cabinet). Any button bound to Test still
// works as well.
//
// Measured with recorded inputs (m2run, both sets): the game wants a fresh
// press, from about frame 200 after power-on. A press already down then is
// ignored, even held to frame 287; one starting at frame 200 or later enters
// the test menu, and a 3 s hold gives the same screen as a 10-frame press
// (the menu's own Test-selects-an-item does not fire). So the hold waits for
// frame 240, holds 3 s and lets go.
#pragma once

#include <cstdint>

namespace rt {

class TestHold {
public:
    static constexpr uint64_t kEarliest = 240; // game frames after power-on
    static constexpr uint64_t kFrames = 173;   // 3 s at 57.52 frames/s

    void arm() { left_ = kFrames; }
    void cancel() { left_ = 0; }
    bool armed() const { return left_ > 0; }

    // Before each game frame: holds Test (in0 bit 0x04, active low) while
    // armed and the game has run `frame` frames. True on the frame the hold
    // finishes, to clear the frontend's checkbox.
    bool apply(uint64_t frame, uint8_t &in0) {
        if (!left_ || frame < kEarliest) return false;
        in0 = uint8_t(in0 & ~0x04);
        return --left_ == 0;
    }

private:
    uint64_t left_ = 0;
};

} // namespace rt
