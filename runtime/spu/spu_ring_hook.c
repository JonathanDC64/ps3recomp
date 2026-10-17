/*
 * ps3recomp - native block for a lock-line ring consumer's fetch step
 * (spu_lifter.py --native-block ADDR-END=SYMBOL)
 *
 * A native block runs in place of the lifted instructions from ADDR up to,
 * not including, END. It returns 1 when it did their work, with every
 * register, local-store byte and channel they write left as they leave
 * them, and the lifted code goes on at END; 0 runs the lifted instructions
 * as usual.
 *
 * Demon's Souls' HighGraphics ring consumer (SPURS image 2) takes each
 * 16-byte command from the ring HighGraphics fills like this, at LS 0x4618:
 *
 *     GETLLAR the ring's control line (EA in word 3 of the copy at 0x13480)
 *     if line.word0 (put) == state.word0 (get, LS 0x13400): wait for a
 *         lock-line event, GETLLAR again, compare again
 *     PUTLLC the line unchanged
 *     GET the 128 bytes holding the command at get, copy the command to
 *         0x13560, get += 16
 *     PUT the state back to memory (0xC358), dispatch on the command byte
 *
 * so every command costs a GETLLAR and an unchanged PUTLLC, each a trip
 * through the runtime's reservation machinery against the cache line
 * HighGraphics is writing. This block does the step from 0x4618 to the PUT
 * call at 0x4730 when the line already shows a command: the GETLLAR and the
 * unchanged PUTLLC become one plain read of the line
 * (spu_llar_noop_commit), and the GET and its tag wait go through the
 * channels as before. When the ring is empty it returns 0 and the lifted
 * code waits exactly as it always has. While the line copy from the last
 * read still shows commands ahead of get, the line is not read at all.
 *
 * SPU_NATIVE_RING (live): 1 (default) as above, 2 reads the line for every
 * command, 0 runs the lifted code, for A/B.
 */

#include <string.h>
#include "ps3emu/ps3_knob.h"
#include "spu_context.h"
#include "spu_helpers.h"

extern uint8_t* vm_base;
extern int g_spu_ch_diag;
extern unsigned g_spu_ch_diag_gen;
u128 spu_rdch(spu_context* ctx, uint32_t channel);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
int  spu_llar_noop_commit(spu_context* ctx, uint32_t lsa, uint32_t ea);

#define RING_LINE_LS  0x13480u   /* the control line's copy (GETLLAR target) */
#define RING_STATE_LS 0x13400u   /* word 0: get; word 2: where the state is PUT */
#define RING_CMD_LS   0x13560u   /* the command being dispatched */

static int native_ring_on(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("SPU_NATIVE_RING", 1));
    return on;
}

static inline uint32_t be32_at(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int spu_hook_ring_fetch(spu_context* ctx)
{
    if (!native_ring_on() || !vm_base) return 0;
    /* The channel latches below are written in place, as spu_wrch_latch
     * writes them; a channel diagnostic wants every write to go through
     * spu_wrch, so leave those runs to the lifted code. */
    if (g_spu_ch_diag_gen != ps3_env_gen || g_spu_ch_diag) return 0;
    u128* r = ctx->gpr;
    if (r[82]._u32[0] != RING_LINE_LS) return 0;   /* not the layout this was written for */

    const u128 old_line = spu_ls_read128(ctx, RING_LINE_LS);
    const u128 state = spu_ls_read128(ctx, RING_STATE_LS);
    const uint32_t line_ea = old_line._u32[3] & ~127u;
    if (line_ea < 0x10000u) return 0;
    /* While the copy from the last GETLLAR still shows a put ahead of get,
     * the commands up to it are published (the producer moves put only past
     * whole commands), so the line is not read again: the copy stays as it
     * is, as if this GETLLAR had come before the producer's later stores.
     * SPU_NATIVE_RING=2 reads it for every command. */
    const int known = native_ring_on() == 1 && old_line._u32[0] != state._u32[0];
    /* The live put. Empty ring: the lifted code GETLLARs and waits. */
    if (!known && be32_at(vm_base + line_ea) == state._u32[0]) return 0;

    /* 0x4618-0x4690: wrch ch69 (no channel), event mask, GETLLAR. */
    r[58] = spu_il(0);
    r[57] = spu_il(1024);
    ctx->event_mask = 1024;
    r[56] = old_line;
    r[55] = spu_rotqbyi(r[56], 8);
    r[54] = spu_shlqbyi(r[55], 4);
    r[52] = spu_il(208);
    r[53] = spu_ori(r[52], 0);
    /* The GETLLAR and the unchanged PUTLLC at 0x46BC, together. */
    if (known) {
        if (ctx->resv_valid) __atomic_store_n(&ctx->resv_valid, 0, __ATOMIC_RELEASE);
        ctx->atomic_stat = 0;
    } else if (!spu_llar_noop_commit(ctx, r[82]._u32[0], r[54]._u32[0])) return 0;
    r[5] = spu_il(0);
    r[6] = spu_ori(r[52], 0);
    /* 0x4694: the compare (not equal: the live put was read above, and the
     * copy is as new as that). */
    r[4] = spu_ls_read128(ctx, RING_LINE_LS);
    r[63] = state;
    r[61] = spu_ceq(r[4], r[63]);
    r[8] = spu_rotqbyi(r[4], 8);
    r[14] = spu_shlqbyi(r[8], 4);
    r[6] = spu_il(180);
    r[2] = spu_make_preferred_u32(ctx->atomic_stat);
    /* 0x46C4-0x4700: GET the command's 128 bytes, wait for tag 0. */
    r[76] = state;
    r[10] = spu_ori(r[85], 0);
    r[13] = spu_andi(r[76], -128);
    r[3] = spu_il(128);
    r[2] = spu_il(0);
    r[81] = spu_il(64);
    ctx->mfc_lsa = r[10]._u32[0];
    ctx->mfc_eal = r[13]._u32[0];
    ctx->mfc_size = 128;
    ctx->mfc_tag = 0;
    spu_wrch(ctx, MFC_Cmd, r[81]);
    r[79] = spu_il(1);
    ctx->mfc_tag_mask = 1;
    r[78] = spu_il(2);
    spu_wrch(ctx, MFC_WrTagUpdate, r[78]);
    r[2] = spu_make_preferred_u32(ctx->mfc_tag_status);
    /* 0x4704-0x472C: copy the command out, get += 16. */
    r[73] = spu_ls_read128(ctx, RING_STATE_LS);
    r[5] = spu_il(16);
    r[4] = spu_ila(RING_STATE_LS);
    r[75] = spu_andi(r[76], 127);
    r[74] = spu_cwd(r[1], 0);
    r[71] = spu_ls_read128(ctx, r[75]._u32[0] + r[85]._u32[0]);
    r[72] = spu_ai(r[73], 16);
    r[70] = spu_shufb(r[72], r[73], r[74]);
    spu_ls_write128(ctx, RING_CMD_LS, r[71]);
    r[3] = spu_rotqbyi(r[70], 8);
    spu_ls_write128(ctx, RING_STATE_LS, r[70]);
    return 1;
}
