#include "app/ffb.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace app {

namespace {

// The device steering is bound to: a wheel/joystick binding first, else the gamepad.
SDL_Joystick *steering_device(const Devices &d, const Controls &c) {
    for (Action a : {SteerRight, SteerLeft}) {
        const JoyInput &j = c.bind[a].joy;
        if (j.kind != JoyInput::None)
            if (SDL_Joystick *s = d.find(j.guid)) return s;
    }
    return d.pad ? SDL_GetGamepadJoystick(d.pad) : nullptr;
}

SDL_HapticDirection steering_axis() {
    SDL_HapticDirection dir{};
    dir.type = SDL_HAPTIC_STEERING_AXIS;
    return dir;
}

constexpr uint64_t kRetryMs = 3000; // between tries to open a wheel's haptics

} // namespace

const char *ForceFeedback::device_kind() const {
    if (haptic_) return "wheel (force feedback)";
    if (wants_haptic_) return "wheel (force feedback did not open; retrying)";
    if (joy_) return "gamepad (rumble)";
    return "none";
}

void ForceFeedback::open(SDL_Joystick *joy) {
    joy_ = joy;
    reported_ = false;
    wants_haptic_ = SDL_IsJoystickHaptic(joy);
    if (wants_haptic_) {
        open_haptic();
        return;
    }
    // no haptics: rumble, if the device has it
    if (!SDL_GetBooleanProperty(SDL_GetJoystickProperties(joy), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false)) joy_ = nullptr;
}

void ForceFeedback::open_haptic() {
    if (!(haptic_ = SDL_OpenHapticFromJoystick(joy_))) {
        if (!reported_ || log_)
            std::fprintf(stderr, "daytona: force feedback on %s did not open (%s); trying again\n",
                         SDL_GetJoystickName(joy_), SDL_GetError());
        reported_ = true;
        retry_at_ = SDL_GetTicks() + kRetryMs;
        return;
    }
    const Uint32 features = SDL_GetHapticFeatures(haptic_);
    if (features & SDL_HAPTIC_AUTOCENTER) SDL_SetHapticAutocenter(haptic_, 0); // the game centres the wheel itself
    if (features & SDL_HAPTIC_GAIN) SDL_SetHapticGain(haptic_, 100);
    auto create = [&](Uint32 type) {
        if (!(features & type)) return -1;
        SDL_HapticEffect e{};
        if (type == SDL_HAPTIC_CONSTANT) {
            e.constant.type = SDL_HAPTIC_CONSTANT;
            e.constant.direction = steering_axis();
            e.constant.length = SDL_HAPTIC_INFINITY;
        } else {
            e.condition.type = Uint16(type);
            e.condition.direction = steering_axis();
            e.condition.length = SDL_HAPTIC_INFINITY;
        }
        const int id = SDL_CreateHapticEffect(haptic_, &e);
        if (id < 0 && log_) std::fprintf(stderr, "daytona: force feedback: effect not created (%s)\n", SDL_GetError());
        return id;
    };
    spring_ = create(SDL_HAPTIC_SPRING);
    friction_ = create(SDL_HAPTIC_FRICTION);
    constant_ = create(SDL_HAPTIC_CONSTANT);
    if (log_)
        std::fprintf(stderr, "daytona: force feedback on %s: features 0x%" PRIx32 ", spring %s, friction %s, constant %s\n",
                     SDL_GetHapticName(haptic_), features, spring_ >= 0 ? "yes" : "no", friction_ >= 0 ? "yes" : "no",
                     constant_ >= 0 ? "yes" : "no");
    std::fill(std::begin(sent_), std::end(sent_), -1);
    running_ = false;
}

void ForceFeedback::close() {
    if (haptic_) {
        SDL_StopHapticEffects(haptic_);
        SDL_CloseHaptic(haptic_);
    } else if (joy_ && !wants_haptic_) {
        SDL_RumbleJoystick(joy_, 0, 0, 0);
    }
    haptic_ = nullptr;
    joy_ = nullptr;
    wants_haptic_ = false;
    spring_ = friction_ = constant_ = -1;
    running_ = false;
}

void ForceFeedback::update(const std::vector<uint8_t> &commands, const Devices &devices, const Controls &controls,
                           float strength, bool invert, bool log, uint64_t frame, float wheel, bool centring_override) {
    stopped_ = false;
    log_ = log;
    wheel_ = std::clamp(wheel, -1.0f, 1.0f);
    if (centring_override != override_ && log_)
        std::fprintf(stderr, "daytona: force feedback: centring %s\n",
                     centring_override ? "worked out from the wheel's position, in the constant force (override)"
                                       : "through the device's spring");
    override_ = centring_override;
    for (uint8_t c : commands) {
        if (log_) std::fprintf(stderr, "daytona: force feedback: frame %" PRIu64 ": command %02x\n", frame, c);
        drive_.command(c);
    }
    SDL_Joystick *j = strength > 0 ? steering_device(devices, controls) : nullptr;
    if (j != joy_) {
        close();
        if (j) open(j);
    } else if (wants_haptic_ && !haptic_ && SDL_GetTicks() >= retry_at_) {
        open_haptic();
    }
    apply(strength, invert);
}

