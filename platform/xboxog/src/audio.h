// The game's sound through the native sequencer and sample mixer
// (snd::NativeSoundEngine, the desktop's --audio native) into one looping
// 48 kHz 16-bit DirectSound buffer. Each frame the game's sound board bytes
// go to the engine, then the engine renders just enough to keep kTarget
// frames queued ahead of the play cursor: the output clock paces the mixing,
// as SDL's audio callback does on the desktop, so the music never stretches
// with the frame rate.
#pragma once

#include <xtl.h>

#include "runtime/native_sound_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace xbox {

class Audio {
public:
    static constexpr DWORD kRate = 48000, kFrames = 16384, kBytes = kFrames * 4;
    static constexpr DWORD kTarget = 3840; // 80 ms queued

    explicit Audio(std::unique_ptr<snd::NativeSoundEngine> engine) : engine_(std::move(engine)) {
        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = kRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = kRate * 4;
        DSBUFFERDESC desc = {};
        desc.dwSize = sizeof(desc);
        desc.dwBufferBytes = kBytes;
        desc.lpwfxFormat = &format;
        if (FAILED(DirectSoundCreateBuffer(&desc, &buffer_))) {
            buffer_ = nullptr;
            return;
        }
        void *a = nullptr, *b = nullptr;
        DWORD an = 0, bn = 0;
        if (SUCCEEDED(buffer_->Lock(0, kBytes, &a, &an, &b, &bn, 0))) {
            std::fill_n(static_cast<uint8_t *>(a), an, uint8_t(0));
            buffer_->Unlock(a, an, b, bn);
        }
        buffer_->Play(0, 0, DSBPLAY_LOOPING);
    }

    bool playing() const { return buffer_ != nullptr; }

    // After each game frame: its sound board bytes, then top up the buffer.
    void frame(const std::vector<uint8_t> &bytes) {
        if (!bytes.empty()) engine_->send(bytes.data(), bytes.size());
        if (!buffer_) return;
        DirectSoundDoWork();
        DWORD play = 0, write = 0;
        if (FAILED(buffer_->GetCurrentPosition(&play, &write))) return;
        play /= 4;
        DWORD ahead = (write_ + kFrames - play) % kFrames;
        if (ahead > kFrames / 2) { // the play cursor passed us: start again ahead of it
            write_ = (play + kTarget / 2) % kFrames;
            ahead = kTarget / 2;
            ++underruns;
        }
        if (ahead >= kTarget) return;
        const DWORD frames = kTarget - ahead;
        mix_.resize(size_t(frames) * 2);
        engine_->render(mix_.data(), frames);
        void *a = nullptr, *b = nullptr;
        DWORD an = 0, bn = 0;
        if (FAILED(buffer_->Lock(write_ * 4, frames * 4, &a, &an, &b, &bn, 0))) return;
        convert(static_cast<int16_t *>(a), an / 2, mix_.data());
        convert(static_cast<int16_t *>(b), bn / 2, mix_.data() + an / 2);
        buffer_->Unlock(a, an, b, bn);
        write_ = (write_ + frames) % kFrames;
    }

    snd::NativeSoundEngine &engine() { return *engine_; }
    uint64_t underruns = 0;

private:
    std::unique_ptr<snd::NativeSoundEngine> engine_;
    LPDIRECTSOUNDBUFFER buffer_ = nullptr;
    DWORD write_ = 0; // frames
    std::vector<float> mix_;

    static void convert(int16_t *out, DWORD count, const float *in) {
        if (!out) return;
        for (DWORD i = 0; i < count; ++i) out[i] = int16_t(std::lround(std::clamp(in[i], -1.f, 1.f) * 32767.f));
    }
};

} // namespace xbox
