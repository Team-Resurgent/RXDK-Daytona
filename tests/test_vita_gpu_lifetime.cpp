// Exercises the actual renderer with host allocation and completion-fence mocks.
// Passing proves CPU lifetime/contracts only, not GXM rendering or device speed.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "runtime/video.h"
#define private public
#include "../platform/vita/gpu_fast.h"
#undef private
#include "../platform/vita/gpu_fast.cpp"

namespace mock {
void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "Contract failure: %s\n", message); throw std::runtime_error(message); }
}
struct Block { void *base; size_t bytes; size_t alignment; bool mapped; };
struct Reader { const uint8_t *base; std::vector<uint8_t> original; };
std::unordered_map<int, Block> blocks;
std::vector<Reader> readers;
std::vector<uint8_t> pool(8u * 1024u * 1024u);
size_t pool_used = 0;
uint64_t textured_vertices = 0;
unsigned allocations = 0, maps = 0, unmaps = 0, frees = 0, waits = 0, draws = 0;
int next_uid = 1;
bool scene = false;
void reset_graphics_state();
std::vector<float> ordered_x;
struct LayerDraw { float x, y, w, h, xs, ys; };
std::vector<LayerDraw> layers;
std::vector<const void *> ordered_palettes;
#include "vita_gpu_capture.inc"

bool live(const void *pointer, size_t bytes) {
    const auto p = reinterpret_cast<uintptr_t>(pointer);
    for (const auto &[uid, block] : blocks) {
        (void)uid;
        const auto start = reinterpret_cast<uintptr_t>(block.base);
        if (block.mapped && p >= start && p - start <= block.bytes && bytes <= block.bytes - (p - start)) return true;
    }
    const auto start = reinterpret_cast<uintptr_t>(pool.data());
    return p >= start && p - start <= pool_used && bytes <= pool_used - (p - start);
}
void queue(const void *pointer, size_t bytes) {
    require(scene, "GPU work submitted outside a scene");
    require(live(pointer, bytes), "GPU submitted unmapped or out-of-bounds memory");
    const auto *p = static_cast<const uint8_t *>(pointer);
    readers.push_back({p, std::vector<uint8_t>(p, p + bytes)});
}
void start_scene() {
    require(!scene && readers.empty(), "temporary pool reused before GPU completion");
    pool_used = 0;
    reset_graphics_state();
    ordered_x.clear(); ordered_palettes.clear(); layers.clear();
    captured_draws.clear(); clip_rectangle = {0, 0, 960, 544};
    scene = true;
}
void end_scene() { require(scene, "no scene to end"); scene = false; }
size_t reserved() {
    size_t bytes = 0;
    for (const auto &[uid, block] : blocks) { (void)uid; bytes += block.bytes; }
    return bytes;
}
} // namespace mock

