/* tests/plat_threads -- runtime/platform/plat.h's time and sleep: the
 * monotonic clock, the thread's sleep timer and the precise sleep written
 * once on them (what sys_timer_usleep runs on every platform).
 *
 * The clock is checked for going back, within a thread and across threads,
 * and against another clock for its rate. Each sleep is timed on the same
 * clock and the figures printed: requested against measured, p50 and p99.
 * Only what holds on a busy CI machine is checked: the precise sleep never
 * ends before its deadline, and its median is near it.
 *
 * Exit code 0 if every check passed; each failure prints a line.
 */
#include "platform/plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- test helpers (the test's own, not plat.h's) ------------------------- */

static int g_fail;
#define CHECK(c) do { if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#ifdef _WIN32
static int64_t atom_get(volatile int64_t* p) { return InterlockedCompareExchange64(p, 0, 0); }
static long atom_inc(volatile long* p) { return InterlockedIncrement(p); }
/* Another clock, for the rate: the tick count, in milliseconds. */
static double other_ms(void) { return (double)GetTickCount64(); }
#else
static int64_t atom_get(volatile int64_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static long atom_inc(volatile long* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static double other_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}
#endif

static double ticks_us(int64_t d) { return (double)d * 1e6 / (double)plat_clock_freq(); }

static int cmp_double(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* The q-th quantile (0..1) of n sorted values. */
static double quantile(const double* v, int n, double q)
{
    int i = (int)(q * (double)(n - 1) + 0.5);
    return v[i < 0 ? 0 : i >= n ? n - 1 : i];
}

/* ---- the clock ----------------------------------------------------------- */

#define MONO_THREADS 4
#define MONO_READS   200000

typedef struct {
    volatile int64_t latest;     /* the highest reading any thread published */
    volatile long back_self, back_cross, done;
    plat_event fin;
} mono_arg;

/* Each reading must be at least this thread's last one, and at least the
 * latest one any thread had published before this thread read the clock. */
static plat_thread_ret PLAT_THREAD_CALL mono_main(void* p)
{
    mono_arg* a = (mono_arg*)p;
    int64_t last = 0;
    for (int i = 0; i < MONO_READS; i++) {
        const int64_t seen = atom_get(&a->latest);
        const int64_t now = plat_clock_now();
        if (now < last) atom_inc(&a->back_self);
        if (now < seen) atom_inc(&a->back_cross);
        last = now;
        int64_t cur = seen;
        while (cur < now) {
            const int64_t prev = plat_atomic_cas64(&a->latest, now, cur);
            if (prev == cur) break;
            cur = prev;
        }
    }
    if (atom_inc(&a->done) == MONO_THREADS) plat_event_set(&a->fin);
    return 0;
}

static void test_clock(void)
{
    const int64_t f = plat_clock_freq();
    CHECK(f >= 1000000);                     /* at least microsecond ticks */
    CHECK(plat_clock_freq() == f);
#ifndef _WIN32
    CHECK(f == 1000000000);
#endif
    printf("clock: %lld ticks a second\n", (long long)f);

    /* plat_ticks_scale: exact, and no overflow at a month of 1 GHz ticks. */
    CHECK(plat_ticks_scale(0, 1000000000u, 1000000u) == 0);
    CHECK(plat_ticks_scale(999, 1000000000u, 1000000u) == 0);
    CHECK(plat_ticks_scale(1000, 1000000000u, 1000000u) == 1);
    const uint64_t month = 30ull * 86400ull;
    CHECK(plat_ticks_scale(month * 1000000000ull + 123456789u, 1000000000u, 1000000u)
          == month * 1000000ull + 123456u);
    CHECK(plat_ticks_scale(month * 10000000ull + 9999999u, 10000000u, 79800000u)
          == month * 79800000ull + 79799992u);  /* 9999999 * 79.8e6 / 1e7, rounded down */

    /* Never back, in a thread or across threads. */
    static mono_arg m;
    memset(&m, 0, sizeof m);
    CHECK(plat_event_init(&m.fin));
    for (int i = 0; i < MONO_THREADS; i++) CHECK(plat_thread_start_detached(mono_main, &m, 0));
    CHECK(plat_event_wait(&m.fin, 60000));
    CHECK(m.back_self == 0);
    CHECK(m.back_cross == 0);
    printf("clock: %d threads x %d readings: %ld went back in a thread, %ld across threads\n",
           MONO_THREADS, MONO_READS, m.back_self, m.back_cross);
    plat_event_destroy(&m.fin);

    /* The rate, against another clock over about 300 ms. */
    const double o0 = other_ms();
    const int64_t c0 = plat_clock_now();
    plat_sleep_us(300000);
    const double o1 = other_ms();
    const int64_t c1 = plat_clock_now();
    const double mine = ticks_us(c1 - c0) / 1000.0, other = o1 - o0;
    printf("clock: %.1f ms against %.1f ms on another clock\n", mine, other);
    CHECK(mine > other * 0.9 - 20.0 && mine < other * 1.1 + 20.0);
}

/* ---- thread-local storage ------------------------------------------------ */

static PLAT_THREAD_LOCAL int t_mine;

typedef struct { volatile long ok; plat_event fin; } tls_arg;

static plat_thread_ret PLAT_THREAD_CALL tls_main(void* p)
{
    tls_arg* a = (tls_arg*)p;
    if (t_mine == 0) { t_mine = 2; if (t_mine == 2) atom_inc(&a->ok); }
    plat_event_set(&a->fin);
    return 0;
}

static void test_tls(void)
{
    tls_arg a;
    memset(&a, 0, sizeof a);
    CHECK(plat_event_init(&a.fin));
    t_mine = 1;
    CHECK(plat_thread_start_detached(tls_main, &a, 0));
    CHECK(plat_event_wait(&a.fin, 10000));
    CHECK(a.ok == 1);
    CHECK(t_mine == 1);
    plat_event_destroy(&a.fin);

    volatile int64_t v = 5;
    CHECK(plat_atomic_cas64(&v, 7, 4) == 5 && v == 5);
    CHECK(plat_atomic_cas64(&v, 7, 5) == 5 && v == 7);
}

/* ---- sleeps -------------------------------------------------------------- */

#define REPS 200

static plat_timer t_timer;   /* the main thread's, as sys_timer_usleep keeps one per thread */

typedef enum { PRECISE, PRECISE_SPIN, TIMER } sleep_kind;

/* Times REPS sleeps of `us` and prints requested against measured. Returns
 * the median, in microseconds; *early counts sleeps that ended before `us`. */
static double run_sleeps(sleep_kind kind, uint64_t us, int* early)
{
    static double v[REPS];
    *early = 0;
    for (int i = 0; i < REPS; i++) {
        const int64_t start = plat_clock_now();
        int64_t end;
        if (kind == TIMER) {
            plat_timer_sleep_us(&t_timer, us);
            end = plat_clock_now();
        } else {
            /* As sys_timer_usleep calls it: the 500 us default tail, and a
             * sleep of 10 us or less waits in place. */
            end = plat_sleep_precise(&t_timer, start, us, kind == PRECISE ? 500 : -1, us <= 10);
        }
        v[i] = ticks_us(end - start);
        if (end - start < (int64_t)(us * (uint64_t)plat_clock_freq() / 1000000u)) (*early)++;
    }
    qsort(v, REPS, sizeof v[0], cmp_double);
    const char* name = kind == PRECISE ? "precise" : kind == PRECISE_SPIN ? "spin only" : "timer";
    printf("  %-9s %5llu us: p50 %8.1f  p99 %8.1f  max %8.1f us  (%d early)\n", name,
           (unsigned long long)us, quantile(v, REPS, 0.5), quantile(v, REPS, 0.99), v[REPS - 1], *early);
    return quantile(v, REPS, 0.5);
}

static void test_sleeps(void)
{
    CHECK(plat_timer_open(&t_timer));
    CHECK(plat_timer_is_open(&t_timer));
    CHECK(plat_timer_open(&t_timer));        /* opening again keeps it */
    printf("sleeps (%d each, measured on plat_clock_now; the timer is %s):\n", REPS,
           t_timer.precise ? "precise" : "plain");

    /* The precise sleep never ends early (it spins to the deadline). Up to
     * the tail plus 100 us it only spins, so its median lands near the
     * request: 200 us of slack keeps a busy CI machine green, an idle one is
     * within a microsecond. Longer ones sleep on the timer first, and how late
     * that wakes is the host's: GitHub's Windows machines wake a 300 us
     * high-resolution timer at ~1 ms, past the 500 us tail. Figures only. */
    static const uint64_t sizes[] = { 1, 5, 10, 30, 100, 250, 500, 800 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        int early;
        const double p50 = run_sleeps(PRECISE, sizes[i], &early);
        CHECK(early == 0);
        if (sizes[i] <= 500 + 100) CHECK(p50 < (double)sizes[i] + 200.0);
    }
    {
        int early;
        const double p50 = run_sleeps(PRECISE_SPIN, 800, &early);
        CHECK(early == 0);
        CHECK(p50 < 800.0 + 200.0);
    }

    /* The timer alone (sys_timer_usleep's idle streak): figures only, as it
     * wakes when the system lets it. It must at least sleep, and not for
     * seconds. */
    static const uint64_t tsizes[] = { 30, 100, 800 };
    for (size_t i = 0; i < sizeof tsizes / sizeof tsizes[0]; i++) {
        int early;
        const double p50 = run_sleeps(TIMER, tsizes[i], &early);
        CHECK(p50 > 0.0 && p50 < 100000.0);
    }

    /* The plain sleep of a millisecond or more. */
    const int64_t s0 = plat_clock_now();
    plat_sleep_us(2000);
    const double slept = ticks_us(plat_clock_now() - s0);
    printf("  plain      2000 us: %.1f us\n", slept);
    CHECK(slept > 1000.0 && slept < 1000000.0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_clock();
    test_tls();
    test_sleeps();
    if (g_fail) { fprintf(stderr, "plat_time: %d check(s) failed\n", g_fail); return 1; }
    printf("plat_time: all checks passed\n");
    return 0;
}
