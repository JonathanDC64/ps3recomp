/*
 * ps3recomp - <fenv.h> for a freestanding game module (PS3_GAME_FREESTANDING,
 * ps3emu/game_freestanding.h).
 *
 * For sse2neon (ps3emu/ps3_sse.h) on arm64 only: its inline
 * _MM_GET_ROUNDING_MODE and _MM_SET_ROUNDING_MODE use these. The values are
 * the rounding field of the arm64 FPCR, as macOS's header has them. Nothing
 * calls the two functions: they are not in PS3_GAME_HOST_CRT_FUNCS, so a
 * source that did would fail to link, not run with the wrong rounding.
 */
#ifndef PS3_GAME_LIBC_FENV_H
#define PS3_GAME_LIBC_FENV_H

#ifdef __cplusplus
extern "C" {
#endif

#define FE_TONEAREST  0x00000000
#define FE_UPWARD     0x00400000
#define FE_DOWNWARD   0x00800000
#define FE_TOWARDZERO 0x00C00000

int fegetround(void);
int fesetround(int round);

#ifdef __cplusplus
}
#endif

#endif /* PS3_GAME_LIBC_FENV_H */
