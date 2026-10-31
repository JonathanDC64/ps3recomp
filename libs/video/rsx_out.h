/*
 * ps3recomp - the renderer's front: the route switch (PS3RECOMP_RSX_HAL)
 *
 * With the portable renderer built (PS3RECOMP_RSX_HAL), the host calls the
 * renderer through these rsx_out_* functions instead of rsx_d3d12_*. Each
 * one goes to the route picked once, from RSX_BACKEND, on the first call:
 *   d3d12      today's renderer (rsx_d3d12_backend.c); the default. Each
 *              rsx_out_X calls rsx_d3d12_X directly.
 *   hal-d3d12  the new path on its own D3D12 backend (libs/video/rsxcore/,
 *              Windows only), for developers' A/Bs. Step 1.0 is today's
 *              renderer copied as it is, so it draws the same frames.
 *   null       the new path on the null backend (gpu_null.c, Windows only
 *              for now): no window and no GPU; every command is checked
 *              against gpu.h and hashed. For developers and CI only, never
 *              in Settings (rsxc_null.c says what it prints).
 * Anything else (vulkan, metal, auto, a typo) is not in this build yet and
 * runs d3d12. The pick prints one line, on every route:
 *   [rsx_out] route d3d12
 *   [rsx_out] route hal-d3d12 (the new path on its own D3D12 backend)
 *   [rsx_out] route null (the new path on the null backend: checks every command, draws nothing)
 *   [rsx_out] route d3d12 (RSX_BACKEND=vulkan is not in this build)
 *
 * The functions are rsx_d3d12_backend.h's, with the same parameters and
 * meaning (see there), plus the profile sessions (rsx_prof_*, boot_main's
 * F11) and rsx_d3d12_composite_from_offscreen, which the original also
 * exports. Plain C with <stdint.h> types only, so C and C++ hosts can include
 * it without the runtime's headers. Built without PS3RECOMP_RSX_HAL, nothing
 * uses this file and rsx_out.c is empty.
 */
#ifndef PS3RECOMP_RSX_OUT_H
#define PS3RECOMP_RSX_OUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rsx_d3d12_overlay;   /* rsx_d3d12_backend.h (the port's des_ui.cpp has its own copy) */

/* One line per function, as V(name, legacy, params, args) for one that
 * returns nothing and R(ret, name, legacy, params, args) for one that
 * returns ret. legacy is today's renderer's name for it; the copy in
 * rsxcore names it rc_<name>. */
