/*
 * ps3recomp - native blocks for a lock-line ring consumer's fetch and end-of-command steps
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
 *
 * SPU_RING_HIST=<n> (live; 0, the default, is off) counts what the block
 * hands to the lifted loop, to choose what else to write natively, and
 * prints the counts after every n commands it took, then starts the next
 * interval from 0:
 *     each command byte (byte 3 of the command at 0x13560);
 *     for command 37 (a copy into the local RSX command buffer), power-of-two
 *         buckets of its payload size (w0 >> 8) and of its copy length (w1);
 *     segment switches, 0x3C90's slow path: the segment index at 0x13358
 *         differs from the last counted command's (the semaphore at 0x13350,
 *         +1 per switch, is printed beside it as a check);
 *     fallbacks, the returns of 0: each is one command the lifted code
 *         fetches (the first after every wait), and is in no other count.
 * The first counted command also prints the context fields
 * spu_indirect_branch reads, and the end of an interval prints them again if
 * they changed. Nothing else changes with it on: it reads local store
 * directly (no LS probe sees it) and writes only its own counters.
 *
 * spu_hook_ring_post (0x4808-0x4818) is the end of every command: the call to
 * 0x3C90, which stores the next output position (the local RSX buffer's cur)
 * into the segment quadword at 0x13350. While the local buffer is under its
 * limit and the command is not 35 (a flush), 0x3C90 only saves 13 registers,
 * reads the buffer's descriptor at 0x13380, stores cur into word 3 of
 * 0x13350 and restores the registers. This block does that in C, with the
 * same local-store bytes, registers and context fields the call leaves; the
 * segment switch (limit reached) and command 35 run lifted.
 *
 * SPU_NATIVE_POST (live): 1 (default) as above, 0 runs the lifted call,
 * 2 checks: the lifted call runs, and at the next fetch its result is
 * compared with what the block would have left ([ring-post] lines; a
 * mismatch keeps the block off on that thread for the session).
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
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
#define RING_SEG_LS   0x13350u   /* FIFO segments: semaphore, count, index, next output */

#if defined(_MSC_VER)
#define RING_INLINE   static __forceinline
#define RING_NOINLINE static __declspec(noinline)
#else
#define RING_INLINE   static inline __attribute__((always_inline))
#define RING_NOINLINE static __attribute__((noinline))
#endif

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

/* The fetch step: 1 when it took a command, 0 runs the lifted code. */
RING_INLINE int ring_fetch(spu_context* ctx)
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

/* SPU_RING_HIST: n, or 0 for off. Refreshed on ps3_env_gen as g_spu_ch_diag
 * is, so with it off a fetch tests two globals that do not change. */
static int g_spu_ring_hist;
static unsigned g_spu_ring_hist_gen = ~0u;
static unsigned g_spu_ring_hist_epoch;   /* +1 when it is turned on or n changes */
RING_NOINLINE int spu_ring_hist_load(void)
{
    const unsigned gen = ps3_env_gen;
    int n = ps3_env_int("SPU_RING_HIST", 0);
    if (n < 0) n = 0;
    if (n && n != g_spu_ring_hist) g_spu_ring_hist_epoch++;
    g_spu_ring_hist = n;
    g_spu_ring_hist_gen = gen;
    return n;
}

/* What spu_indirect_branch reads to pick the image a target resolves in. */
typedef struct ring_fields {
    int image_id, resident_task, resident_ovl, policy_mode, module_img_a00;
    int code_img[4];
    uint32_t code_lsa[4], code_size[4];
    uint32_t host_depth;
} ring_fields;

/* The counts are per host thread, which makes them the running context's:
 * a context stays on one host thread for its lifetime (spu_context.h). Two
 * contexts running this image at once would each count their own, with no
 * atomics and no line shared between threads. */
typedef struct ring_hist_state {
    unsigned epoch;
    int armed;                        /* a command was counted in this epoch */
    int field_prints;
    uint32_t seg;                     /* segment index at the last counted command */
    uint32_t sem0;                    /* semaphore at the start of the interval */
    uint64_t t0_ms, interval, cmds, fallbacks, switches;
    uint32_t cmd[256];
    uint32_t size37[13], copy37[13];  /* ring_bucket */
    ring_fields fields;               /* as last printed */
} ring_hist_state;
static SPU_THREAD_LOCAL ring_hist_state t_ring_hist;

