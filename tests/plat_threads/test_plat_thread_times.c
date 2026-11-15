/* tests/plat_threads -- runtime/platform/plat.h's view of the process's
 * threads as the kernel counts them (the problem report's CPU table on Linux,
 * ps3_diag_report).
 *
 * On Linux: plat_threads_list has the calling thread and one it started;
 * plat_thread_times counts about 60 ms on a CPU for a thread that spun that
 * long and almost none for one that slept, never goes backwards, and gives 0
 * for a thread that has ended; plat_thread_name reads back the name a thread
 * set. Built twice: as the runtime has it (schedstat, so both times: 2, or 1
 * on a kernel without schedstat) and with PLAT_THREAD_TIMES_NO_SCHEDSTAT (the
 * stat file alone: 1, a wait of 0). Off Linux all three give 0.
 *
 * Exit code 0 if every check passed; each failure prints a line.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* pthread_setname_np */
#endif
#include "platform/plat.h"

#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

static int g_fail;
#define CHECK(c) do { if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#if defined(__linux__)
#ifdef PLAT_THREAD_TIMES_NO_SCHEDSTAT
#define KIND_OK(k) ((k) == 1)
#else
#define KIND_OK(k) ((k) == 2 || (k) == 1)
#endif

static long self_tid(void) { return (long)syscall(SYS_gettid); }

/* This thread's own CPU clock, to spin a known amount of CPU time. */
static double thread_cpu_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static volatile int g_stop;
static volatile long g_sleeper_tid;

static void* sleeper(void* arg)
{
    (void)arg;
    g_sleeper_tid = self_tid();
    while (!g_stop) usleep(2000);
    return NULL;
}

static void* quick(void* arg)
{
    *(volatile long*)arg = self_tid();
    return NULL;
}

static int listed(const long* tids, int n, long tid)
{
    for (int i = 0; i < n; i++) if (tids[i] == tid) return 1;
    return 0;
}

/* Two threads held to one CPU, both spinning: each is ready to run while the
 * other has the CPU, so the kernel counts waits for them. */
static volatile int g_spin_stop;
static volatile long g_spinner_tid;
static cpu_set_t g_one;

static void* spinner(void* arg)
{
    (void)arg;
    sched_setaffinity(0, sizeof g_one, &g_one);
    g_spinner_tid = self_tid();
    volatile unsigned long x = 0;
    while (!g_spin_stop) x++;
    return NULL;
}

static double wall_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}
#endif

