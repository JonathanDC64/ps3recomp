/*
 * ps3recomp - the portable renderer's GPU layer: the null backend's own types
 *
 * What gpu_null.c keeps, and the out-of-line commands gpu_null_cmd.h's
 * static inline ones call. The "API" under the null backend is its recorder:
 * a command is built into a record of tagged arguments (gpu_null.h has the
 * format), checked against the recorder's bindings when it is recorded, and
 * run at submit, in submission order, against the tracked state of every
 * texture and buffer (the GPU is instant). The records are hashed and dumped
 * when they are run.
 *
 * Handle tables grow as objects are made, from the render thread. Shaders and
 * pipelines, which pipeline workers make beside it (gpu.h), are fixed arrays
 * under the device's lock, so a set_pipeline reads one without it.
 *
 * Plain C99 with no system or GPU headers: Windows, Linux and macOS.
 */
#ifndef PS3RECOMP_GPU_NULL_INTERNAL_H
#define PS3RECOMP_GPU_NULL_INTERNAL_H

#if defined(PS3RECOMP_RSX_HAL)

#include "gpu.h"
#include "gpu_cmd_ops.h"
#include "gpu_null.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Table sizes. gpu_addr holds a buffer's slot in 16 bits and its offset in 40. */
#define GPU_NULL_MAX_BUFFERS   0xFFFFu
#define GPU_NULL_MAX_SHADERS   (1u << 16)
#define GPU_NULL_MAX_PIPELINES (1u << 16)
#define GPU_NULL_MAX_QPOOLS    64u
#define GPU_NULL_MAX_TPOOLS    64u
#define GPU_NULL_MAX_SWAPS     8u
#define GPU_NULL_MAX_IMAGES    8u
#define GPU_NULL_MAX_CMD_SLOTS 16u
#define GPU_NULL_ADDR_BITS     40
#define GPU_NULL_HOST_ALIGN    65536u    /* gpu_buffer_import_host's, as on D3D12 */

/* A tracked state with this bit was promoted from COMMON to a read state by
 * a use (D3D12's implicit promotion), and decays to COMMON at the end of the
 * submit. */
#define GPU_NULL_PROMOTED 0x80000000u

/* FNV-1a 64, as the D3D12 backend names its pipelines. */
#define GPU_NULL_FNV_BASIS 14695981039346656037ull
static inline uint64_t gpu_null_fnv(uint64_t h, const void* p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t*)p)[i]; h *= 1099511628211ull; }
    return h;
}

/* One argument of a record. A record starts with a head (tag 0): v its op,
 * raw its argument count. */
typedef struct gpu_null_arg {
    uint8_t  tag;    /* GPU_NULL_T_*, or 0 for a head */
    uint8_t  pad[3];
    uint32_t raw;    /* a handle's id (a pipeline's index + 1), for running it; not hashed */
    uint64_t v;      /* what is hashed */
} gpu_null_arg;

typedef struct gpu_null_vec {
    gpu_null_arg* a;
    uint32_t      n, cap;
} gpu_null_vec;

/* The device calls' ops, after the commands' (GPU_CMD_OP_*). The first ones
 * are in the stream; the rest only name a call in a check's message. */
#define GPU_NULL_DEV_CALLS(X) \
    X(buffer_create) X(buffer_import_host) X(buffer_destroy) X(heap_create) X(texture_create) \
    X(texture_destroy) X(view_create) X(sampler_create) X(table_pool_create) X(table_write_tex) \
    X(table_write_sampler) X(layout_create) X(query_pool_create) X(swap_create) X(swap_destroy) \
    X(swap_resize) X(swap_acquire) X(swap_present) X(submit) X(cmd) X(signal) X(wait_idle) \
    X(buffer_map) X(buffer_unmap) X(fence_wait) X(cmd_begin) X(cmd_end) X(table_ref) \
    X(shader_create) X(pipeline_create) X(view_get) X(buffer_addr) X(destroy)
#define GPU__NULL_DEV_OP(name) GPU_NULL_OP_##name,
enum { GPU_NULL_OP_DEV_FIRST = 128 - 1, GPU_NULL_DEV_CALLS(GPU__NULL_DEV_OP) GPU_NULL_OP_END };
#undef GPU__NULL_DEV_OP

/* One kind of handle's slots, as the D3D12 backend keeps them: a slot's
 * generation goes up when it is freed, and one at 255 is retired. made counts
 * the objects made, for their numbers in the stream. */
typedef struct gpu_null_slots {
    uint32_t  cap, max;    /* cap grows up to max */
    uint32_t  used, nfree;
    uint32_t* free;
    uint8_t*  gen;
    uint8_t*  live;
    uint32_t  made;
} gpu_null_slots;

