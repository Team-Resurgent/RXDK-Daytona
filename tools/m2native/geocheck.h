// Geometry and screen check for m2native, against MAME's M2TRACE_GEOLOG:
//  - at each vblank start ("vb", same completed i960 instruction count) the
//    native geometrizer parses our buffer RAM; every word it hands the
//    rasterizer and every polygon it keeps must equal MAME's (bit for bit);
//  - at each screen update ("su", vblank end) the native video output
//    composes the 2D tilemap layers and the 3D layer; the 3D layer ("fb") and
//    the whole screen ("scr") must hash equal to MAME's.
#pragma once

#include "runtime/geo.h"
#include "runtime/lockstep.h"
#include "runtime/m2_replay_bus.h"
#include "runtime/m2_tgp_board.h"
#include "runtime/video.h"

#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../src/compat/bit_cast.h"
class GeoCheck {
public:
    GeoCheck(const std::string &path, rt::Geo &geo, rt::TgpBoard &board, rt::M2ReplayBus &bus, rt::Lockstep &ls)
        : f_(path), geo_(geo), board_(board), bus_(bus), video_(bus.tile_ram(), bus.char_ram()) {
        if (!f_) throw std::runtime_error("cannot open " + path);
        std::string line;
        while (std::getline(f_, line)) {
            if (line.compare(0, 3, "vb ") == 0) {
                Frame fr{};
                unsigned long long count = 0;
                int parse = 0;
                std::sscanf(line.c_str(), "vb %u %llu %d", &fr.frame, &count, &parse);
                fr.count = count;
                fr.parse = parse != 0;
                fr.off = f_.tellg();
                frames_.push_back(fr);
            } else if (line.compare(0, 3, "su ") == 0) {
                unsigned frame = 0;
                unsigned long long count = 0;
                std::sscanf(line.c_str(), "su %u %llu", &frame, &count);
                updates_.push_back({frame, uint64_t(count)});
            }
        }
        for (size_t k = 0; k < frames_.size(); k++) ls.add_callback(frames_[k].count, [this, k] { at_vblank(k); });
        for (size_t k = 0; k < updates_.size(); k++) ls.add_callback(updates_[k].count, [this, k] { at_update(k); });

        // Video registers: palette and colour translation (pens), CRTC offsets.
        bus.hook(0x01800000, 0x01803fff);
        bus.hook(0x01810000, 0x0181bfff);
        for (uint32_t a : {0x01040000u, 0x01140000u, 0x01060000u, 0x01160000u}) bus.hook(a, a);
        bus.on_write = [this](uint32_t addr, uint32_t data, uint32_t mask) { video_write(addr, data, mask); };
    }

    uint64_t frames_checked = 0, words = 0, polys = 0;
    uint64_t fb_checked = 0, fb_mismatch = 0, scr_checked = 0, scr_mismatch = 0;
    std::string first_fb_mismatch, first_scr_mismatch;

private:
    struct Frame {
        unsigned frame = 0;
        uint64_t count = 0;
        bool parse = false;
        std::streamoff off = 0;
    };
    struct Update {
        unsigned frame = 0;
        uint64_t count = 0;
    };

    [[noreturn]] void diverge(const Frame &fr, const std::string &what) {
        char b[96];
        std::snprintf(b, sizeof b, "geometrizer, frame %u (i960 instruction %" PRIu64 "): ", fr.frame, fr.count);
        throw rt::Divergence(b + what);
    }

    void video_write(uint32_t addr, uint32_t data, uint32_t mask) {
        const rt::VideoMem m = bus_.video_mem();
        for (uint32_t lane = 0; lane < 2; lane++) {
            if (!((mask >> (16 * lane)) & 0xffff)) continue;
            const uint16_t v = uint16_t(data >> (16 * lane));
            if (addr >= 0x01800000 && addr <= 0x01803fff) video_.palette_w(((addr & 0x3fff) >> 1) + lane, m.palram, m.colorxlat);
            else if (addr >= 0x01810000 && addr <= 0x0181bfff) video_.colorxlat_w(((addr - 0x01810000) >> 1) + lane);
            else if (lane == 0 && (addr & ~0x100000u) == 0x01040000) video_.xhout_w(v);
            else if (lane == 0 && (addr & ~0x100000u) == 0x01060000) video_.xvout_w(v);
        }
    }

