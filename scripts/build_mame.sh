#!/usr/bin/env sh
# Build a Model-2-only MAME from extern/mame (fetch_mame.sh first).
# Output: extern/mame/m2 (the SUBTARGET name). Several tens of minutes on 4 cores.
set -eu
cd "$(dirname "$0")/../extern/mame"
# The build needs the whole tree; fetch_mame.sh checks out only what the
# tests read. Our patches are already applied to the tracked files.
[ "$(git config core.sparseCheckout)" = true ] && git sparse-checkout disable
# MAME's build scripts parse its layouts with Python's XML parser: use the
# first Python whose parser loads (a Homebrew Python can have a pyexpat built
# against a newer libexpat than the system's, and fail with "No parsers found").
PY=""
for p in python3 /usr/bin/python3 python; do
    if command -v "$p" >/dev/null 2>&1 && "$p" -c 'import xml.sax; xml.sax.make_parser()' >/dev/null 2>&1; then
        PY="$(command -v "$p")"
        break
    fi
done
[ -n "$PY" ] || { echo "build_mame: no Python with a working XML parser (pyexpat)"; exit 1; }
# No fused multiply-add: MAME is the oracle for code built with
# -ffp-contract=off. Clang contracts a*b+c by default where the CPU has FMA
# (arm64), which rounds once instead of twice: an Apple silicon MAME built
# without this differs from ours in the geometrizer's last bits (seen: a
# rasterizer word off by 1 at attract frame 173). x86-64 builds had no FMA.
make SUBTARGET=m2 SOURCES=src/mame/sega/model2.cpp TOOLS=0 USE_QTDEBUG=0 NOWERROR=1 SYMBOLS=0 OPTIMIZE=2 \
    ARCHOPTS=-ffp-contract=off PYTHON_EXECUTABLE="$PY" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" "$@"
