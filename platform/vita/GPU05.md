# GPU05 Vita crash fix

GPU05 fixes the GPU04 texture row-stride corruption. `vita2d_texture_get_stride()` returns bytes per row; GPU04 used that byte count as a `uint32_t` element count in both framebuffer-layer uploads and cached material uploads. For the 496x384 layer texture the correct stride is 1,984 bytes, while the bad pointer arithmetic advanced 7,936 bytes per row.

The uploaded crash dump records `r6 = 0x1f00` (7,936), matching the erroneous row advance. GPU05 keeps all texture-row addressing byte-based and validates stride/alignment before writing. It also adds startup stage markers around SDL, vita2d, framebuffer texture and audio initialization.

This fixes a memory-corruption crash only. GPU FAST remains experimental and approximate with respect to Model 2 perspective/mipmap behavior. CPU EXACT remains the parity reference.
