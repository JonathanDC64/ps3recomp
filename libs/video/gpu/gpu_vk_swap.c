/*
 * ps3recomp - the portable renderer's GPU layer: the Vulkan backend's swap chains
 *
 * A window's surface and swap chain. Its images are textures (made again
 * after each resize), as on D3D12; their size is the window's (Vulkan's
 * surfaces say it), and there may be more of them than asked
 * (gpu_swap_image_count).
 *
 * Presenting: interval 0 is IMMEDIATE (tearing; RSX_VSYNC=0), 1 or more is
 * FIFO (RSX_VSYNC=1); never MAILBOX. With swapchain_maintenance1 the mode
 * changes per present; without it the swap chain is made again at the next
 * acquire. Statistics come from present_id and present_wait. Intervals 2
 * to 4 (RSX_VSYNC=2-4) hold the frame before that many refreshes, as DXGI's
 * sync interval does, with present_wait (present_gate, step VC2).
 *
 * The semaphores: an acquire's semaphore is waited for by the first submit
 * with GPU_SUBMIT_ACQUIRE_WAIT; the present's own empty submit (after every
 * submit of the frame, GPU_SUBMIT_PRESENT_SIGNAL or not) signals the
 * image's done semaphore, and the present waits for that (the empty submit
 * waits for the acquire too if no submit did).
 *
 * From the render thread. Built with PS3RECOMP_RSX_HAL and
 * PS3RECOMP_RSX_HAL_VULKAN; the window is an HWND on Windows and an
 * SDL_Window* (made with SDL_WINDOW_VULKAN) elsewhere.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* clock_gettime under -std=c99 */
#endif
#if defined(PS3RECOMP_RSX_HAL) && defined(PS3RECOMP_RSX_HAL_VULKAN)

#include "gpu_vk_internal.h"

#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <time.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#endif

const char* const* gpu_vk_surface_extensions(void)
{
#ifdef _WIN32
    static const char* const k[] = {"VK_KHR_win32_surface", NULL};
#else
    static const char* const k[] = {"VK_KHR_xlib_surface", "VK_KHR_xcb_surface", "VK_KHR_wayland_surface", NULL};
#endif
    return k;
}

static gpu_vk_swap* swap_of(gpu_vk_device* dd, gpu_swapchain sc)
{
    return gpu_vk_slot_valid(&dd->swap_slots, sc.id) ? &dd->swap[GPU_HANDLE_INDEX(sc.id)] : NULL;
}

static int64_t now_ticks(void)
{
#ifdef _WIN32
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (int64_t)q.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
#endif
}

/* ---- step VC2 ---- */
static int64_t ticks_per_s(void)
{
#ifdef _WIN32
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (int64_t)f.QuadPart;
#else
    return 1000000000ll;
#endif
}

/* The refresh period of the display the window is on, in now_ticks units;
 * 0 when the system does not say. Whole hertz, as the display mode has it
 * (59.94 Hz reads 59): close enough for present_gate, which aims a quarter
 * of a refresh before a vblank. */
