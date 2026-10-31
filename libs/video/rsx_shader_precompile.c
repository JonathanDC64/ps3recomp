/*
 * rsx_shader_precompile.c -- see rsx_shader_precompile.h.
 *
 * One thread decompiles the queued programs in order (the decompiler is not
 * thread-safe) and hands each distinct HLSL text to a queue; the compile
 * threads take texts off it and put them in the cache. A text already cached
 * costs a hash and a file check, so a run over a full cache takes seconds.
 *
 * The threads, locks and counters are rsx_pc_sync.h's: on Windows the Win32
 * calls themselves. Built with the portable renderer (PS3RECOMP_RSX_HAL) the
 * pass calls through a sink (rsx_shader_precompile.h's targets), and builds
 * off Windows too, for the spv target; without it, it is the pass it was.
 */
#if defined(PS3RECOMP_RSX_HAL) && !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* strdup, clock_gettime, pthreads, under -std=c99 too */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE          /* and macOS's _SC_NPROCESSORS_ONLN beside it */
#endif
#endif
#include "rsx_shader_precompile.h"

#if defined(_WIN32) || defined(PS3RECOMP_RSX_HAL)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define pc_strdup _strdup
#else
#include <time.h>
#include <unistd.h>
#define pc_strdup strdup
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#endif
#include "rsx_pc_sync.h"

int rsx_d3d12_shader_hlsl(int fragment, const uint8_t* ucode, uint32_t ucode_len,
                          uint32_t vp_start_slot, int exp32, uint32_t cube_mask,
                          uint32_t d2c_mask, char* out, uint32_t cap);
int rsx_d3d12_shader_hdr(char* hlsl, uint32_t cap, uint32_t hdrv);
int rsx_d3d12_shader_cache_put(int fragment, const char* hlsl);
const char* rsx_d3d12_shader_cache_path(void);
int rsx_d3d12_shader_fp_variants(const uint8_t* ucode, uint32_t ucode_len, uint32_t* out, int max);
uint32_t rsx_fp_texture_units(const uint8_t* ucode, uint32_t max_bytes);
uint64_t rsx_d3d12_program_hash64(int fragment, const uint8_t* ucode, uint32_t ucode_len);
uint64_t rsx_d3d12_shader_key(int fragment, const char* hlsl);
void rsx_d3d12_pso_index_add(int vs_fetch, uint64_t vs_key, int clamp, uint64_t ps_key,
                             uint32_t blend, int nrt, uint32_t rtfmt, uint32_t cmask, uint32_t cull,
                             uint32_t depth);
void rsx_d3d12_pso_index_ready(uint64_t hash);
#if defined(PS3RECOMP_RSX_HAL)
/* ---- The sinks (rsx_shader_precompile.h's targets) ----
 * d3d12 is the calls above, as the pass made them before the sinks, but for
 * the pipeline index's rows, which go to the renderer the route switch
 * picked (rsx_out.h). dxbc and spv take the new path's texts, the copies of
 * those calls in rsxcore/rsxc_shader.c, into the compile service. Below,
 * each of the calls above is renamed to the sink's, so the pass reads as it
 * did; a sink without rows to hand over leaves those two calls out. */
#include "rsx_out.h"
#include "gpu/gpu_shader.h"
#include "rsxcore/rsxc_shader.h"
#if defined(PS3RECOMP_RSX_HAL_SPV)
#include "gpu/gpu_shader_spv.h"   /* gpu_spv_tag */
#endif

#ifdef _WIN32
static const rsx_shader_precompile_sink s_sink_d3d12 = {
    "d3d12", "FXC", NULL,
    rsx_d3d12_shader_hlsl, rsx_d3d12_shader_hdr, rsx_d3d12_shader_key, rsx_d3d12_shader_cache_put,
    rsx_d3d12_shader_cache_path, rsx_d3d12_shader_fp_variants, rsx_d3d12_program_hash64,
    rsx_out_pso_index_add, rsx_out_pso_index_ready
};

static int dxbc_put(int fragment, const char* hlsl)
{
    return gpu_sc_put(GPU_SC_DXBC, fragment ? GPU_SC_PS : GPU_SC_VS, hlsl);
}
static const char* dxbc_tag(char* buf, size_t cap)
{
    snprintf(buf, cap, "dxbc;FXC vs_5_0 ps_5_0");
    return buf;
}
static const rsx_shader_precompile_sink s_sink_dxbc = {
    "dxbc", "FXC", dxbc_tag,
    rsxc_shader_hlsl, rsxc_shader_hdr, rsxc_shader_key, dxbc_put,
    gpu_sc_cache_dir, rsxc_shader_fp_variants, rsxc_program_hash64,
    NULL, NULL
};
#endif

