/*
 * ps3recomp - the portable renderer's GPU layer: the Vulkan backend's own types
 *
 * What gpu_vk*.c share, and what gpu_vk_cmd.h's static inline commands index
 * straight into: the device (it starts with gpu_device), its handle tables,
 * the recorder (it starts with gpu_cmd_head) and the rules that map gpu.h's
 * D3D12 numbers onto Vulkan.
 *
 * Handle tables are fixed arrays, made once with the device and never moved,
 * as on D3D12: a command reads a handle's object with one index and no lock,
 * slots are handed out and freed under the device's lock, and a slot whose
 * generation would come round again is retired (gpu.h).
 *
 * Every device call goes through the device's VolkDeviceTable (volk), so a
 * command is one call into the driver with no loader trampoline.
 *
 * Bindings (what the SPIR-V target must produce for a gpu_layout_desc):
 *   - set 0: each CB slot b<n> at binding n (a dynamic uniform buffer, b0-b15),
 *     each RAW slot t<n> at binding 16 + n (a dynamic storage buffer, t0-t31),
 *     each static sampler s<n> at binding 48 + n (an immutable sampler, s0-s15);
 *   - set 1, 2, ...: the TABLE_TEX and TABLE_SAMPLER slots in slot order, one
 *     set each; entry j of a texture table is binding 16 + j (t<j>), of a
 *     sampler table binding 48 + j (s<j>), counted from the slot's first register (step VM: any);
 *   - CONST32 slots: push constants, one block, the slots in slot order, each
 *     at 4 bytes times the counts before it;
 *   - vertex attribute k of gpu_vertex_layout is location k.
 * So the binding numbers are DXC's with -fvk-t-shift 16 and -fvk-s-shift 48
 * (glslang: --shift-texture-binding 16 --shift-ssbo-binding 16
 * --shift-sampler-binding 48), and only the set of each needs moving.
 *
 * Built with PS3RECOMP_RSX_HAL and PS3RECOMP_RSX_HAL_VULKAN: Windows and
 * Linux today. C only (C++ can include it too).
 */
#ifndef PS3RECOMP_GPU_VK_INTERNAL_H
#define PS3RECOMP_GPU_VK_INTERNAL_H

#if defined(PS3RECOMP_RSX_HAL) && defined(PS3RECOMP_RSX_HAL_VULKAN)

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#else
#include <pthread.h>
#endif
#include "volk.h"

#include <string.h>

#include "gpu.h"
#include "gpu_cmd_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Table sizes, as on D3D12. */
#define GPU_VK_MAX_TEXTURES   (1u << 17)
#define GPU_VK_MAX_BUFFERS    (1u << 16)
#define GPU_VK_MAX_VIEWS      (1u << 17)
#define GPU_VK_MAX_SAMPLERS   (1u << 14)
#define GPU_VK_MAX_SHADERS    (1u << 16)
#define GPU_VK_MAX_LAYOUTS    256u
#define GPU_VK_MAX_PIPELINES  (1u << 16)
#define GPU_VK_MAX_QPOOLS     64u
#define GPU_VK_MAX_HEAPS      4096u
#define GPU_VK_MAX_TPOOLS     64u
#define GPU_VK_MAX_SWAPS      8u
#define GPU_VK_MAX_CMD_SLOTS  16u
#define GPU_VK_MAX_IMAGES     8u      /* a swap chain's */
#define GPU_VK_MAX_SETS       (1u + GPU_MAX_SLOTS)
#define GPU_VK_MAX_DYN        GPU_MAX_SLOTS
#define GPU_VK_INIT_RING      8u      /* command buffers for new textures' first layouts */
#define GPU_VK_MARKS          4096u   /* the checkpoint markers' ring (GPU_DEVICE_BREADCRUMBS) */
#define GPU_VK_MARK_LEN       64u

/* The binding numbers above. */
#define GPU_VK_BIND_CB      0u
#define GPU_VK_BIND_T       16u
#define GPU_VK_BIND_S       48u

/* ---- step VC1 ---- */
/* The entries a table's set layout has: tables of up to 16 entries share
 * one 16-entry layout, so a table slot binds a table at least as long as
 * its count (D3D12 lets a slot read the start of a longer table: the blit's
 * one-entry slot binds the HDR blit's two-entry tables); a longer table's
 * layout is its own size. The entries past a table's own are never written
 * and never read (partially bound). */
#define GPU_VK_TABLE_MIN 16u
static inline uint32_t gpu_vk_table_entries(uint32_t size)
{
    return size < GPU_VK_TABLE_MIN ? GPU_VK_TABLE_MIN : size;
}

/* gpu_addr: the buffer's handle in the high 32 bits, the byte offset in the
 * low 32 (buffers are below 4 GB), so an address decodes with no lookup. */
#define GPU_VK_ADDR(id)       ((uint64_t)(id) << 32)
#define GPU_VK_ADDR_ID(a)     ((uint32_t)((a) >> 32))
#define GPU_VK_ADDR_OFF(a)    ((uint32_t)(a))

