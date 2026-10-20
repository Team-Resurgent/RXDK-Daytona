#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/app/native_audio.h"
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

struct SDL_AudioStream { SDL_AudioStreamCallback callback; void *context; };
namespace mock {
std::mutex lock;
SDL_AudioStream *stream = nullptr;
bool paused = true, fail_open = false, fail_put = false;
unsigned pauses = 0, resumes = 0;
std::vector<float> output;
std::vector<float> render(unsigned bytes) {
    std::lock_guard guard(lock);
    output.clear();
    if (stream && !paused && stream->callback) stream->callback(stream->context, stream, int(bytes), int(bytes));
    return output;
}
}
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID, const SDL_AudioSpec *spec,
                                         SDL_AudioStreamCallback callback, void *context) {
    assert(!mock::stream && spec->freq == 48000 && spec->channels == 2 && spec->format == SDL_AUDIO_F32);
    if (mock::fail_open) return nullptr;
    mock::paused = true;
    return mock::stream = new SDL_AudioStream{callback, context};
}
bool SDL_SetAudioStreamGetCallback(SDL_AudioStream *stream, SDL_AudioStreamCallback callback, void *context) {
    std::lock_guard guard(mock::lock);
    assert(stream == mock::stream);
    stream->callback = callback; stream->context = context;
    return true;
}
void SDL_DestroyAudioStream(SDL_AudioStream *stream) {
    std::lock_guard guard(mock::lock);
    assert(stream == mock::stream && !stream->callback);
    delete stream; mock::stream = nullptr;
}
bool SDL_PauseAudioStreamDevice(SDL_AudioStream *stream) {
    std::lock_guard guard(mock::lock); assert(stream == mock::stream); ++mock::pauses; mock::paused = true; return true;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream) {
    std::lock_guard guard(mock::lock); assert(stream == mock::stream); ++mock::resumes; mock::paused = false; return true;
}
bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *data, int bytes) {
    assert(stream == mock::stream && bytes > 0 && bytes % 8 == 0 && bytes <= 256 * 8);
    if (mock::fail_put) return false;
    const auto *samples = static_cast<const float *>(data);
    mock::output.insert(mock::output.end(), samples, samples + bytes / 4);
    return true;
}

struct State { uint64_t bytes = 0, hash = 0, frames = 0; bool destroyed = false; };
struct Gate { std::mutex mutex; std::condition_variable condition; bool entered = false, release = false; };
struct Engine {
    State &state;
    float level = 0;
    bool fail = false;
    struct Stats { uint64_t unsupported = 0, invalid = 0; } health;
    Stats stats() const { return health; }
    Gate *gate = nullptr;
    explicit Engine(State &s) : state(s) {}
    ~Engine() { state.destroyed = true; }
    void send(const uint8_t *data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            ++state.bytes; state.hash = state.hash * 131 + data[i]; level = float(data[i]) / 255.f;
        }
    }
    void render(float *output, size_t frames) {
        if (gate) {
            std::unique_lock lock(gate->mutex);
            gate->entered = true; gate->condition.notify_all();
            gate->condition.wait(lock, [&] { return gate->release; });
        }
        if (fail) throw std::runtime_error("engine fault");
        state.frames += frames;
        std::fill_n(output, frames * 2, level);
    }
};

