// The game's picture on the NV2A: the background tile layers as a texture,
// the game's polygons over them, then the front tile layers (their empty
// pixels see-through), 496x384 at 640x480 (scaled 1.25, centred). The
// runtime is in external 3D mode (Video::set_external_3d, with the
// Dreamcast's M2_DC_MEMORY composing both layers on the CPU): it hands over
// this frame's polygons and both tile layers and draws no 3D.
//
// A port of platform/dreamcast/game/renderer.h, which documents the order
// and the shading approximation: descending window, ascending z, newest
// first on ties, drawn back to front with no depth test; a textured polygon
// is its texture's luma as greys (luma RAM at the material's luma base)
// times its colour at full light scaled by its light. With no palette banks
// to run out of, every material is a real texture here: swizzled A8R8G8B8,
// transparent texel 15 with alpha 0 for the alpha test.
//
// Textures are cached by content, not by where they sit in texture RAM: a
// source is the 4-bit texel indices of one texture-sheet rectangle, checked
// again whenever the game writes texture RAM (VideoMem::tex_generation), and
// a material is a source with its 16 luma values. A material's key (64-bit,
// hex) names its HD replacement (hd.h): D:\hd\<key>.dds (drawn as the greys
// are, times the polygon's colour) or <key>_c.dds (a full-colour texture,
// times the light alone); with dumping on, each new material is written to
// T:\dump\<key>.tga at its source size.
#pragma once

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
        if (FAILED(device_->CreateTexture(512, 512, 1, 0, D3DFMT_LIN_X8R8G8B8, 0, &background_)) ||
            FAILED(device_->CreateTexture(512, 512, 1, 0, D3DFMT_LIN_A8R8G8B8, 0, &foreground_)))
            throw std::runtime_error("no memory for the tile layer textures");
        vertices_.reserve(kBatchVertices);
    }

    // video: after a frame, in external 3D mode.
    void draw(const rt::Video &video) {
        ++frame_;
        upload(video.background_layer(), background_);
        upload(video.foreground_layer(), foreground_);
        const auto &polys = video.gpu_polys();
        const rt::VideoMem &mem = video.gpu_mem();
        if (mem.tex_generation != tex_generation_) { // the game wrote texture RAM: check sources again
            tex_generation_ = mem.tex_generation;
            ++source_epoch_;
        }
        if (texture_bytes_ > kBudget) evict();
        sort(polys);
        ++colour_stamp_;
        builds_ = 0;

        display_.draw_texture(background_, float(kW), float(kH), kOffsetX, 0, kW * kScale, kH * kScale, false);

        // Materials front to back first: the build budget goes to what is
        // nearest (drawn last, largest on screen).
        for (Entry &e : order_) {
            const rt::GeoPoly &poly = polys[e.index];
            e.material = nullptr;
            if (!visible(poly, video)) continue;
            if ((poly.texheader[0] >> 14) & 1) e.material = material_for(poly, mem);
        }

        polygon_state();
        drawn = textured = 0;
        current_ = nullptr;
        bool current_set = false;
        for (auto e = order_.rbegin(); e != order_.rend(); ++e) {
            const rt::GeoPoly &poly = polys[e->index];
            if (!visible(poly, video)) continue;
            const int renderer = (poly.texheader[0] >> 13) & 3;
            Material *material = nullptr;
            if (renderer & 2) material = e->material;
            else if (renderer & 1) continue; // as the Vita and the Dreamcast: not drawn
            if (!current_set || material != current_) {
                flush();
                bind(material);
                current_set = true;
            }
            if (polygon(poly, video, colour(poly, mem, material), material)) {
                ++drawn;
                if (material) ++textured;
            }
        }
        flush();
        device_->SetTexture(0, nullptr);
        display_.draw_texture(foreground_, float(kW), float(kH), kOffsetX, 0, kW * kScale, kH * kScale, true);
    }

    // The last frame: polygons drawn (of them textured); the cache.
    unsigned drawn = 0, textured = 0;
    size_t materials() const { return materials_.size(); }
    size_t texture_bytes() const { return texture_bytes_; }
    unsigned evictions = 0, rebuilt_sources = 0, hd_loaded = 0;

