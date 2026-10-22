/*
 * ps3recomp - <math.h> for a freestanding game module (PS3_GAME_FREESTANDING,
 * ps3emu/game_freestanding.h).
 *
 * Declared with their standard prototypes, so the compiler knows them as the
 * library functions:
 *   - the rounding functions, fabs and copysign are instructions (SSE4.1:
 *     the split's game sources build with it) and are never called;
 *   - sqrt and sqrtf are an instruction, plus a call when the result is a NaN
 *     (to set errno, as with the UCRT's header). That call, fma, ldexp and
 *     exp2 are jump thunks to the host's C runtime (PS3_GAME_HOST_CRT_FUNCS),
 *     so their results are the same bits as in a single exe.
 * signbit is the compiler's: the UCRT's calls _fdsign / _dsign for it.
 */
#ifndef PS3_GAME_LIBC_MATH_H
#define PS3_GAME_LIBC_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

double __cdecl sqrt(double x);
float  __cdecl sqrtf(float x);
double __cdecl fabs(double x);
float  __cdecl fabsf(float x);
double __cdecl trunc(double x);
float  __cdecl truncf(float x);
double __cdecl floor(double x);
float  __cdecl floorf(float x);
double __cdecl ceil(double x);
float  __cdecl ceilf(float x);
double __cdecl nearbyint(double x);
float  __cdecl nearbyintf(float x);
double __cdecl round(double x);
float  __cdecl roundf(float x);
double __cdecl copysign(double x, double y);
float  __cdecl copysignf(float x, float y);

double __cdecl fma(double x, double y, double z);
float  __cdecl fmaf(float x, float y, float z);   /* spu_helpers.h, only with __FMA__: an instruction */
double __cdecl ldexp(double x, int e);
double __cdecl exp2(double x);

#ifdef __cplusplus
}
#endif

#define signbit(x) (sizeof(x) == sizeof(float) ? __builtin_signbitf((float)(x)) \
                                               : __builtin_signbit((double)(x)))
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)

#define INFINITY  __builtin_inff()
#define NAN       __builtin_nanf("")
#define HUGE_VAL  __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()

#endif /* PS3_GAME_LIBC_MATH_H */
