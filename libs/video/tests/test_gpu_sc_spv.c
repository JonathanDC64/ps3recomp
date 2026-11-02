/*
 * test_gpu_sc_spv -- the compile service's spv target (gpu_shader_spv.h)
 *
 *   test_gpu_sc_spv [--seed N] [--fp N] [--vp N] [--cache DIR] [--dump DIR] [--verbose]
 *   test_gpu_sc_spv --corpus DIR [--tsv FILE] [--threads N] [--spv-out DIR]
 *
 * Every module the target makes for the texts here must
 *  - compile, and validate (SPIRV-Tools, Vulkan 1.1) both as glslang made it
 *    (what the cache holds) and with every post-pass on;
 *  - put each resource where the Vulkan backend's binding model puts it for
 *    the text's layout, worked out here from the text's own declarations:
 *    set 0 the CB slots b<n> at n, the RAW slots t<n> at 16 + n and the
 *    static samplers s<n> at 48 + n; the table slots sets 1, 2, ... in slot
 *    order, texture entry j at 16 + j and sampler entry j at 48 + j; the
 *    CONST32 slots' members in the one push constant block, at their offsets;
 *  - give each varying its semantic's location (COLOR0 0, COLOR1 1, FOG 2,
 *    TEXCOORDn 3 + n, UVBOUNDn 13 + n; vertex inputs ATTRn n; SV_TARGETn n),
 *    read from a build that keeps the semantics (GPU_SPV_GL_SEMANTICS), with
 *    the same locations as the module itself;
 *  - be the same bytes from gpu_sc_compile_layout as from the steps one by
 *    one, so twice the same.
 * The texts: the seeded generator's programs (rsx_prog_gen.h, shared with
 * test_shader_text_identity.c) through rsxc_shader.c (vertex, fetch form,
 * fragment, fragment with the sprite-edge clamp, HDR variants), the
 * sprite-edge geometry shader, and the built-in passes with their layouts
 * (output filter, blit, HDR blit and its Sharp variant, write-back
 * downscale, FXAA, SMAA's six entry points).
 * Then: each edit's output on fixed texts against a golden hash per edit
 * version (an edit whose output changes must take a new version: the
 * versions are in the cache's key); the generated texts keep none of the
 * idioms the edits rewrite; the post-passes do what they say; the layout
 * pass and E4 refuse what the model has no place for; and the cache round
 * trip (compile, load, put, the header's checks, a layout's own key).
 *
 * --corpus runs the module checks over a directory of kept texts
 * (<profile>_<key>.dxbc.hlsl, as PS3_SHADER_CACHE_HLSL=1 keeps them) and
 * prints, and writes as a TSV, each one's result and the CPU time of each
 * step: a measurement on whatever machine runs it, not an A/B; --spv-out
 * keeps each module (every post-pass on) for other tools. Nothing here
 * comes from a game, and a corpus is not committed. Windows and Linux.
 * Exit 0 when everything holds.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <intrin.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#endif

#include "rsxcore/rsxc_shader.h"
#include "gpu/gpu.h"
#include "gpu/gpu_shader.h"
#include "gpu/gpu_shader_spv.h"
#include "rsx_outfilter.h"
#include "rsx_fxaa.h"
#include "rsx_smaa.h"
#include "rsx_smaa_src.h"
#include "rsx_prog_gen.h"

/* Guest memory, which host_stubs.c (the runtime's host side) names; no guest
 * runs here. */
uint8_t* vm_base = NULL;

static int g_verbose;
static const char* g_dump;   /* --dump: a failing text and its edited text go here */
static unsigned long g_checks, g_fail;
static char g_ctx[200];

#define CHECK(cond, ...)                                                          \
    do {                                                                          \
        g_checks++;                                                               \
        if (!(cond)) {                                                            \
            g_fail++;                                                             \
            if (g_fail <= 40) { printf("FAIL [%s] ", g_ctx); printf(__VA_ARGS__); printf("\n"); } \
        }                                                                         \
    } while (0)

