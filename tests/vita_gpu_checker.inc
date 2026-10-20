Images checker_images() {
    Images images;
    std::fill(images.palette.begin(), images.palette.end(), 0);
    images.palette[0x1001 * 2] = 0xff;
    images.palette[0x1001 * 2 + 1] = 0x7f;
    for (unsigned channel = 0; channel < 3; ++channel)
        for (unsigned color = 0; color < 32; ++color)
            for (unsigned luma = 0; luma < 256; ++luma) {
                const size_t index = (channel * 8192u + color * 256u + luma) * 2;
                images.xlat[index] = uint8_t(color * 255u / 31u);
                images.xlat[index + 1] = 0;
            }
    return images;
}

rt::GeoPoly checker_quad(float l, float t, float r, float b, uint16_t depth,
                        bool checker, unsigned color) {
    auto p = polygon();
    p.texheader[0] = checker ? 0x8000 : 0;
    p.texheader[3] = uint16_t(color << 6);
    p.viewport[3] = 384;
    p.center[0] = 0; p.center[1] = 384;
    p.num_vertices = 4; p.z = depth;
    p.v[0] = {l, -t, {1, 0, 0}};
    p.v[1] = {r, -t, {1, 0, 0}};
    p.v[2] = {r, -b, {1, 0, 0}};
    p.v[3] = {l, -b, {1, 0, 0}};
    return p;
}

// Sample only the declared point-filtered RGBA contract at native pixel centers.
// This is deliberately independent of emit code; it does not emulate Vita GXM.
uint32_t sample_checker_draws(float x, float y) {
    uint32_t rgb = 0x00ff00ff;
    for (const auto &draw : mock::captured_draws) {
        if (x < draw.clip[0] || y < draw.clip[1] || x >= draw.clip[2] || y >= draw.clip[3]) continue;
        const size_t count = draw.textured ? draw.vertices.size() : draw.colors.size();
        for (size_t i = 0; i < count; i += 3) {
            auto coord = [&](size_t j, bool yc) {
                if (draw.textured) return yc ? draw.vertices[j].y : draw.vertices[j].x;
                return yc ? draw.colors[j].y : draw.colors[j].x;
            };
            const float ax = coord(i, false), ay = coord(i, true);
            const float bx = coord(i + 1, false), by = coord(i + 1, true);
            const float cx = coord(i + 2, false), cy = coord(i + 2, true);
            const float area = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
            if (area == 0) continue;
            const float wa = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / area;
            const float wb = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / area;
            const float wc = 1.0f - wa - wb;
            if (wa < -0.00001f || wb < -0.00001f || wc < -0.00001f) continue;
            uint32_t rgba;
            if (draw.textured) {
                const auto &texture = draw.texture;
                mock::require(texture.format == SCE_GXM_TEXTURE_FORMAT_A8B8G8R8 &&
                    texture.min_filter == SCE_GXM_TEXTURE_FILTER_POINT &&
                    texture.mag_filter == SCE_GXM_TEXTURE_FILTER_POINT &&
                    texture.u_mode == SCE_GXM_TEXTURE_ADDR_REPEAT &&
                    texture.v_mode == SCE_GXM_TEXTURE_ADDR_REPEAT, "checker sampler must be point/repeat RGBA");
                const float u = wa * draw.vertices[i].u + wb * draw.vertices[i + 1].u + wc * draw.vertices[i + 2].u;
                const float v = wa * draw.vertices[i].v + wb * draw.vertices[i + 1].v + wc * draw.vertices[i + 2].v;
                auto repeat = [](float uv, uint32_t extent) {
                    const int at = int(std::floor(uv * float(extent)));
                    return unsigned((at % int(extent) + int(extent)) % int(extent));
                };
                const auto *row = static_cast<const uint8_t *>(texture.data) + repeat(v, texture.height) * texture.stride;
                std::memcpy(&rgba, row + repeat(u, texture.width) * 4u, sizeof(rgba));
                uint32_t tinted = 0;
                for (unsigned b = 0; b < 4; ++b)
                    tinted |= (((rgba >> (b * 8)) & 255u) * ((draw.tint >> (b * 8)) & 255u) / 255u) << (b * 8);
                rgba = tinted;
            } else rgba = draw.colors[i].color;
            if ((rgba >> 24) == 0) continue;
            mock::require((rgba >> 24) == 255, "checker has unexpected partial alpha");
            rgb = ((rgba & 0xffu) << 16) | (rgba & 0xff00u) | ((rgba >> 16) & 0xffu);
        }
    }
    return rgb;
}

