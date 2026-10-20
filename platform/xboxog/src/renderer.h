// The game's picture on the NV2A: the background tile layers as a texture,
// the game's polygons over them, then the front tile layers (their empty
// pixels see-through), 496x384 at 640x480 (scaled 1.25, centred). The
// runtime is in external 3D mode (Video::set_external_3d, with the
// Dreamcast's M2_DC_MEMORY and M2_DC_SPEED: both layers composed in 16-bit
// formats, only when they change, and the race's scenery as a separately
// scrolled layer): it hands over this frame's polygons and the layers and
// draws no 3D.
//
// A port of platform/dreamcast/game/renderer.h, which documents the order
// and the shading approximation: descending window, ascending z, newest
// first on ties, drawn back to front with no depth test; a textured polygon
// is its texture's luma as greys (luma RAM at the material's luma base)
// times its colour at full light scaled by its light. As on the Dreamcast,
// the texels are palette indices: a source (the 4-bit texel indices of one
// texture-sheet rectangle) is one swizzled P8 texture, and its greys a
// 32-entry palette shared by every texture with the same 16 luma values
// (transparent texel 15 with alpha 0 for the alpha test). The NV2A has no
// palette banks to run out of, so the lighting variants of a texture cost a
// palette each, not a texture each.
//
// Sources are cached by content, not by where they sit in texture RAM, and
// read again only when the game writes one of their rows (VideoMem::tex_rows).
// A material is a source with its 16 luma values: its key (64-bit, hex) names
// its HD replacement (hd.h): D:\hd\<key>.dds (drawn as the greys are, times
// the polygon's colour) or <key>_c.dds (a full-colour texture, times the
// light alone); with dumping on, each new material is written to
// T:\dump\<key>.tga at its source size. A texture let go (changed, evicted)
// is released two frames later, when the GPU is done with it.
//
// Model 2 has no blending: its translucency (smoke, shadows) is the checker
// flag (texheader[0] bit 15), which draws only every other pixel, as a
// checkerboard. Here a checkerboard is written into the stencil each frame
// and checker polygons are drawn where it is set: 1 output pixel cells, the
// look of the arcade's at 1.25 times its resolution.
#pragma once

#ifndef M2_DC_SPEED
#error "renderer.h draws the runtime's 16-bit layers: build with M2_DC_SPEED"
#endif

#include <xtl.h>
#include <xgraphics.h>

#include "runtime/raster_texel.h"
#include "runtime/video.h"

#include "display.h"
#include "hd.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace xbox {

