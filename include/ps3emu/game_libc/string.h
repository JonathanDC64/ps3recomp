/*
 * ps3recomp - <string.h> for a freestanding game module (PS3_GAME_FREESTANDING,
 * ps3emu/game_freestanding.h).
 *
 * The compiler inlines small fixed-size copies (the lifted code's memcpy of 2,
 * 4, 8 or 16 bytes), as with the UCRT's header: this is a hosted compile. For a
 * large or variable size it may call these, so runtime/ppu/game_libc.c
 * defines them.
 */
#ifndef PS3_GAME_LIBC_STRING_H
#define PS3_GAME_LIBC_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void*  memcpy(void* dst, const void* src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
void*  memset(void* dst, int c, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);

#ifdef __cplusplus
}
#endif

#endif /* PS3_GAME_LIBC_STRING_H */