#if defined(PS3RECOMP_RSX_HAL_SPV)
/* A text that glslang (or a text edit before it) refuses is compiled once
 * more without the cache, for the reason: one line, before the pass's
 * failure line. */
static int spv_put(int fragment, const char* hlsl)
{
    const gpu_sc_stage stage = fragment ? GPU_SC_PS : GPU_SC_VS;
    const int r = gpu_sc_put(GPU_SC_SPV, stage, hlsl);
    if (!r && hlsl) {
        char err[512];
        gpu_sc_blob b;
        if (gpu_sc_compile(GPU_SC_SPV, stage, hlsl, strlen(hlsl), fragment ? "guest_fp" : "guest_vp2",
                           GPU_SC_NO_CACHE, &b, err, sizeof err) == GPU_SC_OK) {
            gpu_sc_blob_release(&b);
            snprintf(err, sizeof err, "compiles, but could not be stored");
        }
        char* nl = strchr(err, '\n');
        if (nl) *nl = 0;
        fprintf(stderr, "[precompile] spv: %s\n", err[0] ? err : "no reason given");
    }
    return r;
}
static const char* spv_tag(char* buf, size_t cap) { return gpu_spv_tag(NULL, buf, cap); }
static const rsx_shader_precompile_sink s_sink_spv = {
    "spv", "glslang", spv_tag,
    rsxc_shader_hlsl, rsxc_shader_hdr, rsxc_shader_key, spv_put,
    gpu_sc_cache_dir, rsxc_shader_fp_variants, rsxc_program_hash64,
    NULL, NULL
};
#endif

#if defined(_WIN32)
static const rsx_shader_precompile_sink* s_sink = &s_sink_d3d12;
#elif defined(PS3RECOMP_RSX_HAL_SPV)
static const rsx_shader_precompile_sink* s_sink = &s_sink_spv;
#else
static const rsx_shader_precompile_sink* s_sink = NULL;   /* no target here: _start fails */
#endif

#define rsx_d3d12_shader_hlsl        (s_sink->shader_hlsl)
#define rsx_d3d12_shader_hdr         (s_sink->shader_hdr)
#define rsx_d3d12_shader_key         (s_sink->shader_key)
#define rsx_d3d12_shader_cache_put   (s_sink->shader_cache_put)
#define rsx_d3d12_shader_cache_path  (s_sink->shader_cache_path)
#define rsx_d3d12_shader_fp_variants (s_sink->shader_fp_variants)
#define rsx_d3d12_program_hash64     (s_sink->program_hash64)
#define rsx_d3d12_pso_index_add(...) (s_sink->pso_index_add ? s_sink->pso_index_add(__VA_ARGS__) : (void)0)
#define rsx_d3d12_pso_index_ready(h) (s_sink->pso_index_ready ? s_sink->pso_index_ready(h) : (void)0)
#endif

#define CG_PROFILE_SCE_VP_RSX 0x1B5Bu
#define CG_PROFILE_SCE_FP_RSX 0x1B5Cu
#define HLSL_CAP              (256u * 1024u)
#define MAX_VARIANTS          64
#define MAX_THREADS           64

/* A texture-kind rule set as the decompiler takes it: cube bits 0-15,
 * shadow bits 16-31 of "cube", depth-as-colour in "d2c". */
typedef struct { uint32_t cube, d2c; } Kinds;
static Kinds s_conv;
static Kinds s_alt[16];
static int   s_nalt;

typedef struct { char* name; uint8_t* data; size_t n; unsigned flags; } Program;
static Program* s_progs;
static unsigned s_nprogs, s_progcap;

typedef struct { int fragment; char* hlsl; const char* name; } Job;
static Job*     s_jobs;            /* grows; guarded by s_lock */
static unsigned s_njobs, s_jobcap, s_next;
static int      s_produced;        /* the decompile thread is done */
static rsx_pc_lock s_lock = RSX_PC_LOCK_INIT;
static rsx_pc_cond s_cv   = RSX_PC_COND_INIT;

/* HLSL texts already queued (FNV-1a 64), open addressing; decompile thread only. */
static uint64_t* s_texts;
static unsigned  s_textcap, s_ntexts;

