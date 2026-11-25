/*
 * spu_synth_hook.c's four blocks against the lifted statements they stand
 * for, on random contexts and local stores.
 *
 * For each block: copy A runs the block and then one pass as lifted (what
 * the lifted code does after a block returns 0); copy B runs the lifted
 * passes alone until the same point. Every register, the step count and the
 * whole local store must agree. The lifted passes here are the statements
 * spu_lifter.py --regions writes for those loops, with each SPU_RGOTO as a
 * step and a return.
 *
 * The resampler's states are drawn so that its passes step 0 or 1 samples
 * (a phase step of at most 1), with some at 1-3 to reach the other paths, and
 * with the buffer's end, the input refill and the output word in the input
 * counter's quadword all in reach. A block that stops before a pass it could
 * do is a failure too.
 *
 * Build (x86-64; on arm64 leave out -msse4.1):
 *   clang -std=gnu17 -msse4.1 -I include -I runtime/spu \
 *         -o /tmp/test_spu_synth_hook runtime/spu/tests/test_spu_synth_hook.c -lm
 *
 * Exit code: 0 if all passed, 1 if any failed.
 */
#include "../spu_synth_hook.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__SSE4_1__) && !defined(PS3_SSE2NEON)
#error "the SIMD paths are off: build with -msse4.1 on x86-64, or for arm64"
#endif

/* What the blocks read and the test never sets. */
volatile int   g_spu_goto_slow;
volatile int   g_yz_lockstep_on;
void* volatile g_pm_flow_ctx;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
uint32_t spu_interp_run_until(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa)
{
    (void)ctx; (void)start_lsa;
    return stop_lsa;
}

static int g_fail, g_pass;
static uint32_t s_seed = 20261125u;
static uint32_t rnd(void) { s_seed = s_seed * 1103515245u + 12345u; return s_seed >> 8; }
static uint32_t rnd_w(void) { return (rnd() << 16) ^ rnd(); }
static u128 rnd_q(void) { u128 r; for (int k = 0; k < 4; k++) r._u32[k] = rnd_w(); return r; }
static float rnd_f(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }
static uint32_t f_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static u128 splat(uint32_t w) { u128 r; r._u32[0] = r._u32[1] = r._u32[2] = r._u32[3] = w; return r; }
static u128 splat_f(float f) { return splat(f_bits(f)); }
static void ls_put_w(uint8_t* ls, uint32_t a, uint32_t v) { ls_set_w(ls, a, v); }

/* A float word now and then zero, or a denormal, for the zero rule. */
static uint32_t rnd_sample(void)
{
    switch (rnd() & 15) {
    case 0: return 0;
    case 1: return 0x80000000u;
    case 2: return rnd() & 0x007FFFFFu;
    default: return f_bits(rnd_f(-1.0f, 1.0f));
    }
}

/* ---- the lifted passes ------------------------------------------------- */

static int ref_fill(spu_context* ctx)   /* 0: back to 0xAC4 */
{
    u128* g = ctx->gpr;
    g[15] = spu_ls_read128(ctx, g[5]._u32[0] + g[4]._u32[0]);
    g[16] = spu_cbx(g[5], g[4]);
    g[14] = spu_shufb(g[7], g[15], g[16]);
    spu_ls_write128(ctx, g[5]._u32[0] + g[4]._u32[0], g[14]);
    g[5] = spu_ai(g[5], 1);
    g[13] = spu_ceqi(g[5], 112);
    if (g[13]._u32[0] == 0) { ctx->steps++; return 0; }
    return 1;
}

static int ref_gain(spu_context* ctx)   /* 0: back to 0x2C20 */
{
    u128* g = ctx->gpr;
    g[30] = spu_fa(g[9], g[5]);
    g[32] = spu_ls_read128(ctx, g[7]._u32[0] + 0x0);
    g[8] = spu_ai(g[8], -1);
    g[28] = spu_cwd(g[7], 0);
    g[31] = spu_fcgt(g[30], g[13]);
    g[29] = spu_rotqby(g[32], g[7]);
    g[5] = spu_selb(g[30], g[13], g[31]);
    g[27] = spu_fm(g[29], g[5]);
    g[25] = spu_shufb(g[27], g[32], g[28]);
    spu_ls_write128(ctx, g[7]._u32[0] + 0x0, g[25]);
    g[7] = spu_ai(g[7], 4);
    if (g[8]._u32[0] != 0) { ctx->steps++; return 0; }
    return 1;
}

