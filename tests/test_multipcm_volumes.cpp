// The MultiPCM's music and effects volumes (the launcher's Audio tab) on a
// synthetic sample: at 1 and 1 the output is the chip's own, bit for bit; a
// volume of 0 leaves exactly the other kind's voices. No game ROM data.
#include "runtime/multipcm.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Sample 0: 256 bytes of 8-bit square wave at 0x1000, looped, instant attack.
std::vector<uint8_t> rom() {
    std::vector<uint8_t> r(0x2000, 0);
    r[0] = 0x00, r[1] = 0x10, r[2] = 0x00; // start 0x1000, 8-bit
    r[3] = 0x00, r[4] = 0x00;              // loop 0
    r[5] = 0xff, r[6] = 0x00;              // end 256
    r[8] = 0xf0;                           // attack 15, decay 1 0
    r[10] = 0x0f;                          // release 15
    for (int i = 0; i < 256; ++i) r[size_t(0x1000 + i)] = (i / 16) & 1 ? 0x40 : 0xc0;
    return r;
}

void key_on(snd::MultiPcm &pcm, uint8_t slot, uint8_t pan, uint8_t octave, bool effect) {
    auto reg = [&](uint8_t r, uint8_t v) { pcm.write(2, r), pcm.write(0, v); };
    pcm.write(1, slot);
    reg(0, pan);
    reg(1, 0);                           // sample 0
    reg(2, 0), reg(3, uint8_t(octave << 4)); // pitch
    reg(5, 0x01);                        // full level, no interpolation
    pcm.set_effect(effect);
    reg(4, 0x80);
}

std::array<float, 512> render(snd::MultiPcm &pcm) {
    std::array<float, 512> out{};
    pcm.generate(out.data(), out.data() + 256, 256);
    return out;
}
} // namespace

int main() {
    const auto r = rom();
    auto chip = [&](bool music, bool effect, float mv, float ev, bool set) {
        snd::MultiPcm pcm(10000000, r.data(), uint32_t(r.size()));
        if (music) key_on(pcm, 0, 0x20, 0, false);
        if (effect) key_on(pcm, 1, 0xd0, 1, true);
        if (set) pcm.set_volumes(mv, ev);
        return render(pcm);
    };
    const auto both = chip(true, true, 1, 1, false), unity = chip(true, true, 1, 1, true);
    const auto music = chip(true, false, 1, 1, false), effects = chip(false, true, 1, 1, false);
    check(both == unity, "1 and 1: the chip's own output");
    check(chip(true, true, 1, 0, true) == music, "effects at 0: the music alone");
    check(chip(true, true, 0, 1, true) == effects, "music at 0: the effects alone");
    bool sounds = false;
    for (float v : effects) sounds |= v != 0;
    check(sounds, "the effect voice sounds");
    const auto half = chip(true, true, 1, 0.5f, true);
    bool scaled = true;
    for (size_t i = 0; i < half.size(); ++i) // integer sums, rounded once: within one step of 16 bits
        scaled &= std::abs(half[i] - (music[i] + 0.5f * effects[i])) <= 1.0f / 32768.0f;
    check(scaled, "effects at half");
    if (failures) return 1;
    std::printf("test_multipcm_volumes: ok\n");
    return 0;
}
