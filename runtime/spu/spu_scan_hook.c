/*
 * ps3recomp - a native block for a 16-entry scan
 * (spu_lifter.py --native-block ADDR-END=SYMBOL; see spu_ring_hook.c for the
 * contract: return 1 having done the work of the instructions from ADDR up to
 * END, with every register and local-store byte as they leave them, or 0 to
 * run them lifted).
 *
 * Demon's Souls' HighGraphics ring consumer (SPURS image 2), LS 0x7348-0x7440,
 * in region 0x7140: a loop over 16 entries of three tables, r30's (32 bytes an
 * entry) and r31's and r32's (96 bytes an entry). An entry builds five
 * quadwords from r30's and r31's entries (three shufbs, with the loop
 * constants in r40-r44) and compares them with five of r32's entry. For each
 * that differs the loop leaves for a handler (0x7718, 0x77F0, 0x7888, 0x74B8,
 * 0x7CA0), and the handlers come back into the loop. Every entry also ors a
 * word compare into r34. The loop itself stores nothing. A block-count
 * profile of the deep Palace: the loop always runs all 16 entries, about 1.9
 * of them a time go to a handler, and the loop is about 12.5% of the image's
 * lifted statements.
 *
 * At the loop head this block works out the entries ahead on host locals and
 * stops at the first one that goes to a handler. It leaves every register as
 * the lifted loop leaves it at that entry's head, the last skipped entry's
 * values included, and returns 0, so that entry runs lifted. When no entry
 * is left it leaves them as at the loop's exit and returns 1 (the lifted code
 * goes on at 0x7440). When the entry at the head goes to a handler it changes
 * nothing.
 *
 * On x86-64, and on arm64 through sse2neon, each entry's test runs on SSE
 * registers (scan_stop_sse: a load and a shuffle a quadword, the lift's
 * shufb, word compares), and only the last skipped entry runs again on the
 * lifted helpers for the registers it leaves. On the helpers alone every
 * entry built all its registers through scalar lane loops and a 37-quadword
 * struct, about as slow as the lifted loop.
 *
 * It runs the lifted loop instead (returns 0) whenever the lifted loop would
 * leave its region at the next back edge (the knobs and the pending-interrupt
 * test of SPU_RGOTO), a local-store probe is on, or the counter is not below
 * 16. An event that arrives during the skipped entries is taken at the next
 * exit test after the block, as if it had arrived then.
 *
 * SPU_NATIVE_SCAN (live): 1 (default) as above; 0 runs the lifted loop, for
 * A/B; 3 tests every entry on the lifted helpers, for A/B; 2 checks: every
 * SPU_NATIVE_SCAN_EVERY-th call that skips an entry (default 64), the skipped
 * entries also run in the SPU interpreter on a copy of the context and its
 * local store, one entry at a time, and every register and the whole local
 * store are compared with the copy's ([spu-scan] lines). A mismatch puts the
 * state back, so the lifted loop runs those entries, and turns the block off
 * for the session. The check also counts stops at an entry that goes to no
 * handler.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/ps3_knob.h"
#include "ps3emu/ps3_sse.h"   /* PS3_SSE2NEON: scan_run_sse on arm64 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_interp.h"

#define SCAN_START  0x7348u
#define SCAN_BRANCH 0x73FCu   /* the first of the five handler branches */
#define SCAN_TAIL   0x7414u   /* the counters and the back edge */
#define SCAN_END    0x7440u
#define SCAN_N      16u

extern volatile int   g_spu_goto_slow;
extern volatile int   g_yz_lockstep_on;
extern void* volatile g_pm_flow_ctx;

static volatile int s_scan_bad;   /* a check mismatch: off for the session */

static int native_scan_mode(void)
{
    static int mode = 1;
    PS3_KNOB(mode, ps3_env_int("SPU_NATIVE_SCAN", 1));
    return mode;
}

static unsigned native_scan_every(void)
{
    static int n = 64;
    PS3_KNOB(n, ps3_env_int("SPU_NATIVE_SCAN_EVERY", 64));
    return n > 0 ? (unsigned)n : 1u;
}

/* The registers one entry leaves (0x7348-0x7410), but r25 and r33, which the
 * tail writes again. */
