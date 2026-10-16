#pragma once
#include <stdlib.h>
/* Live diagnostic knobs.
 *
 * Most runtime knobs are environment variables read once into a static, so a
 * change meant a restart. The debug console's setenv bumps ps3_env_gen, and a
 * knob written as
 *
 *     static int cull_off = -1;
 *     PS3_KNOB(cull_off, getenv("CULL_OFF") ? 1 : 0);
 *
 * re-evaluates its expression on the next use after any setenv: one load and
 * compare otherwise, cheap enough for per-draw paths. The value is computed
 * first and stored in one write, so another thread never sees it half reset.
 * getenv itself is served from boot_main's cache, which setenv updates. */
#ifdef __cplusplus
extern "C" {
#endif

/* Defined here, once per program (weak / selectany), so a test that compiles
 * a few library sources on its own links without runtime/diag/ps3_knob.c. */
#if defined(_MSC_VER)
#define PS3_KNOB_SHARED __declspec(selectany)
#else
#define PS3_KNOB_SHARED __attribute__((weak))
#endif
PS3_KNOB_SHARED volatile unsigned ps3_env_gen = 1;   /* setenv increments it */
void ps3_env_bump(void);
/* Set a variable for the getenv cache as well as the process (boot_main's
 * cached_setenv, which the console's setenv uses). _putenv_s alone is not
 * seen by a name the cache has already looked up. */
void ps3_setenv(const char* name, const char* value);

/* atoi(getenv(name)), def if unset */
static inline int ps3_env_int(const char* name, int def)
{
    const char* e = getenv(name);
    return e ? atoi(e) : def;
}

#ifdef __cplusplus
}
#endif

#define PS3_KNOB(var, expr)                                              \
    do {                                                                 \
        static unsigned var##_kgen_;                                     \
        const unsigned kg_now_ = ps3_env_gen;                            \
        if (var##_kgen_ != kg_now_) { (var) = (expr); var##_kgen_ = kg_now_; } \
    } while (0)
