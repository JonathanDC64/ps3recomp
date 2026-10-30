/*
 * ps3recomp - the portable renderer's GPU layer: the D3D12 backend
 *
 * gpu.h's device functions on D3D12, making the calls today's renderer
 * (rsx_d3d12_backend.c) makes, with the same parameters: its adapter choice,
 * DRED, its descriptor memo, its bounded waits, its write-back heap. The
 * copied ranges are listed in libs/video/rsxcore/copy_manifest.txt. Pipelines
 * are in gpu_d3d12_pipe.c, swap chains in gpu_d3d12_swap.c, the commands in
 * gpu_d3d12_cmd.h; gpu_d3d12_internal.h has the types.
 *
 * Thread-safety is gpu.h's: handle tables are fixed arrays read without a
 * lock, and objects are made and freed under the device's lock, so the start's
 * pipeline workers can make shaders and pipelines beside the render thread.
 *
 * Built with PS3RECOMP_RSX_HAL on Windows; vkd3d (the same code on Linux)
 * comes later.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(_WIN32)

#include "gpu_d3d12_internal.h"
#include "gpu_d3d12_cmd.h"
#include "gpu_d3d12_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

/* gpu_sampler_desc is D3D12_SAMPLER_DESC and gpu_footprint is
 * D3D12_PLACED_SUBRESOURCE_FOOTPRINT, field for field (the contract test
 * checks every field), so both are handed to D3D12 as they are. */

/* ---------------------------------------------------------------------------
 * Handle slots
 * -------------------------------------------------------------------------*/

static int slots_init(gpu_d3d12_slots* s, uint32_t cap)
{
    s->cap = cap;
    s->used = s->nfree = 0;
    s->free = (uint32_t*)malloc(cap * sizeof(uint32_t));
    s->gen = (uint8_t*)calloc(cap, 1);
    s->live = (uint8_t*)calloc(cap, 1);
    return s->free && s->gen && s->live;
}

static void slots_fini(gpu_d3d12_slots* s)
{
    free(s->free);
    free(s->gen);
    free(s->live);
    memset(s, 0, sizeof *s);
}

int gpu_d3d12_slot_valid(const gpu_d3d12_slots* s, uint32_t id)
{
    const uint32_t i = GPU_HANDLE_INDEX(id);
    return id && i < s->used && s->live[i] && s->gen[i] == GPU_HANDLE_GEN(id);
}

uint32_t gpu_d3d12_slot_alloc(gpu_d3d12_slots* s)
{
    uint32_t i;
    if (s->nfree) i = s->free[--s->nfree];
    else if (s->used < s->cap) i = s->used++;
    else return 0;
    s->live[i] = 1;
    return GPU_HANDLE_MAKE(i, s->gen[i]);
}

/* A slot at generation 255 is retired, not reused: a handle names one object
 * for the device's life, so the table memo can compare descriptions. */
void gpu_d3d12_slot_free(gpu_d3d12_slots* s, uint32_t id)
{
    const uint32_t i = GPU_HANDLE_INDEX(id);
    s->live[i] = 0;
    if (s->gen[i] == 255) return;
    s->gen[i]++;
    s->free[s->nfree++] = i;
}

/* ---------------------------------------------------------------------------
 * The backend's own RTV and DSV heaps (CPU only)
 * -------------------------------------------------------------------------*/

static int cpu_heap_init(gpu_d3d12_device* dd, gpu_d3d12_cpu_heap* h, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    memset(h, 0, sizeof *h);
    h->type = type;
    h->inc = dd->device->lpVtbl->GetDescriptorHandleIncrementSize(dd->device, type);
    h->free = (uint64_t*)malloc(sizeof(uint64_t) * GPU_D3D12_CPU_HEAPS * GPU_D3D12_CPU_HEAP_SIZE);
    return h->free != NULL;
}

static void cpu_heap_fini(gpu_d3d12_cpu_heap* h)
{
    for (uint32_t i = 0; i < h->nheaps; i++) h->heap[i]->lpVtbl->Release(h->heap[i]);
    free(h->free);
    memset(h, 0, sizeof *h);
}

/* Under the device's lock. */
static int cpu_heap_alloc(gpu_d3d12_device* dd, gpu_d3d12_cpu_heap* h, uint64_t* out)
{
    if (h->nfree) { *out = h->free[--h->nfree]; return 1; }
    if (!h->nheaps || h->next == GPU_D3D12_CPU_HEAP_SIZE) {
        if (h->nheaps == GPU_D3D12_CPU_HEAPS) return 0;
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        memset(&hd, 0, sizeof hd);
        hd.Type = h->type;
        hd.NumDescriptors = GPU_D3D12_CPU_HEAP_SIZE;
        ID3D12DescriptorHeap* nh = NULL;
        if (FAILED(dd->device->lpVtbl->CreateDescriptorHeap(dd->device, &hd, &IID_ID3D12DescriptorHeap,
                                                            (void**)&nh)) || !nh)
            return 0;
        D3D12_CPU_DESCRIPTOR_HANDLE s;
        nh->lpVtbl->GetCPUDescriptorHandleForHeapStart(nh, &s);
        h->heap[h->nheaps++] = nh;
        h->start = s.ptr;
        h->next = 0;
    }
    *out = h->start + (uint64_t)h->next++ * h->inc;
    return 1;
}

static void cpu_heap_free(gpu_d3d12_cpu_heap* h, uint64_t v)
{
    h->free[h->nfree++] = v;
}

/* ---------------------------------------------------------------------------
 * Freeing, now or once the GPU passes a fence
 * -------------------------------------------------------------------------*/

/* Under lock: a texture, its views and its reference. */
static void tex_free_now(gpu_d3d12_device* dd, uint32_t id)
{
    const uint32_t i = GPU_HANDLE_INDEX(id);
    for (uint32_t v = dd->tex[i].views; v; ) {
        const uint32_t j = v - 1;
        v = dd->view[j].next;
        cpu_heap_free(dd->view[j].kind == GPU_VIEW_DSV ? &dd->dsv_heap : &dd->rtv_heap, dd->view_cpu[j]);
        dd->view_cpu[j] = 0;
        gpu_d3d12_slot_free(&dd->view_slots, GPU_HANDLE_MAKE(j, dd->view_slots.gen[j]));
    }
    if (dd->tex_res[i]) dd->tex_res[i]->lpVtbl->Release(dd->tex_res[i]);
    dd->tex_res[i] = NULL;
    memset(&dd->tex[i], 0, sizeof dd->tex[i]);
    gpu_d3d12_slot_free(&dd->tex_slots, id);
}

/* Under lock. A mapped buffer is unmapped: an adopted one's resource lives on
 * in the old code, with its own map. */
static void buf_free_now(gpu_d3d12_device* dd, uint32_t id)
{
    const uint32_t i = GPU_HANDLE_INDEX(id);
    ID3D12Resource* const r = dd->buf_res[i];
    if (r) {
        if (dd->buf[i].map && dd->buf[i].memory != GPU_MEM_HOST) {
            const D3D12_RANGE none = {0, 0};
            r->lpVtbl->Unmap(r, 0, &none);
        }
        r->lpVtbl->Release(r);
    }
    if (dd->buf[i].host_heap) dd->buf[i].host_heap->lpVtbl->Release(dd->buf[i].host_heap);
    dd->buf_res[i] = NULL;
    dd->buf_va[i] = 0;
    memset(&dd->buf[i], 0, sizeof dd->buf[i]);
    gpu_d3d12_slot_free(&dd->buf_slots, id);
}

static void free_now(gpu_d3d12_device* dd, uint32_t kind, uint32_t id)
{
    if (kind == GPU_D3D12_DEFER_TEXTURE) {
        if (gpu_d3d12_slot_valid(&dd->tex_slots, id)) tex_free_now(dd, id);
    } else if (gpu_d3d12_slot_valid(&dd->buf_slots, id)) {
        buf_free_now(dd, id);
    }
}

/* Frees what the GPU is done with (all of it with all set). */
static void defer_collect(gpu_d3d12_device* dd, int all)
{
    if (!dd->ndefer) return;
    const uint64_t done = dd->fence ? dd->fence->lpVtbl->GetCompletedValue(dd->fence) : UINT64_MAX;
    AcquireSRWLockExclusive(&dd->lock);
    uint32_t k = 0;
    for (uint32_t i = 0; i < dd->ndefer; i++) {
        const gpu_d3d12_defer e = dd->defer[i];
        if (all || e.after <= done) free_now(dd, e.kind, e.id);
        else dd->defer[k++] = e;
    }
    dd->ndefer = k;
    ReleaseSRWLockExclusive(&dd->lock);
}

static void free_after(gpu_d3d12_device* dd, uint32_t kind, uint32_t id, gpu_fence after)
{
    const int now = !after || dd->fence->lpVtbl->GetCompletedValue(dd->fence) >= after;
    AcquireSRWLockExclusive(&dd->lock);
    if (now) {
        free_now(dd, kind, id);
    } else {
        if (dd->ndefer == dd->defer_cap) {
            const uint32_t cap = dd->defer_cap ? dd->defer_cap * 2 : 256;
            gpu_d3d12_defer* n = (gpu_d3d12_defer*)realloc(dd->defer, cap * sizeof *n);
            if (!n) {   /* (no memory: keep it for good rather than free it under the GPU) */
                ReleaseSRWLockExclusive(&dd->lock);
                return;
            }
            dd->defer = n;
            dd->defer_cap = cap;
        }
        dd->defer[dd->ndefer].kind = kind;
        dd->defer[dd->ndefer].id = id;
        dd->defer[dd->ndefer].after = after;
        dd->ndefer++;
    }
    ReleaseSRWLockExclusive(&dd->lock);
}

/* ---------------------------------------------------------------------------
 * Device: DRED, the adapter, caps
 * -------------------------------------------------------------------------*/

/* GPU_DEVICE_BREADCRUMBS (the core sets it from D3D12_DRED=1): Device Removed
 * Extended Data, auto-breadcrumbs and page faults. Part of the D3D12 runtime,
 * not the debug layer; gpu_fault_report prints it once the device is lost.
 * Today's init_d3d12, with its getenv in the flag. */
static int dred_enable(void)
{
    ID3D12DeviceRemovedExtendedDataSettings* ds = NULL;
    if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings,
                                         (void**)&ds)) && ds) {
        ds->lpVtbl->SetAutoBreadcrumbsEnablement(ds, D3D12_DRED_ENABLEMENT_FORCED_ON);
        ds->lpVtbl->SetPageFaultEnablement(ds, D3D12_DRED_ENABLEMENT_FORCED_ON);
        int markers = 0;
        /* DRED 1.2: keep the strings of the per-draw markers (gpu_cmd_marker). */
        ID3D12DeviceRemovedExtendedDataSettings1* ds1 = NULL;
        const char* text = ", without the draws' markers";
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings1,
                                             (void**)&ds1)) && ds1) {
            ds1->lpVtbl->SetBreadcrumbContextEnablement(ds1, D3D12_DRED_ENABLEMENT_FORCED_ON);
            ds1->lpVtbl->Release(ds1);
            text = ", with the draws' markers";
            markers = 1;
        }
        ds->lpVtbl->Release(ds);
        printf("[D3D12] DRED enabled%s\n", text);
        return 1 + markers;
    }
    printf("[D3D12] DRED unavailable\n");
    return 0;
}

