/*
 * ps3recomp - a native block for a table compare-and-copy loop
 * (spu_lifter.py --native-block ADDR-END=SYMBOL; see spu_ring_hook.c for the
 * contract: return 1 having done the work of the instructions from ADDR up to
 * END, with every register and local-store byte as they leave them, or 0 to
 * run them lifted).
 *
 * Demon's Souls' HighGraphics ring consumer (SPURS image 2), LS 0x8E18-0x8F60,
 * in region 0x8CB8, which ring commands 31-34 reach through 0x5400. The loop
 * walks two pairs of tables down from their ends, 64 bytes a pass, 32 passes:
 * the copy at r22 (A) and the new values at r67 (B), each as a 2 KB half at
 * +0 and another at +0x800. A pass loads the four quadwords of each half of
 * A and B, copies B's eight over A's, and shifts each of two masks (r56 for
 * the +0 halves, r57 for the +0x800 ones) right by 4 bits, setting from the
 * constants in r24-r27 a bit for each of its four quadwords that changed.
 * Lifted, a pass is 77 statements through ctx->gpr; a block-count profile of
 * the deep Palace put the loop at about 13% of the image's lifted
 * statements, the largest single block.
 *
 * This block runs every pass but the last with plain 16-byte compares and
 * copies, keeping only what the next pass reads (A, B, the counter r23 and
 * the two masks), then runs the last pass and the four instructions after
 * the loop as the lifted code does, register by register. So every register
 * leaves exactly as lifted, whatever the loop computed on the way.
 *
 * It runs the lifted loop instead (returns 0) whenever the lifted loop would
 * leave its region at the next back edge (the knobs and the pending-interrupt
 * test of SPU_RGOTO), a local-store probe is on, or the counter is not below
 * 32. An event that arrives during the 32 passes is taken at the region's
 * next exit test instead of at one of the loop's back edges, a few
 * microseconds later, as if it had arrived then.
 *
 * SPU_NATIVE_DIFF (live): 1 (default) as above; 0 runs the lifted loop, for
 * A/B; 3 runs the block's passes through byte copies (diff_pass_fast), as
 * before the SSE passes (diff_pass_sse); 2 checks the SSE passes: every
 * SPU_NATIVE_DIFF_EVERY-th time (default 64) the loop also runs in the SPU
 * interpreter on a copy of the context and its local store, and the block's
 * registers and local store are compared with the copy's ([spu-diff] lines).
 * A mismatch turns the block off for the session.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/ps3_knob.h"
#include "ps3emu/ps3_sse.h"   /* PS3_SSE2NEON: diff_pass_sse on arm64 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_interp.h"

#define DIFF_START 0x8E18u
#define DIFF_END   0x8F60u
#define DIFF_PASSES 32u

extern volatile int   g_spu_goto_slow;
extern volatile int   g_yz_lockstep_on;
extern void* volatile g_pm_flow_ctx;

static volatile int s_diff_bad;   /* a check mismatch: off for the session */

static int native_diff_mode(void)
{
    static int mode = 1;
    PS3_KNOB(mode, ps3_env_int("SPU_NATIVE_DIFF", 1));
    return mode;
}

static unsigned native_diff_every(void)
{
    static int n = 64;
    PS3_KNOB(n, ps3_env_int("SPU_NATIVE_DIFF_EVERY", 64));
    return n > 0 ? (unsigned)n : 1u;
}

/* The constant from r24-r27 for a quadword that changed, else 0: the lifted
 * andc(c, ceqi(gb(ceq(a, b)), 15)), whose flag is all ones in word 0 only
 * when the four words are equal. */
static inline u128 diff_bit(u128 c, const uint8_t* a, const uint8_t* b)
{
    if (memcmp(a, b, 16) == 0) c._u32[0] = 0;
    return c;
}

