/*
 * ps3recomp - <malloc.h> for a freestanding game module (PS3_GAME_FREESTANDING,
 * ps3emu/game_freestanding.h).
 *
 * For clang's <mm_malloc.h> only, which <intrin.h> includes on Windows: its
 * inline _mm_malloc and _mm_free use these. Nothing calls them, so nothing is
 * linked, and the module has no heap.
 */
#ifndef PS3_GAME_LIBC_MALLOC_H
#define PS3_GAME_LIBC_MALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* __cdecl _aligned_malloc(size_t size, size_t alignment);
void  __cdecl _aligned_free(void* block);

#ifdef __cplusplus
}
#endif

#endif /* PS3_GAME_LIBC_MALLOC_H */