/* 0, then up to 16, 32, ... 16K bytes, then above 16K (the helpers' DMA
 * fast case ends at 16K). */
static unsigned ring_bucket(uint32_t v)
{
    if (!v) return 0;
    unsigned b = 1;
    for (uint32_t top = 16; v > top && b < 12; top <<= 1) b++;
    return b;
}

static uint64_t ring_ms(void)
{
    struct timespec ts;
#ifdef _WIN32
    timespec_get(&ts, TIME_UTC);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void ring_fields_get(const spu_context* ctx, ring_fields* f)
{
    memset(f, 0, sizeof *f);
    f->image_id = ctx->image_id;
    f->resident_task = ctx->resident_task;
    f->resident_ovl = ctx->resident_ovl;
    f->policy_mode = ctx->policy_mode;
    f->module_img_a00 = ctx->module_img_a00;
    for (int s = 0; s < 4; s++) {
        f->code_img[s] = ctx->resident_code[s].image_id;
        f->code_lsa[s] = ctx->resident_code[s].lsa;
        f->code_size[s] = ctx->resident_code[s].size;
    }
    f->host_depth = ctx->host_depth;
}

static void ring_fields_print(const ring_fields* f, uint32_t spu, const char* what)
{
    char rc[4][48];
    for (int s = 0; s < 4; s++) {
        if (f->code_img[s])
            snprintf(rc[s], sizeof rc[s], "%d@0x%05X+0x%X",
                     f->code_img[s], f->code_lsa[s], f->code_size[s]);
        else
            snprintf(rc[s], sizeof rc[s], "0");
    }
    fprintf(stderr, "[ring-hist] spu=%u %s: image_id=%d resident_task=%d resident_ovl=%d"
                    " policy_mode=%d module_img_a00=%d resident_code=%s,%s,%s,%s"
                    " host_depth=%u\n",
            spu, what, f->image_id, f->resident_task, f->resident_ovl, f->policy_mode,
            f->module_img_a00, rc[0], rc[1], rc[2], rc[3], f->host_depth);
}

/* The end of an interval: three lines, then the counts start again. */
RING_NOINLINE void ring_hist_print(const spu_context* ctx, ring_hist_state* t, uint32_t sem)
{
    static const char* const bk[13] = { "0", "<=16", "<=32", "<=64", "<=128", "<=256", "<=512",
                                        "<=1K", "<=2K", "<=4K", "<=8K", "<=16K", ">16K" };
    const uint64_t now = ring_ms(), ms = now - t->t0_ms;
    const unsigned long long iv = (unsigned long long)++t->interval;
    char line[4096];
    int p = 0;
#define RING_CAT(...) do { if (p < (int)sizeof line) \
        p += snprintf(line + p, sizeof line - (size_t)p, __VA_ARGS__); } while (0)
    const double per_s = ms ? (double)(t->cmds + t->fallbacks) * 1000.0 / (double)ms : 0.0;
    fprintf(stderr, "[ring-hist] spu=%u #%llu: %llu taken + %llu fallbacks in %llu ms"
                    " (%.0f a second), %llu segment switches (semaphore +%u)\n",
            ctx->spu_id, iv, (unsigned long long)t->cmds, (unsigned long long)t->fallbacks,
            (unsigned long long)ms, per_s, (unsigned long long)t->switches, sem - t->sem0);
    RING_CAT("[ring-hist] spu=%u #%llu cmd", ctx->spu_id, iv);
    for (int c = 0; c < 256; c++)
        if (t->cmd[c]) RING_CAT(" %d:%u", c, t->cmd[c]);
    fprintf(stderr, "%s\n", line);
    if (t->cmd[37]) {
        p = 0;
        RING_CAT("[ring-hist] spu=%u #%llu cmd37 size", ctx->spu_id, iv);
        for (int b = 0; b < 13; b++)
            if (t->size37[b]) RING_CAT(" %s:%u", bk[b], t->size37[b]);
        RING_CAT(" | w1");
        for (int b = 0; b < 13; b++)
            if (t->copy37[b]) RING_CAT(" %s:%u", bk[b], t->copy37[b]);
        fprintf(stderr, "%s\n", line);
    }
#undef RING_CAT
    ring_fields f;
    ring_fields_get(ctx, &f);
    if (memcmp(&f, &t->fields, sizeof f)) {
        if (t->field_prints < 8 && ++t->field_prints)
            ring_fields_print(&f, ctx->spu_id, "image fields changed");
        t->fields = f;
    }
    fflush(stderr);
    t->cmds = t->fallbacks = t->switches = 0;
    memset(t->cmd, 0, sizeof t->cmd);
    memset(t->size37, 0, sizeof t->size37);
    memset(t->copy37, 0, sizeof t->copy37);
    t->sem0 = sem;
    t->t0_ms = now;
}

