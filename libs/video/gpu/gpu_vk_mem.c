/*
 * ps3recomp - the portable renderer's GPU layer: the Vulkan backend's memory
 *
 * Formats, buffers, heaps and textures, their footprints (D3D12's rules, so
 * the copied code's upload arithmetic holds), freeing after a fence, and
 * new textures' first layouts.
 *
 * Built with PS3RECOMP_RSX_HAL and PS3RECOMP_RSX_HAL_VULKAN.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(PS3RECOMP_RSX_HAL_VULKAN)

#include "gpu_vk_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Formats
 * -------------------------------------------------------------------------*/

#define SZ VK_COMPONENT_SWIZZLE_ZERO
#define SO VK_COMPONENT_SWIZZLE_ONE
#define SR VK_COMPONENT_SWIZZLE_R
#define SG VK_COMPONENT_SWIZZLE_G
#define SB VK_COMPONENT_SWIZZLE_B
#define SA VK_COMPONENT_SWIZZLE_A
#define ID4 {SR, SG, SB, SA}
#define C_ GPU_VK_ASPECT_COLOR
#define D_ GPU_VK_ASPECT_DEPTH
#define DS_ (GPU_VK_ASPECT_DEPTH | GPU_VK_ASPECT_STENCIL)
#define S_ GPU_VK_ASPECT_STENCIL

/* ds: 0 any texture, 1 only a depth target's (GPU_USE_DS), 2 only another's. */
typedef struct fmt_row {
    uint16_t      dxgi;
    uint8_t       ds;
    gpu_vk_format f;
} fmt_row;

