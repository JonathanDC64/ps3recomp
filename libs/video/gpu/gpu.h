/*
 * ps3recomp - the portable renderer's GPU layer (HAL): the contract
 *
 * One thin layer over D3D12, Vulkan and later Metal. The RSX code above it
 * (libs/video/rsxcore) is written once; each backend (gpu_d3d12*, gpu_vk*,
 * gpu_null*) implements this file. Built only with PS3RECOMP_RSX_HAL; today's
 * renderer (rsx_d3d12_backend.c) does not use it.
 *
 * Plain C99 with no system headers beyond <stdint.h> and <stddef.h>, so C and
 * C++ code on every platform can include it.
 *
 * The parts:
 *   - objects: a device, command recorders, and small handles for the rest;
 *   - numbers: formats, states, blend and compare values, swizzles. They are
 *     D3D12's own numbers, so code copied from today's renderer needs no
 *     translation; the Vulkan and Metal backends convert them with tables;
 *   - descriptions, one per kind of object;
 *   - the device's function table (gpu_vt), for creation and the calls made a
 *     few times a frame, with gpu_<name>() wrappers;
 *   - the commands a recorder takes are in gpu_cmd_ops.h. Code compiled once
 *     for every backend calls them through gpu_vt_cmd.h; the draw loop calls a
 *     backend's own static inline versions (gpu_<backend>_cmd.h) directly.
 *
 * Rules that keep it thin:
 *   - handles are small integers with a generation count, not reference
 *     counted objects, so passing one costs nothing;
 *   - the two handles used on every draw (gpu_pipeline, gpu_table) hold the
 *     native object itself, so a draw does no lookups;
 *   - barriers are the caller's: it already tracks each target's state;
 *   - feature bits are read once, from dev->caps.
 */
#ifndef PS3RECOMP_GPU_H
#define PS3RECOMP_GPU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C++11 or later (MSVC's cl says so in _MSVC_LANG, not __cplusplus). */
#if defined(__cplusplus) && (__cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L))
#define GPU_CPP11 1
#else
#define GPU_CPP11 0
#endif

/* A compile-time check that works in C99, C11 and C++. */
#if GPU_CPP11
#define GPU_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define GPU_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define GPU__SA_CAT2(a, b) a##b
#define GPU__SA_CAT(a, b) GPU__SA_CAT2(a, b)
#define GPU_STATIC_ASSERT(cond, msg) \
    typedef char GPU__SA_CAT(gpu_static_assert_, __LINE__)[(cond) ? 1 : -1]
#endif

/* ---------------------------------------------------------------------------
 * Objects
 * -------------------------------------------------------------------------*/

typedef struct gpu_device gpu_device;   /* below: a function table and the feature bits */
typedef struct gpu_vt gpu_vt;
/* A command recorder. Each backend defines the struct; it starts with a
 * gpu_cmd_head, so code built against gpu_vt_cmd.h can reach its commands. */
typedef struct gpu_cmd gpu_cmd;

/* Handles: 0 is none. The low 24 bits are the slot in the backend's table plus
 * one, the high 8 a generation count that changes each time the slot is
 * reused, so a stale handle is told from the slot's new object. A slot is
 * retired before its count comes round again, so a handle names one object
 * for the device's life: a table entry can be compared by its description
 * (gpu_table_write_tex). Wrapped in structs so the compiler tells a texture
 * from a buffer. */
typedef struct gpu_buffer     { uint32_t id; } gpu_buffer;      /* memory: vertices, constants, uploads, readbacks */
typedef struct gpu_texture    { uint32_t id; } gpu_texture;     /* 2D image, array or cube, any sample count */
typedef struct gpu_view       { uint32_t id; } gpu_view;        /* a texture as a colour target, depth target or texture */
typedef struct gpu_sampler    { uint32_t id; } gpu_sampler;     /* filtering and addressing */
typedef struct gpu_shader     { uint32_t id; } gpu_shader;      /* one compiled stage */
typedef struct gpu_layout     { uint32_t id; } gpu_layout;      /* what shaders bind: a root signature / pipeline layout */
typedef struct gpu_query_pool { uint32_t id; } gpu_query_pool;  /* occlusion counters */
typedef struct gpu_heap       { uint32_t id; } gpu_heap;        /* memory that textures are placed in */
typedef struct gpu_table_pool { uint32_t id; } gpu_table_pool;  /* shader-visible descriptors, cut into tables */
typedef struct gpu_swapchain  { uint32_t id; } gpu_swapchain;   /* a window's images */

#define GPU_HANDLE_INDEX(id)      (((id) & 0xFFFFFFu) - 1u)
#define GPU_HANDLE_GEN(id)        ((id) >> 24)
#define GPU_HANDLE_MAKE(idx, gen) ((((uint32_t)(gen) & 0xFFu) << 24) | (((uint32_t)(idx) + 1u) & 0xFFFFFFu))
#define GPU_VALID(h)              ((h).id != 0)

/* The handles a draw uses, holding the native object: an ID3D12PipelineState*
 * or VkPipeline, and a GPU descriptor handle or VkDescriptorSet. They live as
 * long as the device. */
typedef struct gpu_pipeline { uint64_t v; } gpu_pipeline;
typedef struct gpu_table    { uint64_t v; } gpu_table;

/* A table pool as its backend keeps it, for the per-draw writes
 * (table_writer_get, table_writer_tex): the record a write reaches, found
 * once. On D3D12 and Vulkan a pointer to it, on the null backend the pool's
 * id. It lives as long as the device, as pools do. */
typedef struct gpu_table_writer { uint64_t v; } gpu_table_writer;

/* A point on the device's timeline: work submitted before gpu_signal()
 * returned it is done when gpu_fence_completed() reaches it. A lost device
 * reads UINT64_MAX, as D3D12's fences do. */
typedef uint64_t gpu_fence;

/* A place in a buffer, for the commands that bind one: gpu_buffer_addr() of
 * the buffer plus a byte offset into it, the same arithmetic today's code
 * does on GetGPUVirtualAddress(). On D3D12 it is that address; elsewhere the
 * backend's own encoding. Addresses of two buffers are not comparable. */
typedef uint64_t gpu_addr;

/* ---------------------------------------------------------------------------
 * Numbers (D3D12's)
 * -------------------------------------------------------------------------*/

/* Which API a device runs on. */
enum {
    GPU_API_NULL   = 0,   /* records and checks, draws nothing (headless CI) */
    GPU_API_D3D12  = 1,
    GPU_API_VULKAN = 2,
    GPU_API_METAL  = 3
};

/* Formats: DXGI_FORMAT's numbers. Any DXGI number is a format; these are the
 * ones the renderer and the menu use. */