/* One fetch, with SPU_RING_HIST on (or its generation stale). */
RING_NOINLINE void ring_hist_count(spu_context* ctx, int took)
{
    if (g_spu_ring_hist_gen != ps3_env_gen) spu_ring_hist_load();
    const int n = g_spu_ring_hist;
    if (!n) return;
    ring_hist_state* t = &t_ring_hist;
    if (t->epoch != g_spu_ring_hist_epoch) {   /* turned on, or a new n: start over */
        memset(t, 0, sizeof *t);
        t->epoch = g_spu_ring_hist_epoch;
    }
    if (!took) { t->fallbacks++; return; }
    const uint8_t* ls = ctx->ls;
    const uint32_t w0 = be32_at(ls + RING_CMD_LS);
    const uint32_t sem = be32_at(ls + RING_SEG_LS), seg = be32_at(ls + RING_SEG_LS + 8);
    const unsigned cmd = w0 & 255u;              /* byte 3 of 0x13560 */
    t->cmd[cmd]++;
    if (cmd == 37) {
        t->size37[ring_bucket(w0 >> 8)]++;
        t->copy37[ring_bucket(be32_at(ls + RING_CMD_LS + 4))]++;
    }
    if (!t->armed) {
        t->armed = 1;
        t->seg = seg;
        t->sem0 = sem;
        t->t0_ms = ring_ms();
        ring_fields_get(ctx, &t->fields);
        ring_fields_print(&t->fields, ctx->spu_id, "image fields");
        fflush(stderr);
    } else if (seg != t->seg) {
        t->switches++;
        t->seg = seg;
    }
    if (++t->cmds >= (uint64_t)n) ring_hist_print(ctx, t, sem);
}

/* ---- spu_hook_ring_post: 0x3C90's fast path ------------------------------ */

#define RING_DESC_LS  0x13380u   /* the local buffer {base, limit, cur, 0} */
#define RING_POST_END 0x4818u    /* the loop test after the call to 0x3C90 */

static int native_post_mode(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("SPU_NATIVE_POST", 1));
    return on;
}

/* 0x3C90's prologue stores: the slot's offset from sp and the register stored
 * there (0 = the link the brsl at 0x4814 sets). */
static const int16_t k_post_slot[13] = { -0x20, -0x60, -0x10, -0x30, -0x40, -0x50,
                                         -0x80, -0x90, 0x10, -0x70, -0xA0, -0xB0, -0x100 };
static const uint8_t k_post_reg[13]  = { 81, 85, 80, 82, 83, 84, 87, 88, 0, 86, 89, 90, 1 };

enum { POST_OK, POST_FRAME, POST_SWITCH, POST_CMD35, POST_NREASON };

/* What 0x4808-0x4818 leaves on the fast path (SPU_NATIVE_POST=2 compares it). */
typedef struct ring_post {
    uint32_t sp;
    int32_t image_id;
    u128 slot[13];
    u128 seg;
    u128 r0, r2, r3, r4, r5, r6, r7, r8, r29, r30;
    u128 keep[11];               /* r80-r90 come back unchanged */
} ring_post;

/* The fast path, or the reason it is not taken (nothing written then). With
 * rec, the result goes there and nothing is written; without, it is applied. */
