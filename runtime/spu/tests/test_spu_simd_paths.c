/*
 * The SPU runtime's SIMD paths against their scalar forms, on the host the
 * test is built for: x86-64 with -msse4.1, or arm64, where the same SSE
 * intrinsics come from sse2neon (include/ps3emu/ps3_sse.h). Built without
 * either it stops at the #error below rather than compare a loop with itself.
 *
 *   - spu_shufb against spu_shufb_ref;
 *   - the quadword byte shifts and rotates, and the cbd/chd/cwd/cdd controls,
 *     against byte loops;
 *   - spu_diff_hook.c's block: diff_run(ctx, 1) against diff_run(ctx, 0);
 *   - spu_scan_hook.c's block: scan_run(ctx, &done, 1) against
 *     scan_run(ctx, &done, 0), on tables built so that runs of entries skip.
 *
 * The two blocks run on copies of one random context and local store, and
 * every register, the step count and the whole local store must agree.
 *
 * Build (x86-64; on arm64 leave out -msse4.1):
 *   clang -std=gnu17 -msse4.1 -I include -I runtime/spu \
 *         -o /tmp/test_spu_simd_paths runtime/spu/tests/test_spu_simd_paths.c
 *
 * Exit code: 0 if all passed, 1 if any failed.
 */
#include "../spu_diff_hook.c"
#include "../spu_scan_hook.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__SSE4_1__) && !defined(PS3_SSE2NEON)
#error "the SIMD paths are off: build with -msse4.1 on x86-64, or for arm64"
#endif
#if !defined(DIFF_HAVE_SSE) || !defined(SCAN_HAVE_SSE)
#error "a block was built without its SIMD path"
#endif

/* What the hooks read and the test never sets. */
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

static uint32_t s_seed = 12345u;
static uint32_t rnd(void) { s_seed = s_seed * 1103515245u + 12345u; return s_seed >> 8; }
static u128 rnd_q(void) { u128 r; for (int k = 0; k < 4; k++) r._u32[k] = (rnd() << 16) ^ rnd(); return r; }
static int q_eq(u128 a, u128 b) { return memcmp(&a, &b, 16) == 0; }

static void check(const char* what, int bad)
{
    if (bad) { g_fail++; fprintf(stderr, "FAIL: %s: %d mismatches\n", what, bad); }
    else g_pass++;
}

/* ---- spu_helpers.h ---- */
static u128 ref_qb_shl(u128 a, int sh)
{
    u128 r = spu_zero();
    if (sh >= 16) return r;
    for (int i = 0; i < 16; i++) { int s = i + sh; if (s < 16) r._u8[SPU_W(i)] = a._u8[SPU_W(s)]; }
    return r;
}
static u128 ref_qb_rot(u128 a, int sh)
{
    u128 r;
    for (int i = 0; i < 16; i++) r._u8[SPU_W(i)] = a._u8[SPU_W((i + sh) & 0x0F)];
    return r;
}
static u128 ref_qb_shr(u128 a, int sh)
{
    u128 r = spu_zero();
    if (sh >= 16) return r;
    for (int i = 0; i < 16; i++) { int s = i - sh; if (s >= 0) r._u8[SPU_W(i)] = a._u8[SPU_W(s)]; }
    return r;
}
static u128 ref_ctl(int pos, int size)
{
    u128 r; for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~(size - 1);
    for (int k = 0; k < size; k++) r._u8[SPU_W(p + k)] = (uint8_t)((size == 8 ? 0 : 4 - size) + k);
    return r;
}

