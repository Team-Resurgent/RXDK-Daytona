// Bounded, host-testable profiling summaries. No per-instruction logging.
#pragma once
#include "runtime/frame_profile.h"
#include "runtime/video_profile.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace vita {
class Performance {
public:
    enum Stage { Total, Core, Geometry, Video, Sound, Audio, Upload, Draw, Present, Save, TileCache, TileDraw, Raster, Composite, Count };
    explicit Performance(double frequency) : frequency_(frequency > 0 ? frequency : 1.0) {}
    void reset(uint64_t now, bool menu) {
        averages_ = {}; frames_ = presents_ = 0;
        tile_rebuilds_ = character_changes_ = layer_redraws_ = video_frames_ = 0;
        started_ = now; menu_ = menu; initialized_ = true;
    }
    void begin(uint64_t now, bool menu) {
        if (!initialized_ || menu != menu_) reset(now, menu);
    }
    void add(Stage stage, uint64_t ticks) {
        auto &a = averages_[stage];
        a.total += ticks; ++a.count;
    }
    void span(Stage stage, uint64_t start, uint64_t end) {
        add(stage, end >= start ? end - start : 0);
    }
    void frame(const rt::FrameProfile &p) {
        ++frames_;
        add(Total, p.total); add(Core, p.core()); add(Geometry, p.geometry);
        add(Video, p.video); add(Sound, p.sound);
    }
    void video(const rt::VideoProfile &p) {
        add(TileCache, p.tile_cache); add(TileDraw, p.tile_draw);
        add(Raster, p.raster); add(Composite, p.composite);
        tile_rebuilds_ += p.tiles_rebuilt; character_changes_ += p.characters_changed;
        layer_redraws_ += p.layers_rebuilt ? 1u : 0u; ++video_frames_;
    }
    void presented() { ++presents_; }
    bool ready(uint64_t now) const {
        return initialized_ && now >= started_ && double(now - started_) / frequency_ >= 2.0;
    }
    double average_ms(Stage stage) const {
        const auto &a = averages_[stage];
        return a.count ? double(a.total) * 1000.0 / frequency_ / double(a.count) : 0.0;
    }
    int format(char *out, size_t size, uint64_t now, int cpu, int gpu, int bus) const {
        const double seconds = now >= started_ ? double(now - started_) / frequency_ : 0.0;
        return std::snprintf(out, size,
            "perf: mode=%s sim_fps=%.2f present_fps=%.2f "
            "total_ms=%.2f core_ms=%.2f geo_ms=%.2f video_ms=%.2f sound_ms=%.2f "
            "audio_ms=%.2f upload_ms=%.2f draw_ms=%.2f present_ms=%.2f save_ms=%.2f "
            "cpu_mhz=%d gpu_mhz=%d bus_mhz=%d "
            "tile_cache_ms=%.2f tile_draw_ms=%.2f raster_ms=%.2f compose_ms=%.2f "
            "tiles_rebuilt_avg=%.1f chars_changed_avg=%.1f layers_redrawn_pct=%.1f\n",
            menu_ ? "MENU" : "GAME", seconds > 0 ? double(frames_) / seconds : 0.0,
            seconds > 0 ? double(presents_) / seconds : 0.0,
            average_ms(Total), average_ms(Core), average_ms(Geometry), average_ms(Video),
            average_ms(Sound), average_ms(Audio), average_ms(Upload), average_ms(Draw),
            average_ms(Present), average_ms(Save), cpu, gpu, bus,
            average_ms(TileCache), average_ms(TileDraw), average_ms(Raster), average_ms(Composite),
            video_frames_ ? double(tile_rebuilds_) / double(video_frames_) : 0.0,
            video_frames_ ? double(character_changes_) / double(video_frames_) : 0.0,
            video_frames_ ? double(layer_redraws_) * 100.0 / double(video_frames_) : 0.0);
    }
    int format_overlay(char *out, size_t size, uint64_t now) const {
        const double seconds = now >= started_ ? double(now - started_) / frequency_ : 0.0;
        return std::snprintf(out, size, "%s SIM %.2f FPS CORE %.1f VIDEO %.1f SND %.1f MS",
            menu_ ? "MENU" : "GAME", seconds > 0 ? double(frames_) / seconds : 0.0,
            average_ms(Core), average_ms(Video), average_ms(Sound));
    }
private:
    struct Average { uint64_t total = 0, count = 0; };
    std::array<Average, Count> averages_{};
    uint64_t started_ = 0, frames_ = 0, presents_ = 0;
    uint64_t tile_rebuilds_ = 0, character_changes_ = 0, layer_redraws_ = 0, video_frames_ = 0;
    double frequency_;
    bool menu_ = true, initialized_ = false;
};
} // namespace vita