int main() {
    State state;
    app::NativeAudio<Engine> audio;
    assert(audio.open(std::make_unique<Engine>(state)));
    const uint8_t command = 128;
    assert(audio.send(&command, 1));
    assert(mock::render(4096).empty() && state.frames == 0);
    audio.volume(1); assert(audio.resume());
    const auto resume_calls = mock::resumes;
    for (int i = 0; i < 100; ++i) assert(audio.resume());
    assert(mock::resumes == resume_calls);
    assert(mock::render(4096)[0] > .5f && state.frames == 512 && state.bytes == 1);
    // Playback progresses for a second without a single graphics update.
    for (int i = 0; i < 100; ++i) assert(mock::render(480 * 8)[0] > .5f);
    assert(state.frames == 48512 && state.bytes == 1);
    audio.mute(true); assert(mock::render(4096)[0] == 0 && state.frames == 49024);
    assert(audio.pause()); assert(mock::render(4096).empty() && state.frames == 49024);
    const auto pause_calls = mock::pauses;
    for (int i = 0; i < 100; ++i) assert(audio.pause());
    assert(mock::pauses == pause_calls);
    audio.mute(false); audio.volume(.25f); assert(audio.resume());
    assert(mock::render(9).size() == 4); // odd SDL request rounds to complete stereo frames
    assert(mock::output[0] > .125f && mock::output[0] < .127f);
    audio.volume(NAN); assert(mock::render(8)[0] == 0);
    audio.close(); assert(state.destroyed && !mock::stream);

    State stress;
    assert(audio.open(std::make_unique<Engine>(stress)));
    std::vector<uint8_t> packet(app::NativeAudio<Engine>::kQueueSize, 7);
    assert(audio.send(packet.data(), packet.size()));
    assert(!audio.send(&command, 1) && audio.stats().overflows == 1);
    assert(audio.resume()); mock::render(8);
    assert(stress.bytes == packet.size());
    uint64_t expected = stress.hash;
    std::atomic<bool> stop{false};
    std::thread callback([&] { while (!stop.load()) mock::render(32 * 8); });
    for (unsigned n = 0; n < 10000; ++n) {
        std::array<uint8_t, 7> bytes{};
        for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(n + i);
        while (!audio.send(bytes.data(), bytes.size())) std::this_thread::yield();
        for (uint8_t byte : bytes) expected = expected * 131 + byte;
        audio.volume(float(n % 101) / 100.f); audio.mute(n % 31 == 0);
        assert(audio.stats().queued <= app::NativeAudio<Engine>::kQueueSize);
    }
    stop = true; callback.join(); mock::render(8); assert(audio.pause());
    assert(stress.hash == expected && stress.bytes == packet.size() + 70000);
    assert(audio.stats().queued == 0 && audio.stats().failed == 0);
    audio.close(); assert(stress.destroyed);

    State blocked;
    Gate gate;
    auto engine = std::make_unique<Engine>(blocked); engine->gate = &gate;
    assert(audio.open(std::move(engine)) && audio.resume());
    std::thread rendering([&] { mock::render(8); });
    { std::unique_lock lock(gate.mutex); gate.condition.wait(lock, [&] { return gate.entered; }); }
    std::atomic<bool> closing{false}, closed{false};
    std::thread close([&] { closing = true; audio.close(); closed = true; });
    while (!closing.load()) std::this_thread::yield();
    assert(!closed.load() && !blocked.destroyed);
    { std::lock_guard lock(gate.mutex); gate.release = true; gate.condition.notify_all(); }
    rendering.join(); close.join(); assert(closed && blocked.destroyed);

    State error;
    auto failing = std::make_unique<Engine>(error); failing->fail = true;
    assert(audio.open(std::move(failing)) && audio.resume());
    mock::render(4096); assert(audio.stats().failed == 1 && !audio.send(&command, 1));
    const auto silence = mock::render(4096);
    assert(silence.size() == 1024 && std::all_of(silence.begin(), silence.end(), [](float x) { return x == 0; }));
    audio.close(); assert(error.destroyed);
    State invalid;
    auto invalid_engine = std::make_unique<Engine>(invalid);
    invalid_engine->health = {3, 7};
    assert(audio.open(std::move(invalid_engine)) && audio.resume());
    mock::render(8);
    assert(audio.stats().unsupported == 3 && audio.stats().invalid == 7);
    audio.close(); assert(invalid.destroyed);
    State io_error;
    assert(audio.open(std::make_unique<Engine>(io_error)) && audio.resume());
    mock::fail_put = true; mock::render(4096); assert(audio.stats().failed == 1);
    audio.close(); mock::fail_put = false;
    State unavailable;
    mock::fail_open = true;
    assert(!audio.open(std::make_unique<Engine>(unavailable)) && unavailable.destroyed);
    audio.close();
    std::puts("PASS: SDL3 native callback clock, packet order/overflow, pause/reset, in-flight close, mute and faults");
}
