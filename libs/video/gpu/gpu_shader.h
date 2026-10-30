/*
 * ps3recomp - the shader compile service (portable renderer)
 *
 * Turns shader text into the bytecode a GPU backend takes, through a disk
 * cache, with no device: setup's shader pass can use it as well as a
 * running game. One target per bytecode format, each with its own compiler
 * and flags:
 *
 *   dxbc      FXC (D3DCompile), today's D3D12 renderer's exact call: the
 *             files are today's <cache>/<profile>_<key>.dxbc, shared with it.
 *   spv       glslang's HLSL front end, SPIR-V 1.3 for Vulkan 1.1, after the
 *             text edits E1-E5, with the Vulkan backend's binding model and
 *             the post-passes (gpu_shader_spv.h). Files are
 *             <cache>/gpu/spv/<vs|ps|gs>_<T>.spv, T from the text's dxbc
 *             key K, so one text has one K on every target. A vertex
 *             shader flips y (D3D's clip space is y-up, Vulkan's y-down),
 *             so the backend sets D3D's viewport as it is; a geometry
 *             shader passes the position on unflipped, so there is one flip
 *             with it or without it. A pixel shader's SV_Position.w is
 *             D3D's, the reciprocal of Vulkan's FragCoord.w.
 *   dxil, metallib, msl
 *             declared; not available yet.
 *
 * The cache directory is PS3_SHADER_CACHE (default shader_cache beside the
 * executable; 0 turns the cache off), as today's. Built only with
 * PS3RECOMP_RSX_HAL; dxbc only on Windows and on vkd3d, spv with
 * PS3RECOMP_RSX_HAL_SPV (Windows and Linux).
 */
#ifndef PS3RECOMP_GPU_SHADER_H
#define PS3RECOMP_GPU_SHADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum gpu_sc_target {
    GPU_SC_DXBC = 0,
    GPU_SC_SPV,
    GPU_SC_DXIL,
    GPU_SC_METALLIB,
    GPU_SC_MSL,
    GPU_SC_TARGET_COUNT
} gpu_sc_target;

typedef enum gpu_sc_stage {
    GPU_SC_VS = 0,
    GPU_SC_PS,
    GPU_SC_GS,
    GPU_SC_STAGE_COUNT
} gpu_sc_stage;

/* gpu_sc_compile's results. */
enum {
    GPU_SC_OK          = 0,
    GPU_SC_UNAVAILABLE = -1,   /* the target is not built here */
    GPU_SC_FAILED      = -2,   /* the compiler refused the text */
    GPU_SC_BAD_ARG     = -3,
    GPU_SC_NOT_CACHED  = -4    /* gpu_sc_load: no whole program under that key */
};

/* gpu_sc_compile's flags. The cache's key holds the text and the profile
 * only, so GPU_SC_OPTIMIZE needs GPU_SC_NO_CACHE. */
#define GPU_SC_NO_CACHE 1u     /* compile, and neither read nor write the cache */
#define GPU_SC_OPTIMIZE 2u     /* the compiler's highest level (dxbc: D3DCOMPILE_OPTIMIZATION_LEVEL3) */

/* Compiled bytecode. Release with gpu_sc_blob_release. */
typedef struct gpu_sc_blob {
    const void* data;
    size_t      size;
    void*       impl;          /* the target's own object */
} gpu_sc_blob;

/* 1 if the target is built here. */
int gpu_sc_available(gpu_sc_target target);
/* The compiler's name for the stage ("vs_5_0" for dxbc), NULL if none. The
 * spv target gives the dxbc names too: its texts are the same HLSL, and the
 * key K of a text is gpu_sc_key over them on every target. */
const char* gpu_sc_profile(gpu_sc_target target, gpu_sc_stage stage);

/* The cache directory ("" when the cache is off or there is none here). The
 * first call reads PS3_SHADER_CACHE and creates the directory. */
const char* gpu_sc_cache_dir(void);
/* Key K of a text: FNV-1a 64 over profile, 0xFF, text (today's .dxbc key). */
uint64_t gpu_sc_key(const char* src, size_t n, const char* profile);
/* The cache file a text is stored under (path, cap >= 324); 0 when there
 * is no cache or no such target. */
int gpu_sc_cache_path(gpu_sc_target target, gpu_sc_stage stage, const char* src, size_t n,
                      char* path, size_t cap);