class Renderer {
public:
    explicit Renderer(Display &display, HdTextures &hd) : display_(display), device_(display.device()), hd_(hd) {
        for (int i = 0; i < 256; i++) gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
        // The runtime's 16-bit layers (M2_DC_SPEED): the background RGB565,
        // or ARGB1555 over the scrolled layer; the front layers ARGB1555, 0
        // see-through; the scrolled layer's 512 x 512 pixmap RGB565.
        if (FAILED(device_->CreateTexture(512, 512, 1, 0, D3DFMT_LIN_R5G6B5, 0, &background_)) ||
            FAILED(device_->CreateTexture(512, 512, 1, 0, D3DFMT_LIN_A1R5G5B5, 0, &background_over_)) ||
            FAILED(device_->CreateTexture(512, 512, 1, 0, D3DFMT_LIN_A1R5G5B5, 0, &foreground_)) ||
            FAILED(device_->CreateTexture(512, 512, 1, 0, D3DFMT_LIN_R5G6B5, 0, &scroll_)))
            throw std::runtime_error("no memory for the tile layer textures");
        // The checkerboard: 2 x 2 texels, alpha set where x ^ y is odd (a 2 x 2
        // texture's swizzled order is its linear one).
        if (FAILED(device_->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, 0, &checker_)))
            throw std::runtime_error("no memory for the checker texture");
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(checker_->LockRect(0, &locked, nullptr, 0))) {
            auto *texels = static_cast<uint32_t *>(locked.pBits);
            texels[0] = 0;
            texels[1] = 0xffffffffu;
            texels[2] = 0xffffffffu;
            texels[3] = 0;
            checker_->UnlockRect(0);
        }
        vertices_.reserve(kBatchVertices);
    }

    // video: after a drawn frame, in external 3D mode.
    void draw(const rt::Video &video) {
        ++frame_;
        uint64_t t = clock();
        const auto lap = [&](int step) {
            const uint64_t now = clock();
            step_ticks[step] += now - t;
            t = now;
        };
        // A layer the runtime did not compose again is already in its texture.
        const bool scrolled = video.back_scrolled();
        // Only its lines composed since (lines16_changed): all of them the first
        // time and when the back layer moves between its two textures.
        auto &layers = const_cast<rt::Video &>(video);
        if (video.background16_generation() != back_uploaded_ || scrolled != back_scrolled_) {
            const bool whole = back_uploaded_ == ~0ull || scrolled != back_scrolled_;
            upload(video.background16(), scrolled ? background_over_ : background_, whole ? nullptr : video.lines16_changed(0));
            back_uploaded_ = video.background16_generation();
            back_scrolled_ = scrolled;
        }
        layers.lines16_taken(0);
        if (video.foreground16_generation() != front_uploaded_) {
            upload(video.foreground16(), foreground_, front_uploaded_ == ~0ull ? nullptr : video.lines16_changed(1));
            front_uploaded_ = video.foreground16_generation();
        }
        layers.lines16_taken(1);
        if (scrolled) update_scroll(video);
        lap(0);
        const auto &polys = video.gpu_polys();
        const rt::VideoMem &mem = video.gpu_mem();
        bury();
        if (texture_bytes_ > kBudget) evict();
        else evict_palettes();
        sort(polys);
        ++colour_stamp_;
        builds_ = 0;
        build_deadline_ = clock() + kBuildTicks;
        lap(1);

        if (scrolled) {
            scroll_quads(video.scroll_x(), video.scroll_y());
            display_.draw_texels(background_over_, 0, 0, float(kW), float(kH), kOffsetX, 0, kW * kScale, kH * kScale, true);
        } else {
            display_.draw_texels(background_, 0, 0, float(kW), float(kH), kOffsetX, 0, kW * kScale, kH * kScale, false);
        }

        // Materials front to back first: the build budget goes to what is
        // nearest (drawn last, largest on screen).
        for (Entry &e : order_) {
            const rt::GeoPoly &poly = polys[e.index];
            e.material = nullptr;
            if (!visible(poly, video)) continue;
            if ((poly.texheader[0] >> 14) & 1) e.material = cached_material(poly, mem);
        }
        lap(2);

        write_checker();
        polygon_state();
        drawn = textured = checkered = 0;
        current_ = nullptr;
        bool current_set = false, current_checker = false;
        for (auto e = order_.rbegin(); e != order_.rend(); ++e) {
            const rt::GeoPoly &poly = polys[e->index];
            if (!visible(poly, video)) continue;
            const int renderer = (poly.texheader[0] >> 13) & 3;
            Material *material = nullptr;
            if (renderer & 2) material = e->material;
            else if (renderer & 1) continue; // as the Vita and the Dreamcast: not drawn
            const bool checker = (poly.texheader[0] >> 15) & 1;
            if (!current_set || material != current_ || checker != current_checker) {
                flush();
                bind(material);
                if (!current_set || checker != current_checker)
                    device_->SetRenderState(D3DRS_STENCILENABLE, checker ? TRUE : FALSE);
                current_set = true;
                current_checker = checker;
            }
            if (polygon(poly, video, colour(poly, mem, material), material)) {
                ++drawn;
                if (material) ++textured;
                if (checker) ++checkered;
            }
        }
        flush();
        device_->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device_->SetTexture(0, nullptr);
        display_.draw_texels(foreground_, 0, 0, float(kW), float(kH), kOffsetX, 0, kW * kScale, kH * kScale, true);
        lap(3);
    }

    // Time in draw's steps since cleared (QueryPerformanceCounter ticks):
    // the tile layers' uploads, the cache upkeep and sort, the materials, the
    // polygons (and the layers' quads).
    uint64_t step_ticks[4] = {};
    static double milliseconds(uint64_t ticks) { return double(ticks) * 1000.0 / double(ticks_per_second()); }
    static uint64_t ticks_per_second() {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return uint64_t(frequency.QuadPart);
    }

    // The last frame: polygons drawn (of them textured, and checker ones); the cache.
    unsigned drawn = 0, textured = 0, checkered = 0;
    size_t materials() const { return materials_.size(); }
    size_t palettes() const { return palettes_.size(); }
    size_t texture_bytes() const { return texture_bytes_; }
    unsigned evictions = 0, rebuilt_sources = 0, hd_loaded = 0;
    // Since the start: sources read (texel decodes), source textures built,
    // and textures that waited a frame for the build budget.
    unsigned source_reads = 0, built = 0, deferred = 0, palettes_made = 0;

