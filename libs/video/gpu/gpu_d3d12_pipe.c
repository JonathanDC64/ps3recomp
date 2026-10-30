/*
 * ps3recomp - the portable renderer's GPU layer: the D3D12 backend's pipelines
 *
 * gpu_pipeline_desc to D3D12_GRAPHICS_PIPELINE_STATE_DESC, and today's
 * ID3D12PipelineLibrary around it (rsx_d3d12_backend.c, pso_create_cached and
 * pso_lib_*): a pipeline is loaded from the library by its name, or made and
 * stored under it; the library is read from and written to the file the
 * caller names. Thread-safe: the start's pipeline workers make pipelines
 * beside the render thread, as today's do.
 *
 * Every field a description sets is passed through, so each of today's
 * pipelines comes out as the same D3D12 description, byte for byte except
 * the pointers (gpu_d3d12_pipeline_d3d_desc hands it back for the check).
 *
 * Built with PS3RECOMP_RSX_HAL on Windows; vkd3d comes later.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(_WIN32)

#include "gpu_d3d12_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The D3D12 description. Zeroed whole first, as today's are, then:
 *   - every target below nrt takes the blend state with its write mask; with
 *     no targets, target 0 still holds the blend state with every channel
 *     written (vp_fp_desc leaves it so for depth-only pipelines);
 *   - FillMode SOLID, cull 0 read as NONE, one sample when samples is 0;
 *   - the input layout from slot 0, per vertex. */
