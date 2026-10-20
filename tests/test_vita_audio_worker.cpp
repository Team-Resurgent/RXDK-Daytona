// The production Audio queue/mixer runs against SDL-shaped streams and real
// host threads. Stream mocks preserve bytes; they do not validate SDL resampling.
#include "../platform/vita/audio.h"
#include "../platform/vita/sound_worker.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "vita_audio_thread_mock.inc"

namespace audio_mock {
struct Device {
    SDL_AudioSpec spec;
    std::mutex lock;
    std::thread::id lock_owner;
    bool paused = true, alive = true;
};
std::array<std::unique_ptr<Device>, 8> devices;
SDL_AudioDeviceID last_id = 0;
bool fail_open = false;
std::mutex queue_gate;
std::condition_variable queue_changed;
bool block_queue = false, entered_queue = false;
std::atomic<uint64_t> clock_ticks{0};
uint64_t clock() { return clock_ticks.fetch_add(11); }
Device &device(SDL_AudioDeviceID id) {
    mock::require(id && id < devices.size() && devices[id] && devices[id]->alive, "live audio device");
    return *devices[id];
}
void require_locked(SDL_AudioDeviceID id) {
    mock::require(device(id).lock_owner == std::this_thread::get_id(), "stream touched under audio device lock");
}
std::vector<int16_t> render(SDL_AudioDeviceID id, int samples);
}
struct SDL_AudioStream { SDL_AudioDeviceID owner; std::vector<uint8_t> data; };

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *, int, const SDL_AudioSpec *want, SDL_AudioSpec *got, int) {
    if (audio_mock::fail_open) return 0;
    mock::require(want->freq == 48000 && want->format == AUDIO_S16SYS && want->channels == 2, "output format preserved");
    const auto id = ++audio_mock::last_id;
    auto &device = audio_mock::devices[id];
    device = std::make_unique<audio_mock::Device>();
    device->spec = *want; *got = *want; return id;
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID id) {
    auto &device = audio_mock::device(id);
    std::lock_guard<std::mutex> lock(device.lock);
    device.alive = false;
}
void SDL_LockAudioDevice(SDL_AudioDeviceID id) {
    auto &device = audio_mock::device(id);
    device.lock.lock(); device.lock_owner = std::this_thread::get_id();
}
void SDL_UnlockAudioDevice(SDL_AudioDeviceID id) {
    auto &device = audio_mock::device(id);
    device.lock_owner = {}; device.lock.unlock();
}
void SDL_PauseAudioDevice(SDL_AudioDeviceID id, int paused) {
    SDL_LockAudioDevice(id);
    audio_mock::device(id).paused = paused != 0;
    SDL_UnlockAudioDevice(id);
}
SDL_AudioStream *SDL_NewAudioStream(Uint16 src, Uint8 src_channels, int src_rate,
                                   Uint16 dst, Uint8 dst_channels, int dst_rate) {
    mock::require(src == AUDIO_F32SYS && dst == AUDIO_F32SYS &&
                  src_channels == 2 && dst_channels == 2 && dst_rate == 48000 &&
                  (src_rate == 55556 || src_rate == 44643), "original SDL resampler configuration preserved");
    return new SDL_AudioStream{audio_mock::last_id, {}};
}
void SDL_FreeAudioStream(SDL_AudioStream *stream) { delete stream; }
int SDL_AudioStreamAvailable(SDL_AudioStream *stream) {
    audio_mock::require_locked(stream->owner); return int(stream->data.size());
}
int SDL_AudioStreamGet(SDL_AudioStream *stream, void *output, int bytes) {
    audio_mock::require_locked(stream->owner);
    const int count = std::min(bytes, int(stream->data.size()));
    std::memcpy(output, stream->data.data(), size_t(count));
    stream->data.erase(stream->data.begin(), stream->data.begin() + count);
    return count;
}
int SDL_AudioStreamPut(SDL_AudioStream *stream, const void *input, int bytes) {
    audio_mock::require_locked(stream->owner);
    {
        std::unique_lock<std::mutex> lock(audio_mock::queue_gate);
        if (audio_mock::block_queue) {
            audio_mock::entered_queue = true; audio_mock::queue_changed.notify_all();
            audio_mock::queue_changed.wait(lock, [] { return !audio_mock::block_queue; });
        }
    }
    const auto *data = static_cast<const uint8_t *>(input);
    stream->data.insert(stream->data.end(), data, data + bytes);
    return 0;
}
void SDL_AudioStreamClear(SDL_AudioStream *stream) {
    audio_mock::require_locked(stream->owner); stream->data.clear();
}
const char *SDL_GetError() { return "mock audio unavailable"; }
std::vector<int16_t> audio_mock::render(SDL_AudioDeviceID id, int samples) {
    std::vector<int16_t> output(size_t(samples), 0);
    SDL_LockAudioDevice(id);
    auto &dev = device(id);
    if (!dev.paused) dev.spec.callback(dev.spec.userdata, reinterpret_cast<Uint8 *>(output.data()), samples * int(sizeof(int16_t)));
    SDL_UnlockAudioDevice(id);
    return output;
}

