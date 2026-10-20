#!/usr/bin/env bash
# Opt-in native sequencer audit against the original statically compiled sound
# program. Requires the user's imported ROM cache and generated host build.
# The ROM set is the ROM_DIR's name (daytona93, or daytona with a build
# directory recompiled for it); M2_NVRAM=DIR starts from the app's saved
# EEPROM and backup RAM (Revision A needs a single cabinet set).
# No copyrighted code, tables, audio or event logs are stored in the repository.
set -euo pipefail
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-"$repo_root/build"}
frames=${2:-6000}
rom_dir=${3:-"$build_dir/rom_cache/daytona93"}
output_prefix=${4:-"$build_dir/native-sound-oracle"}
mode=${5:-race}
if [[ "$mode" != race && "$mode" != attract ]]; then
    printf 'Usage: %s [BUILD_DIR] [FRAMES] [ROM_DIR] [OUTPUT_PREFIX] [race|attract]\n' "$0" >&2
    exit 2
fi

cmake --build "$build_dir" --target m2run -j "${BUILD_JOBS:-2}"
set_name=$(basename -- "$rom_dir")
objects=("$build_dir"/CMakeFiles/m2run.dir/gen/"$set_name"/*.o
         "$build_dir"/CMakeFiles/m2run.dir/gen/"$set_name"_tgp/*.o
         "$build_dir"/CMakeFiles/m2run.dir/gen/"$set_name"_snd/*.o)
for object in "${objects[@]}"; do
    if [[ ! -f "$object" ]]; then
        printf 'Missing generated host object: %s\n' "$object" >&2
        exit 2
    fi
done
libraries=("$build_dir/libruntime.a")
if [[ -f "$build_dir/liblzma7z.a" ]]; then libraries+=("$build_dir/liblzma7z.a"); fi
libraries+=("$build_dir/libtrace.a" "$build_dir/libi960.a"
            "$build_dir/libsoftfloat.a" "$build_dir/libymfm.a")
"${CXX:-c++}" -std=c++20 -O2 -fno-fast-math -ffp-contract=off \
    -DM2_ROMSET="\"$set_name\"" -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -DTHREAD_LOCAL=__thread \
    -I"$repo_root/src" -I"$repo_root/extern/ymfm/src" \
    "$repo_root/tests/test_native_sound_oracle_rom.cpp" "${objects[@]}" "${libraries[@]}" \
    -o "$build_dir/test_native_sound_oracle_rom"
args=("$rom_dir" "$frames" "$output_prefix")
if [[ "$mode" == attract ]]; then args+=(attract); fi
"$build_dir/test_native_sound_oracle_rom" "${args[@]}"