static int ref_pan(spu_context* ctx)   /* 0: back to 0x63C8 */
{
    u128* g = ctx->gpr;
    g[8] = spu_ai(g[8], 1);
    g[52] = spu_ls_read128(ctx, g[7]._u32[0] + 0x0);
    g[49] = spu_ls_read128(ctx, g[6]._u32[0] + 0xC00);
    g[45] = spu_ceq(g[82], g[8]);
    g[50] = spu_ls_read128(ctx, g[6]._u32[0] + 0x1000);
    g[51] = spu_rotqby(g[52], g[7]);
    g[7] = spu_ai(g[7], 4);
    g[48] = spu_shufb(g[51], g[51], g[11]);
    g[47] = spu_fma(g[48], g[9], g[50]);
    g[46] = spu_fma(g[48], g[10], g[49]);
    spu_ls_write128(ctx, g[6]._u32[0] + 0x1000, g[47]);
    spu_ls_write128(ctx, g[6]._u32[0] + 0xC00, g[46]);
    g[6] = spu_ai(g[6], 16);
    if (g[45]._u32[0] == 0) { ctx->steps++; return 0; }
    return 1;
}

/* 0: back to 0x1090 (0x1174's back edge or 0x0FC4's); else where it leaves. */
static uint32_t ref_resample(spu_context* ctx)
{
    u128* g = ctx->gpr;
    g[23] = spu_shli(g[88], 2);
    g[26] = spu_ls_read128(ctx, g[80]._u32[0] + 0xA0);
    g[28] = spu_sfi(g[88], 511);
    g[27] = spu_cwd(g[1], 12);
    g[25] = spu_sfi(g[88], 255);
    g[10] = spu_cwd(g[81], 0);
    g[22] = spu_shli(g[28], 2);
    g[11] = spu_shlqbyi(g[91], 0);
    g[8] = spu_a(g[23], g[90]);
    g[21] = spu_shli(g[25], 2);
    g[92] = spu_ai(g[92], 1);
    g[24] = spu_shufb(g[91], g[26], g[27]);
    g[17] = spu_a(g[22], g[90]);
    g[89] = spu_fa(g[93], g[89]);
    g[15] = spu_a(g[21], g[90]);
    g[88] = spu_ceq(g[85], g[92]);
    spu_ls_write128(ctx, g[80]._u32[0] + 0xA0, g[24]);
    g[20] = spu_ls_read128(ctx, g[8]._u32[0] + 0x400);
    g[18] = spu_ls_read128(ctx, g[23]._u32[0] + g[90]._u32[0]);
    g[16] = spu_ls_read128(ctx, g[22]._u32[0] + g[90]._u32[0]);
    g[5] = spu_ls_read128(ctx, g[21]._u32[0] + g[90]._u32[0]);
    g[12] = spu_ls_read128(ctx, g[81]._u32[0] + 0x0);
    g[19] = spu_rotqby(g[20], g[8]);
    g[14] = spu_rotqby(g[18], g[8]);
    g[4] = spu_rotqby(g[16], g[17]);
    g[9] = spu_rotqby(g[5], g[15]);
    g[84] = spu_fm(g[86], g[19]);
    g[13] = spu_fma(g[87], g[14], g[84]);
    g[7] = spu_fma(g[82], g[4], g[13]);
    g[3] = spu_fma(g[83], g[9], g[7]);
    g[91] = spu_shufb(g[3], g[12], g[10]);
    spu_ls_write128(ctx, g[81]._u32[0] + 0x0, g[91]);
    if (g[88]._u32[0] != 0) { ctx->steps++; return 0x1260; }
    g[81] = spu_ai(g[81], 4);
    g[84] = spu_shlqbyi(g[82], 0);
    g[65] = spu_cflts(g[89], 165);
    g[91] = spu_rotmai(g[65], -8);
    g[88] = spu_andi(g[65], 255);
    g[3] = spu_sf(g[11], g[91]);
    g[64] = spu_ceqi(g[3], 1);
    if (g[64]._u32[0] == 0) {
        ctx->steps++;
        g[75] = spu_ceqi(g[3], 2);
        if (g[75]._u32[0] != 0) { ctx->steps++; return 0x1178; }
        g[18] = spu_ceqi(g[3], 3);
        if (g[18]._u32[0] != 0) { ctx->steps++; return 0x11D4; }
        g[43] = spu_cgti(g[3], 3);
        if (g[43]._u32[0] == 0) { ctx->steps++; return 0; }
        return 0x0FC8;
    }
    g[5] = spu_ls_read128(ctx, g[80]._u32[0] + 0x90);
    g[66] = spu_cgti(g[5], 27);
    if (g[66]._u32[0] != 0) { ctx->steps++; return 0x12B8; }
    g[70] = spu_shli(g[5], 2);
    g[71] = spu_cwd(g[1], 0);
    g[72] = spu_ai(g[5], 1);
    g[87] = spu_shlqbyi(g[86], 0);
    g[86] = spu_ori(g[82], 0);
    g[82] = spu_shlqbyi(g[83], 0);
    g[68] = spu_a(g[70], g[80]);
    g[69] = spu_shufb(g[72], g[5], g[71]);
    g[67] = spu_ls_read128(ctx, g[68]._u32[0] + 0x10);
    spu_ls_write128(ctx, g[80]._u32[0] + 0x90, g[69]);
    g[83] = spu_rotqby(g[67], g[68]);
    ctx->steps++;
    return 0;
}

