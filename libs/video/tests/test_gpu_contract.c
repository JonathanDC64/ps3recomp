/*
 * ps3recomp - the GPU layer's contract test, C half (C99 where the compiler
 * has the switch; CMake target test_gpu_contract, built with
 * PS3RECOMP_RSX_HAL=ON)
 *
 * What it pins:
 *   - gpu.h, gpu_cmd_ops.h and gpu_vt_cmd.h build as plain C;
 *   - the numbers gpu.h borrows from D3D12 are D3D12's: against <d3d12.h>
 *     and <dxgi1_6.h> on Windows, against the documented values everywhere;
 *   - the structures copied field for field from D3D12 have its layout;
 *   - gpu_vt_cmd.h defines every command with the list's parameters and
 *     every per-draw device call (GPU_FAST_FUNCS) with its gpu_vt
 *     function's (GPU_CMD_CHECK), and its wrappers and gpu.h's reach the
 *     right entry of a recorder's or a device's table.
 * The pipeline description's size is pinned too: its bytes name a pipeline
 * in the cache, so a change to it is a change to every name.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

#include "gpu.h"
#include "gpu_vt_cmd.h"
#include "test_gpu_contract.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond)                                                              \
    do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
                        g_fail++; } } while (0)

/* NUM(ours, value, theirs): gpu.h's number is the documented one and, on
 * Windows, the SDK's. */
#ifdef _WIN32
#define NUM(ours, value, theirs) GPU_STATIC_ASSERT((ours) == (value) && (ours) == (theirs), #ours)
#else
#define NUM(ours, value, theirs) GPU_STATIC_ASSERT((ours) == (value), #ours)
#endif