static volatile rsx_pc_counter s_prog_done, s_skipped, s_bad, s_dups;
/* The pipeline index (rsx_shader_precompile_index). */
typedef struct {
    uint64_t vp, fp;
    uint32_t vstart, vfetch, exp32, cube, d2c, clamp, blend, nrt, rtfmt, cmask, cull, depth;
    uint32_t hdrv;   /* the 15th field, 0 when the row has none */
} IndexRow;
static IndexRow* s_rows;
static unsigned  s_nrows, s_rows_found;
static uint64_t  s_rows_hash;
static int       s_index_only;
static volatile rsx_pc_counter s_done, s_built, s_cached, s_failed, s_cancel, s_live;
static int    s_threads;
static rsx_pc_thread s_handles[MAX_THREADS + 1];
static int    s_nhandles;
#if defined(PS3RECOMP_RSX_HAL)
static double s_t0, s_t1;   /* pc_seconds() */

/* A monotonic clock in seconds, and the logical processors. */
static double pc_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}
static int pc_cpus(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}
#else
static LARGE_INTEGER s_t0, s_t1;
#endif

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int parse_kinds(const char* s, Kinds* conv, Kinds* alts, int* nalt)
{
    while (s && *s) {
        char* end;
        const long u = strtol(s, &end, 10);
        if (end == s || *end != '=' || u < 0 || u > 15) return 0;
        s = end + 1;
        Kinds k = { 0, 0 };
        if (!strncmp(s, "cube", 4))        { k.cube = 1u << u;         s += 4; }
        else if (!strncmp(s, "shadow", 6)) { k.cube = 1u << (16 + u);  s += 6; }
        else if (!strncmp(s, "d2c", 3))    { k.d2c = 1u << u;          s += 3; }
        else return 0;
        if (conv) { conv->cube |= k.cube; conv->d2c |= k.d2c; }
        else if (*nalt < 16) alts[(*nalt)++] = k;
        if (*s == ',') s++;
        else if (*s) return 0;
    }
    return 1;
}

int rsx_shader_precompile_kinds(const char* units, const char* alts)
{
    Kinds conv = { 0, 0 }, alt[16];
    int nalt = 0;
    if (!parse_kinds(units, &conv, NULL, NULL) || !parse_kinds(alts, NULL, alt, &nalt)) return 0;
    s_conv = conv;
    memcpy(s_alt, alt, sizeof alt);
    s_nalt = nalt;
    return 1;
}

