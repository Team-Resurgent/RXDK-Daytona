#pragma once
#include "app/touch_controls.h"
#include "imgui.h"

namespace app {
inline void draw_touch_controls(const TouchControls &touch) {
    auto *draw = ImGui::GetForegroundDrawList();
    for (const auto &b : touch.boxes) {
        bool active = b.action == TouchControls::Steering ? touch.steering :
            b.action < kNumActions && touch.values[b.action] > 0;
        draw->AddRectFilled({b.x, b.y}, {b.x + b.w, b.y + b.h},
            active ? IM_COL32(40, 140, 220, 180) : IM_COL32(20, 28, 42, 115), 10);
        draw->AddRect({b.x, b.y}, {b.x + b.w, b.y + b.h}, IM_COL32(255, 255, 255, 160), 10);
        const float font_size = std::min(b.h * .32f, b.w / 5.f);
        const auto size = ImGui::GetFont()->CalcTextSizeA(font_size, b.w, 0, b.label);
        draw->AddText(ImGui::GetFont(), font_size,
            {b.x + (b.w - size.x) / 2, b.y + (b.h - size.y) / 2}, IM_COL32_WHITE, b.label);
        if (b.action == TouchControls::Steering) {
            const float v = touch.values[SteerRight] - touch.values[SteerLeft];
            draw->AddCircleFilled({b.x + b.w * (.5f + .45f * v), b.y + b.h * .86f}, b.h * .06f, IM_COL32_WHITE);
        }
    }
}
} // namespace app
