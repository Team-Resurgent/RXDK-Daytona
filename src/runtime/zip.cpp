#include "runtime/zip.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>

namespace rt {

uint32_t crc32(const uint8_t *data, size_t len, uint32_t crc) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

namespace {

uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

// --- inflate (RFC 1951) ---------------------------------------------------------

struct Bits {
    const uint8_t *p;
    size_t len, pos = 0;
    uint32_t buf = 0;
    int n = 0;
    uint32_t get(int k) {
        while (n < k) {
            if (pos >= len) throw ZipError("deflate stream truncated");
            buf |= uint32_t(p[pos++]) << n;
            n += 8;
        }
        const uint32_t v = buf & ((1u << k) - 1);
        buf >>= k;
        n -= k;
        return v;
    }
    void align() { buf = 0, n = 0; }
};

struct Huffman {
    uint16_t counts[16] = {};
    uint16_t symbols[320] = {};
    void build(const uint8_t *lengths, int num) {
        std::memset(counts, 0, sizeof counts);
        for (int i = 0; i < num; i++) counts[lengths[i]]++;
        counts[0] = 0;
        uint16_t offs[16] = {};
        for (int i = 1; i < 16; i++) offs[i] = uint16_t(offs[i - 1] + counts[i - 1]);
        for (int i = 0; i < num; i++)
            if (lengths[i]) symbols[offs[lengths[i]]++] = uint16_t(i);
    }
    int decode(Bits &b) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= int(b.get(1));
            const int count = counts[len];
            if (code - count < first) return symbols[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw ZipError("bad deflate code");
    }
};

const uint16_t kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

} // namespace

void inflate(const uint8_t *in, size_t in_len, std::vector<uint8_t> &out) {
    Bits b{in, in_len};
    size_t o = 0;
    auto put = [&](uint8_t v) {
        if (o >= out.size()) throw ZipError("deflate output longer than expected");
        out[o++] = v;
    };
    bool last = false;
    while (!last) {
        last = b.get(1);
        const uint32_t type = b.get(2);
        if (type == 0) { // stored
            b.align();
            if (b.pos + 4 > b.len) throw ZipError("deflate stream truncated");
            const uint16_t n = rd16(in + b.pos);
            b.pos += 4;
            if (b.pos + n > b.len) throw ZipError("deflate stream truncated");
            for (uint16_t i = 0; i < n; i++) put(in[b.pos + i]);
            b.pos += n;
            continue;
        }
        Huffman lit, dist;
        if (type == 1) { // fixed
            uint8_t l[288];
            for (int i = 0; i < 144; i++) l[i] = 8;
            for (int i = 144; i < 256; i++) l[i] = 9;
            for (int i = 256; i < 280; i++) l[i] = 7;
            for (int i = 280; i < 288; i++) l[i] = 8;
            lit.build(l, 288);
            uint8_t d[30];
            std::memset(d, 5, sizeof d);
            dist.build(d, 30);
        } else if (type == 2) { // dynamic
            const int hlit = int(b.get(5)) + 257, hdist = int(b.get(5)) + 1, hclen = int(b.get(4)) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {};
            for (int i = 0; i < hclen; i++) cl[order[i]] = uint8_t(b.get(3));
            Huffman clh;
            clh.build(cl, 19);
            uint8_t lens[320] = {};
            for (int i = 0; i < hlit + hdist;) {
                const int sym = clh.decode(b);
                if (sym < 16) lens[i++] = uint8_t(sym);
                else {
                    int rep = 0;
                    uint8_t val = 0;
                    if (sym == 16) {
                        if (i == 0) throw ZipError("bad deflate lengths");
                        val = lens[i - 1];
                        rep = 3 + int(b.get(2));
                    } else if (sym == 17) rep = 3 + int(b.get(3));
                    else rep = 11 + int(b.get(7));
                    if (i + rep > hlit + hdist) throw ZipError("bad deflate lengths");
                    while (rep--) lens[i++] = val;
                }
            }
            lit.build(lens, hlit);
            dist.build(lens + hlit, hdist);
        } else {
            throw ZipError("bad deflate block type");
        }
        for (;;) {
            const int sym = lit.decode(b);
            if (sym < 256) put(uint8_t(sym));
            else if (sym == 256) break;
            else {
                const int li = sym - 257;
                if (li >= 29) throw ZipError("bad deflate length");
                const size_t len = kLenBase[li] + b.get(kLenExtra[li]);
                const int di = dist.decode(b);
                if (di >= 30) throw ZipError("bad deflate distance");
                const size_t d = kDistBase[di] + b.get(kDistExtra[di]);
                if (d > o) throw ZipError("bad deflate distance");
                for (size_t k = 0; k < len; k++) put(out[o - d]);
            }
        }
    }
    if (o != out.size()) throw ZipError("deflate output shorter than expected");
}

Zip::Zip(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw ZipError("cannot open " + path);
    data_.assign(std::istreambuf_iterator<char>(f), {});
    // end of central directory: the last 0x06054b50 within 64 KiB of the end
    const size_t n = data_.size();
    if (n < 22) throw ZipError(path + " is not a zip file");
    size_t eocd = SIZE_MAX;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 22 + 65535;)
        if (rd32(&data_[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd == SIZE_MAX) throw ZipError(path + " is not a zip file");
    const uint16_t count = rd16(&data_[eocd + 10]);
    size_t p = rd32(&data_[eocd + 16]);
    for (uint16_t k = 0; k < count; k++) {
        if (p + 46 > n || rd32(&data_[p]) != 0x02014b50) throw ZipError(path + ": bad central directory");
        Entry e;
        e.method = rd16(&data_[p + 10]);
        e.crc = rd32(&data_[p + 16]);
        e.csize = rd32(&data_[p + 20]);
        e.usize = rd32(&data_[p + 24]);
        const uint16_t nl = rd16(&data_[p + 28]), xl = rd16(&data_[p + 30]), cl = rd16(&data_[p + 32]);
        e.local = rd32(&data_[p + 42]);
        if (p + 46 + nl > n) throw ZipError(path + ": bad central directory");
        std::string name(reinterpret_cast<const char *>(&data_[p + 46]), nl);
        const auto slash = name.find_last_of('/'); // members may sit in a folder
        if (slash != std::string::npos) name = name.substr(slash + 1);
        if (!name.empty()) entries_[name] = e;
        p += 46 + nl + xl + cl;
    }
}

std::vector<uint8_t> Zip::read(const std::string &name) const {
    const auto it = entries_.find(name);
    if (it == entries_.end()) throw ZipError("missing " + name);
    const Entry &e = it->second;
    if (size_t(e.local) + 30 > data_.size() || rd32(&data_[e.local]) != 0x04034b50) throw ZipError(name + ": bad local header");
    const size_t start = e.local + 30 + rd16(&data_[e.local + 26]) + rd16(&data_[e.local + 28]);
    if (start + e.csize > data_.size()) throw ZipError(name + ": truncated");
    std::vector<uint8_t> out(e.usize);
    if (e.method == 0) {
        if (e.csize != e.usize) throw ZipError(name + ": bad stored size");
        std::memcpy(out.data(), &data_[start], e.usize);
    } else if (e.method == 8) {
        inflate(&data_[start], e.csize, out);
    } else {
        throw ZipError(name + ": unsupported compression method " + std::to_string(e.method));
    }
    if (crc32(out.data(), out.size()) != e.crc) throw ZipError(name + ": CRC does not match the zip directory");
    return out;
}

} // namespace rt
