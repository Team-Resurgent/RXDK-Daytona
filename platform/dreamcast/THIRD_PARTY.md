# Dreamcast target dependency record

The root [THIRD_PARTY.md](../../THIRD_PARTY.md) remains authoritative for
SoftFloat, ymfm and the shared runtime. No third-party source is vendored in
this folder; what the port builds with is fetched or unpacked into `extern/`
(git-ignored) or comes from the user's own installs.

## KallistiOS

Upstream: https://github.com/KallistiOS/KallistiOS (KOS License, "new" BSD;
`doc/LICENSE.md` in the tree; newlib and the addons carry their own).
Version 2.2.1, GitHub commit `857e4e69` (2025-08-30). On Windows it is
unpacked from DreamSDK R4's own offline package,
`opt/dreamsdk/packages/kallisti-offline-src.7z` (SHA-256
`751aa3bf657410c045ef9ea03c3287dd0452c8313361f21438b4f77ea53e3480`,
`OFFLINE` file: `github-857e4e69-2025.08.30-offline`); elsewhere it is cloned
at that commit. Into `extern/kos-dc`, by `build_dreamcast.py kos`.

Changes: none to its source. The driver writes `environ.sh` from KOS's
`doc/environ.sh.sample` (`KOS_BASE` set to `extern/kos-dc`,
`KOS_SH4_PRECISION` `-m4-single`), copies the PC-side helper programs
(`utils/*/*.exe`: bin2c, scramble, makeip and others) already built in
DreamSDK's own KOS, and builds the kernel and addons.

Why this version and not DreamSDK's installed KOS: DreamSDK R4 installed KOS
git master (`d458073c`, 2026-10-01), newer than its prebuilt toolchain's
libstdc++; every C++ program using libstdc++'s exception support stopped at
startup on a KOS mutex assertion (see HANDOFF.md). KOS 2.2.1 is the version
DreamSDK R4's toolchains were packaged with.

## Toolchain (not vendored)

DreamSDK R4, build 4.0.11.2508 (2025-08-30), https://dreamsdk.org/: sh-elf
GCC 13.2.0 with newlib 4.3.0.20230120, the "stable" toolchain package
(`sh-elf-toolchain-stable-bin.7z`, SHA-256
`97c9d9b30ea8c03eae080005f91fffee80d4ddff1cc40c964d081389fa429e88`),
multilibs `m4-single` (default) and `m4-single-only`. Disc image tools from
the same install: KOS's `makeip` and `scramble`, `mkisofs`, `cdi4dc`.

## Flycast (testing only)

Flycast 2.7 (Windows x64 release), https://github.com/flyinghead/flycast
(GPL-2.0). Used only to run the built programs; nothing from it is linked or
distributed. The driver runs a copy of the user's `flycast.exe` from the
build directory with its own `emu.cfg`.