typedef struct {
    u128 r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15, r16, r17, r18, r19, r20, r21, r22, r23,
         r35, r38, r45, r53, r54, r55, r56, r57, r58, r59, r60, r61, r62, r63, r64, r65, r66, r67;
} scan_regs;

typedef struct {
    u128 r40, r41, r42, r43, r44;   /* set before the loop */
} scan_consts;

/* One entry, as lifted (lift lines of region 0x7140, 0x7348-0x73F8), on
 * locals. Nonzero when one of its five branches to a handler is taken. */
static inline int scan_entry(const spu_context* ctx, const scan_consts* k, u128 r30, u128 r31, u128 r32,
                             scan_regs* t)
{
    t->r23 = spu_ori(r30, 0);
    t->r17 = spu_ls_read128(ctx, r30._u32[0] + 0x0);
    t->r66 = spu_ls_read128(ctx, r31._u32[0] + 0x0);
    t->r67 = spu_ls_read128(ctx, r31._u32[0] + 0x50);
    t->r19 = spu_ls_read128(ctx, r30._u32[0] + 0x10);
    const u128 r16 = spu_ls_read128(ctx, r31._u32[0] + 0x10);
    const u128 r20 = spu_ls_read128(ctx, r31._u32[0] + 0x20);
    t->r65 = spu_ceqbi(t->r17, 1);
    t->r62 = spu_ls_read128(ctx, r32._u32[0] + 0x30);
    t->r60 = spu_ls_read128(ctx, r32._u32[0] + 0x40);
    t->r63 = spu_andc(k->r44, t->r65);
    t->r56 = spu_ls_read128(ctx, r32._u32[0] + 0x0);
    t->r64 = spu_ceqi(t->r19, 0);
    const u128 r15 = spu_shufb(t->r66, t->r67, k->r40);
    t->r58 = spu_ai(r16, 4);
    t->r61 = spu_shlqbyi(t->r63, 4);
    t->r10 = spu_xori(t->r64, -1);
    t->r53 = spu_ls_read128(ctx, r32._u32[0] + 0x10);
    t->r35 = spu_ceq(t->r62, t->r17);
    t->r55 = spu_ls_read128(ctx, r32._u32[0] + 0x20);
    t->r15 = spu_selb(r15, t->r17, k->r42);
    t->r45 = spu_ls_read128(ctx, r32._u32[0] + 0x50);
    t->r59 = spu_a(k->r41, t->r61);
    const u128 r33 = spu_shlqbyi(t->r66, 12);
    const u128 r22 = spu_ceq(t->r56, t->r15);
    t->r20 = spu_shufb(r20, t->r10, k->r43);
    t->r57 = spu_ceq(t->r60, t->r19);
    t->r16 = spu_shufb(r16, t->r58, t->r59);
    t->r12 = spu_xori(r22, -1);
    t->r54 = spu_nand(t->r57, t->r35);
    t->r13 = spu_ceq(t->r55, t->r20);
    t->r9 = spu_gb(t->r12);
    t->r14 = spu_ceq(t->r53, t->r16);
    t->r8 = spu_gb(t->r54);
    t->r11 = spu_ceq(t->r45, r33);
    t->r5 = spu_xori(t->r14, -1);
    t->r6 = spu_xori(t->r13, -1);
    t->r7 = spu_xori(t->r11, -1);
    t->r18 = spu_gb(t->r5);
    t->r22 = spu_and(t->r10, t->r8);
    t->r21 = spu_gb(t->r6);
    t->r38 = spu_gb(t->r7);
    /* 0x73FC, 0x7400, 0x7404, 0x7408, 0x7410: brnz on the preferred word. */
    return (t->r9._u32[0] | t->r18._u32[0] | t->r21._u32[0] | t->r22._u32[0] | t->r38._u32[0]) != 0;
}

/* The registers after n skipped entries: s is the last one's, r34 has every
 * skipped entry's word compare ored in, and done says the loop has ended. */
