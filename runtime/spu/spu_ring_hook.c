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
#include <stdlib.h>
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
void spu_wrch_word(spu_context* ctx, uint32_t channel, uint32_t value);
void spu_wrch_xfer_wait(spu_context* ctx, uint32_t cmd, uint32_t mode);
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

/* The fetch GETs the 128 bytes around get for every 16-byte command, so a
 * block holding eight commands is copied up to eight times, and each copy
 * pulls back the lines the producer is still writing commands into (its next
 * store to them then waits for the line, and its sync with it). The commands
 * before the put that was read ahead of a GET were already in memory when it
 * copied them, and the producer does not write them again until get has gone
 * past them, so the next command is skipped when that GET already covered it:
 *     the same block and LS buffer, and get is the command after the last one
 *         this fetch took (no ring reset, no command taken elsewhere);
 *     get + 16 is at or before the put read ahead of that GET (no wrap since);
 *     the command's quadword in the LS buffer is still what the GET left.
 * The GET's other quadwords then keep the older copy, as if it had come before
 * the producer's later stores; the fetch reads only the command's. The tag
 * wait still goes through the channel, so the tag status is the same.
 *
 * SPU_RING_GET_SKIP (live): 1 (default) as above, 0 GETs every command, 2 as
 * 1 and prints how many were skipped every 2^22 fetches. */
typedef struct ring_get_memo {
    const spu_context* ctx;
    uint32_t ea, lsa;      /* the block last GET, and where it went in local store */
    uint32_t put;          /* put as read before that GET */
    uint32_t next;         /* get after the last command the fetch took */
    u128 q[8];             /* the block as that GET left it in local store */
    unsigned long long n, skipped;
} ring_get_memo;
static SPU_THREAD_LOCAL ring_get_memo t_get;

/* SPU_XFER_WAIT (live): 1 (default) issues a transfer and its tag wait as
 * one spu_wrch_xfer_wait call; 0 as two spu_wrch_word calls, for A/B. */
RING_INLINE int ring_xfer_wait_on(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("SPU_XFER_WAIT", 1));
    return on;
}

static int ring_get_skip_mode(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("SPU_RING_GET_SKIP", 1));
    return on;
}

RING_NOINLINE void ring_get_stats(const spu_context* ctx)
{
    fprintf(stderr, "[ring-get] spu=%u %llu fetches, %llu GETs skipped (%.1f%%)\n", ctx->spu_id,
            t_get.n, t_get.skipped, 100.0 * (double)t_get.skipped / (double)t_get.n);
    fflush(stderr);
}

/* What a fetch that took a command read; its register writes are worked out
 * from these (ring_fetch_regs). */
typedef struct ring_fetched {
    u128 old_line, state, line, r73, r71;
} ring_fetched;

/* 0x4618-0x4690's writes before the GETLLAR, which stay when the fetch gives
 * the command back at the GETLLAR. */
RING_INLINE void ring_fetch_regs_head(spu_context* ctx, u128 old_line)
{
    u128* r = ctx->gpr;
    const u128 r52 = spu_il(208);
    const u128 r55 = spu_rotqbyi(old_line, 8);
    r[58] = spu_il(0);
    r[57] = spu_il(1024);
    r[56] = old_line;
    r[55] = r55;
    r[54] = spu_shlqbyi(r55, 4);
    r[52] = r52;
    r[53] = spu_ori(r52, 0);
}

/* The registers 0x4618-0x472C leave (a register written twice is written its
 * last value). The tag status is the fetch's tag wait's: nothing after the
 * fetch changes it before these are written. */
RING_INLINE void ring_fetch_regs(spu_context* ctx, const ring_fetched* f)
{
    u128* r = ctx->gpr;
    ring_fetch_regs_head(ctx, f->old_line);
    const u128 r8 = spu_rotqbyi(f->line, 8);
    r[63] = f->state;
    r[61] = spu_ceq(f->line, f->state);
    r[8] = r8;
    r[14] = spu_shlqbyi(r8, 4);
    r[6] = spu_il(180);
    r[76] = f->state;
    r[10] = spu_ori(r[85], 0);
    r[13] = spu_andi(f->state, -128);
    r[81] = spu_il(64);
    r[79] = spu_il(1);
    r[78] = spu_il(2);
    r[2] = spu_make_preferred_u32(ctx->mfc_tag_status);
    const u128 r74 = spu_cwd(r[1], 0);
    const u128 r72 = spu_ai(f->r73, 16);
    const u128 r70 = spu_shufb(r72, f->r73, r74);
    r[73] = f->r73;
    r[5] = spu_il(16);
    r[4] = spu_ila(RING_STATE_LS);
    r[75] = spu_andi(f->state, 127);
    r[74] = r74;
    r[71] = f->r71;
    r[72] = r72;
    r[70] = r70;
    r[3] = spu_rotqbyi(r70, 8);
}

/* The fetch step without its register writes: 1 when it took a command (f
 * then has what ring_fetch_regs works them out from), 0 runs the lifted code.
 * `lazy` (ring_lazy, inside a run of commands): a skipped GET's tag wait is
 * not made, as the tag status it reads cannot have changed since the last
 * one (see ring_lazy). */
