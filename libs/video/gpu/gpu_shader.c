/*
 * ps3recomp - the shader compile service (portable renderer): gpu_shader.h
 *
 * The dxbc target is today's D3D12 renderer's shader disk cache and FXC call,
 * copied from rsx_d3d12_backend.c line for line (copy_manifest.txt): the same
 * key, file name, header checks and store, so its files are today's. The
 * other targets are declared and not available yet.
 */
#if defined(PS3RECOMP_RSX_HAL)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/ps3_knob.h"
#include "ps3emu/ps3types.h"
#include "gpu_shader.h"

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

static const char* const s_dxbc_profile[GPU_SC_STAGE_COUNT] = { "vs_5_0", "ps_5_0", "gs_5_0" };

const char* gpu_sc_cache_dir(void) { return shader_cache_dir(); }

void gpu_sc_stats_take(gpu_sc_stats* out, int reset)
{
    if (out) { out->compile_s = s_perf_fxc; out->compiles = s_perf_fxc_n; out->cache_hits = s_perf_sc_hits; }
    if (reset) { s_perf_fxc = 0.0; s_perf_fxc_n = 0; s_perf_sc_hits = 0; }
}

#else /* no dxbc here */

const char* gpu_sc_cache_dir(void) { return ""; }

void gpu_sc_stats_take(gpu_sc_stats* out, int reset)
{
    (void)reset;
    if (out) { out->compile_s = 0.0; out->compiles = 0; out->cache_hits = 0; }
}

#endif /* _WIN32 || PS3RECOMP_RSX_D3D12_VKD3D */

int gpu_sc_available(gpu_sc_target target)
{
#ifdef GPU_SC_HAVE_DXBC
    if (target == GPU_SC_DXBC) return 1;
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
    return NULL;
}

int gpu_sc_cache_path(gpu_sc_target target, gpu_sc_stage stage, const char* src, size_t n,
                      char* path, size_t cap)
{
    const char* profile = gpu_sc_profile(target, stage);
    const char* dir = gpu_sc_cache_dir();
    if (!profile || !src || !path || !cap || !dir[0]) return 0;
    snprintf(path, cap, "%s/%s_%016llx.dxbc", dir, profile,
             (unsigned long long)shader_key(src, n, profile));
    return 1;
}

int gpu_sc_compile(gpu_sc_target target, gpu_sc_stage stage, const char* src, size_t n,
                   const char* name, unsigned flags, gpu_sc_blob* out, char* err, size_t err_cap)
{
    if (out) { out->data = NULL; out->size = 0; out->impl = NULL; }
    if (err && err_cap) err[0] = 0;
    if (!src || !out || (unsigned)stage >= GPU_SC_STAGE_COUNT) return GPU_SC_BAD_ARG;
    if (!gpu_sc_available(target)) return GPU_SC_UNAVAILABLE;
#ifdef GPU_SC_HAVE_DXBC
    const char* profile = s_dxbc_profile[stage];
    ID3DBlob* b = NULL; ID3DBlob* e = NULL;
    HRESULT hr = (flags & GPU_SC_NO_CACHE)
        ? D3DCompile(src, n, name, NULL, NULL, "main", profile, 0, 0, &b, &e)
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

void gpu_sc_blob_release(gpu_sc_blob* blob)
{
    if (!blob) return;
#ifdef GPU_SC_HAVE_DXBC
    if (blob->impl) { ID3DBlob* b = (ID3DBlob*)blob->impl; b->lpVtbl->Release(b); }
#endif
    blob->data = NULL; blob->size = 0; blob->impl = NULL;
}

/* rsx_d3d12_shader_cache_put, for any target that has a cache. */
int gpu_sc_put(gpu_sc_target target, gpu_sc_stage stage, const char* src)
{
    if (!gpu_sc_available(target) || (stage != GPU_SC_VS && stage != GPU_SC_PS)) return 0;
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