SceUID sceKernelAllocMemBlock(const char *, SceKernelMemBlockType type, SceSize bytes, void *) {
    const size_t alignment = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256u * 1024u : 4096u;
    mock::require(bytes && bytes % alignment == 0, "incorrect kernel block granularity");
    void *base = ::operator new(bytes, std::align_val_t(alignment), std::nothrow);
    if (!base) return -1;
    std::memset(base, 0, bytes);
    const int uid = mock::next_uid++;
    mock::blocks.emplace(uid, mock::Block{base, bytes, alignment, false});
    ++mock::allocations;
    return uid;
}
int sceKernelGetMemBlockBase(SceUID uid, void **out) {
    const auto found = mock::blocks.find(uid);
    if (found == mock::blocks.end()) return -1;
    *out = found->second.base;
    return 0;
}
int sceKernelFreeMemBlock(SceUID uid) {
    auto found = mock::blocks.find(uid);
    mock::require(found != mock::blocks.end(), "double free or invalid kernel block");
    mock::require(!found->second.mapped, "kernel block freed before GXM unmap");
    mock::require(mock::readers.empty() && !mock::scene, "kernel block freed with pending GPU work");
    ::operator delete(found->second.base, std::align_val_t(found->second.alignment));
    mock::blocks.erase(found);
    ++mock::frees;
    return 0;
}
int sceGxmMapMemory(void *base, SceSize bytes, int) {
    for (auto &[uid, block] : mock::blocks) {
        (void)uid;
        if (block.base == base) {
            mock::require(bytes == block.bytes && !block.mapped, "invalid GXM mapping");
            block.mapped = true;
            ++mock::maps;
            return 0;
        }
    }
    return -1;
}
int sceGxmUnmapMemory(void *base) {
    mock::require(mock::readers.empty() && !mock::scene, "GPU memory unmapped before completion");
    for (auto &[uid, block] : mock::blocks) {
        (void)uid;
        if (block.base == base && block.mapped) { block.mapped = false; ++mock::unmaps; return 0; }
    }
    return -1;
}
int sceGxmTextureInitLinear(SceGxmTexture *texture, void *base, SceGxmTextureFormat format,
                            uint32_t width, uint32_t height, uint32_t) {
    const uint32_t stride = ((width + 7u) & ~7u) * (format == SCE_GXM_TEXTURE_FORMAT_P8_ABGR ? 1u : 4u);
    mock::require((reinterpret_cast<uintptr_t>(base) & 15u) == 0, "texture alignment");
    mock::require(mock::live(base, size_t(stride) * height), "texture outside mapping");
    *texture = {base, nullptr, width, height, stride, format};
    return 0;
}
int sceGxmTextureSetUAddrMode(SceGxmTexture *t, int mode) { t->u_mode = mode; return 0; }
int sceGxmTextureSetVAddrMode(SceGxmTexture *t, int mode) { t->v_mode = mode; return 0; }
int sceGxmTextureSetPalette(SceGxmTexture *texture, void *palette) {
    mock::require((reinterpret_cast<uintptr_t>(palette) & 63u) == 0, "palette alignment");
    mock::require(mock::live(palette, 1024), "palette outside mapping");
    texture->palette = static_cast<uint32_t *>(palette);
    return 0;
}
uint64_t sceKernelGetProcessTimeWide() { static uint64_t value = 0; return ++value; }
void vita2d_wait_rendering_done() {
    mock::require(!mock::scene, "completion fence called inside an active scene");
    for (const auto &reader : mock::readers) {
        mock::require(mock::live(reader.base, reader.original.size()), "GPU reader's mapping was destroyed");
        mock::require(std::memcmp(reader.base, reader.original.data(), reader.original.size()) == 0,
                      "GPU reader's data overwritten before completion");
    }
    mock::readers.clear();
    ++mock::waits;
}
void vita2d_texture_set_filters(vita2d_texture *t, int min, int mag) { t->gxm_tex.min_filter = min; t->gxm_tex.mag_filter = mag; }
void *vita2d_texture_get_datap(const vita2d_texture *t) { return t->gxm_tex.data; }
unsigned vita2d_texture_get_stride(const vita2d_texture *t) { return t->gxm_tex.stride; }
unsigned vita2d_pool_free_space() { return unsigned(mock::pool.size() - mock::pool_used); }
void *vita2d_pool_memalign(unsigned bytes, unsigned alignment) {
    const auto start = reinterpret_cast<uintptr_t>(mock::pool.data()) + mock::pool_used;
    const size_t padding = (alignment - (start & (alignment - 1))) & (alignment - 1);
    if (padding + bytes > vita2d_pool_free_space()) return nullptr;
    mock::pool_used += padding;
    void *out = mock::pool.data() + mock::pool_used;
    mock::pool_used += bytes;
    return out;
}
void vita2d_draw_rectangle(float, float, float, float, uint32_t) {}
void mock_draw_array_textured(const vita2d_texture *texture, int,
                                const vita2d_texture_vertex *vertices, unsigned count, uint32_t tint) {
    mock::require(count <= 65532 && count % 3 == 0, "draw exceeds the 16-bit libvita2d triangle index table");
    mock::textured_vertices += count;
    if (mock::capture_draws) mock::captured_draws.push_back({true, texture->gxm_tex,
        {vertices, vertices + count}, {}, tint, mock::clip_rectangle});
    for (unsigned i = 0; i < count; ++i) {
        mock::ordered_x.push_back(vertices[i].x);
        mock::ordered_palettes.push_back(texture->gxm_tex.palette);
    }
    for (unsigned i = 0; i < count; ++i)
        mock::require(std::isfinite(vertices[i].x) && std::isfinite(vertices[i].y) &&
                      std::isfinite(vertices[i].z) && std::isfinite(vertices[i].u) &&
                      std::isfinite(vertices[i].v), "nonfinite textured vertex submitted");
    mock::queue(vertices, count * sizeof(*vertices));
    mock::queue(texture->gxm_tex.data, size_t(texture->gxm_tex.stride) * texture->gxm_tex.height);
    if (texture->gxm_tex.format == SCE_GXM_TEXTURE_FORMAT_P8_ABGR) mock::queue(texture->gxm_tex.palette, 1024);
    ++mock::draws;
}
void mock_draw_array(int, const vita2d_color_vertex *vertices, unsigned count) {
    if (mock::capture_draws) mock::captured_draws.push_back({false, {}, {},
        {vertices, vertices + count}, 0, mock::clip_rectangle});
    for (unsigned i = 0; i < count; ++i) {
        mock::ordered_x.push_back(vertices[i].x);
        mock::ordered_palettes.push_back(nullptr);
    }
    for (unsigned i = 0; i < count; ++i)
        mock::require(std::isfinite(vertices[i].x) && std::isfinite(vertices[i].y) &&
                      std::isfinite(vertices[i].z), "nonfinite solid vertex submitted");
    mock::queue(vertices, count * sizeof(*vertices));
    ++mock::draws;
}
void vita2d_draw_texture_part_scale(const vita2d_texture* t, float x, float y, float tx, float ty,
                                   float w, float h, float xs, float ys) {
    mock::require(tx >= 0 && ty >= 0 && tx + w <= t->gxm_tex.width && ty + h <= t->gxm_tex.height,
                  "layer source rectangle exceeds texture");
    mock::layers.push_back({x, y, w, h, xs, ys});
    vita2d_draw_texture_scale(t, x, y, xs, ys);
}
void vita2d_draw_texture_scale(const vita2d_texture *t, float, float, float, float) {
    mock::queue(t->gxm_tex.data, size_t(t->gxm_tex.stride) * t->gxm_tex.height);
}
void vita2d_enable_clipping() {}
void vita2d_disable_clipping() {}
void vita2d_set_clip_rectangle(int l, int t, int r, int b) {
    mock::reset_graphics_state(); mock::clip_rectangle = {l, t, r, b};
}
vita2d_texture *vita2d_create_empty_texture_format(unsigned, unsigned, SceGxmTextureFormat) {
    throw std::runtime_error("legacy libvita2d texture allocator called");
}
void vita2d_free_texture(vita2d_texture *) { throw std::runtime_error("arena descriptor passed to libvita2d free"); }

