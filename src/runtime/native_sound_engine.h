// Game-level sequencer plus direct PCM voices. This backend does not create
// a SoundBoard, Cpu68k, chip register bus, YM generator or MultiPCM device.
#pragma once
#include "runtime/native_sample_mixer.h"
#include "runtime/native_sound_sequencer.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace snd {
class NativeSoundEngine {
public:
    struct Stats {
        uint64_t frames = 0, bytes = 0, notes = 0, unsupported = 0, invalid = 0, clipped = 0;
        unsigned voices = 0;
        uint64_t limited_frames = 0;
    };
    NativeSoundEngine(std::vector<uint8_t> program, std::vector<uint8_t> pcm1,
                      std::vector<uint8_t> pcm2);
    NativeSoundEngine(const NativeSoundEngine &) = delete;
    NativeSoundEngine &operator=(const NativeSoundEngine &) = delete;
    void send(const uint8_t *bytes, size_t count);
    void render(float *stereo, size_t frames);
    Stats stats() const;
    void set_volumes(float music, float effects) { mixer_.set_volumes(music, effects); } // the launcher's two volumes
    const NativeSoundSequencer::Stats &sequence_stats() const { return sequencer_.stats(); }
private:
    std::vector<uint8_t> program_;
    std::array<std::vector<uint8_t>, 2> pcm_;
    std::array<std::array<NativeSampleBank, 4>, 2> banks_;
    NativeSampleMixer mixer_;
    NativeSoundSequencer sequencer_;
    uint64_t cursor_ = 0, end_ = 0, invalid_ = 0;
    float *output_ = nullptr;
    size_t written_ = 0;
    static void event(void *context, const NativeSoundSequencer::VoiceEvent &event);
    void apply(const NativeSoundSequencer::VoiceEvent &event);
    void render_to(uint64_t frame);
};
} // namespace snd
