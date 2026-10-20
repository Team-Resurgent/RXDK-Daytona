// m2import: write the memory images from the user's ROM set (zip or 7z; the
// set this build is for, M2_ROMSET) for
// the build: the recompilers read program.bin, tgp_program.bin and
// sound_program.bin; the
// checking tools read the rest. The same importer the game uses
// (src/runtime/rom_import.cpp); scripts/m2import.py is the zip-only
// reference it was checked against (identical images).
//
//   m2import ROMS.zip|ROMS.7z OUTDIR
//
// Output is game data: OUTDIR must be git-ignored (build/ is).

#include "runtime/rom_import.h"
#include "runtime/zip.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
void write(const std::string &path, const uint8_t *p, size_t n) {
    std::ofstream f(path, std::ios::binary);
    if (!f.write(reinterpret_cast<const char *>(p), std::streamsize(n))) throw std::runtime_error("cannot write " + path);
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: m2import ROMS.zip|ROMS.7z OUTDIR\n");
        return 2;
    }
    try {
        const rt::M2Board::Images img = rt::import_rom_set(argv[1]);
        const std::string out = argv[2];
        std::filesystem::create_directories(out);
        write(out + "/program.bin", img.program.data(), img.program.size());
        write(out + "/main_data.bin", img.main_data.data(), img.main_data.size());
        write(out + "/copro_data.bin", img.copro_data.data(), img.copro_data.size());
        write(out + "/copro_tables.bin", img.copro_tables.data(), img.copro_tables.size());
        write(out + "/polygons.bin", img.polygons.data(), img.polygons.size());
        write(out + "/textures.bin", img.textures.data(), img.textures.size());
        write(out + "/sound_program.bin", img.sound_program.data(), img.sound_program.size());
        write(out + "/pcm1.bin", img.pcm1.data(), img.pcm1.size());
        write(out + "/pcm2.bin", img.pcm2.data(), img.pcm2.size());
        const std::vector<uint8_t> tgp = rt::tgp_program(img); // the TGP program the i960 uploads at boot
        write(out + "/tgp_program.bin", tgp.data(), tgp.size());
        std::printf("m2import: wrote program, main_data, copro_data, copro_tables, polygons, textures, sound_program, pcm1, pcm2, tgp_program to %s\n",
                    out.c_str());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2import: %s\n", e.what());
        return 1;
    }
}
