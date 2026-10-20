// Opt-in real-ROM health test. Requires the user's imported ROM directory and
// generated main/TGP code. No reference sound board or sound CPU is instantiated.
// Usage: test_native_sound_rom ROM_CACHE_DIR [FRAMES=6000] [OUTPUT.wav]
#include "runtime/game_loop.h"
#include "runtime/native_sound_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
bool count_audio_allocations = false;
uint64_t audio_allocations = 0;
}
void* operator new(size_t bytes) {
    if (count_audio_allocations) ++audio_allocations;
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::vector<uint8_t> load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "cannot open imported audio ROM");
    return {std::istreambuf_iterator<char>(file), {}};
}
rt::Inputs input_for(unsigned frame) {
    rt::Inputs inputs;
    if ((frame >= 1200 && frame < 1210) || (frame >= 1240 && frame < 1250) ||
        (frame >= 1280 && frame < 1290)) inputs.in0 &= uint8_t(~0x01);
    if ((frame >= 1400 && frame < 1410) || (frame >= 1600 && frame < 1610) ||
        (frame >= 2000 && frame < 2010) || (frame >= 2400 && frame < 2410))
        inputs.in0 &= uint8_t(~0x10);
    if ((frame >= 1800 && frame < 1810) || (frame >= 2200 && frame < 2210))
        inputs.in0 &= uint8_t(~0x20);
    if (frame >= 1400) inputs.accel = 0xe0;
    return inputs;
}
struct AllocationGuard {
    AllocationGuard() { count_audio_allocations = true; }
    ~AllocationGuard() { count_audio_allocations = false; }
};
struct Energy {
    uint64_t samples = 0, nonzero = 0;
    double squares = 0, peak = 0;
    void add(const float* data, size_t frames) {
        samples += frames * 2;
        for (size_t i = 0; i < frames * 2; ++i) {
            require(std::isfinite(data[i]), "nonfinite native output");
            require(data[i] >= -1 && data[i] <= 1, "native output exceeds float range");
            nonzero += data[i] != 0;
            squares += double(data[i]) * data[i];
            peak = std::max(peak, std::fabs(double(data[i])));
        }
    }
    double rms() const { return samples ? std::sqrt(squares / samples) : 0; }
};
struct EventObserver {
    std::array<std::array<snd::NativeSampleBank, 4>, 2> banks;
    std::array<std::array<bool, 512>, 2> used{};
    uint64_t nonengine_notes = 0, engine_notes = 0, engine_updates = 0;
    uint64_t invalid = 0, last_frame = 0;
    double lowest_rate = std::numeric_limits<double>::infinity(), highest_rate = 0;
    static void receive(void* context, const snd::NativeSoundSequencer::VoiceEvent& event) {
        auto& self = *static_cast<EventObserver*>(context);
        require(event.frame >= self.last_frame, "native event time regressed");
        self.last_frame = event.frame;
        using Kind = snd::NativeSoundSequencer::EventKind;
        if (event.kind == Kind::Update && event.voice_id >= 46) ++self.engine_updates;
        if (event.kind != Kind::NoteOn) return;
        if (event.voice_id >= 46) ++self.engine_notes; else ++self.nonengine_notes;
        self.lowest_rate = std::min(self.lowest_rate, event.source_rate_hz);
        self.highest_rate = std::max(self.highest_rate, event.source_rate_hz);
        const bool valid = event.rom < 2 && event.bank < 4 &&
            self.banks[event.rom][event.bank].sample(event.sample_index) &&
            std::isfinite(event.source_rate_hz) && event.source_rate_hz > 0 &&
            event.source_rate_hz <= 256.0 * snd::NativeSampleMixer::kOutputRate &&
            std::isfinite(event.gain) && event.gain >= 0 && event.gain <= 4 &&
            std::isfinite(event.pan) && event.pan >= -1 && event.pan <= 1;
        if (!valid) {
            if (self.invalid < 8)
                std::fprintf(stderr, "invalid native note: output_frame=%llu voice=%u rom=%u bank=%u sample=%u rate=%g gain=%g pan=%g\n",
                    (unsigned long long)event.frame, event.voice_id, event.rom, event.bank,
                    event.sample_index, event.source_rate_hz, event.gain, event.pan);
            ++self.invalid;
        } else self.used[event.rom][event.sample_index] = true;
    }
    unsigned samples_used() const {
        unsigned count = 0;
        for (const auto& rom : used) for (bool used_sample : rom) count += used_sample;
        return count;
    }
};
class Wav {
public:
    explicit Wav(const char* path) {
        if (!path) return;
        file_.open(path, std::ios::binary | std::ios::trunc);
        require(bool(file_), "cannot create requested WAV output");
        file_.write("RIFF", 4); u32(0); file_.write("WAVEfmt ", 8);
        u32(16); u16(1); u16(2); u32(48000); u32(48000 * 4); u16(4); u16(16);
        file_.write("data", 4); u32(0);
    }
    void append(const float* stereo, size_t frames) {
        if (!file_.is_open()) return;
        require(frames <= 512 && bytes_ <= UINT32_MAX - frames * 4 - 36, "WAV exceeds supported size");
        std::array<uint8_t, 2048> bytes;
        for (size_t i = 0; i < frames * 2; ++i) {
            const auto value = uint16_t(int16_t(std::lrint(stereo[i] * 32767.0f)));
            bytes[i * 2] = uint8_t(value); bytes[i * 2 + 1] = uint8_t(value >> 8);
        }
        file_.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(frames * 4));
        bytes_ += uint32_t(frames * 4);
        require(bool(file_), "WAV write failed");
    }
    void close() {
        if (!file_.is_open()) return;
        file_.seekp(4); u32(bytes_ + 36); file_.seekp(40); u32(bytes_);
        require(bool(file_), "WAV header write failed");
        file_.close();
        require(bool(file_), "WAV close failed");
    }
