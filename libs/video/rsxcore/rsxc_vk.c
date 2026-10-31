/*
 * ps3recomp - the portable renderer's core, compiled for the Vulkan backend
 *
 * The route RSX_BACKEND=vulkan (libs/video/rsx_out.c): the same fragments as
 * rsxc_d3d12.c, after the Vulkan backend's static inline command header
 * (gpu_vk_cmd.h), so the draw loop calls vkCmd* through the device's
 * VolkDeviceTable directly, with no function table between.
 *
 * What differs from the D3D12 instance, all in the rc_be_* hooks below:
 *   - the shaders are SPIR-V: the compile service's spv target (glslang,
 *     the text edits E1-E5 and the post-passes; gpu_shader.h), each compiled
 *     for the layout it is bound with (the built-in passes pass theirs), and
 *     cached under <cache>/gpu/spv/ by the service. The post-passes the
 *     device cannot run are left out (rc_vk_spv_options);
 *   - none of today's cache files: no .dxbc, fp_variants.txt,
 *     pso_index_seen.tsv, pso_manifest.bin or pso_library.bin is read or
 *     written (rc_be_cache_files 0, as on null). Their records are D3D12
 *     descriptions, and the manifest is shared with the d3d12 route. The
 *     pipeline library is a VkPipelineCache file of its own, by driver
 *     (rc_be_lib_file, step VC2), with no manifest to build ahead from;
 *   - a frame slot is not a swap chain image (rc_be_frame_slot): the driver
 *     may make more images than FRAME_COUNT and hands them out in any order,
 *     so the slot (recorders, fences, display targets, the blit's tables)
 *     moves on by one at each present, and the image index only picks the
 *     window's image and its view;
 *   - the menu gets no native objects: its renderer is D3D12's until it is
 *     ported (step VC2).
 *
 * Windows and Linux (step VC2). The core still calls Win32 (threads, locks,
 * files, timing); on Linux those come from runtime/platform/win32_compat.h,
 * as for today's renderer on vkd3d (rsx_vkd3d_host.h), and the window is
 * the core's SDL one (rsxc_head.inc's and rsxc_window.inc's other branches,
 * copied with today's renderer). The platform module (Phase 5) takes them
 * later. Built with PS3RECOMP_RSX_HAL_VULKAN; the route needs the spv target
 * (PS3RECOMP_RSX_HAL_SPV) at run time, and says so when it is not built.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(PS3RECOMP_RSX_HAL_VULKAN) && (defined(_WIN32) || defined(__linux__))

#ifndef _WIN32
#include "../../../runtime/platform/win32_compat.h"
#include "rsxc_posix.h"   /* (the few Windows names win32_compat.h leaves out) */
#endif
#include "rsxc_head.inc"
/* The GPU layer, with the Vulkan backend's static inline commands (the draw
 * loop calls them directly), and the compile service. */
#include "gpu_vk_cmd.h"
#include "gpu_shader.h"

/* ---------------------------------------------------------------------------
 * The backend's hooks (rsxc_d3d12.c says what each is for)
 * -------------------------------------------------------------------------*/

/* The spv target's post-passes for this device: the float controls only
 * where the device keeps them (an execution mode the device lacks is
 * invalid SPIR-V for it), NoContraction and the invariant position always,
 * base vertex never (every draw starts at vertex 0, gpu_shader.h). */
static void rc_vk_spv_options(gpu_device* d)
{
    VkPhysicalDeviceVulkan12Properties p12;
    VkPhysicalDeviceProperties2 p2;
    memset(&p12, 0, sizeof p12);
    memset(&p2, 0, sizeof p2);
    p12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &p12;
    vkGetPhysicalDeviceProperties2(GPU_VK_DEV(d)->phys, &p2);
    unsigned opts = GPU_SC_SPV_DEFAULT & ~(GPU_SC_SPV_FLOAT_PRESERVE | GPU_SC_SPV_DENORM_PRESERVE);
    if (p12.shaderSignedZeroInfNanPreserveFloat32) opts |= GPU_SC_SPV_FLOAT_PRESERVE;
    if (p12.shaderDenormPreserveFloat32) opts |= GPU_SC_SPV_DENORM_PRESERVE;
    gpu_sc_spv_set_options(opts);
}

/* Step V6: the start's steps (rsxc_init.inc). */
static int start_step(const char* step, int ok);

/* The device, once the spv target is there to give it shaders. Step V6: the
 * start's steps it tells apart: shaders (no spv target), loader (no Vulkan
 * 1.3 library), and instance, adapter and features only when
 * RSX_FAIL_START_AT names them (the backend's own line says which failed,
 * and init_d3d12 calls any other failure to make the device "device"). */