struct AudioGame {
    snd::SoundBoard board;
    const std::thread::id owner = std::this_thread::get_id();
    bool expect_worker = true;
    int frames = 0, completed = 0;
    uint64_t execute_deferred_sound() {
        mock::require((std::this_thread::get_id() != owner) == expect_worker, "sound generation thread");
        board.fm.resize(4096); board.pcm.resize(4096);
        for (int i = 0; i < 4096; ++i) {
            board.fm[size_t(i)] = float((i + frames) % 31 - 15) / 64.f;
            board.pcm[size_t(i)] = float((i * 3 + frames) % 23 - 11) / 128.f;
        }
        ++frames; return 17;
    }
    snd::SoundBoard *sound() { return &board; }
    void complete_deferred_sound(uint64_t ticks) {
        mock::require(std::this_thread::get_id() == owner && ticks == 17, "owner completes sound timing");
        mock::require(board.fm.empty() && board.pcm.empty(), "queue drained sound before completion");
        ++completed;
    }
};

struct AudioPacket {
    AudioGame *source;
    uint64_t execute() { return source->execute_deferred_sound(); }
    snd::SoundBoard *sound() { return source->sound(); }
};

int main() {
    try {
        mock::reset();
        vita::Audio reference, threaded;
        mock::require(reference.open(), "reference audio opens");
        const auto reference_id = audio_mock::last_id;
        mock::require(threaded.open(), "threaded audio opens");
        const auto threaded_id = audio_mock::last_id;
        vita::SoundWorker worker;
        mock::require(worker.open(), "audio worker opens");
        AudioGame serial_game, worker_game;
        serial_game.expect_worker = false;
        for (int frame = 0; frame < 64; ++frame) {
            const float gain = float(frame % 11) / 10.f;
            reference.volume(gain); threaded.volume(gain);
            reference.mute(frame % 13 == 0); threaded.mute(frame % 13 == 0);
            serial_game.execute_deferred_sound();
            reference.push(*serial_game.sound());
            serial_game.complete_deferred_sound(17);
            worker.dispatch(worker_game, threaded, audio_mock::clock);
            worker.finish();
            mock::require(worker.last_audio_ticks() == 11, "queue time measured separately");
            mock::require(audio_mock::render(reference_id, 4096) == audio_mock::render(threaded_id, 4096),
                          "queue thread leaves callback PCM samples bit-identical");
        }
        // The detached packet queues through the same production Audio path,
        // without completing into the current main-board profile.
        AudioPacket packet{&worker_game};
        for (int frame = 0; frame < 64; ++frame) {
            reference.volume(float(frame % 7) / 6.f); threaded.volume(float(frame % 7) / 6.f);
            reference.mute(frame % 11 == 0); threaded.mute(frame % 11 == 0);
            serial_game.execute_deferred_sound(); reference.push(*serial_game.sound());
            serial_game.complete_deferred_sound(17);
            worker.dispatch_packet(packet, threaded, audio_mock::clock);
            worker.finish();
            mock::require(worker_game.completed == 64 && worker.last_sound_ticks() == 17,
                          "detached queue has no game-profile completion");
            mock::require(audio_mock::render(reference_id, 4096) == audio_mock::render(threaded_id, 4096),
                          "detached callback PCM bit-identical with settings after join");
        }
        reference.pause(); threaded.pause();

        // Block inside stream conversion: dispatch returns, and finish must
        // wait for conversion as well as emulation before main may touch audio.
        {
            std::lock_guard<std::mutex> lock(audio_mock::queue_gate);
            audio_mock::block_queue = true; audio_mock::entered_queue = false;
        }
        worker.dispatch(worker_game, threaded, audio_mock::clock);
        {
            std::unique_lock<std::mutex> lock(audio_mock::queue_gate);
            audio_mock::queue_changed.wait(lock, [] { return audio_mock::entered_queue; });
            mock::require(worker_game.completed == 64, "conversion still pending after sound execution");
            audio_mock::block_queue = false; audio_mock::queue_changed.notify_all();
        }
        worker.finish();
        mock::require(worker_game.completed == 65, "finish includes queue conversion");

        // A playback thread races queue producers exactly as SDL does, using
        // the device lock. ThreadSanitizer verifies the production lock scope.
        std::atomic<bool> stop{false};
        std::thread playback([&] {
            while (!stop.load()) { audio_mock::render(threaded_id, 512); std::this_thread::yield(); }
        });
        for (int frame = 0; frame < 120; ++frame) {
            worker.dispatch_packet(packet, threaded, audio_mock::clock);
            worker.finish();
            threaded.volume(float(frame % 5) / 4.f);
            threaded.mute(frame % 17 == 0);
        }
        stop = true; playback.join();
        worker.close(); threaded.pause(); threaded.close(); reference.close();

        // No worker or unavailable audio still executes and drains each frame.
        mock::reset(4);
        vita::SoundWorker fallback;
        mock::require(!fallback.open(), "thread failure gives serial fallback");
        audio_mock::fail_open = true;
        vita::Audio unavailable;
        mock::require(!unavailable.open(), "device failure reproduced");
        AudioGame fallback_game; fallback_game.expect_worker = false;
        fallback.dispatch(fallback_game, unavailable, audio_mock::clock);
        fallback.finish();
        mock::require(fallback_game.completed == 1 && fallback.last_audio_ticks() == 11,
                      "fallback drains audio and retains queue timing");
        fallback.close(); mock::reset();
        std::puts("PASS: actual Audio queue/callback parity, conversion overlap, SDL device locking, settings after join and unavailable-device fallback");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
