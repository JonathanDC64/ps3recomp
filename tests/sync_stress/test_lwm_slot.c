/*
 * ps3recomp - tests/sync_stress/test_lwm_slot.c
 *
 * The host side of a sys_lwmutex (runtime/ppu/ppu_lwmutex_slot.h), run as it
 * is, on host threads:
 *
 *   test_lwm_slot exclusion   6 threads x 40,000 lock pairs, some timed and
 *                             retried: never two inside, the slot left free
 *   test_lwm_slot starve      one thread relocks right after each 2 ms hold;
 *                             another wants the lock 20 times and is handed
 *                             it within a hold or two, with the default bound
 *                             (1 ms) and with every unlock handing it over
 *   test_lwm_slot order       three sleepers are served in the order they
 *                             queued
 *   test_lwm_slot timeout     timed waits that expire leave the queue empty;
 *                             one handed the lock at its deadline owns it
 *   test_lwm_slot retry       exclusion with no hand-off (SYS_SYNC_RETRY)
 *   test_lwm_slot always      exclusion with every unlock handing over
 *                             (PPU_LWM_FAIR_US=0, lv2's rule)
 *
 * Exit 0 on success. starve prints the wait with no hand-off too, without
 * judging it (it is the race the hand-off removes, and a scheduler's to win).
 */