static int64_t refresh_ticks(void* window)
{
    int hz = 0;
#ifdef _WIN32
    MONITORINFOEXA mi;
    DEVMODEA dm;
    memset(&mi, 0, sizeof mi);
    memset(&dm, 0, sizeof dm);
    mi.cbSize = sizeof mi;
    dm.dmSize = sizeof dm;
    const HMONITOR m = MonitorFromWindow((HWND)window, MONITOR_DEFAULTTONEAREST);
    if (m && GetMonitorInfoA(m, (MONITORINFO*)&mi) && EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        hz = (int)dm.dmDisplayFrequency;
#else
    SDL_DisplayMode dm;
    const int i = SDL_GetWindowDisplayIndex((SDL_Window*)window);
    if (i >= 0 && SDL_GetCurrentDisplayMode(i, &dm) == 0) hz = dm.refresh_rate;
#endif
    return hz >= 20 && hz <= 1000 ? ticks_per_s() / hz : 0;
}

/* Until now_ticks reaches t: sleeps, then yields for the last millisecond. */
static void wait_until(int64_t t)
{
    const int64_t ms = ticks_per_s() / 1000;
    for (;;) {
        const int64_t left = t - now_ticks();
        if (left <= 0) return;
#ifdef _WIN32
        if (left > 2 * ms) Sleep((DWORD)(left / ms - 1));
        else SwitchToThread();
#else
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = left > 2 * ms ? (long)(left - ms) : 0;
        nanosleep(&ts, NULL);
#endif
    }
}

/* Interval n (2 to 4) before a FIFO present: the frame before stays on the
 * display n refreshes, as DXGI's sync interval n holds it. The last
 * present's time on the display comes from present_wait: waited for here,
 * it is now; if it was up already, the first vblank on the grid of those
 * seen at or after its present. This present is then queued n refreshes
 * less a quarter after it, so FIFO shows it at the n-th vblank (a display
 * with a variable refresh shows it at once: the frame before stayed n less
 * a quarter). A window the compositor does not show never says: after one
 * wait that timed out the gate is off until a present is seen again. Costs
 * nothing below interval 2; at 2-4, one vkWaitForPresentKHR and the wait it
 * is for. */
static void present_gate(gpu_vk_device* dd, gpu_vk_swap* s, uint32_t n)
{
    if (!(dd->base.caps.bits & GPU_CAP_PRESENT_WAIT) || s->present_id <= s->chain_id0 || !s->period || !s->sc)
        return;
    const int64_t p = s->period, t0 = now_ticks();
    const uint64_t ns = (uint64_t)((n + 2) * p) * 1000000000ull / (uint64_t)ticks_per_s();
    const VkResult r = dd->vk.vkWaitForPresentKHR(dd->device, s->sc, s->present_id, s->gate_miss ? 0 : ns);
    const int64_t t1 = now_ticks();
    if (r != VK_SUCCESS) {
        if (r == VK_TIMEOUT) s->gate_miss = 1;
        else gpu_vk_lost_note(dd, r);
        return;
    }
    s->gate_miss = 0;
    int64_t shown = t1;
    if (t1 - t0 > p / 16) {
        s->vblank = t1;   /* (waited for: it went up just now) */
    } else if (s->vblank && s->queued_at > s->vblank) {
        const int64_t k = (s->queued_at - s->vblank + p - 1) / p;
        shown = s->vblank + k * p;
        if (shown > t1) shown = t1;
    }
    s->done_id = s->present_id;
    s->done_qpc = shown;
    wait_until(shown + (int64_t)n * p - p / 4);
    s->gate_n++;
    s->gate_ticks += now_ticks() - t0;
}

static VkFormat vkfmt_of(gpu_format f)
{
    switch (f) {
    case GPU_FORMAT_R8G8B8A8_UNORM:     return VK_FORMAT_R8G8B8A8_UNORM;
    case GPU_FORMAT_B8G8R8A8_UNORM:     return VK_FORMAT_B8G8R8A8_UNORM;
    case GPU_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case GPU_FORMAT_R10G10B10A2_UNORM:  return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    default:                            return VK_FORMAT_UNDEFINED;
    }
}

static VkColorSpaceKHR cs_of(uint32_t colour_space)
{
    switch (colour_space) {
    case GPU_COLOR_SCRGB: return VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    case GPU_COLOR_HDR10: return VK_COLOR_SPACE_HDR10_ST2084_EXT;
    default:              return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }
}

static int surface_has(gpu_vk_device* dd, const gpu_vk_swap* s, VkFormat f, VkColorSpaceKHR cs)
{
    VkSurfaceFormatKHR list[64];
    uint32_t n = 64;
    const VkResult r = vkGetPhysicalDeviceSurfaceFormatsKHR(dd->phys, s->surface, &n, list);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (list[i].format == f && list[i].colorSpace == cs) return 1;
    return 0;
}

/* swapchain_maintenance1: can a FIFO swap chain also present IMMEDIATE? */
static int modes_compatible(gpu_vk_device* dd, const gpu_vk_swap* s)
{
    if (!dd->has_swap_maint1 || !dd->has_surface_caps2 || !s->has_immediate) return 0;
    VkSurfacePresentModeKHR pm;
    VkPhysicalDeviceSurfaceInfo2KHR si;
    VkPresentModeKHR modes[16];
    VkSurfacePresentModeCompatibilityKHR pc;
    VkSurfaceCapabilities2KHR c2;
    memset(&pm, 0, sizeof pm);
    memset(&si, 0, sizeof si);
    memset(&pc, 0, sizeof pc);
    memset(&c2, 0, sizeof c2);
    pm.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR;
    pm.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    si.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    si.pNext = &pm;
    si.surface = s->surface;
    pc.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR;
    pc.presentModeCount = 16;
    pc.pPresentModes = modes;
    c2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    c2.pNext = &pc;
    if (vkGetPhysicalDeviceSurfaceCapabilities2KHR(dd->phys, &si, &c2) != VK_SUCCESS) return 0;
    for (uint32_t i = 0; i < pc.presentModeCount && i < 16; i++)
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) return 1;
    return 0;
}

/* The presents' fences (maintenance1): once they signal, the presents are
 * done with their semaphores. */
static void presents_wait(gpu_vk_device* dd, gpu_vk_swap* s)
{
    for (uint32_t i = 0; i < GPU_VK_MAX_IMAGES; i++)
        if (s->fence_pending[i]) {
            dd->vk.vkWaitForFences(dd->device, 1, &s->present_fence[i], VK_TRUE, 2000000000ull);
            s->fence_pending[i] = 0;
        }
}

/* The images' textures and the semaphores go (the GPU idle, the presents
 * done: the caller's). */
