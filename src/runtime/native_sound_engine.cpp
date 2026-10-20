#include "runtime/native_sound_engine.h"
#include <stdexcept>
#include <utility>

namespace snd {
NativeSoundEngine::NativeSoundEngine(std::vector<uint8_t> program,
                                   std::vector<uint8_t> pcm1, std::vector<uint8_t> pcm2)
    : program_(std::move(program)), pcm_{std::move(pcm1), std::move(pcm2)},
      sequencer_(program_, NativeSampleMixer::kOutputRate) {
    for (unsigned rom = 0; rom < 2; ++rom)
        for (unsigned bank = 0; bank < 4; ++bank)
            if (!banks_[rom][bank].load(pcm_[rom].data(), pcm_[rom].size(), bank))
                throw std::runtime_error("native audio: missing PCM sample table");
    sequencer_.set_sink(event, this);
}

void NativeSoundEngine::send(const uint8_t *bytes, size_t count) {
    if (count && !bytes) throw std::runtime_error("native audio: null command packet");
    sequencer_.send(bytes, count);
    if (sequencer_.failed()) throw std::runtime_error("native audio: invalid command/sequence data");
}

void NativeSoundEngine::event(void *context, const NativeSoundSequencer::VoiceEvent &value) {
    static_cast<NativeSoundEngine *>(context)->apply(value);
}

void NativeSoundEngine::render_to(uint64_t frame) {
    if (frame < cursor_ || (output_ && frame > end_) || (!output_ && frame != cursor_))
        throw std::runtime_error("native audio: non-monotonic event timestamp");
    const size_t count = size_t(frame - cursor_);
    if (count) {
        mixer_.render(output_ + written_ * 2, count);
        written_ += count;
        cursor_ = frame;
    }
}

void NativeSoundEngine::apply(const NativeSoundSequencer::VoiceEvent &value) {
    render_to(value.frame);
    if (value.voice_id >= NativeSampleMixer::kVoices) { ++invalid_; return; }
    using Kind = NativeSoundSequencer::EventKind;
    switch (value.kind) {
    case Kind::NoteOn: {
        if (value.rom >= banks_.size() || value.bank >= banks_[value.rom].size()) { ++invalid_; return; }
        NativeSampleMixer::VoiceParams params;
        params.pitch = value.source_rate_hz / NativeSampleMixer::kOutputRate;
        params.gain = value.gain;
        params.pan = value.pan;
        params.effect = !value.music;
        mixer_.note_on(value.voice_id, banks_[value.rom][value.bank], value.sample_index, params);
        break;
    }
    case Kind::NoteOff: mixer_.note_off(value.voice_id); break;
    case Kind::Stop: mixer_.stop(value.voice_id); break;
    case Kind::Update:
        // A released one-shot may have ended before a late control update.
        if (mixer_.active(value.voice_id)) {
            mixer_.set_pitch(value.voice_id, value.source_rate_hz / NativeSampleMixer::kOutputRate);
            mixer_.set_gain(value.voice_id, value.gain);
            mixer_.set_pan(value.voice_id, value.pan);
        }
        break;
    }
}

void NativeSoundEngine::render(float *stereo, size_t frames) {
    if (!frames) return;
    if (!stereo || frames > UINT64_MAX - cursor_) throw std::runtime_error("native audio: invalid output buffer");
    output_ = stereo; written_ = 0; end_ = cursor_ + frames;
    try {
        sequencer_.advance(frames);
        render_to(end_);
        if (sequencer_.failed()) throw std::runtime_error("native audio: sequence validation failed");
    } catch (...) { output_ = nullptr; throw; }
    output_ = nullptr;
}

NativeSoundEngine::Stats NativeSoundEngine::stats() const {
    const auto &sequence = sequencer_.stats();
    const auto &mix = mixer_.stats();
    return {cursor_, sequence.input_bytes, sequence.note_ons, sequence.unsupported,
            invalid_ + sequence.invalid_data + sequence.event_limit_hits + mix.rejected_commands,
            mix.clipped_samples, mixer_.active_voices(), mix.limited_frames};
}
} // namespace snd