static void scan_leave(spu_context* ctx, const scan_regs* s, uint32_t n, int done, u128 r30, u128 r31, u128 r32,
                       u128 r34, u128 r36, u128 r37, u128 r39)
{
    u128* r = ctx->gpr;
    r[5] = s->r5;   r[6] = s->r6;   r[7] = s->r7;   r[8] = s->r8;   r[9] = s->r9;
    r[10] = s->r10; r[11] = s->r11; r[12] = s->r12; r[13] = s->r13; r[14] = s->r14;
    r[15] = s->r15; r[16] = s->r16; r[17] = s->r17; r[18] = s->r18; r[19] = s->r19;
    r[20] = s->r20; r[21] = s->r21; r[22] = s->r22; r[23] = s->r23;
    r[35] = s->r35; r[38] = s->r38; r[45] = s->r45;
    r[53] = s->r53; r[54] = s->r54; r[55] = s->r55; r[56] = s->r56; r[57] = s->r57;
    r[58] = s->r58; r[59] = s->r59; r[60] = s->r60; r[61] = s->r61; r[62] = s->r62;
    r[63] = s->r63; r[64] = s->r64; r[65] = s->r65; r[66] = s->r66; r[67] = s->r67;
    r[33] = spu_rotmi(s->r35, -16);
    r[25] = spu_ceqi(r37, 16);
    r[30] = r30; r[31] = r31; r[32] = r32; r[34] = r34; r[36] = r36; r[37] = r37; r[39] = r39;
    ctx->steps += done ? n - 1 : n;   /* one SPU_RGOTO per back edge taken */
}

/* Runs the entries from the head on, stopping at the first that goes to a
 * handler, every entry on the lifted helpers (SPU_NATIVE_SCAN=3). Returns the
 * entries skipped (0: nothing changed) and sets *done when they were the last
 * ones, so the loop has ended. */
static uint32_t scan_run_u128(spu_context* ctx, int* done)
{
    u128* r = ctx->gpr;
    *done = 0;
    if (r[37]._u32[0] >= SCAN_N) return 0;   /* the lifted loop would run until the counter wraps */
    const scan_consts k = { r[40], r[41], r[42], r[43], r[44] };
    u128 r30 = r[30], r31 = r[31], r32 = r[32], r34 = r[34], r36 = r[36], r37 = r[37], r39 = r[39];
    scan_regs t[2];
    int cur = 0;
    uint32_t n = 0;
    for (;;) {
        if (scan_entry(ctx, &k, r30, r31, r32, &t[cur])) break;   /* that entry runs lifted */
        /* The tail, 0x7414-0x743C, but r33 and r25. */
        r34 = spu_or(r34, spu_rotmi(t[cur].r35, -16));
        r37 = spu_ai(r37, 1);
        r32 = spu_ai(r32, 96);
        r31 = spu_ai(r31, 96);
        r30 = spu_ai(r30, 32);
        r39 = spu_ai(r39, 4);
        r36 = spu_ai(r36, 32);
        n++;
        cur ^= 1;
        if (r37._u32[0] == SCAN_N) { *done = 1; break; }   /* the brz at 0x743C falls through */
    }
    if (!n) return 0;
    scan_leave(ctx, &t[cur ^ 1], n, *done, r30, r31, r32, r34, r36, r37, r39);
    return n;
}

#if defined(__SSE4_1__) || defined(PS3_SSE2NEON)
#if defined(__SSE4_1__)
#include <smmintrin.h>
#endif
#define SCAN_HAVE_SSE 1

/* spu_ls_read128 as one load and one shuffle: SPU byte P to host byte P^3,
 * the u128 layout. */
static inline __m128i scan_q(const uint8_t* ls, uint32_t lsa, __m128i jx3)
{
    const uint8_t* p = ls + (lsa & (SPU_LS_MASK & ~0xFu));
    return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(const void*)p), jx3);
}

static inline __m128i scan_shufb(__m128i a, __m128i b, __m128i c)
{
    u128 A, B, C;
    memcpy(&A, &a, 16); memcpy(&B, &b, 16); memcpy(&C, &c, 16);
    const u128 r = spu_shufb(A, B, C);
    __m128i R;
    memcpy(&R, &r, 16);
    return R;
}

typedef struct {
    __m128i r40, r41, r42, r43, r44;
} scan_kx;

