/*
 * ps3recomp - native blocks for the four hot loops of a software synth's SPU
 * mixer (spu_lifter.py --native-block ADDR-END=SYMBOL; the contract is
 * spu_ring_hook.c's: return 1 having done the instructions from ADDR up to
 * END, or 0 to run them lifted).
 *
 * Demon's Souls mixes its sound on one SPU, in libsynth2's synth (a raw SPU
 * thread group, the image a port lifts as spu_0010_at_01818080). On a slower
 * CPU with many sounds playing at once (a few rolls, or walking in water) the
 * synth fell behind, and the game's stream mixer, finding no new block, played
 * its last 4096 samples (85 ms) again: the sound stutters and repeats. An
 * address profile of the synth with the game at its voice limit put 99% of
 * the synth's lifted time in four loops:
 *
 *   0x1090  the resampler, one output sample a pass: a phase step, then a
 *           four-tap interpolation from a 512-entry table (51% of it);
 *   0x0AC4  a 112-byte clear, one byte a pass (25%);
 *   0x63C8  the pan: each mono sample times two quadwords of gains, added to
 *           two quadwords of channel sums (13%);
 *   0x2C20  a gain ramp toward 1.0, applied to a buffer of floats (10%).
 *
 * Lifted, each pass goes through ctx->gpr in memory and back-edge tests, a
 * byte or word store is a quadword read, shuffle and write, and the float ops
 * work all four lanes where the loop uses one.
 *
 * Each block here does the passes it can natively and then returns 0, so the
 * lifted code runs the next pass from ADDR: the loop's last pass, or for the
 * resampler the first pass this block does not do. The registers a pass only
 * uses within itself are written again by that lifted pass before anything
 * reads them, so the block leaves them as they were; it writes every other
 * register a pass writes, exactly as the lifted passes would (all four lanes,
 * through the same helpers), and does the passes' local-store writes in their
 * order. So whatever follows reads what the lifted loop would have left. The
 * steps are counted as the lifted back edges count them.
 *
 * The resampler does a pass natively when its new position is 0 or 1 sample
 * ahead (the 0x10D4 / 0x0FB0 back edges) with no refill of the input due, and
 * leaves to the lifted code a pass that ends the buffer (0x1260), moves 2 or
 * more samples, refills the input (0x12B8) or whose output store would land
 * on the input counter (it reads that counter before the store).
 *
 * As with the other blocks, an event that arrives during the native passes is
 * taken at the next back edge the lifted code tests, a few microseconds
 * later. None runs (returns 0 at once) when the lifted code would leave its
 * region at the next back edge (the knobs and the pending-interrupt test of
 * SPU_RGOTO), a local-store probe is on, or the loop's inputs are outside
 * what the block was written for (a misaligned pointer, a count that would
 * wrap).
 *
 * SPU_NATIVE_SYNTH (live): 1 (default) on; 0 off, for A/B; 2 checks: every
 * SPU_NATIVE_SYNTH_EVERY-th call (default 64) of a block also runs its passes
 * in the SPU interpreter on a copy of the context and local store, one pass
 * at a time, and compares the registers the block writes and the whole local
 * store ([spu-synth] lines). A mismatch turns that block off for the session.
 * SPU_NATIVE_SYNTH_MASK (live, default 15): which blocks run, 1 the clear, 2
 * the gain ramp, 4 the pan, 8 the resampler. SPU_NATIVE_SYNTH_STATS=1 prints
 * the passes each block did and why the resampler stopped, now and then.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/ps3_knob.h"
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_interp.h"

extern volatile int   g_spu_goto_slow;
extern volatile int   g_yz_lockstep_on;
extern void* volatile g_pm_flow_ctx;

enum { SYN_FILL, SYN_GAIN, SYN_PAN, SYN_RESAMPLE, SYN_N };
static const char* const syn_name[SYN_N] = { "clear", "gain", "pan", "resample" };
static const uint32_t syn_start[SYN_N] = { 0xAC4u, 0x2C20u, 0x63C8u, 0x1090u };

/* A block's passes per call at most: the loops here run a buffer's length
 * (the resampler a few hundred samples), so this only bounds a corrupt count. */
