# GPU04 experimental Vita 3D path

GPU04 moves the expensive Model 2 polygon raster stage out of the ARM CPU
reference renderer and submits polygons through libvita2d/sceGxm. The existing
CPU rasterizer remains available at runtime as **CPU EXACT** and is still the
parity reference.

Build prerequisites:

```sh
vdpm sdl2
vdpm libvita2d
python3 scripts/build_vita.py --gpu-fast --build-dir build/vita-gpu04 --jobs 4
```

The package remains `build/vita-gpu04/daytona_vita.vpk` and uses the same
`ux0:data/daytona93/daytona93.zip` user-supplied ROM location.

## Accuracy status

This first GPU backend is intentionally marked experimental. It preserves the
existing CPU System-24 background/foreground layers and polygon ordering, but
its 3D texture path is an approximation:

- Model 2 packed 4-bpp textures are expanded into cached RGBA textures.
- Polygon palette/luma state is baked into each cached material.
- GXM performs triangle rasterization and bilinear filtering.
- Texture mip selection and microtexture blending are not yet reproduced.
- vita2d's stock orthographic textured shader interpolates UVs affinely; the
  CPU reference renderer remains the source of truth for perspective-correct
  interpolation and exact transparent-texel thresholds.
- oversized Model 2 textures are capped to 512x512 cached material textures to
  bound Vita memory use.

The menu exposes **RENDERER: GPU FAST / CPU EXACT** so visual differences can
be compared immediately without rebuilding. GPU FAST never changes game logic,
TGP output, i960 execution, sound timing or input handling.

## Diagnostics

`ux0:data/daytona93/vita-diag.log` includes `gpu04:` records with simulation
FPS, CPU frame time, GPU submission time, CPU raster time, tile/composition
cost, material count and texture-cache memory. In GPU FAST mode the CPU
`raster_ms` measurement should remain at zero; a non-zero value means the
external 3D path was not active for that frame.

The next parity work is a custom GXM shader path reproducing the reference
mipmap/microtexture and perspective interpolation rules, followed by frame
comparisons against CPU EXACT and MAME captures.