RING_INLINE int ring_post_run(spu_context* ctx, ring_post* rec)
{
    u128* r = ctx->gpr;
    const uint32_t sp = r[1]._u32[0];
    if ((sp & 15u) || sp < 0x100u || sp > SPU_LS_SIZE - 0x20u) return POST_FRAME;
    /* The frame must miss both quadwords the step reads and writes, so the
     * order of its accesses cannot matter. */
    if (sp - 0x100u < RING_DESC_LS + 16u && sp + 0x20u > RING_SEG_LS) return POST_FRAME;
    const u128 d = spu_ls_read128(ctx, RING_DESC_LS);
    const u128 r5 = spu_andi(r[5], 255);                          /* 0x4808 */
    if (!(d._u32[1] > d._u32[2])) return POST_SWITCH;             /* limit <= cur */
    if (r5._u32[0] == 35u) return POST_CMD35;
    const u128 link = spu_link(RING_POST_END);
    const u128 seg_old = spu_ls_read128(ctx, RING_SEG_LS);
    u128 seg = seg_old;
    seg._u32[3] = d._u32[2];                                      /* LS[0x1335C] = cur */
    const u128 r2 = spu_rotqbyi(d, 4);                            /* {limit, cur, 0, base} */
    const u128 r3 = spu_rotqbyi(d, 8);                            /* {cur, 0, base, limit} */
    const int32_t img = ctx->module_img_a00 ? ctx->module_img_a00 : ctx->image_id;
    if (rec) {
        rec->sp = sp;
        rec->image_id = img;
        for (int k = 0; k < 13; k++) rec->slot[k] = k_post_reg[k] ? r[k_post_reg[k]] : link;
        rec->seg = seg;
        rec->r0 = link;
        rec->r2 = r2;
        rec->r3 = r3;
        rec->r4 = spu_clgt(r2, r3);
        rec->r5 = r5;
        rec->r6 = spu_ila(RING_DESC_LS + 4u);
        rec->r7 = d;
        rec->r8 = spu_ceqi(r5, 35);
        rec->r29 = seg_old;
        rec->r30 = spu_cwd(spu_ai(r[1], -256), 12);
        for (int k = 0; k < 11; k++) rec->keep[k] = r[80 + k];
        return POST_OK;
    }
    for (int k = 0; k < 13; k++)
        spu_ls_write128(ctx, sp + (uint32_t)(int32_t)k_post_slot[k], k_post_reg[k] ? r[k_post_reg[k]] : link);
    spu_ls_write128(ctx, RING_SEG_LS, seg);
    r[30] = spu_cwd(spu_ai(r[1], -256), 12);
    r[0] = link;
    r[2] = r2;
    r[3] = r3;
    r[4] = spu_clgt(r2, r3);
    r[5] = r5;
    r[6] = spu_ila(RING_DESC_LS + 4u);
    r[7] = d;
    r[8] = spu_ceqi(r5, 35);
    r[29] = seg_old;
    /* What SPU_RET and the call bracket leave after a plain return. */
    ctx->pc = RING_POST_END;
    ctx->drain_ret_pc = 0;
    ctx->image_id = img;
    return POST_OK;
}

/* SPU_NATIVE_POST=2. The prediction is made at 0x4808, the lifted call runs,
 * and the next fetch (0x4618, reached from 0x4818 by the loop's branch with
 * nothing in between) compares. Per host thread, so per context. */
static volatile int g_post_checking;
static SPU_THREAD_LOCAL ring_post t_post;
static SPU_THREAD_LOCAL int t_post_pending, t_post_bad, t_post_prints;
static SPU_THREAD_LOCAL unsigned long long t_post_n, t_post_miss, t_post_why[POST_NREASON];

static u128 ls_raw128(const spu_context* ctx, uint32_t lsa)
{
    const uint8_t* p = &ctx->ls[lsa & (SPU_LS_MASK & ~0xFu)];
    u128 v;
    for (int i = 0; i < 4; i++) v._u32[i] = be32_at(p + 4 * i);
    return v;
}