static void chain_drop(gpu_vk_device* dd, gpu_vk_swap* s)
{
    gpu_vk_lock(&dd->lock);
    for (uint32_t i = 0; i < GPU_VK_MAX_IMAGES; i++)
        if (GPU_VALID(s->image[i])) {
            if (gpu_vk_slot_valid(&dd->tex_slots, s->image[i].id)) gpu_vk_tex_free_now(dd, s->image[i].id);
            s->image[i].id = 0;
        }
    gpu_vk_unlock(&dd->lock);
    for (uint32_t i = 0; i <= GPU_VK_MAX_IMAGES; i++)
        if (s->acquire_sem[i]) {
            dd->vk.vkDestroySemaphore(dd->device, s->acquire_sem[i], NULL);
            s->acquire_sem[i] = VK_NULL_HANDLE;
        }
    for (uint32_t i = 0; i < GPU_VK_MAX_IMAGES; i++) {
        if (s->image_sem[i]) dd->vk.vkDestroySemaphore(dd->device, s->image_sem[i], NULL);
        if (s->done_sem[i]) dd->vk.vkDestroySemaphore(dd->device, s->done_sem[i], NULL);
        if (s->present_fence[i]) dd->vk.vkDestroyFence(dd->device, s->present_fence[i], NULL);
        s->image_sem[i] = s->done_sem[i] = VK_NULL_HANDLE;
        s->present_fence[i] = VK_NULL_HANDLE;
        s->fence_pending[i] = 0;
        if (s->blit_fence[i]) dd->vk.vkDestroyFence(dd->device, s->blit_fence[i], NULL);
        s->blit_fence[i] = VK_NULL_HANDLE;
        s->blit_cb[i] = VK_NULL_HANDLE;
    }
    if (s->blit_pool) dd->vk.vkDestroyCommandPool(dd->device, s->blit_pool, NULL);   /* (and its recorders) */
    s->blit_pool = VK_NULL_HANDLE;
    s->nimages = 0;
    s->current = -1;
}

/* ---- step L1 ---- */
/* Can an RGBA8 swap chain be a BGRA8 one with the frame blitted in: the
 * surface has BGRA8 images that take a copy, and the device blits between
 * the two. */
