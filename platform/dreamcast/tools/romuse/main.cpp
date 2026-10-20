// romuse: which 4 KB pages of each ROM region the game reads, and when (the
// Dreamcast port's memory budget). Runs the unchanged desktop runtime, as
// m2run does, and watches the ROM buffers from outside it with Windows guard
// pages: each guarded page reports its first read and is re-armed every
// block of --block frames. No hooks in the runtime.
//
//   romuse IMAGES_DIR FRAMES --out FILE [--inputs FILE] [--nvram DIR] [--block N]
//          [--sound-log FILE]
//
// The buffers are found by content: the board reads program and main_data
// from the images it was given; the geometrizer, TGP board and sound board
// read their own copies of the polygon, texture, copro and PCM ROMs, which
// are found by searching the process's memory for each image. Only whole
// pages inside a buffer can be guarded; the partial pages at its ends (at
// most two per region) are not measured.
//
// --sound-log: no reference sound board (so pcm1/pcm2 are not read); the
// game's sound commands are written instead, a line per frame that sent
// any: "FRAME HEX-BYTES". Game logic runs the same either way.
//
// Output, for platform/dreamcast/scripts/rom_usage.py:
//   frames F block B blocks N
//   region NAME BYTES
//   page REGION PAGE HEX     (blocks read, bit k = block k, lowest first)
// Only pages that were read are listed.

#ifndef _WIN32
#error "romuse uses Windows guard pages"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "runtime/game_loop.h"
#include "../../../../tools/common/input_script.h"
#include "../../../../tools/common/nvram.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uintptr_t kPage = 4096;

struct Region {
    std::string name;
    uintptr_t base = 0;                       // the buffer
    size_t bytes = 0;
    uintptr_t lo = 0, hi = 0;                 // the whole pages inside it, guarded
    std::vector<std::vector<uint64_t>> pages; // per page of the buffer: blocks read, as bits
};

std::vector<Region> regions;
std::vector<uintptr_t> fired; // pages whose guard went off this block, to re-arm
size_t block = 0, words = 0;

LONG CALLBACK on_guard(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t a = uintptr_t(e->ExceptionRecord->ExceptionInformation[1]);
    for (Region &r : regions) {
        if (a < r.lo || a >= r.hi) continue;
        auto &bits = r.pages[(a - r.base) / kPage];
        if (bits.empty()) bits.resize(words);
        bits[block >> 6] |= uint64_t(1) << (block & 63);
        fired.push_back(a & ~(kPage - 1));
        return EXCEPTION_CONTINUE_EXECUTION; // the guard is cleared; the read is retried
    }
    return EXCEPTION_CONTINUE_SEARCH; // someone else's guard page
}

void guard(uintptr_t page, size_t bytes) {
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void *>(page), bytes, PAGE_READWRITE | PAGE_GUARD, &old))
        throw std::runtime_error("VirtualProtect failed");
}

// Every copy of the `bytes` at `image` in committed read-write memory, except
// the image itself.
std::vector<uintptr_t> find_copies(const uint8_t *image, size_t bytes) {
    std::vector<uintptr_t> found;
    const size_t probe = std::min<size_t>(bytes, 4096);
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t at = 0; VirtualQuery(reinterpret_cast<void *>(at), &mbi, sizeof mbi) == sizeof mbi;
         at = uintptr_t(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Protect != PAGE_READWRITE || mbi.RegionSize < bytes) continue;
        const uintptr_t start = uintptr_t(mbi.BaseAddress), end = start + mbi.RegionSize;
        // A large heap block starts a header's length into its own region.
        for (uintptr_t p = start; p < start + 0x10000 && p + bytes <= end; p += 16) {
            if (p == uintptr_t(image)) continue;
            if (std::memcmp(reinterpret_cast<const void *>(p), image, probe)) continue;
            if (!std::memcmp(reinterpret_cast<const void *>(p), image, bytes)) found.push_back(p);
        }
    }
    return found;
}

void add_region(const std::string &name, uintptr_t base, size_t bytes) {
    Region r;
    r.name = name;
    r.base = base;
    r.bytes = bytes;
    r.lo = (base + kPage - 1) & ~(kPage - 1);
    r.hi = (base + bytes) & ~(kPage - 1);
    r.pages.resize((bytes + kPage - 1) / kPage + 1);
    regions.push_back(std::move(r));
}