typedef uint32_t gpu_format;
enum {
    GPU_FORMAT_UNKNOWN                  = 0,
    GPU_FORMAT_R32G32B32A32_FLOAT       = 2,
    GPU_FORMAT_R32G32B32_FLOAT          = 6,
    GPU_FORMAT_R16G16B16A16_FLOAT       = 10,
    GPU_FORMAT_R16G16B16A16_UNORM       = 11,
    GPU_FORMAT_R32G32_FLOAT             = 16,
    GPU_FORMAT_R32G8X24_TYPELESS        = 19,   /* D32S8: the resource */
    GPU_FORMAT_D32_FLOAT_S8X24_UINT     = 20,   /* D32S8: the depth target */
    GPU_FORMAT_R32_FLOAT_X8X24_TYPELESS = 21,   /* D32S8: depth read as a texture */
    GPU_FORMAT_X32_TYPELESS_G8X24_UINT  = 22,   /* D32S8: stencil read as a texture */
    GPU_FORMAT_R10G10B10A2_UNORM        = 24,
    GPU_FORMAT_R8G8B8A8_TYPELESS        = 27,
    GPU_FORMAT_R8G8B8A8_UNORM           = 28,
    GPU_FORMAT_R8G8B8A8_UNORM_SRGB      = 29,
    GPU_FORMAT_R16G16_FLOAT             = 34,
    GPU_FORMAT_R16G16_UNORM             = 35,
    GPU_FORMAT_R32_TYPELESS             = 39,
    GPU_FORMAT_D32_FLOAT                = 40,
    GPU_FORMAT_R32_FLOAT                = 41,
    GPU_FORMAT_R32_UINT                 = 42,   /* also the 32-bit index format */
    GPU_FORMAT_R24G8_TYPELESS           = 44,   /* D24S8: the resource */
    GPU_FORMAT_D24_UNORM_S8_UINT        = 45,   /* D24S8: the depth target */
    GPU_FORMAT_R24_UNORM_X8_TYPELESS    = 46,   /* D24S8: depth read as a texture */
    GPU_FORMAT_X24_TYPELESS_G8_UINT     = 47,   /* D24S8: stencil read as a texture */
    GPU_FORMAT_R8G8_UNORM               = 49,
    GPU_FORMAT_R16_FLOAT                = 54,
    GPU_FORMAT_D16_UNORM                = 55,
    GPU_FORMAT_R16_UNORM                = 56,
    GPU_FORMAT_R16_UINT                 = 57,   /* also the 16-bit index format */
    GPU_FORMAT_R8_UNORM                 = 61,
    GPU_FORMAT_R8_UINT                  = 62,
    GPU_FORMAT_A8_UNORM                 = 65,
    GPU_FORMAT_BC1_UNORM                = 71,   /* DXT1 */
    GPU_FORMAT_BC2_UNORM                = 74,   /* DXT3 */
    GPU_FORMAT_BC3_UNORM                = 77,   /* DXT5 */
    GPU_FORMAT_B5G6R5_UNORM             = 85,
    GPU_FORMAT_B5G5R5A1_UNORM           = 86,
    GPU_FORMAT_B8G8R8A8_UNORM           = 87,
    GPU_FORMAT_B8G8R8X8_UNORM           = 88,
    GPU_FORMAT_B8G8R8A8_UNORM_SRGB      = 91,
    GPU_FORMAT_B4G4R4A4_UNORM           = 115
};

/* What a format can do at a sample count (gpu_format_support). */
enum {
    GPU_FMT_SAMPLED = 1u << 0,   /* read by shaders */
    GPU_FMT_FILTER  = 1u << 1,   /* ... with linear filtering */
    GPU_FMT_RT      = 1u << 2,   /* a colour target */
    GPU_FMT_BLEND   = 1u << 3,   /* ... with blending */
    GPU_FMT_DS      = 1u << 4,   /* a depth-stencil target */
    GPU_FMT_MSAA    = 1u << 5,   /* a target at the sample count asked */
    GPU_FMT_DISPLAY = 1u << 6    /* a swap chain's format */
};

/* Resource states: D3D12_RESOURCE_STATES' bits. On Vulkan a state is an
 * image layout plus the stages and accesses that use it. */
typedef uint32_t gpu_state;
enum {
    GPU_STATE_COMMON                     = 0,
    GPU_STATE_PRESENT                    = 0,
    GPU_STATE_VERTEX_AND_CONSTANT_BUFFER = 0x1,
    GPU_STATE_INDEX_BUFFER               = 0x2,
    GPU_STATE_RENDER_TARGET              = 0x4,
    GPU_STATE_UNORDERED_ACCESS           = 0x8,
    GPU_STATE_DEPTH_WRITE                = 0x10,
    GPU_STATE_DEPTH_READ                 = 0x20,
    GPU_STATE_NON_PIXEL_SHADER_RESOURCE  = 0x40,
    GPU_STATE_PIXEL_SHADER_RESOURCE      = 0x80,
    GPU_STATE_INDIRECT_ARGUMENT          = 0x200,
    GPU_STATE_COPY_DEST                  = 0x400,
    GPU_STATE_COPY_SOURCE                = 0x800,
    GPU_STATE_RESOLVE_DEST               = 0x1000,
    GPU_STATE_RESOLVE_SOURCE             = 0x2000,
    GPU_STATE_GENERIC_READ               = 0xAC3   /* an upload buffer's, for good */
};

/* A subresource: D3D12's number, mip + layer * mips. */
#define GPU_SUB(mip, layer, mips) ((uint32_t)(mip) + (uint32_t)(layer) * (uint32_t)(mips))
#define GPU_SUB_ALL               0xFFFFFFFFu

/* Swizzle: D3D12's Shader4ComponentMapping, 3 bits a channel (R, G, B, A
 * from the low bits up) plus bit 12. Each says which texel channel it reads,
 * or a forced 0 or 1. */
enum {
    GPU_SWZ_R = 0, GPU_SWZ_G = 1, GPU_SWZ_B = 2, GPU_SWZ_A = 3,
    GPU_SWZ_ZERO = 4, GPU_SWZ_ONE = 5
};
#define GPU_SWIZZLE(r, g, b, a) \
    (((uint32_t)(r) & 7u) | (((uint32_t)(g) & 7u) << 3) | (((uint32_t)(b) & 7u) << 6) | \
     (((uint32_t)(a) & 7u) << 9) | (1u << 12))
#define GPU_SWIZZLE_IDENTITY 0x1688u   /* GPU_SWIZZLE(R, G, B, A) */
#define GPU_SWIZZLE_GET(m, channel) (((uint32_t)(m) >> (3u * (uint32_t)(channel))) & 7u)

/* Blend factors and operations: D3D12_BLEND and D3D12_BLEND_OP. */
enum {
    GPU_BLEND_ZERO = 1, GPU_BLEND_ONE = 2,
    GPU_BLEND_SRC_COLOR = 3, GPU_BLEND_INV_SRC_COLOR = 4,
    GPU_BLEND_SRC_ALPHA = 5, GPU_BLEND_INV_SRC_ALPHA = 6,
    GPU_BLEND_DEST_ALPHA = 7, GPU_BLEND_INV_DEST_ALPHA = 8,
    GPU_BLEND_DEST_COLOR = 9, GPU_BLEND_INV_DEST_COLOR = 10,
    GPU_BLEND_SRC_ALPHA_SAT = 11,
    GPU_BLEND_BLEND_FACTOR = 14, GPU_BLEND_INV_BLEND_FACTOR = 15,
    GPU_BLEND_SRC1_COLOR = 16, GPU_BLEND_INV_SRC1_COLOR = 17,
    GPU_BLEND_SRC1_ALPHA = 18, GPU_BLEND_INV_SRC1_ALPHA = 19
};
enum {
    GPU_BLEND_OP_ADD = 1, GPU_BLEND_OP_SUBTRACT = 2, GPU_BLEND_OP_REV_SUBTRACT = 3,
    GPU_BLEND_OP_MIN = 4, GPU_BLEND_OP_MAX = 5
};
/* Colour write mask: an RGBA nibble (D3D12_COLOR_WRITE_ENABLE). */
enum { GPU_WRITE_R = 1, GPU_WRITE_G = 2, GPU_WRITE_B = 4, GPU_WRITE_A = 8, GPU_WRITE_ALL = 15 };

/* Compare functions (D3D12_COMPARISON_FUNC) and stencil operations
 * (D3D12_STENCIL_OP). */
enum {
    GPU_CMP_NEVER = 1, GPU_CMP_LESS = 2, GPU_CMP_EQUAL = 3, GPU_CMP_LESS_EQUAL = 4,
    GPU_CMP_GREATER = 5, GPU_CMP_NOT_EQUAL = 6, GPU_CMP_GREATER_EQUAL = 7, GPU_CMP_ALWAYS = 8
};
enum {
    GPU_STENCIL_KEEP = 1, GPU_STENCIL_ZERO = 2, GPU_STENCIL_REPLACE = 3, GPU_STENCIL_INCR_SAT = 4,
    GPU_STENCIL_DECR_SAT = 5, GPU_STENCIL_INVERT = 6, GPU_STENCIL_INCR = 7, GPU_STENCIL_DECR = 8
};

