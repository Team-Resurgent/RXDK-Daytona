// Raw DEFLATE (RFC 1951) decoding of one whole block, in memory: the ROM
// regions are stored as independently compressed 16 KB blocks (rom.h,
// stage_data.py, Python's zlib at wbits -15), and a block is unpacked when
// the game first reads it. Huffman codes of up to kFast bits decode from one
// table lookup; longer ones (rare) bit by bit, canonically.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace xbox {

class Inflate {
public:
    // Decodes in[0, in_size) into exactly out_size bytes at out. False on
    // malformed data or a size mismatch.
    static bool run(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
        Inflate d(in, in_size, out, out_size);
        return d.blocks();
    }

private:
    static constexpr unsigned kFast = 10;
    struct Huffman {
        uint16_t fast[1u << kFast]; // symbol | length << 9; 0: longer than kFast bits
        uint16_t count[16];         // codes of each length
        uint16_t symbol[320];       // symbols in canonical order
    };

    const uint8_t *in_, *in_end_;
    uint8_t *out_, *out_begin_, *out_end_;
    uint64_t bits_ = 0;
    unsigned have_ = 0;
    // Zero bytes fed in past the input: the table lookups peek kFast bits
    // ahead, which near the end is past it. Only consuming them is an error.
    unsigned padding_ = 0;
    bool overrun() const { return padding_ * 8 > have_; }

    Inflate(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size)
        : in_(in), in_end_(in + in_size), out_(out), out_begin_(out), out_end_(out + out_size) {}

    void need(unsigned n) {
        while (have_ < n) {
            uint64_t byte = 0;
            if (in_ < in_end_) byte = *in_++;
            else ++padding_;
            bits_ |= byte << have_;
            have_ += 8;
        }
    }
    uint32_t take(unsigned n) {
        if (!n) return 0;
        need(n);
        const uint32_t v = uint32_t(bits_ & ((uint64_t(1) << n) - 1));
        bits_ >>= n;
        have_ -= n;
        return v;
    }

    static bool build(Huffman &h, const uint8_t *lengths, unsigned n) {
        std::memset(h.count, 0, sizeof(h.count));
        std::memset(h.fast, 0, sizeof(h.fast));
        for (unsigned i = 0; i < n; ++i) ++h.count[lengths[i]];
        h.count[0] = 0;
        uint16_t offs[16] = {};
        for (unsigned len = 1; len < 16; ++len) offs[len] = uint16_t(offs[len - 1] + h.count[len - 1]);
        for (unsigned i = 0; i < n; ++i)
            if (lengths[i]) h.symbol[offs[lengths[i]]++] = uint16_t(i);
        // Canonical codes, bit-reversed (DEFLATE sends them MSB first), into
        // the fast table for every fill of the bits after them.
        uint32_t code = 0;
        unsigned index = 0;
        for (unsigned len = 1; len < 16; ++len, code <<= 1) {
            for (unsigned k = 0; k < h.count[len]; ++k, ++code, ++index) {
                if (len > kFast) continue;
                uint32_t rev = 0;
                for (unsigned b = 0; b < len; ++b) rev |= ((code >> b) & 1) << (len - 1 - b);
                for (uint32_t fill = rev; fill < (1u << kFast); fill += 1u << len)
                    h.fast[fill] = uint16_t(h.symbol[index] | len << 9);
            }
        }
        return true;
    }

    int decode(const Huffman &h) {
        need(kFast);
        const uint16_t e = h.fast[bits_ & ((1u << kFast) - 1)];
        if (e) {
            const unsigned len = e >> 9;
            bits_ >>= len;
            have_ -= len;
            return e & 0x1ff;
        }
        // Longer than kFast bits: canonical decoding, a bit at a time.
        int code = 0, first = 0, index = 0;
        for (unsigned len = 1; len < 16; ++len) {
            code |= int(take(1));
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    bool codes(const Huffman &lit, const Huffman &dist) {
        static constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                   33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            const int sym = decode(lit);
            if (sym < 0 || overrun()) return false;
            if (sym < 256) {
                if (out_ == out_end_) return false;
                *out_++ = uint8_t(sym);
            } else if (sym == 256) {
                return true;
            } else {
                const int l = sym - 257;
                if (l >= 29) return false;
                const size_t length = kLenBase[l] + take(kLenExtra[l]);
                const int d = decode(dist);
                if (d < 0 || d >= 30) return false;
                const size_t distance = kDistBase[d] + take(kDistExtra[d]);
                if (distance > size_t(out_ - out_begin_) || length > size_t(out_end_ - out_)) return false;
                const uint8_t *from = out_ - distance;
                for (size_t i = 0; i < length; ++i) out_[i] = from[i]; // (may overlap: byte by byte)
                out_ += length;
            }
        }
    }

    bool stored() {
        bits_ >>= have_ & 7; // to the byte boundary
        have_ -= have_ & 7;
        const uint32_t len = take(16), nlen = take(16);
        if ((len ^ 0xffff) != nlen) return false;
        // Bytes still in the bit buffer first, then straight from the input.
        uint32_t left = len;
        while (left && have_) {
            if (out_ == out_end_) return false;
            *out_++ = uint8_t(take(8));
            --left;
        }
        if (left > size_t(in_end_ - in_) || left > size_t(out_end_ - out_)) return false;
        std::memcpy(out_, in_, left);
        in_ += left;
        out_ += left;
        return true;
    }

    bool fixed() {
        static Huffman lit, dist;
        static bool built = false;
        if (!built) {
            uint8_t lengths[288];
            for (unsigned i = 0; i < 288; ++i) lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            build(lit, lengths, 288);
            for (unsigned i = 0; i < 30; ++i) lengths[i] = 5;
            build(dist, lengths, 30);
            built = true;
        }
        return codes(lit, dist);
    }

    bool dynamic() {
        static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const unsigned nlen = take(5) + 257, ndist = take(5) + 1, ncode = take(4) + 4;
        if (nlen > 286 || ndist > 30) return false;
        uint8_t lengths[320] = {};
        for (unsigned i = 0; i < ncode; ++i) lengths[kOrder[i]] = uint8_t(take(3));
        Huffman code;
        build(code, lengths, 19);
        unsigned i = 0;
        uint8_t all[320] = {};
        while (i < nlen + ndist) {
            const int sym = decode(code);
            if (sym < 0 || overrun()) return false;
            if (sym < 16) {
                all[i++] = uint8_t(sym);
                continue;
            }
            uint8_t value = 0;
            unsigned repeat;
            if (sym == 16) {
                if (!i) return false;
                value = all[i - 1];
                repeat = 3 + take(2);
            } else if (sym == 17) {
                repeat = 3 + take(3);
            } else {
                repeat = 11 + take(7);
            }
            if (i + repeat > nlen + ndist) return false;
            while (repeat--) all[i++] = value;
        }
        Huffman lit, dist;
        build(lit, all, nlen);
        build(dist, all + nlen, ndist);
        return codes(lit, dist);
    }

    bool blocks() {
        for (bool last = false; !last;) {
            last = take(1);
            const uint32_t type = take(2);
            const bool ok = type == 0 ? stored() : type == 1 ? fixed() : type == 2 ? dynamic() : false;
            if (!ok || overrun()) return false;
        }
        return out_ == out_end_;
    }
};

} // namespace xbox