static int adapter_try(gpu_d3d12_device* dd, IDXGIAdapter1* adapter)
{
    if (FAILED(D3D12CreateDevice((IUnknown*)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                                 (void**)&dd->device)))
        return 0;
    DXGI_ADAPTER_DESC1 ad;
    adapter->lpVtbl->GetDesc1(adapter, &ad);
    printf("[D3D12] adapter: %ls (VRAM %llu MB)\n", ad.Description,
           (unsigned long long)(ad.DedicatedVideoMemory >> 20));
    dd->adapter = adapter;
    return 1;
}

/* The adapter as today's init_d3d12 picks it: on a dual-GPU laptop a NULL
 * adapter usually lands on the integrated GPU, so the high-performance
 * (discrete) one is asked for through IDXGIFactory6, or the low-power one
 * with GPU_DEVICE_LOW_POWER (CELLMARK_IGPU); software adapters are skipped.
 * New beside it: GPU_DEVICE_USE_LUID first tries that adapter, and
 * GPU_DEVICE_WARP takes WARP and nothing else (tests and CI). */
static int adapter_pick(gpu_d3d12_device* dd, const gpu_device_desc* desc)
{
    IDXGIFactory4* const factory = dd->factory;
    IDXGIAdapter1* adapter = NULL;
    if (desc->flags & GPU_DEVICE_WARP) {
        if (SUCCEEDED(factory->lpVtbl->EnumWarpAdapter(factory, &IID_IDXGIAdapter1, (void**)&adapter)) &&
            adapter && !adapter_try(dd, adapter)) {
            adapter->lpVtbl->Release(adapter);
            adapter = NULL;
        }
        if (!dd->device) printf("[D3D12] ERROR: no WARP device\n");
        return dd->device != NULL;
    }
    if (desc->flags & GPU_DEVICE_USE_LUID) {
        LUID luid;
        memcpy(&luid, desc->luid, sizeof luid);
        if (SUCCEEDED(factory->lpVtbl->EnumAdapterByLuid(factory, luid, &IID_IDXGIAdapter1, (void**)&adapter)) &&
            adapter && !adapter_try(dd, adapter)) {
            adapter->lpVtbl->Release(adapter);
            adapter = NULL;
        }
        if (dd->device) return 1;
    }
    {
        IDXGIFactory6* factory6 = NULL;
        if (SUCCEEDED(factory->lpVtbl->QueryInterface(
                factory, &IID_IDXGIFactory6, (void**)&factory6))) {
            DXGI_GPU_PREFERENCE pref = (desc->flags & GPU_DEVICE_LOW_POWER)
                ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
            for (UINT ai = 0; factory6->lpVtbl->EnumAdapterByGpuPreference(
                     factory6, ai, pref, &IID_IDXGIAdapter1, (void**)&adapter)
                     != DXGI_ERROR_NOT_FOUND; ai++) {
                DXGI_ADAPTER_DESC1 ad;
                adapter->lpVtbl->GetDesc1(adapter, &ad);
                if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && adapter_try(dd, adapter))
                    break;
                adapter->lpVtbl->Release(adapter);
                adapter = NULL;
            }
            factory6->lpVtbl->Release(factory6);
        }
    }
    if (!dd->device) {
        HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                                       &IID_ID3D12Device, (void**)&dd->device);
        if (FAILED(hr)) {
            printf("[D3D12] ERROR: D3D12CreateDevice failed (0x%08lX)\n", (unsigned long)hr);
            dd->device = NULL;
            return 0;
        }
        printf("[D3D12] Device created on default adapter (feature level 11.0)\n");
        /* Its adapter, for gpu_adapter and gpu_native. */
        LUID luid;
        dd->device->lpVtbl->GetAdapterLuid(dd->device, &luid);
        if (FAILED(factory->lpVtbl->EnumAdapterByLuid(factory, luid, &IID_IDXGIAdapter1,
                                                      (void**)&dd->adapter)))
            dd->adapter = NULL;
    }
    return 1;
}

static uint32_t fmt_support(gpu_d3d12_device* dd, gpu_format format, uint32_t samples)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs;
    memset(&fs, 0, sizeof fs);
    fs.Format = (DXGI_FORMAT)format;
    if (FAILED(dd->device->lpVtbl->CheckFeatureSupport(dd->device, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)))
        return 0;
    uint32_t r = 0;
    if (fs.Support1 & (D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)) r |= GPU_FMT_SAMPLED;
    if (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) r |= GPU_FMT_FILTER;
    if (fs.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) r |= GPU_FMT_RT;
    if (fs.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) r |= GPU_FMT_BLEND;
    if (fs.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) r |= GPU_FMT_DS;
    if (fs.Support1 & D3D12_FORMAT_SUPPORT1_DISPLAY) r |= GPU_FMT_DISPLAY;
    if (samples <= 1) {
        if (r & (GPU_FMT_RT | GPU_FMT_DS)) r |= GPU_FMT_MSAA;
    } else {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ms;
        memset(&ms, 0, sizeof ms);
        ms.Format = (DXGI_FORMAT)format;
        ms.SampleCount = samples;
        if (SUCCEEDED(dd->device->lpVtbl->CheckFeatureSupport(dd->device, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                                              &ms, sizeof ms)) && ms.NumQualityLevels)
            r |= GPU_FMT_MSAA;
    }
    return r;
}

/* Feature bits. D3D12 at feature level 11.0 has most of them; the rest are
 * asked. GPU_CAP_UNUSED_ATTACHMENTS: today's renderer binds targets that its
 * pipelines do not name, which D3D12 drivers take. */
static void caps_read(gpu_d3d12_device* dd, const gpu_device_desc* desc)
{
    gpu_caps* const c = &dd->base.caps;
    uint32_t b = GPU_CAP_GS | GPU_CAP_DEPTH_CLAMP | GPU_CAP_MIRROR_ONCE | GPU_CAP_BORDER | GPU_CAP_ANISO16 |
                 GPU_CAP_SWIZZLE | GPU_CAP_OCCLUSION_PRECISE | GPU_CAP_NULL_DESCRIPTOR |
                 GPU_CAP_PRESENT_MODE_SWITCH | GPU_CAP_HDR_SCRGB | GPU_CAP_HDR_METADATA |
                 GPU_CAP_UNUSED_ATTACHMENTS;
    if (fmt_support(dd, GPU_FORMAT_D24_UNORM_S8_UINT, 1) & GPU_FMT_DS) b |= GPU_CAP_D24S8;
    if (fmt_support(dd, GPU_FORMAT_BC1_UNORM, 1) & GPU_FMT_SAMPLED) b |= GPU_CAP_BC;
    {
        const uint32_t f = fmt_support(dd, GPU_FORMAT_R32G32B32A32_FLOAT, 1);
        if (f & GPU_FMT_BLEND) b |= GPU_CAP_RGBA32F_BLEND;
        if (f & GPU_FMT_FILTER) b |= GPU_CAP_RGBA32F_FILTER;
    }
    {   /* Today's swap chain check: tearing needs the OS and the driver. */
        IDXGIFactory5* f5 = NULL;
        BOOL allow = FALSE;
        if (SUCCEEDED(dd->factory->lpVtbl->QueryInterface(dd->factory, &IID_IDXGIFactory5, (void**)&f5))) {
            if (FAILED(f5->lpVtbl->CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                       &allow, sizeof allow)))
                allow = FALSE;
            f5->lpVtbl->Release(f5);
        }
        if (allow) b |= GPU_CAP_TEARING;
    }
    {
        ID3D12Device3* d3 = NULL;
        if (SUCCEEDED(dd->device->lpVtbl->QueryInterface(dd->device, &IID_ID3D12Device3, (void**)&d3)) && d3) {
            b |= GPU_CAP_HOST_IMPORT;
            d3->lpVtbl->Release(d3);
        }
        ID3D12Device1* d1 = NULL;
        if (SUCCEEDED(dd->device->lpVtbl->QueryInterface(dd->device, &IID_ID3D12Device1, (void**)&d1)) && d1) {
            b |= GPU_CAP_PIPELINE_CACHE;
            d1->lpVtbl->Release(d1);
        }
    }
    if (dd->dred) b |= GPU_CAP_DEVICE_FAULT;
    if (dd->dred > 1) b |= GPU_CAP_BREADCRUMBS;
    b &= ~desc->cap_mask;
    c->api = GPU_API_D3D12;
    c->bits = b;
    c->cb_align = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    c->max_aniso = 16;
    c->host_import_align = (b & GPU_CAP_HOST_IMPORT) ? 65536u : 0u;
}

/* ---------------------------------------------------------------------------
 * Device functions
 * -------------------------------------------------------------------------*/

static int tables_init(gpu_d3d12_device* dd)
{
    int ok = slots_init(&dd->tex_slots, GPU_D3D12_MAX_TEXTURES) &&
             slots_init(&dd->buf_slots, GPU_D3D12_MAX_BUFFERS) &&
             slots_init(&dd->view_slots, GPU_D3D12_MAX_VIEWS) &&
             slots_init(&dd->samp_slots, GPU_D3D12_MAX_SAMPLERS) &&
             slots_init(&dd->shader_slots, GPU_D3D12_MAX_SHADERS) &&
             slots_init(&dd->layout_slots, GPU_D3D12_MAX_LAYOUTS) &&
             slots_init(&dd->qpool_slots, GPU_D3D12_MAX_QPOOLS) &&
             slots_init(&dd->heap_slots, GPU_D3D12_MAX_HEAPS) &&
             slots_init(&dd->tpool_slots, GPU_D3D12_MAX_TPOOLS) &&
             slots_init(&dd->swap_slots, GPU_D3D12_MAX_SWAPS);
    if (!ok) return 0;
    dd->tex_res = (ID3D12Resource**)calloc(GPU_D3D12_MAX_TEXTURES, sizeof *dd->tex_res);
    dd->tex = (gpu_d3d12_tex*)calloc(GPU_D3D12_MAX_TEXTURES, sizeof *dd->tex);
    dd->buf_res = (ID3D12Resource**)calloc(GPU_D3D12_MAX_BUFFERS, sizeof *dd->buf_res);
    dd->buf_va = (uint64_t*)calloc(GPU_D3D12_MAX_BUFFERS, sizeof *dd->buf_va);
    dd->buf = (gpu_d3d12_buf*)calloc(GPU_D3D12_MAX_BUFFERS, sizeof *dd->buf);
    dd->view_cpu = (uint64_t*)calloc(GPU_D3D12_MAX_VIEWS, sizeof *dd->view_cpu);
    dd->view = (gpu_d3d12_view*)calloc(GPU_D3D12_MAX_VIEWS, sizeof *dd->view);
    dd->samp = (D3D12_SAMPLER_DESC*)calloc(GPU_D3D12_MAX_SAMPLERS, sizeof *dd->samp);
    dd->samp_hash_cap = GPU_D3D12_MAX_SAMPLERS * 2;
    dd->samp_hash = (uint32_t*)calloc(dd->samp_hash_cap, sizeof *dd->samp_hash);
    dd->shader = (gpu_d3d12_shader*)calloc(GPU_D3D12_MAX_SHADERS, sizeof *dd->shader);
    dd->layout_rs = (ID3D12RootSignature**)calloc(GPU_D3D12_MAX_LAYOUTS, sizeof *dd->layout_rs);
    dd->layout_hash = (uint64_t*)calloc(GPU_D3D12_MAX_LAYOUTS, sizeof *dd->layout_hash);
    dd->heap = (ID3D12Heap**)calloc(GPU_D3D12_MAX_HEAPS, sizeof *dd->heap);
    return dd->tex_res && dd->tex && dd->buf_res && dd->buf_va && dd->buf && dd->view_cpu && dd->view &&
           dd->samp && dd->samp_hash && dd->shader && dd->layout_rs && dd->layout_hash && dd->heap;
}