/* Rasterizer: face culling (D3D12_CULL_MODE), the pipeline's primitive kind
 * (D3D12_PRIMITIVE_TOPOLOGY_TYPE), a draw's topology (D3D_PRIMITIVE_TOPOLOGY)
 * and the strip cut index (D3D12_INDEX_BUFFER_STRIP_CUT_VALUE). */
enum { GPU_CULL_NONE = 1, GPU_CULL_FRONT = 2, GPU_CULL_BACK = 3 };
enum { GPU_TOPOLOGY_TYPE_POINT = 1, GPU_TOPOLOGY_TYPE_LINE = 2, GPU_TOPOLOGY_TYPE_TRIANGLE = 3 };
enum {
    GPU_TOPO_POINTLIST = 1, GPU_TOPO_LINELIST = 2, GPU_TOPO_LINESTRIP = 3,
    GPU_TOPO_TRIANGLELIST = 4, GPU_TOPO_TRIANGLESTRIP = 5
};
enum { GPU_STRIP_CUT_NONE = 0, GPU_STRIP_CUT_FFFF = 1, GPU_STRIP_CUT_FFFFFFFF = 2 };

/* Sampler filter: D3D12_FILTER's encoding. min, mag and mip are 0 point or
 * 1 linear; reduction 0 standard or 1 comparison (a depth compare). */
#define GPU_FILTER(min, mag, mip, reduction) \
    ((((uint32_t)(min) & 3u) << 4) | (((uint32_t)(mag) & 3u) << 2) | ((uint32_t)(mip) & 3u) | \
     (((uint32_t)(reduction) & 3u) << 7))
#define GPU_FILTER_ANISO(reduction) (0x40u | GPU_FILTER(1, 1, 1, reduction))
#define GPU_FILTER_IS_ANISO(f)      (((f) & 0x40u) != 0)
enum { GPU_FILTER_POINT = 0, GPU_FILTER_LINEAR = 1 };
enum { GPU_REDUCE_STANDARD = 0, GPU_REDUCE_COMPARE = 1 };
/* Addressing (D3D12_TEXTURE_ADDRESS_MODE). A border is transparent black. */
enum {
    GPU_ADDRESS_WRAP = 1, GPU_ADDRESS_MIRROR = 2, GPU_ADDRESS_CLAMP = 3,
    GPU_ADDRESS_BORDER = 4, GPU_ADDRESS_MIRROR_ONCE = 5
};

/* Clear flags (D3D12_CLEAR_FLAGS). */
enum { GPU_CLEAR_DEPTH = 1, GPU_CLEAR_STENCIL = 2 };

/* Which stages see a layout slot (D3D12_SHADER_VISIBILITY). */
enum { GPU_VIS_ALL = 0, GPU_VIS_VERTEX = 1, GPU_VIS_GEOMETRY = 4, GPU_VIS_PIXEL = 5 };

/* Swap chain colour spaces (DXGI_COLOR_SPACE_TYPE). */
enum {
    GPU_COLOR_SRGB  = 0,    /* RGB_FULL_G22_NONE_P709: RGBA8, SDR */
    GPU_COLOR_SCRGB = 1,    /* RGB_FULL_G10_NONE_P709: RGBA16F linear, HDR */
    GPU_COLOR_HDR10 = 12    /* RGB_FULL_G2084_NONE_P2020 */
};

/* Query kinds (D3D12_QUERY_TYPE). */
enum { GPU_QUERY_OCCLUSION = 0, GPU_QUERY_BINARY_OCCLUSION = 1 };

/* ---------------------------------------------------------------------------
 * Feature bits, read once at start (dev->caps)
 * -------------------------------------------------------------------------*/
enum {
    GPU_CAP_GS                  = 1u << 0,    /* geometry shaders (else the vertex-shader clamp) */
    GPU_CAP_D24S8               = 1u << 1,    /* D24_UNORM_S8_UINT targets (else Vulkan makes the D24S8
                                               * formats D32S8: the same use, float depth in copies) */
    GPU_CAP_DEPTH_CLAMP         = 1u << 2,    /* depth_clip 0 pipelines */
    GPU_CAP_MIRROR_ONCE         = 1u << 3,    /* GPU_ADDRESS_MIRROR_ONCE */
    GPU_CAP_BORDER              = 1u << 4,    /* GPU_ADDRESS_BORDER, transparent black */
    GPU_CAP_ANISO16             = 1u << 5,    /* anisotropy up to 16 */
    GPU_CAP_SWIZZLE             = 1u << 6,    /* any view swizzle, forced 0 and 1 included */
    GPU_CAP_BC                  = 1u << 7,    /* BC1-3 textures */
    GPU_CAP_RGBA32F_BLEND       = 1u << 8,    /* blending into RGBA32F targets */
    GPU_CAP_RGBA32F_FILTER      = 1u << 9,    /* linear filtering of RGBA32F */
    GPU_CAP_OCCLUSION_PRECISE   = 1u << 10,   /* sample counts, not just zero / not zero */
    GPU_CAP_HOST_IMPORT         = 1u << 11,   /* gpu_buffer_import_host */
    GPU_CAP_NULL_DESCRIPTOR     = 1u << 12,   /* empty table entries read zero */
    GPU_CAP_ROBUST2             = 1u << 13,   /* out-of-range buffer reads give zero */
    GPU_CAP_TEARING             = 1u << 14,   /* presenting at once with tearing (RSX_VSYNC=0) */
    GPU_CAP_PRESENT_MODE_SWITCH = 1u << 15,   /* the sync interval may change per present */
    GPU_CAP_PRESENT_WAIT        = 1u << 16,   /* waiting for a present to reach the display */
    GPU_CAP_PRESENT_TIMING      = 1u << 17,   /* presents at a given time, display timings */
    GPU_CAP_HDR_SCRGB           = 1u << 18,   /* GPU_COLOR_SCRGB swap chains */
    GPU_CAP_HDR_METADATA        = 1u << 19,   /* gpu_swap_hdr's metadata */
    GPU_CAP_PIPELINE_CACHE      = 1u << 20,   /* gpu_pipeline_cache_load / _save keep pipelines */
    GPU_CAP_DEVICE_FAULT        = 1u << 21,   /* gpu_fault_report says where a lost device was */
    GPU_CAP_BREADCRUMBS         = 1u << 22,   /* gpu_cmd_marker's text survives a lost device */
    GPU_CAP_UNUSED_ATTACHMENTS  = 1u << 23    /* a pipeline's targets may differ from the bound ones */
};

typedef struct gpu_caps {
    uint32_t api;                 /* GPU_API_* */
    uint32_t bits;                /* GPU_CAP_* */
    uint32_t cb_align;            /* a constant buffer address's alignment (256 on D3D12) */
    uint32_t max_aniso;           /* the highest sampler anisotropy */
    uint32_t host_import_align;   /* gpu_buffer_import_host's address and size alignment; 0 without it */
} gpu_caps;

/* ---------------------------------------------------------------------------
 * Descriptions
 * -------------------------------------------------------------------------*/

/* What a buffer or texture is used for. A texture is used only as its bits
 * say. On a buffer they are hints, but for CONSTANT: any buffer may be a
 * copy's source or destination (upload, readback and host ones always are,
 * with or without the COPY bits), and Vulkan gives every buffer every use. */