static gpu_device* rc_be_create(const gpu_device_desc* dd)
{
    if (!gpu_sc_available(GPU_SC_SPV)) {
        start_step("shaders", 0);
        printf("[VK] this build has no SPIR-V compiler (PS3RECOMP_RSX_HAL_SPV): no Vulkan renderer\n");
        return NULL;
    }
    if (volkInitialize() != VK_SUCCESS || volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        start_step("loader", 0);
        printf("[VK] no Vulkan 1.3 library: no Vulkan renderer\n");
        return NULL;
    }
    if (!start_step("loader", 1) || !start_step("instance", 1) || !start_step("adapter", 1) ||
        !start_step("features", 1))
        return NULL;
    gpu_device* const d = gpu_vk_create(dd);
    if (d) rc_vk_spv_options(d);
    return d;
}

/* A window to present into. */
static const int rc_be_headless = 0;

/* Step V6: a frame buffer that cannot be made fails the start (init_d3d12),
 * and the route switch then starts today's renderer instead (rsx_out.c). */
static const int rc_be_start_strict = 1;

/* RSX_VK_BREADCRUMBS=1: the draws' markers as checkpoints (NVIDIA's
 * diagnostic checkpoints) and the device fault's report, which
 * gpu_fault_report prints once the device is lost. */
static int rc_be_breadcrumbs(void) { return getenv("RSX_VK_BREADCRUMBS") ? 1 : 0; }
static const char rc_be_lost_hint[] = " RSX_VK_BREADCRUMBS=1 on the next run says where the GPU was.";

/* Why the device was lost, from gpu_lost's reason (the VkResult). */
static const char* rc_be_lost_text(u32 reason)
{
    switch ((int32_t)reason) {
    case VK_ERROR_DEVICE_LOST: return "the GPU stopped responding or its driver was reset (VK_ERROR_DEVICE_LOST)";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "the GPU ran out of memory (VK_ERROR_OUT_OF_DEVICE_MEMORY)";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "the system ran out of memory (VK_ERROR_OUT_OF_HOST_MEMORY)";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "the window's surface was lost and could not be made again (VK_ERROR_SURFACE_LOST_KHR)";
    default: return "an unknown reason";
    }
}

/* Step VC2: the reason check_device's line and the host's report give when
 * the window's surface is lost for good (gpu.h's GPU_PRESENT_SURFACE_LOST):
 * the device works on, but nothing reaches the window again. */
static const u32 rc_be_surface_lost = (u32)VK_ERROR_SURFACE_LOST_KHR;

/* RSX_LOSE_DEVICE_AT_FRAME: Vulkan cannot remove its device on purpose, so
 * the backend notes it lost, as a driver reset would leave it (by hand,
 * step VC2: the fault report asks the driver nothing then). */
static void rc_be_lose_device(gpu_device* d, int frames)
{
    fprintf(stderr, "[VK] RSX_LOSE_DEVICE_AT_FRAME: noting the device lost at frame %d%c", frames, 10);
    gpu_vk_lose_by_hand(GPU_VK_DEV(d));
}

/* The menu's renderer (D3D12's, until it is ported) has nothing to draw with
 * here. */
static void* rc_be_device(gpu_device* d) { (void)d; return NULL; }
static void* rc_be_queue(gpu_device* d)  { (void)d; return NULL; }
static void* rc_be_image(gpu_device* d, gpu_texture t) { (void)d; (void)t; return NULL; }

/* None of today's cache files (rsxc_shadercache.inc; the file's comment
 * says why). The spv target keeps its own, in <cache>/gpu/spv/. */
static const int rc_be_cache_files = 0;

/* Step VC2: the pipeline library is the device's VkPipelineCache
 * (rsxc_psolib.inc, the backend's gpu_pipeline_cache_load/_save), in
 * <cache>/gpu/vulkan/<vendor>_<device>_<driver>_<uuid>.vkpc: one file per
 * driver and GPU, so another driver's is never offered to this one (the
 * file's own header and Vulkan's are checked as well). Loaded at the start,
 * saved where the d3d12 route saves pipelines.lib: after a burst of new
 * pipelines has settled, and at shutdown. No manifest and no startup build:
 * the cache alone. */
static const char rc_be_lib_dir[] = "vulkan";
static const char* rc_be_lib_file(gpu_device* d)
{
    static char name[96];
    if (!name[0]) {
        gpu_adapter_info ai;
        memset(&ai, 0, sizeof ai);
        gpu_adapter(d, &ai);
        int n = snprintf(name, sizeof name, "%04x_%04x_%08x_", ai.vendor, ai.device, (unsigned)ai.driver_version);
        for (int i = 0; i < 16 && n > 0 && n + 2 < (int)sizeof name; i++)
            n += snprintf(name + n, sizeof name - (size_t)n, "%02x", ai.uuid[i]);
        snprintf(name + n, sizeof name - (size_t)n, ".vkpc");
    }
    return name;
}

