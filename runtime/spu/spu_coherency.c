/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is deliberately
 * plain: a bit per 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line spinlock. Everything that WRITES it runs
 * with the lock-line lock held (the SPU's GETLLAR, the PPU's coherent store),
 * so the read-modify-write on a shared bitmap byte is serialized. Only
 * spu_coh_is_reserved reads without the lock, which is the entire point: it is
 * on every PPU store.
 */

#include <stdlib.h>
#include "ps3emu/ps3_knob.h"
#include "spu_coherency.h"

/* A variable alone on a 64-byte cache line. */
#if defined(_MSC_VER)
#define SPU_COH_LINE(T) __declspec(align(64)) struct { T v; char pad_[64 - sizeof(T)]; }
#else
#define SPU_COH_LINE(T) struct __attribute__((aligned(64))) { T v; char pad_[64 - sizeof(T)]; }
#endif

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * The lock-line lock
 *
 * Moved here from spu_channels.c, where it was a file-static and so could
 * serialize SPU against SPU but not SPU against PPU -- which left the PPU free
 * to write a line in the middle of a PUTLLC's compare-and-commit. Same
 * spinlock, same semantics, now shared. _InterlockedExchange is a clang-cl/MSVC
 * intrinsic (no runtime library symbol needed); elsewhere use the C11
 * equivalent, which lowers to the same LL/SC or lock-xchg on every target.
 * ===========================================================================*/
#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>   /* SwitchToThread */
/* On a cache line of its own: SPU atomics and PPU stores into reserved
 * lines take this lock millions of times a second, and anything sharing its
 * line (s_coh_armed, which every guest store reads) would bounce with it. */
static SPU_COH_LINE(volatile long) g_lockline_line;
#define g_lockline g_lockline_line.v
/* SPU_LOCKLINE_STATS: the host thread holding the lock (its own line, so the
 * stats do not add traffic to the lock's). */
static SPU_COH_LINE(volatile unsigned long) g_lockline_holder_line;
/* Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with a
 * pause, and yield after a long wait. The bare exchange loop let GH3's four
 * idle job-policy pollers saturate the line and delay every other SPU atomic
 * -- FMOD's mixer task among them, 25-40 ms at a time. */
/* SPU_LOCKLINE_STATS=1: per host thread, every 2^20 acquisitions, how long
 * they took to accumulate and how many found the lock held -- which threads
 * share this lock, and how hard. */
static void spu_lockline_stats(int contended, unsigned long holder, unsigned long long wait)
{
    static __declspec(thread) unsigned long n, c;
    static __declspec(thread) unsigned long long t0, tw;
    /* ...and, of the contended ones, whose turn it was and how long the
     * wait took, so a lock held too long names its holder. */
    enum { HN = 32 };
    static __declspec(thread) unsigned long h_tid[HN], h_n[HN];
    static __declspec(thread) unsigned long long h_w[HN];
    n++; c += contended;
    if (contended) {
        tw += wait;
        int k = 0;
        while (k < HN && h_tid[k] && h_tid[k] != holder) k++;
        if (k == HN) k = HN - 1;             /* the last slot collects the rest */
        if (!h_tid[k]) h_tid[k] = holder;
        h_n[k]++; h_w[k] += wait;
    }
    if (n & ((1u << 20) - 1)) return;
    const unsigned long long now = GetTickCount64();
    if (t0) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        char buf[768];
        int p = snprintf(buf, sizeof buf, "[lockline] host tid %lu: 1M in %llu ms, %lu contended,"
                         " %.1f ms waiting; behind:", GetCurrentThreadId(), now - t0, c,
                         1000.0 * (double)tw / (double)f.QuadPart);
        for (int r = 0; r < 6; r++) {           /* the six longest waits */
            int b = -1;
            for (int k = 0; k < HN; k++) if (h_n[k] && (b < 0 || h_w[k] > h_w[b])) b = k;
            if (b < 0) break;
            p += snprintf(buf + p, sizeof buf - p, " %lu x%lu %.1fms", h_tid[b], h_n[b],
                          1000.0 * (double)h_w[b] / (double)f.QuadPart);
            h_n[b] = 0;
        }
        fprintf(stderr, "%s%c", buf, 10);
    }
    t0 = now; c = 0; tw = 0;
    memset(h_tid, 0, sizeof h_tid); memset(h_n, 0, sizeof h_n); memset(h_w, 0, sizeof h_w);
}
void spu_lockline_lock(void)
{
    static int s_st = -1;
    PS3_KNOB(s_st, getenv("SPU_LOCKLINE_STATS") ? 1 : 0);
    unsigned spins = 0;
    int contended = 0;
    unsigned long holder = 0;
    LARGE_INTEGER w0 = {0};
    while (_InterlockedExchange(&g_lockline, 1)) {
        if (!contended && s_st) { holder = g_lockline_holder_line.v; QueryPerformanceCounter(&w0); }
        contended = 1;
        do {
            _mm_pause();
            if (++spins >= 4096) { spins = 0; SwitchToThread(); }
        } while (g_lockline);
    }
    if (s_st) {
        unsigned long long wait = 0;
        if (contended) { LARGE_INTEGER w1; QueryPerformanceCounter(&w1);
                         wait = (unsigned long long)(w1.QuadPart - w0.QuadPart); }
        g_lockline_holder_line.v = GetCurrentThreadId();
        spu_lockline_stats(contended, holder, wait);
    }
}
void spu_lockline_unlock(void) { _InterlockedExchange(&g_lockline, 0); }
#else
#include <stdatomic.h>
static atomic_flag g_lockline = ATOMIC_FLAG_INIT;
void spu_lockline_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_lockline, memory_order_acquire)) { } }
void spu_lockline_unlock(void) { atomic_flag_clear_explicit(&g_lockline, memory_order_release); }
#endif

