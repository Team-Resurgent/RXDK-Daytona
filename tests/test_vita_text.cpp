#include "../platform/vita/text.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
static std::vector<SDL_Rect> recorded;
static int calls;
int SDL_RenderFillRects(SDL_Renderer *, const SDL_Rect *rects, int count) {
    CHECK(count > 0 && count <= 256);
    recorded.insert(recorded.end(), rects, rects + count); ++calls; return 0;
}
int main() {
    SDL_Renderer renderer;
    vita::text(&renderer, "a", 7, 11, 2, 10, 1);
    constexpr unsigned rows[] = {14,17,17,31,17,17,17};
    size_t index = 0;
    for (int row = 0; row < 7; ++row) for (int bit = 0; bit < 5; ++bit)
        if (rows[row] & (16u >> bit)) {
            CHECK(index < recorded.size());
            const auto &p = recorded[index++];
            CHECK(p.x == 7 + bit * 2 && p.y == 11 + row * 2 && p.w == 2 && p.h == 2);
        }
    CHECK(index == 18 && index == recorded.size() && calls == 1);
    recorded.clear(); calls = 0;
    vita::text(&renderer, std::string(512, 'A'), 0, 0, 1, 64, 8);
    CHECK(recorded.size() == 512 * 18 && calls == 36);
    for (const auto &p : recorded) CHECK(p.x >= 0 && p.x < 384 && p.y >= 0 && p.y < 72);
    recorded.clear(); calls = 0;
    vita::text(&renderer, "A\nA", 0, 0, 1, 64, 1);
    CHECK(recorded.size() == 18 && calls == 1);
    recorded.clear(); calls = 0;
    vita::text(&renderer, "A", 0, 0, 0);
    vita::text(&renderer, "A", 0, 0, 1, 0);
    vita::text(&renderer, "A", 0, 0, 1, 64, 0);
    vita::text(&renderer, "   ", 0, 0);
    CHECK(recorded.empty() && calls == 0);
    std::puts("Vita text pixel/layout tests passed: 9,216 pixel rectangles in 36 bounded submissions");
}
