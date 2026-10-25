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

double sqrt(double x);
float  sqrtf(float x);
double fabs(double x);
float  fabsf(float x);
double trunc(double x);
float  truncf(float x);
double floor(double x);
float  floorf(float x);
double ceil(double x);
float  ceilf(float x);
double nearbyint(double x);
float  nearbyintf(float x);
double round(double x);
float  roundf(float x);
double copysign(double x, double y);
float  copysignf(float x, float y);

double fma(double x, double y, double z);
float  fmaf(float x, float y, float z);   /* spu_helpers.h, only with __FMA__: an instruction */
double ldexp(double x, int e);
double exp2(double x);

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