static int text_seen(const char* t)
{
    uint64_t h = 14695981039346656037ull;
    for (const char* p = t; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    if (!h) h = 1;   /* 0 marks an empty slot */
    if ((s_ntexts + 1) * 2 > s_textcap) {
        const unsigned cap = s_textcap ? s_textcap * 2 : 4096;
        uint64_t* t2 = (uint64_t*)calloc(cap, sizeof(uint64_t));
        if (!t2) return 0;   /* out of memory: compile it again rather than lose it */
        for (unsigned i = 0; i < s_textcap; i++)
            if (s_texts[i]) {
                unsigned j = (unsigned)s_texts[i] & (cap - 1);
                while (t2[j]) j = (j + 1) & (cap - 1);
                t2[j] = s_texts[i];
            }
        free(s_texts);
        s_texts = t2;
        s_textcap = cap;
    }
    unsigned j = (unsigned)h & (s_textcap - 1);
    while (s_texts[j]) {
        if (s_texts[j] == h) return 1;
        j = (j + 1) & (s_textcap - 1);
    }
    s_texts[j] = h;
    s_ntexts++;
    return 0;
}

/* A text for the compile threads, which take it over; dropped if the same
 * text was queued before. fragment: 1 for a pixel shader. */
static void push_job(const char* name, int fragment, char* hlsl)
{
    if (text_seen(hlsl)) { rsx_pc_inc(&s_dups); free(hlsl); return; }
    char* fit = (char*)realloc(hlsl, strlen(hlsl) + 1);
    if (fit) hlsl = fit;
    rsx_pc_lock_x(&s_lock);
    if (s_njobs == s_jobcap) {
        const unsigned cap = s_jobcap ? s_jobcap * 2 : 1024;
        Job* j2 = (Job*)realloc(s_jobs, sizeof(Job) * cap);
        if (!j2) { rsx_pc_unlock_x(&s_lock); free(hlsl); return; }
        s_jobs = j2;
        s_jobcap = cap;
    }
    s_jobs[s_njobs].fragment = fragment;
    s_jobs[s_njobs].hlsl = hlsl;
    s_jobs[s_njobs].name = name;
    s_njobs++;
    rsx_pc_unlock_x(&s_lock);
    rsx_pc_wake_one(&s_cv);
}

static void queue(const char* name, int fragment, const uint8_t* uc, uint32_t usz, uint32_t slot,
                  uint32_t exp32, uint32_t cube, uint32_t d2c)
{
    char* hlsl = (char*)malloc(HLSL_CAP);
    if (!hlsl) return;
    if (rsx_d3d12_shader_hlsl(fragment, uc, usz, slot, (int)exp32, cube, d2c, hlsl, HLSL_CAP) <= 0) {
        fprintf(stderr, "[precompile] cannot decompile %s\n", name);
        rsx_pc_inc(&s_bad); free(hlsl); return;
    }
    push_job(name, fragment == 1 || fragment == 3, hlsl);
}

/* One fragment-program variant, and its sprite-edge-clamp form for a program
 * drawn in 2D (RSX_PRECOMPILE_2D). */
static void queue_fp(const Program* p, const uint8_t* uc, uint32_t usz, uint32_t exp32, uint32_t cube, uint32_t d2c)
{
    queue(p->name, 1, uc, usz, 0, exp32, cube, d2c);
    if (p->flags & RSX_PRECOMPILE_2D) queue(p->name, 3, uc, usz, 0, exp32, cube, d2c);
}

/* One Cg binary: queue the HLSL texts a draw would compile for it. */
static void add_program(const Program* p)
{
    const uint8_t* b = p->data;
    const size_t n = p->n;
    if (n < 32) { rsx_pc_inc(&s_skipped); return; }
    const uint32_t profile = be32(b), total = be32(b + 8);
    const uint32_t prog = be32(b + 20), usz = be32(b + 24), uoff = be32(b + 28);
    const int fragment = profile == CG_PROFILE_SCE_FP_RSX;
    if ((profile != CG_PROFILE_SCE_VP_RSX && !fragment) || total > n ||
        uoff > total || usz > total - uoff || (uint64_t)prog + 24 > total) {
        rsx_pc_inc(&s_skipped); return;
    }
    const uint8_t* uc = b + uoff;
    if (!fragment) {   /* CgBinaryVertexProgram.instructionSlot: where it is loaded */
        /* Both forms: most draws fetch their own vertices (the GPU fetch
         * path), and a vertex shader compiled at draw time stalls the RSX
         * thread, and the game waiting on it, for as long as FXC takes. */
        queue(p->name, 0, uc, usz, be32(b + prog + 4), 0, 0, 0);
        queue(p->name, 2, uc, usz, be32(b + prog + 4), 0, 0, 0);
        return;
    }
    const uint32_t exp32 = b[prog + 19] ? 0 : 1;   /* CgBinaryFragmentProgram.outputFromH0 */
    const uint32_t used = rsx_fp_texture_units(uc, usz);
    queue_fp(p, uc, usz, exp32, 0, 0);
    const uint32_t cc = s_conv.cube & (used | used << 16), cd = s_conv.d2c & used;
    queue_fp(p, uc, usz, exp32, cc, cd);
    for (int i = 0; i < s_nalt; i++)
        if ((s_alt[i].cube & (used | used << 16)) || (s_alt[i].d2c & used)) {
            /* The alternative replaces whatever the convention made that unit. */
            const uint32_t u = (s_alt[i].cube | s_alt[i].cube >> 16 | s_alt[i].d2c) & 0xFFFFu;
            queue_fp(p, uc, usz, exp32, (cc & ~(u | u << 16)) | s_alt[i].cube, (cd & ~u) | s_alt[i].d2c);
        }
    uint32_t v[MAX_VARIANTS * 3];
    const int nv = rsx_d3d12_shader_fp_variants(uc, usz, v, MAX_VARIANTS);
    for (int i = 0; i < nv; i++) queue_fp(p, uc, usz, v[i * 3], v[i * 3 + 1], v[i * 3 + 2]);
}

void rsx_shader_precompile_add(const char* name, const uint8_t* cg, size_t n)
{
    rsx_shader_precompile_add_flags(name, cg, n, 0);
}

void rsx_shader_precompile_add_flags(const char* name, const uint8_t* cg, size_t n, unsigned flags)
{
    if (s_nhandles) return;   /* started */
    if (s_nprogs == s_progcap) {
        const unsigned cap = s_progcap ? s_progcap * 2 : 512;
        Program* p2 = (Program*)realloc(s_progs, sizeof(Program) * cap);
        if (!p2) return;
        s_progs = p2;
        s_progcap = cap;
    }
    Program* p = &s_progs[s_nprogs];
    p->data = (uint8_t*)malloc(n ? n : 1);
    p->name = pc_strdup(name ? name : "?");
    if (!p->data || !p->name) { free(p->data); free(p->name); return; }
    memcpy(p->data, cg, n);
    p->n = n;
    p->flags = flags;
    s_nprogs++;
}

/* FNV-1a 64. */
static uint64_t text_hash(const char* t, size_t n)
{
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)t[i]; h *= 1099511628211ull; }
    return h;
}

