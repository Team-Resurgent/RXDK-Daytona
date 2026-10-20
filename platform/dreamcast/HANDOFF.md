# Dreamcast port: handoff

## Rules for this port

- Dreamcast frontend, build and tools are in `platform/dreamcast/`.
- Changes the port needs in the shared runtime (`src/runtime`) go there,
  like the Vita's `M2_VITA_RENDER_OPT`, behind compile-time defines
  (`M2_DC_*`) that only the Dreamcast build sets. Windows, macOS and Linux
  compile exactly what they did before; each change is checked with desktop
  replays (screen hashes and instruction counts identical before and after).
- The generated game code is used as the desktop recompile writes it.
- **ROM set: `daytona` (Revision A) only**, for link play. Not `daytona93`,
  for builds or for measurements.
- It runs after the desktop build (`build_dreamcast.py`), from its generated
  code and ROM images, like `platform/vita` and `scripts/build_vita.py`.
- Proof of concept in Flycast first; the console after that. Floating-point
  and speed results from Flycast are stated as Flycast results.

## Current state (2026-10-03, overnight)

**A whole recorded race runs in Flycast, in lockstep with the desktop**:
`build_dreamcast.py game --inputs scripts/inputs/race_basic.txt` (coin-up,
course and transmission select, rolling start, the Beginner race) matched
`tools/tracecheck` with the same inputs at all 100 checkpoints of its 6,000
frames (i960 and TGP instruction counts, the display list's hash). Drawn by
the PVR with textures and both tile layers (`game/renderer.h`); the busiest
frame 1,826 polygons, 1,626 textured, none dropped; 292 textures cached; main
RAM steady at 1.35 MB free; 1,133 ROM misses in the race.

**Slow, but nearly five times as fast as at the start of the night**:
6,000 frames in 224 s, about 27 frames/s in Flycast (not a console figure;
the arcade runs 57.52). Main RAM 2.0 MB free in the race. Every 4th frame is drawn. Every 60 frames the frontend prints
`PROFILE` (ms per frame, `timer_us_gettime64`, Flycast), late in the race:

| Step | core (i960 code) | geometrizer | video | drawing | race |
| --- | --- | --- | --- | --- | --- |
| Start of the night | 64 | 52 | 80 | 15 | 1,414 s |
| Runtime frame skip (`set_frame_skip(3)`: tile layers composed only for drawn frames) | 85 | 61 | 33 | 16 | 1,045 s |
| ROM's last page remembered (`RomSource::page_fast`) | | | | | 965 s (with `--sample`) |
| Frame-wait skip (M2_DC_SPIN_SKIP) and tile runs (M2_DC_SPEED) | 39 | 41 | 22 | 16 | 626 s |
| Tile rows with nothing to draw skipped; back layers straight onto the screen; clear front-layer rows not uploaded again | 39 | 41 | 11 | 13 | 540 s |
| Geometrizer object data parsed only for frames that are shown; frame skip 3 really set (it was clamped to 2) | 39 | 11 | 8 | 13 | 374 s |
| Renderer: polygons through KOS's direct rendering, no `std::isfinite` (soft-float calls at -fno-fast-math); layers written through the store queues; TGP status helpers inline | 38 | 11 | 8 | 9 | 350 s |
| Tile layers composed in 16 bits (RGB565, ARGB1555) by the runtime: no conversion, 0.75 MB less RAM | 38 | 11 | 8 | 4 | 315 s |
| Board: last plain-memory page cached; `has_code` only after a run that did nothing; `Lockstep::boundary` one compare; TGP FIFOs a ring; PROFILE every 300 frames | 34 | 10 | 8 | 4 | 298 s |
| The TGP's generated code at -O2 (146 KB instead of 88 KB; its template helpers inline) | 31 | 10 | 8 | 4 | 284 s |
| Draw distance -1 (the runtime's enhancement: course cells one around the car); ROM cache 576 pages | 31 | 6 | 8 | 3 | 271 s |
| Tiles drawn 8 pixels at a time by their class (skipped, copied, or tested) | 31 | 6 | 7 | 3 | 269 s |
| The i960 code rewritten by `scripts/fast_gen.py`: register-only instructions (49%) count down instead of calling `boundary()` | 29 | 6 | 7 | 3 | 258 s |
| `fast_gen.py`: 3,289 fixed, aligned work-RAM accesses straight to it (`gen::wram_*`, `Cpu::work_ram`); register-only now 58% | 28.5 | 6 | 7 | 3 | 255 s |
| A tile layer composed (and uploaded) only when its inputs changed: a tile of its category rebuilt, a pen changed, or tile RAM 0x8000-0xdfff (line tables, registers, masks) differs from a copy | 28.6 | 6 | 5 | 2.7 | 242 s |
| Board memory accesses marked aligned (`M2_AL`): GCC had called memcpy for every word (the SH-4 traps on unaligned access); ROM cache 512 pages | 25.3 | 6 | 5 | 2.7 | 226 s |
| `RomSource::dword`/`word`: one aligned load on the little-endian SH-4 instead of four bytes | 25.4 | 5.6 | 5 | 2.7 | 224 s |

**Every frame drawn** (`--draw-every 1`): race_basic 462 s, 77 ms a frame,
about 13 frames/s (core 25, geometrizer 21, tile layers 20, drawing 11 ms);
all 100 checkpoints match. The geometrizer with -ffast-math/-ffp-contract=
fast/-mfsrra/-mfsca made no difference (464 s; it is not arithmetic-bound)
and was taken out. Skipping the tile decode when neither tile nor character
RAM was written (M2_DC_SPEED) is exact but saves under 1% in a race (the HUD
writes tile RAM nearly every frame).

**Geometrizer, direct rasterizer path** (`geo.cpp`, M2_DC_SPEED): in
`geo_parse_np_s`, when the rasterizer is waiting for a polygon's attribute
(polygon data, slot 8), the polygon's words go straight into its command
slots and `model2_3d_process_polygon` is called directly, as
`model2_3d_push` would do word by word. dcmemcheck's polygon hash identical
at every frame. Every frame drawn: geometry 20.8 -> 16.3 ms, race 462 ->
429 s (about 14 frames/s). `model2_3d_process_polygon`'s temporaries
(`quad_m2 object`, `GeoVertex vertices[2][8]`) are no longer zero-filled for
every polygon (uninitialised union storage; every field used is written
first): geometry 14.9 ms, race 419 s. Tile decode: with no character
changed, blocks of 16 tile values equal to the last decode's copy are
skipped with one memcmp, and the 16 KB character-dirty table is cleared only
when something was set: tile layers 19.6 -> 16.3 ms, race 404 s. Tile
layers by DMA: the 16-bit layers have rows of 512 (`Video::kLayerStride`,
the texture's width; columns 496 and up composed but not shown, dcmemcheck
hashes the shown ones), large allocations are 32-byte aligned (main.cpp's
operator new), and the renderer starts each layer's DMA after
`pvr_wait_ready` (no longer writing a texture the PVR may still be
drawing from) and waits before `pvr_scene_finish`: drawing 10.8 -> 4.5 ms,
race 373 s, about 16 frames/s with every frame drawn. Whole tiles
unrolled (371 s); the window mask copied out of tile RAM only on the path
that reads it (disabled layers and the special modes do not): tile layers
15.6 ms, race 368 s. Tried and not kept (no gain): a table-driven palette
recompute, GeoVertex without zero initialisers.

Tile paths in a race (a temporary counter, frames 2700-6000): nearly every
pass is in the special split/window modes, where the odd layers return at
once, so about four full-screen `tilemap_draw` passes a frame do the work.
On the PVR they would need the four 512x512 layer pixmaps as textures (2 MB
of video RAM, 0.7 MB free) or pen-resolved pixmaps in main RAM (2 MB, 0.9 MB
free): not within this memory.

**`M2_AL(p, n)`** (`cpu.h`): `__builtin_assume_aligned` with M2_DC_SPEED on
GCC, otherwise `p` itself (the desktop compiles the same code). Found with
`--sample`'s new CALLER lines (who called memcpy/memset, from PR when the PC
is in them): the board's `read_dword`/`write_dword`/`read_word` were the
biggest callers. The sampler's tables are 64-byte buckets now (the 32-byte
ones and the caller table ran a race out of memory).