static int shadow_ok(gpu_vk_device* dd, const gpu_vk_swap* s, const VkSurfaceCapabilitiesKHR* caps)
{
    VkFormatProperties src, dst;
    if (s->vkfmt != VK_FORMAT_R8G8B8A8_UNORM || s->cs != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
        !(caps->supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !surface_has(dd, s, VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR))
        return 0;
    vkGetPhysicalDeviceFormatProperties(dd->phys, VK_FORMAT_R8G8B8A8_UNORM, &src);
    vkGetPhysicalDeviceFormatProperties(dd->phys, VK_FORMAT_B8G8R8A8_UNORM, &dst);
    return (src.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
           (dst.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
}

/* The present's blit of image idx (the core's, GENERAL in its PRESENT state)
 * into the swap chain's, which ends in PRESENT_SRC. Its last blit is done
 * first (the image's fence), so its recorder is free. */
static int shadow_record(gpu_vk_device* dd, gpu_vk_swap* s, uint32_t idx)
{
    const VkImage from = dd->tex[GPU_HANDLE_INDEX(s->image[idx].id)].image;
    const VkCommandBuffer cb = s->blit_cb[idx];
    dd->vk.vkWaitForFences(dd->device, 1, &s->blit_fence[idx], VK_TRUE, 2000000000ull);
    dd->vk.vkResetFences(dd->device, 1, &s->blit_fence[idx]);
    dd->vk.vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dd->vk.vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return 0;
    VkImageMemoryBarrier2 b[2];
    VkDependencyInfo di;
    memset(b, 0, sizeof b);
    memset(&di, 0, sizeof di);
    for (int i = 0; i < 2; i++) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b[i].subresourceRange.levelCount = b[i].subresourceRange.layerCount = 1;
        b[i].dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    }
    b[0].image = from;   /* the frame's writes, then the blit reads */
    b[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b[0].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b[0].oldLayout = b[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b[1].image = s->vkimage[idx];   /* after the acquire (this submit waits for it at the blit) */
    b[1].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    b[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 2;
    di.pImageMemoryBarriers = b;
    dd->vk.vkCmdPipelineBarrier2(cb, &di);
    VkImageBlit r;
    memset(&r, 0, sizeof r);
    r.srcSubresource.aspectMask = r.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.srcSubresource.layerCount = r.dstSubresource.layerCount = 1;
    r.srcOffsets[1].x = r.dstOffsets[1].x = (int32_t)s->width;
    r.srcOffsets[1].y = r.dstOffsets[1].y = (int32_t)s->height;
    r.srcOffsets[1].z = r.dstOffsets[1].z = 1;
    dd->vk.vkCmdBlitImage(cb, from, VK_IMAGE_LAYOUT_GENERAL, s->vkimage[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          1, &r, VK_FILTER_NEAREST);
    b[1].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;   /* then the present */
    b[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b[1].dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    b[1].dstAccessMask = 0;
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b[1];
    dd->vk.vkCmdPipelineBarrier2(cb, &di);
    return dd->vk.vkEndCommandBuffer(cb) == VK_SUCCESS;
}

/* The swap chain, made (again) for the window's size now. */
static int chain_make(gpu_vk_device* dd, gpu_vk_swap* s)
{
    VkSurfaceCapabilitiesKHR caps;
    const VkResult capr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dd->phys, s->surface, &caps);
    if (capr != VK_SUCCESS) {
        if (capr == VK_ERROR_SURFACE_LOST_KHR) s->surface_lost = 1;   /* (step VC2: surface_again) */
        gpu_vk_lost_note(dd, capr);
        return -1;
    }
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext.width = s->width < caps.minImageExtent.width ? caps.minImageExtent.width : s->width;
        ext.height = s->height < caps.minImageExtent.height ? caps.minImageExtent.height : s->height;
        if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    if (!ext.width || !ext.height) return -1;   /* (minimized: no images until it is shown again) */

    /* scRGB is RGBA16F's colour space where the surface has nothing else for it. */
    VkFormat chain_fmt = s->vkfmt;
    s->shadow = 0;
    if (!surface_has(dd, s, s->vkfmt, s->cs)) {
        if (s->vkfmt == VK_FORMAT_R16G16B16A16_SFLOAT && dd->has_colorspace &&
            surface_has(dd, s, s->vkfmt, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)) {
            s->cs = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
            s->colour_space = GPU_COLOR_SCRGB;
        } else if (surface_has(dd, s, s->vkfmt, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)) {
            s->cs = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            s->colour_space = GPU_COLOR_SRGB;
        } else if (shadow_ok(dd, s, &caps)) {   /* (step L1: BGRA8, the frame blitted in) */
            static int said;
            if (!said++) printf("[VK] the window takes no RGBA8 images: its swap chain is BGRA8, each frame copied in\n");
            chain_fmt = VK_FORMAT_B8G8R8A8_UNORM;
            s->cs = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            s->colour_space = GPU_COLOR_SRGB;
            s->shadow = 1;
        } else {
            printf("[VK] the window takes no swap chain of format %u\n", (unsigned)s->format);
            return -1;
        }
    }
    {
        VkPresentModeKHR modes[16];
        uint32_t n = 16;
        s->has_immediate = 0;
        const VkResult r = vkGetPhysicalDeviceSurfacePresentModesKHR(dd->phys, s->surface, &n, modes);
        if (r == VK_SUCCESS || r == VK_INCOMPLETE)
            for (uint32_t i = 0; i < n; i++)
                if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) s->has_immediate = 1;
    }
    s->modes_switch = (uint32_t)modes_compatible(dd, s);
    s->mode = s->want_immediate && s->has_immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t count = s->buffers > caps.minImageCount ? s->buffers : caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > GPU_VK_MAX_IMAGES) count = GPU_VK_MAX_IMAGES;

    VkSwapchainCreateInfoKHR ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = s->surface;
    ci.minImageCount = count;
    ci.imageFormat = chain_fmt;
    ci.imageColorSpace = s->cs;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = caps.supportedUsageFlags & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha &
                                                            (~caps.supportedCompositeAlpha + 1u));
    ci.presentMode = s->mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = s->sc;
    const VkPresentModeKHR both[2] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    VkSwapchainPresentModesCreateInfoKHR pm;
    memset(&pm, 0, sizeof pm);
    if (s->modes_switch) {
        pm.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR;
        pm.presentModeCount = 2;
        pm.pPresentModes = both;
        ci.pNext = &pm;
    }
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    const VkResult r = dd->vk.vkCreateSwapchainKHR(dd->device, &ci, NULL, &sc);
    if (s->sc) dd->vk.vkDestroySwapchainKHR(dd->device, s->sc, NULL);   /* (retired either way) */
    s->sc = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) {
        if (r == VK_ERROR_SURFACE_LOST_KHR) s->surface_lost = 1;
        gpu_vk_lost_note(dd, r);
        return -1;
    }
    s->sc = sc;
    s->width = ext.width;
    s->height = ext.height;

    VkImage imgs[GPU_VK_MAX_IMAGES];
    uint32_t n = GPU_VK_MAX_IMAGES;
    if (dd->vk.vkGetSwapchainImagesKHR(dd->device, sc, &n, imgs) != VK_SUCCESS) return -1;
    gpu_texture_desc td;
    memset(&td, 0, sizeof td);
    td.width = ext.width;
    td.height = ext.height;
    td.layers = 1;
    td.mips = 1;
    td.format = s->format;
    td.samples = 1;
    td.use = GPU_USE_RT | GPU_USE_SAMPLED | GPU_USE_COPY_SRC | GPU_USE_COPY_DST;
    td.state = GPU_STATE_PRESENT;
    VkSemaphoreCreateInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (s->shadow) {   /* (step L1: the blits' recorders, one an image) */
        VkCommandPoolCreateInfo pi;
        VkCommandBufferAllocateInfo ai;
        memset(&pi, 0, sizeof pi);
        memset(&ai, 0, sizeof ai);
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = dd->qfamily;
        if (dd->vk.vkCreateCommandPool(dd->device, &pi, NULL, &s->blit_pool) != VK_SUCCESS) return -1;
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = s->blit_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = n;
        if (dd->vk.vkAllocateCommandBuffers(dd->device, &ai, s->blit_cb) != VK_SUCCESS) return -1;
        td.name = "swap chain image (RGBA8, blitted)";
    }
    for (uint32_t i = 0; i < n; i++) {
        s->vkimage[i] = imgs[i];
        if (s->shadow) {
            VkFenceCreateInfo bf = fi;
            bf.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            s->image[i] = gpu_vk_texture_create(&dd->base, &td);
            if (dd->vk.vkCreateFence(dd->device, &bf, NULL, &s->blit_fence[i]) != VK_SUCCESS) return -1;
        } else
            s->image[i] = gpu_vk_tex_add(dd, imgs[i], VK_NULL_HANDLE, &td, GPU_VK_TEX_SWAP, 0);
        if (!GPU_VALID(s->image[i]) ||
            dd->vk.vkCreateSemaphore(dd->device, &si, NULL, &s->done_sem[i]) != VK_SUCCESS ||
            (dd->has_swap_maint1 && dd->vk.vkCreateFence(dd->device, &fi, NULL, &s->present_fence[i]) != VK_SUCCESS))
            return -1;
        s->image_sem[i] = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i <= n; i++)
        if (dd->vk.vkCreateSemaphore(dd->device, &si, NULL, &s->acquire_sem[i]) != VK_SUCCESS) return -1;
    s->nimages = n;
    s->next_sem = n + 1;   /* free acquire semaphores: a stack */
    s->current = -1;
    s->waited = s->signalled = 0;
    s->recreate = 0;
    if (!s->has_immediate) dd->base.caps.bits &= ~(uint32_t)GPU_CAP_TEARING;
    s->period = refresh_ticks(s->window);   /* (step VC2: present_gate; a new chain, maybe a new display) */
    s->vblank = 0;
    s->chain_id0 = s->present_id;
    return 0;
}

/* Made again: the GPU idle first, and the presents done. */
static int chain_again(gpu_vk_device* dd, gpu_vk_swap* s)
{
    dd->vk.vkDeviceWaitIdle(dd->device);
    presents_wait(dd, s);
    chain_drop(dd, s);
    return chain_make(dd, s);
}

/* The window's surface, one the queue can present to; none on failure. */
static VkSurfaceKHR surface_make(gpu_vk_device* dd, void* window)
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR wi;
    memset(&wi, 0, sizeof wi);
    wi.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    wi.hinstance = GetModuleHandleA(NULL);
    wi.hwnd = (HWND)window;
    if (vkCreateWin32SurfaceKHR(dd->instance, &wi, NULL, &surface) != VK_SUCCESS) return VK_NULL_HANDLE;
#else
    if (!SDL_Vulkan_CreateSurface((SDL_Window*)window, dd->instance, &surface)) return VK_NULL_HANDLE;
#endif
    VkBool32 ok = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(dd->phys, dd->qfamily, surface, &ok) != VK_SUCCESS || !ok) {
        vkDestroySurfaceKHR(dd->instance, surface, NULL);
        return VK_NULL_HANDLE;
    }
    return surface;
}