private:
    static constexpr int kW = rt::Video::W, kH = rt::Video::H;
    static constexpr float kScale = 480.0f / kH, kOffsetX = (640.0f - kW * kScale) * 0.5f;
    static constexpr uint32_t kTextureLimit = 256;     // source textures at most this a side
    static constexpr uint32_t kPaletteLuma = 0xf0;     // the light the greys are shaded at (luma & 0xf0 at most)
    static constexpr size_t kBudget = 4u << 20;        // source textures before the least recently used go
    static constexpr size_t kBatchVertices = 3 * 1024; // vertices in one DrawPrimitiveUP
    // Source textures made again a frame (their texels changed): as many as
    // fit in 4 ms, at most 256; the rest are drawn with what they had before
    // and wait a frame. New ones are always made (texture_for).
    static constexpr uint64_t kBuildMicroseconds = 4000;
    static constexpr unsigned kBuildsPerFrame = 256;

    struct Source {
        uint32_t w = 0, h = 0, source_w = 0, source_h = 0, bx = 0, by = 0, sheet = 0;
        uint64_t hash = 0;
        uint32_t checked = 0;        // VideoMem::tex_generation (truncated) when last read or checked
        bool valid = false;          // read at least once (hash, size and place known)
        // w x h indices 0..15: only until the texture is made from them (the
        // texture holds them then; keeping both doubled the memory).
        std::vector<uint8_t> texels;
        IDirect3DTexture8 *texture = nullptr; // P8, made from texels of hash `built`
        uint64_t built = 0, used = 0;
    };
    // How a polygon is textured: a source's texture and a palette, or an HD
    // replacement (no palette). Cheap: one per content, greys and mirroring.
    struct Material {
        IDirect3DTexture8 *texture = nullptr;
        IDirect3DPalette8 *palette = nullptr;
        size_t hd_bytes = 0; // an HD replacement's own texture (the material owns it)
        float u_scale = 0, v_scale = 0;
        DWORD address_u = D3DTADDRESS_WRAP, address_v = D3DTADDRESS_WRAP;
        bool colour_texture = false; // an HD replacement in full colour: times the light only
        uint64_t used = 0;           // the last frame it was drawn
    };
    struct Entry { uint32_t key; size_t index; Material *material; };
    struct Vertex { float x, y, z, rhw; D3DCOLOR diffuse; float u, v; };
    static constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    static uint16_t le16(const uint8_t *p, uint32_t index) { return uint16_t(p[index * 2] | p[index * 2 + 1] << 8); }
    static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
        return h;
    }

    static bool visible(const rt::GeoPoly &poly, const rt::Video &video) {
        return poly.window <= video.gpu_windows() && poly.num_vertices >= 3 && poly.num_vertices <= 8;
    }

    // A 16-bit layer (rows of rt::Video::kLayerStride, the first kH shown)
    // into a linear 512-wide texture: the rows flagged in lines, or all.
    static_assert(rt::Video::kLayerStride == 512, "a layer's rows are its texture's");
    static void upload(const std::vector<uint16_t> &layer, IDirect3DTexture8 *texture, const uint8_t *lines) {
        if (layer.size() < size_t(512) * kH) return;
        D3DLOCKED_RECT locked;
        if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) return;
        for (int y = 0; y < kH; ++y)
            if (!lines || lines[y])
                std::memcpy(static_cast<uint8_t *>(locked.pBits) + size_t(y) * locked.Pitch, &layer[size_t(y) * 512], 512 * 2);
        texture->UnlockRect(0);
    }

    // The scrolled layer's texture: the 8 x 8 tiles whose pixels changed
    // (all of them after a pen change), from the runtime's scroll_pixel.
    void update_scroll(const rt::Video &video) {
        const bool all = video.scroll_all_changed();
        const uint8_t *changed = video.scroll_tiles_changed();
        D3DLOCKED_RECT locked;
        if (FAILED(scroll_->LockRect(0, &locked, nullptr, 0))) return;
        for (uint32_t t = 0; t < 64 * 64; ++t) {
            if (!all && !changed[t]) continue;
            const uint32_t tx = (t & 63) * 8, ty = (t >> 6) * 8;
            for (uint32_t y = ty; y < ty + 8; ++y) {
                auto *row = reinterpret_cast<uint16_t *>(static_cast<uint8_t *>(locked.pBits) + size_t(y) * locked.Pitch);
                for (uint32_t x = tx; x < tx + 8; ++x) row[x] = video.scroll_pixel(x, y);
            }
            ++scroll_tiles;
        }
        scroll_->UnlockRect(0);
        const_cast<rt::Video &>(video).scroll_taken();
    }

    // The screen's kW x kH from (sx, sy) of the 512 x 512 scrolled layer,
    // wrapping as the pixmap does: a linear texture cannot repeat, so the
    // view is cut where it wraps, into up to four quads.
    void scroll_quads(int sx, int sy) {
        const float u0 = float(sx & 511), v0 = float(sy & 511);
        const float wu = std::min(float(kW), 512.f - u0), hv = std::min(float(kH), 512.f - v0);
        const float pieces[4][4] = {{u0, v0, 0, 0}, {0, v0, wu, 0}, {u0, 0, 0, hv}, {0, 0, wu, hv}};
        for (const auto &p : pieces) {
            const float w = p[2] ? float(kW) - wu : wu, h = p[3] ? float(kH) - hv : hv;
            if (w <= 0 || h <= 0) continue;
            display_.draw_texels(scroll_, p[0], p[1], p[0] + w, p[1] + h, kOffsetX + p[2] * kScale, p[3] * kScale,
                                 w * kScale, h * kScale, false);
        }
    }