RING_INLINE int ring_fetch_take(spu_context* ctx, ring_fetched* f, int lazy)
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

    /* The registers are worked out by ring_fetch_regs, from what this reads
     * (a value stored into ctx->gpr a word at a time and read back whole
     * cannot be forwarded from the store buffer). */
    /* 0x4618-0x4690: wrch ch69 (no channel), event mask, GETLLAR. */
    ctx->event_mask = 1024;
    /* The GETLLAR and the unchanged PUTLLC at 0x46BC, together. */
    if (known) {
        if (ctx->resv_valid) __atomic_store_n(&ctx->resv_valid, 0, __ATOMIC_RELEASE);
        ctx->atomic_stat = 0;
    } else if (!spu_llar_noop_commit(ctx, RING_LINE_LS, old_line._u32[3])) {
        ring_fetch_regs_head(ctx, old_line);
        return 0;
    }
    /* 0x4694: the compare (not equal: the live put was read above, and the
     * copy is as new as that). */
    const u128 line = spu_ls_read128(ctx, RING_LINE_LS);
    /* 0x46C4-0x4700: GET the command's 128 bytes, wait for tag 0. */
    const uint32_t blk = state._u32[0] & ~127u;
    const uint32_t lsa_raw = r[85]._u32[0];
    ctx->mfc_lsa = lsa_raw;
    ctx->mfc_eal = blk;
    ctx->mfc_size = 128;
    ctx->mfc_tag = 0;
    {
        const int skip_mode = ring_get_skip_mode();
        const uint32_t get = state._u32[0];
        const uint32_t lsa = lsa_raw & SPU_LS_MASK;
        ring_get_memo* m = &t_get;
        int skip = 0;
        if (skip_mode && m->ctx == ctx && m->next == get && m->ea == blk && m->lsa == lsa &&
            get + 16u <= m->put)
            skip = memcmp(ctx->ls + lsa + (get & 112u), &m->q[(get & 112u) >> 4], 16) == 0;
        ctx->mfc_tag_mask = 1;
        if (skip && lazy && !(ctx->event_mask & 1u)) {
            /* the tag wait alone, which reads what the last one read */
        } else if (ring_xfer_wait_on()) {
            spu_wrch_xfer_wait(ctx, skip ? 0u : 64u, 2);   /* the GET, then wait for all of tag 0 */
        } else {
            if (!skip) spu_wrch_word(ctx, MFC_Cmd, 64);
            spu_wrch_word(ctx, MFC_WrTagUpdate, 2);
        }
        if (!skip) {
            if (skip_mode && !(lsa & 15u) && lsa + 128u <= SPU_LS_SIZE) {
                m->ctx = ctx; m->ea = blk; m->lsa = lsa; m->put = line._u32[0];
                memcpy(m->q, ctx->ls + lsa, 128);
            } else {
                m->ctx = NULL;
            }
        }
        m->next = get + 16u;
        if (__builtin_expect(skip_mode == 2, 0)) {
            m->skipped += (unsigned)skip;
            if ((++m->n & ((1ull << 22) - 1)) == 0) ring_get_stats(ctx);
        }
    }
    /* 0x4704-0x472C: copy the command out, get += 16. */
    const u128 r73 = spu_ls_read128(ctx, RING_STATE_LS);
    const u128 r71 = spu_ls_read128(ctx, (state._u32[0] & 127u) + lsa_raw);
    const u128 r72 = spu_ai(r73, 16);
    const u128 r70 = spu_shufb(r72, r73, spu_cwd(r[1], 0));
    spu_ls_write128(ctx, RING_CMD_LS, r71);
    spu_ls_write128(ctx, RING_STATE_LS, r70);
    f->old_line = old_line;
    f->state = state;
    f->line = line;
    f->r73 = r73;
    f->r71 = r71;
    return 1;
}

/* The fetch step: 1 when it took a command, 0 runs the lifted code. */
RING_INLINE int ring_fetch(spu_context* ctx)
{
    ring_fetched f;
    if (!ring_fetch_take(ctx, &f, 0)) return 0;
    ring_fetch_regs(ctx, &f);
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

/* SPU_POST_FAST=0 (live): the prologue stores below go through
 * spu_ls_write128 one table entry at a time, as before. =2 also checks each
 * slot against the table (post_fast_check). */
static int post_fast(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("SPU_POST_FAST", 1));
    return on;
}

/* A quadword into local store as spu_ls_write128 stores it, without its
 * probe test: for code that runs only while g_spu_ls_dbg is 0. */
RING_INLINE void ls_put128(uint8_t* ls, uint32_t lsa, u128 v)
{
    uint8_t* p = ls + (lsa & (SPU_LS_MASK & ~0xFu));
    const uint32_t w0 = SPU_BSWAP32(v._u32[0]), w1 = SPU_BSWAP32(v._u32[1]);
    const uint32_t w2 = SPU_BSWAP32(v._u32[2]), w3 = SPU_BSWAP32(v._u32[3]);
    memcpy(p, &w0, 4); memcpy(p + 4, &w1, 4);
    memcpy(p + 8, &w2, 4); memcpy(p + 12, &w3, 4);
}

/* SPU_POST_FAST=2: after the straight stores, each slot against what the
 * table loop stores there. */
RING_NOINLINE void post_fast_check(spu_context* ctx, uint32_t sp, u128 link, u128 seg)
{
    static unsigned long long n, bad;
    const u128* r = ctx->gpr;
    int ok = 1;
    for (int k = 0; k < 14; k++) {
        const uint32_t lsa = k < 13 ? sp + (uint32_t)(int32_t)k_post_slot[k] : RING_SEG_LS;
        const u128 v = k == 13 ? seg : k_post_reg[k] ? r[k_post_reg[k]] : link;
        uint8_t want[16];
        for (int i = 0; i < 4; i++) {
            want[4 * i] = (uint8_t)(v._u32[i] >> 24); want[4 * i + 1] = (uint8_t)(v._u32[i] >> 16);
            want[4 * i + 2] = (uint8_t)(v._u32[i] >> 8); want[4 * i + 3] = (uint8_t)v._u32[i];
        }
        if (memcmp(ctx->ls + (lsa & (SPU_LS_MASK & ~0xFu)), want, 16)) {
            ok = 0;
            if (bad < 8)
                fprintf(stderr, "[ring-post] spu=%u slot %d (LS 0x%05X) differs from the table\n",
                        ctx->spu_id, k, lsa & SPU_LS_MASK);
        }
    }
    bad += !ok;
    n++;
    if ((n & (n - 1)) == 0 && n >= 1024) {
        fprintf(stderr, "[ring-post] fast stores: %llu checked, %llu differ\n", n, bad);
        fflush(stderr);
    }
}

/* The registers and context fields the fast path leaves, from the buffer's
 * descriptor d, the segment quadword before its store, r5 = the command byte
 * and the image id after the return. */
RING_INLINE void ring_post_regs(spu_context* ctx, u128 d, u128 seg_old, u128 r5, int32_t img)
{
    u128* r = ctx->gpr;
    const u128 r2 = spu_rotqbyi(d, 4);                            /* {limit, cur, 0, base} */
    const u128 r3 = spu_rotqbyi(d, 8);                            /* {cur, 0, base, limit} */
    r[30] = spu_cwd(spu_ai(r[1], -256), 12);
    r[0] = spu_link(RING_POST_END);
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
}

/* The prologue stores in k_post_slot's order, one statement each, and the
 * segment quadword: only while g_spu_ls_dbg is 0, with sp past the frame
 * checks (so every slot is in local store). */
