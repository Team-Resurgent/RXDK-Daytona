// M2_DC_MEMORY builds only (platform/dreamcast): the ROM regions read through
// pages the frontend supplies from a small cache, instead of whole images
// held in memory (16 MB of RAM, 44 MB of ROM). The board, geometrizer and TGP
// board ask for a 4 KB page at a time. Desktop builds do not define
// M2_DC_MEMORY and never include this.
#pragma once

#ifdef M2_DC_MEMORY
#include <cstdint>
#include <utility>

namespace rt {

enum class RomRegion : uint8_t { Program, MainData, Polygons, Textures, CoproData };

class RomSource {
public:
    static constexpr unsigned kPageBits = 12;
    static constexpr uint32_t kPageSize = 1u << kPageBits;
    virtual ~RomSource() = default;
    // Bytes in the region: the size of the image it replaces (a power of two
    // for Polygons, Textures and CoproData).
    virtual uint32_t size(RomRegion region) const = 0;
    // Page `index` of the region (bytes index * 4096 on). The pointer stays
    // valid until at least 8 more pages have been asked for: callers use it
    // at once.
    virtual const uint8_t *page(RomRegion region, uint32_t index) = 0;

    // page(), remembering the last page of each region: most reads follow
    // on in the same 4 KB, and page() is a virtual call and a cache lookup.
    // The source calls forget() whenever it loads a page (which may evict
    // one), so a remembered pointer is never stale.
#ifdef M2_DC_SPEED
    // (The last two pages of each region: a polygon's texture coordinates
    // and its texture header are in different pages, read in turn.)
    const uint8_t *page_fast(RomRegion region, uint32_t index) {
        Last *last = last_[int(region)];
        if (last[0].page && last[0].index == index) return last[0].page;
        if (last[1].page && last[1].index == index) {
            std::swap(last[0], last[1]);
            return last[0].page;
        }
        const uint8_t *p = page(region, index);
        last[1] = last[0];
        last[0] = {index, p};
        return p;
    }
#else
    const uint8_t *page_fast(RomRegion region, uint32_t index) {
        Last &last = last_[int(region)];
        if (last.page && last.index == index) return last.page;
        const uint8_t *p = page(region, index);
        last = {index, p};
        return p;
    }
#endif

    // A little-endian word at a byte offset (aligned), through page_fast().
    uint32_t dword(RomRegion region, uint32_t offset) {
        const uint8_t *p = page_fast(region, offset >> kPageBits) + (offset & (kPageSize - 1) & ~3u);
#if defined(M2_DC_SPEED) && defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        // The Dreamcast (little-endian; pages are aligned): one load.
        uint32_t v;
        __builtin_memcpy(&v, __builtin_assume_aligned(p, 4), 4);
        return v;
#else
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
#endif
    }
    uint16_t word(RomRegion region, uint32_t offset) {
        const uint8_t *p = page_fast(region, offset >> kPageBits) + (offset & (kPageSize - 1) & ~1u);
#if defined(M2_DC_SPEED) && defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        uint16_t v;
        __builtin_memcpy(&v, __builtin_assume_aligned(p, 2), 2);
        return v;
#else
        return uint16_t(p[0] | p[1] << 8);
#endif
    }

protected:
#ifdef M2_DC_SPEED
    void forget() {
        for (auto &region : last_)
            for (Last &last : region) last.page = nullptr;
    }

private:
    struct Last { uint32_t index = 0; const uint8_t *page = nullptr; };
    Last last_[5][2];
#else
    void forget() {
        for (Last &last : last_) last.page = nullptr;
    }

private:
    struct Last { uint32_t index = 0; const uint8_t *page = nullptr; };
    Last last_[5];
#endif
};

} // namespace rt
#endif
