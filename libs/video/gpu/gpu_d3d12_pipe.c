/*
 * ps3recomp - the portable renderer's GPU layer: the D3D12 backend's pipelines
 *
 * gpu_pipeline_desc to D3D12_GRAPHICS_PIPELINE_STATE_DESC, and today's
 * ID3D12PipelineLibrary around it (rsx_d3d12_backend.c, pso_create_cached and
 * pso_lib_*): a pipeline is loaded from the library by its name, or made and
 * stored under it; the library is made from the file the caller names,
 * mapped, and written to it. Thread-safe: the start's pipeline workers make
 * pipelines beside the render thread, as today's do.
 *
 * Every field a description sets is passed through, so each of today's
 * pipelines comes out as the same D3D12 description, byte for byte except
 * the pointers (gpu_d3d12_pipeline_d3d_desc hands it back for the check).
 *
 * Built with PS3RECOMP_RSX_HAL on Windows; vkd3d comes later.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(_WIN32)

#include "gpu_d3d12_internal.h"

#include <io.h>
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
    plat_file_unmap(&dd->lib_blob);
}

/* A crash inside the library. D3D12 throws at a damaged pipeline in it (seen
 * 2026-11-26: a C++ exception from LoadGraphicsPipeline, which ends the
 * process, as D3D12's own code lets no exception out), and every start after
 * it crashed the same way, since the start's warm-up loads that pipeline. So
 * a thread marks what it does in the library around each call to it; a crash
 * handler that finds a mark on its thread (gpu_d3d12_crash_note) writes the
 * note <library file>.bad. The next pipeline_cache_load deletes the file,
 * starts a new library, and adds the pipeline's name to <library file>.skip:
 * those are never loaded from the library or stored in it again, so neither
 * a damaged file nor a driver that crashes on one good pipeline ends a run
 * twice. */
enum { LIB_IN_NONE = 0, LIB_IN_LOAD, LIB_IN_STORE, LIB_IN_OPEN, LIB_IN_SAVE };
static __declspec(thread) volatile uint32_t t_lib_in;     /* LIB_IN_* */
static __declspec(thread) volatile uint64_t t_lib_name;   /* the pipeline's name, for a load or a store */
static char          s_lib_path[MAX_PATH + 32];          /* the library's file (pipeline_cache_load), or "" */
static volatile LONG s_lib_noted;                        /* a note written */
static int           s_lib_crash_test;                   /* RSX_PSO_LIB_CRASH_TEST=1 */

static void lib_in(uint32_t what, uint64_t name)
{
    t_lib_name = name;
    t_lib_in = what;
}

static int lib_skipped(const gpu_d3d12_device* dd, uint64_t name)
{
    for (uint32_t i = 0; i < dd->nlib_skip; i++)
        if (dd->lib_skip[i] == name) return 1;
    return 0;
}

/* RSX_PSO_LIB_CRASH_TEST=1: the first pipeline loaded from the library ends
 * the process inside the load, through abort, as the throw above did. For
 * testing the note and the start after it (run that one without the knob). */
static void lib_crash_test(uint64_t name)
{
    fprintf(stderr, "[D3D12] RSX_PSO_LIB_CRASH_TEST=1: ending the process inside the load of pipeline %016llx"
            " (test)\n", (unsigned long long)name);
    fflush(stderr);
    abort();
}

int gpu_d3d12_crash_note(char* line, size_t cap)
{
    if (line && cap) line[0] = 0;
    const uint32_t in = t_lib_in;
    const unsigned long long name = t_lib_name;
    if (in == LIB_IN_NONE || in > LIB_IN_SAVE || !s_lib_path[0] || InterlockedExchange(&s_lib_noted, 1)) return 0;
    static const char* const k_in[] = { "", "load", "store", "open", "save" };
    char p[sizeof s_lib_path + 8], text[48];
    snprintf(p, sizeof p, "%s.bad", s_lib_path);
    const int n = snprintf(text, sizeof text, "%s %016llx\n", k_in[in], name);
    const HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wrote = 0;
    const int ok = h != INVALID_HANDLE_VALUE && n > 0 && WriteFile(h, text, (DWORD)n, &wrote, NULL) &&
                   wrote == (DWORD)n && FlushFileBuffers(h);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (line && cap) {
        const char* const next = ok ? "the next start makes a new library"
                                    : "its note for the next start could not be written";
        if (in == LIB_IN_LOAD || in == LIB_IN_STORE)
            snprintf(line, cap, "D3D12 pipeline library: crashed %s pipeline %016llx; %s%s",
                     in == LIB_IN_LOAD ? "loading" : "storing", name, next,
                     ok ? ", and makes that pipeline without it" : "");
        else
            snprintf(line, cap, "D3D12 pipeline library: crashed %s its file; %s",
                     in == LIB_IN_OPEN ? "reading" : "saving", next);
    }
    return 1;
}

