// The old GPU18 emitter is kept independent of the production lattice cache.
// This proves vertex values/order and finite rejection, not device raster output.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "perspective_vertices.h"

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include "../src/compat/bit_cast.h"
#if defined(_MSC_VER)
#define TEST_NOINLINE __declspec(noinline)
#else
#define TEST_NOINLINE __attribute__((noinline))
#endif

namespace {
using Point = vita::PerspectivePoint;
using Vertex = vita2d_texture_vertex;
constexpr std::size_t kMaxVertices = 3 * 8 * 8;
using Output = std::array<Vertex, kMaxVertices + 2>;
std::uint32_t random_state = 0x853c49e6u;
std::uint64_t checked_cases = 0;
std::uint64_t rejected_nan_payload_differences = 0;
volatile std::uint64_t benchmark_sink;

std::uint32_t next_random() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

float finite_random(float scale) {
    return (float(next_random() & 0xffffu) - 32768.0f) * scale;
}

// Copied from GPU18's per-triangle emitter, before cached lattice evaluation.
// Do not share an evaluator with the implementation being checked.
TEST_NOINLINE bool gpu18_triangle(Vertex *verts, const Point &a, const Point &b,
        const Point &c, int subdiv, float source_w, float source_h) {
    bool valid = true;
    std::size_t v = 0;
    auto emit = [&](int ib, int ic) {
        const float wb = float(ib) / float(subdiv);
        const float wc = float(ic) / float(subdiv);
        const float wa = 1.0f - wb - wc;
        const float q = wa * a.q + wb * b.q + wc * c.q;
        Vertex &dst = verts[v++];
        dst.x = wa * a.x + wb * b.x + wc * c.x;
        dst.y = wa * a.y + wb * b.y + wc * c.y;
        dst.z = 0.5f;
        dst.u = (wa * a.u * a.q + wb * b.u * b.q + wc * c.u * c.q) / q / source_w;
        dst.v = (wa * a.v * a.q + wb * b.v * b.q + wc * c.v * c.q) / q / source_h;
        if (!std::isfinite(dst.x) || !std::isfinite(dst.y) ||
            !std::isfinite(dst.u) || !std::isfinite(dst.v)) valid = false;
    };
    for (int row = 0; row < subdiv; ++row) {
        for (int col = 0; col < subdiv - row; ++col) {
            emit(col, row);
            emit(col + 1, row);
            emit(col, row + 1);
            if (col < subdiv - row - 1) {
                emit(col + 1, row);
                emit(col + 1, row + 1);
                emit(col, row + 1);
            }
        }
    }
    return valid;
}

TEST_NOINLINE bool cached_triangle(Vertex *out, const Point &a, const Point &b,
        const Point &c, int subdiv, float source_w, float source_h) {
    return vita::emit_perspective_triangle(out, a, b, c, subdiv, source_w, source_h);
}

std::array<std::uint32_t, 5> bits(const Vertex &v) {
    return {compat::bit_cast<std::uint32_t>(v.x), compat::bit_cast<std::uint32_t>(v.y),
            compat::bit_cast<std::uint32_t>(v.z), compat::bit_cast<std::uint32_t>(v.u),
            compat::bit_cast<std::uint32_t>(v.v)};
}

void check(const Point &a, const Point &b, const Point &c, float sw, float sh) {
    const Vertex sentinel{123.5f, -456.25f, 0.125f, -2.0f, 32.0f};
    for (int subdiv : {1, 2, 4, 8}) {
        Output reference, actual;
        reference.fill(sentinel);
        actual.fill(sentinel);
        const bool expected_valid = gpu18_triangle(reference.data() + 1, a, b, c, subdiv, sw, sh);
        const bool actual_valid = cached_triangle(actual.data() + 1, a, b, c, subdiv, sw, sh);
        if (expected_valid != actual_valid) {
            std::fprintf(stderr, "finite mismatch: case=%llu subdiv=%d\n",
                         static_cast<unsigned long long>(checked_cases), subdiv);
            std::abort();
        }
        // Compare all five components, including signed zero; compilers may
        // choose different NaN operands after inlining. Only rejected NaNs may
        // differ in payload; their classification and rejection must agree.
        // Also verify the leading guard and every byte beyond the output count.
        for (std::size_t v = 0; v < actual.size(); ++v) {
            const auto expected_bits = bits(reference[v]);
            const auto actual_bits = bits(actual[v]);
            for (std::size_t field = 0; field < actual_bits.size(); ++field) {
                if (expected_bits[field] != actual_bits[field]) {
                    if (!expected_valid &&
                        std::isnan(compat::bit_cast<float>(expected_bits[field])) &&
                        std::isnan(compat::bit_cast<float>(actual_bits[field]))) {
                        ++rejected_nan_payload_differences;
                        continue;
                    }
                    std::fprintf(stderr,
                        "vertex mismatch: case=%llu subdiv=%d vertex=%zu field=%zu expected=%08x actual=%08x\n",
                        static_cast<unsigned long long>(checked_cases), subdiv, v, field,
                        expected_bits[field], actual_bits[field]);
                    std::abort();
                }
            }
        }
        assert(bits(actual[0]) == bits(sentinel));
        assert(bits(actual[std::size_t(3 * subdiv * subdiv) + 1]) == bits(sentinel));
        ++checked_cases;
    }
}

void set_field(Point &p, int field, float value) {
    switch (field) {
    case 0: p.x = value; break;
    case 1: p.y = value; break;
    case 2: p.u = value; break;
    case 3: p.v = value; break;
    default: p.q = value; break;
    }
}

void regression_tests() {
    for (int trial = 0; trial < 4000; ++trial) {
        std::array<Point, 3> points;
        for (auto &p : points) {
            p = {finite_random(1.0f / 16.0f), finite_random(1.0f / 16.0f),
                 finite_random(1.0f / 128.0f), finite_random(1.0f / 128.0f),
                 1.0f / (1.0f + float(next_random() & 0xffffu))};
        }
        const float sw = float(8u << (next_random() % 7));
        const float sh = float(8u << (next_random() % 7));
        check(points[0], points[1], points[2], sw, sh);
    }

    // Exercise cancellation, overflows, denormals, both infinities and payloads.
    constexpr std::uint32_t unusual[] = {
        0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u,
        0x007fffffu, 0x807fffffu, 0x00800000u, 0x80800000u,
        0x3f800000u, 0xbf800000u, 0x7f7fffffu, 0xff7fffffu,
        0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u,
        0x7fc12345u, 0xffc54321u, 0x7f800001u, 0xff800001u
    };
    const std::array<Point, 3> base = {{{-10.25f, 4.5f, -2.0f, 8.0f, 0.125f},
                                      {800.0f, -60.0f, 128.0f, 0.0f, 0.03125f},
                                      {70.5f, 600.0f, 4.0f, 128.0f, 2.0f}}};
    for (auto raw : unusual) {
        const float value = compat::bit_cast<float>(raw);
        for (int vertex = 0; vertex < 3; ++vertex) {
            for (int field = 0; field < 5; ++field) {
                auto points = base;
                set_field(points[vertex], field, value);
                check(points[0], points[1], points[2], 64.0f, 256.0f);
            }
        }
        check(base[0], base[1], base[2], value, 64.0f);
        check(base[0], base[1], base[2], 64.0f, value);
        auto points = base;
        for (auto &p : points) p.q = value;
        check(points[0], points[1], points[2], 64.0f, 64.0f);
    }
    for (int trial = 0; trial < 2000; ++trial) {
        std::array<Point, 3> points;
        for (auto &p : points)
            for (int field = 0; field < 5; ++field)
                set_field(p, field, compat::bit_cast<float>(next_random()));
        const float sw = compat::bit_cast<float>(next_random());
        const float sh = compat::bit_cast<float>(next_random());
        check(points[0], points[1], points[2], sw, sh);
    }

    // Unsupported subdivisions must reject without writing anything.
    Output out{};
    const Output unchanged = out;
    for (int subdiv : {-100, -1, 0, 3, 5, 7, 9, 16, std::numeric_limits<int>::max()}) {
        assert(!cached_triangle(out.data(), base[0], base[1], base[2], subdiv, 64.0f, 64.0f));
        assert(std::memcmp(out.data(), unchanged.data(), sizeof(out)) == 0);
    }
    std::printf("Vita tessellation: %llu triangle cases; finite bits, bounds and rejection passed (%llu rejected NaN payload differences)\n",
                static_cast<unsigned long long>(checked_cases),
                static_cast<unsigned long long>(rejected_nan_payload_differences));
}

using Emitter = bool (*)(Vertex *, const Point &, const Point &, const Point &, int, float, float);

double benchmark(Emitter emitter, int subdiv) {
    constexpr unsigned iterations = 200000;
    Output out{};
    std::uint64_t checksum = 0;
    Point a{20.0f, 500.0f, 8.0f, 16.0f, 0.003f};
    const Point b{600.0f, 280.0f, 128.0f, -32.0f, 0.07f};
    const Point c{180.0f, -30.0f, -8.0f, 64.0f, 0.21f};
    const std::clock_t begin = std::clock();
    for (unsigned iteration = 0; iteration < iterations; ++iteration) {
        a.x = 20.0f + float(iteration & 31u);
        checksum += emitter(out.data(), a, b, c, subdiv, 128.0f, 256.0f);
        checksum += compat::bit_cast<std::uint32_t>(out[(iteration % unsigned(3 * subdiv * subdiv))].u);
    }
    const std::clock_t end = std::clock();
    benchmark_sink = checksum;
    return double(end - begin) / double(CLOCKS_PER_SEC) * 1.0e9 / iterations;
}
} // namespace

