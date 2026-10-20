#include "runtime/archive.h"

#include "runtime/zip.h"

#include <cstring>
#include <fstream>
#include <iterator>

#ifdef M2_HAVE_7Z
extern "C" {
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "Alloc.h"
}
#endif

namespace rt {

namespace {

class ZipArchive : public Archive {
public:
    explicit ZipArchive(const std::string &path) : zip_(path) {
        for (const auto &[name, e] : zip_.entries()) entries_[name] = Entry{e.usize, e.crc, true};
    }
    std::vector<uint8_t> read(const std::string &name) override { return zip_.read(name); }

private:
    Zip zip_;
};

#ifdef M2_HAVE_7Z
// The SDK reads through a seekable stream; ours is the file held in memory.
struct MemStream {
    ISeekInStream vt;
    const std::vector<uint8_t> *data;
    size_t pos;
};

SRes mem_read(ISeekInStreamPtr p, void *buf, size_t *size) {
    auto *s = reinterpret_cast<MemStream *>(const_cast<ISeekInStream *>(p));
    const size_t n = std::min(*size, s->data->size() - s->pos);
    std::memcpy(buf, s->data->data() + s->pos, n);
    s->pos += n;
    *size = n;
    return SZ_OK;
}

SRes mem_seek(ISeekInStreamPtr p, Int64 *pos, ESzSeek origin) {
    auto *s = reinterpret_cast<MemStream *>(const_cast<ISeekInStream *>(p));
    Int64 base = origin == SZ_SEEK_SET ? 0 : origin == SZ_SEEK_CUR ? Int64(s->pos) : Int64(s->data->size());
    const Int64 np = base + *pos;
    if (np < 0 || np > Int64(s->data->size())) return SZ_ERROR_READ;
    s->pos = size_t(np);
    *pos = np;
    return SZ_OK;
}

class SevenZipArchive : public Archive {
public:
    explicit SevenZipArchive(std::vector<uint8_t> data) : data_(std::move(data)) {
        alloc_ = g_Alloc;
        temp_.Alloc = SzAllocTemp;
        temp_.Free = SzFreeTemp;
        mem_.vt.Read = mem_read;
        mem_.vt.Seek = mem_seek;
        mem_.data = &data_;
        mem_.pos = 0;
        LookToRead2_CreateVTable(&look_, False);
        look_.buf = static_cast<Byte *>(ISzAlloc_Alloc(&alloc_, kBuf));
        look_.bufSize = kBuf;
        look_.realStream = &mem_.vt;
        LookToRead2_INIT(&look_)
        CrcGenerateTable();
        SzArEx_Init(&db_);
        if (!look_.buf || SzArEx_Open(&db_, &look_.vt, &alloc_, &temp_) != SZ_OK) {
            cleanup();
            throw ZipError("damaged or unsupported 7z archive");
        }
        for (UInt32 i = 0; i < db_.NumFiles; i++) {
            if (SzArEx_IsDir(&db_, i)) continue;
            std::string name = file_name(i);
            const auto slash = name.find_last_of("/\\");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            Entry e;
            e.size = SzArEx_GetFileSize(&db_, i);
            e.has_crc = SzBitWithVals_Check(&db_.CRCs, i);
            if (e.has_crc) e.crc = db_.CRCs.Vals[i];
            entries_[name] = e;
            index_[name] = i;
        }
    }
    ~SevenZipArchive() override { cleanup(); }

    std::vector<uint8_t> read(const std::string &name) override {
        const auto it = index_.find(name);
        if (it == index_.end()) throw ZipError("missing " + name);
        size_t offset = 0, processed = 0;
        // decodes the whole solid block once; later files in it come from the cache
        const SRes res =
            SzArEx_Extract(&db_, &look_.vt, it->second, &block_, &out_, &out_size_, &offset, &processed, &alloc_, &temp_);
        if (res != SZ_OK)
            throw ZipError(name + ": could not be decompressed from the 7z archive (error " + std::to_string(res) + ")");
        std::vector<uint8_t> v(out_ + offset, out_ + offset + processed);
        const Entry &e = entries_.at(name);
        if (e.has_crc && crc32(v.data(), v.size()) != e.crc) throw ZipError(name + ": CRC does not match the 7z archive");
        return v;
    }

private:
    static constexpr size_t kBuf = size_t(1) << 18;
    std::string file_name(UInt32 i) {
        const size_t len = SzArEx_GetFileNameUtf16(&db_, i, nullptr);
        std::vector<UInt16> w(len);
        SzArEx_GetFileNameUtf16(&db_, i, w.data());
        std::string s;
        for (size_t k = 0; k + 1 < len; k++) s += w[k] < 0x80 ? char(w[k]) : '?'; // ROM names are ASCII
        return s;
    }
    void cleanup() {
        if (out_) ISzAlloc_Free(&alloc_, out_), out_ = nullptr;
        SzArEx_Free(&db_, &alloc_);
        if (look_.buf) ISzAlloc_Free(&alloc_, look_.buf), look_.buf = nullptr;
    }

    std::vector<uint8_t> data_;
    ISzAlloc alloc_{}, temp_{};
    MemStream mem_{};
    CLookToRead2 look_{};
    CSzArEx db_{};
    std::map<std::string, UInt32> index_;
    UInt32 block_ = 0xFFFFFFFF;
    Byte *out_ = nullptr;
    size_t out_size_ = 0;
};
#endif

} // namespace

std::unique_ptr<Archive> open_archive(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw ZipError("cannot open " + path);
    unsigned char sig[6] = {};
    f.read(reinterpret_cast<char *>(sig), 6);
    if (sig[0] == 'P' && sig[1] == 'K') return std::make_unique<ZipArchive>(path);
    static const unsigned char k7z[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
    if (std::memcmp(sig, k7z, 6) == 0) {
#ifdef M2_HAVE_7Z
        f.seekg(0);
        std::vector<uint8_t> data{std::istreambuf_iterator<char>(f), {}};
        return std::make_unique<SevenZipArchive>(std::move(data));
#else
        throw ZipError("7z support was not built (run scripts/setup.py to fetch the LZMA SDK)");
#endif
    }
    throw ZipError(path + " is neither a zip nor a 7z archive");
}

} // namespace rt
