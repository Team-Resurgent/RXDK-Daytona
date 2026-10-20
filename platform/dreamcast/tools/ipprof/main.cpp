// ipprof: where the i960 spends its instructions (desktop). Every 61st
// instruction the IP is counted (a lockstep callback; the generated code sets
// m_IP before each instruction), and the busiest addresses and 64-byte
// blocks are printed. A measurement only: the extra callbacks can move when
// an interrupt is taken, so the run is not the game's exact one.
//
// GameLoop keeps the CPU and the lockstep private; this tool alone opens
// them (the define below), so the runtime needs no accessor for it.
//
//   ipprof IMAGES_DIR FIRST LAST [--inputs FILE] [--nvram DIR] [--top N]

// The standard library first: only the runtime's own classes are opened.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define _ALLOW_KEYWORD_MACROS
#define private public
#include "runtime/game_loop.h"
#undef private
#include "../../../../tools/common/input_script.h"
#include "../../../../tools/common/nvram.h"

namespace {
constexpr uint64_t kEvery = 61;
std::map<uint32_t, uint64_t> g_ips;
bool g_counting = false;

void arm(rt::GameLoop &game) {
    game.ls_->add_callback(game.ls_->count + kEvery, [&game] {
        if (g_counting) ++g_ips[game.cpu_->m_IP];
        arm(game);
    });
}

template <typename Map> void print_top(const char *title, const Map &m, uint64_t total, int top) {
    std::vector<std::pair<uint64_t, uint32_t>> v;
    for (const auto &[ip, n] : m) v.push_back({n, ip});
    std::sort(v.rbegin(), v.rend());
    std::printf("%s\n", title);
    for (int i = 0; i < top && i < int(v.size()); i++)
        std::printf("  %08x  %6.2f%%  %llu\n", v[i].second, 100.0 * double(v[i].first) / double(total),
                    (unsigned long long)v[i].first);
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: ipprof IMAGES_DIR FIRST LAST [--inputs FILE] [--nvram DIR] [--top N]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const int first = std::atoi(argv[2]), last = std::atoi(argv[3]);
    std::string inputs, nvram;
    int top = 40;
    for (int i = 4; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--inputs")) inputs = argv[i + 1];
        else if (!std::strcmp(argv[i], "--nvram")) nvram = argv[i + 1];
        else if (!std::strcmp(argv[i], "--top")) top = std::atoi(argv[i + 1]);
    }
    try {
        rt::GameLoop game(dir, false);
        if (!nvram.empty()) tools::load_nvram(game, nvram);
        tools::Script script;
        if (!inputs.empty()) script.load(inputs);
        arm(game);
        for (int frame = 1; frame <= last; frame++) {
            g_counting = frame >= first;
            game.run_frame(script.at(game.board().frame()));
            (void)game.board().take_sound_bytes();
        }
        uint64_t total = 0;
        std::map<uint32_t, uint64_t> blocks;
        for (const auto &[ip, n] : g_ips) {
            total += n;
            blocks[ip & ~63u] += n;
        }
        std::printf("ipprof: frames %d-%d, %llu samples (every %llu instructions), last frame's count %llu\n", first,
                    last, (unsigned long long)total, (unsigned long long)kEvery,
                    (unsigned long long)game.instructions());
        print_top("64-byte blocks:", blocks, total, top);
        print_top("addresses:", g_ips, total, top);
        return 0;
    } catch (const std::exception &e) {
        std::printf("ipprof: stopped: %s\n", e.what());
        return 1;
    }
}
