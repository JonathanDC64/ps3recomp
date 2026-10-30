/*
 * ps3recomp - the GPU layer's contract test: what its C and C++ halves share
 *
 * test_gpu_contract.c (C99 where the compiler has the switch) and
 * test_gpu_contract.cpp (C++, with main) are one program, so the two can
 * check that gpu.h means the same structures in both languages.
 */
#ifndef PS3RECOMP_TEST_GPU_CONTRACT_H
#define PS3RECOMP_TEST_GPU_CONTRACT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Every type gpu.h and gpu_cmd_ops.h define, for sizeof in both languages. */
#define GPU_CONTRACT_TYPES(X) \
    X(gpu_buffer) X(gpu_texture) X(gpu_view) X(gpu_sampler) X(gpu_shader) X(gpu_layout) \
    X(gpu_query_pool) X(gpu_heap) X(gpu_table_pool) X(gpu_swapchain) X(gpu_pipeline) X(gpu_table) \
    X(gpu_caps) X(gpu_buffer_desc) X(gpu_heap_desc) X(gpu_texture_desc) X(gpu_footprint) \
    X(gpu_box) X(gpu_rect) X(gpu_viewport) X(gpu_view_desc) X(gpu_sampler_desc) \
    X(gpu_table_region) X(gpu_table_pool_desc) X(gpu_shader_desc) X(gpu_layout_slot) \
    X(gpu_static_sampler) X(gpu_layout_desc) X(gpu_vertex_attr) X(gpu_vertex_layout) \
    X(gpu_blend_state) X(gpu_stencil_face) X(gpu_pipeline_desc) X(gpu_pass) X(gpu_barrier) \
    X(gpu_submit_desc) X(gpu_swapchain_desc) X(gpu_present_stats) X(gpu_hdr_metadata) \
    X(gpu_adapter_info) X(gpu_device_desc) X(gpu_cmd_head) X(gpu_vt) X(gpu_cmd_ops) X(gpu_device)

/* The C half: sizeof each type above, in the list's order, into out (n of
 * them at most); returns how many types there are. */
int gpu_contract_c_sizes(size_t* out, int n);
/* The C half's checks; returns its failures. */
int gpu_contract_c_run(void);

#ifdef __cplusplus
}
#endif
#endif
