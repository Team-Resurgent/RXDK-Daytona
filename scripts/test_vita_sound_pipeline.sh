#!/usr/bin/env bash
# Opt-in native test; requires an already configured/generated host build and
# an imported Daytona93 ROM cache. Keeps generated game code out of the repo.
set -euo pipefail
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-"$repo_root/build"}
frames=${2:-6000}
rom_dir=${3:-"$build_dir/rom_cache/daytona93"}

cmake --build "$build_dir" --target m2run -j "${BUILD_JOBS:-2}"
objects=("$build_dir"/CMakeFiles/m2run.dir/gen/daytona93/*.o
         "$build_dir"/CMakeFiles/m2run.dir/gen/daytona93_tgp/*.o
         "$build_dir"/CMakeFiles/m2run.dir/gen/daytona93_snd/*.o)
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
"${CXX:-c++}" -std=c++20 -O2 -pthread -fno-fast-math -ffp-contract=off \
    -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -DTHREAD_LOCAL=__thread \
    -I"$repo_root/src" -I"$repo_root/tests/vita_audio_shim" -I"$repo_root/extern/ymfm/src" \
    "$repo_root/tests/test_vita_sound_pipeline_rom.cpp" "${objects[@]}" "${libraries[@]}" \
    -o "$build_dir/test_vita_sound_pipeline_rom"
"$build_dir/test_vita_sound_pipeline_rom" "$rom_dir" "$frames"
