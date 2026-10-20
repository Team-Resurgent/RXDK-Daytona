// HD texture replacement and dumping (renderer.h names the materials).
//
// Replacements: D:\hd\<key>.dds, <key> being a material's 16 hex digits.
// The texture is drawn as the original's greys are, times the polygon's
// colour; <key>_c.dds is a full-colour texture, drawn times the light only.
// DXT1, DXT3, DXT5 and uncompressed 32-bit DDS files load, any power-of-two
// size up to 2048, with or without mipmaps.
//
// Dumping: with D:\hd\dump.on (or T:\dump.on) present, each material the
// game draws is written once to T:\dump\<key>.tga at its source size: the
// greys and transparency the game drew, a starting point for a replacement.
#pragma once

#include <xtl.h>
#include <xgraphics.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xbox {

class HdTextures {
public:
    struct Loaded { IDirect3DTexture8 *texture = nullptr; size_t bytes = 0; bool colour = false; };

    HdTextures() {
        WIN32_FIND_DATAA found;
        HANDLE find = FindFirstFileA("D:\\hd\\*.dds", &found);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                uint64_t key = 0;
                bool colour = false;
                if (parse_name(found.cFileName, key, colour))
                    index_[key] = {std::string("D:\\hd\\") + found.cFileName, colour};
            } while (FindNextFileA(find, &found));
            FindClose(find);
        }
        dumping_ = GetFileAttributesA("D:\\hd\\dump.on") != INVALID_FILE_ATTRIBUTES ||
                   GetFileAttributesA("T:\\dump.on") != INVALID_FILE_ATTRIBUTES;
        if (dumping_) CreateDirectoryA("T:\\dump", nullptr);
    }

    size_t available() const { return index_.size(); }
    bool dumping() const { return dumping_; }
    // Whether dump() would write this one (dumping on, not written yet).
    bool wants_dump(uint64_t key) const { return dumping_ && !dumped_.count(key); }

    Loaded load(IDirect3DDevice8 *device, uint64_t key) {
        auto found = index_.find(key);
        if (found == index_.end()) return {};
        Loaded loaded = load_dds(device, found->second.path);
        loaded.colour = found->second.colour;
        if (!loaded.texture) index_.erase(found); // (a bad file: not tried again)
        return loaded;
    }

    // ARGB8888, w x h, as a 32-bit top-down TGA.
    void dump(uint64_t key, const uint32_t *pixels, uint32_t w, uint32_t h) {
        if (!dumped_.insert(key).second) return;
        char path[64];
        std::snprintf(path, sizeof(path), "T:\\dump\\%016llx.tga", (unsigned long long)key);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
        FILE *file = std::fopen(path, "wb");
        if (!file) return;
        const uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(w), uint8_t(w >> 8),
                                    uint8_t(h), uint8_t(h >> 8), 32, 0x28};
        std::fwrite(header, 1, sizeof(header), file);
        std::fwrite(pixels, 4, size_t(w) * h, file); // little-endian ARGB is TGA's BGRA
        std::fclose(file);
        ++dumps;
    }

    unsigned dumps = 0;

private:
    struct Entry { std::string path; bool colour; };
    std::unordered_map<uint64_t, Entry> index_;
    std::unordered_set<uint64_t> dumped_;
    bool dumping_ = false;

    static bool parse_name(const char *name, uint64_t &key, bool &colour) {
        key = 0;
        int digits = 0;
        for (; digits < 16; ++digits) {
            const char c = name[digits];
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) return false;
            key = key << 4 | uint64_t(v);
        }
        const char *rest = name + 16;
        colour = (rest[0] == '_' && (rest[1] == 'c' || rest[1] == 'C'));
        if (colour) rest += 2;
        return _stricmp(rest, ".dds") == 0;
    }

    static uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

    static Loaded load_dds(IDirect3DDevice8 *device, const std::string &path) {
        FILE *file = std::fopen(path.c_str(), "rb");
        if (!file) return {};
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        std::vector<uint8_t> data(size > 0 ? size_t(size) : 0);
        const bool read = !data.empty() && std::fread(data.data(), 1, data.size(), file) == data.size();
        std::fclose(file);
        if (!read || data.size() < 128 || std::memcmp(data.data(), "DDS ", 4) != 0) return {};
        const uint8_t *h = data.data() + 4;
        const uint32_t height = u32(h + 8), width = u32(h + 12);
        uint32_t levels = std::max<uint32_t>(1, u32(h + 24));
        const uint32_t pf_flags = u32(h + 76), fourcc = u32(h + 80), bits = u32(h + 84);
        auto pow2 = [](uint32_t v) { return v && !(v & (v - 1)) && v <= 2048; };
        if (!pow2(width) || !pow2(height)) return {};
        D3DFORMAT format;
        uint32_t block = 0; // bytes per 4x4 block (compressed), 0: 32-bit texels
        if ((pf_flags & 4) && fourcc == 0x31545844u) { format = D3DFMT_DXT1; block = 8; }       // "DXT1"
        else if ((pf_flags & 4) && fourcc == 0x33545844u) { format = D3DFMT_DXT3; block = 16; } // "DXT3"
        else if ((pf_flags & 4) && fourcc == 0x35545844u) { format = D3DFMT_DXT5; block = 16; } // "DXT5"
        else if ((pf_flags & 0x40) && bits == 32) format = D3DFMT_A8R8G8B8;
        else return {};
        auto level_bytes = [&](uint32_t w, uint32_t h) {
            return block ? size_t(std::max(1u, w / 4)) * std::max(1u, h / 4) * block : size_t(w) * h * 4;
        };
        // Only the levels the file holds in full (a truncated chain stops early).
        size_t offset = 128, total = 0;
        uint32_t have = 0;
        for (uint32_t l = 0, w = width, hh = height; l < levels; ++l, w = std::max(1u, w / 2), hh = std::max(1u, hh / 2)) {
            if (offset + total + level_bytes(w, hh) > data.size()) break;
            total += level_bytes(w, hh);
            ++have;
        }
        if (!have) return {};
        levels = have;
        IDirect3DTexture8 *texture = nullptr;
        if (FAILED(device->CreateTexture(width, height, levels, 0, format, 0, &texture))) return {};
        const uint8_t *src = data.data() + offset;
        for (uint32_t l = 0, w = width, hh = height; l < levels; ++l, w = std::max(1u, w / 2), hh = std::max(1u, hh / 2)) {
            D3DLOCKED_RECT locked;
            if (FAILED(texture->LockRect(l, &locked, nullptr, 0))) {
                texture->Release();
                return {};
            }
            if (block) std::memcpy(locked.pBits, src, level_bytes(w, hh)); // (DXT is not swizzled)
            else XGSwizzleRect(src, w * 4, nullptr, locked.pBits, w, hh, nullptr, 4);
            texture->UnlockRect(l);
            src += level_bytes(w, hh);
        }
        return {texture, total, false};
    }
};

} // namespace xbox