**`scripts/fast_gen.py`** (run by the driver after the desktop build, into
`build-daytona/dreamcast/gen_fast`; the Makefile's `GEN_I960`): each chunk
counts down `left`, the instructions it may run before the next lockstep
event, from the last full check (`Lockstep::check`). Register-only
instructions cost a decrement; the others keep their IP store, and if the
lockstep's `epoch` moved during their body (a poke, a callback, the
frame-wait skip moving the count) the next instruction checks in full.
When `left` runs out the instruction stores its IP and jumps to the chunk's
one `recheck:`, which re-enters through the chunk's dispatch switch (a first
version with the slow path at every instruction was 900 KB bigger and ran
out of memory; this one is 320 KB bigger, 0.65 MB free in the race).
`tools/dcfastcheck` (configure with `-DDC_FAST_GEN=...gen_fast/daytona`)
compiles the rewritten code on the desktop: identical to the reference at
every frame of race_basic and attract_long.

**Every 2nd frame drawn is now the default** (`build_dreamcast.py game
--draw-every`, default 2; the user's minimum): race_basic 272 s, about 22
game frames/s and 11 pictures/s (core 25.5, geometry 13.5, tile layers 7.9,
drawing 4.1 ms a frame), all 100 checkpoints match.

**Draw distance: back to the game's own (0)** at the user's request
(`kDrawDistance` in `game/main.cpp`); the lockstep reference is plain
`tools/tracecheck` again. race_basic, every 4th frame drawn: 207 s, about 29
frames/s (core 25.4, geometry 7.0, tile layers 3.9, drawing 2.1 ms a
frame), all 100 checkpoints match.

**Draw distance -1** (was the user's choice for a while):
the game's own list of course cells is cut to those around the car, so the
distant grandstand and treelines are not drawn (screenshot compared at the
same moment); the i960 and TGP instruction counts stay the same, the
display list changes. The lockstep reference is therefore `tools/tracecheck
--draw-distance -1` (matched at all 100 checkpoints).

**ROM cache 576 pages (2.25 MB)**: misses in the race 1,134 -> 710, no
change in Flycast (its CD reads cost little); a real drive seeks, so it
should matter on the console. 0.96 MB free in the race.

A CPU-side fast path for the i960's memory helpers (straight to the board's
last page, skipping the virtual call) was tried and measured: 286 s against
284, no gain; taken out.

(Drawing is about 64 ms for each drawn frame. The first row is from earlier
in the race, so its core and geometry figures are lower.) Every step matched
the desktop at all 100 checkpoints.

Drawing, per drawn frame (`PROFILE draw` lines): the layers' upload 7 ms
(27 when they were converted from 32 bits), sorting and waiting for the PVR
1, materials 2, polygons 6 (21 before direct rendering). Core (the i960's
code, the TGP's, the board's memory and devices, `Lockstep::boundary`) is
now most of a frame.

Screenshots of the Flycast window (`scripts/flycast_shot.ps1 OUT.png`, no
input sent) at the attract mode's opening scene and mid-race (lap 1, HUD,
cars, track, grandstand) show the picture right.

Where the time went before that round (`build_dreamcast.py game --sample`: the game
thread's PC at each KOS timer tick, about every 10 ms; `scripts/pc_profile.py`
names the functions from `game.elf`), frames 2700-6000: `Renderer::draw` 11%
(the layers' conversion to 16 bits), `Lockstep::boundary` 8% (once per i960
instruction, out of line at -Os), `tilemap_draw` 5%, `draw_rect` 3%, the
geometrizer 9%, board memory access about 8%.

**Controls**: the controller in port A (`game/controls.h`, tested on the PC
by `tools/test_controls`), or a recorded input script compiled in. With the
controller (`build_dreamcast.py game` without `--inputs`) the game runs until
Flycast is closed: tested past frame 10,000 in the attract mode, controller
A0 found, free RAM steady at 1.33-1.37 MB.

**Lockstep tools**: every 60th frame the frontend prints `TRACE frame i960
tgp buffer`; `tools/tracecheck` prints the same on the desktop.
`tools/dcmemcheck` runs the runtime built with M2_DC_MEMORY on the desktop
(ROM pages from the image files): it matches the desktop at all 60
checkpoints of 3,600 attract frames and all 333 of a whole race
(race_to_end), also with the texture cache's half of texture RAM filled with
a pattern and no frame buffer RAM.

Not done yet: sound, speed, the Medium and Long courses' ROM figures, link
play. Not run on a console.

## Plan

0. **Measure (desktop, no Dreamcast code).** Done: ROM read per run
   (`romuse`), sound commands, SH-4 code size, denormals (`ftzcheck`).
   Still open: the Medium and Long courses as races.
1. **Skeleton.** Done: KOS build, bootable CDI, runs in Flycast.
2. **Floating point on the SH-4.** Self-test done (Flycast); the game's own
   results match the desktop so far. Not yet on a console.
3. **The game runs** (no speed): the attract mode runs, in lockstep with the
   desktop (above).
4. **PowerVR2 renderer.** 3D through the PVR (fixed function, no shaders);
   tilemaps drawn on the CPU, uploaded as textures, as before the desktop's
   tile shaders. Done: polygons (textured: luma as grey palettes, colour and
   light per vertex, an approximation), both tile layers.
5. **Controls (pad, racing controller), VMU saves, 57.52 Hz on 60 Hz.**
   Controls done (`game/controls.h`); saves and pacing not yet.