static void tables_fini(gpu_d3d12_device* dd)
{
    slots_fini(&dd->tex_slots);
    slots_fini(&dd->buf_slots);
    slots_fini(&dd->view_slots);
    slots_fini(&dd->samp_slots);
    slots_fini(&dd->shader_slots);
    slots_fini(&dd->layout_slots);
    slots_fini(&dd->qpool_slots);
    slots_fini(&dd->heap_slots);
    slots_fini(&dd->tpool_slots);
    slots_fini(&dd->swap_slots);
    free(dd->tex_res);
    free(dd->tex);
    free(dd->buf_res);
    free(dd->buf_va);
    free(dd->buf);
    free(dd->view_cpu);
    free(dd->view);
    free(dd->samp);
    free(dd->samp_hash);
    free(dd->shader);
    free(dd->layout_rs);
    free(dd->layout_hash);
    free(dd->heap);
    free(dd->defer);
}

static void d12_wait_idle(gpu_device* d);

/* Everything the device holds, then the device: gpu_destroy, and a failed
 * gpu_d3d12_create. */
static void device_free(gpu_d3d12_device* dd)
{
    if (dd->queue && dd->fence) d12_wait_idle(&dd->base);
    defer_collect(dd, 1);
    for (uint32_t i = 0; i < dd->swap_slots.used; i++)
        if (dd->swap_slots.live[i]) {
            gpu_swapchain sc;
            sc.id = GPU_HANDLE_MAKE(i, dd->swap_slots.gen[i]);
            gpu_d3d12_swap_destroy(&dd->base, sc);
        }
    for (uint32_t i = 0; i < dd->tex_slots.used; i++)
        if (dd->tex_slots.live[i]) tex_free_now(dd, GPU_HANDLE_MAKE(i, dd->tex_slots.gen[i]));
    for (uint32_t i = 0; i < dd->buf_slots.used; i++)
        if (dd->buf_slots.live[i]) buf_free_now(dd, GPU_HANDLE_MAKE(i, dd->buf_slots.gen[i]));
    for (uint32_t i = 0; i < dd->shader_slots.used; i++) free(dd->shader[i].code);
    for (uint32_t i = 0; i < dd->layout_slots.used; i++)
        if (dd->layout_rs[i]) dd->layout_rs[i]->lpVtbl->Release(dd->layout_rs[i]);
    for (uint32_t i = 0; i < GPU_D3D12_MAX_QPOOLS; i++)
        if (dd->qpool[i].heap) dd->qpool[i].heap->lpVtbl->Release(dd->qpool[i].heap);
    for (uint32_t i = 0; i < dd->heap_slots.used; i++)
        if (dd->heap[i]) dd->heap[i]->lpVtbl->Release(dd->heap[i]);
    for (uint32_t i = 0; i < GPU_D3D12_MAX_TPOOLS; i++) {
        if (dd->tpool[i].heap) dd->tpool[i].heap->lpVtbl->Release(dd->tpool[i].heap);
        free(dd->tpool[i].memo);
    }
    gpu_d3d12_pipe_release(dd);
    cpu_heap_fini(&dd->rtv_heap);
    cpu_heap_fini(&dd->dsv_heap);
    for (uint32_t i = 0; i < dd->nslots; i++) {
        if (dd->slot[i].cl) dd->slot[i].cl->lpVtbl->Release(dd->slot[i].cl);
        if (dd->slot[i].alloc) dd->slot[i].alloc->lpVtbl->Release(dd->slot[i].alloc);
    }
    if (dd->fence) dd->fence->lpVtbl->Release(dd->fence);
    if (dd->queue) dd->queue->lpVtbl->Release(dd->queue);
    if (dd->device) dd->device->lpVtbl->Release(dd->device);
    if (dd->adapter) dd->adapter->lpVtbl->Release(dd->adapter);
    if (dd->factory) dd->factory->lpVtbl->Release(dd->factory);
    tables_fini(dd);
    free(dd);
}

void gpu_d3d12_cmd_init(gpu_d3d12_device* dd, gpu_cmd* c, ID3D12GraphicsCommandList* cl,
                        ID3D12CommandAllocator* alloc)
{
    c->head.ops = &gpu_d3d12_cmd_ops;
    c->head.dev = &dd->base;
    c->cl = cl;
    c->tex = dd->tex_res;
    c->buf = dd->buf_res;
    c->view = dd->view_cpu;
    c->layout = dd->layout_rs;
    c->tpool = dd->tpool;
    c->qpool = dd->qpool;
    c->alloc = alloc;
}

/* The device's objects, as today's init_d3d12 makes them; 0 on failure (the
 * caller frees what was made). */
static int device_init(gpu_d3d12_device* dd, const gpu_device_desc* desc)
{
    if (!tables_init(dd)) return 0;
    if (desc->flags & GPU_DEVICE_BREADCRUMBS) dd->dred = dred_enable();

    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&dd->factory);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateDXGIFactory1 failed (0x%08lX)\n", (unsigned long)hr);
        dd->factory = NULL;
        return 0;
    }
    if (!adapter_pick(dd, desc)) return 0;

    D3D12_COMMAND_QUEUE_DESC queue_desc;
    memset(&queue_desc, 0, sizeof queue_desc);
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = dd->device->lpVtbl->CreateCommandQueue(
        dd->device, &queue_desc, &IID_ID3D12CommandQueue, (void**)&dd->queue);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateCommandQueue failed\n");
        dd->queue = NULL;
        return 0;
    }
    hr = dd->device->lpVtbl->CreateFence(dd->device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                                         (void**)&dd->fence);
    if (FAILED(hr)) { dd->fence = NULL; return 0; }

    /* A command allocator and a list (made closed) per recorder: today's
     * renderer has an allocator per frame and one list. */
    dd->nslots = desc->cmd_slots ? desc->cmd_slots : 2;
    if (dd->nslots > GPU_D3D12_MAX_CMD_SLOTS) dd->nslots = GPU_D3D12_MAX_CMD_SLOTS;
    for (uint32_t i = 0; i < dd->nslots; i++) {
        ID3D12CommandAllocator* alloc = NULL;
        ID3D12GraphicsCommandList* cl = NULL;
        if (FAILED(dd->device->lpVtbl->CreateCommandAllocator(dd->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                       &IID_ID3D12CommandAllocator, (void**)&alloc)))
            return 0;
        dd->slot[i].alloc = alloc;
        if (FAILED(dd->device->lpVtbl->CreateCommandList(dd->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                       &IID_ID3D12GraphicsCommandList, (void**)&cl)))
            return 0;
        cl->lpVtbl->Close(cl);
        gpu_d3d12_cmd_init(dd, &dd->slot[i], cl, alloc);
    }

    dd->srv_inc = dd->device->lpVtbl->GetDescriptorHandleIncrementSize(dd->device,
                                                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    dd->samp_inc = dd->device->lpVtbl->GetDescriptorHandleIncrementSize(dd->device,
                                                                        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    if (!cpu_heap_init(dd, &dd->rtv_heap, D3D12_DESCRIPTOR_HEAP_TYPE_RTV) ||
        !cpu_heap_init(dd, &dd->dsv_heap, D3D12_DESCRIPTOR_HEAP_TYPE_DSV))
        return 0;
    caps_read(dd, desc);
    return 1;
}

static void d12_destroy(gpu_device* d)
{
    if (d) device_free(GPU_D3D12_DEV(d));
}

static void d12_adapter(gpu_device* d, gpu_adapter_info* out)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    memset(out, 0, sizeof *out);
    if (!dd->adapter) return;
    DXGI_ADAPTER_DESC1 ad;
    if (FAILED(dd->adapter->lpVtbl->GetDesc1(dd->adapter, &ad))) return;
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, out->name, (int)sizeof out->name, NULL, NULL);
    out->vendor = ad.VendorId;
    out->device = ad.DeviceId;
    out->subsystem = ad.SubSysId;
    out->revision = ad.Revision;
    out->vram = (uint64_t)ad.DedicatedVideoMemory;
    memcpy(out->luid, &ad.AdapterLuid, sizeof out->luid);
    LARGE_INTEGER umd;
    if (SUCCEEDED(dd->adapter->lpVtbl->CheckInterfaceSupport(dd->adapter, &IID_IDXGIDevice, &umd))) {
        out->driver_version = (uint64_t)umd.QuadPart;
        snprintf(out->driver, sizeof out->driver, "%u.%u.%u.%u", (unsigned)HIWORD(umd.HighPart),
                 (unsigned)LOWORD(umd.HighPart), (unsigned)HIWORD(umd.LowPart), (unsigned)LOWORD(umd.LowPart));
    }
}

/* GetDeviceRemovedReason; the first reason is kept, as today's check_device
 * keeps it. Any thread. */
static uint32_t d12_lost(gpu_device* d)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    if (dd->lost) return (uint32_t)dd->lost;
    const HRESULT rr = dd->device->lpVtbl->GetDeviceRemovedReason(dd->device);
    if (rr == S_OK) return 0;
    InterlockedCompareExchange(&dd->lost, (LONG)rr, 0);
    return (uint32_t)dd->lost;
}

static uint32_t d12_format_support(gpu_device* d, gpu_format format, uint32_t samples)
{
    return fmt_support(GPU_D3D12_DEV(d), format, samples);
}

/* ---------------------------------------------------------------------------
 * Buffers
 * -------------------------------------------------------------------------*/

