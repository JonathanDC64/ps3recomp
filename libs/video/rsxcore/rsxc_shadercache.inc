/* Build (or fetch) a PSO pairing the current decompiled VS with the guest's
 * FRAGMENT program at fp_addr: read the FP ucode from guest memory, decompile
 * to HLSL (rsx_fp_decompiler), compile, and cache. This replaces the two
 * hardcoded pixel shaders for draws whose FP we can translate -- e.g.
 * gcm/cube's plasma FP `c = tex2D(t0, uv).x; out = (c, 0, c, 1)`. */
#include "rsx_fp_decompiler.h"
#include "rsxc_shader.h"
#include "../gpu/gpu_shader.h"

/* The core's shaders (step C3a). Their texts are rsxc_shader.c's, and every
 * compile goes through the instance file's rc_be_sc_compile (step C8b). On
 * D3D12 that is the compile service (gpu_shader.c), whose dxbc target is
 * today's FXC call and .dxbc cache: the same keys, files and knobs
 * (PS3_SHADER_CACHE, PS3_SHADER_CACHE_HLSL, PS3_SHADER_CACHE_WARM), so this
 * path and today's renderer share the cache; a backend without cache files
 * (rc_be_cache_files 0: null) has no cache folder, so none of the files
 * named after it is read or written. The service keeps PERF=1's compile
 * counters (fxc, disk) while perf_on() is on; each compile sets its switch
 * from perf_on(), where the renderer read it. This fragment comes right
 * after the state, before the first one that compiles. */
static double s_perf_cpso = 0.0;           /* CreateGraphicsPipelineState, guest-FP PSOs */
static int    s_perf_cpso_n = 0;
static double s_perf_pmiss = 0.0;          /* whole vp_get_fp_pso calls that missed */
static const char* shader_cache_dir(void) { return rc_be_cache_files ? gpu_sc_cache_dir() : ""; }
static void shader_cache_warm(void) { if (rc_be_cache_files) gpu_sc_warm(); }

/* A compile for the backend (rc_be_sc_compile; on D3D12 the service's dxbc
 * target): 1 with the code in *out (the caller's to release,
 * gpu_sc_blob_release), else 0 with the compiler's message for sc_msg.
 * entry NULL is "main". layout is the one the shader is bound with, NULL for
 * the guest programs' (vp_layout's): the spv target places its resources by
 * it (step VC1); dxbc's registers are the root signature's, so D3D12 does
 * not use it. */
static __declspec(thread) char t_sc_msg[8192];
static int sc_compile(gpu_sc_stage stage, const char* src, size_t n, const char* name,
                      const char* entry, unsigned flags, const gpu_layout_desc* layout, gpu_sc_blob* out)
{
    gpu_sc_perf(perf_on());
    return rc_be_sc_compile(stage, src, n, name, entry, flags, layout, out, t_sc_msg, sizeof t_sc_msg);
}
/* The message of this thread's last compile that failed, or `none` when the
 * compiler gave none. */
static const char* sc_msg(const char* none) { return t_sc_msg[0] ? t_sc_msg : none; }

/* Step C3b: the shaders are the GPU layer's. A compiled program becomes a
 * gpu_shader (its bytecode copied; the service's blob released), which
 * pipelines are made from; sc_shader_drop frees it where today's code
 * released its blob, once its pipelines are made. none on failure. */
static gpu_shader sc_shader_from(gpu_sc_stage stage, gpu_sc_blob* b)
{
    gpu_shader_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.stage = stage == GPU_SC_PS ? GPU_STAGE_PS : stage == GPU_SC_GS ? GPU_STAGE_GS : GPU_STAGE_VS;
    sd.code = rc_be_shader_code;   /* (the instance file's: GPU_CODE_*) */
    sd.bytes = b->data;
    sd.size = b->size;
    const gpu_shader s = gpu_shader_create(s_rc_dev, &sd);
    gpu_sc_blob_release(b);
    return s;
}
/* A built-in pass's shader, for the layout it is bound with (step VC1). */
static gpu_shader sc_shader_for(const gpu_layout_desc* layout, gpu_sc_stage stage, const char* src, size_t n,
                                const char* name, const char* entry, unsigned flags)
{
    gpu_sc_blob b;
    if (!sc_compile(stage, src, n, name, entry, flags, layout, &b)) { const gpu_shader none = {0}; return none; }
    return sc_shader_from(stage, &b);
}
/* A shader for the guest programs' layout. */
static gpu_shader sc_shader(gpu_sc_stage stage, const char* src, size_t n, const char* name,
                            const char* entry, unsigned flags)
{
    return sc_shader_for(NULL, stage, src, n, name, entry, flags);
}
static void sc_shader_drop(gpu_shader* s)
{
    if (GPU_VALID(*s)) gpu_shader_destroy(s_rc_dev, *s);
    s->id = 0;
}