public:
    unsigned scroll_tiles = 0; // scrolled-layer tiles converted, since the start

private:
    void sort(const std::vector<rt::GeoPoly> &polys) {
        const size_t count = polys.size();
        order_.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const size_t index = count - 1 - i;
            order_[i] = {((255u - uint32_t(polys[index].window)) << 16) | uint32_t(polys[index].z), index, nullptr};
        }
        // Key ascending, equal keys index descending (as built): a stable
        // radix sort of the 24-bit key, a byte a pass.
        order_tmp_.resize(count);
        for (unsigned shift = 0; shift < 24; shift += 8) {
            size_t at[257] = {};
            for (const Entry &e : order_) ++at[((e.key >> shift) & 0xff) + 1];
            for (int b = 0; b < 256; ++b) at[b + 1] += at[b];
            for (const Entry &e : order_) order_tmp_[at[(e.key >> shift) & 0xff]++] = e;
            order_.swap(order_tmp_);
        }
    }

    // A textured polygon's vertex colour: its colour at full light (luma
    // 0x3f through the translation table), scaled by its light; a
    // full-colour HD texture's: the light alone.
    uint32_t textured_colour(const rt::GeoPoly &poly, const rt::VideoMem &mem, bool colour_texture) const {
        const uint32_t light = std::min<uint32_t>(256, uint32_t(poly.luma & 0xf0) * 256 / kPaletteLuma);
        if (colour_texture) {
            const uint32_t g = std::min<uint32_t>(255, light);
            return 0xff000000u | g << 16 | g << 8 | g;
        }
        const uint32_t color = le16(mem.palram, ((poly.texheader[3] >> 6) & 0x3ff) + 0x1000);
        auto channel = [&](uint32_t base, uint32_t c) {
            return uint32_t(gamma_[le16(mem.colorxlat, base + (c << 8) + 0x3f) & 0xff]) * light >> 8;
        };
        return 0xff000000u | std::min<uint32_t>(255, channel(0, color & 0x1f)) << 16 |
               std::min<uint32_t>(255, channel(0x4000 / 2, (color >> 5) & 0x1f)) << 8 |
               std::min<uint32_t>(255, channel(0x8000 / 2, (color >> 10) & 0x1f));
    }

    uint32_t solid_colour(const rt::GeoPoly &poly, const rt::VideoMem &mem) const {
        const uint32_t color = le16(mem.palram, ((poly.texheader[3] >> 6) & 0x3ff) + 0x1000);
        const uint32_t luma = poly.luma >> 2;
        const uint8_t r = gamma_[le16(mem.colorxlat, (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
        const uint8_t g = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
        const uint8_t b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
        return 0xff000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | b;
    }

    // Within a frame a polygon's colour depends only on its colour entry and
    // light (and, textured, whether the texture is a full-colour one).
    uint32_t colour(const rt::GeoPoly &poly, const rt::VideoMem &mem, const Material *material) {
        const uint32_t entry = (poly.texheader[3] >> 6) & 0x3ff;
        const uint32_t key = material ? 0x10000u | uint32_t(material->colour_texture) << 15 | entry << 4 | (poly.luma & 0xf0u) >> 4
                                      : entry << 6 | poly.luma >> 2;
        ColourSlot &slot = colours_[(key ^ key >> 7) & (kColourSlots - 1)];
        if (slot.stamp != colour_stamp_ || slot.key != key)
            slot = {colour_stamp_, key,
                    material ? textured_colour(poly, mem, material->colour_texture) : solid_colour(poly, mem)};
        return slot.argb;
    }
    struct ColourSlot { uint32_t stamp, key, argb; };

    // Within a frame a polygon's material depends only on its texture header
    // words 0-2 (luma, palette and texture RAM change between frames, not
    // within one): this frame's lookups, direct-mapped, in front of
    // material_for's hashing (most of a frame's polygons share a few dozen).
    Material *cached_material(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
        const uint32_t a = poly.texheader[0], b = poly.texheader[1], c = poly.texheader[2];
        uint32_t h = a * 0x9e3779b1u ^ b * 0x85ebca6bu ^ c * 0xc2b2ae35u;
        h ^= h >> 15;
        MaterialSlot &slot = material_slots_[h & (kMaterialSlots - 1)];
        if (slot.stamp == colour_stamp_ && slot.a == a && slot.b == b && slot.c == c) return slot.material;
        Material *material = material_for(poly, mem);
        slot = {colour_stamp_, a, b, c, material};
        return material;
    }
    struct MaterialSlot { uint32_t stamp, a, b, c; Material *material; };
    static constexpr uint32_t kMaterialSlots = 512;
    MaterialSlot material_slots_[kMaterialSlots] = {};
    static constexpr uint32_t kColourSlots = 1024;
    ColourSlot colours_[kColourSlots] = {};
    uint32_t colour_stamp_ = 1;

    // The source a polygon samples: its sheet rectangle's texel indices,
    // read again only when the game has written one of its rows since.
    Source &source_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
        const uint32_t key = uint32_t(poly.texheader[0] & 0x23ff) | uint32_t(poly.texheader[2] & 0x1fff) << 16;
        Source &s = sources_[key];
        s.used = frame_;
        const uint32_t generation = uint32_t(mem.tex_generation);
        if (s.valid) {
            if (s.checked == generation) return s;
            if (!written_since(s, mem)) {
                s.checked = generation;
                return s;
            }
        }
        s.source_w = 32u << (poly.texheader[0] & 7);
        s.source_h = 32u << ((poly.texheader[0] >> 3) & 7);
        const uint32_t step_x = std::max(1u, s.source_w / kTextureLimit), step_y = std::max(1u, s.source_h / kTextureLimit);
        s.w = s.source_w / step_x;
        s.h = s.source_h / step_y;
        s.bx = 32u * (poly.texheader[2] & 0x3f);
        s.by = 32u * ((poly.texheader[2] >> 6) & 0x1f);
        s.sheet = (poly.texheader[2] & 0x1000) ? 1 : 0;
        read_texels(s, mem, s.texels);
        const uint64_t hash =
            fnv(0xcbf29ce484222325ull ^ (uint64_t(s.source_w) << 32 | s.source_h), s.texels.data(), s.texels.size());
        if (s.valid && hash != s.hash) ++rebuilt_sources;
        ++source_reads;
        s.hash = hash;
        s.checked = generation;
        s.valid = true;
        if (s.texture && s.built == hash) release(s.texels); // (written, but the same texels)
        return s;
    }

    // The source's texel indices, at most kTextureLimit a side (larger ones
    // sampled down), from texture RAM as it is now.
    static void read_texels(const Source &s, const rt::VideoMem &mem, std::vector<uint8_t> &out) {
        const uint32_t *sheet = s.sheet ? mem.tex1 : mem.tex0;
        const uint32_t step_x = s.source_w / s.w, step_y = s.source_h / s.h;
        out.resize(size_t(s.w) * s.h);
        if (step_x == 1 && step_y == 1) {
            decode(s, sheet, out.data());
            return;
        }
        for (uint32_t y = 0; y < s.h; ++y)
            for (uint32_t x = 0; x < s.w; ++x)
                out[size_t(y) * s.w + x] = uint8_t(
                    rt::read_texel_quad(s.bx, s.by, x * step_x, x * step_x, y * step_y, y * step_y, sheet).t00 >> 4);
    }
    static void release(std::vector<uint8_t> &bytes) { std::vector<uint8_t>().swap(bytes); }

    // A whole source at its own size, a 16-bit cell (2 x 2 texels) at a
    // time: read_texel_quad's layout (cell row (y >> 1), flipped by 512 for
    // columns past the 1024 fold; the nibble by the texel's parity, which is
    // the sheet's as sources start on multiples of 32), without its per-texel
    // call. The same texels as read_texel_quad's t00, one by one.
    static void decode(const Source &s, const uint32_t *sheet, uint8_t *texels) {
        for (uint32_t y = 0; y < s.h; y += 2) {
            uint8_t *even = texels + size_t(y) * s.w, *odd = even + s.w;
            for (uint32_t x = 0; x < s.w; x += 2) {
                uint32_t sx = s.bx + x, row = (s.by + y) >> 1;
                if (sx >= 1024) {
                    sx -= 1024;
                    row ^= 512;
                }
                const uint32_t off = (row << 9) + (sx >> 1);
                const uint32_t cell = sheet[(off >> 1) & 0x7ffff] >> ((off & 1) << 4);
                even[x] = uint8_t((cell >> 12) & 15);
                even[x + 1] = uint8_t((cell >> 8) & 15);
                odd[x] = uint8_t((cell >> 4) & 15);
                odd[x + 1] = uint8_t(cell & 15);
            }
        }
    }

    // Whether the game wrote any of the source's rows of 16-bit cells
    // (VideoMem::tex_rows) after it was last checked. A cell row holds two
    // texel rows; columns from 1024 on are on the row with bit 9 flipped
    // (read_texel_quad's fold).
    static bool written_since(const Source &s, const rt::VideoMem &mem) {
        if (!mem.tex_rows) return true;
        const uint32_t *rows = mem.tex_rows + s.sheet * 2048;
        const bool low = s.bx < 1024, high = s.bx + s.source_w > 1024;
        for (uint32_t y = s.by; y < s.by + s.source_h; y += 2) {
            const uint32_t r = (y >> 1) & 2047;
            if (low && int32_t(rows[r] - s.checked) > 0) return true;
            if (high && int32_t(rows[r ^ 512] - s.checked) > 0) return true;
        }
        return false;
    }

    // The source's P8 texture, made again when its texels changed. A new one
    // is always made (cheap now: no texture yet means a polygon drawn flat,
    // a white flash); a changed one only within the frame's build budget,
    // past which the texture it had stays a frame longer.
    bool texture_for(Source &s) {
        if (s.texture && s.built == s.hash) return true;
        if (s.texture && (builds_ >= kBuildsPerFrame || clock() > build_deadline_)) {
            ++deferred;
            return true;
        }
        IDirect3DTexture8 *texture = nullptr;
        if (FAILED(device_->CreateTexture(s.w, s.h, 1, 0, D3DFMT_P8, 0, &texture))) return s.texture != nullptr;
        D3DLOCKED_RECT locked;
        if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
            texture->Release();
            return s.texture != nullptr;
        }
        XGSwizzleRect(s.texels.data(), s.w, nullptr, locked.pBits, s.w, s.h, nullptr, 1);
        texture->UnlockRect(0);
        release(s.texels);
        retire(s);
        s.texture = texture;
        s.built = s.hash;
        texture_bytes_ += size_t(s.w) * s.h;
        ++builds_;
        ++built;
        return true;
    }

    // A texel's grey: its luma (luma RAM at the material's luma base, at
    // kPaletteLuma); alpha 0 for a transparent material's texel 15.
    static void greys(const rt::GeoPoly &poly, const rt::VideoMem &mem, uint32_t out[16], uint8_t lumas[16]) {
        const uint32_t lumabase = uint32_t(poly.texheader[1] & 0xff) << 7;
        const bool transparent = (poly.texheader[0] >> 13) & 1;
        for (unsigned t = 0; t < 16; ++t) {
            lumas[t] = mem.lumaram[(lumabase + (t << 3)) * 4];
            const uint32_t luma = std::min<uint32_t>(uint32_t(lumas[t]) * kPaletteLuma / 256, 0x3f);
            const uint32_t g = luma * 255 / 0x3f;
            out[t] = (transparent && t == 0x0f) ? 0 : 0xff000000u | g << 16 | g << 8 | g;
        }
    }

    // The palette of these greys, shared by every texture lit the same way.
    IDirect3DPalette8 *palette_for(uint64_t key, const uint32_t greys[16]) {
        auto found = palettes_.find(key);
        if (found != palettes_.end()) {
            found->second.used = frame_;
            return found->second.palette;
        }
        IDirect3DPalette8 *palette = nullptr;
        if (FAILED(device_->CreatePalette(D3DPALETTE_32, &palette))) return nullptr;
        D3DCOLOR *colours = nullptr;
        if (FAILED(palette->Lock(&colours, 0))) {
            palette->Release();
            return nullptr;
        }
        for (unsigned i = 0; i < 32; ++i) colours[i] = i < 16 ? greys[i] : 0;
        palette->Unlock();
        palettes_.emplace(key, Palette{palette, frame_});
        ++palettes_made;
        return palette;
    }

    Material *material_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
        Source &source = source_for(poly, mem);
        uint32_t palette_colours[16];
        uint8_t lumas[16];
        greys(poly, mem, palette_colours, lumas);
        const uint8_t transparent = (poly.texheader[0] >> 13) & 1;
        const uint64_t look = fnv(fnv(0xcbf29ce484222325ull, lumas, 16), &transparent, 1);
        const uint64_t key = fnv(fnv(source.hash, lumas, 16), &transparent, 1); // the HD name
        // The addressing is the polygon's (mirror bits), not the texture's.
        const uint64_t slot_key = key ^ (uint64_t((poly.texheader[0] >> 8) & 3) << 62);
        auto found = materials_.find(slot_key);
        if (found == materials_.end()) {
            Material m;
            m.u_scale = 1.0f / (8.0f * float(source.source_w));
            m.v_scale = 1.0f / (8.0f * float(source.source_h));
            m.address_u = ((poly.texheader[0] >> 8) & 1) ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP;
            m.address_v = ((poly.texheader[0] >> 9) & 1) ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP;
            if (HdTextures::Loaded hd = hd_.load(device_, key); hd.texture) {
                m.texture = hd.texture;
                m.hd_bytes = hd.bytes;
                m.colour_texture = hd.colour;
                texture_bytes_ += hd.bytes;
                ++hd_loaded;
            } else if (hd_.wants_dump(key)) {
                // (The source's texels may be gone into its texture: read again.)
                read_texels(source, mem, scratch_);
                argb_.resize(scratch_.size());
                for (size_t i = 0; i < argb_.size(); ++i) argb_[i] = palette_colours[scratch_[i]];
                hd_.dump(key, argb_.data(), source.w, source.h);
            }
            found = materials_.emplace(slot_key, m).first;
        }
        Material &m = found->second;
        m.used = frame_;
        if (m.hd_bytes) return &m;
        // The source's texture now (it may have been made again since).
        if (!texture_for(source)) return nullptr;
        m.texture = source.texture;
        m.palette = palette_for(look, palette_colours);
        return m.palette ? &m : nullptr;
    }

    // Over budget: let go of the least recently drawn source textures (never
    // one drawn in the last two frames) until a quarter of the budget is free;
    // they are made again when drawn. Materials not drawn for a minute go too.
    void evict() {
        std::vector<std::pair<uint64_t, uint32_t>> by_age; // (last drawn, key)
        for (const auto &[key, source] : sources_)
            if (source.texture && frame_ - source.used > 2) by_age.emplace_back(source.used, key);
        std::sort(by_age.begin(), by_age.end());
        for (const auto &[used, key] : by_age) {
            if (texture_bytes_ <= kBudget / 4 * 3) break;
            auto it = sources_.find(key);
            retire(it->second);
            sources_.erase(it);
        }
        evict_palettes();
        for (auto it = materials_.begin(); it != materials_.end();) {
            if (frame_ - it->second.used > 1800) {
                if (it->second.hd_bytes) {
                    texture_bytes_ -= it->second.hd_bytes;
                    graveyard_.emplace_back(frame_, it->second.texture);
                }
                it = materials_.erase(it);
            } else {
                ++it;
            }
        }
        ++evictions;
    }

    // Past kPalettes, the least recently used palettes (never one used in
    // the last two frames) go, down to three quarters of it: a lighting
    // table the game animates makes a new palette for every step.
    void evict_palettes() {
        if (palettes_.size() <= kPalettes) return;
        std::vector<std::pair<uint64_t, uint64_t>> by_age; // (last used, key)
        for (const auto &[key, palette] : palettes_)
            if (frame_ - palette.used > 2) by_age.emplace_back(palette.used, key);
        std::sort(by_age.begin(), by_age.end());
        for (const auto &[used, key] : by_age) {
            if (palettes_.size() <= kPalettes / 4 * 3) break;
            auto it = palettes_.find(key);
            graveyard_.emplace_back(frame_, it->second.palette);
            palettes_.erase(it);
        }
    }

    // A source's texture to the graveyard: released once the GPU is past it.
    void retire(Source &s) {
        if (!s.texture) return;
        texture_bytes_ -= size_t(s.w) * s.h;
        graveyard_.emplace_back(frame_, s.texture);
        s.texture = nullptr;
    }
    void bury() {
        size_t kept = 0;
        for (auto &[frame, resource] : graveyard_) {
            if (frame_ - frame >= 3) resource->Release();
            else graveyard_[kept++] = {frame, resource};
        }
        graveyard_.resize(kept);
    }

    static uint64_t clock() {
        LARGE_INTEGER count;
        QueryPerformanceCounter(&count);
        return uint64_t(count.QuadPart);
    }
    static uint64_t ticks(uint64_t microseconds) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return uint64_t(frequency.QuadPart) * microseconds / 1000000;
    }

    // The checkerboard into the stencil (1 where x ^ y is odd), over the
    // whole picture: the checker texture's texels alpha-tested, colour
    // writes off. Checker polygons then draw where it is 1 (polygon_state).
    void write_checker() {
        struct Vertex { float x, y, z, rhw, u, v; };
        const float w = 640.f, h = 480.f;
        const Vertex quad[4] = {{-.5f, -.5f, 0, 1, 0, 0},
                                {w - .5f, -.5f, 0, 1, w / 2, 0},
                                {-.5f, h - .5f, 0, 1, 0, h / 2},
                                {w - .5f, h - .5f, 0, 1, w / 2, h / 2}};
        device_->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device_->SetRenderState(D3DRS_LIGHTING, FALSE);
        device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device_->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        device_->SetRenderState(D3DRS_ALPHAREF, 0x80);
        device_->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        device_->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        device_->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        device_->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
        device_->SetRenderState(D3DRS_STENCILREF, 1);
        device_->SetRenderState(D3DRS_STENCILMASK, 0xff);
        device_->SetRenderState(D3DRS_STENCILWRITEMASK, 0xff);
        device_->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
        device_->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
        device_->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
        device_->SetTexture(0, checker_);
        device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device_->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
        device_->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        device_->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
        device_->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
        device_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device_->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
        device_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
        device_->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALL);
        device_->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        // For the checker polygons: drawn only where the stencil is 1.
        device_->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
        device_->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    }

    void polygon_state() {
        device_->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device_->SetRenderState(D3DRS_LIGHTING, FALSE);
        device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device_->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        device_->SetRenderState(D3DRS_ALPHAREF, 0x80);
        device_->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        device_->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
        device_->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
        device_->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
        device_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device_->SetVertexShader(kFvf);
    }

    void bind(Material *material) {
        current_ = material;
        if (material) {
            device_->SetTexture(0, material->texture);
            if (material->palette) device_->SetPalette(0, material->palette);
            device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device_->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            device_->SetTextureStageState(0, D3DTSS_ADDRESSU, material->address_u);
            device_->SetTextureStageState(0, D3DTSS_ADDRESSV, material->address_v);
        } else {
            device_->SetTexture(0, nullptr);
            device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }
    }

    void flush() {
        if (vertices_.empty()) return;
        device_->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(vertices_.size() / 3), vertices_.data(), sizeof(Vertex));
        vertices_.clear();
    }

    // A convex polygon as a fan of triangles into the batch.
    bool polygon(const rt::GeoPoly &poly, const rt::Video &video, uint32_t argb, const Material *material) {
        Vertex v[8];
        const int n = poly.num_vertices;
        const float cx = float(video.crtc_x() + poly.center[0]), cy = float((384 - poly.center[1]) + video.crtc_y());
        for (int i = 0; i < n; i++) {
            const float pz = poly.v[i].p[0];
            if (!(pz > 0.0f) || !std::isfinite(pz)) return false;
            const float inv = 1.0f / pz;
            v[i].x = kOffsetX + (cx + poly.v[i].x * inv) * kScale - 0.5f;
            v[i].y = (cy - poly.v[i].y * inv) * kScale - 0.5f;
            v[i].z = 0.5f;
            v[i].rhw = inv;
            v[i].diffuse = argb;
            v[i].u = material ? poly.v[i].p[1] * material->u_scale : 0.0f;
            v[i].v = material ? poly.v[i].p[2] * material->v_scale : 0.0f;
            if (!std::isfinite(v[i].x) || !std::isfinite(v[i].y) || !std::isfinite(inv)) return false;
        }
        if (vertices_.size() + size_t(n - 2) * 3 > kBatchVertices) flush();
        for (int k = 1; k < n - 1; ++k) {
            vertices_.push_back(v[0]);
            vertices_.push_back(v[k]);
            vertices_.push_back(v[k + 1]);
        }
        return true;
    }

    Display &display_;
    IDirect3DDevice8 *device_;
    HdTextures &hd_;
    IDirect3DTexture8 *background_ = nullptr, *background_over_ = nullptr, *foreground_ = nullptr, *scroll_ = nullptr;
    IDirect3DTexture8 *checker_ = nullptr;
    uint64_t back_uploaded_ = ~0ull, front_uploaded_ = ~0ull;
    bool back_scrolled_ = false;
    uint64_t frame_ = 0, build_deadline_ = 0;
    const uint64_t kBuildTicks = ticks(kBuildMicroseconds);
    unsigned builds_ = 0;
    size_t texture_bytes_ = 0;
    Material *current_ = nullptr;
    std::vector<Entry> order_, order_tmp_;
    std::vector<Vertex> vertices_;
    std::vector<uint32_t> argb_;
    std::vector<uint8_t> scratch_;
    std::unordered_map<uint32_t, Source> sources_;
    std::unordered_map<uint64_t, Material> materials_;
    struct Palette { IDirect3DPalette8 *palette; uint64_t used; };
    static constexpr size_t kPalettes = 256;
    std::unordered_map<uint64_t, Palette> palettes_; // by luma values and transparency
    std::vector<std::pair<uint64_t, IDirect3DResource8 *>> graveyard_; // (frame let go, texture or palette)
    uint8_t gamma_[256] = {};
};

} // namespace xbox