/* ---- the comparison ---------------------------------------------------- */

static uint8_t *s_ls_a, *s_ls_b;

static int same(const spu_context* a, const spu_context* b, const char* what, int trial)
{
    int bad = 0;
    for (int k = 0; k < 128; k++)
        if (memcmp(&a->gpr[k], &b->gpr[k], 16)) {
            if (bad++ < 3)
                fprintf(stderr, "  %s #%d: r%d %08X %08X %08X %08X, lifted %08X %08X %08X %08X\n", what, trial, k,
                        a->gpr[k]._u32[0], a->gpr[k]._u32[1], a->gpr[k]._u32[2], a->gpr[k]._u32[3],
                        b->gpr[k]._u32[0], b->gpr[k]._u32[1], b->gpr[k]._u32[2], b->gpr[k]._u32[3]);
        }
    if (a->steps != b->steps) {
        bad++;
        fprintf(stderr, "  %s #%d: steps %llu, lifted %llu\n", what, trial, (unsigned long long)a->steps,
                (unsigned long long)b->steps);
    }
    if (memcmp(a->ls, b->ls, SPU_LS_SIZE)) {
        bad++;
        for (unsigned i = 0; i < SPU_LS_SIZE; i++)
            if (a->ls[i] != b->ls[i]) { fprintf(stderr, "  %s #%d: LS 0x%X\n", what, trial, i); break; }
    }
    return bad;
}

static void fresh(spu_context* c)
{
    memset(c, 0, sizeof *c);
    for (int k = 0; k < 128; k++) c->gpr[k] = rnd_q();
    for (unsigned i = 0; i < SPU_LS_SIZE; i += 4) ls_put_w(s_ls_a, i, rnd_w());
    c->ls = s_ls_a;
    c->steps = rnd_w();
}

/* The block on A, then the lifted pass after it; the lifted passes alone on B. */
static int run_one(spu_context* a, const char* what, int trial, uint32_t (*block)(spu_context*),
                   uint32_t (*ref)(spu_context*), uint32_t* passes)
{
    static spu_context b;
    memcpy(&b, a, sizeof b);
    memcpy(s_ls_b, s_ls_a, SPU_LS_SIZE);
    b.ls = s_ls_b;
    const uint32_t n = block(a);
    ref(a);
    uint32_t i = 0, r = 0;
    for (; i <= n; i++) {
        r = ref(&b);
        if (r != 0 && i < n) {
            fprintf(stderr, "  %s #%d: the block did %u passes, the lifted loop left after %u\n", what, trial, n, i + 1);
            return 1;
        }
    }
    *passes = n;
    return same(a, &b, what, trial);
}

