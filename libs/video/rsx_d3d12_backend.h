/*
 * ps3recomp - D3D12 RSX Backend
 *
 * Translates RSX GPU commands to Direct3D 12 rendering.
 * Implements the rsx_backend callback interface.
 *
 * Architecture:
 *   rsx_commands.c (state tracking) → rsx_backend callbacks → this file
 *                                                               ↓
 *                                                         D3D12 device
 *                                                         swap chain
 *                                                         command lists
 *                                                         pipeline states
 *
 * Current scope (Phase 1):
 *   - Device/swap chain creation
 *   - Clear to RSX clear color
 *   - Present with vsync
 *   - Basic triangle rendering (vertex position + color)
 *
 * Future phases:
 *   - Texture upload and sampling
 *   - RSX vertex/fragment program → HLSL translation
 *   - Framebuffer resolve and display buffer management
 *   - Depth/stencil buffer creation
 */

#ifndef PS3RECOMP_RSX_D3D12_BACKEND_H
#define PS3RECOMP_RSX_D3D12_BACKEND_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the D3D12 backend.
 * Creates a Win32 window, D3D12 device, swap chain, and registers
 * as the active rsx_backend.
 * Returns 0 on success, -1 on failure. */
int rsx_d3d12_backend_init(u32 width, u32 height, const char* title);

/* Shut down the D3D12 backend and release all resources. */
void rsx_d3d12_backend_shutdown(void);

/* Process Win32 messages. Returns 0 normally, -1 if window closed. */
int rsx_d3d12_backend_pump_messages(void);

/* Force a present (useful for debugging). */
void rsx_d3d12_backend_present(void);

/* Shader disk cache (PS3_SHADER_CACHE, default shader_cache beside the exe),
 * for filling it before a title runs (tools: rsx_shader_precompile).
 * rsx_d3d12_shader_hlsl builds the HLSL a draw would compile for one RSX
 * program (fragment: exp32 = SET_SHADER_CONTROL 32-bit exports, cube/d2c =
 * per-unit masks; vertex: vp_start_slot = the slot it is loaded at) and
 * returns its instruction count, <= 0 on failure. Not thread-safe.
 * rsx_d3d12_shader_cache_put compiles that text into the cache: 1 = already
 * there, 2 = compiled, 0 = failed. Thread-safe. */
int rsx_d3d12_shader_hlsl(int fragment, const u8* ucode, u32 ucode_len, u32 vp_start_slot,
                          int exp32, u32 cube_mask, u32 d2c_mask, char* out, u32 cap);
int rsx_d3d12_shader_cache_put(int fragment, const char* hlsl);
const char* rsx_d3d12_shader_cache_path(void);
/* Variants (exp32, cube mask, d2c mask: 3 words each) that earlier runs
 * compiled this fragment program with, from the cache's fp_variants.txt.
 * Returns how many were written to out (at most max). */
int rsx_d3d12_shader_fp_variants(const u8* ucode, u32 ucode_len, u32* out, int max);

#ifdef __cplusplus
}
#endif
#endif
