# GPU06 Vita GPU lifetime fix

GPU06 fixes a second GPU FAST crash found after the GPU05 stride correction. The device log reached 331 cached materials and the exact 24 MiB cache limit before the kernel reported a render GPU crash. GPU05 evicted an LRU material by calling `vita2d_free_texture()` immediately while GXM commands could still reference that texture asynchronously. GPU06 retires evicted textures during a scene and physically frees them only after `vita2d_wait_rendering_done()` confirms submitted GPU work is finished.

The material cache limit is raised from 24 MiB to 40 MiB to reduce churn. The vita2d temporary pool is raised from 4 MiB to 8 MiB. Clipping is enabled once per polygon pass instead of once per polygon, identical clip rectangles are reused, and each polygon checks pool headroom before invoking libvita2d helpers whose internal rectangle path does not null-check an exhausted temporary pool. Diagnostics add pool free space and dropped polygon counts.

GPU FAST remains approximate. CPU EXACT remains the reference renderer.