static uint32_t fill_ref(spu_context* c) { return (uint32_t)ref_fill(c); }
static uint32_t gain_ref(spu_context* c) { return (uint32_t)ref_gain(c); }
static uint32_t pan_ref(spu_context* c) { return (uint32_t)ref_pan(c); }

static void test_fill(void)
{
    static spu_context a;
    int bad = 0;
    unsigned long long total = 0;
    for (int t = 0; t < 400; t++) {
        fresh(&a);
        a.gpr[5] = splat(t < 300 ? rnd() % 112 : (t & 1 ? 110 + (rnd() & 3) : rnd_w()));
        a.gpr[4] = splat((rnd() % (SPU_LS_SIZE - 0x100)) & ~0xFu);
        if (t & 2) a.gpr[4]._u32[0] = SPU_LS_SIZE - 40;   /* wraps past the end */
        uint32_t n;
        if ((a.gpr[5]._u32[0] >= 112 && a.gpr[5]._u32[0] < 0x1000u) || a.gpr[5]._u32[0] > 0x10000000u) {
            /* the lifted loop would run to the wrap: only check the block declines */
            spu_context b = a;
            if (fill_run(&b)) { fprintf(stderr, "  clear #%d: ran past 112\n", t); bad++; }
            continue;
        }
        bad += run_one(&a, "clear", t, fill_run, fill_ref, &n);
        total += n;
    }
    printf("clear: %llu native passes\n", total);
    if (bad) { g_fail++; fprintf(stderr, "FAIL: clear: %d mismatches\n", bad); } else g_pass++;
}

static void test_gain(void)
{
    static spu_context a;
    int bad = 0;
    unsigned long long total = 0;
    for (int t = 0; t < 400; t++) {
        fresh(&a);
        a.gpr[8] = splat(t < 20 ? (uint32_t)t : 1 + rnd() % 300);
        a.gpr[7] = splat(((rnd() % (SPU_LS_SIZE - 0x2000)) & ~3u) | (t % 50 == 0 ? 1u : 0u));
        a.gpr[13] = splat_f(1.0f);
        for (int k = 0; k < 4; k++) {
            a.gpr[5]._u32[k] = f_bits(rnd_f(0.0f, 1.2f));
            a.gpr[9]._u32[k] = t % 7 == 0 ? rnd_sample() : f_bits(rnd_f(-0.01f, 0.02f));
        }
        for (uint32_t i = 0; i < 1300; i++) ls_put_w(s_ls_a, a.gpr[7]._u32[0] + 4 * i, rnd_sample());
        uint32_t n;
        bad += run_one(&a, "gain", t, gain_run, gain_ref, &n);
        total += n;
    }
    printf("gain: %llu native passes\n", total);
    if (bad) { g_fail++; fprintf(stderr, "FAIL: gain: %d mismatches\n", bad); } else g_pass++;
}

static void test_pan(void)
{
    static spu_context a;
    int bad = 0;
    unsigned long long total = 0;
    for (int t = 0; t < 400; t++) {
        fresh(&a);
        const uint32_t n0 = rnd_w();
        a.gpr[8] = splat(n0);
        a.gpr[82] = splat(n0 + (t < 20 ? (uint32_t)t : 1 + rnd() % 200));
        a.gpr[11] = splat(t % 40 == 0 ? 0x10203u + 1u : 0x00010203u);
        a.gpr[7] = splat(((rnd() % (SPU_LS_SIZE - 0x2000)) & ~3u) | (t % 50 == 1 ? 2u : 0u));
        a.gpr[6] = splat((rnd() % (SPU_LS_SIZE - 0x5000)) & ~0xFu);
        if (t % 5 == 0) a.gpr[6]._u32[0] = (a.gpr[7]._u32[0] & ~0xFu) - 0x1000u;   /* the sums over the samples */
        for (int k = 0; k < 4; k++) {
            a.gpr[9]._u32[k] = rnd_sample();
            a.gpr[10]._u32[k] = rnd_sample();
        }
        for (uint32_t i = 0; i < 1024; i++) ls_put_w(s_ls_a, a.gpr[7]._u32[0] + 4 * i, rnd_sample());
        for (uint32_t i = 0; i < 4096; i++) {
            ls_put_w(s_ls_a, a.gpr[6]._u32[0] + 0xC00 + 4 * i, rnd_sample());
            ls_put_w(s_ls_a, a.gpr[6]._u32[0] + 0x1000 + 4 * i, rnd_sample());
        }
        uint32_t n;
        bad += run_one(&a, "pan", t, pan_run, pan_ref, &n);
        total += n;
    }
    printf("pan: %llu native passes\n", total);
    if (bad) { g_fail++; fprintf(stderr, "FAIL: pan: %d mismatches\n", bad); } else g_pass++;
}