private:
    static constexpr int kW = rt::Video::W, kH = rt::Video::H;
    static constexpr float kScale = 480.0f / kH, kOffsetX = (640.0f - kW * kScale) * 0.5f;
    static constexpr uint32_t kTextureLimit = 256;     // source textures at most this a side
    static constexpr unsigned kBuildsPerFrame = 48;    // new materials a frame
    static constexpr uint32_t kPaletteLuma = 0xf0;     // the light the greys are shaded at (luma & 0xf0 at most)
    static constexpr size_t kBudget = 4u << 20;        // texture memory before unused materials go
    static constexpr size_t kBatchVertices = 3 * 1024; // vertices in one DrawPrimitiveUP

    struct Source {
        uint32_t w = 0, h = 0, source_w = 0, source_h = 0;
        uint64_t hash = 0, epoch = 0;
        std::vector<uint8_t> texels; // w x h indices 0..15
    };
    struct Material {
        IDirect3DTexture8 *texture = nullptr;
        size_t bytes = 0;
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

    // ARGB8888 (kW wide) into a linear 512-wide texture.
    static void upload(const std::vector<uint32_t> &layer, IDirect3DTexture8 *texture) {
        if (layer.size() < size_t(kW) * kH) return;
        D3DLOCKED_RECT locked;
        if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) return;
        for (int y = 0; y < kH; ++y)
            std::memcpy(static_cast<uint8_t *>(locked.pBits) + size_t(y) * locked.Pitch, &layer[size_t(y) * kW],
                        size_t(kW) * 4);
        texture->UnlockRect(0);
    }

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
    static constexpr uint32_t kColourSlots = 1024;
    ColourSlot colours_[kColourSlots] = {};
    uint32_t colour_stamp_ = 1;

    // The source a polygon samples: its sheet rectangle's texel indices,
    // read again when texture RAM has changed since they were.
    Source &source_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
        const uint32_t key = uint32_t(poly.texheader[0] & 0x23ff) | uint32_t(poly.texheader[2] & 0x1fff) << 16;
        Source &s = sources_[key];
        if (s.epoch == source_epoch_ && !s.texels.empty()) return s;
        s.source_w = 32u << (poly.texheader[0] & 7);
        s.source_h = 32u << ((poly.texheader[0] >> 3) & 7);
        const uint32_t step_x = std::max(1u, s.source_w / kTextureLimit), step_y = std::max(1u, s.source_h / kTextureLimit);
        s.w = s.source_w / step_x;
        s.h = s.source_h / step_y;
        const uint32_t bx = 32u * (poly.texheader[2] & 0x3f);
        const uint32_t by = 32u * ((poly.texheader[2] >> 6) & 0x1f);
        const uint32_t *sheet = (poly.texheader[2] & 0x1000) ? mem.tex1 : mem.tex0;
        s.texels.resize(size_t(s.w) * s.h);
        for (uint32_t y = 0; y < s.h; ++y)
            for (uint32_t x = 0; x < s.w; ++x)
                s.texels[size_t(y) * s.w + x] =
                    uint8_t(rt::read_texel_quad(bx, by, x * step_x, x * step_x, y * step_y, y * step_y, sheet).t00 >> 4);
        const uint64_t hash =
            fnv(0xcbf29ce484222325ull ^ (uint64_t(s.source_w) << 32 | s.source_h), s.texels.data(), s.texels.size());
        if (s.epoch && hash != s.hash) ++rebuilt_sources;
        s.hash = hash;
        s.epoch = source_epoch_;
        return s;
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

    Material *material_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
        Source &source = source_for(poly, mem);
        uint32_t palette[16];
        uint8_t lumas[16];
        greys(poly, mem, palette, lumas);
        const uint8_t transparent = (poly.texheader[0] >> 13) & 1;
        const uint64_t key = fnv(fnv(source.hash, lumas, 16), &transparent, 1);
        // The addressing is the polygon's (mirror bits), not the texture's.
        const uint64_t slot_key = key ^ (uint64_t((poly.texheader[0] >> 8) & 3) << 62);
        auto found = materials_.find(slot_key);
        if (found != materials_.end()) {
            found->second.used = frame_;
            return &found->second;
        }
        if (builds_ >= kBuildsPerFrame) return nullptr;
        ++builds_;
        Material m;
        m.u_scale = 1.0f / (8.0f * float(source.source_w));
        m.v_scale = 1.0f / (8.0f * float(source.source_h));
        m.address_u = ((poly.texheader[0] >> 8) & 1) ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP;
        m.address_v = ((poly.texheader[0] >> 9) & 1) ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP;
        m.used = frame_;
        if (HdTextures::Loaded hd = hd_.load(device_, key); hd.texture) {
            m.texture = hd.texture;
            m.bytes = hd.bytes;
            m.colour_texture = hd.colour;
            ++hd_loaded;
        } else {
            pixels_.resize(size_t(source.w) * source.h);
            for (size_t i = 0; i < pixels_.size(); ++i) pixels_[i] = palette[source.texels[i]];
            if (hd_.dumping()) hd_.dump(key, pixels_.data(), source.w, source.h);
            if (FAILED(device_->CreateTexture(source.w, source.h, 1, 0, D3DFMT_A8R8G8B8, 0, &m.texture))) return nullptr;
            D3DLOCKED_RECT locked;
            if (FAILED(m.texture->LockRect(0, &locked, nullptr, 0))) {
                m.texture->Release();
                return nullptr;
            }
            XGSwizzleRect(pixels_.data(), source.w * 4, nullptr, locked.pBits, source.w, source.h, nullptr, 4);
            m.texture->UnlockRect(0);
            m.bytes = size_t(source.w) * source.h * 4;
        }
        texture_bytes_ += m.bytes;
        return &materials_.emplace(slot_key, m).first->second;
    }

    // Over budget: let the GPU finish with them, then drop the materials not
    // drawn for a second (or, still over, every one; they build again).
    void evict() {
        device_->BlockUntilIdle();
        for (int pass = 0; pass < 2 && texture_bytes_ > kBudget / 2; ++pass) {
            for (auto it = materials_.begin(); it != materials_.end();) {
                if (pass == 1 || frame_ - it->second.used > 60) {
                    texture_bytes_ -= it->second.bytes;
                    it->second.texture->Release();
                    it = materials_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        ++evictions;
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
    IDirect3DTexture8 *background_ = nullptr, *foreground_ = nullptr;
    uint64_t frame_ = 0, tex_generation_ = ~0ull, source_epoch_ = 1;
    unsigned builds_ = 0;
    size_t texture_bytes_ = 0;
    Material *current_ = nullptr;
    std::vector<Entry> order_, order_tmp_;
    std::vector<Vertex> vertices_;
    std::vector<uint32_t> pixels_;
    std::unordered_map<uint32_t, Source> sources_;
    std::unordered_map<uint64_t, Material> materials_;
    uint8_t gamma_[256] = {};
};

} // namespace xbox
