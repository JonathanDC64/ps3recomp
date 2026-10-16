/*
 * ps3recomp - native hooks for SPU zlib inflate wrappers (spu_lifter.py
 * --native-hook ADDR=SYMBOL)
 *
 * A hook runs at the entry of the lifted function it replaces. It returns 1
 * when it did that function's work, and the lifted code returns to the caller
 * at once; 0 runs the lifted function as usual. A hook leaves the callee-saved
 * registers (r80-r127) and r1 alone and sets the result in r3.
 *
 * Every hook here does only the case it can do exactly and hands anything
 * else (a bad stream, an output size that does not match) back to the lifted
 * code, so the title's own error handling and messages still apply.
 *
 * SPU_NATIVE_INFLATE=0 (live) runs the lifted code instead, for A/B.
 */

#include <string.h>
#include "ps3emu/ps3_knob.h"
#include "spu_context.h"
#include "spu_inflate.h"

static int native_inflate_on(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("SPU_NATIVE_INFLATE", 1));
    return on;
}

/* int inflate_ls(void* dst, uint32_t dst_len, const void* src, uint32_t src_len)
 *
 * Raw-inflates (inflateInit2 with -15 window bits, one inflate(Z_FINISH)) the
 * local-store buffer src into dst; returns 0 when the output came to exactly
 * dst_len bytes. Demon's Souls's resource decompressor task (Edge zlib, each
 * 64 KB block of a DCX file) has this wrapper at 0x8700. */
int spu_hook_inflate_ls(spu_context* ctx)
{
    if (!native_inflate_on()) return 0;
    const uint32_t dst = ctx->gpr[3]._u32[0], dst_len = ctx->gpr[4]._u32[0];
    const uint32_t src = ctx->gpr[5]._u32[0], src_len = ctx->gpr[6]._u32[0];
    if (dst >= SPU_LS_SIZE || dst_len > SPU_LS_SIZE - dst ||
        src >= SPU_LS_SIZE || src_len > SPU_LS_SIZE - src)
        return 0;
    if (dst < src + src_len && src < dst + dst_len)   /* overlap: leave it to the SPU */
        return 0;
    uint32_t got;
    if (ps3_inflate_raw(ctx->ls + dst, dst_len, ctx->ls + src, src_len, &got) != 0 ||
        got != dst_len)
        return 0;
    memset(&ctx->gpr[3], 0, sizeof ctx->gpr[3]);
    return 1;
}