static gpu_buffer buf_add(gpu_d3d12_device* dd, ID3D12Resource* res, uint64_t size, uint32_t memory, void* map,
                          ID3D12Heap* host_heap)
{
    gpu_buffer r = {0};
    AcquireSRWLockExclusive(&dd->lock);
    r.id = gpu_d3d12_slot_alloc(&dd->buf_slots);
    if (r.id) {
        const uint32_t i = GPU_HANDLE_INDEX(r.id);
        dd->buf_res[i] = res;
        dd->buf_va[i] = res->lpVtbl->GetGPUVirtualAddress(res);
        dd->buf[i].size = size;
        dd->buf[i].map = (uint8_t*)map;
        dd->buf[i].memory = memory;
        dd->buf[i].host_heap = host_heap;
    }
    ReleaseSRWLockExclusive(&dd->lock);
    if (!r.id) {
        if (map && memory != GPU_MEM_HOST) res->lpVtbl->Unmap(res, 0, NULL);
        res->lpVtbl->Release(res);
        if (host_heap) host_heap->lpVtbl->Release(host_heap);
    }
    return r;
}

static void buf_rd(D3D12_RESOURCE_DESC* bd, uint64_t size)
{
    memset(bd, 0, sizeof *bd);
    bd->Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd->Width = size;
    bd->Height = 1;
    bd->DepthOrArraySize = 1;
    bd->MipLevels = 1;
    bd->SampleDesc.Count = 1;
    bd->Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
}

/* Today's buffers: committed, no flags; upload ones mapped once with an
 * empty read range and kept mapped. A readback buffer is mapped for its life
 * the same way, and gpu_buffer_map maps it again over the range read. */
static gpu_buffer d12_buffer_create(gpu_device* d, const gpu_buffer_desc* desc)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    if (desc->memory < GPU_MEM_GPU || desc->memory > GPU_MEM_READBACK || !desc->size) return (gpu_buffer){0};
    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof hp);
    hp.Type = (D3D12_HEAP_TYPE)desc->memory;
    D3D12_RESOURCE_DESC bd;
    buf_rd(&bd, desc->size);
    ID3D12Resource* res = NULL;
    if (FAILED(dd->device->lpVtbl->CreateCommittedResource(dd->device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                   (D3D12_RESOURCE_STATES)desc->state, NULL, &IID_ID3D12Resource, (void**)&res)) || !res)
        return (gpu_buffer){0};
    void* map = NULL;
    if (desc->memory != GPU_MEM_GPU) {
        const D3D12_RANGE nr = {0, 0};
        if (FAILED(res->lpVtbl->Map(res, 0, &nr, &map))) map = NULL;
    }
    return buf_add(dd, res, desc->size, desc->memory, map, NULL);
}

/* Today's write-back heap (wbg_ready): the caller's memory must be an
 * allocation of its own (RSX local memory is), all of it is committed (a page
 * the guest never wrote reads zero, as on first touch), and the buffer is
 * placed on a heap opened over it, in COPY_DEST. */
static gpu_buffer d12_buffer_import_host(gpu_device* d, void* host, uint64_t size)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    if (!(d->caps.bits & GPU_CAP_HOST_IMPORT) || !host || !size) return (gpu_buffer){0};
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(host, &mi, sizeof mi) || mi.AllocationBase != host) {
        fprintf(stderr, "[D3D12] host memory at %p is not an allocation of its own; no buffer over it\n", host);
        return (gpu_buffer){0};
    }
    if (!VirtualAlloc(host, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE)) return (gpu_buffer){0};
    ID3D12Device3* d3 = NULL;
    if (FAILED(dd->device->lpVtbl->QueryInterface(dd->device, &IID_ID3D12Device3, (void**)&d3)) || !d3)
        return (gpu_buffer){0};
    ID3D12Heap* heap = NULL;
    HRESULT hr = d3->lpVtbl->OpenExistingHeapFromAddress(d3, host, &IID_ID3D12Heap, (void**)&heap);
    d3->lpVtbl->Release(d3);
    if (FAILED(hr) || !heap) {
        fprintf(stderr, "[D3D12] no heap on host memory (0x%08lX)\n", (unsigned long)hr);
        return (gpu_buffer){0};
    }
    D3D12_RESOURCE_DESC bd;
    buf_rd(&bd, size);
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    ID3D12Resource* res = NULL;
    hr = dd->device->lpVtbl->CreatePlacedResource(dd->device, heap, 0, &bd,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&res);
    if (FAILED(hr) || !res) {
        fprintf(stderr, "[D3D12] no buffer on host memory (0x%08lX)\n", (unsigned long)hr);
        heap->lpVtbl->Release(heap);
        return (gpu_buffer){0};
    }
    return buf_add(dd, res, size, GPU_MEM_HOST, host, heap);
}

static void d12_buffer_destroy(gpu_device* d, gpu_buffer b, gpu_fence after)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    if (gpu_d3d12_slot_valid(&dd->buf_slots, b.id)) free_after(dd, GPU_D3D12_DEFER_BUFFER, b.id, after);
}

/* A readback buffer is mapped again over the range read, so D3D12 makes it
 * visible to the CPU (today's Map(0, {0, n})); the rest hand back their
 * pointer. */
static void* d12_buffer_map(gpu_device* d, gpu_buffer b, uint64_t read_offset, uint64_t read_size)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    if (!gpu_d3d12_slot_valid(&dd->buf_slots, b.id)) return NULL;
    const uint32_t i = GPU_HANDLE_INDEX(b.id);
    if (dd->buf[i].memory == GPU_MEM_READBACK) {
        const D3D12_RANGE rr = {(SIZE_T)read_offset, (SIZE_T)(read_offset + read_size)};
        void* p = NULL;
        ID3D12Resource* const r = dd->buf_res[i];
        return SUCCEEDED(r->lpVtbl->Map(r, 0, &rr, &p)) ? p : NULL;
    }
    return dd->buf[i].map;
}

static void d12_buffer_unmap(gpu_device* d, gpu_buffer b, uint64_t written_offset, uint64_t written_size)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    if (!gpu_d3d12_slot_valid(&dd->buf_slots, b.id)) return;
    const uint32_t i = GPU_HANDLE_INDEX(b.id);
    if (dd->buf[i].memory == GPU_MEM_READBACK) {   /* the map above's other half */
        const D3D12_RANGE wr = {(SIZE_T)written_offset, (SIZE_T)(written_offset + written_size)};
        dd->buf_res[i]->lpVtbl->Unmap(dd->buf_res[i], 0, &wr);
    }
    /* Upload memory stays mapped: D3D12 reads a written range only for tools. */
}

static gpu_addr d12_buffer_addr(gpu_device* d, gpu_buffer b)
{
    return gpu_fast_buffer_addr(d, b);
}

/* ---------------------------------------------------------------------------
 * Heaps and textures
 * -------------------------------------------------------------------------*/

static gpu_heap d12_heap_create(gpu_device* d, const gpu_heap_desc* desc)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_heap r = {0};
    D3D12_HEAP_DESC hd = {0};
    hd.SizeInBytes = desc->size;
    hd.Properties.Type = (D3D12_HEAP_TYPE)(desc->memory ? desc->memory : GPU_MEM_GPU);
    hd.Flags = (D3D12_HEAP_FLAGS)desc->flags;
    ID3D12Heap* h = NULL;
    if (FAILED(dd->device->lpVtbl->CreateHeap(dd->device, &hd, &IID_ID3D12Heap, (void**)&h)) || !h)
        return r;
    AcquireSRWLockExclusive(&dd->lock);
    r.id = gpu_d3d12_slot_alloc(&dd->heap_slots);
    if (r.id) dd->heap[GPU_HANDLE_INDEX(r.id)] = h;
    ReleaseSRWLockExclusive(&dd->lock);
    if (!r.id) h->lpVtbl->Release(h);
    return r;
}

/* Today's texture descriptions: 2D, Alignment 0, Layout UNKNOWN, and only the
 * target flags (ALLOW_RENDER_TARGET, ALLOW_DEPTH_STENCIL). */
static void tex_rd(const gpu_texture_desc* t, D3D12_RESOURCE_DESC* rd)
{
    memset(rd, 0, sizeof *rd);
    rd->Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd->Width = t->width;
    rd->Height = t->height;
    rd->DepthOrArraySize = t->layers ? t->layers : 1;
    rd->MipLevels = t->mips ? t->mips : 1;
    rd->Format = (DXGI_FORMAT)t->format;
    rd->SampleDesc.Count = t->samples ? t->samples : 1;
    rd->Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (t->use & GPU_USE_RT) rd->Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (t->use & GPU_USE_DS) rd->Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
}

/* Today's tex_res_create asks GetResourceAllocationInfo(0, 1, &desc). */
static void d12_texture_size(gpu_device* d, const gpu_texture_desc* desc, uint64_t* size, uint64_t* align)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    D3D12_RESOURCE_DESC rd;
    tex_rd(desc, &rd);
    D3D12_RESOURCE_ALLOCATION_INFO ai;
    dd->device->lpVtbl->GetResourceAllocationInfo(dd->device, &ai, 0, 1, &rd);
    if (size) *size = ai.SizeInBytes;
    if (align) *align = ai.Alignment;
}

static uint64_t d12_texture_footprints(gpu_device* d, const gpu_texture_desc* desc, uint32_t first_sub, uint32_t n,
                                       uint64_t base_offset, gpu_footprint* out, uint32_t* rows,
                                       uint64_t* row_bytes)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    D3D12_RESOURCE_DESC rd;
    tex_rd(desc, &rd);
    UINT64 total = 0;
    dd->device->lpVtbl->GetCopyableFootprints(dd->device, &rd, first_sub, n, base_offset,
                                              (D3D12_PLACED_SUBRESOURCE_FOOTPRINT*)out, (UINT*)rows,
                                              (UINT64*)row_bytes, &total);
    return (uint64_t)total;
}

gpu_texture gpu_d3d12_tex_add(gpu_d3d12_device* dd, ID3D12Resource* res, const gpu_texture_desc* desc,
                              uint32_t flags)
{
    gpu_texture r = {0};
    AcquireSRWLockExclusive(&dd->lock);
    r.id = gpu_d3d12_slot_alloc(&dd->tex_slots);
    if (r.id) {
        const uint32_t i = GPU_HANDLE_INDEX(r.id);
        gpu_d3d12_tex* const t = &dd->tex[i];
        dd->tex_res[i] = res;
        t->width = desc->width;
        t->height = desc->height;
        t->layers = desc->layers ? desc->layers : 1;
        t->mips = desc->mips ? desc->mips : 1;
        t->format = desc->format;
        t->samples = desc->samples ? desc->samples : 1;
        t->use = desc->use;
        t->views = 0;
        t->flags = flags;
    }
    ReleaseSRWLockExclusive(&dd->lock);
    if (!r.id) res->lpVtbl->Release(res);
    return r;
}

void gpu_d3d12_tex_free(gpu_d3d12_device* dd, gpu_texture t)
{
    AcquireSRWLockExclusive(&dd->lock);
    if (gpu_d3d12_slot_valid(&dd->tex_slots, t.id)) tex_free_now(dd, t.id);
    ReleaseSRWLockExclusive(&dd->lock);
}

/* Committed (DEFAULT heap, no heap flags), or placed in a heap at an offset
 * (today's texture pool), in the state asked, with the clear value asked. */
