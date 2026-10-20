// The original Xbox: the ROM regions the board, geometrizer and TGP read
// through pages (rt::RomSource, the runtime's M2_DC_MEMORY, as on the
// Dreamcast). Daytona's regions are 48 MB of real data, more than a 64 MB
// console can hold beside everything else, so they are read from D:\data a
// page at a time into an LRU cache (as platform/dreamcast/game/main.cpp's
// DiscRom, with an O(1) LRU for a cache many times larger). A miss that
// carries on from the last read in its region reads ahead kRun pages: the
// game's reads are mostly scattered pages, and reading ahead on every miss
// fills the cache with pages nobody asked for.
//
// stage_data.py stored each region up to its last nonzero page; past that
// (the importer's zero padding) reads return one shared zero page without
// touching the disc. regions.txt gives each region's full size.
#pragma once

#include "runtime/rom_source.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace xbox {

inline std::vector<uint8_t> read_file(const std::string &path) {
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) throw std::runtime_error("cannot open " + path);
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<uint8_t> bytes(length > 0 ? size_t(length) : 0);
    const size_t read = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) throw std::runtime_error("short read from " + path);
    return bytes;
}

class CachedRom final : public rt::RomSource {
public:
    // In rt::RomRegion order.
    static constexpr const char *kNames[5] = {"program", "main_data", "polygons", "textures", "copro_data"};

    // cache_pages: pages held at once (4 KB each); never more than the
    // regions' stored pages.
    CachedRom(const std::string &directory, size_t cache_pages) {
        const std::vector<uint8_t> table = read_file(directory + "regions.txt");
        const std::string text(table.begin(), table.end());
        size_t stored_pages = 0;
        for (int r = 0; r < 5; ++r) {
            const std::string key = std::string(kNames[r]) + " ";
            const size_t at = text.find(key);
            if (at == std::string::npos) throw std::runtime_error(std::string("regions.txt has no ") + kNames[r]);
            sizes_[r] = uint32_t(std::strtoul(text.c_str() + at + key.size(), nullptr, 10));
            const std::string path = directory + kNames[r] + ".bin";
            files_[r] = std::fopen(path.c_str(), "rb");
            if (!files_[r]) throw std::runtime_error("cannot open " + path);
            std::fseek(files_[r], 0, SEEK_END);
            const long stored = std::ftell(files_[r]);
            if (stored < 0 || uint32_t(stored) > sizes_[r] || stored % kPageSize)
                throw std::runtime_error("bad staged ROM region " + path);
            stored_[r] = uint32_t(stored) >> kPageBits;
            index_[r].assign(stored_[r], kNone);
            stored_pages += stored_[r];
        }
        slots_.resize(cache_pages < stored_pages ? cache_pages : stored_pages);
        data_ = static_cast<uint8_t *>(std::malloc(slots_.size() * kPageSize));
        if (!data_) throw std::runtime_error("no memory for the ROM page cache");
        // All slots free, in a list from head_ (most recent) to tail_ (next victim).
        for (uint32_t i = 0; i < slots_.size(); ++i) {
            slots_[i].prev = i ? i - 1 : kNone;
            slots_[i].next = i + 1 < slots_.size() ? i + 1 : kNone;
        }
        head_ = slots_.empty() ? kNone : 0;
        tail_ = slots_.empty() ? kNone : uint32_t(slots_.size() - 1);
    }
    ~CachedRom() override {
        for (FILE *file : files_)
            if (file) std::fclose(file);
        std::free(data_);
    }

    uint32_t size(rt::RomRegion region) const override { return sizes_[int(region)]; }

    const uint8_t *page(rt::RomRegion region, uint32_t index) override {
        const int r = int(region);
        if (index >= stored_[r]) return zero_;
        uint32_t slot = index_[r][index];
        if (slot == kNone) slot = load(r, index);
        else touch(slot);
        return data_ + size_t(slot) * kPageSize;
    }

    size_t cache_bytes() const { return slots_.size() * kPageSize; }
    uint64_t misses = 0, pages_read = 0, read_us = 0;

private:
    static constexpr uint32_t kNone = 0xffffffffu, kRun = 8;
    struct Slot { int region = -1; uint32_t index = 0, prev = kNone, next = kNone; };

    void unlink(uint32_t s) {
        Slot &slot = slots_[s];
        (slot.prev == kNone ? head_ : slots_[slot.prev].next) = slot.next;
        (slot.next == kNone ? tail_ : slots_[slot.next].prev) = slot.prev;
    }
    void push_front(uint32_t s) {
        slots_[s].prev = kNone;
        slots_[s].next = head_;
        if (head_ != kNone) slots_[head_].prev = s;
        head_ = s;
        if (tail_ == kNone) tail_ = s;
    }
    void touch(uint32_t s) {
        if (s == head_) return;
        unlink(s);
        push_front(s);
    }

    // Read index and the pages after it that are not cached, up to kRun.
    uint32_t load(int r, uint32_t index) {
        ++misses;
        forget(); // pages may be evicted below: no remembered pointer may outlive them
        uint32_t n = 1;
        if (index == next_[r])
            while (n < kRun && index + n < stored_[r] && index_[r][index + n] == kNone) ++n;
        next_[r] = index + n;
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        run_.resize(size_t(n) * kPageSize);
        std::fseek(files_[r], long(index) << kPageBits, SEEK_SET);
        if (std::fread(run_.data(), kPageSize, n, files_[r]) != n) throw std::runtime_error("ROM read failed");
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        read_us += uint64_t((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
        uint32_t first = kNone;
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t s = tail_; // the least recently used
            Slot &slot = slots_[s];
            if (slot.region >= 0) index_[slot.region][slot.index] = kNone;
            slot.region = r;
            slot.index = index + k;
            index_[r][index + k] = s;
            std::memcpy(data_ + size_t(s) * kPageSize, run_.data() + size_t(k) * kPageSize, kPageSize);
            touch(s);
            if (!k) first = s;
            ++pages_read;
        }
        touch(first); // the page asked for, most recent
        return first;
    }

    FILE *files_[5] = {};
    uint32_t sizes_[5] = {}, stored_[5] = {}, next_[5] = {};
    std::vector<uint32_t> index_[5]; // page -> cache slot, or kNone
    std::vector<Slot> slots_;
    uint32_t head_ = kNone, tail_ = kNone;
    uint8_t *data_ = nullptr;
    std::vector<uint8_t> run_;
    alignas(16) static inline const uint8_t zero_[kPageSize] = {};
};

} // namespace xbox