RING_INLINE void ring_post_frame(spu_context* ctx, uint32_t sp, u128 link, u128 seg)
{
    const u128* r = ctx->gpr;
    uint8_t* const ls = ctx->ls;
    ls_put128(ls, sp - 0x20u, r[81]);
    ls_put128(ls, sp - 0x60u, r[85]);
    ls_put128(ls, sp - 0x10u, r[80]);
    ls_put128(ls, sp - 0x30u, r[82]);
    ls_put128(ls, sp - 0x40u, r[83]);
    ls_put128(ls, sp - 0x50u, r[84]);
    ls_put128(ls, sp - 0x80u, r[87]);
    ls_put128(ls, sp - 0x90u, r[88]);
    ls_put128(ls, sp + 0x10u, link);
    ls_put128(ls, sp - 0x70u, r[86]);
    ls_put128(ls, sp - 0xA0u, r[89]);
    ls_put128(ls, sp - 0xB0u, r[90]);
    ls_put128(ls, sp - 0x100u, r[1]);
    ls_put128(ls, RING_SEG_LS, seg);
}

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
    /* The prologue stores in k_post_slot's order, one statement each. Both
     * callers run this only while g_spu_ls_dbg is 0 (spu_hook_ring_post's
     * entry test, ring_loop's loop_yield), so no probe has to see them, and
     * sp passed the frame checks above, so every slot is in local store. The
     * loop over the tables went through spu_ls_write128, which reloaded
     * ctx->ls and tested the probe flag for each of the 13: 3.9% of image 2
     * in the deep Palace, every command. */
    const int fast = post_fast();
    if (fast) {
        ring_post_frame(ctx, sp, link, seg);
        if (__builtin_expect(fast == 2, 0)) post_fast_check(ctx, sp, link, seg);
    } else {
        for (int k = 0; k < 13; k++)
            spu_ls_write128(ctx, sp + (uint32_t)(int32_t)k_post_slot[k], k_post_reg[k] ? r[k_post_reg[k]] : link);
        spu_ls_write128(ctx, RING_SEG_LS, seg);
    }
    ring_post_regs(ctx, d, seg_old, r5, img);
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

/* ---- SPU_NATIVE_LOOP: the common commands without leaving the fetch ------
 *
 * Commands 7 and 8 are four in five the ring carries in the Palace (8 alone
 * is two in three): 7 stores a word into the state block and ORs a byte pick
 * from the table at 0x12D60 into the dirty quadword, 8 stores a word and sets
 * a dirty quadword to {1, 1, 1, 1}. 12 stores a 16-byte record into the
 * state block. 10, 11, 13, 17 and 18 GET 16 to 128 bytes from an EA in the
 * command into the state block, through the GET-and-wait helper at 0xC4D8,
 * and 10 then sets a dirty quadword as 8 does. Together they are nineteen in
 * twenty commands there.
 * For each, the lifted loop still calls 0xC358 to PUT the state (a call
 * bracket around spu_hook_put_wait), decodes the command, branches through
 * the table, runs the handler against registers in memory, calls 0x3C90
 * (spu_hook_ring_post) and branches back to 0x4618, the fetch. With the loop
 * on, the fetch block does those steps itself and goes on to the next
 * command:
 *     0x4730  the call bracket and spu_hook_put_wait, as the lifted call runs
 *             them (r0 = the link, the hook's stack stores and channel
 *             accesses, pc, drain_ret_pc and the image id after the return);
 *     0x4734  the dispatch: nothing it writes is read again (r2-r79 are
 *             volatile and dead at the handler's end -- see below);
 *     handler the lifted instructions in their order, with the volatile
 *             registers kept in locals: the same local-store reads and
 *             writes, r80 (commands 7 and 12) and r81 = r83 & 255, r5 = the
 *             command. A GET is the call bracket and 0xC4D8's main case as
 *             the lifted code runs them: the link at sp + 0x10 and the back
 *             chain at sp - 0x20, the latches, the GET on tag 0 and its wait,
 *             r0 read back from sp + 0x10;
 *     0x4808  spu_hook_ring_post;
 *     0x4818  r81 == 0, back to 0x4618: the fetch block again.
 * plus ctx->steps for the region branches the lifted path takes (three, six
 * or seven).
 *
 * Every condition that would take the lifted path off this course is checked
 * before the command's first side effect, and any of them leaves the command
 * to the lifted code from 0x4730, with the fetch's registers written as
 * always: another command; r83 & 255 != 0 (the loop would end); a put_wait
 * or post case the hooks leave to the lifted code (state EA not 16-byte
 * aligned; the frame; the local buffer at its limit, a segment switch); a GET
 * 0xC4D8 would halt on (LSA or EA not 16-byte aligned) or that runs off the
 * end of local store; a handler store or GET into the ring's own quadwords
 * 0x13300-0x135FF or the frame (sp - 0x100 to sp + 0x20), which those steps
 * read; and whatever a
 * region branch would hand to the trampoline -- the slow-goto knobs, the
 * lockstep scheduler, a PM flow trace, a pending interrupt -- or an LS or
 * channel diagnostic, a knob change, the post check or the histogram.
 *
 * Registers: r2-r79 are volatile in the SPU ABI. At 0x4618 none is live: the
 * branch there comes from 0x4818, after a call, and the fetch writes every
 * one it reads before reading it (the same rule spu_hook_put_wait already
 * relies on). So the handlers' scratch values stay in host locals, and what
 * reaches ctx is what can be read later: r0 and r2-r8, r29, r30 from the
 * hooks, r5 and r81 from the handler, r80 from commands 7 and 12.
 *
 * SPU_NATIVE_LOOP (live): 1 (default) on; 0 off, for A/B; 2 checks: every
 * SPU_NATIVE_LOOP_EVERY-th command it would run (default 256) runs lifted
 * instead, against a prediction made on a copy of local store, and the next
 * fetch compares all of local store outside the frame and the segment
 * quadword, and r80 and r81 ([ring-loop] lines; a mismatch turns the loop off
 * on that thread for the session). A GET's prediction copies the guest bytes
 * when the command is sampled, so a source the PPU rewrites in the moment
 * between would read as a mismatch. */
int spu_hook_put_wait(spu_context* ctx);
int spu_hook_put_wait_defer(spu_context* ctx);
void spu_put_flush(spu_context* ctx, uint32_t eah, uint32_t eal, uint32_t lsa, uint32_t size);

#define RING_DIRTY_PTR_LS 0x133A0u   /* word 0: the dirty block */
#define RING_STATE_PTR_LS 0x13390u   /* word 0: the state block */

static int native_loop_mode(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("SPU_NATIVE_LOOP", 1));
    return on;
}
static int native_loop_every(void)
{
    static int n = -1;
    PS3_KNOB(n, ps3_env_int("SPU_NATIVE_LOOP_EVERY", 256));
    return n > 0 ? n : 1;
}
/* SPU_NATIVE_LOOP_V2 (live): 1 (default) the loop runs commands 10-13, 17
 * and 18 too; 0 only 7 and 8, as before, so one exe A/Bs the two. */
static int native_loop_v2(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("SPU_NATIVE_LOOP_V2", 1));
    return on;
}

/* spu_ls_read128 / write128 on any copy of local store (no probes: the loop
 * is off while g_spu_ls_dbg is set). */