/* Store a pipeline under its name, unless the library has that name. Storing
 * a name the library held (from the file, or stored by another thread in the
 * meantime) made the library's next Serialize read past a buffer in
 * D3D12Core (seen 2026-10-23), so the name is looked up first, under the
 * same lock. 1 if stored. Today's pso_lib_store. */
static int lib_store(gpu_d3d12_device* dd, const WCHAR* name, uint64_t nm,
                     const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, ID3D12PipelineState* pso)
{
    ID3D12PipelineState* had = NULL;
    int stored = 0;
    AcquireSRWLockExclusive(&dd->lib_lock);
    if (dd->lib) {   /* (else the library went in the meantime) */
        lib_in(LIB_IN_LOAD, nm);
        if (SUCCEEDED(dd->lib->lpVtbl->LoadGraphicsPipeline(dd->lib, name, pd,
                          &IID_ID3D12PipelineState, (void**)&had))) {
            had->lpVtbl->Release(had);
        } else {
            lib_in(LIB_IN_STORE, nm);
            stored = SUCCEEDED(dd->lib->lpVtbl->StorePipeline(dd->lib, name, pso));
        }
        lib_in(LIB_IN_NONE, 0);
    }
    ReleaseSRWLockExclusive(&dd->lib_lock);
    return stored;
}

/* A name's lock. The library synchronises itself, but two threads must not
 * load the same name at once, as that may change the pipeline's state in the
 * library (ID3D12PipelineLibrary's documentation). So a load holds lib_lock
 * shared and its name's stripe exclusive: loads of other names go on beside
 * it. Before, every load held lib_lock exclusive, so a draw's load waited
 * while the start's warm-up loaded a pipeline whose shaders the driver
 * compiles (30 to 70 ms; docs/97). A store and a new library hold lib_lock
 * exclusive, which keeps out every load. */
static SRWLOCK* lib_stripe(gpu_d3d12_device* dd, uint64_t name)
{
    return &dd->lib_name_lock[(uint32_t)(name ^ (name >> 32)) % GPU_D3D12_LIB_STRIPES];
}

/* Today's pso_create_cached: without a library, CreateGraphicsPipelineState;
 * with one, LoadGraphicsPipeline by name under the lock (lib_stripe), else
 * create and store; a name in lib_skip as without one. GPU_PIPELINE_STORE_ONLY
 * is today's startup and index builds (pso_prebuild_one, pxi_build_one): the
 * pipeline loaded or stored is released at once, as theirs are, so it is not
 * kept beside the one a draw later loads from the library. */
gpu_pipeline gpu_d3d12_pipeline_create(gpu_device* d, const gpu_pipeline_desc* desc, uint32_t flags, uint32_t* how)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_pipeline r = {0};
    uint32_t made;
    if (how) *how = GPU_PIPELINE_FAILED;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    D3D12_INPUT_ELEMENT_DESC il[GPU_MAX_VERTEX_ATTRS];
    gpu_d3d12_pipe_d3d_desc(dd, desc, &pd, il);
    ID3D12PipelineState* pso = NULL;
    WCHAR name[20];
    const uint64_t nm = gpu_d3d12_pipe_name(dd, desc);
    swprintf(name, 20, L"%016llx", (unsigned long long)nm);
    SRWLOCK* const stripe = lib_stripe(dd, nm);
    const int skip = dd->nlib_skip && lib_skipped(dd, nm);
    AcquireSRWLockShared(&dd->lib_lock);
    AcquireSRWLockExclusive(stripe);
    const int have_lib = dd->lib != NULL && !skip;
    HRESULT hr = E_FAIL;
    if (have_lib) {
        lib_in(LIB_IN_LOAD, nm);
        hr = dd->lib->lpVtbl->LoadGraphicsPipeline(dd->lib, name, &pd, &IID_ID3D12PipelineState, (void**)&pso);
        if (s_lib_crash_test && SUCCEEDED(hr)) lib_crash_test(nm);
        lib_in(LIB_IN_NONE, 0);
    }
    ReleaseSRWLockExclusive(stripe);
    ReleaseSRWLockShared(&dd->lib_lock);
    if (SUCCEEDED(hr) && pso) {
        made = GPU_PIPELINE_LOADED;
    } else {
        pso = NULL;
        hr = dd->device->lpVtbl->CreateGraphicsPipelineState(dd->device, &pd, &IID_ID3D12PipelineState,
                                                             (void**)&pso);
        if (FAILED(hr) || !pso) return r;
        made = have_lib && lib_store(dd, name, nm, &pd, pso) ? GPU_PIPELINE_STORED : GPU_PIPELINE_COMPILED;
    }
    if (how) *how = made;
    if (flags & GPU_PIPELINE_STORE_ONLY) {   /* (nothing has recorded it) */
        pso->lpVtbl->Release(pso);
        return r;
    }
    gpu_d3d12_pipe_keep(dd, pso);
    r.v = (uint64_t)(uintptr_t)pso;
    return r;
}

