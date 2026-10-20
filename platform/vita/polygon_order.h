#pragma once

#include "runtime/geo.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vita {

// Same total order as the CPU reference: descending window, ascending depth,
// newest polygon first on ties. The painter renderer traverses it in reverse.
// Extract small integer keys once instead of repeatedly chasing GeoPoly data.
class PolygonOrder {
public:
    struct Entry { uint32_t key; size_t index; };

    const std::vector<Entry> &sort(const std::vector<rt::GeoPoly> &polys) {
        const size_t count = polys.size();
        first_.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const size_t index = count - 1 - i;
            first_[i] = {((255u - uint32_t(polys[index].window)) << 16) |
                         uint32_t(polys[index].z), index};
        }
        if (count <= 32) {
            std::sort(first_.begin(), first_.end(), [](const Entry &a, const Entry &b) {
                return a.key != b.key ? a.key < b.key : a.index > b.index;
            });
            return first_;
        }
        std::array<std::array<size_t, 256>, 3> counts{};
        for (const Entry &entry : first_)
            for (unsigned digit = 0; digit < 3; ++digit)
                ++counts[digit][(entry.key >> (digit * 8)) & 255u];
        second_.resize(count);
        auto *source = &first_, *destination = &second_;
        for (unsigned digit = 0; digit < 3; ++digit) {
            auto &offsets = counts[digit];
            size_t total = 0;
            bool single_bucket = false;
            for (size_t &bucket : offsets) {
                const size_t size = bucket;
                single_bucket |= size == count;
                bucket = total;
                total += size;
            }
            if (single_bucket) continue;
            for (const Entry &entry : *source)
                (*destination)[offsets[(entry.key >> (digit * 8)) & 255u]++] = entry;
            std::swap(source, destination);
        }
        return *source;
    }

private:
    std::vector<Entry> first_, second_;
};

} // namespace vita
