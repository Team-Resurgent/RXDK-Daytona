// Host test declarations only. Never included in a Vita build.
#pragma once
#include <cstdint>
using SceUID = int;
using SceSSize = int;
using SceSize = uint32_t;
using SceMode = uint32_t;
constexpr int SCE_O_WRONLY = 2, SCE_O_CREAT = 0x200, SCE_O_TRUNC = 0x400, SCE_O_APPEND = 0x100;
extern "C" {
SceUID sceIoOpen(const char *, int, SceMode);
SceSSize sceIoWrite(SceUID, const void *, SceSize);
int sceIoSyncByFd(SceUID, int);
int sceIoClose(SceUID);
}