/* Through a temporary name and a rename, so a crash or a second process never
 * leaves a half-written file under the real name. Today's pso_write_file.
 * The rename also replaces the file the library is mapped from (plat.h: a
 * rename over a mapped file is allowed), and the library goes on reading
 * the old one's bytes. The bytes are on the disk before the rename, and the
 * rename before it returns: after a power cut or a hard reset the real name
 * could otherwise hold a file of the right size whose pages were never
 * written, and D3D12 throws at a damaged pipeline (lib_in). */
static int write_file(const char* path, const void* p, size_t n)
{
    char tmp[MAX_PATH + 64];
    snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetCurrentThreadId());
    FILE* f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(p, 1, n, f) == n;
    if (ok && (fflush(f) || _commit(_fileno(f)))) ok = 0;   /* (_commit: FlushFileBuffers) */
    if (fclose(f)) ok = 0;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp);
        return 0;
    }
    return 1;
}

/* pipeline_cache_load's half of the crash note (lib_in): the names made
 * without the library (<path>.skip), and the note a crash left (<path>.bad),
 * whose pipeline joins them; then the file and the note go, and the library
 * is made new. 1 when there was a note. */
static int lib_note_take(gpu_d3d12_device* dd, const char* path)
{
    dd->nlib_skip = 0;
    s_lib_path[0] = 0;
    InterlockedExchange(&s_lib_noted, 0);
    if (!path || strlen(path) >= sizeof s_lib_path) return 0;
    memcpy(s_lib_path, path, strlen(path) + 1);
    char p[sizeof s_lib_path + 8];
    snprintf(p, sizeof p, "%s.skip", path);
    FILE* f = fopen(p, "rb");
    if (f) {
        unsigned long long v;
        while (dd->nlib_skip < GPU_D3D12_LIB_SKIP_MAX && fscanf(f, "%llx", &v) == 1)
            if (!lib_skipped(dd, v)) dd->lib_skip[dd->nlib_skip++] = v;
        fclose(f);
    }
    snprintf(p, sizeof p, "%s.bad", path);
    f = fopen(p, "rb");
    if (!f) {
        if (dd->nlib_skip)
            printf("[D3D12] pipeline library: %u pipeline%s made without it (%s.skip)\n", dd->nlib_skip,
                   dd->nlib_skip == 1 ? "" : "s", path);
        return 0;
    }
    char what[8] = "";
    unsigned long long name = 0;
    const int got = fscanf(f, "%7s %llx", what, &name);
    fclose(f);
    const int named = got == 2 && (!strcmp(what, "load") || !strcmp(what, "store"));
    int kept = named && lib_skipped(dd, name);
    if (named && !kept && dd->nlib_skip < GPU_D3D12_LIB_SKIP_MAX) {
        dd->lib_skip[dd->nlib_skip++] = name;
        kept = 1;
        char txt[GPU_D3D12_LIB_SKIP_MAX * 17 + 1];
        size_t n = 0;
        for (uint32_t i = 0; i < dd->nlib_skip; i++)
            n += (size_t)snprintf(txt + n, sizeof txt - n, "%016llx\n", (unsigned long long)dd->lib_skip[i]);
        char sp[sizeof p];
        snprintf(sp, sizeof sp, "%s.skip", path);
        if (!write_file(sp, txt, n)) printf("[D3D12] pipeline library: could not write %s\n", sp);
    }
    if (named)
        printf("[D3D12] pipeline library: the last run crashed %s pipeline %016llx; a new library, %s (%u so)\n",
               what[0] == 'l' ? "loading" : "storing", name,
               kept ? "and that pipeline made without it from now on" : "the list of such is full", dd->nlib_skip);
    else
        printf("[D3D12] pipeline library: the last run crashed %s %s; a new library\n",
               !strcmp(what, "open") ? "reading" : "saving", path);
    fflush(stdout);
    DeleteFileA(path);   /* (not loaded: the next save writes a new one) */
    DeleteFileA(p);      /* (the note, once its name is kept) */
    return 1;
}

