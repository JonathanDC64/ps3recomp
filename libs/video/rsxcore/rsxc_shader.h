/*
 * ps3recomp - the RSX shader texts, for the portable renderer (rsxcore)
 *
 * The HLSL today's D3D12 renderer compiles for a guest program, built
 * without any graphics API (rsxc_shader.c, copied from rsx_d3d12_backend.c).
 * Same program and draw state, same text, so the same shader cache key K:
 * the new path's D3D12 backend shares today's .dxbc files.
 *
 * Built only with PS3RECOMP_RSX_HAL.
 */
#ifndef PS3RECOMP_RSXC_SHADER_H
#define PS3RECOMP_RSXC_SHADER_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rsx_state;

/* Today's offline exports, renamed (rsx_d3d12_backend.h has the contracts):
 *  rsxc_shader_hlsl   rsx_d3d12_shader_hlsl: kind 0 vertex, 1 fragment,
 *                     2 vertex in fetch form, 3 fragment with the sprite-edge
 *                     clamp. Returns the instruction count, <= 0 on failure.
 *                     Not thread-safe (the decompilers' statics).
 *  rsxc_shader_hdr    rsx_d3d12_shader_hdr: the HDR variant hdrv, in place.
 *  rsxc_shader_key    rsx_d3d12_shader_key: the .dxbc cache key of a text.
 *  rsxc_program_hash64  rsx_d3d12_program_hash64: the pipeline index's
 *                     fingerprint of a program.
 *  rsxc_shader_fp_variants  rsx_d3d12_shader_fp_variants: the cache's
 *                     fp_variants.txt entries for a fragment program. */
int rsxc_shader_hlsl(int fragment, const u8* ucode, u32 ucode_len, u32 vp_start_slot,
                     int exp32, u32 cube_mask, u32 d2c_mask, char* out, u32 cap);
int rsxc_shader_hdr(char* hlsl, u32 cap, u32 hdrv);
u64 rsxc_shader_key(int fragment, const char* hlsl);
u64 rsxc_program_hash64(int fragment, const u8* ucode, u32 ucode_len);
int rsxc_shader_fp_variants(const u8* ucode, u32 ucode_len, u32* out, int max);

/* The draw path's pieces.
 *  rsxc_fp_build_hlsl   fp_build_hlsl: a fragment program's text, its
 *                       inline constants from b1 (ucode readable for
 *                       RSX_FP_MAX_BYTES); note = 1 records the variant in
 *                       fp_variants.txt. Returns the instruction count.
 *  rsxc_fp_baked        1 if the last one had more constants than b1 holds
 *                       (baked into its text).
 *  rsxc_fp_inject_uvclamp  the pixel half of the sprite-edge clamp; 0 leaves
 *                       the text as it was.
 *  rsxc_uvclamp_gs_hlsl the geometry half's source (gs_5_0, entry "main").
 *  rsxc_fp_out_anchor / rsxc_fp_set_out  colour 0's expression, for the
 *                       debug rewrites.
 *  rsxc_fp_hash_at / rsxc_fp_units_at  rsx_fp_code_hash, size and sampled
 *                       units of the program at a guest offset, memoised.
 *  rsxc_fp_seen         the guest ranges rsxc_fp_hash_at has seen (for the
 *                       write-back's RSX_WRITE_COLOR_LOG): entry i into
 *                       *ea and *len; returns how many there are. */
int rsxc_fp_build_hlsl(const u8* ucode, int exp32, u32 cube_mask, u32 d2c_mask,
                       char* hlsl, size_t cap, u32 fp_addr, int note);
int rsxc_fp_baked(void);
int rsxc_fp_inject_uvclamp(char* hlsl, size_t cap);
const char* rsxc_uvclamp_gs_hlsl(size_t* len);
char* rsxc_fp_out_anchor(char* hlsl, char** end, int want_stmt);
int rsxc_fp_set_out(char* hlsl, size_t cap, const char* expr);
u32 rsxc_fp_hash_at(u32 off, u32* size_out);
u32 rsxc_fp_units_at(u32 off);
int rsxc_fp_seen(int i, u32* ea, u32* len);

/* A vertex program as the state holds it at its transform program start. */
typedef struct rsxc_vs_program {
    const u8* code;   /* in the state's store, from the start slot */
    u32 vstart;       /* its byte offset there */
    u32 vlen;         /* its bytes, to its END instruction (all that follow without one) */
    int end;          /* 1 if it has an END instruction */
    u32 hash;         /* the VS cache's key: FNV-1a 32 of the bytes, salted for fetch form */
} rsxc_vs_program;

/* rsxc_vs_locate: find and key the state's program (0, -1 if there is none).
 * rsxc_vs_text: its text, as vp_get_vs_mode compiles it (today's buffer is
 * 262144 bytes); *uses_c03 = 1 if it reads vp_c[0..3]. Returns the
 * instruction count, <= 0 on failure.
 * rsxc_vs_code64: FNV-1a 64 of its bytes (the pipeline index's fingerprint). */
int rsxc_vs_locate(const struct rsx_state* st, int fetch, rsxc_vs_program* vp);
int rsxc_vs_text(const rsxc_vs_program* vp, int fetch, char* hlsl, u32 cap, int* uses_c03);
u64 rsxc_vs_code64(const rsxc_vs_program* vp);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSXC_SHADER_H */
