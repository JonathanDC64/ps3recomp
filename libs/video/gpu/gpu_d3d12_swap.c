/*
 * ps3recomp - the portable renderer's GPU layer: the D3D12 backend's swap chains
 *
 * Today's swap chain (rsx_d3d12_backend.c, init_d3d12, swap_present,
 * swap_resize, swap_color_space): flip model, FLIP_DISCARD, ALLOW_TEARING
 * when asked and the system has it, Alt+Enter left to the window. Its buffers
 * are textures (gpu_swap_acquire), made again after each resize.
 * From the render thread.
 *
 * Built with PS3RECOMP_RSX_HAL on Windows; vkd3d comes later.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(_WIN32)

#include "gpu_d3d12_internal.h"
#include "gpu_d3d12_bridge.h"

#include <stdio.h>
#include <string.h>

static gpu_d3d12_swap* swap_of(gpu_d3d12_device* dd, gpu_swapchain sc)
{
    return gpu_d3d12_slot_valid(&dd->swap_slots, sc.id) ? &dd->swap[GPU_HANDLE_INDEX(sc.id)] : NULL;
}

/* The buffers as textures, in PRESENT (today's swap_buffers_get; their RTVs
 * come from gpu_view_get). */
static int images_get(gpu_d3d12_device* dd, gpu_d3d12_swap* s)
{
    gpu_texture_desc td;
    memset(&td, 0, sizeof td);
    td.width = s->width;
    td.height = s->height;
    td.layers = 1;
    td.mips = 1;
    td.format = s->format;
    td.samples = 1;
    td.use = GPU_USE_RT | GPU_USE_SAMPLED | GPU_USE_COPY_SRC | GPU_USE_COPY_DST;
    td.state = GPU_STATE_PRESENT;
    for (uint32_t i = 0; i < s->buffers; i++) {
        ID3D12Resource* b = NULL;
        if (FAILED(s->sc->lpVtbl->GetBuffer(s->sc, i, &IID_ID3D12Resource, (void**)&b)) || !b) return -1;
        s->image[i] = gpu_d3d12_tex_add(dd, b, &td, GPU_D3D12_TEX_SWAP);
        if (!GPU_VALID(s->image[i])) return -1;
    }
    return 0;
}

/* Before ResizeBuffers, which fails while a buffer is referenced: the GPU is
 * idle (the caller's), so the textures and their views go now. */
static void images_drop(gpu_d3d12_device* dd, gpu_d3d12_swap* s)
{
    for (uint32_t i = 0; i < GPU_D3D12_MAX_IMAGES; i++)
        if (GPU_VALID(s->image[i])) {
            gpu_d3d12_tex_free(dd, s->image[i]);
            s->image[i].id = 0;
        }
}

static void size_read(gpu_d3d12_swap* s)
{
    DXGI_SWAP_CHAIN_DESC1 d1;
    if (SUCCEEDED(s->sc->lpVtbl->GetDesc1(s->sc, &d1))) {
        s->width = d1.Width;
        s->height = d1.Height;
    }
}