/* One pass but the last: the copies, the masks, A, B and the counter. */
static inline void diff_pass_fast(spu_context* ctx, u128* A, u128* B, u128* m56, u128* m57,
                                  const u128* r24, const u128* r25, const u128* r26, const u128* r27)
{
    static const uint32_t off[8] = { 0x0, 0x10, 0x20, 0x30, 0x800, 0x810, 0x820, 0x830 };
    uint8_t a[8][16], b[8][16];
    const uint32_t ab = A->_u32[0] - 64u, bb = B->_u32[0] - 64u;
    for (int k = 0; k < 8; k++) {   /* every load of the pass comes before its stores, as lifted */
        memcpy(a[k], ctx->ls + ((ab + off[k]) & SPU_LS_MASK & ~0xFu), 16);
        memcpy(b[k], ctx->ls + ((bb + off[k]) & SPU_LS_MASK & ~0xFu), 16);
    }
    /* r56 from the +0 half, r57 from the +0x800 half, as the lifted ors pair them. */
    const u128 n56 = spu_or(spu_or(spu_or(diff_bit(*r27, a[0], b[0]), diff_bit(*r26, a[1], b[1])),
                                   spu_rotqmbii(*m56, -4)),
                            spu_or(diff_bit(*r25, a[2], b[2]), diff_bit(*r24, a[3], b[3])));
    const u128 n57 = spu_or(spu_or(spu_or(diff_bit(*r27, a[4], b[4]), diff_bit(*r26, a[5], b[5])),
                                   spu_rotqmbii(*m57, -4)),
                            spu_or(diff_bit(*r25, a[6], b[6]), diff_bit(*r24, a[7], b[7])));
    /* The stores in the lifted order: +0, +0x10, +0x830, +0x20, +0x30, +0x800, +0x810, +0x820. */
    static const int order[8] = { 0, 1, 7, 2, 3, 4, 5, 6 };
    for (int k = 0; k < 8; k++)
        memcpy(ctx->ls + ((ab + off[order[k]]) & SPU_LS_MASK & ~0xFu), b[order[k]], 16);
    *A = spu_ai(*A, -64);
    *B = spu_ai(*B, -64);
    *m56 = n56;
    *m57 = n57;
}

#if defined(_M_X64) || defined(__x86_64__) || defined(PS3_SSE2NEON)
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif
/* The constant c with word 0 cleared when its quadwords were equal (eq):
 * diff_bit, without the branch. */
static inline u128 diff_bit_eq(u128 c, unsigned eq)
{
    c._u32[0] &= eq ? 0u : ~0u;
    return c;
}

/* diff_pass_fast with the sixteen quadwords in SSE registers: the copy above
 * went through two arrays on the stack, compared each pair with a branch,
 * and read ctx->ls again before each store (a byte store may change it). ls
 * is the context's local store, read once by the caller. */