void gpu_d3d12_pipe_d3d_desc(gpu_d3d12_device* dd, const gpu_pipeline_desc* desc,
                             D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd,
                             D3D12_INPUT_ELEMENT_DESC il[GPU_MAX_VERTEX_ATTRS])
{
    memset(pd, 0, sizeof *pd);
    if (gpu_d3d12_slot_valid(&dd->layout_slots, desc->layout.id))
        pd->pRootSignature = dd->layout_rs[GPU_HANDLE_INDEX(desc->layout.id)];
    const gpu_shader sh[3] = { desc->vs, desc->gs, desc->ps };
    D3D12_SHADER_BYTECODE* const bc[3] = { &pd->VS, &pd->GS, &pd->PS };
    for (int k = 0; k < 3; k++)
        if (gpu_d3d12_slot_valid(&dd->shader_slots, sh[k].id)) {
            const gpu_d3d12_shader* s = &dd->shader[GPU_HANDLE_INDEX(sh[k].id)];
            bc[k]->pShaderBytecode = s->code;
            bc[k]->BytecodeLength = (SIZE_T)s->size;
        }
    const UINT na = desc->vertex.count < GPU_MAX_VERTEX_ATTRS ? desc->vertex.count : GPU_MAX_VERTEX_ATTRS;
    memset(il, 0, sizeof(D3D12_INPUT_ELEMENT_DESC) * GPU_MAX_VERTEX_ATTRS);
    for (UINT i = 0; i < na; i++) {
        const gpu_vertex_attr* a = &desc->vertex.attr[i];
        il[i].SemanticName = a->semantic;
        il[i].SemanticIndex = a->semantic_index;
        il[i].Format = (DXGI_FORMAT)a->format;
        il[i].InputSlot = 0;
        il[i].AlignedByteOffset = a->offset;
        il[i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[i].InstanceDataStepRate = 0;
    }
    pd->InputLayout.pInputElementDescs = il;
    pd->InputLayout.NumElements = na;

    D3D12_RENDER_TARGET_BLEND_DESC rt;
    memset(&rt, 0, sizeof rt);
    rt.BlendEnable = desc->blend.enable ? TRUE : FALSE;
    rt.SrcBlend = (D3D12_BLEND)desc->blend.src;
    rt.DestBlend = (D3D12_BLEND)desc->blend.dst;
    rt.BlendOp = (D3D12_BLEND_OP)desc->blend.op;
    rt.SrcBlendAlpha = (D3D12_BLEND)desc->blend.src_alpha;
    rt.DestBlendAlpha = (D3D12_BLEND)desc->blend.dst_alpha;
    rt.BlendOpAlpha = (D3D12_BLEND_OP)desc->blend.op_alpha;
    const UINT nrt = desc->nrt < 4 ? desc->nrt : 4;
    rt.RenderTargetWriteMask = nrt ? (UINT8)(desc->blend.write_mask & 0xF) : (UINT8)D3D12_COLOR_WRITE_ENABLE_ALL;
    for (UINT r = 0; r < (nrt ? nrt : 1); r++) pd->BlendState.RenderTarget[r] = rt;
    pd->SampleMask = desc->sample_mask;

    pd->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd->RasterizerState.CullMode = desc->cull ? (D3D12_CULL_MODE)desc->cull : D3D12_CULL_MODE_NONE;
    pd->RasterizerState.FrontCounterClockwise = desc->front_ccw ? TRUE : FALSE;
    pd->RasterizerState.DepthClipEnable = desc->depth_clip ? TRUE : FALSE;
    pd->RasterizerState.MultisampleEnable = desc->multisample ? TRUE : FALSE;

    pd->DepthStencilState.DepthEnable = desc->depth_test ? TRUE : FALSE;
    pd->DepthStencilState.DepthWriteMask = desc->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL
                                                             : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd->DepthStencilState.DepthFunc = (D3D12_COMPARISON_FUNC)desc->depth_func;
    pd->DepthStencilState.StencilEnable = desc->stencil_test ? TRUE : FALSE;
    pd->DepthStencilState.StencilReadMask = desc->stencil_read_mask;
    pd->DepthStencilState.StencilWriteMask = desc->stencil_write_mask;
    pd->DepthStencilState.FrontFace.StencilFailOp = (D3D12_STENCIL_OP)desc->stencil_front.fail;
    pd->DepthStencilState.FrontFace.StencilDepthFailOp = (D3D12_STENCIL_OP)desc->stencil_front.depth_fail;
    pd->DepthStencilState.FrontFace.StencilPassOp = (D3D12_STENCIL_OP)desc->stencil_front.pass;
    pd->DepthStencilState.FrontFace.StencilFunc = (D3D12_COMPARISON_FUNC)desc->stencil_front.func;
    pd->DepthStencilState.BackFace.StencilFailOp = (D3D12_STENCIL_OP)desc->stencil_back.fail;
    pd->DepthStencilState.BackFace.StencilDepthFailOp = (D3D12_STENCIL_OP)desc->stencil_back.depth_fail;
    pd->DepthStencilState.BackFace.StencilPassOp = (D3D12_STENCIL_OP)desc->stencil_back.pass;
    pd->DepthStencilState.BackFace.StencilFunc = (D3D12_COMPARISON_FUNC)desc->stencil_back.func;

    pd->IBStripCutValue = (D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)desc->strip_cut;
    pd->PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)desc->topology_type;
    pd->NumRenderTargets = nrt;
    for (UINT r = 0; r < nrt; r++) pd->RTVFormats[r] = (DXGI_FORMAT)desc->rt_format[r];
    pd->DSVFormat = (DXGI_FORMAT)desc->ds_format;
    pd->SampleDesc.Count = desc->samples ? desc->samples : 1;
}

/* A pipeline's name in the library: FNV-1a 64 over the description with its
 * handles zeroed, then each shader's code hash (vs, gs, ps; 0 for none),
 * then the layout's serialized bytes' hash. The same description, shaders
 * and layout give the same name in every run (gpu.h asks callers to zero a
 * description first, so padding hashes the same). */
uint64_t gpu_d3d12_pipe_name(gpu_d3d12_device* dd, const gpu_pipeline_desc* desc)
{
    gpu_pipeline_desc c;
    memcpy(&c, desc, sizeof c);
    c.layout.id = c.vs.id = c.gs.id = c.ps.id = 0;
    uint64_t h = gpu_d3d12_fnv(GPU_D3D12_FNV_BASIS, &c, sizeof c);
    const gpu_shader sh[3] = { desc->vs, desc->gs, desc->ps };
    for (int k = 0; k < 3; k++) {
        const uint64_t s = gpu_d3d12_slot_valid(&dd->shader_slots, sh[k].id)
                         ? dd->shader[GPU_HANDLE_INDEX(sh[k].id)].hash : 0;
        h = gpu_d3d12_fnv(h, &s, sizeof s);
    }
    const uint64_t l = gpu_d3d12_slot_valid(&dd->layout_slots, desc->layout.id)
                     ? dd->layout_hash[GPU_HANDLE_INDEX(desc->layout.id)] : 0;
    return gpu_d3d12_fnv(h, &l, sizeof l);
}

