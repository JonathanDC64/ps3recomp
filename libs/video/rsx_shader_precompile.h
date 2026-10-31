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
 * A vertex program is built twice: as it is, and in fetch form (reading its
 * vertex arrays itself), which the draws the GPU fetch path records use.
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
 *     cache), which covers whatever the rules above do not;
 *   - for a program queued with RSX_PRECOMPILE_2D, each of the above once more
 *     with the sprite-edge clamp that draws without a depth test use on a
 *     scaled target (RSX_SCALE above 100: HUD, menus, post passes). The clamp
 *     takes the scale from the draw's constants, so these serve every scale.
 *
 * Decompiling is not thread-safe with draws: run it before the title draws.
 * Windows only (the D3D12 backend's cache); elsewhere every call is a no-op.
 *
 * Built with the portable renderer (PS3RECOMP_RSX_HAL), the pass makes its
 * texts and fills its cache through a sink, one target a pass (the end of
 * this file), and runs off Windows too, for the spv target.
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

/* The same, with what the title knows of the program's draws:
 * RSX_PRECOMPILE_2D, it draws this fragment program without a depth test
 * (its HUD, menus, post passes), so its sprite-edge-clamp forms are built
 * too. Ignored for a vertex program. */
#define RSX_PRECOMPILE_2D 1u
void rsx_shader_precompile_add_flags(const char* name, const uint8_t* cg, size_t n, unsigned flags);

/* The pipeline index: the pipelines a title is known to ask for, as text,
 * one row a line (rsx_d3d12_backend.c describes a row; '#' lines are
 * skipped). After the queued programs, the decompile thread finds each
 * row's two programs among them by their fingerprints
 * (rsx_d3d12_program_hash64), makes their shader texts as a draw does,
 * has the compile threads put those in the cache, and hands the row to the
 * D3D12 backend, which builds the pipelines in the background once the
 * title runs. index_only: only that, the programs themselves are not built
 * (the cache is up to date). Before _start; returns the rows read. */
int rsx_shader_precompile_index(const char* rows, size_t len, int index_only);

/* 1 when the backend has built every row of this index into the cache's
 * pipeline library (pso_index.built in the cache holds its hash). */
int rsx_shader_precompile_index_built(const char* rows, size_t len);

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
    unsigned index_rows;     /* pipeline index rows read (_index) */
    unsigned index_found;    /* of those, with both programs among the queued ones */
} rsx_shader_precompile_stats;
void rsx_shader_precompile_get(rsx_shader_precompile_stats* st);

/* 0..1, for a progress bar: shaders done out of the shaders expected, the
 * total estimated from the programs decompiled so far. */
float rsx_shader_precompile_fraction(void);

/* Stop early: the shaders not yet compiled are compiled at draw time. */
void rsx_shader_precompile_cancel(void);
void rsx_shader_precompile_wait(void);

#if defined(PS3RECOMP_RSX_HAL)
/* ---- Targets (built with the portable renderer) ----
 * A pass makes its texts and fills its cache through a sink: the calls it
 * makes, as a table. One target a pass, each with its own files:
 *   d3d12  today's renderer's calls (rsx_d3d12_*), the default: the pass as
 *          it was, today's .dxbc files; its pipeline index rows go through
 *          the route switch (rsx_out.h) to the renderer that runs. Windows.
 *   dxbc   the new path's texts (rsxcore/rsxc_shader.c, copies of today's)
 *          into the compile service's dxbc target (gpu/gpu_shader.h): FXC
 *          with today's call, so today's .dxbc names and bytes. Windows.
 *   spv    the same texts into its spv target, glslang, under
 *          <cache>/gpu/spv/. Where PS3RECOMP_RSX_HAL_SPV is built.
 * No renderer takes the rows of dxbc or spv yet (their pipeline stores come
 * later): their pso_index_add/ready are NULL, so a pass hands no rows over,
 * _index_built is 0 and an index_only pass has nothing to do (_index
 * returns 0); a full pass still makes the rows' texts. */
typedef struct rsx_shader_precompile_sink {
    const char* name;       /* "d3d12", "dxbc", "spv" */
    const char* compiler;   /* for the failure line: "FXC", "glslang" */
    /* What the target's files depend on besides the programs (its compiler,
     * flags and text edits), for a stamp; NULL for d3d12, whose stamp is
     * today's. */
    const char* (*tag)(char* buf, size_t cap);
    /* Today's calls, with rsx_d3d12_backend.h's meanings. */
    int         (*shader_hlsl)(int kind, const uint8_t* ucode, uint32_t ucode_len, uint32_t vp_start_slot,
                               int exp32, uint32_t cube_mask, uint32_t d2c_mask, char* out, uint32_t cap);
    int         (*shader_hdr)(char* hlsl, uint32_t cap, uint32_t hdrv);
    uint64_t    (*shader_key)(int fragment, const char* hlsl);
    int         (*shader_cache_put)(int fragment, const char* hlsl);   /* 1 there, 2 compiled, 0 failed */
    const char* (*shader_cache_path)(void);
    int         (*shader_fp_variants)(const uint8_t* ucode, uint32_t ucode_len, uint32_t* out, int max);
    uint64_t    (*program_hash64)(int fragment, const uint8_t* ucode, uint32_t ucode_len);
    void        (*pso_index_add)(int vs_fetch, uint64_t vs_key, int clamp, uint64_t ps_key, uint32_t blend,
                                 int nrt, uint32_t rtfmt, uint32_t cmask, uint32_t cull, uint32_t depth);
    void        (*pso_index_ready)(uint64_t hash);
} rsx_shader_precompile_sink;

#define RSX_PRECOMPILE_D3D12   0
#define RSX_PRECOMPILE_DXBC    1
#define RSX_PRECOMPILE_SPV     2
#define RSX_PRECOMPILE_TARGETS 3

/* A target's sink; NULL when this build does not have it. */
const rsx_shader_precompile_sink* rsx_shader_precompile_sink_of(int target);
/* The sink the next pass uses (the first target this build has until one is
 * set; NULL if it has none, and _start fails). _use takes any sink that
 * lives as long as the pass, e.g. a test's copy of one: 0 while a pass has
 * started (_reset first) or for NULL. */
const rsx_shader_precompile_sink* rsx_shader_precompile_current(void);
int rsx_shader_precompile_use(const rsx_shader_precompile_sink* sink);
/* Waits for a pass, lets go of everything it held (programs, texts, index,
 * kinds, counts), and readies the next: _kinds, _add, _index and _start
 * again. The sink stays. */
void rsx_shader_precompile_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