typedef struct gpu_null_tex {
    uint32_t   order;      /* its number in the stream */
    uint32_t   width, height;
    uint16_t   layers, mips;
    gpu_format format;
    uint32_t   samples, use;
    uint32_t   planes;     /* 2 for depth-stencil formats: the stencil plane's subresources follow the depth's */
    uint32_t   nsub;       /* mips * layers * planes */
    uint32_t*  state;      /* each subresource's tracked state */
    uint32_t   views;      /* its first view, index + 1 (a list through gpu_null_view.next) */
    uint32_t   swap;       /* a swap chain's image: its swap chain's slot + 1 */
    uint32_t   image;      /* ... and its index there */
    uint64_t   last_use;   /* the fence that ends the last work using it */
} gpu_null_tex;

typedef struct gpu_null_buf {
    uint32_t  order;
    uint32_t  use, memory;
    uint64_t  size;
    uint8_t*  mem;         /* its bytes; the caller's for GPU_MEM_HOST */
    gpu_state state;       /* tracked for GPU memory; upload ones stay GENERIC_READ, readback and host COPY_DEST */
    uint64_t  last_use, last_write;
} gpu_null_buf;

typedef struct gpu_null_view {
    uint32_t      order;
    gpu_view_desc desc;
    uint32_t      next;    /* the texture's next view, index + 1 */
} gpu_null_view;

typedef struct gpu_null_samp {
    uint32_t         order;
    gpu_sampler_desc desc;
} gpu_null_samp;

typedef struct gpu_null_shader {
    uint64_t hash;         /* FNV-1a 64 of the code: part of a pipeline's name */
    uint32_t stage, code;
} gpu_null_shader;

typedef struct gpu_null_layout {
    uint32_t        order;
    uint64_t        hash;  /* of its slots and static samplers */
    gpu_layout_desc desc;
} gpu_null_layout;

/* A pipeline: what a draw checks. gpu_pipeline.v is its index + 1. */
typedef struct gpu_null_pipe {
    uint64_t name;         /* its contents' FNV-1a 64 (gpu_null.h) */
    uint64_t layout_hash;
    uint32_t topology_type, depth_write, vertex_count;
} gpu_null_pipe;

typedef struct gpu_null_qpool {
    uint32_t order, type, count;
} gpu_null_qpool;

typedef struct gpu_null_heap {
    uint32_t order, flags;
    uint64_t size;
} gpu_null_heap;

/* A table pool. memo holds what each entry was written with (as the D3D12
 * backend's: a gpu_view_desc, swizzle 0 stored as the identity, or a sampler
 * id); last_fence the fence ending the last work that read it, pending the
 * recorders (a bit each) holding unsubmitted draws that read it. */
typedef struct gpu_null_tpool {
    uint32_t            order, kind, count;
    gpu_table_pool_desc desc;
    void*               memo;
    uint64_t*           last_fence;
    uint16_t*           pending;
} gpu_null_tpool;

typedef struct gpu_null_swap {
    uint32_t    order;
    uint32_t    width, height, buffers;
    gpu_format  format;
    uint32_t    flags;       /* GPU_SWAP_* */
    gpu_texture image[GPU_NULL_MAX_IMAGES];
    uint32_t    next;        /* the image the next acquire hands out */
    uint32_t    acquired;    /* an image is acquired and not yet presented */
    uint32_t    waited;      /* ... and a submit has waited for it (GPU_SUBMIT_ACQUIRE_WAIT) */
    uint32_t    used;        /* ... and submitted work has used it */
    uint32_t    signalled;   /* ... and a submit since its last use lets the present wait (PRESENT_SIGNAL) */
    uint32_t    presents;
} gpu_null_swap;

/* Objects freed once the timeline reaches a fence. */
enum { GPU_NULL_DEFER_BUFFER = 0, GPU_NULL_DEFER_TEXTURE = 1 };
typedef struct gpu_null_defer {
    uint32_t  kind, id;
    gpu_fence after;
} gpu_null_defer;

/* What a recording has bound, for the checks made as it records. */
typedef struct gpu_null_bind {
    uint32_t layout;               /* the layout's id, or 0 */
    uint32_t pipeline;             /* the pipeline's index + 1, or 0 */
    uint32_t topology;             /* 0: not set */
    uint32_t viewport, scissor, vb, ib, pass;   /* set */
    uint32_t depth_write;          /* 2: not set */
    uint32_t bound;                /* a bit per slot bound since set_layout */
    uint32_t tex_pool, samp_pool;  /* set_table_pools' ids */
    uint64_t table[GPU_MAX_SLOTS]; /* bind_table's values */
} gpu_null_bind;

/* A table read by a recording's draws: entries first .. first + count - 1. */
typedef struct gpu_null_tuse {
    uint32_t pool, first, count;
} gpu_null_tuse;

