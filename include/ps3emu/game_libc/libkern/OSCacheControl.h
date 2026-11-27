/*
 * ps3recomp - <libkern/OSCacheControl.h> for a freestanding game module
 * (PS3_GAME_FREESTANDING, ps3emu/game_freestanding.h).
 *
 * For sse2neon (ps3emu/ps3_sse.h) on macOS only: its inline _mm_clflush calls
 * this. Nothing calls that, and the function is not in
 * PS3_GAME_HOST_CRT_FUNCS, so a source that did would fail to link.
 */
#ifndef PS3_GAME_LIBC_OSCACHECONTROL_H
#define PS3_GAME_LIBC_OSCACHECONTROL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void sys_icache_invalidate(void* start, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PS3_GAME_LIBC_OSCACHECONTROL_H */