gpu_swapchain gpu_d3d12_swap_create(gpu_device* d, const gpu_swapchain_desc* desc)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_swapchain r = {0};
    const uint32_t n = desc->buffers ? desc->buffers : 2;
    if (!desc->window || n < 2 || n > GPU_D3D12_MAX_IMAGES) return r;
    HWND const hwnd = (HWND)desc->window;
    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = desc->width;
    sc_desc.Height = desc->height;
    sc_desc.Format = (DXGI_FORMAT)desc->format;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = n;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    /* A windowed flip-model swap chain presenting with sync interval 0 is
     * still paced by the compositor at the display's refresh unless it may
     * tear: the swap chain has to be made with ALLOW_TEARING and each such
     * Present pass DXGI_PRESENT_ALLOW_TEARING (gpu_swap_present's
     * GPU_PRESENT_TEARING). */
    if ((desc->flags & GPU_SWAP_TEARING) && (d->caps.bits & GPU_CAP_TEARING))
        sc_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    IDXGISwapChain1* swap_chain1 = NULL;
    HRESULT hr = dd->factory->lpVtbl->CreateSwapChainForHwnd(
        dd->factory, (IUnknown*)dd->queue, hwnd, &sc_desc, NULL, NULL, &swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateSwapChainForHwnd failed (0x%08lX)\n", (unsigned long)hr);
        return r;
    }
    /* Disable Alt+Enter fullscreen toggle */
    dd->factory->lpVtbl->MakeWindowAssociation(dd->factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
    IDXGISwapChain3* sc3 = NULL;
    hr = swap_chain1->lpVtbl->QueryInterface(swap_chain1, &IID_IDXGISwapChain3, (void**)&sc3);
    swap_chain1->lpVtbl->Release(swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: QueryInterface for SwapChain3 failed\n");
        return r;
    }
    AcquireSRWLockExclusive(&dd->lock);
    r.id = gpu_d3d12_slot_alloc(&dd->swap_slots);
    ReleaseSRWLockExclusive(&dd->lock);
    if (!r.id) {
        sc3->lpVtbl->Release(sc3);
        return r;
    }
    gpu_d3d12_swap* const s = &dd->swap[GPU_HANDLE_INDEX(r.id)];
    memset(s, 0, sizeof *s);
    s->sc = sc3;
    s->window = desc->window;
    s->buffers = n;
    s->format = desc->format;
    s->dxgi_flags = sc_desc.Flags;
    size_read(s);
    if (images_get(dd, s) != 0) {
        gpu_d3d12_swap_destroy(d, r);
        r.id = 0;
        return r;
    }
    if (desc->colour_space != GPU_COLOR_SRGB) gpu_d3d12_swap_hdr(d, r, desc->colour_space, NULL);
    return r;
}

void gpu_d3d12_swap_destroy(gpu_device* d, gpu_swapchain sc)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_d3d12_swap* const s = swap_of(dd, sc);
    if (!s) return;
    images_drop(dd, s);
    if (s->sc) s->sc->lpVtbl->Release(s->sc);
    memset(s, 0, sizeof *s);
    AcquireSRWLockExclusive(&dd->lock);
    gpu_d3d12_slot_free(&dd->swap_slots, sc.id);
    ReleaseSRWLockExclusive(&dd->lock);
}

/* Today's swap_resize, its D3D12 half: ResizeBuffers with the buffer count
 * and flags the swap chain was made with, then the buffers again (new
 * handles; the old ones are stale), whether or not the resize took. */
int gpu_d3d12_swap_resize(gpu_device* d, gpu_swapchain sc, uint32_t width, uint32_t height, gpu_format format)
{
    gpu_d3d12_device* const dd = GPU_D3D12_DEV(d);
    gpu_d3d12_swap* const s = swap_of(dd, sc);
    if (!s) return -1;
    images_drop(dd, s);
    const HRESULT hr = s->sc->lpVtbl->ResizeBuffers(s->sc, s->buffers, width, height, (DXGI_FORMAT)format,
                                                    s->dxgi_flags);
    if (SUCCEEDED(hr)) s->format = format;
    size_read(s);
    const int got = images_get(dd, s);
    if (FAILED(hr)) return (int)hr;
    return got;
}

/* gpu_swap_image_count: the buffers asked (DXGI makes exactly those). gpu_swap_image is
 * gpu_d3d12_swap_image, below. */
uint32_t gpu_d3d12_swap_image_count(gpu_device* d, gpu_swapchain sc)
{
    gpu_d3d12_swap* const s = swap_of(GPU_D3D12_DEV(d), sc);
    return s ? s->buffers : 0;
}

uint32_t gpu_d3d12_swap_acquire(gpu_device* d, gpu_swapchain sc, gpu_texture* image)
{
    gpu_d3d12_swap* const s = swap_of(GPU_D3D12_DEV(d), sc);
    if (!s) {
        if (image) image->id = 0;
        return 0;
    }
    const uint32_t i = s->sc->lpVtbl->GetCurrentBackBufferIndex(s->sc);
    if (image) *image = i < GPU_D3D12_MAX_IMAGES ? s->image[i] : s->image[0];
    return i;
}

/* The bridge (gpu_d3d12_bridge.h): image i of the swap chain. */
gpu_texture gpu_d3d12_swap_image(gpu_device* d, gpu_swapchain sc, uint32_t i)
{
    gpu_d3d12_swap* const s = swap_of(GPU_D3D12_DEV(d), sc);
    gpu_texture none = {0};
    return s && i < s->buffers ? s->image[i] : none;
}

