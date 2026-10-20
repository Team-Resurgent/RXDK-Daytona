// Frame pacing (src/app/pacing.h) with simulated time: which mode a display
// gets for each setting, that the default (Clock) runs exactly as the main
// loop always did, and how many game frames each mode runs over many passes.
#include "app/pacing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
using Mode = app::Pacing::Mode;
constexpr double kArcade = app::kArcadeFrameHz;

// Game frames run over `seconds` of passes `period_ns` apart (+- jitter).
struct Run { uint64_t frames = 0; int most = 0; std::vector<int> per_pass; };
Run simulate(const app::Pacing &p, double period_ns, double seconds, double jitter_ns = 0, bool vrr_sleep = false) {
    app::Pacer pacer;
    uint64_t now = 1'000'000'000;
    pacer.set(p, now);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> j(-jitter_ns, jitter_ns);
    Run r;
    for (double t = 0; t < seconds * 1e9; t += period_ns) {
        if (vrr_sleep) now += pacer.wait_ns(now);
        const int n = pacer.frames(now);
        r.frames += uint64_t(n), r.most = std::max(r.most, n), r.per_pass.push_back(n);
        now += uint64_t(std::max(0.0, period_ns + j(rng)));
    }
    return r;
}
} // namespace

int main() {
    const app::PacingSettings off, smooth{true, false, false}, sync{false, true, false}, vrr{false, false, true};
    const app::PacingSettings both{true, true, false};

    // Everything off: the arcade's speed on the clock, on any display.
    for (double hz : {0.0, 50.0, 57.524, 60.0, 120.0, 144.0, 165.0, 240.0}) {
        const auto p = app::choose_pacing(hz, off);
        check(p.mode == Mode::Clock && p.game_hz == kArcade, "off: clock at the arcade's rate");
    }
    // Sync to display: a whole division of the refresh between 56 and 61 Hz.
    auto is = [](const app::Pacing &p, Mode m, int every, double hz) {
        return p.mode == m && p.every == every && std::fabs(p.game_hz - hz) < 1e-9;
    };
    check(is(app::choose_pacing(60, sync), Mode::Display, 1, 60), "sync 60 Hz: 60, every refresh");
    check(is(app::choose_pacing(59.94, sync), Mode::Display, 1, 59.94), "sync 59.94 Hz");
    check(is(app::choose_pacing(120, sync), Mode::Display, 2, 60), "sync 120 Hz: every 2nd");
    check(is(app::choose_pacing(180, sync), Mode::Display, 3, 60), "sync 180 Hz: every 3rd");
    check(is(app::choose_pacing(240, sync), Mode::Display, 4, 60), "sync 240 Hz: every 4th");
    check(is(app::choose_pacing(115.048, sync), Mode::Display, 2, 57.524), "sync 115 Hz: every 2nd");
    check(app::choose_pacing(144, sync).mode == Mode::Clock, "sync 144 Hz: no effect");
    check(app::choose_pacing(165, sync).mode == Mode::Clock, "sync 165 Hz: no effect (55 is too slow)");
    check(app::choose_pacing(50, sync).mode == Mode::Clock, "sync 50 Hz: no effect");
    check(app::choose_pacing(0, sync).mode == Mode::Clock, "sync, refresh unknown: no effect");
    // Smooth pacing: only a multiple of the arcade's rate, within 1%.
    check(is(app::choose_pacing(57.524, smooth), Mode::Display, 1, 57.524), "smooth 57.52 Hz");
    check(is(app::choose_pacing(115.05, smooth), Mode::Display, 2, 57.525), "smooth 115 Hz: every 2nd");
    check(app::choose_pacing(57.0, smooth).mode == Mode::Display, "smooth 57.0 Hz: within 1%");
    check(app::choose_pacing(60, smooth).mode == Mode::Clock, "smooth 60 Hz: no effect");
    check(app::choose_pacing(144, smooth).mode == Mode::Clock, "smooth 144 Hz: no effect");
    check(is(app::choose_pacing(57.524, both), Mode::Display, 1, 57.524), "both, 57.52 Hz: smooth's");
    check(is(app::choose_pacing(60, both), Mode::Display, 1, 60), "both, 60 Hz: sync's");
    // VRR: always the arcade's rate.
    for (double hz : {0.0, 60.0, 144.0, 240.0}) check(app::choose_pacing(hz, vrr).mode == Mode::Vrr, "vrr");

    // Clock: the main loop's arithmetic as it was (owed time, at most 4 frames).
    {
        app::Pacer pacer;
        const app::Pacing clock = app::choose_pacing(144, off);
        uint64_t now = 5'000'000'000, last = now;
        pacer.set(clock, now);
        const double frame_ns = 1e9 / kArcade;
        double pending = 0;
        std::mt19937 rng(3);
        std::uniform_int_distribution<int> step(1'000'000, 60'000'000);
        bool same = true;
        for (int i = 0; i < 20000; ++i) {
            now += uint64_t(step(rng));
            pending = std::min(pending + double(now - last), frame_ns * 4);
            last = now;
            int old = 0;
            while (pending >= frame_ns) ++old, pending -= frame_ns;
            same &= pacer.frames(now) == old;
        }
        check(same, "clock: as the loop always ran, 20,000 random passes");
    }
    // Clock at 144 Hz: 57.52 frames/s, shown 2 or 3 refreshes each.
    {
        const auto r = simulate(app::choose_pacing(144, off), 1e9 / 144, 60);
        check(std::llabs(int64_t(r.frames) - int64_t(std::llround(60 * kArcade))) <= 2, "clock 144 Hz: arcade speed");
    }
    // Display at 60 Hz with 2 ms of jitter: exactly one frame every pass.
    {
        const auto r = simulate(app::choose_pacing(60, sync), 1e9 / 60, 60, 2e6);
        bool every = std::all_of(r.per_pass.begin(), r.per_pass.end(), [](int n) { return n == 1; });
        check(every && r.frames == r.per_pass.size(), "display 60 Hz: one frame every refresh, with jitter");
    }
    // Display at 120 Hz: one frame every 2nd pass, alternating.
    {
        const auto r = simulate(app::choose_pacing(120, sync), 1e9 / 120, 10, 1e6);
        bool alternate = true;
        for (size_t i = 0; i < r.per_pass.size(); ++i) alternate &= r.per_pass[i] == int(i % 2 == 1);
        check(alternate, "display 120 Hz: every 2nd refresh");
    }
    // Display with vsync forced off (passes 1 ms apart): never faster than its rate.
    {
        const auto r = simulate(app::choose_pacing(60, sync), 1e6, 10);
        check(r.frames <= 60 * 10 + 2 && r.frames >= 60 * 10 - 2, "display, vsync off: held to 60 frames/s");
    }
    // VRR: one frame per pass, each pass held to 1/57.52 s however fast the loop is.
    {
        const auto r = simulate(app::choose_pacing(144, vrr), 2e6, 10, 0, true);
        check(r.most == 1, "vrr: one frame per pass");
        app::Pacer pacer;
        uint64_t now = 0;
        pacer.set(app::choose_pacing(144, vrr), now);
        const uint64_t step = uint64_t(std::llround(1e9 / kArcade));
        std::vector<uint64_t> starts;
        for (int i = 0; i < 100; ++i) {
            now += pacer.wait_ns(now);
            starts.push_back(now);
            pacer.frames(now);
            now += 3'000'000; // the frame's own work
        }
        bool even = true;
        for (size_t i = 1; i < starts.size(); ++i) even &= starts[i] - starts[i - 1] == step;
        check(even, "vrr: frames exactly 1/57.52 s apart");
        now += 500'000'000; // a stall: starts again, no burst of frames
        const uint64_t before = now;
        now += pacer.wait_ns(now);
        check(now == before, "vrr: after a stall, no wait and no catching up");
    }
    if (failures) return 1;
    std::printf("test_app_pacing: ok\n");
    return 0;
}
