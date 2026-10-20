// Exercises the production arena against the narrow, shared host API shim.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "gpu_memory.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {
// A 256 KiB-aligned block, like the Vita's. Aligned at run time: Windows
// compilers cap alignas at 8 KiB (C2345, clang-cl likewise).
constexpr std::uintptr_t kBlockAlign = 262144;
unsigned char raw_storage[524288 + kBlockAlign];
unsigned char *const storage = reinterpret_cast<unsigned char *>(
    (reinterpret_cast<std::uintptr_t>(raw_storage) + kBlockAlign - 1) & ~(kBlockAlign - 1));
int alloc_calls, free_calls, map_calls, unmap_calls;
int failure_stage;
std::size_t allocated_size;
}

SceUID sceKernelAllocMemBlock(const char *, SceKernelMemBlockType, SceSize bytes, void *) {
    ++alloc_calls;
    allocated_size = bytes;
    return failure_stage == 1 ? -11 : 42;
}

int sceKernelGetMemBlockBase(SceUID id, void **out) {
    assert(id == 42);
    *out = failure_stage == 6 ? nullptr : storage;
    return failure_stage == 2 ? -12 : 0;
}

int sceKernelFreeMemBlock(SceUID id) {
    assert(id == 42);
    ++free_calls;
    return failure_stage == 5 ? -15 : 0;
}

int sceGxmMapMemory(void *base, SceSize bytes, int flags) {
    assert(base == storage && bytes == allocated_size && flags == SCE_GXM_MEMORY_ATTRIB_READ);
    ++map_calls;
    return failure_stage == 3 ? -13 : 0;
}

int sceGxmUnmapMemory(void *base) {
    assert(base == storage);
    ++unmap_calls;
    return failure_stage == 4 ? -14 : 0;
}

int main() {
    // No invalid unmap/free on construction failures; successful blocks are
    // released even if obtaining their base or mapping them for GXM fails.
    for (int failure = 1; failure <= 3; ++failure) {
        failure_stage = failure;
        alloc_calls = free_calls = map_calls = unmap_calls = 0;
        {
            vita::GpuMemoryArena arena(1024, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
            assert(!arena.ok() && arena.error() == -10 - failure);
            assert(arena.allocate(1, 1) == nullptr && arena.capacity() == 0);
        }
        assert(alloc_calls == 1 && free_calls == (failure == 1 ? 0 : 1));
        assert(unmap_calls == 0);
    }
    failure_stage = 6;
    const int before_null_free = free_calls;
    {
        vita::GpuMemoryArena arena(1024, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
        assert(!arena.ok() && arena.error() < 0);
    }
    assert(free_calls == before_null_free + 1);

    failure_stage = 0;
    alloc_calls = free_calls = map_calls = unmap_calls = 0;
    {
        vita::GpuMemoryArena arena(4097, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
        assert(arena.ok() && arena.capacity() == 8192 && arena.used() == 0);
        assert(arena.allocate(3, 1) == storage);
        assert(arena.allocate(1024, 64) == storage + 64);
        assert(arena.used() == 1088);
        assert(arena.allocate(1, 0) == nullptr);
        assert(arena.allocate(1, 3) == nullptr);
        assert(arena.allocate(0, 64) == nullptr);
        assert(arena.allocate(std::numeric_limits<std::size_t>::max(), 64) == nullptr);
        assert(arena.used() == 1088);
        assert(arena.allocate(8192 - 1088, 1) == storage + 1088);
        assert(arena.allocate(1, 1) == nullptr);
        arena.reset();
        assert(arena.allocate(8192, 64) == storage);
        failure_stage = 4;
        arena.shutdown();
        assert(arena.ok() && free_calls == 0 && arena.error() == -14);
        failure_stage = 0;
        arena.shutdown();
        assert(!arena.ok() && free_calls == 1 && arena.capacity() == 0);
        arena.shutdown();
        assert(free_calls == 1);
    }
    assert(map_calls == 1 && unmap_calls == 2 && free_calls == 1);

    // If explicit shutdown fails, destruction must not issue another GXM
    // call after the renderer may already have called vita2d_fini().
    {
        vita::GpuMemoryArena arena(4096, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
        failure_stage = 4;
        arena.shutdown();
    }
    assert(unmap_calls == 3 && free_calls == 1);
    failure_stage = 0;
    {
        vita::GpuMemoryArena arena(4096, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
        failure_stage = 5;
        arena.shutdown();
        assert(!arena.ok() && arena.error() == -15);
        failure_stage = 0;
        arena.shutdown();
    }
    assert(unmap_calls == 4 && free_calls == 3);

    {
        vita::GpuMemoryArena arena(1, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW);
        assert(arena.ok() && arena.capacity() == 262144);
        for (int i = 0; i < 4096; ++i)
            assert(arena.allocate(64, 64) == storage + i * 64);
        assert(arena.allocate(64, 64) == nullptr);
    }
    const int before = alloc_calls;
    {
        vita::GpuMemoryArena empty(0, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW);
        vita::GpuMemoryArena huge(std::numeric_limits<std::size_t>::max(), SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW);
        vita::GpuMemoryArena bad_type(1024, 0);
        assert(!empty.ok() && !huge.ok() && !bad_type.ok());
    }
    assert(alloc_calls == before);
    std::puts("Vita GPU arena: bounds, alignment, reset, shutdown and allocation failure checks passed");
}