private:
    void u16(uint16_t n) { file_.put(char(n)); file_.put(char(n >> 8)); }
    void u32(uint32_t n) { u16(uint16_t(n)); u16(uint16_t(n >> 16)); }
    std::ofstream file_;
    uint32_t bytes_ = 0;
};
} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        std::fprintf(stderr, "usage: %s ROM_CACHE_DIR [FRAMES=6000] [OUTPUT.wav]\n", argv[0]);
        return 2;
    }
    unsigned game_frame = 0;
    try {
        char* count_end = nullptr;
        const unsigned long parsed = argc > 2 ? std::strtoul(argv[2], &count_end, 10) : 6000;
        require(parsed && parsed <= 1000000 && (!count_end || !*count_end), "invalid game frame count");
        const unsigned count = unsigned(parsed);
        const std::string directory(argv[1]);
        const auto program = load(directory + "/sound_program.bin");
        const auto pcm1 = load(directory + "/pcm1.bin"), pcm2 = load(directory + "/pcm2.bin");
        snd::NativeSoundEngine engine(program, pcm1, pcm2);
        snd::NativeSoundSequencer observer_sequence(program);
        EventObserver observer;
        for (unsigned bank = 0; bank < 4; ++bank) {
            require(observer.banks[0][bank].load(pcm1.data(), pcm1.size(), bank), "PCM1 table missing");
            require(observer.banks[1][bank].load(pcm2.data(), pcm2.size(), bank), "PCM2 table missing");
        }
        observer_sequence.set_sink(EventObserver::receive, &observer);
        rt::GameLoop game(directory, false);
        require(game.sound() == nullptr, "reference sound board must not exist");
        game.board().video().set_external_3d(true);
        Wav wav(argc > 3 ? argv[3] : nullptr);
        std::array<float, 1024> audio;
        Energy total, attract, race, stall;
        uint64_t fraction = 0, expected_frames = 0, command_bytes = 0;
        uint64_t render_calls = 0;
        double render_ms = 0, max_render_ms = 0;
        unsigned max_voices = 0;
        auto render = [&](size_t frames, Energy& phase) {
            while (frames) {
                const size_t block = std::min<size_t>(frames, 512);
                audio.fill(std::numeric_limits<float>::quiet_NaN());
                const auto begin = std::chrono::steady_clock::now();
                {
                    AllocationGuard guard;
                    engine.render(audio.data(), block);
                }
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - begin).count();
                render_ms += ms; max_render_ms = std::max(max_render_ms, ms); ++render_calls;
                observer_sequence.advance(block);
                require(!observer_sequence.failed(), "observer sequence validation failed");
                total.add(audio.data(), block); phase.add(audio.data(), block);
                wav.append(audio.data(), block);
                expected_frames += block;
                max_voices = std::max(max_voices, engine.stats().voices);
                require(engine.stats().frames == expected_frames, "native output clock drifted");
                frames -= block;
            }
        };
        for (; game_frame < count; ++game_frame) {
            game.run_frame(input_for(game_frame));
            require(game.sound() == nullptr, "reference sound board appeared during run");
            auto bytes = game.board().take_sound_bytes();
            command_bytes += bytes.size();
            observer_sequence.send(bytes.data(), bytes.size());
            {
                AllocationGuard guard;
                engine.send(bytes.data(), bytes.size());
            }
            // Exact board timing: 16 MHz / (656 * 424), not rounded 60 Hz.
            fraction += uint64_t(snd::NativeSampleMixer::kOutputRate) * 656 * 424;
            const size_t frames = size_t(fraction / 16000000);
            fraction %= 16000000;
            render(frames, game_frame < 1200 ? attract : race);
            if (game_frame == 3000) {
                const uint64_t before_game_frames = game.frames(), before_instructions = game.instructions();
                const uint64_t before_bytes = engine.stats().bytes, before_audio = engine.stats().frames;
                const auto before_notes = observer_sequence.stats().note_ons;
                render(48000, stall);
                require(engine.stats().frames == before_audio + 48000, "render-only stall did not advance one second");
                require(game.frames() == before_game_frames && game.instructions() == before_instructions,
                        "render-only stall advanced main board");
                require(engine.stats().bytes == before_bytes, "render-only stall invented main-board commands");
                std::printf("stall: no game steps/commands, audio_frames=48000 nonzero=%llu rms=%.6f new_notes=%llu\n",
                    (unsigned long long)stall.nonzero, stall.rms(),
                    (unsigned long long)(observer_sequence.stats().note_ons - before_notes));
            }
            if ((game_frame + 1) % 1000 == 0) {
                const auto stats = engine.stats();
                std::printf("progress: game_frames=%u audio_frames=%llu bytes=%llu notes=%llu voices=%u unsupported=%llu invalid=%llu\n",
                    game_frame + 1, (unsigned long long)stats.frames, (unsigned long long)stats.bytes,
                    (unsigned long long)stats.notes, stats.voices, (unsigned long long)stats.unsupported,
                    (unsigned long long)stats.invalid);
                std::fflush(stdout);
            }
        }
        wav.close();
        const auto stats = engine.stats();
        const auto& sequence = engine.sequence_stats();
        require(stats.bytes == command_bytes && command_bytes == game.board().sound_bytes_total(), "UART byte accounting differs");
        require(stats.frames == expected_frames && observer_sequence.frames() == expected_frames, "native clocks differ");
        require(sequence.note_ons == observer_sequence.stats().note_ons &&
                sequence.sequence_events == observer_sequence.stats().sequence_events, "native observer event totals differ");
        require(stats.invalid == 0 && observer.invalid == 0 && sequence.event_limit_hits == 0, "invalid native events or bounded parser overflow");
        require(audio_allocations == 0, "native command/render path allocated heap storage");
        require(stats.clipped == 0, "native output hard-clipped");
        require(total.peak <= snd::NativeSampleMixer::kPeakCeiling + 0.000001,
                "native output exceeded peak-protection ceiling");
        if (count >= 1200) require(total.nonzero && total.peak > 0.001, "native audio remained silent");
        if (count > 3000) {
            require(stall.nonzero && stall.rms() > 0.000001, "native audio stopped during graphics stall");
            require(sequence.sequence_events && observer.nonengine_notes && observer.engine_notes && observer.engine_updates,
                    "music or continuous engine event path was not exercised");
        }
        if (stats.unsupported) std::fprintf(stderr, "WARNING: unsupported native events=%llu\n", (unsigned long long)stats.unsupported);
        std::printf("PASS: game_frames=%u output_frames=%llu UART_bytes=%llu messages=%llu sequence_events=%llu notes=%llu off=%llu updates=%llu\n",
            count, (unsigned long long)stats.frames, (unsigned long long)command_bytes,
            (unsigned long long)sequence.messages, (unsigned long long)sequence.sequence_events,
            (unsigned long long)sequence.note_ons, (unsigned long long)sequence.note_offs,
            (unsigned long long)sequence.updates);
        std::printf("health: unsupported=%llu invalid=%llu callback_allocations=%llu clipped=%llu limited_frames=%llu nonzero=%llu peak=%.6f rms=%.6f attract_rms=%.6f race_rms=%.6f max_voices=%u\n",
            (unsigned long long)stats.unsupported, (unsigned long long)stats.invalid,
            (unsigned long long)audio_allocations, (unsigned long long)stats.clipped,
            (unsigned long long)stats.limited_frames,
            (unsigned long long)total.nonzero, total.peak, total.rms(), attract.rms(), race.rms(), max_voices);
        std::printf("coverage: unique_samples=%u nonengine_notes=%llu engine_notes=%llu engine_updates=%llu source_rate_hz=[%.3f,%.3f] render_calls=%llu avg_ms=%.6f max_ms=%.6f\n",
            observer.samples_used(), (unsigned long long)observer.nonengine_notes,
            (unsigned long long)observer.engine_notes, (unsigned long long)observer.engine_updates,
            observer.lowest_rate, observer.highest_rate, (unsigned long long)render_calls,
            render_calls ? render_ms / render_calls : 0, max_render_ms);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL at game_frame=%u: %s\n", game_frame, error.what());
        return 1;
    }
}