enum {
    GPU_USE_VERTEX    = 1u << 0,    /* buffers */
    GPU_USE_INDEX     = 1u << 1,
    GPU_USE_CONSTANT  = 1u << 2,    /* bind_cb: needed (Vulkan pads the buffer for a whole CB at any offset) */
    GPU_USE_RAW       = 1u << 3,    /* bind_raw: a shader's ByteAddressBuffer */
    GPU_USE_QUERY     = 1u << 4,    /* query_resolve's destination (a hint: any readback buffer will do) */
    GPU_USE_SAMPLED   = 1u << 8,    /* textures */
    GPU_USE_RT        = 1u << 9,    /* a colour target */
    GPU_USE_DS        = 1u << 10,   /* a depth-stencil target */
    GPU_USE_COPY_SRC  = 1u << 16,   /* both */
    GPU_USE_COPY_DST  = 1u << 17
};

/* Where memory lives: D3D12_HEAP_TYPE's numbers, plus imported host memory. */
enum {
    GPU_MEM_GPU      = 1,   /* the GPU's own */
    GPU_MEM_UPLOAD   = 2,   /* CPU-written, mapped for good */
    GPU_MEM_READBACK = 3,   /* GPU-written, read by the CPU */
    GPU_MEM_HOST     = 4    /* the caller's memory (gpu_buffer_import_host) */
};

typedef struct gpu_buffer_desc {
    uint64_t    size;
    uint32_t    use;      /* GPU_USE_* */
    uint32_t    memory;   /* GPU_MEM_GPU, _UPLOAD or _READBACK */
    gpu_state   state;    /* the state it starts in: GENERIC_READ for upload, COPY_DEST for readback */
    const char* name;     /* for debuggers and logs; may be NULL */
} gpu_buffer_desc;

/* Memory to place textures in (the texture pool's 64 MB heaps). */
enum { GPU_HEAP_SAMPLED_TEXTURES = 0x44 };   /* D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES */
typedef struct gpu_heap_desc {
    uint64_t size;
    uint32_t memory;   /* GPU_MEM_GPU */
    uint32_t flags;    /* GPU_HEAP_* */
} gpu_heap_desc;

typedef struct gpu_texture_desc {
    uint32_t    width, height;
    uint16_t    layers;          /* array slices; 6 for a cube */
    uint16_t    mips;
    gpu_format  format;          /* a TYPELESS format when views reinterpret it */
    uint32_t    samples;         /* 1, or the MSAA count */
    uint32_t    use;             /* GPU_USE_* */
    gpu_state   state;           /* the state it starts in */
    gpu_heap    heap;            /* placed at heap_offset when set; else memory of its own,
                                    which starts zeroed (step VC1: Vulkan too, as D3D12;
                                    except BC formats there); placed: what the heap held */
    uint64_t    heap_offset;     /* aligned as gpu_texture_size says */
    gpu_format  clear_format;    /* the clear value it is fastest to clear to; 0 for none */
    float       clear_color[4];
    float       clear_depth;
    uint32_t    clear_stencil;
    const char* name;
} gpu_texture_desc;

/* A buffer's layout of one subresource: D3D12_PLACED_SUBRESOURCE_FOOTPRINT,
 * field for field. offset is from the buffer's start; on D3D12 it is a
 * multiple of 512 and row_pitch of 256. */
typedef struct gpu_footprint {
    uint64_t   offset;
    gpu_format format;
    uint32_t   width, height, depth;
    uint32_t   row_pitch;
} gpu_footprint;

/* A region of a texture (D3D12_BOX): left, top and front in, right, bottom and
 * back out. */
typedef struct gpu_box { uint32_t left, top, front, right, bottom, back; } gpu_box;
/* A rectangle (D3D12_RECT): right and bottom out. */
typedef struct gpu_rect { int32_t left, top, right, bottom; } gpu_rect;
/* A viewport (D3D12_VIEWPORT), y down. Vulkan takes it as it is: its
 * shaders flip y instead (the SPIR-V target). */
typedef struct gpu_viewport { float x, y, width, height, min_depth, max_depth; } gpu_viewport;

/* Views. A colour or depth target view is a gpu_view (gpu_view_get); a
 * texture in a table is written from its description (gpu_table_write_tex),
 * which compares it with what the entry holds first. */
enum { GPU_VIEW_SRV = 0, GPU_VIEW_RTV = 1, GPU_VIEW_DSV = 2 };
enum { GPU_DIM_2D = 0, GPU_DIM_CUBE = 1, GPU_DIM_2DMS = 2, GPU_DIM_2D_ARRAY = 3 };
/* A depth view bound for reading while it is sampled (D3D12_DSV_FLAGS). */
enum { GPU_VIEW_READ_ONLY_DEPTH = 1, GPU_VIEW_READ_ONLY_STENCIL = 2 };
typedef struct gpu_view_desc {
    gpu_texture texture;
    uint8_t     kind;        /* GPU_VIEW_* */
    uint8_t     dim;         /* GPU_DIM_*; a cube's must match the shader's TextureCube */
    uint8_t     flags;       /* GPU_VIEW_READ_ONLY_* (depth views) */
    uint8_t     mips;        /* textures: mip levels from mip; 0 = all */
    gpu_format  format;      /* 0: the texture's own */
    uint32_t    swizzle;     /* textures: GPU_SWIZZLE(...); 0 = identity */
    uint16_t    mip;         /* the first (texture) or only (target) mip */
    uint16_t    layer;       /* targets: the array slice */
} gpu_view_desc;

/* A sampler: D3D12_SAMPLER_DESC, field for field. */
typedef struct gpu_sampler_desc {
    uint32_t filter;                          /* GPU_FILTER(...) or GPU_FILTER_ANISO(...) */
    uint32_t address_u, address_v, address_w; /* GPU_ADDRESS_* */
    float    mip_lod_bias;
    uint32_t max_aniso;                       /* 1-16 */
    uint32_t compare;                         /* GPU_CMP_*, with a comparison filter; else GPU_CMP_NEVER */
    float    border[4];                       /* transparent black: all 0 */
    float    min_lod, max_lod;                /* max_lod GPU_LOD_MAX: every mip */
} gpu_sampler_desc;
#define GPU_LOD_MAX 3.402823466e+38f          /* D3D12_FLOAT32_MAX */

/* Shader-visible descriptors for one kind of table entry. On D3D12 a pool is
 * one descriptor heap, and a table is any run of it, named by its first
 * entry. Vulkan makes one descriptor set per table up front, so the pool says
 * where its tables are: regions of `tables` tables of `size` entries from
 * `first`. Regions may overlap; a write reaches every table holding the
 * entry. A table is a region's first entry plus a multiple of its size
 * (table_ref gives none elsewhere on Vulkan), and a table slot binds only
 * tables of its own count (the region's size), from register 0.
 * ---- step VC1 ---- Up to 16 entries counts as 16 on both sides: a slot of
 * up to 16 binds any table of up to 16 (a 1-texture blit slot binds a pool's
 * 2-entry tables); past 16, only its own count. */
enum { GPU_TABLE_TEX = 0, GPU_TABLE_SAMPLER = 1 };
#define GPU_MAX_TABLE_REGIONS 4
typedef struct gpu_table_region { uint32_t first, tables, size; } gpu_table_region;
typedef struct gpu_table_pool_desc {
    uint32_t         kind;       /* GPU_TABLE_* */
    uint32_t         count;      /* entries in all */
    uint32_t         nregions;
    gpu_table_region region[GPU_MAX_TABLE_REGIONS];
} gpu_table_pool_desc;

/* A compiled stage. The compile service (gpu_shader.h) makes the code. */
enum { GPU_STAGE_VS = 0, GPU_STAGE_GS = 1, GPU_STAGE_PS = 2 };
enum { GPU_CODE_DXBC = 0, GPU_CODE_DXIL = 1, GPU_CODE_SPIRV = 2, GPU_CODE_MSL = 3, GPU_CODE_METALLIB = 4,
       GPU_CODE_KEY = 5 };   /* KEY: no code, a name for it (the null backend's: rsxc_null.c) */
