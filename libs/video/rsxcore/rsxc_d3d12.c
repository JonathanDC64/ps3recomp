/*
 * ps3recomp - the portable renderer's core, compiled for its D3D12 backend
 *
 * The route RSX_BACKEND=hal-d3d12 (libs/video/rsx_out.c). Step 1.0 of the
 * portable renderer: today's renderer, libs/video/rsx_d3d12_backend.c, copied
 * word for word into the .inc fragments of this folder and compiled here as
 * one translation unit, so it draws exactly the frames the d3d12 route draws.
 * The steps after it moved the fragments onto the GPU layer (gpu.h) one piece
 * at a time, checking the frames against captures after each. rsxc_null.c
 * (and rsxc_vk.c, later) include the same fragments after their backend's own
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
 * The fragments name no backend (step C8b): what the core needs of D3D12
 * beyond gpu.h is in this file, as the rc_be_* hooks below, and only this
 * file includes D3D12's headers (through the backend's).
 *
 * Windows only for now: off Windows the new path's backend is Vulkan, later.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(_WIN32)

#include "rsxc_head.inc"
/* The GPU layer, with the D3D12 backend's static inline commands (the draw
 * loop calls them directly), and the backend's own calls the hooks make. */
#include "gpu_d3d12_cmd.h"
#include "gpu_d3d12_bridge.h"
#include "gpu_shader.h"

/* ---------------------------------------------------------------------------
 * The backend's hooks: what the fragments need of their backend that gpu.h
 * does not give. Every instance defines the same set (rsxc_null.c too).
 * -------------------------------------------------------------------------*/

/* The device (init_d3d12). */
static gpu_device* rc_be_create(const gpu_device_desc* dd) { return gpu_d3d12_create(dd); }

/* A window to present into (rc_backend_init): 0, today's window. */
static const int rc_be_headless = 0;

/* Step V6: a frame buffer that cannot be made does not fail the start (as
 * today's; init_d3d12). */
static const int rc_be_start_strict = 0;

/* D3D12_DRED=1: Device Removed Extended Data, the device's breadcrumbs
 * (auto-breadcrumbs, page faults and the draws' markers, which
 * gpu_fault_report prints once the device is lost). rc_be_lost_hint ends
 * check_device's line with the knob. */
static int rc_be_breadcrumbs(void) { return getenv("D3D12_DRED") ? 1 : 0; }
static const char rc_be_lost_hint[] = " D3D12_DRED=1 on the next run says where the GPU was.";

/* Why the device was lost, from gpu_lost's reason (GetDeviceRemovedReason's
 * HRESULT), for check_device's line. */
static const char* rc_be_lost_text(u32 reason)
{
    switch ((unsigned long)reason) {
    case 0x887A0006ul: return "the GPU stopped responding and the driver was reset (DXGI_ERROR_DEVICE_HUNG)";
    case 0x887A0005ul: return "the GPU was removed or its driver changed (DXGI_ERROR_DEVICE_REMOVED)";
    case 0x887A0007ul: return "the device was reset after a bad command (DXGI_ERROR_DEVICE_RESET)";
    case 0x887A0020ul: return "an internal driver error (DXGI_ERROR_DRIVER_INTERNAL_ERROR)";
    case 0x8007000Eul: return "the GPU ran out of memory (E_OUTOFMEMORY)";
    default: return "an unknown reason";
    }
}

/* A window's surface lost for good (gpu.h's GPU_PRESENT_SURFACE_LOST, step
 * VC2): never on D3D12, whose swap chains have no surface of their own. */
static const u32 rc_be_surface_lost = 0;

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

/* The menu renderer (the port's, D3D12 only until it is ported) takes the
 * device, the queue and the swap chain's images as native objects
 * (rsxc_api.inc: rc_device, rc_queue, rc_back_buffer and the overlay's
 * frame): the GPU layer's own, borrowed as today's were. */
static void* rc_be_device(gpu_device* d) { return gpu_native(d, GPU_NATIVE_DEVICE, 0); }
static void* rc_be_queue(gpu_device* d)  { return gpu_native(d, GPU_NATIVE_QUEUE, 0); }
static void* rc_be_image(gpu_device* d, gpu_texture t) { return gpu_native(d, GPU_NATIVE_TEXTURE, t.id); }

/* The shader cache's files (rsxc_shadercache.inc): today's, all of them
 * (.dxbc, fp_variants.txt, pso_index_seen.tsv, pso_manifest.bin and the
 * pipeline library). */
static const int rc_be_cache_files = 1;

/* The pipeline library's folder in the shader cache: <cache>/gpu/d3d12/
 * (pipelines.lib, pipelines.n, index.built: rsxc_psolib.inc). */
static const char rc_be_lib_dir[] = "d3d12";
/* The library's file there (step VC2: Vulkan's is named by its driver). */
static const char* rc_be_lib_file(gpu_device* d) { (void)d; return "pipelines.lib"; }