/* ---- Formats (DXGI_FORMAT) ---- */
NUM(GPU_FORMAT_UNKNOWN,                  0, DXGI_FORMAT_UNKNOWN);
NUM(GPU_FORMAT_R32G32B32A32_FLOAT,       2, DXGI_FORMAT_R32G32B32A32_FLOAT);
NUM(GPU_FORMAT_R32G32B32_FLOAT,          6, DXGI_FORMAT_R32G32B32_FLOAT);
NUM(GPU_FORMAT_R16G16B16A16_FLOAT,      10, DXGI_FORMAT_R16G16B16A16_FLOAT);
NUM(GPU_FORMAT_R16G16B16A16_UNORM,      11, DXGI_FORMAT_R16G16B16A16_UNORM);
NUM(GPU_FORMAT_R32G32_FLOAT,            16, DXGI_FORMAT_R32G32_FLOAT);
NUM(GPU_FORMAT_R32G8X24_TYPELESS,       19, DXGI_FORMAT_R32G8X24_TYPELESS);
NUM(GPU_FORMAT_D32_FLOAT_S8X24_UINT,    20, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
NUM(GPU_FORMAT_R32_FLOAT_X8X24_TYPELESS, 21, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
NUM(GPU_FORMAT_X32_TYPELESS_G8X24_UINT, 22, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT);
NUM(GPU_FORMAT_R10G10B10A2_UNORM,       24, DXGI_FORMAT_R10G10B10A2_UNORM);
NUM(GPU_FORMAT_R8G8B8A8_TYPELESS,       27, DXGI_FORMAT_R8G8B8A8_TYPELESS);
NUM(GPU_FORMAT_R8G8B8A8_UNORM,          28, DXGI_FORMAT_R8G8B8A8_UNORM);
NUM(GPU_FORMAT_R8G8B8A8_UNORM_SRGB,     29, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
NUM(GPU_FORMAT_R16G16_FLOAT,            34, DXGI_FORMAT_R16G16_FLOAT);
NUM(GPU_FORMAT_R16G16_UNORM,            35, DXGI_FORMAT_R16G16_UNORM);
NUM(GPU_FORMAT_R32_TYPELESS,            39, DXGI_FORMAT_R32_TYPELESS);
NUM(GPU_FORMAT_D32_FLOAT,               40, DXGI_FORMAT_D32_FLOAT);
NUM(GPU_FORMAT_R32_FLOAT,               41, DXGI_FORMAT_R32_FLOAT);
NUM(GPU_FORMAT_R32_UINT,                42, DXGI_FORMAT_R32_UINT);
NUM(GPU_FORMAT_R24G8_TYPELESS,          44, DXGI_FORMAT_R24G8_TYPELESS);
NUM(GPU_FORMAT_D24_UNORM_S8_UINT,       45, DXGI_FORMAT_D24_UNORM_S8_UINT);
NUM(GPU_FORMAT_R24_UNORM_X8_TYPELESS,   46, DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
NUM(GPU_FORMAT_X24_TYPELESS_G8_UINT,    47, DXGI_FORMAT_X24_TYPELESS_G8_UINT);
NUM(GPU_FORMAT_R8G8_UNORM,              49, DXGI_FORMAT_R8G8_UNORM);
NUM(GPU_FORMAT_R16_FLOAT,               54, DXGI_FORMAT_R16_FLOAT);
NUM(GPU_FORMAT_D16_UNORM,               55, DXGI_FORMAT_D16_UNORM);
NUM(GPU_FORMAT_R16_UNORM,               56, DXGI_FORMAT_R16_UNORM);
NUM(GPU_FORMAT_R16_UINT,                57, DXGI_FORMAT_R16_UINT);
NUM(GPU_FORMAT_R8_UNORM,                61, DXGI_FORMAT_R8_UNORM);
NUM(GPU_FORMAT_R8_UINT,                 62, DXGI_FORMAT_R8_UINT);
NUM(GPU_FORMAT_A8_UNORM,                65, DXGI_FORMAT_A8_UNORM);
NUM(GPU_FORMAT_BC1_UNORM,               71, DXGI_FORMAT_BC1_UNORM);
NUM(GPU_FORMAT_BC2_UNORM,               74, DXGI_FORMAT_BC2_UNORM);
NUM(GPU_FORMAT_BC3_UNORM,               77, DXGI_FORMAT_BC3_UNORM);
NUM(GPU_FORMAT_B5G6R5_UNORM,            85, DXGI_FORMAT_B5G6R5_UNORM);
NUM(GPU_FORMAT_B5G5R5A1_UNORM,          86, DXGI_FORMAT_B5G5R5A1_UNORM);
NUM(GPU_FORMAT_B8G8R8A8_UNORM,          87, DXGI_FORMAT_B8G8R8A8_UNORM);
NUM(GPU_FORMAT_B8G8R8X8_UNORM,          88, DXGI_FORMAT_B8G8R8X8_UNORM);
NUM(GPU_FORMAT_B8G8R8A8_UNORM_SRGB,     91, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
NUM(GPU_FORMAT_B4G4R4A4_UNORM,         115, DXGI_FORMAT_B4G4R4A4_UNORM);

/* ---- Resource states (D3D12_RESOURCE_STATES) ---- */
NUM(GPU_STATE_COMMON,                    0x0, D3D12_RESOURCE_STATE_COMMON);
NUM(GPU_STATE_PRESENT,                   0x0, D3D12_RESOURCE_STATE_PRESENT);
NUM(GPU_STATE_VERTEX_AND_CONSTANT_BUFFER, 0x1, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
NUM(GPU_STATE_INDEX_BUFFER,              0x2, D3D12_RESOURCE_STATE_INDEX_BUFFER);
NUM(GPU_STATE_RENDER_TARGET,             0x4, D3D12_RESOURCE_STATE_RENDER_TARGET);
NUM(GPU_STATE_UNORDERED_ACCESS,          0x8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
NUM(GPU_STATE_DEPTH_WRITE,              0x10, D3D12_RESOURCE_STATE_DEPTH_WRITE);
NUM(GPU_STATE_DEPTH_READ,               0x20, D3D12_RESOURCE_STATE_DEPTH_READ);
NUM(GPU_STATE_NON_PIXEL_SHADER_RESOURCE, 0x40, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
NUM(GPU_STATE_PIXEL_SHADER_RESOURCE,    0x80, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
NUM(GPU_STATE_INDIRECT_ARGUMENT,       0x200, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
NUM(GPU_STATE_COPY_DEST,               0x400, D3D12_RESOURCE_STATE_COPY_DEST);
NUM(GPU_STATE_COPY_SOURCE,             0x800, D3D12_RESOURCE_STATE_COPY_SOURCE);
NUM(GPU_STATE_RESOLVE_DEST,           0x1000, D3D12_RESOURCE_STATE_RESOLVE_DEST);
NUM(GPU_STATE_RESOLVE_SOURCE,         0x2000, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
NUM(GPU_STATE_GENERIC_READ,            0xAC3, D3D12_RESOURCE_STATE_GENERIC_READ);
NUM(GPU_SUB_ALL,                 0xFFFFFFFFu, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
NUM(GPU_BARRIER_TRANSITION,                0, D3D12_RESOURCE_BARRIER_TYPE_TRANSITION);
NUM(GPU_BARRIER_ALIASING,                  1, D3D12_RESOURCE_BARRIER_TYPE_ALIASING);

/* ---- Swizzles (Shader4ComponentMapping) ---- */
NUM(GPU_SWIZZLE_IDENTITY, 0x1688u, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
NUM(GPU_SWIZZLE(GPU_SWZ_R, GPU_SWZ_G, GPU_SWZ_B, GPU_SWZ_A), 0x1688u, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
NUM(GPU_SWIZZLE(0, 0, 0, 0), 0x1000u, D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0));   /* today's B8 */
NUM(GPU_SWIZZLE(GPU_SWZ_A, GPU_SWZ_ZERO, GPU_SWZ_ONE, GPU_SWZ_B), 0x1563u,
    D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(3, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                                            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1, 2));
NUM(GPU_SWIZZLE_GET(0x1563u, 2), GPU_SWZ_ONE, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);

/* ---- Blending, depth, stencil ---- */
NUM(GPU_BLEND_ZERO,              1, D3D12_BLEND_ZERO);
NUM(GPU_BLEND_ONE,               2, D3D12_BLEND_ONE);
NUM(GPU_BLEND_SRC_COLOR,         3, D3D12_BLEND_SRC_COLOR);
NUM(GPU_BLEND_INV_SRC_COLOR,     4, D3D12_BLEND_INV_SRC_COLOR);
NUM(GPU_BLEND_SRC_ALPHA,         5, D3D12_BLEND_SRC_ALPHA);
NUM(GPU_BLEND_INV_SRC_ALPHA,     6, D3D12_BLEND_INV_SRC_ALPHA);
NUM(GPU_BLEND_DEST_ALPHA,        7, D3D12_BLEND_DEST_ALPHA);
NUM(GPU_BLEND_INV_DEST_ALPHA,    8, D3D12_BLEND_INV_DEST_ALPHA);
NUM(GPU_BLEND_DEST_COLOR,        9, D3D12_BLEND_DEST_COLOR);
NUM(GPU_BLEND_INV_DEST_COLOR,   10, D3D12_BLEND_INV_DEST_COLOR);
NUM(GPU_BLEND_SRC_ALPHA_SAT,    11, D3D12_BLEND_SRC_ALPHA_SAT);
NUM(GPU_BLEND_BLEND_FACTOR,     14, D3D12_BLEND_BLEND_FACTOR);
NUM(GPU_BLEND_INV_BLEND_FACTOR, 15, D3D12_BLEND_INV_BLEND_FACTOR);
NUM(GPU_BLEND_SRC1_COLOR,       16, D3D12_BLEND_SRC1_COLOR);
NUM(GPU_BLEND_INV_SRC1_COLOR,   17, D3D12_BLEND_INV_SRC1_COLOR);
NUM(GPU_BLEND_SRC1_ALPHA,       18, D3D12_BLEND_SRC1_ALPHA);
NUM(GPU_BLEND_INV_SRC1_ALPHA,   19, D3D12_BLEND_INV_SRC1_ALPHA);
NUM(GPU_BLEND_OP_ADD,            1, D3D12_BLEND_OP_ADD);
NUM(GPU_BLEND_OP_SUBTRACT,       2, D3D12_BLEND_OP_SUBTRACT);
NUM(GPU_BLEND_OP_REV_SUBTRACT,   3, D3D12_BLEND_OP_REV_SUBTRACT);
NUM(GPU_BLEND_OP_MIN,            4, D3D12_BLEND_OP_MIN);
NUM(GPU_BLEND_OP_MAX,            5, D3D12_BLEND_OP_MAX);
NUM(GPU_WRITE_R,                 1, D3D12_COLOR_WRITE_ENABLE_RED);
NUM(GPU_WRITE_G,                 2, D3D12_COLOR_WRITE_ENABLE_GREEN);
NUM(GPU_WRITE_B,                 4, D3D12_COLOR_WRITE_ENABLE_BLUE);
NUM(GPU_WRITE_A,                 8, D3D12_COLOR_WRITE_ENABLE_ALPHA);
NUM(GPU_WRITE_ALL,              15, D3D12_COLOR_WRITE_ENABLE_ALL);
NUM(GPU_CMP_NEVER,               1, D3D12_COMPARISON_FUNC_NEVER);
NUM(GPU_CMP_LESS,                2, D3D12_COMPARISON_FUNC_LESS);
NUM(GPU_CMP_EQUAL,               3, D3D12_COMPARISON_FUNC_EQUAL);
NUM(GPU_CMP_LESS_EQUAL,          4, D3D12_COMPARISON_FUNC_LESS_EQUAL);
NUM(GPU_CMP_GREATER,             5, D3D12_COMPARISON_FUNC_GREATER);
NUM(GPU_CMP_NOT_EQUAL,           6, D3D12_COMPARISON_FUNC_NOT_EQUAL);
NUM(GPU_CMP_GREATER_EQUAL,       7, D3D12_COMPARISON_FUNC_GREATER_EQUAL);
NUM(GPU_CMP_ALWAYS,              8, D3D12_COMPARISON_FUNC_ALWAYS);
NUM(GPU_STENCIL_KEEP,            1, D3D12_STENCIL_OP_KEEP);
NUM(GPU_STENCIL_ZERO,            2, D3D12_STENCIL_OP_ZERO);
NUM(GPU_STENCIL_REPLACE,         3, D3D12_STENCIL_OP_REPLACE);
NUM(GPU_STENCIL_INCR_SAT,        4, D3D12_STENCIL_OP_INCR_SAT);
NUM(GPU_STENCIL_DECR_SAT,        5, D3D12_STENCIL_OP_DECR_SAT);
NUM(GPU_STENCIL_INVERT,          6, D3D12_STENCIL_OP_INVERT);
NUM(GPU_STENCIL_INCR,            7, D3D12_STENCIL_OP_INCR);
NUM(GPU_STENCIL_DECR,            8, D3D12_STENCIL_OP_DECR);

/* ---- Rasterizer and input ---- */
NUM(GPU_CULL_NONE,               1, D3D12_CULL_MODE_NONE);
NUM(GPU_CULL_FRONT,              2, D3D12_CULL_MODE_FRONT);
NUM(GPU_CULL_BACK,               3, D3D12_CULL_MODE_BACK);
NUM(GPU_TOPOLOGY_TYPE_POINT,     1, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
NUM(GPU_TOPOLOGY_TYPE_LINE,      2, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
NUM(GPU_TOPOLOGY_TYPE_TRIANGLE,  3, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
NUM(GPU_TOPO_POINTLIST,          1, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
NUM(GPU_TOPO_LINELIST,           2, D3D_PRIMITIVE_TOPOLOGY_LINELIST);
NUM(GPU_TOPO_LINESTRIP,          3, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP);
NUM(GPU_TOPO_TRIANGLELIST,       4, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
NUM(GPU_TOPO_TRIANGLESTRIP,      5, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
NUM(GPU_STRIP_CUT_NONE,          0, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED);
NUM(GPU_STRIP_CUT_FFFF,          1, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
NUM(GPU_STRIP_CUT_FFFFFFFF,      2, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF);

/* ---- Samplers ---- */
NUM(GPU_FILTER(0, 0, 0, 0),    0x00, D3D12_FILTER_MIN_MAG_MIP_POINT);
NUM(GPU_FILTER(1, 1, 0, 0),    0x14, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT);
NUM(GPU_FILTER(1, 1, 1, 0),    0x15, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
NUM(GPU_FILTER(1, 1, 1, 1),    0x95, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR);
NUM(GPU_FILTER(1, 0, 1, 0),    0x11, D3D12_ENCODE_BASIC_FILTER(D3D12_FILTER_TYPE_LINEAR, D3D12_FILTER_TYPE_POINT,
                                                               D3D12_FILTER_TYPE_LINEAR, D3D12_FILTER_REDUCTION_TYPE_STANDARD));
NUM(GPU_FILTER_ANISO(0),       0x55, D3D12_FILTER_ANISOTROPIC);
NUM(GPU_FILTER_ANISO(1),       0xD5, D3D12_FILTER_COMPARISON_ANISOTROPIC);
NUM(GPU_ADDRESS_WRAP,             1, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
NUM(GPU_ADDRESS_MIRROR,           2, D3D12_TEXTURE_ADDRESS_MODE_MIRROR);
NUM(GPU_ADDRESS_CLAMP,            3, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
NUM(GPU_ADDRESS_BORDER,           4, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
NUM(GPU_ADDRESS_MIRROR_ONCE,      5, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE);

/* ---- Layouts, views, memory, queries ---- */
NUM(GPU_CLEAR_DEPTH,              1, D3D12_CLEAR_FLAG_DEPTH);
NUM(GPU_CLEAR_STENCIL,            2, D3D12_CLEAR_FLAG_STENCIL);
NUM(GPU_VIS_ALL,                  0, D3D12_SHADER_VISIBILITY_ALL);
NUM(GPU_VIS_VERTEX,               1, D3D12_SHADER_VISIBILITY_VERTEX);
NUM(GPU_VIS_GEOMETRY,             4, D3D12_SHADER_VISIBILITY_GEOMETRY);
NUM(GPU_VIS_PIXEL,                5, D3D12_SHADER_VISIBILITY_PIXEL);
NUM(GPU_LAYOUT_VERTEX_INPUT,      1, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
NUM(GPU_VIEW_READ_ONLY_DEPTH,     1, D3D12_DSV_FLAG_READ_ONLY_DEPTH);
NUM(GPU_VIEW_READ_ONLY_STENCIL,   2, D3D12_DSV_FLAG_READ_ONLY_STENCIL);
NUM(GPU_MEM_GPU,                  1, D3D12_HEAP_TYPE_DEFAULT);
NUM(GPU_MEM_UPLOAD,               2, D3D12_HEAP_TYPE_UPLOAD);
NUM(GPU_MEM_READBACK,             3, D3D12_HEAP_TYPE_READBACK);
NUM(GPU_HEAP_SAMPLED_TEXTURES, 0x44, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES);
NUM(GPU_QUERY_OCCLUSION,          0, D3D12_QUERY_TYPE_OCCLUSION);
NUM(GPU_QUERY_BINARY_OCCLUSION,   1, D3D12_QUERY_TYPE_BINARY_OCCLUSION);

/* ---- Presenting ---- */
NUM(GPU_COLOR_SRGB,               0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
NUM(GPU_COLOR_SCRGB,              1, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
NUM(GPU_COLOR_HDR10,             12, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
NUM(GPU_PRESENT_TEARING,     0x200u, DXGI_PRESENT_ALLOW_TEARING);

/* ---- Structures copied field for field from D3D12 ---- */
#ifdef _WIN32
#define SAME(ours, theirs) GPU_STATIC_ASSERT(sizeof(ours) == sizeof(theirs), #ours " size")
#define FIELD(ours, a, theirs, b) GPU_STATIC_ASSERT(offsetof(ours, a) == offsetof(theirs, b) && \
                                                    sizeof(((ours*)0)->a) == sizeof(((theirs*)0)->b), #ours "." #a)
SAME(gpu_sampler_desc, D3D12_SAMPLER_DESC);
FIELD(gpu_sampler_desc, filter, D3D12_SAMPLER_DESC, Filter);
FIELD(gpu_sampler_desc, address_u, D3D12_SAMPLER_DESC, AddressU);
FIELD(gpu_sampler_desc, address_v, D3D12_SAMPLER_DESC, AddressV);
FIELD(gpu_sampler_desc, address_w, D3D12_SAMPLER_DESC, AddressW);
FIELD(gpu_sampler_desc, mip_lod_bias, D3D12_SAMPLER_DESC, MipLODBias);
FIELD(gpu_sampler_desc, max_aniso, D3D12_SAMPLER_DESC, MaxAnisotropy);
FIELD(gpu_sampler_desc, compare, D3D12_SAMPLER_DESC, ComparisonFunc);
FIELD(gpu_sampler_desc, border, D3D12_SAMPLER_DESC, BorderColor);
FIELD(gpu_sampler_desc, min_lod, D3D12_SAMPLER_DESC, MinLOD);
FIELD(gpu_sampler_desc, max_lod, D3D12_SAMPLER_DESC, MaxLOD);
SAME(gpu_footprint, D3D12_PLACED_SUBRESOURCE_FOOTPRINT);
FIELD(gpu_footprint, offset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT, Offset);
FIELD(gpu_footprint, format, D3D12_PLACED_SUBRESOURCE_FOOTPRINT, Footprint.Format);
FIELD(gpu_footprint, width, D3D12_PLACED_SUBRESOURCE_FOOTPRINT, Footprint.Width);
FIELD(gpu_footprint, height, D3D12_PLACED_SUBRESOURCE_FOOTPRINT, Footprint.Height);
FIELD(gpu_footprint, depth, D3D12_PLACED_SUBRESOURCE_FOOTPRINT, Footprint.Depth);
FIELD(gpu_footprint, row_pitch, D3D12_PLACED_SUBRESOURCE_FOOTPRINT, Footprint.RowPitch);
SAME(gpu_box, D3D12_BOX);
FIELD(gpu_box, left, D3D12_BOX, left);
FIELD(gpu_box, front, D3D12_BOX, front);
FIELD(gpu_box, back, D3D12_BOX, back);
SAME(gpu_rect, D3D12_RECT);
FIELD(gpu_rect, left, D3D12_RECT, left);
FIELD(gpu_rect, bottom, D3D12_RECT, bottom);
SAME(gpu_viewport, D3D12_VIEWPORT);
FIELD(gpu_viewport, x, D3D12_VIEWPORT, TopLeftX);
FIELD(gpu_viewport, width, D3D12_VIEWPORT, Width);
FIELD(gpu_viewport, max_depth, D3D12_VIEWPORT, MaxDepth);
#endif

/* Pinned: these bytes are a pipeline's name in the cache, and a view's
 * description is what a table entry is compared by. */
GPU_STATIC_ASSERT(sizeof(gpu_pipeline_desc) == 536, "gpu_pipeline_desc's size (the pipeline cache's names)");
GPU_STATIC_ASSERT(sizeof(gpu_view_desc) == 20, "gpu_view_desc's size");
GPU_STATIC_ASSERT(sizeof(gpu_vertex_attr) == 28, "gpu_vertex_attr's size");
GPU_STATIC_ASSERT(sizeof(gpu_pipeline) == 8 && sizeof(gpu_table) == 8, "the native handles");
GPU_STATIC_ASSERT(sizeof(gpu_buffer) == 4 && sizeof(gpu_texture) == 4, "the small handles");
/* What pipeline_create and pipeline_cache_load say: FAILED is 0, so a zeroed
 * answer is none; NONE is below the old 0 / 1 answers. */
GPU_STATIC_ASSERT(GPU_PIPELINE_FAILED == 0 && GPU_PIPELINE_LOADED == 1 && GPU_PIPELINE_COMPILED == 2 &&
                  GPU_PIPELINE_STORED == 3, "pipeline_create's answers");
GPU_STATIC_ASSERT(GPU_PIPELINE_CACHE_NONE == -1 && GPU_PIPELINE_CACHE_NEW == 0 && GPU_PIPELINE_CACHE_LOADED == 1,
                  "pipeline_cache_load's answers");
GPU_STATIC_ASSERT(GPU_NATIVE_PIPELINE == 9, "gpu_native's kinds");

/* ---- Every command, with the list's parameters ---- */
GPU_CMD_CHECK(vt_c);

/* A recorder that notes which entry each call reached. */
struct gpu_cmd {
    gpu_cmd_head head;
    int          last;
    int          count[GPU_CMD_OP_COUNT];
};
#define FAKE_CMD_V(name, ret, params) \
    static void fake_##name params { c->last = GPU_CMD_OP_##name; c->count[GPU_CMD_OP_##name]++; }
#define FAKE_CMD_R(name, ret, params) \
    static ret fake_##name params { ret r_; memset(&r_, 0, sizeof r_); \
                                    c->last = GPU_CMD_OP_##name; c->count[GPU_CMD_OP_##name]++; return r_; }
#define FAKE_CMD(name, form, ret, params, args) FAKE_CMD_##form(name, ret, params)
GPU_CMD_OPS(FAKE_CMD)
#define FAKE_CMD_INIT(name, form, ret, params, args) fake_##name,
static const gpu_cmd_ops k_fake_ops = { GPU_CMD_OPS(FAKE_CMD_INIT) };

/* GPU_CMD_OPS_TABLE from the commands in scope (here gpu_vt_cmd.h's): each
 * entry must be the command of its own name. */
static const gpu_cmd_ops k_wrapper_ops = GPU_CMD_OPS_TABLE;

/* A device whose every function notes it was called. */
#define VT_INDEX(name, form, ret, params, args) VT_##name,
enum { GPU_VT_FUNCS(VT_INDEX) VT_COUNT };
static int g_vt_last = -1;
#define FAKE_VT_V(name, ret, params) static void fakevt_##name params { (void)d; g_vt_last = VT_##name; }
#define FAKE_VT_R(name, ret, params) \
    static ret fakevt_##name params { ret r_; (void)d; memset(&r_, 0, sizeof r_); g_vt_last = VT_##name; return r_; }
#define FAKE_VT(name, form, ret, params, args) FAKE_VT_##form(name, ret, params)
GPU_VT_FUNCS(FAKE_VT)
#define FAKE_VT_INIT(name, form, ret, params, args) fakevt_##name,
static const gpu_vt k_fake_vt = { GPU_VT_FUNCS(FAKE_VT_INIT) };

typedef void (*any_fn)(void);
GPU_STATIC_ASSERT(sizeof(gpu_cmd_ops) == GPU_CMD_OP_COUNT * sizeof(any_fn), "one entry per command");
GPU_STATIC_ASSERT(sizeof(gpu_vt) == VT_COUNT * sizeof(any_fn), "one entry per device function");

int gpu_contract_c_sizes(size_t* out, int n)
{
    int i = 0;
#define C_SIZE(t) if (i < n) out[i] = sizeof(t); i++;
    GPU_CONTRACT_TYPES(C_SIZE)
#undef C_SIZE
    return i;
}

int gpu_contract_c_run(void)
{
    g_fail = 0;

    /* Handles. */
    {
        const uint32_t id = GPU_HANDLE_MAKE(5, 3);
        gpu_texture t;
        t.id = id;
        CHECK(GPU_HANDLE_INDEX(id) == 5);
        CHECK(GPU_HANDLE_GEN(id) == 3);
        CHECK(GPU_VALID(t));
        t.id = 0;
        CHECK(!GPU_VALID(t));
        CHECK(GPU_HANDLE_INDEX(GPU_HANDLE_MAKE(0xFFFFFE, 255)) == 0xFFFFFE);
    }
    CHECK(GPU_SUB(1, 2, 4) == 9);
    CHECK(GPU_FILTER_IS_ANISO(GPU_FILTER_ANISO(0)) && !GPU_FILTER_IS_ANISO(GPU_FILTER(1, 1, 1, 0)));
#ifdef _WIN32
    CHECK(GPU_LOD_MAX == D3D12_FLOAT32_MAX);
#endif

    /* The table macro: in the list's order. */
#define WRAPPER_IS(name, form, ret, params, args) CHECK(k_wrapper_ops.name == gpu_cmd_##name);
    GPU_CMD_OPS(WRAPPER_IS)
#undef WRAPPER_IS

    /* Commands through gpu_vt_cmd.h reach the recorder's own entries. */
    {
        struct gpu_cmd rec;
        memset(&rec, 0, sizeof rec);
        rec.head.ops = &k_fake_ops;
        gpu_cmd* const c = &rec;
        gpu_pass pass;
        gpu_viewport vp;
        gpu_rect sc;
        gpu_barrier b[2];
        gpu_footprint fp;
        gpu_texture tex;
        gpu_buffer buf;
        gpu_pipeline pso;
        gpu_table table;
        const float rgba[4] = {0, 0, 0, 1};
        memset(&pass, 0, sizeof pass);
        memset(&vp, 0, sizeof vp);
        memset(&sc, 0, sizeof sc);
        memset(b, 0, sizeof b);
        memset(&fp, 0, sizeof fp);
        tex.id = 1;
        buf.id = 2;
        pso.v = 0x1234;
        table.v = 0x5678;

        gpu_cmd_pass_begin(c, &pass);             CHECK(rec.last == GPU_CMD_OP_pass_begin);
        gpu_cmd_clear_color(c, pass.rt[0], rgba, NULL); CHECK(rec.last == GPU_CMD_OP_clear_color);
        gpu_cmd_set_viewport(c, &vp);             CHECK(rec.last == GPU_CMD_OP_set_viewport);
        gpu_cmd_set_scissor(c, &sc);              CHECK(rec.last == GPU_CMD_OP_set_scissor);
        gpu_cmd_set_pipeline(c, pso);             CHECK(rec.last == GPU_CMD_OP_set_pipeline);
        gpu_cmd_bind_table(c, 1, table);          CHECK(rec.last == GPU_CMD_OP_bind_table);
        gpu_cmd_bind_cb(c, 0, 0x1000);            CHECK(rec.last == GPU_CMD_OP_bind_cb);
        gpu_cmd_push_const1(c, 5, 7, 0);          CHECK(rec.last == GPU_CMD_OP_push_const1);
        gpu_cmd_draw_indexed(c, 3, 1, 0, -1, 0);  CHECK(rec.last == GPU_CMD_OP_draw_indexed);
        gpu_cmd_draw(c, 3, 1, 0, 0);              CHECK(rec.last == GPU_CMD_OP_draw);
        gpu_cmd_barrier(c, tex, GPU_STATE_RENDER_TARGET, GPU_STATE_COPY_SOURCE);
                                                  CHECK(rec.last == GPU_CMD_OP_barrier);
        gpu_cmd_barrier_n(c, 2, b);               CHECK(rec.last == GPU_CMD_OP_barrier_n);
        gpu_cmd_copy_tex_to_buf(c, buf, &fp, tex, 0, NULL); CHECK(rec.last == GPU_CMD_OP_copy_tex_to_buf);
        gpu_cmd_marker(c, "draw 0", 6);           CHECK(rec.last == GPU_CMD_OP_marker);
        gpu_cmd_pass_end(c);                      CHECK(rec.last == GPU_CMD_OP_pass_end);
        CHECK(rec.count[GPU_CMD_OP_draw] == 1 && rec.count[GPU_CMD_OP_resolve] == 0);
    }

    /* gpu.h's wrappers reach the device's own entries. */
    {
        gpu_device dev;
        gpu_buffer_desc bd;
        memset(&dev, 0, sizeof dev);
        memset(&bd, 0, sizeof bd);
        dev.vt = &k_fake_vt;
        gpu_device* const d = &dev;
        gpu_table_pool pool;
        gpu_buffer buf;
        gpu_sampler smp;
        gpu_shader sh;
        gpu_swapchain sc;
        pool.id = 1;
        buf.id = 2;
        smp.id = 3;
        sh.id = 4;
        sc.id = 5;
        (void)gpu_buffer_create(d, &bd);          CHECK(g_vt_last == VT_buffer_create);
        gpu_shader_destroy(d, sh);                CHECK(g_vt_last == VT_shader_destroy);
        CHECK(gpu_swap_image_count(d, sc) == 0);  CHECK(g_vt_last == VT_swap_image_count);
        CHECK(!GPU_VALID(gpu_swap_image(d, sc, 1))); CHECK(g_vt_last == VT_swap_image);
        /* The per-draw device calls through gpu_vt_cmd.h. */
        gpu_fast_table_write_tex(d, pool, 7, NULL); CHECK(g_vt_last == VT_table_write_tex);
        gpu_fast_table_write_sampler(d, pool, 7, smp); CHECK(g_vt_last == VT_table_write_sampler);
        {
            const gpu_table_writer w = gpu_table_writer_get(d, pool);
            CHECK(w.v == 0);                      CHECK(g_vt_last == VT_table_writer_get);
            gpu_fast_table_writer_tex(d, w, 7, NULL); CHECK(g_vt_last == VT_table_writer_tex);
        }
        CHECK(gpu_fast_table_ref(d, pool, 7).v == 0); CHECK(g_vt_last == VT_table_ref);
        CHECK(gpu_fast_buffer_addr(d, buf) == 0); CHECK(g_vt_last == VT_buffer_addr);
        (void)gpu_signal(d);                      CHECK(g_vt_last == VT_signal);
        (void)gpu_fence_wait(d, 1, 2000);         CHECK(g_vt_last == VT_fence_wait);
        CHECK(gpu_cmd_begin(d, 0) == NULL);       CHECK(g_vt_last == VT_cmd_begin);
        CHECK(gpu_native(d, GPU_NATIVE_DEVICE, 0) == NULL); CHECK(g_vt_last == VT_native);
        {
            gpu_pipeline_desc pd;
            uint32_t how = 7;
            memset(&pd, 0, sizeof pd);
            CHECK(gpu_pipeline_create(d, &pd, GPU_PIPELINE_STORE_ONLY, &how).v == 0);
            CHECK(g_vt_last == VT_pipeline_create);
            CHECK(gpu_pipeline_cache_load(d, "x") == 0); CHECK(g_vt_last == VT_pipeline_cache_load);
        }
        gpu_destroy(d);                           CHECK(g_vt_last == VT_destroy);
    }

    if (g_fail) printf("GPU contract (C): %d FAILED\n", g_fail);
    return g_fail;
}
