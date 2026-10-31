/*
 * ps3recomp - the portable renderer's core, compiled for the null backend
 *
 * The route RSX_BACKEND=null (libs/video/rsx_out.c), for developers and CI
 * only, never in Settings: the same fragments as rsxc_d3d12.c, after the
 * null backend's static inline command header (gpu_null_cmd.h), so the core
 * records and the null backend checks every command against the contract
 * (gpu.h, gpu_cmd_ops.h) and hashes the stream, drawing nothing. It includes
 * no D3D12, DXGI or shader compiler header: compiling it proves the
 * fragments name no backend (step C8b).
 *
 * What differs from the D3D12 instance, all in the rc_be_* hooks below:
 *   - headless: no window. The swap chain is the null backend's, with no
 *     window; the display's HDR is none (rsxc_display.h with no window);
 *   - no shader compiler: a shader is named by its text's key K
 *     (gpu_sc_key, FNV-1a 64 over the profile, 0xFF and the text: the .dxbc
 *     cache's key) and, for an entry point other than main, its name, in
 *     place of compiled code (GPU_CODE_KEY). The null backend names
 *     pipelines by their shaders' code, so the stream hash does not depend
 *     on FXC's version;
 *   - none of today's cache files: no shader cache directory, so no .dxbc,
 *     fp_variants.txt, pso_index_seen.tsv, pso_manifest.bin or pipeline
 *     library is read or written (the null backend has no pipeline cache);
 *   - after each present, one line: the present's number, the stream's hash
 *     so far and the failed checks so far (gpu_null.h). GPU_NULL_DUMP=<file>
 *     writes the stream as text (flushed at each present);
 *   - the menu gets no native objects (its renderer is D3D12's until V4).
 *
 * Windows only for now, as the D3D12 instance: the core still calls Win32
 * (threads, locks, the window, DWM); the platform module (Phase 5) takes
 * those.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(_WIN32)

#include "rsxc_head.inc"
/* The GPU layer, with the null backend's static inline commands, and its own
 * calls (gpu_null.h). */
#include "gpu_null_cmd.h"
#include "gpu_shader.h"

/* ---------------------------------------------------------------------------
 * The backend's hooks (rsxc_d3d12.c says what each is for)
 * -------------------------------------------------------------------------*/

static gpu_device* rc_be_create(const gpu_device_desc* dd) { return gpu_null_create(dd); }

/* No window: the swap chain's images are presented to nothing. */
static const int rc_be_headless = 1;

/* No breadcrumbs (the null backend keeps none). */
static int rc_be_breadcrumbs(void) { return 0; }
static const char rc_be_lost_hint[] = "";

/* The only loss is the test hook's (rc_be_lose_device). */
static const char* rc_be_lost_text(u32 reason)
{
    return reason == 0x887A0005u ? "the device was lost on purpose (gpu_null_test_lose)" : "an unknown reason";
}
static void rc_be_lose_device(gpu_device* d, int frames)
{
    fprintf(stderr, "[null] RSX_LOSE_DEVICE_AT_FRAME: losing the device at frame %d%c", frames, 10);
    gpu_null_test_lose(d, 0x887A0005u);   /* (the number a removed D3D12 device gives) */
}

/* The menu's renderer has nothing to draw with here. */
static void* rc_be_device(gpu_device* d) { (void)d; return NULL; }
static void* rc_be_queue(gpu_device* d)  { (void)d; return NULL; }
static void* rc_be_image(gpu_device* d, gpu_texture t) { (void)d; (void)t; return NULL; }

/* No shader cache: none of its files (rsxc_shadercache.inc). */
static const int rc_be_cache_files = 0;
static const char rc_be_lib_dir[] = "null";   /* (never used: no pipeline library) */
static int rc_be_cache_put(gpu_sc_stage stage, const char* hlsl) { (void)stage; (void)hlsl; return 0; }

/* A shader's "code": K as 8 bytes, little-endian, then the entry point's
 * name when it is not main. One malloc, which gpu_sc_blob_release frees. */
static const uint32_t rc_be_shader_code = GPU_CODE_KEY;
static const char* const k_rc_null_profile[GPU_SC_STAGE_COUNT] = { "vs_5_0", "ps_5_0", "gs_5_0" };
static int rc_be_key_blob(u64 key, const char* entry, gpu_sc_blob* out)
{
    const size_t ne = entry && strcmp(entry, "main") ? strlen(entry) : 0;
    u8* const p = (u8*)malloc(8 + ne);
    if (!p) return 0;
    for (int i = 0; i < 8; i++) p[i] = (u8)(key >> (8 * i));
    if (ne) memcpy(p + 8, entry, ne);
    out->data = p;
    out->size = 8 + ne;
    out->impl = p;
    return 1;
}
static int rc_be_sc_compile(gpu_sc_stage stage, const char* src, size_t n, const char* name, const char* entry,
                            unsigned flags, gpu_sc_blob* out, char* msg, size_t cap)
{
    (void)name; (void)flags;
    if (msg && cap) msg[0] = 0;
    if ((unsigned)stage >= GPU_SC_STAGE_COUNT || !src) return 0;
    return rc_be_key_blob(gpu_sc_key(src, n, k_rc_null_profile[stage]), entry, out);
}
static int rc_be_sc_load(gpu_sc_stage stage, u64 key, gpu_sc_blob* out)
{
    (void)stage;
    return rc_be_key_blob(key, NULL, out);
}

/* No manifest (no pipeline cache): no record, no name. */
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

/* After each present: what the null backend has seen so far, on stdout.
 *   [null] present 3: stream 0123456789abcdef, 0 failed checks
 * The hash covers everything up to and with this present. */
static u32 s_rc_null_presents;
static void rc_be_presented(gpu_device* d)
{
    s_rc_null_presents++;
    printf("[null] present %u: stream %016llx, %u failed checks\n", s_rc_null_presents,
           (unsigned long long)gpu_null_stream_hash(d), gpu_null_errors(d, GPU_NULL_ERR_ALL));
}

#include "rsxc_core.inc"

#include "rsx_out.h"

/* The copy's exports, as the route switch calls them. */
#define RSXC_ROUTE_V_(name, legacy, params, args)      .name = rc_##name,
#define RSXC_ROUTE_R_(ret, name, legacy, params, args) .name = rc_##name,
const rsx_route rsxc_route_null = {
    .name = "null",
    RSX_OUT_FUNCS(RSXC_ROUTE_V_, RSXC_ROUTE_R_)
};
#undef RSXC_ROUTE_V_
#undef RSXC_ROUTE_R_

#endif /* PS3RECOMP_RSX_HAL && _WIN32 */