typedef struct gpu_shader_desc {
    uint32_t    stage;    /* GPU_STAGE_* */
    uint32_t    code;     /* GPU_CODE_* */
    const void* bytes;    /* copied: the caller may free it */
    uint64_t    size;
    const char* entry;    /* NULL: "main" */
} gpu_shader_desc;

/* What shaders bind, slot by slot: the root signature's parameters, in
 * order. The commands name a slot by its place here (bind_cb(0, ...) binds
 * slot 0). The guest pipelines' layout is
 *   0 CB b0 (vertex), 1 TABLE_TEX t0-t15 (pixel), 2 CB b1 (pixel),
 *   3 TABLE_SAMPLER s0-s15 (pixel), 4 RAW t20 (vertex), 5 CONST32 b2 x1 (vertex). */
enum {
    GPU_SLOT_CB = 0,             /* a constant buffer at an address: b<reg> */
    GPU_SLOT_RAW = 1,            /* a raw buffer at an address: t<reg> */
    GPU_SLOT_CONST32 = 2,        /* count 32-bit values: b<reg> (a push constant) */
    GPU_SLOT_TABLE_TEX = 3,      /* count textures t<reg>.. from a table */
    GPU_SLOT_TABLE_SAMPLER = 4   /* count samplers s<reg>.. from a table */
};
#define GPU_MAX_SLOTS 8
#define GPU_MAX_STATIC_SAMPLERS 4
typedef struct gpu_layout_slot {
    uint8_t kind;          /* GPU_SLOT_* */
    uint8_t reg;           /* the first shader register */
    uint8_t count;         /* a table's entries, or CONST32's values */
    uint8_t visibility;    /* GPU_VIS_* */
} gpu_layout_slot;
/* A sampler fixed in the layout (a post pass's linear clamp). Its border is
 * transparent black, opaque black or opaque white. */
typedef struct gpu_static_sampler {
    gpu_sampler_desc desc;
    uint32_t         reg;          /* s<reg> */
    uint32_t         visibility;   /* GPU_VIS_* */
} gpu_static_sampler;
enum { GPU_LAYOUT_VERTEX_INPUT = 0x1 };   /* D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT */
typedef struct gpu_layout_desc {
    uint32_t           flags;      /* GPU_LAYOUT_* */
    uint32_t           nslots;
    gpu_layout_slot    slot[GPU_MAX_SLOTS];
    uint32_t           nstatic;
    gpu_static_sampler statics[GPU_MAX_STATIC_SAMPLERS];
} gpu_layout_desc;

/* A pipeline's vertex input, from one vertex buffer (set_vb slot 0). None
 * (count 0) for the fetch draws, which read their vertices themselves; the
 * guest pipelines' is 16 x RGBA32F "ATTR" 0-15, 256 bytes a vertex; the
 * menu's are its own. */
#define GPU_MAX_VERTEX_ATTRS 16
typedef struct gpu_vertex_attr {
    char       semantic[16];   /* the HLSL semantic, without its index: "ATTR", "POSITION" */
    uint32_t   semantic_index;
    gpu_format format;
    uint32_t   offset;         /* in the vertex */
} gpu_vertex_attr;
typedef struct gpu_vertex_layout {
    uint32_t        count;
    uint32_t        stride;    /* bytes a vertex (set_vb passes it too) */
    gpu_vertex_attr attr[GPU_MAX_VERTEX_ATTRS];
} gpu_vertex_layout;

typedef struct gpu_blend_state {
    uint8_t enable;
    uint8_t src, dst, op;                    /* GPU_BLEND_*, GPU_BLEND_OP_* */
    uint8_t src_alpha, dst_alpha, op_alpha;
    uint8_t write_mask;                      /* GPU_WRITE_*: 0 writes no colour */
} gpu_blend_state;
typedef struct gpu_stencil_face {
    uint8_t fail, depth_fail, pass;          /* GPU_STENCIL_* */
    uint8_t func;                            /* GPU_CMP_* */
} gpu_stencil_face;

/* A pipeline. Zero the whole description before filling it: the backend
 * names the pipeline in its cache by these bytes, with the shaders' code and
 * the layout's contents in place of their handles, as today's code names
 * its pipelines (pso_desc_key). So what it does not use stays zero (targets
 * past nrt, stencil faces with stencil_test 0), and the struct has no
 * padding: keep it so (pad0) when adding fields. */
typedef struct gpu_pipeline_desc {
    gpu_layout        layout;
    gpu_shader        vs, gs, ps;        /* gs may be none */
    gpu_vertex_layout vertex;
    uint8_t           topology_type;     /* GPU_TOPOLOGY_TYPE_* */
    uint8_t           cull;              /* GPU_CULL_* */
    uint8_t           front_ccw;         /* counter-clockwise faces are front faces */
    uint8_t           depth_clip;        /* D3D12's DepthClipEnable. 0 (the guest pipelines): depth is
                                          * clamped, not clipped (GPU_CAP_DEPTH_CLAMP) */
    uint8_t           multisample;       /* D3D12's MultisampleEnable (the menu's MSAA layers) */
    uint8_t           depth_test;
    uint8_t           depth_write;
    uint8_t           depth_func;        /* GPU_CMP_* */
    uint8_t           stencil_test;
    uint8_t           stencil_read_mask;
    uint8_t           stencil_write_mask;
    uint8_t           pad0;
    gpu_stencil_face  stencil_front, stencil_back;
    gpu_blend_state   blend;             /* every target's */
    uint32_t          nrt;               /* colour targets, 0-4 */
    gpu_format        rt_format[4];
    gpu_format        ds_format;         /* 0: no depth target */
    uint32_t          samples;           /* 1, or the targets' MSAA count */
    uint32_t          sample_mask;       /* 0xFFFFFFFF */
    uint32_t          strip_cut;         /* GPU_STRIP_CUT_* */
} gpu_pipeline_desc;

/* pipeline_create's flags. STORE_ONLY: a pipeline made only for the cache,
 * as the start's builds make the pipelines a later draw loads: loaded, or
 * compiled and stored, then freed at once (nothing has recorded it), and no
 * handle is returned. Without a cache it is compiled and freed. */
enum { GPU_PIPELINE_STORE_ONLY = 1u << 0 };
/* What pipeline_create did (*how). */
enum {
    GPU_PIPELINE_FAILED   = 0,   /* nothing made: a bad description or handle, or the driver refused it */
    GPU_PIPELINE_LOADED   = 1,   /* from the cache */
    GPU_PIPELINE_COMPILED = 2,   /* compiled, not stored: no cache, or another thread stored it first */
    GPU_PIPELINE_STORED   = 3    /* compiled and stored in the cache: new there, for the next save */
};
/* What pipeline_cache_load found. */
enum {
    GPU_PIPELINE_CACHE_NONE   = -1,   /* no cache on this device: pipelines are compiled, never stored */
    GPU_PIPELINE_CACHE_NEW    = 0,    /* an empty cache: no file, or another driver's or GPU's */
    GPU_PIPELINE_CACHE_LOADED = 1     /* the file's pipelines */
};

/* The targets of a pass: OMSetRenderTargets. ds none for no depth target;
 * a depth view made with GPU_VIEW_READ_ONLY_* binds it for reading. */
typedef struct gpu_pass {
    uint32_t nrt;
    gpu_view rt[4];
    gpu_view ds;
} gpu_pass;

/* One barrier (D3D12_RESOURCE_BARRIER). A transition moves a texture or a
 * buffer (one of the two set) between states; an aliasing barrier says a
 * placed texture now owns memory another one held. */
