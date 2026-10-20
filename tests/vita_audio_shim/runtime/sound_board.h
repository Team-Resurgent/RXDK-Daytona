// Test fixture only. The production Audio code sees the same sample-drain API;
// no sound emulation or resampling behavior is modeled by this ownership test.
#pragma once
#include <cstdint>
#include <utility>
#include <vector>
namespace snd {
struct SoundBoard {
    static constexpr uint32_t kYmClock = 8000000, kPcmClock = 10000000;
    std::vector<float> fm, pcm;
    std::vector<float> take_fm() { return std::exchange(fm, {}); }
    std::vector<float> take_pcm() { return std::exchange(pcm, {}); }
};
}