/* Step VC2: the window's surface was lost (VK_ERROR_SURFACE_LOST_KHR): a
 * new one from the same window, and a swap chain on it. 0 when made; -1 when
 * not yet (a minimized window has no images), and then the next acquire
 * tries again. When the surface or its swap chain cannot be made, the swap
 * chain gives up: no image, and GPU_PRESENT_SURFACE_LOST from every present. */
static int surface_again(gpu_vk_device* dd, gpu_vk_swap* s)
{
    dd->vk.vkDeviceWaitIdle(dd->device);
    presents_wait(dd, s);
    chain_drop(dd, s);
    if (s->sc) dd->vk.vkDestroySwapchainKHR(dd->device, s->sc, NULL);   /* (before its surface) */
    s->sc = VK_NULL_HANDLE;
    if (s->surface) vkDestroySurfaceKHR(dd->instance, s->surface, NULL);
    s->surface = surface_make(dd, s->window);
    s->surface_lost = 0;
    if (s->surface && chain_make(dd, s) == 0) {
        printf("[VK] the window's surface was lost; made again\n");
        return 0;
    }
    if (s->surface && !s->surface_lost) return -1;
    s->surface_dead = 1;
    printf("[VK] the window's surface was lost and cannot be made again: nothing more is shown in it\n");
    return -1;
}