static inline void diff_pass_sse(uint8_t* ls, u128* A, u128* B, u128* m56, u128* m57,
                                 const u128* r24, const u128* r25, const u128* r26, const u128* r27)
{
    const uint32_t ab = A->_u32[0] - 64u, bb = B->_u32[0] - 64u;
#define DIFF_AT(base, o) ((__m128i*)(void*)(ls + (((base) + (o)) & SPU_LS_MASK & ~0xFu)))
    /* Every load of the pass before its stores, as lifted. */
    const __m128i a0 = _mm_loadu_si128(DIFF_AT(ab, 0x0)),   b0 = _mm_loadu_si128(DIFF_AT(bb, 0x0));
    const __m128i a1 = _mm_loadu_si128(DIFF_AT(ab, 0x10)),  b1 = _mm_loadu_si128(DIFF_AT(bb, 0x10));
    const __m128i a2 = _mm_loadu_si128(DIFF_AT(ab, 0x20)),  b2 = _mm_loadu_si128(DIFF_AT(bb, 0x20));
    const __m128i a3 = _mm_loadu_si128(DIFF_AT(ab, 0x30)),  b3 = _mm_loadu_si128(DIFF_AT(bb, 0x30));
    const __m128i a4 = _mm_loadu_si128(DIFF_AT(ab, 0x800)), b4 = _mm_loadu_si128(DIFF_AT(bb, 0x800));
    const __m128i a5 = _mm_loadu_si128(DIFF_AT(ab, 0x810)), b5 = _mm_loadu_si128(DIFF_AT(bb, 0x810));
    const __m128i a6 = _mm_loadu_si128(DIFF_AT(ab, 0x820)), b6 = _mm_loadu_si128(DIFF_AT(bb, 0x820));
    const __m128i a7 = _mm_loadu_si128(DIFF_AT(ab, 0x830)), b7 = _mm_loadu_si128(DIFF_AT(bb, 0x830));
#define DIFF_EQ(x, y) (_mm_movemask_epi8(_mm_cmpeq_epi8((x), (y))) == 0xFFFF)
    const u128 n56 = spu_or(spu_or(spu_or(diff_bit_eq(*r27, DIFF_EQ(a0, b0)), diff_bit_eq(*r26, DIFF_EQ(a1, b1))),
                                   spu_rotqmbii(*m56, -4)),
                            spu_or(diff_bit_eq(*r25, DIFF_EQ(a2, b2)), diff_bit_eq(*r24, DIFF_EQ(a3, b3))));
    const u128 n57 = spu_or(spu_or(spu_or(diff_bit_eq(*r27, DIFF_EQ(a4, b4)), diff_bit_eq(*r26, DIFF_EQ(a5, b5))),
                                   spu_rotqmbii(*m57, -4)),
                            spu_or(diff_bit_eq(*r25, DIFF_EQ(a6, b6)), diff_bit_eq(*r24, DIFF_EQ(a7, b7))));
#undef DIFF_EQ
    /* The stores in the lifted order: +0, +0x10, +0x830, +0x20, +0x30, +0x800, +0x810, +0x820. */
    _mm_storeu_si128(DIFF_AT(ab, 0x0), b0);
    _mm_storeu_si128(DIFF_AT(ab, 0x10), b1);
    _mm_storeu_si128(DIFF_AT(ab, 0x830), b7);
    _mm_storeu_si128(DIFF_AT(ab, 0x20), b2);
    _mm_storeu_si128(DIFF_AT(ab, 0x30), b3);
    _mm_storeu_si128(DIFF_AT(ab, 0x800), b4);
    _mm_storeu_si128(DIFF_AT(ab, 0x810), b5);
    _mm_storeu_si128(DIFF_AT(ab, 0x820), b6);
#undef DIFF_AT
    *A = spu_ai(*A, -64);
    *B = spu_ai(*B, -64);
    *m56 = n56;
    *m57 = n57;
}
#define DIFF_HAVE_SSE 1
#endif

/* The last pass and the instructions after the loop, as lifted (lift lines of
 * region 0x8CB8, 0x8E18-0x8F60), through ctx->gpr. */