/* FNV-1a 64 (pipeline cache file checks, sampler and set caches). */
#define GPU_VK_FNV_BASIS 14695981039346656037ull
static inline uint64_t gpu_vk_fnv(uint64_t h, const void* p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t*)p)[i]; h *= 1099511628211ull; }
    return h;
}

/* ---- Locks and atomics ---- */
#ifdef _WIN32
typedef SRWLOCK gpu_vk_mutex;
static inline void gpu_vk_mutex_init(gpu_vk_mutex* m) { InitializeSRWLock(m); }
static inline void gpu_vk_mutex_fini(gpu_vk_mutex* m) { (void)m; }
static inline void gpu_vk_lock(gpu_vk_mutex* m) { AcquireSRWLockExclusive(m); }
static inline void gpu_vk_unlock(gpu_vk_mutex* m) { ReleaseSRWLockExclusive(m); }
static inline uint32_t gpu_vk_cas32(volatile uint32_t* p, uint32_t want, uint32_t was)
{
    return (uint32_t)InterlockedCompareExchange((volatile LONG*)p, (LONG)want, (LONG)was);
}
static inline uint32_t gpu_vk_add32(volatile uint32_t* p) { return (uint32_t)InterlockedIncrement((volatile LONG*)p); }
#else
typedef pthread_mutex_t gpu_vk_mutex;
static inline void gpu_vk_mutex_init(gpu_vk_mutex* m) { pthread_mutex_init(m, NULL); }
static inline void gpu_vk_mutex_fini(gpu_vk_mutex* m) { pthread_mutex_destroy(m); }
static inline void gpu_vk_lock(gpu_vk_mutex* m) { pthread_mutex_lock(m); }
static inline void gpu_vk_unlock(gpu_vk_mutex* m) { pthread_mutex_unlock(m); }
static inline uint32_t gpu_vk_cas32(volatile uint32_t* p, uint32_t want, uint32_t was)
{
    uint32_t e = was;
    __atomic_compare_exchange_n(p, &e, want, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return e;
}
static inline uint32_t gpu_vk_add32(volatile uint32_t* p) { return __atomic_add_fetch(p, 1u, __ATOMIC_SEQ_CST); }
#endif

/* One kind of handle's slots (gpu_d3d12_slots' rules). */
typedef struct gpu_vk_slots {
    uint32_t  cap;
    uint32_t  used;      /* slots handed out at least once */
    uint32_t  nfree;
    uint32_t* free;
    uint8_t*  gen;
    uint8_t*  live;
} gpu_vk_slots;

/* ---- Formats ---- */

/* A DXGI number's Vulkan side. aspect: GPU_VK_ASPECT_*; kind: what a shader
 * reads (0 float or unorm, 1 uint); swz: for each of the DXGI format's
 * channels R, G, B, A, the image's channel that holds it (VK_COMPONENT_SWIZZLE_*
 * values: a format Vulkan stores in another order, or one it lacks). */
enum { GPU_VK_ASPECT_COLOR = 1, GPU_VK_ASPECT_DEPTH = 2, GPU_VK_ASPECT_STENCIL = 4 };
typedef struct gpu_vk_format {
    VkFormat vk;
    uint8_t  bpb;        /* bytes a texel block */
    uint8_t  bw;         /* block width and height: 4 for BC, else 1 */
    uint8_t  aspect;
    uint8_t  kind;
    uint8_t  typeless;   /* views may reinterpret it (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) */
    uint8_t  swz[4];
} gpu_vk_format;
/* The table entry, or NULL for a format Vulkan does not have here. use: the
 * texture's GPU_USE_* (a TYPELESS depth format is a depth image with
 * GPU_USE_DS, a colour one without). */
const gpu_vk_format* gpu_vk_format_of(gpu_format f, uint32_t use);
struct gpu_vk_device;
/* The same on a device: NULL for BC without GPU_CAP_BC; D24S8's numbers as
 * D32S8 without GPU_CAP_D24S8. */
const gpu_vk_format* gpu_vk_format_dev(const struct gpu_vk_device* dd, gpu_format f, uint32_t use);

/* ---- Objects ---- */

enum { GPU_VK_TEX_SWAP = 1u };   /* a swap chain's image */
/* ---- step VC1 ---- */
enum { GPU_VK_TEX_ZERO = 2u };   /* its own memory, zeroed before its first use (gpu_vk_init_record) */
typedef struct gpu_vk_tex {
    VkImage            image;
    VkDeviceMemory     memory;    /* its own; none when placed or a swap chain's */
    VkFormat           vkfmt;
    gpu_format         format;
    uint32_t           width, height;
    uint16_t           layers, mips;
    uint32_t           samples, use;
    VkImageAspectFlags aspect;    /* every aspect it has */
    uint32_t           views;     /* its first view, index + 1 (a list through gpu_vk_view.next) */
    uint32_t           flags;     /* GPU_VK_TEX_* */
    uint32_t           bpb, bw;   /* a texel block's bytes and size, for buffer copies */
    uint32_t           depth;     /* a depth format: read in DEPTH_STENCIL_READ_ONLY_OPTIMAL */
    VkImageLayout      init_layout;   /* its first state's layout: set at its first use when placed */
} gpu_vk_tex;

/* ---- D3D12 states as Vulkan layouts, stages and accesses ----
 *
 * A texture that is neither a colour nor a depth target is GENERAL in every
 * state: D3D12 promotes such a texture from COMMON to any read or copy state
 * without a barrier, and GENERAL serves them all. A target's states have
 * their optimal layouts; COMMON is GENERAL, a swap chain image's PRESENT_SRC. */
static inline VkImageLayout gpu_vk_state_layout(gpu_state s, const gpu_vk_tex* t)
{
    const gpu_state reads = GPU_STATE_NON_PIXEL_SHADER_RESOURCE | GPU_STATE_PIXEL_SHADER_RESOURCE;
    if (s == GPU_STATE_COMMON)
        return (t->flags & GPU_VK_TEX_SWAP) ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL;
    if (!(t->use & (GPU_USE_RT | GPU_USE_DS))) return VK_IMAGE_LAYOUT_GENERAL;
    if (s == GPU_STATE_RENDER_TARGET) return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (s == GPU_STATE_DEPTH_WRITE) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (s & GPU_STATE_DEPTH_READ)
        return (s & ~(GPU_STATE_DEPTH_READ | reads)) ? VK_IMAGE_LAYOUT_GENERAL
                                                     : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    if (!(s & ~reads))
        return t->depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (s == GPU_STATE_COPY_DEST || s == GPU_STATE_RESOLVE_DEST) return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (!(s & ~(gpu_state)(GPU_STATE_COPY_SOURCE | GPU_STATE_RESOLVE_SOURCE))) return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return VK_IMAGE_LAYOUT_GENERAL;
}

/* The layout a table entry reads the texture in: its shader-read state's. */
static inline VkImageLayout gpu_vk_read_layout(const gpu_vk_tex* t)
{
    if (!(t->use & (GPU_USE_RT | GPU_USE_DS))) return VK_IMAGE_LAYOUT_GENERAL;
    return t->depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/* The stages and accesses of a state; src keeps only the writes (a barrier
 * makes writes visible; reads before it need only the stages). COMMON is
 * every stage and access, as D3D12's COMMON is. */
static inline void gpu_vk_state_sync(gpu_state s, int src, VkPipelineStageFlags2* stage, VkAccessFlags2* access)
{
    VkPipelineStageFlags2 st = 0;
    VkAccessFlags2 ac = 0;
    if (s == GPU_STATE_COMMON) {
        st = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ac = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    } else {
        if (s & GPU_STATE_VERTEX_AND_CONSTANT_BUFFER) {
            st |= VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT |
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            ac |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
        }
        if (s & GPU_STATE_INDEX_BUFFER) {
            st |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
            ac |= VK_ACCESS_2_INDEX_READ_BIT;
        }
        if (s & GPU_STATE_RENDER_TARGET) {
            st |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            ac |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        }
        if (s & GPU_STATE_UNORDERED_ACCESS) {
            st |= VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            ac |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        }
        if (s & (GPU_STATE_DEPTH_WRITE | GPU_STATE_DEPTH_READ)) {
            st |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            ac |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            if (s & GPU_STATE_DEPTH_WRITE) ac |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        }
        if (s & GPU_STATE_NON_PIXEL_SHADER_RESOURCE) {
            st |= VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT;
            ac |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        }
        if (s & GPU_STATE_PIXEL_SHADER_RESOURCE) {
            st |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            ac |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        }
        if (s & GPU_STATE_INDIRECT_ARGUMENT) {
            st |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            ac |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        }
        if (s & (GPU_STATE_COPY_DEST | GPU_STATE_COPY_SOURCE | GPU_STATE_RESOLVE_DEST | GPU_STATE_RESOLVE_SOURCE))
            st |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        if (s & (GPU_STATE_COPY_DEST | GPU_STATE_RESOLVE_DEST)) ac |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
        if (s & (GPU_STATE_COPY_SOURCE | GPU_STATE_RESOLVE_SOURCE)) ac |= VK_ACCESS_2_TRANSFER_READ_BIT;
    }
    if (src)
        ac &= VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
              VK_ACCESS_2_TRANSFER_WRITE_BIT;
    *stage = st ? st : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    *access = ac;
}

typedef struct gpu_vk_buf {
    VkDeviceMemory memory;
    uint64_t       size;          /* as asked; the VkBuffer may be longer (bind_cb's range) */
    uint8_t*       map;
    uint32_t       memory_kind;   /* GPU_MEM_* */
    uint32_t       coherent;      /* else buffer_map invalidates and unmap flushes */
    uint32_t       padded;        /* GPU_USE_CONSTANT: a CB range longer than size */
} gpu_vk_buf;

/* A view (a target, or a texture written into a table), kept with its
 * texture until the texture goes. */
typedef struct gpu_vk_view {
    gpu_view_desc      desc;      /* the cache's key */
    VkImageView        iv;
    uint32_t           next;      /* the texture's next view, index + 1 */
    uint32_t           tex;       /* its texture's index */
    uint32_t           width, height;   /* at its mip */
    VkImageAspectFlags aspect;
} gpu_vk_view;

typedef struct gpu_vk_sampler {
    gpu_sampler_desc desc;
    VkSampler        s;
} gpu_vk_sampler;

typedef struct gpu_vk_shader {
    VkShaderModule module;
    uint32_t       stage;
    uint64_t       hash;          /* FNV-1a 64 of the code */
    char           entry[64];
    /* Step VC1: its interface, for pipelines that do not use all of it (D3D12
     * ignores what the other side does not take; Vulkan's validation says
     * so): the Locations its Input and Output variables take (builtins left
     * out), and a pixel shader's code, kept to make a copy without the
     * outputs a pipeline has no target for (gpu_vk_pipe.c). */
    uint32_t       in_mask, out_mask;
    uint32_t*      words;
    uint32_t       nwords;
} gpu_vk_shader;

/* A layout slot's place: a set, and in set 0 the dynamic offset's index (in
 * binding order, as vkCmdBindDescriptorSets takes them), or the push
 * constant bytes. */
typedef struct gpu_vk_slot {
    uint8_t  kind;                /* GPU_SLOT_* */
    uint8_t  set;
    uint8_t  dyn;                 /* CB, RAW: the dynamic offset's index */
    uint8_t  pad;
    uint16_t push_off, push_size; /* CONST32 */
} gpu_vk_slot;

typedef struct gpu_vk_layout {
    VkPipelineLayout      pl;
    VkDescriptorSetLayout set0;        /* CB, RAW and static samplers (empty when none) */
    VkDescriptorSet       set0_fixed;  /* static samplers only: the one set */
    uint32_t              nsets;       /* 1 + the table slots */
    uint32_t              first;       /* the first set bound: 0, or 1 when set 0 is empty */
    uint32_t              ndyn;        /* set 0's dynamic buffers */
    uint32_t              sets_mask;   /* the sets a draw binds */
    uint8_t               dyn_raw[GPU_VK_MAX_DYN];   /* by dynamic index: 1 for RAW, 0 for CB */
    uint32_t              dyn_binding[GPU_VK_MAX_DYN];
    VkShaderStageFlags    push_stages;
    uint32_t              push_size;
    gpu_vk_slot           slot[GPU_MAX_SLOTS];
    uint32_t              index;       /* its slot in the table (the set-0 cache's key) */
    VkSampler             statics[GPU_MAX_STATIC_SAMPLERS];   /* its immutable samplers */
    uint32_t              nstatics;
} gpu_vk_layout;

/* Shader-visible descriptors: one set per table, made with the pool. memo
 * holds what each entry was written with, as on D3D12. */
typedef struct gpu_vk_tpool {
    uint32_t            kind, count;
    gpu_table_pool_desc desc;
    VkDescriptorPool    pool;
    VkDescriptorSet*    sets;          /* every table's, region by region */
    uint32_t            region_base[GPU_MAX_TABLE_REGIONS];
    VkDescriptorSet*    set_at;        /* by entry: the set of the table starting there */
    void*               memo;
} gpu_vk_tpool;

/* A pipeline: gpu_pipeline.v is its index + 1. dyn_dw: depth writes are its
 * dynamic state (it writes depth), set with the bind. */
typedef struct gpu_vk_pso {
    VkPipeline p;
    uint32_t   dyn_dw;
    uint32_t   pad;
} gpu_vk_pso;

typedef struct gpu_vk_qpool {
    VkQueryPool pool;
    uint32_t    type, count;
    uint32_t    precise;               /* VK_QUERY_CONTROL_PRECISE_BIT */
} gpu_vk_qpool;

typedef struct gpu_vk_heap {
    VkDeviceMemory memory;
    uint64_t       size;
    uint32_t       type;               /* its memory type */
} gpu_vk_heap;

typedef struct gpu_vk_swap {
    VkSurfaceKHR     surface;
    VkSwapchainKHR   sc;
    void*            window;
    uint32_t         buffers;          /* asked: the images' least count */
    uint32_t         nimages, width, height;   /* nimages: the driver's (swap_image_count) */
    gpu_format       format;
    VkFormat         vkfmt;
    VkColorSpaceKHR  cs;
    uint32_t         colour_space;     /* GPU_COLOR_* */
    VkPresentModeKHR mode;
    uint32_t         modes_switch;     /* FIFO and IMMEDIATE switch per present (swapchain_maintenance1) */
    uint32_t         has_immediate;
    uint32_t         want_immediate;   /* the last present asked for it; a swap chain made again if needed */
    VkImage          vkimage[GPU_VK_MAX_IMAGES];
    gpu_texture      image[GPU_VK_MAX_IMAGES];
    VkSemaphore      acquire_sem[GPU_VK_MAX_IMAGES + 1];
    VkSemaphore      image_sem[GPU_VK_MAX_IMAGES];    /* the acquire semaphore each image's acquire used */
    VkSemaphore      done_sem[GPU_VK_MAX_IMAGES];     /* rendering done, for the present */
    VkFence          present_fence[GPU_VK_MAX_IMAGES];   /* swapchain_maintenance1: the present let go */
    uint8_t          fence_pending[GPU_VK_MAX_IMAGES];
    uint32_t         recreate;         /* out of date, or the present mode changes without maintenance1 */
    uint32_t         next_sem;
    int32_t          current;          /* the acquired image, -1 when none */
    uint32_t         waited, signalled;
    uint64_t         present_id, done_id;
    int64_t          done_qpc;
    /* Step VC2: the window's surface lost (made again at the next acquire),
     * and given up on (it could not be: GPU_PRESENT_SURFACE_LOST). */
    uint32_t         surface_lost, surface_dead;
    /* Step VC2: intervals 2-4 held by present_wait (present_gate). The
     * display's refresh period and a vblank it was seen at, in now_ticks
     * units (0: not known); when the last present was queued; a wait that
     * timed out (the window is not shown: no gate until a present is seen);
     * the gate's waits and the ticks they took (test hooks). chain_id0: the
     * last present id before this swap chain was made (ids up to it were
     * another's, and no wait may name them). */
    int64_t          period, vblank, queued_at;
    uint64_t         chain_id0;
    uint32_t         gate_miss, gate_n;
    int64_t          gate_ticks;
    /* Step L1: a window with no RGBA8 images (X11's and Wayland's on Mesa
     * and NVIDIA): the swap chain is BGRA8, image[] are RGBA8 textures of
     * the backend's that the core draws into, and the present's own submit
     * blits each into its vkimage[] (a blit swaps the channels' order). */
    uint32_t         shadow;
    VkCommandPool    blit_pool;
    VkCommandBuffer  blit_cb[GPU_VK_MAX_IMAGES];
    VkFence          blit_fence[GPU_VK_MAX_IMAGES];   /* the image's last blit done */
} gpu_vk_swap;

enum { GPU_VK_DEFER_BUFFER = 0, GPU_VK_DEFER_TEXTURE = 1 };
typedef struct gpu_vk_defer {
    uint32_t  kind, id;
    gpu_fence after;
} gpu_vk_defer;

/* A texture's first layout, set before the next submit's work (Vulkan makes
 * images UNDEFINED; D3D12 makes them in the state asked). */
typedef struct gpu_vk_init {
    uint32_t      tex;                 /* handle */
    VkImageLayout layout;
} gpu_vk_init;

/* The set-0 cache: a layout's dynamic buffers to the set that holds them. */
typedef struct gpu_vk_set0_key {
    uint32_t layout;
    uint32_t id[GPU_VK_MAX_DYN];       /* buffer handles */
    uint32_t raw_off[GPU_VK_MAX_DYN];  /* RAW: the range starts here (it runs to the buffer's end) */
} gpu_vk_set0_key;
typedef struct gpu_vk_set0_entry {
    gpu_vk_set0_key key;
    VkDescriptorSet set;
    uint32_t        used;
} gpu_vk_set0_entry;

/* A recorder: one command buffer, the device's arrays, and what is bound. */
struct gpu_cmd {
    gpu_cmd_head                 head;
    VkCommandBuffer              cb;
    const struct VolkDeviceTable* vk;
    struct gpu_vk_device*        dd;
    gpu_vk_tex*                  tex;
    uint32_t*                    tex_layout;   /* each texture's layout as recorded so far */
    const VkBuffer*              buf;
    const gpu_vk_buf*            bufi;
    const gpu_vk_view*           view;
    const gpu_vk_layout*         layouts;
    const gpu_vk_qpool*          qpool;
    const gpu_vk_pso*            pso;
    VkCommandPool                pool;

    /* Bound. */
    const gpu_vk_layout*         lay;
    VkDescriptorSet              sets[GPU_VK_MAX_SETS];
    uint32_t                     have;          /* sets[] bits that hold a set */
    uint32_t                     dyn[GPU_VK_MAX_DYN];
    gpu_vk_set0_key              key;           /* set 0's buffers */
    uint32_t                     set0_stale;    /* key changed since sets[0] was found */
    uint32_t                     dirty;         /* bind before the next draw */

    /* The pass (targets) and its rendering. */
    gpu_pass                     pass;
    uint32_t                     rendering;
    uint32_t                     npasses;
    VkRect2D                     area;
    uint32_t                     ro_depth;      /* the bound depth view is read-only */
    uint32_t                     dw;            /* set_depth_write's */
    uint32_t                     pipe_dw;       /* the bound pipeline's depth writes are dynamic */
    uint32_t                     restart;
};

typedef struct gpu_vk_device {
    gpu_device                 base;
    uint32_t                   flags;           /* GPU_DEVICE_* */
    VkInstance                 instance;
    VkDebugUtilsMessengerEXT   messenger;
    VkPhysicalDevice           phys;
    VkDevice                   device;
    struct VolkDeviceTable     vk;
    VkQueue                    queue;
    uint32_t                   qfamily;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    uint8_t                    luid[8];
    uint32_t                   luid_valid;
    char                       driver[64];

    /* What was enabled (after RSX_VK_MASK). */
    uint32_t has_gs, has_null_desc, has_robust2, has_host_import, has_present_id, has_present_wait,
             has_swap_maint1, has_fault, has_checkpoints, has_hdr_meta, has_unused_att, has_debug_utils,
             has_colorspace, has_surface_caps2, has_surface_maint1, has_mirror, has_aniso, has_precise,
             has_depth_clamp, has_bc, has_d24s8, has_independent_blend, has_maint1_khr, has_robust2_khr;
    uint32_t                   validation;      /* the validation layer is on (RSX_VK_VALIDATION=1) */
    uint32_t                   has_surface, has_host_reset, has_dual_src;
    VkCommandPool              oneshot_pool;    /* gpu_vk_oneshot_*, under qlock */
    uint32_t                   cb_range;        /* a CB slot's bytes (64 KB, or the device's limit) */
    uint64_t                   host_align;      /* minImportedHostPointerAlignment */

    /* The timeline: one semaphore for every fence. */
    VkSemaphore                timeline;
    uint64_t                   timeline_value;  /* the last value signalled, under qlock */
    gpu_vk_mutex               qlock;           /* the queue: submits, signals, presents */
    volatile uint32_t          lost;            /* the first VkResult that said so */
    volatile uint32_t          fault_done;

    /* Handle tables. */
    gpu_vk_mutex               lock;
    gpu_vk_slots               tex_slots, buf_slots, view_slots, samp_slots, shader_slots,
                               layout_slots, qpool_slots, heap_slots, tpool_slots, swap_slots;
    gpu_vk_tex*                tex;
    uint32_t*                  tex_layout;
    VkBuffer*                  buf_vk;
    gpu_vk_buf*                buf;
    gpu_vk_view*               view;
    gpu_vk_sampler*            samp;
    uint32_t*                  samp_hash;
    uint32_t                   samp_hash_cap;
    gpu_vk_shader*             shader;
    gpu_vk_layout*             layout;
    gpu_vk_qpool               qpool[GPU_VK_MAX_QPOOLS];
    gpu_vk_heap*               heap;
    gpu_vk_tpool               tpool[GPU_VK_MAX_TPOOLS];
    gpu_vk_swap                swap[GPU_VK_MAX_SWAPS];
    gpu_vk_defer*              defer;
    uint32_t                   ndefer, defer_cap;
    gpu_vk_init*               init;            /* under lock */
    uint32_t                   ninit, init_cap;
    struct { VkCommandPool pool; VkCommandBuffer cb; uint64_t value; } init_ring[GPU_VK_INIT_RING];
    uint32_t                   init_next;

    /* Descriptor set layouts for tables, by kind and size; set-0 sets. */
    struct { uint32_t kind, size; VkDescriptorSetLayout l; } tlayout[64];
    uint32_t                   ntlayout;
    VkDescriptorPool*          set0_pools;
    uint32_t                   nset0_pools, set0_left;
    gpu_vk_set0_entry*         set0_map;
    uint32_t                   set0_cap, set0_n;

    /* Empty table entries without null descriptors: a 1x1 transparent black
     * texture per view dimension and kind (float, uint). */
    gpu_texture                dummy[4][2];
    uint32_t                   ntlayout_max;

    /* Pipelines: a fixed table, kept until the device goes. */
    gpu_vk_mutex               pso_lock;
    VkPipelineCache            pcache;
    VkPipelineCache            old_caches[8];
    uint32_t                   nold_caches;
    gpu_vk_pso*                pso;
    uint32_t                   npso;

    /* Recorders. */
    uint32_t                   nslots;
    gpu_cmd                    slot[GPU_VK_MAX_CMD_SLOTS];

    /* Checkpoint markers (GPU_DEVICE_BREADCRUMBS on NVIDIA): their text lives here. */
    char*                      marks;
    volatile uint32_t          mark_next;

    /* Test hooks: descriptors the table writes wrote, validation messages. */
    volatile uint32_t          n_srv_writes, n_sampler_writes, n_validation;

    /* Step VC2: lost by hand (gpu_vk_lose_by_hand), not by the driver. The
     * pipelines made, those the pipeline cache had, and the driver's time
     * for them (creation feedback, ns), under pso_lock: PS3_PSO_LOG=1 prints
     * them when the device goes. */
    volatile uint32_t          lost_by_hand;
    uint32_t                   pso_made, pso_hits;
    uint64_t                   pso_ns;
} gpu_vk_device;

#define GPU_VK_DEV(d) ((gpu_vk_device*)(void*)(d))

/* Every device's validation messages, those at a device's end included (a test hook). */
extern volatile uint32_t gpu_vk_n_validation_all;

/* The recorders' command table (gpu_vk_cmd.h's commands). */
extern const gpu_cmd_ops gpu_vk_cmd_ops;

/* ---- gpu_vk.c ---- */
int         gpu_vk_slot_valid(const gpu_vk_slots* s, uint32_t id);
uint32_t    gpu_vk_slot_alloc(gpu_vk_slots* s);                 /* under lock; 0 when full */
void        gpu_vk_slot_free(gpu_vk_slots* s, uint32_t id);    /* under lock */
uint32_t    gpu_vk_lost_note(gpu_vk_device* dd, VkResult r);    /* keeps the first loss; returns r */
uint32_t    gpu_vk_mem_type(const gpu_vk_device* dd, uint32_t bits, VkMemoryPropertyFlags want,
                            VkMemoryPropertyFlags avoid);      /* UINT32_MAX when none */
/* Recording helpers the inline commands call when there is more to do. */
void        gpu_vk_pass_open(gpu_cmd* c);
void        gpu_vk_binds_slow(gpu_cmd* c);
void        gpu_vk_clear_unbound(gpu_cmd* c, gpu_view v, const VkClearValue* value, VkImageAspectFlags aspects,
                                 const gpu_rect* rect);
void        gpu_vk_rec_copy_texture(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, uint32_t x, uint32_t y,
                                    uint32_t z, gpu_texture src, uint32_t src_sub, const gpu_box* box);
void        gpu_vk_rec_copy_whole(gpu_cmd* c, gpu_texture dst, gpu_texture src);
void        gpu_vk_rec_buf_tex(gpu_cmd* c, int to_tex, gpu_texture t, uint32_t sub, uint32_t x, uint32_t y,
                               uint32_t z, gpu_buffer b, const gpu_footprint* fp, const gpu_box* box);
void        gpu_vk_rec_resolve(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, gpu_texture src, uint32_t src_sub);
void        gpu_vk_rec_marker(gpu_cmd* c, const char* text, uint32_t len);
void        gpu_vk_rec_barriers(gpu_cmd* c, uint32_t n, const gpu_barrier* list);
/* A placed texture's first use: UNDEFINED to its first state's layout. */
void        gpu_vk_tex_touch(gpu_cmd* c, uint32_t index);
/* A command buffer run at once and waited for (the empty entries' textures); NULL / 0 on failure. */
VkCommandBuffer gpu_vk_oneshot_begin(gpu_vk_device* dd);
int         gpu_vk_oneshot_end(gpu_vk_device* dd, VkCommandBuffer cb);
/* Names an object for debuggers (with the debug utils). */
void        gpu_vk_name(gpu_vk_device* dd, VkObjectType type, uint64_t handle, const char* name);

/* ---- gpu_vk_mem.c ---- */
gpu_buffer  gpu_vk_buffer_create(gpu_device* d, const gpu_buffer_desc* desc);
gpu_buffer  gpu_vk_buffer_import_host(gpu_device* d, void* host, uint64_t size);
void        gpu_vk_buffer_destroy(gpu_device* d, gpu_buffer b, gpu_fence after);
void*       gpu_vk_buffer_map(gpu_device* d, gpu_buffer b, uint64_t read_offset, uint64_t read_size);
void        gpu_vk_buffer_unmap(gpu_device* d, gpu_buffer b, uint64_t written_offset, uint64_t written_size);
gpu_heap    gpu_vk_heap_create(gpu_device* d, const gpu_heap_desc* desc);
void        gpu_vk_texture_size(gpu_device* d, const gpu_texture_desc* desc, uint64_t* size, uint64_t* align);
uint64_t    gpu_vk_texture_footprints(gpu_device* d, const gpu_texture_desc* desc, uint32_t first_sub, uint32_t n,
                                      uint64_t base_offset, gpu_footprint* out, uint32_t* rows, uint64_t* row_bytes);
gpu_texture gpu_vk_texture_create(gpu_device* d, const gpu_texture_desc* desc);
void        gpu_vk_texture_destroy(gpu_device* d, gpu_texture t, gpu_fence after);
/* A texture's slot. init_now: its first layout is set by the next submit's
 * first command buffer (a texture with memory of its own); else it stays
 * UNDEFINED until its first use (placed: its memory may still be another's)
 * or its first barrier (a swap chain's). */
gpu_texture gpu_vk_tex_add(gpu_vk_device* dd, VkImage image, VkDeviceMemory memory, const gpu_texture_desc* desc,
                           uint32_t flags, int init_now);
void        gpu_vk_tex_free_now(gpu_vk_device* dd, uint32_t id);   /* under lock */
void        gpu_vk_buf_free_now(gpu_vk_device* dd, uint32_t id);   /* under lock */
void        gpu_vk_defer_collect(gpu_vk_device* dd, int all);
void        gpu_vk_free_after(gpu_vk_device* dd, uint32_t kind, uint32_t id, gpu_fence after);
/* Records the pending first layouts into a command buffer of the ring; NULL when none. Under qlock. */
VkCommandBuffer gpu_vk_init_record(gpu_vk_device* dd, uint64_t value);

/* ---- gpu_vk_desc.c ---- */
gpu_view       gpu_vk_view_get(gpu_device* d, const gpu_view_desc* desc);
uint32_t       gpu_vk_view_find(gpu_vk_device* dd, const gpu_view_desc* desc, uint32_t kind);  /* under lock: index + 1 */
gpu_sampler    gpu_vk_sampler_get(gpu_device* d, const gpu_sampler_desc* desc);
VkSampler      gpu_vk_sampler_make(gpu_vk_device* dd, const gpu_sampler_desc* desc, VkBorderColor border);
gpu_table_pool gpu_vk_table_pool_create(gpu_device* d, const gpu_table_pool_desc* desc);
VkDescriptorSetLayout gpu_vk_table_layout(gpu_vk_device* dd, uint32_t kind, uint32_t size);   /* under lock */
void           gpu_vk_write_tex(gpu_vk_device* dd, gpu_vk_tpool* p, uint32_t index, const gpu_view_desc* v);
void           gpu_vk_write_sampler(gpu_vk_device* dd, gpu_vk_tpool* p, uint32_t index, gpu_sampler s);
VkDescriptorSet gpu_vk_set0_get(gpu_vk_device* dd, const gpu_vk_layout* l, const gpu_vk_set0_key* key);
VkDescriptorSet gpu_vk_set0_alloc(gpu_vk_device* dd, VkDescriptorSetLayout l);   /* under lock */
int            gpu_vk_desc_init(gpu_vk_device* dd);       /* the empty entries' textures, without null descriptors */
void           gpu_vk_desc_release(gpu_vk_device* dd);

/* ---- gpu_vk_pipe.c ---- */
gpu_shader   gpu_vk_shader_create(gpu_device* d, const gpu_shader_desc* desc);
void         gpu_vk_shader_destroy(gpu_device* d, gpu_shader s);
gpu_layout   gpu_vk_layout_create(gpu_device* d, const gpu_layout_desc* desc);
gpu_pipeline gpu_vk_pipeline_create(gpu_device* d, const gpu_pipeline_desc* desc, uint32_t flags, uint32_t* how);
int          gpu_vk_pipeline_cache_load(gpu_device* d, const char* path);
int          gpu_vk_pipeline_cache_save(gpu_device* d, const char* path);
void         gpu_vk_pipe_release(gpu_vk_device* dd);

/* ---- gpu_vk_swap.c ---- */
gpu_swapchain gpu_vk_swap_create(gpu_device* d, const gpu_swapchain_desc* desc);
void          gpu_vk_swap_destroy(gpu_device* d, gpu_swapchain sc);
int           gpu_vk_swap_resize(gpu_device* d, gpu_swapchain sc, uint32_t width, uint32_t height, gpu_format format);
uint32_t      gpu_vk_swap_acquire(gpu_device* d, gpu_swapchain sc, gpu_texture* image);
uint32_t      gpu_vk_swap_image_count(gpu_device* d, gpu_swapchain sc);
gpu_texture   gpu_vk_swap_image(gpu_device* d, gpu_swapchain sc, uint32_t index);
int           gpu_vk_swap_present(gpu_device* d, gpu_swapchain sc, uint32_t interval, uint32_t flags);
int           gpu_vk_swap_stats(gpu_device* d, gpu_swapchain sc, gpu_present_stats* out);
int           gpu_vk_swap_hdr(gpu_device* d, gpu_swapchain sc, uint32_t colour_space, const gpu_hdr_metadata* meta);
/* Under qlock, in a submit: the waits and signals its swap chain options ask. */
void          gpu_vk_swap_submit_sems(gpu_vk_device* dd, const gpu_submit_desc* how, VkSemaphoreSubmitInfo* wait,
                                      uint32_t* nwait, VkSemaphoreSubmitInfo* sig, uint32_t* nsig);
/* The instance extensions a surface needs here (NULL-terminated). */
const char* const* gpu_vk_surface_extensions(void);

/* ---- step VC2 ---- */
/* A present's VkResult as gpu.h's GPU_PRESENT_* (out of date and suboptimal
 * are GPU_PRESENTED: the swap chain is made again). */
int           gpu_vk_present_code(VkResult r);
/* The device noted lost as a driver reset leaves it, without the driver
 * saying so (RSX_LOSE_DEVICE_AT_FRAME, the tests): the fault report then
 * asks the driver nothing (device_fault is for a device the driver lost). */
void          gpu_vk_lose_by_hand(gpu_vk_device* dd);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_HAL && PS3RECOMP_RSX_HAL_VULKAN */
#endif /* PS3RECOMP_GPU_VK_INTERNAL_H */