/* A Cg binary's ucode, as add_program reads it; 0 if it is not one. */
static int cg_ucode(const Program* p, const uint8_t** uc, uint32_t* usz, int* fragment)
{
    const uint8_t* b = p->data;
    if (!b || p->n < 32) return 0;
    const uint32_t profile = be32(b), total = be32(b + 8);
    const uint32_t prog = be32(b + 20), sz = be32(b + 24), uoff = be32(b + 28);
    *fragment = profile == CG_PROFILE_SCE_FP_RSX;
    if ((profile != CG_PROFILE_SCE_VP_RSX && !*fragment) || total > p->n ||
        uoff > total || sz > total - uoff || (uint64_t)prog + 24 > total)
        return 0;
    *uc = b + uoff;
    *usz = sz;
    return 1;
}

typedef struct { uint64_t h; unsigned prog; int fragment; } ProgPrint;
static int print_cmp(const void* a, const void* b)
{
    const ProgPrint* x = (const ProgPrint*)a;
    const ProgPrint* y = (const ProgPrint*)b;
    if (x->h != y->h) return x->h < y->h ? -1 : 1;
    return x->fragment - y->fragment;
}
static const Program* print_find(const ProgPrint* t, unsigned n, uint64_t h, int fragment)
{
    const ProgPrint k = { h, 0, fragment };
    const ProgPrint* f = (const ProgPrint*)bsearch(&k, t, n, sizeof *t, print_cmp);
    return f ? &s_progs[f->prog] : NULL;
}

/* The cache keys of the texts index_resolve has made, by program and form
 * (open addressing; 0 marks an empty slot, a key of 0 a text that could not
 * be made): rows share programs, so each text is made once. */
typedef struct { uint64_t form, key; } FormKey;
static FormKey* s_fk;
static unsigned s_fk_n, s_fk_cap;
static FormKey* form_slot(uint64_t form)
{
    if ((s_fk_n + 1) * 2 > s_fk_cap) {
        const unsigned cap = s_fk_cap ? s_fk_cap * 2 : 1024;
        FormKey* t = (FormKey*)calloc(cap, sizeof *t);
        if (!t) return NULL;
        for (unsigned i = 0; i < s_fk_cap; i++)
            if (s_fk[i].form) {
                unsigned j = (unsigned)s_fk[i].form & (cap - 1);
                while (t[j].form) j = (j + 1) & (cap - 1);
                t[j] = s_fk[i];
            }
        free(s_fk);
        s_fk = t; s_fk_cap = cap;
    }
    unsigned j = (unsigned)form & (s_fk_cap - 1);
    while (s_fk[j].form && s_fk[j].form != form) j = (j + 1) & (s_fk_cap - 1);
    return &s_fk[j];
}

/* The cache key of one program's text in one form (rsx_d3d12_shader_hlsl's
 * kind and kinds, and an HDR variant: rsx_d3d12_shader_hdr), made and queued
 * for the compile threads the first time; 0 if it cannot be made. */
static uint64_t form_key(const Program* p, int kind, uint32_t exp32, uint32_t cube, uint32_t d2c, uint32_t hdrv,
                         char* buf)
{
    const uint32_t f[6] = { (uint32_t)(p - s_progs), (uint32_t)kind, exp32, cube, d2c, hdrv };
    uint64_t form = text_hash((const char*)f, sizeof f);
    if (!form) form = 1;
    FormKey* slot = form_slot(form);
    if (slot && slot->form) return slot->key;
    const uint8_t* uc; uint32_t usz; int fragment;
    uint64_t key = 0;
    if (cg_ucode(p, &uc, &usz, &fragment) &&
        rsx_d3d12_shader_hlsl(kind, uc, usz, 0, (int)exp32, cube, d2c, buf, HLSL_CAP) > 0) {
        if (hdrv) rsx_d3d12_shader_hdr(buf, HLSL_CAP, hdrv);
        key = rsx_d3d12_shader_key(kind == 1 || kind == 3, buf);
        char* c = pc_strdup(buf);
        if (c) push_job(p->name, kind == 1 || kind == 3, c);
    }
    if (slot) { slot->form = form; slot->key = key; s_fk_n++; }
    return key;
}

/* The decompile thread, after the programs: each row's programs by their
 * fingerprints, and their texts as a draw makes them (vp_get_vs_mode,
 * vp_get_fp_pso): the vertex program at branch base 0, as the draw
 * decompiles it; the fragment program with the row's kinds, in the clamp
 * form if the row has it, and in its HDR variant if it has one. The texts go
 * to the compile threads, so the cache has them, and the row to the backend.
 * A pipeline is found by its description (the library), so a variant needs
 * nothing from the backend beyond its text. */