static void diff_pass_last(spu_context* ctx)
{
    u128* r = ctx->gpr;
    r[17] = spu_ai(r[22], -64);
    r[42] = spu_rotqmbii(r[56], -4);
    r[16] = spu_ai(r[67], -64);
    r[40] = spu_rotqmbii(r[57], -4);
    r[23] = spu_ai(r[23], 1);
    r[6] = spu_ls_read128(ctx, r[17]._u32[0] + 0x0);
    r[22] = spu_ori(r[17], 0);
    r[8] = spu_ls_read128(ctx, r[16]._u32[0] + 0x0);
    r[30] = spu_ceqi(r[23], 32);
    r[13] = spu_ls_read128(ctx, r[16]._u32[0] + 0x10);
    r[67] = spu_ori(r[16], 0);
    r[14] = spu_ls_read128(ctx, r[16]._u32[0] + 0x800);
    r[7] = spu_ls_read128(ctx, r[16]._u32[0] + 0x810);
    r[10] = spu_ls_read128(ctx, r[17]._u32[0] + 0x10);
    r[5] = spu_ls_read128(ctx, r[17]._u32[0] + 0x800);
    r[2] = spu_ceq(r[6], r[8]);
    r[9] = spu_ls_read128(ctx, r[17]._u32[0] + 0x810);
    r[75] = spu_ls_read128(ctx, r[16]._u32[0] + 0x830);
    r[12] = spu_ls_read128(ctx, r[16]._u32[0] + 0x20);
    r[81] = spu_ls_read128(ctx, r[16]._u32[0] + 0x30);
    r[4] = spu_ceq(r[10], r[13]);
    r[82] = spu_ceq(r[5], r[14]);
    r[78] = spu_ls_read128(ctx, r[16]._u32[0] + 0x820);
    r[79] = spu_ceq(r[9], r[7]);
    r[11] = spu_ls_read128(ctx, r[17]._u32[0] + 0x20);
    r[80] = spu_ls_read128(ctx, r[17]._u32[0] + 0x30);
    r[77] = spu_ls_read128(ctx, r[17]._u32[0] + 0x820);
    r[71] = spu_ls_read128(ctx, r[17]._u32[0] + 0x830);
    r[69] = spu_gb(r[2]);
    r[64] = spu_gb(r[4]);
    r[76] = spu_ceq(r[11], r[12]);
    r[62] = spu_gb(r[82]);
    r[70] = spu_ceq(r[80], r[81]);
    r[59] = spu_gb(r[79]);
    r[66] = spu_ceq(r[77], r[78]);
    r[58] = spu_gb(r[76]);
    r[63] = spu_ceq(r[71], r[75]);
    r[57] = spu_gb(r[70]);
    r[54] = spu_ceqi(r[69], 15);
    r[56] = spu_gb(r[66]);
    r[53] = spu_ceqi(r[64], 15);
    r[55] = spu_gb(r[63]);
    r[52] = spu_ceqi(r[62], 15);
    spu_ls_write128(ctx, r[17]._u32[0] + 0x0, r[8]);
    r[51] = spu_ceqi(r[59], 15);
    spu_ls_write128(ctx, r[17]._u32[0] + 0x10, r[13]);
    r[45] = spu_ceqi(r[57], 15);
    spu_ls_write128(ctx, r[17]._u32[0] + 0x830, r[75]);
    r[44] = spu_ceqi(r[56], 15);
    spu_ls_write128(ctx, r[17]._u32[0] + 0x20, r[12]);
    r[46] = spu_ceqi(r[58], 15);
    spu_ls_write128(ctx, r[17]._u32[0] + 0x30, r[81]);
    r[43] = spu_ceqi(r[55], 15);
    spu_ls_write128(ctx, r[17]._u32[0] + 0x800, r[14]);
    r[49] = spu_andc(r[27], r[54]);
    spu_ls_write128(ctx, r[17]._u32[0] + 0x810, r[7]);
    r[50] = spu_andc(r[26], r[53]);
    spu_ls_write128(ctx, r[17]._u32[0] + 0x820, r[78]);
    r[47] = spu_andc(r[27], r[52]);
    r[48] = spu_andc(r[26], r[51]);
    r[41] = spu_or(r[49], r[50]);
    r[39] = spu_or(r[47], r[48]);
    r[37] = spu_andc(r[25], r[46]);
    r[38] = spu_andc(r[24], r[45]);
    r[35] = spu_andc(r[25], r[44]);
    r[36] = spu_andc(r[24], r[43]);
    r[34] = spu_or(r[41], r[42]);
    r[32] = spu_or(r[39], r[40]);
    r[33] = spu_or(r[37], r[38]);
    r[31] = spu_or(r[35], r[36]);
    r[56] = spu_or(r[34], r[33]);
    r[57] = spu_or(r[32], r[31]);
    /* r30 is all ones: the loop ends; then 0x8F50-0x8F5C. */
    r[26] = spu_ceqi(r[56], 0);
    r[7] = spu_fsmbi(0x0);
    r[27] = spu_ceqi(r[57], 0);
    r[8] = spu_ori(r[7], 0);
}

/* sse: the passes in SSE registers (diff_pass_sse); 0, SPU_NATIVE_DIFF=3, the
 * byte copies of diff_pass_fast. */
static int diff_run(spu_context* ctx, int sse)
{
    u128* r = ctx->gpr;
    const uint32_t n = r[23]._u32[0];
    if (n >= DIFF_PASSES) return 0;   /* the lifted loop would run until the counter wraps */
    u128 A = r[22], B = r[67], m56 = r[56], m57 = r[57];
    const u128 c24 = r[24], c25 = r[25], c26 = r[26], c27 = r[27];
    const uint32_t fast = DIFF_PASSES - 1u - n;
#ifdef DIFF_HAVE_SSE
    if (sse) {
        uint8_t* const ls = ctx->ls;
        for (uint32_t i = 0; i < fast; i++)
            diff_pass_sse(ls, &A, &B, &m56, &m57, &c24, &c25, &c26, &c27);
    } else
#endif
    for (uint32_t i = 0; i < fast; i++)
        diff_pass_fast(ctx, &A, &B, &m56, &m57, &c24, &c25, &c26, &c27);
    (void)sse;
    r[22] = A;
    r[67] = B;
    r[56] = m56;
    r[57] = m57;
    for (int k = 0; k < 4; k++) r[23]._u32[k] += fast;
    ctx->steps += fast;               /* one SPU_RGOTO per back edge taken */
    diff_pass_last(ctx);
    return 1;
}