/* scan_entry's return and its r35, on SSE registers: an entry goes to a
 * handler when one of its five built quadwords differs from r32's entry
 * (r9, r18, r21, r38: gb of a word compare's complement is nonzero when any
 * word differs), or (r22) when r19's preferred word is nonzero and r60 or r62
 * differs from r19 or r17. */
static inline int scan_stop_sse(const uint8_t* ls, const scan_kx* k, __m128i jx3, uint32_t a30, uint32_t a31,
                                uint32_t a32, __m128i* r35)
{
    const __m128i r17 = scan_q(ls, a30, jx3);
    const __m128i r19 = scan_q(ls, a30 + 0x10, jx3);
    const __m128i r66 = scan_q(ls, a31, jx3);
    const __m128i q16 = scan_q(ls, a31 + 0x10, jx3);
    const __m128i q20 = scan_q(ls, a31 + 0x20, jx3);
    const __m128i r67 = scan_q(ls, a31 + 0x50, jx3);
    const __m128i r56 = scan_q(ls, a32, jx3);
    const __m128i r53 = scan_q(ls, a32 + 0x10, jx3);
    const __m128i r55 = scan_q(ls, a32 + 0x20, jx3);
    const __m128i r62 = scan_q(ls, a32 + 0x30, jx3);
    const __m128i r60 = scan_q(ls, a32 + 0x40, jx3);
    const __m128i r45 = scan_q(ls, a32 + 0x50, jx3);
    /* r15 = selb(shufb(r66, r67, r40), r17, r42) */
    const __m128i r15 = _mm_or_si128(_mm_andnot_si128(k->r42, scan_shufb(r66, r67, k->r40)),
                                     _mm_and_si128(r17, k->r42));
    /* r16 = shufb(q16, q16 + 4, r41 + shlqbyi(r44 & ~ceqbi(r17, 1), 4)); a
     * shift by whole words moves host words as it moves SPU words. */
    const __m128i r63 = _mm_andnot_si128(_mm_cmpeq_epi8(r17, _mm_set1_epi8(1)), k->r44);
    const __m128i r59 = _mm_add_epi32(k->r41, _mm_srli_si128(r63, 4));
    const __m128i r16 = scan_shufb(q16, _mm_add_epi32(q16, _mm_set1_epi32(4)), r59);
    /* r20 = shufb(q20, ~ceqi(r19, 0), r43) */
    const __m128i r10 = _mm_xor_si128(_mm_cmpeq_epi32(r19, _mm_setzero_si128()), _mm_set1_epi32(-1));
    const __m128i r20 = scan_shufb(q20, r10, k->r43);
    /* r33 = shlqbyi(r66, 12) */
    const __m128i r33 = _mm_srli_si128(r66, 12);
    *r35 = _mm_cmpeq_epi32(r62, r17);
    const __m128i d = _mm_or_si128(_mm_or_si128(_mm_xor_si128(r56, r15), _mm_xor_si128(r53, r16)),
                                   _mm_or_si128(_mm_xor_si128(r55, r20), _mm_xor_si128(r45, r33)));
    if (!_mm_testz_si128(d, d)) return 1;
    const __m128i e = _mm_or_si128(_mm_xor_si128(r60, r19), _mm_xor_si128(r62, r17));
    return _mm_cvtsi128_si32(r19) != 0 && !_mm_testz_si128(e, e);
}

/* scan_run_u128 with each entry's test on SSE registers; only the last
 * skipped entry runs again on the lifted helpers, for the registers it
 * leaves. If that run would go to a handler the two disagree: nothing is
 * changed and the block is off for the session. */