void ForceFeedback::stop() {
    if (stopped_) return;
    stopped_ = true;
    if (haptic_) {
        SDL_StopHapticEffects(haptic_);
        running_ = false;
        std::fill(std::begin(sent_), std::end(sent_), -1);
    } else if (joy_ && !wants_haptic_) {
        SDL_RumbleJoystick(joy_, 0, 0, 0);
    }
}

void ForceFeedback::reset() {
    stop();
    drive_ = rt::DriveBoard{};
}

void ForceFeedback::apply(float strength, bool invert) {
    using Effect = rt::DriveBoard::Effect;
    if (!joy_) return;
    strength = std::clamp(strength, 0.0f, 1.0f);
    // With the centring override the springs come from the board's own loop on
    // the wheel's position and go out in the constant force; the spring effect
    // is left at nothing.
    const float push = override_ && haptic_ ? drive_.motor(wheel_) : drive_.force();
    const float force = std::clamp(push * (invert ? -1.0f : 1.0f), -1.0f, 1.0f) * strength;
    const float uncentre = drive_.level(Effect::Uncentre) * strength;
    if (!haptic_) {
        if (wants_haptic_) return; // a wheel whose haptics did not open: nothing until they do
        // rumble: the turning force's size on the large motor, uncentring on the small one
        const auto lo = Uint16(std::lround(std::fabs(force) * 0xffff));
        const auto hi = Uint16(std::lround(uncentre * 0xffff));
        SDL_RumbleJoystick(joy_, lo, hi, 200); // renewed every frame
        return;
    }
    // Only changes go to the device, and only once they are accepted: a failed
    // call is tried again next frame.
    auto send = [&](int slot, int id, int key, SDL_HapticEffect &e) {
        if (id < 0 || sent_[slot] == key) return;
        if (SDL_UpdateHapticEffect(haptic_, id, &e)) sent_[slot] = key;
        else if (log_) std::fprintf(stderr, "daytona: force feedback: effect update failed (%s)\n", SDL_GetError());
    };
    {   // the springs: towards the centre (the dead zone about 6% of the travel,
        // else 2%, as the board's), or away from it as a negative spring
        const float centre = override_ ? 0.0f : std::max(drive_.level(Effect::Centre), drive_.level(Effect::CentreDeadZone)) * strength;
        const auto sat = Uint16(std::lround((override_ ? 0.0f : std::max(centre, uncentre)) * 0xffff));
        const bool away = uncentre > 0;
        const Uint16 deadband = drive_.effect == Effect::CentreDeadZone ? 0x1000 : drive_.effect == Effect::Centre ? 0x0500 : 0;
        SDL_HapticEffect e{};
        e.condition.type = SDL_HAPTIC_SPRING;
        e.condition.direction = steering_axis();
        e.condition.length = SDL_HAPTIC_INFINITY;
        e.condition.right_sat[0] = e.condition.left_sat[0] = sat;
        e.condition.right_coeff[0] = e.condition.left_coeff[0] = Sint16(away ? -0x7fff : 0x7fff);
        e.condition.deadband[0] = deadband;
        send(0, spring_, sat ? int(sat) | int(away) << 16 | int(deadband) << 17 : 0, e);
    }
    {
        const auto level = Sint16(std::lround(drive_.level(Effect::Resistance) * strength * 0x7fff));
        SDL_HapticEffect e{};
        e.condition.type = SDL_HAPTIC_FRICTION;
        e.condition.direction = steering_axis();
        e.condition.length = SDL_HAPTIC_INFINITY;
        e.condition.right_sat[0] = e.condition.left_sat[0] = 0xffff;
        e.condition.right_coeff[0] = e.condition.left_coeff[0] = level;
        send(1, friction_, level, e);
    }
    {
        const auto level = Sint16(std::lround(force * 0x7fff));
        SDL_HapticEffect e{};
        e.constant.type = SDL_HAPTIC_CONSTANT;
        e.constant.direction = steering_axis();
        e.constant.length = SDL_HAPTIC_INFINITY;
        e.constant.level = level;
        send(2, constant_, level, e);
    }
    if (!running_) {
        running_ = true;
        for (int id : {spring_, friction_, constant_})
            if (id >= 0 && !SDL_RunHapticEffect(haptic_, id, 1)) {
                running_ = false; // all of them again next frame
                if (log_) std::fprintf(stderr, "daytona: force feedback: effect did not start (%s)\n", SDL_GetError());
            }
    }
}

} // namespace app
