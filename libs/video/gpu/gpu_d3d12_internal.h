/*
 * ps3recomp - the portable renderer's GPU layer: the D3D12 backend's own types
 *
 * What gpu_d3d12*.c share, and what gpu_d3d12_cmd.h's static inline commands
 * index straight into: the device (it starts with gpu_device), its handle
 * tables and the recorder (it starts with gpu_cmd_head).
 *
 * Handle tables are fixed arrays, made once with the device and never moved,
 * so a command reads a handle's native object with one index and no lock:
 * the hot arrays (tex_res, buf_res, buf_va, view_cpu, layout_rs, tpool,
 * qpool) hold what commands need, the rest is beside them. Slots are handed
 * out and freed under the device's lock. A slot's generation goes up when it
 * is freed, and a slot whose generation would come round again is retired,
 * so a handle names one object for the device's life (gpu.h).
 *
 * C only (the D3D12 headers' COM in C). Windows only for now: vkd3d, the
 * same code on Linux, comes later.
 */
#ifndef PS3RECOMP_GPU_D3D12_INTERNAL_H
#define PS3RECOMP_GPU_D3D12_INTERNAL_H

#if defined(PS3RECOMP_RSX_HAL) && defined(_WIN32)

#ifdef __cplusplus
#error "the D3D12 backend's headers are C: the D3D12 headers' C interfaces (lpVtbl)"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <string.h>

#include "gpu.h"
#include "gpu_cmd_ops.h"

/* Table sizes. Each slot costs a few bytes until it is used. */
#define GPU_D3D12_MAX_TEXTURES   (1u << 17)
#define GPU_D3D12_MAX_BUFFERS    (1u << 16)
#define GPU_D3D12_MAX_VIEWS      (1u << 17)
#define GPU_D3D12_MAX_SAMPLERS   (1u << 14)
#define GPU_D3D12_MAX_SHADERS    (1u << 16)
#define GPU_D3D12_MAX_LAYOUTS    256u
#define GPU_D3D12_MAX_QPOOLS     64u
#define GPU_D3D12_MAX_HEAPS      4096u
#define GPU_D3D12_MAX_TPOOLS     64u
#define GPU_D3D12_MAX_SWAPS      8u
#define GPU_D3D12_MAX_CMD_SLOTS  16u
#define GPU_D3D12_MAX_IMAGES     8u      /* a swap chain's buffers */
#define GPU_D3D12_CPU_HEAP_SIZE  1024u   /* RTVs or DSVs in each of the backend's CPU heaps */
#define GPU_D3D12_CPU_HEAPS      64u

/* FNV-1a 64, as today's renderer names its pipelines (fnv_bytes). */
#define GPU_D3D12_FNV_BASIS 14695981039346656037ull
static inline uint64_t gpu_d3d12_fnv(uint64_t h, const void* p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t*)p)[i]; h *= 1099511628211ull; }
    return h;
}

/* One kind of handle's slots. */
typedef struct gpu_d3d12_slots {
    uint32_t  cap;
    uint32_t  used;      /* slots handed out at least once */
    uint32_t  nfree;
    uint32_t* free;      /* freed slots to hand out again */
    uint8_t*  gen;       /* each slot's generation */
    uint8_t*  live;      /* 1 while it holds an object */
} gpu_d3d12_slots;

/* A texture, beside its ID3D12Resource* (tex_res). */
enum { GPU_D3D12_TEX_SWAP = 1u };   /* a swap chain's image: freed by the swap chain */
typedef struct gpu_d3d12_tex {
    uint32_t   width, height;
    uint16_t   layers, mips;
    gpu_format format;
    uint32_t   samples, use;
    uint32_t   views;    /* its first view, index + 1 (a list through gpu_d3d12_view.next) */
    uint32_t   flags;    /* GPU_D3D12_TEX_* */
} gpu_d3d12_tex;

/* A buffer, beside its ID3D12Resource* and GPU address (buf_res, buf_va). */
typedef struct gpu_d3d12_buf {
    uint64_t size;
    uint8_t* map;        /* upload and readback: the CPU's pointer, mapped for the buffer's life */
    uint32_t memory;     /* GPU_MEM_* */
    ID3D12Heap* host_heap;   /* GPU_MEM_HOST: the heap opened over the caller's memory */
} gpu_d3d12_buf;

/* A target view, beside its CPU descriptor (view_cpu). */
typedef struct gpu_d3d12_view {
    gpu_view_desc desc;  /* what it was made from: the cache's key */
    uint32_t      next;  /* the texture's next view, index + 1 */
    uint32_t      kind;  /* GPU_VIEW_RTV or _DSV */
} gpu_d3d12_view;

typedef struct gpu_d3d12_shader {
    void*    code;       /* the bytecode, kept for pipelines */
    uint64_t size;
    uint64_t hash;       /* FNV-1a 64 of the code: part of a pipeline's name */
    uint32_t stage;
} gpu_d3d12_shader;

