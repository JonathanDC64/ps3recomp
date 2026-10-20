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

int spu_hook_ring_fetch(spu_context* ctx)
{
    const int took = ring_fetch(ctx);
    if (__builtin_expect(g_spu_ring_hist_gen != ps3_env_gen || g_spu_ring_hist, 0))
        ring_hist_count(ctx, took);
    return took;
}
