#!/usr/bin/env bash
# Standalone ROM-free tests: no Android SDK, SDL checkout or generated game needed.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT
for PLATFORM in android desktop; do
    FLAGS=()
    if [[ "$PLATFORM" == android ]]; then FLAGS+=(-DSDL_PLATFORM_ANDROID=1); fi
    "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -fno-fast-math -ffp-contract=off \
        "${FLAGS[@]}" -I"$ROOT/tests/mobile_io_shim" -I"$ROOT/src" \
        "$ROOT/tests/test_mobile_rom_file.cpp" -o "$BUILD/test-$PLATFORM"
    "$BUILD/test-$PLATFORM" "$BUILD/data-$PLATFORM"
done