static uint32_t scan_run_sse(spu_context* ctx, int* done)
{
    u128* r = ctx->gpr;
    *done = 0;
    const uint32_t c = r[37]._u32[0];
    if (c >= SCAN_N) return 0;   /* the lifted loop would run until the counter wraps */
    scan_kx k;
    memcpy(&k.r40, &r[40], 16); memcpy(&k.r41, &r[41], 16); memcpy(&k.r42, &r[42], 16);
    memcpy(&k.r43, &r[43], 16); memcpy(&k.r44, &r[44], 16);
    const __m128i jx3 = spu_qb_jx3();
    const uint8_t* ls = ctx->ls;
    const uint32_t a30 = r[30]._u32[0], a31 = r[31]._u32[0], a32 = r[32]._u32[0];
    __m128i or34 = _mm_setzero_si128(), r35;
    uint32_t n = 0;
    while (c + n < SCAN_N && !scan_stop_sse(ls, &k, jx3, a30 + 32 * n, a31 + 96 * n, a32 + 96 * n, &r35)) {
        or34 = _mm_or_si128(or34, _mm_srli_epi32(r35, 16));   /* the tail's rotmi(r35, -16) */
        n++;
    }
    if (!n) return 0;
    const int32_t m = (int32_t)n - 1;
    const scan_consts kc = { r[40], r[41], r[42], r[43], r[44] };
    scan_regs t;
    if (scan_entry(ctx, &kc, spu_ai(r[30], 32 * m), spu_ai(r[31], 96 * m), spu_ai(r[32], 96 * m), &t)) {
        s_scan_bad = 1;
        fprintf(stderr, "[spu-scan] MISMATCH: the SSE test skipped an entry that goes to a handler; the block is "
                "off for the session\n");
        return 0;
    }
    *done = c + n == SCAN_N;
    u128 o34;
    memcpy(&o34, &or34, 16);
    const int32_t a = (int32_t)n;
    scan_leave(ctx, &t, n, *done, spu_ai(r[30], 32 * a), spu_ai(r[31], 96 * a), spu_ai(r[32], 96 * a),
               spu_or(r[34], o34), spu_ai(r[36], 32 * a), spu_ai(r[37], a), spu_ai(r[39], 4 * a));
    return n;
}
#endif

static uint32_t scan_run(spu_context* ctx, int* done, int sse)
{
#ifdef SCAN_HAVE_SSE
    if (sse) return scan_run_sse(ctx, done);
#endif
    (void)sse;
    return scan_run_u128(ctx, done);
}

/* SPU_NATIVE_SCAN=2: the block on the real context, then the same entries in
 * the interpreter on a copy, and compare. *used is 0 when the block skipped
 * no entry, so there was nothing to compare. */
