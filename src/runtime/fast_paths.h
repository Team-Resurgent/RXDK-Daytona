// Fast paths of the shared runtime: the same results, less work. Each flag is
// one subject, so a port turns on the ones that pay on its host. The
// Dreamcast's M2_DC_SPEED turns them all on (and keeps its own, tied to its
// display or its -Os build, under M2_DC_SPEED itself); the Vita build defines
// the flags it uses (platform/vita/CMakeLists.txt). Desktop builds define
// none: the reference.
//
// M2_FAST_GEN  runtime support of the i960 code rewritten by fast_gen.py
//              (platform/dreamcast/scripts) and of m2recomp --fast_inaccuracy:
//              Cpu::work_ram, gen::wram_*, Lockstep's pending/check/set_end/epoch.
// M2_FAST_BUS  M2Board: the TGP's FIFO, function port, status and buffer RAM
//              decoded before the page table; the last plain RAM/texture page
//              read and the last plain RAM page written, remembered.
// M2_FAST_TGP  the TGP's FIFOs as a ring (WordFifo), read and written by the
//              TGP directly instead of through TgpBus's virtual calls.
// M2_FAST_CPU  register frames copied as 16 words; spills to and refills from
//              work RAM straight through Cpu::work_ram (needs M2_FAST_GEN).
// M2_FAST_GEO  geometrizer: block reads of points and texture words, no
//              zero-fill of polygons and clip buffers, culled polygons skip
//              their texture reads and lighting, polygon words straight into
//              the rasterizer's slots.
#pragma once

#ifdef M2_DC_SPEED
#ifndef M2_FAST_GEN
#define M2_FAST_GEN 1
#endif
#ifndef M2_FAST_BUS
#define M2_FAST_BUS 1
#endif
#ifndef M2_FAST_TGP
#define M2_FAST_TGP 1
#endif
#ifndef M2_FAST_CPU
#define M2_FAST_CPU 1
#endif
#ifndef M2_FAST_GEO
#define M2_FAST_GEO 1
#endif
#endif

#if defined(M2_FAST_CPU) && !defined(M2_FAST_GEN)
#error "M2_FAST_CPU spills register frames through Cpu::work_ram: define M2_FAST_GEN too"
#endif
