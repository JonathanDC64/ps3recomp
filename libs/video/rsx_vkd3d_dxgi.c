/*
 * ps3recomp - DXGI for the D3D12 backend on vkd3d (off Windows)
 *
 * rsx_vkd3d_host.h declares the part of DXGI that rsx_d3d12_backend.c uses;
 * this implements it over Vulkan. The swap chain's buffers are images of its
 * own, wrapped as vkd3d resources: the backend renders to them and moves them
 * to PRESENT exactly as on Windows, which vkd3d is told means "ready to be
 * copied" (VKD3D_RESOURCE_PRESENT_STATE_TRANSITION). Present then blits the
 * current one into an image of a Vulkan swap chain on the SDL window's
 * surface, on vkd3d's own queue, and presents that.
 *
 * The device is made here as well (rsx_vkd3d_create_device, which the host
 * header puts in place of D3D12CreateDevice): with the surface extensions the
 * window needs, and with fences that signal the win32_compat events the
 * backend waits on.
 */
#if !defined(_WIN32) && defined(PS3RECOMP_RSX_D3D12_VKD3D)

#include "rsx_vkd3d_host.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The interface ids, DXGI's own values. */
const GUID IID_IDXGIFactory4   = {0x1bc6ea02, 0xef36, 0x464f, {0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a}};
const GUID IID_IDXGIFactory5   = {0x7632e1f5, 0xee65, 0x4dca, {0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d}};
const GUID IID_IDXGIFactory6   = {0xc1b6694f, 0xff09, 0x44a9, {0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17}};
const GUID IID_IDXGIAdapter1   = {0x29038f61, 0x3839, 0x4626, {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}};
const GUID IID_IDXGISwapChain1 = {0x790a45f7, 0x0d42, 0x4876, {0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa}};
const GUID IID_IDXGISwapChain3 = {0x94d99bdb, 0xf1f8, 0x4ab0, {0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1}};

#define LOG(...) printf("[D3D12] vkd3d: " __VA_ARGS__)

/* ---------------------------------------------------------------------------
 * Vulkan, as the device's instance and device load it
 * -----------------------------------------------------------------------*/

static struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
#define VK_INSTANCE_FNS(X) \
    X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceSurfacePresentModesKHR) \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceProperties) X(DestroySurfaceKHR) \
    X(GetDeviceProcAddr)
#define VK_DEVICE_FNS(X) \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) \
    X(QueuePresentKHR) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) \
    X(FreeMemory) X(BindImageMemory) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdPipelineBarrier) X(CmdBlitImage) \
    X(QueueSubmit) X(QueueWaitIdle) X(CreateSemaphore) X(DestroySemaphore) X(CreateFence) X(DestroyFence) \
    X(WaitForFences) X(ResetFences)
#define VK_FN_FIELD(name) PFN_vk##name name;
    VK_INSTANCE_FNS(VK_FN_FIELD)
    VK_DEVICE_FNS(VK_FN_FIELD)
#undef VK_FN_FIELD
} vk;

static HRESULT signal_event(HANDLE event)
{
    return SetEvent(event) ? S_OK : E_FAIL;
}