#define SYN_MAX_PASSES 65536u

static volatile int s_syn_bad[SYN_N];   /* a check mismatch: that block is off for the session */

static int syn_mode(void)
{
    static int mode = 1;
    PS3_KNOB(mode, ps3_env_int("SPU_NATIVE_SYNTH", 1));
    return mode;
}

static int syn_mask(void)
{
    static int m = 15;
    PS3_KNOB(m, ps3_env_int("SPU_NATIVE_SYNTH_MASK", 15));
    return m;
}

static unsigned syn_every(void)
{
    static int n = 64;
    PS3_KNOB(n, ps3_env_int("SPU_NATIVE_SYNTH_EVERY", 64));
    return n > 0 ? (unsigned)n : 1u;
}

/* SPU_NATIVE_SYNTH_STATS=1: [spu-synth] counts, every 2^20 calls of the blocks. */
static int syn_stats_on(void)
{
    static int on = 0;
    PS3_KNOB(on, ps3_env_int("SPU_NATIVE_SYNTH_STATS", 0));
    return on;
}

enum { RS_END, RS_ADV2, RS_ADV3, RS_ADVN, RS_REFILL, RS_ALIAS, RS_CAP, RS_N };
static const char* const rs_why[RS_N] = { "end", "adv2", "adv3", "adv4+", "refill", "alias", "cap" };
static unsigned long long s_calls[SYN_N], s_passes[SYN_N], s_rs_adv[2], s_rs_stop[RS_N];
static unsigned long long s_gain_flat;   /* the gain passes past the ramp's end */

static void syn_stats_tick(void)
{
    static unsigned long long n;
    if ((++n & ((1ull << 20) - 1)) != 0) return;
    fprintf(stderr, "[spu-synth] calls/passes:");
    for (int i = 0; i < SYN_N; i++) fprintf(stderr, " %s %llu/%llu", syn_name[i], s_calls[i], s_passes[i]);
    fprintf(stderr, " | resample passes adv0 %llu adv1 %llu, stops:", s_rs_adv[0], s_rs_adv[1]);
    for (int i = 0; i < RS_N; i++) fprintf(stderr, " %s %llu", rs_why[i], s_rs_stop[i]);
    fprintf(stderr, " | gain passes past the ramp %llu\n", s_gain_flat);
}

/* The test of SPU_RGOTO, and the other states a block leaves to the lifted
 * code: a pending trampoline, a local-store probe. */
static inline int syn_slow(const spu_context* ctx)
{
    return g_spu_goto_slow || g_yz_lockstep_on || g_pm_flow_ctx || g_spu_trampoline_fn || g_spu_ls_dbg ||
           (ctx->int_enable && (__atomic_load_n(&ctx->event_status, __ATOMIC_RELAXED) & ctx->event_mask));
}

/* The big-endian word at a local-store address, rounded down to a word: what
 * rotqby(lqd(a), a) puts in word 0 for an aligned a, and what shufb with
 * cwd(a, 0) and stqd at a replaces. */
static inline uint32_t ls_w(const uint8_t* ls, uint32_t a)
{
    uint32_t w;
    memcpy(&w, ls + (a & SPU_LS_MASK & ~3u), 4);
    return SPU_BSWAP32(w);
}

static inline void ls_set_w(uint8_t* ls, uint32_t a, uint32_t v)
{
    v = SPU_BSWAP32(v);
    memcpy(ls + (a & SPU_LS_MASK & ~3u), &v, 4);
}

static inline u128 q_w0(uint32_t w)   /* a register with w in word 0, for a helper whose word 0 alone is kept */
{
    u128 r;
    memset(&r, 0, sizeof r);
    r._u32[0] = w;
    return r;
}

static inline void add_lanes(u128* r, uint32_t n)
{
    for (int k = 0; k < 4; k++) r->_u32[k] += n;
}