static gpu_texture d12_texture_create(gpu_device* d, const gpu_texture_desc* desc)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    D3D12_RESOURCE_DESC rd;
    tex_rd(desc, &rd);
    D3D12_CLEAR_VALUE cv;
    const D3D12_CLEAR_VALUE* pcv = NULL;
    if (desc->clear_format) {
        memset(&cv, 0, sizeof cv);
        cv.Format = (DXGI_FORMAT)desc->clear_format;
        if (desc->use & GPU_USE_DS) {
            cv.DepthStencil.Depth = desc->clear_depth;
            cv.DepthStencil.Stencil = (UINT8)desc->clear_stencil;
        } else {
            memcpy(cv.Color, desc->clear_color, sizeof cv.Color);
        }
        pcv = &cv;
    }
    ID3D12Resource* res = NULL;
    HRESULT hr;
    if (GPU_VALID(desc->heap)) {
        if (!gpu_d3d12_slot_valid(&dd->heap_slots, desc->heap.id)) return (gpu_texture){0};
        hr = dd->device->lpVtbl->CreatePlacedResource(dd->device, dd->heap[GPU_HANDLE_INDEX(desc->heap.id)],
                 desc->heap_offset, &rd, (D3D12_RESOURCE_STATES)desc->state, pcv, &IID_ID3D12Resource,
                 (void**)&res);
    } else {
        D3D12_HEAP_PROPERTIES hp;
        memset(&hp, 0, sizeof hp);
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hr = dd->device->lpVtbl->CreateCommittedResource(dd->device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                 (D3D12_RESOURCE_STATES)desc->state, pcv, &IID_ID3D12Resource, (void**)&res);
    }
    if (FAILED(hr) || !res) return (gpu_texture){0};
    return gpu_d3d12_tex_add(dd, res, desc, 0);
}

static void d12_texture_destroy(gpu_device* d, gpu_texture t, gpu_fence after)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    if (!gpu_d3d12_slot_valid(&dd->tex_slots, t.id)) return;
    if (dd->tex[GPU_HANDLE_INDEX(t.id)].flags & GPU_D3D12_TEX_SWAP) return;   /* its swap chain's */
    free_after(dd, GPU_D3D12_DEFER_TEXTURE, t.id, after);
}

/* ---------------------------------------------------------------------------
 * Target views
 * -------------------------------------------------------------------------*/

/* The format a depth resource's view takes, from its TYPELESS format. */
static DXGI_FORMAT depth_view_format(gpu_format f)
{
    switch (f) {
    case GPU_FORMAT_R24G8_TYPELESS:    return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case GPU_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case GPU_FORMAT_R32_TYPELESS:      return DXGI_FORMAT_D32_FLOAT;
    case 53 /* R16_TYPELESS */:        return DXGI_FORMAT_D16_UNORM;
    default:                           return (DXGI_FORMAT)f;
    }
}

/* Today's RTVs are made with a NULL description (the whole of mip 0 in the
 * texture's own format); DSVs always spell theirs out. */
static void view_make(gpu_d3d12_device* dd, const gpu_d3d12_tex* t, ID3D12Resource* res, const gpu_view_desc* v,
                      uint64_t cpu)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    h.ptr = (SIZE_T)cpu;
    const int arr = t->layers > 1 || v->layer || v->dim == GPU_DIM_2D_ARRAY || v->dim == GPU_DIM_CUBE;
    const int ms = t->samples > 1 || v->dim == GPU_DIM_2DMS;
    if (v->kind == GPU_VIEW_RTV) {
        if (!v->format && !v->mip && !v->layer && v->dim == GPU_DIM_2D && !arr && !ms) {
            dd->device->lpVtbl->CreateRenderTargetView(dd->device, res, NULL, h);
            return;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rv;
        memset(&rv, 0, sizeof rv);
        rv.Format = (DXGI_FORMAT)(v->format ? v->format : t->format);
        if (ms && arr) {
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            rv.Texture2DMSArray.FirstArraySlice = v->layer;
            rv.Texture2DMSArray.ArraySize = 1;
        } else if (ms) {
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        } else if (arr) {
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rv.Texture2DArray.MipSlice = v->mip;
            rv.Texture2DArray.FirstArraySlice = v->layer;
            rv.Texture2DArray.ArraySize = 1;
        } else {
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            rv.Texture2D.MipSlice = v->mip;
        }
        dd->device->lpVtbl->CreateRenderTargetView(dd->device, res, &rv, h);
        return;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dv;
    memset(&dv, 0, sizeof dv);
    dv.Format = v->format ? (DXGI_FORMAT)v->format : depth_view_format(t->format);
    dv.Flags = (D3D12_DSV_FLAGS)v->flags;
    if (ms && arr) {
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
        dv.Texture2DMSArray.FirstArraySlice = v->layer;
        dv.Texture2DMSArray.ArraySize = 1;
    } else if (ms) {
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    } else if (arr) {
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dv.Texture2DArray.MipSlice = v->mip;
        dv.Texture2DArray.FirstArraySlice = v->layer;
        dv.Texture2DArray.ArraySize = 1;
    } else {
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dv.Texture2D.MipSlice = v->mip;
    }
    dd->device->lpVtbl->CreateDepthStencilView(dd->device, res, &dv, h);
}

/* Made once per description and kept with the texture: its list of views
 * is the cache (a texture has one to three). */
static gpu_view d12_view_get(gpu_device* d, const gpu_view_desc* desc)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_view r = {0};
    if (desc->kind != GPU_VIEW_RTV && desc->kind != GPU_VIEW_DSV) return r;
    AcquireSRWLockExclusive(&dd->lock);
    if (gpu_d3d12_slot_valid(&dd->tex_slots, desc->texture.id)) {
        const uint32_t ti = GPU_HANDLE_INDEX(desc->texture.id);
        gpu_d3d12_tex* const t = &dd->tex[ti];
        for (uint32_t v = t->views; v; v = dd->view[v - 1].next)
            if (!memcmp(&dd->view[v - 1].desc, desc, sizeof *desc)) {
                r.id = GPU_HANDLE_MAKE(v - 1, dd->view_slots.gen[v - 1]);
                break;
            }
        gpu_d3d12_cpu_heap* const heap = desc->kind == GPU_VIEW_DSV ? &dd->dsv_heap : &dd->rtv_heap;
        uint64_t cpu;
        if (!r.id && cpu_heap_alloc(dd, heap, &cpu)) {
            r.id = gpu_d3d12_slot_alloc(&dd->view_slots);
            if (r.id) {
                const uint32_t j = GPU_HANDLE_INDEX(r.id);
                view_make(dd, t, dd->tex_res[ti], desc, cpu);
                dd->view_cpu[j] = cpu;
                dd->view[j].desc = *desc;
                dd->view[j].kind = desc->kind;
                dd->view[j].next = t->views;
                t->views = j + 1;
            } else {
                cpu_heap_free(heap, cpu);
            }
        }
    }
    ReleaseSRWLockExclusive(&dd->lock);
    return r;
}

/* ---------------------------------------------------------------------------
 * Samplers
 * -------------------------------------------------------------------------*/

/* Made once per description (a D3D12 sampler is only its description,
 * written into a table by gpu_table_write_sampler). */
static gpu_sampler d12_sampler_get(gpu_device* d, const gpu_sampler_desc* desc)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_sampler r = {0};
    const uint32_t mask = dd->samp_hash_cap - 1;
    uint32_t i = (uint32_t)gpu_d3d12_fnv(GPU_D3D12_FNV_BASIS, desc, sizeof *desc) & mask;
    AcquireSRWLockExclusive(&dd->lock);
    for (;; i = (i + 1) & mask) {
        const uint32_t id = dd->samp_hash[i];
        if (!id) break;
        if (!memcmp(&dd->samp[GPU_HANDLE_INDEX(id)], desc, sizeof *desc)) { r.id = id; break; }
    }
    if (!r.id) {
        r.id = gpu_d3d12_slot_alloc(&dd->samp_slots);
        if (r.id) {
            memcpy(&dd->samp[GPU_HANDLE_INDEX(r.id)], desc, sizeof *desc);
            dd->samp_hash[i] = r.id;
        }
    }
    ReleaseSRWLockExclusive(&dd->lock);
    return r;
}

/* ---------------------------------------------------------------------------
 * Tables
 * -------------------------------------------------------------------------*/

/* The empty entry a new texture pool holds: today's srv_heap is filled with
 * R8_UNORM 2D null SRVs (init_d3d12). */
static void null_r8(gpu_view_desc* v)
{
    memset(v, 0, sizeof *v);
    v->mips = 1;
    v->format = GPU_FORMAT_R8_UNORM;
    v->swizzle = GPU_SWIZZLE_IDENTITY;
}

static gpu_table_pool tpool_add(gpu_d3d12_device* dd, ID3D12DescriptorHeap* heap, const gpu_table_pool_desc* desc,
                                int owned)
{
    gpu_table_pool r = {0};
    const int samp = desc->kind == GPU_TABLE_SAMPLER;
    void* memo = calloc(desc->count ? desc->count : 1, samp ? sizeof(uint32_t) : sizeof(gpu_view_desc));
    if (memo) {
        AcquireSRWLockExclusive(&dd->lock);
        r.id = gpu_d3d12_slot_alloc(&dd->tpool_slots);
        ReleaseSRWLockExclusive(&dd->lock);
    }
    if (!r.id) {
        free(memo);
        heap->lpVtbl->Release(heap);
        return r;
    }
    gpu_d3d12_tpool* const p = &dd->tpool[GPU_HANDLE_INDEX(r.id)];
    D3D12_CPU_DESCRIPTOR_HANDLE c;
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &c);
    heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &g);
    p->cpu = c.ptr;
    p->gpu = g.ptr;
    p->inc = samp ? dd->samp_inc : dd->srv_inc;
    p->count = desc->count;
    p->kind = desc->kind;
    p->owned = (uint32_t)owned;
    p->memo = memo;
    p->desc = *desc;
    if (!samp) {
        gpu_view_desc* const m = (gpu_view_desc*)memo;
        if (owned) {
            D3D12_SHADER_RESOURCE_VIEW_DESC nv;
            memset(&nv, 0, sizeof nv);
            nv.Format = DXGI_FORMAT_R8_UNORM;
            nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE hh = c;
            for (uint32_t k = 0; k < desc->count; k++) {
                dd->device->lpVtbl->CreateShaderResourceView(dd->device, NULL, &nv, hh);
                hh.ptr += p->inc;
                null_r8(&m[k]);
            }
        } else {
            for (uint32_t k = 0; k < desc->count; k++) m[k].kind = 0xFF;   /* unknown: never matches */
        }
    }
    p->heap = heap;   /* last: set_table_pools reads it */
    return r;
}

static gpu_table_pool d12_table_pool_create(gpu_device* d, const gpu_table_pool_desc* desc)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    memset(&hd, 0, sizeof hd);
    hd.Type = desc->kind == GPU_TABLE_SAMPLER ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                                              : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = desc->count;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* heap = NULL;
    const HRESULT hr = dd->device->lpVtbl->CreateDescriptorHeap(dd->device, &hd, &IID_ID3D12DescriptorHeap,
                                                                (void**)&heap);
    if (FAILED(hr) || !heap) {
        printf("[D3D12] descriptor heap of %u failed (0x%08lX)\n", desc->count, (unsigned long)hr);
        return (gpu_table_pool){0};
    }
    return tpool_add(dd, heap, desc, 1);
}

