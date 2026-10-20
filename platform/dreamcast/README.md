# Sega Dreamcast port (in progress)

The recompiled game on a Dreamcast (SH-4 at 200 MHz, 16 MB main RAM, 8 MB
video RAM, 2 MB sound RAM), built with KallistiOS. Like `platform/vita`, this
folder is a separate build: it compiles the main tree's runtime and the
host-generated game code. Changes the Dreamcast needs in the runtime are
behind `M2_DC_*` compile-time defines that only this build sets, so the
Windows, macOS and Linux builds are unchanged. It is built from the `daytona` ROM set only (Revision A, 1994, the set
with link play).

Status: **the game runs and plays in Flycast**, drawn by the PVR with
textures and both tile layers; a whole recorded race matches the desktop at
every checkpoint. About 32 game frames/s in Flycast (the arcade runs
57.52), with sound (the native sequencer on the AICA), not yet on a console. Details and the plan: [HANDOFF.md](HANDOFF.md).

## Requirements

- The desktop build first: setup, then
  `python scripts/recompile.py --set daytona --build-dir build-daytona`.
- An SH-4 toolchain. On Windows, [DreamSDK](https://dreamsdk.org/) (R4 tested,
  at `C:/DreamSDK`); elsewhere, a KOS dc-chain toolchain (`KOS_CC_BASE`).
- KallistiOS 2.2.1 is set up by the driver in `extern/kos-dc` (git-ignored),
  from DreamSDK's offline package or cloned; DreamSDK's own installed KOS is
  not used (see [THIRD_PARTY.md](THIRD_PARTY.md)).
- Flycast to run it (2.7 tested); a console later.

## Commands

All through `platform/dreamcast/build_dreamcast.py`; output goes under the
host build directory (`build-daytona/dreamcast*`, git-ignored).

| Command | What it does |
| --- | --- |
| `kos` | Sets up and builds KallistiOS 2.2.1 in `extern/kos-dc` (the other commands do it first). |
| `selftest` | Builds the floating-point self-test as a bootable disc image and runs it in Flycast; exit 0 only if every result matches the PC's bits. |
| `videotest` | The display path: a 496x384 frame through the PVR every frame; prints the per-frame cost. |
| `compile` | Compiles the runtime and the generated game code for the SH-4 and reports their size. |
| `game` | (`--draw-every N`: frames per picture, 1 = every frame, default 2; `--inputs FILE`: a recorded input script instead of the pad, 6,000 frames, then `GAME DONE`; without it the game runs on the pad until Flycast is closed; `--sample`: sample the game thread's PC, read with `scripts/pc_profile.py`.) Builds the game and its disc image (with the ROM images and the saved settings EEPROM and backup RAM, `--nvram`, by default `%APPDATA%/daytona-recomp/daytona`) and runs it in Flycast. |
| `tools` | Builds the host measuring tools in `tools/` (desktop compiler). |
| `measure` | Runs `romuse` over the input scripts: which ROM pages the game reads, and when. |
| (tools) `tracecheck` | The desktop's side of the lockstep check: the per-frame `TRACE` line the Dreamcast prints every 60 frames. Use `--draw-distance -1`, as the frontend. |
| (tools) `dcmemcheck` | The runtime built with the Dreamcast's defines (M2_DC_*) on the desktop, ROM pages from the image files: the Dreamcast's code paths at desktop speed. Prints hashes of the tile layers and of each drawn frame's polygons too (`--frame-skip N` as the frontend); configure with `-DDC_SPEED=OFF` to compare them without M2_DC_SPEED. |
| (scripts) `flycast_shot.ps1 OUT.png` | A PNG of the running Flycast window (the picture check; no input sent). |
| (tools) `soundusage` | The samples the game plays (the native sequencer over input scripts): the sound pack's list. |
| (scripts) `sound_pack.py` | Those samples as AICA ADPCM, `sound.pak` on the disc. |
| (tools) `ipprof` | Where the i960 spends its instructions: IP samples every 61 instructions, busiest blocks and addresses. |

`--flycast PATH` (or `FLYCAST`) points at `flycast.exe`. Flycast runs from a
portable copy in `build-daytona/dreamcast/flycast` with its own `emu.cfg`, so
your own Flycast settings are not changed. The program's serial output is read
from Flycast's console window (`scripts/flycast_run.py`).

## Controls

The controller in port A (a standard controller or the Racing Controller):

| Dreamcast | Cabinet |
| --- | --- |
| Stick / wheel, or D-pad left and right | Steering |
| Right trigger / left trigger | Accelerator / brake |
| D-pad up / down | Shift up / down (gears 1-4) |
| A, B, X | View buttons VR1, VR2, VR3 |
| Y | Coin |
| Start | Start |

`build_dreamcast.py game --inputs scripts/inputs/race_basic.txt` plays a
recorded input script instead (compiled into the program), so a race can be
checked against the desktop (`tools/tracecheck` with the same `--inputs`).

## Build notes

- **`-m4-single`, not `-m4-single-only`.** The i960 code's floating-point fast
  paths need a 64-bit `double`; `-m4-single-only` makes it 32-bit. KOS is
  built that way in `extern/kos-dc`.
- **`int32_t` is `long` in newlib for SH.** The shared sources assume `int`
  (`std::max(int32_t, int)` in `raster.cpp`, `multipcm.cpp`). Both are 32 bits
  on SH-4, so the Makefile redefines `__INT32_TYPE__` as `int` for this build;
  the source is not changed.
- **The game's disc image contains your ROM set's images** (`rom/`), built
  locally from your own set. It is build output: never share or commit it.
- No iostreams (`<fstream>`, `<sstream>`, `<iostream>`) in anything the
  Dreamcast build links: their start-up stops KOS before `main`.
- On Windows the KOS build runs in DreamSDK's own login shell. A plain
  `bash -c` from Git Bash mixes two MSYS runtimes and hangs.