/* selb(a, b, fcgt(a, b)): b in the lanes where a > b, else a. The masks are
 * whole lanes, so a blend of the same bits; spu_selb's 64-bit halves made the
 * compiler take each through general registers, on the loop's critical path. */
static inline u128 syn_sel_gt(u128 a, u128 b)
{
#ifdef SPU_FLOAT_SIMD
    const __m128 A = spu__ps(a), B = spu__ps(b);
    return spu__q(_mm_blendv_ps(A, B, _mm_cmpgt_ps(A, B)));
#else
    return spu_selb(a, b, spu_fcgt(a, b));
#endif
}

/* fm and fma on one lane, for a loop that keeps word 0 alone: spu_fm_ref's
 * and spu_fma_ref's rule and arithmetic for a lane (the SIMD forms give the
 * same bits, tests/test_spu_simd_paths.c). Four lanes in double cost about
 * three times as much, for three lanes the loop never reads. */
static inline float syn_f(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
static inline uint32_t syn_w(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }
static inline float syn_fm1(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }
static inline float syn_fma1(float a, float b, float c) { return (a == 0.0f || b == 0.0f) ? c : SPU__FMA(a, b, c); }

/* cflts(a, 165): each lane times 2^8 in double, clamped to int32, truncated.
 * spu_cflts does it a lane at a time; on x86 SSE2 does the four at once with
 * the same result for every input: min(MAX, x) and max(MIN, x) keep a NaN
 * (they return their second operand then), and a NaN converts to 0x80000000
 * as the scalar cast does. (sse2neon's min and max may not keep it: there the
 * scalar form.) */
static inline u128 syn_cflts165(u128 a)
{
#if defined(SPU_FLOAT_SIMD) && !defined(PS3_SSE2NEON)
    const __m128 A = spu__ps(a);
    const __m128d k = _mm_set1_pd(256.0), hi = _mm_set1_pd(2147483647.0), lo = _mm_set1_pd(-2147483648.0);
    const __m128d d0 = _mm_max_pd(lo, _mm_min_pd(hi, _mm_mul_pd(_mm_cvtps_pd(A), k)));
    const __m128d d1 = _mm_max_pd(lo, _mm_min_pd(hi, _mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(A, A)), k)));
    const __m128i v = _mm_unpacklo_epi64(_mm_cvttpd_epi32(d0), _mm_cvttpd_epi32(d1));
    u128 r;
    memcpy(&r, &v, 16);
    return r;
#else
    return spu_cflts(a, 165);
#endif
}

/* syn_cflts165's word 0 alone, by the same instructions on one lane. */
static inline int32_t syn_cflts165_w0(u128 a)
{
#if defined(SPU_FLOAT_SIMD) && !defined(PS3_SSE2NEON)
    const __m128d v = _mm_mul_sd(_mm_cvtss_sd(_mm_setzero_pd(), spu__ps(a)), _mm_set_sd(256.0));
    return _mm_cvttsd_si32(_mm_max_sd(_mm_set_sd(-2147483648.0), _mm_min_sd(_mm_set_sd(2147483647.0), v)));
#else
    return spu_cflts(a, 165)._s32[0];
#endif
}

/* ---- 0x0AC4-0x0AE0: the 112-byte clear --------------------------------
 *   lqx r15,r5,r4 ; cbx r16,r5,r4 ; shufb r14,r7,r15,r16 ; stqx r14,r5,r4
 *   ai r5,r5,1 ; ceqi r13,r5,112 ; brz r13,0xAC4
 * Each pass stores r7's low byte at r4 + r5. All but the last pass; the
 * lifted code runs the last. Writes r5 (r13-r16 are the pass's own). */
