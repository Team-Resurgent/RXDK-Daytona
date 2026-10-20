// The force feedback drive board (838-10646 on Daytona USA), as the game
// sees it: the I/O board passes on each command byte the game writes to its
// dual-port RAM byte 0x11 (IoBoard), and the drive board's Z80 turns the
// wheel motor. Its program (EPR-16488A) is not run; its command handling is
// modelled instead, as read from the program and checked in MAME 0.289 by
// feeding the board commands and wheel positions and recording its motor
// output (docs/issues.md, #9).
//
// The board reads the wheel's position itself and runs one effect at a time:
// the last command sets it until the next. The motor runs only after an
// "on" command (0x0-). Strength n is the low 3 bits:
//   0x00-04, 08, 09, 0B, 0C  motor off       0x05-07, 0A, 0D-0F  motor on
//   0x10-1F                  no force        0x20-27  resistance (a brake)
//   0x30-37                  centring spring 0x38-3F  centring spring, dead zone
//   0x40-47                  uncentring: away from the centre
//   0x50-57 / 0x60-67        constant force, one way / the other
//   0x28-2F, 48-4F, 58-5F, 68-6F  no force
//   0x70-7F  a spring parameter; 0x80-FF  status queries and nothing: the
//   effect is unchanged.
// Daytona sends 0x0-, 0x2-, 0x3-, 0x4-, 0x5-, 0x6-, 0x7- and 0xF- in a race.
//
// The result is the effect for a host force feedback device, each strength
// 0..1 relative to the board's strongest command (at the factory DIP setting).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rt {

struct DriveBoard {
    enum class Effect : uint8_t { None, Resistance, Centre, CentreDeadZone, Uncentre, Force };
    bool motor_on = false; // off at power-on, until the game turns it on
    Effect effect = Effect::None;
    int strength = 0; // n, 0..7
    int direction = 1; // Force: +1 0x5- (right), -1 0x6- (left)

    void command(uint8_t cmd) {
        const int n = cmd & 7;
        switch (cmd >> 4) {
        case 0x0: motor_on = cmd == 0x0a || n >= 5; return;
        case 0x1: set(Effect::None, 0); return;
        case 0x2: set(cmd & 8 ? Effect::None : Effect::Resistance, n); return;
        case 0x3: set(cmd & 8 ? Effect::CentreDeadZone : Effect::Centre, n); return;
        case 0x4: set(cmd & 8 ? Effect::None : Effect::Uncentre, n); return;
        case 0x5:
        case 0x6:
            set(cmd & 8 ? Effect::None : Effect::Force, n);
            direction = (cmd >> 4) == 0x5 ? 1 : -1;
            return;
        default: return; // 0x7-: the spring's slope (not modelled); 0x8- and up: queries, nothing
        }
    }

    // The effect's strength, 0 when the motor is off or another effect runs.
    // The board's motor power is 0..63: about 7 + 2n for the springs and
    // resistance, 17 + 2n for uncentring and the constant force (its minimum
    // and the spring parameter the game sets included); 31 is the strongest.
    float level(Effect e) const {
        if (!motor_on || effect != e || e == Effect::None) return 0.0f;
        const bool push = e == Effect::Uncentre || e == Effect::Force;
        return float((push ? 17 : 7) + 2 * strength) / 31.0f;
    }
    // The constant force, -1 (left) .. +1 (right).
    float force() const { return level(Effect::Force) * float(direction); }

    // The force the board's own closed loop drives for a wheel at x (-1 full
    // left .. +1 full right; +1 pushes right), for a host device whose driver
    // has no spring effect (the launcher's centring override). The springs
    // push towards the centre, rising over 8% of the travel beyond their dead
    // zone (2%, or 6% for 0x38-0x3F) to their level: the board's ramp, about
    // 2 power steps per position step, reaches it within a few percent.
    // Uncentring pushes away; the constant force is as it is; resistance has
    // no direction and gives nothing here.
    float motor(float x) const {
        switch (effect) {
        case Effect::Centre:
        case Effect::CentreDeadZone: {
            const float dead = effect == Effect::CentreDeadZone ? 0.06f : 0.02f;
            const float ramp = std::clamp((std::fabs(x) - dead) / 0.08f, 0.0f, 1.0f);
            return -std::copysign(level(effect) * ramp, x);
        }
        case Effect::Uncentre: return std::copysign(level(effect), x); // at the centre: right, as the board's direction 1
        case Effect::Force: return force();
        default: return 0.0f;
        }
    }

private:
    void set(Effect e, int n) { effect = e, strength = n; }
};

} // namespace rt
