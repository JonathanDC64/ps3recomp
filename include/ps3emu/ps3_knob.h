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

/* PS3_NO_USER_INPUT=1, for test runs: the title ignores the user's pads,
 * keyboard and mouse, and its window never takes the focus (when it opens or
 * when it is clicked), so a script drives the run (PAD_FILE, PAD_STICK,
 * PAD_SCRIPT, a title's own hooks) while the user goes on working at the PC.
 * 0 or unset: off. */
static inline int ps3_no_user_input(void)
{
    static int on = 0;
    PS3_KNOB(on, ps3_env_int("PS3_NO_USER_INPUT", 0) != 0);
    return on;
}

/* RSX_DEV_HOTKEYS=1: the developer hot keys in the game's window -- F9 (a
 * frame capture for rsx_replay), F10 (a burst of screenshots) and F11 (a
 * profile session start or stop). 0 or unset, the default: a player's build
 * ignores those keys. A port turns it on for its developer builds and test
 * scripts. */
static inline int ps3_dev_hotkeys(void)
{
    static int on = 0;
    PS3_KNOB(on, ps3_env_int("RSX_DEV_HOTKEYS", 0) != 0);
    return on;
}
