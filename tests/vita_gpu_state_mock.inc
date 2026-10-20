// Contract model for state retained between GXM draws. No shader execution.
struct SceGxmContext {
    int shader = 0;
    SceGxmTexture texture{};
    const void *vertices = nullptr;
    uint32_t tint = 0;
};
namespace mock {
SceGxmContext context;
unsigned full_setups = 0, direct_draws = 0;
int fail_direct_stage = 0;
void reset_graphics_state() { context = {}; }
}
SceGxmContext *vita2d_get_context() { return &mock::context; }
const uint16_t *vita2d_get_linear_indices() {
    static const auto indices = [] {
        std::array<uint16_t, 65536> out{};
        for (unsigned i = 0; i < out.size(); ++i) out[i] = uint16_t(i);
        return out;
    }();
    return indices.data();
}
void vita2d_draw_array_textured(const vita2d_texture *t, int primitive,
                              const vita2d_texture_vertex *v, unsigned count, uint32_t color) {
    mock::context.shader = 1;
    mock::context.texture = t->gxm_tex;
    mock::context.tint = color;
    mock::context.vertices = v;
    ++mock::full_setups;
    // libvita2d allocates temporary tint floats for each full setup.
    mock::require(vita2d_pool_memalign(16, 4) != nullptr, "tint allocation exhausted");
    mock_draw_array_textured(t, primitive, v, count, color);
}
void vita2d_draw_array(int primitive, const vita2d_color_vertex *v, unsigned count) {
    mock::context.shader = 2;
    mock::context.vertices = v;
    ++mock::full_setups;
    mock_draw_array(primitive, v, count);
}
int sceGxmSetFragmentTexture(SceGxmContext *c, unsigned slot, const SceGxmTexture *t) {
    mock::require(c == &mock::context && c->shader == 1 && slot == 0, "wrong cached texture shader");
    if (mock::fail_direct_stage == 1) return -1;
    c->texture = *t;
    return 0;
}
int sceGxmSetVertexStream(SceGxmContext *c, unsigned slot, const void *v) {
    mock::require(c == &mock::context && c->shader != 0 && slot == 0, "wrong cached vertex shader");
    if (mock::fail_direct_stage == 2) return -1;
    c->vertices = v;
    return 0;
}
int sceGxmDraw(SceGxmContext *c, int primitive, int index_format, const void *indices, unsigned count) {
    mock::require(c == &mock::context && c->shader != 0, "draw without valid shader state");
    mock::require(index_format == SCE_GXM_INDEX_FORMAT_U16 && indices == vita2d_get_linear_indices(), "wrong indices");
    if (mock::fail_direct_stage == 3) return -1;
    ++mock::direct_draws;
    if (c->shader == 1) {
        vita2d_texture view{c->texture};
        mock_draw_array_textured(&view, primitive, static_cast<const vita2d_texture_vertex *>(c->vertices), count, c->tint);
    } else mock_draw_array(primitive, static_cast<const vita2d_color_vertex *>(c->vertices), count);
    return 0;
}