/* A guest program through the cache, by its profile. */
static gpu_shader shader_compile_cached(const char* src, size_t n, const char* name, const char* profile)
{
    return sc_shader(strcmp(profile, "ps_5_0") ? GPU_SC_VS : GPU_SC_PS, src, n, name, NULL, 0);
}

/* The program the cache holds under key K, or none (the pipeline index's
 * build: its rows name programs by key). */
static gpu_shader sc_load(gpu_sc_stage stage, u64 key)
{
    gpu_sc_blob b;
    if (!rc_be_sc_load(stage, key, &b)) { const gpu_shader none = {0}; return none; }
    return sc_shader_from(stage, &b);
}

/* Step C3b: every pipeline is the GPU layer's, from its library when that
 * has it (its own file: pso_lib_open). The core keeps its handle (step C8a);
 * pipelines live as long as the device, so it is never released. None (.v
 * 0) on failure; *how (may be NULL) says whether it came from the library
 * (GPU_PIPELINE_LOADED) or was compiled and stored in it (_STORED, step
 * C3c). Thread-safe. */
static gpu_pipeline rc_pipeline(const gpu_pipeline_desc* d, uint32_t* how)
{
    uint32_t h = GPU_PIPELINE_FAILED;
    const gpu_pipeline p = gpu_pipeline_create(s_rc_dev, d, 0, &h);
    if (p.v) InterlockedIncrement(h == GPU_PIPELINE_LOADED ? &s_rc_pipe_loaded : &s_rc_pipe_made);
    if (how) *how = p.v ? h : GPU_PIPELINE_FAILED;
    return p;
}

/* A built-in pass's pipeline (the blit, the output filter, the HDR blit, the
 * write-back's downscale, FXAA, SMAA): a full-screen triangle without vertex
 * input or culling, into one colour target of format fmt written with mask,
 * no depth target, blending off; everything else 0, as today's zeroed
 * descriptions leave it. depth_clip as each sets it. */
static void pass_desc(gpu_pipeline_desc* d, gpu_layout layout, gpu_shader vs, gpu_shader ps,
                      gpu_format fmt, u8 mask, int depth_clip)
{
    memset(d, 0, sizeof *d);   /* zeroed whole: the GPU layer names the pipeline by its bytes */
    d->layout = layout;
    d->vs = vs;
    d->ps = ps;
    d->topology_type = GPU_TOPOLOGY_TYPE_TRIANGLE;
    d->cull = GPU_CULL_NONE;
    d->depth_clip = depth_clip ? 1 : 0;
    d->blend.write_mask = mask;
    d->nrt = 1;
    d->rt_format[0] = fmt;
    d->samples = 1;
    d->sample_mask = UINT_MAX;
}

/* A layout slot, and the passes' static sampler at s<reg>: filter, clamped
 * on every axis, every mip, for the pixel shader (today's zeroed
 * static sampler description with those set). */
static gpu_layout_slot lay_slot(u8 kind, u8 reg, u8 count, u8 vis)
{
    gpu_layout_slot s;
    s.kind = kind; s.reg = reg; s.count = count; s.visibility = vis;
    return s;
}
static gpu_static_sampler pass_sampler(u32 filter, u32 reg)
{
    gpu_static_sampler s;
    memset(&s, 0, sizeof s);
    s.desc.filter = filter;
    s.desc.address_u = s.desc.address_v = s.desc.address_w = GPU_ADDRESS_CLAMP;
    s.desc.max_lod = GPU_LOD_MAX;
    s.reg = reg;
    s.visibility = GPU_VIS_PIXEL;
    return s;
}
/* A layout (not GPU_VALID on failure). */
static gpu_layout lay_make(const gpu_layout_desc* ld)
{
    return gpu_layout_create(s_rc_dev, ld);
}
