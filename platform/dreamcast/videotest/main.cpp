// Dreamcast display-path test (no game code): the game's screen, a 496x384
// ARGB8888 frame as the runtime's CPU renderer produces it, converted to
// RGB565 each frame, uploaded to a PVR texture and drawn scaled to 640x480
// with the arcade aspect kept. The tilemap layers will take this path.
// Reports the per-frame cost of conversion and upload, the frame rate, and
// the controller, on the serial console. Speed figures from Flycast are not
// the console's.

#include <kos.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kW = 496, kH = 384;      // the Model 2's screen
constexpr int kTexW = 512, kTexH = 512; // the PVR texture holding it (powers of two)
constexpr int kFrames = 600;

// A moving test frame, standing in for Video::screen() (0xAARRGGBB).
void draw(std::vector<uint32_t> &screen, int frame) {
    for (int y = 0; y < kH; y++)
        for (int x = 0; x < kW; x++) {
            uint32_t c = ((x / 31 + y / 24) & 1) ? 0xff203040u : 0xff4060a0u; // 16x16 checks
            if (((x + frame * 2) % kW) < 8) c = 0xffffffffu;                 // a moving bar
            if (x == 0 || y == 0 || x == kW - 1 || y == kH - 1) c = 0xffff0000u;   // the edge, to see cropping
            screen[size_t(y) * kW + x] = c;
        }
}

uint16_t rgb565(uint32_t c) {
    return uint16_t(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
}

} // namespace

int main() {
    pvr_init_defaults();
    pvr_ptr_t texture = pvr_mem_malloc(kTexW * kTexH * 2);
    std::vector<uint32_t> screen(size_t(kW) * kH);
    std::vector<uint16_t> staging(size_t(kTexW) * kH, 0);

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, kTexW, kTexH, texture,
                     PVR_FILTER_BILINEAR);
    pvr_poly_hdr_t header;
    pvr_poly_compile(&header, &cxt);

    // 496x384 is 1.29:1; at the full 480 lines it is 620 pixels wide, centred.
    const float h = 480.0f, w = h * kW / kH, x0 = (640.0f - w) / 2, x1 = x0 + w;
    const float u1 = float(kW) / kTexW, v1 = float(kH) / kTexH;

    std::printf("VIDEOTEST %dx%d -> %.0fx%.0f at x %.0f\n", kW, kH, w, h, x0);
    uint64_t convert_us = 0, upload_us = 0, last = timer_us_gettime64();
    for (int frame = 1; frame <= kFrames; frame++) {
        draw(screen, frame);
        const uint64_t t0 = timer_us_gettime64();
        for (int y = 0; y < kH; y++)
            for (int x = 0; x < kW; x++) staging[size_t(y) * kTexW + x] = rgb565(screen[size_t(y) * kW + x]);
        const uint64_t t1 = timer_us_gettime64();
        pvr_txr_load(staging.data(), texture, staging.size() * 2);
        const uint64_t t2 = timer_us_gettime64();
        convert_us += t1 - t0;
        upload_us += t2 - t1;

        pvr_wait_ready();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        pvr_prim(&header, sizeof header);
        pvr_vertex_t v{};
        v.argb = 0xffffffffu;
        v.z = 1.0f;
        const float corners[4][4] = {{x0, 0, 0, 0}, {x1, 0, u1, 0}, {x0, h, 0, v1}, {x1, h, u1, v1}};
        for (int i = 0; i < 4; i++) {
            v.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v.x = corners[i][0];
            v.y = corners[i][1];
            v.u = corners[i][2];
            v.v = corners[i][3];
            pvr_prim(&v, sizeof v);
        }
        pvr_list_finish();
        pvr_scene_finish();

        if (frame % 120 == 0) {
            const uint64_t now = timer_us_gettime64();
            int buttons = -1, joyx = 0, rtrig = 0, ltrig = 0;
            if (maple_device_t *pad = maple_enum_type(0, MAPLE_FUNC_CONTROLLER))
                if (auto *s = static_cast<cont_state_t *>(maple_dev_status(pad))) {
                    buttons = int(s->buttons);
                    joyx = s->joyx;
                    rtrig = s->rtrig;
                    ltrig = s->ltrig;
                }
            std::printf("VIDEOTEST frame %d: %.1f fps, convert %.2f ms, upload %.2f ms; pad %s buttons %x joyx %d "
                        "triggers %d/%d\n",
                        frame, 120e6 / double(now - last), convert_us / 120e3, upload_us / 120e3,
                        buttons < 0 ? "none" : "A0", buttons < 0 ? 0 : buttons, joyx, ltrig, rtrig);
            convert_us = upload_us = 0;
            last = now;
        }
    }
    std::printf("VIDEOTEST DONE\n");
    for (;;) thd_sleep(1000);
}