static uint32_t fill_run(spu_context* ctx)
{
    u128* r = ctx->gpr;
    const uint32_t x = r[5]._u32[0];
    if (x >= 111u) return 0;   /* one pass left, or past 112: the lifted loop would wrap */
    const uint32_t fast = 111u - x;
    const uint32_t base = r[4]._u32[0];
    const uint8_t v = (uint8_t)r[7]._u32[0];
    uint8_t* const ls = ctx->ls;
    const uint32_t off = (base + x) & SPU_LS_MASK;
    if (off + fast <= SPU_LS_MASK + 1u)
        memset(ls + off, v, fast);
    else
        for (uint32_t i = x; i < 111u; i++) ls[(base + i) & SPU_LS_MASK] = v;
    add_lanes(&r[5], fast);
    ctx->steps += fast;
    return fast;
}

/* ---- 0x2C20-0x2C50: the gain ramp --------------------------------------
 *   fa r30,r9,r5 ; lqd r32,0(r7) ; ai r8,r8,-1 ; cwd r28,0(r7)
 *   fcgt r31,r30,r13 ; rotqby r29,r32,r7 ; selb r5,r30,r13,r31
 *   fm r27,r29,r5 ; shufb r25,r27,r32,r28 ; stqd r25,0(r7) ; ai r7,r7,4
 *   brnz r8,0x2C20
 * r5 steps by r9 up to r13 (1.0 in the game), and each word at r7 is
 * multiplied by it. Writes r5, r7 and r8.
 * Once a pass leaves r5 as it found it (bit for bit), every later pass does
 * too: the ramp is done (in the game r5 has reached 1.0), and the rest of the
 * words only take the multiply by its word 0, with no pass waiting on the
 * last. */
static uint32_t gain_run(spu_context* ctx)
{
    u128* r = ctx->gpr;
    const uint32_t n = r[8]._u32[0];
    if (n < 2u || n > SYN_MAX_PASSES || (r[7]._u32[0] & 3u)) return 0;
    const uint32_t fast = n - 1u;
    const u128 r9 = r[9], r13 = r[13];
    u128 r5 = r[5];
    uint32_t p = r[7]._u32[0];
    uint8_t* const ls = ctx->ls;
    for (uint32_t i = 0; i < fast; i++, p += 4u) {
        const u128 r30 = spu_fa(r9, r5);
        const u128 n5 = syn_sel_gt(r30, r13);
        if (memcmp(&n5, &r5, sizeof r5) == 0) {
            const float g = r5._f32[0];
            const uint32_t cnt = fast - i, off = p & SPU_LS_MASK;
            if (off + 4u * cnt <= SPU_LS_MASK + 1u) {
                uint8_t* const q = ls + off;
                for (uint32_t k = 0; k < cnt; k++) {
                    uint32_t w;
                    memcpy(&w, q + 4u * k, 4);
                    w = SPU_BSWAP32(syn_w(syn_fm1(syn_f(SPU_BSWAP32(w)), g)));
                    memcpy(q + 4u * k, &w, 4);
                }
            } else {
                for (uint32_t k = 0; k < cnt; k++, p += 4u) ls_set_w(ls, p, syn_w(syn_fm1(syn_f(ls_w(ls, p)), g)));
            }
            if (syn_stats_on()) s_gain_flat += cnt;
            break;
        }
        r5 = n5;
        ls_set_w(ls, p, syn_w(syn_fm1(syn_f(ls_w(ls, p)), r5._f32[0])));
    }
    r[5] = r5;
    add_lanes(&r[7], fast * 4u);
    add_lanes(&r[8], 0u - fast);
    ctx->steps += fast;
    return fast;
}

/* ---- 0x63C8-0x6408: the pan --------------------------------------------
 *   ai r8,r8,1 ; lqd r52,0(r7) ; lqd r49,0xC00(r6) ; ceq r45,r82,r8
 *   lqd r50,0x1000(r6) ; rotqby r51,r52,r7 ; ai r7,r7,4
 *   shufb r48,r51,r51,r11 ; fma r47,r48,r9,r50 ; fma r46,r48,r10,r49
 *   stqd r47,0x1000(r6) ; stqd r46,0xC00(r6) ; ai r6,r6,16 ; brz r45,0x63C8
 * r11 splats word 0 (0x00010203 in each word). Writes r6, r7 and r8. */
