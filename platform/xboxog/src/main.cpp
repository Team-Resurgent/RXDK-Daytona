// Daytona USA on the original Xbox.
//
// The same generated i960/TGP code and native Model 2 board as the desktop
// (src/app/main.cpp), with the Dreamcast port's low-memory runtime
// (M2_DC_MEMORY: ROM read through pages, texture RAM the frontend's) and an
// Xbox frontend: the ROM regions held in memory (rom.h), the controller as
// the cabinet (input.h), the native sound sequencer into DirectSound
// (audio.h) and the NV2A drawing the polygons (renderer.h, display.h).
// Everything under D:\data is the user's own, written by stage_data.py from
// their ROM set; saves go to T:\.
#include <xtl.h>

#include "runtime/game_loop.h"
#include "runtime/native_sound_engine.h"

#include "audio.h"
#include "display.h"
#include "hd.h"
#include "input.h"
#include "renderer.h"
#include "rom.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char *kData = "D:\\data\\";
constexpr const char *kEeprom = "T:\\ioboard_eeprom.bin";
constexpr const char *kBackup = "T:\\backup_ram.bin";
constexpr int kReport = 575; // frames between log lines (10 s)
constexpr int kDrawEvery = 2;  // frames per picture: the board's geometry and tile layers only for those
constexpr size_t kCachePages = 4096; // 16 MB of ROM pages (rom.h): of 48 MB of real ROM data

void log(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    OutputDebugStringA(line);
}

// The generated code computes the i960's floating point in host doubles,
// validated on SSE2. The Pentium III does doubles on the x87: round them to
// a double's 53 bits, not the x87's 64.
void double_precision() {
    unsigned short cw;
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    cw = uint16_t((cw & ~0x0300u) | 0x0200u);
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
}

uint64_t now_us() {
    static const double scale = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e6 / double(f.QuadPart);
    }();
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return uint64_t(double(count.QuadPart) * scale);
}

unsigned free_kb() {
    MEMORYSTATUS status = {};
    GlobalMemoryStatus(&status);
    return unsigned(status.dwAvailPhys / 1024);
}

std::vector<uint8_t> read_optional(const std::string &path) {
    try {
        return xbox::read_file(path);
    } catch (const std::exception &) {
        return {};
    }
}

void write_file(const char *path, const uint8_t *bytes, size_t size) {
    if (FILE *file = std::fopen(path, "wb")) {
        std::fwrite(bytes, 1, size, file);
        std::fclose(file);
    }
}

void save(rt::GameLoop &game) {
    const auto &eeprom = game.board().io().eeprom;
    write_file(kEeprom, eeprom.data(), eeprom.size());
    const auto &backup = game.board().backup_ram();
    write_file(kBackup, backup.data(), backup.size());
}