enum { GPU_BARRIER_TRANSITION = 0, GPU_BARRIER_ALIASING = 1 };
typedef struct gpu_barrier {
    uint32_t    type;      /* GPU_BARRIER_* */
    gpu_texture texture;
    gpu_buffer  buffer;
    uint32_t    sub;       /* GPU_SUB(...) or GPU_SUB_ALL */
    gpu_state   before, after;
} gpu_barrier;

/* gpu_submit's options. The swap chain ones are needed: ACQUIRE_WAIT on the
 * first submit that touches the image swap_acquire gave, PRESENT_SIGNAL on
 * the last before swap_present, and the image is used only between the two. */
enum {
    GPU_SUBMIT_ACQUIRE_WAIT   = 1u << 0,   /* wait for the swap chain's acquired image first */
    GPU_SUBMIT_PRESENT_SIGNAL = 1u << 1,   /* let the next present wait for this work */
    GPU_SUBMIT_SIGNAL         = 1u << 2    /* return a fence for this work (as gpu_signal after it) */
};
typedef struct gpu_submit_desc {
    gpu_swapchain swapchain;   /* for the two swap chain options */
    uint32_t      flags;       /* GPU_SUBMIT_* */
} gpu_submit_desc;

/* A swap chain. The window is an HWND on Windows and an SDL_Window* elsewhere. */
enum { GPU_SWAP_TEARING = 1u << 0 };      /* allow presenting with tearing (GPU_CAP_TEARING) */
enum { GPU_PRESENT_TEARING = 0x200u };    /* DXGI_PRESENT_ALLOW_TEARING: with interval 0 */
typedef struct gpu_swapchain_desc {
    void*      window;
    uint32_t   width, height;
    gpu_format format;          /* R8G8B8A8_UNORM, or R16G16B16A16_FLOAT for HDR */
    uint32_t   buffers;         /* images in flight (FRAME_COUNT); Vulkan may make more (swap_image_count) */
    uint32_t   colour_space;    /* GPU_COLOR_* */
    uint32_t   flags;           /* GPU_SWAP_* */
} gpu_swapchain_desc;

/* Present statistics (DXGI_FRAME_STATISTICS plus GetLastPresentCount): for
 * the display timing code. sync_qpc in QueryPerformanceCounter ticks on
 * Windows, CLOCK_MONOTONIC nanoseconds elsewhere. */
typedef struct gpu_present_stats {
    uint32_t present_count;        /* the last present that reached the display */
    uint32_t present_refresh_count;
    uint32_t sync_refresh_count;   /* the vblank count at sync_qpc */
    uint32_t last_present_count;   /* presents made */
    int64_t  sync_qpc;
} gpu_present_stats;

/* HDR10 metadata (DXGI_HDR_METADATA_HDR10's meaning, in floats). */
typedef struct gpu_hdr_metadata {
    float red[2], green[2], blue[2], white[2];   /* CIE xy */
    float max_nits, min_nits, max_cll, max_fall;
} gpu_hdr_metadata;

/* The adapter a device runs on. */
typedef struct gpu_adapter_info {
    char     name[128];
    char     driver[64];       /* the driver's name and version, as the API tells them */
    uint32_t vendor, device, subsystem, revision;
    uint64_t driver_version;
    uint64_t vram;             /* dedicated memory, bytes */
    uint8_t  luid[8];          /* Windows' adapter id: the same GPU in DXGI and Vulkan */
    uint8_t  uuid[16];         /* Vulkan's pipeline-cache id; zero on D3D12 */
} gpu_adapter_info;

/* What gpu_native hands back. */
enum {
    GPU_NATIVE_DEVICE = 0,    /* ID3D12Device* / VkDevice */
    GPU_NATIVE_QUEUE = 1,     /* ID3D12CommandQueue* / VkQueue */
    GPU_NATIVE_ADAPTER = 2,   /* IDXGIAdapter1* / VkPhysicalDevice */
    GPU_NATIVE_INSTANCE = 3,  /* IDXGIFactory* / VkInstance */
    GPU_NATIVE_CMD = 4,       /* handle (uintptr_t)gpu_cmd*: ID3D12GraphicsCommandList* / VkCommandBuffer */
    GPU_NATIVE_BUFFER = 5,    /* handle gpu_buffer.id: ID3D12Resource* / VkBuffer */
    GPU_NATIVE_TEXTURE = 6,   /* handle gpu_texture.id: ID3D12Resource* / VkImage */
    GPU_NATIVE_LAYOUT = 7,    /* handle gpu_layout.id: ID3D12RootSignature* / VkPipelineLayout */
    GPU_NATIVE_SWAPCHAIN = 8, /* handle gpu_swapchain.id: IDXGISwapChain3* / VkSwapchainKHR */
    GPU_NATIVE_PIPELINE = 9   /* handle gpu_pipeline.v: ID3D12PipelineState* / VkPipeline */
};

/* Creating a device. Each backend has its own create function (below). */
enum {
    GPU_DEVICE_LOW_POWER   = 1u << 0,   /* the low-power adapter (CELLMARK_IGPU) */
    GPU_DEVICE_BREADCRUMBS = 1u << 1,   /* crash breadcrumbs and page faults (D3D12_DRED) */
    GPU_DEVICE_USE_LUID    = 1u << 2,   /* the adapter with this luid */
    /* The software adapter, for tests and CI: WARP on D3D12
     * (IDXGIFactory4::EnumWarpAdapter); a CPU device such as lavapipe on
     * Vulkan (VK_PHYSICAL_DEVICE_TYPE_CPU). */
    GPU_DEVICE_WARP        = 1u << 3
};
typedef struct gpu_device_desc {
    uint32_t flags;       /* GPU_DEVICE_* */
    uint32_t cmd_slots;   /* command recorders (gpu_cmd_begin's slots): 0 for 2, up to 16 */
    uint32_t cap_mask;    /* GPU_CAP_* bits to turn off, to try a fallback (RSX_VK_MASK) */
    uint8_t  luid[8];
} gpu_device_desc;

/* ---------------------------------------------------------------------------
 * The device's function table
 * -------------------------------------------------------------------------*/
/* Every entry, as X(name, form, ret, params, args): form V returns nothing,
 * R returns ret. The first parameter is always the device, named d. Each
 * gets a wrapper, gpu_<name>(d, ...). Thread-safe where it says so;
 * otherwise from the render thread. */
