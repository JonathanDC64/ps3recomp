/*
 * Shader precompile: fills the D3D12 backend's shader disk cache from a
 * title's Cg binaries, so no draw waits on the shader compiler. Used by the
 * rsx_shader_precompile tool, and by a title at startup (with the window
 * open and a progress bar over it, before the guest runs).
 *
 * Each program is decompiled by the same code a draw uses
 * (rsx_d3d12_shader_hlsl) and compiled into the cache
 * (rsx_d3d12_shader_cache_put). The cache is keyed by the HLSL text: a
 * program the title modifies before binding, or binds with other state than
 * assumed here, simply misses and is compiled at draw time as before.
 *
 * A fragment program's text also depends on what kind of texture each unit
 * it samples is bound to (cube, shadow = depth compare, d2c = depth read as
 * colour), which is draw state, not part of the binary. Each program is
 * built:
 *   - with every unit plain, and 32-bit exports unless the binary says the
 *     colour comes from H0;
 *   - with the title's conventions (units: "7=shadow,8=cube": that kind in
 *     every program that samples the unit);
 *   - once more per alternative unit it samples, on top of the conventions
 *     (alts: "1=d2c", a unit only some programs bind a depth surface to);
 *   - with every variant earlier runs recorded for it (fp_variants.txt in the
 *     cache), which covers whatever the rules above do not.
 *
 * Decompiling is not thread-safe with draws: run it before the title draws.
 * Windows only (the D3D12 backend's cache); elsewhere every call is a no-op.
 */
#ifndef RSX_SHADER_PRECOMPILE_H
#define RSX_SHADER_PRECOMPILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The texture-kind conventions and alternatives (either may be NULL or "").
 * Returns 0 on a malformed list (want U=cube|shadow|d2c,...). */
int rsx_shader_precompile_kinds(const char* units, const char* alts);

/* Queue one Cg binary (.vpo/.fpo as the SDK's cgc writes it: big-endian,
 * profile sce_vp_rsx 0x1B5B or sce_fp_rsx 0x1B5C at offset 0); the bytes are
 * copied. Anything else is counted as skipped. Before _start only. */
void rsx_shader_precompile_add(const char* name, const uint8_t* cg, size_t n);

/* Decompile the queued programs on one thread and compile their shaders on
 * `threads` more (<= 0: one per logical processor, at most 64). Returns 0 if
 * it started; -1 if the cache is off (PS3_SHADER_CACHE=0) or nothing is
 * queued. Runs in the background; _get reports, _wait waits. */
int rsx_shader_precompile_start(int threads);

typedef struct rsx_shader_precompile_stats {
    unsigned programs;       /* Cg binaries queued */
    unsigned programs_done;  /* decompiled so far */
    unsigned skipped;        /* not Cg binaries */
    unsigned bad;            /* not decompiled */
    unsigned shaders;        /* distinct HLSL texts so far */
    unsigned dups;           /* texts dropped as already queued */
    unsigned shaders_done;   /* compiled, cached or failed so far */
    unsigned built, cached, failed;
    int      running;        /* 1 until every thread has finished */
    int      cancelled;
    int      threads;
    double   seconds;        /* since _start (until the end, once finished) */
} rsx_shader_precompile_stats;
void rsx_shader_precompile_get(rsx_shader_precompile_stats* st);

/* 0..1, for a progress bar: shaders done out of the shaders expected, the
 * total estimated from the programs decompiled so far. */
float rsx_shader_precompile_fraction(void);

/* Stop early: the shaders not yet compiled are compiled at draw time. */
void rsx_shader_precompile_cancel(void);
void rsx_shader_precompile_wait(void);

#ifdef __cplusplus
}
#endif

#endif