#include "vita_gpu_state_mock.inc"

namespace {
struct Images {
    std::vector<uint8_t> palette = std::vector<uint8_t>(0x4000, 0xff);
    std::vector<uint8_t> xlat = std::vector<uint8_t>(0xc000, 0xff);
    std::vector<uint8_t> luma = std::vector<uint8_t>(0x20000, 0x7f);
    std::vector<uint32_t> texture = std::vector<uint32_t>(0x80000, 0x76543210);
    rt::VideoMem mem{palette.data(), xlat.data(), luma.data(), texture.data(), texture.data()};
};
rt::GeoPoly polygon() {
    rt::GeoPoly p;
    p.texheader[0] = 0x4000;
    p.luma = 0x70;
    p.viewport[0] = 0; p.viewport[1] = 0; p.viewport[2] = 495; p.viewport[3] = 383;
    p.center[0] = 248; p.center[1] = 192;
    p.num_vertices = 3;
    p.v[0] = {-50.0f, -50.0f, {10.0f, 0.0f, 0.0f}};
    p.v[1] = {50.0f, -50.0f, {10.0f, 256.0f, 0.0f}};
    p.v[2] = {0.0f, 50.0f, {10.0f, 128.0f, 256.0f}};
    return p;
}
vita::GpuFastRenderer::Material *build(vita::GpuFastRenderer &renderer, const rt::GeoPoly &p, const rt::VideoMem &mem) {
    // Material budget is per presented frame. This isolated cache test advances
    // that counter without inventing GXM scenes for unsubmitted materials.
    renderer.material_builds_ = 0;
    return renderer.material_for(p, mem);
}
void test_cache(vita::GpuFastRenderer &renderer, const Images &images) {
    constexpr size_t mib = 1024u * 1024u;
    mock::require(renderer.ok(), "renderer initialization failed");
    mock::require(renderer.reserved_bytes() == 32u * mib && mock::reserved() == 32u * mib,
                  "renderer reservation must be the real 32 MiB arena capacity");
    mock::require(mock::allocations == 3 && mock::maps == 3, "renderer should use exactly three kernel blocks");
    for (unsigned i = 0; i < 800; ++i) {
        auto p = polygon();
        p.texheader[2] = uint16_t(i % 40);
        p.texheader[1] = uint16_t(i / 40);
        mock::require(build(renderer, p, images.mem) != nullptr, "800-material scene failed to cache");
    }
    mock::require(renderer.cached_materials() == 800 && renderer.cached_sources() == 40, "shared source cache mismatch");
    mock::require(renderer.cached_bytes() == (800u + 40u) * 1024u, "logical source/palette bytes mismatch");
    mock::require(mock::allocations == 3 && mock::reserved() == 32u * mib, "per-material physical allocation growth");
    auto *m = build(renderer, polygon(), images.mem);
    const unsigned before_wait = mock::waits;
    mock::start_scene();
    mock::queue(m->palette, 1024);
    mock::queue(m->source->texture->gxm_tex.data, m->source->bytes);
    mock::end_scene();
    renderer.prepare_frame();
    mock::require(mock::waits == before_wait + 1 && renderer.cached_materials() == 800,
                  "prepare_frame must fence even without a cache reset");
    renderer.reset_materials();

    // Sixty-four 512x512 P8 sources fill exactly the 16 MiB source arena.
    auto big = polygon(); big.texheader[0] |= 4u | (4u << 3);
    for (unsigned i = 0; i < 64; ++i) {
        big.texheader[2] = uint16_t(i);
        mock::require(build(renderer, big, images.mem) != nullptr, "source arena exhausted too early");
    }
    big.texheader[2] = 0;
    m = build(renderer, big, images.mem);
    const auto *old_data = static_cast<const uint8_t *>(m->source->texture->gxm_tex.data);
    const unsigned overflow_waits = mock::waits;
    mock::start_scene();
    mock::queue(old_data, m->source->bytes);
    mock::queue(m->palette, 1024);
    big.texheader[2] = 64;
    mock::require(build(renderer, big, images.mem) == nullptr, "source arena overflow did not refuse allocation");
    mock::require(renderer.cache_reset_pending_ && renderer.cached_sources() == 64 &&
                  mock::waits == overflow_waits && mock::frees == 0, "cache reclaimed during an active scene");
    mock::end_scene();
    renderer.prepare_frame();
    mock::require(renderer.cache_resets() == 1 && renderer.cached_sources() == 0 && renderer.cached_materials() == 0,
                  "deferred cache reset did not run after the fence");
    mock::require(renderer.source_memory_.used() == 0 && renderer.palette_memory_.used() == 0 && mock::allocations == 3,
                  "cache reset should rewind arenas without kernel allocation");
    m = build(renderer, polygon(), images.mem);
    mock::require(m && m->source->texture->gxm_tex.data == old_data, "source arena not safely reused after completion");
    renderer.reset_materials();

    // All 4096 material palettes fit. A 4097th request waits until a later frame.
    for (unsigned i = 0; i < 4096; ++i) {
        auto p = polygon(); p.texheader[1] = uint16_t(i & 255u); p.texheader[3] = uint16_t((i >> 8) << 6);
        mock::require(build(renderer, p, images.mem) != nullptr, "palette arena exhausted before material limit");
    }
    auto extra = polygon(); extra.texheader[3] = 16u << 6;
    mock::require(build(renderer, extra, images.mem) == nullptr && renderer.cache_reset_pending_, "material limit overflow not deferred");
    renderer.prepare_frame();
    mock::require(renderer.cache_resets() == 2 && mock::allocations == 3, "material overflow caused allocation churn");
}

void test_vertices(vita::GpuFastRenderer &renderer, const Images &images) {
    std::vector<uint8_t> tile_ram(0x10000), char_ram(0x80000);
    rt::Video video(tile_ram.data(), char_ram.data());
    video.set_external_3d(true);
    video.enable_write_tracking();
    std::vector<rt::GeoPoly> polys{polygon()};
    auto run = [&] {
        video.screen_update(polys, 0, images.mem);
        renderer.prepare_frame();
        mock::start_scene();
        const unsigned draws = mock::draws;
        renderer.draw_polygons(video);
        mock::end_scene();
        renderer.prepare_frame();
        return mock::draws - draws;
    };
    mock::require(run() == 1, "valid textured triangle did not submit");
    for (unsigned mode = 0; mode < 10; ++mode) {
        polys[0] = polygon();
        const float inf = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        switch (mode) {
        case 0: polys[0].num_vertices = 9; break;
        case 1: polys[0].v[0].x = nan; break;
        case 2: polys[0].v[0].y = inf; break;
        case 3: polys[0].v[0].p[0] = -1.0f; break;
        case 4: polys[0].v[0].p[0] = 0.0f; break;
        case 5: polys[0].v[0].p[0] = inf; break;
        case 6: polys[0].v[0].p[1] = nan; break;
        case 7: polys[0].v[0].p[2] = inf; break;
        case 8: polys[0].v[0].x = std::numeric_limits<float>::max(); polys[0].v[0].p[0] = 0.01f; break;
        case 9:
            for (auto &v : polys[0].v) v.p[0] = 1.0e-20f;
            polys[0].v[0].p[1] = std::numeric_limits<float>::max();
            break;
        }
        mock::require(run() == 0, "invalid polygon reached a GPU draw");
    }
    polys[0] = polygon();
    for (int margin : {0, 59, 93, 200, 0}) {
        video.set_wide_margin(margin);
        video.set_hud_edges(true);
        video.frame_start();
        video.screen_update(polys, 0, images.mem);
        mock::require(video.width() == 496 + 2 * margin, "external wide width");
        if (margin) {
            mock::require(video.background_layer().size() == 496u * 384u, "wide backdrop must stay native-sized");
            const auto generation = video.foreground_generation();
            video.screen_update(polys, 0, images.mem);
            mock::require(video.foreground_generation() == generation, "unchanged wide HUD uploaded again");
        }
        renderer.prepare_frame();
        mock::start_scene();
        renderer.draw(video);
        mock::require(std::abs(renderer.sx(-float(margin)) -
            (960.0f - video.width() * renderer.scale_) / 2) < 0.01f, "wide left edge layout");
        mock::require(renderer.sy(0) >= 0 && renderer.sy(384) <= 544.01f, "wide vertical letterbox");
        mock::end_scene();
        renderer.prepare_frame();
        if (margin) {
            video.set_stretch_backdrop(true);
            mock::start_scene(); renderer.draw(video);
            mock::require(mock::layers.size() == 2, "wide background and foreground draw count");
            const auto back = mock::layers[0];
            mock::require(back.w == 496 && back.h == 384, "stretch must sample native backdrop");
            mock::require(std::abs(back.w * back.xs - video.width() * renderer.scale_) < 0.01f,
                          "stretch must fill wide viewport");
            mock::require(std::abs(back.x - renderer.sx(-float(margin))) < 0.01f,
                          "stretch left edge");
            mock::end_scene(); renderer.prepare_frame();
            video.set_stretch_backdrop(false);
            const auto old_front = video.foreground_layer();
            const auto background_generation = video.background_generation();
            video.set_gpu_background(true);
            tile_ram[0xa000] ^= 1; // scrolling dirties both CPU tile passes
            video.tile_memory_w(); // same write notification as the board bus
            video.frame_start(); video.screen_update(polys, 0, images.mem);
            mock::require(!video.last_profile().layers_rebuilt, "GPU tiles rebuilt a CPU bitmap on scroll");
            mock::require(video.foreground_layer() == old_front, "GPU backdrop changed HUD pixels");
            mock::require(video.background_generation() == background_generation, "GPU backdrop rebuilt CPU background");
            renderer.prepare_frame(); mock::start_scene(); renderer.draw(video);
            mock::require(mock::layers.empty(), "GPU tile path still draws a CPU layer without relocated HUD");
            mock::end_scene(); renderer.prepare_frame();
            mock::capture_draws = true;
            mock::start_scene(); renderer.draw_system24(video, false);
            const auto centred = mock::captured_draws;
            mock::require(!centred.empty(), "no GPU backdrop tiles");
            mock::end_scene(); renderer.prepare_frame();
            video.set_stretch_backdrop(true);
            mock::start_scene(); renderer.draw_system24(video, false);
            mock::require(centred.size() == mock::captured_draws.size(), "stretch changed tile draws");
            for (size_t d = 0; d < centred.size(); ++d) {
                const auto &a = centred[d].vertices;
                const auto &b = mock::captured_draws[d].vertices;
                mock::require(a.size() == b.size(), "stretch changed tile topology");
                for (size_t i = 0; i < a.size(); ++i) {
                    const float source_x = (a[i].x - renderer.sx(0)) / renderer.scale_;
                    const float expected = renderer.sx(source_x * video.width() / 496.f - margin);
                    mock::require(std::abs(b[i].x - expected) < .001f, "GPU backdrop stretch coordinate");
                    mock::require(a[i].y == b[i].y && a[i].u == b[i].u && a[i].v == b[i].v,
                                  "GPU backdrop changed UV or vertical coordinate");
                }
            }
            mock::end_scene(); renderer.prepare_frame(); mock::capture_draws = false;
            video.set_stretch_backdrop(false); video.set_gpu_background(false);
            video.frame_start(); video.screen_update(polys, 0, images.mem);
            mock::require(video.last_profile().layers_rebuilt, "CPU fallback lost deferred dirty layers");
        }
    }
    video.set_wide_margin(93);
    video.set_hud_edges(true);
    video.set_gpu_background(true);
    auto panel = polygon();
    panel.num_vertices = 4; panel.z = 0x600; panel.texheader[0] = 0x8000;
    panel.center[0] = 0; panel.center[1] = 384;
    panel.v[0] = {380, -60, {1, 0, 0}}; panel.v[1] = {470, -60, {1, 0, 0}};
    panel.v[2] = {470, -150, {1, 0, 0}}; panel.v[3] = {380, -150, {1, 0, 0}};
    polys = {panel};
    video.frame_start(); video.screen_update(polys, 0, images.mem);
    mock::require(video.hud_at_edges_active(), "race panel enables HUD relocation");
    mock::require(!video.gpu_foreground(), "edge HUD must retain the shared per-item CPU fallback");
    const auto unchanged_generation = video.foreground_generation();
    video.screen_update(polys, 0, images.mem);
    mock::require(video.foreground_generation() == unchanged_generation, "static edge HUD cache missed");
    tile_ram[1] = 0x80; // Foreground-category character zero.
    std::fill_n(char_ram.begin(), 32, uint8_t(0x11));
    video.tile_memory_w(); video.character_memory_w();
    video.screen_update(polys, 0, images.mem);
    mock::require(video.foreground_generation() > unchanged_generation, "changed HUD was not invalidated");
    mock::require(std::any_of(video.foreground_layer().begin(), video.foreground_layer().end(),
                  [](uint32_t pixel) { return pixel != 0; }), "changed HUD pixels lost");
    const auto changed_generation = video.foreground_generation();
    video.screen_update(polys, 0, images.mem);
    mock::require(video.foreground_generation() == changed_generation, "unchanged populated HUD uploaded again");
    auto projected = panel;
    for (int i = 0; i < 4; ++i) {
        projected.v[i].x += 93; projected.v[i].y = -projected.v[i].y;
    }
    mock::require(video.raster().hud_polygon_offset(projected) == 93, "right HUD polygon moves to edge");
    projected.z = 0x601;
    mock::require(video.raster().hud_polygon_offset(projected) == 0, "only panel exact z moves");
    projected.z = 0x8000;
    mock::require(video.raster().hud_polygon_offset(projected) == 0, "scenery is not moved with HUD");
    video.set_wide_margin(0);
    polys[0] = polygon(); polys[0].texheader[0] = 0;
    mock::require(run() == 1, "valid solid triangle did not submit");
    polys[0].v[0].x = std::numeric_limits<float>::quiet_NaN();
    mock::require(run() == 0, "invalid solid polygon reached a GPU draw");
}
#include "vita_gpu_batch_limit.inc"
#include "vita_gpu_batch_state.inc"
#include "vita_gpu_checker.inc"
#include "vita_gpu_addressing.inc"
} // namespace

int main() {
    try {
        const Images images;
        vita::GpuFastRenderer renderer;
        test_cache(renderer, images);
        test_vertices(renderer, images);
        test_system24_batch_limit(renderer, images);
        test_polygon_batch_state(renderer, images);
        test_checker_pixels(renderer);
        test_checker_state(renderer);
        test_source_addressing(renderer);
        renderer.shutdown();
        renderer.shutdown();
        mock::require(mock::blocks.empty() && mock::maps == mock::unmaps && mock::allocations == mock::frees,
                      "shutdown leaked or double-freed GPU memory");
        std::puts("Vita renderer host contracts: 800 shared materials, 32 MiB/3 blocks, source/palette overflow, fences, invalid vertices and shutdown passed");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Vita renderer host contract failed: %s\n", error.what());
        return 1;
    }
}
