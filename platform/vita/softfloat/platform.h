// Project-owned SoftFloat platform configuration. Do not enable
// SOFTFLOAT_INTRINSIC_INT128: ARMv7 does not provide GCC's __int128 type.
#pragma once
#define LITTLEENDIAN 1
#ifdef __GNUC_STDC_INLINE__
#define INLINE inline
#else
#define INLINE extern inline
#endif
#define SOFTFLOAT_BUILTIN_CLZ 1
#include "opts-GCC.h"
