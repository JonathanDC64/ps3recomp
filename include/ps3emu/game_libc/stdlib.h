/*
 * ps3recomp - <stdlib.h> for a freestanding game module (PS3_GAME_FREESTANDING,
 * ps3emu/game_freestanding.h).
 *
 * Declarations only:
 *   - getenv, atoi, strtoul: jump thunks to the host's C runtime
 *     (PS3_GAME_HOST_CRT_FUNCS). In the exe, getenv is boot_main's cached one;
 *   - the byte swaps: compiler builtins once declared (spu_context.h's
 *     SPU_BSWAP32 uses _byteswap_ulong);
 *   - malloc and free: for clang's <mm_malloc.h> only, which <intrin.h>
 *     includes. Nothing calls them, and the module has no heap.
 */
#ifndef PS3_GAME_LIBC_STDLIB_H
#define PS3_GAME_LIBC_STDLIB_H

#include <stddef.h>
#include <limits.h>   /* as the UCRT's <stdlib.h> does */

#ifdef __cplusplus
extern "C" {
#endif

char*              __cdecl getenv(const char* name);
int                __cdecl atoi(const char* s);
unsigned long      __cdecl strtoul(const char* s, char** end, int base);

unsigned short     __cdecl _byteswap_ushort(unsigned short v);
unsigned long      __cdecl _byteswap_ulong(unsigned long v);
unsigned long long __cdecl _byteswap_uint64(unsigned long long v);

void*              __cdecl malloc(size_t size);
void               __cdecl free(void* block);

#ifdef __cplusplus
}
#endif

#endif /* PS3_GAME_LIBC_STDLIB_H */