RING_INLINE u128 lsq(const uint8_t* ls, uint32_t lsa)
{
    const uint8_t* p = ls + (lsa & (SPU_LS_MASK & ~0xFu));
    u128 v;
    v._u32[0] = be32_at(p); v._u32[1] = be32_at(p + 4);
    v._u32[2] = be32_at(p + 8); v._u32[3] = be32_at(p + 12);
    return v;
}
RING_INLINE void lsq_put(uint8_t* ls, uint32_t lsa, u128 v)
{
    uint8_t* p = ls + (lsa & (SPU_LS_MASK & ~0xFu));
    for (int i = 0; i < 4; i++) {
        p[4 * i] = (uint8_t)(v._u32[i] >> 24); p[4 * i + 1] = (uint8_t)(v._u32[i] >> 16);
        p[4 * i + 2] = (uint8_t)(v._u32[i] >> 8); p[4 * i + 3] = (uint8_t)v._u32[i];
    }
}

/* One command, worked out from local store as it is before the command: the
 * handlers read only 0x13560, 0x13390 and 0x133A0 besides their own targets
 * and the table, and the targets are checked to miss those. */
typedef struct loop_plan {
    unsigned cmd;
    unsigned steps;              /* region branches of the lifted path */
    uint32_t tgt[4];             /* the quadwords the handler stores to */
    int ntgt;
    int get;                     /* a GET through 0xC4D8 */
    uint32_t get_lsa, get_eah, get_eal, get_size, get_link;
} loop_plan;

/* The arguments 10, 11, 13, 17 and 18 pass 0xC4D8: r3 = the LSA, r4 = the EA
 * (words 0-1), r5 = the size; lifted, 0x4AA0 and 0x4AF0. */
RING_INLINE void loop_get_args(const uint8_t* ls, unsigned cmd, loop_plan* p)
{
    const u128 c = lsq(ls, RING_CMD_LS);
    const u128 st = lsq(ls, RING_STATE_PTR_LS);
    u128 r3, r4, r5;
    if (cmd == 10u) {
        r5 = spu_il(32);
        const u128 r2 = spu_rotqbyi(c, 4);
        const u128 r78 = spu_rotqbyi(c, 8);
        const u128 r77 = spu_shli(r2, 5);
        r4 = spu_rotqmbyi(r78, -4);
        const u128 r75 = spu_ai(r77, 240);
        r3 = spu_a(r75, st);
        p->get_link = 0x4AD0;
    } else {
        const int off = cmd == 11u ? 10192 : cmd == 13u ? 1136 : cmd == 17u ? 1152 : 1184;
        r5 = spu_il(cmd == 11u ? 128 : cmd == 13u ? 16 : cmd == 17u ? 32 : 48);
        r3 = spu_a(st, spu_il(off));
        r4 = spu_rotqmbyi(spu_rotqbyi(c, 4), -4);
        p->get_link = 0x4AFC;
    }
    p->get_lsa = r3._u32[0];
    p->get_eah = r4._u32[0];
    p->get_eal = spu_shlqbyi(r4, 4)._u32[0];
    p->get_size = r5._u32[0];
}

RING_INLINE uint32_t loop_dirty_target(const uint8_t* ls)   /* 0x4A08's store */
{
    const uint32_t w1 = lsq(ls, RING_CMD_LS)._u32[1];
    return (w1 & 16u) + lsq(ls, RING_DIRTY_PTR_LS)._u32[0] + 16u;
}

/* 0, or the plan for the command at 0x13560. */
RING_INLINE int loop_plan_cmd(const uint8_t* ls, unsigned cmd, loop_plan* p)
{
    const u128 c = lsq(ls, RING_CMD_LS);
    const uint32_t w1 = c._u32[1], w2 = c._u32[2];
    const uint32_t state = lsq(ls, RING_STATE_PTR_LS)._u32[0];
    p->cmd = cmd;
    p->get = 0;
    switch (cmd) {
    case 7:
        p->tgt[0] = 4u * w1 + state + 10320u;
        p->tgt[1] = lsq(ls, RING_DIRTY_PTR_LS)._u32[0];
        p->ntgt = 2; p->steps = 3;
        break;
    case 8:
        p->tgt[0] = 4u * (24u * w1 + w2) + state + 10672u;
        p->tgt[1] = loop_dirty_target(ls);
        p->ntgt = 2; p->steps = 3;
        break;
    case 12: {
        const uint32_t r16 = 16u * ((w1 >> 24) & 255u) + state;
        p->tgt[0] = r16 + 890u; p->tgt[1] = r16 + 888u; p->tgt[2] = r16 + 0x370u; p->tgt[3] = r16 + 884u;
        p->ntgt = 4; p->steps = 3;
        break;
    }
    case 10: case 11: case 13: case 17: case 18:
        loop_get_args(ls, cmd, p);
        p->get = 1;
        p->ntgt = 0;
        if (cmd == 10u) p->tgt[p->ntgt++] = loop_dirty_target(ls);
        p->steps = cmd == 11u ? 6 : 7;
        break;
    default:
        return 0;
    }
    for (int i = 0; i < p->ntgt; i++) p->tgt[i] &= SPU_LS_MASK & ~0xFu;
    return 1;
}

/* A shufb by a cwd / cwx mask stored back where its quadword came from
 * changes one word: the value's preferred word lands on the word the
 * address names and the other twelve bytes are stored as they were. So it
 * is that word's store; chd's is the halfword's. SPU_LOOP_INSERTS=0 (live)
 * runs the load, the shuffle and the 16-byte store again, as the lifted
 * instructions do. */