HRESULT rsx_vkd3d_create_device(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** device)
{
    (void)adapter;
    *device = NULL;
    /* SDL loaded the Vulkan loader when it made the (Vulkan) window. */
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    unsigned n = 0;
    const char* exts[16];
    if (!gipa || !SDL_Vulkan_GetInstanceExtensions(NULL, &n, NULL) || n > 16 ||
        !SDL_Vulkan_GetInstanceExtensions(NULL, &n, exts)) {
        LOG("no Vulkan for the window (%s)\n", SDL_GetError());
        return E_FAIL;
    }
    static const char* const dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    struct vkd3d_instance_create_info ii;
    memset(&ii, 0, sizeof ii);
    ii.type = VKD3D_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ii.pfn_signal_event = signal_event;
    ii.wchar_size = sizeof(WCHAR);
    ii.pfn_vkGetInstanceProcAddr = gipa;
    ii.instance_extensions = exts;
    ii.instance_extension_count = n;
    struct vkd3d_device_create_info di;
    memset(&di, 0, sizeof di);
    di.type = VKD3D_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.minimum_feature_level = level;
    di.instance_create_info = &ii;
    di.device_extensions = dev_exts;
    di.device_extension_count = 1;
    ID3D12Device* d = NULL;
    HRESULT hr = vkd3d_create_device(&di, &IID_ID3D12Device, (void**)&d);
    if (FAILED(hr)) {
        LOG("device creation failed (0x%08X)\n", (unsigned)hr);
        return hr;
    }
    vk.dev = vkd3d_get_vk_device(d);
    vk.phys = vkd3d_get_vk_physical_device(d);
    vk.inst = vkd3d_instance_get_vk_instance(vkd3d_instance_from_device(d));
#define VK_LOAD_I(name) vk.name = (PFN_vk##name)gipa(vk.inst, "vk" #name);
    VK_INSTANCE_FNS(VK_LOAD_I)
#undef VK_LOAD_I
#define VK_LOAD_D(name) vk.name = vk.GetDeviceProcAddr ? (PFN_vk##name)vk.GetDeviceProcAddr(vk.dev, "vk" #name) : NULL;
    VK_DEVICE_FNS(VK_LOAD_D)
#undef VK_LOAD_D
    if (!vk.GetPhysicalDeviceSurfaceSupportKHR || !vk.CreateSwapchainKHR || !vk.QueuePresentKHR || !vk.CmdBlitImage) {
        LOG("the device has no swap chain functions\n");
        d->lpVtbl->Release(d);
        return E_FAIL;
    }
    VkPhysicalDeviceProperties pp;
    vk.GetPhysicalDeviceProperties(vk.phys, &pp);
    LOG("%s (Vulkan %u.%u.%u)\n", pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion),
        VK_VERSION_PATCH(pp.apiVersion));
    hr = d->lpVtbl->QueryInterface(d, iid, device);
    d->lpVtbl->Release(d);
    return hr;
}

/* ---------------------------------------------------------------------------
 * Swap chain
 * -----------------------------------------------------------------------*/

#define SC_BUFFERS_MAX 4   /* the backend's buffers (it asks for FRAME_COUNT) */
#define SC_IMAGES_MAX  8   /* the Vulkan swap chain's images */
#define SC_SLOTS       3   /* presents in flight */

typedef struct dxgi_swap_chain {
    IDXGISwapChain3 iface;
    LONG refs;
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    SDL_Window* window;
    VkSurfaceKHR surface;
    /* The backend's buffers: what GetBuffer hands out. */
    UINT count, width, height, current, presents;
    DXGI_FORMAT format;
    VkImage buffer[SC_BUFFERS_MAX];
    VkDeviceMemory memory[SC_BUFFERS_MAX];
    ID3D12Resource* resource[SC_BUFFERS_MAX];
    /* The Vulkan swap chain, made again whenever it goes stale. */
    VkSwapchainKHR chain;
    VkExtent2D extent;
    VkPresentModeKHR mode, tear_mode;
    int stale, said_vsync;
    uint32_t images;
    VkImage image[SC_IMAGES_MAX];
    VkSemaphore blitted[SC_IMAGES_MAX];   /* per image: its blit is done (the present waits on it) */
    VkCommandPool pool;
    VkCommandBuffer cmd[SC_SLOTS];
    VkSemaphore acquired[SC_SLOTS];
    VkFence done[SC_SLOTS];
    uint32_t slot;
} dxgi_swap_chain;

static dxgi_swap_chain* sc_impl(IDXGISwapChain3* iface)
{
    return (dxgi_swap_chain*)iface;
}

static void sc_idle(dxgi_swap_chain* sc)
{
    VkQueue q = vkd3d_acquire_vk_queue(sc->queue);
    vk.QueueWaitIdle(q);
    vkd3d_release_vk_queue(sc->queue);
}

