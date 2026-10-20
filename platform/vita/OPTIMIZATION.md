# OPT03: exact CPU-renderer optimizations for Vita

This update builds on DIAG02. It retains the 192 MiB startup heap, direct
`vita-diag.log` output, the one-step presentation policy and the existing
input, audio, ROM validation and save behavior. It does not supply generated
game code or change the recompiler, board timing, game logic or clock speeds.
It is not a new GPU renderer and is not a full-speed claim.

## Build and identify the installed package

Apply `apply_vita_optimisation.py` from the repository root. The installer
requires the known DIAG02 source, refuses unknown edits and backs up replaced
files under `build/opt03-backups/`. It does not modify ROMs, generated sources,
SDK files or saved data. Then:

```sh
python3 scripts/build_vita.py --build-dir build/vita-opt03 --jobs 4
```

Install `build/vita-opt03/daytona_vita.vpk`. Keep the existing complete ZIP
at `ux0:data/daytona93/daytona93.zip`. No host recompile of the ROMs is needed.
The launcher reads `DAYTONA RECOMP - OPT03`, the package version is 01.01,
and `ux0:data/daytona93/vita-diag.log` contains:

```text
render_opt: OPT03 enabled - exact tile/layer cache, mip setup, lighting LUT and packed texture reads
```

The normal desktop build does not define `M2_VITA_RENDER_OPT`, so it retains
the reference rendering path. The Vita definition is PUBLIC on its runtime
CMake target, because it affects class layouts and must reach all consumers.
For an on-device A/B comparison (use the same scene, settings and clocks):

```sh
python3 scripts/build_vita.py --build-dir build/vita-reference --reference-renderer --jobs 4
```

That package still includes OPT03 diagnostics, but logs `optimization disabled`.
Only one package with title ID DAYT00093 is installed at a time. Copy each log
before replacing/reopening the application; launch truncates the log.

## What changes

* Character and tile snapshots update only changed 8x8 tiles. Byte comparisons
  rather than a checksum catch every change, including direct memory writes in
  replay/test code. All four layers remain current, including off-screen tiles
  and glyphs first used after being modified while unused.
* The composed 2D background and foreground are reused until a relevant tile,
  character, palette, scrolling or window-mask change. Foreground transparency,
  layer priority and the reference's sticky palette-refresh flag are retained.
  The final image still has native 496x384 resolution and the same aspect ratio.
* Mip descriptors are prepared once per polygon, not for every sampled pixel.
  Perspective division, FP accumulation, bilinear interpolation, mip blending,
  microtextures, transparency and checkerboard coverage are unchanged.
* Packed texture sampling shares integer address calculations. An aligned 2x2
  footprint reads its four texels from one packed cell; all other footprints
  use the exact original fold/mask equations and local-coordinate nibble parity.
* A fixed 64-entry lighting cache maps the 128 possible luminance indices to
  final RGB. Keys include palette color, polygon lighting and luminance-table
  row, and entries are invalidated every raster render. Tables are built only
  on demand for spans at least 32 pixels wide; small spans keep direct lighting
  calculations, avoiding a measured small-polygon regression.
* Completely covered scanlines are rejected before shading. Partially covered
  scanlines keep every original FP increment: no approximate skip-ahead, divide
  substitution, fast-math, frame dropping or quality-reduction shortcut is used.
* Polygon sort-index storage is reused. The original window/z/newest-first
  ordering is unchanged.

The added persistent snapshot/background/lighting arrays use approximately
1.38 MiB, plus the retained sort-index vector capacity. This is allocated within
the existing heap, not by increasing its reservation. Peak memory on Vita still
needs measurement. No simulation thread or new SDK dependency is introduced.

These decisions follow the design document's Architecture separation and the
standing parity-before-optimization rules. Host differential tests are evidence
against the previous source, not a substitute for a recorded full-race check.

## New measurements

The existing `perf:` fields remain, with these additions:

| Field | Meaning |
| --- | --- |
| tile_cache_ms | Palette refresh, snapshot comparison and changed-tile decoding |
| tile_draw_ms | 2D background/foreground layer drawing (near zero on cache hits) |
| raster_ms | Polygon rasterization (zero when the board reuses its 3D frame) |
| compose_ms | Final background/3D/foreground copies |
| tiles_rebuilt_avg | Average decoded 8x8 tile count per board frame |
| chars_changed_avg | Average changed 32-byte character count per board frame |
| layers_redrawn_pct | Percentage of board frames that redrew 2D layers |

Times use the same host counter and units as `video_ms`. The smaller timers
are components of video time, not extra work to add on top of `video_ms`.

## ROM-free host verification

```sh
python3 scripts/test_vita_renderer.py
python3 scripts/test_vita_renderer.py --sanitize
python3 scripts/test_vita_renderer.py --bench
c++ -std=c++20 -O2 -Wall -Wextra -Werror -Isrc tests/test_vita_texel.cpp -o build/test_vita_texel
build/test_vita_texel
python3 -m unittest discover -s tests -p test_build_vita.py -v
```

The renderer test compiles separate reference and optimized translation units.
It runs focused invalidation cases, 160 randomized full-screen cases and 640
randomized raster cases with 3-8 vertices, all four shading modes, scroll/window
modes, mip/microtexture combinations, clipping, ordering and RAM changes. The
packed-texture test compares two million footprints (eight million texels).
`--reference-root PATH` also supports an independently retained old source tree.

Synthetic host benchmark improvements are not Vita gameplay FPS. A full VitaSDK
build/link, on-device comparison, complete race parity and peak memory are not
verified by these host checks. Retain the matching unstripped ELF when testing.