static const fmt_row k_formats[] = {
    {1,   0, {VK_FORMAT_R32G32B32A32_SFLOAT, 16, 1, C_, 0, 1, ID4}},
    {2,   0, {VK_FORMAT_R32G32B32A32_SFLOAT, 16, 1, C_, 0, 0, ID4}},
    {3,   0, {VK_FORMAT_R32G32B32A32_UINT, 16, 1, C_, 1, 0, ID4}},
    {4,   0, {VK_FORMAT_R32G32B32A32_SINT, 16, 1, C_, 1, 0, ID4}},
    {6,   0, {VK_FORMAT_R32G32B32_SFLOAT, 12, 1, C_, 0, 0, ID4}},
    {7,   0, {VK_FORMAT_R32G32B32_UINT, 12, 1, C_, 1, 0, ID4}},
    {9,   0, {VK_FORMAT_R16G16B16A16_SFLOAT, 8, 1, C_, 0, 1, ID4}},
    {10,  0, {VK_FORMAT_R16G16B16A16_SFLOAT, 8, 1, C_, 0, 0, ID4}},
    {11,  0, {VK_FORMAT_R16G16B16A16_UNORM, 8, 1, C_, 0, 0, ID4}},
    {12,  0, {VK_FORMAT_R16G16B16A16_UINT, 8, 1, C_, 1, 0, ID4}},
    {13,  0, {VK_FORMAT_R16G16B16A16_SNORM, 8, 1, C_, 0, 0, ID4}},
    {14,  0, {VK_FORMAT_R16G16B16A16_SINT, 8, 1, C_, 1, 0, ID4}},
    {16,  0, {VK_FORMAT_R32G32_SFLOAT, 8, 1, C_, 0, 0, ID4}},
    {17,  0, {VK_FORMAT_R32G32_UINT, 8, 1, C_, 1, 0, ID4}},
    {19,  1, {VK_FORMAT_D32_SFLOAT_S8_UINT, 4, 1, DS_, 0, 1, ID4}},
    {20,  0, {VK_FORMAT_D32_SFLOAT_S8_UINT, 4, 1, DS_, 0, 0, ID4}},
    {21,  0, {VK_FORMAT_D32_SFLOAT_S8_UINT, 4, 1, D_, 0, 0, ID4}},
    {22,  0, {VK_FORMAT_D32_SFLOAT_S8_UINT, 4, 1, S_, 1, 0, {SZ, SR, SZ, SO}}},
    {23,  0, {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 1, C_, 0, 1, ID4}},
    {24,  0, {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 1, C_, 0, 0, ID4}},
    {25,  0, {VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, 1, C_, 1, 0, ID4}},
    {26,  0, {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 1, C_, 0, 0, ID4}},
    {27,  0, {VK_FORMAT_R8G8B8A8_UNORM, 4, 1, C_, 0, 1, ID4}},
    {28,  0, {VK_FORMAT_R8G8B8A8_UNORM, 4, 1, C_, 0, 0, ID4}},
    {29,  0, {VK_FORMAT_R8G8B8A8_SRGB, 4, 1, C_, 0, 0, ID4}},
    {30,  0, {VK_FORMAT_R8G8B8A8_UINT, 4, 1, C_, 1, 0, ID4}},
    {31,  0, {VK_FORMAT_R8G8B8A8_SNORM, 4, 1, C_, 0, 0, ID4}},
    {32,  0, {VK_FORMAT_R8G8B8A8_SINT, 4, 1, C_, 1, 0, ID4}},
    {33,  0, {VK_FORMAT_R16G16_UNORM, 4, 1, C_, 0, 1, ID4}},
    {34,  0, {VK_FORMAT_R16G16_SFLOAT, 4, 1, C_, 0, 0, ID4}},
    {35,  0, {VK_FORMAT_R16G16_UNORM, 4, 1, C_, 0, 0, ID4}},
    {36,  0, {VK_FORMAT_R16G16_UINT, 4, 1, C_, 1, 0, ID4}},
    {37,  0, {VK_FORMAT_R16G16_SNORM, 4, 1, C_, 0, 0, ID4}},
    {39,  1, {VK_FORMAT_D32_SFLOAT, 4, 1, D_, 0, 1, ID4}},
    {39,  2, {VK_FORMAT_R32_SFLOAT, 4, 1, C_, 0, 1, ID4}},
    {40,  0, {VK_FORMAT_D32_SFLOAT, 4, 1, D_, 0, 0, ID4}},
    {41,  0, {VK_FORMAT_R32_SFLOAT, 4, 1, C_, 0, 0, ID4}},
    {42,  0, {VK_FORMAT_R32_UINT, 4, 1, C_, 1, 0, ID4}},
    {43,  0, {VK_FORMAT_R32_SINT, 4, 1, C_, 1, 0, ID4}},
    {44,  1, {VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, DS_, 0, 1, ID4}},
    {45,  0, {VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, DS_, 0, 0, ID4}},
    {46,  0, {VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, D_, 0, 0, ID4}},
    {47,  0, {VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, S_, 1, 0, {SZ, SR, SZ, SO}}},
    {48,  0, {VK_FORMAT_R8G8_UNORM, 2, 1, C_, 0, 1, ID4}},
    {49,  0, {VK_FORMAT_R8G8_UNORM, 2, 1, C_, 0, 0, ID4}},
    {50,  0, {VK_FORMAT_R8G8_UINT, 2, 1, C_, 1, 0, ID4}},
    {51,  0, {VK_FORMAT_R8G8_SNORM, 2, 1, C_, 0, 0, ID4}},
    {53,  1, {VK_FORMAT_D16_UNORM, 2, 1, D_, 0, 1, ID4}},
    {53,  2, {VK_FORMAT_R16_UNORM, 2, 1, C_, 0, 1, ID4}},
    {54,  0, {VK_FORMAT_R16_SFLOAT, 2, 1, C_, 0, 0, ID4}},
    {55,  0, {VK_FORMAT_D16_UNORM, 2, 1, D_, 0, 0, ID4}},
    {56,  0, {VK_FORMAT_R16_UNORM, 2, 1, C_, 0, 0, ID4}},
    {57,  0, {VK_FORMAT_R16_UINT, 2, 1, C_, 1, 0, ID4}},
    {58,  0, {VK_FORMAT_R16_SNORM, 2, 1, C_, 0, 0, ID4}},
    {59,  0, {VK_FORMAT_R16_SINT, 2, 1, C_, 1, 0, ID4}},
    {60,  0, {VK_FORMAT_R8_UNORM, 1, 1, C_, 0, 1, ID4}},
    {61,  0, {VK_FORMAT_R8_UNORM, 1, 1, C_, 0, 0, ID4}},
    {62,  0, {VK_FORMAT_R8_UINT, 1, 1, C_, 1, 0, ID4}},
    {63,  0, {VK_FORMAT_R8_SNORM, 1, 1, C_, 0, 0, ID4}},
    {64,  0, {VK_FORMAT_R8_SINT, 1, 1, C_, 1, 0, ID4}},
    {65,  0, {VK_FORMAT_R8_UNORM, 1, 1, C_, 0, 0, {SZ, SZ, SZ, SR}}},
    {70,  0, {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, C_, 0, 1, ID4}},
    {71,  0, {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, C_, 0, 0, ID4}},
    {72,  0, {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, 4, C_, 0, 0, ID4}},
    {73,  0, {VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, C_, 0, 1, ID4}},
    {74,  0, {VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, C_, 0, 0, ID4}},
    {75,  0, {VK_FORMAT_BC2_SRGB_BLOCK, 16, 4, C_, 0, 0, ID4}},
    {76,  0, {VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, C_, 0, 1, ID4}},
    {77,  0, {VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, C_, 0, 0, ID4}},
    {78,  0, {VK_FORMAT_BC3_SRGB_BLOCK, 16, 4, C_, 0, 0, ID4}},
    {80,  0, {VK_FORMAT_BC4_UNORM_BLOCK, 8, 4, C_, 0, 0, ID4}},
    {83,  0, {VK_FORMAT_BC5_UNORM_BLOCK, 16, 4, C_, 0, 0, ID4}},
    {85,  0, {VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 1, C_, 0, 0, ID4}},
    {86,  0, {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, 1, C_, 0, 0, ID4}},
    {87,  0, {VK_FORMAT_B8G8R8A8_UNORM, 4, 1, C_, 0, 0, ID4}},
    {88,  0, {VK_FORMAT_B8G8R8A8_UNORM, 4, 1, C_, 0, 0, {SR, SG, SB, SO}}},
    {90,  0, {VK_FORMAT_B8G8R8A8_UNORM, 4, 1, C_, 0, 1, ID4}},
    {91,  0, {VK_FORMAT_B8G8R8A8_SRGB, 4, 1, C_, 0, 0, ID4}},
    /* DXGI's B4G4R4A4 holds A R G B from the top bit down; Vulkan's
     * (always there) B G R A: so each channel is read from another. */
    {115, 0, {VK_FORMAT_B4G4R4A4_UNORM_PACK16, 2, 1, C_, 0, 0, {SG, SR, SA, SB}}}
};

