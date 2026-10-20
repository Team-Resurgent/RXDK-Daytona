#pragma once

#include <vita2d.h>
#include <cmath>

namespace vita {

struct PerspectivePoint { float x, y, u, v, q; };

// Main's shader interpolates (1/z,u/z,v/z), then divides per fragment.
// The stock Vita 2D shader cannot do that division. Measure the error of
// affine edge spans in texels instead of selecting detail from depth alone.
// Preserve the existing eight-way cap and vertex-pool safeguards.
inline int texture_error_subdivision(const PerspectivePoint *p, int count, int minimum) {
    for (int n = minimum; n < 8; n *= 2) {
        bool fits = true;
        for (int a = 0; a < count && fits; ++a) {
            for (int b = a + 1; b < count && fits; ++b) {
                const float dx = p[a].x - p[b].x, dy = p[a].y - p[b].y;
                if (dx * dx + dy * dy < 4.f || p[a].q == p[b].q) continue;
                auto uv = [&](float t, bool v) {
                    const float qa = (1.f - t) * p[a].q, qb = t * p[b].q;
                    return (qa * (v ? p[a].v : p[a].u) + qb * (v ? p[b].v : p[b].u)) / (qa + qb);
                };
                for (int i = 0; i < n && fits; ++i) {
                    const float t0 = float(i) / n, t1 = float(i + 1) / n;
                    for (int v = 0; v < 2; ++v) {
                        const float error = std::abs(uv((t0 + t1) * .5f, v) - (uv(t0, v) + uv(t1, v)) * .5f);
                        if (!std::isfinite(error) || error > .5f) { fits = false; break; }
                    }
                }
            }
        }
        if (fits) return n;
    }
    return 8;
}

// Preserve GPU18's interpolation and triangle order. A tessellated triangle
// repeats its lattice points up to six times; calculate each point only once
// in cached CPU memory before copying it into the GPU's vertex pool.
inline bool emit_perspective_triangle(vita2d_texture_vertex *out,
        const PerspectivePoint &a, const PerspectivePoint &b, const PerspectivePoint &c,
        int subdiv, float source_w, float source_h) {
    if (subdiv != 1 && subdiv != 2 && subdiv != 4 && subdiv != 8) return false;
    bool valid = true;
    auto evaluate = [&](int ib, int ic, vita2d_texture_vertex &dst) {
        const float wb = float(ib) / float(subdiv);
        const float wc = float(ic) / float(subdiv);
        const float wa = 1.0f - wb - wc;
        const float q = wa * a.q + wb * b.q + wc * c.q;
        dst.x = wa * a.x + wb * b.x + wc * c.x;
        dst.y = wa * a.y + wb * b.y + wc * c.y;
        dst.z = 0.5f;
        dst.u = (wa * a.u * a.q + wb * b.u * b.q + wc * c.u * c.q) / q / source_w;
        dst.v = (wa * a.v * a.q + wb * b.v * b.q + wc * c.v * c.q) / q / source_h;
        if (!std::isfinite(dst.x) || !std::isfinite(dst.y) ||
            !std::isfinite(dst.u) || !std::isfinite(dst.v)) valid = false;
    };
    if (subdiv == 1) {
        evaluate(0, 0, out[0]);
        evaluate(1, 0, out[1]);
        evaluate(0, 1, out[2]);
        return valid;
    }
    vita2d_texture_vertex grid[9][9];
    for (int row = 0; row <= subdiv; ++row)
        for (int col = 0; col <= subdiv - row; ++col)
            evaluate(col, row, grid[row][col]);
    for (int row = 0; row < subdiv; ++row) {
        for (int col = 0; col < subdiv - row; ++col) {
            *out++ = grid[row][col];
            *out++ = grid[row][col + 1];
            *out++ = grid[row + 1][col];
            if (col < subdiv - row - 1) {
                *out++ = grid[row][col + 1];
                *out++ = grid[row + 1][col + 1];
                *out++ = grid[row + 1][col];
            }
        }
    }
    return valid;
}

} // namespace vita