/* what: a label with one %d, for n (a slot's offset or a register number). */
static int post_same(const spu_context* ctx, const char* what, int n, u128 lifted, u128 native)
{
    if (!memcmp(&lifted, &native, sizeof lifted)) return 1;
    if (t_post_prints < 8) {
        char name[16];
        snprintf(name, sizeof name, what, n);
        t_post_prints++;
        fprintf(stderr, "[ring-post] spu=%u mismatch: %s lifted %08X %08X %08X %08X native %08X %08X %08X %08X"
                        " (cmd %u, sp 0x%05X)\n", ctx->spu_id, name,
                lifted._u32[0], lifted._u32[1], lifted._u32[2], lifted._u32[3],
                native._u32[0], native._u32[1], native._u32[2], native._u32[3],
                t_post.r5._u32[0], t_post.sp);
    }
    return 0;
}

RING_NOINLINE void ring_post_check(spu_context* ctx)
{
    if (!t_post_pending) return;
    t_post_pending = 0;
    const ring_post* p = &t_post;
    const u128* r = ctx->gpr;
    static const uint8_t regs[10] = { 0, 2, 3, 4, 5, 6, 7, 8, 29, 30 };
    const u128* want[10] = { &p->r0, &p->r2, &p->r3, &p->r4, &p->r5, &p->r6, &p->r7, &p->r8, &p->r29, &p->r30 };
    int ok = 1;
    for (int k = 0; k < 13; k++)
        ok &= post_same(ctx, "slot %d", k_post_slot[k],
                        ls_raw128(ctx, p->sp + (uint32_t)(int32_t)k_post_slot[k]), p->slot[k]);
    ok &= post_same(ctx, "0x%05X", (int)RING_SEG_LS, ls_raw128(ctx, RING_SEG_LS), p->seg);
    for (int k = 0; k < 10; k++)
        ok &= post_same(ctx, "r%d", regs[k], r[regs[k]], *want[k]);
    for (int k = 0; k < 11; k++)
        ok &= post_same(ctx, "r%d", 80 + k, r[80 + k], p->keep[k]);
    if (r[1]._u32[0] != p->sp || ctx->image_id != p->image_id || ctx->drain_ret_pc != 0) {
        if (t_post_prints < 8) {
            t_post_prints++;
            fprintf(stderr, "[ring-post] spu=%u mismatch: sp 0x%05X/0x%05X image_id %d/%d drain_ret_pc 0x%X\n",
                    ctx->spu_id, r[1]._u32[0], p->sp, ctx->image_id, p->image_id, ctx->drain_ret_pc);
        }
        ok = 0;
    }
    if (!ok) { t_post_miss++; t_post_bad = 1; }
    if ((++t_post_n & ((1ull << 20) - 1)) == 0) {
        fprintf(stderr, "[ring-post] spu=%u %llu checked, %llu mismatches; not taken: frame %llu, switch %llu, cmd35 %llu\n",
                ctx->spu_id, t_post_n, t_post_miss, t_post_why[POST_FRAME], t_post_why[POST_SWITCH],
                t_post_why[POST_CMD35]);
        fflush(stderr);
    }
}

int spu_hook_ring_post(spu_context* ctx)
{
    const int mode = native_post_mode();
    if (!mode || g_spu_trampoline_fn || g_spu_ls_dbg) return 0;
    if (mode == 2) {                       /* check: the lifted call runs */
        const int why = ring_post_run(ctx, &t_post);
        t_post_why[why]++;
        if (why == POST_OK && ctx->gpr[81]._u32[0] == 0) {   /* (else the loop ends: no next fetch) */
            t_post_pending = 1;
            g_post_checking = 1;
        }
        return 0;
    }
    if (t_post_bad) return 0;              /* this thread saw a mismatch */
    return ring_post_run(ctx, NULL) == POST_OK;
}

int spu_hook_ring_fetch(spu_context* ctx)
{
    if (__builtin_expect(g_post_checking, 0)) ring_post_check(ctx);   /* SPU_NATIVE_POST=2 */
    const int took = ring_fetch(ctx);
    if (__builtin_expect(g_spu_ring_hist_gen != ps3_env_gen || g_spu_ring_hist, 0))
        ring_hist_count(ctx, took);
    return took;
}