gpu_swapchain gpu_vk_swap_create(gpu_device* d, const gpu_swapchain_desc* desc)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_swapchain r = {0};
    const VkFormat vkfmt = vkfmt_of(desc->format);
    if (!dd->has_surface || !desc->window || !vkfmt) return r;
    const VkSurfaceKHR surface = surface_make(dd, desc->window);
    if (!surface) return r;
    gpu_vk_lock(&dd->lock);
    r.id = gpu_vk_slot_alloc(&dd->swap_slots);
    gpu_vk_unlock(&dd->lock);
    if (!r.id) {
        vkDestroySurfaceKHR(dd->instance, surface, NULL);
        return r;
    }
    gpu_vk_swap* const s = &dd->swap[GPU_HANDLE_INDEX(r.id)];
    memset(s, 0, sizeof *s);
    s->surface = surface;
    s->window = desc->window;
    s->buffers = desc->buffers ? (desc->buffers < GPU_VK_MAX_IMAGES ? desc->buffers : GPU_VK_MAX_IMAGES) : 2u;
    s->width = desc->width;
    s->height = desc->height;
    s->format = desc->format;
    s->vkfmt = vkfmt;
    s->colour_space = desc->colour_space;
    s->cs = cs_of(desc->colour_space);
    s->current = -1;
    if (chain_make(dd, s)) {
        gpu_vk_swap_destroy(d, r);
        r.id = 0;
    }
    return r;
}

void gpu_vk_swap_destroy(gpu_device* d, gpu_swapchain sc)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_vk_swap* const s = swap_of(dd, sc);
    if (!s) return;
    dd->vk.vkDeviceWaitIdle(dd->device);
    presents_wait(dd, s);
    chain_drop(dd, s);
    if (s->sc) dd->vk.vkDestroySwapchainKHR(dd->device, s->sc, NULL);
    if (s->surface) vkDestroySurfaceKHR(dd->instance, s->surface, NULL);
    memset(s, 0, sizeof *s);
    gpu_vk_lock(&dd->lock);
    gpu_vk_slot_free(&dd->swap_slots, sc.id);
    gpu_vk_unlock(&dd->lock);
}

/* The window's size now (width and height are used only where the surface
 * leaves the size to the swap chain), in the format asked. */
int gpu_vk_swap_resize(gpu_device* d, gpu_swapchain sc, uint32_t width, uint32_t height, gpu_format format)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_vk_swap* const s = swap_of(dd, sc);
    const VkFormat vkfmt = vkfmt_of(format);
    if (!s || !vkfmt) return -1;
    s->width = width;
    s->height = height;
    s->format = format;
    s->vkfmt = vkfmt;
    return chain_again(dd, s);
}

uint32_t gpu_vk_swap_acquire(gpu_device* d, gpu_swapchain sc, gpu_texture* image)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_vk_swap* const s = swap_of(dd, sc);
    gpu_texture none = {0};
    if (image) *image = none;
    if (!s) return 0;
    if (s->current >= 0) {
        if (image) *image = s->image[s->current];
        return (uint32_t)s->current;
    }
    if (s->surface_dead) return 0;   /* (step VC2: surface_again) */
    if (!s->surface_lost && (s->recreate || !s->sc)) chain_again(dd, s);
    if (s->surface_lost) surface_again(dd, s);
    for (int tries = 0; s->sc && tries < 2; tries++) {
        const VkSemaphore sem = s->acquire_sem[s->next_sem - 1];
        uint32_t idx = 0;
        const VkResult r = dd->vk.vkAcquireNextImageKHR(dd->device, s->sc, 2000000000ull, sem, VK_NULL_HANDLE, &idx);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            if (r == VK_SUBOPTIMAL_KHR) s->recreate = 1;
            /* The image's last acquire semaphore is free again (its waits
             * are done: the image came back). */
            s->acquire_sem[--s->next_sem] = VK_NULL_HANDLE;
            if (s->image_sem[idx]) s->acquire_sem[s->next_sem++] = s->image_sem[idx];
            s->image_sem[idx] = sem;
            s->current = (int32_t)idx;
            s->waited = s->signalled = 0;
            if (image) *image = s->image[idx];
            return idx;
        }
        if (r == VK_ERROR_SURFACE_LOST_KHR) {
            surface_again(dd, s);
            continue;
        }
        if (r != VK_ERROR_OUT_OF_DATE_KHR) {
            gpu_vk_lost_note(dd, r);
            return 0;
        }
        if (chain_again(dd, s) && s->surface_lost) surface_again(dd, s);
    }
    return 0;
}

uint32_t gpu_vk_swap_image_count(gpu_device* d, gpu_swapchain sc)
{
    gpu_vk_swap* const s = swap_of(GPU_VK_DEV(d), sc);
    return s ? s->nimages : 0u;
}

gpu_texture gpu_vk_swap_image(gpu_device* d, gpu_swapchain sc, uint32_t index)
{
    gpu_vk_swap* const s = swap_of(GPU_VK_DEV(d), sc);
    gpu_texture none = {0};
    return s && index < s->nimages ? s->image[index] : none;
}

