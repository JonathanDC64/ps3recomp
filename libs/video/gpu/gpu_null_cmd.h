/*
 * ps3recomp - the portable renderer's GPU layer: the null backend's commands
 *
 * Every command of gpu_cmd_ops.h as a static inline function, each one call
 * into the null backend's recorder (gpu_null.c), which records and checks it;
 * and the per-draw device calls (GPU_FAST_FUNCS), each one call too. The draw
 * loop includes this file instead of gpu_vt_cmd.h (gpu_cmd_ops.h says how),
 * so the core compiled for the null backend (rsxc_null.c) makes direct calls,
 * as it does on D3D12 and Vulkan.
 *
 * gpu_pipeline.v is the pipeline's index + 1, gpu_table.v its pool's id
 * times 2^32 plus its first entry, gpu_addr its buffer's slot and
 * generation over a 40-bit offset (gpu_null.c). Plain C99; every system.
 */
#ifndef PS3RECOMP_GPU_NULL_CMD_H
#define PS3RECOMP_GPU_NULL_CMD_H

#include "gpu_null_internal.h"

#if defined(PS3RECOMP_RSX_HAL)

#ifdef __cplusplus
extern "C" {
#endif

#define GPU__NULL_CMD_V(name, ret, params, args) \
    static inline void gpu_cmd_##name params { gpu_null_cmd_##name args; }
#define GPU__NULL_CMD_R(name, ret, params, args) \
    static inline ret gpu_cmd_##name params { return gpu_null_cmd_##name args; }
#define GPU__NULL_CMD(name, form, ret, params, args) GPU__NULL_CMD_##form(name, ret, params, args)
GPU_CMD_OPS(GPU__NULL_CMD)
#undef GPU__NULL_CMD
#undef GPU__NULL_CMD_V
#undef GPU__NULL_CMD_R

#define GPU__NULL_FAST_V(name, ret, params, args) \
    static inline void gpu_fast_##name params { gpu_null_fast_##name args; }
#define GPU__NULL_FAST_R(name, ret, params, args) \
    static inline ret gpu_fast_##name params { return gpu_null_fast_##name args; }
#define GPU__NULL_FAST(name, form, ret, params, args) GPU__NULL_FAST_##form(name, ret, params, args)
GPU_FAST_FUNCS(GPU__NULL_FAST)
#undef GPU__NULL_FAST
#undef GPU__NULL_FAST_V
#undef GPU__NULL_FAST_R

/* (In C11 the check is _Static_assert, which clang's -Weverything counts as
 * new to C11; clang before 19 has no such warning.) */
#if defined(__clang__)
#pragma clang diagnostic push
#if __has_warning("-Wpre-c11-compat")
#pragma clang diagnostic ignored "-Wpre-c11-compat"
#endif
#endif
GPU_CMD_CHECK(null);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_HAL */
#endif /* PS3RECOMP_GPU_NULL_CMD_H */