/* The shaders: the compile service's spv target. A compile (sc_compile)
 * places the text's resources where the layout puts them (NULL: the guest
 * programs'); a load (sc_load) is the program the cache holds under key K; a
 * put (rc_shader_cache_put) compiles a guest text into the cache. */
static const uint32_t rc_be_shader_code = GPU_CODE_SPIRV;
static int rc_be_sc_compile(gpu_sc_stage stage, const char* src, size_t n, const char* name, const char* entry,
                            unsigned flags, const gpu_layout_desc* layout, gpu_sc_blob* out, char* msg, size_t cap)
{
    return gpu_sc_compile_layout(GPU_SC_SPV, stage, src, n, name, entry, layout, flags, out, msg, cap) == GPU_SC_OK;
}
static int rc_be_sc_load(gpu_sc_stage stage, u64 key, gpu_sc_blob* out)
{
    return gpu_sc_load(GPU_SC_SPV, stage, key, out) == GPU_SC_OK;
}
static int rc_be_cache_put(gpu_sc_stage stage, const char* hlsl) { return gpu_sc_put(GPU_SC_SPV, stage, hlsl); }

/* No manifest (no pipeline library): no record, no name. */
typedef struct rc_be_pso_desc { u32 none; } rc_be_pso_desc;
static u64 rc_be_layout_hash(gpu_device* d, gpu_layout l) { (void)d; (void)l; return 0; }
static u64 rc_be_pso_name(gpu_device* d, const gpu_pipeline_desc* pd, u64 layout_hash)
{
    (void)d; (void)pd; (void)layout_hash;
    return 0;
}
static int rc_be_pso_record(gpu_device* d, const gpu_pipeline_desc* pd, u64 layout_hash, u64* name,
                            rc_be_pso_desc* rec, u32* nel, const void* code[2], u32 len[2])
{
    (void)d; (void)pd; (void)layout_hash; (void)name; (void)rec; (void)nel; (void)code; (void)len;
    return 0;
}
static int rc_be_pso_unrecord(const rc_be_pso_desc* rec, u32 nel, gpu_pipeline_desc* pd)
{
    (void)rec; (void)nel; (void)pd;
    return 0;
}

/* After each present: counted, for the frame slot. Step V6: the first ends
 * the start for the route switch, which takes its start mark away
 * (rsx_out.c; the route is Windows' only for now). */
static u32 s_rc_vk_presents, s_rc_vk_slot_presents;
#if defined(_WIN32)
void rsx_out_route_presented(void);   /* rsx_out.h */
#endif
static void rc_be_presented(gpu_device* d)
{
    (void)d;
#if defined(_WIN32)
    if (!s_rc_vk_presents) rsx_out_route_presented();
#endif
    s_rc_vk_presents++;
}

/* The frame slot after an acquire that gave image `image` (rc_acquire): the
 * next slot once a present has been made since the last acquire, else the
 * same (an unpresented image stays acquired, as D3D12's buffer index stays
 * when nothing is presented). FRAME_COUNT is the core's: defined below it. */
static u32 rc_be_frame_slot(u32 prev, u32 image);

#include "rsxc_core.inc"

static u32 rc_be_frame_slot(u32 prev, u32 image)
{
    (void)image;
    if (s_rc_vk_presents == s_rc_vk_slot_presents) return prev;
    s_rc_vk_slot_presents = s_rc_vk_presents;
    return (prev + 1u) % FRAME_COUNT;
}

#include "rsx_out.h"

/* The copy's exports, as the route switch calls them. */
#define RSXC_ROUTE_V_(name, legacy, params, args)      .name = rc_##name,
#define RSXC_ROUTE_R_(ret, name, legacy, params, args) .name = rc_##name,
const rsx_route rsxc_route_vk = {
    .name = "vulkan",
    RSX_OUT_FUNCS(RSXC_ROUTE_V_, RSXC_ROUTE_R_)
};
#undef RSXC_ROUTE_V_
#undef RSXC_ROUTE_R_

/* Step V6: the step the last failed start stopped at (rsxc_init.inc's
 * start_step), NULL after none. */
const char* rsxc_vk_start_failed(void) { return s_rc_start_failed; }

/* ---- step VM ---- */
/* The overlay v2's calls (rsx_out.h; rsxc_api.inc): the menu draws here on
 * the core's own device (the port's render_gpu.cpp), as rc_be_device and
 * the rest above have nothing for a D3D12 one. */
const rsx_route_gpu rsxc_route_gpu_vk = { rc_set_overlay_gpu, rc_gpu_device, rc_overlay_gpu_flush };

#endif /* PS3RECOMP_RSX_HAL && PS3RECOMP_RSX_HAL_VULKAN && (_WIN32 || __linux__) */