#define GPU_VT_FUNCS(X) \
    /* Frees everything the device made, after waiting for the GPU. */ \
    X(destroy,           V, void,           (gpu_device* d), (d)) \
    /* The adapter it runs on (gpu_adapter(d, &info)). */ \
    X(adapter,           V, void,           (gpu_device* d, gpu_adapter_info* out), (d, out)) \
    /* 0 while the device works; once lost, why (an HRESULT, a VkResult). Any thread. */ \
    X(lost,              R, uint32_t,       (gpu_device* d), (d)) \
    X(wait_idle,         V, void,           (gpu_device* d), (d)) \
    /* GPU_FMT_* bits for a format at a sample count. */ \
    X(format_support,    R, uint32_t,       (gpu_device* d, gpu_format format, uint32_t samples), (d, format, samples)) \
    /* Buffers. Upload and readback ones are mapped for their whole life. */ \
    X(buffer_create,     R, gpu_buffer,     (gpu_device* d, const gpu_buffer_desc* desc), (d, desc)) \
    /* A buffer over the caller's memory (RSX local memory, for the write-back), in COPY_DEST. */ \
    X(buffer_import_host, R, gpu_buffer,    (gpu_device* d, void* host, uint64_t size), (d, host, size)) \
    /* Freed once the GPU has passed the fence `after`, one made after its last use (0: now, \
     * when the caller has seen a fence after its last use pass: fence_completed, fence_wait \
     * or wait_idle). The same for every destroy that takes `after`. */ \
    X(buffer_destroy,    V, void,           (gpu_device* d, gpu_buffer b, gpu_fence after), (d, b, after)) \
    /* The CPU's pointer to it; read_size bytes from read_offset are read (readback: once the \
     * caller has seen a fence after the GPU's writes pass). */ \
    X(buffer_map,        R, void*,          (gpu_device* d, gpu_buffer b, uint64_t read_offset, uint64_t read_size), (d, b, read_offset, read_size)) \
    /* Ends a map; written_size bytes from written_offset were written. */ \
    X(buffer_unmap,      V, void,           (gpu_device* d, gpu_buffer b, uint64_t written_offset, uint64_t written_size), (d, b, written_offset, written_size)) \
    X(buffer_addr,       R, gpu_addr,       (gpu_device* d, gpu_buffer b), (d, b)) \
    /* Textures. A heap holds placed textures; heaps live as long as the device. */ \
    X(heap_create,       R, gpu_heap,       (gpu_device* d, const gpu_heap_desc* desc), (d, desc)) \
    /* The memory a texture takes when placed, and its alignment. */ \
    X(texture_size,      V, void,           (gpu_device* d, const gpu_texture_desc* desc, uint64_t* size, uint64_t* align), (d, desc, size, align)) \
    /* Buffer layouts of n subresources from first_sub, packed from base_offset (GetCopyableFootprints); \
     * rows and row_bytes may be NULL. Returns the bytes they take. */ \
    X(texture_footprints, R, uint64_t,      (gpu_device* d, const gpu_texture_desc* desc, uint32_t first_sub, uint32_t n, uint64_t base_offset, gpu_footprint* out, uint32_t* rows, uint64_t* row_bytes), (d, desc, first_sub, n, base_offset, out, rows, row_bytes)) \
    X(texture_create,    R, gpu_texture,    (gpu_device* d, const gpu_texture_desc* desc), (d, desc)) \
    /* Freed once the GPU has passed `after`; its views go with it. A placed texture's memory \
     * is the caller's to reuse (with an aliasing barrier). */ \
    X(texture_destroy,   V, void,           (gpu_device* d, gpu_texture t, gpu_fence after), (d, t, after)) \
    /* A target view, made once per description and kept (cached). */ \
    X(view_get,          R, gpu_view,       (gpu_device* d, const gpu_view_desc* desc), (d, desc)) \
    /* A sampler, made once per description and kept (cached). */ \
    X(sampler_get,       R, gpu_sampler,    (gpu_device* d, const gpu_sampler_desc* desc), (d, desc)) \
    /* Tables. The GPU must be done with an entry before it is written: the caller has seen a \
     * fence after every submit that used a table holding it pass. */ \
    X(table_pool_create, R, gpu_table_pool, (gpu_device* d, const gpu_table_pool_desc* desc), (d, desc)) \
    /* Puts a texture at index. An empty entry, which reads zero: srv's texture none (an entry \
     * of srv's format, dim and mips), or srv NULL (R8G8B8A8_UNORM, 2D, one mip: today's \
     * per-draw empty slot). A new pool's entries are empty R8_UNORM ones. An entry already \
     * holding this description is left as it is (today's memo). */ \
    X(table_write_tex,   V, void,           (gpu_device* d, gpu_table_pool pool, uint32_t index, const gpu_view_desc* srv), (d, pool, index, srv)) \
    X(table_write_sampler, V, void,         (gpu_device* d, gpu_table_pool pool, uint32_t index, gpu_sampler s), (d, pool, index, s)) \
    /* The pool found once, for table_writer_tex, which skips the lookup of the pool's record \
     * table_write_tex makes on each call (pool: one this device made). */ \
    X(table_writer_get,  R, gpu_table_writer, (gpu_device* d, gpu_table_pool pool), (d, pool)) \
    /* table_write_tex through a writer: the same memo, the same write. */ \
    X(table_writer_tex,  V, void,           (gpu_device* d, gpu_table_writer w, uint32_t index, const gpu_view_desc* srv), (d, w, index, srv)) \
    /* The table starting at index, for bind_table: a table's first entry (the regions above; \
     * Vulkan gives none for any other index). */ \
    X(table_ref,         R, gpu_table,      (gpu_device* d, gpu_table_pool pool, uint32_t index), (d, pool, index)) \
    /* The compile service's hook for this device's shader target. request and result are \
     * gpu_shader.h's; opaque here. Returns nonzero on success. Thread-safe. */ \
    X(compile_hook,      R, int,            (gpu_device* d, const void* request, void* result), (d, request, result)) \
    /* A shader from compiled code (copied). Thread-safe: the start's pipeline workers make and \
     * free shaders beside the render thread. */ \
    X(shader_create,     R, gpu_shader,     (gpu_device* d, const gpu_shader_desc* desc), (d, desc)) \
    /* Frees a shader's code (today's code drops a blob once its pipeline is built); pipelines \
     * made from it keep working. Not while a pipeline_create that uses it runs. Vulkan: \
     * vkDestroyShaderModule. Thread-safe. */ \
    X(shader_destroy,    V, void,           (gpu_device* d, gpu_shader s), (d, s)) \
    X(layout_create,     R, gpu_layout,     (gpu_device* d, const gpu_layout_desc* desc), (d, desc)) \
    /* A pipeline, from the cache when it has it, else compiled (and stored in the cache). flags \
     * GPU_PIPELINE_*: with STORE_ONLY it is only stored, and none is returned. *how (may be \
     * NULL) says which (GPU_PIPELINE_LOADED, ...). A pipeline returned lives as long as the \
     * device. Thread-safe: the start's pipeline workers call it beside the render thread. */ \
    X(pipeline_create,   R, gpu_pipeline,   (gpu_device* d, const gpu_pipeline_desc* desc, uint32_t flags, uint32_t* how), (d, desc, flags, how)) \
    /* The pipeline cache's file. Load once, before pipelines are made: GPU_PIPELINE_CACHE_* \
     * (NONE on a device without GPU_CAP_PIPELINE_CACHE, or when the API cannot make one). Save \
     * may run on a thread of its own while pipelines are made: 1 when written. */ \
    X(pipeline_cache_load, R, int,          (gpu_device* d, const char* path), (d, path)) \
    X(pipeline_cache_save, R, int,          (gpu_device* d, const char* path), (d, path)) \
    X(query_pool_create, R, gpu_query_pool, (gpu_device* d, uint32_t type, uint32_t count), (d, type, count)) \
    /* Starts recording on a slot (0 to cmd_slots - 1) the GPU has finished with: the caller \
     * has seen a fence after the slot's last submit pass. */ \
    X(cmd_begin,         R, gpu_cmd*,       (gpu_device* d, uint32_t slot), (d, slot)) \
    X(cmd_end,           V, void,           (gpu_device* d, gpu_cmd* c), (d, c)) \
    /* Runs n ended recorders in order; a fence for them with GPU_SUBMIT_SIGNAL, else 0. */ \
    X(submit,            R, gpu_fence,      (gpu_device* d, gpu_cmd* const* cmds, uint32_t n, const gpu_submit_desc* how), (d, cmds, n, how)) \
    /* A new point on the timeline, after everything submitted and presented so far. */ \
    X(signal,            R, gpu_fence,      (gpu_device* d), (d)) \
    /* The last point the GPU has passed. Any thread. */ \
    X(fence_completed,   R, uint64_t,       (gpu_device* d), (d)) \
    /* 0 once the GPU has passed f, 1 after timeout_ms without (never unbounded: a lost device \
     * never signals). */ \
    X(fence_wait,        R, int,            (gpu_device* d, gpu_fence f, uint32_t timeout_ms), (d, f, timeout_ms)) \
    /* Swap chains. */ \
    X(swap_create,       R, gpu_swapchain,  (gpu_device* d, const gpu_swapchain_desc* desc), (d, desc)) \
    X(swap_destroy,      V, void,           (gpu_device* d, gpu_swapchain sc), (d, sc)) \
    /* New size or format (ResizeBuffers). The GPU must be idle and the images unused. 0 on success. */ \
    X(swap_resize,       R, int,            (gpu_device* d, gpu_swapchain sc, uint32_t width, uint32_t height, gpu_format format), (d, sc, width, height, format)) \
    /* The image to draw next and its index (below swap_image_count); it starts in GPU_STATE_PRESENT. \
     * Its size is the window's on Vulkan (the surface's); a minimized window gives none. */ \
    X(swap_acquire,      R, uint32_t,       (gpu_device* d, gpu_swapchain sc, gpu_texture* image), (d, sc, image)) \
    /* The swap chain's images: D3D12 has the buffers asked; Vulkan at least that many (the driver \
     * may make more), so arrays by image index take this count, not FRAME_COUNT. Changes only \
     * with swap_resize. */ \
    X(swap_image_count,  R, uint32_t,       (gpu_device* d, gpu_swapchain sc), (d, sc)) \
    /* Image index, the texture swap_acquire hands back with that index (none past the count): \
     * for making every image's view up front. Valid until the next swap_resize or swap_destroy. */ \
    X(swap_image,        R, gpu_texture,    (gpu_device* d, gpu_swapchain sc, uint32_t index), (d, sc, index)) \
    /* Shows it after `interval` vblanks (0: at once, GPU_PRESENT_TEARING to tear). GPU_PRESENTED (0) on \
     * success, else a GPU_PRESENT_* code (step VC2, at the end). */ \
    X(swap_present,      R, int,            (gpu_device* d, gpu_swapchain sc, uint32_t interval, uint32_t flags), (d, sc, interval, flags)) \
    /* 0 with *out filled; else nonzero (no statistics yet). Vulkan has them with \
     * GPU_CAP_PRESENT_WAIT: the last present seen on the display, sync_qpc when it was seen, \
     * no refresh counts. */ \
    X(swap_stats,        R, int,            (gpu_device* d, gpu_swapchain sc, gpu_present_stats* out), (d, sc, out)) \
    /* The colour space (GPU_COLOR_*) and, when not NULL, HDR10 metadata. 1 if the display took it. \
     * On Vulkan a new colour space makes the swap chain again: as for swap_resize. */ \
    X(swap_hdr,          R, int,            (gpu_device* d, gpu_swapchain sc, uint32_t colour_space, const gpu_hdr_metadata* meta), (d, sc, colour_space, meta)) \
    /* On a lost device, prints what the API says of where the GPU was (DRED, device fault). */ \
    X(fault_report,      V, void,           (gpu_device* d, const char* where), (d, where)) \
    /* The API's own object (GPU_NATIVE_*), for the few things that stay per backend. */ \
    X(native,            R, void*,          (gpu_device* d, uint32_t kind, uint64_t handle), (d, kind, handle))

