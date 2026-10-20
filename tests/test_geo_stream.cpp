// Point batching must preserve the original three scalar rasterizer writes.
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>
#define private public
#include "runtime/geo.h"
#undef private
#include "../src/runtime/geo.cpp"
#include "../src/compat/bit_cast.h"
namespace {
bool same_poly(const rt::GeoPoly &a, const rt::GeoPoly &b) {
    if (a.z != b.z || a.luma != b.luma || a.texlod != b.texlod || a.window != b.window ||
        a.reverse != b.reverse || a.num_vertices != b.num_vertices) return false;
    for (unsigned i = 0; i < 4; ++i)
        if (a.texheader[i] != b.texheader[i] || a.viewport[i] != b.viewport[i]) return false;
    for (unsigned i = 0; i < 2; ++i) if (a.center[i] != b.center[i]) return false;
    for (unsigned i = 0; i < a.num_vertices; ++i) {
        if (compat::bit_cast<uint32_t>(a.v[i].x) != compat::bit_cast<uint32_t>(b.v[i].x) ||
            compat::bit_cast<uint32_t>(a.v[i].y) != compat::bit_cast<uint32_t>(b.v[i].y)) return false;
        for (unsigned j = 0; j < 3; ++j)
            if (compat::bit_cast<uint32_t>(a.v[i].p[j]) != compat::bit_cast<uint32_t>(b.v[i].p[j])) return false;
    }
    return true;
}
void reset(rt::Geo &geo, unsigned trial, unsigned index, bool triangle) {
    const uint16_t *texture = geo.raster_.texture_rom;
    const uint32_t texture_mask = geo.raster_.texture_rom_mask;
    geo.raster_ = {};
    auto &r = geo.raster_;
    r.texture_rom = texture;
    r.texture_rom_mask = texture_mask;
    r.master_z_clip = 0xff;
    r.cur_command = 1;
    r.command_index = index;
    r.command_buffer[0] = trial % 64;
    r.command_buffer[1] = (trial * 7) % 64;
    r.command_buffer[8] = 0x20600u | (triangle ? 2u : 1u);
    r.command_buffer[9] = (trial & 255u) << 15;
    r.command_buffer[10] = 0x3f8000;
    auto point = [&](unsigned at, float x, float y) {
        r.command_buffer[at] = compat::bit_cast<uint32_t>(x) >> 8;
        r.command_buffer[at + 1] = compat::bit_cast<uint32_t>(y) >> 8;
        r.command_buffer[at + 2] = compat::bit_cast<uint32_t>(10.0f) >> 8;
    };
    point(2, -20.0f, -20.0f); point(5, -20.0f, 20.0f);
    point(11, 20.0f, -20.0f); point(14, 20.0f, 20.0f);
    for (auto &planes : r.clip_plane) {
        planes[0].normal = {1, 0, {2, 0, 0}};
        planes[1].normal = {-1, 0, {2, 0, 0}};
        planes[2].normal = {0, 1, {2, 0, 0}};
        planes[3].normal = {0, -1, {2, 0, 0}};
    }
    geo.polys.clear(); geo.pushed.clear(); geo.record_pushes = (trial & 8) != 0;
}
} // namespace

int main() {
    std::vector<uint8_t> polygons(128), textures(128);
    for (unsigned i = 0; i < textures.size(); ++i) textures[i] = uint8_t(i * 7);
    std::array<uint32_t, 0x8000> buffer{};
    auto scalar = std::make_unique<rt::Geo>(polygons, textures, buffer.data());
    auto batch = std::make_unique<rt::Geo>(polygons, textures, buffer.data());
    constexpr unsigned cases = 6000;
    unsigned kept = 0;
    const unsigned slots[] = {0, 1, 2, 5, 11, 14};
    for (unsigned trial = 0; trial < cases; ++trial) {
        const unsigned index = slots[trial % std::size(slots)];
        const bool triangle = (trial / std::size(slots)) % 2 != 0 && index != 14;
        reset(*scalar, trial, index, triangle); reset(*batch, trial, index, triangle);
        rt::GeoVertex point{float(1 + trial % 40), float(int(trial % 51) - 25), {10, 0, 0}};
        if (trial % 17 == 0) { // Window command: force the generic fallback.
            scalar->raster_.cur_command = batch->raster_.cur_command = 3;
            scalar->raster_.command_index = batch->raster_.command_index = 3;
        }
        scalar->model2_3d_push(&scalar->raster_, compat::bit_cast<uint32_t>(point.x) >> 8);
        scalar->model2_3d_push(&scalar->raster_, compat::bit_cast<uint32_t>(point.y) >> 8);
        scalar->model2_3d_push(&scalar->raster_, compat::bit_cast<uint32_t>(point.p[0]) >> 8);
        batch->model2_3d_push_point(&batch->raster_, point);
        const auto &a = scalar->raster_, &b = batch->raster_;
        bool same = a.cur_command == b.cur_command && a.command_index == b.command_index &&
                    a.poly_list_index == b.poly_list_index && a.min_z == b.min_z && a.max_z == b.max_z &&
                    compat::bit_cast<uint32_t>(a.polygon_z) == compat::bit_cast<uint32_t>(b.polygon_z) &&
                    std::memcmp(a.command_buffer, b.command_buffer, sizeof(a.command_buffer)) == 0 &&
                    scalar->pushed == batch->pushed && scalar->polys.size() == batch->polys.size();
        for (size_t i = 0; same && i < scalar->polys.size(); ++i) same = same_poly(scalar->polys[i], batch->polys[i]);
        if (!same) { std::fprintf(stderr, "geometry point stream differs at case %u\n", trial); return 1; }
        kept += unsigned(scalar->polys.size());
    }
    if (!kept) return 1;
    std::printf("geometry point batches: %u scalar-equivalent states, %u kept polygons passed\n", cases, kept);
}