static void index_resolve(void)
{
    ProgPrint* t = (ProgPrint*)malloc(sizeof(ProgPrint) * (s_nprogs ? s_nprogs : 1));
    char* buf = (char*)malloc(HLSL_CAP);
    if (!t || !buf) { free(t); free(buf); return; }
    unsigned nt = 0;
    for (unsigned i = 0; i < s_nprogs; i++) {
        const uint8_t* uc; uint32_t usz; int fragment;
        if (!cg_ucode(&s_progs[i], &uc, &usz, &fragment)) continue;
        t[nt].h = rsx_d3d12_program_hash64(fragment, uc, usz);
        t[nt].prog = i;
        t[nt].fragment = fragment;
        nt++;
    }
    qsort(t, nt, sizeof *t, print_cmp);
    for (unsigned r = 0; r < s_nrows && !s_cancel; r++) {
        const IndexRow* w = &s_rows[r];
        const Program* vp = print_find(t, nt, w->vp, 0);
        const Program* fp = print_find(t, nt, w->fp, 1);
        if (!vp || !fp) continue;
        const uint64_t vk = form_key(vp, w->vfetch ? 2 : 0, 0, 0, 0, 0, buf);
        const uint64_t pk = vk ? form_key(fp, w->clamp ? 3 : 1, w->exp32, w->cube, w->d2c, w->hdrv, buf) : 0;
        if (!pk) continue;
        rsx_d3d12_pso_index_add((int)w->vfetch, vk, (int)w->clamp, pk, w->blend, (int)w->nrt, w->rtfmt,
                                w->cmask, w->cull, w->depth);
        s_rows_found++;
    }
    free(t); free(buf);
    free(s_fk); s_fk = NULL; s_fk_n = s_fk_cap = 0;
}

int rsx_shader_precompile_index(const char* rows, size_t len, int index_only)
{
    if (s_nhandles) return 0;   /* started */
    free(s_rows);
    s_rows = NULL; s_nrows = 0;
#if defined(PS3RECOMP_RSX_HAL)
    /* Only the rows, for a target no renderer takes them from: nothing to do
     * (a full pass still makes their texts). */
    if (index_only && !(s_sink && s_sink->pso_index_add)) { s_rows_hash = 0; s_index_only = 0; return 0; }
#endif
    unsigned cap = 0;
    for (size_t i = 0; rows && i < len; ) {
        size_t e = i;
        while (e < len && rows[e] != '\n') e++;
        if (e > i && rows[i] != '#') {
            char line[256];
            const size_t n = e - i < sizeof line - 1 ? e - i : sizeof line - 1;
            memcpy(line, rows + i, n);
            line[n] = 0;
            IndexRow w;
            unsigned long long vp, fp;
            memset(&w, 0, sizeof w);
            if (sscanf(line, "%llx %x %x %llx %x %x %x %x %x %x %x %x %x %x %x", &vp, &w.vstart, &w.vfetch, &fp,
                       &w.exp32, &w.cube, &w.d2c, &w.clamp, &w.blend, &w.nrt, &w.rtfmt, &w.cmask, &w.cull,
                       &w.depth, &w.hdrv) >= 14) {   /* hdrv: an optional 15th field */
                w.vp = vp; w.fp = fp;
                if (s_nrows == cap) {
                    const unsigned c = cap ? cap * 2 : 1024;
                    IndexRow* g = (IndexRow*)realloc(s_rows, sizeof *g * c);
                    if (!g) break;
                    s_rows = g; cap = c;
                }
                s_rows[s_nrows++] = w;
            }
        }
        i = e + 1;
    }
    s_rows_hash = s_nrows ? text_hash(rows, len) : 0;
    s_index_only = s_nrows && index_only;
    return (int)s_nrows;
}

int rsx_shader_precompile_index_built(const char* rows, size_t len)
{
#if defined(PS3RECOMP_RSX_HAL)
    /* pso_index.built is written by the renderer that builds the rows; a
     * target with none has built nothing. */
    if (!s_sink || !s_sink->pso_index_add) return 0;
#endif
    const char* dir = rsx_d3d12_shader_cache_path();
    if (!rows || !dir || !dir[0]) return 0;
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s/pso_index.built", dir);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned long long h = 0;
    const int ok = fscanf(f, "%llx", &h) == 1;
    fclose(f);
    return ok && h == text_hash(rows, len);
}

/* The last thread out tells the backend the index is complete (a stopped
 * pass: it builds what it has, and does not mark the index built). */
static void thread_done(void)
{
    if (rsx_pc_dec(&s_live) == 0) {
#if defined(PS3RECOMP_RSX_HAL)
        s_t1 = pc_seconds();
#else
        QueryPerformanceCounter(&s_t1);
#endif
        if (s_nrows) rsx_d3d12_pso_index_ready(s_cancel ? 0 : s_rows_hash);
    }
}

