// The original Xbox: the ROM regions the board, geometrizer and TGP read
// through pages (rt::RomSource, the runtime's M2_DC_MEMORY, as on the
// Dreamcast). Daytona's regions are 48 MB of real data, more than a 64 MB
// console can hold beside everything else, but they compress to about
// 12 MB: stage_data.py writes each as independently deflated 16 KB blocks
// (<name>.z), all of which are loaded at the start. A block is unpacked
// (inflate.h) into an LRU cache when the game first reads one of its pages,
// so the game never waits for the disc: reading ROM from it as needed
// stalled scene changes by 100 ms and more.
//
// Past the blocks stored (the importer's zero padding, cut by
// stage_data.py) reads return one shared zero page.
#pragma once

#include "runtime/rom_source.h"

#include "inflate.h"

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

class PackedRom final : public rt::RomSource {
public:
    // In rt::RomRegion order.
    static constexpr const char *kNames[5] = {"program", "main_data", "polygons", "textures", "copro_data"};
    static constexpr uint32_t kBlock = 16384, kPagesPerBlock = kBlock / kPageSize;

    // cache_blocks: unpacked 16 KB blocks held at once.
    PackedRom(const std::string &directory, size_t cache_blocks) {
        for (int r = 0; r < 5; ++r) {
            Region &region = regions_[r];
            region.file = read_file(directory + kNames[r] + ".z");
            const std::vector<uint8_t> &f = region.file;
            if (f.size() < 16 || std::memcmp(f.data(), "M2Z1", 4) != 0 || u32(f, 4) != kBlock)
                throw std::runtime_error(std::string("bad packed ROM region ") + kNames[r]);
            region.size = u32(f, 8);
            region.blocks = u32(f, 12);
            region.data = 16 + (size_t(region.blocks) + 1) * 4;
            if (region.data > f.size() || region.data + u32(f, 16 + size_t(region.blocks) * 4) > f.size() ||
                size_t(region.blocks) * kBlock > region.size)
                throw std::runtime_error(std::string("bad packed ROM region ") + kNames[r]);
            region.slot.assign(region.blocks, kNone);
            packed_bytes_ += f.size();
        }
        slots_.resize(cache_blocks);
        data_ = static_cast<uint8_t *>(std::malloc(slots_.size() * size_t(kBlock)));
        if (!data_) throw std::runtime_error("no memory for the ROM block cache");
        // All slots free, in a list from head_ (most recent) to tail_ (next victim).
        for (uint32_t i = 0; i < slots_.size(); ++i) {
            slots_[i].prev = i ? i - 1 : kNone;
            slots_[i].next = i + 1 < slots_.size() ? i + 1 : kNone;
        }
        head_ = slots_.empty() ? kNone : 0;
        tail_ = slots_.empty() ? kNone : uint32_t(slots_.size() - 1);
    }
    ~PackedRom() override { std::free(data_); }

    uint32_t size(rt::RomRegion region) const override { return regions_[int(region)].size; }

    const uint8_t *page(rt::RomRegion r, uint32_t index) override {
        Region &region = regions_[int(r)];
        const uint32_t block = index / kPagesPerBlock;
        if (block >= region.blocks) return zero_;
        uint32_t slot = region.slot[block];
        if (slot == kNone) slot = unpack(int(r), block);
        else touch(slot);
        return data_ + size_t(slot) * kBlock + size_t(index % kPagesPerBlock) * kPageSize;
    }

    size_t packed_bytes() const { return packed_bytes_; }
    size_t cache_bytes() const { return slots_.size() * size_t(kBlock); }
    uint64_t misses = 0, unpack_us = 0;

private:
    static constexpr uint32_t kNone = 0xffffffffu;
    struct Region {
        std::vector<uint8_t> file;
        size_t data = 0;            // where the blocks' data starts in file
        uint32_t size = 0, blocks = 0;
        std::vector<uint32_t> slot; // block -> cache slot, or kNone
    };
    struct Slot { int region = -1; uint32_t block = 0, prev = kNone, next = kNone; };

    static uint32_t u32(const std::vector<uint8_t> &f, size_t at) {
        return uint32_t(f[at]) | uint32_t(f[at + 1]) << 8 | uint32_t(f[at + 2]) << 16 | uint32_t(f[at + 3]) << 24;
    }

    void unlink(uint32_t s) {
        Slot &slot = slots_[s];
        (slot.prev == kNone ? head_ : slots_[slot.prev].next) = slot.next;
        (slot.next == kNone ? tail_ : slots_[slot.next].prev) = slot.prev;
    }
    void touch(uint32_t s) {
        if (s == head_) return;
        unlink(s);
        slots_[s].prev = kNone;
        slots_[s].next = head_;
        if (head_ != kNone) slots_[head_].prev = s;
        head_ = s;
        if (tail_ == kNone) tail_ = s;
    }

    // The block into the least recently used slot.
    uint32_t unpack(int r, uint32_t block) {
        ++misses;
        forget(); // a page may be evicted below: no remembered pointer may outlive it
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        const uint32_t s = tail_;
        Slot &slot = slots_[s];
        if (slot.region >= 0) regions_[slot.region].slot[slot.block] = kNone;
        Region &region = regions_[r];
        const uint32_t begin = u32(region.file, 16 + size_t(block) * 4), end = u32(region.file, 20 + size_t(block) * 4);
        const uint8_t *in = region.file.data() + region.data + begin;
        uint8_t *out = data_ + size_t(s) * kBlock;
        if (end - begin == kBlock) {
            std::memcpy(out, in, kBlock);
        } else if (!Inflate::run(in, end - begin, out, kBlock)) {
            throw std::runtime_error(std::string("corrupt packed ROM block in ") + kNames[r]);
        }
        slot.region = r;
        slot.block = block;
        region.slot[block] = s;
        touch(s);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        unpack_us += uint64_t((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
        return s;
    }

    Region regions_[5];
    std::vector<Slot> slots_;
    uint32_t head_ = kNone, tail_ = kNone;
    uint8_t *data_ = nullptr;
    size_t packed_bytes_ = 0;
    alignas(16) static inline const uint8_t zero_[kPageSize] = {};
};

} // namespace xbox
