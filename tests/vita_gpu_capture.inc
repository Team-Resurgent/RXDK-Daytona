// Optional draw capture for checker contracts. Still a CPU-side contract model,
// not proof of Vita shader execution or hardware pixel-center conventions.
struct CapturedDraw {
    bool textured = false;
    SceGxmTexture texture{};
    std::vector<vita2d_texture_vertex> vertices;
    std::vector<vita2d_color_vertex> colors;
    uint32_t tint = 0;
    std::array<int, 4> clip{};
};
bool capture_draws = false;
std::array<int, 4> clip_rectangle{};
std::vector<CapturedDraw> captured_draws;