/* ===========================================================================
 * The reserved-line bitmap
 *
 * One bit per 128-byte line over the whole 32-bit guest address space: 4 MiB of
 * zero-initialized BSS, of which only the pages around addresses an SPU has
 * actually reserved are ever touched. The reference implementation scopes its
 * bitmap to the one window a single title's SPURS structures live in; this
 * runtime hands out guest memory from several disjoint regions (the ELF image,
 * the sys_memory window at 0x40000000, the raw-SPU local-store windows), and a
 * line outside a hardcoded window would be silently non-coherent, which is the
 * failure this whole file exists to remove.
 * ===========================================================================*/
#define SPU_COH_LINE_SHIFT  7                                  /* 128-byte lines */
#define SPU_COH_LINES       (1u << (32 - SPU_COH_LINE_SHIFT))   /* 2^25 lines */
#define SPU_COH_BITMAP_SZ   (SPU_COH_LINES / 8)                 /* 4 MiB */

static unsigned char s_coh_bitmap[SPU_COH_BITMAP_SZ];

/* Zero until the first GETLLAR anywhere. Keeps the PPU fast path off the
 * bitmap entirely for a title that runs no SPU code, and keeps every existing
 * test that stores through vm_write* paying one compare against a hot global.
 * Written once, under the lock; read unlocked, where a stale zero costs at most
 * one missed event on the very first reservation of the run. */
static SPU_COH_LINE(int) s_coh_armed_line;   /* own line, as g_lockline */
#define s_coh_armed s_coh_armed_line.v

/* The SPU contexts that have reserved a line. Sized to match the MFC engine
 * registry in spu_channels.c, which is the ceiling on live SPU contexts. */
#define SPU_COH_MAX_CTX 8
static spu_context* s_coh_ctxs[SPU_COH_MAX_CTX];

unsigned long g_spu_lr_raise = 0;

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    /* Store only on a change. Every guest store on every PPU thread reads
     * s_coh_armed and its line's bitmap byte (spu_coh_is_reserved), and an SPU
     * polling one line re-reserves it on each GETLLAR -- Demon's Souls'
     * HighGraphics ring consumer ~1.8 million times a second. An unconditional
     * store dirties those cache lines every time and takes them away from
     * every core that reads them. */
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    const unsigned char bit = (unsigned char)(1u << (line & 7));
    if (!(s_coh_bitmap[line >> 3] & bit)) s_coh_bitmap[line >> 3] |= bit;
    if (!s_coh_armed) s_coh_armed = 1;

    if (!ctx) return;
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        if (s_coh_ctxs[i] == ctx) return;
        if (s_coh_ctxs[i] == NULL) { s_coh_ctxs[i] = ctx; return; }
    }
    /* More live SPU contexts than the registry holds. The ones already in it
     * still get their events; this one would silently never wake, so say so. */
    { static int warned = 0;
      if (!warned) { warned = 1;
          fprintf(stderr, "[spu-coh] more than %d reserving SPU contexts -- "
                          "spu=0x%X will not receive lock-line events\n",
                  SPU_COH_MAX_CTX, ctx->spu_id);
          fflush(stderr); } }
}

/* Drop a context from the reserving set.
 *
 * There was no way to do this, and the contexts that most need it are
 * TRANSIENT: spu_run_lifted_job_abi runs each job on a STACK-LOCAL
 * spu_context, which registered itself here on its first reservation and then
 * returned. The entry outlived the object, so spu_coh_notify_write walked a
 * dangling pointer -- reading resv_valid, writing event_status and calling
 * spu_ch_wake on reclaimed stack. Undefined behaviour, and observable: Guitar
 * Hero III showed THREE live-looking contexts for one dispatch, stealing each
 * other`s reservations, with every PUTLLC failing "no reservation". */
void spu_coh_unregister(spu_context* ctx)
{
    if (!ctx) return;
    /* Under the lock-line lock, which every walker of s_coh_ctxs holds: a
     * notifier that loaded this pointer just before the NULL store would
     * otherwise go on dereferencing it after the job returned and its stack
     * (or its whole async thread) was gone. GH3 at -O2 crashed that way in
     * spu_coh_notify_write, reading a freed thread stack. */
    spu_lockline_lock();
    for (int i = 0; i < SPU_COH_MAX_CTX; i++)
        if (s_coh_ctxs[i] == ctx) { s_coh_ctxs[i] = NULL; break; }
    spu_lockline_unlock();
}

