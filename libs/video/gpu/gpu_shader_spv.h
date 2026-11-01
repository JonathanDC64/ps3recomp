/*
 * ps3recomp - the shader compile service's spv target, inside (gpu_shader.c)
 *
 * What gpu_shader.c and the test share of the spv target, in three steps:
 *
 *   1. the text edits (gpu_shader_spv_edit.cpp), on a text from rsxc_shader.c
 *      (or a built-in pass's) as it is, each a small deterministic pass with
 *      its own version:
 *        E1  [[vk::location(n)]] by semantic: COLOR0 0, COLOR1 1, FOG 2,
 *            TEXCOORDn 3 + n, UVBOUNDn 13 + n; vertex inputs ATTRn n
 *        E2  the NaN and zero guards as integer bit tests (asuint)
 *        E3  the vertex fetch's loads kept inside t20 (0 past its end)
 *        E4  the layout's CONST32 cbuffers as one push constant block, each
 *            slot at 4 bytes times the counts before it, its members at
 *            their HLSL offsets (packoffset)
 *        E5  texture and sampler arrays as one declaration per element, so
 *            entry j of a table is a binding of its own
 *   2. glslang (gpu_shader_spv.cpp): HLSL -> SPIR-V 1.3 for Vulkan 1.1, then
 *      the layout's sets and bindings (gpu_spv_apply_layout). This module is
 *      what the cache holds.
 *   3. the post-passes (gpu_shader_spv_edit.cpp), on every module handed out,
 *      by the options the backend set (gpu_sc_spv_set_options): float
 *      controls, NoContraction, Invariant position, base vertex.
 *
 * The binding model is the Vulkan backend's (gpu_vk_internal.h) for a
 * gpu_layout_desc: set 0 holds the CB slots b<n> at binding n (uniform
 * buffers), the RAW slots t<n> at 16 + n (storage buffers) and the static
 * samplers s<n> at 48 + n; the table slots are sets 1, 2, ... in slot order,
 * entry j of a texture table at binding 16 + j and of a sampler table at
 * 48 + j; the CONST32 slots are the push constant block (E4). For the guest
 * programs (gpu_spv_guest_layout, gpu.h's six slots) that is set 0 b0, b1
 * and t20 at 0, 1 and 36; set 1 t0-t15 at 16-31; set 2 s0-s15 at 48-63; b2
 * (VFDraw) the push constant at offset 0. A text that names its own
 * [[vk::binding]] keeps glslang's sets and bindings.
 *
 * Built with PS3RECOMP_RSX_HAL and PS3RECOMP_RSX_HAL_SPV. C++ includes it too.
 */
#ifndef PS3RECOMP_GPU_SHADER_SPV_H
#define PS3RECOMP_GPU_SHADER_SPV_H

#include <stddef.h>
#include <stdint.h>

#include "gpu.h"
#include "gpu_shader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The edits, as bits for gpu_spv_edit, and their versions. An edit whose
 * output changes takes a new version: the versions are in the cache's key,
 * and test_gpu_sc_spv holds each edit's output to a golden hash. */
#define GPU_SPV_E1 1u
#define GPU_SPV_E2 2u
#define GPU_SPV_E3 4u
#define GPU_SPV_E4 8u
#define GPU_SPV_E5 16u
#define GPU_SPV_EDITS_ALL 31u
#define GPU_SPV_E1_VERSION 1
#define GPU_SPV_E2_VERSION 1
#define GPU_SPV_E3_VERSION 2
#define GPU_SPV_E4_VERSION 1
#define GPU_SPV_E5_VERSION 1

/* The guest programs' layout (gpu.h): the default of gpu_sc_compile. */
const gpu_layout_desc* gpu_spv_guest_layout(void);

/* The text with the edits in `edits` applied, in E1-E5 order, for a layout
 * (E4; NULL is the guest layout): malloc'd, NUL-terminated, its length in
 * *len. NULL when out of memory or when an edit meets a form it does not
 * know (err says which). */
char* gpu_spv_edit(gpu_sc_stage stage, const char* src, size_t n, unsigned edits, const gpu_layout_desc* layout,
                   size_t* len, char* err, size_t err_cap);

/* 1 if the text names its own bindings ([[vk::binding]]): glslang's sets and
 * bindings then stay, and the layout's are not applied. */
int gpu_spv_text_has_bindings(const char* src, size_t n);

/* Step 2 for an edited text: glslang, then the layout's sets and bindings
 * (layout NULL: none applied). The words are malloc'd. GPU_SC_OK,
 * GPU_SC_FAILED (message in err), GPU_SC_BAD_ARG. */
#define GPU_SPV_GL_OPTIMIZE  1u   /* SpvOptions.optimizePerformance (GPU_SC_OPTIMIZE) */
#define GPU_SPV_GL_SEMANTICS 2u   /* each varying's HLSL semantic as UserSemantic (the test's reflection) */
int gpu_spv_glslang(gpu_sc_stage stage, const char* text, size_t n, const char* name, const char* entry,
                    unsigned gl_flags, const gpu_layout_desc* layout, uint32_t** words, size_t* nwords, char* err,
                    size_t err_cap);

/* The layout's sets and bindings on a module glslang made with the shifts
 * (b n, t 16 + n, s 48 + n), in place. 0, or -1 with err for a resource the
 * module uses and the layout does not have, or a malformed module. */
int gpu_spv_apply_layout(uint32_t* words, size_t nwords, const gpu_layout_desc* layout, char* err, size_t err_cap);

/* Step 3: the post-passes in opts (GPU_SC_SPV_*) for a stage, into a new
 * malloc'd module. GPU_SC_OK, or GPU_SC_FAILED for a malformed module. */
int gpu_spv_post(gpu_sc_stage stage, const uint32_t* words, size_t nwords, unsigned opts, uint32_t** out,
                 size_t* out_n);

/* SPIRV-Tools' validator, Vulkan 1.1 rules: 1 valid, 0 not (message in err). */
int gpu_spv_validate(const uint32_t* words, size_t nwords, char* err, size_t err_cap);

/* "glslang@16.6.0+spirv-tools@v2026.4": the compilers whose output the cache
 * holds (their versions are in its key). */
const char* gpu_spv_compiler(void);

/* The cache key's tag for a layout (NULL: the guest's),
 * "spv;<compiler>;<flags>;edits=<E1>.<E2>.<E3>.<E4>.<E5>;layout=<16 hex>"
 * into buf, and key T of a text whose dxbc key is k: FNV-1a 64 of
 * "<tag>;<k, 16 hex digits>". */
const char* gpu_spv_tag(const gpu_layout_desc* layout, char* buf, size_t cap);
uint64_t gpu_spv_key(uint64_t k, const gpu_layout_desc* layout);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_GPU_SHADER_SPV_H */
