#pragma once

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace vita {

// One bounded GPU mapping, suballocated without per-texture kernel blocks.
// The owner must finish outstanding GPU work before reset() or shutdown().
// Deliberately accepts only uncached memory types: CPU writes need no separate
// cache flush before the GPU reads the texture/palette data.
class GpuMemoryArena {
public:
    GpuMemoryArena(std::size_t requested_capacity, SceKernelMemBlockType type) {
        std::size_t granularity = 0;
        if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW)
            granularity = 256u * 1024u;
        else if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE)
            granularity = 4096u;
        else {
            error_ = -1;
            return;
        }
        const std::size_t limit = std::numeric_limits<SceSize>::max();
        if (!requested_capacity || requested_capacity > limit - (granularity - 1)) {
            error_ = -1;
            return;
        }
        const std::size_t bytes = (requested_capacity + granularity - 1) & ~(granularity - 1);
        uid_ = sceKernelAllocMemBlock("daytona_gpu_arena", type, SceSize(bytes), nullptr);
        if (uid_ < 0) {
            error_ = uid_;
            return;
        }
        error_ = sceKernelGetMemBlockBase(uid_, &base_);
        if (error_ < 0 || !base_) {
            if (error_ >= 0) error_ = -1;
            sceKernelFreeMemBlock(uid_);
            uid_ = -1;
            base_ = nullptr;
            return;
        }
        error_ = sceGxmMapMemory(base_, SceSize(bytes), SCE_GXM_MEMORY_ATTRIB_READ);
        if (error_ < 0) {
            sceKernelFreeMemBlock(uid_);
            uid_ = -1;
            base_ = nullptr;
            return;
        }
        capacity_ = bytes;
        mapped_ = true;
    }

    // An explicit shutdown may precede vita2d_fini(). If it failed, leave the
    // block to process teardown rather than retrying GXM after finalization.
    ~GpuMemoryArena() { if (!shutdown_attempted_) shutdown(); }
    GpuMemoryArena(const GpuMemoryArena &) = delete;
    GpuMemoryArena &operator=(const GpuMemoryArena &) = delete;
    GpuMemoryArena(GpuMemoryArena &&) = delete;
    GpuMemoryArena &operator=(GpuMemoryArena &&) = delete;

    bool ok() const { return mapped_; }
    void *data() const { return base_; }
    std::size_t capacity() const { return capacity_; }
    std::size_t used() const { return used_; }
    int error() const { return error_; }

    void *allocate(std::size_t bytes, std::size_t alignment) {
        if (!mapped_ || !bytes || !alignment || (alignment & (alignment - 1))) return nullptr;
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(base_) + used_;
        const std::size_t padding = (alignment - (address & (alignment - 1))) & (alignment - 1);
        const std::size_t remaining = capacity_ - used_;
        if (padding > remaining || bytes > remaining - padding) return nullptr;
        auto *out = static_cast<std::uint8_t *>(base_) + used_ + padding;
        used_ += padding + bytes;
        return out;
    }

    // Caller has already waited for all users of the previous allocations.
    void reset() { used_ = 0; }

    // Call explicitly before vita2d_fini(), after a GPU completion fence.
    void shutdown() {
        shutdown_attempted_ = true;
        if (mapped_) {
            const int result = sceGxmUnmapMemory(base_);
            if (result < 0) {
                // Keep the block alive when the GPU mapping could not be
                // removed. Freeing it here would leave GXM a stale address.
                error_ = result;
                return;
            }
            mapped_ = false;
        }
        if (uid_ >= 0) {
            const int result = sceKernelFreeMemBlock(uid_);
            if (result < 0) { error_ = result; return; }
            uid_ = -1;
        }
        base_ = nullptr;
        capacity_ = used_ = 0;
    }

private:
    void *base_ = nullptr;
    SceUID uid_ = -1;
    std::size_t capacity_ = 0;
    std::size_t used_ = 0;
    int error_ = 0;
    bool mapped_ = false;
    bool shutdown_attempted_ = false;
};

} // namespace vita
