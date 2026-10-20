// platform/dreamcast/game/controls.h on the PC: every stick value, the
// triggers, the D-pad, gears on presses only, each button's bit.
#include "../game/controls.h"

#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;
void expect(bool ok, const char *what, int value = 0) {
    if (!ok) {
        std::printf("FAIL: %s (%d)\n", what, value);
        ++failures;
    }
}
} // namespace

int main() {
    // Steering: monotonic over the whole stick range, centred inside the
    // deadzone, 0x20..0xe0 at the ends.
    {
        dc::Controls c;
        int last = -1;
        for (int x = -128; x <= 127; ++x) {
            dc::Pad pad;
            pad.joyx = x;
            const int steer = c.sample(pad).steer;
            expect(steer >= last, "steer is monotonic", x);
            expect(steer >= 0x20 && steer <= 0xe0, "steer within 0x20..0xe0", x);
            last = steer;
        }
        dc::Pad pad;
        pad.joyx = 5;
        expect(c.sample(pad).steer == 0x80, "small stick values are centre");
        pad.joyx = -128;
        expect(c.sample(pad).steer == 0x20, "full left is 0x20");
        pad.joyx = 127;
        expect(c.sample(pad).steer == 0xe0, "full right is 0xe0");
        pad = {};
        pad.buttons = dc::Left;
        expect(c.sample(pad).steer == 0x20, "D-pad left is full left");
        pad.buttons = dc::Right;
        expect(c.sample(pad).steer == 0xe0, "D-pad right is full right");
    }
    // Pedals: the triggers, 0x20 released to 0xe0 fully pressed.
    {
        dc::Controls c;
        dc::Pad pad;
        dc::Input in = c.sample(pad);
        expect(in.accel == 0x20 && in.brake == 0x20, "pedals released");
        pad.rtrig = 255;
        pad.ltrig = 255;
        in = c.sample(pad);
        expect(in.accel == 0xe0 && in.brake == 0xe0, "pedals pressed");
    }
    // Gears: one step per press, held buttons do not repeat, clamped to 1..4.
    {
        dc::Controls c;
        dc::Pad pad;
        for (int i = 0; i < 10; ++i) {
            pad.buttons = dc::Up;
            c.sample(pad);
            pad.buttons = 0;
            c.sample(pad);
        }
        expect(c.gear() == 4, "gear clamps at 4", c.gear());
        pad.buttons = dc::Down;
        for (int i = 0; i < 5; ++i) c.sample(pad); // held: one shift only
        expect(c.gear() == 3, "a held button shifts once", c.gear());
        constexpr unsigned codes[] = {0, 2, 1, 6, 5};
        dc::Controls g;
        for (int gear = 1; gear <= 4; ++gear) {
            dc::Pad p;
            const dc::Input in = g.sample(p);
            expect(unsigned((in.in1 >> 4) & 7) == codes[gear], "gearbox code", gear);
            p.buttons = dc::Up;
            g.sample(p);
        }
    }
    // Buttons: each its active-low bit, nothing else.
    {
        const struct { uint32_t button; int port; uint8_t bit; } map[] = {
            {dc::Y, 0, 0x01}, {dc::Start, 0, 0x10}, {dc::A, 0, 0x20}, {dc::B, 0, 0x40}, {dc::X, 0, 0x80}};
        for (const auto &m : map) {
            dc::Controls c;
            dc::Pad pad;
            pad.buttons = m.button;
            const dc::Input in = c.sample(pad);
            expect(in.in0 == uint8_t(0xff & ~m.bit), "button bit in IN0", int(m.bit));
        }
    }
    std::printf("%s: %d failure%s\n", failures ? "test_controls FAILED" : "test_controls passed", failures,
                failures == 1 ? "" : "s");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
