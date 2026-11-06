/*
 * ps3recomp - a trace of the SPURS queue calls (LIBSRE_TRACE, libsre_trace.h)
 *
 * The records, one a line, fields split by spaces, numbers in hex unless said:
 *
 *   C <seq> <t_us> <tid> <name> a=<r3>,...,<r10> lr=<lr>
 *       a traced call starts. seq numbers the call; t_us is microseconds
 *       (decimal) since the trace opened; tid is the guest thread (decimal).
 *   L <seq> <arg> <ea> <256 hex digits>
 *       the 128-byte line at <ea> as the call starts. <arg> is the argument
 *       that named it (3 = r3). An initialize dumps every 128-aligned argument
 *       among r3..r6, the others only their queue.
 *   E <seq> <in|out> <ea> <hex bytes> slot=<n>/<matches>
 *       the entry a push takes from <ea> (in, at the start) or a pop leaves
 *       at <ea> (out, at the end), up to 64 bytes, and the first buffer slot
 *       holding the same bytes at the end, with how many slots do (-1/0:
 *       none, or the queue's buffer is not known).
 *   R <seq> <t_us> <tid> <name> ret=<r3> dt=<us> lle=<0|1> [<off>:<old>><new> ...]
 *       the call ends: its return value, how long it took (decimal), whether
 *       the loaded libsre served it, and each 4-byte word of the queue line
 *       that differs from the start (<off> from the line).
 *   S <seq> <t_us> <tid> <num> a=<r3>,<r4>,<r5>,<r6> -> <r3>,<r4>,<r5>,<r6>,<r7> dt=<us>
 *       an lv2 syscall (num in decimal) made inside traced call <seq>: its
 *       arguments, its r3..r7 at the end (receive leaves the event there).
 *   H <seq> <t_us> <tid> <nid> a=<r3>,<r4>,<r5>,<r6> -> <r3> dt=<us>
 *       another import called inside traced call <seq>.
 *   U <t_us> <line> img=<n> lr=<lr> <off>:<old>><new> ...
 *       an SPU PUTLLC or PUTLLUC changed a watched line (any line a traced call
 *       named): the words it changed.
 *   Z <t_us> <line> commits=<n> fails=<n> (decimal)
 *       every 5 s: the SPU stores to each watched line, and its failed PUTLLCs.
 *
 * What the trace does not see: a plain PPU store to a queue line outside the
 * traced calls, and the SPU's DMA into a queue's buffer.
 */
#include "libsre_trace.h"
#include "../../runtime/platform/plat.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base */
#include "../../runtime/spu/spu_coherency.h" /* spu_lockline_lock_ea */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#  define LT_TLS __declspec(thread)
#else
#  define LT_TLS __thread
#endif

#define LT_LINE      128u
#define LT_WATCH_MAX 32
#define LT_DEPTH     6

/* Hooks the trace installs: the SPU's lock-line stores (spu_channels.c) and
 * the lv2 syscall dispatch (ppu_loader.cpp). Both null while the trace is
 * off. */
extern void (*g_spu_line_store_watch)(uint32_t ea, const uint8_t* before, const uint8_t* after,
                                      int image, uint32_t lr, int stored);
extern void (*g_ppu_sc_watch)(int phase, uint64_t num, const uint64_t* gpr, uint32_t tid);

typedef struct {
    uint32_t    nid;
    const char* name;
    int         qarg;    /* the argument holding the queue (0 = r3) */
    int         data;    /* 1: r4 is a push's source, 2: a pop's destination, 3: an out word */
    int         init;    /* 1: LFQueue init (buffer r5, size r6, depth r7); 2: SpursQueue init (r6, r7, r8) */
} lt_fn;

