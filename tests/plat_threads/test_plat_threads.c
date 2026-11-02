/* tests/plat_threads -- runtime/platform/plat.h's threads, events and mutex,
 * and the SPU async job pool written on them (runtime/spu/spu_async_pool.c).
 *
 * The pool is built here with a 1 s idle time and room for 16 idle workers
 * (CMakeLists.txt), so the idle exit and the idle limit can be watched in a
 * few seconds. Everything else is the code the runtime runs.
 *
 * Exit code 0 if every check passed; each failure prints a line.
 */
#include "platform/plat.h"
#include "spu/spu_async_pool.h"
#include "ps3emu/ps3_knob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <time.h>
#endif

#ifndef SPU_ASYNC_POOL_IDLE_MS
#error "build with the test's SPU_ASYNC_POOL_IDLE_MS (CMakeLists.txt)"
#endif
#define IDLE_MS  SPU_ASYNC_POOL_IDLE_MS
#define MAX_IDLE SPU_ASYNC_POOL_MAX_IDLE

/* ---- test helpers (the test's own, not plat.h's) ------------------------- */

static int g_fail;
#define CHECK(c) do { if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#if defined(_MSC_VER)
#  define TEST_TLS __declspec(thread)
#else
#  define TEST_TLS _Thread_local
#endif

