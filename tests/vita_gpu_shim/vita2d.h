#pragma once
#include <psp2/gxm.h>
#include <cstdint>
#define RGBA8(r, g, b, a) (uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24))
struct vita2d_texture { SceGxmTexture gxm_tex; };
struct vita2d_texture_vertex { float x, y, z, u, v; };
struct vita2d_color_vertex { float x, y, z; uint32_t color; };
void vita2d_wait_rendering_done();
SceGxmContext *vita2d_get_context();
const uint16_t *vita2d_get_linear_indices();
void vita2d_texture_set_filters(vita2d_texture *, int, int);
void *vita2d_texture_get_datap(const vita2d_texture *);
unsigned vita2d_texture_get_stride(const vita2d_texture *);
unsigned vita2d_pool_free_space();
void *vita2d_pool_memalign(unsigned, unsigned);
void vita2d_draw_rectangle(float, float, float, float, uint32_t);
void vita2d_draw_array_textured(const vita2d_texture *, int, const vita2d_texture_vertex *, unsigned, uint32_t);
void vita2d_draw_array(int, const vita2d_color_vertex *, unsigned);
void vita2d_draw_texture_part_scale(const vita2d_texture *, float, float, float, float, float, float, float, float);
void vita2d_draw_texture_scale(const vita2d_texture *, float, float, float, float);
void vita2d_enable_clipping();
void vita2d_disable_clipping();
void vita2d_set_clip_rectangle(int, int, int, int);
// Any return to the old per-texture allocation path must fail this test.
vita2d_texture *vita2d_create_empty_texture_format(unsigned, unsigned, SceGxmTextureFormat);
void vita2d_free_texture(vita2d_texture *);