static uint32_t pan_run(spu_context* ctx)
{
    u128* r = ctx->gpr;
    const uint32_t n = r[82]._u32[0] - r[8]._u32[0];
    if (n < 2u || n > SYN_MAX_PASSES || (r[7]._u32[0] & 3u)) return 0;
    for (int k = 0; k < 4; k++)
        if (r[11]._u32[k] != 0x00010203u) return 0;
    const uint32_t fast = n - 1u;
    const u128 r9 = r[9], r10 = r[10];
    uint32_t p = r[7]._u32[0], q = r[6]._u32[0];
    const uint8_t* const ls = ctx->ls;
    for (uint32_t i = 0; i < fast; i++, p += 4u, q += 16u) {
        const uint32_t s = ls_w(ls, p);
        u128 s4;
        s4._u32[0] = s4._u32[1] = s4._u32[2] = s4._u32[3] = s;
        const u128 r49 = spu_ls_read128(ctx, q + 0xC00u);
        const u128 r50 = spu_ls_read128(ctx, q + 0x1000u);
        spu_ls_write128(ctx, q + 0x1000u, spu_fma(s4, r9, r50));
        spu_ls_write128(ctx, q + 0xC00u, spu_fma(s4, r10, r49));
    }
    add_lanes(&r[6], fast * 16u);
    add_lanes(&r[7], fast * 4u);
    add_lanes(&r[8], fast);
    ctx->steps += fast;
    return fast;
}

/* ---- 0x1090: the resampler ---------------------------------------------
 * One output sample a pass (0x1090-0x1118): the table offsets from the
 * phase's fraction r88 (r90 + 4*(256+f), 4*f, 4*(511-f), 4*(255-f)), the
 * previous position r91 into word 3 of r80+0xA0, the phase r89 += r93, the
 * count r92 += 1 (the buffer ends when it reaches r85: 0x1260), then
 * fm/fma of the four history samples r86, r87, r82, r83 with the four table
 * words into the word at r81, and r81 += 4. At 0x1120: the new position and
 * fraction from cflts(r89, 165), and the step from the old position r11:
 *   1 (0x1138): if the input counter at r80+0x90 is past 27, refill (0x12B8);
 *     else the history moves up one (r87 = r86, r86 = r82, r82 = r83), r83 is
 *     the input sample at r80+0x10 + 4*counter, and the counter goes up one;
 *   0 or less (0x0FB0, to 0x1090): the history stays;
 *   2, 3, more: 0x1178, 0x11D4, the general path (lifted).
 * Registers the block writes: the loop's own (r81, r82, r83, r86, r87, r88,
 * r89, r91, r92), those written after the 0x1118 exit test (r64, r65) and those
 * of the step-1 and step-0 tails (r66-r72, r75, r43). The rest (r3-r28 but
 * r6, r84) every pass writes from 0x1090 on, before reading. */
static const uint8_t rs_regs[] = { 81, 82, 83, 86, 87, 88, 89, 91, 92, 64, 65, 66, 67, 68, 69, 70, 71, 72, 75, 43 };

