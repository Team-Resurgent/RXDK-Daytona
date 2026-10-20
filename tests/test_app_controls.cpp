// Wheel and pedal bindings (src/app/controls.cpp) against SDL virtual
// joysticks: a wheel (one axis) and separate pedals that rest at +32767 and
// press toward -32768, as many do. Checks the device list follows SDL's
// events, bindings save and load, calibrated axes reach the I/O board's ADC
// ranges, and a binding to a device that is not connected reads as nothing.
// Force feedback: the drive board's commands decode (rt::DriveBoard), and a
// steering device without haptics gets the pushing forces as rumble.

#include "app/controls.h"
#include "app/ffb.h"
#include "runtime/drive_board.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

Uint16 rumble_low = 0, rumble_high = 0;
bool SDLCALL rumble(void *, Uint16 low, Uint16 high) {
    rumble_low = low, rumble_high = high;
    return true;
}

SDL_JoystickID attach(SDL_JoystickType type, int axes, int buttons, const char *name, bool can_rumble = false) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = Uint16(type);
    d.naxes = Uint16(axes);
    d.nbuttons = Uint16(buttons);
    d.name = name;
    if (can_rumble) d.Rumble = rumble;
    return SDL_AttachVirtualJoystick(&d);
}

void pump(app::Devices &devices) {
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PollEvent(&e)) devices.handle_event(e);
}

std::string guid(SDL_Joystick *j) {
    char b[64];
    SDL_GUIDToString(SDL_GetJoystickGUID(j), b, sizeof b);
    return b;
}
} // namespace