static const lt_fn s_fns[] = {
    { 0x011EE38Bu, "_cellSpursLFQueueInitialize",         1, 0, 1 },
    { 0x1656D49Fu, "cellSpursLFQueueAttachLv2EventQueue", 0, 0, 0 },
    { 0x8A85674Du, "_cellSpursLFQueuePushBody",           0, 1, 0 },
    { 0x73E06F91u, "cellSpursLFQueueDetachLv2EventQueue", 0, 0, 0 },
    { 0x082BFB09u, "_cellSpursQueueInitialize",           2, 0, 2 },
    { 0xE5443BE7u, "cellSpursQueueAttachLv2EventQueue",   0, 0, 0 },
    { 0x039D70B7u, "cellSpursQueueDetachLv2EventQueue",   0, 0, 0 },
    { 0x91066667u, "cellSpursQueuePopBody",               0, 2, 0 },
    { 0xD59AA307u, "cellSyncLFQueueGetDirection",         0, 3, 0 },
};
#define LT_NFNS ((int)(sizeof s_fns / sizeof s_fns[0]))

typedef struct {
    uint32_t line;
    uint32_t buf, size, depth;           /* from the initialize, 0 if not seen */
    volatile long commits, fails;
} lt_watch;

static lt_watch      s_watch[LT_WATCH_MAX];
static volatile long s_nwatch;

typedef struct {
    int      fn;          /* index in s_fns, -1 for an import inside a traced call */
    uint32_t nid, seq, tid, q;
    int      lle;
    int64_t  t0;
    uint64_t a[8];
    uint8_t  before[LT_LINE];
} lt_call;

static LT_TLS lt_call t_stack[LT_DEPTH];
static LT_TLS int     t_depth;
static LT_TLS int64_t t_sc_t0;
static LT_TLS uint64_t t_sc_a[4];

volatile int     g_libsre_trace_on = -1;
static FILE*     s_out;
static plat_mutex s_mtx;
static int64_t   s_t_open, s_last_flush, s_last_sum;
static long      s_records, s_max;
static volatile long s_seq;

static uint64_t lt_now_us(void)
{
    return plat_ticks_scale((uint64_t)(plat_clock_now() - s_t_open), (uint64_t)plat_clock_freq(), 1000000u);
}

static void lt_summary_locked(void);

/* One record. The FILE has its own buffer; it is flushed at least every
 * 200 ms, so a run that is killed loses little. */
static void lt_write(const char* s, size_t n)
{
    plat_mutex_lock(&s_mtx);
    if (s_records < s_max) {
        fwrite(s, 1, n, s_out);
        if (++s_records == s_max) fputs("# LIBSRE_TRACE_MAX reached: no more records\n", s_out);
    }
    const int64_t now = plat_clock_now(), f = plat_clock_freq();
    if (now - s_last_sum > 5 * f) { s_last_sum = now; lt_summary_locked(); }
    if (now - s_last_flush > f / 5) { s_last_flush = now; fflush(s_out); }
    plat_mutex_unlock(&s_mtx);
}

static void lt_summary_locked(void)
{
    char b[160];
    const long n = s_nwatch;
    for (long i = 0; i < n && s_records < s_max; i++) {
        const int k = snprintf(b, sizeof b, "Z %llu %08X commits=%ld fails=%ld\n",
                               (unsigned long long)lt_now_us(), s_watch[i].line,
                               s_watch[i].commits, s_watch[i].fails);
        fwrite(b, 1, (size_t)k, s_out);
        s_records++;
    }
}

static lt_watch* lt_watch_find(uint32_t line)
{
    const long n = __atomic_load_n(&s_nwatch, __ATOMIC_ACQUIRE);
    for (long i = 0; i < n; i++)
        if (s_watch[i].line == line) return &s_watch[i];
    return NULL;
}

