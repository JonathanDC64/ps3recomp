/*
 * Equivalence test for image 2's lazy ring loop (SPU_RING_LAZY in
 * runtime/spu/spu_ring_hook.c).
 *
 * Each case builds a local store and a guest memory as Demon's Souls' ring
 * consumer (SPURS image 2) has them at its fetch, 0x4618: the control line's
 * copy and the state, the state and dirty blocks, the segment quadword and
 * the local buffer's descriptor, the frame's registers; in guest memory the
 * control line, a ring of random commands (7, 8, 10, 11, 12, 13, 17, 18, now
 * and then one the loop leaves to the lifted code, or a GET it does not
 * take) and the GETs' sources. Then spu_hook_ring_fetch runs on two copies,
 * one with SPU_RING_LAZY=0 (each command writes every register and the
 * frame, as before) and one with 1, and everything they can leave different
 * is compared: the return, the registers (all 128 when the run ends at
 * 0x4730; at 0x4618 all but the ones the fetch alone writes, which the
 * lifted code there writes again before it reads them), the context fields
 * the loop writes, the whole local store, the whole guest memory, the
 * transfers in order and the fetch's GET memo.
 *
 * The MFC is a stub that moves the bytes at once and keeps tag 0's
 * completion bit as the engine does (cleared by nothing, set by a transfer,
 * clear at the start of some cases). The lazy loop leaves out only the tag
 * waits with no transfer since the last one, so each wait that follows a
 * transfer goes into the transfer log with the status it read. The GETLLAR
 * no-op commit copies the line, or now and then fails, the same calls
 * failing in both runs. The knobs the loop reads are varied across cases:
 * the PUT interval, the one-call transfers, the GET skip, the line read for
 * every command.
 *
 * Build (clang targeting MSVC, or clang-cl's driver):
 *   clang -O2 -msse4.1 -D_CRT_SECURE_NO_WARNINGS -I include -I runtime -I runtime/spu \
 *       -o test_spu_ring_lazy.exe runtime/spu/tests/test_spu_ring_lazy.c
 * Run: test_spu_ring_lazy [cases, default 20000]
 * Exit code 0 if every case matched; the last line is a summary.
 */

#include "../spu_ring_hook.c"
#include "../spu_put_hook.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
int g_spu_ch_diag;
unsigned g_spu_ch_diag_gen;
volatile int g_spu_goto_slow;
volatile int g_yz_lockstep_on;
void* volatile g_pm_flow_ctx;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);

#define GUEST_SIZE (2u << 20)
#define LINE_EA    0x00100000u
#define STATE_EA   0x00100400u
#define RING_EA    0x00110000u
#define SRC_EA     0x00140000u
#define STATE_LS   0x1F600u
#define DIRTY_LS   0x22740u
#define SP         0x3FE30u

/* ---- the stubs: an MFC that moves the bytes at once ---------------------- */

typedef struct { uint32_t cmd, lsa, eah, eal, size, tag; } xfer_rec;
static xfer_rec g_log[8192];
static int g_nlog, g_nwait, g_since_xfer;
static uint32_t g_tag_done;          /* the engine's tag completion bits */
static uint32_t g_rng_commit;        /* decides which no-op commits fail */

static void stub_xfer(spu_context* ctx, uint32_t cmd)
{
    if (g_nlog < 8192) {
        xfer_rec* x = &g_log[g_nlog++];
        x->cmd = cmd; x->lsa = ctx->mfc_lsa; x->eah = ctx->mfc_eah; x->eal = ctx->mfc_eal;
        x->size = ctx->mfc_size; x->tag = ctx->mfc_tag;
    }
    const uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK, ea = ctx->mfc_eal, size = ctx->mfc_size;
    if (ea + size <= GUEST_SIZE && lsa + size <= SPU_LS_SIZE) {
        if (cmd & 0x40) memcpy(ctx->ls + lsa, vm_base + ea, size);
        else memcpy(vm_base + ea, ctx->ls + lsa, size);
    }
    g_tag_done |= 1u << (ctx->mfc_tag & 31);
    g_since_xfer = 1;
}
/* The lazy loop leaves out only tag waits with no transfer since the last
 * wait; a wait after a transfer is logged (as cmd ~0), so the logs show one
 * left out after a transfer. */