#define RSX_OUT_FUNCS(V, R) \
    R(int,         backend_init,          rsx_d3d12_backend_init,          (uint32_t width, uint32_t height, const char* title), (width, height, title)) \
    V(             backend_shutdown,      rsx_d3d12_backend_shutdown,      (void), ()) \
    R(int,         backend_pump_messages, rsx_d3d12_backend_pump_messages, (void), ()) \
    V(             backend_present,       rsx_d3d12_backend_present,       (void), ()) \
    V(             set_overlay,           rsx_d3d12_set_overlay,           (const struct rsx_d3d12_overlay* overlay), (overlay)) \
    V(             overlay_repaint,       rsx_d3d12_overlay_repaint,       (int on), (on)) \
    V(             overlay_wake,          rsx_d3d12_overlay_wake,          (void), ()) \
    V(             overlay_tick,          rsx_d3d12_overlay_tick,          (void), ()) \
    R(void*,       device,                rsx_d3d12_device,                (void), ()) \
    R(long,        device_lost,           rsx_d3d12_device_lost,           (long long* qpc), (qpc)) \
    R(void*,       queue,                 rsx_d3d12_queue,                 (void), ()) \
    R(void*,       hwnd,                  rsx_d3d12_hwnd,                  (void), ()) \
    V(             post_message,          rsx_d3d12_post_message,          (unsigned msg, unsigned long long wp, long long lp), (msg, wp, lp)) \
    R(void*,       back_buffer,           rsx_d3d12_back_buffer,           (unsigned index), (index)) \
    R(unsigned,    back_buffer_count,     rsx_d3d12_back_buffer_count,     (void), ()) \
    R(unsigned,    back_buffer_format,    rsx_d3d12_back_buffer_format,    (void), ()) \
    R(int,         hdr,                   rsx_d3d12_hdr,                   (float* ui_scale, int* srgb), (ui_scale, srgb)) \
    V(             set_borderless,        rsx_d3d12_set_borderless,        (int on), (on)) \
    R(int,         borderless,            rsx_d3d12_borderless,            (void), ()) \
    R(long long,   display_timing,        rsx_d3d12_display_timing,        (long long* vblank), (vblank)) \
    R(int,         display_refresh_mhz,   rsx_d3d12_display_refresh_mhz,   (void), ()) \
    R(int,         display_mode_mhz,      rsx_d3d12_display_mode_mhz,      (void), ()) \
    R(int,         display_shown_mhz,     rsx_d3d12_display_shown_mhz,     (void), ()) \
    R(int,         shader_hlsl,           rsx_d3d12_shader_hlsl,           (int fragment, const uint8_t* ucode, uint32_t ucode_len, uint32_t vp_start_slot, \
                                                                            int exp32, uint32_t cube_mask, uint32_t d2c_mask, char* out, uint32_t cap), \
                                                                           (fragment, ucode, ucode_len, vp_start_slot, exp32, cube_mask, d2c_mask, out, cap)) \
    R(int,         shader_hdr,            rsx_d3d12_shader_hdr,            (char* hlsl, uint32_t cap, uint32_t hdrv), (hlsl, cap, hdrv)) \
    R(int,         shader_cache_put,      rsx_d3d12_shader_cache_put,      (int fragment, const char* hlsl), (fragment, hlsl)) \
    R(const char*, shader_cache_path,     rsx_d3d12_shader_cache_path,     (void), ()) \
    R(int,         shader_fp_variants,    rsx_d3d12_shader_fp_variants,    (const uint8_t* ucode, uint32_t ucode_len, uint32_t* out, int max), (ucode, ucode_len, out, max)) \
    V(             pso_index_game,        rsx_d3d12_pso_index_game,        (const char* game), (game)) \
    R(uint64_t,    program_hash64,        rsx_d3d12_program_hash64,        (int fragment, const uint8_t* ucode, uint32_t ucode_len), (fragment, ucode, ucode_len)) \
    R(uint64_t,    shader_key,            rsx_d3d12_shader_key,            (int fragment, const char* hlsl), (fragment, hlsl)) \
    V(             pso_index_add,         rsx_d3d12_pso_index_add,         (int vs_fetch, uint64_t vs_key, int clamp, uint64_t ps_key, uint32_t blend, int nrt, \
                                                                            uint32_t rtfmt, uint32_t cmask, uint32_t cull, uint32_t depth), \
                                                                           (vs_fetch, vs_key, clamp, ps_key, blend, nrt, rtfmt, cmask, cull, depth)) \
    V(             pso_index_ready,       rsx_d3d12_pso_index_ready,       (uint64_t hash), (hash)) \
    V(             composite_from_offscreen, rsx_d3d12_composite_from_offscreen, (uint32_t raw_off), (raw_off)) \
    V(             prof_begin,            rsx_prof_begin,                  (const char* prefix), (prefix)) \
    R(int,         prof_end,              rsx_prof_end,                    (char* digest, size_t cap), (digest, cap)) \
    V(             prof_title,            rsx_prof_title,                  (const char* note), (note))

/* rsx_out_<name> for each. */
#define RSX_OUT_DECL_V_(name, legacy, params, args)      void rsx_out_##name params;
#define RSX_OUT_DECL_R_(ret, name, legacy, params, args) ret rsx_out_##name params;
RSX_OUT_FUNCS(RSX_OUT_DECL_V_, RSX_OUT_DECL_R_)
#undef RSX_OUT_DECL_V_
#undef RSX_OUT_DECL_R_

/* The route that runs: "d3d12", "hal-d3d12" or "null". Picks it if no call
 * has. */
const char* rsx_out_renderer(void);

/* A renderer other than today's, as a table of the functions above. The
 * new path's core defines one per backend it is compiled for:
 * rsxc_route_d3d12 (libs/video/rsxcore/rsxc_d3d12.c, Windows) and
 * rsxc_route_null (rsxc_null.c, Windows for now). */
#define RSX_OUT_FIELD_V_(name, legacy, params, args)      void (*name) params;
#define RSX_OUT_FIELD_R_(ret, name, legacy, params, args) ret (*name) params;
typedef struct rsx_route {
    const char* name;   /* as RSX_BACKEND spells it */
    RSX_OUT_FUNCS(RSX_OUT_FIELD_V_, RSX_OUT_FIELD_R_)
} rsx_route;
#undef RSX_OUT_FIELD_V_
#undef RSX_OUT_FIELD_R_

#if defined(_WIN32)
extern const rsx_route rsxc_route_d3d12;
extern const rsx_route rsxc_route_null;
#endif

#ifdef __cplusplus
}
#endif
#endif