void gpu_vk_swap_submit_sems(gpu_vk_device* dd, const gpu_submit_desc* how, VkSemaphoreSubmitInfo* wait,
                             uint32_t* nwait, VkSemaphoreSubmitInfo* sig, uint32_t* nsig)
{
    if (!how || !(how->flags & (GPU_SUBMIT_ACQUIRE_WAIT | GPU_SUBMIT_PRESENT_SIGNAL))) return;
    gpu_vk_swap* const s = swap_of(dd, how->swapchain);
    if (!s || s->current < 0) return;
    /* (step L1: with the frame blitted in, the blit waits for the acquire;
     * the core's submits draw into images of the backend's) */
    if ((how->flags & GPU_SUBMIT_ACQUIRE_WAIT) && !s->waited && !s->shadow) {
        memset(&wait[*nwait], 0, sizeof wait[*nwait]);
        wait[*nwait].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait[*nwait].semaphore = s->image_sem[s->current];
        wait[*nwait].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        (*nwait)++;
        s->waited = 1;
    }
    /* ---- step VC1 ---- */
    /* PRESENT_SIGNAL signals nothing here: a binary semaphore is signalled
     * once, and the first such submit need not be the last (the core says it
     * on every submit of a frame). The present's own empty submit signals
     * it, after all of them. */
    (void)sig;
    (void)nsig;
}

/* gpu.h's codes (step VC2): GPU_PRESENTED (out of date and suboptimal too:
 * the next acquire makes it again); GPU_PRESENT_LOST on a lost device;
 * GPU_PRESENT_SURFACE_LOST once the window's surface cannot be made again
 * (one lost is made again here, and this frame is GPU_PRESENT_FAILED);
 * GPU_PRESENT_FAILED with no image acquired or another failure. */
int gpu_vk_swap_present(gpu_device* d, gpu_swapchain sc, uint32_t interval, uint32_t flags)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_vk_swap* const s = swap_of(dd, sc);
    (void)flags;
    if (!s) return GPU_PRESENT_FAILED;
    if (s->surface_dead) return GPU_PRESENT_SURFACE_LOST;
    if (dd->lost) return GPU_PRESENT_LOST;
    if (s->current < 0 || !s->sc) return GPU_PRESENT_FAILED;
    const uint32_t idx = (uint32_t)s->current;
    const int imm = interval == 0 && s->has_immediate && (dd->base.caps.bits & GPU_CAP_TEARING);
    VkPresentModeKHR mode = imm ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    s->want_immediate = (uint32_t)imm;
    if (interval >= 2) present_gate(dd, s, interval > 4 ? 4u : interval);   /* (step VC2: RSX_VSYNC=2-4) */
    if (s->fence_pending[idx]) {
        dd->vk.vkWaitForFences(dd->device, 1, &s->present_fence[idx], VK_TRUE, 2000000000ull);
        s->fence_pending[idx] = 0;
    }
    if (s->present_fence[idx]) dd->vk.vkResetFences(dd->device, 1, &s->present_fence[idx]);
    const int blit = s->shadow && !s->signalled && shadow_record(dd, s, idx);   /* (step L1) */
    gpu_vk_lock(&dd->qlock);
    if (!s->signalled) {   /* (step VC1: always, gpu_vk_swap_submit_sems says why) */
        VkSemaphoreSubmitInfo w, g;
        VkCommandBufferSubmitInfo cbi;
        VkSubmitInfo2 si;
        memset(&w, 0, sizeof w);
        memset(&g, 0, sizeof g);
        memset(&cbi, 0, sizeof cbi);
        memset(&si, 0, sizeof si);
        w.sType = g.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        w.semaphore = s->image_sem[idx];
        w.stageMask = blit ? VK_PIPELINE_STAGE_2_BLIT_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        g.semaphore = s->done_sem[idx];
        g.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cbi.commandBuffer = s->blit_cb[idx];
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        si.waitSemaphoreInfoCount = s->waited ? 0u : 1u;
        si.pWaitSemaphoreInfos = &w;
        si.commandBufferInfoCount = blit ? 1u : 0u;
        si.pCommandBufferInfos = &cbi;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &g;
        dd->vk.vkQueueSubmit2(dd->queue, 1, &si, blit ? s->blit_fence[idx] : VK_NULL_HANDLE);
        s->waited = s->signalled = 1;
    }
    VkPresentInfoKHR pi;
    VkPresentIdKHR pid;
    VkSwapchainPresentModeInfoKHR pm;
    VkSwapchainPresentFenceInfoKHR pf;
    uint64_t id = 0;
    const void** tail = &pi.pNext;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &s->done_sem[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &s->sc;
    pi.pImageIndices = &idx;
    if (dd->has_present_id) {
        id = ++s->present_id;
        memset(&pid, 0, sizeof pid);
        pid.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
        pid.swapchainCount = 1;
        pid.pPresentIds = &id;
        *tail = &pid;
        tail = &pid.pNext;
    }
    if (s->modes_switch) {
        memset(&pm, 0, sizeof pm);
        pm.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR;
        pm.swapchainCount = 1;
        pm.pPresentModes = &mode;
        *tail = &pm;
        tail = &pm.pNext;
        s->mode = mode;
    } else if (mode != s->mode) {
        s->recreate = 1;   /* (the new mode from the next acquire) */
    }
    if (s->present_fence[idx]) {
        memset(&pf, 0, sizeof pf);
        pf.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR;
        pf.swapchainCount = 1;
        pf.pFences = &s->present_fence[idx];
        *tail = &pf;
        tail = &pf.pNext;
    }
    const VkResult r = dd->vk.vkQueuePresentKHR(dd->queue, &pi);
    gpu_vk_unlock(&dd->qlock);
    s->queued_at = now_ticks();
    s->current = -1;
    const int code = gpu_vk_present_code(r);
    if (code == GPU_PRESENTED) {
        if (s->present_fence[idx]) s->fence_pending[idx] = 1;
        if (r != VK_SUCCESS) s->recreate = 1;
        return GPU_PRESENTED;
    }
    gpu_vk_lost_note(dd, r);
    if (code == GPU_PRESENT_SURFACE_LOST) {
        s->surface_lost = 1;
        return surface_again(dd, s) == 0 || !s->surface_dead ? GPU_PRESENT_FAILED : GPU_PRESENT_SURFACE_LOST;
    }
    return code;
}