void run() {
    double_precision();
    XInitDevices(0, nullptr);
    log("Daytona: start, %u KB free\n", free_kb());

    xbox::CachedRom rom(kData, kCachePages);
    log("Daytona: ROM page cache %u KB; %u KB free\n", unsigned(rom.cache_bytes() / 1024), free_kb());
    // Texture RAM (tex0 then tex1, 2 MB each), as the Dreamcast frontend
    // supplies it.
    std::vector<uint8_t> texture_ram(0x400000, 0);
    rt::M2Board::Images images;
    images.rom = &rom;
    images.texture_ram = texture_ram.data();
    images.copro_tables = xbox::read_file(std::string(kData) + "copro_tables.bin");
    auto game = std::make_unique<rt::GameLoop>(std::move(images), false); // sound: the native engine below
    // The console's own settings, or on a first run the ones staged with the
    // data (stage_data.py --nvram: a single cabinet set on the desktop).
    auto eeprom = read_optional(kEeprom), backup = read_optional(kBackup);
    if (eeprom.empty()) {
        eeprom = read_optional(std::string(kData) + "ioboard_eeprom.bin");
        backup = read_optional(std::string(kData) + "backup_ram.bin");
    }
    if (eeprom.size() == game->board().io().eeprom.size())
        std::copy(eeprom.begin(), eeprom.end(), game->board().io().eeprom.begin());
    if (backup.size() == game->board().backup_ram().size())
        std::copy(backup.begin(), backup.end(), game->board().backup_ram().begin());
    log("Daytona: board ready, %u KB free\n", free_kb());

    xbox::Display display;
    double_precision(); // (in case D3D changed it)
    // The NV2A draws the polygons (renderer.h) unless D:\cpu.on asks for the
    // runtime's CPU rasterizer, the reference picture.
    const bool cpu = GetFileAttributesA("D:\\cpu.on") != INVALID_FILE_ATTRIBUTES;
    xbox::HdTextures hd;
    std::unique_ptr<xbox::Renderer> renderer;
    if (!cpu) {
        game->board().video().set_external_3d(true);
        renderer = std::make_unique<xbox::Renderer>(display, hd);
    }
    xbox::Audio audio(std::make_unique<snd::NativeSoundEngine>(xbox::read_file(std::string(kData) + "sound_program.bin"),
                                                               xbox::read_file(std::string(kData) + "pcm1.bin"),
                                                               xbox::read_file(std::string(kData) + "pcm2.bin")));
    xbox::Pad pad;
    log("Daytona: %s renderer, sound %s, %u HD textures%s; %u KB free\n", cpu ? "CPU" : "NV2A",
        audio.playing() ? "on" : "off", unsigned(hd.available()), hd.dumping() ? ", dumping to T:\\dump" : "",
        free_kb());

    const double frame_us = 1e6 / rt::GameLoop::kFrameHz;
    const uint64_t start = now_us();
    uint64_t frames = 0, presented = 0, skipped = 0, run_us = 0, draw_us = 0, sound_us = 0;
    // Where a frame's time goes (the runtime's profile): i960, TGP and
    // scheduling ("core"), geometry, and the tile layers ("video").
    uint64_t core_us = 0, geometry_us = 0, video_us = 0;
    game->set_profile_clock(now_us);
    // Draw mode, as the Dreamcast: the game runs every frame, its picture
    // (geometry and tile layers) is made and shown every kDrawEvery-th.
    game->set_frame_skip(kDrawEvery - 1);
    for (;;) {
        const uint64_t t0 = now_us();
        game->run_frame(pad.sample());
        const rt::FrameProfile &profile = game->last_profile();
        core_us += profile.core();
        geometry_us += profile.geometry;
        video_us += profile.video;
        const uint64_t ts = now_us();
        audio.frame(game->board().take_sound_bytes());
        ++frames;
        const uint64_t t1 = now_us();
        run_us += ts - t0;
        sound_us += t1 - ts;
        // Just after a screen update (vblank_end updates when frame %
        // kDrawEvery is 0, then counts on), unless behind real time by more
        // than kDrawEvery frames: then that picture is skipped to catch up.
        const double due = double(frames) * frame_us;
        if (game->board().frame() % kDrawEvery == 1 % kDrawEvery) {
            if (double(t1 - start) > due + kDrawEvery * frame_us) {
                ++skipped;
            } else {
                display.clear();
                if (renderer) renderer->draw(game->board().video());
                else display.draw_screen(game->board().video());
                display.present();
                ++presented;
                draw_us += now_us() - t1;
            }
        }
        const double ahead = due - double(now_us() - start);
        if (ahead > 1500) Sleep(DWORD(ahead / 1000));
        if (frames % kReport == 0) {
            log("Daytona: frame %llu, %.1f s; presented %llu skipped %llu; ms/frame run %.1f draw %.1f; "
                "ROM misses %llu (%.0f ms reading); sound underruns %llu; polygons %u textured %u materials %u (%u KB) hd %u dumped %u; %u KB free\n",
                (unsigned long long)frames, double(now_us() - start) / 1e6, (unsigned long long)presented,
                (unsigned long long)skipped, run_us / 1000.0 / kReport, presented ? draw_us / 1000.0 / kReport : 0.0,
                (unsigned long long)rom.misses, rom.read_us / 1000.0, (unsigned long long)audio.underruns, renderer ? renderer->drawn : 0u, renderer ? renderer->textured : 0u,
                renderer ? unsigned(renderer->materials()) : 0u,
                renderer ? unsigned(renderer->texture_bytes() / 1024) : 0u, renderer ? renderer->hd_loaded : 0u,
                hd.dumps, free_kb());
            log("Daytona: profile ms/frame: core %.1f geometry %.1f video %.1f sound %.1f; draw %.1f per shown frame\n",
                core_us / 1000.0 / kReport, geometry_us / 1000.0 / kReport, video_us / 1000.0 / kReport,
                sound_us / 1000.0 / kReport, presented ? draw_us / 1000.0 / double(presented) : 0.0);
            run_us = draw_us = sound_us = core_us = geometry_us = video_us = 0;
            presented = skipped = 0;
            if (game->board().io().eeprom_dirty) {
                save(*game);
                game->board().io().eeprom_dirty = false;
            }
        }
    }
}

} // namespace

void __cdecl main() {
    try {
        run();
    } catch (const std::exception &error) {
        log("Daytona: fatal: %s\n", error.what());
    }
    for (;;) Sleep(1000);
}
