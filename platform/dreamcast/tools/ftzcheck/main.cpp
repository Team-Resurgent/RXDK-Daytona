// ftzcheck: does flushing denormals change the game? The SH-4 FPU runs with
// FPSCR.DN set (denormal operands and results are zero; the self-test shows
// it), where the PC is IEEE. This runs a replay twice on the PC, normally and
// with SSE's flush-to-zero and denormals-are-zero on (the same treatment),
// and compares every frame: screen hash, i960 and TGP instruction counts.
// Identical runs mean the game never depends on a denormal in that replay.
//
//   ftzcheck IMAGES_DIR FRAMES [--inputs FILE] [--nvram DIR]

#include "runtime/game_loop.h"
#include "../../../../tools/common/input_script.h"
#include "../../../../tools/common/nvram.h"

#include <xmmintrin.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Frame {
    uint64_t screen, i960, tgp;
    bool operator==(const Frame &) const = default;
};

std::vector<Frame> play(const std::string &dir, uint64_t frames, const std::string &inputs, const std::string &nvram) {
    rt::GameLoop game(dir, false); // no reference sound board: game logic is the same, and faster
    if (!nvram.empty()) tools::load_nvram(game, nvram);
    tools::Script script;
    if (!inputs.empty()) script.load(inputs);
    std::vector<Frame> out;
    out.reserve(size_t(frames));
    for (uint64_t f = 0; f < frames; f++) {
        game.run_frame(script.at(game.board().frame()));
        (void)game.board().take_sound_bytes();
        out.push_back({game.board().video().screen_hash(), game.instructions(), game.board().tgp().tgp_instructions()});
    }
    return out;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ftzcheck IMAGES_DIR FRAMES [--inputs FILE] [--nvram DIR]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    std::string inputs, nvram;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--inputs")) inputs = argv[i + 1];
        else if (!std::strcmp(argv[i], "--nvram")) nvram = argv[i + 1];
    }
    try {
        const unsigned ieee = _mm_getcsr();
        const auto reference = play(dir, frames, inputs, nvram);
        _mm_setcsr(ieee | _MM_FLUSH_ZERO_ON | 0x0040); // FTZ, DAZ
        const auto flushed = play(dir, frames, inputs, nvram);
        _mm_setcsr(ieee);
        for (size_t f = 0; f < reference.size(); f++) {
            if (reference[f] == flushed[f]) continue;
            std::printf("ftzcheck: DIFFERENT from frame %zu: screen %016" PRIx64 " vs %016" PRIx64 ", i960 %" PRIu64
                        " vs %" PRIu64 ", TGP %" PRIu64 " vs %" PRIu64 "\n",
                        f + 1, reference[f].screen, flushed[f].screen, reference[f].i960, flushed[f].i960,
                        reference[f].tgp, flushed[f].tgp);
            return 1;
        }
        const Frame &last = reference.back();
        std::printf("ftzcheck: identical over %" PRIu64 " frames (screen hash, i960 %" PRIu64 " and TGP %" PRIu64
                    " instructions); last screen %016" PRIx64 "\n",
                    frames, last.i960, last.tgp, last.screen);
        return 0;
    } catch (const std::exception &e) {
        std::printf("ftzcheck: stopped: %s\n", e.what());
        return 1;
    }
}