static void stub_wait(spu_context* ctx, uint32_t mode)
{
    g_nwait++;
    ctx->mfc_tag_status = g_tag_done & ctx->mfc_tag_mask;
    if (g_since_xfer && g_nlog < 8192) {
        xfer_rec* x = &g_log[g_nlog++];
        memset(x, 0, sizeof *x);
        x->cmd = ~0u; x->size = mode; x->tag = ctx->mfc_tag_status;
    }
    g_since_xfer = 0;
}
void spu_wrch_word(spu_context* ctx, uint32_t channel, uint32_t v)
{
    if (channel == MFC_Cmd) stub_xfer(ctx, v);
    else if (channel == MFC_WrTagUpdate) stub_wait(ctx, v);
}
void spu_wrch_xfer_wait(spu_context* ctx, uint32_t cmd, uint32_t mode)
{
    if (cmd) stub_xfer(ctx, cmd);
    stub_wait(ctx, mode);
}
u128 spu_rdch(spu_context* ctx, uint32_t channel) { (void)ctx; (void)channel; u128 z = {{0}}; return z; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) { spu_wrch_word(ctx, channel, value._u32[0]); }
int spu_llar_noop_commit(spu_context* ctx, uint32_t lsa, uint32_t ea)
{
    g_rng_commit = g_rng_commit * 1103515245u + 12345u;
    if (((g_rng_commit >> 16) & 31u) == 0) return 0;   /* one in 32 fails */
    ea &= ~127u;
    memcpy(ctx->ls + (lsa & SPU_LS_MASK), vm_base + ea, 128);
    memcpy(ctx->resv_line, vm_base + ea, 128);
    ctx->resv_ea = ea;
    if (ctx->resv_valid) ctx->resv_valid = 0;
    ctx->atomic_stat = 0;
    return 1;
}

/* ---- a case ----------------------------------------------------------- */

static uint32_t g_rng;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }
static uint32_t rndn(uint32_t n) { return rnd() % n; }

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void ls_q(uint8_t* ls, uint32_t a, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    put32(ls + a, w0); put32(ls + a + 4, w1); put32(ls + a + 8, w2); put32(ls + a + 12, w3);
}

/* One random command at p (16 bytes, big-endian words): mostly the ones the
 * loop runs, with fields in their ranges. */
static void make_cmd(uint8_t* p)
{
    static const unsigned kinds[] = {8, 8, 8, 8, 8, 8, 7, 7, 12, 10, 11, 13, 17, 18};
    const unsigned r = rndn(100);
    unsigned c = kinds[rndn(sizeof kinds / sizeof kinds[0])];
    uint32_t w0 = rnd() & 0xFFFFFF00u, w1 = 0, w2 = rnd(), w3 = rnd();
    if (r < 4) c = 23;                         /* a no-op: the lifted code takes it */
    else if (r < 6) c = 5;
    switch (c) {
    case 7:  w1 = rndn(200); break;
    case 8:  w1 = rndn(16) | (rndn(2) << 4); w2 = rndn(24); break;
    case 12: w1 = (rndn(16) << 24) | (rnd() & 0xFFFFFF); break;
    case 10: w1 = rndn(8); w2 = SRC_EA + 16u * rndn(4096); break;
    case 11: case 13: case 17: case 18:
        w1 = SRC_EA + 16u * rndn(4096);
        if (rndn(50) == 0) w1 += 4;                /* misaligned: left to the lifted code */
        break;
    default: break;
    }
    put32(p, w0 | c); put32(p + 4, w1); put32(p + 8, w2); put32(p + 12, w3);
}

typedef struct { spu_context* ctx; uint8_t* ls; uint8_t* mem; } image;

