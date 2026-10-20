// Included by the real-renderer host contract test, inside its fixture namespace.
void test_system24_batch_limit(vita::GpuFastRenderer &renderer, const Images &images) {
    std::vector<uint8_t> tile_ram(0x10000), char_ram(0x80000);
    auto word = [&](unsigned index, uint16_t value) {
        tile_ram[index * 2] = uint8_t(value);
        tile_ram[index * 2 + 1] = uint8_t(value >> 8);
    };
    for (unsigned layer = 0; layer < 4; ++layer) {
        word(0x5000 + layer, 0x8000); // Distinct scroll on every scanline.
        for (unsigned y = 0; y < rt::Video::H; ++y) word(0x4000 + layer * 0x200 + y, uint16_t(y));
    }
    for (unsigned y = 0; y < rt::Video::H; ++y)
        for (unsigned mask = 0; mask < 4; ++mask) {
            word(0x6000 + y * 4 + mask, 0xaaaa);
            word(0x6800 + y * 4 + mask, 0xaaaa);
        }
    rt::Video video(tile_ram.data(), char_ram.data());
    video.set_external_3d(true);
    const std::vector<rt::GeoPoly> empty;
    video.screen_update(empty, 0, images.mem);
    renderer.prepare_frame();
    mock::start_scene();
    const uint64_t before = mock::textured_vertices;
    const unsigned before_draws = mock::draws;
    mock::require(renderer.draw_system24(video, false), "worst-case mask layer failed submission");
    mock::end_scene();
    renderer.prepare_frame();
    constexpr unsigned quads = 4u * 31u * rt::Video::H;
    mock::require(renderer.system24_quads() == quads && mock::textured_vertices - before == quads * 6u,
                  "System24 batching lost or repeated vertices");
    mock::require(mock::draws - before_draws == 8, "large mask layers did not split at the index-table limit");
}