std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: romuse IMAGES_DIR FRAMES --out FILE [--inputs FILE] [--nvram DIR] [--block N] [--sound-log FILE]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    std::string out_path, inputs_path, nvram_dir, sound_log_path;
    uint64_t block_frames = 16;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--out")) out_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--inputs")) inputs_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--nvram")) nvram_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--block")) block_frames = std::strtoull(argv[i + 1], nullptr, 10);
        else if (!std::strcmp(argv[i], "--sound-log")) sound_log_path = argv[i + 1];
    }
    if (out_path.empty() || block_frames == 0) {
        std::fprintf(stderr, "romuse: --out FILE is required (and --block must be at least 1)\n");
        return 2;
    }
    const size_t blocks = size_t((frames + block_frames - 1) / block_frames);
    words = (blocks + 63) / 64;

    try {
        rt::M2Board::Images img;
        img.program = load(dir + "/program.bin");
        img.main_data = load(dir + "/main_data.bin");
        img.copro_tables = load(dir + "/copro_tables.bin");
        img.copro_data = load(dir + "/copro_data.bin");
        img.polygons = load(dir + "/polygons.bin");
        img.textures = load(dir + "/textures.bin");
        img.sound_program = load(dir + "/sound_program.bin");
        img.pcm1 = load(dir + "/pcm1.bin");
        img.pcm2 = load(dir + "/pcm2.bin");
        // The board keeps these vectors (moved, so the buffers stay put) and
        // reads program and main_data from them; the other images it keeps
        // are only the source of the copies the devices read.
        const struct { const char *name; const std::vector<uint8_t> *image; bool board_reads; } wanted[] = {
            {"program", &img.program, true},      {"main_data", &img.main_data, true},
            {"polygons", &img.polygons, false},   {"textures", &img.textures, false},
            {"copro_data", &img.copro_data, false}, {"pcm1", &img.pcm1, false}, {"pcm2", &img.pcm2, false},
        };
        struct Want { std::string name; uintptr_t own; size_t bytes; bool board_reads; };
        std::vector<Want> want;
        for (const auto &w : wanted) want.push_back({w.name, uintptr_t(w.image->data()), w.image->size(), w.board_reads});

        rt::GameLoop game(std::move(img), sound_log_path.empty());
        if (!nvram_dir.empty()) tools::load_nvram(game, nvram_dir);

        for (Want &w : want) {
            if (!sound_log_path.empty() && w.name.rfind("pcm", 0) == 0) continue;
            if (w.board_reads) { add_region(w.name, w.own, w.bytes); continue; }
            const auto copies = find_copies(reinterpret_cast<const uint8_t *>(w.own), w.bytes);
            if (copies.size() != 1)
                throw std::runtime_error(w.name + ": expected one device copy, found " + std::to_string(copies.size()));
            add_region(w.name, copies[0], w.bytes);
        }
        AddVectoredExceptionHandler(1, on_guard);
        for (const Region &r : regions) guard(r.lo, r.hi - r.lo);

        FILE *sound_log = nullptr;
        if (!sound_log_path.empty() && !(sound_log = std::fopen(sound_log_path.c_str(), "w")))
            throw std::runtime_error("cannot write " + sound_log_path);
        tools::Script script;
        if (!inputs_path.empty()) script.load(inputs_path);
        for (uint64_t f = 0; f < frames; f++) {
            const size_t b = size_t(f / block_frames);
            if (b != block) { // re-arm what fired in the last block
                for (uintptr_t p : fired) guard(p, kPage);
                fired.clear();
                block = b;
            }
            game.run_frame(script.at(game.board().frame()));
            if (sound_log) {
                const auto bytes = game.board().take_sound_bytes();
                if (!bytes.empty()) {
                    std::fprintf(sound_log, "%" PRIu64, game.board().frame());
                    for (uint8_t v : bytes) std::fprintf(sound_log, " %02x", v);
                    std::fprintf(sound_log, "\n");
                }
            }
        }
        for (const Region &r : regions) { // unguarded again: nothing else is measured
            DWORD old;
            VirtualProtect(reinterpret_cast<void *>(r.lo), r.hi - r.lo, PAGE_READWRITE, &old);
        }
        if (sound_log) std::fclose(sound_log);

        FILE *out = std::fopen(out_path.c_str(), "w");
        if (!out) throw std::runtime_error("cannot write " + out_path);
        std::fprintf(out, "frames %" PRIu64 " block %" PRIu64 " blocks %zu\n", frames, block_frames, blocks);
        for (const Region &r : regions) std::fprintf(out, "region %s %zu\n", r.name.c_str(), r.bytes);
        for (size_t ri = 0; ri < regions.size(); ri++) {
            const Region &r = regions[ri];
            size_t read = 0;
            for (size_t pi = 0; pi < r.pages.size(); pi++) {
                const auto &bits = r.pages[pi];
                if (bits.empty()) continue;
                ++read;
                std::fprintf(out, "page %zu %zu ", ri, pi);
                bool lead = true; // most significant word first, without leading zeros
                for (size_t w = words; w-- > 0;) {
                    if (lead && !bits[w] && w) continue;
                    std::fprintf(out, lead ? "%" PRIx64 : "%016" PRIx64, bits[w]);
                    lead = false;
                }
                std::fprintf(out, "\n");
            }
            std::printf("romuse: %-10s %6.2f of %6.2f MB read (%zu pages)\n", r.name.c_str(),
                        double(read * kPage) / 1048576.0, double(r.bytes) / 1048576.0, read);
        }
        std::fclose(out);
        std::printf("romuse: %" PRIu64 " frames, last screen hash %016" PRIx64 "\n", game.frames(),
                    game.board().video().screen_hash());
        return 0;
    } catch (const std::exception &e) {
        std::printf("romuse: stopped: %s\n", e.what());
        return 1;
    }
}
