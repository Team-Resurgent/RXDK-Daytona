#pragma once

#include "runtime/video.h"
#include "gpu_memory.h"
#include "polygon_order.h"
#include <vita2d.h>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vita {

class GpuFastRenderer {
public:
    GpuFastRenderer();
    ~GpuFastRenderer();
    GpuFastRenderer(const GpuFastRenderer &) = delete;
    GpuFastRenderer &operator=(const GpuFastRenderer &) = delete;

    bool ok() const { return background_ && foreground_ && checker_texture_ && system24_ok_ && source_memory_.ok() && palette_memory_.ok(); }
    void reset_materials();
    // Call after CPU simulation and before vita2d_start_drawing (also for menus).
    void prepare_frame();
    void shutdown();
    void draw(rt::Video &video);
    void draw_exact(rt::Video &video);
    double last_gpu_ms() const { return last_gpu_ms_; }
    uint64_t last_sort_us() const { return last_sort_us_; }
    uint64_t last_polygon_us() const { return last_polygon_us_; }
    uint64_t last_tile_us() const { return last_tile_us_; }
    uint64_t last_upload_us() const { return last_upload_us_; }
    std::size_t cached_bytes() const { return cached_bytes_; }
    std::size_t cached_materials() const { return materials_.size(); }
    std::size_t cached_sources() const { return sources_.size(); }
    std::size_t reserved_bytes() const { return layer_memory_.capacity() + source_memory_.capacity() + palette_memory_.capacity(); }
    unsigned cache_resets() const { return cache_resets_; }
    unsigned pool_drops() const { return pool_drops_; }
    unsigned material_drops() const { return material_drops_; }
    unsigned material_builds() const { return material_builds_; }
    unsigned material_defers() const { return material_defers_; }
    unsigned subdivided_polys() const { return subdivided_polys_; }
    std::size_t submitted_vertices() const { return submitted_vertices_; }
    unsigned min_pool_free() const { return min_pool_free_; }
    unsigned system24_quads() const { return system24_quads_; }
    unsigned system24_uploaded_tiles() const { return system24_uploaded_tiles_; }
    unsigned textured_draws() const { return textured_draws_; }
    unsigned solid_draws() const { return solid_draws_; }
    unsigned textured_polys() const { return textured_polys_; }
    unsigned solid_polys() const { return solid_polys_; }
    unsigned checker_polys() const { return checker_polys_; }
    unsigned textured_checker_polys() const { return textured_checker_polys_; }
    unsigned shader_setups() const { return shader_setups_; }
    unsigned state_reuses() const { return state_reuses_; }
    unsigned draw_errors() const { return draw_errors_; }
    unsigned clip_changes() const { return clip_changes_; }

private:
    struct MaterialKey {
        uint16_t h0 = 0, h1 = 0, h2 = 0, h3 = 0;
        uint8_t luma = 0;
        bool operator==(const MaterialKey &o) const {
            return h0 == o.h0 && h1 == o.h1 && h2 == o.h2 && h3 == o.h3 &&
                   luma == o.luma;
        }
    };
    struct MaterialKeyHash {
        std::size_t operator()(const MaterialKey &k) const {
            uint64_t x = uint64_t(k.h0) | (uint64_t(k.h1) << 16) |
                         (uint64_t(k.h2) << 32) | (uint64_t(k.h3) << 48);
            x ^= uint64_t(k.luma);
            x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 27; x *= 0x94d049bb133111ebULL;
            return std::size_t(x ^ (x >> 31));
        }
    };
    struct SourceKey {
        uint16_t h0 = 0, h2 = 0;
        bool operator==(const SourceKey &o) const { return h0 == o.h0 && h2 == o.h2; }
    };
    struct SourceKeyHash {
        std::size_t operator()(const SourceKey &k) const {
            uint32_t x = uint32_t(k.h0) | (uint32_t(k.h2) << 16);
            x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu;
            return std::size_t(x ^ (x >> 16));
        }
    };
    struct Source {
        vita2d_texture *texture = nullptr;
        uint32_t source_w = 1, source_h = 1;
        uint32_t tex_w = 1, tex_h = 1;
        std::size_t bytes = 0;
    };
    struct Material {
        MaterialKey key;
        Source *source = nullptr;
        uint32_t *palette = nullptr;
        vita2d_texture view{}; // Immutable, non-owning source plus palette binding.
        std::size_t bytes = 0;
        uint64_t stamp = 0;
    };