/* The shaders: the compile service's dxbc target (gpu_shader.c), today's
 * FXC call and .dxbc cache. A compile (sc_compile) is 1 with the code in
 * *out, else 0 with FXC's message in msg; a load (sc_load) the program the
 * cache holds under key K (the pipeline index's rows); a put
 * (rc_shader_cache_put) compiles a text into the cache. The layout a shader
 * is bound with is the root signature's business here: dxbc does not use it. */
static const uint32_t rc_be_shader_code = GPU_CODE_DXBC;
static int rc_be_sc_compile(gpu_sc_stage stage, const char* src, size_t n, const char* name, const char* entry,
                            unsigned flags, const gpu_layout_desc* layout, gpu_sc_blob* out, char* msg, size_t cap)
{
    (void)layout;
    return gpu_sc_compile_entry(GPU_SC_DXBC, stage, src, n, name, entry, flags, out, msg, cap) == GPU_SC_OK;
}
static int rc_be_sc_load(gpu_sc_stage stage, u64 key, gpu_sc_blob* out)
{
    return gpu_sc_load(GPU_SC_DXBC, stage, key, out) == GPU_SC_OK;
}
static int rc_be_cache_put(gpu_sc_stage stage, const char* hlsl) { return gpu_sc_put(GPU_SC_DXBC, stage, hlsl); }

/* After each present (swap_present): nothing more on D3D12. */
static void rc_be_presented(gpu_device* d) { (void)d; }

/* The frame slot after an acquire that gave image `image` (rc_acquire): the
 * image's index itself, as today's (the swap chain has FRAME_COUNT buffers,
 * one per slot, and its index stays when nothing is presented). */
static u32 rc_be_frame_slot(u32 prev, u32 image) { (void)prev; return image; }

/* A layout's serialized bytes' hash, part of every pipeline's name in
 * pso_manifest.bin (s_vp_root_hash). */
static u64 rc_be_layout_hash(gpu_device* d, gpu_layout l) { return gpu_d3d12_layout_hash(d, l); }

/* pso_manifest.bin's records (rsxc_psolib.inc) hold the d3d12 route's
 * description of a pipeline, so the two routes share the file: the D3D12
 * description the backend makes of the GPU layer's (call map F, G8), with
 * its pointers zeroed, and its input layout. The record's header is the
 * core's. */
typedef struct rc_be_pso_desc {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;   /* pointers zeroed */
    struct { char sem[16]; D3D12_INPUT_ELEMENT_DESC e; } el[16];
} rc_be_pso_desc;

/* A pipeline's name in the manifest, today's: FNV-1a 64 over the root
 * signature's serialized bytes (layout_hash: every caller passes
 * vp_layout's), the description without its pointers, the shaders' code and
 * the input layout. Callers zero the whole description first, so padding
 * hashes the same every run. */
static u64 pso_desc_key(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, u64 layout_hash)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC c;
    memcpy(&c, pd, sizeof c);
    c.pRootSignature = NULL;
    c.VS.pShaderBytecode = c.PS.pShaderBytecode = c.DS.pShaderBytecode = NULL;
    c.HS.pShaderBytecode = c.GS.pShaderBytecode = NULL;
    c.StreamOutput.pSODeclaration = NULL; c.StreamOutput.pBufferStrides = NULL;
    c.InputLayout.pInputElementDescs = NULL; c.CachedPSO.pCachedBlob = NULL;
    u64 h = gpu_d3d12_fnv(14695981039346656037ull, &layout_hash, sizeof layout_hash);
    h = gpu_d3d12_fnv(h, &c, sizeof c);
    const D3D12_SHADER_BYTECODE* sh[5] = { &pd->VS, &pd->PS, &pd->DS, &pd->HS, &pd->GS };
    for (int i = 0; i < 5; i++)
        if (sh[i]->pShaderBytecode) h = gpu_d3d12_fnv(h, sh[i]->pShaderBytecode, sh[i]->BytecodeLength);
    for (UINT i = 0; i < pd->InputLayout.NumElements; i++) {
        D3D12_INPUT_ELEMENT_DESC e = pd->InputLayout.pInputElementDescs[i];
        h = gpu_d3d12_fnv(h, e.SemanticName, strlen(e.SemanticName));
        e.SemanticName = NULL;
        h = gpu_d3d12_fnv(h, &e, sizeof e);
    }
    return h;
}

/* pd's name in the manifest (pso_desc_key of the D3D12 description the
 * backend makes of it). */
static u64 rc_be_pso_name(gpu_device* d, const gpu_pipeline_desc* pd, u64 layout_hash)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC dd;
    D3D12_INPUT_ELEMENT_DESC il[GPU_MAX_VERTEX_ATTRS];
    gpu_d3d12_pipeline_d3d_desc(d, pd, &dd, il);
    return pso_desc_key(&dd, layout_hash);
}

/* pd as a manifest record: its name, its description (rec) and input
 * layout's length (nel), and its two shaders' code (VS, PS: the backend's
 * copies, valid while the shaders live). 0 when a record cannot hold it: a
 * geometry, hull or domain shader, no vertex or pixel shader, more than 16
 * input elements, stream output. */
