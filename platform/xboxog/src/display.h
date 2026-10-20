// The D3D8 device (640x480, vsync) and the plain presenter: the runtime's
// CPU-composed 496x384 screen (rt::Video::screen, 0xAARRGGBB) as a linear
// texture, stretched over the whole 4:3 picture. The hardware renderer
// (renderer.h) draws through the same device.
#pragma once

#include <xtl.h>

#include "runtime/video.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace xbox {

class Display {
public:
    static constexpr UINT kWidth = 640, kHeight = 480;

    Display() {
        d3d_ = Direct3DCreate8(D3D_SDK_VERSION);
        if (!d3d_) throw std::runtime_error("Direct3DCreate8 failed");
        D3DPRESENT_PARAMETERS pp = {};
        pp.BackBufferWidth = kWidth;
        pp.BackBufferHeight = kHeight;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferCount = 1;
        // No depth test (renderer.h draws in the game's order), but a stencil:
        // Model 2's checker polygons (its translucency, every other pixel)
        // are drawn through a checkerboard written into it.
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        // A frame that misses its vblank goes out at once rather than
        // waiting a whole one more: Model 2 runs at 57.5 Hz, the Xbox at 60.
        pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE_OR_IMMEDIATE;
        if (FAILED(d3d_->CreateDevice(0, D3DDEVTYPE_HAL, nullptr, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device_)))
            throw std::runtime_error("CreateDevice failed");
    }

    IDirect3DDevice8 *device() const { return device_; }

    void clear() { device_->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.f, 0); }
    void present() { device_->Present(nullptr, nullptr, nullptr, nullptr); }

    // The CPU path: copy the composed screen and draw it.
    void draw_screen(const rt::Video &video) {
        const std::vector<uint32_t> &pixels = video.screen();
        const int width = video.width(), height = rt::Video::H;
        // (Made on first use: the NV2A renderer never needs it.)
        if (!screen_ && FAILED(device_->CreateTexture(512, 512, 1, 0, D3DFMT_LIN_X8R8G8B8, 0, &screen_)))
            throw std::runtime_error("no memory for the screen texture");
        D3DLOCKED_RECT locked;
        if (FAILED(screen_->LockRect(0, &locked, nullptr, 0))) return;
        for (int y = 0; y < height; ++y)
            std::memcpy(static_cast<uint8_t *>(locked.pBits) + size_t(y) * locked.Pitch, pixels.data() + size_t(y) * width,
                        size_t(width) * 4);
        screen_->UnlockRect(0);
        draw_texels(screen_, 0, 0, float(width), float(height), 0, 0, float(kWidth), float(kHeight), false);
    }

    // Texels (u0, v0)-(u1, v1) of a linear texture (linear textures take
    // texel, not normalized, coordinates) over the screen rectangle x, y,
    // cw, ch; with blend, texels of alpha 0 are left out (the alpha test).
    void draw_texels(IDirect3DTexture8 *texture, float u0, float v0, float u1, float v1, float x, float y, float cw,
                     float ch, bool blend) {
        struct Vertex { float x, y, z, rhw, u, v; };
        const Vertex quad[4] = {
            {x - .5f, y - .5f, 0, 1, u0, v0},
            {x + cw - .5f, y - .5f, 0, 1, u1, v0},
            {x - .5f, y + ch - .5f, 0, 1, u0, v1},
            {x + cw - .5f, y + ch - .5f, 0, 1, u1, v1},
        };
        device_->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device_->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device_->SetRenderState(D3DRS_LIGHTING, FALSE);
        device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device_->SetRenderState(D3DRS_ALPHATESTENABLE, blend);
        device_->SetRenderState(D3DRS_ALPHAREF, 0x80);
        device_->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        device_->SetTexture(0, texture);
        device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device_->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
        device_->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
        device_->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
        device_->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
        device_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device_->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
        device_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
        device_->SetTexture(0, nullptr);
    }

private:
    IDirect3D8 *d3d_ = nullptr;
    IDirect3DDevice8 *device_ = nullptr;
    IDirect3DTexture8 *screen_ = nullptr;
};

} // namespace xbox
