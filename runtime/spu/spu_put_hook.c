/*
 * ps3recomp - native hook for a "PUT and wait" helper (spu_lifter.py
 * --native-hook ADDR=SYMBOL)
 *
 * A hook runs at the entry of the lifted function it replaces. It returns 1
 * when it did that function's work, and the lifted code returns to the caller
 * at once; 0 runs the lifted function as usual. A hook leaves the callee-saved
 * registers (r80-r127) and r1 alone and sets the result in r3.
 *
 * Demon's Souls' HighGraphics ring consumer (SPURS image 2) calls this one at
 * LS 0xC358 after every 16-byte ring command, to PUT its state back:
 *
 *     put_wait(r3 = 64-bit EA, r4 = LSA, r5 = size)
 *         size 1/2/4/8: one PUT, after a naturally-aligned check
 *         size 16..16384, LSA, EA and size 16-byte aligned: one PUT
 *         larger: a chunked PUT (a call)
 *         anything else: halt (an assert)
 *         then tag 0, wait for it (WrTagUpdate ANY), return the tag status
 *
 * Lifted, its prologue works out every case's checks in SPU registers kept in
 * memory before it branches, ~11% of the thread. This does the middle case,
 * the one the ring takes, with the same channel accesses in the same order;
 * any other case runs the lifted code.
 *
 * SPU_NATIVE_PUT (live): 1 (default) as above, 0 runs the lifted code.
 */

#include "ps3emu/ps3_knob.h"
#include "spu_context.h"
#include "spu_helpers.h"

extern int g_spu_ch_diag;
extern unsigned g_spu_ch_diag_gen;
void spu_wrch_word(spu_context* ctx, uint32_t channel, uint32_t value);   /* spu_wrch, an il value */
void spu_wrch_xfer_wait(spu_context* ctx, uint32_t cmd, uint32_t mode);    /* the PUT and its tag wait */

static int native_put_on(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("SPU_NATIVE_PUT", 1));
    return on;
}

/* put_wait with the PUT itself left to the caller when `defer` is set: the
 * prologue's stores, the channel latches and the tag wait are as the PUT
 * leaves them (the transfer is synchronous, so tag 0 reads complete either
 * way), and the caller issues the PUT later with spu_put_flush. */
static int put_wait(spu_context* ctx, int defer);

int spu_hook_put_wait(spu_context* ctx) { return put_wait(ctx, 0); }
int spu_hook_put_wait_defer(spu_context* ctx) { return put_wait(ctx, 1); }

/* The PUT a deferred put_wait left out, of the local-store bytes as they are
 * now; the channel latches are kept as the program left them. */
void spu_put_flush(spu_context* ctx, uint32_t eah, uint32_t eal, uint32_t lsa, uint32_t size)
{
    const uint32_t l = ctx->mfc_lsa, h = ctx->mfc_eah, e = ctx->mfc_eal, s = ctx->mfc_size, t = ctx->mfc_tag;
    ctx->mfc_lsa = lsa;
    ctx->mfc_eah = eah;
    ctx->mfc_eal = eal;
    ctx->mfc_size = size;
    ctx->mfc_tag = 0;
    spu_wrch_word(ctx, MFC_Cmd, 32);             /* PUT */
    ctx->mfc_lsa = l; ctx->mfc_eah = h; ctx->mfc_eal = e; ctx->mfc_size = s; ctx->mfc_tag = t;
}

static int put_wait(spu_context* ctx, int defer)
{
    if (!native_put_on()) return 0;
    /* The latches are written in place, as spu_wrch_latch writes them; a
     * channel diagnostic wants every access to go through the channel calls. */
    if (g_spu_ch_diag_gen != ps3_env_gen || g_spu_ch_diag) return 0;
    u128* r = ctx->gpr;
    const uint32_t eah = r[3]._u32[0], eal = r[3]._u32[1];
    const uint32_t lsa = r[4]._u32[0], size = r[5]._u32[0];
    if (size < 16 || size > 16384 || ((lsa | eal | size) & 15)) return 0;
    /* The prologue's stores: the link register into the caller's frame and
     * the back chain below it. */
    const uint32_t sp = r[1]._u32[0];
    spu_ls_write128(ctx, sp + 0x10, r[0]);
    spu_ls_write128(ctx, sp - 0x20, r[1]);
    ctx->mfc_lsa = lsa;
    ctx->mfc_eah = eah;
    ctx->mfc_eal = eal;
    ctx->mfc_size = size;
    ctx->mfc_tag = 0;
    ctx->mfc_tag_mask = 1;
    /* SPU_XFER_WAIT (live, as spu_ring_hook.c): 0 makes the two calls. */
    static int one = -1;
    PS3_KNOB(one, ps3_env_int("SPU_XFER_WAIT", 1));
    if (one) {
        spu_wrch_xfer_wait(ctx, defer ? 0u : 32u, 1);   /* PUT, then ANY: waits for tag 0 */
    } else {
        if (!defer) spu_wrch_word(ctx, MFC_Cmd, 32);    /* PUT */
        spu_wrch_word(ctx, MFC_WrTagUpdate, 1);         /* ANY: waits for tag 0 */
    }
    r[3] = spu_make_preferred_u32(ctx->mfc_tag_status);
    return 1;
}