const gpu_vk_format* gpu_vk_format_of(gpu_format f, uint32_t use)
{
    const uint8_t want = (use & GPU_USE_DS) ? 1 : 2;
    for (size_t i = 0; i < sizeof k_formats / sizeof k_formats[0]; i++)
        if (k_formats[i].dxgi == f && (!k_formats[i].ds || k_formats[i].ds == want)) return &k_formats[i].f;
    return NULL;
}

static VkImageAspectFlags aspect_flags(uint32_t a)
{
    VkImageAspectFlags r = 0;
    if (a & GPU_VK_ASPECT_COLOR) r |= VK_IMAGE_ASPECT_COLOR_BIT;
    if (a & GPU_VK_ASPECT_DEPTH) r |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (a & GPU_VK_ASPECT_STENCIL) r |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return r;
}

/* A texture's format, NULL when the device cannot make it (BC or D24S8
 * turned off, or missing). */
/* Without D24S8 targets (AMD), the D24S8 numbers are D32S8 images, so the
 * core's zeta surfaces work as they are; only their depth plane's bytes in
 * copies are floats. */
const gpu_vk_format* gpu_vk_format_dev(const gpu_vk_device* dd, gpu_format format, uint32_t use)
{
    const gpu_vk_format* f = gpu_vk_format_of(format, use);
    if (!f) return NULL;
    if (f->bw == 4 && !dd->has_bc) return NULL;
    if (f->vk == VK_FORMAT_D24_UNORM_S8_UINT && !dd->has_d24s8) {
        /* R24G8_TYPELESS, D24_UNORM_S8_UINT, R24_UNORM_X8, X24_G8 as R32G8X24_TYPELESS, D32_FLOAT_S8X24_UINT,
         * R32_FLOAT_X8X24, X32_G8X24 */
        if (format < GPU_FORMAT_R24G8_TYPELESS || format > GPU_FORMAT_X24_TYPELESS_G8_UINT) return NULL;
        return gpu_vk_format_of((gpu_format)(GPU_FORMAT_R32G8X24_TYPELESS + (format - GPU_FORMAT_R24G8_TYPELESS)), use);
    }
    return f;
}

static const gpu_vk_format* tex_format(gpu_vk_device* dd, gpu_format format, uint32_t use)
{
    return gpu_vk_format_dev(dd, format, use);
}

/* ---------------------------------------------------------------------------
 * Freeing, now or once the GPU passes a fence
 * -------------------------------------------------------------------------*/

static uint64_t completed(gpu_vk_device* dd)
{
    uint64_t v = 0;
    if (dd->lost || dd->vk.vkGetSemaphoreCounterValue(dd->device, dd->timeline, &v) != VK_SUCCESS) return UINT64_MAX;
    return v;
}

void gpu_vk_tex_free_now(gpu_vk_device* dd, uint32_t id)
{
    const uint32_t i = GPU_HANDLE_INDEX(id);
    gpu_vk_tex* const t = &dd->tex[i];
    for (uint32_t v = t->views; v;) {
        const uint32_t j = v - 1;
        v = dd->view[j].next;
        dd->vk.vkDestroyImageView(dd->device, dd->view[j].iv, NULL);
        memset(&dd->view[j], 0, sizeof dd->view[j]);
        gpu_vk_slot_free(&dd->view_slots, GPU_HANDLE_MAKE(j, dd->view_slots.gen[j]));
    }
    if (!(t->flags & GPU_VK_TEX_SWAP) && t->image) dd->vk.vkDestroyImage(dd->device, t->image, NULL);
    if (t->memory) dd->vk.vkFreeMemory(dd->device, t->memory, NULL);
    memset(t, 0, sizeof *t);
    dd->tex_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    gpu_vk_slot_free(&dd->tex_slots, id);
}

void gpu_vk_buf_free_now(gpu_vk_device* dd, uint32_t id)
{
    const uint32_t i = GPU_HANDLE_INDEX(id);
    if (dd->buf_vk[i]) dd->vk.vkDestroyBuffer(dd->device, dd->buf_vk[i], NULL);
    if (dd->buf[i].memory) dd->vk.vkFreeMemory(dd->device, dd->buf[i].memory, NULL);
    dd->buf_vk[i] = VK_NULL_HANDLE;
    memset(&dd->buf[i], 0, sizeof dd->buf[i]);
    gpu_vk_slot_free(&dd->buf_slots, id);
}

static void free_now(gpu_vk_device* dd, uint32_t kind, uint32_t id)
{
    if (kind == GPU_VK_DEFER_TEXTURE) {
        if (gpu_vk_slot_valid(&dd->tex_slots, id)) gpu_vk_tex_free_now(dd, id);
    } else if (gpu_vk_slot_valid(&dd->buf_slots, id)) {
        gpu_vk_buf_free_now(dd, id);
    }
}

void gpu_vk_defer_collect(gpu_vk_device* dd, int all)
{
    if (!dd->ndefer) return;
    const uint64_t done = completed(dd);
    gpu_vk_lock(&dd->lock);
    uint32_t k = 0;
    for (uint32_t i = 0; i < dd->ndefer; i++) {
        const gpu_vk_defer e = dd->defer[i];
        if (all || e.after <= done) free_now(dd, e.kind, e.id);
        else dd->defer[k++] = e;
    }
    dd->ndefer = k;
    gpu_vk_unlock(&dd->lock);
}