/* Drop every registered context that lies in [lo, hi) -- a host thread's
 * stack, called as that thread exits. A stack-local context still registered
 * then has leaked past its unregister; the next walker would read an unmapped
 * stack (GH3 at the end of a Havok task: spu_coh_notify_write faulting).
 * ponytail: safety net that also names the leak; find the path that skips
 * spu_coh_unregister if it keeps firing. */
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi)
{
    spu_lockline_lock();
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        uintptr_t c = (uintptr_t)s_coh_ctxs[i];
        if (c >= lo && c < hi) {
            static int _n = 0;
            if (_n++ < 16)
                fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n",
                        (void*)c, s_coh_ctxs[i]->image_id);
            s_coh_ctxs[i] = NULL;
        }
    }
    spu_lockline_unlock();
}

int spu_coh_is_reserved(uint32_t addr)
{
    if (!s_coh_armed) return 0;
    uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    return (s_coh_bitmap[line >> 3] >> (line & 7)) & 1u;
}

/* A DMA PUT is issued BY an SPU, and hardware does not take that SPU's own
 * reservation away for its own MFC write -- the MFC is part of the same SPE.
 * Our notify had no way to say "everyone but me", so an SPU that reserved a
 * line and then DMA'd a buffer overlapping it killed its own reservation and
 * its next PUTLLC could never succeed. Guitar Hero III's job policy module
 * livelocks exactly there: 16 million atomic ops on one line, every PUTLLC
 * failing for "no reservation", 600k of them self-inflicted.
 *
 * The PUTLLC path already guards this case by dropping its own reservation
 * before notifying; this gives the DMA path the same ability. */
void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    uint32_t line = ea & ~127u;
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        spu_context* c = s_coh_ctxs[i];
        if (!c || (const void*)c == self) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            __atomic_fetch_or(&c->event_status, SPU_EVENT_LR, __ATOMIC_SEQ_CST);   /* the SPU acks without the lock */
            c->resv_valid = 0;
            g_spu_lr_raise++;
            spu_ch_wake(c);
        }
    }
}

/* A read-modify-write the runtime makes on guest memory an SPU updates
 * atomically -- the SPURS taskset bitsets, where tasks 0-7 share one byte of
 * each. Done as vm_read then vm_write, an SPU PUTLLC can commit between the
 * two and be overwritten: Demon's Souls' Havok task enables the next task
 * (task 4) on that line while our runtime clears the bits of a task that just
 * exited, and the cleared-back enabled bit means task 4 never starts, its
 * semaphore post never comes, and the physics step waits forever.
 *
 * Under the lock-line lock the RMW is one transaction against every PUTLLC,
 * and the notify fails any reservation taken before it, as a store from
 * another processor does on hardware. */
uint32_t spu_coh_rmw32(uint32_t ea, uint32_t set, uint32_t clr)
{
    extern uint8_t* vm_base;
    uint32_t* p = (uint32_t*)(vm_base + (ea & ~3u));
    spu_lockline_lock();
    uint32_t raw = __atomic_load_n(p, __ATOMIC_SEQ_CST), nraw;
    do {
        nraw = __builtin_bswap32((__builtin_bswap32(raw) | set) & ~clr);
    } while (nraw != raw &&
             !__atomic_compare_exchange_n(p, &raw, nraw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    if (nraw != raw) spu_coh_notify_write(ea);
    spu_lockline_unlock();
    return __builtin_bswap32(raw);
}

void spu_coh_notify_write(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        spu_context* c = s_coh_ctxs[i];
        if (!c) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            __atomic_fetch_or(&c->event_status, SPU_EVENT_LR, __ATOMIC_SEQ_CST);   /* the SPU acks without the lock */
            c->resv_valid = 0;          /* reservation lost, PUTLLC must fail */
            /* SPU_PUTLLC_WHY=1: name the agent that killed it. A PUTLLC that
             * always fails for "no reservation" is useless without knowing who
             * took it away -- a peer SPU, or the PPU committing to the line. */
            { static int s_w = -1;
              PS3_KNOB(s_w, getenv("SPU_PUTLLC_WHY") ? 1 : 0);
              if (s_w) { static unsigned long long n;
                  if ((++n % 200000) == 1)
                      fprintf(stderr, "[resv-lost] %llu: line 0x%08X taken from img=%d ctx=%p\n",
                              n, line, c->image_id, (void*)c); } }
            g_spu_lr_raise++;
            /* The event_status store has to be visible before the wake. The
             * waiter re-polls its predicate on a timeout as well, so a
             * straggling store costs latency and not the wakeup itself. */
            spu_ch_wake(c);
        }
    }
}

#ifdef __cplusplus
}
#endif
