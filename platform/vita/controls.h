// Vita input mapping, independent of SDL and VitaSDK so it can be host-tested.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vita {
enum Button : uint32_t {
    Cross = 1u << 0, Circle = 1u << 1, Square = 1u << 2, Triangle = 1u << 3,
    Up = 1u << 4, Down = 1u << 5, Left = 1u << 6, Right = 1u << 7,
    L = 1u << 8, R = 1u << 9, Start = 1u << 10, Select = 1u << 11
};
struct Pad { uint32_t buttons = 0; uint8_t lx = 128, ry = 128; };
struct Input {
    uint8_t steer = 0x80, accel = 0x20, brake = 0x20;
    uint8_t in0 = 0xff, in1 = 0x8f, in2 = 0xff;
};
inline bool menu_chord(uint32_t buttons) {
    return (buttons & (Start | Select)) == (Start | Select);
}
inline float axis(uint8_t value, float deadzone = 0.12f) {
    deadzone = std::clamp(deadzone, 0.0f, 0.4f);
    const float x = value < 128 ? (int(value) - 128) / 128.f : (int(value) - 128) / 127.f;
    const float magnitude = std::abs(x);
    if (magnitude <= deadzone) return 0.f;
    return std::copysign((magnitude - deadzone) / (1.f - deadzone), x);
}

// Game actions the player can rebind (GPU launcher, Controls page). One button each;
// the analog sticks (left: steering, right: pedals) and the system chords
// (Start+Select menu, Select+Triangle test, Select+Square service) stay fixed.
enum Action : int {
    Accelerate, Brake, SteerLeft, SteerRight, ShiftUp, ShiftDown,
    StartButton, Coin, View1, View2, View3, View4, ActionCount
};
inline constexpr uint32_t kButtonMask = 0xfffu;
inline const char *action_name(int action) {
    static const char *names[ActionCount] = {"ACCELERATE", "BRAKE", "STEER LEFT", "STEER RIGHT",
        "SHIFT UP", "SHIFT DOWN", "START", "COIN", "VIEW 1", "VIEW 2", "VIEW 3", "VIEW 4"};
    return action >= 0 && action < ActionCount ? names[action] : "?";
}
inline bool single_button(uint32_t button) {
    return button && !(button & ~kButtonMask) && !(button & (button - 1));
}
inline const char *button_name(uint32_t button) {
    static const char *names[] = {"CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "UP", "DOWN",
        "LEFT", "RIGHT", "L", "R", "START", "SELECT"};
    if (!single_button(button)) return "NONE";
    int bit = 0;
    while (!(button & (1u << bit))) ++bit;
    return names[bit];
}
struct Bindings {
    // Defaults: the original Vita layout.
    uint32_t button[ActionCount] = {R, L, Left, Right, Up, Down, Start, Select,
                                    Cross, Circle, Square, Triangle};
    // Gives `action` the button `value`; an action that had it takes the old one
    // instead, so no action is ever left without a button.
    void bind(int action, uint32_t value) {
        if (action < 0 || action >= ActionCount || !single_button(value)) return;
        for (uint32_t &other : button)
            if (other == value) other = button[action];
        button[action] = value;
    }
    // Next/previous button in the fixed order above (Left/Right on the Controls page).
    void cycle(int action, int direction) {
        if (action < 0 || action >= ActionCount) return;
        int bit = 0;
        while (bit < 11 && !(button[action] & (1u << bit))) ++bit;
        bind(action, 1u << ((bit + (direction < 0 ? 11 : 1)) % 12));
    }
    // Saved files may be edited by hand: any invalid or duplicated entry resets all.
    void sanitize() {
        uint32_t seen = 0;
        for (uint32_t value : button) {
            if (!single_button(value) || (seen & value)) { *this = Bindings{}; return; }
            seen |= value;
        }
    }
};

