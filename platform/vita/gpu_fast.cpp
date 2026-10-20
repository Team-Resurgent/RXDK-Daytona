#include "gpu_fast.h"
#include "perspective_vertices.h"
#include "runtime/raster_texel.h"
#include "system24_upload.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <psp2/kernel/processmgr.h>

namespace vita {
namespace {
constexpr float kDisplayW = 960.0f;
constexpr float kDisplayH = 544.0f;
constexpr float kSourceW = float(rt::Video::W);
constexpr float kSourceH = float(rt::Video::H);
constexpr float kScale = kDisplayH / kSourceH;
constexpr float kOffsetX = (kDisplayW - kSourceW * kScale) * 0.5f;

inline float sx(float x) { return kOffsetX + x * kScale; }
inline float sy(float y) { return y * kScale; }

} // namespace

GpuFastRenderer::GpuFastRenderer() {
    background_ = make_texture(layer_memory_, 896, rt::Video::H, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, 4);
    foreground_ = make_texture(layer_memory_, 896, rt::Video::H, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, 4);
    if (background_) vita2d_texture_set_filters(background_, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    if (foreground_) vita2d_texture_set_filters(foreground_, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    // Model 2's checker flag keeps only odd (native screen x XOR y) pixels.
    // A fixed point-sampled mask preserves those holes without extra geometry.
    checker_texture_ = make_texture(layer_memory_, 2, 2, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, 4);
    if (checker_texture_) {
        vita2d_texture_set_filters(checker_texture_, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
        const int u_result = sceGxmTextureSetUAddrMode(&checker_texture_->gxm_tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
        const int v_result = sceGxmTextureSetVAddrMode(&checker_texture_->gxm_tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
        auto *pixels = static_cast<uint32_t *>(vita2d_texture_get_datap(checker_texture_));
        const unsigned stride_bytes = vita2d_texture_get_stride(checker_texture_);
        if (!pixels || stride_bytes < 2 * sizeof(uint32_t) || stride_bytes % sizeof(uint32_t) != 0 ||
            u_result < 0 || v_result < 0) {
            delete checker_texture_;
            checker_texture_ = nullptr;
        } else {
            const unsigned stride = stride_bytes / sizeof(uint32_t);
            std::fill_n(pixels, stride * 2u, 0u);
            pixels[1] = RGBA8(255,255,255,255);
            pixels[stride] = RGBA8(255,255,255,255);
        }
    }
    for (auto &texture : system24_textures_) {
        texture = make_texture(layer_memory_, 512, 512, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, 4);
        if (!texture) { system24_ok_ = false; continue; }
        vita2d_texture_set_filters(texture, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetUAddrMode(&texture->gxm_tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
        sceGxmTextureSetVAddrMode(&texture->gxm_tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
    }
    materials_.reserve(2048);
    for (int i = 0; i < 256; ++i) gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
}

GpuFastRenderer::~GpuFastRenderer() {
    // main_gpu calls shutdown() before vita2d_fini(). Do not touch GXM-owned
    // allocations after libvita2d has already been torn down.
}

uint16_t GpuFastRenderer::le16(const uint8_t *base, uint32_t index) {
    return uint16_t(base[index * 2] | uint16_t(base[index * 2 + 1]) << 8);
}

uint32_t GpuFastRenderer::swap_rb(uint32_t argb) {
    const uint32_t a = argb & 0xff000000u;
    const uint32_t r = (argb >> 16) & 0xff;
    const uint32_t g = (argb >> 8) & 0xff;
    const uint32_t b = argb & 0xff;
    return a | (b << 16) | (g << 8) | r;
}

uint16_t GpuFastRenderer::rgba5551(uint32_t rgba) {
    return uint16_t(((rgba >> 3) & 0x001f) | ((rgba >> 6) & 0x03e0) |
                    ((rgba >> 9) & 0x7c00) | ((rgba >> 16) & 0x8000));
}

GpuFastRenderer::MaterialKey GpuFastRenderer::material_key(const rt::GeoPoly &poly) {
    return MaterialKey{uint16_t(poly.texheader[0] & 0x23ff),
                       uint16_t(poly.texheader[1] & 0x00ff),
                       uint16_t(poly.texheader[2] & 0x1fff),
                       uint16_t(poly.texheader[3] & 0xffc0), uint8_t(poly.luma & 0xf0)};
}

GpuFastRenderer::SourceKey GpuFastRenderer::source_key(const rt::GeoPoly &poly) {
    return SourceKey{uint16_t(poly.texheader[0] & 0x23ff),
                     uint16_t(poly.texheader[2] & 0x1fff)};
}

vita2d_texture *GpuFastRenderer::make_texture(GpuMemoryArena &arena, uint32_t w, uint32_t h,
                                               SceGxmTextureFormat format, uint32_t bytes_per_pixel) {
    // Descriptors do not own memory; only the arenas unmap/free it. In particular,
    // do not use vita2d_free_texture on these zero-initialized descriptors.
    auto *texture = new (std::nothrow) vita2d_texture{};
    if (!texture) return nullptr;
    const size_t stride = size_t((w + 7u) & ~7u) * bytes_per_pixel;
    void *data = arena.allocate(stride * h, 16);
    if (!data || sceGxmTextureInitLinear(&texture->gxm_tex, data, format, w, h, 0) < 0) {
        delete texture;
        return nullptr;
    }
    return texture;
}

void GpuFastRenderer::clear_cache() {
    materials_.clear();
    for (auto &entry : sources_) delete entry.second.texture;
    sources_.clear();
    source_memory_.reset();
    palette_memory_.reset();
    cached_bytes_ = 0;
    cache_reset_pending_ = false;
}

void GpuFastRenderer::reset_materials() {
    vita2d_wait_rendering_done();
    clear_cache();
    background_generation_ = foreground_generation_ = system24_generation_ = UINT64_MAX;
}

void GpuFastRenderer::prepare_frame() {
    // libvita2d reuses ONE temporary vertex pool, even with triple buffering.
    // Finish just before reuse, after CPU simulation has overlapped the old GPU work.
    // This also protects in-place tile texture uploads.
    vita2d_wait_rendering_done();
    pool_drops_ = 0;
    if (cache_reset_pending_) {
        clear_cache();
        ++cache_resets_;
    }
}

void GpuFastRenderer::shutdown() {
    if (shutdown_) return;
    reset_materials();
    delete background_; background_ = nullptr;
    delete foreground_; foreground_ = nullptr;
    delete checker_texture_; checker_texture_ = nullptr;
    for (auto &texture : system24_textures_) {
        delete texture;
        texture = nullptr;
    }
    layer_memory_.shutdown();
    source_memory_.shutdown();
    palette_memory_.shutdown();
    shutdown_ = true;
}

void GpuFastRenderer::upload_layer(vita2d_texture *texture, const std::vector<uint32_t> &pixels) {
    const size_t width = pixels.size() / rt::Video::H;
    if (!texture || width < rt::Video::W || width > 896) return;
    // libvita2d returns texture stride in BYTES. GPU04 accidentally treated
    // that value as a uint32_t element count, advancing each row four times
    // too far and eventually writing outside CDRAM. Keep the address
    // arithmetic byte-based, then cast only the selected row.
    auto *base = static_cast<uint8_t *>(vita2d_texture_get_datap(texture));
    const size_t stride_bytes = vita2d_texture_get_stride(texture);
    if (!base || stride_bytes < width * sizeof(uint32_t) ||
        (stride_bytes & (alignof(uint32_t) - 1)) != 0) return;
    for (int y = 0; y < rt::Video::H; ++y) {
        auto *row = reinterpret_cast<uint32_t *>(base + size_t(y) * stride_bytes);
        const uint32_t *src = pixels.data() + size_t(y) * width;
        for (size_t x = 0; x < width; ++x) row[x] = swap_rb(src[x]);
        if (width < 896) row[width] = row[width - 1]; // Linear-filter border padding.
    }
}

void GpuFastRenderer::update_system24_textures(const rt::Video &video) {
    system24_uploaded_tiles_ = 0;
    if (system24_generation_ == video.system24_texture_generation()) return;
    for (int layer = 0; layer < 4; ++layer) {
        vita2d_texture *background = system24_textures_[size_t(layer)];
        vita2d_texture *foreground = system24_textures_[size_t(layer + 4)];
        if (!background || !foreground) continue;
        system24_uploaded_tiles_ += upload_system24_layer(video, layer, system24_generation_,
            vita2d_texture_get_datap(background), vita2d_texture_get_stride(background),
            vita2d_texture_get_datap(foreground), vita2d_texture_get_stride(foreground));
    }
    system24_generation_ = video.system24_texture_generation();
}

bool GpuFastRenderer::draw_system24(const rt::Video &video, bool foreground) {
    if (!video.system24_gpu_compatible() &&
        !(foreground ? video.gpu_foreground() : video.gpu_background())) return false;
    if (!foreground) {
        system24_quads_ = 0;
        vita2d_draw_rectangle(sx(-float(video.wide_margin())), sy(0),
            video.width() * scale_, rt::Video::H * scale_, swap_rb(video.system24_pen(0)));
    }
    struct Rect { int x0, x1, y0, y1, h, v; };
    auto submit = [&](int source_layer, const std::vector<Rect> &rects) {
        if (rects.empty()) return true;
        const size_t count = rects.size() * 6u;
        if (vita2d_pool_free_space() < count * sizeof(vita2d_texture_vertex) + 2048u) {
            ++pool_drops_;
            return false;
        }
        auto *vertices = static_cast<vita2d_texture_vertex *>(
            vita2d_pool_memalign(unsigned(count * sizeof(vita2d_texture_vertex)), 4));
        if (!vertices) { ++pool_drops_; return false; }
        size_t out = 0;
        auto vertex = [&](float x, float y, float u, float v) {
            // Stretch only backdrop screen coordinates; texture coordinates and
            // the 3D/HUD projection stay untouched.
            const float draw_x = !foreground && video.gpu_background() && video.stretch_backdrop()
                ? x * float(video.width()) / rt::Video::W - video.wide_margin() : x;
            vertices[out++] = vita2d_texture_vertex{sx(draw_x), sy(y), 0.5f, u / 512.0f, v / 512.0f};
        };
        for (const Rect &r : rects) {
            const float u0 = float(r.x0 + r.h), u1 = float(r.x1 + r.h);
            const float v0 = float(r.v), v1 = float(r.v + (r.y1 - r.y0));
            vertex(float(r.x0), float(r.y0), u0, v0); vertex(float(r.x1), float(r.y0), u1, v0);
            vertex(float(r.x1), float(r.y1), u1, v1); vertex(float(r.x0), float(r.y0), u0, v0);
            vertex(float(r.x1), float(r.y1), u1, v1); vertex(float(r.x0), float(r.y1), u0, v1);
        }
        // libvita2d uses a 16-bit linear index buffer. A pathological window
        // mask can produce more than 65,536 vertices; keep complete triangles
        // within the index table without changing their painter order.
        constexpr size_t max_batch = 65532;
        for (size_t first = 0; first < count; first += max_batch) {
            const unsigned batch = unsigned(std::min(max_batch, count - first));
            vita2d_draw_array_textured(system24_textures_[size_t((foreground ? 4 : 0) + source_layer)],
                SCE_GXM_PRIMITIVE_TRIANGLES, vertices + first, batch, RGBA8(255,255,255,255));
        }
        system24_quads_ += unsigned(rects.size());
        return true;
    };

    for (int layer = 3; layer >= 0; --layer) {
        const uint16_t hreg = video.system24_word(0x5000u + unsigned(layer));
        const uint16_t vreg = video.system24_word(0x5004u + unsigned(layer));
        if (vreg & 0x8000) continue;
        const uint16_t ctrl = video.system24_word(0x5004u + unsigned(layer & 2));
        const int split_mode = (ctrl >> 13) & 3;
        const bool line_scroll = (hreg & 0x8000) != 0;
        const uint32_t line_base = 0x4000u + 0x200u * unsigned(layer);

        if (split_mode) {
            // In split modes the even layer combines itself with the following
            // odd layer; the odd draw call is suppressed by the System24 chip.
            if (layer & 1) continue;
            std::array<std::vector<Rect>, 2> split_rects;
            const int source_y = vreg & 511;
            auto add = [&](int source_layer, int x0, int x1, int y0, int y1, int scroll) {
                x0 = std::clamp(x0, 0, rt::Video::W);
                x1 = std::clamp(x1, 0, rt::Video::W);
                y0 = std::clamp(y0, 0, rt::Video::H);
                y1 = std::clamp(y1, 0, rt::Video::H);
                if (x0 < x1 && y0 < y1)
                    split_rects[size_t(source_layer - layer)].push_back({x0, x1, y0, y1, scroll, source_y + y0});
            };

            if (split_mode == 1) {
                const int neg_v = (-int(vreg)) & 0x3ff;
                const int cut = neg_v & 511;
                int first_layer = layer;
                if (!(neg_v & 0x200)) first_layer ^= 1;
                if (line_scroll) {
                    for (int y = 0; y < rt::Video::H;) {
                        const int raw = video.system24_word(line_base + unsigned(y));
                        const int scroll = (-raw) & 511;
                        const int source_layer = y >= cut ? first_layer ^ 1 : first_layer;
                        int y1 = y + 1;
                        while (y1 < rt::Video::H) {
                            const int next_raw = video.system24_word(line_base + unsigned(y1));
                            const int next_layer = y1 >= cut ? first_layer ^ 1 : first_layer;
                            if (((-next_raw) & 511) != scroll || next_layer != source_layer) break;
                            ++y1;
                        }
                        add(source_layer, 0, rt::Video::W, y, y1, scroll);
                        y = y1;
                    }
                } else {
                    const int scroll = (-int(hreg)) & 511;
                    add(first_layer, 0, rt::Video::W, 0, cut, scroll);
                    add(first_layer ^ 1, 0, rt::Video::W, cut, rt::Video::H, scroll);
                }
            } else {
                if (line_scroll) {
                    for (int y = 0; y < rt::Video::H; ++y) {
                        const int raw = video.system24_word(line_base + unsigned(y));
                        const int cut = raw & 511;
                        int first_layer = layer;
                        if (!(raw & 0x200)) first_layer ^= 1;
                        const int scroll = (-cut) & 511;
                        add(first_layer, 0, cut, y, y + 1, scroll);
                        add(first_layer ^ 1, cut, rt::Video::W, y, y + 1, scroll);
                    }
                } else {
                    const int cut = hreg & 511;
                    int first_layer = layer;
                    if (!(hreg & 0x200)) first_layer ^= 1;
                    const int scroll = (-cut) & 511;
                    add(first_layer, 0, cut, 0, rt::Video::H, scroll);
                    add(first_layer ^ 1, cut, rt::Video::W, 0, rt::Video::H, scroll);
                }
            }
            if (!submit(layer, split_rects[0]) || !submit(layer + 1, split_rects[1])) return false;
            continue;
        }

        const bool win = (layer & 1) != 0;
        const uint32_t mask_base = layer >= 2 ? 0x6800u : 0x6000u;
        std::vector<Rect> rects;
        for (int y = 0; y < rt::Video::H;) {
            const int h = line_scroll ? ((-int(video.system24_word(line_base + unsigned(y)))) & 511) :
                                        ((-int(hreg)) & 511);
            std::array<uint16_t, 4> masks{};
            for (int w = 0; w < 4; ++w) masks[size_t(w)] = video.system24_word(mask_base + unsigned(y * 4 + w));
            int y1 = y + 1;
            while (y1 < rt::Video::H) {
                const int next_h = line_scroll ? ((-int(video.system24_word(line_base + unsigned(y1)))) & 511) : h;
                bool same = next_h == h;
                for (int w = 0; same && w < 4; ++w)
                    same = video.system24_word(mask_base + unsigned(y1 * 4 + w)) == masks[size_t(w)];
                if (!same) break;
                ++y1;
            }
            int block = 0;
            while (block * 8 < rt::Video::W) {
                auto visible = [&](int b) {
                    const bool bit = (masks[size_t(b / 16)] & uint16_t(0x8000u >> (b & 15))) != 0;
                    return bit == win;
                };
                while (block * 8 < rt::Video::W && !visible(block)) ++block;
                const int first = block;
                while (block * 8 < rt::Video::W && visible(block)) ++block;
                if (first < block) rects.push_back({first * 8, std::min(block * 8, rt::Video::W),
                                                    y, y1, h, (vreg & 511) + y});
            }
            y = y1;
        }
        if (!submit(layer, rects)) return false;
    }
    return true;
}

uint32_t GpuFastRenderer::shade_texel(const rt::GeoPoly &poly, const rt::VideoMem &mem, uint8_t texel, uint8_t material_luma) const {
    const uint32_t lumabase = uint32_t(poly.texheader[1] & 0xff) << 7;
    const uint32_t color_index = ((poly.texheader[3] >> 6) & 0x3ff) + 0x1000;
    const uint32_t color = le16(mem.palram, color_index) & 0x7fff;
    uint8_t luma = uint8_t(uint32_t(mem.lumaram[(lumabase + (uint32_t(texel) << 3)) * 4]) * material_luma / 256);
    luma = std::min(luma, uint8_t(0x3f));
    const uint32_t cr = (((color >> 0) & 0x1f) << 8) + luma;
    const uint32_t cg = 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma;
    const uint32_t cb = 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma;
    const uint8_t r = gamma_[le16(mem.colorxlat, cr) & 0xff];
    const uint8_t g = gamma_[le16(mem.colorxlat, cg) & 0xff];
    const uint8_t b = gamma_[le16(mem.colorxlat, cb) & 0xff];
    return RGBA8(r, g, b, 255);
}

uint32_t GpuFastRenderer::solid_color(const rt::GeoPoly &poly, const rt::VideoMem &mem) const {
    const uint32_t color_index = ((poly.texheader[3] >> 6) & 0x3ff) + 0x1000;
    const uint32_t color = le16(mem.palram, color_index) & 0xffff;
    const uint8_t luma = poly.luma >> 2;
    const uint8_t r = gamma_[le16(mem.colorxlat, (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
    const uint8_t g = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
    const uint8_t b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
    return RGBA8(r, g, b, 255);
}

GpuFastRenderer::Source GpuFastRenderer::build_source(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    Source out;
    out.source_w = 32u << (poly.texheader[0] & 7);
    out.source_h = 32u << ((poly.texheader[0] >> 3) & 7);
    const uint32_t step_x = std::max(1u, (out.source_w + kTextureLimit - 1) / kTextureLimit);
    const uint32_t step_y = std::max(1u, (out.source_h + kTextureLimit - 1) / kTextureLimit);
    out.tex_w = std::max(1u, (out.source_w + step_x - 1) / step_x);
    out.tex_h = std::max(1u, (out.source_h + step_y - 1) / step_y);
    out.texture = make_texture(source_memory_, out.tex_w, out.tex_h, SCE_GXM_TEXTURE_FORMAT_P8_ABGR, 1);
    if (!out.texture) {
        cache_reset_pending_ = true;
        ++material_drops_;
        return out;
    }
    vita2d_texture_set_filters(out.texture, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    const bool mirror_x = (poly.texheader[0] >> 8) & 1;
    const bool mirror_y = (poly.texheader[0] >> 9) & 1;
    // The CPU always masks coordinates by the texture dimensions, even when
    // the header's wrap bits are clear. Those bits only change interpolation
    // across the last/first texel seam; they do not clamp outside coordinates.
    // GXM repeat preserves that addressing (the special seam filter remains
    // an approximation); edge-clamp turned repeated road regions solid black.
    sceGxmTextureSetUAddrMode(&out.texture->gxm_tex, mirror_x ? SCE_GXM_TEXTURE_ADDR_MIRROR :
        SCE_GXM_TEXTURE_ADDR_REPEAT);
    sceGxmTextureSetVAddrMode(&out.texture->gxm_tex, mirror_y ? SCE_GXM_TEXTURE_ADDR_MIRROR :
        SCE_GXM_TEXTURE_ADDR_REPEAT);

    auto *base = static_cast<uint8_t *>(vita2d_texture_get_datap(out.texture));
    const size_t stride_bytes = vita2d_texture_get_stride(out.texture);
    if (!base || stride_bytes < out.tex_w) {
        delete out.texture;
        out.texture = nullptr;
        return out;
    }
    const uint32_t bx = 32u * (poly.texheader[2] & 0x3f);
    const uint32_t by = 32u * ((poly.texheader[2] >> 6) & 0x1f);
    const uint32_t *sheet = (poly.texheader[2] & 0x1000) ? mem.tex1 : mem.tex0;
    for (uint32_t y = 0; y < out.tex_h; ++y) {
        uint8_t *row = base + size_t(y) * stride_bytes;
        const uint32_t sy0 = std::min(y * step_y, out.source_h - 1);
        for (uint32_t x = 0; x < out.tex_w; ++x) {
            const uint32_t sx0 = std::min(x * step_x, out.source_w - 1);
            const rt::TexelQuad q = rt::read_texel_quad(bx, by, sx0, sx0, sy0, sy0, sheet);
            row[x] = uint8_t((q.t00 >> 4) & 0x0f);
        }
    }
    out.bytes = stride_bytes * out.tex_h;
    return out;
}

GpuFastRenderer::Source *GpuFastRenderer::source_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    const SourceKey key = source_key(poly);
    auto found = sources_.find(key);
    if (found != sources_.end()) return &found->second;
    Source source = build_source(poly, mem);
    if (!source.texture) return nullptr;
    cached_bytes_ += source.bytes;
    auto inserted = sources_.emplace(key, std::move(source));
    return &inserted.first->second;
}

GpuFastRenderer::Material GpuFastRenderer::build_material(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    Material out;
    out.key = material_key(poly);
    out.source = source_for(poly, mem);
    if (!out.source) return out;
    constexpr std::size_t kPaletteBytes = 1024u;
    out.palette = static_cast<uint32_t *>(palette_memory_.allocate(kPaletteBytes, 64));
    if (!out.palette) {
        cache_reset_pending_ = true;
        ++material_drops_;
        return out;
    }
    auto *palette = out.palette;
    std::fill_n(palette, 256, 0u);
    const bool transparent = (poly.texheader[0] >> 13) & 1;
    for (uint8_t index = 0; index < 16; ++index)
        palette[index] = (transparent && index == 0x0f) ? 0u : shade_texel(poly, mem, index, out.key.luma);
    out.view = *out.source->texture;
    if (sceGxmTextureSetPalette(&out.view.gxm_tex, out.palette) < 0) {
        out.palette = nullptr;
        ++material_drops_;
        return out;
    }
    out.bytes = kPaletteBytes;
    cached_bytes_ += out.bytes;
    return out;
}

GpuFastRenderer::Material *GpuFastRenderer::material_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    const MaterialKey key = material_key(poly);
    ++stamp_;
    auto found = materials_.find(key);
    if (found != materials_.end()) { found->second.stamp = stamp_; return &found->second; }
    if (materials_.size() >= kMaterialLimit) {
        cache_reset_pending_ = true;
        ++material_drops_;
        return nullptr;
    }
    if (material_builds_ >= kMaterialBuildBudget) { ++material_defers_; return nullptr; }
    Material material = build_material(poly, mem);
    if (!material.palette || !material.source) return nullptr;
    ++material_builds_;
    material.stamp = stamp_;
    auto inserted = materials_.emplace(key, std::move(material));
    return &inserted.first->second;
}

void GpuFastRenderer::draw_polygons(rt::Video &video) {
    const auto &polys = video.gpu_polys();
    const rt::VideoMem &mem = video.gpu_mem();
    material_drops_ = material_builds_ = material_defers_ = subdivided_polys_ = 0;
    textured_draws_ = solid_draws_ = clip_changes_ = 0;
    textured_polys_ = solid_polys_ = 0;
    checker_polys_ = textured_checker_polys_ = 0;
    shader_setups_ = state_reuses_ = draw_errors_ = 0;
    submitted_vertices_ = 0;
    min_pool_free_ = vita2d_pool_free_space();
    const uint64_t sort_begin = sceKernelGetProcessTimeWide();
    const auto &order = order_.sort(polys);
    last_sort_us_ = sceKernelGetProcessTimeWide() - sort_begin;

    // CPU rasterizer is first-hit/front-to-back. Draw the reverse list with
    // alpha-tested textures to obtain the same basic visibility ordering.
    // Enable stencil clipping once. GPU05 enabled it again for every polygon,
    // which redundantly drew the old clip rectangle before setting the new one.
    vita2d_enable_clipping();
    int last_clip[4] = {-1, -1, -1, -1};
    // Combine adjacent triangles only: never reorder translucent polygons or
    // cross a clip/material boundary. Pool allocations remain alive until the
    // next frame's completion fence.
    enum class Batch { Empty, Textured, Solid, Checker };
    Batch batch = Batch::Empty, bound_shader = Batch::Empty;
    SceGxmContext *context = vita2d_get_context();
    const uint16_t *indices = vita2d_get_linear_indices();
    Material *batch_material = nullptr;
    uint32_t batch_checker_color = 0;
    vita2d_texture_vertex *batch_texture_vertices = nullptr;
    vita2d_color_vertex *batch_color_vertices = nullptr;
    size_t batch_count = 0;
    constexpr size_t max_batch = 65532;
    auto flush = [&] {
        if (batch == Batch::Empty) return;
        const vita2d_texture *view = batch == Batch::Textured ? &batch_material->view :
                                     batch == Batch::Checker ? checker_texture_ : nullptr;
        // Checker tint is per batch. Always bind it through the full API;
        // a following ordinary texture draw must restore the white tint.
        if (batch != Batch::Checker && bound_shader == batch && context && indices) {
            // The MVP and white tint do not change within this run. GXM keeps
            // their bindings across draws, so only update the texture/stream.
            // Invalidate after every vita2d clip draw and on a shader switch.
            int result = 0;
            if (batch == Batch::Textured) result = sceGxmSetFragmentTexture(context, 0, &view->gxm_tex);
            const void *vertices = batch == Batch::Textured ? static_cast<void *>(batch_texture_vertices) :
                                                              static_cast<void *>(batch_color_vertices);
            if (result >= 0) result = sceGxmSetVertexStream(context, 0, vertices);
            if (result >= 0) result = sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
                SCE_GXM_INDEX_FORMAT_U16, indices, unsigned(batch_count));
            if (result < 0) {
                ++draw_errors_;
                bound_shader = Batch::Empty;
                batch = Batch::Empty;
                batch_count = 0;
                return;
            }
            ++state_reuses_;
        } else {
            if (batch == Batch::Textured || batch == Batch::Checker)
                vita2d_draw_array_textured(view, SCE_GXM_PRIMITIVE_TRIANGLES,
                    batch_texture_vertices, batch_count,
                    batch == Batch::Checker ? batch_checker_color : RGBA8(255,255,255,255));
            else vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, batch_color_vertices, batch_count);
            bound_shader = batch;
            ++shader_setups_;
        }
        if (batch == Batch::Textured || batch == Batch::Checker) ++textured_draws_;
        else ++solid_draws_;
        batch = Batch::Empty;
        batch_count = 0;
    };
    for (auto oi = order.rbegin(); oi != order.rend(); ++oi) {
        const rt::GeoPoly &poly = polys[oi->index];
        if (poly.window > video.gpu_windows() || poly.num_vertices < 3 || poly.num_vertices > 8) continue;
        const int renderer = (poly.texheader[0] >> 13) & 3;
        const bool solid_checker = renderer == 0 && (poly.texheader[0] & 0x8000);
        const int margin = video.wide_margin();
        int hud_dx = 0;
        if (video.hud_at_edges_active() && poly.z <= rt::Raster::kHudOverlayZ) {
            rt::GeoPoly projected = poly;
            for (int i = 0; i < poly.num_vertices; ++i) {
                const float z = poly.v[i].p[0] + std::numeric_limits<float>::min();
                projected.v[i].x = float(video.crtc_x() + poly.center[0] + margin) + poly.v[i].x / z;
                projected.v[i].y = float(384 - poly.center[1] + video.crtc_y()) - poly.v[i].y / z;
            }
            hud_dx = video.raster().hud_polygon_offset(projected);
        }
        const int wide = poly.viewport[0] <= 0 && poly.viewport[2] >= 495 ? margin : 0;
        int clip_l = std::max<int>(poly.viewport[0] + video.render_x() - wide + hud_dx, -margin);
        int clip_r = std::min<int>(poly.viewport[2] + video.render_x() + wide + hud_dx, rt::Video::W + margin - 1);
        int clip_t = std::max<int>((384 - poly.viewport[3]) + video.render_y(), 0);
        int clip_b = std::min<int>((384 - poly.viewport[1]) + video.render_y(), rt::Video::H - 1);
        if (clip_l > clip_r || clip_t > clip_b) continue;
        // libvita2d's clip implementation consumes temporary-pool vertices
        // and does not null-check an exhausted pool. Reserve headroom for the
        // two clip rectangles plus this polygon's vertices/tint data.
        unsigned free_pool = vita2d_pool_free_space();
        min_pool_free_ = std::min(min_pool_free_, free_pool);
        if (free_pool < 2048) { ++pool_drops_; break; }
        const int gclip[4] = {int(sx(float(clip_l))), int(sy(float(clip_t))),
                              int(sx(float(clip_r + 1))), int(sy(float(clip_b + 1)))};
        if (gclip[0] != last_clip[0] || gclip[1] != last_clip[1] ||
            gclip[2] != last_clip[2] || gclip[3] != last_clip[3]) {
            flush();
            vita2d_set_clip_rectangle(gclip[0], gclip[1], gclip[2], gclip[3]);
            bound_shader = Batch::Empty;
            for (int c = 0; c < 4; ++c) last_clip[c] = gclip[c];
            ++clip_changes_;
        }

        PerspectivePoint p[8];
        bool valid = true;
        for (int i = 0; i < poly.num_vertices; ++i) {
            const float pz = poly.v[i].p[0] + std::numeric_limits<float>::min();
            if (!(pz > 0.0f) || !std::isfinite(pz)) { valid = false; break; }
            const float x = float(video.crtc_x() + poly.center[0]) + poly.v[i].x / pz;
            const float y = float((384 - poly.center[1]) + video.crtc_y()) - poly.v[i].y / pz;
            p[i].x = sx(x + float(hud_dx)); p[i].y = sy(y);
            p[i].u = solid_checker ? x * 0.5f : poly.v[i].p[1] / 8.0f;
            p[i].v = solid_checker ? y * 0.5f : poly.v[i].p[2] / 8.0f;
            p[i].q = 1.0f / pz;
            if (!std::isfinite(p[i].x) || !std::isfinite(p[i].y) ||
                !std::isfinite(p[i].u) || !std::isfinite(p[i].v) ||
                !std::isfinite(p[i].q)) { valid = false; break; }
        }
        if (!valid) continue;

        if (renderer & 2) {
            Material *m = material_for(poly, mem);
            if (!m) continue;
            const Source &source = *m->source;
            float min_q = p[0].q, max_q = p[0].q;
            float min_x = p[0].x, max_x = p[0].x, min_y = p[0].y, max_y = p[0].y;
            for (int j = 1; j < poly.num_vertices; ++j) {
                min_q = std::min(min_q, p[j].q); max_q = std::max(max_q, p[j].q);
                min_x = std::min(min_x, p[j].x); max_x = std::max(max_x, p[j].x);
                min_y = std::min(min_y, p[j].y); max_y = std::max(max_y, p[j].y);
            }
            const float q_ratio = min_q > 0.0f ? max_q / min_q : 1.0f;
            const float span = std::max(max_x - min_x, max_y - min_y);
            // Two-by-two perspective subdivision removes the road affine warp at
            // a quarter of GPU09 worst-case vertex multiplication.
            int subdiv = (q_ratio > 3.0f && span > 320.0f) ? 8 :
                         (q_ratio > 1.75f && span > 128.0f) ? 4 :
                         (q_ratio > 1.25f && span > 48.0f) ? 2 : 1;
            subdiv = texture_error_subdivision(p, poly.num_vertices, subdiv);
            size_t n = size_t(poly.num_vertices - 2) * 3u * size_t(subdiv * subdiv);
            if (batch != Batch::Textured || batch_material != m || batch_count + n > max_batch) flush();
            while (subdiv > 1 && vita2d_pool_free_space() < n * sizeof(vita2d_texture_vertex) + 2048u) {
                subdiv /= 2;
                n = size_t(poly.num_vertices - 2) * 3u * size_t(subdiv * subdiv);
            }
            if (subdiv > 1) ++subdivided_polys_;
            submitted_vertices_ += n;
            auto *verts = static_cast<vita2d_texture_vertex *>(vita2d_pool_memalign(unsigned(n * sizeof(vita2d_texture_vertex)), 4));
            if (!verts) continue;
            for (int fan = 1; fan + 1 < poly.num_vertices; ++fan) {
                auto *triangle = verts + size_t(fan - 1) * 3u * size_t(subdiv * subdiv);
                if (!emit_perspective_triangle(triangle, p[0], p[fan], p[fan + 1],
                        subdiv, float(source.source_w), float(source.source_h))) valid = false;
            }
            if (!valid) { submitted_vertices_ -= n; continue; }
            if (batch == Batch::Textured && batch_texture_vertices + batch_count != verts) flush();
            if (batch == Batch::Empty) {
                batch = Batch::Textured;
                batch_material = m;
                batch_texture_vertices = verts;
            }
            batch_count += n;
            ++textured_polys_;
            if (poly.texheader[0] & 0x8000) ++textured_checker_polys_;
        } else if (solid_checker) {
            const size_t n = size_t(poly.num_vertices - 2) * 3;
            const uint32_t color = solid_color(poly, mem);
            if (batch != Batch::Checker || batch_checker_color != color || batch_count + n > max_batch) flush();
            auto *verts = static_cast<vita2d_texture_vertex *>(vita2d_pool_memalign(
                unsigned(n * sizeof(vita2d_texture_vertex)), 4));
            if (!verts) { ++pool_drops_; continue; }
            size_t v = 0;
            for (int fan = 1; fan + 1 < poly.num_vertices; ++fan) {
                for (int j : {0, fan, fan + 1}) {
                    // Native screen coordinates keep the checker anchored to
                    // the display, not the polygon or its perspective depth.
                    verts[v++] = vita2d_texture_vertex{p[j].x, p[j].y, 0.5f, p[j].u, p[j].v};
                }
            }
            if (batch == Batch::Checker && batch_texture_vertices + batch_count != verts) flush();
            if (batch == Batch::Empty) {
                batch = Batch::Checker;
                batch_checker_color = color;
                batch_texture_vertices = verts;
            }
            batch_count += n;
            submitted_vertices_ += n;
            ++solid_polys_;
            ++checker_polys_;
        } else if (!(renderer & 1)) {
            const size_t n = size_t(poly.num_vertices - 2) * 3;
            if (batch != Batch::Solid || batch_count + n > max_batch) flush();
            submitted_vertices_ += n;
            auto *verts = static_cast<vita2d_color_vertex *>(vita2d_pool_memalign(unsigned(n * sizeof(vita2d_color_vertex)), 4));
            if (!verts) continue;
            const uint32_t c = solid_color(poly, mem);
            size_t v = 0;
            for (int i = 1; i + 1 < poly.num_vertices; ++i) {
                for (int j : {0, i, i + 1}) verts[v++] = vita2d_color_vertex{p[j].x, p[j].y, 0.5f, c};
            }
            if (batch == Batch::Solid && batch_color_vertices + batch_count != verts) flush();
            if (batch == Batch::Empty) {
                batch = Batch::Solid;
                batch_color_vertices = verts;
            }
            batch_count += n;
            ++solid_polys_;
        }
    }
    flush();
    min_pool_free_ = std::min(min_pool_free_, vita2d_pool_free_space());
    vita2d_disable_clipping();
}

void GpuFastRenderer::draw_exact(rt::Video &video) {
    layout(video);
    const uint64_t begin = sceKernelGetProcessTimeWide();
    upload_layer(background_, video.screen());
    draw_layer(background_, video);
    last_gpu_ms_ = double(sceKernelGetProcessTimeWide() - begin) / 1000.0;
}

void GpuFastRenderer::draw(rt::Video &video) {
    layout(video);
    const uint64_t begin = sceKernelGetProcessTimeWide();
    last_sort_us_ = last_polygon_us_ = last_tile_us_ = last_upload_us_ = 0;
    if (video.system24_gpu_compatible()) {
        update_system24_textures(video);
        const uint64_t uploaded = sceKernelGetProcessTimeWide();
        draw_system24(video, false);
        const uint64_t background = sceKernelGetProcessTimeWide();
        draw_polygons(video);
        const uint64_t polygons = sceKernelGetProcessTimeWide();
        draw_system24(video, true);
        const uint64_t foreground = sceKernelGetProcessTimeWide();
        last_upload_us_ = uploaded - begin;
        last_polygon_us_ = polygons - background;
        last_tile_us_ = (background - uploaded) + (foreground - polygons);
    } else if (video.gpu_background()) {
        update_system24_textures(video);
        if (!video.gpu_foreground() && foreground_generation_ != video.foreground_generation()) {
            upload_layer(foreground_, video.foreground_layer());
            foreground_generation_ = video.foreground_generation();
        }
        const uint64_t uploaded = sceKernelGetProcessTimeWide();
        draw_system24(video, false);
        const uint64_t background = sceKernelGetProcessTimeWide();
        draw_polygons(video);
        const uint64_t polygons = sceKernelGetProcessTimeWide();
        if (video.gpu_foreground()) draw_system24(video, true);
        else draw_layer(foreground_, video);
        const uint64_t foreground = sceKernelGetProcessTimeWide();
        last_upload_us_ = uploaded - begin;
        last_polygon_us_ = polygons - background;
        last_tile_us_ = (background - uploaded) + (foreground - polygons);
    } else {
        if (background_generation_ != video.background_generation()) {
            upload_layer(background_, video.background_layer());
            background_generation_ = video.background_generation();
        }
        if (foreground_generation_ != video.foreground_generation()) {
            upload_layer(foreground_, video.foreground_layer());
            foreground_generation_ = video.foreground_generation();
        }
        if (video.wide_margin() && video.background_layer().size() == size_t(rt::Video::W) * rt::Video::H) {
            const float left = sx(-float(video.wide_margin()));
            const float width = float(video.width()) * scale_;
            if (video.stretch_backdrop()) {
                vita2d_draw_texture_part_scale(background_, left, sy(0), 0, 0,
                    float(rt::Video::W), float(rt::Video::H), width / float(rt::Video::W), scale_);
            } else {
                vita2d_draw_rectangle(left, sy(0), width, float(rt::Video::H) * scale_,
                                     swap_rb(video.background_layer()[0]));
                vita2d_draw_texture_part_scale(background_, sx(0), sy(0), 0, 0,
                    float(rt::Video::W), float(rt::Video::H), scale_, scale_);
            }
        } else draw_layer(background_, video);
        draw_polygons(video);
        draw_layer(foreground_, video);
    }
    last_gpu_ms_ = double(sceKernelGetProcessTimeWide() - begin) / 1000.0;
}

} // namespace vita
