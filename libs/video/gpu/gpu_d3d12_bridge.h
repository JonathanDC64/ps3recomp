/*
 * ps3recomp - the portable renderer's GPU layer: D3D12 bridges for the conversion
 *
 * TEMPORARY: this file is deleted when the conversion of the copied RSX code
 * (libs/video/rsxcore) onto gpu.h ends. Nothing outside that conversion may
 * use it, and nothing in gpu.h depends on it.
 *
 * The conversion moves the copied code onto the GPU layer one piece at a
 * time, so for a while converted and not-yet-converted code share objects.
 * These turn the old code's D3D12 objects into the layer's handles; the
 * other way is gpu_native(). Each adopted object is AddRef'd and released
 * when its handle is destroyed (or, for pipelines and layouts, with the
 * device), so either side may drop its own reference first.
 *
 * Windows only for now (vkd3d comes later).
 */
#ifndef PS3RECOMP_GPU_D3D12_BRIDGE_H
#define PS3RECOMP_GPU_D3D12_BRIDGE_H

#if defined(PS3RECOMP_RSX_HAL) && defined(_WIN32)

#include <d3d12.h>
#include "gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A texture over res. desc says what it is (size, layers, mips, format,
 * samples, use); its state is the one res is in now, for the caller's own
 * tracking. heap and clear values are ignored. */
gpu_texture gpu_d3d12_adopt_texture(gpu_device* d, ID3D12Resource* res, const gpu_texture_desc* desc);

/* A buffer over res. An upload or readback buffer (desc->memory) is mapped
 * again, so gpu_buffer_map hands back the pointer the old code writes
 * through. */
gpu_buffer gpu_d3d12_adopt_buffer(gpu_device* d, ID3D12Resource* res, const gpu_buffer_desc* desc);

/* A pipeline the old code made; kept until gpu_destroy. */
gpu_pipeline gpu_d3d12_adopt_pipeline(gpu_device* d, ID3D12PipelineState* pso);

/* A layout over a root signature the old code made, with the FNV-1a 64 of
 * its serialized bytes (today's s_vp_root_hash for vp_root_sig), so
 * pipelines made on it are named as gpu_layout_create's would be. Kept
 * until gpu_destroy. */
gpu_layout gpu_d3d12_adopt_layout(gpu_device* d, ID3D12RootSignature* rs, uint64_t blob_hash);

/* A table pool over a shader-visible heap the old code made and writes
 * into too. desc's kind, count and regions describe it. Its memo starts
 * empty, so the pool's first write to each entry always writes. Kept until
 * gpu_destroy. */
gpu_table_pool gpu_d3d12_adopt_table_pool(gpu_device* d, ID3D12DescriptorHeap* heap,
                                          const gpu_table_pool_desc* desc);

/* A table from a GPU descriptor handle of the old code's (its heap must be
 * the one bound with set_table_pools, or bound by the old code). */
static inline gpu_table gpu_d3d12_wrap_table(D3D12_GPU_DESCRIPTOR_HANDLE h)
{
    gpu_table t;
    t.v = (uint64_t)h.ptr;
    return t;
}

/* A recorder that records into cl, a list the old code has open (it stays
 * the old code's: it resets, closes and submits it). Free it with
 * gpu_d3d12_cmd_unwrap; until then it may be used again. */
gpu_cmd* gpu_d3d12_cmd_wrap(gpu_device* d, ID3D12GraphicsCommandList* cl);
void     gpu_d3d12_cmd_unwrap(gpu_device* d, gpu_cmd* c);

/* A swap chain's image i, as gpu_swap_acquire hands back the current one
 * (none past its buffer count): the old code makes every buffer's RTV, and
 * hands every buffer to the menu renderer, up front. Valid until the next
 * gpu_swap_resize or gpu_swap_destroy. */
gpu_texture gpu_d3d12_swap_image(gpu_device* d, gpu_swapchain sc, uint32_t i);

/* The D3D12 description gpu_pipeline_create builds from desc (pointers into
 * desc and the shaders' code), so the conversion can check it byte for byte
 * against today's (gates G2: pso_manifest.bin). il holds the input layout. */
void gpu_d3d12_pipeline_d3d_desc(gpu_device* d, const gpu_pipeline_desc* desc,
                                 D3D12_GRAPHICS_PIPELINE_STATE_DESC* out,
                                 D3D12_INPUT_ELEMENT_DESC il[GPU_MAX_VERTEX_ATTRS]);

/* A layout's serialized root signature hash (FNV-1a 64). */
uint64_t gpu_d3d12_layout_hash(gpu_device* d, gpu_layout layout);

/* ---- step C4 ---- */

/* A texture over res, as gpu_d3d12_adopt_texture, its description read from
 * res itself (GetDesc: size, layers, mips, format, samples, and the target
 * flags as use). For the old code's creation sites: one line after each. */
gpu_texture gpu_d3d12_adopt_texture_of(gpu_device* d, ID3D12Resource* res);

/* A table pool's descriptor heap, borrowed (gpu_native has no kind for it),
 * for the old code's SetDescriptorHeaps and descriptor arithmetic. Freed
 * with the pool, at gpu_destroy: never Released by the caller. */
ID3D12DescriptorHeap* gpu_d3d12_table_pool_heap(gpu_device* d, gpu_table_pool pool);

/* ---- step C5b ---- */

/* A query pool's heap, borrowed (gpu_native has no kind for it), for the old
 * code's BeginQuery, EndQuery and ResolveQueryData. Freed with the pool, at
 * gpu_destroy: never Released by the caller. */
ID3D12QueryHeap* gpu_d3d12_query_pool_heap(gpu_device* d, gpu_query_pool pool);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_HAL && _WIN32 */
#endif /* PS3RECOMP_GPU_D3D12_BRIDGE_H */
