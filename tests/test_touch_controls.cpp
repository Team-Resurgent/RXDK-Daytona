#include "app/touch_controls.h"
#include <cassert>
#include <iostream>

int main() {
    app::TouchControls t;
    t.layout(30, 10, 800, 360);
    auto box = [&](int action) {
        for (const auto &b : t.boxes) if (b.action == action) return b;
        std::abort();
    };
    auto steer = box(t.Steering), gas = box(app::Accelerate), brake = box(app::Brake);
    t.down(1, 1, steer.x + steer.w, steer.y + 10);
    t.down(1, 2, gas.x + 5, gas.y + 5);
    t.down(2, 2, brake.x + 5, brake.y + 5);
    assert(t.steering && t.values[app::SteerRight] == 1);
    assert(t.values[app::Accelerate] == 1 && t.values[app::Brake] == 1);
    t.consumed();
    t.up(1, 2);
    assert(t.values[app::Accelerate] == 0 && t.values[app::Brake] == 1);
    t.move(1, 1, steer.x - 200, steer.y);
    assert(t.values[app::SteerLeft] == 1 && t.values[app::SteerRight] == 0);
    t.up(1, 1);
    assert(!t.steering && t.values[app::SteerLeft] == 0);
    t.clear();
    auto coin = box(app::Coin);
    t.down(1, 3, coin.x + 5, coin.y + 5);
    t.up(1, 3);
    assert(t.values[app::Coin] == 1); // short tap survives until a game frame
    t.consumed();
    assert(t.values[app::Coin] == 0);
    t.down(1, 3, coin.x + 5, coin.y + 5);
    t.cancel(1, 3);
    assert(t.values[app::Coin] == 0);
    t.down(1, 4, gas.x + 5, gas.y + 5);
    t.consumed();
    t.move(1, 4, brake.x + 5, brake.y + 5);
    assert(t.values[app::Accelerate] == 0 && t.values[app::Brake] == 0);
    auto menu = box(t.Menu);
    assert(t.down(1, 5, menu.x + 5, menu.y + 5));
    for (float v : t.values) assert(v == 0);
    for (const auto dims : {std::pair{480.f, 272.f}, {844.f, 390.f}, {2400.f, 1080.f}, {1024.f, 768.f}}) {
        t.layout(20, 10, dims.first, dims.second);
        for (size_t i = 0; i < t.boxes.size(); ++i) {
            const auto &a = t.boxes[i];
            assert(a.x >= 20 && a.y >= 10 && a.x + a.w <= 20 + dims.first && a.y + a.h <= 10 + dims.second);
            for (size_t j = i + 1; j < t.boxes.size(); ++j) {
                const auto &b = t.boxes[j];
                assert(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
            }
        }
    }
    t.down(1, 6, gas.x + 5, gas.y + 5);
    t.layout(0, 0, 480, 272);
    for (float v : t.values) assert(v == 0);
    std::cout << "Touch tests passed: multitouch, capture, release, taps, menu, resize\n";
}
