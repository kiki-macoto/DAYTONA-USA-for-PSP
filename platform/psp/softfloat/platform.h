// SoftFloat platform configuration for the PSP (Allegrex, 32-bit MIPS, GCC).
// As platform/dreamcast's: SOFTFLOAT_INTRINSIC_INT128 stays off, a 32-bit
// target has no __int128; the portable 128-bit paths give the same results.
#pragma once
#define LITTLEENDIAN 1
#ifdef __GNUC_STDC_INLINE__
#define INLINE inline
#else
#define INLINE extern inline
#endif
#define SOFTFLOAT_BUILTIN_CLZ 1
#include "opts-GCC.h"