static void test_helpers(void)
{
    int bad = 0;
    for (int it = 0; it < 200000; it++) {
        const u128 a = rnd_q(), b = rnd_q(), c = rnd_q();   /* every selector class */
        bad += !q_eq(spu_shufb(a, b, c), spu_shufb_ref(a, b, c));
    }
    check("shufb vs the byte loop", bad);

    int qb[13] = {0};
    for (int it = 0; it < 64; it++) {
        const u128 a = rnd_q();
        for (int v = -300; v < 300; v++) {
            const uint32_t w = (uint32_t)v + (rnd() << 16);   /* junk in the count's unused bits */
            u128 b = spu_zero(); b._u32[0] = w;
            qb[0] += !q_eq(spu_shlqby(a, b),    ref_qb_shl(a, w & 0x1F));
            qb[1] += !q_eq(spu_rotqby(a, b),    ref_qb_rot(a, w & 0x0F));
            qb[2] += !q_eq(spu_shlqbybi(a, b),  ref_qb_shl(a, (w >> 3) & 0x1F));
            qb[3] += !q_eq(spu_rotqbybi(a, b),  ref_qb_rot(a, (w >> 3) & 0x0F));
            qb[4] += !q_eq(spu_rotqmby(a, b),   ref_qb_shr(a, (0 - (int)w) & 0x1F));
            qb[5] += !q_eq(spu_rotqmbybi(a, b), ref_qb_shr(a, (0 - ((int)w >> 3)) & 0x1F));
            qb[6] += !q_eq(spu_shlqbyi(a, v),   ref_qb_shl(a, v & 0x1F));
            qb[7] += !q_eq(spu_rotqbyi(a, v),   ref_qb_rot(a, v & 0x0F));
            qb[8] += !q_eq(spu_rotqmbyi(a, v),  ref_qb_shr(a, (0 - v) & 0x1F));
            qb[9]  += !q_eq(spu_cbd_pos(v), ref_ctl(v, 1));
            qb[10] += !q_eq(spu_chd_pos(v), ref_ctl(v, 2));
            qb[11] += !q_eq(spu_cwd_pos(v), ref_ctl(v, 4));
            qb[12] += !q_eq(spu_cdd_pos(v), ref_ctl(v, 8));
        }
    }
    static const char* nm[13] = { "shlqby", "rotqby", "shlqbybi", "rotqbybi", "rotqmby", "rotqmbybi",
                                  "shlqbyi", "rotqbyi", "rotqmbyi", "cbd", "chd", "cwd", "cdd" };
    for (int i = 0; i < 13; i++) {
        char what[64];
        snprintf(what, sizeof what, "%s vs the byte loop", nm[i]);
        check(what, qb[i]);
    }
}

/* ---- the two blocks, on a pair of contexts ---- */
static spu_context *s_ctx[2];
static uint8_t* s_ls[2];

static void fill_ls(void)
{
    for (uint32_t i = 0; i < SPU_LS_SIZE; i += 4) {
        const uint32_t v = (rnd() << 16) ^ rnd();
        memcpy(s_ls[0] + i, &v, 4);
    }
}

static void fill_gpr(void)
{
    for (int k = 0; k < 128; k++) s_ctx[0]->gpr[k] = rnd_q();
}

/* ctx 1 becomes a copy of ctx 0, local store and all. */
static void copy_ctx(void)
{
    memcpy(s_ctx[1], s_ctx[0], sizeof *s_ctx[0]);
    s_ctx[1]->ls = s_ls[1];
    memcpy(s_ls[1], s_ls[0], SPU_LS_SIZE);
}

/* Nonzero when the two contexts differ anywhere. */
static int ctx_differ(void)
{
    return memcmp(s_ctx[0]->gpr, s_ctx[1]->gpr, sizeof s_ctx[0]->gpr) != 0 ||
           s_ctx[0]->steps != s_ctx[1]->steps || memcmp(s_ls[0], s_ls[1], SPU_LS_SIZE) != 0;
}

static void splat(u128* q, uint32_t v) { for (int k = 0; k < 4; k++) q->_u32[k] = v; }

static void test_diff(void)
{
    int bad = 0, ran = 0;
    for (int it = 0; it < 400; it++) {
        fill_ls();
        fill_gpr();
        u128* r = s_ctx[0]->gpr;
        const uint32_t a = (rnd() % SPU_LS_SIZE) & ~0xFu, b = (rnd() % SPU_LS_SIZE) & ~0xFu;
        splat(&r[22], a);
        splat(&r[67], b);
        splat(&r[23], it % 8 == 7 ? 32 + rnd() % 4 : rnd() % 32);   /* the passes left; 32 and up run lifted */
        /* About half the quadword pairs equal, so both sides of each compare run. The
         * passes walk down from A and B by 64 bytes; this covers them all. */
        for (int x = -2048 - 64; x < 0x840; x += 16)
            if (rnd() & 1)
                memcpy(s_ls[0] + ((a + x) & SPU_LS_MASK), s_ls[0] + ((b + x) & SPU_LS_MASK), 16);
        copy_ctx();
        const int x0 = diff_run(s_ctx[0], 1), x1 = diff_run(s_ctx[1], 0);
        ran += x0;
        bad += x0 != x1 || ctx_differ();
    }
    check("diff block, SIMD vs scalar", bad);
    printf("  diff: %d of 400 contexts ran the block\n", ran);
    if (!ran) check("diff block ran", 1);
}