static int is_typeless(gpu_format f)
{
    switch (f) {
    case 1: case 5: case 9: case 15: case 19: case 23: case 27: case 33: case 39: case 44: case 48:
    case 53: case 60: case 70: case 73: case 76: case 79: case 82: case 90: case 92: case 94: case 97:
        return 1;
    default:
        return 0;
    }
}

/* The format a TYPELESS texture is read in when the view does not say. */
static DXGI_FORMAT read_format(gpu_format f)
{
    switch (f) {
    case GPU_FORMAT_R24G8_TYPELESS:    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case GPU_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case GPU_FORMAT_R32_TYPELESS:      return DXGI_FORMAT_R32_FLOAT;
    case 53 /* R16_TYPELESS */:        return DXGI_FORMAT_R16_UNORM;
    case GPU_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:                           return (DXGI_FORMAT)f;
    }
}

/* The write the memo did not skip (gpu_fast_table_write_tex). v's swizzle is
 * never 0 here. A description with every field at its default on a plain
 * texture is written with a NULL description, as today's renderer writes the
 * frame's and the targets' views; the rest spell it out, as srv_write_ex
 * does. */
void gpu_d3d12_write_srv(gpu_d3d12_device* dd, gpu_d3d12_tpool* p, uint32_t index, const gpu_view_desc* v)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    h.ptr = (SIZE_T)(p->cpu + (uint64_t)index * p->inc);
    ID3D12Resource* res = NULL;
    const gpu_d3d12_tex* t = NULL;
    if (GPU_VALID(v->texture) && gpu_d3d12_slot_valid(&dd->tex_slots, v->texture.id)) {
        t = &dd->tex[GPU_HANDLE_INDEX(v->texture.id)];
        res = dd->tex_res[GPU_HANDLE_INDEX(v->texture.id)];
    }
    ((gpu_view_desc*)p->memo)[index] = *v;
    dd->n_srv_writes++;
    if (res && !v->format && v->swizzle == GPU_SWIZZLE_IDENTITY && !v->mips && !v->mip && !v->layer &&
        v->dim == GPU_DIM_2D && t->layers == 1 && t->samples == 1 && !is_typeless(t->format)) {
        dd->device->lpVtbl->CreateShaderResourceView(dd->device, res, NULL, h);
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;
    memset(&sv, 0, sizeof sv);
    sv.Format = v->format ? (DXGI_FORMAT)v->format
              : t ? read_format(t->format) : DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.Shader4ComponentMapping = v->swizzle;
    const UINT mips = v->mips ? v->mips : res ? (UINT)-1 : 1u;
    switch (v->dim) {
    case GPU_DIM_CUBE:   /* must match the TextureCube declaration in the HLSL */
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MostDetailedMip = v->mip;
        sv.TextureCube.MipLevels = mips;
        sv.TextureCube.ResourceMinLODClamp = 0.0f;
        break;
    case GPU_DIM_2DMS:
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        break;
    case GPU_DIM_2D_ARRAY:
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sv.Texture2DArray.MostDetailedMip = v->mip;
        sv.Texture2DArray.MipLevels = mips;
        sv.Texture2DArray.FirstArraySlice = v->layer;
        sv.Texture2DArray.ArraySize = t && t->layers > v->layer ? (UINT)(t->layers - v->layer) : 1u;
        break;
    default:
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MostDetailedMip = v->mip;
        sv.Texture2D.MipLevels = mips;
        break;
    }
    dd->device->lpVtbl->CreateShaderResourceView(dd->device, res, &sv, h);
}

void gpu_d3d12_write_sampler(gpu_d3d12_device* dd, gpu_d3d12_tpool* p, uint32_t index, gpu_sampler s)
{
    if (!gpu_d3d12_slot_valid(&dd->samp_slots, s.id)) return;
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    h.ptr = (SIZE_T)(p->cpu + (uint64_t)index * p->inc);
    ((uint32_t*)p->memo)[index] = s.id;
    dd->n_sampler_writes++;
    dd->device->lpVtbl->CreateSampler(dd->device, &dd->samp[GPU_HANDLE_INDEX(s.id)], h);
}

static void d12_table_write_tex(gpu_device* d, gpu_table_pool pool, uint32_t index, const gpu_view_desc* srv)
{
    gpu_fast_table_write_tex(d, pool, index, srv);
}

static void d12_table_write_sampler(gpu_device* d, gpu_table_pool pool, uint32_t index, gpu_sampler s)
{
    gpu_fast_table_write_sampler(d, pool, index, s);
}

static gpu_table d12_table_ref(gpu_device* d, gpu_table_pool pool, uint32_t index)
{
    return gpu_fast_table_ref(d, pool, index);
}

/* ---------------------------------------------------------------------------
 * Shaders, layouts, queries
 * -------------------------------------------------------------------------*/

/* D3D12 takes DXBC (the compile service's dxbc target, which needs no
 * device) and DXIL. */
static int d12_compile_hook(gpu_device* d, const void* request, void* result)
{
    (void)d; (void)request; (void)result;
    return 0;
}

static gpu_shader d12_shader_create(gpu_device* d, const gpu_shader_desc* desc)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_shader r = {0};
    if ((desc->code != GPU_CODE_DXBC && desc->code != GPU_CODE_DXIL) || !desc->bytes || !desc->size) return r;
    void* code = malloc((size_t)desc->size);
    if (!code) return r;
    memcpy(code, desc->bytes, (size_t)desc->size);
    AcquireSRWLockExclusive(&dd->lock);
    r.id = gpu_d3d12_slot_alloc(&dd->shader_slots);
    if (r.id) {
        gpu_d3d12_shader* const s = &dd->shader[GPU_HANDLE_INDEX(r.id)];
        s->code = code;
        s->size = desc->size;
        s->hash = gpu_d3d12_fnv(GPU_D3D12_FNV_BASIS, code, (size_t)desc->size);
        s->stage = desc->stage;
    }
    ReleaseSRWLockExclusive(&dd->lock);
    if (!r.id) free(code);
    return r;
}

static void d12_shader_destroy(gpu_device* d, gpu_shader s)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    AcquireSRWLockExclusive(&dd->lock);
    if (gpu_d3d12_slot_valid(&dd->shader_slots, s.id)) {
        gpu_d3d12_shader* const sh = &dd->shader[GPU_HANDLE_INDEX(s.id)];
        free(sh->code);
        memset(sh, 0, sizeof *sh);
        gpu_d3d12_slot_free(&dd->shader_slots, s.id);
    }
    ReleaseSRWLockExclusive(&dd->lock);
}

static D3D12_STATIC_BORDER_COLOR border_of(const float b[4])
{
    if (b[3] == 0.0f) return D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    return b[0] >= 0.5f ? D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE : D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
}

/* A root signature as today's are written: each slot one root parameter in
 * register space 0, a table one range with OFFSET_APPEND, serialized at
 * version 1.0. The serialized bytes' hash names the pipelines made on it, as
 * s_vp_root_hash names today's. */
static gpu_layout d12_layout_create(gpu_device* d, const gpu_layout_desc* desc)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_layout r = {0};
    D3D12_ROOT_PARAMETER rp[GPU_MAX_SLOTS];
    D3D12_DESCRIPTOR_RANGE rg[GPU_MAX_SLOTS];
    D3D12_STATIC_SAMPLER_DESC ss[GPU_MAX_STATIC_SAMPLERS];
    memset(rp, 0, sizeof rp);
    memset(rg, 0, sizeof rg);
    memset(ss, 0, sizeof ss);
    const UINT n = desc->nslots < GPU_MAX_SLOTS ? desc->nslots : GPU_MAX_SLOTS;
    const UINT ns = desc->nstatic < GPU_MAX_STATIC_SAMPLERS ? desc->nstatic : GPU_MAX_STATIC_SAMPLERS;
    for (UINT i = 0; i < n; i++) {
        const gpu_layout_slot* s = &desc->slot[i];
        switch (s->kind) {
        case GPU_SLOT_CB:
            rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            rp[i].Descriptor.ShaderRegister = s->reg;
            break;
        case GPU_SLOT_RAW:
            rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            rp[i].Descriptor.ShaderRegister = s->reg;
            break;
        case GPU_SLOT_CONST32:
            rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[i].Constants.ShaderRegister = s->reg;
            rp[i].Constants.Num32BitValues = s->count;
            break;
        default:   /* the two tables */
            rg[i].RangeType = s->kind == GPU_SLOT_TABLE_SAMPLER ? D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
                                                                : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            rg[i].NumDescriptors = s->count;
            rg[i].BaseShaderRegister = s->reg;
            rg[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[i].DescriptorTable.NumDescriptorRanges = 1;
            rp[i].DescriptorTable.pDescriptorRanges = &rg[i];
            break;
        }
        rp[i].ShaderVisibility = (D3D12_SHADER_VISIBILITY)s->visibility;
    }
    for (UINT k = 0; k < ns; k++) {
        const gpu_static_sampler* s = &desc->statics[k];
        ss[k].Filter = (D3D12_FILTER)s->desc.filter;
        ss[k].AddressU = (D3D12_TEXTURE_ADDRESS_MODE)s->desc.address_u;
        ss[k].AddressV = (D3D12_TEXTURE_ADDRESS_MODE)s->desc.address_v;
        ss[k].AddressW = (D3D12_TEXTURE_ADDRESS_MODE)s->desc.address_w;
        ss[k].MipLODBias = s->desc.mip_lod_bias;
        ss[k].MaxAnisotropy = s->desc.max_aniso;
        ss[k].ComparisonFunc = (D3D12_COMPARISON_FUNC)s->desc.compare;
        ss[k].BorderColor = border_of(s->desc.border);
        ss[k].MinLOD = s->desc.min_lod;
        ss[k].MaxLOD = s->desc.max_lod;
        ss[k].ShaderRegister = s->reg;
        ss[k].RegisterSpace = 0;
        ss[k].ShaderVisibility = (D3D12_SHADER_VISIBILITY)s->visibility;
    }
    D3D12_ROOT_SIGNATURE_DESC rd;
    rd.NumParameters = n;
    rd.pParameters = n ? rp : NULL;
    rd.NumStaticSamplers = ns;
    rd.pStaticSamplers = ns ? ss : NULL;
    rd.Flags = (D3D12_ROOT_SIGNATURE_FLAGS)desc->flags;
    ID3DBlob* sig = NULL;
    ID3DBlob* err = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        printf("[D3D12] root signature: %s\n", err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) err->lpVtbl->Release(err);
        return r;
    }
    if (err) err->lpVtbl->Release(err);
    ID3D12RootSignature* rs = NULL;
    hr = dd->device->lpVtbl->CreateRootSignature(dd->device, 0, sig->lpVtbl->GetBufferPointer(sig),
                                                 sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature,
                                                 (void**)&rs);
    const uint64_t hash = gpu_d3d12_fnv(GPU_D3D12_FNV_BASIS, sig->lpVtbl->GetBufferPointer(sig),
                                        sig->lpVtbl->GetBufferSize(sig));
    sig->lpVtbl->Release(sig);
    if (FAILED(hr) || !rs) return r;
    AcquireSRWLockExclusive(&dd->lock);
    r.id = gpu_d3d12_slot_alloc(&dd->layout_slots);
    if (r.id) {
        dd->layout_rs[GPU_HANDLE_INDEX(r.id)] = rs;
        dd->layout_hash[GPU_HANDLE_INDEX(r.id)] = hash;
    }
    ReleaseSRWLockExclusive(&dd->lock);
    if (!r.id) rs->lpVtbl->Release(rs);
    return r;
}