/* Compile src (n bytes; name is the compiler's source name) for a stage,
 * from the cache when it holds a whole program, into it otherwise.
 * PS3_SHADER_CACHE_HLSL=1 keeps each compiled text beside its program. On
 * GPU_SC_FAILED the compiler's message is in err (when given). Thread-safe. */
int gpu_sc_compile(gpu_sc_target target, gpu_sc_stage stage, const char* src, size_t n,
                   const char* name, unsigned flags, gpu_sc_blob* out, char* err, size_t err_cap);
/* gpu_sc_compile from the entry point entry (NULL: "main"); another entry
 * needs GPU_SC_NO_CACHE, as the cache's key does not hold it. */
int gpu_sc_compile_entry(gpu_sc_target target, gpu_sc_stage stage, const char* src, size_t n,
                         const char* name, const char* entry, unsigned flags, gpu_sc_blob* out,
                         char* err, size_t err_cap);
/* gpu_sc_compile_entry for a layout of the GPU layer (gpu.h): the spv target
 * puts each resource where the Vulkan backend's pipeline layout for it has
 * it, and the layout's CONST32 slots in its push constant block. NULL is the
 * guest programs' layout, which gpu_sc_compile and gpu_sc_compile_entry use;
 * a built-in pass passes its own. With the cache, the spv key holds the
 * layout. dxbc does not use it (the registers are the root signature's). */
struct gpu_layout_desc;
int gpu_sc_compile_layout(gpu_sc_target target, gpu_sc_stage stage, const char* src, size_t n,
                          const char* name, const char* entry, const struct gpu_layout_desc* layout,
                          unsigned flags, gpu_sc_blob* out, char* err, size_t err_cap);
/* The program the cache holds under key K (gpu_sc_key of its text), with the
 * checks a cache hit makes: GPU_SC_OK, or GPU_SC_NOT_CACHED. Not counted as a
 * hit. Thread-safe. */
int gpu_sc_load(gpu_sc_target target, gpu_sc_stage stage, uint64_t key, gpu_sc_blob* out);
void gpu_sc_blob_release(gpu_sc_blob* blob);

/* Read every program in the cache once, in the background, on
 * PS3_SHADER_CACHE_WARM=<n> threads (default 4; 0 = off); the first call
 * only. Windows' antivirus scans a file the first time it is opened after it
 * was written, so this takes that wait off the first draws. Nothing off
 * Windows. */
void gpu_sc_warm(void);

/* Compile a guest program's text into the cache (setup's pass):
 * 1 = already there, 2 = compiled, 0 = failed (or no cache), as
 * rsx_d3d12_shader_cache_put. Thread-safe. */
int gpu_sc_put(gpu_sc_target target, gpu_sc_stage stage, const char* src);

/* The spv target's post-passes, run on every module it hands out (compiled
 * or read from the cache, which holds glslang's output), so a backend can
 * leave out what its device lacks: set them before compiling. The default
 * is all but BASE_VERTEX (every draw today starts at vertex 0, where the two
 * agree); RSX_SPV_NOCONTRACT=0 leaves NoContraction out (an A/B knob). */
#define GPU_SC_SPV_FLOAT_PRESERVE  1u   /* SignedZeroInfNanPreserve 32 (shaderSignedZeroInfNanPreserveFloat32) */
#define GPU_SC_SPV_DENORM_PRESERVE 2u   /* DenormPreserve 32 (shaderDenormPreserveFloat32) */
#define GPU_SC_SPV_NO_CONTRACT     4u   /* NoContraction on float arithmetic: no fused multiply-add */
#define GPU_SC_SPV_INVARIANT       8u   /* Invariant on the position output */
#define GPU_SC_SPV_BASE_VERTEX     16u  /* SV_VertexID as D3D12's: VertexIndex - BaseVertex (shaderDrawParameters) */
#define GPU_SC_SPV_ALL             31u
#define GPU_SC_SPV_DEFAULT         15u
void gpu_sc_spv_set_options(unsigned opts);
unsigned gpu_sc_spv_options(void);

/* PERF counters (today's s_perf_fxc, s_perf_fxc_n, s_perf_sc_hits): kept
 * while gpu_sc_perf(1) is on. take copies them and, with reset, zeroes them. */
typedef struct gpu_sc_stats {
    double compile_s;          /* time inside the compiler (every target's) */
    int    compiles;
    int    cache_hits;
} gpu_sc_stats;
void gpu_sc_perf(int on);
void gpu_sc_stats_take(gpu_sc_stats* out, int reset);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_GPU_SHADER_H */