/* SPU_NATIVE_DIFF=2: run the interpreter on a copy, the block on the real
 * context, and compare. */
static int diff_check(spu_context* ctx)
{
    static uint8_t* ls_copy;
    static unsigned long long checked, bad;
    if (!ls_copy && !(ls_copy = (uint8_t*)malloc(SPU_LS_SIZE))) return diff_run(ctx, 1);
    spu_context* c = (spu_context*)malloc(sizeof *c);
    if (!c) return diff_run(ctx, 1);
    memcpy(c, ctx, sizeof *c);
    memcpy(ls_copy, ctx->ls, SPU_LS_SIZE);
    c->ls = ls_copy;
    /* An image id nothing registered: the interpreter's lookups find no lifted
     * function to rejoin, so it stops at the end (spu_interp_run_until stops
     * only for a non-negative id). An image registered as 0 matches any id;
     * if one of its functions sits in the loop, the copy stops early and the
     * check is skipped. */
    c->image_id = 0x7FFF;
    const uint32_t at = spu_interp_run_until(c, DIFF_START, DIFF_END);
    const int ran = diff_run(ctx, 1);
    if (!ran) {                       /* the lifted loop runs: nothing to compare */
        free(c);
        return 0;
    }
    if (at != DIFF_END) {
        static int said;
        if (!said++) fprintf(stderr, "[spu-diff] check skipped: the interpreter stopped at 0x%X\n", at);
        free(c);
        return ran;
    }
    checked++;
    int mism = 0;
    int reg = -1;
    for (int k = 0; k < 128 && !mism; k++)
        if (memcmp(&c->gpr[k], &ctx->gpr[k], sizeof(u128))) { mism = 1; reg = k; }
    unsigned lsoff = SPU_LS_SIZE;     /* none */
    if (!mism && memcmp(ls_copy, ctx->ls, SPU_LS_SIZE)) {
        mism = 1;
        for (unsigned i = 0; i < SPU_LS_SIZE; i++) if (ls_copy[i] != ctx->ls[i]) { lsoff = i; break; }
    }
    if (mism) {
        bad++;
        s_diff_bad = 1;
        fprintf(stderr, "[spu-diff] MISMATCH: register %d, LS 0x%X (0x40000: none); the block is off for "
                "the session\n", reg, lsoff);
        if (reg >= 0)
            fprintf(stderr, "[spu-diff]   r%d block %08X %08X %08X %08X, interpreter %08X %08X %08X %08X\n", reg,
                    ctx->gpr[reg]._u32[0], ctx->gpr[reg]._u32[1], ctx->gpr[reg]._u32[2], ctx->gpr[reg]._u32[3],
                    c->gpr[reg]._u32[0], c->gpr[reg]._u32[1], c->gpr[reg]._u32[2], c->gpr[reg]._u32[3]);
    }
    if (checked == 1 || (checked & (checked - 1)) == 0 || mism)
        fprintf(stderr, "[spu-diff] %llu checked, %llu mismatches\n", checked, bad);
    free(c);
    return ran;
}

int spu_hook_state_diff(spu_context* ctx)
{
    const int mode = native_diff_mode();
    if (!mode || s_diff_bad) return 0;
    if (g_spu_goto_slow || g_yz_lockstep_on || g_pm_flow_ctx || g_spu_trampoline_fn || g_spu_ls_dbg ||
        (ctx->int_enable && (__atomic_load_n(&ctx->event_status, __ATOMIC_RELAXED) & ctx->event_mask)))
        return 0;
    if (mode == 2) {
        static unsigned tick;
        if (++tick % native_diff_every() == 0) return diff_check(ctx);
    }
    return diff_run(ctx, mode != 3);
}
