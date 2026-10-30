/*
 * ps3recomp - the portable renderer's GPU layer: commands through the table
 *
 * gpu_cmd_<name>(c, ...) for every command in gpu_cmd_ops.h, each calling the
 * recorder's own table (its gpu_cmd_head's ops). For code compiled once for
 * every backend: the passes that run a few times a frame. The draw loop
 * includes its backend's gpu_<backend>_cmd.h instead, never this file too.
 */
#ifndef PS3RECOMP_GPU_VT_CMD_H
#define PS3RECOMP_GPU_VT_CMD_H

#include "gpu_cmd_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GPU__CMD_WRAP_V(name, ret, params, args) \
    static inline void gpu_cmd_##name params { GPU_CMD_OPS_OF(c)->name args; }
#define GPU__CMD_WRAP_R(name, ret, params, args) \
    static inline ret gpu_cmd_##name params { return GPU_CMD_OPS_OF(c)->name args; }
#define GPU__CMD_WRAP(name, form, ret, params, args) GPU__CMD_WRAP_##form(name, ret, params, args)
GPU_CMD_OPS(GPU__CMD_WRAP)
#undef GPU__CMD_WRAP
#undef GPU__CMD_WRAP_V
#undef GPU__CMD_WRAP_R

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_GPU_VT_CMD_H */