/* 0, or the failed Present's HRESULT (for the caller's check_device). */
int gpu_d3d12_swap_present(gpu_device* d, gpu_swapchain sc, uint32_t interval, uint32_t flags)
{
    gpu_d3d12_swap* const s = swap_of(GPU_D3D12_DEV(d), sc);
    if (!s) return -1;
    const HRESULT hr = s->sc->lpVtbl->Present(s->sc, interval, flags);
    return FAILED(hr) ? (int)hr : 0;
}

/* GetFrameStatistics (display_note) and GetLastPresentCount
 * (present_queued); nonzero when either has nothing to say. */
int gpu_d3d12_swap_stats(gpu_device* d, gpu_swapchain sc, gpu_present_stats* out)
{
    gpu_d3d12_swap* const s = swap_of(GPU_D3D12_DEV(d), sc);
    if (!s) return -1;
    DXGI_FRAME_STATISTICS fs;
    UINT last = 0;
    memset(&fs, 0, sizeof fs);
    if (FAILED(s->sc->lpVtbl->GetFrameStatistics(s->sc, &fs)) ||
        FAILED(s->sc->lpVtbl->GetLastPresentCount(s->sc, &last)))
        return 1;
    out->present_count = fs.PresentCount;
    out->present_refresh_count = fs.PresentRefreshCount;
    out->sync_refresh_count = fs.SyncRefreshCount;
    out->last_present_count = last;
    out->sync_qpc = fs.SyncQPCTime.QuadPart;
    return 0;
}

static UINT16 xy(float v)
{
    const float c = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    return (UINT16)(c * 50000.0f + 0.5f);
}

/* Today's swap_color_space: the colour space if the swap chain can present
 * it; then, when given, HDR10 metadata (IDXGISwapChain4; chromaticities in
 * 1/50000, mastering luminance in 1/10000 nit, MaxCLL and MaxFALL in nits). */
int gpu_d3d12_swap_hdr(gpu_device* d, gpu_swapchain sc, uint32_t colour_space, const gpu_hdr_metadata* meta)
{
    gpu_d3d12_swap* const s = swap_of(GPU_D3D12_DEV(d), sc);
    if (!s) return 0;
    const DXGI_COLOR_SPACE_TYPE cs = (DXGI_COLOR_SPACE_TYPE)colour_space;
    UINT sup = 0;
    if (!(SUCCEEDED(s->sc->lpVtbl->CheckColorSpaceSupport(s->sc, cs, &sup)) &&
          (sup & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) &&
          SUCCEEDED(s->sc->lpVtbl->SetColorSpace1(s->sc, cs))))
        return 0;
    if (meta) {
        IDXGISwapChain4* s4 = NULL;
        if (SUCCEEDED(s->sc->lpVtbl->QueryInterface(s->sc, &IID_IDXGISwapChain4, (void**)&s4)) && s4) {
            DXGI_HDR_METADATA_HDR10 m;
            memset(&m, 0, sizeof m);
            m.RedPrimary[0] = xy(meta->red[0]);     m.RedPrimary[1] = xy(meta->red[1]);
            m.GreenPrimary[0] = xy(meta->green[0]); m.GreenPrimary[1] = xy(meta->green[1]);
            m.BluePrimary[0] = xy(meta->blue[0]);   m.BluePrimary[1] = xy(meta->blue[1]);
            m.WhitePoint[0] = xy(meta->white[0]);   m.WhitePoint[1] = xy(meta->white[1]);
            m.MaxMasteringLuminance = (UINT)(meta->max_nits * 10000.0f + 0.5f);
            m.MinMasteringLuminance = (UINT)(meta->min_nits * 10000.0f + 0.5f);
            m.MaxContentLightLevel = (UINT16)(meta->max_cll + 0.5f);
            m.MaxFrameAverageLightLevel = (UINT16)(meta->max_fall + 0.5f);
            s4->lpVtbl->SetHDRMetaData(s4, DXGI_HDR_METADATA_TYPE_HDR10, sizeof m, &m);
            s4->lpVtbl->Release(s4);
        }
    }
    return 1;
}

#endif /* PS3RECOMP_RSX_HAL && _WIN32 */
