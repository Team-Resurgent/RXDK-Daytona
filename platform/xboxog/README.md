# Original Xbox port

Daytona USA on the original Xbox (733 MHz Pentium III, 64 MB), built with
[RXDK](https://github.com/Team-Resurgent). It is the same recompiled game as
the desktop build: the game code is generated from your own ROM set, so the
result is yours alone and is never shared.

Status: runs on a console at the arcade's 57.5 Hz, the picture drawn by
the Xbox's GPU, in 64 MB: a picture every second frame (about 28 a second)
in the attract mode, every third (about 19) when a race gets busy, so the
game itself keeps full speed. Tested with the attract mode and recorded
races on the beginner and advanced courses on a console. (xemu runs it too,
much slower than a console.)

<p>
  <img src="images/xbox-title.png" width="49%" alt="The title screen on an original Xbox">
  <img src="images/xbox-race.png" width="49%" alt="The attract mode's race on an original Xbox">
</p>

*On an original Xbox, captured from the console's screen over the debug
connection.*

## Build it

First install RXDK (v1.3.8 or newer) with its extension for
[Visual Studio](https://github.com/Team-Resurgent/RXDK-VS20XX/releases/latest)
or [VS Code](https://github.com/Team-Resurgent/RXDK-VSCode/releases/latest).
(The archived Team-Resurgent/RXDK installer, 1.4.x, is older: it will not do.)

Then, on Windows, from the project folder, one command installs anything
else the build needs (Git, CMake, Ninja, Python and the Visual Studio 2022
C++ tools with Clang, through winget, skipping what you have) and builds:

    powershell -ExecutionPolicy Bypass -File platform\xboxog\build.ps1 C:\path\to\daytona.zip

With those tools already installed, `build.py` alone does the build:

    python platform/xboxog/build.py --rom C:\path\to\daytona.zip

The ROM set can be a `.zip`, a `.7z` (named `daytona.7z` or
`daytona93.7z`) or a folder of its files; a clone set and its parent may be
given together. Both `daytona` (Revision A, 1994) and `daytona93`
(Deluxe '93) are accepted; Revision A is the tested one. The first build
fetches the pinned sources and builds the desktop tools (some minutes);
later ones skip what is already done.

Out come, in `platform/xboxog/dist/`:

- `Daytona USA/`: `default.xbe` and `data/`. Copy the folder to the Xbox's
  hard disk (e.g. `E:\Games\Daytona USA`) and launch it.
- `Daytona USA.iso`: the same as a disc image, for xemu.

Revision A's factory settings are a linked twin cabinet, which waits for a
second machine; the build sets a single cabinet in test mode once on the PC
(`single_cabinet.txt`), so the Xbox starts ready to play. Settings and
records are then saved on the Xbox (`T:\`).

## Controls

| Xbox controller | Cabinet |
| --- | --- |
| Left stick, or D-pad left/right | Steering |
| Right / left trigger | Accelerator / brake |
| D-pad up / down, or Black / White | Shift up / down (gears 1-4) |
| A, B, X | View buttons VR1, VR2, VR3 |
| Y or Back | Coin |
| Start | Start |
| Left / right stick click | Test / Service (test mode) |

## HD textures

Every texture the game draws has a name: 16 hex digits from its contents.
With a file `D:\hd\dump.on` (or `T:\dump.on`), each one the game draws is
saved once as `T:\dump\<name>.tga` at its original size. A replacement goes
in `data`'s sibling folder `hd\` as `<name>.dds` (DXT1, DXT3, DXT5 or 32-bit,
power-of-two sizes up to 2048, mipmaps optional): drawn like the original,
greys times the polygon's colour. `<name>_c.dds` is a full-colour
replacement, lit but not tinted.

## How it works

`build.py` drives the steps, each a script of its own:

- `scripts/setup.py` and `scripts/recompile.py` (the desktop's), in
  `build-<set>-fast`: the ROM import, which checks every file, and the game
  code generated from it with `--fast_inaccuracy`, as for the PS Vita: about
  a fifth less work for the CPU, enough to keep the busiest courses at full
  speed, with interrupts that are not instruction-exact against MAME
  (recorded replays can drift from the arcade's; play is the same).
- `stage_data.py`: the imported images into `game/data` (`D:\data` on the
  Xbox, about 21 MB), the ROM regions packed (`<name>.z`).
- `gen_project.py`: the RXDK projects. The title (`src/`, the runtime) and,
  as libraries, the generated game code (at `-O2`; exactly generated code
  is rewritten first by the Dreamcast's `fast_gen.py`, `--fast_inaccuracy`
  code is used as it is), SoftFloat and ymfm. Everything is built with
  the Dreamcast port's low-memory runtime and its fast paths
  (`M2_DC_MEMORY`, `M2_DC_SPIN_SKIP`, `M2_DC_SPEED`): the tile layers in
  16-bit formats, composed only when they change, and geometry and layers
  only for the frames that are shown.

The frontend, in `src/`:

- `rom.h`: the 48 MB of ROM regions, packed by `stage_data.py` into about
  12 MB of independently deflated 16 KB blocks, all loaded at the start; a
  block is unpacked (`inflate.h`) into a 6 MB cache when the game first
  reads it, so nothing is read from the disc during play.
- `renderer.h`: the 16-bit tile layers (the race's scenery as a scrolled
  layer) and the polygons through Direct3D 8, a port of the Dreamcast's
  PVR renderer: each texture once, as a palettized (P8) texture made again
  only when the game rewrites its rows, its lighting as shared 32-entry
  palettes (`hd.h`: the replacements and dumps).
- `audio.h`: the native sound sequencer into DirectSound.
- `input.h`: the controller as the cabinet.
- `main.cpp`: the loop, paced to the board's 57.52 Hz: a picture every
  second frame, every third while the game is falling behind (back to
  every second after 3 seconds with time to spare), and never two pictures
  skipped in a row.

Its log goes to the debug output (serial in xemu): every 10 seconds memory,
frame times by stage, the slowest frame and what it spent its time on, ROM
blocks unpacked, polygons and textures. For measuring, a recorded input
script (`scripts/inputs`, e.g. `race_basic.txt`) copied next to
`default.xbe` as `inputs.txt` plays instead of the controller.
