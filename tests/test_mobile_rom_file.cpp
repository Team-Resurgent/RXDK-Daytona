// ROM-free import tests. The SDL shim only supplies streams and app paths;
// Android's document provider and permission grant still need a device test.
#include "app/rom_file.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>

namespace fs = std::filesystem;
namespace {
fs::path root;
std::string pref, bytes, opened;
std::size_t chunk = 3, error_after = std::numeric_limits<std::size_t>::max();
int streams = 0, cases = 0;
bool no_pref = false, open_error = false, close_error = false, stall = false;
bool eof_with_data = false;

void require(bool ok, const char *what) {
    if (!ok) throw std::runtime_error(what);
}
[[maybe_unused]] std::string read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
[[maybe_unused]] void write(const fs::path &p, const std::string &s) {
    std::ofstream f(p, std::ios::binary);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    require(bool(f), "test file write failed");
}
void no_temporary_files() {
    for (const auto &entry : fs::directory_iterator(root))
        require(entry.path().extension() != ".tmp", "temporary import leaked");
    require(streams == 0, "SDL input stream leaked");
}
template <typename F> void fails(F fn) {
    bool failed = false;
    try { fn(); } catch (const std::runtime_error &) { failed = true; }
    require(failed, "expected import failure");
    no_temporary_files();
    ++cases;
}
} // namespace

struct SDL_IOStream { std::size_t pos = 0; SDL_IOStatus status = SDL_IO_STATUS_READY; };
char *SDL_GetPrefPath(const char *, const char *) {
    if (no_pref) return nullptr;
    char *out = static_cast<char *>(std::malloc(pref.size() + 1));
    if (out) std::memcpy(out, pref.c_str(), pref.size() + 1);
    return out;
}
void SDL_free(void *p) { std::free(p); }
const char *SDL_GetError() { return "injected provider error"; }
std::uint64_t SDL_GetTicksNS() { return 1; } // exercise unique names at identical ticks
SDL_IOStream *SDL_IOFromFile(const char *file, const char *mode) {
    require(std::strcmp(mode, "rb") == 0, "provider must be opened read-only");
    opened = file;
    if (open_error) return nullptr;
    ++streams;
    return new SDL_IOStream;
}
std::size_t SDL_ReadIO(SDL_IOStream *s, void *out, std::size_t n) {
    if (s->pos >= error_after) { s->status = SDL_IO_STATUS_ERROR; return 0; }
    if (stall) return 0;
    if (s->pos == bytes.size()) { s->status = SDL_IO_STATUS_EOF; return 0; }
    n = std::min({n, chunk, bytes.size() - s->pos, error_after - s->pos});
    std::memcpy(out, bytes.data() + s->pos, n);
    s->pos += n;
    if (eof_with_data && s->pos == bytes.size()) s->status = SDL_IO_STATUS_EOF;
    return n;
}
SDL_IOStatus SDL_GetIOStatus(SDL_IOStream *s) { return s->status; }
bool SDL_CloseIO(SDL_IOStream *s) { --streams; delete s; return !close_error; }

int main(int argc, char **argv) {
    try {
        require(argc == 2, "pass a temporary test directory");
        root = argv[1];
        fs::create_directories(root);
        pref = root.string() + "/";
        const std::string uri = "content://com.android.providers.downloads.documents/document/raw%3A%2Fstorage%2Femulated%2F0%2FDownload%2Fdaytona93.zip";
        require(app::RomFile::is_content_uri(uri), "content URI not recognised");
        require(!app::RomFile::is_content_uri("roms/daytona93.zip"), "normal path classified as URI");
        {
            app::RomFile plain("roms/daytona93.zip");
            require(plain.path() == "roms/daytona93.zip", "normal path changed");
            require(plain.commit() == plain.path(), "normal path commit changed");
            require(opened.empty(), "normal path opened through Android");
        }
        ++cases;
#ifdef SDL_PLATFORM_ANDROID
        const fs::path destination = root / "imported-daytona93.rom";
        // Opaque document ID, short reads, embedded zero bytes and replacement.
        bytes = std::string("PK\003\004\0", 5) + std::string(180000, 'x');
        chunk = 1111;
        write(destination, "previous import");
        {
            app::RomFile imported(uri);
            require(opened == uri, "URI was decoded or rewritten");
            require(read(imported.path()) == bytes, "streaming copy is not byte-identical");
            require(read(destination) == "previous import", "old import replaced before validation");
            require(imported.commit() == destination.string(), "wrong committed path");
            require(imported.commit() == destination.string(), "second commit not idempotent");
        }
        require(read(destination) == bytes, "committed content mismatch");
        no_temporary_files();
        ++cases;

        const std::string previous = bytes;
        bytes = "unverified data";
        {
            app::RomFile rejected(uri);
            require(fs::exists(rejected.path()), "staged file missing");
            // Not calling commit models a rejected ROM manifest.
        }
        require(read(destination) == previous, "validation failure replaced the good import");
        no_temporary_files();
        ++cases;

        { // Two selections in the same clock tick must not use the same file.
            app::RomFile first(uri), second(uri);
            require(first.path() != second.path(), "temporary filename collision");
        }
        no_temporary_files();
        ++cases;

        open_error = true;
        fails([&] { app::RomFile denied(uri); });
        open_error = false;
        no_pref = true;
        fails([&] { app::RomFile missing_storage(uri); });
        no_pref = false;
        bytes.clear();
        fails([&] { app::RomFile empty(uri); });
        bytes = "PK data with read failure";
        error_after = 7;
        fails([&] { app::RomFile partial(uri); });
        error_after = std::numeric_limits<std::size_t>::max();
        close_error = true;
        fails([&] { app::RomFile failed_close(uri); });
        close_error = false;
        stall = true;
        fails([&] { app::RomFile failed_read(uri); });
        stall = false;
        require(read(destination) == previous, "failed read replaced previous import");

        // A storage creation error must close the already opened provider.
        const std::string good_pref = pref;
        pref += "missing/subdirectory/";
        fails([&] { app::RomFile bad_output(uri); });
        pref = good_pref;

        // A failing rename must not leave a staged file behind.
        fs::remove(destination);
        fs::create_directory(destination);
        write(destination / "keep", "unchanged");
        fails([&] { app::RomFile imported(uri); imported.commit(); });
        require(read(destination / "keep") == "unchanged", "rename failure removed the existing destination");
        fs::remove_all(destination);

        // EOF can accompany the last bytes; extensionless IDs work for 7z too.
        bytes = std::string("7z\274\257\047\034", 6) + "example bytes, not a ROM";
        eof_with_data = true;
        {
            app::RomFile imported("content://provider/document/42");
            imported.commit();
        }
        require(read(destination) == bytes, "7z-style bytes truncated at EOF");
        no_temporary_files();
        ++cases;
        std::cout << cases << " Android-path import cases passed (host SDL shim)\n";
#else
        fails([&] { app::RomFile not_android(uri); });
        require(opened.empty(), "desktop tried to open an Android provider");
        std::cout << cases << " desktop-path cases passed\n";
#endif
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "mobile ROM import test failed: " << e.what() << '\n';
        return 1;
    }
}