6. **Sound.** The native sequencer; samples in the 2 MB sound RAM (AICA
   ADPCM) or music from CD audio, decided by the Phase 0 numbers.
7. **Speed.**

Memory plan to test: music on CD audio tracks (no RAM), effects in sound
RAM as AICA ADPCM, textures in video RAM, and in main RAM only the code and
the ROM a course reads, loaded at course select. The drive cannot play CD
audio and read data at once, so nothing can stream from disc during a race.

## Runtime changes: M2_DC_MEMORY

All in `src/runtime`, all inside `#ifdef M2_DC_MEMORY` (only the Dreamcast
Makefile and `tools/dcmemcheck` define it):

- `rom_source.h` (new): `RomSource`, the ROM regions a 4 KB page at a time
  from the frontend's cache; `page_fast` remembers each region's last page
  (the source calls `forget()` when it loads one, which may evict).
- `m2_board`: `Images` gains `rom`, `texture_ram`, `frame_buffer_ram`; ROM
  pages map to the source (`map_rom`); texture RAM (4 MB) and frame buffer
  RAM (1 MB) are the frontend's (video RAM); a two-level page table (1 MB
  chunks of 256 pages, allocated as mapped) instead of 1M entries (12 MB
  with the extra fields); the duplicate `copro_tables` image is released.
- `geo`: a constructor taking the source (it also reserves room for 2,400
  kept polygons: the desktop's maximum in attract and in a whole race is
  2,182; growing past 2,048 needed 800 KB while the old 400 KB existed);
  `GeoPtr`/`GeoPtr16` read polygon and texture ROM through it; `geo_test`
  (the ROM self-test) only moves the cursor on (below).
- `m2_tgp_board`: a constructor taking the source; copro data read through it.
- `video`: no GPU-layer copies (1.5 MB): in external 3D the layers are the
  screen and sys24 buffers themselves (`background_layer`/`foreground_layer`),
  and they are composed (the Vita's early return, for its own tile textures,
  is not taken).
- `m2_board`: frame buffer RAM optional (never written by the game in 9,000
  attract frames or a whole race; without it the range is unmapped).
- `raster.cpp`: its buffers (1.25 MB) only on the first CPU render.
- `lockstep`: called callbacks' slots are reused (below).

## Runtime changes for speed: M2_DC_SPIN_SKIP, M2_DC_SPEED

Also only in the Dreamcast Makefile and `tools/dcmemcheck`:

- **M2_DC_SPIN_SKIP** (`m2_board.cpp`, `read_byte`): the game's wait for the
  next frame, `0x1394: ldob 0x500000,r3` / `0x139c: cmpibe r3,g0,0x1394`,
  is 76% of the i960's instructions in a race (`tools/ipprof`, frames
  2700-6000). Every frame runs to the probe's instruction caps (110,592 +
  40,960 = 151,552 a frame) because `in_idle_loop` lists only 0x12b0 and
  0x12f0; that timing is the game's on the desktop too, so it stays. When the
  read at 0x1394 will compare equal, the passes up to just before the next
  lockstep event are skipped: the count moves on by whole passes, nothing
  else changes (the byte is RAM, only an interrupt or callback changes it).
  race_basic: 715,083,926 of 909,312,001 instructions skipped; dcmemcheck
  matches the desktop at all 100 checkpoints.
- **M2_DC_SPEED**, the same output, faster:
  - `video.cpp`: `tilemap_draw` a row at a time in runs that do not wrap;
    `build_layer` counts, per layer and row of tiles, the tiles of each
    category and those with an opaque pixel, and `tilemap_draw`/`draw_rect`
    skip rows where nothing can match; the back layers are drawn straight
    over pen 0 (as the Vita's path) instead of into a cleared `sys24_`
    copied over it.
  - `geo.cpp`, `m2_board.cpp`: with a frame skip, object data is not parsed
    for frames whose polygons are never shown (neither that frame nor, in
    30 Hz mode, the next is drawn): the rasterizer gets the command's
    opening and closing words, every other command runs, so the
    geometrizer's state is unchanged.
  - `video.cpp`/`video.h`: in external 3D the two layers are composed
    straight into the PVR's formats (`background16`: RGB565, `foreground16`:
    ARGB1555, 0 see-through) with pens kept in both by `palette_w`; `draw`,
    `draw_rect` and `tilemap_draw` are templates on the pixel type there
    (macros expand to exactly the old 32-bit functions on the desktop). The
    32-bit `screen_`/`sys24_` are allocated only if a frame is drawn without
    external 3D. dcmemcheck hashes the layers in 16 bits in both builds (the
    reference converts its 32-bit layers as the renderer did): identical at
    every frame of race_basic and attract_long.
  - `tgp.h`: the TGP's status helpers `always_inline` (GCC only).
  - `m2_board`: the last page read from plain memory (RAM, texture RAM)
    and the last RAM page written outside the video registers' ranges
    (where `ram_written` does nothing) are used straight through their
    base; the map never changes after the constructor. The frame-wait read
    (0x500000) still goes the long way.
  - `game_loop.cpp`: `gen::has_code` (a binary search over every address)
    only after a `gen::run` that did nothing, the one case where it can be
    false; the same error.
  - `lockstep`: `next_count` kept at or below `end_count` (`refresh_next`
    clamps; the game loop sets the end with `set_end`, which takes a poke
    not yet taken at once, as before), so `boundary()` compares once.
  - `m2_tgp_board.h`: the FIFOs a growing ring (`WordFifo`), not
    `std::deque`.
  - `m2_board.h`: `set_frame_skip` allows 3. The desktop's range is 0-2, and
    the frontend's `set_frame_skip(3)` had been clamped to 2: the layers
    were composed every 3rd frame and drawn every 4th, up to two frames old.

  Checked with dcmemcheck (`-DDC_SPEED=OFF` builds the reference): with and
  without M2_DC_SPEED at frame skip 2, race_basic (6,000 frames) and
  attract_long (9,000) give identical lines every frame (instruction counts,
  display list, tile-layer hash, and the drawn frames' polygon hash); at frame
  skip 3 the 1,500 + 2,250 drawn frames are identical to a frame skip 0 run.

Desktop check after the changes (build-daytona m2run, race_basic, 6,000
frames, single-cabinet settings): screen hash `9427a612c5cb7511`,
909,312,001 i960 and 195,261,176 TGP instructions, the same as before them.

## For the desktop too (not changed there: the user's decision)

- **`Lockstep::calls_` never shrinks.** `GameLoop::probe` adds a callback
  every 1,024 i960 instructions (about 150 a frame) and every callback stays
  in `calls_` for good. On the Dreamcast (16 bytes per `std::function`) that
  was 1 MB by frame 220; on the desktop (32 bytes) it is about 270 KB a
  second, roughly 1 GB an hour (estimated from those numbers, not measured).
  The M2_DC_MEMORY fix (reuse a slot once its callback has run) is
  behaviour-neutral (dcmemcheck matches the desktop at 60 checkpoints) and
  could be made unconditional.

## Found on the way (and what was wrong)

- **iostreams stop KOS before `main`.** Reading the recorded input script
  with `tools::Script::load` (an `std::ifstream`) linked libstdc++'s
  iostreams (+263 KB of code) and their start-up, and the program stopped
  during KOS's start-up ("SH4 exception when blocked" in Flycast), at
  different points as the layout changed. Two theories were wrong and were
  tested and dropped: the disc layout (an extra file on the disc in `rom/` or
  the root) and the binary's size (padding 1ST_READ to whole sectors). With an
  empty script the compiler had dropped the `load` call (`kInputs[0]` is a
  constant), which is why that build booted. The frontend now parses the
  script itself (`parse_script`) and uses `Script::at` as it is. Do not use
  iostreams in the Dreamcast build.

- **The geometrizer's ROM self-test (`geo_test`) and the compiler.** Its sums
  have no effect (the LEDs are not emulated), so the desktop's compiler
  removes the loop: it costs nothing there and romuse never saw it. Through
  `RomSource` the reads cannot be removed. In the attract mode (frame 189) the
  display list asks for a huge number of blocks: first the Dreamcast read
  megabytes of polygon ROM from the disc (it looked like cache thrashing),
  then, with only the inner loop skipped, it stepped through billions of
  words. M2_DC_MEMORY now moves the cursor on 3 * blocks words, the result
  the desktop's compiled code has. Found with `dcmemcheck` (the same hang on
  the PC) and cdb's stack at a runaway-read throw; the Dreamcast's watchdog
  reads the game thread's saved PC for addr2line.
