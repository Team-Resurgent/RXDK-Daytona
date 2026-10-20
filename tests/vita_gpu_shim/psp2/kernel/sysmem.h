// Host-test declarations only; excluded from Vita builds.
#pragma once
#include <cstdint>
using SceUID = int;
using SceSize = uint32_t;
using SceKernelMemBlockType = int;
constexpr SceKernelMemBlockType SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW = 1;
constexpr SceKernelMemBlockType SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE = 2;
SceUID sceKernelAllocMemBlock(const char *, SceKernelMemBlockType, SceSize, void *);
int sceKernelGetMemBlockBase(SceUID, void **);
int sceKernelFreeMemBlock(SceUID);
