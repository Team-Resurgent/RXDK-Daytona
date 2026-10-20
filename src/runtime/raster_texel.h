// Exact packed-4bpp reads for the Model 2 texture layout. Coordinates entering
// here are masked/wrapped by fetch_bilinear_texel, so they are non-negative.
#pragma once
#include <cstdint>

namespace rt {
struct TexelQuad { uint32_t t00, t01, t10, t11; };
inline TexelQuad read_texel_quad(uint32_t bx, uint32_t by, uint32_t u0, uint32_t u1,
                                 uint32_t v0, uint32_t v1, const uint32_t *sheet) {
    uint32_t x0 = bx + u0, x1 = bx + u1;
    const uint32_t y0 = by + v0, y1 = by + v1;
    // The x>=1024 fold flips y bit 10, even when x is still >=1024 after
    // subtraction. Match that one fold and the original final address mask.
    const uint32_t fold0 = x0 >= 1024 ? 512 : 0;
    const uint32_t fold1 = x1 >= 1024 ? 512 : 0;
    if (fold0) x0 -= 1024;
    if (fold1) x1 -= 1024;
    const uint32_t off00 = (((y0 >> 1) ^ fold0) << 9) + (x0 >> 1);
    const unsigned bit00 = unsigned(((1u ^ (v0 & 1u)) << 3) | ((1u ^ (u0 & 1u)) << 2));
    // An aligned adjacent 2x2 footprint occupies a single 16-bit cell.
    // Nibble selection uses LOCAL u/v parity, not the sheet coordinate parity.
    if (((x0 | y0) & 1u) == 0 && u1 == u0 + 1 && v1 == v0 + 1) {
        const uint32_t cell = sheet[(off00 >> 1) & 0x7ffff] >> ((off00 & 1) << 4);
        return {((cell >> bit00) & 15) << 4, ((cell >> (bit00 ^ 4)) & 15) << 4,
                ((cell >> (bit00 ^ 8)) & 15) << 4, ((cell >> (bit00 ^ 12)) & 15) << 4};
    }
    const uint32_t off01 = (((y0 >> 1) ^ fold1) << 9) + (x1 >> 1);
    const uint32_t off10 = (((y1 >> 1) ^ fold0) << 9) + (x0 >> 1);
    const uint32_t off11 = (((y1 >> 1) ^ fold1) << 9) + (x1 >> 1);
    auto fetch = [sheet](uint32_t off, uint32_t u, uint32_t v) {
        const unsigned bit = unsigned(((off & 1) << 4) | ((1u ^ (v & 1u)) << 3) | ((1u ^ (u & 1u)) << 2));
        return ((sheet[(off >> 1) & 0x7ffff] >> bit) & 15) << 4;
    };
    return {fetch(off00,u0,v0),fetch(off01,u1,v0),fetch(off10,u0,v1),fetch(off11,u1,v1)};
}
} // namespace rt
