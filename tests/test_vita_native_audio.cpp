#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../platform/vita/native_audio.h"
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace mock {
std::mutex lock;
SDL_AudioSpec spec;
bool alive = false, paused = true, fail_open = false;
std::vector<int16_t> render(unsigned frames) {
    std::vector<int16_t> out(frames * 2);
    std::lock_guard guard(lock);
    if (alive && !paused) spec.callback(spec.userdata, reinterpret_cast<Uint8 *>(out.data()), int(out.size() * 2));
    return out;
}
}
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *, int, const SDL_AudioSpec *want, SDL_AudioSpec *got, int) {
    assert(!mock::alive && want->freq == 48000 && want->channels == 2 && want->format == AUDIO_S16SYS);
    if (mock::fail_open) return 0;
    mock::spec = *want; *got = *want;
    mock::alive = true; mock::paused = true;
    return 1;
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID id) {
    assert(id == 1);
    std::lock_guard guard(mock::lock); mock::alive = false;
}
void SDL_PauseAudioDevice(SDL_AudioDeviceID id, int paused) {
    assert(id == 1);
    std::lock_guard guard(mock::lock); mock::paused = paused != 0;
}

struct State { uint64_t bytes = 0, hash = 0, rendered = 0; bool destroyed = false; };
struct Engine {
    State &state;
    float level = 0;
    bool fail = false;
    explicit Engine(State &s) : state(s) {}
    ~Engine() { state.destroyed = true; }
    void send(const uint8_t *data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            ++state.bytes; state.hash = state.hash * 131 + data[i];
            level = float(data[i]) / 255.f;
        }
    }
    void render(float *output, size_t frames) {
        if (fail) throw std::runtime_error("native engine fault");
        state.rendered += frames;
        std::fill_n(output, frames * 2, level);
    }
};

int main() {
    State state;
    vita::NativeAudio<Engine> audio;
    assert(audio.open(std::make_unique<Engine>(state)));
    uint8_t command = 128;
    assert(audio.send(&command, 1));
    assert(mock::render(512)[0] == 0 && state.rendered == 0);
    audio.volume(1); audio.resume();
    assert(mock::render(512)[0] > 16000);
    assert(state.bytes == 1 && state.rendered == 512);
    // No graphics/game updates or new packets for a full second: the native
    // engine keeps advancing on the audio callback's clock alone.
    for (int i = 0; i < 100; ++i) assert(mock::render(480)[0] > 16000);
    assert(state.rendered == 48512 && state.bytes == 1);
    audio.mute(true);
    assert(mock::render(512)[0] == 0 && state.rendered == 49024);
    audio.pause();
    mock::render(512); assert(state.rendered == 49024);
    audio.mute(false); audio.volume(0.25f); audio.resume();
    const auto quiet = mock::render(512)[0]; assert(quiet > 4000 && quiet < 4200);
    audio.close(); assert(state.destroyed && !mock::alive);

    State stress;
    assert(audio.open(std::make_unique<Engine>(stress)));
    std::vector<uint8_t> packet(vita::NativeAudio<Engine>::kQueueSize, 7);
    assert(audio.send(packet.data(), packet.size()));
    assert(!audio.send(&command, 1) && audio.stats().overflows == 1);
    audio.resume(); mock::render(1);
    assert(stress.bytes == packet.size());
    uint64_t expected = stress.hash;
    std::atomic<bool> stop{false};
    std::thread callback([&] { while (!stop.load()) mock::render(32); });
    for (unsigned n = 0; n < 10000; ++n) {
        std::array<uint8_t, 7> bytes{};
        for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(n + i);
        while (!audio.send(bytes.data(), bytes.size())) std::this_thread::yield();
        for (uint8_t b : bytes) expected = expected * 131 + b;
        audio.volume(float(n % 101) / 100.f);
        audio.mute(n % 31 == 0);
        (void)audio.stats();
    }
    stop = true; callback.join();
    mock::render(1); audio.pause();
    assert(stress.hash == expected && stress.bytes == packet.size() + 70000);
    assert(audio.stats().queued == 0 && audio.stats().failed == 0);
    audio.close(); assert(stress.destroyed);

    State error;
    auto failing = std::make_unique<Engine>(error); failing->fail = true;
    assert(audio.open(std::move(failing)));
    audio.resume(); assert(mock::render(512)[0] == 0 && audio.stats().failed == 1);
    assert(!audio.send(&command, 1));
    audio.close(); assert(error.destroyed);
    State unavailable;
    mock::fail_open = true;
    assert(!audio.open(std::make_unique<Engine>(unavailable)) && unavailable.destroyed);
    audio.close();
    std::puts("PASS: native callback clock, pause/resume, atomic packet queue/order, concurrent controls, faults and lifecycle");
}
