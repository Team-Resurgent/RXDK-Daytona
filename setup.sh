#!/usr/bin/env sh
# One-step setup for Linux and macOS: installs the build prerequisites, then
# fetches the pinned dependencies and builds (scripts/setup.py).
#
#   ./setup.sh [--test-extras] [--with-mame] [--build-mame]
#
# Linux: apt (Debian/Ubuntu), dnf (Fedora/RHEL), pacman (Arch) or zypper
# (openSUSE); uses sudo. macOS: Xcode command line tools and Homebrew.
# Put your own ROM set at roms/daytona93.zip (Deluxe '93) and/or
# roms/daytona.zip (Revision A, 1994) to have the game recompiled too.
set -eu
cd "$(dirname "$0")"

BUILD_MAME=0
for a in "$@"; do [ "$a" = "--build-mame" ] && BUILD_MAME=1; done

SUDO=""
[ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1 && SUDO="sudo"

case "$(uname -s)" in
Linux)
    if command -v apt-get >/dev/null 2>&1; then
        $SUDO apt-get update
        $SUDO apt-get install -y build-essential cmake ninja-build python3 python3-pip git clang pkg-config
        # SDL 3 (window, input, audio, SDL_GPU) and the Vulkan loader
        $SUDO apt-get install -y libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
            libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libdrm-dev libgbm-dev libegl-dev \
            libwayland-dev libdecor-0-dev libudev-dev libdbus-1-dev libvulkan1 mesa-vulkan-drivers zenity
        # MAME's build (validation only) needs SDL2 and friends
        [ "$BUILD_MAME" = 1 ] && $SUDO apt-get install -y libsdl2-dev libsdl2-ttf-dev libfontconfig-dev \
            libpulse-dev libasound2-dev libxinerama-dev libxi-dev qtbase5-dev
    elif command -v dnf >/dev/null 2>&1; then
        $SUDO dnf install -y gcc-c++ make cmake ninja-build python3 python3-pip git clang pkgconf
        $SUDO dnf install -y alsa-lib-devel pulseaudio-libs-devel libX11-devel libXext-devel libXrandr-devel \
            libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel libXtst-devel libxkbcommon-devel \
            libdrm-devel mesa-libgbm-devel mesa-libEGL-devel wayland-devel libdecor-devel systemd-devel dbus-devel \
            vulkan-loader mesa-vulkan-drivers zenity
        [ "$BUILD_MAME" = 1 ] && $SUDO dnf install -y SDL2-devel SDL2_ttf-devel fontconfig-devel \
            pulseaudio-libs-devel alsa-lib-devel libXinerama-devel libXi-devel
    elif command -v pacman >/dev/null 2>&1; then
        $SUDO pacman -S --needed --noconfirm base-devel cmake ninja python python-pip git clang pkgconf \
            alsa-lib libpulse libx11 libxext libxrandr libxcursor libxfixes libxi libxss libxtst libxkbcommon \
            libdrm mesa wayland libdecor systemd-libs dbus vulkan-icd-loader zenity
        [ "$BUILD_MAME" = 1 ] && $SUDO pacman -S --needed --noconfirm sdl2 sdl2_ttf fontconfig libpulse alsa-lib \
            libxinerama libxi
    elif command -v zypper >/dev/null 2>&1; then
        $SUDO zypper install -y gcc-c++ make cmake ninja python3 python3-pip git clang pkg-config \
            alsa-devel libpulse-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel \
            libXi-devel libXss-devel libXtst-devel libxkbcommon-devel libdrm-devel libgbm-devel Mesa-libEGL-devel \
            wayland-devel libdecor-devel systemd-devel dbus-1-devel libvulkan1 zenity
        [ "$BUILD_MAME" = 1 ] && $SUDO zypper install -y libSDL2-devel libSDL2_ttf-devel fontconfig-devel \
            libpulse-devel alsa-devel libXinerama-devel libXi-devel
    else
        echo "setup.sh: unknown package manager; install a C++20 compiler (GCC 11+ or Clang 14+)," \
             "CMake 3.20+, Ninja, Python 3 and Git, then run: python3 scripts/setup.py" >&2
        exit 1
    fi
    ;;
Darwin)
    if ! xcode-select -p >/dev/null 2>&1; then
        echo "Installing the Xcode command line tools (a dialog opens); run ./setup.sh again when it finishes."
        xcode-select --install || true
        exit 1
    fi
    if ! command -v brew >/dev/null 2>&1; then
        echo "setup.sh: Homebrew is needed (https://brew.sh). Install it with:" >&2
        echo '  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"' >&2
        echo "then run ./setup.sh again." >&2
        exit 1
    fi
    # SDL 3 needs only the system frameworks (Cocoa, Metal) on macOS
    brew install cmake ninja python git
    # MAME's build (validation only): its macOS front end is SDL 3, found
    # through pkg-config (without it MAME looks for an SDL3 framework instead)
    [ "$BUILD_MAME" = 1 ] && brew install sdl3 pkgconf
    ;;
*)
    echo "setup.sh: $(uname -s) not supported here; on Windows run setup.ps1" >&2
    exit 1
    ;;
esac

# Debian 11 and similar ship CMake older than 3.20: use pip's.
if ! cmake --version | head -1 | awk '{split($3,v,"."); exit !(v[1] > 3 || (v[1] == 3 && v[2] >= 20))}'; then
    python3 -m pip install --user --upgrade cmake
    PATH="$HOME/.local/bin:$PATH"
    export PATH
fi

exec python3 scripts/setup.py "$@"