/* Query states in one recording. */
enum { GPU_NULL_Q_RESET = 1u, GPU_NULL_Q_ACTIVE = 2u, GPU_NULL_Q_ENDED = 4u };

enum { GPU_NULL_REC_IDLE = 0, GPU_NULL_REC_OPEN = 1, GPU_NULL_REC_ENDED = 2, GPU_NULL_REC_SUBMITTED = 3 };

struct gpu_null_device;

/* A recorder: its records since cmd_begin, what they bound, their queries. */
struct gpu_cmd {
    gpu_cmd_head            head;
    struct gpu_null_device* nd;
    uint32_t                slot, state;
    gpu_null_vec            rec;
    gpu_null_bind           b;
    uint8_t*                q[GPU_NULL_MAX_QPOOLS];   /* each query's GPU_NULL_Q_* bits, per pool */
    uint32_t                qcount[GPU_NULL_MAX_QPOOLS];
    uint32_t                qactive;                  /* queries begun and not ended */
    gpu_null_tuse*          tuse;
    uint32_t                ntuse, tuse_cap;
    uint64_t                fence;                    /* the fence ending its last submitted work */
};

typedef struct gpu_null_device {
    gpu_device      base;
    uint32_t        lock;          /* shaders and pipelines (a spin lock) */
    uint64_t        fence;         /* the last value signalled: all of it is done */
    uint64_t        seen;          /* the highest fence the caller has seen pass */
    uint32_t        lost;          /* gpu_null_test_lose's reason */

    gpu_null_slots  tex_slots, buf_slots, view_slots, samp_slots, shader_slots, layout_slots,
                    qpool_slots, heap_slots, tpool_slots, swap_slots;
    gpu_null_tex*   tex;
    gpu_null_buf*   buf;
    gpu_null_view*  view;
    gpu_null_samp*  samp;
    uint32_t*       samp_hash;     /* sampler ids by their description's hash */
    uint32_t        samp_hash_cap;
    gpu_null_shader* shader;       /* fixed: GPU_NULL_MAX_SHADERS */
    gpu_null_layout* layout;
    gpu_null_pipe*  pipe;          /* fixed: GPU_NULL_MAX_PIPELINES */
    uint32_t        npipe;
    uint32_t        stored_only;   /* GPU_PIPELINE_STORE_ONLY creates (under the lock): they keep no entry */
    uint32_t*       pipe_hash;     /* pipeline indexes + 1 by name, 2 * GPU_NULL_MAX_PIPELINES */
    gpu_null_heap*  heap;
    gpu_null_qpool  qpool[GPU_NULL_MAX_QPOOLS];
    gpu_null_tpool  tpool[GPU_NULL_MAX_TPOOLS];
    gpu_null_swap   swap[GPU_NULL_MAX_SWAPS];
    gpu_null_defer* defer;
    uint32_t        ndefer, defer_cap;
    /* What decays to COMMON at the end of a submit: textures with a promoted
     * subresource, and GPU-memory buffers (D3D12's buffers always decay). */
    uint32_t*       decay_tex;
    uint32_t        ndecay_tex, decay_tex_cap;
    uint32_t*       decay_buf;
    uint32_t        ndecay_buf, decay_buf_cap;

    uint32_t        nslots;
    gpu_cmd         slot[GPU_NULL_MAX_CMD_SLOTS];

    /* The stream: device calls since the last flush, the hash so far, the
     * dump. scratch holds the records of calls outside the stream. */
    gpu_null_vec    ev, scratch;
    uint64_t        hash;
    void*           dump;          /* a FILE* */
    gpu_null_counts counts;

    uint32_t        err[GPU_NULL_ERR_COUNT];
    uint32_t        shown[GPU_NULL_ERR_COUNT];
    uint32_t        oom;
} gpu_null_device;

#define GPU_NULL_DEV(d) ((gpu_null_device*)(void*)(d))

/* The recorders' command table (gpu_null_cmd.h's commands). */
extern const gpu_cmd_ops gpu_null_cmd_ops;

/* gpu_null.c: each command, recorded and checked (gpu_null_cmd.h calls these),
 * and the per-draw device calls. */
#define GPU__NULL_CMD_DECL(name, form, ret, params, args) ret gpu_null_cmd_##name params;
GPU_CMD_OPS(GPU__NULL_CMD_DECL)
#undef GPU__NULL_CMD_DECL
#define GPU__NULL_FAST_DECL(name, form, ret, params, args) ret gpu_null_fast_##name params;
GPU_FAST_FUNCS(GPU__NULL_FAST_DECL)
#undef GPU__NULL_FAST_DECL

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_HAL */
#endif /* PS3RECOMP_GPU_NULL_INTERNAL_H */