/* Today's pso_lib_open, its library half: the file's library, or, when the
 * file is another driver's or GPU's (or there is none, or the last run
 * crashed in the library: lib_note_take), a new empty one.
 * Called before pipelines are made (it replaces the library).
 * GPU_PIPELINE_CACHE_LOADED when the file's library is used, _NEW for an
 * empty one, _NONE when there can be none (no ID3D12Device1, the cap masked,
 * or the driver makes no library): today's s_pso_lib NULL.
 * D3D12 keeps reading the bytes a library is made from, for as long as it
 * lives, so the file is mapped instead of read into memory: its pages stay
 * the file's (plat_file_map), not tens of MB of private memory. */
int gpu_d3d12_pipeline_cache_load(gpu_device* d, const char* path)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    ID3D12Device1* d1 = NULL;
    if (!(d->caps.bits & GPU_CAP_PIPELINE_CACHE) ||
        FAILED(dd->device->lpVtbl->QueryInterface(dd->device, &IID_ID3D12Device1, (void**)&d1)))
        return GPU_PIPELINE_CACHE_NONE;
    const char* const t = getenv("RSX_PSO_LIB_CRASH_TEST");
    s_lib_crash_test = t && t[0] == '1';
    const int noted = lib_note_take(dd, path);
    plat_file_map blob = { NULL, 0 };
    if (path && !noted) plat_file_map_read(path, &blob);
    ID3D12PipelineLibrary* lib = NULL;
    HRESULT hr = E_FAIL;
    if (blob.p) {
        lib_in(LIB_IN_OPEN, 0);
        hr = d1->lpVtbl->CreatePipelineLibrary(d1, blob.p, (SIZE_T)blob.n,
                 &IID_ID3D12PipelineLibrary, (void**)&lib);
        lib_in(LIB_IN_NONE, 0);
        if (FAILED(hr)) {
            printf("[D3D12] pipeline library: '%s' is from another driver or GPU (0x%08lX); rebuilding it\n",
                   path, (unsigned long)hr);
            plat_file_unmap(&blob);
            lib = NULL;
        }
    }
    if (!lib && FAILED(d1->lpVtbl->CreatePipelineLibrary(d1, NULL, 0,
                           &IID_ID3D12PipelineLibrary, (void**)&lib)))
        lib = NULL;
    d1->lpVtbl->Release(d1);
    AcquireSRWLockExclusive(&dd->lib_lock);
    ID3D12PipelineLibrary* const old = dd->lib;
    plat_file_map old_blob = dd->lib_blob;
    dd->lib = lib;
    dd->lib_blob = blob;
    ReleaseSRWLockExclusive(&dd->lib_lock);
    if (old) old->lpVtbl->Release(old);
    plat_file_unmap(&old_blob);
    if (!lib) return GPU_PIPELINE_CACHE_NONE;
    return SUCCEEDED(hr) ? GPU_PIPELINE_CACHE_LOADED : GPU_PIPELINE_CACHE_NEW;
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
        lib_in(LIB_IN_SAVE, 0);
        const SIZE_T n = lib->lpVtbl->GetSerializedSize(lib);
        void* buf = malloc(n ? n : 1);
        const int got = buf && SUCCEEDED(lib->lpVtbl->Serialize(lib, buf, n));
        lib_in(LIB_IN_NONE, 0);
        if (got) {
            const int ok = write_file(path, buf, n);
            free(buf);
            return ok;
        }
        free(buf);
    }
    return 0;
}

#endif /* PS3RECOMP_RSX_HAL && _WIN32 */
