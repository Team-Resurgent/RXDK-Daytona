// Direct 48 kHz PCM voice mixer. This is an approximate native audio path,
// not a MultiPCM register, envelope, LFO, or timing implementation.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace snd {

class NativeSampleBank {
public:
    static constexpr unsigned kSamples = 512;
    enum class Format : uint8_t { Signed8, PackedSigned12 };
    enum class Status : uint8_t { MissingTable, InvalidLoop, UnmappedData, Valid };
    struct Sample {
        uint32_t byte_start = 0;
        uint32_t frames = 0;
        uint32_t loop_start = 0;
        Format format = Format::Signed8;
        // Source articulation metadata only. The native mixer deliberately
        // uses the caller's linear ADSR rather than chip-specific rate tables.
        uint8_t attack_hint = 0, decay_hint = 0, sustain_decay_hint = 0;
        uint8_t sustain_hint = 0, release_hint = 0;
    };

    NativeSampleBank() = default;
    NativeSampleBank(const uint8_t* rom, size_t bytes, unsigned data_bank) {
        load(rom, bytes, data_bank);
    }
    // Borrowed immutable storage must outlive this bank and all its voices.
    // Load only while no mixer voice refers to this bank. Headers occupy the
    // fixed first 512*12 bytes; logical 0x100000..0x1fffff selects data_bank.
    // Success means the table exists, not that every sample entry is valid.
    bool load(const uint8_t* rom, size_t bytes, unsigned data_bank) noexcept;
    const Sample* sample(unsigned index) const noexcept;
    Status status(unsigned index) const noexcept;
    unsigned valid_sample_count() const noexcept { return valid_count_; }
    unsigned data_bank() const noexcept { return data_bank_; }
    float value(unsigned index, uint32_t frame) const noexcept;

private:
    friend class NativeSampleMixer;
    uint8_t byte(uint32_t logical) const noexcept;
    float value(const Sample& sample, uint32_t frame) const noexcept;
    bool mapped_range(uint32_t start, uint32_t bytes) const noexcept;
    const uint8_t* rom_ = nullptr;
    size_t rom_bytes_ = 0;
    unsigned data_bank_ = 0, valid_count_ = 0;
    std::array<Sample, kSamples> samples_{};
    std::array<Status, kSamples> status_{};
};

class NativeSampleMixer {
public:
    static constexpr unsigned kOutputRate = 48000;
    static constexpr unsigned kVoices = 64;
    // Native-only calibration from reference/native attract and race RMS.
    static constexpr float kDefaultMasterGain = 1.95f;
    static constexpr float kPeakCeiling = 0.98f;
    static constexpr uint32_t kHeaderLoop = std::numeric_limits<uint32_t>::max();
    struct Envelope {
        float attack_seconds = 0.002f;
        float decay_seconds = 0.0f;
        float sustain = 1.0f;
        float release_seconds = 0.020f;
    };
    struct VoiceParams {
        // Source frames advanced per output frame. No chip clock in renderer.
        double pitch = 1.0;
        float gain = 1.0f;
        float pan = 0.0f; // equal-power pan: -1 left, 0 centre, +1 right
        uint32_t start_frame = 0;
        bool loop = true;
        uint32_t loop_start = kHeaderLoop;
        uint32_t loop_end = 0; // exclusive; zero uses the sample header end
        Envelope envelope{};
        bool effect = false; // an effect's voice, else music's: which of the two volumes scales it
    };
    struct Stats {
        uint64_t rendered_frames = 0;
        uint64_t clipped_samples = 0; // hard-clamped channel samples after limiting
        uint64_t limited_frames = 0; // stereo frames attenuated by peak protection
        uint64_t rejected_commands = 0;
    };

    // All methods belong to one audio owner thread; callers serialize commands.
    // These methods and render allocate no memory. Invalid commands leave an
    // existing voice unchanged. Banks and their ROM storage are borrowed.
    bool note_on(unsigned slot, const NativeSampleBank& bank, unsigned sample,
                 const VoiceParams& params) noexcept;
    bool set_pitch(unsigned slot, double pitch) noexcept;
    bool set_gain(unsigned slot, float gain) noexcept;
    bool set_pan(unsigned slot, float pan) noexcept;
    void note_off(unsigned slot) noexcept;
    void stop(unsigned slot) noexcept;
    void all_stop() noexcept;
    bool active(unsigned slot) const noexcept;
    unsigned active_voices() const noexcept;
    bool set_master_gain(float gain) noexcept;
    // The launcher's music and effects volumes (0..1; both 1: unchanged).
    void set_volumes(float music, float effects) noexcept {
        volumes_[0] = std::clamp(music, 0.0f, 1.0f), volumes_[1] = std::clamp(effects, 0.0f, 1.0f);
    }
    // Enabled by default. Disable only for raw mixer/reference measurements.
    // Stereo-linked immediate attack, 50 ms exponential recovery; no lookahead.
    // Toggling or all_stop clears the attenuation state.
    void set_peak_limiter(bool enabled) noexcept;
    const Stats& stats() const noexcept { return stats_; }
    // Replaces output with stereo float [-1,1]. Linear source interpolation,
    // equal-power pan and linear ADSR are intentionally not chip-bit-exact.
    // Nominal source range is [-1,1); master gain precedes peak protection.
    // Limiter state persists across render calls; rendering adds no latency.
    void render(float* interleaved_stereo, size_t frames) noexcept;

private:
    enum class Stage : uint8_t { Attack, Decay, Sustain, Release };
    struct Voice {
        const NativeSampleBank* bank = nullptr;
        const NativeSampleBank::Sample* sample = nullptr;
        uint64_t phase = 0, step = 0;
        uint32_t loop_start = 0, end = 0;
        bool loop = false, active = false, effect = false;
        float gain = 0, pan = 0, left = 0, right = 0;
        float level = 0, stage_start = 0, stage_end = 0, sustain = 1;
        uint32_t stage_elapsed = 0, stage_frames = 0;
        uint32_t decay_frames = 0, release_frames = 0;
        Stage stage = Stage::Sustain;
    };
    static bool pitch_step(double pitch, uint64_t& step) noexcept;
    static void gains(Voice& voice) noexcept;
    static void begin_stage(Voice& voice, Stage stage, uint32_t frames,
                            float target) noexcept;
    static void advance_envelope(Voice& voice) noexcept;
    std::array<Voice, kVoices> voices_{};
    float master_gain_ = kDefaultMasterGain;
    float volumes_[2] = {1.0f, 1.0f}; // music, effects
    float limiter_gain_ = 1.0f;
    bool peak_limiter_ = true;
    Stats stats_{};
};

} // namespace snd
