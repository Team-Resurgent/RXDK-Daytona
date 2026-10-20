#ifdef NDEBUG
#undef NDEBUG
#endif
#include "polygon_order.h"
#include <cassert>
#include <cstdio>
#include <ctime>
#include <numeric>
#include <random>
#include <string_view>

namespace {
std::vector<size_t> reference(const std::vector<rt::GeoPoly> &polys) {
    std::vector<size_t> order(polys.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (polys[a].window != polys[b].window) return polys[a].window > polys[b].window;
        if (polys[a].z != polys[b].z) return polys[a].z < polys[b].z;
        return a > b;
    });
    return order;
}
volatile size_t sink;
}

int main(int argc, char **argv) {
    vita::PolygonOrder order;
    std::mt19937 random(0x20a0de);
    uint64_t checked = 0;
    for (int trial = 0; trial < 1000; ++trial) {
        size_t count = trial < 68 ? size_t(trial) : size_t(random() % 8001);
        std::vector<rt::GeoPoly> polys(count);
        for (auto &poly : polys) {
            poly.window = trial % 4 == 0 ? 0 : trial % 4 == 1 ? 255 : uint8_t(random());
            poly.z = trial % 7 == 0 ? 0 : trial % 7 == 1 ? 65535 :
                     trial % 7 == 2 ? uint16_t(random() % 8) : uint16_t(random());
        }
        const auto expected = reference(polys);
        const auto &actual = order.sort(polys);
        assert(actual.size() == expected.size());
        for (size_t i = 0; i < expected.size(); ++i) assert(actual[i].index == expected[i]);
        checked += count;
    }
    std::printf("Vita polygon order: %llu exact indices, empty/small lists, depth/window boundaries and ties passed\n",
                static_cast<unsigned long long>(checked));
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
        std::vector<rt::GeoPoly> polys(2000);
        for (auto &p : polys) { p.window = 0; p.z = uint16_t(random()); }
        constexpr unsigned rounds = 5000;
        size_t sum = 0;
        const auto begin = std::clock();
        for (unsigned i = 0; i < rounds; ++i) {
            polys[i % polys.size()].z ^= uint16_t(i);
            const auto result = reference(polys);
            sum += result[i % result.size()];
        }
        const auto middle = std::clock();
        for (unsigned i = 0; i < rounds; ++i) {
            polys[i % polys.size()].z ^= uint16_t(i);
            const auto &result = order.sort(polys);
            sum += result[i % result.size()].index;
        }
        const auto end = std::clock();
        sink = sum;
        std::printf("Host CPU time per 2000-polygon list: reference %.2f us; integer keys %.2f us (not Vita FPS)\n",
                    double(middle - begin) * 1e6 / CLOCKS_PER_SEC / rounds,
                    double(end - middle) * 1e6 / CLOCKS_PER_SEC / rounds);
    }
}