/* ---- step VC2 ---- */
/* What a present's (or an acquire's) VkResult means, as gpu.h's codes. */
int gpu_vk_present_code(VkResult r)
{
    switch (r) {
    case VK_SUCCESS:
    case VK_SUBOPTIMAL_KHR:
    case VK_ERROR_OUT_OF_DATE_KHR:   /* (made again at the next acquire) */
        return GPU_PRESENTED;
    case VK_ERROR_DEVICE_LOST:      return GPU_PRESENT_LOST;
    case VK_ERROR_SURFACE_LOST_KHR: return GPU_PRESENT_SURFACE_LOST;
    default:                        return GPU_PRESENT_FAILED;
    }
}

/* present_wait: the newest present that reached the display, and when this
 * call saw it (sync_qpc); refresh counts are V3's (present timing). */
int gpu_vk_swap_stats(gpu_device* d, gpu_swapchain sc, gpu_present_stats* out)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_vk_swap* const s = swap_of(dd, sc);
    if (!s || !s->sc || !(dd->base.caps.bits & GPU_CAP_PRESENT_WAIT) || !s->present_id) return 1;
    gpu_vk_lock(&dd->qlock);
    for (uint64_t id = s->present_id; id > s->done_id && id > s->chain_id0 && id + 4 > s->present_id; id--) {
        const VkResult r = dd->vk.vkWaitForPresentKHR(dd->device, s->sc, id, 0);
        if (r == VK_SUCCESS) {
            s->done_id = id;
            s->done_qpc = now_ticks();
            break;
        }
        if (r != VK_TIMEOUT) break;
    }
    gpu_vk_unlock(&dd->qlock);
    if (!s->done_id) return 1;
    memset(out, 0, sizeof *out);
    out->present_count = (uint32_t)s->done_id;
    out->last_present_count = (uint32_t)s->present_id;
    out->sync_qpc = s->done_qpc;
    return 0;
}

/* A new colour space makes the swap chain again (the GPU must be idle, as
 * for swap_resize); scRGB is RGBA16F with EXTENDED_SRGB_LINEAR. 1 if taken. */
int gpu_vk_swap_hdr(gpu_device* d, gpu_swapchain sc, uint32_t colour_space, const gpu_hdr_metadata* meta)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_vk_swap* const s = swap_of(dd, sc);
    if (!s || !s->sc) return 0;
    if (colour_space != s->colour_space) {
        const VkColorSpaceKHR cs = cs_of(colour_space);
        if (colour_space != GPU_COLOR_SRGB && !(dd->base.caps.bits & GPU_CAP_HDR_SCRGB)) return 0;
        /* As DXGI has them: scRGB on RGBA16F, HDR10 on R10G10B10A2. */
        if ((colour_space == GPU_COLOR_SCRGB && s->format != GPU_FORMAT_R16G16B16A16_FLOAT) ||
            (colour_space == GPU_COLOR_HDR10 && s->format != GPU_FORMAT_R10G10B10A2_UNORM))
            return 0;
        if (!surface_has(dd, s, s->vkfmt, cs)) return 0;
        s->colour_space = colour_space;
        s->cs = cs;
        if (chain_again(dd, s)) return 0;
    }
    if (meta) {
        if (!(dd->base.caps.bits & GPU_CAP_HDR_METADATA)) return 0;
        VkHdrMetadataEXT md;
        memset(&md, 0, sizeof md);
        md.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;
        md.displayPrimaryRed.x = meta->red[0];
        md.displayPrimaryRed.y = meta->red[1];
        md.displayPrimaryGreen.x = meta->green[0];
        md.displayPrimaryGreen.y = meta->green[1];
        md.displayPrimaryBlue.x = meta->blue[0];
        md.displayPrimaryBlue.y = meta->blue[1];
        md.whitePoint.x = meta->white[0];
        md.whitePoint.y = meta->white[1];
        md.maxLuminance = meta->max_nits;
        md.minLuminance = meta->min_nits;
        md.maxContentLightLevel = meta->max_cll;
        md.maxFrameAverageLightLevel = meta->max_fall;
        dd->vk.vkSetHdrMetadataEXT(dd->device, 1, &s->sc, &md);
    }
    return 1;
}

#endif /* PS3RECOMP_RSX_HAL && PS3RECOMP_RSX_HAL_VULKAN */