static void buffers_free(dxgi_swap_chain* sc)
{
    for (UINT i = 0; i < SC_BUFFERS_MAX; i++) {
        if (sc->resource[i]) sc->resource[i]->lpVtbl->Release(sc->resource[i]);
        if (sc->buffer[i]) vk.DestroyImage(vk.dev, sc->buffer[i], NULL);
        if (sc->memory[i]) vk.FreeMemory(vk.dev, sc->memory[i], NULL);
        sc->resource[i] = NULL;
        sc->buffer[i] = VK_NULL_HANDLE;
        sc->memory[i] = VK_NULL_HANDLE;
    }
}

static int memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vk.GetPhysicalDeviceMemoryProperties(vk.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return (int)i;
    return -1;
}

static HRESULT buffers_make(dxgi_swap_chain* sc)
{
    for (UINT i = 0; i < sc->count; i++) {
        VkImageCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = vkd3d_get_vk_format(sc->format);
        ci.extent.width = sc->width;
        ci.extent.height = sc->height;
        ci.extent.depth = 1;
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vk.CreateImage(vk.dev, &ci, NULL, &sc->buffer[i]) != VK_SUCCESS) return E_OUTOFMEMORY;
        VkMemoryRequirements mr;
        vk.GetImageMemoryRequirements(vk.dev, sc->buffer[i], &mr);
        const int type = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = (uint32_t)type;
        if (type < 0 || vk.AllocateMemory(vk.dev, &ai, NULL, &sc->memory[i]) != VK_SUCCESS ||
            vk.BindImageMemory(vk.dev, sc->buffer[i], sc->memory[i], 0) != VK_SUCCESS)
            return E_OUTOFMEMORY;

        struct vkd3d_image_resource_create_info ri;
        memset(&ri, 0, sizeof ri);
        ri.type = VKD3D_STRUCTURE_TYPE_IMAGE_RESOURCE_CREATE_INFO;
        ri.vk_image = sc->buffer[i];
        ri.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ri.desc.Width = sc->width;
        ri.desc.Height = sc->height;
        ri.desc.DepthOrArraySize = 1;
        ri.desc.MipLevels = 1;
        ri.desc.Format = sc->format;
        ri.desc.SampleDesc.Count = 1;
        ri.desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ri.desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        /* PRESENT (= COMMON) is the layout Present copies from. */
        ri.flags = VKD3D_RESOURCE_INITIAL_STATE_TRANSITION | VKD3D_RESOURCE_PRESENT_STATE_TRANSITION;
        ri.present_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        HRESULT hr = vkd3d_create_image_resource(sc->device, &ri, &sc->resource[i]);
        if (FAILED(hr)) return hr;
    }
    sc->current = 0;
    return S_OK;
}

static void chain_free_images(dxgi_swap_chain* sc)
{
    for (uint32_t i = 0; i < SC_IMAGES_MAX; i++) {
        if (sc->blitted[i]) vk.DestroySemaphore(vk.dev, sc->blitted[i], NULL);
        sc->blitted[i] = VK_NULL_HANDLE;
        sc->image[i] = VK_NULL_HANDLE;
    }
    sc->images = 0;
}

/* (Re)make the Vulkan swap chain at the surface's size with the given mode.
 * 0 when the window has no area (minimised): nothing is presented then. */
