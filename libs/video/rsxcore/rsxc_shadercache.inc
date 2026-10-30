/* Build (or fetch) a PSO pairing the current decompiled VS with the guest's
 * FRAGMENT program at fp_addr: read the FP ucode from guest memory, decompile
 * to HLSL (rsx_fp_decompiler), compile, and cache. This replaces the two
 * hardcoded pixel shaders for draws whose FP we can translate -- e.g.
 * gcm/cube's plasma FP `c = tex2D(t0, uv).x; out = (c, 0, c, 1)`. */
#include "rsx_fp_decompiler.h"

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
static double s_perf_cpso = 0.0;           /* CreateGraphicsPipelineState, guest-FP PSOs */
static int    s_perf_cpso_n = 0;
static double s_perf_pmiss = 0.0;          /* whole vp_get_fp_pso calls that missed */
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
static u64 shader_key(const char* src, size_t n, const char* profile)
{
    u64 h = 14695981039346656037ull;
    for (const char* p = profile; *p; p++) { h ^= (u8)*p; h *= 1099511628211ull; }
    h ^= 0xFF; h *= 1099511628211ull;
    for (size_t i = 0; i < n; i++) { h ^= (u8)src[i]; h *= 1099511628211ull; }
    return h;
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

