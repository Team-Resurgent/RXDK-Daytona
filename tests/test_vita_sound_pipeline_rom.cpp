// Opt-in real-ROM test. Link with runtime and the same generated i960/TGP/68000
// objects as m2run, plus tests/vita_audio_shim for SDL-shaped host threads.
// Usage: test_vita_sound_pipeline_rom ROM_CACHE_DIR [FRAMES=6000]
#include "runtime/game_loop.h"
#include "../platform/vita/sound_worker.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "vita_audio_thread_mock.inc"

namespace {
uint64_t ticks() {
    return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
}
rt::Inputs input_for(int frame) {
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
struct GatedPacket {
    rt::GameLoop::SoundPacket packet;
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false, started = false;
    uint64_t execute() {
        {
            std::unique_lock<std::mutex> lock(mutex);
            started = true; changed.notify_all();
            changed.wait(lock, [&] { return released; });
        }
        return packet.execute();
    }
    snd::SoundBoard *sound() { return packet.sound(); }
    void wait_started() {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return started; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true; changed.notify_all();
    }
};
struct ReleaseOnExit {
    GatedPacket &active;
    ~ReleaseOnExit() { active.release(); }
};
struct Output {
    std::vector<float> fm, pcm;
    uint64_t instructions = 0, bytes = 0, frames = 0;
    void push(snd::SoundBoard &board) {
        fm = board.take_fm(); pcm = board.take_pcm();
        instructions = board.instructions(); bytes = board.bytes_received();
        ++frames;
    }
};
bool identical(const std::vector<float> &a, const std::vector<float> &b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}
bool same_profile(const rt::FrameProfile &a, const rt::FrameProfile &b) {
    return a.total == b.total && a.geometry == b.geometry && a.video == b.video && a.sound == b.sound;
}
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s ROM_CACHE_DIR [FRAMES=6000]\n", argv[0]); return 2; }
    try {
        const int count = argc > 2 ? std::atoi(argv[2]) : 6000;
        rt::GameLoop serial(argv[1]), pipelined(argv[1]);
        pipelined.set_profile_clock(ticks);
        GatedPacket active;
        Output output, reference, previous;
        vita::SoundWorker worker;
        ReleaseOnExit release_on_exit{active};
        mock::require(worker.open(), "worker opened");
        uint64_t fm_samples = 0, pcm_samples = 0, hashes = 0;
        auto compare_sound = [&] {
            mock::require(identical(output.fm, previous.fm), "FM samples bit-identical");
            mock::require(identical(output.pcm, previous.pcm), "PCM samples bit-identical");
            mock::require(output.instructions == previous.instructions, "68000 instructions identical");
            mock::require(output.bytes == previous.bytes, "received UART bytes identical");
            mock::require(output.frames == previous.frames, "one sound frame per board frame");
            fm_samples += output.fm.size(); pcm_samples += output.pcm.size();
        };
        for (int frame = 0; frame < count; ++frame) {
            const auto inputs = input_for(frame);
            // This intentionally runs BEFORE finishing sound frame N-1.
            auto next = pipelined.run_frame_sound_packet(inputs);
            const rt::FrameProfile profile = pipelined.last_profile();
            mock::require(profile.sound == 0, "detached profile is board-only");
            serial.run_frame(inputs);
            reference.push(*serial.sound());
            mock::require(serial.instructions() == pipelined.instructions(), "i960 instruction parity");
            mock::require(serial.board().tgp().tgp_instructions() == pipelined.board().tgp().tgp_instructions(), "TGP instruction parity");
            mock::require(serial.board().sound_bytes_total() == pipelined.board().sound_bytes_total(), "sent UART byte parity");
            mock::require(serial.board().video().screen_hash() == pipelined.board().video().screen_hash(), "CPU-rendered screen hash parity");
            ++hashes;
            if (frame == 1) active.release(); // frame N completed while N-1 was blocked
            worker.finish();
            if (frame) compare_sound();
            mock::require(same_profile(profile, pipelined.last_profile()), "previous completion cannot change current profile");
            previous = std::move(reference);
            // Preserve the serial reference count when its sample vectors move.
            reference.frames = previous.frames;
            active.packet = std::move(next);
            mock::require(next.sound() == nullptr && next.execute() == 0, "moved-from packet inert");
            worker.dispatch_packet(active, output, ticks);
            if (!frame) active.wait_started();
            if ((frame + 1) % 1000 == 0) {
                std::printf("progress frames=%d screen_hashes=%llu\n", frame + 1, (unsigned long long)hashes);
                std::fflush(stdout);
            }
        }
        // Shutdown joins the last sound + queue job before either game dies.
        active.release();
        worker.close();
        if (count) {
            compare_sound();
            const uint64_t before = pipelined.sound()->instructions();
            bool caught = false;
            try { active.packet.execute(); } catch (const rt::Fatal &) { caught = true; }
            mock::require(caught && before == pipelined.sound()->instructions(), "packet cannot advance sound twice");
        }
        mock::reset();
        std::printf("PASS: %d pipelined real-ROM frames, %llu CPU screen hashes, %llu FM + %llu PCM float samples bit-identical; i960/TGP/68000 and UART parity, immutable board profiles, single-use moves and joined shutdown\n",
            count, (unsigned long long)hashes, (unsigned long long)fm_samples, (unsigned long long)pcm_samples);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