int main(void)
{
#if defined(__linux__)
    pthread_t th;
    CHECK(pthread_create(&th, NULL, sleeper, NULL) == 0);
    while (!g_sleeper_tid) usleep(100);
    const long me = self_tid(), other = g_sleeper_tid;

    /* The list: this thread and the one it started. */
    long tids[512];
    const int n = plat_threads_list(tids, 512);
    CHECK(n >= 2);
    CHECK(listed(tids, n, me));
    CHECK(listed(tids, n, other));
    CHECK(plat_threads_list(tids, 1) == 1);

    /* The times: about 60 ms spun here, next to nothing for the sleeper. */
    uint64_t r0 = 0, w0 = 0, r1 = 0, w1 = 0, o0 = 0, ow0 = 0, o1 = 0, ow1 = 0;
    const int k = plat_thread_times(me, &r0, &w0);
    CHECK(KIND_OK(k));
    CHECK(plat_thread_times(other, &o0, &ow0) == k);
    const double start = thread_cpu_ms();
    volatile unsigned long x = 0;
    while (thread_cpu_ms() - start < 60.0) x++;
    CHECK(plat_thread_times(me, &r1, &w1) == k);
    CHECK(plat_thread_times(other, &o1, &ow1) == k);
    const double spun = (double)(r1 - r0) / 1e6, slept = (double)(o1 - o0) / 1e6;
    printf("kind %d: spun 60 ms of CPU -> %.1f ms counted; the sleeper %.1f ms; waits %llu ns / %llu ns\n",
           k, spun, slept, (unsigned long long)(w1 - w0), (unsigned long long)(ow1 - ow0));
    CHECK(r1 >= r0 && o1 >= o0 && w1 >= w0 && ow1 >= ow0);
    CHECK(spun >= 40.0 && spun <= 250.0);   /* the stat file counts to the clock tick (10 ms) */
    CHECK(slept < 30.0);
    if (k == 1) CHECK(w0 == 0 && w1 == 0);

    /* The waits: this thread and a spinner on one CPU for 200 ms. Both share
     * it, so between them they wait about as long as they run (with schedstat;
     * the stat file has no waits). */
    if (k == 2) {
        cpu_set_t was;
        CHECK(sched_getaffinity(0, sizeof was, &was) == 0);
        CPU_ZERO(&g_one);
        CPU_SET(sched_getcpu(), &g_one);
        CHECK(sched_setaffinity(0, sizeof g_one, &g_one) == 0);
        pthread_t ts;
        CHECK(pthread_create(&ts, NULL, spinner, NULL) == 0);
        while (!g_spinner_tid) sched_yield();
        uint64_t a0 = 0, aw0 = 0, b0 = 0, bw0 = 0, a1 = 0, aw1 = 0, b1 = 0, bw1 = 0;
        CHECK(plat_thread_times(me, &a0, &aw0) == 2 && plat_thread_times(g_spinner_tid, &b0, &bw0) == 2);
        const double t0 = wall_ms();
        volatile unsigned long y = 0;
        while (wall_ms() - t0 < 200.0) y++;
        CHECK(plat_thread_times(me, &a1, &aw1) == 2 && plat_thread_times(g_spinner_tid, &b1, &bw1) == 2);
        g_spin_stop = 1;
        pthread_join(ts, NULL);
        sched_setaffinity(0, sizeof was, &was);
        const double ran = (double)((a1 - a0) + (b1 - b0)) / 1e6, waited = (double)((aw1 - aw0) + (bw1 - bw0)) / 1e6;
        printf("two spinners on one CPU for 200 ms: %.1f ms on it, %.1f ms waiting for it\n", ran, waited);
        CHECK(ran <= 260.0);                /* one CPU between them */
        CHECK(ran + waited >= 300.0);       /* each was running or ready all along */
        CHECK(waited >= 60.0);
        (void)y;
    }

    /* A thread that has ended: nothing to read. */
    volatile long gone = 0;
    pthread_t tq;
    CHECK(pthread_create(&tq, NULL, quick, (void*)&gone) == 0);
    CHECK(pthread_join(tq, NULL) == 0);
    uint64_t gr = 1, gw = 1;
    CHECK(gone != 0);
    CHECK(plat_thread_times(gone, &gr, &gw) == 0);
    CHECK(gr == 0 && gw == 0);

    /* The name: what the thread set, cut to the kernel's 15 characters. */
    char nm[32];
    CHECK(pthread_setname_np(pthread_self(), "ptt-self") == 0);
    CHECK(plat_thread_name(me, nm, sizeof nm) == 1);
    CHECK(strcmp(nm, "ptt-self") == 0);
    CHECK(pthread_setname_np(th, "ptt-sleeper-xyz") == 0);   /* 15: the most it takes */
    CHECK(plat_thread_name(other, nm, sizeof nm) == 1);
    CHECK(strcmp(nm, "ptt-sleeper-xyz") == 0);
    CHECK(plat_thread_name(other, nm, 4) == 1 && strcmp(nm, "ptt") == 0);
    CHECK(plat_thread_name(gone, nm, sizeof nm) == 0 && nm[0] == 0);

    g_stop = 1;
    pthread_join(th, NULL);
    (void)x;
#else
    long tids[4];
    uint64_t r = 1, w = 1;
    char nm[8] = "x";
    CHECK(plat_threads_list(tids, 4) == 0);
    CHECK(plat_thread_times(1, &r, &w) == 0 && r == 0 && w == 0);
    CHECK(plat_thread_name(1, nm, sizeof nm) == 0 && nm[0] == 0);
#endif
    if (g_fail) { fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    printf("plat_thread_times: all checks passed\n");
    return 0;
}