void test_checker_pixels(vita::GpuFastRenderer &renderer) {
    const Images images = checker_images();
    std::vector<uint8_t> tiles(0x10000), chars(0x80000);
    rt::Video video(tiles.data(), chars.data());
    video.set_external_3d(true);
    std::vector<rt::GeoPoly> polys{
        checker_quad(-8, -8, 88, 88, 20, false, 1),
        checker_quad(12, 12, 76, 76, 10, true, 0)};
    rt::Raster cpu;
    mock::capture_draws = true;
    for (const auto offset : {std::array<int, 2>{0, 0}, std::array<int, 2>{5, 7}}) {
        video.xhout_w(uint16_t(offset[0] - 84));
        video.xvout_w(uint16_t(offset[1] - 130));
        video.screen_update(polys, 0, images.mem);
        cpu.render(polys, 0, images.mem, video.crtc_x(), video.crtc_y(),
            video.render_x(), video.render_y(), 0, 495, 0, 383);
        renderer.prepare_frame(); mock::start_scene();
        renderer.draw_polygons(video);
        mock::end_scene();
        mock::require(mock::captured_draws.size() == 2, "checker shadow must follow the road in painter order");
        mock::require(!mock::captured_draws[0].textured && mock::captured_draws[1].textured,
            "solid checker was submitted as an opaque solid");
        mock::require(renderer.checker_polys() == 1 && renderer.textured_checker_polys() == 0,
            "checker diagnostics did not count the solid checker");
        unsigned black = 0, white = 0;
        for (int y = 32; y < 64; ++y)
            for (int x = 32; x < 64; ++x) {
                const uint32_t actual = sample_checker_draws(vita::sx(float(x) + 0.5f), vita::sy(float(y) + 0.5f));
                const uint32_t expected = cpu.pixels()[y * 512 + x] & 0x00ffffffu;
                mock::require(expected == (((x ^ y) & 1) ? 0u : 0xffffffu), "CPU reference checker fixture is wrong");
                mock::require(actual == expected, "GPU checker UV/tint differs from CPU native pixel parity");
                black += actual == 0; white += actual == 0xffffffu;
            }
        mock::require(black == 512 && white == 512, "black shadow must preserve exactly half the underlying road samples");
        // The queued texture/vertex reads are checked at the actual completion
        // fence, then must survive unrelated material-cache resets unchanged.
        const auto texture = mock::captured_draws[1].texture;
        renderer.prepare_frame(); renderer.reset_materials();
        mock::require(mock::live(texture.data, size_t(texture.stride) * texture.height),
            "checker texture was reclaimed with the material cache");
    }
    mock::capture_draws = false;
}

void test_checker_state(vita::GpuFastRenderer &renderer) {
    const Images images = checker_images();
    std::vector<uint8_t> tiles(0x10000), chars(0x80000);
    rt::Video video(tiles.data(), chars.data());
    video.set_external_3d(true);
    std::vector<rt::GeoPoly> polys(9, polygon());
    for (unsigned i = 0; i < polys.size(); ++i) {
        auto &p = polys[i]; p.z = uint16_t(polys.size() - i);
        for (auto &v : p.v) v.x += float(i * 10);
        if (i < 3 || i == 6 || i == 7) p.texheader[0] = 0x8000;
        if (i == 2) p.texheader[3] = 1u << 6;
        if (i == 4) p.texheader[1] = 1;
        if (i == 5) p.texheader[0] = 0;
        if (i == 6) p.viewport[2] = 300;
        if (i == 7) p.viewport[2] = 350;
    }
    mock::capture_draws = true;
    video.screen_update(polys, 0, images.mem);
    renderer.prepare_frame(); mock::start_scene();
    renderer.draw_polygons(video); mock::end_scene();
    mock::require(mock::captured_draws.size() == 8 && mock::ordered_x.size() == 27,
        "checker color/type/clip boundaries lost or reordered a batch");
    mock::require(renderer.checker_polys() == 5 && renderer.shader_setups() == 7 &&
        renderer.state_reuses() == 1 && renderer.draw_errors() == 0,
        "checker tint did not force proper shader rebinds");
    const unsigned starts[] = {0, 2, 3, 4, 5, 6, 7, 8};
    for (unsigned batch = 0; batch < 8; ++batch) {
        const auto &draw = mock::captured_draws[batch];
        const auto &p = polys[starts[batch]];
        if (draw.textured) {
            const bool checker = (p.texheader[0] & 0x8000) != 0;
            mock::require(draw.tint == (checker ? renderer.solid_color(p, images.mem) : 0xffffffffu),
                "retained checker tint leaked into a following texture or another checker");
            mock::require((draw.texture.format == SCE_GXM_TEXTURE_FORMAT_A8B8G8R8) == checker,
                "cached checker texture leaked into normal material draw");
        }
        const int expected_right = int(vita::sx(float(std::min<int>(p.viewport[2] + video.render_x(), rt::Video::W - 1) + 1)));
        mock::require(draw.clip[2] == expected_right, "checker batch crossed a clip boundary");
    }
    for (unsigned i = 0; i < polys.size(); ++i)
        for (unsigned j = 0; j < 3; ++j) {
            const auto &p = polys[i];
            const float expected = vita::sx(float(p.center[0]) + p.v[j].x / p.v[j].p[0]);
            mock::require(std::abs(mock::ordered_x[i * 3 + j] - expected) < 0.0001f,
                "checker changed painter/triangle vertex order");
        }
    renderer.prepare_frame(); mock::capture_draws = false;
    std::puts("Vita checker contracts: CPU parity at 2048 native samples, road holes, tint/type/clip/order and fences passed");
}
