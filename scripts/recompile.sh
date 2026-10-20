#!/usr/bin/env sh
# Recompile the game's i960 code and its TGP program to native C++ and build
# them: a wrapper for scripts/recompile.py (the same steps on every OS).
#   scripts/recompile.sh [--build-dir build]
set -eu
exec python3 "$(dirname "$0")/recompile.py" "$@"
