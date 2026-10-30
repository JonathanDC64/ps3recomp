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

/* What the core needs of its backend that gpu.h does not give (step C8a):
 * the fragments call these, and only this file names the backend. */
/* The device (init_d3d12). */
static gpu_device* rc_be_create(const gpu_device_desc* dd) { return gpu_d3d12_create(dd); }
/* A layout's serialized bytes' hash, part of every pipeline's name in
 * pso_manifest.bin (s_vp_root_hash, pso_desc_key). */
static u64 rc_be_layout_hash(gpu_device* d, gpu_layout l) { return gpu_d3d12_layout_hash(d, l); }
/* The D3D12 description the backend makes of pd: pso_manifest.bin's records
 * are these, shared with the d3d12 route (pso_desc_key, pso_rec). */
static void rc_be_pipeline_desc(gpu_device* d, const gpu_pipeline_desc* pd,
                                D3D12_GRAPHICS_PIPELINE_STATE_DESC* out,
                                D3D12_INPUT_ELEMENT_DESC il[GPU_MAX_VERTEX_ATTRS])
{
    gpu_d3d12_pipeline_d3d_desc(d, pd, out, il);
}
/* RSX_LOSE_DEVICE_AT_FRAME (lose_device_test): the device removed on purpose
 * at frame `frames`, as a driver reset would lose it. */
static void rc_be_lose_device(gpu_device* d, int frames)
{
    ID3D12Device* const dev = (ID3D12Device*)gpu_native(d, GPU_NATIVE_DEVICE, 0);
    ID3D12Device5* d5 = NULL;
    if (SUCCEEDED(dev->lpVtbl->QueryInterface(dev, &IID_ID3D12Device5, (void**)&d5)) && d5) {
        fprintf(stderr, "[D3D12] RSX_LOSE_DEVICE_AT_FRAME: removing the device at frame %d%c", frames, 10);
        d5->lpVtbl->RemoveDevice(d5);
        d5->lpVtbl->Release(d5);
    }
}

#include "rsxc_state.inc"
#include "rsxc_shadercache.inc"   /* (before the first fragment that compiles a shader) */
#include "rsxc_window.inc"
#include "rsxc_hdr.inc"
#include "rsxc_init.inc"
#include "rsxc_sync.inc"
#include "rsxc_present.inc"
#include "rsxc_cap.inc"
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
