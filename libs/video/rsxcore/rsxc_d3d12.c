/*
 * ps3recomp - the portable renderer's core, compiled for its D3D12 backend
 *
 * The route RSX_BACKEND=hal-d3d12 (libs/video/rsx_out.c). Step 1.0 of the
 * portable renderer: today's renderer, libs/video/rsx_d3d12_backend.c, copied
 * word for word into the .inc fragments of this folder and compiled here as
 * one translation unit, so it draws exactly the frames the d3d12 route draws.
 * The steps after it move the fragments onto the GPU layer (gpu.h) one piece
 * at a time, checking the frames against captures after each; rsxc_vk.c and
 * rsxc_null.c will include the same fragments after their backend's own
 * static inline command header, so the draw loop makes direct calls on every
 * backend.
 *
 * copy_manifest.txt gives each fragment's lines of the original and the only
 * edits the copy makes, as a sed script: the original's exports are static
 * here and renamed rc_*, and the two globals it defines (g_rsx_title_base,
 * g_rsx_prof_toggle) are extern, since the original is always linked beside
 * the copy and owns them. Statics keep their names. So this file exports one
 * symbol, the route table at the end.
 *
 * Windows only for now: off Windows the new path's backend is Vulkan, later.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(_WIN32)

#include "rsxc_head.inc"
/* The GPU layer, with the D3D12 backend's static inline commands (the draw
 * loop calls them directly) and, while the conversion runs, its bridges. */
#include "gpu_d3d12_cmd.h"
#include "gpu_d3d12_bridge.h"
#include "rsxc_state.inc"
#include "rsxc_window.inc"
#include "rsxc_hdr.inc"
#include "rsxc_init.inc"
#include "rsxc_sync.inc"
#include "rsxc_present.inc"
#include "rsxc_cap.inc"
#include "rsxc_shadercache.inc"
#include "rsxc_psolib.inc"
#include "rsxc_vs.inc"
#include "rsxc_fp.inc"
#include "rsxc_pipeline.inc"
#include "rsxc_texture.inc"
#include "rsxc_targets.inc"
#include "rsxc_writeback.inc"
#include "rsxc_zpass.inc"
#include "rsxc_diag.inc"
#include "rsxc_post.inc"
#include "rsxc_frame.inc"
#include "rsxc_record.inc"
#include "rsxc_vertex.inc"
#include "rsxc_record2.inc"
#include "rsxc_api.inc"

#include "rsx_out.h"

/* The copy's exports, as the route switch calls them. */
#define RSXC_ROUTE_V_(name, legacy, params, args)      .name = rc_##name,
#define RSXC_ROUTE_R_(ret, name, legacy, params, args) .name = rc_##name,
const rsx_route rsxc_route_d3d12 = {
    .name = "hal-d3d12",
    RSX_OUT_FUNCS(RSXC_ROUTE_V_, RSXC_ROUTE_R_)
};
#undef RSXC_ROUTE_V_
#undef RSXC_ROUTE_R_

#endif /* PS3RECOMP_RSX_HAL && _WIN32 */
