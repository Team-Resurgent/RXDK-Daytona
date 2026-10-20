// Host model of the API contract, not a GPU implementation.
#pragma once
#include <cstdint>
#include "kernel/sysmem.h"
using SceGxmTextureFormat = int;
struct SceGxmContext;
constexpr int SCE_GXM_INDEX_FORMAT_U16 = 0;
constexpr int SCE_GXM_TEXTURE_FORMAT_A8B8G8R8 = 1, SCE_GXM_TEXTURE_FORMAT_P8_ABGR = 2;
constexpr int SCE_GXM_TEXTURE_FILTER_LINEAR = 1, SCE_GXM_TEXTURE_FILTER_POINT = 2;
constexpr int SCE_GXM_TEXTURE_ADDR_REPEAT = 1, SCE_GXM_TEXTURE_ADDR_MIRROR = 2,
              SCE_GXM_TEXTURE_ADDR_CLAMP = 3;
constexpr int SCE_GXM_MEMORY_ATTRIB_READ = 1, SCE_GXM_PRIMITIVE_TRIANGLES = 1;
struct SceGxmTexture {
    void *data = nullptr;
    uint32_t *palette = nullptr;
    uint32_t width = 0, height = 0, stride = 0;
    SceGxmTextureFormat format = 0;
    int u_mode = 0, v_mode = 0, min_filter = 0, mag_filter = 0;
};
int sceGxmMapMemory(void *, SceSize, int);
int sceGxmUnmapMemory(void *);
int sceGxmTextureInitLinear(SceGxmTexture *, void *, SceGxmTextureFormat, uint32_t, uint32_t, uint32_t);
int sceGxmTextureSetUAddrMode(SceGxmTexture *, int);
int sceGxmTextureSetVAddrMode(SceGxmTexture *, int);
int sceGxmTextureSetPalette(SceGxmTexture *, void *);
int sceGxmSetFragmentTexture(SceGxmContext *, unsigned, const SceGxmTexture *);
int sceGxmSetVertexStream(SceGxmContext *, unsigned, const void *);
int sceGxmDraw(SceGxmContext *, int, int, const void *, unsigned);