static int loop_inserts(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("SPU_LOOP_INSERTS", 1));
    return on;
}
RING_INLINE void ls_put_word(uint8_t* ls, uint32_t lsa, uint32_t v)
{
    uint8_t* p = ls + (lsa & (SPU_LS_MASK & ~3u));
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
RING_INLINE void ls_put_half(uint8_t* ls, uint32_t lsa, uint32_t v)
{
    uint8_t* p = ls + (lsa & (SPU_LS_MASK & ~1u));
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

/* 0x4958: command 7, the lifted instructions in order. Returns r80. */
RING_INLINE u128 loop_cmd7(uint8_t* ls)
{
    const u128 r37 = spu_il(10320);
    const u128 r50 = lsq(ls, RING_CMD_LS);
    const u128 r46 = spu_ila(0x12D60);
    const u128 r44 = lsq(ls, RING_STATE_PTR_LS);
    const u128 r47 = spu_rotqbyi(r50, 4);
    const u128 r41 = spu_rotqbyi(r50, 8);
    const u128 r43 = spu_shli(r47, 2);
    const u128 r45 = spu_andi(r47, 240);
    const u128 r35 = spu_andi(r47, 15);
    const u128 r36 = spu_a(r45, r46);
    const u128 r38 = spu_a(r43, r44);
    if (loop_inserts()) {
        ls_put_word(ls, r38._u32[0] + r37._u32[0], r41._u32[0]);    /* cwx, shufb */
    } else {
        const u128 r40 = lsq(ls, r38._u32[0] + r37._u32[0]);
        const u128 r42 = spu_cwx(r38, r37);
        const u128 r39 = spu_shufb(r41, r40, r42);
        lsq_put(ls, r38._u32[0] + r37._u32[0], r39);
    }
    const u128 r80 = lsq(ls, RING_DIRTY_PTR_LS);
    const u128 r34 = lsq(ls, r36._u32[0]);
    const u128 r33 = lsq(ls, r80._u32[0]);
    const u128 r32 = spu_shufb(r34, r34, r35);
    const u128 r31 = spu_or(r32, r33);
    lsq_put(ls, r80._u32[0], r31);
    return r80;
}

/* 0x4A08: the dirty quadword = {1, 1, 1, 1} (the end of 8 and 10). */
RING_INLINE void loop_4a08(uint8_t* ls)
{
    const u128 r10 = spu_il(1);
    const u128 r11 = lsq(ls, RING_CMD_LS);
    const u128 r8 = lsq(ls, RING_DIRTY_PTR_LS);
    const u128 r9 = spu_rotqbyi(r11, 4);
    const u128 r14 = spu_ai(r8, 16);
    const u128 r6 = spu_andi(r9, 16);
    const u128 r13 = spu_a(r6, r14);
    lsq_put(ls, r13._u32[0], r10);
}

/* 0x4B08: command 12. Returns r80. */
RING_INLINE u128 loop_cmd12(uint8_t* ls)
{
    const u128 r80 = spu_il(890);
    const u128 r41 = lsq(ls, RING_CMD_LS);
    const u128 r26 = spu_il(888);
    const u128 r36 = lsq(ls, RING_STATE_PTR_LS);
    const u128 r15 = spu_il(884);
    const u128 r40 = spu_rotqbyi(r41, 1);
    const u128 r37 = spu_rotqbyi(r41, 2);
    const u128 r38 = spu_andi(r40, 255);
    const u128 r33 = spu_andi(r37, 255);
    const u128 r35 = spu_shli(r38, 4);
    const u128 r16 = spu_a(r35, r36);
    if (loop_inserts()) {
        /* chd 10 / 8 and cwd 0 / 4 at r16, into the quadwords at r16 + 890,
         * 888, 0x370 and 884: the same low four bits, so those addresses. */
        const u128 r30 = lsq(ls, RING_CMD_LS);
        ls_put_half(ls, r16._u32[0] + 890u, r33._u32[0]);
        ls_put_half(ls, r16._u32[0] + 888u, spu_rotqbyi(r30, 4)._u32[0]);
        const u128 r25 = lsq(ls, RING_CMD_LS);
        ls_put_word(ls, r16._u32[0] + 0x370u, spu_rotqbyi(r25, 8)._u32[0]);
        const u128 r21 = lsq(ls, RING_CMD_LS);
        ls_put_word(ls, r16._u32[0] + 884u, spu_rotqbyi(r21, 12)._u32[0]);
        return r80;
    }
    const u128 r32 = lsq(ls, r16._u32[0] + r80._u32[0]);
    const u128 r34 = spu_chd(r16, 10);
    const u128 r29 = spu_chd(r16, 8);
    const u128 r24 = spu_cwd(r16, 0);
    const u128 r20 = spu_cwd(r16, 4);
    const u128 r31 = spu_shufb(r33, r32, r34);
    lsq_put(ls, r16._u32[0] + r80._u32[0], r31);
    const u128 r30 = lsq(ls, RING_CMD_LS);
    const u128 r27 = lsq(ls, r16._u32[0] + r26._u32[0]);
    const u128 r28 = spu_rotqbyi(r30, 4);
    const u128 r7 = spu_shufb(r28, r27, r29);
    lsq_put(ls, r16._u32[0] + r26._u32[0], r7);
    const u128 r25 = lsq(ls, RING_CMD_LS);
    const u128 r4 = lsq(ls, r16._u32[0] + 0x370);
    const u128 r23 = spu_rotqbyi(r25, 8);
    const u128 r22 = spu_shufb(r23, r4, r24);
    lsq_put(ls, r16._u32[0] + 0x370, r22);
    const u128 r21 = lsq(ls, RING_CMD_LS);
    const u128 r18 = lsq(ls, r16._u32[0] + r15._u32[0]);
    const u128 r19 = spu_rotqbyi(r21, 12);
    const u128 r17 = spu_shufb(r19, r18, r20);
    lsq_put(ls, r16._u32[0] + r15._u32[0], r17);
    return r80;
}

/* 0x49C0, then 0x4A08: command 8. */
RING_INLINE void loop_cmd8(uint8_t* ls)
{
    const u128 r51 = spu_il(10672);
    const u128 r67 = lsq(ls, RING_CMD_LS);
    const u128 r58 = lsq(ls, RING_STATE_PTR_LS);
    const u128 r65 = spu_rotqbyi(r67, 4);
    const u128 r61 = spu_rotqbyi(r67, 8);
    const u128 r55 = spu_rotqbyi(r67, 12);
    const u128 r64 = spu_a(r65, r65);
    const u128 r62 = spu_a(r64, r65);
    const u128 r60 = spu_shli(r62, 3);
    const u128 r59 = spu_a(r60, r61);
    const u128 r57 = spu_shli(r59, 2);
    const u128 r52 = spu_a(r57, r58);
    if (loop_inserts()) {
        ls_put_word(ls, r52._u32[0] + r51._u32[0], r55._u32[0]);    /* cwx, shufb */
    } else {
        const u128 r54 = lsq(ls, r52._u32[0] + r51._u32[0]);
        const u128 r56 = spu_cwx(r52, r51);
        const u128 r53 = spu_shufb(r55, r54, r56);
        lsq_put(ls, r52._u32[0] + r51._u32[0], r53);
    }
    loop_4a08(ls);
}

/* A store target that misses the ring's quadwords and the frame. */
RING_INLINE int loop_target_ok(uint32_t a, uint32_t sp)
{
    if (a >= 0x13300u && a < 0x13600u) return 0;
    return !(a + 16u > sp - 0x100u && a < sp + 0x20u);
}

/* SPU_NATIVE_LOOP=2: per host thread, so per context. */
typedef struct loop_check {
    uint8_t* ls;                 /* the prediction: local store after the command */
    u128 r80;
    unsigned cmd;
    uint32_t sp;
    int pending, bad, prints;
    unsigned long long n, miss, skip;
} loop_check;
static SPU_THREAD_LOCAL loop_check t_loop;
static SPU_THREAD_LOCAL unsigned t_loop_count;

/* What would hand the next region branch to the trampoline, or turn a hook
 * or this loop off. */
RING_INLINE int loop_yield(const spu_context* ctx, unsigned gen)
{
    return g_spu_goto_slow || g_yz_lockstep_on || g_pm_flow_ctx ||
           (ctx->int_enable && (__atomic_load_n(&ctx->event_status, __ATOMIC_RELAXED) & ctx->event_mask)) ||
           ps3_env_gen != gen || g_spu_trampoline_fn || g_spu_ls_dbg || g_spu_ch_diag ||
           g_post_checking || g_spu_ring_hist || t_post_bad || t_loop.bad;
}

/* The checks that depend on the command itself: one of the commands above,
 * its stores and its GET clear of the ring's quadwords and the frame at sp. */
RING_INLINE int loop_cmd_ok(const uint8_t* ls, loop_plan* p, uint32_t sp, int v2)
{
    if (!loop_plan_cmd(ls, ls[RING_CMD_LS + 3], p)) return 0;
    if (p->cmd != 7u && p->cmd != 8u && !v2) return 0;
    for (int i = 0; i < p->ntgt; i++)
        if (!loop_target_ok(p->tgt[i], sp)) return 0;
    if (p->get) {
        /* 0xC4D8's main case (it halts on a misaligned one), inside local
         * store, every quadword it fills clear of the ring and the frame. */
        const uint32_t lsa = p->get_lsa & SPU_LS_MASK, size = p->get_size;
        if ((p->get_lsa & 15u) || (p->get_eal & 15u) || lsa + size > SPU_LS_SIZE) return 0;
        for (uint32_t a = lsa; a < lsa + size; a += 16)
            if (!loop_target_ok(a, sp)) return 0;
    }
    return 1;
}

/* The command at 0x13560 (fetched, the lifted code about to go on at 0x4730)
 * can run here: one of the commands above, and every check. */
RING_INLINE int loop_case_ok(const spu_context* ctx, loop_plan* p)
{
    const uint8_t* ls = ctx->ls;
    const u128* r = ctx->gpr;
    const uint32_t sp = r[1]._u32[0];
    if (!loop_cmd_ok(ls, p, sp, native_loop_v2())) return 0;
    if ((r[83]._u32[0] & 255u) != 0) return 0;
    /* put_wait's case: r3 = the state's EA, r4 = 0x13400, r5 = 16. */
    if ((r[3]._u32[1] & 15u) || r[4]._u32[0] != RING_STATE_LS || r[5]._u32[0] != 16u) return 0;
    /* post's frame, and its local buffer under the limit. */
    if ((sp & 15u) || sp < 0x100u || sp > SPU_LS_SIZE - 0x20u) return 0;
    if (sp - 0x100u < RING_DESC_LS + 16u && sp + 0x20u > RING_SEG_LS) return 0;
    const u128 d = lsq(ls, RING_DESC_LS);
    if (!(d._u32[1] > d._u32[2])) return 0;
    return 1;
}

/* The command's local-store effects on any copy (ctx->ls when run, the check
 * mode's prediction otherwise) except a GET, which the caller does. Returns
 * r80 as the command leaves it. */
RING_INLINE u128 loop_cmd_ls(uint8_t* ls, unsigned cmd, u128 r80)
{
    switch (cmd) {
    case 7: return loop_cmd7(ls);
    case 8: loop_cmd8(ls); break;
    case 12: return loop_cmd12(ls);
    case 10: loop_4a08(ls); break;       /* after its GET */
    default: break;
    }
    return r80;
}

/* A brsl to 0xC4D8 and its main case, as the lifted code runs them (r0 = the
 * link is set by the caller, as the brsl sets it). */
RING_INLINE void loop_get_wait(spu_context* ctx, const loop_plan* p)
{
    u128* r = ctx->gpr;
    const int32_t si = (int32_t)ctx->image_id;
    ctx->host_depth++;
    const uint32_t sp = r[1]._u32[0];
    spu_ls_write128(ctx, sp + 0x10, r[0]);
    spu_ls_write128(ctx, sp - 0x20, r[1]);
    ctx->mfc_lsa = p->get_lsa;
    ctx->mfc_eah = p->get_eah;
    ctx->mfc_eal = p->get_eal;
    ctx->mfc_size = p->get_size;
    ctx->mfc_tag = 0;
    ctx->mfc_tag_mask = 1;
    if (ring_xfer_wait_on()) {
        spu_wrch_xfer_wait(ctx, 64, 1);          /* GET, then ANY: waits for tag 0 */
    } else {
        spu_wrch_word(ctx, MFC_Cmd, 64);         /* GET */
        spu_wrch_word(ctx, MFC_WrTagUpdate, 1);  /* ANY: waits for tag 0 */
    }
    r[3] = spu_make_preferred_u32(ctx->mfc_tag_status);
    r[0] = spu_ls_read128(ctx, sp + 0x10);
    ctx->pc = r[0]._u32[0];                      /* SPU_RET */
    ctx->drain_ret_pc = 0;                       /* spu_call_done */
    ctx->host_depth--;
    ctx->image_id = ctx->module_img_a00 ? ctx->module_img_a00 : si;
}

RING_NOINLINE void loop_check_start(spu_context* ctx, const loop_plan* p)
{
    loop_check* k = &t_loop;
    if (p->get && (p->get_eah || p->get_eal < 0x10000u)) return;   /* not the GET fast path */
    if (!k->ls && !(k->ls = (uint8_t*)malloc(SPU_LS_SIZE))) return;
    memcpy(k->ls, ctx->ls, SPU_LS_SIZE);
    if (p->get) memcpy(k->ls + (p->get_lsa & SPU_LS_MASK), vm_base + p->get_eal, p->get_size);
    k->r80 = loop_cmd_ls(k->ls, p->cmd, ctx->gpr[80]);
    k->cmd = p->cmd;
    k->sp = ctx->gpr[1]._u32[0];
    k->pending = 1;
}

/* At the next fetch: local store as the lifted command left it, outside the
 * frame (put_wait's and post's stack stores) and the segment quadword
 * (post's store), and r80 and r81. */
RING_NOINLINE void loop_check_end(spu_context* ctx)
{
    loop_check* k = &t_loop;
    k->pending = 0;
    if ((uint32_t)(ctx->pc & SPU_LS_MASK) != RING_POST_END) { k->skip++; return; }   /* something else ran */
    const uint32_t lo = k->sp - 0x100u, hi = k->sp + 0x20u;
    uint32_t bad = ~0u;
    for (uint32_t a = 0; a < SPU_LS_SIZE; a += 16) {
        if ((a + 16u > lo && a < hi) || a == RING_SEG_LS) continue;
        if (memcmp(k->ls + a, ctx->ls + a, 16)) { bad = a; break; }
    }
    const u128* r = ctx->gpr;
    const int regs_bad = memcmp(&k->r80, &r[80], 16) != 0 ||
                         (r[81]._u32[0] | r[81]._u32[1] | r[81]._u32[2] | r[81]._u32[3]) != 0;
    k->n++;
    if (bad != ~0u || regs_bad) {
        k->miss++;
        k->bad = 1;
        if (k->prints++ < 8) {
            const uint32_t a = bad == ~0u ? 0u : bad;
            fprintf(stderr, "[ring-loop] spu=%u cmd %u MISMATCH at LS 0x%05X (r80/r81 %s): "
                    "lifted %08X %08X %08X %08X, loop %08X %08X %08X %08X -- loop off on this thread\n",
                    ctx->spu_id, k->cmd, a, regs_bad ? "differ" : "same",
                    be32_at(ctx->ls + a), be32_at(ctx->ls + a + 4), be32_at(ctx->ls + a + 8), be32_at(ctx->ls + a + 12),
                    be32_at(k->ls + a), be32_at(k->ls + a + 4), be32_at(k->ls + a + 8), be32_at(k->ls + a + 12));
            fflush(stderr);
        }
    }
    if ((k->n & (k->n - 1)) == 0 && k->n >= 64) {
        fprintf(stderr, "[ring-loop] spu=%u check: %llu compared, %llu mismatches, %llu skipped\n",
                ctx->spu_id, k->n, k->miss, k->skip);
        fflush(stderr);
    }
}

/* Each command PUTs the consumer's state (get) back to memory, where the
 * producer reads it before every command it writes, to see whether the ring
 * is full: one transfer of that line each way per command. Inside the loop
 * the PUT can wait for the next few commands: what reaches memory is the
 * newest state, as the last of those PUTs would have written it, only later,
 * as from a slower consumer. Every way out of the loop PUTs first, so the
 * producer always sees get before this SPU waits for a command or runs
 * lifted code, and a full ring still drains: the loop goes on while there
 * are commands, and PUTs every n-th.
 *
 * SPU_RING_PUT_EVERY=<n> (live): PUT the state every n-th command in the
 * loop (default 16); 1 every command, as the program does. In the deep
 * Palace the frame rate rose with n up to 16 (+8%) and stayed there to no
 * limit at all. */
static int ring_put_every(void)
{
    static int n = -1;
    PS3_KNOB(n, ps3_env_int("SPU_RING_PUT_EVERY", 16));
    return n;
}
typedef struct ring_put_pend { int n; uint32_t eah, eal, lsa, size; } ring_put_pend;
RING_INLINE void ring_put_out(spu_context* ctx, ring_put_pend* p)
{
    if (p->n) { spu_put_flush(ctx, p->eah, p->eal, p->lsa, p->size); p->n = 0; }
}

/* ---- SPU_RING_LAZY: a run of commands, written out once at its end -------
 *
 * Inside ring_loop every command leaves every register, the frame and the
 * context fields as the lifted path leaves them at that command's end, and
 * the next command writes nearly all of them again before anything reads
 * them:
 *     the fetch's registers (r2-r8, r10, r13, r14, r52-r58, r61, r63, r70-r76,
 *         r78, r79, r81): volatile, and read only by the put at 0x4730 (r3,
 *         r4, r5: the state's EA, 0x13400, 16, the same for the whole run)
 *         and by the lifted code if the run ends there;
 *     the post's registers (r0, r2-r8, r29, r30), pc, drain_ret_pc and the
 *         image id: the same in every command but r5 and r8, and the fetch
 *         and the next post write them again;
 *     put_wait's and 0xC4D8's stack stores (the link at sp + 0x10, the back
 *         chain at sp - 0x20) and the post's 13 prologue stores: the post
 *         stores both slots again, with values that change only with r80;
 *         nothing in the loop reads the frame;
 *     the segment quadword: the post stores cur into it, and no command the
 *         loop runs moves cur (their stores miss 0x13300-0x135FF);
 *     the tag waits of a deferred PUT and a skipped GET: they read tag 0's
 *         completion in this context's own engine, which only this context's
 *         transfers change, and every transfer in the run does its own wait,
 *         so such a wait reads what the last one read (with the event mask
 *         at 1024, as every fetch sets it, no tag event either).
 * So ring_lazy keeps r80, the last command and the last fetch's reads in
 * locals and writes the registers, the frame and the segment quadword once,
 * when the run ends, as the last command's post and (if it ends at 0x4730)
 * the last fetch leave them: ring_post_frame, ring_post_regs and
 * ring_fetch_regs, the same code the per-command path runs. Per command it
 * does the rest as that path does: the latches, the PUT every n-th command,
 * the GETs, the handler's local-store stores, the fetch's local-store and
 * channel work. The checks that hold for the whole run (r83, the put's case,
 * the frame and the buffer's limit, the knobs; a knob change ends the run)
 * are made for its first command only; each command's own checks
 * (loop_cmd_ok) for every one.
 *
 * SPU_RING_LAZY (live): 1 (default) as above; 0 every command writes all of
 * it, as before, for A/B. */
static int ring_lazy_mode(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("SPU_RING_LAZY", 1));
    return on;
}
int spu_put_hook_on(void);

/* A transfer and its tag wait as the hooks issue them (cmd 0: the wait). */
RING_INLINE void ring_xfer(spu_context* ctx, int one, uint32_t cmd, uint32_t mode)
{
    if (one) {
        spu_wrch_xfer_wait(ctx, cmd, mode);
    } else {
        if (cmd) spu_wrch_word(ctx, MFC_Cmd, cmd);
        spu_wrch_word(ctx, MFC_WrTagUpdate, mode);
    }
}

/* ring_loop's run of commands from the one the fetch just took. 1: the lifted
 * code goes on at 0x4730 with the command at 0x13560; 0: at 0x4618; -1: the
 * run needs the per-command path (nothing done). */
RING_NOINLINE int ring_lazy(spu_context* ctx)
{
    const unsigned gen = ps3_env_gen;
    u128* r = ctx->gpr;
    loop_plan p;
    if (loop_yield(ctx, gen) || native_post_mode() != 1 || !loop_case_ok(ctx, &p)) return 1;
    const int32_t img = ctx->image_id;
    if (!spu_put_hook_on() || (ctx->module_img_a00 && ctx->module_img_a00 != img)) return -1;
    const int every = ring_put_every(), v2 = native_loop_v2(), one = ring_xfer_wait_on();
    const uint32_t sp = r[1]._u32[0];
    const uint32_t eah = r[3]._u32[0], eal = r[3]._u32[1];   /* the state's home, the PUT's EA */
    const u128 d = spu_ls_read128(ctx, RING_DESC_LS);
    const u128 seg0 = spu_ls_read128(ctx, RING_SEG_LS);
    u128 seg = seg0;
    seg._u32[3] = d._u32[2];                                  /* what each post stores */
    u128 r80 = r[80], cmd;
    ring_put_pend pend = {0};
    ring_fetched f;
    unsigned n = 0, steps = 0;
    int out;
    for (;;) {
        /* 0x4730: the brsl to 0xC358, spu_hook_put_wait's work. */
        int defer = 0;
        if (every > 1) {
            if (pend.n && (pend.eah != eah || pend.eal != eal || pend.lsa != RING_STATE_LS || pend.size != 16u))
                ring_put_out(ctx, &pend);
            defer = pend.n + 1 < every;
            pend.eah = eah; pend.eal = eal; pend.lsa = RING_STATE_LS; pend.size = 16;
        }
        ctx->mfc_lsa = RING_STATE_LS;
        ctx->mfc_eah = eah;
        ctx->mfc_eal = eal;
        ctx->mfc_size = 16;
        ctx->mfc_tag = 0;
        ctx->mfc_tag_mask = 1;
        if (!defer) ring_xfer(ctx, one, 32, 1);
        else if (ctx->event_mask & 1u) ring_xfer(ctx, one, 0, 1);
        pend.n = defer ? pend.n + 1 : 0;
        /* 0x4734 and the handler: a GET through 0xC4D8's main case, then the
         * handler's stores. */
        if (p.get) {
            ctx->mfc_lsa = p.get_lsa;
            ctx->mfc_eah = p.get_eah;
            ctx->mfc_eal = p.get_eal;
            ctx->mfc_size = p.get_size;
            ctx->mfc_tag = 0;
            ctx->mfc_tag_mask = 1;
            ring_xfer(ctx, one, 64, 1);
        }
        r80 = loop_cmd_ls(ctx->ls, p.cmd, r80);
        cmd = spu_ls_read128(ctx, RING_CMD_LS);
        steps += p.steps;
        n++;
        /* 0x4808: the post, written out at the end. 0x4818: back to 0x4618. */
        if (loop_yield(ctx, gen) || !ring_fetch_take(ctx, &f, 1)) { out = 0; break; }
        if (loop_yield(ctx, gen) || !loop_cmd_ok(ctx->ls, &p, sp, v2)) { out = 1; break; }
    }
    /* The last command's post, then (at 0x4730) the last fetch. */
    r[80] = r80;
    r[81] = spu_andi(r[83], 255);
    ring_post_frame(ctx, sp, spu_link(RING_POST_END), seg);
    ring_post_regs(ctx, d, n > 1 ? seg : seg0, spu_andi(cmd, 255), img);
    ctx->steps += steps;
    if (out) ring_fetch_regs(ctx, &f);
    ring_put_out(ctx, &pend);
    return out;
}

/* From the fetch, which just took a command: run the commands it can. 1: the
 * lifted code goes on at 0x4730 with the command at 0x13560; 0: at 0x4618. */
RING_NOINLINE int ring_loop(spu_context* ctx, int mode)
{
    if (mode == 1 && ring_lazy_mode() == 1) {
        const int out = ring_lazy(ctx);
        if (out >= 0) return out;
    }
    const unsigned gen = ps3_env_gen;
    const int every = ring_put_every();
    ring_put_pend pend = {0};
    for (;;) {
        loop_plan p;
        if (loop_yield(ctx, gen) || native_post_mode() != 1 || !loop_case_ok(ctx, &p)) {
            ring_put_out(ctx, &pend);
            return 1;
        }
        if (mode == 2) {
            if (++t_loop_count % (unsigned)native_loop_every() == 0) loop_check_start(ctx, &p);
            return 1;
        }
        u128* r = ctx->gpr;
        /* 0x4730: brsl to 0xC358, whose entry hook does the PUT. */
        r[0] = spu_link(0x4734);
        int defer = 0;
        if (every > 1) {
            const uint32_t eah = r[3]._u32[0], eal = r[3]._u32[1], lsa = r[4]._u32[0], size = r[5]._u32[0];
            if (pend.n && (pend.eah != eah || pend.eal != eal || pend.lsa != lsa || pend.size != size))
                ring_put_out(ctx, &pend);
            defer = pend.n + 1 < every;
            pend.eah = eah; pend.eal = eal; pend.lsa = lsa; pend.size = size;
        }
        const int32_t si = (int32_t)ctx->image_id;
        ctx->host_depth++;
        if (!(defer ? spu_hook_put_wait_defer(ctx) : spu_hook_put_wait(ctx))) {   /* SPU_NATIVE_PUT=0 */
            ctx->host_depth--;
            ring_put_out(ctx, &pend);
            return 1;
        }
        pend.n = defer ? pend.n + 1 : 0;
        ctx->pc = 0x4734;                                                /* SPU_RET */
        ctx->drain_ret_pc = 0;                                           /* spu_call_done */
        ctx->host_depth--;
        ctx->image_id = ctx->module_img_a00 ? ctx->module_img_a00 : si;
        /* 0x4734-0x4758 and the handler. */
        r[81] = spu_andi(r[83], 255);
        if (p.get) {
            r[0] = spu_link(p.get_link);
            loop_get_wait(ctx, &p);
        }
        r[80] = loop_cmd_ls(ctx->ls, p.cmd, r[80]);
        r[5] = spu_ls_read128(ctx, RING_CMD_LS);
        ctx->steps += p.steps - 1;
        /* 0x4808 */
        if (ring_post_run(ctx, NULL) != POST_OK) {   /* cannot happen: loop_case_ok checked its cases */
            fprintf(stderr, "[ring-loop] spu=%u post refused after cmd %u\n", ctx->spu_id, p.cmd);
            fflush(stderr);
            abort();
        }
        /* 0x4818: r81 == 0, the branch back to 0x4618. */
        ctx->steps++;
        if (loop_yield(ctx, gen) || !ring_fetch(ctx)) {
            ring_put_out(ctx, &pend);
            return 0;
        }
    }
}

int spu_hook_ring_fetch(spu_context* ctx)
{
    if (__builtin_expect(g_post_checking, 0)) ring_post_check(ctx);   /* SPU_NATIVE_POST=2 */
    if (__builtin_expect(t_loop.pending, 0)) loop_check_end(ctx);     /* SPU_NATIVE_LOOP=2 */
    const int took = ring_fetch(ctx);
    if (__builtin_expect(g_spu_ring_hist_gen != ps3_env_gen || g_spu_ring_hist, 0))
        ring_hist_count(ctx, took);
    if (took) {
        const int mode = native_loop_mode();
        if (mode) return ring_loop(ctx, mode);
    }
    return took;
}
