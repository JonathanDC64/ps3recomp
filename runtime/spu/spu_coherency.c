/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is deliberately
 * plain: a bit per 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line spinlocks (one stripe per line). A
 * writer holds only its own line's stripe, so the bitmap and the table are
 * updated atomically; dropping a context takes every stripe, which excludes
 * every walker. spu_coh_is_reserved reads without a lock, which is the
 * entire point: it is on every PPU store.
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
/* Striped by line: one spinlock per (line % 64), each on a cache line of its
 * own. What the lock serializes is per line -- a GETLLAR's snapshot, a
 * PUTLLC's compare and commit, a PPU or DMA store into a reserved line and its
 * notify -- so two lines never need to exclude each other. One global lock
 * made every SPU atomic wait on every other: in Demon's Souls the
 * HighGraphics ring (its producer's per-command store and the img-2
 * consumer's GETLLAR/PUTLLC, ~4 million a second) spent about half its lock
 * waits behind SPURS kernels polling their own lines. Striped, both ring
 * threads spend ~2% of their time on the lock instead of ~20%. The Palace
 * frame rate did not move (those waits were not on the frame's critical
 * path), but the spinning was most of a core, and a burst of SPU atomics
 * elsewhere (Havok tasks) no longer queues behind the ring.
 *
 * A transfer that spans lines takes each line's stripe in ascending stripe
 * order, and spu_lockline_lock() takes them all in the same order, so no two
 * holders can deadlock. SPU_LL_STRIPES=1 (read once at the first lock) puts
 * every line on one stripe: the old global lock, for an A/B. */
#define SPU_LL_NSTRIPE 64
static SPU_COH_LINE(volatile long) g_ll[SPU_LL_NSTRIPE];
/* SPU_LOCKLINE_STATS: the host thread holding the lock (its own line, so the
 * stats do not add traffic to the lock's). One for all stripes: a hint. */
static SPU_COH_LINE(volatile unsigned long) g_lockline_holder_line;
static unsigned spu_ll_mask(void)
{
    /* Same value from every thread, so a race on the first read is benign. */
    static volatile int s_mask = -1;
    if (s_mask < 0) s_mask = getenv("SPU_LL_STRIPES") ? 0 : SPU_LL_NSTRIPE - 1;
    return (unsigned)s_mask;
}
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
/* Each stripe is a sequence lock: odd while held, and every release moves it
 * on by two. A writer takes it as before (test-and-test-and-set below); a
 * GETLLAR on a line already reserved can instead read the line without
 * taking it (spu_lockline_read_begin/_ok), so the polling SPU stops writing
 * the lock's cache line on every read -- the HighGraphics ring consumer's
 * GETLLAR and its producer's store fought over that line once per entry.
 *
 * Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with a
 * pause, and yield after a long wait. The bare exchange loop let GH3's four
 * idle job-policy pollers saturate the line and delay every other SPU atomic
 * -- FMOD's mixer task among them, 25-40 ms at a time. */
