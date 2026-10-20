// ROM-free clipping parity against the original modulo-based edge walk.
// Include the implementation so its file-local clipper is exercised directly.
#include "../src/runtime/geo.cpp"

#include <cstdio>
#include <random>
#include "../src/compat/bit_cast.h"
namespace {
int reference_clip(rt::GeoVertex *v, int count, rt::GeoVertex *out, rt::Geo::plane plane) {
    int n = 0;
    const rt::GeoVertex *cur = v;
    float curdot = rt::dot_product(*cur, plane.normal);
    bool curin = curdot >= plane.distance;
    for (int i = 0; i < count; ++i, ++cur) {
        const int next = (i + 1) % count;
        if (curin) out[n++] = *cur;
        const float nextdot = rt::dot_product(v[next], plane.normal);
        const bool nextin = nextdot >= plane.distance;
        if (curin != nextin && !std::isnan(curdot) && !std::isnan(nextdot)) {
            const float scale = (plane.distance - curdot) / (nextdot - curdot);
            out[n].x = cur->x + ((v[next].x - cur->x) * scale);
            out[n].y = cur->y + ((v[next].y - cur->y) * scale);
            for (int p = 0; p < 3; ++p)
                out[n].p[p] = cur->p[p] + ((v[next].p[p] - cur->p[p]) * scale);
            ++n;
        }
        curdot = nextdot;
        curin = nextin;
    }
    return n;
}

bool same(const rt::GeoVertex &a, const rt::GeoVertex &b) {
    if (compat::bit_cast<uint32_t>(a.x) != compat::bit_cast<uint32_t>(b.x) ||
        compat::bit_cast<uint32_t>(a.y) != compat::bit_cast<uint32_t>(b.y)) return false;
    for (int p = 0; p < 3; ++p)
        if (compat::bit_cast<uint32_t>(a.p[p]) != compat::bit_cast<uint32_t>(b.p[p])) return false;
    return true;
}
} // namespace

int main() {
    if (rt::clip_polygon(nullptr, 0, nullptr, {}) != 0) return 1;
    std::mt19937 random(0x24c11f);
    const auto value = [&] { return float(int(random() % 200001) - 100000) / 100.0f; };
    constexpr unsigned cases = 100000;
    unsigned accepted_without_clipping = 0;
    for (unsigned trial = 0; trial < cases; ++trial) {
        rt::GeoVertex input[8], expected[16], actual[16];
        const int count = 1 + random() % 8;
        for (auto &v : input) {
            v.x = value(); v.y = value();
            for (float &p : v.p) p = value();
        }
        // Cover NaNs, infinities, exact plane boundaries and signed zeros.
        if (trial % 19 == 0) input[0].x = std::numeric_limits<float>::quiet_NaN();
        if (trial % 23 == 0) input[0].p[0] = std::numeric_limits<float>::infinity();
        if (trial % 29 == 0) input[0].y = -0.0f;
        rt::Geo::plane plane;
        plane.normal.x = value(); plane.normal.y = value(); plane.normal.p[0] = value();
        plane.distance = trial % 7 == 0 ? rt::dot_product(input[0], plane.normal) : value();
        const int want = reference_clip(input, count, expected, plane);
        if (rt::polygon_inside_plane(input, count, plane)) {
            if (want != count) return 1;
            for (int i = 0; i < count; ++i)
                if (!same(input[i], expected[i])) return 1;
            ++accepted_without_clipping;
        }
        const int got = rt::clip_polygon(input, count, actual, plane);
        if (got != want) {
            std::fprintf(stderr, "clip count differs at case %u: %d / %d\n", trial, got, want);
            return 1;
        }
        for (int i = 0; i < got; ++i)
            if (!same(expected[i], actual[i])) {
                std::fprintf(stderr, "clip vertex differs at case %u vertex %d\n", trial, i);
                return 1;
            }
    }
    if (!accepted_without_clipping) return 1;
    std::printf("geometry clipping: %u bit-identical cases, %u exact bypasses and empty-input check passed\n",
                cases, accepted_without_clipping);
}
