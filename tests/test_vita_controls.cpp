#include "../platform/vita/controls.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (0)
int main() {
    vita::Controls controls;
    auto in = controls.sample({});
    CHECK(in.steer == 0x80 && in.accel == 0x20 && in.brake == 0x20);
    CHECK(in.in0 == 0xff && in.in1 == 0xaf && in.in2 == 0xff);
    int last = 0;
    for (int v = 0; v <= 255; ++v) {
        in = controls.sample({0, uint8_t(v), 128});
        CHECK(in.steer >= 0x20 && in.steer <= 0xe0 && in.steer >= last);
        last = in.steer;
    }
    CHECK(controls.sample({0, 0, 128}).steer == 0x20);
    CHECK(controls.sample({0, 255, 128}).steer == 0xe0);
    CHECK(controls.sample({0, 128, 0}).accel == 0xe0);
    CHECK(controls.sample({0, 128, 255}).brake == 0xe0);
    CHECK(controls.sample({vita::L | vita::R}).accel == 0xe0);
    CHECK(controls.sample({vita::L | vita::R}).brake == 0xe0);
    CHECK(controls.sample({vita::Left}).steer == 0x20);
    CHECK(controls.sample({vita::Right}).steer == 0xe0);
    CHECK(controls.sample({vita::Left | vita::Right}).steer == 0x80);
    controls.sample({});
    controls.sample({vita::Up}); CHECK(controls.gear() == 2);
    for (int i = 0; i < 10; ++i) controls.sample({vita::Up});
    CHECK(controls.gear() == 2);
    for (int i = 0; i < 10; ++i) { controls.sample({}); controls.sample({vita::Up}); }
    CHECK(controls.gear() == 4);
    CHECK((controls.sample({}).in1 & 0x70) == 0x50);
    for (int i = 0; i < 10; ++i) { controls.sample({}); controls.sample({vita::Down}); }
    CHECK(controls.gear() == 1);
    in = controls.sample({vita::Cross | vita::Circle | vita::Square | vita::Triangle});
    CHECK((in.in0 & 0xe0) == 0 && (in.in1 & 1) == 0);
    CHECK(controls.sample({vita::Select}).in0 == 0xff);
    CHECK((controls.sample({}).in0 & 1) == 0);
    CHECK((controls.sample({vita::Start}).in0 & 0x10) == 0);
    CHECK(controls.sample({vita::Start | vita::Select}).in0 == 0xff);
    CHECK(vita::menu_chord(vita::Start | vita::Select));
    CHECK(!vita::menu_chord(vita::Start));
    in = controls.sample({vita::Select | vita::Triangle});
    CHECK(in.in0 == 0xfb && (in.in1 & 1) != 0); // Test, no coin/view.
    in = controls.sample({vita::Select | vita::Square});
    CHECK(in.in0 == 0xf7); // Service, no coin/view.
    CHECK(controls.sample({}).in0 == 0xff);
    controls.sample({vita::Select});
    CHECK(controls.sample({vita::Select | vita::Triangle}).in0 == 0xfb);
    CHECK(controls.sample({}).in0 == 0xff);
    CHECK(controls.sample({vita::Start | vita::Select | vita::Triangle}).in0 == 0xff);
    // Test every digital combination; no ADC or active-low port overflow.
    for (uint32_t buttons = 0; buttons < 4096; ++buttons) {
        in = controls.sample({buttons});
        CHECK(in.steer >= 32 && in.steer <= 224);
        CHECK(in.accel >= 32 && in.accel <= 224);
        CHECK(in.brake >= 32 && in.brake <= 224);
        CHECK(controls.gear() >= 1 && controls.gear() <= 4);
        if (vita::menu_chord(buttons)) CHECK(in.in0 == 0xff);
    }
    for (int curve = -1; curve <= 3; ++curve) {
        vita::Controls curved;
        curved.set_steer_curve(curve);
        int previous = 0;
        for (int v = 0; v < 256; ++v) {
            const int steer = curved.sample({0, uint8_t(v), 128}).steer;
            CHECK(steer >= previous && steer >= 32 && steer <= 224);
            previous = steer;
        }
        CHECK(curved.sample({0, 0, 128}).steer == 32);
        CHECK(curved.sample({0, 128, 128}).steer == 128);
        CHECK(curved.sample({0, 255, 128}).steer == 224);
        CHECK(curved.sample({vita::Left, 255, 128}).steer == 32);
        CHECK(curved.sample({vita::Right, 0, 128}).steer == 224);
        const int right = curved.sample({0, 192, 128}).steer;
        CHECK(right <= (curve <= 0 ? 171 : curve == 1 ? 147 : 136));
        curved.set_steer_invert(true);
        CHECK(curved.sample({0, 192, 128}).steer == 256 - right);
    }
    // Rebinding: one button per action; taking a used button swaps the two.
    {
        vita::Bindings b;
        b.bind(vita::Accelerate, vita::Cross);
        CHECK(b.button[vita::Accelerate] == vita::Cross && b.button[vita::View1] == vita::R);
        b.bind(vita::ShiftUp, vita::Triangle);
        CHECK(b.button[vita::ShiftUp] == vita::Triangle && b.button[vita::View4] == vita::Up);
        b.bind(vita::Brake, vita::Cross | vita::Circle); // not a single button: ignored
        b.bind(vita::ActionCount, vita::Cross);
        CHECK(b.button[vita::Brake] == vita::L);
        vita::Controls remapped;
        remapped.set_bindings(b);
        CHECK(remapped.sample({vita::Cross}).accel == 0xe0);
        CHECK(remapped.sample({vita::R}).accel == 0x20);
        in = remapped.sample({vita::R});
        CHECK((in.in0 & 0x20) == 0);
        remapped.sample({});
        remapped.sample({vita::Triangle}); CHECK(remapped.gear() == 2);
        remapped.sample({});
        remapped.sample({vita::Up}); CHECK(remapped.gear() == 2);
        in = remapped.sample({vita::Up});
        CHECK((in.in1 & 1) == 0);
        // Coin on another button still fires on release; Select chords stay fixed.
        b = vita::Bindings{};
        b.bind(vita::Coin, vita::L);
        CHECK(b.button[vita::Brake] == vita::Select);
        remapped.set_bindings(b);
        remapped.sample({});
        CHECK(remapped.sample({vita::L}).in0 == 0xff);
        CHECK((remapped.sample({}).in0 & 1) == 0);
        CHECK(remapped.sample({vita::Select | vita::Triangle}).in0 == 0xfb);
        CHECK(remapped.sample({vita::Start | vita::Select}).in0 == 0xff);
        // Cycling visits all twelve buttons and keeps every binding unique.
        b = vita::Bindings{};
        for (int i = 0; i < 12; ++i) {
            b.cycle(vita::Coin, 1);
            uint32_t seen = 0;
            for (uint32_t v : b.button) { CHECK(vita::single_button(v) && !(seen & v)); seen |= v; }
        }
        CHECK(b.button[vita::Coin] == vita::Select);
        b = vita::Bindings{};
        b.cycle(vita::Coin, -1);
        CHECK(b.button[vita::Coin] == vita::Start && b.button[vita::StartButton] == vita::Select);
        // Hand-edited duplicates reset the whole set.
        b.button[vita::View1] = b.button[vita::View2];
        b.sanitize();
        CHECK(b.button[vita::View1] == vita::Cross && b.button[vita::Coin] == vita::Select);
        CHECK(!std::strcmp(vita::button_name(vita::Select), "SELECT"));
        CHECK(!std::strcmp(vita::button_name(0), "NONE"));
    }
    vita::FrameClock clock(16000000.0 / (656.0 * 424.0));
    int frames = 0;
    for (int i = 0; i < 6000; ++i) frames += clock.advance(1.0 / 60.0);
    CHECK(frames == 5752);
    // Shipping GXM loop permits only one simulation step per presentation.
    // Fractional carry must still avoid an accidental half-refresh cap.
    vita::FrameClock presentation_clock(16000000.0 / (656.0 * 424.0), 1);
    int presented_frames = 0;
    for (int i = 0; i < 6000; ++i) {
        const int steps = presentation_clock.advance(1.0 / 60.0);
        CHECK(steps >= 0 && steps <= 1);
        presented_frames += steps;
    }
    CHECK(presented_frames == 5752);
    CHECK(clock.advance(20.0) <= 4);
    clock.reset(); CHECK(clock.advance(0) == 0);
    CHECK(clock.advance(-1) == 0);
    CHECK(clock.advance(std::numeric_limits<double>::infinity()) == 0);
    CHECK(clock.advance(std::numeric_limits<double>::quiet_NaN()) == 0);
    std::puts("Vita input and timing tests passed (all 256 axis values, 4096 button combinations, 100 seconds of pacing)");
}