static void spu_ll_acquire(unsigned i)
{
    static int s_st = -1;
    PS3_KNOB(s_st, getenv("SPU_LOCKLINE_STATS") ? 1 : 0);
    volatile long* l = &g_ll[i].v;
    unsigned spins = 0;
    int contended = 0;
    unsigned long holder = 0;
    LARGE_INTEGER w0 = {0};
    /* lock bts: one read-for-ownership of the line, as the exchange was. A
     * plain read then a compare-exchange costs two transfers of a line the
     * other side has just written (shared, then the upgrade). */
    while (_interlockedbittestandset(l, 0)) {
        if (!contended && s_st) { holder = g_lockline_holder_line.v; QueryPerformanceCounter(&w0); }
        contended = 1;
        do {
            _mm_pause();
            if (++spins >= 4096) { spins = 0; SwitchToThread(); }
        } while (*l & 1);
    }
    if (s_st) {
        unsigned long long wait = 0;
        if (contended) { LARGE_INTEGER w1; QueryPerformanceCounter(&w1);
                         wait = (unsigned long long)(w1.QuadPart - w0.QuadPart); }
        g_lockline_holder_line.v = GetCurrentThreadId();
        spu_lockline_stats(contended, holder, wait);
    }
}
/* The stripes [ea, ea+len) touches, as a bit set. */
static unsigned long long spu_ll_set(uint32_t ea, uint32_t len)
{
    const unsigned m = spu_ll_mask();
    const unsigned long long l0 = ea >> 7;
    const unsigned long long l1 = ((unsigned long long)ea + (len ? len : 1) - 1) >> 7;
    if (l1 - l0 >= m) return m == SPU_LL_NSTRIPE - 1 ? ~0ull : 1ull;
    unsigned long long s = 0;
    for (unsigned long long l = l0; l <= l1; l++) s |= 1ull << (l & m);
    return s;
}
void spu_lockline_lock_ea(uint32_t ea, uint32_t len)
{
    unsigned long long s = spu_ll_set(ea, len);
    while (s) { unsigned long i; _BitScanForward64(&i, s); s &= s - 1; spu_ll_acquire((unsigned)i); }
}
void spu_lockline_unlock_ea(uint32_t ea, uint32_t len)
{
    unsigned long long s = spu_ll_set(ea, len);
    while (s) { unsigned long i; _BitScanForward64(&i, s); s &= s - 1; _InterlockedIncrement(&g_ll[i].v); }
}
int spu_lockline_read_begin(uint32_t ea, long* seq)
{
    const long v = __atomic_load_n(&g_ll[(ea >> 7) & spu_ll_mask()].v, __ATOMIC_ACQUIRE);
    *seq = v;
    return !(v & 1);
}
int spu_lockline_read_ok(uint32_t ea, long seq)
{
    /* StoreLoad: the caller's reservation stores must be visible before this
     * read, so a writer that takes the stripe after it finds and drops them. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return __atomic_load_n(&g_ll[(ea >> 7) & spu_ll_mask()].v, __ATOMIC_ACQUIRE) == seq;
}
int spu_lockline_read_same(uint32_t ea, long seq)
{
    /* The copy's loads before the second read of the stripe word. */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&g_ll[(ea >> 7) & spu_ll_mask()].v, __ATOMIC_RELAXED) == seq;
}
void spu_lockline_lock(void)
{
    const unsigned m = spu_ll_mask();
    for (unsigned i = 0; i <= m; i++) spu_ll_acquire(i);
}
void spu_lockline_unlock(void)
{
    const unsigned m = spu_ll_mask();
    for (unsigned i = 0; i <= m; i++) _InterlockedIncrement(&g_ll[i].v);
}
#else
#include <stdatomic.h>
static atomic_flag g_lockline = ATOMIC_FLAG_INIT;
void spu_lockline_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_lockline, memory_order_acquire)) { } }
void spu_lockline_unlock(void) { atomic_flag_clear_explicit(&g_lockline, memory_order_release); }
void spu_lockline_lock_ea(uint32_t ea, uint32_t len)   { (void)ea; (void)len; spu_lockline_lock(); }
void spu_lockline_unlock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; spu_lockline_unlock(); }
int spu_lockline_read_begin(uint32_t ea, long* seq) { (void)ea; *seq = 0; return 0; }   /* always lock */
int spu_lockline_read_ok(uint32_t ea, long seq)     { (void)ea; (void)seq; return 0; }
int spu_lockline_read_same(uint32_t ea, long seq)   { (void)ea; (void)seq; return 0; }
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

/* Exported (ppu_loader.cpp tests it inline on every guest store). */
#ifndef PS3_GAME_SPLIT
unsigned char g_spu_coh_bitmap[SPU_COH_BITMAP_SZ];
#define s_coh_bitmap g_spu_coh_bitmap
#else
/* Split build: the lifted code's inline stores test the game module's copy,
 * so the host uses it too once the module loads (ps3emu/game_split.h). */
static unsigned char s_coh_bitmap_host[SPU_COH_BITMAP_SZ];
unsigned char* ps3_split_coh_bitmap = s_coh_bitmap_host;
#define s_coh_bitmap ps3_split_coh_bitmap
#endif

/* Zero until the first GETLLAR anywhere. Keeps the PPU fast path off the
 * bitmap entirely for a title that runs no SPU code, and keeps every existing
 * test that stores through vm_write* paying one compare against a hot global.
 * Written once, under the lock; read unlocked, where a stale zero costs at most
 * one missed event on the very first reservation of the run. */
#ifndef PS3_GAME_SPLIT
SPU_COH_LINE(int) g_spu_coh_armed_line;   /* own line, as g_lockline; exported as above */
#define s_coh_armed g_spu_coh_armed_line.v
#else
static SPU_COH_LINE(int) s_coh_armed_host;   /* the game module's once it loads */
int* ps3_split_coh_armed = &s_coh_armed_host.v;
#define s_coh_armed (*ps3_split_coh_armed)
#endif

/* The SPU contexts that have reserved a line. Sized to match the MFC engine
 * registry in spu_channels.c, which is the ceiling on live SPU contexts. */
#define SPU_COH_MAX_CTX 8
static spu_context* s_coh_ctxs[SPU_COH_MAX_CTX];

/* The line (ea & ~127) each slot's context reserved last, so a notify passes
 * over the contexts whose reservation is on another line without reading
 * them. spu_coh_notify_write looked at all eight on every store into a
 * reserved line -- Demon's Souls' HighGraphics makes ~30,000 a frame, and the
 * scan was 3.3% of that thread -- although nearly every store is to a line
 * one context or none holds.
 *
 * Written only by the slot's own context, in spu_coh_reserve, before the
 * GETLLAR sets resv_valid: under the line's stripe on the locked path, and
 * before the StoreLoad fence of spu_lockline_read_ok on the lock-free one. A
 * notifier holds the line's stripe, so it sees the hint of every reservation
 * it has to break, by the argument that makes it see resv_valid (see
 * spu_coh_notify_write). A hint is never cleared: a stale one names a line
 * whose context then fails the full check, as before. On a cache line of its
 * own, as it changes only when a context moves to another line. */