/* Under s_mtx, so two threads naming one new line add it once. */
static lt_watch* lt_watch_add(uint32_t line)
{
    lt_watch* w = lt_watch_find(line);
    if (w || !line) return w;
    plat_mutex_lock(&s_mtx);
    w = lt_watch_find(line);
    if (!w && s_nwatch < LT_WATCH_MAX) {
        w = &s_watch[s_nwatch];
        memset(w, 0, sizeof *w);
        w->line = line;
        __atomic_store_n(&s_nwatch, s_nwatch + 1, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&s_mtx);
    return w;
}

static void lt_read_line(uint32_t line, uint8_t* out)
{
    spu_lockline_lock_ea(line, LT_LINE);
    memcpy(out, vm_base + line, LT_LINE);
    spu_lockline_unlock_ea(line, LT_LINE);
}

static int lt_hex(char* p, const uint8_t* b, unsigned n)
{
    static const char hx[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < n; i++) { p[2 * i] = hx[b[i] >> 4]; p[2 * i + 1] = hx[b[i] & 15]; }
    return (int)(2 * n);
}

static uint32_t lt_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* " off:old>new" for each 4-byte word that differs. */
static int lt_diff(char* p, size_t cap, const uint8_t* a, const uint8_t* b)
{
    int k = 0;
    for (unsigned o = 0; o < LT_LINE && (size_t)k + 24 < cap; o += 4)
        if (memcmp(a + o, b + o, 4))
            k += snprintf(p + k, cap - (size_t)k, " %02X:%08X>%08X", o, lt_be32(a + o), lt_be32(b + o));
    return k;
}

static void lt_spu_store(uint32_t ea, const uint8_t* before, const uint8_t* after,
                         int image, uint32_t lr, int stored)
{
    lt_watch* w = lt_watch_find(ea & ~(LT_LINE - 1));
    if (!w) return;
    if (!stored) { __atomic_add_fetch(&w->fails, 1, __ATOMIC_RELAXED); return; }
    __atomic_add_fetch(&w->commits, 1, __ATOMIC_RELAXED);
    if (!memcmp(before, after, LT_LINE)) return;
    char b[1400];
    int k = snprintf(b, sizeof b, "U %llu %08X img=%d lr=%05X", (unsigned long long)lt_now_us(),
                     w->line, image, lr);
    k += lt_diff(b + k, sizeof b - (size_t)k - 2, before, after);
    b[k++] = '\n';
    lt_write(b, (size_t)k);
}

static void lt_sc(int phase, uint64_t num, const uint64_t* gpr, uint32_t tid)
{
    if (!t_depth) return;
    if (phase == 0) {
        t_sc_t0 = plat_clock_now();
        for (int i = 0; i < 4; i++) t_sc_a[i] = gpr[3 + i];
        return;
    }
    const lt_call* c = &t_stack[t_depth - 1];
    const uint64_t dt = plat_ticks_scale((uint64_t)(plat_clock_now() - t_sc_t0), (uint64_t)plat_clock_freq(), 1000000u);
    char b[300];
    const int k = snprintf(b, sizeof b, "S %u %llu %u %u a=%llX,%llX,%llX,%llX -> %llX,%llX,%llX,%llX,%llX dt=%llu\n",
                           c->seq, (unsigned long long)lt_now_us(), tid, (unsigned)num,
                           (unsigned long long)t_sc_a[0], (unsigned long long)t_sc_a[1],
                           (unsigned long long)t_sc_a[2], (unsigned long long)t_sc_a[3],
                           (unsigned long long)gpr[3], (unsigned long long)gpr[4],
                           (unsigned long long)gpr[5], (unsigned long long)gpr[6],
                           (unsigned long long)gpr[7], (unsigned long long)dt);
    lt_write(b, (size_t)k);
}

int libsre_trace_init(void)
{
    static volatile long s_once;
    if (__atomic_exchange_n(&s_once, 1, __ATOMIC_ACQ_REL)) {
        while (g_libsre_trace_on < 0) plat_relax();
        return g_libsre_trace_on;
    }
    const char* path = getenv("LIBSRE_TRACE");
    FILE* f = (path && *path) ? fopen(path, "w") : NULL;
    if (!f) {
        if (path && *path) fprintf(stderr, "[libsre-trace] cannot open %s: trace off\n", path);
        __atomic_store_n(&g_libsre_trace_on, 0, __ATOMIC_RELEASE);
        return 0;
    }
    setvbuf(f, NULL, _IOFBF, 1u << 20);
    plat_mutex_init(&s_mtx);
    const char* m = getenv("LIBSRE_TRACE_MAX");
    s_max = m ? atol(m) : 2000000L;
    if (s_max <= 0) s_max = 2000000L;
    s_out = f;
    s_t_open = s_last_flush = s_last_sum = plat_clock_now();
    fputs("# libsre trace 1 (format: libs/spurs/libsre_trace.c)\n", f);
    g_spu_line_store_watch = lt_spu_store;
    g_ppu_sc_watch = lt_sc;
    fprintf(stderr, "[libsre-trace] writing %s\n", path);
    __atomic_store_n(&g_libsre_trace_on, 1, __ATOMIC_RELEASE);
    return 1;
}

static int lt_fn_find(uint32_t nid)
{
    for (int i = 0; i < LT_NFNS; i++) if (s_fns[i].nid == nid) return i;
    return -1;
}

static int lt_ea_ok(uint64_t ea, uint32_t len)
{
    return ea >= 0x10000u && ea + len <= 0xF0000000u;
}

void* libsre_trace_enter(uint32_t nid, const uint64_t* gpr, uint32_t tid, uint32_t lr)
{
    const int fi = lt_fn_find(nid);
    if (fi < 0 && !t_depth) return NULL;
    if (t_depth >= LT_DEPTH) return NULL;
    lt_call* c = &t_stack[t_depth];
    memset(c, 0, offsetof(lt_call, before));
    c->fn = fi; c->nid = nid; c->tid = tid;
    for (int i = 0; i < 8; i++) c->a[i] = gpr[3 + i];
    c->t0 = plat_clock_now();
    if (fi < 0) {                       /* an import inside a traced call */
        c->seq = t_stack[t_depth - 1].seq;
        t_depth++;
        return c;
    }
    c->seq = (uint32_t)__atomic_add_fetch(&s_seq, 1, __ATOMIC_RELAXED);
    const lt_fn* fn = &s_fns[fi];
    char b[1800];
    int k = snprintf(b, sizeof b, "C %u %llu %u %s a=%llX,%llX,%llX,%llX,%llX,%llX,%llX,%llX lr=%08X\n",
                     c->seq, (unsigned long long)lt_now_us(), tid, fn->name,
                     (unsigned long long)c->a[0], (unsigned long long)c->a[1], (unsigned long long)c->a[2],
                     (unsigned long long)c->a[3], (unsigned long long)c->a[4], (unsigned long long)c->a[5],
                     (unsigned long long)c->a[6], (unsigned long long)c->a[7], lr);
    const uint64_t q = c->a[fn->qarg];
    if (lt_ea_ok(q, LT_LINE)) {
        c->q = (uint32_t)q & ~(LT_LINE - 1);
        lt_watch* w = lt_watch_add(c->q);
        if (w && fn->init == 1) { w->buf = (uint32_t)c->a[2]; w->size = (uint32_t)c->a[3]; w->depth = (uint32_t)c->a[4]; }
        if (w && fn->init == 2) { w->buf = (uint32_t)c->a[3]; w->size = (uint32_t)c->a[4]; w->depth = (uint32_t)c->a[5]; }
        lt_read_line(c->q, c->before);
        k += snprintf(b + k, sizeof b - (size_t)k, "L %u %d %08X ", c->seq, 3 + fn->qarg, c->q);
        k += lt_hex(b + k, c->before, LT_LINE);
        b[k++] = '\n';
    }
    if (fn->init) {                      /* the other aligned arguments too */
        for (int i = 0; i < 4 && (size_t)k + 300 < sizeof b; i++) {
            if (i == fn->qarg || (c->a[i] & (LT_LINE - 1)) || !lt_ea_ok(c->a[i], LT_LINE)) continue;
            uint8_t l[LT_LINE];
            lt_read_line((uint32_t)c->a[i], l);
            k += snprintf(b + k, sizeof b - (size_t)k, "L %u %d %08X ", c->seq, 3 + i, (uint32_t)c->a[i]);
            k += lt_hex(b + k, l, LT_LINE);
            b[k++] = '\n';
        }
    }
    if (fn->data == 1 && lt_ea_ok(c->a[1], 64)) {
        k += snprintf(b + k, sizeof b - (size_t)k, "E %u in %08X ", c->seq, (uint32_t)c->a[1]);
        k += lt_hex(b + k, vm_base + (uint32_t)c->a[1], 64);
        b[k++] = '\n';
    }
    lt_write(b, (size_t)k);
    t_depth++;
    return c;
}

void libsre_trace_lle(void* tok)
{
    if (tok) ((lt_call*)tok)->lle = 1;
}

/* The first slot of the queue's buffer holding the entry at ea, and how many
 * do. */
static void lt_slot(const lt_call* c, uint32_t ea, int* first, int* matches)
{
    *first = -1; *matches = 0;
    const lt_watch* w = c->q ? lt_watch_find(c->q) : NULL;
    if (!w || !w->buf || !w->size || !w->depth || w->depth > 0x8000u) return;
    const uint32_t n = w->size < 64 ? w->size : 64;
    if (!lt_ea_ok(w->buf, w->size * w->depth) || !lt_ea_ok(ea, n)) return;
    for (uint32_t s = 0; s < w->depth; s++)
        if (!memcmp(vm_base + w->buf + s * w->size, vm_base + ea, n)) {
            if (*first < 0) *first = (int)s;
            ++*matches;
        }
}

void libsre_trace_leave(void* tok, uint64_t r3)
{
    lt_call* c = (lt_call*)tok;
    if (!c) return;
    t_depth--;
    const uint64_t dt = plat_ticks_scale((uint64_t)(plat_clock_now() - c->t0), (uint64_t)plat_clock_freq(), 1000000u);
    char b[1800];
    int k;
    if (c->fn < 0) {
        k = snprintf(b, sizeof b, "H %u %llu %u %08X a=%llX,%llX,%llX,%llX -> %llX dt=%llu\n",
                     c->seq, (unsigned long long)lt_now_us(), c->tid, c->nid,
                     (unsigned long long)c->a[0], (unsigned long long)c->a[1],
                     (unsigned long long)c->a[2], (unsigned long long)c->a[3],
                     (unsigned long long)r3, (unsigned long long)dt);
        lt_write(b, (size_t)k);
        return;
    }
    const lt_fn* fn = &s_fns[c->fn];
    k = snprintf(b, sizeof b, "R %u %llu %u %s ret=%08X dt=%llu lle=%d", c->seq,
                 (unsigned long long)lt_now_us(), c->tid, fn->name, (uint32_t)r3,
                 (unsigned long long)dt, c->lle);
    if (c->q) {
        uint8_t after[LT_LINE];
        lt_read_line(c->q, after);
        k += lt_diff(b + k, sizeof b - (size_t)k - 300, c->before, after);
    }
    b[k++] = '\n';
    int first, matches;
    if (fn->data == 1 && lt_ea_ok(c->a[1], 64)) {
        lt_slot(c, (uint32_t)c->a[1], &first, &matches);
        k += snprintf(b + k, sizeof b - (size_t)k, "E %u slot %08X slot=%d/%d\n", c->seq,
                      (uint32_t)c->a[1], first, matches);
    }
    if (fn->data == 2 && lt_ea_ok(c->a[1], 64)) {
        lt_slot(c, (uint32_t)c->a[1], &first, &matches);
        k += snprintf(b + k, sizeof b - (size_t)k, "E %u out %08X ", c->seq, (uint32_t)c->a[1]);
        k += lt_hex(b + k, vm_base + (uint32_t)c->a[1], 64);
        k += snprintf(b + k, sizeof b - (size_t)k, " slot=%d/%d\n", first, matches);
    }
    if (fn->data == 3 && lt_ea_ok(c->a[1], 4))
        k += snprintf(b + k, sizeof b - (size_t)k, "E %u out %08X %08X\n", c->seq,
                      (uint32_t)c->a[1], lt_be32(vm_base + (uint32_t)c->a[1]));
    lt_write(b, (size_t)k);
}