/* Pipelines live as long as the device (gpu.h): kept here, released at
 * gpu_destroy. */
void gpu_d3d12_pipe_keep(gpu_d3d12_device* dd, ID3D12PipelineState* pso)
{
    AcquireSRWLockExclusive(&dd->pso_lock);
    if (dd->npso == dd->pso_cap) {
        const uint32_t cap = dd->pso_cap ? dd->pso_cap * 2 : 1024;
        ID3D12PipelineState** n = (ID3D12PipelineState**)realloc(dd->pso, cap * sizeof *n);
        if (n) {
            dd->pso = n;
            dd->pso_cap = cap;
        }
    }
    if (dd->npso < dd->pso_cap) dd->pso[dd->npso++] = pso;   /* (no memory: it stays alive, unreleased) */
    ReleaseSRWLockExclusive(&dd->pso_lock);
}

void gpu_d3d12_pipe_release(gpu_d3d12_device* dd)
{
    for (uint32_t i = 0; i < dd->npso; i++) dd->pso[i]->lpVtbl->Release(dd->pso[i]);
    free(dd->pso);
    dd->pso = NULL;
    dd->npso = dd->pso_cap = 0;
    if (dd->lib) dd->lib->lpVtbl->Release(dd->lib);
    dd->lib = NULL;
    free(dd->lib_blob);
    dd->lib_blob = NULL;
}

/* Store a pipeline under its name, unless the library has that name. Storing
 * a name the library held (from the file, or stored by another thread in the
 * meantime) made the library's next Serialize read past a buffer in
 * D3D12Core (seen 2026-10-23), so the name is looked up first, under the
 * same lock. 1 if stored. Today's pso_lib_store. */
static int lib_store(gpu_d3d12_device* dd, const WCHAR* name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd,
                     ID3D12PipelineState* pso)
{
    ID3D12PipelineState* had = NULL;
    int stored = 0;
    AcquireSRWLockExclusive(&dd->lib_lock);
    if (!dd->lib) {
        /* (the library went in the meantime) */
    } else if (SUCCEEDED(dd->lib->lpVtbl->LoadGraphicsPipeline(dd->lib, name, pd,
                             &IID_ID3D12PipelineState, (void**)&had))) {
        had->lpVtbl->Release(had);
    } else if (SUCCEEDED(dd->lib->lpVtbl->StorePipeline(dd->lib, name, pso))) {
        stored = 1;
    }
    ReleaseSRWLockExclusive(&dd->lib_lock);
    return stored;
}

/* Today's pso_create_cached: without a library, CreateGraphicsPipelineState;
 * with one, LoadGraphicsPipeline by name under the lock, else create and
 * store. */
gpu_pipeline gpu_d3d12_pipeline_create(gpu_device* d, const gpu_pipeline_desc* desc, uint32_t* from_cache)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_pipeline r = {0};
    if (from_cache) *from_cache = 0;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    D3D12_INPUT_ELEMENT_DESC il[GPU_MAX_VERTEX_ATTRS];
    gpu_d3d12_pipe_d3d_desc(dd, desc, &pd, il);
    ID3D12PipelineState* pso = NULL;
    WCHAR name[20];
    swprintf(name, 20, L"%016llx", (unsigned long long)gpu_d3d12_pipe_name(dd, desc));
    AcquireSRWLockExclusive(&dd->lib_lock);
    const int have_lib = dd->lib != NULL;
    HRESULT hr = have_lib ? dd->lib->lpVtbl->LoadGraphicsPipeline(dd->lib, name, &pd,
                                &IID_ID3D12PipelineState, (void**)&pso)
                          : E_FAIL;
    ReleaseSRWLockExclusive(&dd->lib_lock);
    if (SUCCEEDED(hr) && pso) {
        if (from_cache) *from_cache = 1;
    } else {
        pso = NULL;
        hr = dd->device->lpVtbl->CreateGraphicsPipelineState(dd->device, &pd, &IID_ID3D12PipelineState,
                                                             (void**)&pso);
        if (FAILED(hr) || !pso) return r;
        if (have_lib) lib_store(dd, name, &pd, pso);
    }
    gpu_d3d12_pipe_keep(dd, pso);
    r.v = (uint64_t)(uintptr_t)pso;
    return r;
}