typedef uint32_t spu_coh_hint_t[SPU_COH_MAX_CTX];
static SPU_COH_LINE(spu_coh_hint_t) s_coh_hint;

static void spu_coh_hint_set(int i, uint32_t line_ea)
{
    if (__atomic_load_n(&s_coh_hint.v[i], __ATOMIC_RELAXED) != line_ea)
        __atomic_store_n(&s_coh_hint.v[i], line_ea, __ATOMIC_RELAXED);
}

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
    /* Atomic: the caller holds only its own line's stripe, and eight lines
     * share a bitmap byte. A lost bit would leave a reserved line that PPU
     * stores do not see. */
    if (!(s_coh_bitmap[line >> 3] & bit))
        __atomic_fetch_or(&s_coh_bitmap[line >> 3], bit, __ATOMIC_SEQ_CST);
    if (!s_coh_armed) s_coh_armed = 1;

    if (!ctx) return;
    /* Look for ctx in the whole table before claiming a slot. Only ctx's own
     * thread adds or removes it, so this pass is exact; but other slots empty
     * under us (the GETLLAR fast path reserves without the stripe lock, so it
     * runs during other threads' unregisters), and stopping at the first empty
     * slot registered a context a second time below its own entry. Unregister
     * cleared one; the other dangled once the context's stack was gone, and
     * spu_coh_notify_write faulted on it. The slot found or claimed takes
     * the line's hint before the caller sets resv_valid. */
    const uint32_t line_ea = ea & ~127u;
    for (int i = 0; i < SPU_COH_MAX_CTX; i++)
        if (__atomic_load_n(&s_coh_ctxs[i], __ATOMIC_ACQUIRE) == ctx) {
            spu_coh_hint_set(i, line_ea);
            return;
        }
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        spu_context* c = __atomic_load_n(&s_coh_ctxs[i], __ATOMIC_ACQUIRE);
        if (c == NULL) {
            /* Claim the slot: another stripe's holder may be claiming it too. */
            spu_context* expect = NULL;
            if (__atomic_compare_exchange_n(&s_coh_ctxs[i], &expect, ctx, 0,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ||
                expect == ctx) {
                spu_coh_hint_set(i, line_ea);
                return;
            }
        }
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
     * spu_coh_notify_write, reading a freed thread stack.
     *
     * A context only ever enters the table from its own thread's GETLLAR,
     * and this runs on that thread, so a context not in it now cannot be
     * added under us: most jobs never reserve a line and skip the 64 locks. */
    int in = 0;
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) if (s_coh_ctxs[i] == ctx) in = 1;
    if (!in) return;
    spu_lockline_lock();
    for (int i = 0; i < SPU_COH_MAX_CTX; i++)
        if (s_coh_ctxs[i] == ctx) s_coh_ctxs[i] = NULL;
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
            if (_n < 16 && ++_n)
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
    spu_lockline_lock_ea(ea & ~3u, 4);
    uint32_t raw = __atomic_load_n(p, __ATOMIC_SEQ_CST), nraw;
    do {
        nraw = __builtin_bswap32((__builtin_bswap32(raw) | set) & ~clr);
    } while (nraw != raw &&
             !__atomic_compare_exchange_n(p, &raw, nraw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    if (nraw != raw) spu_coh_notify_write(ea);
    spu_lockline_unlock_ea(ea & ~3u, 4);
    return __builtin_bswap32(raw);
}

void spu_coh_notify_write(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    /* Only the slots whose hint is this line (s_coh_hint). Every caller holds
     * the line's stripe, taken with a locked RMW before this runs. A GETLLAR
     * that took its reservation under the stripe released it after writing
     * the hint; one that took it without the lock wrote the hint, fenced, and
     * found the stripe word unchanged, so its fence came before our RMW and
     * the hint is visible here. Either way a reservation this store has to
     * break shows its line in the hint, unless its context has since begun a
     * GETLLAR of another line: that GETLLAR replaces the reservation, and the
     * store reads as one that landed after it. A reservation whose GETLLAR
     * this store overlaps redoes its read under the stripe and sees the
     * store. SPU_COH_HINT=0 (live) checks every context, as before. */
    static int s_hint = -1;
    PS3_KNOB(s_hint, ps3_env_int("SPU_COH_HINT", 1));
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        if (s_hint && __atomic_load_n(&s_coh_hint.v[i], __ATOMIC_RELAXED) != line) continue;
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