static void build(image* im, unsigned ncmd, uint32_t put_old_delta)
{
    spu_context* ctx = im->ctx;
    uint8_t* ls = im->ls;
    uint8_t* mem = im->mem;
    memset(ctx, 0, sizeof *ctx);
    for (unsigned i = 0; i < SPU_LS_SIZE; i += 4) put32(ls + i, rnd());
    for (unsigned i = 0; i < GUEST_SIZE; i += 4) put32(mem + i, rnd());
    ctx->ls = ls;
    for (int k = 0; k < 128; k++)
        for (int w = 0; w < 4; w++) ctx->gpr[k]._u32[w] = rnd();
    const uint32_t get = RING_EA + 16u * rndn(64);
    const uint32_t put = get + 16u * ncmd;
    ctx->gpr[1] = spu_make_preferred_u32(SP);
    ctx->gpr[1]._u32[1] = rnd(); ctx->gpr[1]._u32[2] = rnd(); ctx->gpr[1]._u32[3] = rnd();
    ctx->gpr[82] = spu_make_preferred_u32(RING_LINE_LS);
    ctx->gpr[83] = spu_make_preferred_u32(rndn(40) == 0 ? 3u : 0u);
    ctx->gpr[84] = spu_make_preferred_u32(0x475C);
    ctx->gpr[85] = spu_make_preferred_u32(0x13580);
    ctx->image_id = 2;
    ctx->event_mask = 1024;
    ctx->mfc_tag_mask = 1;
    ctx->pc = 0x4618;
    /* local store: the line copy (put as last read), the state, the block
     * pointers, the descriptor (cur under the limit), the segment */
    ls_q(ls, RING_LINE_LS, put - put_old_delta, rnd(), rnd(), LINE_EA);
    ls_q(ls, RING_STATE_LS, get, 0, 0, STATE_EA);
    ls_q(ls, RING_STATE_PTR_LS, STATE_LS, rnd(), rnd(), rnd());
    ls_q(ls, RING_DIRTY_PTR_LS, DIRTY_LS, rnd(), rnd(), rnd());
    ls_q(ls, RING_DESC_LS, 0x30000, 0x30800, 0x30000 + 16u * rndn(64), 0);
    /* guest memory: the control line, the commands */
    put32(mem + LINE_EA, put); put32(mem + LINE_EA + 12, LINE_EA);
    for (unsigned i = 0; i < ncmd; i++) make_cmd(mem + get + 16u * i);
    if (rndn(3) == 0) {   /* the ring empty after the first command or two: the live put is get */
        put32(mem + LINE_EA, get + 16u * (1 + rndn(2)));
        ls_q(ls, RING_LINE_LS, get + 16u * (1 + rndn(2)), rnd(), rnd(), LINE_EA);
    }
}

/* Registers only the fetch (0x4618-0x472C) writes. When the run ends at
 * 0x4618 (the return 0) the lazy loop leaves them as its first fetch wrote
 * them, the per-command path as its last; 0x4618 writes each of them before
 * it reads it. */
static int fetch_only_reg(int k)
{
    return k == 10 || k == 13 || k == 14 || (k >= 52 && k <= 58) || k == 61 || k == 63 ||
           (k >= 70 && k <= 76) || k == 78 || k == 79;
}

static int same_ctx(const spu_context* a, const spu_context* b, int at_fetch, char* why)
{
    for (int k = 0; k < 128; k++)
        if (!(at_fetch && fetch_only_reg(k)) && memcmp(&a->gpr[k], &b->gpr[k], 16)) {
            sprintf(why, "r%d", k);
            return 0;
        }
#define F(f) if (a->f != b->f) { sprintf(why, #f " %llX vs %llX", (unsigned long long)a->f, (unsigned long long)b->f); return 0; }
    F(pc) F(drain_ret_pc) F(image_id) F(host_depth) F(steps) F(event_mask) F(event_status)
    F(mfc_lsa) F(mfc_eah) F(mfc_eal) F(mfc_size) F(mfc_tag) F(mfc_tag_mask) F(mfc_tag_status)
    F(resv_valid) F(resv_ea) F(atomic_stat)
#undef F
    if (memcmp(a->resv_line, b->resv_line, 128)) { strcpy(why, "resv_line"); return 0; }
    return 1;
}

static void set_knob(const char* k, int v)
{
    char s[16];
    snprintf(s, sizeof s, "%d", v);
#ifdef _WIN32
    _putenv_s(k, s);
#else
    setenv(k, s, 1);
#endif
}

