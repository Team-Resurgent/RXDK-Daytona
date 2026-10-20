// ROM set archives: zip (our reader, src/runtime/zip.cpp) and 7z (the 7-Zip
// LZMA SDK's public-domain decoder), chosen by the file's signature.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rt {

class Archive {
public:
    struct Entry {
        uint64_t size = 0;
        uint32_t crc = 0;
        bool has_crc = false;
    };
    virtual ~Archive() = default;
    const std::map<std::string, Entry> &entries() const { return entries_; }
    // Decompressed contents; CRC checked against the archive's own when it has one.
    virtual std::vector<uint8_t> read(const std::string &name) = 0;

protected:
    std::map<std::string, Entry> entries_; // by file name (folders inside the archive dropped)
};

// Throws ZipError (unknown format, damaged archive, 7z without the SDK).
std::unique_ptr<Archive> open_archive(const std::string &path);

} // namespace rt