int main(int argc, char **argv) {
    {
        Point road[] = {{0,0,0,0,1}, {300,0,1024,0,1.2f}, {0,200,0,1024,1}};
        const int n = vita::texture_error_subdivision(road, 3, 1);
        assert(n >= 4 && n <= 8); // old q-ratio rule selected one affine triangle
        auto u = [&](float t) { return 1024.f * 1.2f * t / (1.f + .2f * t); };
        const float old_error = std::abs(u(.5f) - 512.f);
        float error = 0;
        for (int i = 0; i < n; ++i) {
            const float a = float(i)/n, b = float(i+1)/n;
            error = std::max(error, std::abs(u((a+b)*.5f) - (u(a)+u(b))*.5f));
        }
        assert(error < 1.f && old_error > 40.f); // eight-way cap can exceed the 0.5 target
        std::printf("Road edge: %.3f -> %.3f texels midpoint error, subdivision %d\n", old_error, error, n);
        for (auto &p : road) p.q = 1;
        assert(vita::texture_error_subdivision(road, 3, 1) == 1);
    }
    regression_tests();
    if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) {
        std::puts("Single-thread host process CPU time, not a Vita frame-rate measurement:");
        for (int subdiv : {1, 2, 4, 8}) {
            const double old_ns = benchmark(gpu18_triangle, subdiv);
            const double new_ns = benchmark(cached_triangle, subdiv);
            std::printf("  subdiv=%d GPU18=%.1f ns cached=%.1f ns speedup=%.2fx\n",
                        subdiv, old_ns, new_ns, old_ns / new_ns);
        }
    }
}