/* A shader-visible descriptor heap. memo holds what each entry was written
 * with: a gpu_view_desc (TEX pools; swizzle 0 stored as identity) or a
 * sampler id (SAMPLER pools). */
typedef struct gpu_d3d12_tpool {
    ID3D12DescriptorHeap* heap;
    uint64_t              cpu, gpu;   /* the first entry's handles */
    uint32_t              inc, count, kind;
    uint32_t              owned;      /* 0: adopted (gpu_d3d12_adopt_table_pool) */
    void*                 memo;
    gpu_table_pool_desc   desc;       /* its regions (Vulkan's sets; unused here) */
} gpu_d3d12_tpool;

typedef struct gpu_d3d12_qpool {
    ID3D12QueryHeap*  heap;
    D3D12_QUERY_TYPE  type;
    uint32_t          count;
} gpu_d3d12_qpool;

/* The backend's own non-shader-visible heaps for RTVs and DSVs. */
typedef struct gpu_d3d12_cpu_heap {
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    uint32_t              inc, nheaps, next;   /* next: entries taken in the last heap */
    uint64_t              start;               /* the last heap's first entry */
    ID3D12DescriptorHeap* heap[GPU_D3D12_CPU_HEAPS];
    uint32_t              nfree;
    uint64_t*             free;                /* freed entries */
} gpu_d3d12_cpu_heap;

typedef struct gpu_d3d12_swap {
    IDXGISwapChain3* sc;
    void*            window;
    uint32_t         buffers, width, height;
    gpu_format       format;
    uint32_t         dxgi_flags;   /* DXGI_SWAP_CHAIN_FLAG_* it was made with */
    gpu_texture      image[GPU_D3D12_MAX_IMAGES];
} gpu_d3d12_swap;

/* Objects freed once the GPU passes a fence. */
enum { GPU_D3D12_DEFER_BUFFER = 0, GPU_D3D12_DEFER_TEXTURE = 1 };
typedef struct gpu_d3d12_defer {
    uint32_t  kind, id;
    gpu_fence after;
} gpu_d3d12_defer;

/* A recorder: one command list, and the device's hot arrays for a direct
 * index (they never move). */
struct gpu_cmd {
    gpu_cmd_head                  head;
    ID3D12GraphicsCommandList*    cl;
    ID3D12Resource* const*        tex;
    ID3D12Resource* const*        buf;
    const uint64_t*               view;     /* CPU descriptor handles */
    ID3D12RootSignature* const*   layout;
    const struct gpu_d3d12_tpool* tpool;
    const struct gpu_d3d12_qpool* qpool;
    ID3D12CommandAllocator*       alloc;    /* its slot's; NULL for a wrapped list (bridge) */
};

typedef struct gpu_d3d12_device {
    gpu_device           base;
    uint32_t             flags;              /* GPU_DEVICE_* */
    IDXGIFactory4*       factory;
    IDXGIAdapter1*       adapter;
    ID3D12Device*        device;
    ID3D12CommandQueue*  queue;
    ID3D12Fence*         fence;
    volatile LONG64      fence_value;        /* the last value signalled */
    SRWLOCK              signal_lock;        /* keeps signalled values in order */
    volatile LONG        lost;               /* the first removed reason seen (an HRESULT) */
    volatile LONG        fault_done;         /* fault_report printed */
    int                  dred;               /* DRED was enabled (GPU_DEVICE_BREADCRUMBS) */
    uint32_t             srv_inc, samp_inc;

    /* Handle tables. Slots are handed out and freed under lock; the arrays
     * are read without it. */
    SRWLOCK              lock;
    gpu_d3d12_slots      tex_slots, buf_slots, view_slots, samp_slots, shader_slots,
                         layout_slots, qpool_slots, heap_slots, tpool_slots, swap_slots;
    ID3D12Resource**     tex_res;
    gpu_d3d12_tex*       tex;
    ID3D12Resource**     buf_res;
    uint64_t*            buf_va;
    gpu_d3d12_buf*       buf;
    uint64_t*            view_cpu;
    gpu_d3d12_view*      view;
    D3D12_SAMPLER_DESC*  samp;
    uint32_t*            samp_hash;          /* sampler ids by their description's hash */
    uint32_t             samp_hash_cap;
    gpu_d3d12_shader*    shader;
    ID3D12RootSignature** layout_rs;
    uint64_t*            layout_hash;        /* FNV-1a 64 of its serialized bytes */
    gpu_d3d12_qpool      qpool[GPU_D3D12_MAX_QPOOLS];
    ID3D12Heap**         heap;
    gpu_d3d12_tpool      tpool[GPU_D3D12_MAX_TPOOLS];
    gpu_d3d12_swap       swap[GPU_D3D12_MAX_SWAPS];
    gpu_d3d12_cpu_heap   rtv_heap, dsv_heap;
    gpu_d3d12_defer*     defer;              /* under lock */
    uint32_t             ndefer, defer_cap;

    /* Pipelines: kept until the device goes (but GPU_PIPELINE_STORE_ONLY
     * ones, released once stored); the library is today's
     * ID3D12PipelineLibrary, under its own lock. */
    SRWLOCK                lib_lock;
    ID3D12PipelineLibrary* lib;
    void*                  lib_blob;         /* the library reads from it while it lives */
    SRWLOCK                pso_lock;
    ID3D12PipelineState**  pso;
    uint32_t               npso, pso_cap;

    /* Recorders: an allocator and a list per slot. */
    uint32_t             nslots;
    gpu_cmd              slot[GPU_D3D12_MAX_CMD_SLOTS];

    /* Test hooks: descriptors written by the table writes (the memo skips
     * the rest). */
    volatile LONG        n_srv_writes, n_sampler_writes;
} gpu_d3d12_device;