void gpu_vk_free_after(gpu_vk_device* dd, uint32_t kind, uint32_t id, gpu_fence after)
{
    const int now = !after || completed(dd) >= after;
    gpu_vk_lock(&dd->lock);
    if (now) {
        free_now(dd, kind, id);
    } else {
        if (dd->ndefer == dd->defer_cap) {
            const uint32_t cap = dd->defer_cap ? dd->defer_cap * 2 : 256;
            gpu_vk_defer* n = (gpu_vk_defer*)realloc(dd->defer, cap * sizeof *n);
            if (!n) {   /* (no memory: keep it for good rather than free it under the GPU) */
                gpu_vk_unlock(&dd->lock);
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
    gpu_vk_unlock(&dd->lock);
}

/* ---------------------------------------------------------------------------
 * Buffers
 * -------------------------------------------------------------------------*/

static const VkBufferUsageFlags k_buf_usage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

static gpu_buffer buf_add(gpu_vk_device* dd, VkBuffer b, VkDeviceMemory m, uint64_t size, uint32_t kind, void* map,
                          uint32_t coherent, uint32_t padded)
{
    gpu_buffer r = {0};
    gpu_vk_lock(&dd->lock);
    r.id = gpu_vk_slot_alloc(&dd->buf_slots);
    if (r.id) {
        const uint32_t i = GPU_HANDLE_INDEX(r.id);
        dd->buf_vk[i] = b;
        dd->buf[i].memory = m;
        dd->buf[i].size = size;
        dd->buf[i].map = (uint8_t*)map;
        dd->buf[i].memory_kind = kind;
        dd->buf[i].coherent = coherent;
        dd->buf[i].padded = padded;
    }
    gpu_vk_unlock(&dd->lock);
    return r;
}

/* Every usage, so any buffer binds anywhere, as on D3D12. A constant
 * buffer's VkBuffer runs a CB range past its end, so bind_cb may take any
 * address in it (the shader reads up to 64 KB from there). Upload memory is
 * the CPU's (D3D12's upload heap); readback is cached where the device has
 * that. Buffers are below 4 GB (gpu_addr's offset). */
gpu_buffer gpu_vk_buffer_create(gpu_device* d, const gpu_buffer_desc* desc)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_buffer none = {0};
    if (!desc->size || desc->size >= (1ull << 32)) return none;
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = desc->size + ((desc->use & GPU_USE_CONSTANT) ? dd->cb_range : 0u);
    bi.usage = k_buf_usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer b = VK_NULL_HANDLE;
    if (dd->vk.vkCreateBuffer(dd->device, &bi, NULL, &b) != VK_SUCCESS) return none;
    VkMemoryRequirements req;
    dd->vk.vkGetBufferMemoryRequirements(dd->device, b, &req);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags hc = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags hk = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (desc->memory == GPU_MEM_UPLOAD) {
        type = gpu_vk_mem_type(dd, req.memoryTypeBits, hv | hc, dl);
        if (type == UINT32_MAX) type = gpu_vk_mem_type(dd, req.memoryTypeBits, hv | hc, 0);
    } else if (desc->memory == GPU_MEM_READBACK) {
        type = gpu_vk_mem_type(dd, req.memoryTypeBits, hv | hc | hk, 0);
        if (type == UINT32_MAX) type = gpu_vk_mem_type(dd, req.memoryTypeBits, hv | hk, 0);
        if (type == UINT32_MAX) type = gpu_vk_mem_type(dd, req.memoryTypeBits, hv | hc, 0);
    } else {
        type = gpu_vk_mem_type(dd, req.memoryTypeBits, dl, hv);
        if (type == UINT32_MAX) type = gpu_vk_mem_type(dd, req.memoryTypeBits, dl, 0);
        if (type == UINT32_MAX) type = gpu_vk_mem_type(dd, req.memoryTypeBits, 0, 0);
    }
    VkDeviceMemory m = VK_NULL_HANDLE;
    void* map = NULL;
    if (type != UINT32_MAX) {
        VkMemoryAllocateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (dd->vk.vkAllocateMemory(dd->device, &ai, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    }
    if (!m || dd->vk.vkBindBufferMemory(dd->device, b, m, 0) != VK_SUCCESS ||
        ((desc->memory == GPU_MEM_UPLOAD || desc->memory == GPU_MEM_READBACK) &&
         dd->vk.vkMapMemory(dd->device, m, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)) {
        if (m) dd->vk.vkFreeMemory(dd->device, m, NULL);
        dd->vk.vkDestroyBuffer(dd->device, b, NULL);
        return none;
    }
    gpu_vk_name(dd, VK_OBJECT_TYPE_BUFFER, (uint64_t)(uintptr_t)b, desc->name);
    const uint32_t coherent = (dd->mem.memoryTypes[type].propertyFlags & hc) != 0;
    gpu_buffer r = buf_add(dd, b, m, desc->size, desc->memory ? desc->memory : GPU_MEM_GPU, map, coherent,
                           (desc->use & GPU_USE_CONSTANT) != 0);
    if (!r.id) {
        dd->vk.vkDestroyBuffer(dd->device, b, NULL);
        dd->vk.vkFreeMemory(dd->device, m, NULL);
    }
    return r;
}

/* The caller's memory as a buffer (VK_EXT_external_memory_host): address
 * and size on host_import_align. */
gpu_buffer gpu_vk_buffer_import_host(gpu_device* d, void* host, uint64_t size)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_buffer none = {0};
    if (!(dd->base.caps.bits & GPU_CAP_HOST_IMPORT) || !host || !size || size >= (1ull << 32) ||
        ((uintptr_t)host & (dd->host_align - 1)) || (size & (dd->host_align - 1)))
        return none;
    VkMemoryHostPointerPropertiesEXT hp;
    memset(&hp, 0, sizeof hp);
    hp.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    if (dd->vk.vkGetMemoryHostPointerPropertiesEXT(dd->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                   host, &hp) != VK_SUCCESS)
        return none;
    VkExternalMemoryBufferCreateInfo eb;
    memset(&eb, 0, sizeof eb);
    eb.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.pNext = &eb;
    bi.size = size;
    bi.usage = k_buf_usage;
    VkBuffer b = VK_NULL_HANDLE;
    if (dd->vk.vkCreateBuffer(dd->device, &bi, NULL, &b) != VK_SUCCESS) return none;
    VkMemoryRequirements req;
    dd->vk.vkGetBufferMemoryRequirements(dd->device, b, &req);
    const uint32_t bits = req.memoryTypeBits & hp.memoryTypeBits;
    uint32_t type = gpu_vk_mem_type(dd, bits, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (type == UINT32_MAX) type = gpu_vk_mem_type(dd, bits, 0, 0);
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (type != UINT32_MAX && req.size <= size) {
        VkImportMemoryHostPointerInfoEXT im;
        VkMemoryAllocateInfo ai;
        memset(&im, 0, sizeof im);
        memset(&ai, 0, sizeof ai);
        im.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
        im.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        im.pHostPointer = host;
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.pNext = &im;
        ai.allocationSize = size;
        ai.memoryTypeIndex = type;
        if (dd->vk.vkAllocateMemory(dd->device, &ai, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    }
    if (!m || dd->vk.vkBindBufferMemory(dd->device, b, m, 0) != VK_SUCCESS) {
        if (m) dd->vk.vkFreeMemory(dd->device, m, NULL);
        dd->vk.vkDestroyBuffer(dd->device, b, NULL);
        return none;
    }
    const uint32_t coherent = (dd->mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    gpu_buffer r = buf_add(dd, b, m, size, GPU_MEM_HOST, host, coherent, 0);
    if (!r.id) {
        dd->vk.vkDestroyBuffer(dd->device, b, NULL);
        dd->vk.vkFreeMemory(dd->device, m, NULL);
    }
    return r;
}

void gpu_vk_buffer_destroy(gpu_device* d, gpu_buffer b, gpu_fence after)
{
    if (GPU_VALID(b)) gpu_vk_free_after(GPU_VK_DEV(d), GPU_VK_DEFER_BUFFER, b.id, after);
}

static void flush_range(gpu_vk_device* dd, uint32_t i, uint64_t off, uint64_t size, int invalidate)
{
    const uint64_t atom = dd->props.limits.nonCoherentAtomSize ? dd->props.limits.nonCoherentAtomSize : 1u;
    VkMappedMemoryRange mr;
    memset(&mr, 0, sizeof mr);
    mr.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    mr.memory = dd->buf[i].memory;
    mr.offset = off / atom * atom;
    mr.size = VK_WHOLE_SIZE;
    (void)size;
    if (invalidate) dd->vk.vkInvalidateMappedMemoryRanges(dd->device, 1, &mr);
    else dd->vk.vkFlushMappedMemoryRanges(dd->device, 1, &mr);
}

void* gpu_vk_buffer_map(gpu_device* d, gpu_buffer b, uint64_t read_offset, uint64_t read_size)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    if (!gpu_vk_slot_valid(&dd->buf_slots, b.id)) return NULL;
    const uint32_t i = GPU_HANDLE_INDEX(b.id);
    if (dd->buf[i].map && !dd->buf[i].coherent && read_size) flush_range(dd, i, read_offset, read_size, 1);
    return dd->buf[i].map;
}

void gpu_vk_buffer_unmap(gpu_device* d, gpu_buffer b, uint64_t written_offset, uint64_t written_size)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    if (!gpu_vk_slot_valid(&dd->buf_slots, b.id)) return;
    const uint32_t i = GPU_HANDLE_INDEX(b.id);
    if (dd->buf[i].map && !dd->buf[i].coherent && written_size) flush_range(dd, i, written_offset, written_size, 0);
}

/* ---------------------------------------------------------------------------
 * Textures
 * -------------------------------------------------------------------------*/

/* The image a description makes: every usage its format has (sampled and
 * both copies), plus the targets it asks for. A TYPELESS colour format's
 * views may reinterpret it; six square layers may be a cube. 0 when the
 * format cannot do what is asked. */
static int image_info(gpu_vk_device* dd, const gpu_texture_desc* desc, const gpu_vk_format* f, VkImageCreateInfo* ci)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(dd->phys, f->vk, &fp);
    const VkFormatFeatureFlags o = fp.optimalTilingFeatures;
    VkImageUsageFlags u = 0;
    if (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (o & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) u |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (o & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) u |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc->use & GPU_USE_RT) {
        if (!(o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return 0;
        u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (desc->use & GPU_USE_DS) {
        if (!(o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) return 0;
        u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if ((desc->use & GPU_USE_SAMPLED) && !(o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return 0;
    if (!u) return 0;
    memset(ci, 0, sizeof *ci);
    ci->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    if (f->typeless && (f->aspect & GPU_VK_ASPECT_COLOR)) ci->flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (desc->layers >= 6 && desc->layers % 6 == 0 && desc->width == desc->height && desc->samples <= 1)
        ci->flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci->imageType = VK_IMAGE_TYPE_2D;
    ci->format = f->vk;
    ci->extent.width = desc->width ? desc->width : 1u;
    ci->extent.height = desc->height ? desc->height : 1u;
    ci->extent.depth = 1;
    ci->mipLevels = desc->mips ? desc->mips : 1u;
    ci->arrayLayers = desc->layers ? desc->layers : 1u;
    ci->samples = (VkSampleCountFlagBits)(desc->samples > 1 ? desc->samples : 1u);
    ci->tiling = VK_IMAGE_TILING_OPTIMAL;
    ci->usage = u;
    ci->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return 1;
}

gpu_texture gpu_vk_tex_add(gpu_vk_device* dd, VkImage image, VkDeviceMemory memory, const gpu_texture_desc* desc,
                           uint32_t flags, int init_now)
{
    const gpu_vk_format* const f = gpu_vk_format_dev(dd, desc->format, desc->use);
    gpu_texture r = {0};
    if (!f) return r;
    gpu_vk_lock(&dd->lock);
    r.id = gpu_vk_slot_alloc(&dd->tex_slots);
    if (r.id && init_now && dd->ninit == dd->init_cap) {
        const uint32_t cap = dd->init_cap ? dd->init_cap * 2 : 256;
        gpu_vk_init* n = (gpu_vk_init*)realloc(dd->init, cap * sizeof *n);
        if (n) {
            dd->init = n;
            dd->init_cap = cap;
        } else {
            gpu_vk_slot_free(&dd->tex_slots, r.id);
            r.id = 0;
        }
    }
    if (r.id) {
        const uint32_t i = GPU_HANDLE_INDEX(r.id);
        gpu_vk_tex* const t = &dd->tex[i];
        memset(t, 0, sizeof *t);
        t->image = image;
        t->memory = memory;
        t->vkfmt = f->vk;
        t->format = desc->format;
        t->width = desc->width ? desc->width : 1u;
        t->height = desc->height ? desc->height : 1u;
        t->layers = desc->layers ? desc->layers : 1u;
        t->mips = desc->mips ? desc->mips : 1u;
        t->samples = desc->samples > 1 ? desc->samples : 1u;
        t->use = desc->use;
        t->aspect = aspect_flags(f->aspect);
        t->flags = flags;
        t->bpb = f->bpb;
        t->bw = f->bw;
        t->depth = (f->aspect & GPU_VK_ASPECT_DEPTH) != 0;
        t->init_layout = gpu_vk_state_layout(desc->state, t);
        dd->tex_layout[i] = init_now ? (uint32_t)t->init_layout : (uint32_t)VK_IMAGE_LAYOUT_UNDEFINED;
        if (init_now) {
            dd->init[dd->ninit].tex = r.id;
            dd->init[dd->ninit].layout = t->init_layout;
            dd->ninit++;
        }
    }
    gpu_vk_unlock(&dd->lock);
    return r;
}

/* Under qlock: the pending first layouts, in a command buffer of the ring
 * (waited for if the GPU still has it, which it never should). */
VkCommandBuffer gpu_vk_init_record(gpu_vk_device* dd, uint64_t value)
{
    if (!dd->ninit) return VK_NULL_HANDLE;   /* (read again under the lock below) */
    const uint32_t k = dd->init_next;
    if (dd->init_ring[k].value && completed(dd) < dd->init_ring[k].value) {
        VkSemaphoreWaitInfo wi;
        memset(&wi, 0, sizeof wi);
        wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = 1;
        wi.pSemaphores = &dd->timeline;
        wi.pValues = &dd->init_ring[k].value;
        dd->vk.vkWaitSemaphores(dd->device, &wi, 2000000000ull);
    }
    const VkCommandBuffer cb = dd->init_ring[k].cb;
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    gpu_vk_lock(&dd->lock);
    if (!dd->ninit) {
        gpu_vk_unlock(&dd->lock);
        return VK_NULL_HANDLE;
    }
    dd->vk.vkResetCommandPool(dd->device, dd->init_ring[k].pool, 0);
    dd->vk.vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier2 ib[32];
    VkDependencyInfo di;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.pImageMemoryBarriers = ib;
    /* Step VC1: three passes. A texture to zero (GPU_VK_TEX_ZERO) goes to
     * TRANSFER_DST first, is cleared, then goes to its layout; the others go
     * to their layouts in the first pass, as before. */
    for (uint32_t pass = 0; pass < 3; pass++) {
        uint32_t n = 0;
        for (uint32_t j = 0; j <= dd->ninit; j++) {
            if (n == 32 || (j == dd->ninit && n)) {
                di.imageMemoryBarrierCount = n;
                dd->vk.vkCmdPipelineBarrier2(cb, &di);
                n = 0;
            }
            if (j == dd->ninit) break;
            const gpu_vk_init e = dd->init[j];
            if (!gpu_vk_slot_valid(&dd->tex_slots, e.tex)) continue;
            const gpu_vk_tex* const t = &dd->tex[GPU_HANDLE_INDEX(e.tex)];
            const int zero = (t->flags & GPU_VK_TEX_ZERO) != 0;
            if (pass && !zero) continue;
            VkImageSubresourceRange all;
            memset(&all, 0, sizeof all);
            all.aspectMask = t->aspect;
            all.levelCount = VK_REMAINING_MIP_LEVELS;
            all.layerCount = VK_REMAINING_ARRAY_LAYERS;
            if (pass == 1) {   /* every bit 0: 0.0, depth 0.0, stencil 0, as D3D12's zeroed memory */
                if (t->aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
                    VkClearColorValue cv;
                    memset(&cv, 0, sizeof cv);
                    dd->vk.vkCmdClearColorImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &all);
                } else {
                    VkClearDepthStencilValue dv;
                    memset(&dv, 0, sizeof dv);
                    dd->vk.vkCmdClearDepthStencilImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dv, 1,
                                                       &all);
                }
                continue;
            }
            VkImageMemoryBarrier2* const b = &ib[n++];
            memset(b, 0, sizeof *b);
            b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b->srcStageMask = pass ? VK_PIPELINE_STAGE_2_CLEAR_BIT : VK_PIPELINE_STAGE_2_NONE;
            b->srcAccessMask = pass ? VK_ACCESS_2_TRANSFER_WRITE_BIT : 0u;
            b->dstStageMask = zero && !pass ? VK_PIPELINE_STAGE_2_CLEAR_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b->dstAccessMask = zero && !pass ? VK_ACCESS_2_TRANSFER_WRITE_BIT
                                             : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b->oldLayout = pass ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            b->newLayout = zero && !pass ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : e.layout;
            b->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b->image = t->image;
            b->subresourceRange = all;
        }
    }
    dd->ninit = 0;
    gpu_vk_unlock(&dd->lock);
    dd->vk.vkEndCommandBuffer(cb);
    dd->init_ring[k].value = value;
    dd->init_next = (k + 1) % GPU_VK_INIT_RING;
    return cb;
}

/* Memory to place textures in: device memory of the type images take. */
gpu_heap gpu_vk_heap_create(gpu_device* d, const gpu_heap_desc* desc)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_heap r = {0};
    gpu_texture_desc td;
    VkImageCreateInfo ci;
    memset(&td, 0, sizeof td);
    td.width = td.height = 64;
    td.layers = td.mips = 1;
    td.samples = 1;
    td.format = GPU_FORMAT_R8G8B8A8_UNORM;
    td.use = GPU_USE_SAMPLED;
    if (!desc->size || !image_info(dd, &td, gpu_vk_format_of(td.format, 0), &ci)) return r;
    VkDeviceImageMemoryRequirements ir;
    VkMemoryRequirements2 mr;
    memset(&ir, 0, sizeof ir);
    memset(&mr, 0, sizeof mr);
    ir.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS;
    ir.pCreateInfo = &ci;
    mr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    dd->vk.vkGetDeviceImageMemoryRequirements(dd->device, &ir, &mr);
    const uint32_t type = gpu_vk_mem_type(dd, mr.memoryRequirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type == UINT32_MAX) return r;
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = desc->size;
    ai.memoryTypeIndex = type;
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (dd->vk.vkAllocateMemory(dd->device, &ai, NULL, &m) != VK_SUCCESS) return r;
    gpu_vk_lock(&dd->lock);
    r.id = gpu_vk_slot_alloc(&dd->heap_slots);
    if (r.id) {
        gpu_vk_heap* const h = &dd->heap[GPU_HANDLE_INDEX(r.id)];
        h->memory = m;
        h->size = desc->size;
        h->type = type;
    }
    gpu_vk_unlock(&dd->lock);
    if (!r.id) dd->vk.vkFreeMemory(dd->device, m, NULL);
    return r;
}

void gpu_vk_texture_size(gpu_device* d, const gpu_texture_desc* desc, uint64_t* size, uint64_t* align)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    const gpu_vk_format* const f = tex_format(dd, desc->format, desc->use);
    VkImageCreateInfo ci;
    *size = 0;
    *align = 0;
    if (!f || !image_info(dd, desc, f, &ci)) return;
    VkDeviceImageMemoryRequirements ir;
    VkMemoryRequirements2 mr;
    memset(&ir, 0, sizeof ir);
    memset(&mr, 0, sizeof mr);
    ir.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS;
    ir.pCreateInfo = &ci;
    mr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    dd->vk.vkGetDeviceImageMemoryRequirements(dd->device, &ir, &mr);
    *size = mr.memoryRequirements.size;
    *align = mr.memoryRequirements.alignment;
}

/* GetCopyableFootprints' rules: each subresource at a 512-byte offset, rows
 * on 256 bytes, BC in rows of blocks (and sizes rounded up to whole
 * blocks); a depth-stencil format's planes are R32_TYPELESS (depth, 4 bytes)
 * and R8_TYPELESS (stencil), their rows on 512 bytes. The last row is not
 * padded. (test_gpu_vk checks them against D3D12's.) */
uint64_t gpu_vk_texture_footprints(gpu_device* d, const gpu_texture_desc* desc, uint32_t first_sub, uint32_t n,
                                   uint64_t base_offset, gpu_footprint* out, uint32_t* rows, uint64_t* row_bytes)
{
    (void)d;
    const gpu_vk_format* f = gpu_vk_format_of(desc->format, desc->use);
    if (!f) f = gpu_vk_format_of(desc->format, GPU_USE_DS);
    if (!f) return 0;
    const uint32_t mips = desc->mips ? desc->mips : 1u, layers = desc->layers ? desc->layers : 1u;
    const uint32_t per = mips * layers;
    const int planes2 = (f->aspect & GPU_VK_ASPECT_DEPTH) && (f->aspect & GPU_VK_ASPECT_STENCIL);
    uint64_t off = base_offset, end = base_offset;
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t sub = first_sub + k, plane = sub / per, mip = (sub % per) % mips;
        uint32_t bpb = f->bpb, bw = f->bw;
        gpu_format pf = desc->format;
        if (planes2) {
            pf = plane ? (gpu_format)60 /* R8_TYPELESS */ : (gpu_format)GPU_FORMAT_R32_TYPELESS;
            bpb = plane ? 1u : 4u;
        }
        uint32_t w = desc->width >> mip, h = desc->height >> mip;
        if (!w) w = 1;
        if (!h) h = 1;
        w = (w + bw - 1) / bw * bw;
        h = (h + bw - 1) / bw * bw;
        const uint64_t rb = (uint64_t)(w / bw) * bpb;
        const uint64_t pal = planes2 ? 511u : 255u;
        const uint64_t pitch = (rb + pal) & ~pal;
        const uint32_t nrows = h / bw;
        off = (off + 511u) & ~(uint64_t)511u;
        if (out) {
            out[k].offset = off;
            out[k].format = pf;
            out[k].width = w;
            out[k].height = h;
            out[k].depth = 1;
            out[k].row_pitch = (uint32_t)pitch;
        }
        if (rows) rows[k] = nrows;
        if (row_bytes) row_bytes[k] = rb;
        end = off + pitch * (nrows - 1) + rb;
        off += pitch * nrows;
    }
    return end - base_offset;
}

/* A texture with memory of its own (one allocation, dedicated), or placed
 * in a heap at heap_offset. Its first state is a layout: set before the
 * next submit's work for its own memory, at its first use when placed. */
gpu_texture gpu_vk_texture_create(gpu_device* d, const gpu_texture_desc* desc)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    gpu_texture none = {0};
    const gpu_vk_format* const f = tex_format(dd, desc->format, desc->use);
    VkImageCreateInfo ci;
    if (!f || !image_info(dd, desc, f, &ci)) return none;
    VkImage image = VK_NULL_HANDLE;
    if (dd->vk.vkCreateImage(dd->device, &ci, NULL, &image) != VK_SUCCESS) return none;
    VkMemoryRequirements req;
    dd->vk.vkGetImageMemoryRequirements(dd->device, image, &req);
    VkDeviceMemory m = VK_NULL_HANDLE;
    const int placed = GPU_VALID(desc->heap);
    int ok = 0;
    if (placed) {
        if (gpu_vk_slot_valid(&dd->heap_slots, desc->heap.id)) {
            const gpu_vk_heap* const h = &dd->heap[GPU_HANDLE_INDEX(desc->heap.id)];
            ok = ((req.memoryTypeBits >> h->type) & 1u) && !(desc->heap_offset % req.alignment) &&
                 desc->heap_offset + req.size <= h->size &&
                 dd->vk.vkBindImageMemory(dd->device, image, h->memory, desc->heap_offset) == VK_SUCCESS;
        }
    } else {
        uint32_t type = gpu_vk_mem_type(dd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        if (type == UINT32_MAX) type = gpu_vk_mem_type(dd, req.memoryTypeBits, 0, 0);
        if (type != UINT32_MAX) {
            VkMemoryDedicatedAllocateInfo da;
            VkMemoryAllocateInfo ai;
            memset(&da, 0, sizeof da);
            memset(&ai, 0, sizeof ai);
            da.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            da.image = image;
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.pNext = &da;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = type;
            ok = dd->vk.vkAllocateMemory(dd->device, &ai, NULL, &m) == VK_SUCCESS &&
                 dd->vk.vkBindImageMemory(dd->device, image, m, 0) == VK_SUCCESS;
        }
    }
    /* Step VC1: a texture with memory of its own starts zeroed, as D3D12's
     * committed resources do (the core writes targets back to guest memory,
     * parts it never drew too); one that cannot be cleared (BC) keeps what
     * the memory held. A placed one is what its heap held, on both. */
    const uint32_t zero = !placed && f->bw == 1 && (ci.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? GPU_VK_TEX_ZERO : 0u;
    gpu_texture r = ok ? gpu_vk_tex_add(dd, image, m, desc, zero, !placed) : none;
    if (!r.id) {
        dd->vk.vkDestroyImage(dd->device, image, NULL);
        if (m) dd->vk.vkFreeMemory(dd->device, m, NULL);
        return none;
    }
    gpu_vk_name(dd, VK_OBJECT_TYPE_IMAGE, (uint64_t)(uintptr_t)image, desc->name);
    return r;
}

/* A swap chain's image is the swap chain's: not freed here. */
void gpu_vk_texture_destroy(gpu_device* d, gpu_texture t, gpu_fence after)
{
    gpu_vk_device* const dd = GPU_VK_DEV(d);
    if (!gpu_vk_slot_valid(&dd->tex_slots, t.id)) return;
    if (dd->tex[GPU_HANDLE_INDEX(t.id)].flags & GPU_VK_TEX_SWAP) return;
    gpu_vk_free_after(dd, GPU_VK_DEFER_TEXTURE, t.id, after);
}

#endif /* PS3RECOMP_RSX_HAL && PS3RECOMP_RSX_HAL_VULKAN */
