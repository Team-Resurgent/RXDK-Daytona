# OPT03 handoff - 30 September 2026

Basis: the DIAG02 device capture measured about 81 ms in video in its lighter
section and 364-374 ms later; total frame time reached 415.64 ms. The capture
reported CPU 333 MHz, GPU 111 MHz and bus 222 MHz. No device dump, ROM memory or
generated game code is included in this update.

Implemented: exact incremental tile decoding, cached 2D composition, packed
texture reads, per-polygon mip setup, adaptive per-render lighting tables,
covered-span rejection, persistent sort indices and a reference-build switch.
The existing DIAG02 logging remains, with OPT03 identification and video-stage
subtimers. Desktop opt-in is not changed; only the Vita target enables the macro.

Failed experiments retained in the record:
- Eagerly building a 128-entry lighting table for every polygon regressed the
  dense synthetic host test (31.87 ms versus 26.52 ms reference). It is not shipped.
- Lazy tables at the first visible row corrected the dense test but regressed
  5,000 small textured quads (5.75 ms versus 4.60 ms reference). The shipped path
  builds a table only for spans >=32 pixels; smaller spans use direct lighting.
- A Clang -Werror check found the old packed-texel helper unused in optimized
  builds. It is now compiled only in the reference path, not warning-suppressed.

Test harness detail: the generated reference sources rename the optimization
macro. This keeps reference header layouts identical to the reference objects
even when the harness itself enables optimization for the real renderer.

Reference files used for independent local differential tests were fetched from
psvita-native-frontend and verified against their Git blob IDs:
- video.cpp: 43ef7c4af9bb7b078b922971b24aac23964ce6e4
- video.h: 184569ebe7db384b3148ecf732b42325cdee86b9
- raster.cpp: 1913f3b3e0bca030ae1b6ad99d3ac95dd5777cd4
- raster.h: 6e65004b735ed7cf790f56677c2ac815317325df
- geo.h: 7b5d23b6644ffcbf552d69a0188f0791d2b39b4a

Next: rebuild the real Vita VPK from the existing ROM-generated C++, compare the
same slow scene in OPT03 and reference mode, collect the split video timings,
and check saved-state behavior, memory headroom and a full race. Do not infer
Vita frame rates by multiplying the old log by synthetic desktop speedups.
This remains a CPU rasterizer; no GPU rewrite, overclock or altered filtering
is hidden behind the update. The system cannot be called full-speed from host
unit tests.