static RSX_PC_THREAD_FN(decompile_thread, unused)
{
    (void)unused;
    for (unsigned i = 0; i < s_nprogs && !s_cancel; i++) {
        if (!s_index_only) add_program(&s_progs[i]);
        if (!s_nrows) {   /* (the index needs the bytes still) */
            free(s_progs[i].data);   /* the name stays: jobs point at it */
            s_progs[i].data = NULL;
        }
        rsx_pc_inc(&s_prog_done);
    }
    if (s_nrows && !s_cancel) index_resolve();
    for (unsigned i = 0; i < s_nprogs; i++) { free(s_progs[i].data); s_progs[i].data = NULL; }
    rsx_pc_lock_x(&s_lock);
    s_produced = 1;
    rsx_pc_unlock_x(&s_lock);
    rsx_pc_wake_all(&s_cv);
    thread_done();
    return 0;
}

static RSX_PC_THREAD_FN(compile_thread, unused)
{
    (void)unused;
    for (;;) {
        rsx_pc_lock_x(&s_lock);
        while (s_next == s_njobs && !s_produced && !s_cancel)
            rsx_pc_cond_wait(&s_cv, &s_lock);
        if (s_cancel || s_next == s_njobs) { rsx_pc_unlock_x(&s_lock); break; }
        const Job j = s_jobs[s_next++];
        rsx_pc_unlock_x(&s_lock);
        const int r = rsx_d3d12_shader_cache_put(j.fragment, j.hlsl);
        if (r == 1) rsx_pc_inc(&s_cached);
        else if (r == 2) rsx_pc_inc(&s_built);
#if defined(PS3RECOMP_RSX_HAL)
        else { rsx_pc_inc(&s_failed); fprintf(stderr, "[precompile] %s failed on %s\n", s_sink->compiler, j.name); }
#else
        else { rsx_pc_inc(&s_failed); fprintf(stderr, "[precompile] FXC failed on %s\n", j.name); }
#endif
        rsx_pc_inc(&s_done);
    }
    thread_done();
    return 0;
}

int rsx_shader_precompile_start(int threads)
{
    if (s_nhandles || !s_nprogs) return -1;
#if defined(PS3RECOMP_RSX_HAL)
    if (!s_sink) return -1;   /* no target in this build */
#endif
    const char* dir = rsx_d3d12_shader_cache_path();   /* also creates it */
    if (!dir || !dir[0]) return -1;
#if defined(PS3RECOMP_RSX_HAL)
    if (threads <= 0) threads = pc_cpus();
    /* With the decompile thread, at most the 64 a wait takes on Windows
     * (_reset frees what the threads use once the wait returns). */
    if (threads > MAX_THREADS - 1) threads = MAX_THREADS - 1;
#else
    if (threads <= 0) { SYSTEM_INFO si; GetSystemInfo(&si); threads = (int)si.dwNumberOfProcessors; }
#endif
    if (threads > MAX_THREADS) threads = MAX_THREADS;
    s_threads = threads;
#if defined(PS3RECOMP_RSX_HAL)
    s_t0 = pc_seconds();
#else
    QueryPerformanceCounter(&s_t0);
#endif
    s_live = threads + 1;
    s_handles[s_nhandles] = rsx_pc_thread_start(decompile_thread);
    if (!s_handles[s_nhandles]) { s_live = 0; return -1; }
    s_nhandles++;
    for (int i = 0; i < threads; i++) {
        rsx_pc_thread h = rsx_pc_thread_start(compile_thread);
        if (h) s_handles[s_nhandles++] = h;
        else thread_done();
    }
    return 0;
}

void rsx_shader_precompile_get(rsx_shader_precompile_stats* st)
{
    memset(st, 0, sizeof *st);
    st->programs      = s_nprogs;
    st->programs_done = (unsigned)s_prog_done;
    st->skipped       = (unsigned)s_skipped;
    st->bad           = (unsigned)s_bad;
    rsx_pc_lock_s(&s_lock);
    st->shaders = s_njobs;
    rsx_pc_unlock_s(&s_lock);
    st->dups         = (unsigned)s_dups;
    st->shaders_done = (unsigned)s_done;
    st->built        = (unsigned)s_built;
    st->cached       = (unsigned)s_cached;
    st->failed       = (unsigned)s_failed;
    st->running      = s_nhandles && s_live > 0;
    st->cancelled    = s_cancel != 0;
    st->threads      = s_threads;
    st->index_rows   = s_nrows;
    st->index_found  = s_rows_found;
    if (s_nhandles) {
#if defined(PS3RECOMP_RSX_HAL)
        st->seconds = (st->running ? pc_seconds() : s_t1) - s_t0;
#else
        LARGE_INTEGER f, now;
        QueryPerformanceFrequency(&f);
        if (st->running) QueryPerformanceCounter(&now); else now = s_t1;
        st->seconds = (double)(now.QuadPart - s_t0.QuadPart) / (double)f.QuadPart;
#endif
    }
}

