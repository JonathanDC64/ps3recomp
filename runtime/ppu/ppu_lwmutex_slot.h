/*
 * ps3recomp - the host side of one sys_lwmutex (ppu_sysprx.cpp)
 *
 * The lock is a count in the slot (1 free, 0 taken), taken with one
 * compare-exchange; only a contended lock sleeps, on the slot's condition
 * variable. ppu_sysprx.cpp keeps one slot per guest lwmutex and the guest's
 * owner and recursion words; this file is only the lock, so that
 * tests/sync_stress/test_lwm_slot.c can run it as it is.
 *
 * An unlock with threads asleep on the lock hands it to the one that has
 * waited longest (fifo), as lv2 does, once that one has waited 1 ms
 * (lwm_fair_ticks): the count stays taken, so nobody can take the lock in
 * between. With fifo 0 (a SYS_SYNC_RETRY mutex, or
 * PPU_LWM_HANDOFF=0) an unlock gives the count back and wakes a waiter to
 * try again. ppu_sysprx.cpp's sys_lwmutex comment has the history.
 *
 * C and C++. On Windows the Win32 API, elsewhere runtime/platform's
 * win32_compat.h.
 */
#ifndef PS3RECOMP_PPU_LWMUTEX_SLOT_H
#define PS3RECOMP_PPU_LWMUTEX_SLOT_H

#include "../platform/win32_compat.h"
#include <stddef.h>
#include <stdlib.h>

struct LwmWaiter { struct LwmWaiter* next; int granted; long long since; };   /* on the waiter's stack, under lk */
struct LwmSlot { volatile long addr;
    volatile long cnt;               /* 1 free, 0 taken */
    volatile long waiters;           /* lockers asleep on cv, or about to be */
    SRWLOCK lk; CONDITION_VARIABLE cv;
    struct LwmWaiter* qhead; struct LwmWaiter* qtail;   /* under lk: the queue, oldest first */
    volatile long holder; volatile long long acq_us; volatile long long acq_fences;   /* PS3_LWMUTEX_TRACE */
    volatile unsigned long long acq_cpu_us; };

static inline int lwm_try(struct LwmSlot* s) { return InterlockedCompareExchange(&s->cnt, 0, 1) == 1; }

/* PPU_LWM_HANDOFF=0: no hand-off for any mutex (the race above). Read once. */
static inline int lwm_handoff_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PPU_LWM_HANDOFF"); v = (e && e[0] == '0') ? 0 : 1; }
    return v;
}

/* How long the oldest sleeper waits before an unlock hands it the lock, in QueryPerformanceCounter ticks
 * (PPU_LWM_FAIR_US, default 1000 us; 0: every unlock with a sleeper hands it over, as lv2 does). Until then an
 * unlock gives the count back and wakes one sleeper, which races any thread that relocks: that keeps a busy lock
 * from waiting on a wake and a reschedule at every unlock (on Linux ~120 us a hand-off, against ~0.1 us for the
 * race, measured by test_lwm_slot), while no sleeper waits much past the bound. A test may set it before use. */
static long long g_lwm_fair_ticks = -1;
static inline long long lwm_qpc(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (long long)c.QuadPart; }
static inline long long lwm_fair_ticks(void)
{
    if (g_lwm_fair_ticks < 0) {
        const char* e = getenv("PPU_LWM_FAIR_US");
        const long long us = e ? atoll(e) : 1000;
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        g_lwm_fair_ticks = us > 0 ? us * (long long)f.QuadPart / 1000000 : 0;
    }
    return g_lwm_fair_ticks;
}

static inline void lwm_unqueue(struct LwmSlot* s, struct LwmWaiter* w)   /* under lk */
{
    struct LwmWaiter* prev = NULL;
    for (struct LwmWaiter* p = s->qhead; p; prev = p, p = p->next)
        if (p == w) {
            if (prev) prev->next = w->next; else s->qhead = w->next;
            if (s->qtail == w) s->qtail = prev;
            return;
        }
}

/* Take the count, waiting up to ms (INFINITE: forever). 1 if taken. A fifo
 * waiter queues; it takes the count when an unlock leaves it free and it wins
 * the race, or, once it is the oldest and has waited lwm_fair_ticks, an unlock
 * hands it the lock (granted). */
static inline int lwm_wait(struct LwmSlot* s, DWORD ms, int fifo)
{
    for (int i = 0; i < 100; i++) {          /* a short hold ends before a sleep would */
        if (s->cnt && lwm_try(s)) return 1;
        YieldProcessor();
    }
    const ULONGLONG t0 = GetTickCount64();
    int got = 0;
    struct LwmWaiter me = { NULL, 0, 0 };
    AcquireSRWLockExclusive(&s->lk);
    InterlockedIncrement(&s->waiters);      /* before the retry: see lwm_release */
    if (fifo) {
        me.since = lwm_qpc();
        if (s->qtail) s->qtail->next = &me; else s->qhead = &me;
        s->qtail = &me;
    }
    for (;;) {
        if (me.granted) { got = 1; break; }  /* handed over: already out of the queue */
        if (lwm_try(s)) { got = 1; break; }
        DWORD w = INFINITE;
        if (ms != INFINITE) {
            const ULONGLONG el = GetTickCount64() - t0;
            if (el >= ms) break;
            w = (DWORD)(ms - el);
        }
        SleepConditionVariableSRW(&s->cv, &s->lk, w, 0);
    }
    if (fifo && !me.granted) lwm_unqueue(s, &me);
    InterlockedDecrement(&s->waiters);
    ReleaseSRWLockExclusive(&s->lk);
    return got;
}

/* Give the count back, from any thread. Over-release of a free mutex leaves
 * it free, as the kernel semaphore's max count of 1 did. A locker counts
 * itself a waiter under lk before its last try, and the exchange is a full
 * barrier: either it sees the count, or this sees it and wakes it, under lk
 * so the wake cannot fall between its try and its sleep. With a waiter
 * queued (fifo) that has waited lwm_fair_ticks, the lock goes to it instead:
 * under lk, so it cannot leave the queue at that moment, and the count stays
 * taken. */
static inline void lwm_release(struct LwmSlot* s, int fifo)
{
    if (fifo && s->waiters) {
        AcquireSRWLockExclusive(&s->lk);
        struct LwmWaiter* h = s->qhead;
        if (h && lwm_qpc() - h->since >= lwm_fair_ticks()) {
            s->qhead = h->next;
            if (!s->qhead) s->qtail = NULL;
            InterlockedExchange(&s->cnt, 0);
            h->granted = 1;
            ReleaseSRWLockExclusive(&s->lk);
            WakeAllConditionVariable(&s->cv);   /* the one granted is among them; the rest sleep again */
            return;
        }
        InterlockedExchange(&s->cnt, 1);    /* under lk: a sleeper cannot miss it (see above) */
        ReleaseSRWLockExclusive(&s->lk);
        WakeConditionVariable(&s->cv);
        return;
    }
    InterlockedExchange(&s->cnt, 1);
    if (s->waiters) {
        AcquireSRWLockExclusive(&s->lk);
        ReleaseSRWLockExclusive(&s->lk);
        WakeConditionVariable(&s->cv);
    }
}

#endif /* PS3RECOMP_PPU_LWMUTEX_SLOT_H */