    void at_vblank(size_t k) {
        const Frame &fr = frames_[k];
        f_.clear();
        f_.seekg(fr.off);
        if (fr.parse) {
            geo_.zclip_w(bus_.peek(0x0181c000));
            geo_.record_pushes = true;
            geo_.parse(board_.geo_read_start());
            video_.frame_start();
        }
        std::string line;
        size_t pi = 0, qi = 0;
        expect_fb_.clear();
        expect_scr_.clear();
        while (std::getline(f_, line) && line.compare(0, 3, "vb ") != 0) {
            if (line.compare(0, 2, "p ") == 0) {
                const uint32_t want = uint32_t(std::stoul(line.substr(2), nullptr, 16));
                if (pi >= geo_.pushed.size()) diverge(fr, "MAME pushed more words to the rasterizer (" + line + ")");
                if (geo_.pushed[pi] != want) {
                    char b[96];
                    std::snprintf(b, sizeof b, "rasterizer word %zu: ours %08x, MAME %08x", pi, geo_.pushed[pi], want);
                    diverge(fr, b);
                }
                ++pi;
            } else if (line.compare(0, 5, "poly ") == 0) {
                if (qi >= geo_.polys.size()) diverge(fr, "MAME kept more polygons");
                check_poly(fr, qi, geo_.polys[qi], line);
                ++qi;
            } else if (line.compare(0, 3, "fb ") == 0) {
                expect_fb_ = line;
            } else if (line.compare(0, 4, "scr ") == 0) {
                expect_scr_ = line;
            }
        }
        if (!fr.parse) return;
        if (pi != geo_.pushed.size()) diverge(fr, "we pushed more words to the rasterizer than MAME");
        if (qi != geo_.polys.size()) diverge(fr, "we kept more polygons than MAME");
        ++frames_checked;
        words += pi;
        polys += qi;
    }

    void at_update(size_t k) {
        const unsigned frame = updates_[k].frame;
        video_.screen_update(geo_.polys, geo_.windows(), bus_.video_mem());

        // 3D layer: MAME logs a hash only when it drew afresh
        unsigned f = 0;
        char what[32] = {};
        if (!expect_fb_.empty() && std::sscanf(expect_fb_.c_str(), "fb %u %31s", &f, what) == 2 && f == frame &&
            std::string(what) != "same" && std::string(what) != "empty") {
            ++fb_checked;
            if (!video_.rendered_now() || video_.raster_hash() != std::strtoull(what, nullptr, 16))
                if (!fb_mismatch++) first_fb_mismatch = "frame " + std::to_string(frame);
        }
        // the whole screen
        if (!expect_scr_.empty() && std::sscanf(expect_scr_.c_str(), "scr %u %31s", &f, what) == 2 && f == frame) {
            ++scr_checked;
            if (video_.screen_hash() != std::strtoull(what, nullptr, 16))
                if (!scr_mismatch++) first_scr_mismatch = "frame " + std::to_string(frame);
        }
        static const char *dir = std::getenv("M2NATIVE_FBDUMP_DIR");
        static const int every = std::getenv("M2NATIVE_FBDUMP_EVERY") ? std::atoi(std::getenv("M2NATIVE_FBDUMP_EVERY")) : 0;
        if (dir && every > 0 && frame % unsigned(every) == 0) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/ours_scr_%05u.rgb", dir, frame);
            if (FILE *d = std::fopen(path, "wb")) {
                std::fwrite(video_.screen().data(), 4, video_.screen().size(), d);
                std::fclose(d);
            }
        }
    }

    void check_poly(const Frame &fr, size_t qi, const rt::GeoPoly &p, const std::string &line) {
        std::istringstream s(line.substr(5));
        auto hx = [&] { std::string t; s >> t; return uint32_t(std::stoul(t, nullptr, 16)); };
        auto dc = [&] { long v; s >> v; return v; };
        bool ok = hx() == p.z && hx() == p.window;
        for (int i = 0; i < 4; i++) ok = ok && hx() == p.texheader[i];
        ok = ok && hx() == p.luma && hx() == uint32_t(p.texlod) && hx() == p.num_vertices;
        for (int i = 0; i < 4; i++) ok = ok && dc() == p.viewport[i];
        for (int i = 0; i < 2; i++) ok = ok && dc() == p.center[i];
        ok = ok && dc() == long(p.reverse);
        for (int i = 0; ok && i < p.num_vertices; i++) {
            const rt::GeoVertex &v = p.v[i];
            const float fl[5] = {v.x, v.y, v.p[0], v.p[1], v.p[2]};
            for (float x : fl) ok = ok && hx() == compat::bit_cast<uint32_t>(x);
        }
        if (!ok) {
            char b[200];
            std::snprintf(b, sizeof b, "polygon %zu differs; ours z %x window %u luma %u verts %u, MAME \"%.60s\"", qi, p.z,
                          p.window, p.luma, p.num_vertices, line.c_str());
            diverge(fr, b);
        }
    }

    std::ifstream f_;
    rt::Geo &geo_;
    rt::TgpBoard &board_;
    rt::M2ReplayBus &bus_;
    rt::Video video_;
    std::vector<Frame> frames_;
    std::vector<Update> updates_;
    std::string expect_fb_, expect_scr_;
};