static uint64_t fnv64(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
#define FNV0 14695981039346656037ull

/* ---- time ------------------------------------------------------------------
 * The calling thread's CPU time. Windows counts a thread's time in clock
 * ticks (15.6 ms), so there it is the thread's cycles at the TSC's rate. */
#ifdef _WIN32
static double s_tsc_hz;
static void cpu_init(void)
{
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    const unsigned long long t0 = __rdtsc();
    Sleep(100);
    QueryPerformanceCounter(&b);
    const unsigned long long t1 = __rdtsc();
    s_tsc_hz = (double)(t1 - t0) / ((double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart);
}
static double cpu_now(void)
{
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return (double)c / s_tsc_hz;
}
static double wall_now(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static void make_dir(const char* p) { _mkdir(p); }
static void set_env(const char* k, const char* v) { _putenv_s(k, v); }
#else
static void cpu_init(void) {}
static double cpu_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static double wall_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static void make_dir(const char* p) { mkdir(p, 0755); }
static void set_env(const char* k, const char* v) { setenv(k, v, 1); }
#endif

static char* read_file(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* p = len >= 0 ? (char*)malloc((size_t)len + 1) : NULL;
    if (p && fread(p, 1, (size_t)len, f) != (size_t)len) { free(p); p = NULL; }
    fclose(f);
    if (p) { p[len] = 0; if (n) *n = (size_t)len; }
    return p;
}
static int write_file(const char* path, const void* p, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static char* dup_str(const char* s)
{
    const size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static int file_exists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* ---- SPIR-V reflection: what a module says about its variables ----------- */
enum {
    OP_NAME = 5, OP_MEMBER_NAME = 6, OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16, OP_CAPABILITY = 17,
    OP_TYPE_ARRAY = 28, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_FUNCTION = 54, OP_VARIABLE = 59,
    OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_DECORATE_STRING = 5632
};
enum {
    DEC_BUILTIN = 11, DEC_INVARIANT = 18, DEC_LOCATION = 30, DEC_BINDING = 33, DEC_SET = 34, DEC_OFFSET = 35,
    DEC_NOCONTRACT = 42, DEC_SEMANTIC = 5635
};
enum { SC_UC = 0, SC_IN = 1, SC_UNIFORM = 2, SC_OUT = 3, SC_PUSH = 9, SC_SSBO = 12 };
enum { BI_POSITION = 0, BI_VERTEX_INDEX = 42, BI_BASE_VERTEX = 4424 };
enum { R_SET = 1, R_BIND = 2, R_LOC = 4, R_BI = 8, R_INV = 16 };

typedef struct {
    uint32_t id, sc, set, binding, location, builtin, flags;
    char name[64], sem[32];
} RVar;
typedef struct { char name[48]; uint32_t offset; } RMember;
typedef struct {
    int ok;
    int nvar;
    RVar var[200];
    int npushvar, npush;
    RMember push[48];
    int szinp_cap, denorm_cap, szinp_mode, denorm_mode;
    int nocontract, float_ops;
    int pos_out, pos_invariant;
    int vertex_index, base_vertex;
} Refl;

static void wstr(const uint32_t* w, size_t nw, char* out, size_t cap)
{
    const char* s = (const char*)w;
    size_t l = 0;
    while (l < nw * 4 && s[l] && l + 1 < cap) { out[l] = s[l]; l++; }
    out[l] = 0;
}

static int is_float_op(uint32_t op)
{
    return op == 129 || op == 131 || op == 133 || op == 136 || op == 140 || op == 141 || (op >= 142 && op <= 148);
}

typedef struct { uint32_t id, mem, val; size_t at; } MemRec;

static Refl* reflect(const uint32_t* w, size_t n)
{
    Refl* r = (Refl*)calloc(1, sizeof *r);
    if (!r || n < 5 || w[0] != 0x07230203u) return r;
    const uint32_t bound = w[3];
    size_t* name_at = (size_t*)calloc(bound + 1, sizeof *name_at);
    size_t* sem_at = (size_t*)calloc(bound + 1, sizeof *sem_at);
    uint32_t* d_set = (uint32_t*)calloc(bound + 1, 4);
    uint32_t* d_bind = (uint32_t*)calloc(bound + 1, 4);
    uint32_t* d_loc = (uint32_t*)calloc(bound + 1, 4);
    uint32_t* d_bi = (uint32_t*)calloc(bound + 1, 4);
    uint32_t* d_fl = (uint32_t*)calloc(bound + 1, 4);
    uint32_t* p_sc = (uint32_t*)calloc(bound + 1, 4);
    uint32_t* p_ty = (uint32_t*)calloc(bound + 1, 4);
    uint32_t* arr_el = (uint32_t*)calloc(bound + 1, 4);
    uint8_t* is_ptr = (uint8_t*)calloc(bound + 1, 1);
    MemRec* mn = (MemRec*)calloc(n, sizeof *mn);   /* member names */
    MemRec* mo = (MemRec*)calloc(n, sizeof *mo);   /* member offsets */
    MemRec* mb = (MemRec*)calloc(n, sizeof *mb);   /* member builtins */
    MemRec* mi = (MemRec*)calloc(n, sizeof *mi);   /* member invariants */
    size_t nmn = 0, nmo = 0, nmb = 0, nmi = 0;
    if (!name_at || !sem_at || !d_set || !d_bind || !d_loc || !d_bi || !d_fl || !p_sc || !p_ty || !arr_el ||
        !is_ptr || !mn || !mo || !mb || !mi)
        goto done;
    uint32_t entry_fn = 0;
    int in_fn = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xFFFF, wc = w[i] >> 16;
        if (!wc || i + wc > n) goto done;
        if (op == OP_FUNCTION) in_fn = 1;
        if (in_fn) {
            if (is_float_op(op)) r->float_ops++;
        } else if (op == OP_CAPABILITY && wc >= 2) {
            if (w[i + 1] == 4466) r->szinp_cap = 1;
            if (w[i + 1] == 4464) r->denorm_cap = 1;
        } else if (op == OP_ENTRY_POINT && wc >= 3 && !entry_fn) {
            entry_fn = w[i + 2];
        } else if (op == OP_EXECUTION_MODE && wc >= 4 && w[i + 1] == entry_fn) {
            if (w[i + 2] == 4461 && w[i + 3] == 32) r->szinp_mode = 1;
            if (w[i + 2] == 4459 && w[i + 3] == 32) r->denorm_mode = 1;
        } else if (op == OP_NAME && wc >= 3 && w[i + 1] < bound) {
            name_at[w[i + 1]] = i;
        } else if (op == OP_MEMBER_NAME && wc >= 4) {
            mn[nmn].id = w[i + 1]; mn[nmn].mem = w[i + 2]; mn[nmn].at = i; nmn++;
        } else if (op == OP_DECORATE && wc >= 3 && w[i + 1] < bound) {
            const uint32_t id = w[i + 1], d = w[i + 2], v = wc >= 4 ? w[i + 3] : 0;
            if (d == DEC_SET) { d_set[id] = v; d_fl[id] |= R_SET; }
            if (d == DEC_BINDING) { d_bind[id] = v; d_fl[id] |= R_BIND; }
            if (d == DEC_LOCATION) { d_loc[id] = v; d_fl[id] |= R_LOC; }
            if (d == DEC_BUILTIN) { d_bi[id] = v; d_fl[id] |= R_BI; }
            if (d == DEC_INVARIANT) d_fl[id] |= R_INV;
            if (d == DEC_NOCONTRACT) r->nocontract++;
        } else if (op == OP_DECORATE_STRING && wc >= 4 && w[i + 1] < bound && w[i + 2] == DEC_SEMANTIC) {
            sem_at[w[i + 1]] = i;
        } else if (op == OP_MEMBER_DECORATE && wc >= 4) {
            const uint32_t d = w[i + 3];
            if (d == DEC_OFFSET && wc >= 5) { mo[nmo].id = w[i + 1]; mo[nmo].mem = w[i + 2]; mo[nmo].val = w[i + 4]; nmo++; }
            if (d == DEC_BUILTIN && wc >= 5) { mb[nmb].id = w[i + 1]; mb[nmb].mem = w[i + 2]; mb[nmb].val = w[i + 4]; nmb++; }
            if (d == DEC_INVARIANT) { mi[nmi].id = w[i + 1]; mi[nmi].mem = w[i + 2]; nmi++; }
        } else if (op == OP_TYPE_POINTER && wc >= 4 && w[i + 1] < bound) {
            is_ptr[w[i + 1]] = 1; p_sc[w[i + 1]] = w[i + 2]; p_ty[w[i + 1]] = w[i + 3];
        } else if (op == OP_TYPE_ARRAY && wc >= 4 && w[i + 1] < bound) {
            arr_el[w[i + 1]] = w[i + 2];
        }
        i += wc;
    }
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        const uint32_t op = w[i] & 0xFFFF, wc = w[i] >> 16;
        if (op == OP_FUNCTION) break;
        if (op != OP_VARIABLE || wc < 4) continue;
        const uint32_t ptr = w[i + 1], id = w[i + 2], sc = w[i + 3];
        if (sc != SC_UC && sc != SC_IN && sc != SC_UNIFORM && sc != SC_OUT && sc != SC_PUSH && sc != SC_SSBO) continue;
        uint32_t ty = ptr < bound && is_ptr[ptr] ? p_ty[ptr] : 0;
        while (ty && ty < bound && arr_el[ty]) ty = arr_el[ty];
        if (sc == SC_PUSH) {
            r->npushvar++;
            for (size_t k = 0; k < nmo && r->npush < 48; k++) {
                if (mo[k].id != ty) continue;
                RMember* m = &r->push[r->npush++];
                m->offset = mo[k].val;
                snprintf(m->name, sizeof m->name, "#%u", mo[k].mem);
                for (size_t q = 0; q < nmn; q++)
                    if (mn[q].id == ty && mn[q].mem == mo[k].mem) {
                        wstr(w + mn[q].at + 3, (w[mn[q].at] >> 16) - 3, m->name, sizeof m->name);
                        break;
                    }
            }
            continue;
        }
        if (r->nvar >= 200) continue;
        RVar* v = &r->var[r->nvar++];
        v->id = id; v->sc = sc; v->flags = d_fl[id];
        v->set = d_set[id]; v->binding = d_bind[id]; v->location = d_loc[id]; v->builtin = d_bi[id];
        if (name_at[id]) wstr(w + name_at[id] + 2, (w[name_at[id]] >> 16) - 2, v->name, sizeof v->name);
        if (!v->name[0] && ty < bound && name_at[ty])
            wstr(w + name_at[ty] + 2, (w[name_at[ty]] >> 16) - 2, v->name, sizeof v->name);
        if (sem_at[id]) {
            wstr(w + sem_at[id] + 3, (w[sem_at[id]] >> 16) - 3, v->sem, sizeof v->sem);
            for (char* c = v->sem; *c; c++) *c = (char)toupper((unsigned char)*c);
        }
        if (sc == SC_OUT) {
            if ((v->flags & R_BI) && v->builtin == BI_POSITION) {
                r->pos_out = 1;
                if (v->flags & R_INV) r->pos_invariant = 1;
            }
            for (size_t k = 0; k < nmb; k++)
                if (mb[k].id == ty && mb[k].val == BI_POSITION) {
                    r->pos_out = 1;
                    for (size_t q = 0; q < nmi; q++)
                        if (mi[q].id == ty && mi[q].mem == mb[k].mem) r->pos_invariant = 1;
                }
        }
        if (sc == SC_IN && (v->flags & R_BI) && v->builtin == BI_VERTEX_INDEX) r->vertex_index = 1;
        if (sc == SC_IN && (v->flags & R_BI) && v->builtin == BI_BASE_VERTEX) r->base_vertex = 1;
    }
    r->ok = 1;
done:
    free(name_at); free(sem_at); free(d_set); free(d_bind); free(d_loc); free(d_bi); free(d_fl);
    free(p_sc); free(p_ty); free(arr_el); free(is_ptr); free(mn); free(mo); free(mb); free(mi);
    return r;
}

/* ---- the binding model, worked out here from a text's declarations -------- */
typedef struct { char name[64]; char cls; int raw; unsigned reg; } Decl;

/* `<type> <name>[N]? : register(<c><n>)` and `cbuffer <name> : register(b<n>)`. */
static int scan_decls(const char* t, Decl* d, int max)
{
    int n = 0;
    for (const char* p = strstr(t, "register("); p && n < max; p = strstr(p + 1, "register(")) {
        const char cls = p[9];
        if (cls != 'b' && cls != 't' && cls != 's') continue;
        const unsigned reg = (unsigned)strtoul(p + 10, NULL, 10);
        const char* q = p;
        while (q > t && isspace((unsigned char)q[-1])) q--;
        if (q == t || q[-1] != ':') continue;
        q--;
        while (q > t && isspace((unsigned char)q[-1])) q--;
        if (q > t && q[-1] == ']') {
            while (q > t && q[-1] != '[') q--;
            if (q > t) q--;
            while (q > t && isspace((unsigned char)q[-1])) q--;
        }
        const char* e = q;
        while (q > t && (isalnum((unsigned char)q[-1]) || q[-1] == '_')) q--;
        if (q == e) continue;
        const char* te = q;
        while (te > t && isspace((unsigned char)te[-1])) te--;
        const char* ts = te;
        while (ts > t && (isalnum((unsigned char)ts[-1]) || ts[-1] == '_' || ts[-1] == '<' || ts[-1] == '>')) ts--;
        char type[64];
        snprintf(type, sizeof type, "%.*s", (int)(te - ts), ts);
        Decl* x = &d[n];
        snprintf(x->name, sizeof x->name, "%.*s", (int)(e - q), q);
        x->cls = cls;
        x->reg = reg;
        x->raw = strstr(type, "ByteAddressBuffer") != NULL || strstr(type, "StructuredBuffer") != NULL;
        n++;
    }
    return n;
}

typedef struct { int kind; uint32_t set, binding; } Where;   /* kind 0 none, 1 a binding, 2 the push block */

static Where model_where(const gpu_layout_desc* L, char cls, int raw, unsigned reg)
{
    Where w = { 0, 0, 0 };
    unsigned tables = 0;
    for (uint32_t k = 0; k < L->nslots; k++) {
        const gpu_layout_slot* s = &L->slot[k];
        const int in_table = reg >= s->reg && reg < (unsigned)s->reg + s->count;
        switch (s->kind) {
        case GPU_SLOT_CB: if (cls == 'b' && s->reg == reg) { w.kind = 1; w.set = 0; w.binding = reg; } break;
        case GPU_SLOT_CONST32: if (cls == 'b' && s->reg == reg) w.kind = 2; break;
        case GPU_SLOT_RAW: if (cls == 't' && raw && s->reg == reg) { w.kind = 1; w.set = 0; w.binding = 16 + reg; } break;
        case GPU_SLOT_TABLE_TEX:
            if (cls == 't' && !raw && in_table) { w.kind = 1; w.set = 1 + tables; w.binding = 16 + reg - s->reg; }
            tables++;
            break;
        case GPU_SLOT_TABLE_SAMPLER:
            if (cls == 's' && in_table) { w.kind = 1; w.set = 1 + tables; w.binding = 48 + reg - s->reg; }
            tables++;
            break;
        default: break;
        }
    }
    if (cls == 's' && !w.kind)
        for (uint32_t k = 0; k < L->nstatic; k++)
            if (L->statics[k].reg == reg) { w.kind = 1; w.set = 0; w.binding = 48 + reg; }
    return w;
}

/* A semantic's location in the model; -1 if it has none. */
static int model_location(gpu_sc_stage stage, int input, const char* sem)
{
    char name[32];
    int k = 0;
    while (sem[k] && !isdigit((unsigned char)sem[k]) && k < 31) { name[k] = (char)toupper((unsigned char)sem[k]); k++; }
    name[k] = 0;
    const int idx = sem[k] ? atoi(sem + k) : 0;
    if (!strcmp(name, "COLOR")) return idx <= 1 ? idx : -1;
    if (!strcmp(name, "FOG")) return idx == 0 ? 2 : -1;
    if (!strcmp(name, "TEXCOORD")) return idx <= 9 ? 3 + idx : -1;
    if (!strcmp(name, "UVBOUND")) return idx <= 7 ? 13 + idx : -1;
    if (!strcmp(name, "ATTR")) return stage == GPU_SC_VS && input && idx <= 15 ? idx : -1;
    if (!strcmp(name, "SV_TARGET")) return stage == GPU_SC_PS && !input && idx <= 7 ? idx : -1;
    return -1;
}

/* ---- one text through the target ------------------------------------------ */
typedef struct { const char* name; uint32_t offset; int optional; } PushWant;   /* optional: E4 may drop it unused */

typedef struct {
    gpu_sc_stage stage;
    const char* text;
    size_t n;
    const char* name;
    const char* entry;
    const gpu_layout_desc* layout;   /* NULL: the guest programs' */
    unsigned flags;                  /* GPU_SC_OPTIMIZE where the core passes it */
    const PushWant* push;            /* the push block's members ({NULL} ends); NULL: a guest text's rule */
    int push_required;               /* every wanted member must be there */
    int guest;                       /* a decompiler's text: the idiom checks too */
    const char* spv_out;             /* the module with every post-pass, written here (--spv-out) */
} Job;

typedef struct {
    int compiled, valid, refl, det, post;
    int problems;
    double cpu_edit_glslang, cpu_post, cpu_validate;
    uint32_t words;
    uint64_t in_locs, out_locs;
    char msg[400];
} JobResult;

static void jr_fail(JobResult* r, const char* fmt, ...)
{
    r->problems++;
    if (r->msg[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->msg, sizeof r->msg, fmt, ap);
    va_end(ap);
}

static int is_tok(char c) { return isalnum((unsigned char)c) || c == '_' || c == '.'; }

/* The edits' idioms left in an edited guest text (the decompilers' forms
 * looser than E2-E5 match them, so a form that drifts shows here). */
static void check_idioms(const char* t, const gpu_layout_desc* L, JobResult* r)
{
    for (const char* p = strstr(t, "=="); p; p = strstr(p + 2, "==")) {
        const char* a = p;
        while (a > t && a[-1] == ' ') a--;
        const char* a0 = a;
        while (a0 > t && is_tok(a0[-1])) a0--;
        const char* b = p + 2;
        while (*b == ' ') b++;
        const char* b1 = b;
        while (is_tok(*b1)) b1++;
        if (a > a0 && b1 - b == a - a0 && !memcmp(a0, b, (size_t)(a - a0)) && !isdigit((unsigned char)*a0)) {
            jr_fail(r, "E2 left a self-compare: %.40s", a0);
            return;
        }
        if (!strncmp(b, "0.0", 3)) {
            const char* q = strstr(b, "? 0.0 :");
            const char* nl = strchr(b, '\n');
            if (q && (!nl || q < nl) && q - b < 64) { jr_fail(r, "E2 left a zero guard: %.60s", a0); return; }
        }
    }
    for (const char* p = strstr(t, "> 0.0 ?"); p; p = strstr(p + 7, "> 0.0 ?")) {
        const char* q = strstr(p, "/ sqrt(");
        const char* nl = strchr(p, '\n');
        if (q && (!nl || q < nl)) { jr_fail(r, "E2 left a DIVSQ guard: %.60s", p); return; }
    }
    for (const char* p = strstr(t, "vf_raw.Load"); p; p = strstr(p + 11, "vf_raw.Load"))
        if (strncmp(p + 11, "(min(w, max(n, 1u)", 18) && strncmp(p + 11, "4(min(w, max(n, 4u)", 19)) {
            jr_fail(r, "E3 left a load: %.40s", p);
            return;
        }
    for (uint32_t k = 0; k < L->nslots; k++)
        if (L->slot[k].kind == GPU_SLOT_CONST32) {
            char reg[32];
            snprintf(reg, sizeof reg, "register(b%u)", L->slot[k].reg);
            if (strstr(t, reg)) { jr_fail(r, "E4 left %s", reg); return; }
        }
    if (strstr(t, "rsx_tex[") || strstr(t, "rsx_samp[")) jr_fail(r, "E5 left an array");
}

static void run_job(const Job* j, JobResult* r)
{
    memset(r, 0, sizeof *r);
    const gpu_layout_desc* L = j->layout ? j->layout : gpu_spv_guest_layout();
    const unsigned gl = (j->flags & GPU_SC_OPTIMIZE) ? GPU_SPV_GL_OPTIMIZE : 0u;
    const gpu_layout_desc* bind_layout = gpu_spv_text_has_bindings(j->text, j->n) ? NULL : L;
    char err[1024];
    err[0] = 0;
    size_t tl = 0;
    double c0 = cpu_now();
    char* text = gpu_spv_edit(j->stage, j->text, j->n, GPU_SPV_EDITS_ALL, L, &tl, err, sizeof err);
    uint32_t* raw = NULL;
    size_t nraw = 0;
    int rc = text ? gpu_spv_glslang(j->stage, text, tl, j->name, j->entry, gl, bind_layout, &raw, &nraw, err,
                                    sizeof err)
                  : GPU_SC_FAILED;
    r->cpu_edit_glslang = cpu_now() - c0;
    if (rc != GPU_SC_OK) {
        jr_fail(r, "compile: %.300s", err[0] ? err : "out of memory");
        free(text);
        free(raw);
        return;
    }
    r->compiled = 1;
    r->words = (uint32_t)nraw;

    /* the post-passes (every one), and the validator on both */
    uint32_t* pw = NULL;
    size_t pn = 0;
    c0 = cpu_now();
    rc = gpu_spv_post(j->stage, raw, nraw, GPU_SC_SPV_ALL, &pw, &pn);
    r->cpu_post = cpu_now() - c0;
    c0 = cpu_now();
    const int v_raw = gpu_spv_validate(raw, nraw, err, sizeof err);
    if (!v_raw) jr_fail(r, "spirv-val, glslang's module: %.300s", err);
    const int v_post = rc == GPU_SC_OK && gpu_spv_validate(pw, pn, err, sizeof err);
    if (rc != GPU_SC_OK) jr_fail(r, "post-passes: malformed module");
    else if (!v_post) jr_fail(r, "spirv-val, post-passes: %.300s", err);
    r->cpu_validate = cpu_now() - c0;
    r->valid = v_raw && v_post;
    if (j->spv_out && pw) write_file(j->spv_out, pw, pn * 4);
    {
        uint32_t* zw = NULL;
        size_t zn = 0;
        if (gpu_spv_post(j->stage, raw, nraw, 0, &zw, &zn) != GPU_SC_OK || zn != nraw ||
            memcmp(zw, raw, nraw * 4))
            jr_fail(r, "the post-passes with no options changed the module");
        free(zw);
    }

    /* the service's own call: the same bytes */
    gpu_sc_blob b;
    rc = gpu_sc_compile_layout(GPU_SC_SPV, j->stage, j->text, j->n, j->name, j->entry, j->layout,
                               GPU_SC_NO_CACHE | j->flags, &b, err, sizeof err);
    r->det = rc == GPU_SC_OK && pw && b.size == pn * 4 && !memcmp(b.data, pw, b.size);
    if (!r->det) jr_fail(r, "gpu_sc_compile_layout gave other bytes%s%.200s", rc ? ": " : "", rc ? err : "");
    gpu_sc_blob_release(&b);

    /* reflection: the module, and a build with the semantics */
    uint32_t* sw = NULL;
    size_t sn = 0;
    rc = gpu_spv_glslang(j->stage, text, tl, j->name, j->entry, gl | GPU_SPV_GL_SEMANTICS, bind_layout, &sw, &sn,
                         err, sizeof err);
    if (rc != GPU_SC_OK) jr_fail(r, "compile with the semantics: %.300s", err);
    Refl* rp = pw ? reflect(pw, pn) : NULL;
    Refl* rs = sw ? reflect(sw, sn) : NULL;
    const int before = r->problems;
    if (!rp || !rp->ok || !rs || !rs->ok) {
        jr_fail(r, "reflection: malformed module");
    } else {
        /* resources by name, against the declarations */
        static const int kmax = 128;
        Decl* d = (Decl*)malloc(sizeof(Decl) * (size_t)kmax);
        const int nd = d ? scan_decls(text, d, kmax) : 0;
        for (int i = 0; i < rp->nvar; i++) {
            const RVar* v = &rp->var[i];
            if (v->sc != SC_UC && v->sc != SC_UNIFORM && v->sc != SC_SSBO) continue;
            int k = 0;
            while (k < nd && strcmp(d[k].name, v->name)) k++;
            if (k == nd) { jr_fail(r, "resource %s: the text does not declare it", v->name); continue; }
            const Where wh = model_where(L, d[k].cls, d[k].raw, d[k].reg);
            if (wh.kind != 1)
                jr_fail(r, "resource %s (%c%u): the layout has no binding for it", v->name, d[k].cls, d[k].reg);
            else if ((v->flags & (R_SET | R_BIND)) != (R_SET | R_BIND) || v->set != wh.set || v->binding != wh.binding)
                jr_fail(r, "resource %s: set %u binding %u, the model's %u %u", v->name, v->set, v->binding, wh.set,
                        wh.binding);
        }
        free(d);
        /* the push constant block */
        static const PushWant k_vfdraw[] = { { "vf_desc", 0 }, { NULL, 0 } };
        static const PushWant k_none[] = { { NULL, 0 } };
        const PushWant* want = j->push ? j->push : strstr(j->text, "cbuffer VFDraw : register(b2)") ? k_vfdraw : k_none;
        if (rp->npushvar > 1) jr_fail(r, "%d push constant blocks", rp->npushvar);
        for (int i = 0; i < rp->npush; i++) {
            int k = 0;
            while (want[k].name && strcmp(want[k].name, rp->push[i].name)) k++;
            if (!want[k].name) jr_fail(r, "push member %s: not one of the layout's", rp->push[i].name);
            else if (want[k].offset != rp->push[i].offset)
                jr_fail(r, "push member %s at %u, the layout's %u", rp->push[i].name, rp->push[i].offset,
                        want[k].offset);
        }
        if (j->push_required)
            for (int k = 0; want[k].name; k++) {
                if (want[k].optional) continue;
                int i = 0;
                while (i < rp->npush && strcmp(want[k].name, rp->push[i].name)) i++;
                if (i == rp->npush) jr_fail(r, "push member %s missing", want[k].name);
            }
        /* locations: the semantics build against the model, the module against that */
        uint64_t in_s = 0, out_s = 0, in_p = 0, out_p = 0;
        for (int i = 0; i < rs->nvar; i++) {
            const RVar* v = &rs->var[i];
            if ((v->sc != SC_IN && v->sc != SC_OUT) || (v->flags & R_BI)) continue;
            if (!(v->flags & R_LOC)) { jr_fail(r, "varying %s: no location", v->name); continue; }
            const int loc = model_location(j->stage, v->sc == SC_IN, v->sem);
            if (loc < 0) jr_fail(r, "varying %s: semantic '%s' is not in the model", v->name, v->sem);
            else if ((uint32_t)loc != v->location)
                jr_fail(r, "varying %s %s: location %u, the model's %d", v->name, v->sem, v->location, loc);
            if (v->location < 64) *(v->sc == SC_IN ? &in_s : &out_s) |= 1ull << v->location;
        }
        for (int i = 0; i < rp->nvar; i++) {
            const RVar* v = &rp->var[i];
            if ((v->sc != SC_IN && v->sc != SC_OUT) || (v->flags & R_BI) || !(v->flags & R_LOC)) continue;
            if (v->location < 64) *(v->sc == SC_IN ? &in_p : &out_p) |= 1ull << v->location;
        }
        if (in_s != in_p || out_s != out_p) jr_fail(r, "the module's locations are not the semantics build's");
        r->in_locs = in_p;
        r->out_locs = out_p;
        r->refl = r->problems == before;
        /* the post-passes' work */
        const int before_post = r->problems;
        if (!rp->szinp_cap || !rp->szinp_mode) jr_fail(r, "no SignedZeroInfNanPreserve 32");
        if (!rp->denorm_cap || !rp->denorm_mode) jr_fail(r, "no DenormPreserve 32");
        if ((j->stage == GPU_SC_VS || j->stage == GPU_SC_GS) && rp->pos_out && !rp->pos_invariant)
            jr_fail(r, "the position output is not Invariant");
        if (j->stage == GPU_SC_VS && rp->vertex_index && !rp->base_vertex) jr_fail(r, "no BaseVertex beside VertexIndex");
        if (rp->nocontract < rp->float_ops)
            jr_fail(r, "NoContraction on %d of %d float operations", rp->nocontract, rp->float_ops);
        r->post = r->problems == before_post;
        if (j->guest) check_idioms(text, L, r);
    }
    free(rp);
    free(rs);
    free(sw);
    free(pw);
    free(raw);
    free(text);
}

/* --dump: a job's text and its edited text. */
static void dump_job(const Job* j, const char* what)
{
    {
        char path[4200], name[128];
        size_t k = 0;
        for (const char* c = what; *c && k + 1 < sizeof name; c++) name[k++] = isalnum((unsigned char)*c) ? *c : '_';
        name[k] = 0;
        snprintf(path, sizeof path, "%s/%s.hlsl", g_dump, name);
        write_file(path, j->text, j->n);
        size_t tl = 0;
        char* t = gpu_spv_edit(j->stage, j->text, j->n, GPU_SPV_EDITS_ALL, j->layout, &tl, NULL, 0);
        snprintf(path, sizeof path, "%s/%s.edited.hlsl", g_dump, name);
        if (t) write_file(path, t, tl);
        free(t);
    }
}

/* A job's result counted. */
static void report_job(const Job* j, const JobResult* rr, const char* what)
{
    const JobResult r = *rr;
    snprintf(g_ctx, sizeof g_ctx, "%s", what);
    CHECK(r.problems == 0, "%s (%d problem%s)", r.msg, r.problems, r.problems == 1 ? "" : "s");
    if (r.problems && g_dump) dump_job(j, what);
    if (g_verbose)
        printf("  %-28s %s %5u words  %.1f ms\n", what, r.problems ? "FAIL" : "ok  ", r.words,
               r.cpu_edit_glslang * 1e3);
}

static JobResult check_job(const Job* j, const char* what)
{
    JobResult r;
    run_job(j, &r);
    report_job(j, &r, what);
    return r;
}

/* ---- the edits' goldens ------------------------------------------------------
 * Each edit alone on a fixed text; the FNV-1a 64 of its output must be the
 * golden of the edit's version. A change to an edit's output takes a new
 * version (GPU_SPV_En_VERSION, in the cache's key) and a new row here. */
static const char k_e1_text[] =
    "struct VSOut { float4 pos : SV_POSITION; float4 c0 : COLOR0; float4 c1 : COLOR1; float fog : FOG;\n"
    "    float4 t0 : TEXCOORD0; float4 t9:TEXCOORD9; nointerpolation float4 b7:UVBOUND7; float2 uv : TEXCOORD; };\n"
    "VSOut main(float4 a0 : ATTR0, float4 a15:ATTR15, uint id : SV_VertexID) {\n"
    "    VSOut o = (VSOut)0; o.pos = a0 + a15; o.t9 = float4(id, 0, 0, 1); return o; }\n";
static const char k_e2_text[] =
    "float4 f(float4 _o, float4 _o1, float a, float b, float4 X, float4 Y) {\n"
    "    float4 r = (_o == _o) ? _o : (float4)0;\n"
    "    r += (_o1 == _o1) ? _o1 : (float4)0;\n"
    "    float m = (a == 0.0 || b == 0.0) ? 0.0 : a * b;\n"
    "    r += (abs(X) > 0.0 ? (X) / sqrt(abs((Y).x)) : (float4)0.0);\n"
    "    r += (abs(X.x) > 0.0 ? (X.x) / sqrt(abs((Y).y)) : 0.0);\n"
    "    return r + m; }\n";
static const char k_e3_text[] =
    "ByteAddressBuffer vf_raw : register(t20);\n"
    "uint f(uint a) { return vf_raw.Load(a) + vf_raw.Load4(a + 16u).x + vf_raw.Load(a * 4u); }\n";
static const char k_e4_guest[] =
    "cbuffer VFDraw : register(b2) { uint vf_desc; };\n"
    "uint f() { return vf_desc; }\n";
static const char k_e4_two[] =
    "cbuffer OF : register(b0) { float2 vp_org; float2 scale, src_size; float2 pad; };\n"
    "float4 g(float4 p) { return float4(p.xy - vp_org, scale * src_size); }\n"
    "cbuffer HDR : register(b1) { float hw_white; float hw_ratio; float hw_knee; float hw_srgb;\n"
    "                             float hw_use_t; float3 hw_pad; };\n"
    "float h() { return hw_white + hw_ratio + hw_knee + hw_srgb + hw_use_t; }\n";
static const char k_e5_text[] =
    "Texture2D rsx_tex[16] : register(t0);\n"
    "TextureCube rsx_cube[2] : register(t4);\n"
    "SamplerState rsx_samp[16] : register(s0);\n"
    "float4 f(float2 uv) { return rsx_tex[3].Sample(rsx_samp[3], uv) + rsx_tex[15].Sample(rsx_samp[15], uv); }\n";

typedef struct { unsigned edit; int version; uint64_t hash; } Golden;
static const Golden k_golden[] = {
    { GPU_SPV_E1, 1, 0x8c7dacc32c2289f1ull },
    { GPU_SPV_E2, 1, 0x65d091bd563b2df9ull },
    { GPU_SPV_E3, 1, 0x18b28b3bc5b2cfe8ull },
    { GPU_SPV_E3, 2, 0x6c9898290b3d4902ull },   /* bounds in words: GetDimensions is glslang's uint count */
    { GPU_SPV_E4, 1, 0x20c7ec2a01ed1b62ull },
    { GPU_SPV_E5, 1, 0xd3c093dd4050a371ull },
};

static gpu_layout_desc g_of, g_blit, g_hdr, g_down, g_aa, g_smaa;

static uint64_t edit_hash(uint64_t h, gpu_sc_stage st, const char* text, unsigned edit, const gpu_layout_desc* L)
{
    char err[256];
    size_t n = 0;
    char* out = gpu_spv_edit(st, text, strlen(text), edit, L, &n, err, sizeof err);
    CHECK(out != NULL, "the edit failed: %s", err);
    if (g_verbose && out) printf("---- edit %u ----\n%s\n", edit, out);
    if (out) {
        /* the same twice */
        size_t n2 = 0;
        char* again = gpu_spv_edit(st, text, strlen(text), edit, L, &n2, err, sizeof err);
        CHECK(again && n2 == n && !memcmp(again, out, n), "the edit is not deterministic");
        free(again);
        h = fnv64(h, out, n);
    }
    free(out);
    return h;
}

static void test_goldens(void)
{
    static const int versions[5] = { GPU_SPV_E1_VERSION, GPU_SPV_E2_VERSION, GPU_SPV_E3_VERSION, GPU_SPV_E4_VERSION,
                                     GPU_SPV_E5_VERSION };
    for (int e = 0; e < 5; e++) {
        const unsigned bit = 1u << e;
        snprintf(g_ctx, sizeof g_ctx, "golden E%d", e + 1);
        uint64_t h = FNV0;
        switch (e) {
        case 0: h = edit_hash(h, GPU_SC_VS, k_e1_text, bit, NULL); h = edit_hash(h, GPU_SC_PS, k_e1_text, bit, NULL); break;
        case 1: h = edit_hash(h, GPU_SC_PS, k_e2_text, bit, NULL); break;
        case 2: h = edit_hash(h, GPU_SC_VS, k_e3_text, bit, NULL); break;
        case 3: h = edit_hash(h, GPU_SC_VS, k_e4_guest, bit, NULL); h = edit_hash(h, GPU_SC_PS, k_e4_two, bit, &g_hdr); break;
        default: h = edit_hash(h, GPU_SC_PS, k_e5_text, bit, NULL); break;
        }
        const Golden* g = NULL;
        for (size_t k = 0; k < sizeof k_golden / sizeof k_golden[0]; k++)
            if (k_golden[k].edit == bit && k_golden[k].version == versions[e]) g = &k_golden[k];
        CHECK(g != NULL, "no golden for E%d version %d (its output hashes to 0x%016llx)", e + 1, versions[e],
              (unsigned long long)h);
        CHECK(!g || g->hash == h,
              "E%d's output changed (0x%016llx, the golden 0x%016llx): raise GPU_SPV_E%d_VERSION and add its golden",
              e + 1, (unsigned long long)h, (unsigned long long)(g ? g->hash : 0), e + 1);
    }
    /* Every edit on its own text compiles. */
    const struct { gpu_sc_stage st; const char* t; const char* body; } whole[] = {
        { GPU_SC_VS, k_e3_text, "float4 main(uint id : SV_VertexID) : SV_Position { return float4(f(id), 0, 0, 1); }\n" },
        { GPU_SC_VS, k_e4_guest, "float4 main() : SV_Position { return float4(f(), 0, 0, 1); }\n" },
        { GPU_SC_PS, k_e5_text, "float4 main(float2 uv : TEXCOORD0) : SV_Target { return f(uv); }\n" },
        { GPU_SC_PS, k_e2_text, "float4 main(float4 x : TEXCOORD0) : SV_Target { return f(x, x, x.x, x.y, x, x); }\n" },
    };
    for (size_t k = 0; k < sizeof whole / sizeof whole[0]; k++) {
        char* t = (char*)malloc(strlen(whole[k].t) + strlen(whole[k].body) + 1);
        strcpy(t, whole[k].t);
        strcat(t, whole[k].body);
        Job j;
        memset(&j, 0, sizeof j);
        j.stage = whole[k].st; j.text = t; j.n = strlen(t); j.name = "golden";
        char what[48];
        snprintf(what, sizeof what, "golden text %u", (unsigned)k);
        check_job(&j, what);
        free(t);
    }
}

/* ---- the built-in passes -----------------------------------------------------
 * Their texts: rsx_outfilter.h, rsx_fxaa.h and rsx_smaa.h as the core has
 * them; the blit's, the HDR blit's and the write-back downscale's copied from
 * rsxcore (rsxc_hdr.inc blit_init and hdr_blit_ready, rsxc_writeback.inc
 * down_init). Their layouts: the root signatures today's renderer makes for
 * them (of_ready, blit_init, hdr_blit_ready, down_init, aa_ready, smaa_ready). */
static const char k_blit_vs[] =
    "struct O { float4 p : SV_POSITION; float2 uv : TEXCOORD; };\n"
    "O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";
static const char k_blit_ps[] =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "float4 main(float4 p : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {\n"
    "  return float4(t.Sample(s, uv).rgb, 1); }\n";
#define HDR_MAP_HLSL \
    "cbuffer HDR : register(b1) { float hw_white; float hw_ratio; float hw_knee; float hw_srgb;\n" \
    "                             float hw_use_t; float3 hw_pad; };\n" \
    "Texture2D<float4> hw_t : register(t1);\n" \
    "SamplerState hw_s : register(s0);\n" \
    "// The frame's colour (with what T holds above 1) to scRGB: decoded, white at\n" \
    "// paper white, and brighter values rolled off towards the peak above a knee.\n" \
    "float3 hw_map(float3 c, float2 uv) {\n" \
    "    if (hw_use_t > 0.5) c += max(hw_t.Sample(hw_s, uv).rgb - 1.0, 0.0);\n" \
    "    c = max(c, 0.0);\n" \
    "    float3 l = hw_srgb > 0.5 ? (c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)) : pow(c, 2.2);\n" \
    "    float m = max(l.r, max(l.g, l.b));\n" \
    "    if (m > hw_knee) {\n" \
    "        float r = hw_ratio - hw_knee;\n" \
    "        float m2 = r > 0.0 ? hw_knee + r * (1.0 - exp((hw_knee - m) / r)) : hw_ratio;\n" \
    "        l *= m2 / m;\n" \
    "    }\n" \
    "    return l * hw_white;\n" \
    "}\n"
static const char k_hdr_ps[] =
    "Texture2D t : register(t0);\n" HDR_MAP_HLSL
    "float4 main(float4 p : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {\n"
    "  return float4(hw_map(t.Sample(hw_s, uv).rgb, uv), 1); }\n";
static const char k_sharp_tail[] =
    "#undef main\n" HDR_MAP_HLSL
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  return float4(hw_map(of_main(pos).rgb, (pos.xy - vp_org) * scale / src_size), 1); }\n";
static const char k_down_vs[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 t = float2((id << 1) & 2, id & 2);\n"
    "    return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";
static const char k_down_ps[] =
    "Texture2D<float4> src : register(t0);\n"
    "cbuffer K : register(b0) { float2 ratio; };\n"
    "float4 main(float4 p : SV_Position) : SV_Target {\n"
    "    return src.Load(int3(int2(p.xy * ratio), 0));\n"
    "}\n";
static const char k_down_ps_argb[] =
    "Texture2D<float4> src : register(t0);\n"
    "cbuffer K : register(b0) { float2 ratio; };\n"
    "float4 main(float4 p : SV_Position) : SV_Target {\n"
    "    return src.Load(int3(int2(p.xy * ratio), 0)).argb;\n"
    "}\n";

static gpu_layout_slot lslot(uint8_t kind, uint8_t reg, uint8_t count, uint8_t vis)
{
    gpu_layout_slot s;
    s.kind = kind; s.reg = reg; s.count = count; s.visibility = vis;
    return s;
}
static void lstatic(gpu_layout_desc* l, uint32_t reg)
{
    gpu_static_sampler* s = &l->statics[l->nstatic++];
    memset(s, 0, sizeof *s);
    s->reg = reg;
    s->visibility = GPU_VIS_PIXEL;
}
static void layouts_init(void)
{
    memset(&g_of, 0, sizeof g_of); memset(&g_blit, 0, sizeof g_blit); memset(&g_hdr, 0, sizeof g_hdr);
    memset(&g_down, 0, sizeof g_down); memset(&g_aa, 0, sizeof g_aa); memset(&g_smaa, 0, sizeof g_smaa);
    g_of.nslots = 2;
    g_of.slot[0] = lslot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
    g_of.slot[1] = lslot(GPU_SLOT_CONST32, 0, 8, GPU_VIS_PIXEL);
    g_blit.nslots = 1;
    g_blit.slot[0] = lslot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
    lstatic(&g_blit, 0);
    g_hdr.nslots = 3;
    g_hdr.slot[0] = lslot(GPU_SLOT_TABLE_TEX, 0, 2, GPU_VIS_PIXEL);
    g_hdr.slot[1] = lslot(GPU_SLOT_CONST32, 0, 8, GPU_VIS_PIXEL);
    g_hdr.slot[2] = lslot(GPU_SLOT_CONST32, 1, 8, GPU_VIS_PIXEL);
    lstatic(&g_hdr, 0);
    g_down.nslots = 2;
    g_down.slot[0] = lslot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
    g_down.slot[1] = lslot(GPU_SLOT_CONST32, 0, 2, GPU_VIS_PIXEL);
    g_aa.nslots = 2;
    g_aa.slot[0] = lslot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
    g_aa.slot[1] = lslot(GPU_SLOT_CONST32, 0, 4, GPU_VIS_PIXEL);
    lstatic(&g_aa, 0);
    g_smaa.nslots = 2;
    g_smaa.slot[0] = lslot(GPU_SLOT_TABLE_TEX, 0, 3, GPU_VIS_PIXEL);
    g_smaa.slot[1] = lslot(GPU_SLOT_CONST32, 0, 4, GPU_VIS_ALL);
    lstatic(&g_smaa, 0);
    lstatic(&g_smaa, 1);
}

static void pair_check(const char* what, const JobResult* vs, const JobResult* ps)
{
    snprintf(g_ctx, sizeof g_ctx, "%s", what);
    CHECK((ps->in_locs & ~vs->out_locs) == 0, "the pixel shader reads locations %llx the stage before does not write (%llx)",
          (unsigned long long)ps->in_locs, (unsigned long long)vs->out_locs);
}

static void test_builtins(void)
{
    static const PushWant of[] = { { "vp_org", 0 }, { "scale", 8 }, { "src_size", 16 }, { NULL, 0 } };
    static const PushWant hdr[] = { { "hw_white", 32 }, { "hw_ratio", 36 }, { "hw_knee", 40 }, { "hw_srgb", 44 },
                                    { "hw_use_t", 48 }, { NULL, 0 } };
    static const PushWant sharp[] = { { "vp_org", 0 }, { "scale", 8 }, { "src_size", 16 }, { "hw_white", 32 },
                                      { "hw_ratio", 36 }, { "hw_knee", 40 }, { "hw_srgb", 44 }, { "hw_use_t", 48 },
                                      { NULL, 0 } };
    static const PushWant down[] = { { "ratio", 0 }, { NULL, 0 } };
    static const PushWant aa[] = { { "rcp_size", 0, 0 }, { "size", 8, 1 }, { NULL, 0, 0 } };   /* size: unused */
    static const PushWant smaa[] = { { "smaa_rt_metrics", 0 }, { NULL, 0 } };
    static const PushWant none[] = { { NULL, 0 } };
    const unsigned NC = GPU_SC_NO_CACHE, OPT = GPU_SC_OPTIMIZE;
    char* sharp_text = (char*)malloc(sizeof rsx_outfilter_ps + sizeof k_sharp_tail + 64);
    snprintf(sharp_text, sizeof rsx_outfilter_ps + sizeof k_sharp_tail + 64, "#define main of_main\n%s\n%s",
             rsx_outfilter_ps, k_sharp_tail);
    Job j;
#define JOB(st, t, len, nm, en, lay, fl, pw, req)                                                     \
    (memset(&j, 0, sizeof j), j.stage = (st), j.text = (t), j.n = (len), j.name = (nm), j.entry = (en), \
     j.layout = (lay), j.flags = (fl), j.push = (pw), j.push_required = (req), &j)
    const JobResult of_vs = check_job(JOB(GPU_SC_VS, rsx_outfilter_vs, sizeof rsx_outfilter_vs - 1, "rsx_outfilter_vs",
                                          NULL, &g_of, OPT, none, 0), "of_ready vs");
    const JobResult of_ps = check_job(JOB(GPU_SC_PS, rsx_outfilter_ps, sizeof rsx_outfilter_ps - 1, "rsx_outfilter_ps",
                                          NULL, &g_of, OPT, of, 1), "of_ready ps");
    pair_check("of_ready", &of_vs, &of_ps);
    const JobResult bl_vs = check_job(JOB(GPU_SC_VS, k_blit_vs, sizeof k_blit_vs - 1, "blit_vs", NULL, &g_blit, 0, none, 0),
                                      "blit vs");
    const JobResult bl_ps = check_job(JOB(GPU_SC_PS, k_blit_ps, sizeof k_blit_ps - 1, "blit_ps", NULL, &g_blit, 0, none, 0),
                                      "blit ps");
    pair_check("blit", &bl_vs, &bl_ps);
    const JobResult hd_vs = check_job(JOB(GPU_SC_VS, k_blit_vs, sizeof k_blit_vs - 1, "hdr_blit_vs", NULL, &g_hdr, 0, none, 0),
                                      "hdr_blit vs");
    const JobResult hd_ps = check_job(JOB(GPU_SC_PS, k_hdr_ps, sizeof k_hdr_ps - 1, "hdr_blit_ps", NULL, &g_hdr, 0, hdr, 1),
                                      "hdr_blit ps");
    pair_check("hdr_blit", &hd_vs, &hd_ps);
    const JobResult hs_vs = check_job(JOB(GPU_SC_VS, rsx_outfilter_vs, sizeof rsx_outfilter_vs - 1, "rsx_outfilter_vs",
                                          NULL, &g_hdr, OPT, none, 0), "hdr_blit sharp vs");
    const JobResult hs_ps = check_job(JOB(GPU_SC_PS, sharp_text, strlen(sharp_text), "hdr_sharp_ps", NULL, &g_hdr, OPT,
                                          sharp, 1), "hdr_blit sharp ps");
    pair_check("hdr_blit sharp", &hs_vs, &hs_ps);
    const JobResult dn_vs = check_job(JOB(GPU_SC_VS, k_down_vs, sizeof k_down_vs - 1, "down_vs", NULL, &g_down, 0, none, 0),
                                      "down vs");
    const JobResult dn_ps = check_job(JOB(GPU_SC_PS, k_down_ps, sizeof k_down_ps - 1, "down_ps", NULL, &g_down, 0, down, 1),
                                      "down ps");
    const JobResult da_ps = check_job(JOB(GPU_SC_PS, k_down_ps_argb, sizeof k_down_ps_argb - 1, "down_ps_argb", NULL,
                                          &g_down, 0, down, 1), "down ps_argb");
    pair_check("down", &dn_vs, &dn_ps);
    pair_check("down argb", &dn_vs, &da_ps);
    const JobResult aa_vs = check_job(JOB(GPU_SC_VS, rsx_fxaa_vs, sizeof rsx_fxaa_vs - 1, "rsx_fxaa_vs", NULL, &g_aa, OPT,
                                          none, 0), "aa vs");
    const JobResult aa_ps = check_job(JOB(GPU_SC_PS, rsx_fxaa_ps, sizeof rsx_fxaa_ps - 1, "rsx_fxaa_ps", NULL, &g_aa, OPT,
                                          aa, 1), "aa ps");
    pair_check("aa", &aa_vs, &aa_ps);
    {
        const size_t nh = sizeof rsx_smaa_head - 1, ns = sizeof rsx_smaa_src - 1, nt = sizeof rsx_smaa_tail - 1;
        char* text = (char*)malloc(nh + ns + nt + 1);
        memcpy(text, rsx_smaa_head, nh);
        memcpy(text + nh, rsx_smaa_src, ns);
        memcpy(text + nh + ns, rsx_smaa_tail, nt);
        text[nh + ns + nt] = 0;
        static const char* const entry[6] = { "smaa_edge_vs", "smaa_edge_ps", "smaa_weight_vs", "smaa_weight_ps",
                                              "smaa_blend_vs", "smaa_blend_ps" };
        JobResult res[6];
        for (int i = 0; i < 6; i++)
            res[i] = check_job(JOB((i & 1) ? GPU_SC_PS : GPU_SC_VS, text, nh + ns + nt, "rsx_smaa", entry[i], &g_smaa,
                                   NC | OPT, smaa, 0), entry[i]);
        for (int i = 0; i < 6; i += 2) pair_check(entry[i], &res[i], &res[i + 1]);
        free(text);
    }
#undef JOB
    free(sharp_text);
}

/* ---- the generated programs --------------------------------------------------- */
#define TEXT_CAP (256u * 1024u)
static char* g_text;
static char* g_sample_vs;
static char* g_sample_ps;
static unsigned g_gen_texts, g_gen_fail;
static double g_gen_cpu;

/* Generated texts the decompiler writes wrong, which FXC refuses too, by key
 * K. rsx_vp_decompiler.c builds a scalar op's right-hand side in a 256-byte
 * buffer; LIT's four copies of a long operand (abs of an indexed constant,
 * swizzled) do not fit, and the text ends "0.0,)". On Windows the test
 * checks with FXC that each one it meets is refused there too. */
static const uint64_t k_bad_texts[] = {
    0xb06af6e3f5d525d5ull,   /* vp 19, kind 0 (the default seed) */
    0x168830dab2fa4bf7ull,   /* vp 19, kind 2 */
};
static unsigned g_gen_skipped;

static JobResult guest_job(gpu_sc_stage st, const char* text, const char* what)
{
    Job j;
    memset(&j, 0, sizeof j);
    j.stage = st;
    j.text = text;
    j.n = strlen(text);
    j.name = st == GPU_SC_PS ? "guest_fp" : st == GPU_SC_VS ? "guest_vp2" : "uvclamp_gs";
    j.guest = 1;
    const unsigned long f0 = g_fail;
    JobResult r;
    run_job(&j, &r);
    if (!r.compiled) {
        const uint64_t K = gpu_sc_key(text, j.n, gpu_sc_profile(GPU_SC_SPV, st));
        int listed = 0;
        for (size_t k = 0; k < sizeof k_bad_texts / sizeof k_bad_texts[0]; k++) listed |= k_bad_texts[k] == K;
        int fxc = -1;   /* 1 compiles it, 0 refuses it, -1 not built here */
        if (gpu_sc_available(GPU_SC_DXBC)) {
            gpu_sc_blob b;
            fxc = gpu_sc_compile(GPU_SC_DXBC, st, text, j.n, "check", GPU_SC_NO_CACHE, &b, NULL, 0) == GPU_SC_OK;
            gpu_sc_blob_release(&b);
        }
        if (listed || fxc == 0) {
            snprintf(g_ctx, sizeof g_ctx, "%s", what);
            CHECK(fxc != 1, "K %016llx is listed as a text FXC refuses, and FXC compiles it", (unsigned long long)K);
            CHECK(listed, "FXC refuses it too: add 0x%016llxull to k_bad_texts", (unsigned long long)K);
            g_gen_skipped++;
            g_gen_texts++;
            if (g_dump) dump_job(&j, what);
            return r;
        }
    }
    report_job(&j, &r, what);
    g_gen_texts++;
    g_gen_cpu += r.cpu_edit_glslang;
    if (g_fail != f0) g_gen_fail++;
    return r;
}

static u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }

static void test_generator(uint64_t seed, int nfp, int nvp)
{
    u8* uc = (u8*)malloc(64 * 1024);
    char what[96];
    size_t gl = 0;
    const char* gs = rsxc_uvclamp_gs_hlsl(&gl);
    char* gs_text = (char*)malloc(gl + 1);
    memcpy(gs_text, gs, gl);
    gs_text[gl] = 0;
    const JobResult gsr = guest_job(GPU_SC_GS, gs_text, "uvclamp gs");
    free(gs_text);
    for (int i = 0; i < nfp; i++) {
        Rng r = rng_for(seed, 1, (uint64_t)i);
        const u32 len = gen_fp(&r, uc, 64 * 1024);
        if (!len) continue;
        for (int kind = 1; kind <= 3; kind += 2) {
            const int exp32 = (int)rn(&r, 2);
            const u32 cube = chance(&r, 3) ? (u32)rnext(&r) & 0xFFFFu : 0u;
            const u32 shadow = chance(&r, 4) ? ((u32)rnext(&r) & 0xFFFFu) << 16 : 0u;
            const u32 d2c = chance(&r, 4) ? (u32)rnext(&r) & 0xFFFFu : 0u;
            const int ni = rsxc_shader_hlsl(kind, uc, len, 0, exp32, cube | shadow, d2c, g_text, TEXT_CAP);
            if (ni <= 0) continue;
            snprintf(what, sizeof what, "fp %d kind %d", i, kind);
            const JobResult pr = guest_job(GPU_SC_PS, g_text, what);
            if (kind == 3) pair_check("uvclamp gs -> fp", &gsr, &pr);
            if (!g_sample_ps && kind == 1 && strstr(g_text, ".Sample") && strlen(g_text) < 64 * 1024)
                g_sample_ps = dup_str(g_text);
            if (kind == 1 && (i % 3) == 0) {
                static const u32 hv[4] = { 0, 0, 0, 0x80000000u | 0xFFFFu };
                u32 hdrv = hv[i % 4];
                if (!hdrv) hdrv = f2u((i & 1) ? 64.0f : 1000.5f);
                if (rsxc_shader_hdr(g_text, TEXT_CAP, hdrv) > 0) {
                    snprintf(what, sizeof what, "fp %d hdr %08x", i, hdrv);
                    guest_job(GPU_SC_PS, g_text, what);
                }
            }
        }
    }
    static u32 w[4 * 200];
    static u8 be[16 * 200];
    for (int i = 0; i < nvp; i++) {
        Rng r = rng_for(seed, 2, (uint64_t)i);
        const u32 n = gen_vp(&r, w, 200, 0, !chance(&r, 40));
        for (u32 k = 0; k < n * 4; k++) put_be(be + 4 * k, w[k]);
        for (int kind = 0; kind <= 2; kind += 2) {
            const int ni = rsxc_shader_hlsl(kind, be, n * 16, 0, (int)(i & 1), 0, 0, g_text, TEXT_CAP);
            if (ni <= 0) continue;
            snprintf(what, sizeof what, "vp %d kind %d", i, kind);
            guest_job(GPU_SC_VS, g_text, what);
            if (!g_sample_vs && kind == 2 && strlen(g_text) < 64 * 1024) g_sample_vs = dup_str(g_text);
        }
    }
    (void)put_le;
    free(uc);
}

/* ---- what the target refuses ---------------------------------------------------- */
static void expect_refused(gpu_sc_stage st, const char* text, const gpu_layout_desc* L, const char* says,
                           const char* what)
{
    gpu_sc_blob b;
    char err[512];
    snprintf(g_ctx, sizeof g_ctx, "%s", what);
    const int r = gpu_sc_compile_layout(GPU_SC_SPV, st, text, strlen(text), "refused", NULL, L, GPU_SC_NO_CACHE, &b,
                                        err, sizeof err);
    CHECK(r == GPU_SC_FAILED, "compiled (%d), where it should refuse", r);
    CHECK(r != GPU_SC_FAILED || strstr(err, says), "the message '%s' does not say '%s'", err, says);
    gpu_sc_blob_release(&b);
}

static void test_refusals(void)
{
    expect_refused(GPU_SC_PS,
                   "Texture2D a : register(t5); SamplerState s : register(s0);\n"
                   "float4 main(float2 uv : TEXCOORD0) : SV_Target { return a.Sample(s, uv); }\n",
                   &g_blit, "is not in the layout", "a texture the layout lacks");
    expect_refused(GPU_SC_PS,
                   "Texture2D a : register(t0); SamplerState s : register(s3);\n"
                   "float4 main(float2 uv : TEXCOORD0) : SV_Target { return a.Sample(s, uv); }\n",
                   &g_blit, "is not in the layout", "a sampler the layout lacks");
    expect_refused(GPU_SC_PS,
                   "cbuffer P : register(b0) { float a; float3 v; };\n"
                   "float4 main() : SV_Target { return float4(v, a); }\n",
                   &g_of, "std430", "E4: a vector std430 would move");
    expect_refused(GPU_SC_PS,
                   "cbuffer P : register(b0) { float2 v[2]; };\n"
                   "float4 main() : SV_Target { return float4(v[0], v[1]); }\n",
                   &g_of, "std430", "E4: an array of float2");
    expect_refused(GPU_SC_PS,
                   "cbuffer P : register(b0) { float4 a, b, c; };\n"
                   "float4 main() : SV_Target { return a + b + c; }\n",
                   &g_of, "slot", "E4: more than the slot holds");
    expect_refused(GPU_SC_PS,
                   "cbuffer P : register(b0) { float4x4 m; };\n"
                   "float4 main() : SV_Target { return m[0]; }\n",
                   &g_of, "E4", "E4: a matrix");
    expect_refused(GPU_SC_PS,
                   "struct S { float4 v; }; ConstantBuffer<S> c : register(b0);\n"
                   "float4 main() : SV_Target { return c.v; }\n",
                   &g_of, "CONST32", "a CONST32 register in a form E4 does not take");
    expect_refused(GPU_SC_PS,
                   "Texture2D a[2] : register(t0); SamplerState s : register(s0);\n"
                   "float4 main(float2 uv : TEXCOORD0, uint i : TEXCOORD1) : SV_Target { return a[i].Sample(s, uv); }\n",
                   &g_hdr, "array", "an array of textures");
    expect_refused(GPU_SC_PS,
                   "float4 k; float4 main() : SV_Target { return k; }\n",
                   &g_blit, "$Global", "a constant outside any cbuffer (D3D's $Globals, b0)");
}

/* ---- the cache ----------------------------------------------------------------- */
static void test_cache(void)
{
    char path[4200], err[512];
    gpu_sc_blob a, b;
    const char* texts[2] = { g_sample_vs, g_sample_ps };
    const gpu_sc_stage stages[2] = { GPU_SC_VS, GPU_SC_PS };
    gpu_sc_perf(1);
    for (int i = 0; i < 2; i++) {
        snprintf(g_ctx, sizeof g_ctx, "cache %s", i ? "ps" : "vs");
        const char* t = texts[i];
        CHECK(t != NULL, "no sample text");
        if (!t) continue;
        const size_t n = strlen(t);
        const gpu_sc_stage st = stages[i];
        CHECK(gpu_sc_cache_path(GPU_SC_SPV, st, t, n, path, sizeof path), "no cache path");
        remove(path);
        const uint64_t K = gpu_sc_key(t, n, gpu_sc_profile(GPU_SC_SPV, st));
        const uint64_t T = gpu_spv_key(K, NULL);
        CHECK(T == gpu_spv_key(K, gpu_spv_guest_layout()), "the guest layout's key is not the default's");
        char want[4200];
        snprintf(want, sizeof want, "%s/gpu/spv/%s_%016llx.spv", gpu_sc_cache_dir(), i ? "ps" : "vs", (unsigned long long)T);
        CHECK(!strcmp(path, want), "the path %s, not %s", path, want);
        gpu_sc_stats s0;
        gpu_sc_stats_take(&s0, 1);
        int r = gpu_sc_compile(GPU_SC_SPV, st, t, n, "sample", 0, &a, err, sizeof err);
        CHECK(r == GPU_SC_OK, "compile: %s", err);
        CHECK(file_exists(path), "no file after a compile");
        /* the file: the header, then glslang's module */
        size_t fl = 0;
        char* f = read_file(path, &fl);
        CHECK(f && fl > 24, "no file");
        if (f && fl > 24) {
            const uint8_t* h = (const uint8_t*)f;
            uint32_t v[5];
            for (int k = 0; k < 5; k++)
                v[k] = (uint32_t)h[4 + 4 * k] | ((uint32_t)h[5 + 4 * k] << 8) | ((uint32_t)h[6 + 4 * k] << 16) |
                       ((uint32_t)h[7 + 4 * k] << 24);
            CHECK(!memcmp(f, "GSPV", 4) && v[0] == 1u, "the header's magic and version");
            CHECK(v[1] == (uint32_t)T && v[2] == (uint32_t)(T >> 32), "the header's key");
            CHECK(v[3] == fl - 24 && v[4] == 0u, "the header's size and reserved word");
            size_t tl = 0;
            char* et = gpu_spv_edit(st, t, n, GPU_SPV_EDITS_ALL, NULL, &tl, err, sizeof err);
            uint32_t* w = NULL;
            size_t nw = 0;
            CHECK(et && gpu_spv_glslang(st, et, tl, "sample", NULL, 0, gpu_spv_guest_layout(), &w, &nw, err,
                                        sizeof err) == GPU_SC_OK, "glslang: %s", err);
            CHECK(w && nw * 4 == fl - 24 && !memcmp(f + 24, w, nw * 4), "the file is not glslang's module");
            free(w);
            free(et);
        }
        /* load by key, and a compile that is a hit: the same bytes */
        r = gpu_sc_load(GPU_SC_SPV, st, K, &b);
        CHECK(r == GPU_SC_OK && b.size == a.size && !memcmp(a.data, b.data, a.size), "load: %d", r);
        gpu_sc_blob_release(&b);
        r = gpu_sc_compile(GPU_SC_SPV, st, t, n, "sample", 0, &b, err, sizeof err);
        CHECK(r == GPU_SC_OK && b.size == a.size && !memcmp(a.data, b.data, a.size), "a hit: %d", r);
        gpu_sc_blob_release(&b);
        gpu_sc_stats s1;
        gpu_sc_stats_take(&s1, 1);
        CHECK(s1.cache_hits == 1 && s1.compiles == 1, "stats: %d hits, %d compiles", s1.cache_hits, s1.compiles);
        /* a file that is not a whole one for T reads as not cached, and a compile replaces it */
        for (int c = 0; f && fl > 24 && c < 7; c++) {
            char* g = (char*)malloc(fl);
            memcpy(g, f, fl);
            size_t gl = fl;
            switch (c) {
            case 0: g[0] = 'X'; break;           /* magic */
            case 1: g[4] = 2; break;             /* version */
            case 2: g[8] ^= 1; break;            /* key, low word */
            case 3: g[15] ^= 0x80; break;        /* key, high word */
            case 4: g[16] += 4; break;           /* size */
            case 5: g[20] = 1; break;            /* reserved */
            default: gl -= 4; break;             /* short */
            }
            write_file(path, g, gl);
            free(g);
            snprintf(g_ctx, sizeof g_ctx, "cache %s corrupt %d", i ? "ps" : "vs", c);
            CHECK(gpu_sc_load(GPU_SC_SPV, st, K, &b) == GPU_SC_NOT_CACHED, "read as cached");
            gpu_sc_blob_release(&b);
            r = gpu_sc_compile(GPU_SC_SPV, st, t, n, "sample", 0, &b, err, sizeof err);
            CHECK(r == GPU_SC_OK && b.size == a.size && !memcmp(a.data, b.data, a.size), "the compile after it");
            gpu_sc_blob_release(&b);
            r = gpu_sc_load(GPU_SC_SPV, st, K, &b);
            CHECK(r == GPU_SC_OK, "the file was not replaced");
            gpu_sc_blob_release(&b);
        }
        free(f);
        /* setup's pass */
        snprintf(g_ctx, sizeof g_ctx, "cache %s put", i ? "ps" : "vs");
        remove(path);
        CHECK(gpu_sc_put(GPU_SC_SPV, st, t) == 2, "put: not compiled");
        CHECK(file_exists(path), "put: no file");
        CHECK(gpu_sc_put(GPU_SC_SPV, st, t) == 1, "put: not there");
        r = gpu_sc_load(GPU_SC_SPV, st, K, &b);
        CHECK(r == GPU_SC_OK && b.size == a.size && !memcmp(a.data, b.data, a.size), "put: not the compile's module");
        gpu_sc_blob_release(&b);
        gpu_sc_blob_release(&a);
        remove(path);
    }
    snprintf(g_ctx, sizeof g_ctx, "cache misc");
    CHECK(gpu_sc_put(GPU_SC_SPV, GPU_SC_GS, "x") == 0, "put of a geometry shader");
    CHECK(gpu_sc_compile(GPU_SC_SPV, GPU_SC_PS, "x", 1, "x", GPU_SC_OPTIMIZE, &a, NULL, 0) == GPU_SC_BAD_ARG,
          "OPTIMIZE with the cache");
    CHECK(gpu_sc_compile_entry(GPU_SC_SPV, GPU_SC_PS, "x", 1, "x", "other", 0, &a, NULL, 0) == GPU_SC_BAD_ARG,
          "an entry point with the cache");
    /* a layout's own key */
    {
        const uint64_t K = gpu_sc_key(k_hdr_ps, sizeof k_hdr_ps - 1, "ps_5_0");
        const uint64_t T = gpu_spv_key(K, &g_hdr);
        CHECK(T != gpu_spv_key(K, NULL), "the HDR layout's key is the guest's");
        snprintf(path, sizeof path, "%s/gpu/spv/ps_%016llx.spv", gpu_sc_cache_dir(), (unsigned long long)T);
        remove(path);
        const int r = gpu_sc_compile_layout(GPU_SC_SPV, GPU_SC_PS, k_hdr_ps, sizeof k_hdr_ps - 1, "hdr_blit_ps", NULL,
                                            &g_hdr, 0, &a, err, sizeof err);
        CHECK(r == GPU_SC_OK, "compile: %s", err);
        CHECK(file_exists(path), "no file under the layout's key");
        CHECK(gpu_sc_load(GPU_SC_SPV, GPU_SC_PS, K, &b) == GPU_SC_NOT_CACHED, "the guest key reads the layout's file");
        gpu_sc_blob_release(&a);
        gpu_sc_blob_release(&b);
        remove(path);
        char tag[320];
        gpu_spv_tag(NULL, tag, sizeof tag);
        if (g_verbose) printf("  tag: %s\n", tag);
        CHECK(strstr(tag, gpu_spv_compiler()) && strstr(tag, ";edits=") && strstr(tag, ";layout="), "tag %s", tag);
        /* Without the fetch clamp E3 is left out, and its files are other ones. */
        const unsigned was = gpu_sc_spv_options();
        gpu_sc_spv_set_options(was | GPU_SC_SPV_FETCH_CLAMP);
        const uint64_t with = gpu_spv_key(K, NULL);
        CHECK(gpu_spv_edits() == GPU_SPV_EDITS_ALL, "the edits with the fetch clamp");
        gpu_sc_spv_set_options(was & ~GPU_SC_SPV_FETCH_CLAMP);
        char tag0[320], ed0[64];
        gpu_spv_tag(NULL, tag0, sizeof tag0);
        snprintf(ed0, sizeof ed0, ";edits=%d.%d.0.%d.%d;", GPU_SPV_E1_VERSION, GPU_SPV_E2_VERSION, GPU_SPV_E4_VERSION,
                 GPU_SPV_E5_VERSION);
        CHECK(gpu_spv_edits() == (GPU_SPV_EDITS_ALL & ~GPU_SPV_E3) && strstr(tag0, ed0) && strcmp(tag0, tag) &&
              gpu_spv_key(K, NULL) != with, "the tag without the fetch clamp: %s", tag0);
        gpu_sc_spv_set_options(was);
    }
    gpu_sc_perf(0);
}

/* ---- the corpus ----------------------------------------------------------------- */
typedef struct {
    char** names;
    int n;
    const char* dir;
    volatile long next;
    JobResult* res;
    double* wall;
    int* key_ok;
    int* stage;
} Corpus;

static long next_index(volatile long* p)
{
#ifdef _WIN32
    return InterlockedIncrement(p) - 1;
#else
    return __atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST);
#endif
}

static const char* g_spv_out;   /* --spv-out DIR */

static void corpus_one(Corpus* c, int i)
{
    char path[4200], out[4200];
    snprintf(path, sizeof path, "%s/%s", c->dir, c->names[i]);
    size_t n = 0;
    char* t = read_file(path, &n);
    const char* nm = c->names[i];
    const gpu_sc_stage st = !strncmp(nm, "vs_", 3) ? GPU_SC_VS : !strncmp(nm, "gs_", 3) ? GPU_SC_GS : GPU_SC_PS;
    c->stage[i] = (int)st;
    if (!t) { jr_fail(&c->res[i], "unreadable"); return; }
    const uint64_t key = strtoull(nm + 7, NULL, 16);
    c->key_ok[i] = gpu_sc_key(t, n, gpu_sc_profile(GPU_SC_SPV, st)) == key;
    Job j;
    memset(&j, 0, sizeof j);
    j.stage = st;
    j.text = t;
    j.n = n;
    j.name = st == GPU_SC_PS ? "guest_fp" : "guest_vp2";
    j.guest = 1;
    if (g_spv_out) {
        snprintf(out, sizeof out, "%s/%.*s.spv", g_spv_out, (int)(strlen(nm) - 10), nm);
        j.spv_out = out;
    }
    const double w0 = wall_now();
    run_job(&j, &c->res[i]);
    c->wall[i] = wall_now() - w0;
    free(t);
}

#ifdef _WIN32
static DWORD WINAPI corpus_worker(LPVOID p)
#else
static void* corpus_worker(void* p)
#endif
{
    Corpus* c = (Corpus*)p;
    for (;;) {
        const long i = next_index(&c->next);
        if (i >= c->n) break;
        corpus_one(c, (int)i);
    }
    return 0;
}

static int cmp_str(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
static int cmp_dbl(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}
static double pct(double* v, int n, double q)
{
    if (n <= 0) return 0.0;
    qsort(v, (size_t)n, sizeof *v, cmp_dbl);
    return v[(int)((n - 1) * q + 0.5)];
}

static int list_corpus(const char* dir, char*** out)
{
    int n = 0, cap = 0;
    char** v = NULL;
#ifdef _WIN32
    char pat[4200];
    snprintf(pat, sizeof pat, "%s\\*.dxbc.hlsl", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (n == cap) { cap = cap ? cap * 2 : 1024; v = (char**)realloc(v, (size_t)cap * sizeof *v); }
            v[n++] = dup_str(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR* d = opendir(dir);
    struct dirent* e;
    while (d && (e = readdir(d)) != NULL) {
        const size_t l = strlen(e->d_name);
        if (l < 11 || strcmp(e->d_name + l - 10, ".dxbc.hlsl")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 1024; v = (char**)realloc(v, (size_t)cap * sizeof *v); }
        v[n++] = dup_str(e->d_name);
    }
    if (d) closedir(d);
#endif
    if (n) qsort(v, (size_t)n, sizeof *v, cmp_str);
    *out = v;
    return n;
}

static int run_corpus(const char* dir, const char* tsv, int threads)
{
    Corpus c;
    memset(&c, 0, sizeof c);
    c.dir = dir;
    c.n = list_corpus(dir, &c.names);
    printf("corpus: %d texts in %s, %d thread%s\n", c.n, dir, threads, threads == 1 ? "" : "s");
    if (!c.n) return 1;
    c.res = (JobResult*)calloc((size_t)c.n, sizeof *c.res);
    c.wall = (double*)calloc((size_t)c.n, sizeof *c.wall);
    c.key_ok = (int*)calloc((size_t)c.n, sizeof *c.key_ok);
    c.stage = (int*)calloc((size_t)c.n, sizeof *c.stage);
    const double w0 = wall_now();
    if (threads < 1) threads = 1;
    if (threads > 16) threads = 16;
#ifdef _WIN32
    HANDLE th[16];
    for (int i = 0; i < threads; i++) th[i] = CreateThread(NULL, 0, corpus_worker, &c, 0, NULL);
    WaitForMultipleObjects((DWORD)threads, th, TRUE, INFINITE);
    for (int i = 0; i < threads; i++) CloseHandle(th[i]);
#else
    pthread_t th[16];
    for (int i = 0; i < threads; i++) pthread_create(&th[i], NULL, corpus_worker, &c);
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
#endif
    const double wall = wall_now() - w0;
    FILE* out = tsv ? fopen(tsv, "w") : NULL;
    if (out)
        fprintf(out, "file\tstage\tkey_ok\tcompiled\tvalid\trefl\tdet\tpost\tproblems\twords\t"
                     "cpu_edit_glslang_ms\tcpu_post_ms\tcpu_validate_ms\twall_ms\tmessage\n");
    int counts[3][8];
    memset(counts, 0, sizeof counts);
    double* v1[3];
    double* v2[3];
    int nv[3] = { 0, 0, 0 };
    for (int s = 0; s < 3; s++) { v1[s] = (double*)malloc(sizeof(double) * (size_t)c.n); v2[s] = (double*)malloc(sizeof(double) * (size_t)c.n); }
    int shown = 0;
    for (int i = 0; i < c.n; i++) {
        const JobResult* r = &c.res[i];
        const int s = c.stage[i];
        counts[s][0]++;
        counts[s][1] += !c.key_ok[i];
        counts[s][2] += !r->compiled;
        counts[s][3] += r->compiled && !r->valid;
        counts[s][4] += r->compiled && !r->refl;
        counts[s][5] += r->compiled && !r->det;
        counts[s][6] += r->compiled && !r->post;
        counts[s][7] += r->problems != 0;
        if (r->compiled) {
            v1[s][nv[s]] = r->cpu_edit_glslang * 1e3;
            v2[s][nv[s]] = (r->cpu_post + r->cpu_validate) * 1e3;
            nv[s]++;
        }
        if (r->problems && shown < 20) { printf("  %s: %s\n", c.names[i], r->msg); shown++; }
        if (out)
            fprintf(out, "%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%u\t%.3f\t%.3f\t%.3f\t%.3f\t%s\n", c.names[i],
                    s == 0 ? "vs" : s == 1 ? "ps" : "gs", c.key_ok[i], r->compiled, r->valid, r->refl, r->det, r->post,
                    r->problems, r->words, r->cpu_edit_glslang * 1e3, r->cpu_post * 1e3, r->cpu_validate * 1e3,
                    c.wall[i] * 1e3, r->msg);
    }
    if (out) fclose(out);
    int bad = 0;
    printf("stage  texts  key!=  compile-fail  spirv-val-fail  refl-fail  det-fail  post-fail  any | "
           "CPU edit+glslang p50 p95 max (ms) | post+validate p50 p95 (ms)\n");
    for (int s = 0; s < 3; s++) {
        if (!counts[s][0]) continue;
        const double mx = nv[s] ? pct(v1[s], nv[s], 1.0) : 0.0;
        const double p50 = pct(v1[s], nv[s], 0.5), p95 = pct(v1[s], nv[s], 0.95);
        const double q50 = pct(v2[s], nv[s], 0.5), q95 = pct(v2[s], nv[s], 0.95);
        printf("%-5s  %5d  %5d  %12d  %14d  %9d  %8d  %9d  %3d | %8.1f %6.1f %6.1f | %6.2f %6.2f\n",
               s == 0 ? "vs" : s == 1 ? "ps" : "gs", counts[s][0], counts[s][1], counts[s][2], counts[s][3],
               counts[s][4], counts[s][5], counts[s][6], counts[s][7], p50, p95, mx, q50, q95);
        bad += counts[s][7] + counts[s][1];
    }
    printf("wall %.1f s\n", wall);
    for (int s = 0; s < 3; s++) { free(v1[s]); free(v2[s]); }
    for (int i = 0; i < c.n; i++) free(c.names[i]);
    free(c.names); free(c.res); free(c.wall); free(c.key_ok); free(c.stage);
    return bad ? 1 : 0;
}

int main(int argc, char** argv)
{
    uint64_t seed = 0x5350565345454431ull;
    int nfp = 60, nvp = 60, threads = 1;
    const char* corpus = NULL;
    const char* tsv = NULL;
    const char* cache = "test_gpu_sc_spv_cache";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fp") && i + 1 < argc) nfp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vp") && i + 1 < argc) nvp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
        else if (!strcmp(argv[i], "--corpus") && i + 1 < argc) corpus = argv[++i];
        else if (!strcmp(argv[i], "--tsv") && i + 1 < argc) tsv = argv[++i];
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose")) g_verbose = 1;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) g_dump = argv[++i];
        else if (!strcmp(argv[i], "--spv-out") && i + 1 < argc) g_spv_out = argv[++i];
        else { printf("unknown argument %s\n", argv[i]); return 2; }
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    cpu_init();
    if (corpus) set_env("PS3_SHADER_CACHE", "0");
    else { make_dir(cache); set_env("PS3_SHADER_CACHE", cache); }

    snprintf(g_ctx, sizeof g_ctx, "service");
    CHECK(gpu_sc_available(GPU_SC_SPV), "spv is not available");
    CHECK(!strcmp(gpu_sc_profile(GPU_SC_SPV, GPU_SC_VS), "vs_5_0") && !strcmp(gpu_sc_profile(GPU_SC_SPV, GPU_SC_PS), "ps_5_0") &&
          !strcmp(gpu_sc_profile(GPU_SC_SPV, GPU_SC_GS), "gs_5_0"), "the profiles");
    CHECK(gpu_sc_spv_options() == GPU_SC_SPV_DEFAULT, "the default options");
    gpu_sc_spv_set_options(GPU_SC_SPV_ALL);
    CHECK(gpu_sc_spv_options() == GPU_SC_SPV_ALL, "set_options");
    printf("spv: %s\n", gpu_spv_compiler());
    if (corpus) return run_corpus(corpus, tsv, threads);

    g_text = (char*)malloc(TEXT_CAP);
    const double t0 = wall_now();
    layouts_init();
    test_goldens();
    test_builtins();
    test_refusals();
    test_generator(seed, nfp, nvp);
    printf("generated: %u texts, %u failing, %u the decompiler writes wrong (FXC refuses them too); "
           "CPU edit+glslang %.1f ms a text\n", g_gen_texts, g_gen_fail, g_gen_skipped,
           g_gen_texts ? g_gen_cpu * 1e3 / g_gen_texts : 0.0);
    test_cache();
    printf("%lu checks, %lu failed, %.1f s\n", g_checks, g_fail, wall_now() - t0);
    free(g_text);
    free(g_sample_vs);
    free(g_sample_ps);
    return g_fail ? 1 : 0;
}