#ifdef _WIN32
static long atom_inc(volatile long* p) { return InterlockedIncrement(p); }
static long atom_dec(volatile long* p) { return InterlockedDecrement(p); }
static long atom_get(volatile long* p) { return InterlockedCompareExchange(p, 0, 0); }
static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
static void sleep_ms(unsigned ms) { Sleep(ms); }
static void set_env(const char* k, const char* v) { _putenv_s(k, v); }
#else
static long atom_inc(volatile long* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static long atom_dec(volatile long* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static long atom_get(volatile long* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}
static void sleep_ms(unsigned ms)
{
    struct timespec d = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&d, NULL);
}
static void set_env(const char* k, const char* v) { setenv(k, v, 1); }
#endif

/* Poll until *p == want or the deadline passes; returns whether it got there. */
static int wait_until_eq(volatile long* p, long want, unsigned ms)
{
    const double end = now_ms() + ms;
    while (atom_get(p) != want) {
        if (now_ms() > end) return 0;
        sleep_ms(1);
    }
    return 1;
}

static int wait_idle(int want, unsigned ms)
{
    const double end = now_ms() + ms;
    while (spu_async_pool_idle() != want) {
        if (now_ms() > end) return 0;
        sleep_ms(1);
    }
    return 1;
}

static int wait_idle_ge(int want, unsigned ms)
{
    const double end = now_ms() + ms;
    while (spu_async_pool_idle() < want) {
        if (now_ms() > end) return 0;
        sleep_ms(1);
    }
    return 1;
}

static int in_range(const void* p, uintptr_t lo, uintptr_t hi)
{
    return (uintptr_t)p >= lo && (uintptr_t)p < hi;
}

/* ---- events ------------------------------------------------------------- */

typedef struct { plat_event* ev; unsigned delay_ms; } setter_arg;

static plat_thread_ret PLAT_THREAD_CALL setter_main(void* p)
{
    setter_arg* a = (setter_arg*)p;
    sleep_ms(a->delay_ms);
    plat_event_set(a->ev);
    return 0;
}

typedef struct { plat_event ping, pong, done; int rounds; volatile long missed; } pingpong;

static plat_thread_ret PLAT_THREAD_CALL ponger_main(void* p)
{
    pingpong* pp = (pingpong*)p;
    for (int i = 0; i < pp->rounds; i++) {
        if (!plat_event_wait(&pp->ping, 5000)) { atom_inc(&pp->missed); break; }
        plat_event_set(&pp->pong);
    }
    plat_event_set(&pp->done);
    return 0;
}

static void test_events(void)
{
    plat_event e;
    CHECK(plat_event_init(&e));
    CHECK(plat_event_wait(&e, 0) == 0);                 /* starts unset */

    double t0 = now_ms();
    CHECK(plat_event_wait(&e, 50) == 0);                /* times out */
    double dt = now_ms() - t0;
    CHECK(dt >= 30.0 && dt < 5000.0);
    if (!(dt >= 30.0 && dt < 5000.0)) fprintf(stderr, "  50 ms wait took %.1f ms\n", dt);

    plat_event_set(&e);
    CHECK(plat_event_wait(&e, 0) == 1);                 /* set ... */
    CHECK(plat_event_wait(&e, 0) == 0);                 /* ... and reset by the wait */

    plat_event_set(&e);
    plat_event_set(&e);
    CHECK(plat_event_wait(&e, 0) == 1);                 /* two sets are one wake */
    CHECK(plat_event_wait(&e, 10) == 0);

    /* Set from another thread while this one waits, with and without a limit. */
    setter_arg a = { &e, 20 };
    CHECK(plat_thread_start_detached(setter_main, &a, 0));
    CHECK(plat_event_wait(&e, PLAT_WAIT_FOREVER) == 1);
    t0 = now_ms();
    CHECK(plat_thread_start_detached(setter_main, &a, 0));
    CHECK(plat_event_wait(&e, 10000) == 1);
    CHECK(now_ms() - t0 < 5000.0);
    plat_event_destroy(&e);

    /* No lost wake-ups: 20000 hand-overs between two threads. */
    static pingpong pp;
    pp.rounds = 20000;
    CHECK(plat_event_init(&pp.ping) && plat_event_init(&pp.pong) && plat_event_init(&pp.done));
    CHECK(plat_thread_start_detached(ponger_main, &pp, 0));
    int ok = 1;
    for (int i = 0; i < pp.rounds && ok; i++) {
        plat_event_set(&pp.ping);
        ok = plat_event_wait(&pp.pong, 5000);
    }
    CHECK(ok);
    CHECK(plat_event_wait(&pp.done, 10000));
    CHECK(atom_get(&pp.missed) == 0);
    plat_event_destroy(&pp.ping);
    plat_event_destroy(&pp.pong);
    plat_event_destroy(&pp.done);
}

/* ---- mutex -------------------------------------------------------------- */

#define MUTEX_THREADS 8
#define MUTEX_ITERS   100000

static plat_mutex s_static_mutex = PLAT_MUTEX_INIT;
typedef struct { plat_mutex* m; volatile long* done; plat_event* all; } mutex_arg;
static long s_plain_counter;

static plat_thread_ret PLAT_THREAD_CALL mutex_main(void* p)
{
    mutex_arg* a = (mutex_arg*)p;
    for (int i = 0; i < MUTEX_ITERS; i++) {
        plat_mutex_lock(a->m);
        s_plain_counter++;          /* not atomic: only the lock keeps it right */
        plat_mutex_unlock(a->m);
    }
    if (atom_inc(a->done) == MUTEX_THREADS) plat_event_set(a->all);
    return 0;
}

static void run_mutex(plat_mutex* m)
{
    volatile long done = 0;
    plat_event all;
    CHECK(plat_event_init(&all));
    mutex_arg a = { m, &done, &all };
    s_plain_counter = 0;
    for (int i = 0; i < MUTEX_THREADS; i++) CHECK(plat_thread_start_detached(mutex_main, &a, 0));
    CHECK(plat_event_wait(&all, 60000));
    plat_mutex_lock(m);
    CHECK(s_plain_counter == (long)MUTEX_THREADS * MUTEX_ITERS);
    plat_mutex_unlock(m);
    plat_event_destroy(&all);
}

static void test_mutex(void)
{
    run_mutex(&s_static_mutex);
    plat_mutex m;
    plat_mutex_init(&m);
    run_mutex(&m);
    plat_mutex_destroy(&m);
}

/* ---- stack range and big reservations ---------------------------------- */

#define BIG_STACK   (256u << 20)
#define BIG_THREADS 32
#define DEEP_BYTES  (16u << 20)   /* more than any host's default stack */

typedef struct {
    volatile long started, left, deep_ok, range_ok, size_ok;
    plat_event go;
} big_arg;

/* Touch DEEP_BYTES of stack: crashes on a default-sized stack. */
static int touch_deep(void)
{
    volatile unsigned char buf[DEEP_BYTES];
    for (size_t i = 0; i < sizeof buf; i += 4096) buf[i] = (unsigned char)(i >> 12);
    return buf[4096] == 1;
}

static plat_thread_ret PLAT_THREAD_CALL big_main(void* p)
{
    big_arg* a = (big_arg*)p;
    int local = 0;
    uintptr_t lo = 0, hi = 0;
    if (plat_thread_stack_range(&lo, &hi) && in_range(&local, lo, hi)) atom_inc(&a->range_ok);
    if (hi - lo >= ((uintptr_t)200 << 20)) atom_inc(&a->size_ok);
    const long me = atom_inc(&a->started);
    if (me <= 4 && touch_deep()) atom_inc(&a->deep_ok);   /* commit 4 x 16 MB, not 32 */
    /* Every thread stays until all of them exist: 32 x 256 MB reserved at
     * once. The last one in sets the event and each waiter passes it on. */
    if (me == BIG_THREADS) plat_event_set(&a->go);
    plat_event_wait(&a->go, 20000);
    plat_event_set(&a->go);
    atom_inc(&a->left);
    return 0;
}

typedef struct { volatile long ok, done; plat_event fin; } dflt_arg;

static plat_thread_ret PLAT_THREAD_CALL default_stack_main(void* p)
{
    dflt_arg* a = (dflt_arg*)p;
    int local = 0;
    uintptr_t lo = 0, hi = 0;
    if (plat_thread_stack_range(&lo, &hi) && in_range(&local, lo, hi) && lo < hi) atom_inc(&a->ok);
    plat_event_set(&a->fin);
    return 0;
}

static void test_stacks(void)
{
    int local = 0;
    uintptr_t lo = 0, hi = 0;
    CHECK(plat_thread_stack_range(&lo, &hi));            /* the main thread */
    CHECK(lo < hi && in_range(&local, lo, hi));
    uintptr_t lo2 = 0, hi2 = 0;
    CHECK(plat_thread_stack_range(&lo2, &hi2) && lo2 == lo && hi2 == hi);

    dflt_arg d;
    memset(&d, 0, sizeof d);
    CHECK(plat_event_init(&d.fin));
    CHECK(plat_thread_start_detached(default_stack_main, &d, 0));
    CHECK(plat_event_wait(&d.fin, 10000));
    CHECK(atom_get(&d.ok) == 1);
    plat_event_destroy(&d.fin);

    static big_arg b;
    CHECK(plat_event_init(&b.go));
    int made = 0;
    for (int i = 0; i < BIG_THREADS; i++) made += plat_thread_start_detached(big_main, &b, BIG_STACK);
    CHECK(made == BIG_THREADS);
    CHECK(wait_until_eq(&b.left, made, 30000));
    CHECK(atom_get(&b.deep_ok) == 4);
    CHECK(atom_get(&b.range_ok) == BIG_THREADS);
    CHECK(atom_get(&b.size_ok) == BIG_THREADS);
    if (atom_get(&b.size_ok) != BIG_THREADS)
        fprintf(stderr, "  %ld of %d threads had a 256 MB stack\n", atom_get(&b.size_ok), BIG_THREADS);
    /* The threads may still be returning; b and its event stay (static). */
}

/* ---- the pool ----------------------------------------------------------- */

static volatile long g_threads;     /* threads that have run a pool job */
static volatile long g_jobs;
static volatile long g_inflight, g_peak;
static volatile long g_bad_stack;
static TEST_TLS int t_seen;

/* Counts each new thread once, and checks the worker's stack. */
static void note_thread(void)
{
    if (t_seen) return;
    t_seen = 1;
    atom_inc(&g_threads);
    int local = 0;
    uintptr_t lo = 0, hi = 0;
    if (!plat_thread_stack_range(&lo, &hi) || !in_range(&local, lo, hi) ||
        hi - lo < ((uintptr_t)200 << 20))
        atom_inc(&g_bad_stack);
}

static void enter_job(void)
{
    note_thread();
    const long n = atom_inc(&g_inflight);
    long pk;
    while ((pk = atom_get(&g_peak)) < n) {
#ifdef _WIN32
        if (InterlockedCompareExchange(&g_peak, n, pk) == pk) break;
#else
        if (__atomic_compare_exchange_n(&g_peak, &pk, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
#endif
    }
}

static void leave_job(void)
{
    atom_dec(&g_inflight);
    atom_inc(&g_jobs);
}

/* A short job; the dispatcher waits for it, as a PPU thread waits for its
 * SPU job. */
typedef struct { plat_event done; } dispatcher;

static void short_job(void* p)
{
    enter_job();
    volatile unsigned x = 0;
    for (unsigned i = 0; i < 2000; i++) x += i;
    leave_job();
    plat_event_set(&((dispatcher*)p)->done);
}

#define DISPATCHERS 4
#define JOBS_EACH   2000

typedef struct { dispatcher d; volatile long* left; volatile long* failed; plat_event* all; } disp_arg;

static plat_thread_ret PLAT_THREAD_CALL dispatcher_main(void* p)
{
    disp_arg* a = (disp_arg*)p;
    for (int i = 0; i < JOBS_EACH; i++) {
        if (!spu_async_pool_start(short_job, &a->d, 0) || !plat_event_wait(&a->d.done, 10000)) {
            atom_inc(a->failed);
            break;
        }
    }
    if (atom_dec(a->left) == 0) plat_event_set(a->all);
    return 0;
}

/* Jobs that wait for each other: all N must run at once. */
typedef struct { volatile long arrived, released, done; long n; double t_done; } gate;

static void gate_job(void* p)
{
    gate* g = (gate*)p;
    enter_job();
    atom_inc(&g->arrived);
    const double end = now_ms() + 20000;
    while (atom_get(&g->arrived) < g->n && now_ms() < end) sleep_ms(1);
    if (atom_get(&g->arrived) >= g->n) atom_inc(&g->released);
    leave_job();
    atom_inc(&g->done);
}

static int run_gate(gate* g, long n, int own)
{
    memset(g, 0, sizeof *g);
    g->n = n;
    int started = 0;
    for (long i = 0; i < n; i++) started += spu_async_pool_start(gate_job, g, own);
    const int ok = started == n && wait_until_eq(&g->done, n, 30000);
    g->t_done = now_ms();
    return ok && atom_get(&g->released) == n;
}

/* One job, waited for; returns whether this thread was new. */
static int one_job(int own)
{
    static dispatcher d;
    static int init;
    if (!init) { CHECK(plat_event_init(&d.done)); init = 1; }
    const long before = atom_get(&g_threads);
    CHECK(spu_async_pool_start(short_job, &d, own));
    CHECK(plat_event_wait(&d.done, 10000));
    return atom_get(&g_threads) != before;
}

static void test_pool(void)
{
    /* 1. Thousands of short jobs from several threads: they all run, on about
     * as many threads as were busy at once, not one per job. */
    static disp_arg da[DISPATCHERS];
    volatile long left = DISPATCHERS, failed = 0;
    plat_event all;
    CHECK(plat_event_init(&all));
    for (int i = 0; i < DISPATCHERS; i++) {
        da[i].left = &left; da[i].failed = &failed; da[i].all = &all;
        CHECK(plat_event_init(&da[i].d.done));
    }
    double t0 = now_ms();
    for (int i = 0; i < DISPATCHERS; i++) CHECK(plat_thread_start_detached(dispatcher_main, &da[i], 0));
    CHECK(plat_event_wait(&all, 120000));
    const double burst_ms = now_ms() - t0;
    CHECK(atom_get(&failed) == 0);
    CHECK(atom_get(&g_jobs) == (long)DISPATCHERS * JOBS_EACH);
    const long burst_threads = atom_get(&g_threads);
    printf("burst: %ld jobs from %d threads in %.0f ms on %ld threads (peak %ld at once)\n",
           atom_get(&g_jobs), DISPATCHERS, burst_ms, burst_threads, atom_get(&g_peak));
    CHECK(burst_threads >= 1 && burst_threads <= 2 * MAX_IDLE);
    CHECK(atom_get(&g_peak) <= DISPATCHERS);
    /* Every worker parks again (none has been idle for IDLE_MS yet, unless
     * this machine stalled for a second). */
    CHECK(spu_async_pool_idle() <= MAX_IDLE);

    /* 2. No queue: N jobs that wait for each other all run at once, and the
     * idle list keeps MAX_IDLE of their threads; the rest exit at once. */
    static gate g;
    const long n = MAX_IDLE + 8;
    CHECK(run_gate(&g, n, 0));
    CHECK(wait_idle(MAX_IDLE, 5000));
    CHECK(spu_async_pool_idle() == MAX_IDLE);
    sleep_ms(50);
    CHECK(spu_async_pool_idle() == MAX_IDLE);           /* never more */

    /* 3. Idle exit: nobody leaves before IDLE_MS, everybody after. */
    const double t_done = g.t_done;
    double now;
    while ((now = now_ms()) < t_done + IDLE_MS * 0.3) sleep_ms(5);
    if (now < t_done + IDLE_MS * 0.8) CHECK(spu_async_pool_idle() == MAX_IDLE);
    CHECK(wait_idle(0, IDLE_MS + 10000));
    const double gone = now_ms() - t_done;
    CHECK(gone >= IDLE_MS * 0.85);
    printf("idle exit: %d idle workers gone %.0f ms after their jobs (idle time %d ms)\n",
           MAX_IDLE, gone, IDLE_MS);
    CHECK(one_job(0));                                   /* the next job: a new thread */
    CHECK(wait_idle(1, 5000));

    /* 4. Own thread: a new thread although one is idle, and it does not park. */
    CHECK(one_job(1));
    sleep_ms(50);
    CHECK(spu_async_pool_idle() <= 1);
    CHECK(!one_job(0));                                  /* an ordinary job reuses */
    CHECK(wait_idle(1, 5000));

    /* 5. SPU_ASYNC_POOL=0 (live): a new thread per job although one is idle. */
    set_env("SPU_ASYNC_POOL", "0");
    ps3_env_gen++;
    int fresh = 0;
    for (int i = 0; i < 10; i++) fresh += one_job(0);
    CHECK(fresh == 10);
    set_env("SPU_ASYNC_POOL", "1");
    ps3_env_gen++;
    CHECK(wait_idle_ge(10, 5000));                       /* as before: they still park */
    CHECK(!one_job(0));                                  /* back on: reused */

    CHECK(atom_get(&g_bad_stack) == 0);
    printf("pool: %ld threads for %ld jobs\n", atom_get(&g_threads), atom_get(&g_jobs));
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_events();
    test_mutex();
    test_stacks();
    test_pool();
    if (g_fail) { fprintf(stderr, "plat_threads: %d check(s) failed\n", g_fail); return 1; }
    printf("plat_threads: all checks passed\n");
    return 0;
}
