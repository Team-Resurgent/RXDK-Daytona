// Stage Android document-provider URIs as ordinary files for the ROM readers.
// Keep this in the SDL frontend: the runtime and command-line tools need no SDL.
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace app {

class RomFile {
public:
    static bool is_content_uri(const std::string &path) {
        return path.compare(0, 10, "content://") == 0;
    }

    explicit RomFile(const std::string &source) : path_(source) {
        if (!is_content_uri(source)) return;
#ifdef SDL_PLATFORM_ANDROID
        // Do not decode a URI into /sdcard/...: the picker grants access to the
        // URI, not to the corresponding raw path. SDL opens it via Android.
        char *pref = SDL_GetPrefPath("daytona-recomp", "daytona93");
        if (!pref) throw std::runtime_error(std::string("Cannot locate ROM import storage: ") + SDL_GetError());
        const std::unique_ptr<char, decltype(&SDL_free)> owned_pref(pref, SDL_free);
        const std::string base(pref);
        static std::atomic<std::uint64_t> sequence{0};
        temporary_ = base + "rom-import-" + std::to_string(SDL_GetTicksNS()) + "-" +
                     std::to_string(sequence.fetch_add(1)) + ".tmp";
        // The archive loader detects ZIP/7z by signature, not by extension.
        destination_ = base + "imported-daytona93.rom";
        path_ = temporary_;
        try {
            copy(source);
        } catch (...) {
            discard();
            throw;
        }
#else
        throw std::runtime_error("An Android content:// ROM cannot be opened here; browse to a local ZIP or 7z.");
#endif
    }

    ~RomFile() { discard(); }
    RomFile(const RomFile &) = delete;
    RomFile &operator=(const RomFile &) = delete;

    const std::string &path() const { return path_; }

    // Call only after check_rom_set accepts the staged file. On Android the
    // same-directory rename atomically replaces the old import. Failed reads,
    // failed validation and failed renames leave the previous import intact.
    std::string commit() {
        if (temporary_.empty()) return path_;
        std::error_code ec;
        std::filesystem::rename(temporary_, destination_, ec);
        if (ec) throw std::runtime_error("Cannot save imported ROM: " + ec.message());
        path_ = destination_;
        temporary_.clear();
        return path_;
    }

private:
    std::string path_, temporary_, destination_;

    void discard() noexcept {
        if (temporary_.empty()) return;
        std::error_code ignored;
        std::filesystem::remove(temporary_, ignored);
    }

    void copy(const std::string &source) {
        using Stream = std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)>;
        Stream input(SDL_IOFromFile(source.c_str(), "rb"), SDL_CloseIO);
        if (!input) throw std::runtime_error(std::string("Cannot read selected ROM; use Browse to select it again: ") + SDL_GetError());
        std::ofstream output(temporary_, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot create a ROM import in app storage.");

        // Stream, rather than SDL_LoadFile: document providers need not support
        // seeking or report a size. Bound both working memory and disk usage.
        std::array<char, 64 * 1024> buffer;
        constexpr std::uint64_t kMaxBytes = 512ull * 1024 * 1024;
        std::uint64_t total = 0;
        for (;;) {
            const size_t n = SDL_ReadIO(input.get(), buffer.data(), buffer.size());
            const SDL_IOStatus status = SDL_GetIOStatus(input.get());
            if (n > kMaxBytes - total) throw std::runtime_error("ROM archive exceeds the 512 MiB import limit.");
            if (n) {
                output.write(buffer.data(), static_cast<std::streamsize>(n));
                if (!output) throw std::runtime_error("Cannot write the imported ROM; check free app storage.");
                total += n;
            }
            if (status == SDL_IO_STATUS_EOF) break;
            if (status != SDL_IO_STATUS_READY || !n)
                throw std::runtime_error(std::string("ROM import stopped before end of file: ") + SDL_GetError());
        }
        if (!total) throw std::runtime_error("The selected ROM archive is empty.");
        if (!SDL_CloseIO(input.release()))
            throw std::runtime_error(std::string("Cannot finish reading the selected ROM: ") + SDL_GetError());
        output.close();
        if (!output) throw std::runtime_error("Cannot finish saving the imported ROM; check free app storage.");
    }
};

} // namespace app