class Controls {
public:
    Input sample(Pad pad) {
        Input in;
        const auto &b = bindings_.button;
        const uint32_t previous = held_;
        const uint32_t pressed = pad.buttons & ~held_;
        if (pressed & b[Coin]) coin_used_ = false;
        held_ = pad.buttons;
        // The menu chord must not inject a coin or start a race.
        if (menu_chord(pad.buttons)) { coin_used_ = true; set_gear(in); return in; }
        // Cabinet controls consume the modifier and view buttons.
        if (pad.buttons & Select) {
            const bool test = (pad.buttons & Triangle) != 0;
            const bool service = (pad.buttons & Square) != 0;
            if (test || service) {
                coin_used_ = true;
                if (test) in.in0 &= uint8_t(~0x04);
                if (service) in.in0 &= uint8_t(~0x08);
                set_gear(in);
                return in;
            }
        }
        float steer = axis(pad.lx, deadzone_);
        if (steer_curve_ == 1) steer *= std::abs(steer);
        else if (steer_curve_ == 2) steer = steer * steer * steer;
        if (pad.buttons & (b[SteerLeft] | b[SteerRight]))
            steer = float(bool(pad.buttons & b[SteerRight])) - float(bool(pad.buttons & b[SteerLeft]));
        if (steer_invert_) steer = -steer;
        const float pedal = axis(pad.ry, deadzone_);
        const float accel = pad.buttons & b[Accelerate] ? 1.f : std::max(-pedal, 0.f);
        const float brake = pad.buttons & b[Brake] ? 1.f : std::max(pedal, 0.f);
        in.steer = uint8_t(std::lround(128.f + 96.f * steer));
        in.accel = uint8_t(std::lround(32.f + 192.f * accel));
        in.brake = uint8_t(std::lround(32.f + 192.f * brake));
        // Shift only on edges, including when several board frames are run.
        const int shift = int(bool(pressed & b[ShiftUp])) - int(bool(pressed & b[ShiftDown]));
        gear_ = std::clamp(gear_ + shift, 1, 4);
        set_gear(in);
        // Coin on release leaves time to distinguish Select-based chords.
        if ((previous & b[Coin]) && !(pad.buttons & b[Coin]) && !coin_used_) in.in0 &= uint8_t(~0x01);
        if (pad.buttons & b[StartButton]) in.in0 &= uint8_t(~0x10);
        if (pad.buttons & b[View1]) in.in0 &= uint8_t(~0x20);
        if (pad.buttons & b[View2]) in.in0 &= uint8_t(~0x40);
        if (pad.buttons & b[View3]) in.in0 &= uint8_t(~0x80);
        if (pad.buttons & b[View4]) in.in1 &= uint8_t(~0x01);
        return in;
    }
    void latch(uint32_t buttons) { held_ = buttons; if (buttons & bindings_.button[Coin]) coin_used_ = true; }
    int gear() const { return gear_; }
    void set_deadzone(float value) { deadzone_ = std::clamp(value, 0.0f, 0.4f); }
    void set_steer_curve(int value) { steer_curve_ = std::clamp(value, 0, 2); }
    void set_steer_invert(bool value) { steer_invert_ = value; }
    void set_bindings(const Bindings &value) { bindings_ = value; bindings_.sanitize(); }
    const Bindings &bindings() const { return bindings_; }
private:
    void set_gear(Input &in) const {
        // Same gearbox encoding as src/app/controls.cpp.
        constexpr uint8_t codes[] = {0, 2, 1, 6, 5};
        in.in1 = uint8_t((in.in1 & ~0x70) | (codes[gear_] << 4));
    }
    Bindings bindings_;
    uint32_t held_ = 0;
    int gear_ = 1, steer_curve_ = 0;
    float deadzone_ = 0.12f;
    bool steer_invert_ = false, coin_used_ = false;
};

// Host time only; each simulation step still advances at GameLoop::kFrameHz.
class FrameClock {
public:
    explicit FrameClock(double hz, int max_steps = 4)
        : step_(1.0 / hz), max_steps_(std::clamp(max_steps, 1, 4)) {}
    int advance(double seconds) {
        if (!std::isfinite(seconds) || seconds < 0) { reset(); return 0; }
        pending_ = std::min(pending_ + seconds, step_ * 4);
        int count = 0;
        while (pending_ >= step_ && count < max_steps_) { pending_ -= step_; ++count; }
        // Under load, present each completed frame instead of computing up to
        // four complete rasters and displaying only the last. Discard overdue
        // host-time debt, not guest instructions; preserve fractional time so
        // 57.524 Hz on a 60 Hz display does not accidentally become 30 Hz.
        if (pending_ >= step_) pending_ = std::fmod(pending_, step_);
        return count;
    }
    void reset() { pending_ = 0; }
private:
    double step_, pending_ = 0;
    int max_steps_;
};
} // namespace vita
