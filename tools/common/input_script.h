// Scripted inputs for the headless tools (m2run, m2gpushot): scripts/inputs.
#pragma once

#include "runtime/game_loop.h"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace tools {

// scripts/inputs format: "frames N", "<from>-<to> name=value" or "<at> name=value".
struct Script {
    struct Line {
        uint64_t from, to;
        std::string name;
        unsigned value;
    };
    std::vector<Line> lines;
    void load(const std::string &path) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("cannot open " + path);
        std::string s;
        while (std::getline(f, s)) {
            if (s.empty() || s[0] == '#' || s.compare(0, 6, "frames") == 0) continue;
            std::istringstream in(s);
            std::string range, kv;
            in >> range >> kv;
            const auto dash = range.find('-'), eq = kv.find('=');
            if (eq == std::string::npos) continue;
            Line l;
            l.from = std::stoull(range.substr(0, dash));
            l.to = dash == std::string::npos ? l.from : std::stoull(range.substr(dash + 1));
            l.name = kv.substr(0, eq);
            l.value = unsigned(std::stoul(kv.substr(eq + 1), nullptr, 0));
            lines.push_back(l);
        }
    }
    rt::Inputs at(uint64_t frame) const {
        rt::Inputs in;
        static const struct {
            const char *name;
            int port; // 0: IN0, 1: IN1
            uint8_t bit;
        } buttons[] = {{"coin", 0, 0x01}, {"test", 0, 0x04}, {"service", 0, 0x08}, {"start", 0, 0x10}, {"vr1", 0, 0x20},
                       {"vr2", 0, 0x40},  {"vr3", 0, 0x80},  {"vr4", 1, 0x01}};
        static const uint8_t gearvalue[5] = {0, 2, 1, 6, 5}; // MAME daytona_gearbox_r
        for (const Line &l : lines) {
            if (frame < l.from || frame > l.to) continue;
            if (l.name == "steer") in.steer = uint8_t(l.value);
            else if (l.name == "accel") in.accel = uint8_t(l.value);
            else if (l.name == "brake") in.brake = uint8_t(l.value);
            else if (l.name.compare(0, 4, "gear") == 0 && l.value) {
                const int g = l.name[4] - '0';
                if (g >= 0 && g < 5) in.in1 = uint8_t((in.in1 & ~0x70) | (gearvalue[g] << 4));
            } else
                for (const auto &b : buttons)
                    if (l.name == b.name && l.value) (b.port ? in.in1 : in.in0) &= uint8_t(~b.bit); // active low
        }
        return in;
    }
};

} // namespace tools
