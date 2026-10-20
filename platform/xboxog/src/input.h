// The original Xbox controller as the Daytona cabinet: the Dreamcast port's
// mapping and gearbox encoding (platform/dreamcast/game/controls.h, after
// platform/vita/controls.h).
//
//   left stick X, or D-pad left/right   steer
//   right trigger / left trigger        accelerate / brake
//   D-pad up / down, or black / white   shift up / down (gears 1-4, on presses)
//   A, B, X                             view buttons VR1, VR2, VR3
//   Y or Back                           coin
//   Start                               start
//   left / right stick click            test / service (test mode: Revision A
//                                       starts as a linked twin cabinet; set
//                                       a single cabinet there once)
#pragma once

#include <xtl.h>

#include "runtime/m2_board.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace xbox {

class Pad {
public:
    Pad() {
        polling_.fAutoPoll = TRUE;
        polling_.fInterruptOut = TRUE;
        polling_.bInputInterval = 8;
        polling_.bOutputInterval = 8;
        connected_ = XGetDevices(XDEVICE_TYPE_GAMEPAD);
        open_all();
    }

    // This frame's cabinet inputs. No controller: the cabinet at rest.
    rt::Inputs sample() {
        DWORD inserted = 0, removed = 0;
        if (XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &inserted, &removed)) {
            for (int port = 0; port < 4; ++port)
                if ((removed & (1u << port)) && handles_[port]) {
                    XInputClose(handles_[port]);
                    handles_[port] = nullptr;
                }
            connected_ = (connected_ & ~removed) | inserted;
            open_all();
        }
        XINPUT_GAMEPAD pad = {};
        for (int port = 0; port < 4; ++port) {
            XINPUT_STATE state;
            if (handles_[port] && XInputGetState(handles_[port], &state) == ERROR_SUCCESS) {
                pad = state.Gamepad;
                break;
            }
        }
        const bool up = (pad.wButtons & XINPUT_GAMEPAD_DPAD_UP) || button(pad, XINPUT_GAMEPAD_BLACK);
        const bool down = (pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || button(pad, XINPUT_GAMEPAD_WHITE);
        // Shift only on presses.
        gear_ = std::clamp(gear_ + int(up && !up_) - int(down && !down_), 1, 4);
        up_ = up;
        down_ = down;

        float steer = stick(pad.sThumbLX);
        if (pad.wButtons & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT))
            steer = float(bool(pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)) - float(bool(pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT));
        const float accel = pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] / 255.f;
        const float brake = pad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] / 255.f;
        rt::Inputs in;
        in.steer = uint8_t(std::lround(128.f + 96.f * steer));
        in.accel = uint8_t(std::lround(32.f + 192.f * accel));
        in.brake = uint8_t(std::lround(32.f + 192.f * brake));
        constexpr uint8_t codes[] = {0, 2, 1, 6, 5}; // the gearbox, as src/app/controls.cpp
        in.in1 = uint8_t((in.in1 & ~0x70) | (codes[gear_] << 4));
        if (button(pad, XINPUT_GAMEPAD_Y) || (pad.wButtons & XINPUT_GAMEPAD_BACK)) in.in0 &= uint8_t(~0x01); // coin
        if (pad.wButtons & XINPUT_GAMEPAD_START) in.in0 &= uint8_t(~0x10);                                     // start
        if (button(pad, XINPUT_GAMEPAD_A)) in.in0 &= uint8_t(~0x20);                                          // VR1
        if (button(pad, XINPUT_GAMEPAD_B)) in.in0 &= uint8_t(~0x40);                                          // VR2
        if (button(pad, XINPUT_GAMEPAD_X)) in.in0 &= uint8_t(~0x80);                                          // VR3
        if (pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) in.in0 &= uint8_t(~0x04);                                // test
        if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) in.in0 &= uint8_t(~0x08);                               // service
        return in;
    }

    int gear() const { return gear_; }

private:
    HANDLE handles_[4] = {};
    DWORD connected_ = 0;
    XINPUT_POLLING_PARAMETERS polling_ = {};
    int gear_ = 1;
    bool up_ = false, down_ = false;

    void open_all() {
        for (DWORD port = 0; port < 4; ++port)
            if ((connected_ & (1u << port)) && !handles_[port])
                handles_[port] = XInputOpen(XDEVICE_TYPE_GAMEPAD, port, XDEVICE_NO_SLOT, &polling_);
    }
    static float stick(SHORT raw, float deadzone = 0.12f) {
        const float x = std::max(-1.f, raw / 32767.f), magnitude = std::abs(x);
        if (magnitude <= deadzone) return 0.f;
        return std::copysign((magnitude - deadzone) / (1.f - deadzone), x);
    }
    static bool button(const XINPUT_GAMEPAD &pad, int index) { return pad.bAnalogButtons[index] > 30; }
};

} // namespace xbox