    // Three bounded allocations replace hundreds of tiny, 256 KiB-rounded blocks.
    GpuMemoryArena layer_memory_{12u * 1024u * 1024u, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW};
    GpuMemoryArena source_memory_{16u * 1024u * 1024u, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW};
    GpuMemoryArena palette_memory_{4u * 1024u * 1024u, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE};
    vita2d_texture *background_ = nullptr;
    vita2d_texture *foreground_ = nullptr;
    vita2d_texture *checker_texture_ = nullptr;
    std::array<vita2d_texture *, 8> system24_textures_{};
    unsigned system24_uploaded_tiles_ = 0;
    std::unordered_map<SourceKey, Source, SourceKeyHash> sources_;
    std::unordered_map<MaterialKey, Material, MaterialKeyHash> materials_;
    unsigned cache_resets_ = 0;
    bool cache_reset_pending_ = false;
    PolygonOrder order_;
    std::size_t cached_bytes_ = 0;
    uint64_t stamp_ = 0;
    double last_gpu_ms_ = 0.0;
    uint64_t last_sort_us_ = 0, last_polygon_us_ = 0, last_tile_us_ = 0, last_upload_us_ = 0;
    unsigned pool_drops_ = 0, material_drops_ = 0, material_builds_ = 0,
             material_defers_ = 0, subdivided_polys_ = 0, min_pool_free_ = 0, system24_quads_ = 0,
             textured_draws_ = 0, solid_draws_ = 0, clip_changes_ = 0;
    unsigned textured_polys_ = 0, solid_polys_ = 0;
    unsigned checker_polys_ = 0, textured_checker_polys_ = 0;
    unsigned shader_setups_ = 0, state_reuses_ = 0, draw_errors_ = 0;
    std::size_t submitted_vertices_ = 0;
    bool shutdown_ = false, system24_ok_ = true;
    uint64_t background_generation_ = UINT64_MAX, foreground_generation_ = UINT64_MAX,
             system24_generation_ = UINT64_MAX;
    uint8_t gamma_[256]{};

    float scale_ = 544.0f / 384.0f, offset_x_ = (960.0f - 496.0f * scale_) / 2, offset_y_ = 0;
    float sx(float x) const { return offset_x_ + x * scale_; }
    float sy(float y) const { return offset_y_ + y * scale_; }
    void layout(const rt::Video& video) {
        scale_ = std::min(960.0f / video.width(), 544.0f / 384.0f);
        offset_x_ = (960.0f - 496.0f * scale_) / 2;
        offset_y_ = (544.0f - 384.0f * scale_) / 2;
    }
    void draw_layer(vita2d_texture* texture, const rt::Video& video) {
        vita2d_draw_texture_part_scale(texture, sx(float(-video.wide_margin())), sy(0),
            0, 0, float(video.width()), 384, scale_, scale_);
    }
    static constexpr std::size_t kMaterialLimit = 4096;
    // Texture conversion allocates and shades on the CPU. Spread cold-cache
    // work across frames so a new scene cannot stall for 100+ ms at once.
    static constexpr unsigned kMaterialBuildBudget = 32;
    static constexpr uint32_t kTextureLimit = 512;

    static uint16_t le16(const uint8_t *base, uint32_t index);
    static uint32_t swap_rb(uint32_t argb);
    static uint16_t rgba5551(uint32_t rgba);
    static MaterialKey material_key(const rt::GeoPoly &poly);
    static SourceKey source_key(const rt::GeoPoly &poly);
    void upload_layer(vita2d_texture *texture, const std::vector<uint32_t> &pixels);
    void update_system24_textures(const rt::Video &video);
    bool draw_system24(const rt::Video &video, bool foreground);
    Material *material_for(const rt::GeoPoly &poly, const rt::VideoMem &mem);
    Source *source_for(const rt::GeoPoly &poly, const rt::VideoMem &mem);
    Source build_source(const rt::GeoPoly &poly, const rt::VideoMem &mem);
    Material build_material(const rt::GeoPoly &poly, const rt::VideoMem &mem);
    vita2d_texture *make_texture(GpuMemoryArena &arena, uint32_t w, uint32_t h, SceGxmTextureFormat format, uint32_t bytes_per_pixel);
    void clear_cache();
    uint32_t shade_texel(const rt::GeoPoly &poly, const rt::VideoMem &mem, uint8_t texel, uint8_t material_luma) const;
    uint32_t solid_color(const rt::GeoPoly &poly, const rt::VideoMem &mem) const;
    void draw_polygons(rt::Video &video);
};

} // namespace vita
