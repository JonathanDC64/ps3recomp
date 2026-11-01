/*
 * ps3recomp - the shader compile service (portable renderer): gpu_shader.h
 *
 * The dxbc target is today's D3D12 renderer's shader disk cache, its
 * read-ahead and FXC call, copied from rsx_d3d12_backend.c line for line
 * (copy_manifest.txt): the same key, file name, header checks and store, so
 * its files are today's. The spv target (PS3RECOMP_RSX_HAL_SPV) has its own
 * files under <cache>/gpu/spv/ and its compiler in gpu_shader_spv*.cpp; the
 * dxbc target's code and files do not change with it. dxil, metallib and
 * msl are declared and not available yet.
 */
#if defined(PS3RECOMP_RSX_HAL)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/ps3_knob.h"
#include "ps3emu/ps3types.h"
#include "gpu_shader.h"
#if defined(PS3RECOMP_RSX_HAL_SPV)
#define GPU_SC_HAVE_SPV 1
#include "gpu_shader_spv.h"
#endif

static u64 shader_key(const char* src, size_t n, const char* profile)
{
    u64 h = 14695981039346656037ull;
    for (const char* p = profile; *p; p++) { h ^= (u8)*p; h *= 1099511628211ull; }
    h ^= 0xFF; h *= 1099511628211ull;
    for (size_t i = 0; i < n; i++) { h ^= (u8)src[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t gpu_sc_key(const char* src, size_t n, const char* profile)
{
    return src && profile ? shader_key(src, n, profile) : 0;
}

/* PERF=1's counters, as the renderer's (gpu_sc_perf turns them on). */
static volatile long s_sc_perf;
void gpu_sc_perf(int on) { s_sc_perf = on != 0; }

#if defined(_WIN32) || defined(PS3RECOMP_RSX_D3D12_VKD3D)
#define GPU_SC_HAVE_DXBC 1

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#else
#include "../rsx_vkd3d_host.h"      /* vkd3d's D3DCompile, the Win32 calls */
#endif

/* Paths are joined with '/' on every host (Windows takes it too); one that
 * comes from the host may hold either separator. */
static char* rsx_path_last_sep(char* path)
{
    char* fwd = strrchr(path, '/');
    char* back = strrchr(path, '\\');
    if (!fwd) return back;
    if (!back) return fwd;
    return fwd > back ? fwd : back;
}


static double perf_now(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static int perf_on(void) { return s_sc_perf != 0; }

/* Compiled guest shaders on disk, so a program FXC has seen once never costs a
 * frame again. FXC takes ~100 ms for one of this title's large vertex
 * programs; a new area used to stall on dozens of them (PERF=1 "vs compile").
 * Keyed by the HLSL text itself plus the target profile, so any decompiler
 * change or diagnostic patch (VP_BYPASS, FP_SUB, ...) simply misses: there is
 * nothing to invalidate. One file per program:
 *   <dir>/<profile>_<fnv64 of profile+hlsl>.dxbc
 * <dir> is PS3_SHADER_CACHE, default "shader_cache" next to the executable;
 * PS3_SHADER_CACHE=0 turns the cache off. */
static double s_perf_fxc = 0.0;            /* time inside D3DCompile proper */
static int    s_perf_fxc_n = 0, s_perf_sc_hits = 0;
static const char* shader_cache_dir(void)
{
    static char dir[MAX_PATH]; static int init = 0;
    if (!init) {
        init = 1;
        const char* e = getenv("PS3_SHADER_CACHE");
        if (e && !strcmp(e, "0")) dir[0] = 0;
        else if (e && *e) snprintf(dir, sizeof dir, "%s", e);
        else {
            char exe[MAX_PATH];
            DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
            char* slash = (n && n < sizeof exe) ? rsx_path_last_sep(exe) : NULL;
            if (slash) { *slash = 0; snprintf(dir, sizeof dir, "%s/shader_cache", exe); }
            else snprintf(dir, sizeof dir, "shader_cache");
        }
        if (dir[0]) CreateDirectoryA(dir, NULL);
    }
    return dir;
}
/* A cached file is used only if it is a whole DXBC container: the header's
 * total-size field (offset 24) must match the file length. */
static ID3DBlob* shader_cache_load(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    ID3DBlob* b = NULL;
    if (n > 32 && SUCCEEDED(D3DCreateBlob((SIZE_T)n, &b))) {
        u8* p = (u8*)b->lpVtbl->GetBufferPointer(b);
        if (fread(p, 1, (size_t)n, f) != (size_t)n || memcmp(p, "DXBC", 4) ||
            ((u32)p[24] | ((u32)p[25] << 8) | ((u32)p[26] << 16) | ((u32)p[27] << 24)) != (u32)n) {
            b->lpVtbl->Release(b); b = NULL;
        }
    }
    fclose(f);
    return b;
}
/* Written to a temporary name and renamed, so a crash or a second process
 * never leaves a half-written program under the real name. */
static void shader_cache_store(const char* path, ID3DBlob* b)
{
    char tmp[MAX_PATH + 32];
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentThreadId());
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    const size_t n = b->lpVtbl->GetBufferSize(b);
    const int ok = fwrite(b->lpVtbl->GetBufferPointer(b), 1, n, f) == n;
    if (fclose(f) || !ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
}
static HRESULT shader_compile_cached(const char* src, size_t n, const char* name,
                                     const char* profile, ID3DBlob** out, ID3DBlob** err)
{
    char path[MAX_PATH + 64]; path[0] = 0;
    const char* dir = shader_cache_dir();
    if (dir[0]) {
        snprintf(path, sizeof path, "%s/%s_%016llx.dxbc", dir, profile,
                 (unsigned long long)shader_key(src, n, profile));
        if ((*out = shader_cache_load(path)) != NULL) { if (perf_on()) s_perf_sc_hits++; return S_OK; }
    }
    const double t0 = perf_on() ? perf_now() : 0.0;
    HRESULT hr = D3DCompile(src, n, name, NULL, NULL, "main", profile, 0, 0, out, err);
    if (perf_on()) { s_perf_fxc += perf_now() - t0; s_perf_fxc_n++; }
    if (SUCCEEDED(hr) && *out && path[0]) {
        shader_cache_store(path, *out);
        /* PS3_SHADER_CACHE_HLSL=1: keep the source beside each program, to
         * see why a precompiled entry and a draw's lookup differ. */
        static int keep = -1;
        PS3_KNOB(keep, ps3_env_int("PS3_SHADER_CACHE_HLSL", 0));
        if (keep) {
            char hp[MAX_PATH + 72];
            snprintf(hp, sizeof hp, "%s.hlsl", path);
            FILE* f = fopen(hp, "wb");
            if (f) { fwrite(src, 1, n, f); fclose(f); }
        }
    }
    return hr;
}

/* PS3_SHADER_CACHE_WARM=<n> (default 4; 0 = off): at startup, n threads read
 * every program in the shader cache once, in the background. Windows'
 * antivirus scans a file the first time it is opened after it was written:
 * about 3 ms for one of these, against 0.07 ms after, and the scan is
 * remembered across runs. Setup's shader pass writes some 3,600 just before
 * a first start, so a first walk opened each for the first time when a draw
 * needed it, on the render thread for a pixel program and the FIFO thread
 * for a vertex program: 520 ms of lookups for 189 new pipelines in a
 * Tutorial walk, up to 54 ms for one. On four threads the read takes about
 * 2 s while the game boots. Off Windows there is no such scan, and nothing
 * is read ahead. */
#ifdef _WIN32
static struct {
    char (*names)[64];
    LONG n;
    volatile LONG next, live;
    int threads;
    double t0;
    char dir[MAX_PATH];
} s_scw;
static void shader_cache_warm_files(void)
{
    char path[MAX_PATH + 80];
    u8 buf[16 * 1024];   /* (the programs are a few KB) */
    for (LONG i; (i = InterlockedIncrement(&s_scw.next) - 1) < s_scw.n; ) {
        snprintf(path, sizeof path, "%s/%s", s_scw.dir, s_scw.names[i]);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        while (fread(buf, 1, sizeof buf, f) == sizeof buf) {}
        fclose(f);
    }
    if (InterlockedDecrement(&s_scw.live) == 0) {
        printf("[D3D12] shader cache: %ld files read ahead in %.0f ms on %d threads (PS3_SHADER_CACHE_WARM)\n",
               (long)s_scw.n, 1000.0 * (perf_now() - s_scw.t0), s_scw.threads);
        free(s_scw.names);
        s_scw.names = NULL;
    }
}
static DWORD WINAPI shader_cache_warm_worker(LPVOID unused)
{
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    shader_cache_warm_files();
    return 0;
}
/* The first thread lists the files, starts the others and reads too. */
static DWORD WINAPI shader_cache_warm_lister(LPVOID unused)
{
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    char pat[MAX_PATH + 16];
    snprintf(pat, sizeof pat, "%s/*.dxbc", s_scw.dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LONG cap = 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || strlen(fd.cFileName) >= 64) continue;
        if (s_scw.n == cap) {
            const LONG c = cap ? cap * 2 : 4096;
            char (*g)[64] = (char (*)[64])realloc(s_scw.names, (size_t)c * 64);
            if (!g) break;
            s_scw.names = g; cap = c;
        }
        memcpy(s_scw.names[s_scw.n++], fd.cFileName, strlen(fd.cFileName) + 1);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (!s_scw.n) { free(s_scw.names); s_scw.names = NULL; return 0; }
    s_scw.t0 = perf_now();
    s_scw.live = s_scw.threads;
    for (int t = 1; t < s_scw.threads; t++) {
        HANDLE w = CreateThread(NULL, 0, shader_cache_warm_worker, NULL, 0, NULL);
        if (w) CloseHandle(w); else InterlockedDecrement(&s_scw.live);
    }
    shader_cache_warm_files();
    return 0;
}
static void shader_cache_warm(void)
{
    static int once = 0;
    const char* dir = shader_cache_dir();
    int nt = ps3_env_int("PS3_SHADER_CACHE_WARM", 4);
    if (once++ || !dir[0] || nt <= 0) return;
    s_scw.threads = nt > 16 ? 16 : nt;
    snprintf(s_scw.dir, sizeof s_scw.dir, "%s", dir);
    HANDLE h = CreateThread(NULL, 0, shader_cache_warm_lister, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
#else
static void shader_cache_warm(void) {}
#endif

void gpu_sc_warm(void) { shader_cache_warm(); }

static const char* const s_dxbc_profile[GPU_SC_STAGE_COUNT] = { "vs_5_0", "ps_5_0", "gs_5_0" };

const char* gpu_sc_cache_dir(void) { return shader_cache_dir(); }

static void spv_stats_take(gpu_sc_stats* out, int reset);

void gpu_sc_stats_take(gpu_sc_stats* out, int reset)
{
    if (out) { out->compile_s = s_perf_fxc; out->compiles = s_perf_fxc_n; out->cache_hits = s_perf_sc_hits; }
    if (reset) { s_perf_fxc = 0.0; s_perf_fxc_n = 0; s_perf_sc_hits = 0; }
    spv_stats_take(out, reset);
}

#else /* no dxbc here */

#if defined(GPU_SC_HAVE_SPV)
#include <sys/stat.h>
#include <unistd.h>
/* Where only spv is built (Linux): the cache directory as the dxbc target's,
 * PS3_SHADER_CACHE or shader_cache beside the executable (0: no cache). */
static const char* shader_cache_dir(void)
{
    static char dir[4096]; static int init = 0;
    if (!init) {
        init = 1;
        const char* e = getenv("PS3_SHADER_CACHE");
        if (e && !strcmp(e, "0")) dir[0] = 0;
        else if (e && *e) snprintf(dir, sizeof dir, "%s", e);
        else {
            char exe[4000];
            const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
            char* slash = NULL;
            if (n > 0) { exe[n] = 0; slash = strrchr(exe, '/'); }
            if (slash) { *slash = 0; snprintf(dir, sizeof dir, "%s/shader_cache", exe); }
            else snprintf(dir, sizeof dir, "shader_cache");
        }
        if (dir[0]) mkdir(dir, 0755);
    }
    return dir;
}
const char* gpu_sc_cache_dir(void) { return shader_cache_dir(); }
#else
const char* gpu_sc_cache_dir(void) { return ""; }
#endif

void gpu_sc_warm(void) {}

static void spv_stats_take(gpu_sc_stats* out, int reset);

void gpu_sc_stats_take(gpu_sc_stats* out, int reset)
{
    if (out) { out->compile_s = 0.0; out->compiles = 0; out->cache_hits = 0; }
    spv_stats_take(out, reset);
}

#endif /* _WIN32 || PS3RECOMP_RSX_D3D12_VKD3D */

#if defined(GPU_SC_HAVE_SPV)
/* ---- the spv target (gpu_shader_spv.h) -------------------------------------
 * The cache holds glslang's module for a text after the edits, with its
 * layout's sets and bindings: one file per text and layout,
 *   <cache>/gpu/spv/<vs|ps|gs>_<T>.spv
 * T = gpu_spv_key(K, layout), K the text's dxbc key (gpu_sc_key over the
 * dxbc profile), behind a 24-byte header the loader checks: "GSPV", version
 * 1, T (low word, high word), the module's size in bytes and 0, each a
 * little-endian u32. The post-passes run on every module handed out, by the
 * options of the moment, so one file serves every device; only
 * GPU_SC_SPV_FETCH_CLAMP, an edit, has files of its own. Debug builds (and
 * RSX_SPV_VALIDATE=1) validate each one. gpu_sc_load, gpu_sc_put and
 * gpu_sc_cache_path are the guest layout's. */
#ifndef _WIN32
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#endif

#define SPV_HDR 24u

static volatile unsigned s_spv_opts = GPU_SC_SPV_DEFAULT;
static double s_spv_s;
static int s_spv_n, s_spv_hits;
static const char* const s_k_profile[GPU_SC_STAGE_COUNT] = { "vs_5_0", "ps_5_0", "gs_5_0" };
static const char* const s_spv_stage[GPU_SC_STAGE_COUNT] = { "vs", "ps", "gs" };

void gpu_sc_spv_set_options(unsigned opts) { s_spv_opts = opts & GPU_SC_SPV_ALL; }

unsigned gpu_sc_spv_options(void)
{
    static int nocontract = -1;
    PS3_KNOB(nocontract, ps3_env_int("RSX_SPV_NOCONTRACT", 1));
    return nocontract ? s_spv_opts : (s_spv_opts & ~GPU_SC_SPV_NO_CONTRACT);
}

unsigned gpu_spv_edits(void)
{
    return (s_spv_opts & GPU_SC_SPV_FETCH_CLAMP) ? GPU_SPV_EDITS_ALL : (GPU_SPV_EDITS_ALL & ~GPU_SPV_E3);
}

static void spv_stats_take(gpu_sc_stats* out, int reset)
{
    if (out) { out->compile_s += s_spv_s; out->compiles += s_spv_n; out->cache_hits += s_spv_hits; }
    if (reset) { s_spv_s = 0.0; s_spv_n = 0; s_spv_hits = 0; }
}

static double spv_now(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static void spv_mkdir(const char* path)
{
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    mkdir(path, 0755);
#endif
}

/* The file a stage's module with key T is stored under; 0 when there is no
 * cache. Makes <cache>/gpu/spv the first time. */
static int spv_path(gpu_sc_stage stage, u64 t, char* path, size_t cap)
{
    static volatile int made = 0;
    const char* root = gpu_sc_cache_dir();
    if (!root[0] || !path || cap < 32) return 0;
    if (!made) {
        snprintf(path, cap, "%s/gpu", root);
        spv_mkdir(path);
        snprintf(path, cap, "%s/gpu/spv", root);
        spv_mkdir(path);
        made = 1;
    }
    snprintf(path, cap, "%s/gpu/spv/%s_%016llx.spv", root, s_spv_stage[stage], (unsigned long long)t);
    return 1;
}

static u32 spv_le32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void spv_put32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

/* A cached module, if the file is a whole one for T: the header's magic,
 * version and key, its size the rest of the file, and SPIR-V's magic. */
static uint32_t* spv_file_load(const char* path, u64 t, size_t* nwords)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t* w = NULL;
    u8 h[SPV_HDR];
    if (n > (long)SPV_HDR + 20 && fread(h, 1, SPV_HDR, f) == SPV_HDR && !memcmp(h, "GSPV", 4) &&
        spv_le32(h + 4) == 1u && spv_le32(h + 8) == (u32)t && spv_le32(h + 12) == (u32)(t >> 32) &&
        spv_le32(h + 16) == (u32)(n - (long)SPV_HDR) && spv_le32(h + 20) == 0u && ((n - (long)SPV_HDR) & 3) == 0) {
        const size_t size = (size_t)(n - (long)SPV_HDR);
        w = (uint32_t*)malloc(size);
        if (w && (fread(w, 1, size, f) != size || w[0] != 0x07230203u)) { free(w); w = NULL; }
        if (w) *nwords = size / 4;
    }
    fclose(f);
    return w;
}

/* Written to a temporary name and renamed, as the dxbc files are. */
static void spv_file_store(const char* path, u64 t, const uint32_t* w, size_t nwords)
{
    char tmp[4200];
#ifdef _WIN32
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentThreadId());
#else
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)pthread_self());
#endif
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    const size_t size = nwords * 4;
    u8 h[SPV_HDR];
    memcpy(h, "GSPV", 4);
    spv_put32(h + 4, 1u);
    spv_put32(h + 8, (u32)t);
    spv_put32(h + 12, (u32)(t >> 32));
    spv_put32(h + 16, (u32)size);
    spv_put32(h + 20, 0u);
    const int ok = fwrite(h, 1, SPV_HDR, f) == SPV_HDR && fwrite(w, 1, size, f) == size;
#ifdef _WIN32
    if (fclose(f) || !ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
#else
    if (fclose(f) || !ok || rename(tmp, path)) remove(tmp);
#endif
}

static int spv_exists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int spv_validate_on(void)
{
#ifndef NDEBUG
    return 1;
#else
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("RSX_SPV_VALIDATE", 0));
    return on != 0;
#endif
}

/* A module from the cache or glslang, through the post-passes into out. */
static int spv_hand_out(gpu_sc_stage stage, const uint32_t* raw, size_t nraw, gpu_sc_blob* out,
                        char* err, size_t err_cap)
{
    uint32_t* w = NULL; size_t n = 0;
    if (gpu_spv_post(stage, raw, nraw, gpu_sc_spv_options(), &w, &n) != GPU_SC_OK) {
        if (err && err_cap) snprintf(err, err_cap, "spv: malformed module");
        return GPU_SC_FAILED;
    }
    if (spv_validate_on()) {
        char msg[1024];
        if (!gpu_spv_validate(w, n, msg, sizeof msg)) {
            static int said = 0;
            if (said < 8 && ++said) printf("[GPU] spv: %s\n", msg);
            if (err && err_cap) snprintf(err, err_cap, "%s", msg);
            free(w);
            return GPU_SC_FAILED;
        }
    }
    out->impl = w;            /* (impl == data: one malloc, see gpu_sc_blob_release) */
    out->data = w;
    out->size = n * 4;
    return GPU_SC_OK;
}

/* The edits and glslang; *text_out the edited text (malloc'd) when asked. */
static int spv_compile(gpu_sc_stage stage, const char* src, size_t n, const char* name, const char* entry,
                       const gpu_layout_desc* layout, unsigned flags, uint32_t** w, size_t* nw, char** text_out,
                       char* err, size_t err_cap)
{
    size_t tl = 0;
    if (!layout) layout = gpu_spv_guest_layout();
    char* text = gpu_spv_edit(stage, src, n, gpu_spv_edits(), layout, &tl, err, err_cap);
    if (!text) {
        if (err && err_cap && !err[0]) snprintf(err, err_cap, "spv: out of memory");
        return GPU_SC_FAILED;
    }
    const int perf = s_sc_perf != 0;
    const double t0 = perf ? spv_now() : 0.0;
    const int r = gpu_spv_glslang(stage, text, tl, name, entry, (flags & GPU_SC_OPTIMIZE) ? GPU_SPV_GL_OPTIMIZE : 0u,
                                  gpu_spv_text_has_bindings(src, n) ? NULL : layout, w, nw, err, err_cap);
    if (perf) { s_spv_s += spv_now() - t0; s_spv_n++; }
    if (text_out && r == GPU_SC_OK) *text_out = text;
    else free(text);
    return r;
}

static int spv_compile_entry(gpu_sc_stage stage, const char* src, size_t n, const char* name, const char* entry,
                             const gpu_layout_desc* layout, unsigned flags, gpu_sc_blob* out, char* err,
                             size_t err_cap)
{
    char path[4200]; path[0] = 0;
    u64 t = 0;
    if (!layout) layout = gpu_spv_guest_layout();
    if (!(flags & GPU_SC_NO_CACHE)) {
        t = gpu_spv_key(shader_key(src, n, s_k_profile[stage]), layout);
        if (!spv_path(stage, t, path, sizeof path)) path[0] = 0;
        size_t nw = 0;
        uint32_t* w = path[0] ? spv_file_load(path, t, &nw) : NULL;
        if (w) {
            if (s_sc_perf) s_spv_hits++;
            const int r = spv_hand_out(stage, w, nw, out, err, err_cap);
            free(w);
            return r;
        }
    }
    uint32_t* w = NULL; size_t nw = 0; char* text = NULL;
    int r = spv_compile(stage, src, n, name, entry, layout, flags, &w, &nw, path[0] ? &text : NULL, err, err_cap);
    if (r != GPU_SC_OK) return r;
    if (path[0]) {
        spv_file_store(path, t, w, nw);
        /* PS3_SHADER_CACHE_HLSL=1: the edited text beside its module. */
        static int keep = -1;
        PS3_KNOB(keep, ps3_env_int("PS3_SHADER_CACHE_HLSL", 0));
        if (keep && text) {
            char hp[4300];
            snprintf(hp, sizeof hp, "%s.hlsl", path);
            FILE* f = fopen(hp, "wb");
            if (f) { fwrite(text, 1, strlen(text), f); fclose(f); }
        }
    }
    free(text);
    r = spv_hand_out(stage, w, nw, out, err, err_cap);
    free(w);
    return r;
}

static int spv_load(gpu_sc_stage stage, u64 key, gpu_sc_blob* out)
{
    char path[4200];
    const u64 t = gpu_spv_key(key, NULL);
    if (!spv_path(stage, t, path, sizeof path)) return GPU_SC_NOT_CACHED;
    size_t nw = 0;
    uint32_t* w = spv_file_load(path, t, &nw);
    if (!w) return GPU_SC_NOT_CACHED;
    const int r = spv_hand_out(stage, w, nw, out, NULL, 0);
    free(w);
    return r;
}

static int spv_put(gpu_sc_stage stage, const char* src)
{
    char path[4200];
    if (!src) return 0;
    const size_t n = strlen(src);
    const u64 t = gpu_spv_key(shader_key(src, n, s_k_profile[stage]), NULL);
    if (!spv_path(stage, t, path, sizeof path)) return 0;
    if (spv_exists(path)) return 1;
    uint32_t* w = NULL; size_t nw = 0;
    if (spv_compile(stage, src, n, stage == GPU_SC_PS ? "guest_fp" : "guest_vp2", NULL, NULL, 0, &w, &nw, NULL,
                    NULL, 0) != GPU_SC_OK)
        return 0;
    spv_file_store(path, t, w, nw);
    free(w);
    return 2;
}

#else /* no spv here */

static void spv_stats_take(gpu_sc_stats* out, int reset) { (void)out; (void)reset; }
void gpu_sc_spv_set_options(unsigned opts) { (void)opts; }
unsigned gpu_sc_spv_options(void) { return 0; }

#endif /* GPU_SC_HAVE_SPV */

int gpu_sc_available(gpu_sc_target target)
{
#ifdef GPU_SC_HAVE_DXBC
    if (target == GPU_SC_DXBC) return 1;
#endif
#ifdef GPU_SC_HAVE_SPV
    if (target == GPU_SC_SPV) return 1;
#endif
    (void)target;
    return 0;
}

const char* gpu_sc_profile(gpu_sc_target target, gpu_sc_stage stage)
{
    if ((unsigned)stage >= GPU_SC_STAGE_COUNT || !gpu_sc_available(target)) return NULL;
#ifdef GPU_SC_HAVE_DXBC
    if (target == GPU_SC_DXBC) return s_dxbc_profile[stage];
#endif
#ifdef GPU_SC_HAVE_SPV
    if (target == GPU_SC_SPV) return s_k_profile[stage];
#endif
    return NULL;
}

int gpu_sc_cache_path(gpu_sc_target target, gpu_sc_stage stage, const char* src, size_t n,
                      char* path, size_t cap)
{
    const char* profile = gpu_sc_profile(target, stage);
    const char* dir = gpu_sc_cache_dir();
    if (!profile || !src || !path || !cap || !dir[0]) return 0;
#ifdef GPU_SC_HAVE_SPV
    if (target == GPU_SC_SPV) return spv_path(stage, gpu_spv_key(shader_key(src, n, profile), NULL), path, cap);
#endif
    snprintf(path, cap, "%s/%s_%016llx.dxbc", dir, profile,
             (unsigned long long)shader_key(src, n, profile));
    return 1;
}

int gpu_sc_compile(gpu_sc_target target, gpu_sc_stage stage, const char* src, size_t n,
                   const char* name, unsigned flags, gpu_sc_blob* out, char* err, size_t err_cap)
{
    return gpu_sc_compile_entry(target, stage, src, n, name, NULL, flags, out, err, err_cap);
}

int gpu_sc_compile_entry(gpu_sc_target target, gpu_sc_stage stage, const char* src, size_t n,
                         const char* name, const char* entry, unsigned flags, gpu_sc_blob* out,
                         char* err, size_t err_cap)
{
    return gpu_sc_compile_layout(target, stage, src, n, name, entry, NULL, flags, out, err, err_cap);
}

int gpu_sc_compile_layout(gpu_sc_target target, gpu_sc_stage stage, const char* src, size_t n,
                          const char* name, const char* entry, const struct gpu_layout_desc* layout,
                          unsigned flags, gpu_sc_blob* out, char* err, size_t err_cap)
{
    if (out) { out->data = NULL; out->size = 0; out->impl = NULL; }
    if (err && err_cap) err[0] = 0;
    if (!src || !out || (unsigned)stage >= GPU_SC_STAGE_COUNT) return GPU_SC_BAD_ARG;
    if (!entry) entry = "main";
    /* The cache's key holds the text and the profile, not the entry or flags. */
    if (!(flags & GPU_SC_NO_CACHE) && ((flags & GPU_SC_OPTIMIZE) || strcmp(entry, "main")))
        return GPU_SC_BAD_ARG;
    if (!gpu_sc_available(target)) return GPU_SC_UNAVAILABLE;
#ifdef GPU_SC_HAVE_SPV
    if (target == GPU_SC_SPV) return spv_compile_entry(stage, src, n, name, entry, layout, flags, out, err, err_cap);
#endif
    (void)layout;   /* dxbc: the registers are the root signature's */
#ifdef GPU_SC_HAVE_DXBC
    const char* profile = s_dxbc_profile[stage];
    const UINT fl = (flags & GPU_SC_OPTIMIZE) ? D3DCOMPILE_OPTIMIZATION_LEVEL3 : 0;
    ID3DBlob* b = NULL; ID3DBlob* e = NULL;
    HRESULT hr = (flags & GPU_SC_NO_CACHE)
        ? D3DCompile(src, n, name, NULL, NULL, entry, profile, fl, 0, &b, &e)
        : shader_compile_cached(src, n, name, profile, &b, &e);
    if (e) {
        if (err && err_cap)
            snprintf(err, err_cap, "%.*s", (int)e->lpVtbl->GetBufferSize(e),
                     (const char*)e->lpVtbl->GetBufferPointer(e));
        e->lpVtbl->Release(e);
    }
    if (FAILED(hr) || !b) {
        if (b) b->lpVtbl->Release(b);
        return GPU_SC_FAILED;
    }
    out->impl = b;
    out->data = b->lpVtbl->GetBufferPointer(b);
    out->size = b->lpVtbl->GetBufferSize(b);
    return GPU_SC_OK;
#else
    (void)n; (void)name; (void)flags;
    return GPU_SC_UNAVAILABLE;
#endif
}

int gpu_sc_load(gpu_sc_target target, gpu_sc_stage stage, uint64_t key, gpu_sc_blob* out)
{
    if (out) { out->data = NULL; out->size = 0; out->impl = NULL; }
    if (!out || (unsigned)stage >= GPU_SC_STAGE_COUNT) return GPU_SC_BAD_ARG;
    if (!gpu_sc_available(target)) return GPU_SC_UNAVAILABLE;
#ifdef GPU_SC_HAVE_SPV
    if (target == GPU_SC_SPV) return spv_load(stage, key, out);
#endif
#ifdef GPU_SC_HAVE_DXBC
    const char* dir = shader_cache_dir();
    if (!dir[0]) return GPU_SC_NOT_CACHED;
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s/%s_%016llx.dxbc", dir, s_dxbc_profile[stage], (unsigned long long)key);
    ID3DBlob* b = shader_cache_load(path);
    if (!b) return GPU_SC_NOT_CACHED;
    out->impl = b;
    out->data = b->lpVtbl->GetBufferPointer(b);
    out->size = b->lpVtbl->GetBufferSize(b);
    return GPU_SC_OK;
#else
    (void)key;
    return GPU_SC_UNAVAILABLE;
#endif
}

void gpu_sc_blob_release(gpu_sc_blob* blob)
{
    if (!blob) return;
    /* An spv module is one malloc, its impl its data; a dxbc one an ID3DBlob,
     * whose buffer is never the object itself. */
    if (blob->impl && blob->impl == blob->data) free(blob->impl);
#ifdef GPU_SC_HAVE_DXBC
    else if (blob->impl) { ID3DBlob* b = (ID3DBlob*)blob->impl; b->lpVtbl->Release(b); }
#endif
    blob->data = NULL; blob->size = 0; blob->impl = NULL;
}

/* rsx_d3d12_shader_cache_put, for any target that has a cache. */
int gpu_sc_put(gpu_sc_target target, gpu_sc_stage stage, const char* src)
{
    if (!gpu_sc_available(target) || (stage != GPU_SC_VS && stage != GPU_SC_PS)) return 0;
#ifdef GPU_SC_HAVE_SPV
    if (target == GPU_SC_SPV) return spv_put(stage, src);
#endif
#ifdef GPU_SC_HAVE_DXBC
    const int fragment = stage == GPU_SC_PS;
    const char* hlsl = src;
    const char* profile = fragment ? "ps_5_0" : "vs_5_0";
    if (!shader_cache_dir()[0] || !hlsl) return 0;
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s/%s_%016llx.dxbc", shader_cache_dir(), profile,
             (unsigned long long)shader_key(hlsl, strlen(hlsl), profile));
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return 1;
    ID3DBlob* b = NULL; ID3DBlob* e = NULL;
    HRESULT hr = shader_compile_cached(hlsl, strlen(hlsl), fragment ? "guest_fp" : "guest_vp2",
                                       profile, &b, &e);
    if (e) e->lpVtbl->Release(e);
    if (b) b->lpVtbl->Release(b);
    return SUCCEEDED(hr) && b ? 2 : 0;
#else
    (void)src;
    return 0;
#endif
}

#else
typedef int gpu_shader_off;   /* (a file with no declaration is not ISO C) */
#endif /* PS3RECOMP_RSX_HAL */