float rsx_shader_precompile_fraction(void)
{
    rsx_shader_precompile_stats st;
    rsx_shader_precompile_get(&st);
    if (!st.programs) return 0.0f;
    if (!st.running) return 1.0f;
    if (!st.programs_done || !st.shaders) return 0.0f;
    const double expect = (double)st.shaders * st.programs / st.programs_done;
    const double f = st.shaders_done / expect;
    return f < 0.0 ? 0.0f : f > 1.0 ? 1.0f : (float)f;
}

void rsx_shader_precompile_cancel(void)
{
    rsx_pc_xchg(&s_cancel, 1);
    rsx_pc_wake_all(&s_cv);
}

void rsx_shader_precompile_wait(void)
{
    if (s_nhandles) rsx_pc_thread_join_all(s_handles, s_nhandles);
}

#if defined(PS3RECOMP_RSX_HAL)
const rsx_shader_precompile_sink* rsx_shader_precompile_sink_of(int target)
{
    switch (target) {
#ifdef _WIN32
    case RSX_PRECOMPILE_D3D12: return &s_sink_d3d12;
    case RSX_PRECOMPILE_DXBC:  return &s_sink_dxbc;
#endif
#if defined(PS3RECOMP_RSX_HAL_SPV)
    case RSX_PRECOMPILE_SPV:   return &s_sink_spv;
#endif
    default:                   return NULL;
    }
}

const rsx_shader_precompile_sink* rsx_shader_precompile_current(void) { return s_sink; }

int rsx_shader_precompile_use(const rsx_shader_precompile_sink* sink)
{
    if (s_nhandles || !sink) return 0;
    s_sink = sink;
    return 1;
}

void rsx_shader_precompile_reset(void)
{
    rsx_shader_precompile_wait();
    for (int i = 0; i < s_nhandles; i++) rsx_pc_thread_close(s_handles[i]);
    s_nhandles = 0;
    s_threads = 0;
    /* The compile threads leave each text in its job (the names point into
     * the programs). */
    for (unsigned i = 0; i < s_njobs; i++) free(s_jobs[i].hlsl);
    free(s_jobs);
    s_jobs = NULL; s_njobs = s_jobcap = s_next = 0;
    s_produced = 0;
    for (unsigned i = 0; i < s_nprogs; i++) { free(s_progs[i].name); free(s_progs[i].data); }
    free(s_progs);
    s_progs = NULL; s_nprogs = s_progcap = 0;
    free(s_texts);
    s_texts = NULL; s_textcap = s_ntexts = 0;
    free(s_rows);
    s_rows = NULL; s_nrows = s_rows_found = 0;
    s_rows_hash = 0; s_index_only = 0;
    memset(&s_conv, 0, sizeof s_conv);
    memset(s_alt, 0, sizeof s_alt);
    s_nalt = 0;
    s_prog_done = s_skipped = s_bad = s_dups = 0;
    s_done = s_built = s_cached = s_failed = s_cancel = s_live = 0;
    s_t0 = s_t1 = 0.0;
}
#endif

#else  /* neither Windows nor the portable renderer */

int  rsx_shader_precompile_kinds(const char* units, const char* alts) { (void)units; (void)alts; return 1; }
void rsx_shader_precompile_add(const char* name, const uint8_t* cg, size_t n) { (void)name; (void)cg; (void)n; }
void rsx_shader_precompile_add_flags(const char* name, const uint8_t* cg, size_t n, unsigned flags)
{
    (void)name; (void)cg; (void)n; (void)flags;
}
int  rsx_shader_precompile_index(const char* rows, size_t len, int index_only)
{
    (void)rows; (void)len; (void)index_only;
    return 0;
}
int  rsx_shader_precompile_index_built(const char* rows, size_t len) { (void)rows; (void)len; return 0; }
int  rsx_shader_precompile_start(int threads) { (void)threads; return -1; }
void rsx_shader_precompile_get(rsx_shader_precompile_stats* st) { if (st) { static const rsx_shader_precompile_stats z; *st = z; } }
float rsx_shader_precompile_fraction(void) { return 1.0f; }
void rsx_shader_precompile_cancel(void) {}
void rsx_shader_precompile_wait(void) {}

#endif
