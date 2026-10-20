#pragma once
#include "app/controls.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace app {
// Raw finger capture, independent of ImGui's single mouse pointer. A finger
// owns its original control until release, so steering and pedals can overlap.
class TouchControls {
public:
    struct Box {
        float x, y, w, h;
        int action;
        const char *label;
        bool contains(float px, float py) const {
            return px >= x && px <= x + w && py >= y && py <= y + h;
        }
    };
    static constexpr int Steering = kNumActions, Menu = kNumActions + 1;
    std::vector<Box> boxes;
    std::array<float, kNumActions> values{};
    bool steering = false;
    void layout(float x, float y, float w, float h) {
        clear();
        boxes.clear();
        const float unit = std::min(w / 800.f, h / 360.f);
        const float bh = 48 * unit, pedal = 100 * unit;
        const int actions[] = {Menu, Coin, Start, View1, View2, View3, View4};
        const char *labels[] = {"Menu", "Coin", "Start", "V1", "V2", "V3", "V4"};
        for (int i = 0; i < 7; ++i)
            boxes.push_back({x + w * (.12f + i * .11f), y + 6 * unit, w * .10f, bh, actions[i], labels[i]});
        boxes.push_back({x + w * .02f, y + h - pedal - 8 * unit, w * .34f, pedal, Steering, "STEER"});
        boxes.push_back({x + w * .72f, y + h - pedal - 8 * unit, w * .12f, pedal, Brake, "BRAKE"});
        boxes.push_back({x + w * .86f, y + h - pedal - 8 * unit, w * .12f, pedal, Accelerate, "GAS"});
        boxes.push_back({x + w * .72f, y + h - pedal - bh - 16 * unit, w * .12f, bh, GearDown, "Gear -"});
        boxes.push_back({x + w * .86f, y + h - pedal - bh - 16 * unit, w * .12f, bh, GearUp, "Gear +"});
    }
    bool down(SDL_TouchID device, SDL_FingerID id, float x, float y) {
        up(device, id);
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (!boxes[i].contains(x, y)) continue;
            if (boxes[i].action == Menu) { clear(); return true; }
            if (fingers.size() < 16) {
                fingers.push_back({device, id, i, x, y});
                if (boxes[i].action < kNumActions) pressed[boxes[i].action] = 1;
            }
            update();
            break;
        }
        return false;
    }
    void move(SDL_TouchID device, SDL_FingerID id, float x, float y) {
        for (auto &f : fingers) if (f.device == device && f.id == id) { f.x = x; f.y = y; }
        update();
    }
    void up(SDL_TouchID device, SDL_FingerID id) {
        std::erase_if(fingers, [&](const Finger &f) { return f.device == device && f.id == id; });
        update();
    }
    void cancel(SDL_TouchID device, SDL_FingerID id) {
        for (const auto &f : fingers)
            if (f.device == device && f.id == id && boxes[f.box].action < kNumActions)
                pressed[boxes[f.box].action] = 0;
        up(device, id);
    }
    void consumed() { pressed.fill(0); update(); }
    void clear() { fingers.clear(); values.fill(0); pressed.fill(0); steering = false; }
private:
    struct Finger { SDL_TouchID device; SDL_FingerID id; size_t box; float x, y; };
    std::vector<Finger> fingers;
    std::array<float, kNumActions> pressed{};
    void update() {
        values = pressed; steering = false;
        for (const auto &f : fingers) {
            const auto &b = boxes[f.box];
            if (b.action == Steering) {
                if (steering) continue;
                steering = true;
                float v = std::clamp((f.x - b.x) / b.w * 2 - 1, -1.f, 1.f);
                const float magnitude = std::max(0.f, (std::abs(v) - .08f) / .92f);
                values[v < 0 ? SteerLeft : SteerRight] = magnitude;
            } else if (b.contains(f.x, f.y)) values[b.action] = 1;
        }
    }
};
} // namespace app