static gpu_query_pool d12_query_pool_create(gpu_device* d, uint32_t type, uint32_t count)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_query_pool r = {0};
    if (type != GPU_QUERY_OCCLUSION && type != GPU_QUERY_BINARY_OCCLUSION) return r;
    D3D12_QUERY_HEAP_DESC qd;
    memset(&qd, 0, sizeof qd);
    qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    qd.Count = count;
    ID3D12QueryHeap* heap = NULL;
    if (FAILED(dd->device->lpVtbl->CreateQueryHeap(dd->device, &qd, &IID_ID3D12QueryHeap, (void**)&heap)) || !heap)
        return r;
    AcquireSRWLockExclusive(&dd->lock);
    r.id = gpu_d3d12_slot_alloc(&dd->qpool_slots);
    if (r.id) {
        gpu_d3d12_qpool* const q = &dd->qpool[GPU_HANDLE_INDEX(r.id)];
        q->type = (D3D12_QUERY_TYPE)type;
        q->count = count;
        q->heap = heap;
    }
    ReleaseSRWLockExclusive(&dd->lock);
    if (!r.id) heap->lpVtbl->Release(heap);
    return r;
}

/* ---------------------------------------------------------------------------
 * Recording, submitting, the timeline
 * -------------------------------------------------------------------------*/

/* Today's allocator Reset, then the list's Reset(allocator, NULL). */
static gpu_cmd* d12_cmd_begin(gpu_device* d, uint32_t slot)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    if (slot >= dd->nslots) return NULL;
    defer_collect(dd, 0);
    gpu_cmd* const c = &dd->slot[slot];
    c->alloc->lpVtbl->Reset(c->alloc);
    c->cl->lpVtbl->Reset(c->cl, c->alloc, NULL);
    return c;
}

static void d12_cmd_end(gpu_device* d, gpu_cmd* c)
{
    (void)d;
    c->cl->lpVtbl->Close(c->cl);
}

static gpu_fence d12_signal(gpu_device* d);

static gpu_fence d12_submit(gpu_device* d, gpu_cmd* const* cmds, uint32_t n, const gpu_submit_desc* how)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    ID3D12CommandList* lists[16];
    uint32_t done = 0;
    while (done < n) {
        const uint32_t k = n - done < 16 ? n - done : 16;
        for (uint32_t i = 0; i < k; i++) lists[i] = (ID3D12CommandList*)cmds[done + i]->cl;
        dd->queue->lpVtbl->ExecuteCommandLists(dd->queue, k, lists);
        done += k;
    }
    /* The swap chain options need nothing here: DXGI orders a present after
     * the queue's work. */
    return how && (how->flags & GPU_SUBMIT_SIGNAL) ? d12_signal(d) : 0;
}

/* One fence for the device. Values are signalled in order under a lock, so
 * two threads' signals never step it back. */
static gpu_fence d12_signal(gpu_device* d)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    AcquireSRWLockExclusive(&dd->signal_lock);
    const uint64_t v = (uint64_t)++dd->fence_value;
    dd->queue->lpVtbl->Signal(dd->queue, dd->fence, v);
    ReleaseSRWLockExclusive(&dd->signal_lock);
    return v;
}

static uint64_t d12_fence_completed(gpu_device* d)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    return dd->fence->lpVtbl->GetCompletedValue(dd->fence);
}

/* Today's waits: SetEventOnCompletion on an auto-reset event and a bounded
 * WaitForSingleObject (a lost device never signals; its fence reads
 * UINT64_MAX, which passes every value). The event is the thread's own, so
 * several threads may wait; an earlier wait that timed out can still set it,
 * so the fence is read again after each wake. */
static __declspec(thread) HANDLE t_fence_event;

static int d12_fence_wait(gpu_device* d, gpu_fence f, uint32_t timeout_ms)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    if (dd->fence->lpVtbl->GetCompletedValue(dd->fence) >= f) return 0;
    if (!t_fence_event) t_fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    const ULONGLONG end = GetTickCount64() + timeout_ms;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        const DWORD left = now >= end ? 0 : (DWORD)(end - now);
        if (t_fence_event &&
            SUCCEEDED(dd->fence->lpVtbl->SetEventOnCompletion(dd->fence, f, t_fence_event)))
            WaitForSingleObject(t_fence_event, left);
        else
            Sleep(left < 1 ? left : 1);
        if (dd->fence->lpVtbl->GetCompletedValue(dd->fence) >= f) return 0;
        if (GetTickCount64() >= end) return 1;
    }
}

/* Two-second waits until the GPU is done or the device is lost. */
static void d12_wait_idle(gpu_device* d)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    const gpu_fence f = d12_signal(d);
    while (d12_fence_wait(d, f, 2000))
        if (d12_lost(d)) break;
    defer_collect(dd, 0);
}

/* ---------------------------------------------------------------------------
 * Diagnostics: today's DRED report
 * -------------------------------------------------------------------------*/

/* One command list of DRED's history: its op count, and for a list the GPU
 * had not finished, the ops around the first one not done, what came before
 * it, and (DRED 1.2) the text of the markers gpu_cmd_marker put before each
 * draw. ctx: D3D12_DRED_BREADCRUMB_CONTEXT[nctx], or none. */
static void dred_list(const void* list, uint32_t count, const UINT* lastp,
                      const D3D12_AUTO_BREADCRUMB_OP* hist, const void* ctx, uint32_t nctx)
{
    static const char* nm[] = { "SETMARKER","BEGINEVENT","ENDEVENT","DRAWINSTANCED",
        "DRAWINDEXEDINSTANCED","EXECUTEINDIRECT","DISPATCH","COPYBUFFERREGION",
        "COPYTEXTUREREGION","COPYRESOURCE","COPYTILES","RESOLVESUBRESOURCE",
        "CLEARRENDERTARGETVIEW","CLEARUNORDEREDACCESSVIEW","CLEARDEPTHSTENCILVIEW",
        "RESOURCEBARRIER" };
    const uint32_t last = lastp ? *lastp : 0;
    fprintf(stderr, "[DRED] list %p: %u ops, last completed %u, %u markers%c",
            list, count, last, nctx, 10);
    if (last >= count || !hist) return;   /* list finished */
    /* The text of op k's marker, if it has one. */
    const D3D12_DRED_BREADCRUMB_CONTEXT* cx = (const D3D12_DRED_BREADCRUMB_CONTEXT*)ctx;
#define DRED_TEXT_AT(k, out) do { out = NULL; \
        for (uint32_t _i = 0; cx && _i < nctx; _i++) \
            if (cx[_i].BreadcrumbIndex == (k)) { out = cx[_i].pContextString; break; } } while (0)
    uint32_t draws = 0, clears = 0, copies = 0, barriers = 0;
    for (uint32_t k = 0; k < last; k++) {
        const uint32_t op = (uint32_t)hist[k];
        if (op == 3 || op == 4) draws++;
        else if (op >= 12 && op <= 14) clears++;
        else if (op >= 7 && op <= 11) copies++;
        else if (op == 15) barriers++;
    }
    fprintf(stderr, "[DRED]   done before it: %u draws, %u clears, %u copies, %u barriers%c",
            draws, clears, copies, barriers, 10);
    /* The GPU works on several draws at once: the first one not done is
     * where it stopped, or a draw or two after it. */
    const uint32_t a = last > 16 ? last - 16 : 0, b = last + 24 < count ? last + 24 : count;
    for (uint32_t k = a; k < b; k++) {
        const uint32_t op = (uint32_t)hist[k];
        const wchar_t* t;
        DRED_TEXT_AT(k, t);
        if (t)
            fprintf(stderr, "[DRED]   %s%5u %s: %ls%c", k == last ? ">>" : "  ", k,
                    op < sizeof nm / sizeof nm[0] ? nm[op] : "op", t, 10);
        else
            fprintf(stderr, "[DRED]   %s%5u %s (%u)%c", k == last ? ">>" : "  ", k,
                    op < sizeof nm / sizeof nm[0] ? nm[op] : "op", op, 10);
    }
    for (uint32_t k = last + 1; k-- > 0; ) {   /* the marker of the draw not done */
        const wchar_t* t;
        DRED_TEXT_AT(k, t);
        if (t) { fprintf(stderr, "[DRED] the GPU stopped at or just after: %ls%c", t, 10); break; }
    }
#undef DRED_TEXT_AT
}

/* On the first device removal, with GPU_DEVICE_BREADCRUMBS: the reason, the
 * command history around the last completed breadcrumb, and the page-fault
 * address. Today's dred_report, once per device. */
static void d12_fault_report(gpu_device* d, const char* where)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    if (dd->fault_done || !dd->dred || !dd->device) return;
    HRESULT rr = dd->device->lpVtbl->GetDeviceRemovedReason(dd->device);
    if (rr == S_OK) return;
    if (InterlockedExchange(&dd->fault_done, 1)) return;
    fprintf(stderr, "[DRED] device removed at %s: reason 0x%08lX%c", where, (unsigned long)rr, 10);
    ID3D12DeviceRemovedExtendedData* dr = NULL;
    if (FAILED(dd->device->lpVtbl->QueryInterface(dd->device,
            &IID_ID3D12DeviceRemovedExtendedData, (void**)&dr)) || !dr) {
        fprintf(stderr, "[DRED] no DRED interface%c", 10); return;
    }
    int listed = 0;
    {   /* DRED 1.2 has the markers' text too. */
        ID3D12DeviceRemovedExtendedData1* d1 = NULL;
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bo1 = {0};
        if (SUCCEEDED(dd->device->lpVtbl->QueryInterface(dd->device,
                &IID_ID3D12DeviceRemovedExtendedData1, (void**)&d1)) && d1) {
            if (SUCCEEDED(d1->lpVtbl->GetAutoBreadcrumbsOutput1(d1, &bo1))) {
                listed = 1;
                fprintf(stderr, "[DRED] version 1.2 output%c", 10);
                for (const D3D12_AUTO_BREADCRUMB_NODE1* n = bo1.pHeadAutoBreadcrumbNode; n; n = n->pNext)
                    dred_list((const void*)n->pCommandList, n->BreadcrumbCount, n->pLastBreadcrumbValue,
                              n->pCommandHistory, n->pBreadcrumbContexts, n->BreadcrumbContextsCount);
            }
            d1->lpVtbl->Release(d1);
        }
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bo = {0};
    if (!listed && SUCCEEDED(dr->lpVtbl->GetAutoBreadcrumbsOutput(dr, &bo))) {
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bo.pHeadAutoBreadcrumbNode; n; n = n->pNext)
            dred_list((const void*)n->pCommandList, n->BreadcrumbCount, n->pLastBreadcrumbValue,
                      n->pCommandHistory, NULL, 0);
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT po = {0};
    if (SUCCEEDED(dr->lpVtbl->GetPageFaultAllocationOutput(dr, &po))) {
        if (po.PageFaultVA)
            fprintf(stderr, "[DRED] page fault VA 0x%llX%c", (unsigned long long)po.PageFaultVA, 10);
        else
            fprintf(stderr, "[DRED] page fault VA 0x0 (none recorded)%c", 10);
        for (const D3D12_DRED_ALLOCATION_NODE* a = po.pHeadExistingAllocationNode; a; a = a->pNext)
            fprintf(stderr, "[DRED]   existing alloc type %d %s%c", (int)a->AllocationType,
                    a->ObjectNameA ? a->ObjectNameA : "", 10);
        for (const D3D12_DRED_ALLOCATION_NODE* a = po.pHeadRecentFreedAllocationNode; a; a = a->pNext)
            fprintf(stderr, "[DRED]   freed alloc type %d %s%c", (int)a->AllocationType,
                    a->ObjectNameA ? a->ObjectNameA : "", 10);
    }
    dr->lpVtbl->Release(dr);
    fflush(stderr);
}