#define T30 0x1000u   /* r30's table, 32 bytes an entry */
#define T31 0x2000u   /* r31's and r32's, 96 */
#define T32 0x3000u

static void test_scan(void)
{
    int bad = 0, skipped = 0, ended = 0;
    for (int it = 0; it < 2000; it++) {
        fill_ls();
        fill_gpr();
        u128* r = s_ctx[0]->gpr;
        const uint32_t c = it % 16 == 15 ? SCAN_N + rnd() % 4 : rnd() % SCAN_N;
        splat(&r[37], c);
        splat(&r[30], T30 + 32 * c);
        splat(&r[31], T31 + 96 * c);
        splat(&r[32], T32 + 96 * c);
        /* Each entry from the head on goes to no handler with a chance of 7 in 8:
         * r32's entry is made to hold what the entry builds, and r22's test is
         * passed one of its two ways. */
        const scan_consts k = { r[40], r[41], r[42], r[43], r[44] };
        for (uint32_t e = c; e < SCAN_N; e++) {
            if (rnd() % 8 == 0) continue;
            const u128 p30 = spu_ai(r[30], 32 * (e - c)), p31 = spu_ai(r[31], 96 * (e - c)),
                       p32 = spu_ai(r[32], 96 * (e - c));
            const uint32_t at = p32._u32[0];
            if (rnd() & 1) {   /* r19's preferred word 0 */
                u128 q19 = spu_ls_read128(s_ctx[0], p30._u32[0] + 0x10);
                q19._u32[0] = 0;
                spu_ls_write128(s_ctx[0], p30._u32[0] + 0x10, q19);
            } else {           /* r60 = r19 and r62 = r17 */
                spu_ls_write128(s_ctx[0], at + 0x30, spu_ls_read128(s_ctx[0], p30._u32[0] + 0x0));
                spu_ls_write128(s_ctx[0], at + 0x40, spu_ls_read128(s_ctx[0], p30._u32[0] + 0x10));
            }
            scan_regs t;
            scan_entry(s_ctx[0], &k, p30, p31, p32, &t);
            spu_ls_write128(s_ctx[0], at + 0x0, t.r15);
            spu_ls_write128(s_ctx[0], at + 0x10, t.r16);
            spu_ls_write128(s_ctx[0], at + 0x20, t.r20);
            spu_ls_write128(s_ctx[0], at + 0x50, spu_shlqbyi(t.r66, 12));
        }
        copy_ctx();
        int d0 = 0, d1 = 0;
        const uint32_t n0 = scan_run(s_ctx[0], &d0, 1), n1 = scan_run(s_ctx[1], &d1, 0);
        skipped += n0 > 0;
        ended += d0;
        bad += n0 != n1 || d0 != d1 || ctx_differ();
    }
    check("scan block, SIMD vs scalar", bad);
    printf("  scan: %d of 2000 contexts skipped entries, %d to the end\n", skipped, ended);
    check("scan block's SIMD test agreed with the helpers", s_scan_bad);
    if (!skipped || !ended) check("scan block skipped entries and reached the end", 1);
}

int main(void)
{
#if defined(PS3_SSE2NEON)
    puts("test_spu_simd_paths: SSE intrinsics through sse2neon (arm64)");
#else
    puts("test_spu_simd_paths: SSE4.1 intrinsics (x86-64)");
#endif
    for (int i = 0; i < 2; i++) {
        s_ctx[i] = (spu_context*)calloc(1, sizeof(spu_context));
        s_ls[i] = (uint8_t*)calloc(1, SPU_LS_SIZE);
        if (!s_ctx[i] || !s_ls[i]) { puts("out of memory"); return 1; }
        s_ctx[i]->ls = s_ls[i];
    }
    s_ctx[0]->image_id = s_ctx[1]->image_id = 0x7FFF;
    test_helpers();
    test_diff();
    test_scan();
    printf("test_spu_simd_paths: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