static int chain_make(dxgi_swap_chain* sc, VkPresentModeKHR mode)
{
    sc_idle(sc);
    VkSurfaceCapabilitiesKHR caps;
    if (vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(vk.phys, sc->surface, &caps) != VK_SUCCESS) return 0;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext.width = sc->width;
        ext.height = sc->height;
    }
    if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (caps.maxImageExtent.width && ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
    if (caps.maxImageExtent.height && ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    if (!ext.width || !ext.height) return 0;

    uint32_t nf = 0;
    VkSurfaceFormatKHR formats[64];
    vk.GetPhysicalDeviceSurfaceFormatsKHR(vk.phys, sc->surface, &nf, NULL);
    if (nf > 64) nf = 64;
    vk.GetPhysicalDeviceSurfaceFormatsKHR(vk.phys, sc->surface, &nf, formats);
    if (!nf) return 0;
    VkSurfaceFormatKHR fmt = formats[0];
    for (uint32_t i = 0; i < nf; i++)   /* the frame is already gamma-encoded: a UNORM target, not sRGB */
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            fmt = formats[i];
            break;
        }

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    if (want > SC_IMAGES_MAX) want = SC_IMAGES_MAX;
    VkSwapchainCreateInfoKHR ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = sc->surface;
    ci.minImageCount = want;
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = sc->chain;
    VkSwapchainKHR chain = VK_NULL_HANDLE;
    const VkResult r = vk.CreateSwapchainKHR(vk.dev, &ci, NULL, &chain);
    if (sc->chain) vk.DestroySwapchainKHR(vk.dev, sc->chain, NULL);
    sc->chain = VK_NULL_HANDLE;
    chain_free_images(sc);
    if (r != VK_SUCCESS) {
        LOG("swap chain %ux%u failed (%d)\n", ext.width, ext.height, (int)r);
        return 0;
    }
    sc->chain = chain;
    sc->extent = ext;
    sc->mode = mode;
    sc->images = SC_IMAGES_MAX;
    vk.GetSwapchainImagesKHR(vk.dev, chain, &sc->images, sc->image);
    VkSemaphoreCreateInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < sc->images; i++) vk.CreateSemaphore(vk.dev, &si, NULL, &sc->blitted[i]);
    sc->stale = 0;
    return 1;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags src_stage, VkAccessFlags src, VkPipelineStageFlags dst_stage,
                          VkAccessFlags dst)
{
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vk.CmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static HRESULT STDMETHODCALLTYPE sc_QueryInterface(IDXGISwapChain3* iface, REFIID iid, void** out)
{
    if (!memcmp(iid, &IID_IDXGISwapChain1, sizeof(GUID)) || !memcmp(iid, &IID_IDXGISwapChain3, sizeof(GUID))) {
        InterlockedIncrement(&sc_impl(iface)->refs);
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE sc_AddRef(IDXGISwapChain3* iface)
{
    return (ULONG)InterlockedIncrement(&sc_impl(iface)->refs);
}

static ULONG STDMETHODCALLTYPE sc_Release(IDXGISwapChain3* iface)
{
    dxgi_swap_chain* sc = sc_impl(iface);
    const LONG n = InterlockedDecrement(&sc->refs);
    if (n) return (ULONG)n;
    sc_idle(sc);
    buffers_free(sc);
    if (sc->chain) vk.DestroySwapchainKHR(vk.dev, sc->chain, NULL);
    chain_free_images(sc);
    for (int i = 0; i < SC_SLOTS; i++) {
        if (sc->acquired[i]) vk.DestroySemaphore(vk.dev, sc->acquired[i], NULL);
        if (sc->done[i]) vk.DestroyFence(vk.dev, sc->done[i], NULL);
    }
    if (sc->pool) vk.DestroyCommandPool(vk.dev, sc->pool, NULL);
    if (sc->surface) vk.DestroySurfaceKHR(vk.inst, sc->surface, NULL);
    sc->queue->lpVtbl->Release(sc->queue);
    sc->device->lpVtbl->Release(sc->device);
    free(sc);
    return 0;
}

static HRESULT STDMETHODCALLTYPE sc_Present(IDXGISwapChain3* iface, UINT sync_interval, UINT flags)
{
    dxgi_swap_chain* sc = sc_impl(iface);
    const UINT src = sc->current;
    sc->current = (sc->current + 1) % sc->count;
    sc->presents++;

    /* Interval 0 with tearing allowed: at once (RSX_VSYNC=0); else at the
     * refresh. A Vulkan swap chain has one mode, so a change makes another. */
    const VkPresentModeKHR mode = sync_interval == 0 && (flags & DXGI_PRESENT_ALLOW_TEARING) ? sc->tear_mode
                                                                                              : VK_PRESENT_MODE_FIFO_KHR;
    if (mode == VK_PRESENT_MODE_FIFO_KHR && sync_interval == 0 && (flags & DXGI_PRESENT_ALLOW_TEARING) &&
        !sc->said_vsync++)
        LOG("this display presents only at its refresh: RSX_VSYNC=0 still waits for it\n");
    if ((!sc->chain || sc->stale || mode != sc->mode) && !chain_make(sc, mode)) return DXGI_STATUS_OCCLUDED;

    const uint32_t slot = sc->slot;
    vk.WaitForFences(vk.dev, 1, &sc->done[slot], VK_TRUE, UINT64_MAX);
    uint32_t img = 0;
    VkResult r = vk.AcquireNextImageKHR(vk.dev, sc->chain, UINT64_MAX, sc->acquired[slot], VK_NULL_HANDLE, &img);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!chain_make(sc, mode)) return DXGI_STATUS_OCCLUDED;
        r = vk.AcquireNextImageKHR(vk.dev, sc->chain, UINT64_MAX, sc->acquired[slot], VK_NULL_HANDLE, &img);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        sc->stale = 1;
        return S_OK;   /* (the frame is dropped; the next present makes the chain again) */
    }
    vk.ResetFences(vk.dev, 1, &sc->done[slot]);

    VkCommandBuffer cb = sc->cmd[slot];
    vk.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.BeginCommandBuffer(cb, &bi);
    /* The buffer is in the layout vkd3d gave PRESENT (TRANSFER_SRC); this
     * only makes the frame's writes visible to the copy. */
    image_barrier(cb, sc->buffer[src], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT);
    image_barrier(cb, sc->image[img], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit;
    memset(&blit, 0, sizeof blit);
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = (int32_t)sc->width;
    blit.srcOffsets[1].y = (int32_t)sc->height;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1].x = (int32_t)sc->extent.width;
    blit.dstOffsets[1].y = (int32_t)sc->extent.height;
    blit.dstOffsets[1].z = 1;
    const int same = sc->extent.width == sc->width && sc->extent.height == sc->height;
    vk.CmdBlitImage(cb, sc->buffer[src], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->image[img],
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    image_barrier(cb, sc->image[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    vk.EndCommandBuffer(cb);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sc->acquired[slot];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sc->blitted[img];
    VkPresentInfoKHR pi;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &sc->blitted[img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc->chain;
    pi.pImageIndices = &img;
    /* On vkd3d's queue, after everything the backend submitted for the frame. */
    VkQueue q = vkd3d_acquire_vk_queue(sc->queue);
    r = vk.QueueSubmit(q, 1, &si, sc->done[slot]);
    if (r == VK_SUCCESS) r = vk.QueuePresentKHR(q, &pi);
    vkd3d_release_vk_queue(sc->queue);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) sc->stale = 1;
    else if (r != VK_SUCCESS) {
        static int said = 0;
        if (!said++) LOG("present failed (%d)\n", (int)r);
    }
    sc->slot = (slot + 1) % SC_SLOTS;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sc_GetBuffer(IDXGISwapChain3* iface, UINT index, REFIID iid, void** out)
{
    dxgi_swap_chain* sc = sc_impl(iface);
    *out = NULL;
    if (index >= sc->count || !sc->resource[index]) return DXGI_ERROR_INVALID_CALL;
    return sc->resource[index]->lpVtbl->QueryInterface(sc->resource[index], iid, out);
}

static HRESULT STDMETHODCALLTYPE sc_ResizeBuffers(IDXGISwapChain3* iface, UINT count, UINT width, UINT height,
                                                  DXGI_FORMAT format, UINT flags)
{
    dxgi_swap_chain* sc = sc_impl(iface);
    (void)flags;
    if (count > SC_BUFFERS_MAX || !width || !height) return DXGI_ERROR_INVALID_CALL;
    sc_idle(sc);
    buffers_free(sc);
    if (count) sc->count = count;
    if (format != DXGI_FORMAT_UNKNOWN) sc->format = format;
    sc->width = width;
    sc->height = height;
    sc->stale = 1;
    return buffers_make(sc);
}

/* The statistics DXGI keeps of the display itself are not kept here: the
 * backend's callers take "cannot say" (present_queued, FRAME_LOG). */
static HRESULT STDMETHODCALLTYPE sc_GetFrameStatistics(IDXGISwapChain3* iface, DXGI_FRAME_STATISTICS* stats)
{
    (void)iface;
    memset(stats, 0, sizeof *stats);
    return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
}

static HRESULT STDMETHODCALLTYPE sc_GetLastPresentCount(IDXGISwapChain3* iface, UINT* count)
{
    *count = sc_impl(iface)->presents;
    return S_OK;
}

static UINT STDMETHODCALLTYPE sc_GetCurrentBackBufferIndex(IDXGISwapChain3* iface)
{
    return sc_impl(iface)->current;
}

static const IDXGISwapChain3Vtbl s_sc_vtbl = {
    sc_QueryInterface, sc_AddRef, sc_Release, sc_Present, sc_GetBuffer, sc_ResizeBuffers,
    sc_GetFrameStatistics, sc_GetLastPresentCount, sc_GetCurrentBackBufferIndex,
};

static HRESULT swap_chain_make(IUnknown* queue_unk, SDL_Window* window, const DXGI_SWAP_CHAIN_DESC1* desc,
                               IDXGISwapChain1** out)
{
    *out = NULL;
    if (!window || !desc || !desc->BufferCount || desc->BufferCount > SC_BUFFERS_MAX) return DXGI_ERROR_INVALID_CALL;
    dxgi_swap_chain* sc = (dxgi_swap_chain*)calloc(1, sizeof *sc);
    if (!sc) return E_OUTOFMEMORY;
    sc->iface.lpVtbl = &s_sc_vtbl;
    sc->refs = 1;
    sc->window = window;
    sc->count = desc->BufferCount;
    sc->width = desc->Width;
    sc->height = desc->Height;
    sc->format = desc->Format;
    HRESULT hr = queue_unk->lpVtbl->QueryInterface(queue_unk, &IID_ID3D12CommandQueue, (void**)&sc->queue);
    if (FAILED(hr)) {
        free(sc);
        return hr;
    }
    hr = sc->queue->lpVtbl->GetDevice(sc->queue, &IID_ID3D12Device, (void**)&sc->device);
    if (FAILED(hr)) {
        sc->queue->lpVtbl->Release(sc->queue);
        free(sc);
        return hr;
    }
    hr = E_FAIL;
    const uint32_t family = vkd3d_get_vk_queue_family_index(sc->queue);
    VkBool32 can = VK_FALSE;
    if (!SDL_Vulkan_CreateSurface(window, vk.inst, &sc->surface)) {
        LOG("no surface for the window (%s)\n", SDL_GetError());
        goto fail;
    }
    if (vk.GetPhysicalDeviceSurfaceSupportKHR(vk.phys, family, sc->surface, &can) != VK_SUCCESS || !can) {
        LOG("the device's queue cannot present to the window\n");
        goto fail;
    }
    uint32_t nm = 0;
    VkPresentModeKHR modes[16];
    vk.GetPhysicalDeviceSurfacePresentModesKHR(vk.phys, sc->surface, &nm, NULL);
    if (nm > 16) nm = 16;
    vk.GetPhysicalDeviceSurfacePresentModesKHR(vk.phys, sc->surface, &nm, modes);
    sc->tear_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < nm; i++) {
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) sc->tear_mode = modes[i];
        else if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR && sc->tear_mode == VK_PRESENT_MODE_FIFO_KHR)
            sc->tear_mode = modes[i];
    }

    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    if (vk.CreateCommandPool(vk.dev, &pci, NULL, &sc->pool) != VK_SUCCESS) goto fail;
    VkCommandBufferAllocateInfo cai;
    memset(&cai, 0, sizeof cai);
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = sc->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = SC_SLOTS;
    if (vk.AllocateCommandBuffers(vk.dev, &cai, sc->cmd) != VK_SUCCESS) goto fail;
    VkSemaphoreCreateInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < SC_SLOTS; i++)
        if (vk.CreateSemaphore(vk.dev, &si, NULL, &sc->acquired[i]) != VK_SUCCESS ||
            vk.CreateFence(vk.dev, &fi, NULL, &sc->done[i]) != VK_SUCCESS)
            goto fail;
    hr = buffers_make(sc);
    if (FAILED(hr)) goto fail;
    if (!chain_make(sc, VK_PRESENT_MODE_FIFO_KHR)) sc->stale = 1;   /* (made at the first present) */
    *out = &sc->iface;
    return S_OK;
fail:
    sc_Release(&sc->iface);
    return FAILED(hr) ? hr : E_FAIL;
}

/* ---------------------------------------------------------------------------
 * Factory
 * -----------------------------------------------------------------------*/

typedef struct dxgi_factory {
    IDXGIFactory6 iface;
    LONG refs;
} dxgi_factory;

static HRESULT STDMETHODCALLTYPE f_QueryInterface(IDXGIFactory6* iface, REFIID iid, void** out)
{
    if (!memcmp(iid, &IID_IDXGIFactory4, sizeof(GUID)) || !memcmp(iid, &IID_IDXGIFactory5, sizeof(GUID)) ||
        !memcmp(iid, &IID_IDXGIFactory6, sizeof(GUID))) {
        InterlockedIncrement(&((dxgi_factory*)iface)->refs);
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE f_AddRef(IDXGIFactory6* iface)
{
    return (ULONG)InterlockedIncrement(&((dxgi_factory*)iface)->refs);
}

static ULONG STDMETHODCALLTYPE f_Release(IDXGIFactory6* iface)
{
    const LONG n = InterlockedDecrement(&((dxgi_factory*)iface)->refs);
    if (!n) free(iface);
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE f_MakeWindowAssociation(IDXGIFactory6* iface, HWND window, UINT flags)
{
    (void)iface; (void)window; (void)flags;   /* (no keys of its own to turn off) */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE f_CreateSwapChainForHwnd(IDXGIFactory6* iface, IUnknown* queue, HWND window,
                                                          const DXGI_SWAP_CHAIN_DESC1* desc,
                                                          const void* fullscreen_desc, void* restrict_to_output,
                                                          IDXGISwapChain1** swap_chain)
{
    (void)iface; (void)fullscreen_desc; (void)restrict_to_output;
    return swap_chain_make(queue, (SDL_Window*)window, desc, swap_chain);
}

/* Tearing is offered; the swap chain falls back to the refresh (and says so)
 * where the window's surface has no immediate or mailbox mode. */
static HRESULT STDMETHODCALLTYPE f_CheckFeatureSupport(IDXGIFactory6* iface, DXGI_FEATURE feature, void* data,
                                                       UINT size)
{
    (void)iface;
    if (feature != DXGI_FEATURE_PRESENT_ALLOW_TEARING || size != sizeof(BOOL)) return DXGI_ERROR_INVALID_CALL;
    *(BOOL*)data = TRUE;
    return S_OK;
}

/* No adapters to list: the backend then makes its device on the default one,
 * which vkd3d picks (VKD3D_VULKAN_DEVICE=<index> chooses another). */
static HRESULT STDMETHODCALLTYPE f_EnumAdapterByGpuPreference(IDXGIFactory6* iface, UINT index,
                                                              DXGI_GPU_PREFERENCE preference, REFIID iid,
                                                              void** adapter)
{
    (void)iface; (void)index; (void)preference; (void)iid;
    *adapter = NULL;
    return DXGI_ERROR_NOT_FOUND;
}

static const IDXGIFactory6Vtbl s_factory_vtbl = {
    f_QueryInterface, f_AddRef, f_Release, f_MakeWindowAssociation, f_CreateSwapChainForHwnd,
    f_CheckFeatureSupport, f_EnumAdapterByGpuPreference,
};

HRESULT CreateDXGIFactory1(REFIID iid, void** factory)
{
    *factory = NULL;
    dxgi_factory* f = (dxgi_factory*)calloc(1, sizeof *f);
    if (!f) return E_OUTOFMEMORY;
    f->iface.lpVtbl = &s_factory_vtbl;
    f->refs = 1;
    const HRESULT hr = f_QueryInterface(&f->iface, iid, factory);
    f_Release(&f->iface);
    return hr;
}

#endif /* !_WIN32 && PS3RECOMP_RSX_D3D12_VKD3D */