#define GPU__VT_MEMBER(name, form, ret, params, args) ret (*name) params;
struct gpu_vt {
    GPU_VT_FUNCS(GPU__VT_MEMBER)
};
#undef GPU__VT_MEMBER

/* A device: every backend's starts with this. */
struct gpu_device {
    const gpu_vt* vt;
    gpu_caps      caps;
};

/* The wrappers: gpu_buffer_create(d, &desc) for d->vt->buffer_create(d, &desc). */
#define GPU__VT_WRAP_V(name, ret, params, args) \
    static inline void gpu_##name params { d->vt->name args; }
#define GPU__VT_WRAP_R(name, ret, params, args) \
    static inline ret gpu_##name params { return d->vt->name args; }
#define GPU__VT_WRAP(name, form, ret, params, args) GPU__VT_WRAP_##form(name, ret, params, args)
GPU_VT_FUNCS(GPU__VT_WRAP)
#undef GPU__VT_WRAP
#undef GPU__VT_WRAP_V
#undef GPU__VT_WRAP_R

/* The start of every backend's struct gpu_cmd. ops is the recorder's command
 * table (gpu_cmd_ops.h), for code built against gpu_vt_cmd.h. */
struct gpu_cmd_ops;
typedef struct gpu_cmd_head {
    const struct gpu_cmd_ops* ops;
    gpu_device*               dev;
} gpu_cmd_head;

/* The backends' create functions; each exists in builds with its backend.
 * NULL when the API, the adapter or a required feature is missing. */
gpu_device* gpu_d3d12_create(const gpu_device_desc* desc);
gpu_device* gpu_vk_create(const gpu_device_desc* desc);
gpu_device* gpu_null_create(const gpu_device_desc* desc);

#ifdef __cplusplus
}
#endif

/* ---- step V4 ---- */
#ifdef __cplusplus
extern "C" {
#endif

/* The menu's renderer on the GPU layer (the port's render_gpu.cpp) over the
 * D3D12 objects of a renderer that is not on it (today's): native handles
 * as void*, so this file needs no D3D12 header. In builds with the D3D12
 * backend; other backends have no such calls yet. */

/* A device over the caller's ID3D12Device* and ID3D12CommandQueue*, which
 * it AddRefs and releases at gpu_destroy. Its fence, recorders and
 * descriptor heaps are its own; its submits go to that queue, in order with
 * the caller's own work there. desc may be NULL; its cmd_slots and cap_mask
 * count, its adapter flags do not, nor GPU_DEVICE_BREADCRUMBS (DRED is
 * turned on before a device is made). NULL when a part cannot be made. */
gpu_device* gpu_d3d12_create_on(void* device, void* queue, const gpu_device_desc* desc);

/* A texture over the caller's ID3D12Resource* (a swap chain's buffer),
 * AddRef'd and released when the handle is destroyed: destroy it before the
 * swap chain's ResizeBuffers. out (may be NULL) gets its description as the
 * resource has it: size, layers, mips, format, samples, and its target
 * flags as use. Its state is the caller's to know. */
gpu_texture gpu_d3d12_texture_on(gpu_device* d, void* resource, gpu_texture_desc* out);

#ifdef __cplusplus
}
#endif

/* ---- step VC2 ---- */
/* A lost device and a present that failed, with one meaning on every
 * backend (each maps its API's results to these):
 *   - lost: 0 while the device works. Once it is lost (a driver reset, the
 *     GPU gone), never 0 again: the API's own code for why, which the caller
 *     names (rsxcore's rc_be_lost_text): D3D12's GetDeviceRemovedReason
 *     HRESULT, Vulkan's VkResult (VK_ERROR_DEVICE_LOST) as a uint32_t. A
 *     lost device's fences read UINT64_MAX, so fence_wait returns 0 at once.
 *   - fence_wait: 0 once the GPU has passed the fence or the device is lost,
 *     1 after the timeout or any other failure of the wait (never "passed"
 *     unless it is).
 *   - swap_present: GPU_PRESENTED when the image went to the display queue,
 *     a swap chain the window no longer fits included (Vulkan's out of date
 *     and suboptimal: the backend makes it again at the next acquire);
 *     GPU_PRESENT_LOST when the device is lost (lost says why);
 *     GPU_PRESENT_SURFACE_LOST when the window's surface is gone and could
 *     not be made again (the device still works; nothing reaches this
 *     window again: Vulkan's VK_ERROR_SURFACE_LOST_KHR); GPU_PRESENT_FAILED
 *     for anything else (no image acquired, out of memory). */
enum {
    GPU_PRESENTED            = 0,
    GPU_PRESENT_LOST         = 1,
    GPU_PRESENT_SURFACE_LOST = 2,
    GPU_PRESENT_FAILED       = 3
};
#endif /* PS3RECOMP_GPU_H */
