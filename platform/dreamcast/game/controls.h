// Dreamcast controller to the cabinet's inputs, independent of KOS so it can
// be tested on the PC (as platform/vita/controls.h, whose mapping and gearbox
// encoding it follows). A standard controller or the Racing Controller (its
// wheel is the stick axis, its pedals the triggers).
//
//   stick / wheel, or D-pad left and right   steer
//   right trigger / left trigger             accelerate / brake
//   D-pad up / down                          shift up / down (gears 1-4, on presses)
//   A, B, X                                  view buttons VR1, VR2, VR3
//   Y                                        coin
//   Start                                    start
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dc {

// KOS's cont_state_t buttons (dc/maple/controller.h), repeated so this
// header does not need KOS.
enum Button : uint32_t {
    B = 1u << 1, A = 1u << 2, Start = 1u << 3,
    Up = 1u << 4, Down = 1u << 5, Left = 1u << 6, Right = 1u << 7,
    Y = 1u << 9, X = 1u << 10
};

struct Pad {
    uint32_t buttons = 0;
    int joyx = 0;            // -128..127
    int ltrig = 0, rtrig = 0; // 0..255
};

struct Input { // as rt::Inputs
    uint8_t steer = 0x80, accel = 0x20, brake = 0x20;
    uint8_t in0 = 0xff, in1 = 0x8f, in2 = 0xff;
};

class Controls {
public:
    Input sample(Pad pad) {
        Input in;
        const uint32_t pressed = pad.buttons & ~held_;
        held_ = pad.buttons;
        float steer = axis(pad.joyx);
        if (pad.buttons & (Left | Right)) steer = float(bool(pad.buttons & Right)) - float(bool(pad.buttons & Left));
        const float accel = std::clamp(pad.rtrig, 0, 255) / 255.f, brake = std::clamp(pad.ltrig, 0, 255) / 255.f;
        in.steer = uint8_t(std::lround(128.f + 96.f * steer));
        in.accel = uint8_t(std::lround(32.f + 192.f * accel));
        in.brake = uint8_t(std::lround(32.f + 192.f * brake));
        // Shift only on presses (several board frames may run per sample).
        const int shift = int(bool(pressed & Up)) - int(bool(pressed & Down));
        gear_ = std::clamp(gear_ + shift, 1, 4);
        constexpr uint8_t codes[] = {0, 2, 1, 6, 5}; // the gearbox, as src/app/controls.cpp
        in.in1 = uint8_t((in.in1 & ~0x70) | (codes[gear_] << 4));
        if (pad.buttons & Y) in.in0 &= uint8_t(~0x01);     // coin
        if (pad.buttons & Start) in.in0 &= uint8_t(~0x10); // start
        if (pad.buttons & A) in.in0 &= uint8_t(~0x20);     // VR1
        if (pad.buttons & B) in.in0 &= uint8_t(~0x40);     // VR2
        if (pad.buttons & X) in.in0 &= uint8_t(~0x80);     // VR3
        return in;
    }
    int gear() const { return gear_; }

private:
    static float axis(int value, float deadzone = 0.12f) {
        const float x = std::clamp(value, -128, 127) / (value < 0 ? 128.f : 127.f);
        const float magnitude = std::abs(x);
        if (magnitude <= deadzone) return 0.f;
        return std::copysign((magnitude - deadzone) / (1.f - deadzone), x);
    }
    uint32_t held_ = 0;
    int gear_ = 1;
};

} // namespace dc