#define GPU_D3D12_DEV(d) ((gpu_d3d12_device*)(void*)(d))
#define GPU_D3D12_IDX(h) GPU_HANDLE_INDEX((h).id)

/* The recorders' command table (gpu_d3d12_cmd.h's commands). */
extern const gpu_cmd_ops gpu_d3d12_cmd_ops;

/* gpu_d3d12.c: what the other files share. */
int         gpu_d3d12_slot_valid(const gpu_d3d12_slots* s, uint32_t id);
uint32_t    gpu_d3d12_slot_alloc(gpu_d3d12_slots* s);          /* under lock; 0 when full */
void        gpu_d3d12_slot_free(gpu_d3d12_slots* s, uint32_t id);   /* under lock */
gpu_texture gpu_d3d12_tex_add(gpu_d3d12_device* dd, ID3D12Resource* res, const gpu_texture_desc* desc,
                              uint32_t flags);                  /* takes the reference */
void        gpu_d3d12_tex_free(gpu_d3d12_device* dd, gpu_texture t);
void        gpu_d3d12_cmd_init(gpu_d3d12_device* dd, gpu_cmd* c, ID3D12GraphicsCommandList* cl,
                               ID3D12CommandAllocator* alloc);
/* The out-of-line halves of the table writes (gpu_d3d12_cmd.h has the memo). */
void        gpu_d3d12_write_srv(gpu_d3d12_device* dd, gpu_d3d12_tpool* p, uint32_t index,
                                const gpu_view_desc* v);
void        gpu_d3d12_write_sampler(gpu_d3d12_device* dd, gpu_d3d12_tpool* p, uint32_t index,
                                    gpu_sampler s);

/* gpu_d3d12_pipe.c */
gpu_pipeline gpu_d3d12_pipeline_create(gpu_device* d, const gpu_pipeline_desc* desc, uint32_t flags, uint32_t* how);
int          gpu_d3d12_pipeline_cache_load(gpu_device* d, const char* path);
int          gpu_d3d12_pipeline_cache_save(gpu_device* d, const char* path);
void         gpu_d3d12_pipe_d3d_desc(gpu_d3d12_device* dd, const gpu_pipeline_desc* desc,
                                     D3D12_GRAPHICS_PIPELINE_STATE_DESC* out,
                                     D3D12_INPUT_ELEMENT_DESC il[GPU_MAX_VERTEX_ATTRS]);
uint64_t     gpu_d3d12_pipe_name(gpu_d3d12_device* dd, const gpu_pipeline_desc* desc);
void         gpu_d3d12_pipe_keep(gpu_d3d12_device* dd, ID3D12PipelineState* pso);
void         gpu_d3d12_pipe_release(gpu_d3d12_device* dd);

/* gpu_d3d12_swap.c */
gpu_swapchain gpu_d3d12_swap_create(gpu_device* d, const gpu_swapchain_desc* desc);
void          gpu_d3d12_swap_destroy(gpu_device* d, gpu_swapchain sc);
int           gpu_d3d12_swap_resize(gpu_device* d, gpu_swapchain sc, uint32_t width, uint32_t height,
                                    gpu_format format);
uint32_t      gpu_d3d12_swap_acquire(gpu_device* d, gpu_swapchain sc, gpu_texture* image);
uint32_t      gpu_d3d12_swap_image_count(gpu_device* d, gpu_swapchain sc);
gpu_texture   gpu_d3d12_swap_image(gpu_device* d, gpu_swapchain sc, uint32_t i);   /* also the bridge's */
int           gpu_d3d12_swap_present(gpu_device* d, gpu_swapchain sc, uint32_t interval, uint32_t flags);
int           gpu_d3d12_swap_stats(gpu_device* d, gpu_swapchain sc, gpu_present_stats* out);
int           gpu_d3d12_swap_hdr(gpu_device* d, gpu_swapchain sc, uint32_t colour_space,
                                 const gpu_hdr_metadata* meta);

#endif /* PS3RECOMP_RSX_HAL && _WIN32 */
#endif /* PS3RECOMP_GPU_D3D12_INTERNAL_H */
