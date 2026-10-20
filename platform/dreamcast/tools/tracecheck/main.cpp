// tracecheck: the desktop's side of a Dreamcast lockstep check. Per frame,
// the same line the Dreamcast frontend prints (platform/dreamcast/game):
// i960 and TGP instruction counts and a hash of the TGP's buffer RAM (where
// the geometrizer reads its display list), from the same start (no sound
// board, the given settings), so the first frame where the two differ can be
// found.
//
//   tracecheck IMAGES_DIR FIRST LAST [--inputs FILE] [--nvram DIR] [--draw-distance N]
//
// --draw-distance: as the Dreamcast frontend's (it changes the game's draw
// list, so the instruction counts with it).

#include "runtime/game_loop.h"
#include "../../../../tools/common/input_script.h"
#include "../../../../tools/common/nvram.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: tracecheck IMAGES_DIR FIRST LAST [--inputs FILE] [--nvram DIR] [--draw-distance N]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const int first = std::atoi(argv[2]), last = std::atoi(argv[3]);
    std::string inputs, nvram;
    for (int i = 4; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--inputs")) inputs = argv[i + 1];
        else if (!std::strcmp(argv[i], "--nvram")) nvram = argv[i + 1];
        else if (!std::strcmp(argv[i], "--draw-distance")) rt::GameLoop::set_draw_distance(std::atoi(argv[i + 1]));
    }
    try {
        rt::GameLoop game(dir, false);
        if (!nvram.empty()) tools::load_nvram(game, nvram);
        tools::Script script;
        if (!inputs.empty()) script.load(inputs);
        for (int frame = 1; frame <= last; frame++) {
            game.run_frame(script.at(game.board().frame()));
            (void)game.board().take_sound_bytes();
            if (frame < first) continue;
            uint64_t h = 0xcbf29ce484222325ULL;
            const uint32_t *buffer = game.board().tgp().buffer_data();
            for (int i = 0; i < 0x8000; i++) h = (h ^ buffer[i]) * 0x100000001b3ULL;
            std::printf("TRACE %d i960 %" PRIu64 " tgp %" PRIu64 " buffer %016" PRIx64 "\n", frame, game.instructions(),
                        game.board().tgp().tgp_instructions(), h);
        }
        return 0;
    } catch (const std::exception &e) {
        std::printf("tracecheck: stopped: %s\n", e.what());
        return 1;
    }
}
