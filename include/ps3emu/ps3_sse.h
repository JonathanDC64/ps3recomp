#pragma once
/* SSE intrinsics on arm64.
 *
 * The SIMD paths in the runtime and the lift are written with SSE intrinsics,
 * each behind its x86 guard (__SSE4_1__, __x86_64__ / _M_X64 or __SSE2__) with
 * a scalar loop beside it. On arm64 this includes sse2neon
 * (third_party/sse2neon), which provides the same intrinsics on NEON, and
 * defines PS3_SSE2NEON; a guard written as `defined(__SSE4_1__) ||
 * defined(PS3_SSE2NEON)` then takes the SIMD path there too. On x86 it
 * defines nothing, and each site includes its own <*mmintrin.h> as before.
 *
 * GCC and Clang only (macOS, Linux): under MSVC's front ends sse2neon takes
 * C++ only, and Windows on arm64 is not a target. Include it outside any
 * extern "C" block. PS3_NO_SSE2NEON keeps arm64 on the scalar loops. */
#if defined(__aarch64__) && !defined(_MSC_VER) && !defined(PS3_NO_SSE2NEON)
#include "../../third_party/sse2neon/sse2neon.h"
#define PS3_SSE2NEON 1
#endif