int main() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    app::Devices devices;
    pump(devices);
    // Real joysticks plugged in while the tests run (a wheel, a pad) are
    // opened too: the counts below are on top of them.
    const size_t present = devices.joys.size();
    const SDL_JoystickID wheel_id = attach(SDL_JOYSTICK_TYPE_WHEEL, 1, 2, "Test wheel");
    const SDL_JoystickID pedals_id = attach(SDL_JOYSTICK_TYPE_THROTTLE, 2, 0, "Test pedals");
    pump(devices);
    check(devices.joys.size() == present + 2, "both virtual devices opened");
    SDL_Joystick *wheel = SDL_GetJoystickFromID(wheel_id), *pedals = SDL_GetJoystickFromID(pedals_id);
    check(wheel && pedals, "devices found by id");
    if (!wheel || !pedals) return 1;
    SDL_SetJoystickVirtualAxis(pedals, 0, 32767); // pedals at rest
    SDL_SetJoystickVirtualAxis(pedals, 1, 32767);
    pump(devices);

    // Bindings as the launcher makes them: steering both ways on the wheel's
    // axis (full lock at a quarter turn each way), pedals from rest to floor.
    app::Controls c;
    c.set_defaults();
    c.joy_deadzone = 0.0f;
    auto axis = [&](SDL_Joystick *j, int index, int rest, int full) {
        app::JoyInput in;
        in.kind = app::JoyInput::Axis, in.guid = guid(j), in.index = index, in.rest = rest, in.full = full;
        return app::JoyInput::parse(in.save()); // through the config format
    };
    c.bind[app::SteerLeft].joy = axis(wheel, 0, 0, -8192);
    c.bind[app::SteerRight].joy = axis(wheel, 0, 0, 8192);
    c.bind[app::Accelerate].joy = axis(pedals, 0, 32767, -32768);
    c.bind[app::Brake].joy = axis(pedals, 1, 32767, -32768);
    app::JoyInput button;
    button.kind = app::JoyInput::Button, button.guid = guid(wheel), button.index = 1;
    c.bind[app::GearUp].joy = app::JoyInput::parse(button.save());
    check(c.bind[app::Accelerate].joy.rest == 32767 && c.bind[app::Accelerate].joy.full == -32768, "axis binding round trip");

    const bool *no_keys = nullptr;
    rt::Inputs in = c.sample(no_keys, devices);
    check(in.steer == 0x80 && in.accel == 0x20 && in.brake == 0x20, "at rest: centred, pedals up");

    SDL_SetJoystickVirtualAxis(wheel, 0, 8192); // full right lock, a quarter turn
    SDL_SetJoystickVirtualAxis(pedals, 0, -32768); // accelerator floored
    SDL_SetJoystickVirtualAxis(pedals, 1, 0);     // brake half way
    pump(devices);
    in = c.sample(no_keys, devices);
    check(in.steer == 0xe0, "full right");
    check(in.accel == 0xe0, "accelerator floored");
    check(std::abs(int(in.brake) - 0x80) <= 1, "brake half way");

    SDL_SetJoystickVirtualAxis(wheel, 0, 4096);
    pump(devices);
    const auto linear = c.sample(no_keys, devices);
    c.wheel_sensitivity = 0.5f;
    const auto gentle = c.sample(no_keys, devices);
    c.wheel_sensitivity = 2.0f;
    const auto sensitive = c.sample(no_keys, devices);
    check(gentle.steer < linear.steer && sensitive.steer > linear.steer,
          "wheel sensitivity changes intermediate steering response");
    check(gentle.accel == linear.accel && sensitive.brake == linear.brake,
          "wheel sensitivity leaves pedals unchanged");
    SDL_SetJoystickVirtualAxis(wheel, 0, 0);
    pump(devices);
    check(c.sample(no_keys, devices).steer == 0x80, "sensitivity preserves centre");
    SDL_SetJoystickVirtualAxis(wheel, 0, 8192);
    pump(devices);
    check(c.sample(no_keys, devices).steer == 0xe0, "sensitivity preserves full right lock");
    c.wheel_sensitivity = 1.0f;

    SDL_SetJoystickVirtualAxis(wheel, 0, -32768); // past the bound lock: clamps
    pump(devices);
    in = c.sample(no_keys, devices);
    check(in.steer == 0x20, "full left (clamped)");

    const int gear = c.gear;
    SDL_SetJoystickVirtualButton(wheel, 1, true);
    pump(devices);
    c.sample(no_keys, devices);
    check(c.gear == std::min(gear + 1, 4), "wheel paddle shifts up");

    app::JoyInput missing = c.bind[app::Accelerate].joy;
    missing.guid = "00000000000000000000000000000000";
    check(missing.value(devices, 0.0f) == 0.0f, "unconnected device reads 0");
    check(missing.describe(devices).find("not connected") != std::string::npos, "unconnected device described");

    // The drive board, as its program (EPR-16488A) handles the commands: one
    // effect at a time, strength in the low 3 bits, only once the motor is on.
    using Effect = rt::DriveBoard::Effect;
    rt::DriveBoard drive;
    drive.command(0x3b);
    check(drive.effect == Effect::CentreDeadZone && drive.level(Effect::CentreDeadZone) == 0, "motor off at power-on");
    drive.command(0x07);
    check(drive.motor_on && std::fabs(drive.level(Effect::CentreDeadZone) - 13.0f / 31.0f) < 1e-6f,
          "0x07 turns the motor on; 0x3b: centring spring with a dead zone");
    drive.command(0x34);
    check(drive.effect == Effect::Centre && drive.level(Effect::CentreDeadZone) == 0 &&
              std::fabs(drive.level(Effect::Centre) - 15.0f / 31.0f) < 1e-6f,
          "0x34: centring spring; one effect at a time");
    drive.command(0x27);
    check(drive.effect == Effect::Resistance && drive.level(Effect::Centre) == 0 &&
              std::fabs(drive.level(Effect::Resistance) - 21.0f / 31.0f) < 1e-6f,
          "0x27: resistance");
    drive.command(0x57);
    check(drive.force() == 1.0f, "0x57: the strongest pull right");
    drive.command(0x64);
    check(std::fabs(drive.force() + 25.0f / 31.0f) < 1e-6f, "0x64: pull left");
    drive.command(0x71), drive.command(0x80), drive.command(0xc0), drive.command(0xff);
    check(std::fabs(drive.force() + 25.0f / 31.0f) < 1e-6f, "0x7- and queries leave the effect");
    drive.command(0x43);
    check(drive.effect == Effect::Uncentre && std::fabs(drive.level(Effect::Uncentre) - 23.0f / 31.0f) < 1e-6f,
          "0x43: uncentring");
    drive.command(0x58);
    check(drive.effect == Effect::None && drive.force() == 0, "0x58: no force");
    drive.command(0x57), drive.command(0x01);
    check(!drive.motor_on && drive.force() == 0, "0x01 turns the motor off");
    drive.command(0x0a);
    check(drive.motor_on && drive.force() == 1.0f, "0x0a turns it on again; the effect was kept");
    drive.command(0x1f);
    check(drive.effect == Effect::None && drive.force() == 0, "0x1-: no force");

    // The board's own loop on the wheel's position (the centring override).
    drive.command(0x34); // centring spring, dead zone 2%, full level by 10%
    const float spring = drive.level(Effect::Centre);
    check(drive.motor(0.0f) == 0 && drive.motor(0.015f) == 0, "spring: nothing in its dead zone");
    check(drive.motor(0.5f) == -spring && drive.motor(-0.5f) == spring, "spring: back to the centre at its level");
    check(std::fabs(drive.motor(0.06f) + spring / 2) < 1e-6f, "spring: half way up its ramp");
    drive.command(0x3c); // with the wider dead zone
    check(drive.motor(0.05f) == 0 && drive.motor(0.5f) < 0, "dead-zone spring: 6%");
    drive.command(0x44);
    check(drive.motor(0.3f) > 0 && drive.motor(-0.3f) < 0, "uncentring: away from the centre");
    drive.command(0x64);
    check(drive.motor(0.7f) == drive.force(), "constant force: as it is");
    drive.command(0x27);
    check(drive.motor(0.5f) == 0, "resistance: no direction");
    drive.command(0x34), drive.command(0x01);
    check(drive.motor(0.5f) == 0, "motor off: nothing");
    drive.command(0x0a);

    // Force feedback on a steering device without haptics: rumble. The wheel
    // here is replaced by a pad-like joystick that can rumble.
    const SDL_JoystickID rumbler_id = attach(SDL_JOYSTICK_TYPE_GAMEPAD, 2, 4, "Test rumbler", true);
    pump(devices);
    SDL_Joystick *rumbler = SDL_GetJoystickFromID(rumbler_id);
    c.bind[app::SteerLeft].joy = axis(rumbler, 0, 0, -32768);
    c.bind[app::SteerRight].joy = axis(rumbler, 0, 0, 32767);
    app::ForceFeedback ffb;
    ffb.update({0x07, 0x57}, devices, c, 1.0f, false); // motor on, the strongest pull right
    SDL_UpdateJoysticks();
    check(std::string(ffb.device_kind()) == "gamepad (rumble)", "rumble device chosen");
    check(rumble_low == 0xffff && rumble_high == 0, "the pull on the large motor");
    ffb.update({0x47}, devices, c, 1.0f, false); // uncentring
    SDL_UpdateJoysticks();
    check(rumble_low == 0 && rumble_high == 0xffff, "uncentring on the small motor");
    ffb.update({}, devices, c, 0.5f, false); // the state holds; the strength scales it
    SDL_UpdateJoysticks();
    check(std::abs(int(rumble_high) - 0x8000) <= 1, "strength scales rumble");
    ffb.update({0x3b}, devices, c, 1.0f, false); // the centring spring: on all race, no rumble
    SDL_UpdateJoysticks();
    check(rumble_low == 0 && rumble_high == 0, "no rumble for the spring");
    ffb.update({0x57}, devices, c, 1.0f, false);
    ffb.stop();
    SDL_UpdateJoysticks();
    check(rumble_low == 0 && rumble_high == 0, "stop: rumble off");
    ffb.update({}, devices, c, 1.0f, false); // resumed: the board's state was kept
    SDL_UpdateJoysticks();
    check(rumble_low == 0xffff, "resume: the pull again");
    ffb.reset();
    ffb.update({}, devices, c, 1.0f, false); // a new game: motor off until the game turns it on
    SDL_UpdateJoysticks();
    check(rumble_low == 0 && rumble_high == 0, "reset: the motor off");
    ffb.update({}, devices, c, 0.0f, false); // strength 0: off, the device released
    check(std::string(ffb.device_kind()) == "none", "strength 0 releases the device");
    ffb.close();
    SDL_DetachVirtualJoystick(rumbler_id);
    pump(devices);

    SDL_DetachVirtualJoystick(pedals_id);
    pump(devices);
    check(devices.joys.size() == present + 1, "unplugged devices closed");
    in = c.sample(no_keys, devices);
    check(in.accel == 0x20, "pedals gone: accelerator up");

    devices.close_all();
    SDL_DetachVirtualJoystick(wheel_id);
    SDL_Quit();
    if (failures) return 1;
    std::printf("test_app_controls: ok\n");
    return 0;
}