/* Through a temporary name and a rename, so a crash or a second process never
 * leaves a half-written file under the real name. Today's pso_write_file. */
static int write_file(const char* path, const void* p, size_t n)
{
    char tmp[MAX_PATH + 64];
    snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetCurrentThreadId());
    FILE* f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(p, 1, n, f) == n;
    if (fclose(f)) ok = 0;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp); return 0; }
    return 1;
}

static void* read_file(const char* path, long* n)
{
    *n = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    void* p = sz > 0 ? malloc((size_t)sz) : NULL;
    if (p && fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); p = NULL; }
    fclose(f);
    if (p) *n = sz;
    return p;
}

/* Today's pso_lib_open, its library half: the file's library, or, when the
 * file is another driver's or GPU's (or there is none), a new empty one.
 * Called before pipelines are made (it replaces the library). 1 when the
 * file's library is used. */
int gpu_d3d12_pipeline_cache_load(gpu_device* d, const char* path)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    ID3D12Device1* d1 = NULL;
    if (FAILED(dd->device->lpVtbl->QueryInterface(dd->device, &IID_ID3D12Device1, (void**)&d1)))
        return 0;
    long n = 0;
    void* blob = path ? read_file(path, &n) : NULL;
    ID3D12PipelineLibrary* lib = NULL;
    HRESULT hr = E_FAIL;
    if (blob) {
        hr = d1->lpVtbl->CreatePipelineLibrary(d1, blob, (SIZE_T)n,
                 &IID_ID3D12PipelineLibrary, (void**)&lib);
        if (FAILED(hr)) {
            printf("[D3D12] pipeline library: '%s' is from another driver or GPU (0x%08lX); rebuilding it\n",
                   path, (unsigned long)hr);
            free(blob); blob = NULL;
            lib = NULL;
        }
    }
    if (!lib && FAILED(d1->lpVtbl->CreatePipelineLibrary(d1, NULL, 0,
                           &IID_ID3D12PipelineLibrary, (void**)&lib)))
        lib = NULL;
    d1->lpVtbl->Release(d1);
    AcquireSRWLockExclusive(&dd->lib_lock);
    ID3D12PipelineLibrary* const old = dd->lib;
    void* const old_blob = dd->lib_blob;
    dd->lib = lib;
    dd->lib_blob = blob;
    ReleaseSRWLockExclusive(&dd->lib_lock);
    if (old) old->lpVtbl->Release(old);
    free(old_blob);
    return SUCCEEDED(hr) && lib ? 1 : 0;
}

/* Today's pso_lib_save: Serialize can fail when a pipeline is stored between
 * sizing and writing, so it tries three times, sizing again each time. May
 * run on a thread of its own while pipelines are made. 1 when written. */
int gpu_d3d12_pipeline_cache_save(gpu_device* d, const char* path)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    ID3D12PipelineLibrary* const lib = dd->lib;
    if (!lib || !path) return 0;
    for (int tries = 0; tries < 3; tries++) {
        const SIZE_T n = lib->lpVtbl->GetSerializedSize(lib);
        void* buf = malloc(n ? n : 1);
        if (buf && SUCCEEDED(lib->lpVtbl->Serialize(lib, buf, n))) {
            const int ok = write_file(path, buf, n);
            free(buf);
            return ok;
        }
        free(buf);
    }
    return 0;
}

#endif /* PS3RECOMP_RSX_HAL && _WIN32 */
