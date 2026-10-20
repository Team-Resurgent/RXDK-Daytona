#include "../platform/vita/performance.h"
#include "../platform/vita/controls.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
static uint64_t ticks;
static uint64_t clock_ticks() { ticks += 10; return ticks; }
int main() {
    rt::FrameProfiler timer;
    { auto t = timer.measure(timer.frame.total); }
    CHECK(timer.frame.total == 0);
    timer.set_clock(clock_ticks);
    {
        auto total = timer.measure(timer.frame.total);
        { auto t = timer.measure(timer.frame.geometry); }
        { auto t = timer.measure(timer.frame.video); }
        { auto t = timer.measure(timer.frame.sound); }
    }
    CHECK(timer.frame.total == 70 && timer.frame.geometry == 10);
    CHECK(timer.frame.video == 10 && timer.frame.sound == 10 && timer.frame.core() == 40);
    timer.reset(); CHECK(timer.frame.total == 0 && timer.frame.core() == 0);
    try {
        auto t = timer.measure(timer.frame.sound);
        throw std::runtime_error("test exception unwinding");
    } catch (const std::runtime_error &) {}
    CHECK(timer.frame.sound == 10);
    CHECK((rt::FrameProfile{10, 20, 30, 40}.core() == 0));
    timer.set_clock(nullptr);
    { auto t = timer.measure(timer.frame.total); }
    CHECK(timer.frame.total == 0);

    vita::Performance perf(1000);
    CHECK(!perf.ready(90000));
    perf.begin(100, false);
    perf.frame({100, 20, 30, 40}); perf.frame({100, 20, 30, 40});
    perf.video({2, 3, 20, 5, 10, 1, true});
    perf.video({4, 0, 22, 4, 0, 0, false});
    for (int i = 0; i < 4; ++i) perf.presented();
    perf.span(vita::Performance::Present, 40, 50);
    CHECK(!perf.ready(2099) && perf.ready(2100));
    CHECK(perf.average_ms(vita::Performance::Core) == 10);
    CHECK(perf.average_ms(vita::Performance::Present) == 10);
    char line[768];
    CHECK(perf.format(line, sizeof line, 2100, 333, 111, 222) > 0);
    CHECK(std::strstr(line, "mode=GAME sim_fps=1.00 present_fps=2.00"));
    CHECK(std::strstr(line, "video_ms=30.00 sound_ms=40.00"));
    CHECK(std::strstr(line, "cpu_mhz=333 gpu_mhz=111 bus_mhz=222"));
    CHECK(std::strstr(line, "tile_cache_ms=3.00 tile_draw_ms=1.50 raster_ms=21.00 compose_ms=4.50"));
    CHECK(std::strstr(line, "tiles_rebuilt_avg=5.0 chars_changed_avg=0.5 layers_redrawn_pct=50.0"));
    char overlay[160];
    CHECK(perf.format_overlay(overlay, sizeof overlay, 2100) > 0);
    CHECK(std::strstr(overlay, "GAME SIM 1.00 FPS CORE 10.0 VIDEO 30.0 SND 40.0 MS"));
    char small[5] = {};
    CHECK(perf.format(small, sizeof small, 2100, 0, 0, 0) >= int(sizeof small));
    CHECK(small[4] == '\0');
    perf.begin(2200, true); CHECK(!perf.ready(2201));
    CHECK(perf.average_ms(vita::Performance::Video) == 0);
    CHECK(perf.average_ms(vita::Performance::Raster) == 0);
    CHECK(perf.format(line, sizeof line, 2200, 0, 0, 0) > 0);
    CHECK(std::strstr(line, "mode=MENU sim_fps=0.00 present_fps=0.00"));
    perf.span(vita::Performance::Upload, 20, 10);
    CHECK(perf.average_ms(vita::Performance::Upload) == 0);

    constexpr double hz = 16000000.0 / (656.0 * 424.0);
    vita::FrameClock single(hz, 1);
    int frames = 0;
    for (int i = 0; i < 6000; ++i) {
        const int steps = single.advance(1.0 / 60.0);
        CHECK(steps == 0 || steps == 1); frames += steps;
    }
    CHECK(frames == 5752); // fractional time must survive one-frame limiting
    for (int i = 0; i < 2000; ++i) CHECK(single.advance(1.0) == 1);
    single.reset(); CHECK(single.advance(0) == 0);
    CHECK(single.advance(-1) == 0);
    // A rendering stall never causes four future rasters before presentation.
    CHECK(single.advance(20) == 1);
    CHECK(single.advance(0) == 0);
    // The old four-step policy remains available for other callers.
    vita::FrameClock batch(hz);
    CHECK(batch.advance(20) > 1 && batch.advance(0) == 0);
    std::puts("Vita profiling and one-frame pacing tests passed");
}