static uint32_t resample_run(spu_context* ctx)
{
    u128* r = ctx->gpr;
    /* The stack pointer's quadword slots (cwd r1,0 and cwd r1,12) are words 0
     * and 3, the table's words are aligned, as the rotates read them, and so
     * is r80, so an input sample is the word at its own address. */
    if ((r[1]._u32[0] & 0xFu) || (r[90]._u32[0] & 3u) || (r[80]._u32[0] & 3u)) return 0;
    const u128 r80q = r[80];
    const uint32_t s80 = r80q._u32[0], s90 = r[90]._u32[0], s85 = r[85]._u32[0];
    const uint32_t a90 = s80 + 0x90u, q90 = a90 & SPU_LS_MASK & ~0xFu;
    const uint32_t aA0w = ((s80 + 0xA0u) & ~0xFu) + 12u;
    uint8_t* const ls = ctx->ls;
    /* In the loop, word 0 alone of what the loop reads, as lanes of their
     * own: the phase moves all four lanes (one addps, its other lanes are
     * registers the block leaves), the rest is word 0. The registers are
     * made whole once at the end, from the last passes' phases and the
     * last four input quadwords, exactly as the passes leave them. Kept
     * whole, the history and the lanes did not fit the CPU's registers and
     * went to the stack and back every pass. */
    const u128 r91_in = r[91];
    u128 r89 = r[89], r89_prev = r[89];
    const u128 r93 = r[93];
    uint32_t r81w = r[81]._u32[0], r92w = r[92]._u32[0];
    uint32_t f88 = r[88]._u32[0];
    int32_t p91 = (int32_t)r91_in._u32[0];
    float h86 = r[86]._f32[0], h87 = r[87]._f32[0], h82 = r[82]._f32[0], h83 = r[83]._f32[0];
    /* The input counter's quadword, as a step-1 pass reads it. Kept here: read
     * back right after the 4-byte store of the counter, a 16-byte load waits
     * for that store to reach the cache. Read again when an output store
     * lands in it. */
    u128 q90v = spu_ls_read128(ctx, a90);
    /* The last four step-1 passes' input quadwords and their r68, newest at
     * (m - 1) & 3, and the last such pass's r5. */
    u128 qin[4];
    uint32_t qa68[4] = { 0, 0, 0, 0 };
    u128 r5 = r[5];
    uint32_t m = 0;
    /* The last step-0 pass's phases before and after it, and whether it was
     * the first pass (its r91 before is then the register's). */
    u128 z89_before = r89, z89_after = r89;
    int z_first = 0;
    uint32_t done = 0, steps = 0, adv1 = 0;
    int why = RS_CAP, any0 = 0;
    for (; done < SYN_MAX_PASSES; done++) {
        if (r92w + 1u == s85) { why = RS_END; break; }
        const u128 n89 = spu_fa(r93, r89);
        const int32_t n65w = syn_cflts165_w0(n89);
        const int32_t n91w = n65w >> 8;
        const int32_t step = (int32_t)((uint32_t)n91w - (uint32_t)p91);
        if (step == 1) {
            if ((r81w & SPU_LS_MASK & ~0xFu) == q90) { why = RS_ALIAS; break; }
            if ((int32_t)q90v._u32[0] > 27) { why = RS_REFILL; break; }
        } else if (step > 0) {
            why = step == 2 ? RS_ADV2 : step == 3 ? RS_ADV3 : RS_ADVN;
            break;
        }
        /* 0x1090-0x1118: only word 0 of the fm/fma chain is stored. */
        const uint32_t a8 = (f88 << 2) + s90, a17 = ((511u - f88) << 2) + s90, a15 = ((255u - f88) << 2) + s90;
        ls_set_w(ls, aA0w, (uint32_t)p91);
        float t = syn_fm1(h86, syn_f(ls_w(ls, a8 + 0x400u)));
        t = syn_fma1(h87, syn_f(ls_w(ls, a8)), t);
        t = syn_fma1(h82, syn_f(ls_w(ls, a17)), t);
        t = syn_fma1(h83, syn_f(ls_w(ls, a15)), t);
        ls_set_w(ls, r81w, syn_w(t));
        if ((r81w & SPU_LS_MASK & ~0xFu) == q90) q90v = spu_ls_read128(ctx, a90);
        r92w++;
        r81w += 4u;
        r89_prev = r89;
        r89 = n89;
        p91 = n91w;
        f88 = (uint32_t)n65w & 255u;
        if (step == 1) {
            /* 0x1138-0x1174: r68 = (counter << 2) + r80; the quadword at
             * r68 + 0x10 rotated by r68 is the next input sample, its word 0
             * the word at r68 + 0x10; the counter goes up one in its word. */
            r5 = q90v;
            const uint32_t c = q90v._u32[0], a68 = (c << 2) + s80;
            qin[m & 3] = spu_ls_read128(ctx, a68 + 0x10u);
            qa68[m & 3] = a68;
            m++;
            h87 = h86;
            h86 = h82;
            h82 = h83;
            h83 = syn_f(ls_w(ls, a68 + 0x10u));
            ls_set_w(ls, q90, c + 1u);
            q90v._u32[0] = c + 1u;
            steps += 1;   /* the back edge to 0x1090 */
            adv1++;
        } else {
            /* 0x0FB0-0x0FC8 */
            z89_before = r89_prev;
            z89_after = n89;
            z_first = done == 0;
            any0 = 1;
            steps += 2;   /* to 0x0FB0, and back to 0x1090 */
        }
    }
    if (syn_stats_on()) {
        s_rs_adv[0] += done - adv1;
        s_rs_adv[1] += adv1;
        s_rs_stop[why]++;
    }
    if (!done) return 0;
    add_lanes(&r[81], 4u * done);
    add_lanes(&r[92], done);
    /* The history moves up one a step-1 pass: r87 = r86, r86 = r82,
     * r82 = r83, r83 = the input quadword rotated by its r68. */
    {
        u128 h[4] = { r[87], r[86], r[82], r[83] };   /* oldest first */
        const uint32_t n = m < 4 ? m : 4u;
        for (uint32_t k = 0; k < 4 - n; k++) h[k] = h[k + n];
        for (uint32_t k = 0; k < n; k++) {
            const uint32_t s = (m - n + k) & 3u;
            h[4 - n + k] = spu_rotqby(qin[s], q_w0(qa68[s]));
        }
        r[87] = h[0]; r[86] = h[1]; r[82] = h[2]; r[83] = h[3];
    }
    const u128 n65 = syn_cflts165(r89), n91 = spu_rotmai(n65, -8);
    const u128 b91 = done == 1 ? r91_in : spu_rotmai(syn_cflts165(r89_prev), -8);
    r[88] = spu_andi(n65, 255);
    r[89] = r89;
    r[91] = n91;
    r[64] = spu_ceqi(spu_sf(b91, n91), 1);
    r[65] = n65;
    if (m) {
        r[66] = spu_cgti(r5, 27);
        r[70] = spu_shli(r5, 2);
        r[71] = spu_cwd(r[1], 0);
        r[72] = spu_ai(r5, 1);
        r[68] = spu_a(r[70], r80q);
        u128 r69 = r5;
        r69._u32[0] = r[72]._u32[0];
        r[69] = r69;
        r[67] = qin[(m - 1) & 3];
    }
    if (any0) {
        const u128 zb = z_first ? r91_in : spu_rotmai(syn_cflts165(z89_before), -8);
        const u128 r3z = spu_sf(zb, spu_rotmai(syn_cflts165(z89_after), -8));
        r[75] = spu_ceqi(r3z, 2);
        r[43] = spu_cgti(r3z, 3);
    }
    ctx->steps += steps;
    return done;
}