#include "../../runtime/platform/win32_compat.h"
#include "../../runtime/platform/msvc_compat.h"   /* __stdcall */
#ifdef _WIN32
#include <process.h>
#endif
#include "../../runtime/ppu/ppu_lwmutex_slot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct LwmSlot S;
static int g_fifo = 1;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double now_ms(void)
{
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
static void spin_ms(double ms) { const double t = now_ms(); while (now_ms() - t < ms) YieldProcessor(); }
static void lock_inf(void) { if (!lwm_try(&S)) lwm_wait(&S, INFINITE, g_fifo); }
static void unlock(void) { lwm_release(&S, g_fifo); }
static void reset(void) { S.cnt = 1; S.waiters = 0; S.qhead = S.qtail = NULL; }   /* lk and cv stay as made */
static int slot_idle(void) { return S.cnt == 1 && S.waiters == 0 && !S.qhead && !S.qtail; }

typedef unsigned (__stdcall* thread_fn)(void*);
static HANDLE start(thread_fn fn, void* arg) { return (HANDLE)_beginthreadex(NULL, 0, fn, arg, 0, NULL); }
static void join(HANDLE h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }

/* ---- exclusion ---------------------------------------------------------- */
#define EX_THREADS 6
static int ex_iters = 40000;   /* a tenth for 'always': each hand-off waits on a wake (~120 us on Linux) */
static volatile long ex_inside, ex_overlap;
static long ex_counter;
static unsigned __stdcall ex_worker(void* arg)
{
    const int t = (int)(intptr_t)arg;
    for (int i = 0; i < ex_iters; i++) {
        if ((t & 1) && (i % 3) == 0) {
            while (!(lwm_try(&S) || lwm_wait(&S, 1, g_fifo))) {}   /* timed and retried */
        } else {
            lock_inf();
        }
        if (InterlockedIncrement(&ex_inside) != 1) InterlockedIncrement(&ex_overlap);
        ex_counter++;
        if ((i % 97) == 0) SwitchToThread();
        InterlockedDecrement(&ex_inside);
        unlock();
    }
    return 0;
}
static void t_exclusion(void)
{
    reset(); ex_inside = ex_overlap = 0; ex_counter = 0;
    const double t0 = now_ms();
    HANDLE h[EX_THREADS];
    for (int t = 0; t < EX_THREADS; t++) h[t] = start(ex_worker, (void*)(intptr_t)t);
    for (int t = 0; t < EX_THREADS; t++) join(h[t]);
    printf("exclusion (fifo %d, hand-off after %lld ticks): %ld lock pairs in %.0f ms, %ld overlaps\n", g_fifo,
           lwm_fair_ticks(), ex_counter, now_ms() - t0, ex_overlap);
    CHECK(ex_counter == (long)EX_THREADS * ex_iters, "counter %ld", ex_counter);
    CHECK(ex_overlap == 0, "%ld overlaps", ex_overlap);
    CHECK(slot_idle(), "slot not idle: cnt %ld waiters %ld", (long)S.cnt, (long)S.waiters);
}

/* ---- starve ------------------------------------------------------------- */
static volatile long st_stop;
static unsigned __stdcall st_holder(void* arg)
{
    (void)arg;
    while (!st_stop) { lock_inf(); spin_ms(2.0); unlock(); }
    return 0;
}
static double starve_worst(int fifo, int timed, int* got_out)
{
    reset(); g_fifo = fifo; st_stop = 0;
    HANDLE a = start(st_holder, NULL);
    Sleep(20);
    double worst = 0; int got = 0;
    const double end = now_ms() + 3000;
    for (int i = 0; i < 20 && now_ms() < end; i++) {
        const double t0 = now_ms();
        int ok;
        if (timed) { do ok = lwm_try(&S) || lwm_wait(&S, 200, fifo); while (!ok && now_ms() < end); }
        else { lock_inf(); ok = 1; }
        if (!ok) break;
        const double w = now_ms() - t0;
        if (w > worst) worst = w;
        got++;
        unlock();
        Sleep(1);
    }
    st_stop = 1; join(a);
    *got_out = got;
    return worst;
}
static void t_starve(void)
{
    for (int always = 0; always < 2; always++) {
        g_lwm_fair_ticks = always ? 0 : -1;   /* -1: the default, read again */
        const char* how = always ? "every unlock" : "after 1 ms";
        int got;
        const double w_inf = starve_worst(1, 0, &got);
        printf("starve: hand-off %s, untimed: 20 wanted, %d got, worst wait %.1f ms\n", how, got, w_inf);
        CHECK(got == 20 && w_inf < 250, "untimed waiter (%s): %d of 20, worst %.1f ms", how, got, w_inf);
        const double w_t = starve_worst(1, 1, &got);
        printf("starve: hand-off %s, 200 ms timeouts: 20 wanted, %d got, worst wait %.1f ms\n", how, got, w_t);
        CHECK(got == 20 && w_t < 250, "timed waiter (%s): %d of 20, worst %.1f ms", how, got, w_t);
    }
    g_lwm_fair_ticks = -1;
    int got;
    const double w_old = starve_worst(0, 1, &got);   /* timed, so it gives up at the 3 s budget */
    printf("starve: no hand-off (not judged): 20 wanted, %d got in 3 s, worst wait %.1f ms\n", got, w_old);
    g_fifo = 1;
}

/* ---- order -------------------------------------------------------------- */
static int ord[3], ord_n;
static SRWLOCK ord_lk = SRWLOCK_INIT;
static unsigned __stdcall ord_worker(void* arg)
{
    lock_inf();
    AcquireSRWLockExclusive(&ord_lk); ord[ord_n++] = (int)(intptr_t)arg; ReleaseSRWLockExclusive(&ord_lk);
    spin_ms(1.0);
    unlock();
    return 0;
}
static void t_order(void)
{
    reset(); ord_n = 0;
    lock_inf();
    HANDLE h[3];
    for (int k = 0; k < 3; k++) {
        h[k] = start(ord_worker, (void*)(intptr_t)k);
        while (S.waiters != k + 1) SwitchToThread();   /* k counted and queued, under lk */
        Sleep(5);
    }
    unlock();
    for (int k = 0; k < 3; k++) join(h[k]);
    printf("order: %d %d %d\n", ord[0], ord[1], ord[2]);
    CHECK(ord_n == 3 && ord[0] == 0 && ord[1] == 1 && ord[2] == 2, "served %d %d %d", ord[0], ord[1], ord[2]);
    CHECK(slot_idle(), "slot not idle");
}

/* ---- timeout ------------------------------------------------------------ */
static volatile long to_ret[2];
static DWORD to_ms[2] = { 20, 40 };
static unsigned __stdcall to_worker(void* arg)
{
    const int k = (int)(intptr_t)arg;
    to_ret[k] = lwm_wait(&S, to_ms[k], g_fifo);
    return 0;
}
static void t_timeout(void)
{
    reset();
    lock_inf();
    HANDLE b = start(to_worker, (void*)0), c = start(to_worker, (void*)1);
    join(b); join(c);
    CHECK(to_ret[0] == 0 && to_ret[1] == 0, "expired waits returned %ld %ld", (long)to_ret[0], (long)to_ret[1]);
    CHECK(!S.qhead && !S.qtail && S.waiters == 0, "queue left after timeouts");
    CHECK(S.cnt == 0, "the holder lost the lock");
    unlock();
    CHECK(slot_idle(), "not free after the unlock");
    /* An unlock near a waiter's deadline: either it was handed the lock and owns it, or it timed out and the
     * lock is free. Never both, never neither, and the queue is always left empty. */
    int handed = 0;
    for (int i = 0; i < 100; i++) {
        reset(); lock_inf();
        to_ms[0] = 2; to_ret[0] = -1;
        HANDLE d = start(to_worker, (void*)0);
        while (S.waiters == 0) SwitchToThread();
        spin_ms(1.5 + (i % 10) * 0.1);
        unlock();
        join(d);
        if (to_ret[0] == 1) { handed++; CHECK(S.cnt == 0, "handed the lock but it reads free (%d)", i); unlock(); }
        else CHECK(to_ret[0] == 0 && S.cnt == 1, "timed out but the lock is not free (%d)", i);
        CHECK(!S.qhead && !S.qtail && S.waiters == 0, "queue left (%d)", i);
    }
    printf("timeout: ok; at the deadline, %d of 100 were handed the lock\n", handed);
    to_ms[0] = 20;
}

int main(int argc, char** argv)
{
    const char* m = argc > 1 ? argv[1] : "all";
    const int all = !strcmp(m, "all");
    if (all || !strcmp(m, "exclusion")) { g_fifo = 1; t_exclusion(); }
    if (all || !strcmp(m, "starve")) t_starve();
    if (all || !strcmp(m, "order")) { g_fifo = 1; t_order(); }
    if (all || !strcmp(m, "timeout")) { g_fifo = 1; t_timeout(); }
    if (all || !strcmp(m, "retry")) { g_fifo = 0; t_exclusion(); g_fifo = 1; }
    if (all || !strcmp(m, "always")) { g_fifo = 1; g_lwm_fair_ticks = 0; ex_iters = 4000; t_exclusion(); ex_iters = 40000; g_lwm_fair_ticks = -1; }
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
