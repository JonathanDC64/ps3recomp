/* Build (or fetch) a PSO pairing the current decompiled VS with the guest's
 * FRAGMENT program at fp_addr: read the FP ucode from guest memory, decompile
 * to HLSL (rsx_fp_decompiler), compile, and cache. This replaces the two
 * hardcoded pixel shaders for draws whose FP we can translate -- e.g.
 * gcm/cube's plasma FP `c = tex2D(t0, uv).x; out = (c, 0, c, 1)`. */
#include "rsx_fp_decompiler.h"
#include "rsxc_shader.h"
#include "../gpu/gpu_shader.h"

/* The core's shaders (step C3a). Their texts are rsxc_shader.c's, and every
 * compile goes through the compile service (gpu_shader.c), whose dxbc target
 * is today's FXC call and .dxbc cache: the same keys, files and knobs
 * (PS3_SHADER_CACHE, PS3_SHADER_CACHE_HLSL, PS3_SHADER_CACHE_WARM), so this
 * path and today's renderer share the cache. The service keeps PERF=1's
 * compile counters (fxc, disk) while perf_on() is on; each compile sets its
 * switch from perf_on(), where the renderer read it. This fragment comes
 * right after the state, before the first one that compiles. */
static double s_perf_cpso = 0.0;           /* CreateGraphicsPipelineState, guest-FP PSOs */
static int    s_perf_cpso_n = 0;
static double s_perf_pmiss = 0.0;          /* whole vp_get_fp_pso calls that missed */
static const char* shader_cache_dir(void) { return gpu_sc_cache_dir(); }
static void shader_cache_warm(void) { gpu_sc_warm(); }

/* TEMPORARY, until the pipeline step makes gpu_shader handles: the core's
 * pipeline code takes bytecode as an ID3DBlob, so the service's bytes are
 * copied into one. */
static ID3DBlob* sc_d3d_blob(const void* p, size_t n)
{
    ID3DBlob* b = NULL;
    if (FAILED(D3DCreateBlob((SIZE_T)n, &b)) || !b) return NULL;
    memcpy(b->lpVtbl->GetBufferPointer(b), p, n);
    return b;
}

/* A compile on the service's dxbc target, with D3DCompile's results: the
 * bytecode in *out and, when it fails, the compiler's message in *err (both
 * the caller's to release). entry NULL is "main". */
static HRESULT sc_compile(gpu_sc_stage stage, const char* src, size_t n, const char* name,
                          const char* entry, unsigned flags, ID3DBlob** out, ID3DBlob** err)
{
    char msg[8192];
    gpu_sc_blob b;
    *out = NULL;
    if (err) *err = NULL;
    gpu_sc_perf(perf_on());
    const int r = gpu_sc_compile_entry(GPU_SC_DXBC, stage, src, n, name, entry, flags, &b, msg, sizeof msg);
    if (r == GPU_SC_OK) {
        *out = sc_d3d_blob(b.data, b.size);
        gpu_sc_blob_release(&b);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    if (err && msg[0]) *err = sc_d3d_blob(msg, strlen(msg) + 1);
    return E_FAIL;
}

/* A guest program through the cache, by its profile. */
static HRESULT shader_compile_cached(const char* src, size_t n, const char* name,
                                     const char* profile, ID3DBlob** out, ID3DBlob** err)
{
    return sc_compile(strcmp(profile, "ps_5_0") ? GPU_SC_VS : GPU_SC_PS, src, n, name, NULL, 0, out, err);
}

/* The program the cache holds under key K, or NULL (the pipeline index's
 * build: its rows name programs by key). */
static ID3DBlob* sc_load(gpu_sc_stage stage, u64 key)
{
    gpu_sc_blob b;
    if (gpu_sc_load(GPU_SC_DXBC, stage, key, &b) != GPU_SC_OK) return NULL;
    ID3DBlob* d = sc_d3d_blob(b.data, b.size);
    gpu_sc_blob_release(&b);
    return d;
}