- **Wrong guesses on the way there**, tested and dropped: read-ahead filling
  the cache (it reads ahead only on sequential misses now, a fine change but
  not the cause), the TGP's floating point (its counts and display list match
  the desktop to frame 188 and beyond), vertex buffer and tile bin sizes.
- **`vbuf_doublebuf_disabled = 1`** stopped Flycast ("SH4 exception when
  blocked"); double-buffered vertex buffers work.
- **"SH4 exception when blocked" was usually KOS aborting** (out of memory)
  with the message lost when Flycast stops; the runner now keeps every
  console text it reads (`dreamcast/flycast/serial.txt`), stdout is
  unbuffered, and large `operator new` calls are logged with their caller.
- **A stale object, not a runtime bug.** After `Images` gained a member,
  the screen vector came out empty (`0 px`) and the run stopped. The cause:
  `game_loop.o` was not rebuilt (still the old `Images` layout), so the
  frame buffer pointer was garbage and the game's frame buffer writes went
  into the heap. DreamSDK's compiler writes `C:/...` paths in `-MMD` files,
  which MSYS make cannot use (the colon), so header changes rebuilt nothing.
  The Makefile now rewrites them to `/c/...` (`FIXDEP`). Two guesses before
  that were wrong and were tested and dropped: the custom `pvr_init`
  parameters, and frame buffer RAM in video RAM (neither mattered).
- **The stack**, also suspected then, was not the cause either, but the game
  runs on a 512 KB-stack thread anyway (KOS's main thread has 64 KB); the
  main thread is a watchdog printing the frame, free RAM, ROM misses, what
  the game thread is doing and its PC every 5 s.
- **Blurred picture in Flycast, the BIOS too**: Flycast was started
  minimised; Direct3D then renders at the minimised window's size. It is
  started normally now, without taking the focus.
- **Leftover Flycast windows**: after a crash the windows outlived the
  process the runner started; it now closes every Flycast running from the
  build directory's copy (by path) before and after each run.
- KOS's `kos.h` defines `BIT(n)`; the runtime has `BIT(x, n)`: the frontend
  includes the runtime's headers first.
- **Serial lines cut in two**: the game thread and the watchdog (and the
  `--sample` thread) printing at once mixed their characters, once inside a
  TRACE line (the checkpoint was right; the comparison failed). The
  frontend's prints go through `say()`, one line at a time under a mutex
  (timed, so a stuck game thread cannot silence the watchdog); the sampler's
  reports are printed by the game thread between frames.
- **Inlining `Lockstep::boundary` is out**: forced inline, one generated
  chunk grew from 97 KB to 191 KB (measured): the generated code would not
  fit. It stays a call per i960 instruction.
- Out of memory before the game ran: the ROM cache moved to video RAM and
  back to main RAM (1.25 MB) once the CPU rasterizer's buffers went; frame
  buffer RAM is in video RAM; the GPU layers are not allocated.

## Measured so far

### ROM read by the game (Revision A, `romuse`, desktop)

MB of each ROM region read (4 KB pages; a page counts once), from the
single-cabinet settings saved by `daytona.exe` (`--nvram`). `romuse` leaves
the game unchanged: race_basic's last screen hash is m2run's
(`9427a612c5cb7511`).

| Run | main_data | polygons | textures | copro | program | pcm1+2 | total |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Power-on, frames 0-1200 | 2.82 | 0.65 | 0.42 | 0.07 | 0.16 | 0.83 | 4.95 |
| Full Beginner race, frames 2400-20000 (race_to_end) | 1.67 | 1.08 | 0.68 | 0.09 | 0.11 | 2.70 | 6.34 |
| Attract demo, frames 1200-9000 (attract_long) | 2.92 | 3.41 | 2.00 | 0.53 | 0.10 | 0.91 | 9.86 |
| Union of all six runs | 5.79 | 3.88 | 2.25 | 0.55 | 0.18 | 3.42 | 16.08 |
| Union from frame 1200 (no power-on) | 4.25 | 3.88 | 2.25 | 0.55 | 0.12 | 3.42 | 14.48 |

- Power-on reads 2.8 MB of main_data in its first 10 s (most likely the
  ROM check); after it, a Beginner race reads 1.7 MB of it.
- **Only the Beginner course is measured as a race.** On Revision A's
  Circuit Select the cursor follows the wheel's position, so
  `scripts/inputs/course_advanced.txt` and `course_expert.txt` (written for
  daytona93) both race Beginner (screens show LAP 1/8).
  `scripts/inputs/reva_long.txt` here selects Long (LAP 1/2), but the fixed
  steering stops the car against the side, so it reads only the start area;
  the Medium and Long courses need scripts that drive them. The attract demo
  shows other courses, so its 3.4 MB of polygons is the best figure so far
  for all courses together.
- Samples: a full race reads 2.7 MB of PCM (music included); every run
  together 3.4 MB.

### Denormals (`ftzcheck`, desktop)

Identical with SSE flush-to-zero and denormals-are-zero (the SH-4's DN
treatment), every frame: race_basic 6,000 frames, course_expert 6,000,
race_to_end 20,000 (3,031,040,001 i960 and 647,890,832 TGP instructions,
screen hashes all equal). The game does not depend on a denormal in these.

### Sound commands (race_basic, `--sound-log`)

All on MIDI channel 14: 37 "start sequence" (0xAn), 284 note-ons, 882
controller changes. Bank 16 is started three times (frames 185, 1212, 2579:
attract, coin-up, race start), which looks like the music; bank 33 is started
often (index 20 about every 2 s in the race), which looks like effects and
voices. Not yet confirmed which bank is which (next: run each sequence through
the native sequencer and measure how long it plays).

### SH-4 code size, Revision A (`build_dreamcast.py compile`)

| | size |
| --- | --- |
| Generated code: i960, TGP, 68000 (`-Os`) | 2.96 MB |
| Runtime, i960 lib, trace, ymfm, SoftFloat (`-O2`) | 0.41 MB |
| Total, before KOS and libstdc++ | 3.37 MB |

Compiled with no errors (9 warnings, all in the shared `geo.cpp`), 2 min 17 s
with 6 jobs. Not linked yet.

### SH-4 code size, daytona93 (compile only, GCC 13.2, KOS flags, `-m4-single`)

| | -O2 | -Os |
| --- | --- | --- |
| Generated i960 code (23 chunks + table) | 6.69 MB | 2.47 MB |
| TGP generated | 0.15 MB | |
| Sound 68000 generated | 0.42 MB | |
| Runtime + i960 lib + ymfm | 0.29 MB | |
| Total | 7.54 MB | about 3.3 MB (generated i960 at -Os) |

No large static arrays (largest `.bss` 17 KB). The generated code compiled
in 517 s at -O2 and 71 s at -Os (8 jobs). **Measured on `daytona93`'s
generated code, before the port settled on Revision A: to be measured again
on `daytona`.**

Only two shared files failed, both for `int32_t` being `long` (see the
README); the `__INT32_TYPE__` override compiles them with no errors or
warnings.

### Floating point (Flycast 2.7, self-test, booted from a CDI)

`double` is 8 bytes with `-m4-single`; FPSCR is `00040000` at start (DN
set). Divide, multiply, add and square root in `double` and `float` match
the PC's bits; `snprintf` of a `double` gives the PC's text. Denormal
results are flushed to zero (`f denormal`, `d denormal`), as the SH-4 does
with DN set; the self-test reports that and does not fail on it. Whether the
game ever depends on a denormal is what `ftzcheck` measures. Not yet run on
a console.

### Display path (Flycast 2.7, videotest)

A 496x384 ARGB8888 frame (as `Video::screen()`) converted to RGB565 and
uploaded to a 512x512 non-twiddled PVR texture each frame, drawn at 620x480
centred (10-pixel bars), bilinear. Per frame, averaged over 120 frames:
conversion 15.3 ms, upload (`pvr_txr_load`) 1.0 ms. Flycast's SH-4 timing is
not the console's, but at that cost the frontend cannot convert the whole
screen each frame (a frame is 17.4 ms): tile layers should be drawn into the
PVR's 16-bit format, or only changed areas converted, as the Vita's tile
upload does. The controller in port A0 is read.

### KallistiOS: 2.2.1 in extern/kos-dc, not DreamSDK's installed KOS

DreamSDK R4 had installed KOS git master (`d458073c`, 2026-10-01) next to
its prebuilt toolchain (GCC 13.2.0, libraries dated 2025-07-05). With that
KOS, every C++ program that pulls in libstdc++'s exception support stops
before `main` prints anything: `Assertion "m->holder == thd && m->count > 0"
failed at mutex.c:184 in mutex_unlock`, from newlib's stdout buffer flush.
Bisected in Flycast: `printf` alone works, `malloc` and `new int[]` work,
one `std::vector` fails (it links libstdc++'s exception-support static
constructors: eh globals, emergency pool, terminate handler). KOS's own
`examples/dreamcast/cpp/concurrency` fails the same way, so it is not this
project's code. Same with `-m4-single-only`, so not the float ABI. Not
proven why; KOS master has since dropped newlib's `_pthreads.h` and changed
its mutexes, which libstdc++'s thread layer was built against.

Fix: KOS 2.2.1 (`857e4e69`, 2025-08-30), the version DreamSDK R4's
toolchains were packaged with, unpacked from DreamSDK's own offline package
into `extern/kos-dc` (git-ignored) and built with `-m4-single`
(`build_dreamcast.py kos`, about a minute). With it the same `std::vector`
program, `videotest` and the self-test all run. DreamSDK's own install is
not changed.

### Toolchain findings

- KOS's `environ_dreamcast.sh` tests for `-m4-single` by compiling to
  `/dev/null` with the Windows-native compiler, which cannot write there; the
  test fails and it falls back to `-m4-single-only` (a 32-bit `double`) with
  a "toolchain does not support m4-single" warning. The toolchain does
  support it (multilibs `m4-single` and `m4-single-only`). DreamSDK's shell
  shows that fallback; the driver's own environment sets `-m4-single`.
- DreamSDK's `makeip` is a MinGW program; its libpng and zlib DLLs are in
  `/mingw64/bin`, which is not on the shell's PATH (exit 127 otherwise).
- KOS's top-level `make` also builds its PC-side helpers with a PC compiler
  the DreamSDK shell does not have; the driver copies DreamSDK's built ones
  and builds only the kernel and addons.
- Flycast prints a program's serial output in a console window of its own,
  which redirecting stdout does not capture; `flycast_run.py` reads the
  console buffer.

## -O3 with unrolling

The runtime, the frontend and the TGP's code at `-O3 -funroll-loops` (the
Makefile's O3FLAGS; the i960 code stays at -Os for size). race_basic, every
2nd frame drawn: `video.cpp` alone 263 -> 246 s (tile layers 7.9 -> 5.5 ms a
frame: their pixel loops unroll), `geo.cpp` 245 s, the rest 240 s; 0.46 MB
of main RAM free after it. All 100 checkpoints match each time. Every frame
drawn: 353 s, about 17 frames/s (geometry 25.7, tile layers 11.0, drawing
7.4, core 23.3 ms a frame).

Where it goes now (every frame drawn, line profile): the tile layers'
pixel copy, waits (mostly the game thread on CD reads for ROM cache misses,
which a bigger cache does not fix: they are first reads), the geometrizer's
per-polygon work and ROM reads. Each remaining exact change found is worth
about 1%.

## Sound (first version)

`game/audio.h`: the desktop's native sound sequencer
(`src/runtime/native_sound_sequencer`, unchanged) fed the bytes the game
sends its sound board, its voices played by the AICA's hardware channels
through KallistiOS's sound effect manager (start with frequency, volume,
pan and loop; `AICA_CH_CMD_UPDATE` for pitch, volume and pan changes; stop).
No software mixer: the SH-4's time is unchanged (race 266.7 s).

- **Samples.** Sound RAM has 2 MB, the PCM ROMs 8. `tools/soundusage` plays
  input scripts on the desktop through the sequencer and lists the samples
  notes start: 217 over race_basic, attract_long, race_to_end, time_attack,
  test_mode, reva_long (the Long course: 43 of them) and the course
  scripts. `scripts/sound_pack.py` converts them to the AICA's 4-bit ADPCM
  (KallistiOS's wav2adpcm encoder, ported) into `sound.pak` (2.44 MB) on the
  disc. `build_dreamcast.py` makes both (the usage list once: about 7
  minutes).
- **Sound RAM as a cache.** At start 155 samples fit (0.1 MB left); a note
  whose sample is not resident loads it from the disc, unloading the least
  recently used samples no channel is playing. race_basic: 35 loads from the
  disc, none missing or failed. On a console each is a disc read during play
  (a short stall); not measured there.
- **Real time.** The sequencer advances by the elapsed microseconds, so
  the music and effects keep their speed whatever the frame rate (the
  user's call). With the game slower than the arcade, sequences meet the
  game's commands at other points and can ask for samples the desktop never
  does at full speed (bank 2's 117 and 137, first time: 810 notes silent);
  `tools/soundusage` therefore also runs sequencers 1.5x to 4x ahead of the
  game on the same commands: 252 samples, 2.86 MB of ADPCM. race_basic: 35
  loads from the disc, none missing.
- **Release.** A note off is the AICA's key off and a fade at the channel's
  release rate; KallistiOS's driver sets the fastest (an abrupt cut).
  `game/audio.h` sets every channel's register 0x14 to release rate 26 with
  key-rate scaling off (`3c1a`, read back during the race): about 18 ms for
  the whole fade by MAME's AICA timing, near the desktop mixer's 20 ms. Not
  judged by ear yet.
- **ADPCM loops:** MAME's AICA keeps the ADPCM decoder state from the loop
  start and restores it at each loop, which makes loops seamless with no
  special encoding; if the chip does as MAME models it (Flycast too), the
  pack's loops are fine. Not checked by ear.
- **Not yet:** listened to (a click at a loop point would mean the AICA does
  not restore the ADPCM state there; 8-bit PCM for the looping samples is
  the fallback); levels against the desktop not compared.

## Where the core's time goes (race, every 2nd frame)

Sampled: i960 memory access about 14% of the frame, the TGP side 11%, i960
call/return 3%, the i960 code and lockstep 2.5%; geometrizer 21%, tile
layers 17%. Counted on the desktop (frames 0-6000): the i960's loads and
stores are mostly work RAM (about 57 million) and the TGP's ports at
0x008xxxxx (about 26 million, mostly FIFO writes).

Tried for the work-RAM accesses with computed addresses, not kept: an inline
plain-RAM check in front of each of the 12,500 calls (`gen::rd32` and
friends, through `fast_gen.py`). Not inlined (GCC at -Os made them calls):
slower, 278.6 s against 272.5. Forced inline: the program grew 960 KB (3.9
to 4.9 MB) and the race did not finish (0.86 MB was free).

**TGP ports straight to the TGP board** (`m2_board.cpp`, M2_DC_SPEED):
`read_dword`/`write_dword` check the TGP's FIFO, function port, status and
buffer RAM first and call the TGP board as `dev_read`/`dev_write` would,
without the page table and the dispatch chain; the status read no longer
calls `getenv` (a desktop debugging print). dcmemcheck identical; race 272.5
-> 268.7 s (core 25.5 -> 24.6 ms), every 2nd frame drawn. Register frames
spilled to or reloaded from work RAM in one copy (`Cpu::do_call`/`do_ret_0`,
M2_DC_SPEED): 266.0 s (core 24.1 ms).

## The geometrizer: the SH-4's vector maths, and straight to the PVR

**ftrv (default; `build_dreamcast.py game --no-native-geo` or Makefile
`NATIVE_GEO=0` for the desktop's arithmetic; define M2_DC_NATIVE_GEO in
`geo.cpp`).** The SH-4 transforms a vector by its 4x4 matrix registers in
one `ftrv`. The four polygon parsers load the object's 3x4 matrix once into
XMTRX and transform its points and normals with it. The picture only:
nothing the geometrizer computes goes back to the game. ftrv rounds
differently from separate multiplies and adds, so the polygons differ from
the desktop's in their last bits. Race in Flycast, every 2nd frame drawn:
240.8 -> 237.0 s (geometry 13.0 -> 12.3 ms a frame). All 100 checkpoints
match, and the same polygon counts are drawn at every report (20 of them).
Flycast's dynarec may not cost ftrv as the console does, so the gain on
hardware is still to be measured.

- **The guard is `__sh__`, not `__SH4__`.** KOS's GCC 13.2 does not define
  `__SH4__` with `-m4-single` (it has `__sh__` and `__SH4_SINGLE__`). Under
  `__SH4__` the code silently compiled out, and a first run "measured" the
  plain path. Count the ftrv instructions in `game/runtime/geo.o`
  (sh-elf-objdump), not in game.elf, which has KOS's own.
- **Switching the setting:** the driver rewrites `game/native_geo.txt` when it
  changes, and geo.o depends on it. A stamp file made by make itself is never
  made: the Makefile's `.SECONDARY` with no prerequisites treats every
  target as an intermediate.

Where the geometrizer's time goes (every frame drawn, sampled):
`geo_parse_np_s` 13% and `model2_3d_process_polygon<4>` 12% of the samples.
Within them it is spread out: the polygon ROM reads (`GeoPtr` through the
page cache), the clip-plane dot products, the perspective divide
(`apply_focus`), the `GeoPoly` set-up. The matrix multiplies are a small part.

**Straight to the PVR: not done.** The separate pass it would remove costs
about 3-4% of the samples: `Renderer::draw` (4.5%, including the material
builds), `Renderer::polygon` 1.2%, the sort 0.6%, the new `GeoPoly`'s
zeroing about 1.2%. The clipping, divide and lighting stay either way. The
PVR's own translucent sort is per triangle, not the game's z order, so
direct submission would still need the order kept. Not worth its
complexity for that.

## Tile layers: only the lines that changed

`Video::compose16` (`video.cpp`, M2_DC_SPEED) composes a 16-bit layer
buffer (back or front) again only on the lines that can differ:
- **Rows of tiles rebuilt.** `build_layer` marks the rows it rebuilt, per
  category and pixmap layer (`dirty_rows_`). Screen line y shows pixmap row
  `(y + vscroll) & 511` of a layer, or of its split pair, in every path of
  `draw()`.
- **Line-scroll entries.** With line scroll on (scroll word bit 15), a
  layer's line-scroll table entries that changed mark only their lines.
- **Every line** when a pen changes, or when a layer the buffer uses (now or
  at the last compose) changes another of its inputs: vscroll, mode word,
  window mask, or the scroll word outside line-scroll mode.

`draw_rect` and `tilemap_draw` skip lines outside `line_filter_`. Exact:
dcmemcheck's per-frame layer hashes for race_basic and attract_long (15,000
frames, frame skip 2) are identical to the M2_DC_SPEED-off references.

race_basic, every 2nd frame drawn: 237.0 -> 228.5 s (tile layers 5.6 -> 4.1
ms a frame). The front layer (the HUD) went from 384 lines on every drawn
frame to 128 lines on average (528 of 2146 composes still whole). The back
layers scroll and cover nearly every line, so they stay whole (1611 of 1749
composes). Marking only the rows of a sideways-scrolled layer that have
tiles made no difference (they have tiles on nearly every line) and was
taken out.

## The scenery layer on the PVR

In the race the back layers are almost all pixmap layer 2: split mode 1
(mode word 0x5006) with the split line off the screen, one scroll, no line
scroll, layer 3 drawing nothing. Its opaque pass cost 2.4 ms a frame, a
palette lookup per pixel on every drawn frame. Now (M2_DC_SPEED;
`Video::scroll_mode` decides each drawn frame from `draw()`'s own
conditions) the runtime leaves layers 2 and 3 out of the back buffer, which
then holds layers 1 and 0 as ARGB1555, see-through where they draw nothing.
The renderer keeps layer 2's whole 512 x 512 pixmap converted in a video
RAM texture (512 KB, of the 0.72 MB that was free). It converts only the
tiles `build_layer` rebuilt (all of them after a pen change), after
`pvr_wait_ready`. It draws that texture as one opaque quad at the scroll
(texture coordinates past 1 wrap like the pixmap), then the back buffer
over it, first in the translucent list.

- **Exact, except green's low bit** on the back buffer's own pixels (layers
  1 and 0, now ARGB1555 instead of RGB565). dcmemcheck rebuilds the back
  layer as the PVR shows it. With `--mask-green` it matches the
  `--no-scroll-layer` run on every frame: race_basic and attract_long, at
  frame skip 2 and 1. With `--no-scroll-layer` it still matches the
  M2_DC_SPEED-off references. Screenshots in Flycast: the sky scrolls and
  sits where it did.
- race_basic, every 2nd frame drawn: 228.5 -> 218.2 s (tile layers 4.1 ->
  1.5 ms a frame). The scrolled layer is on for most of the race; about
  24,000 tile conversions over it (about six whole layers' worth, mostly
  palette changes).

## The geometrizer's reads in blocks

Line-level samples showed the geometrizer's ROM reads (each `*input++`
copying a `GeoPtr` and going through `RomSource::dword` and `page_fast`, and
the texture words through `RomSource::word`) at about 6% of all the game's
time. `GeoPtr::read` and `GeoPtr16::read` (M2_DC_SPEED) copy n words a page
(or up to the wrap) at a time: the same words. `geo_parse_np_s` reads each
point and normal as one block of 3 (`read_point`), and
`model2_3d_process_polygon` its texture coordinates and header as blocks.
dcmemcheck (`--no-scroll-layer`) identical to the references, polygon
hashes included. race_basic 218.2 -> 212.8 s (geometry 12.3 -> 11.2 ms a
frame).

Not a gain: the renderer reusing each polygon's material from the
materials pass instead of a second hash lookup (kept: simpler, 213.0 s).
The polygon step (about 4.5 ms a drawn frame for about 1,400 polygons) is
not the DMA waits (`PROFILE draw` now prints them: under 1 ms).

## Two remembered ROM pages; finiteness in the FPU

A line profile (now over every sampled line, `--sample` with 128-byte
buckets: the 64-byte ones ran the sampling build out of RAM) showed
`GeoPtr16::read` and `DiscRom::page` at about 6%. A polygon's texture
coordinates and its texture header are in different pages of the texture
ROM, read in turn, and `RomSource::page_fast` remembered one page per
region: every read went to the virtual `page()`. It now remembers two
(M2_DC_SPEED; `forget()` clears both). The renderer's `polygon()` tested
each vertex with three `finite()` calls (float to integer through memory,
1.6%); it now tests `(x - x) + (y - y) + (z - z) != 0` (NaN exactly when one
is infinite or NaN), and `pz - pz` for the depth. dcmemcheck identical to
the references; the same polygons drawn. race_basic 212.8 -> 205.3 s
(geometry 11.2 -> 10.2, drawing 3.9 -> 3.5 ms a frame).

- **`build_dreamcast.py` fixes `.d` files before make.** A compile that
  fails still writes its `.d`, without the Makefile's FIXDEP (C:/ paths to
  /c/), and make then never matched that object's dependencies: a changed
  `game/inputs.h` (a `--sample` build) did not rebuild `main.o`.

## Instruction counts in registers

`++ls.count` (a 64-bit count in memory) compiled to nine SH-4 instructions
on every i960 instruction; a register-only instruction is otherwise two to
five. `fast_gen.py` now counts in a local (`n`) and adds it to `ls.count`
before every instruction that is not register-only (its body may call the
runtime, which reads the count), at `recheck:` and at `dispatch:` (before
every return). race_basic 205.3 -> 197.1 s (core 23.3 -> 21.7 ms), 0.11 MB
more RAM free (smaller code). dcfastcheck identical to the references on
every frame (race_basic, attract_long); all 100 checkpoints match.

The TGP's code did the same with `Tgp::count`. `fast_gen.py` run on
`gen/daytona_tgp` (the Makefile's new `GEN_TGP`) keeps it in a local of
`tgpgen::run`, stored back at every return: nothing `run` calls reads it.
197.1 -> 195.1 s (core 21.2 ms). The TGP code it writes refuses to build
with M2TGP_WITH_HOOK (a hook would see a stale count).

- **Measured, not kept:** the board's single-page read and write caches
  (`fast_read_page_`) as two entries: no change (206.3 s against 205.3). Its
  slow path per frame: about 1,900 ROM reads, 1,800 RAM accesses, 950
  device accesses.
- **ipprof** (desktop): apart from the frame-wait loop at 0x1394 (76% of
  the i960's instructions, skipped), nothing stands out: the busiest 64-byte
  block is 0.4%. The other loop polling 0x500000 (0x13d4) is negligible.

## Renderer: radix sort, colour cache

The draw order: a stable radix sort of the 24-bit key (window, z), a byte
a pass, over entries built in descending index order: the order the
`std::sort` comparator (key ascending, index descending) gave. A polygon's
vertex colour (`textured_colour`/`solid_color`) through a 1,024-slot cache
stamped per drawn frame: within a frame it depends only on the colour
entry and the light. The same polygons drawn; race_basic 195.1 -> 193.4 s
(drawing 3.5 -> 3.1 ms a frame).

## Library copies out of the hot paths

memcpy and memset were 3.2% of the samples (`--sample`'s CALLER lines):
`polys.emplace_back()` zero-filling each kept `GeoPoly` (1.5%) and the
i960 register frames copied in `Cpu::do_call`/`do_ret_0` (1.6%; GCC calls
the library for a 64-byte copy on the SH-4). With M2_DC_SPEED, `GeoPoly`'s
vertices are in an anonymous union under an empty constructor (only
`v[0..num_vertices)` is read, and it is written first; the other fields
keep their initialisers), and the frames are copied by `copy_frame`,
16 straight-line loads and stores. dcmemcheck identical; desktop unchanged.
race_basic 193.4 -> 187.5 s (core 21.2 -> 20.7, geometry 10.2 -> 9.5 ms).
`build_layer`'s 32-byte compare of each block of 16 tile values
(`same_block`, eight word loads a side instead of a memcmp call): 187.5 ->
186.5 s (tile layers 1.5 -> 1.3 ms). The other library helpers left in the
hot objects (`___movmem` in vector growth, `___umoddi3` once a frame,
`___unorddf2` in `geo_parse_nn_ns`) are not on hot paths.

## Culled polygons: no texture reads, no lighting

`model2_3d_process_polygon` read each polygon's texture coordinates and
header (texture ROM) and its LOD (log RAM) before `check_culling`, which
uses none of them. With M2_DC_SPEED those reads happen only for a polygon
not culled (tp and th keep the addresses they were set to, before the
address updates). 186.5 -> 181.7 s (geometry 9.5 -> 8.6 ms).

`geo_parse_np_s`'s direct path skips the lighting (diffuse, the specular
chain, the double-precision LOD distance) for a polygon check_culling will
cull whatever its light: single-sided and facing away (its luma word keeps
the face bit, bit 23, which luminance up to 255 never sets), or link type
0. Neither word is read for a culled polygon. 181.7 -> 180.1 s (geometry
8.3 ms). dcmemcheck identical; desktop unchanged.

## Dispatch: two-level entry switch, binary chunk search

About 4,300 dispatches a frame (counted: calls and returns that leave a
chunk, interrupts, rechecks). Each went through the chunk's entry switch on
c.m_IP, a case per instruction 4 apart, which GCC at -Os compiles to a
compare tree about 11 deep (a dense table over the chunks' ranges would be
573,000 entries). `fast_gen.py` now writes it as a switch on the 128-byte
block (a jump table) of switches on the IP (5 deep), except chunk_017
(2 MB of range), and `gen::run` finds the chunk by binary search instead
of scanning 26 ranges. The calls to a constant target already jump
straight to it when it is in the same chunk (m2recomp does that); all
2,269 remaining leave their chunk. race_basic 180.1 -> 176.9 s (core 20.6
-> 19.9 ms), no RAM cost; dcfastcheck identical every frame.

- **Measured, not kept (again):** the CPU's memory helpers checking the
  board's last page and calling it without the virtual call: 180.0 s
  against 180.1.

## The count a call sees: Lockstep::pending

Before every instruction that calls the runtime, the generated code added
its register count to the 64-bit `ls.count` (about seven SH-4
instructions). Only three things read the count during such a call: the
board's UART scheduling its shift callback, the frame-wait skip and
`Lockstep::poke` (in free run, `on_take` does not). With M2_DC_SPEED the
code stores the count in `Lockstep::pending` instead (one store) and adds
it to `count` only at its rechecks and dispatches, which clear `pending`;
those three read `Lockstep::now()` (count + pending). race_basic 176.6 ->
175.9 s, and the generated code 210 KB smaller (0.55 -> 0.76 MB free).
dcfastcheck identical every frame; desktop unchanged.

## Less serial output in the play build

Printing cost 0.3-0.45 ms a frame in the recorded race (`scif_write` about
1% of samples), and on a console the serial port runs at 115,200 baud with
KOS waiting on its FIFO. The play build (no `--inputs`) no longer prints
the TRACE line (only a recorded race has a desktop trace to compare with)
and prints its PROFILE and GAME lines every 1,800 frames instead of 300
(about 800 bytes a report). Recorded-race builds print as before.

- **Measured, not kept:** chunk_000 (the busiest i960 chunk) at -O2 with
  the RAM freed above: 176.0 s against 175.9 (+0.02 MB used).

## Small renderer and geometrizer notes

- `material_for` looks a material up in a 256-slot direct-mapped cache
  (stamped per drawn frame) before the map: the same pointer. 176.9 ->
  176.6 s (materials 1.7 -> 1.5 ms a drawn frame).
- **Measured, not kept:** clipping on positions first so polygons clipped
  away (19% of those not culled; 1.6% are cut) skip their texture reads,
  re-clipping the cut ones with their coordinates: 179.3 s against 176.9
  (the extra zeroing and copies cost more than the reads).

## Where the time is now (race, every 2nd frame drawn, Flycast)

- **PROFILE waits line** (frontend timers): disc reads for ROM pages, the
  sound frame and serial printing together under 1 ms a frame. The core,
  geometry, tile and draw figures add up to the frame. `genwait_wait` in a
  `--sample` profile is the sampler's own report being printed, not a wait
  in the game.
- **All samples** (`--sample` now also prints `COARSE` 16 KB buckets, every
  other report; `pc_profile.py` sums them): geometrizer about 17%, generated
  i960 code 15%, tile layers 15% (before the change above), the rest of the
  runtime (board memory and port access, interrupts) 13%, generated TGP
  code 9%. The i960 code's samples are spread over a dozen chunks (the
  busiest 3.5%), so compiling the hot ones at -O2 would gain about 1%.
- **Flycast models neither the SH-4's caches nor memory latency.** Its
  timing is about one cycle per SH-4 instruction, so these figures measure
  instruction counts. On a console, cache misses add to it (16 KB of
  operand cache against 1 MB of work RAM, the ROM page cache and the layer
  buffers).
- **A double in the geometrizer:** `distance = coef * fabs(dotp) * lod`
  (`::fabs`, the double one) switches the FPU to double precision and back
  for each polygon. The desktop computes the same in double, so changing it
  changes the polygons' LOD bits. Small; not changed.

## What not to re-propose

- The i960's generated code at -O2: after the frame-wait skip it runs about
  32,000 instructions a frame. Its samples are 15% of all (the COARSE
  histogram; the 200 busiest 64-byte buckets missed them, which had made
  them look under 1%), spread over a dozen chunks; -O2 for everything does
  not fit in RAM.

- Measurement hooks in `src/runtime`: not needed. `romuse` watches the ROM
  buffers from outside with guard pages, `ftzcheck` sets MXCSR.
- Building the KOS side through `bash -c` from Git Bash (hangs).
- iostreams (`<fstream>`, `<sstream>`, `<iostream>`) in anything the
  Dreamcast build links: their start-up stops KOS before `main`.
- DreamSDK's installed KOS master with its R4 toolchain (C++ programs stop at
  startup; see above). Use `extern/kos-dc`.