static int rc_be_pso_record(gpu_device* d, const gpu_pipeline_desc* pd, u64 layout_hash, u64* name,
                            rc_be_pso_desc* rec, u32* nel, const void* code[2], u32 len[2])
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC dd;
    D3D12_INPUT_ELEMENT_DESC il[GPU_MAX_VERTEX_ATTRS];
    gpu_d3d12_pipeline_d3d_desc(d, pd, &dd, il);
    if (dd.DS.pShaderBytecode || dd.HS.pShaderBytecode || dd.GS.pShaderBytecode ||
        !dd.VS.pShaderBytecode || !dd.PS.pShaderBytecode || dd.InputLayout.NumElements > 16 ||
        dd.StreamOutput.NumEntries)
        return 0;
    *name = pso_desc_key(&dd, layout_hash);
    memcpy(&rec->desc, &dd, sizeof rec->desc);
    rec->desc.pRootSignature = NULL;
    rec->desc.VS.pShaderBytecode = rec->desc.PS.pShaderBytecode = NULL;
    rec->desc.InputLayout.pInputElementDescs = NULL;
    rec->desc.CachedPSO.pCachedBlob = NULL; rec->desc.CachedPSO.CachedBlobSizeInBytes = 0;
    *nel = dd.InputLayout.NumElements;
    for (u32 i = 0; i < *nel; i++) {
        rec->el[i].e = dd.InputLayout.pInputElementDescs[i];
        snprintf(rec->el[i].sem, sizeof rec->el[i].sem, "%s", rec->el[i].e.SemanticName);
        rec->el[i].e.SemanticName = NULL;
    }
    code[0] = dd.VS.pShaderBytecode; len[0] = (u32)dd.VS.BytecodeLength;
    code[1] = dd.PS.pShaderBytecode; len[1] = (u32)dd.PS.BytecodeLength;
    return 1;
}

/* A record back as the GPU layer's description, all but its layout and
 * shaders (the caller's): the inverse of the D3D12 description the backend
 * makes of vp_fp_desc's, so the record's pipeline gets the name a draw's
 * gets. pd is zeroed first. 0 for a record no vp_fp_desc makes. */
static int rc_be_pso_unrecord(const rc_be_pso_desc* rec, u32 nel, gpu_pipeline_desc* pd)
{
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d = &rec->desc;
    const D3D12_RENDER_TARGET_BLEND_DESC* rt = &d->BlendState.RenderTarget[0];
    if (nel > GPU_MAX_VERTEX_ATTRS || d->NumRenderTargets > 4) return 0;
    memset(pd, 0, sizeof *pd);   /* zeroed whole: the GPU layer names the pipeline by its bytes */
    pd->vertex.count = nel;
    pd->vertex.stride = nel ? 256 : 0;
    for (u32 i = 0; i < nel; i++) {
        snprintf(pd->vertex.attr[i].semantic, sizeof pd->vertex.attr[i].semantic, "%s", rec->el[i].sem);
        pd->vertex.attr[i].semantic_index = rec->el[i].e.SemanticIndex;
        pd->vertex.attr[i].format = (gpu_format)rec->el[i].e.Format;
        pd->vertex.attr[i].offset = rec->el[i].e.AlignedByteOffset;
    }
    pd->topology_type = (u8)d->PrimitiveTopologyType;
    pd->cull = (u8)d->RasterizerState.CullMode;
    pd->front_ccw = d->RasterizerState.FrontCounterClockwise ? 1 : 0;
    pd->depth_clip = d->RasterizerState.DepthClipEnable ? 1 : 0;
    pd->multisample = d->RasterizerState.MultisampleEnable ? 1 : 0;
    pd->depth_test = d->DepthStencilState.DepthEnable ? 1 : 0;
    pd->depth_write = d->DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL ? 1 : 0;
    pd->depth_func = (u8)d->DepthStencilState.DepthFunc;
    pd->blend.enable = rt->BlendEnable ? 1 : 0;
    pd->blend.src = (u8)rt->SrcBlend;
    pd->blend.dst = (u8)rt->DestBlend;
    pd->blend.op = (u8)rt->BlendOp;
    pd->blend.src_alpha = (u8)rt->SrcBlendAlpha;
    pd->blend.dst_alpha = (u8)rt->DestBlendAlpha;
    pd->blend.op_alpha = (u8)rt->BlendOpAlpha;
    pd->blend.write_mask = (u8)rt->RenderTargetWriteMask;
    pd->nrt = d->NumRenderTargets;
    for (u32 i = 0; i < pd->nrt; i++) pd->rt_format[i] = (gpu_format)d->RTVFormats[i];
    pd->ds_format = (gpu_format)d->DSVFormat;
    pd->samples = d->SampleDesc.Count;
    pd->sample_mask = d->SampleMask;
    pd->strip_cut = (u32)d->IBStripCutValue;
    return 1;
}

#include "rsxc_core.inc"

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