static int scan_check(spu_context* ctx, int* used)
{
    static uint8_t* ls_copy;
    static unsigned long long checked, bad;
    int done;
    *used = 1;
    if (!ls_copy && !(ls_copy = (uint8_t*)malloc(SPU_LS_SIZE))) return scan_run(ctx, &done, 1) && done;
    spu_context* c = (spu_context*)malloc(sizeof *c);
    if (!c) return scan_run(ctx, &done, 1) && done;
    memcpy(c, ctx, sizeof *c);
    memcpy(ls_copy, ctx->ls, SPU_LS_SIZE);
    c->ls = ls_copy;
    /* An image id nothing registered: the interpreter's lookups find no lifted
     * function to rejoin, so it stops only at the stop address (see
     * spu_diff_hook.c). If it stops elsewhere the check is skipped. */
    c->image_id = 0x7FFF;
    static u128 pre[128];
    memcpy(pre, ctx->gpr, sizeof pre);
    const uint32_t steps0 = ctx->steps;
    const uint32_t n = scan_run(ctx, &done, 1);
    /* The entry the block stopped at must go to a handler too; a stop there
     * costs only speed, so the interpreter compare below cannot see it. */
    if (!done && ctx->gpr[37]._u32[0] < SCAN_N) {
        static unsigned long long stops, early;
        const scan_consts k = { ctx->gpr[40], ctx->gpr[41], ctx->gpr[42], ctx->gpr[43], ctx->gpr[44] };
        scan_regs t;
        stops++;
        if (!scan_entry(ctx, &k, ctx->gpr[30], ctx->gpr[31], ctx->gpr[32], &t)) {
            early++;
            if ((early & (early - 1)) == 0)
                fprintf(stderr, "[spu-scan] %llu of %llu stops at an entry that goes to no handler\n", early, stops);
        }
    }
    if (!n) {                         /* the lifted loop runs: nothing to compare */
        *used = 0;
        free(c);
        return 0;
    }
    int mism = 0, reg = -1;
    uint32_t at = 0, want = 0;
    for (uint32_t i = 0; i < n && !mism; i++) {
        if ((at = spu_interp_run_until(c, SCAN_START, SCAN_BRANCH)) != (want = SCAN_BRANCH)) break;
        if (c->gpr[9]._u32[0] | c->gpr[18]._u32[0] | c->gpr[21]._u32[0] | c->gpr[22]._u32[0] |
            c->gpr[38]._u32[0]) {
            mism = 1;                 /* the block skipped an entry that goes to a handler */
            fprintf(stderr, "[spu-scan] MISMATCH: entry %u of %u goes to a handler in the interpreter\n", i, n);
            break;
        }
        if ((at = spu_interp_run_until(c, SCAN_BRANCH, SCAN_TAIL)) != (want = SCAN_TAIL)) break;
        want = (i + 1 == n && done) ? SCAN_END : SCAN_START;
        if ((at = spu_interp_run_until(c, SCAN_TAIL, want)) != want) break;
        at = 0;
    }
    if (!mism && at) {
        static int said;
        if (!said++) fprintf(stderr, "[spu-scan] check skipped: the interpreter stopped at 0x%X, not 0x%X\n",
                             at, want);
        free(c);
        return done;
    }
    unsigned lsoff = SPU_LS_SIZE;     /* none */
    for (int k = 0; k < 128 && !mism; k++)
        if (memcmp(&c->gpr[k], &ctx->gpr[k], sizeof(u128))) { mism = 1; reg = k; }
    if (!mism && memcmp(ls_copy, ctx->ls, SPU_LS_SIZE)) {
        mism = 1;
        for (unsigned i = 0; i < SPU_LS_SIZE; i++) if (ls_copy[i] != ctx->ls[i]) { lsoff = i; break; }
    }
    checked++;
    if (mism) {
        bad++;
        s_scan_bad = 1;
        fprintf(stderr, "[spu-scan] MISMATCH after %u entries: register %d, LS 0x%X (0x40000: none); the block "
                "is off for the session\n", n, reg, lsoff);
        if (reg >= 0)
            fprintf(stderr, "[spu-scan]   r%d block %08X %08X %08X %08X, interpreter %08X %08X %08X %08X\n", reg,
                    ctx->gpr[reg]._u32[0], ctx->gpr[reg]._u32[1], ctx->gpr[reg]._u32[2], ctx->gpr[reg]._u32[3],
                    c->gpr[reg]._u32[0], c->gpr[reg]._u32[1], c->gpr[reg]._u32[2], c->gpr[reg]._u32[3]);
        /* The state the loop started from, and the lifted loop runs the
         * entries. The loop stores nothing, so the copy's local store is the
         * one from before; it is put back only where the two differ. */
        memcpy(ctx->gpr, pre, sizeof pre);
        if (lsoff < SPU_LS_SIZE) memcpy(ctx->ls, ls_copy, SPU_LS_SIZE);
        ctx->steps = steps0;
        done = 0;
    }
    if (checked == 1 || (checked & (checked - 1)) == 0 || mism)
        fprintf(stderr, "[spu-scan] %llu checked, %llu mismatches\n", checked, bad);
    free(c);
    return done;
}

int spu_hook_entry_scan(spu_context* ctx)
{
    const int mode = native_scan_mode();
    if (!mode || s_scan_bad) return 0;
    if (g_spu_goto_slow || g_yz_lockstep_on || g_pm_flow_ctx || g_spu_trampoline_fn || g_spu_ls_dbg ||
        (ctx->int_enable && (__atomic_load_n(&ctx->event_status, __ATOMIC_RELAXED) & ctx->event_mask)))
        return 0;
    int done;
    if (mode == 2) {
        /* Only a call that skips an entry counts, so a check that found
         * nothing to compare is tried again at the next call. */
        static unsigned tick;
        if (tick + 1 >= native_scan_every()) {
            int used;
            const int ret = scan_check(ctx, &used);
            if (used) tick = 0;
            return ret;
        }
        if (scan_run(ctx, &done, 1)) tick++;
        return done;
    }
    scan_run(ctx, &done, mode != 3);
    return done;
}