/* ---- the check (SPU_NATIVE_SYNTH=2) ------------------------------------- */
static const uint8_t fill_regs[] = { 5 }, gain_regs[] = { 5, 7, 8 }, pan_regs[] = { 6, 7, 8 };

static uint32_t syn_run(spu_context* ctx, int which)
{
    switch (which) {
    case SYN_FILL: return fill_run(ctx);
    case SYN_GAIN: return gain_run(ctx);
    case SYN_PAN: return pan_run(ctx);
    default: return resample_run(ctx);
    }
}

/* The block on the context, the interpreter on a copy for as many passes (each
 * from the loop's first instruction back to it), then the registers the block
 * writes and the local store compared. */
static void syn_check(spu_context* ctx, int which)
{
    static uint8_t* ls_copy;
    static unsigned long long checked[SYN_N], bad[SYN_N];
    const uint8_t* regs;
    int nregs;
    switch (which) {
    case SYN_FILL: regs = fill_regs; nregs = (int)sizeof fill_regs; break;
    case SYN_GAIN: regs = gain_regs; nregs = (int)sizeof gain_regs; break;
    case SYN_PAN: regs = pan_regs; nregs = (int)sizeof pan_regs; break;
    default: regs = rs_regs; nregs = (int)sizeof rs_regs; break;
    }
    if (!ls_copy && !(ls_copy = (uint8_t*)malloc(SPU_LS_SIZE))) { syn_run(ctx, which); return; }
    spu_context* c = (spu_context*)malloc(sizeof *c);
    if (!c) { syn_run(ctx, which); return; }
    memcpy(c, ctx, sizeof *c);
    memcpy(ls_copy, ctx->ls, SPU_LS_SIZE);
    c->ls = ls_copy;
    /* An image id nothing registered, as spu_diff_hook.c's check: the
     * interpreter finds no lifted function to rejoin. */
    c->image_id = 0x7FFF;
    const uint32_t passes = syn_run(ctx, which);
    const uint32_t a = syn_start[which];
    for (uint32_t i = 0; i < passes; i++) {
        if (spu_interp_run_until(c, a, a + 4u) != a + 4u || spu_interp_run_until(c, a + 4u, a) != a) {
            static int said;
            if (!said++)
                fprintf(stderr, "[spu-synth] %s check skipped: the interpreter stopped at 0x%X on pass %u\n",
                        syn_name[which], (unsigned)c->pc, i);
            free(c);
            return;
        }
    }
    if (!passes) { free(c); return; }
    checked[which]++;
    int reg = -1;
    for (int k = 0; k < nregs && reg < 0; k++)
        if (memcmp(&c->gpr[regs[k]], &ctx->gpr[regs[k]], sizeof(u128))) reg = regs[k];
    unsigned lsoff = SPU_LS_SIZE;   /* none */
    if (reg < 0 && memcmp(ls_copy, ctx->ls, SPU_LS_SIZE))
        for (unsigned i = 0; i < SPU_LS_SIZE; i++)
            if (ls_copy[i] != ctx->ls[i]) { lsoff = i; break; }
    const int mism = reg >= 0 || lsoff != SPU_LS_SIZE;
    if (mism) {
        bad[which]++;
        s_syn_bad[which] = 1;
        fprintf(stderr, "[spu-synth] %s MISMATCH after %u passes: register %d, LS 0x%X (0x40000: none); the "
                "block is off for the session\n", syn_name[which], passes, reg, lsoff);
        if (reg >= 0)
            fprintf(stderr, "[spu-synth]   r%d block %08X %08X %08X %08X, interpreter %08X %08X %08X %08X\n", reg,
                    ctx->gpr[reg]._u32[0], ctx->gpr[reg]._u32[1], ctx->gpr[reg]._u32[2], ctx->gpr[reg]._u32[3],
                    c->gpr[reg]._u32[0], c->gpr[reg]._u32[1], c->gpr[reg]._u32[2], c->gpr[reg]._u32[3]);
    }
    if (checked[which] == 1 || (checked[which] & (checked[which] - 1)) == 0 || mism)
        fprintf(stderr, "[spu-synth] %s: %llu checked, %llu mismatches\n", syn_name[which], checked[which],
                bad[which]);
    free(c);
}

static int syn_hook(spu_context* ctx, int which)
{
    const int mode = syn_mode();
    if (!mode || s_syn_bad[which] || !(syn_mask() & (1 << which)) || syn_slow(ctx)) return 0;
    if (mode == 2) {
        static unsigned tick[SYN_N];
        if (++tick[which] % syn_every() == 0) {
            syn_check(ctx, which);
            return 0;
        }
    }
    const uint32_t n = syn_run(ctx, which);
    if (syn_stats_on()) {
        s_calls[which]++;
        s_passes[which] += n;
        syn_stats_tick();
    }
    return 0;
}

int spu_hook_synth_fill(spu_context* ctx) { return syn_hook(ctx, SYN_FILL); }
int spu_hook_synth_gain(spu_context* ctx) { return syn_hook(ctx, SYN_GAIN); }
int spu_hook_synth_pan(spu_context* ctx) { return syn_hook(ctx, SYN_PAN); }
int spu_hook_synth_resample(spu_context* ctx) { return syn_hook(ctx, SYN_RESAMPLE); }
