#include "native_sample_mixer.h"

#include <algorithm>
#include <cmath>

namespace snd {
namespace {
constexpr uint32_t kWindow = 0x100000;
constexpr double kPhaseScale = 4294967296.0;
constexpr uint64_t kPhaseOne = uint64_t{1} << 32;
// 1 - exp(-1 / (48000 * 0.050)); evaluated once, not in the audio callback.
constexpr float kLimiterRecovery = 0.000416579873f;

bool bounded(float value, float lo, float hi) noexcept {
    return std::isfinite(value) && value >= lo && value <= hi;
}
uint32_t seconds_to_frames(float seconds) noexcept {
    return uint32_t(double(seconds) * NativeSampleMixer::kOutputRate + 0.5);
}
} // namespace

bool NativeSampleBank::load(const uint8_t* rom, size_t bytes,
                            unsigned data_bank) noexcept {
    rom_ = nullptr;
    rom_bytes_ = 0;
    data_bank_ = 0;
    valid_count_ = 0;
    samples_.fill({});
    status_.fill(Status::MissingTable);
    if (!rom || bytes < kSamples * 12 || data_bank >= 4) return false;
    rom_ = rom;
    rom_bytes_ = bytes;
    data_bank_ = data_bank;
    for (unsigned index = 0; index < kSamples; ++index) {
        const uint8_t* h = rom + index * 12;
        const uint32_t address = uint32_t(h[0]) << 16 |
                                 uint32_t(h[1]) << 8 | h[2];
        Sample& s = samples_[index];
        s.byte_start = address & 0x3fffff;
        s.format = (address & 0x400000) ? Format::PackedSigned12 : Format::Signed8;
        // Bit 23 is not part of the address or the packed-sample selector.
        s.loop_start = uint32_t(h[3]) << 8 | h[4];
        s.frames = 65536u - (uint32_t(h[5]) << 8 | h[6]);
        s.attack_hint = h[8] >> 4;
        s.decay_hint = h[8] & 15;
        s.sustain_decay_hint = h[9] & 15;
        s.sustain_hint = h[9] >> 4;
        s.release_hint = h[10] & 15;
        if (s.loop_start >= s.frames) {
            status_[index] = Status::InvalidLoop;
            continue;
        }
        const uint32_t encoded_bytes = s.format == Format::Signed8 ? s.frames :
            (s.frames / 2) * 3 + (s.frames % 2) * 2;
        if (!mapped_range(s.byte_start, encoded_bytes)) {
            status_[index] = Status::UnmappedData;
            continue;
        }
        status_[index] = Status::Valid;
        ++valid_count_;
    }
    return true;
}

bool NativeSampleBank::mapped_range(uint32_t start, uint32_t bytes) const noexcept {
    if (!rom_ || start >= 2 * kWindow || bytes > 2 * kWindow - start) return false;
    const uint32_t end = start + bytes;
    if (start < kWindow && size_t(std::min(end, kWindow)) > rom_bytes_) return false;
    if (end > kWindow) {
        const size_t physical_end = size_t(data_bank_) * kWindow + end - kWindow;
        if (physical_end > rom_bytes_) return false;
    }
    return true;
}

const NativeSampleBank::Sample* NativeSampleBank::sample(unsigned index) const noexcept {
    return index < kSamples && status_[index] == Status::Valid ? &samples_[index] : nullptr;
}

NativeSampleBank::Status NativeSampleBank::status(unsigned index) const noexcept {
    return index < kSamples ? status_[index] : Status::MissingTable;
}

uint8_t NativeSampleBank::byte(uint32_t logical) const noexcept {
    // Called only for ranges validated at load, and frame-bounded by value().
    const size_t physical = logical < kWindow ? logical :
        size_t(data_bank_) * kWindow + logical - kWindow;
    return rom_[physical];
}

float NativeSampleBank::value(const Sample& s, uint32_t frame) const noexcept {
    if (s.format == Format::Signed8) {
        const unsigned raw = byte(s.byte_start + frame);
        return float(raw < 128 ? int(raw) : int(raw) - 256) * (1.0f / 128.0f);
    }
    const uint32_t address = s.byte_start + (frame / 2) * 3;
    const unsigned raw = (frame & 1) ?
        (unsigned(byte(address + 2)) << 4) | (byte(address + 1) >> 4) :
        (unsigned(byte(address)) << 4) | (byte(address + 1) & 15);
    return float(raw < 2048 ? int(raw) : int(raw) - 4096) * (1.0f / 2048.0f);
}

float NativeSampleBank::value(unsigned index, uint32_t frame) const noexcept {
    const Sample* s = sample(index);
    return s && frame < s->frames ? value(*s, frame) : 0.0f;
}

bool NativeSampleMixer::pitch_step(double pitch, uint64_t& step) noexcept {
    if (!std::isfinite(pitch) || pitch < 1.0 / kPhaseScale || pitch > 256.0) return false;
    step = uint64_t(pitch * kPhaseScale + 0.5);
    return true;
}

void NativeSampleMixer::gains(Voice& v) noexcept {
    constexpr float quarter_pi = 0.78539816339744830962f;
    const float angle = (v.pan + 1.0f) * quarter_pi;
    v.left = v.pan == 1.0f ? 0.0f : v.gain * std::cos(angle);
    v.right = v.pan == -1.0f ? 0.0f : v.gain * std::sin(angle);
}

void NativeSampleMixer::begin_stage(Voice& v, Stage stage, uint32_t frames,
                                    float target) noexcept {
    v.stage = stage;
    v.stage_start = v.level;
    v.stage_end = target;
    v.stage_elapsed = 0;
    v.stage_frames = frames;
}

void NativeSampleMixer::advance_envelope(Voice& v) noexcept {
    // At most attack -> decay -> sustain can be skipped, all constant work.
    for (unsigned transitions = 0; transitions < 3; ++transitions) {
        if (v.stage == Stage::Sustain) return;
        if (v.stage_frames) {
            ++v.stage_elapsed;
            const float fraction = float(v.stage_elapsed) / float(v.stage_frames);
            v.level = v.stage_start + (v.stage_end - v.stage_start) * fraction;
            if (v.stage_elapsed < v.stage_frames) return;
        }
        v.level = v.stage_end;
        const bool consumed_frame = v.stage_frames != 0;
        if (v.stage == Stage::Attack) {
            begin_stage(v, Stage::Decay, v.decay_frames, v.sustain);
        } else if (v.stage == Stage::Decay) {
            v.stage = Stage::Sustain;
        } else {
            v.active = false;
            return;
        }
        if (consumed_frame) return;
    }
}

bool NativeSampleMixer::note_on(unsigned slot, const NativeSampleBank& bank,
                                unsigned sample_index, const VoiceParams& p) noexcept {
    const auto* sample = bank.sample(sample_index);
    uint64_t step = 0;
    const uint32_t end = p.loop_end ? p.loop_end : (sample ? sample->frames : 0);
    const uint32_t loop = p.loop_start == kHeaderLoop ?
        (sample ? sample->loop_start : 0) : p.loop_start;
    if (slot >= kVoices || !sample || !pitch_step(p.pitch, step) ||
        !bounded(p.gain, 0, 4) || !bounded(p.pan, -1, 1) ||
        !bounded(p.envelope.attack_seconds, 0, 60) ||
        !bounded(p.envelope.decay_seconds, 0, 60) ||
        !bounded(p.envelope.sustain, 0, 1) ||
        !bounded(p.envelope.release_seconds, 0, 60) ||
        end > sample->frames || p.start_frame >= end || (p.loop && loop >= end)) {
        ++stats_.rejected_commands;
        return false;
    }
    Voice v;
    v.bank = &bank;
    v.sample = sample;
    v.phase = uint64_t(p.start_frame) << 32;
    v.step = step;
    v.loop_start = loop;
    v.end = end;
    v.loop = p.loop;
    v.active = true;
    v.gain = p.gain;
    v.effect = p.effect;
    v.pan = p.pan;
    gains(v);
    v.sustain = p.envelope.sustain;
    v.decay_frames = seconds_to_frames(p.envelope.decay_seconds);
    v.release_frames = seconds_to_frames(p.envelope.release_seconds);
    begin_stage(v, Stage::Attack, seconds_to_frames(p.envelope.attack_seconds), 1.0f);
    voices_[slot] = v;
    return true;
}

bool NativeSampleMixer::set_pitch(unsigned slot, double pitch) noexcept {
    uint64_t step = 0;
    if (slot >= kVoices || !pitch_step(pitch, step)) {
        ++stats_.rejected_commands;
        return false;
    }
    voices_[slot].step = step;
    return true;
}

bool NativeSampleMixer::set_gain(unsigned slot, float gain) noexcept {
    if (slot >= kVoices || !bounded(gain, 0, 4)) {
        ++stats_.rejected_commands;
        return false;
    }
    voices_[slot].gain = gain;
    gains(voices_[slot]);
    return true;
}

bool NativeSampleMixer::set_pan(unsigned slot, float pan) noexcept {
    if (slot >= kVoices || !bounded(pan, -1, 1)) {
        ++stats_.rejected_commands;
        return false;
    }
    voices_[slot].pan = pan;
    gains(voices_[slot]);
    return true;
}

void NativeSampleMixer::note_off(unsigned slot) noexcept {
    if (slot >= kVoices) { ++stats_.rejected_commands; return; }
    Voice& v = voices_[slot];
    if (!v.active || v.stage == Stage::Release) return;
    if (!v.release_frames) { v.active = false; return; }
    begin_stage(v, Stage::Release, v.release_frames, 0.0f);
}

void NativeSampleMixer::stop(unsigned slot) noexcept {
    if (slot >= kVoices) { ++stats_.rejected_commands; return; }
    voices_[slot].active = false;
}

void NativeSampleMixer::all_stop() noexcept {
    for (Voice& v : voices_) v.active = false;
    limiter_gain_ = 1.0f;
}

bool NativeSampleMixer::active(unsigned slot) const noexcept {
    return slot < kVoices && voices_[slot].active;
}

unsigned NativeSampleMixer::active_voices() const noexcept {
    unsigned count = 0;
    for (const Voice& v : voices_) count += v.active;
    return count;
}

bool NativeSampleMixer::set_master_gain(float gain) noexcept {
    if (!bounded(gain, 0, 4)) { ++stats_.rejected_commands; return false; }
    master_gain_ = gain;
    return true;
}

void NativeSampleMixer::set_peak_limiter(bool enabled) noexcept {
    peak_limiter_ = enabled;
    limiter_gain_ = 1.0f;
}

void NativeSampleMixer::render(float* out, size_t frames) noexcept {
    if (!out || frames > std::numeric_limits<size_t>::max() / 2) return;
    std::fill_n(out, frames * 2, 0.0f);
    for (Voice& v : voices_) {
        if (!v.active) continue;
        const uint64_t end = uint64_t(v.end) << 32;
        const uint64_t loop = uint64_t(v.loop_start) << 32;
        const float volume = volumes_[v.effect];
        for (size_t frame = 0; frame < frames && v.active; ++frame) {
            advance_envelope(v);
            if (!v.active) break;
            const uint32_t index = uint32_t(v.phase >> 32);
            const uint32_t next = index + 1 < v.end ? index + 1 :
                (v.loop ? v.loop_start : index);
            const float a = v.bank->value(*v.sample, index);
            const float b = v.bank->value(*v.sample, next);
            const float fraction = float(uint32_t(v.phase)) * float(1.0 / kPhaseScale);
            const float sample = (a + (b - a) * fraction) * v.level * volume;
            out[frame * 2] += sample * v.left;
            out[frame * 2 + 1] += sample * v.right;
            v.phase += v.step;
            if (v.phase >= end) {
                if (v.loop) v.phase = loop + (v.phase - end) % (end - loop);
                else v.active = false;
            }
        }
    }
    for (size_t frame = 0; frame < frames; ++frame) {
        float left = out[frame * 2] * master_gain_;
        float right = out[frame * 2 + 1] * master_gain_;
        if (peak_limiter_) {
            limiter_gain_ += (1.0f - limiter_gain_) * kLimiterRecovery;
            // Avoid a float-rounding tail that never returns exactly to unity.
            if (limiter_gain_ >= 0.9999f) limiter_gain_ = 1.0f;
            const float peak = std::max(std::fabs(left), std::fabs(right));
            if (peak > kPeakCeiling)
                limiter_gain_ = std::min(limiter_gain_, kPeakCeiling / peak);
            // One gain for both channels preserves the stereo position.
            left *= limiter_gain_;
            right *= limiter_gain_;
            if (limiter_gain_ < 1.0f) ++stats_.limited_frames;
        }
        if (left < -1.0f || left > 1.0f) ++stats_.clipped_samples;
        if (right < -1.0f || right > 1.0f) ++stats_.clipped_samples;
        out[frame * 2] = std::clamp(left, -1.0f, 1.0f);
        out[frame * 2 + 1] = std::clamp(right, -1.0f, 1.0f);
    }
    stats_.rendered_frames += frames;
}

} // namespace snd