static void test_resample(void)
{
    static spu_context a;
    int bad = 0;
    unsigned long long total = 0, stops[8] = {0};
    for (int t = 0; t < 3000; t++) {
        fresh(&a);
        const uint32_t sp = 0x3F000u, v80 = 0x20000u + 16u * (rnd() % 64), v90 = 0x30000u + 4u * (rnd() % 64);
        a.gpr[1] = splat(t % 97 == 0 ? sp + 4 : sp);
        a.gpr[80] = splat(v80);
        a.gpr[90] = splat(t % 89 == 0 ? v90 + 2 : v90);
        a.gpr[81] = splat(0x10000u + 4u * (rnd() % 256));
        if (t % 31 == 0) a.gpr[81] = splat(v80 + 0x90u - 4u * (rnd() % 3));   /* the output reaches the counter */
        const uint32_t n92 = rnd_w();
        a.gpr[92] = splat(n92);
        a.gpr[85] = splat(n92 + 1 + rnd() % 300);
        const float step = t % 5 == 0 ? 1.0f : t % 11 == 0 ? rnd_f(1.0f, 3.5f) : rnd_f(0.05f, 1.0f);
        a.gpr[93] = splat_f(step);
        for (int k = 1; k < 4; k++) a.gpr[93]._u32[k] = f_bits(rnd_f(0.0f, 2.0f));
        const float ph = rnd_f(0.0f, 4000.0f);
        a.gpr[89] = splat_f(ph);
        for (int k = 1; k < 4; k++) a.gpr[89]._u32[k] = f_bits(rnd_f(-100.0f, 100.0f));
        /* the position and fraction the last pass left: from the phase */
        a.gpr[65] = spu_cflts(a.gpr[89], 165);
        a.gpr[91] = spu_rotmai(a.gpr[65], -8);
        a.gpr[88] = spu_andi(a.gpr[65], 255);
        if (t % 13 == 0) a.gpr[88] = rnd_q();
        for (int k = 0; k < 4; k++) {
            a.gpr[86]._u32[k] = rnd_sample(); a.gpr[87]._u32[k] = rnd_sample();
            a.gpr[82]._u32[k] = rnd_sample(); a.gpr[83]._u32[k] = rnd_sample();
        }
        for (uint32_t i = 0; i < 512; i++) ls_put_w(s_ls_a, v90 + 4 * i, rnd_sample());
        for (uint32_t i = 0; i < 28; i++) ls_put_w(s_ls_a, v80 + 0x10 + 4 * i, rnd_sample());
        ls_put_w(s_ls_a, v80 + 0x90, t % 17 == 0 ? rnd_w() : rnd() % 30);
        uint32_t n;
        bad += run_one(&a, "resample", t, resample_run, ref_resample, &n);
        total += n;
        stops[n == 0 ? 0 : n < 8 ? 1 : n < 64 ? 2 : 3]++;
    }
    printf("resample: %llu native passes (calls doing 0 / 1-7 / 8-63 / 64+: %llu %llu %llu %llu)\n", total,
           stops[0], stops[1], stops[2], stops[3]);
    if (bad) { g_fail++; fprintf(stderr, "FAIL: resample: %d mismatches\n", bad); } else g_pass++;
}

int main(void)
{
    s_ls_a = (uint8_t*)malloc(SPU_LS_SIZE);
    s_ls_b = (uint8_t*)malloc(SPU_LS_SIZE);
    if (!s_ls_a || !s_ls_b) return 1;
    test_fill();
    test_gain();
    test_pan();
    test_resample();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