int main(int argc, char** argv)
{
    const unsigned cases = argc > 1 ? (unsigned)atoi(argv[1]) : 20000u;
    image im[2];
    for (int i = 0; i < 2; i++) {
        im[i].ctx = (spu_context*)calloc(1, sizeof(spu_context));
        im[i].ls = (uint8_t*)malloc(SPU_LS_SIZE);
        im[i].mem = (uint8_t*)malloc(GUEST_SIZE);
        if (!im[i].ctx || !im[i].ls || !im[i].mem) { fprintf(stderr, "out of memory\n"); return 2; }
    }
    static xfer_rec log0[8192];
    int nlog0 = 0;
    unsigned bad = 0, ran = 0, lazy_runs = 0;
    unsigned long long waits[2] = {0, 0}, cmds = 0;
    for (unsigned c = 0; c < cases; c++) {
        /* knobs for this case */
        const int every = (int[]){16, 16, 16, 1, 3, 8}[c % 6];
        const int one = (c / 6) % 2, skip = (c / 12) % 4 != 0, ring = (c / 48) % 5 == 0 ? 2 : 1;
        set_knob("SPU_RING_PUT_EVERY", every);
        set_knob("SPU_XFER_WAIT", one);
        set_knob("SPU_RING_GET_SKIP", skip);
        set_knob("SPU_NATIVE_RING", ring);
        const unsigned ncmd = 1 + rndn(40);
        const uint32_t put_old_delta = rndn(2) ? 0u : 16u * rndn(ncmd + 1);
        const uint32_t seed = rnd(), tag0 = rndn(4) == 0 ? 0u : 1u, commit_seed = rnd();
        ring_get_memo memo0 = {0};
        memo0.ctx = rndn(2) ? im[0].ctx : NULL;   /* (the context pointer differs between the runs) */
        int rc[2];
        ring_get_memo memo_after[2];
        for (int m = 0; m < 2; m++) {
            g_rng = seed;
            build(&im[m], ncmd, put_old_delta);
            set_knob("SPU_RING_LAZY", m);
            ps3_env_gen++;
            g_spu_ch_diag_gen = ps3_env_gen;
            vm_base = im[m].mem;
            g_tag_done = tag0;
            g_rng_commit = commit_seed;
            g_nlog = 0; g_nwait = 0; g_since_xfer = 0;
            t_get = memo0;
            if (memo0.ctx) t_get.ctx = im[m].ctx;
            rc[m] = spu_hook_ring_fetch(im[m].ctx);
            waits[m] += (unsigned)g_nwait;
            memo_after[m] = t_get;
            memo_after[m].ctx = t_get.ctx == im[m].ctx ? (const spu_context*)1 : t_get.ctx;
            if (m == 0) { memcpy(log0, g_log, sizeof(xfer_rec) * (size_t)g_nlog); nlog0 = g_nlog; }
            if (m == 0) cmds += im[0].ctx->steps;
        }
        char why[96] = "";
        int ok = rc[0] == rc[1];
        if (!ok) snprintf(why, sizeof why, "return %d vs %d", rc[0], rc[1]);
        if (ok) {
            spu_context a = *im[0].ctx, b = *im[1].ctx;
            ok = same_ctx(&a, &b, rc[0] == 0, why);
        }
        if (ok && memcmp(im[0].ls, im[1].ls, SPU_LS_SIZE)) {
            unsigned i = 0;
            while (im[0].ls[i] == im[1].ls[i]) i++;
            snprintf(why, sizeof why, "LS 0x%05X", i);
            ok = 0;
        }
        if (ok && memcmp(im[0].mem, im[1].mem, GUEST_SIZE)) { strcpy(why, "guest memory"); ok = 0; }
        if (ok && memcmp(&memo_after[0], &memo_after[1], sizeof memo_after[0])) { strcpy(why, "GET memo"); ok = 0; }
        if (ok && (nlog0 != g_nlog || memcmp(log0, g_log, sizeof(xfer_rec) * (size_t)g_nlog))) {
            snprintf(why, sizeof why, "transfers (%d vs %d)", nlog0, g_nlog);
            ok = 0;
        }
        ran++;
        if (!ok) {
            if (bad < 10)
                printf("case %u (every %d, one %d, skip %d, ring %d, %u commands): %s\n", c, every, one, skip, ring,
                       ncmd, why);
            bad++;
        }
        if (im[1].ctx->steps) lazy_runs++;
    }
    printf("%u cases (%u ran commands natively, %llu steps), %u differ; tag waits: per-command %llu, lazy %llu\n",
           ran, lazy_runs, cmds, bad, waits[0], waits[1]);
    return bad ? 1 : 0;
}