/* The API's own objects, borrowed (no reference added). */
static void* d12_native(gpu_device* d, uint32_t kind, uint64_t handle)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    const uint32_t id = (uint32_t)handle, i = GPU_HANDLE_INDEX(id);
    switch (kind) {
    case GPU_NATIVE_DEVICE:   return dd->device;
    case GPU_NATIVE_QUEUE:    return dd->queue;
    case GPU_NATIVE_ADAPTER:  return dd->adapter;
    case GPU_NATIVE_INSTANCE: return dd->factory;
    case GPU_NATIVE_CMD:      return handle ? ((gpu_cmd*)(uintptr_t)handle)->cl : NULL;
    case GPU_NATIVE_BUFFER:   return gpu_d3d12_slot_valid(&dd->buf_slots, id) ? dd->buf_res[i] : NULL;
    case GPU_NATIVE_TEXTURE:  return gpu_d3d12_slot_valid(&dd->tex_slots, id) ? dd->tex_res[i] : NULL;
    case GPU_NATIVE_LAYOUT:   return gpu_d3d12_slot_valid(&dd->layout_slots, id) ? dd->layout_rs[i] : NULL;
    case GPU_NATIVE_SWAPCHAIN: return gpu_d3d12_slot_valid(&dd->swap_slots, id) ? dd->swap[i].sc : NULL;
    default:                  return NULL;
    }
}

/* ---------------------------------------------------------------------------
 * The tables
 * -------------------------------------------------------------------------*/

/* The functions in gpu_d3d12_pipe.c and gpu_d3d12_swap.c. */
#define d12_pipeline_create     gpu_d3d12_pipeline_create
#define d12_pipeline_cache_load gpu_d3d12_pipeline_cache_load
#define d12_pipeline_cache_save gpu_d3d12_pipeline_cache_save
#define d12_swap_create         gpu_d3d12_swap_create
#define d12_swap_destroy        gpu_d3d12_swap_destroy
#define d12_swap_resize         gpu_d3d12_swap_resize
#define d12_swap_acquire        gpu_d3d12_swap_acquire
#define d12_swap_image_count    gpu_d3d12_swap_image_count
#define d12_swap_image          gpu_d3d12_swap_image
#define d12_swap_present        gpu_d3d12_swap_present
#define d12_swap_stats          gpu_d3d12_swap_stats
#define d12_swap_hdr            gpu_d3d12_swap_hdr

#define D12_VT_INIT(name, form, ret, params, args) d12_##name,
static const gpu_vt k_d3d12_vt = { GPU_VT_FUNCS(D12_VT_INIT) };
#undef D12_VT_INIT

const gpu_cmd_ops gpu_d3d12_cmd_ops = GPU_CMD_OPS_TABLE;

gpu_device* gpu_d3d12_create(const gpu_device_desc* desc)
{
    gpu_device_desc none;
    if (!desc) {
        memset(&none, 0, sizeof none);
        desc = &none;
    }
    gpu_d3d12_device* dd = (gpu_d3d12_device*)calloc(1, sizeof *dd);
    if (!dd) return NULL;
    InitializeSRWLock(&dd->lock);
    InitializeSRWLock(&dd->signal_lock);
    InitializeSRWLock(&dd->lib_lock);
    InitializeSRWLock(&dd->pso_lock);
    dd->base.vt = &k_d3d12_vt;
    dd->flags = desc->flags;
    if (!device_init(dd, desc)) {
        device_free(dd);
        return NULL;
    }
    return &dd->base;
}

/* ---------------------------------------------------------------------------
 * Bridges for the conversion (gpu_d3d12_bridge.h; deleted with it)
 * -------------------------------------------------------------------------*/

gpu_texture gpu_d3d12_adopt_texture(gpu_device* d, ID3D12Resource* res, const gpu_texture_desc* desc)
{
    if (!res) return (gpu_texture){0};
    res->lpVtbl->AddRef(res);
    return gpu_d3d12_tex_add(GPU_D3D12_DEV(d), res, desc, 0);
}

gpu_buffer gpu_d3d12_adopt_buffer(gpu_device* d, ID3D12Resource* res, const gpu_buffer_desc* desc)
{
    if (!res) return (gpu_buffer){0};
    res->lpVtbl->AddRef(res);
    void* map = NULL;
    if (desc->memory == GPU_MEM_UPLOAD || desc->memory == GPU_MEM_READBACK) {
        const D3D12_RANGE nr = {0, 0};
        if (FAILED(res->lpVtbl->Map(res, 0, &nr, &map))) map = NULL;
    }
    return buf_add(GPU_D3D12_DEV(d), res, desc->size, desc->memory, map, NULL);
}

gpu_pipeline gpu_d3d12_adopt_pipeline(gpu_device* d, ID3D12PipelineState* pso)
{
    gpu_pipeline r = {0};
    if (!pso) return r;
    pso->lpVtbl->AddRef(pso);
    gpu_d3d12_pipe_keep(GPU_D3D12_DEV(d), pso);
    r.v = (uint64_t)(uintptr_t)pso;
    return r;
}

gpu_layout gpu_d3d12_adopt_layout(gpu_device* d, ID3D12RootSignature* rs, uint64_t blob_hash)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_layout r = {0};
    if (!rs) return r;
    AcquireSRWLockExclusive(&dd->lock);
    r.id = gpu_d3d12_slot_alloc(&dd->layout_slots);
    if (r.id) {
        rs->lpVtbl->AddRef(rs);
        dd->layout_rs[GPU_HANDLE_INDEX(r.id)] = rs;
        dd->layout_hash[GPU_HANDLE_INDEX(r.id)] = blob_hash;
    }
    ReleaseSRWLockExclusive(&dd->lock);
    return r;
}

gpu_table_pool gpu_d3d12_adopt_table_pool(gpu_device* d, ID3D12DescriptorHeap* heap,
                                          const gpu_table_pool_desc* desc)
{
    if (!heap) return (gpu_table_pool){0};
    heap->lpVtbl->AddRef(heap);
    return tpool_add(GPU_D3D12_DEV(d), heap, desc, 0);
}

gpu_cmd* gpu_d3d12_cmd_wrap(gpu_device* d, ID3D12GraphicsCommandList* cl)
{
    gpu_cmd* const c = (gpu_cmd*)calloc(1, sizeof *c);
    if (!c || !cl) { free(c); return NULL; }
    cl->lpVtbl->AddRef(cl);
    gpu_d3d12_cmd_init(GPU_D3D12_DEV(d), c, cl, NULL);
    return c;
}

void gpu_d3d12_cmd_unwrap(gpu_device* d, gpu_cmd* c)
{
    (void)d;
    if (!c) return;
    c->cl->lpVtbl->Release(c->cl);
    free(c);
}

void gpu_d3d12_pipeline_d3d_desc(gpu_device* d, const gpu_pipeline_desc* desc,
                                 D3D12_GRAPHICS_PIPELINE_STATE_DESC* out,
                                 D3D12_INPUT_ELEMENT_DESC il[GPU_MAX_VERTEX_ATTRS])
{
    gpu_d3d12_pipe_d3d_desc(GPU_D3D12_DEV(d), desc, out, il);
}

uint64_t gpu_d3d12_layout_hash(gpu_device* d, gpu_layout layout)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    return gpu_d3d12_slot_valid(&dd->layout_slots, layout.id) ? dd->layout_hash[GPU_HANDLE_INDEX(layout.id)] : 0;
}

/* ---- step C4 ---- */

gpu_texture gpu_d3d12_adopt_texture_of(gpu_device* d, ID3D12Resource* res)
{
    D3D12_RESOURCE_DESC rd;
    gpu_texture_desc td;
    if (!res) return (gpu_texture){0};
    res->lpVtbl->GetDesc(res, &rd);
    memset(&td, 0, sizeof td);
    td.width = (uint32_t)rd.Width;
    td.height = rd.Height;
    td.layers = rd.DepthOrArraySize;
    td.mips = rd.MipLevels;
    td.format = (gpu_format)rd.Format;
    td.samples = rd.SampleDesc.Count;
    td.use = GPU_USE_COPY_SRC | GPU_USE_COPY_DST;
    if (!(rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) td.use |= GPU_USE_SAMPLED;
    if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) td.use |= GPU_USE_RT;
    if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) td.use |= GPU_USE_DS;
    return gpu_d3d12_adopt_texture(d, res, &td);
}

ID3D12DescriptorHeap* gpu_d3d12_table_pool_heap(gpu_device* d, gpu_table_pool pool)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    return gpu_d3d12_slot_valid(&dd->tpool_slots, pool.id) ? dd->tpool[GPU_HANDLE_INDEX(pool.id)].heap : NULL;
}

/* ---- step C5a ---- */

D3D12_CPU_DESCRIPTOR_HANDLE gpu_d3d12_view_cpu(gpu_device* d, gpu_view v)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    h.ptr = gpu_d3d12_slot_valid(&dd->view_slots, v.id) ? (SIZE_T)dd->view_cpu[GPU_HANDLE_INDEX(v.id)] : 0;
    return h;
}

/* ---- step C5b ---- */

ID3D12QueryHeap* gpu_d3d12_query_pool_heap(gpu_device* d, gpu_query_pool pool)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    return gpu_d3d12_slot_valid(&dd->qpool_slots, pool.id) ? dd->qpool[GPU_HANDLE_INDEX(pool.id)].heap : NULL;
}

#endif /* PS3RECOMP_RSX_HAL && _WIN32 */
