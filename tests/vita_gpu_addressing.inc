void test_source_addressing(vita::GpuFastRenderer &renderer) {
    Images images;
    std::fill(images.texture.begin(), images.texture.end(), 0);
    // A single bright interior texel in a black sheet, using the Model 2 cell
    // layout. Both positive and negative two-period UVs must still select it.
    constexpr unsigned x = 4, y = 4;
    constexpr unsigned offset = ((y >> 1) << 9) + (x >> 1);
    constexpr unsigned shift = ((offset & 1) << 4) | ((1 ^ (y & 1)) << 3) | ((1 ^ (x & 1)) << 2);
    images.texture[offset >> 1] = 15u << shift;
    std::fill(images.xlat.begin(), images.xlat.end(), 0);
    std::fill(images.luma.begin(), images.luma.end(), 0);
    images.luma[(15u << 3) * 4] = 255;
    for (unsigned channel = 0; channel < 3; ++channel)
        for (unsigned luma = 1; luma < 64; ++luma)
            images.xlat[(channel * 8192u + 31u * 256u + luma) * 2] = 255;
    auto address = [](float uv, unsigned extent, int mode) {
        const int period = int(extent) * (mode == SCE_GXM_TEXTURE_ADDR_MIRROR ? 2 : 1);
        int at = int(std::floor(uv * float(extent)));
        if (mode == SCE_GXM_TEXTURE_ADDR_CLAMP) return unsigned(std::clamp(at, 0, int(extent) - 1));
        at = (at % period + period) % period;
        if (mode == SCE_GXM_TEXTURE_ADDR_MIRROR && at >= int(extent)) at = period - 1 - at;
        return unsigned(at);
    };
    for (unsigned flags = 0; flags < 16; ++flags) {
        auto p = polygon();
        p.texheader[0] |= uint16_t(flags << 6); p.luma = 255;
        auto source = renderer.build_source(p, images.mem);
        mock::require(source.texture != nullptr, "addressing fixture texture allocation failed");
        const auto &texture = source.texture->gxm_tex;
        const int expected_u = flags & 4 ? SCE_GXM_TEXTURE_ADDR_MIRROR : SCE_GXM_TEXTURE_ADDR_REPEAT;
        const int expected_v = flags & 8 ? SCE_GXM_TEXTURE_ADDR_MIRROR : SCE_GXM_TEXTURE_ADDR_REPEAT;
        mock::require(texture.u_mode == expected_u && texture.v_mode == expected_v,
            "CPU wraps source texels regardless of wrap flag; GPU must not clamp");
        const auto *data = static_cast<const uint8_t *>(texture.data);
        mock::require(data[y * texture.stride + x] == 15 && data[31 * texture.stride + 31] == 0,
            "packed texel addressing fixture did not decode its bright interior and black edge");
        for (const float periods : {-2.0f, 2.0f}) {
            const unsigned u = address(periods + (float(x) + 0.5f) / 32.0f, 32, texture.u_mode);
            const unsigned v = address(periods + (float(y) + 0.5f) / 32.0f, 32, texture.v_mode);
            const uint8_t actual = data[v * texture.stride + u];
            const uint8_t reference = uint8_t(rt::read_texel_quad(0, 0, x, x, y, y,
                images.texture.data()).t00 >> 4);
            mock::require(actual == reference && actual == 15, "out-of-range road UV stuck on black source edge");
            mock::require(renderer.shade_texel(p, images.mem, actual, 255) == 0xffffffffu &&
                renderer.shade_texel(p, images.mem, 0, 255) == 0xff000000u,
                "repeat regression did not exercise bright texels versus black clamp edges");
        }
        delete source.texture; // Arena storage is reclaimed only at the safe reset below.
    }
    renderer.reset_materials();
    std::puts("Vita source addressing: all 16 wrap/mirror combinations, positive/negative repeated UVs and black-edge regression passed");
}
