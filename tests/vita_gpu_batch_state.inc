void test_polygon_batch_state(vita::GpuFastRenderer &renderer, const Images &images) {
    std::vector<uint8_t> tiles(0x10000), chars(0x80000);
    rt::Video video(tiles.data(), chars.data());
    video.set_external_3d(true);
    std::vector<rt::GeoPoly> polys(200, polygon());
    for (unsigned i = 0; i < polys.size(); ++i) {
        auto &p = polys[i];
        p.z = uint16_t(polys.size() - i); // Deterministic painter order.
        for (auto &v : p.v) v.x += float(i);
        if (i >= 50 && i < 100) p.texheader[1] = 1;
        if (i >= 100 && i < 150) { p.texheader[0] = 0; p.texheader[3] = uint16_t((i & 1u) << 6); }
        if (i >= 150) p.viewport[2] = i < 175 ? 300 : 350;
    }
    auto run = [&] {
        renderer.prepare_frame();
        video.screen_update(polys, 0, images.mem);
        mock::start_scene();
        renderer.draw_polygons(video);
        mock::end_scene();
        renderer.prepare_frame();
    };
    run();
    mock::require(renderer.textured_polys() == 150 && renderer.solid_polys() == 50, "polygon batch lost polygons");
    mock::require(renderer.textured_draws() == 4 && renderer.solid_draws() == 1, "compatible adjacent polygons did not batch");
    mock::require(renderer.shader_setups() == 4 && renderer.state_reuses() == 1 && renderer.draw_errors() == 0,
                  "shader setup not reused or not invalidated at clip/type change");
    mock::require(mock::ordered_x.size() == 600, "batch vertex count mismatch");
    for (unsigned i = 0; i < polys.size(); ++i) {
        const void *palette = nullptr;
        if (!(i >= 100 && i < 150)) palette = renderer.material_for(polys[i], images.mem)->palette;
        for (unsigned j = 0; j < 3; ++j) {
            const auto &v = polys[i].v[j];
            const float expected = vita::sx(float(polys[i].center[0]) + v.x / v.p[0]);
            mock::require(std::abs(mock::ordered_x[i * 3 + j] - expected) < 0.0001f,
                          "batch changed painter/vertex order");
            mock::require(mock::ordered_palettes[i * 3 + j] == palette, "batch retained another material palette");
        }
    }
    // Each direct API failure must drop only that batch and force a full setup
    // for later work, rather than drawing with a stale texture/vertex stream.
    polys.assign(3, polygon());
    for (unsigned i = 0; i < 3; ++i) { polys[i].z = uint16_t(3 - i); polys[i].texheader[1] = uint16_t(i); }
    for (int stage = 1; stage <= 3; ++stage) {
        mock::fail_direct_stage = stage;
        run();
        mock::require(renderer.draw_errors() == 1 && renderer.shader_setups() == 2 &&
                      renderer.textured_draws() == 2 && mock::ordered_x.size() == 6,
                      "failed cached state did not recover on following batch");
    }
    mock::fail_direct_stage = 0;
    std::puts("Vita batches: painter order, palette/clip/type boundaries, state reuse and API failure recovery passed");
}
