/*
 * ps3recomp - the platform layer
 *
 * The few calls that really differ between Windows, Linux and macOS, so the
 * code above them is written once. It grows one piece at a time: what the SPU
 * async job pool needs (detached threads with a large stack reservation, the
 * calling thread's stack range, an auto-reset event with a timed wait, a
 * small mutex), then time and sleep (one monotonic clock, a thread's sleep
 * timer, and the precise sleep written once on them).
 *
 * On Windows each call is the Win32 call it stands for, inline, so code moved
 * onto this header makes the same calls in the same order as before. Off
 * Windows it is pthreads directly, not the Win32 shim (win32_compat.h); the
 * calls that take more than a line are in plat_posix.c.
 *
 * Calls that can fail return 1 on success and 0 on failure.
 * Usable from C and C++.
 */
#ifndef PS3RECOMP_PLAT_H
#define PS3RECOMP_PLAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#  include <sched.h>
#  include <time.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Threads ------------------------------------------------------------ */

/* A thread's entry has the operating system's own signature, so the OS calls
 * it directly: no wrapper frame and no allocation per thread, and tools that
 * name a thread by its start address see this function. Write one as
 *
 *     static plat_thread_ret PLAT_THREAD_CALL worker(void* arg) { ...; return 0; }
 */
#ifdef _WIN32
typedef DWORD plat_thread_ret;
#  define PLAT_THREAD_CALL WINAPI
#else
typedef void* plat_thread_ret;
#  define PLAT_THREAD_CALL
#endif
typedef plat_thread_ret (PLAT_THREAD_CALL* plat_thread_fn)(void* arg);

/* Start fn(arg) on a new thread that nobody joins. stack_reserve is address
 * space for the thread's stack (0: the default); pages are committed only as
 * the stack reaches them. Off Windows a host that refuses that size (a strict
 * overcommit policy, a low RLIMIT_AS) still gets the thread, on the default
 * stack: a shallow job runs fine there, and a deep one crashing beats not
 * starting at all. */
#ifdef _WIN32
static inline int plat_thread_start_detached(plat_thread_fn fn, void* arg, size_t stack_reserve)
{
    HANDLE th = CreateThread(NULL, stack_reserve, fn, arg,
                             STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!th) return 0;
    CloseHandle(th);
    return 1;
}
#else
int plat_thread_start_detached(plat_thread_fn fn, void* arg, size_t stack_reserve);
#endif

/* Keep `bytes` of the calling thread's stack for the stack-overflow handler,
 * so an overflow in deep lifted code reaches the reporter instead of ending
 * the process silently (Windows: SetThreadStackGuarantee). Off Windows there
 * is nothing to do here: a signal handler that needs room runs on its own
 * alternate stack (sigaltstack), which is per thread and set up by whoever
 * installs the handler. */
static inline void plat_thread_stack_guarantee(unsigned long bytes)
{
#ifdef _WIN32
    ULONG g = (ULONG)bytes;
    SetThreadStackGuarantee(&g);
#else
    (void)bytes;
#endif
}

/* The calling thread's stack: [*lo, *hi). Returns 0, with both 0, if the host
 * cannot say. */
#ifdef _WIN32
static inline int plat_thread_stack_range(uintptr_t* lo, uintptr_t* hi)
{
    ULONG_PTR l, h;
    GetCurrentThreadStackLimits(&l, &h);
    *lo = (uintptr_t)l;
    *hi = (uintptr_t)h;
    return 1;
}
#else
int plat_thread_stack_range(uintptr_t* lo, uintptr_t* hi);
#endif

/* ---- Events --------------------------------------------------------------- */

/* An auto-reset event: plat_event_set wakes one waiter, or the next one to
 * wait, and the wait that takes it resets it. Setting it twice before a wait
 * is one wake, not two. It lives where the caller puts it, and is not copied
 * once initialised. */
#ifdef _WIN32
typedef struct { HANDLE h; } plat_event;
#else
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             set;
} plat_event;
#endif

#define PLAT_WAIT_FOREVER 0xFFFFFFFFu   /* Win32's INFINITE */

#ifdef _WIN32
/* Not set to start with. */
static inline int  plat_event_init(plat_event* e)
{ e->h = CreateEventW(NULL, FALSE, FALSE, NULL); return e->h != NULL; }
static inline void plat_event_destroy(plat_event* e) { CloseHandle(e->h); }
static inline void plat_event_set(plat_event* e)     { SetEvent(e->h); }
/* 1 if the event was set (it is reset again), 0 if `ms` passed first.
 * PLAT_WAIT_FOREVER waits with no time limit; 0 only looks. */
static inline int  plat_event_wait(plat_event* e, unsigned ms)
{ return WaitForSingleObject(e->h, ms) == WAIT_OBJECT_0; }
#else
int  plat_event_init(plat_event* e);
void plat_event_destroy(plat_event* e);
void plat_event_set(plat_event* e);
int  plat_event_wait(plat_event* e, unsigned ms);
#endif

/* ---- Mutex ---------------------------------------------------------------- */

/* A small exclusive lock, not recursive: an SRW lock on Windows, a pthread
 * mutex elsewhere. A static one starts as PLAT_MUTEX_INIT. */
#ifdef _WIN32
typedef struct { SRWLOCK l; } plat_mutex;
#  define PLAT_MUTEX_INIT { SRWLOCK_INIT }
static inline void plat_mutex_init(plat_mutex* m)    { InitializeSRWLock(&m->l); }
static inline void plat_mutex_destroy(plat_mutex* m) { (void)m; }
static inline void plat_mutex_lock(plat_mutex* m)    { AcquireSRWLockExclusive(&m->l); }
static inline void plat_mutex_unlock(plat_mutex* m)  { ReleaseSRWLockExclusive(&m->l); }
#else
typedef struct { pthread_mutex_t l; } plat_mutex;
#  define PLAT_MUTEX_INIT { PTHREAD_MUTEX_INITIALIZER }
static inline void plat_mutex_init(plat_mutex* m)    { pthread_mutex_init(&m->l, NULL); }
static inline void plat_mutex_destroy(plat_mutex* m) { pthread_mutex_destroy(&m->l); }
static inline void plat_mutex_lock(plat_mutex* m)    { pthread_mutex_lock(&m->l); }
static inline void plat_mutex_unlock(plat_mutex* m)  { pthread_mutex_unlock(&m->l); }
#endif

/* ---- Thread-local storage, one atomic ------------------------------------- */

/* A variable each thread has its own copy of: static PLAT_THREAD_LOCAL int t_n; */
#if defined(_MSC_VER)
#  define PLAT_THREAD_LOCAL __declspec(thread)
#elif defined(__cplusplus)
#  define PLAT_THREAD_LOCAL thread_local
#else
#  define PLAT_THREAD_LOCAL _Thread_local
#endif

/* If *p is cmp, store v. Returns what *p held before, either way, as Win32's
 * InterlockedCompareExchange64 does. A full barrier. */
#ifdef _WIN32
static inline int64_t plat_atomic_cas64(volatile int64_t* p, int64_t v, int64_t cmp)
{ return InterlockedCompareExchange64(p, v, cmp); }
#else
static inline int64_t plat_atomic_cas64(volatile int64_t* p, int64_t v, int64_t cmp)
{
    __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
#endif

/* ---- Time ----------------------------------------------------------------- */

/* The monotonic clock: plat_clock_now() counts plat_clock_freq() ticks a
 * second from an arbitrary origin and never goes back. The performance
 * counter on Windows (its frequency is read once per file that asks);
 * CLOCK_MONOTONIC in nanoseconds elsewhere. */
#ifdef _WIN32
static inline int64_t plat_clock_now(void)
{ LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
static inline int64_t plat_clock_freq(void)
{ static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f); return f.QuadPart; }
#else
static inline int64_t plat_clock_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static inline int64_t plat_clock_freq(void) { return 1000000000; }
#endif

/* `ticks` of a clock that counts `freq` a second, in a unit that counts
 * `per_sec` a second (1000000 for microseconds), rounded down. Whole seconds
 * and the rest apart, so the multiply does not overflow. */
static inline uint64_t plat_ticks_scale(uint64_t ticks, uint64_t freq, uint64_t per_sec)
{ return (ticks / freq) * per_sec + (ticks % freq) * per_sec / freq; }

/* Hand the core to another thread that is ready to run, if there is one. */
static inline void plat_yield(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

/* A spin-wait's pause: tells the core this thread is only waiting. */
static inline void plat_relax(void)
{
#if defined(_WIN32)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

/* Sleep about `us` microseconds, one millisecond or more, with no timer of
 * the thread's own. On Windows a plain waitable timer made for the one sleep
 * (it wakes on the system tick), or Sleep if none can be made; elsewhere
 * nanosleep. */
#ifdef _WIN32
static inline void plat_sleep_us(uint64_t us)
{
    HANDLE timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -((LONGLONG)us * 10); /* 100ns units, negative = relative */
        SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(timer, INFINITE);
        CloseHandle(timer);
    } else {
        Sleep((DWORD)(us / 1000));
    }
}
#else
void plat_sleep_us(uint64_t us);
#endif

/* A thread's sleep timer. It starts zeroed (a static PLAT_THREAD_LOCAL), is
 * opened by the thread that sleeps on it and lasts the thread's life.
 * `precise` is 1 where a sleep on it ends within about half a millisecond:
 * Windows 10 1803's high-resolution timer, and every POSIX sleep (late by the
 * timer slack, 50 us by default on Linux). Older Windows has only a plain
 * waitable timer, which wakes on the system tick. */
#ifdef _WIN32
typedef struct { HANDLE h; int precise; } plat_timer;
/* 1 if the timer is open, now or from before. */
static inline int plat_timer_open(plat_timer* t)
{
    if (!t->h) {
        /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+). */
        t->h = CreateWaitableTimerExW(NULL, NULL, 0x00000002u, TIMER_ALL_ACCESS);
        t->precise = t->h != NULL;
        if (!t->h) t->h = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    return t->h != NULL;
}
static inline int plat_timer_is_open(const plat_timer* t) { return t->h != NULL; }
/* Sleep about `us` microseconds on an open timer. */
static inline void plat_timer_sleep_us(plat_timer* t, uint64_t us)
{
    LARGE_INTEGER due;
    due.QuadPart = -((LONGLONG)us * 10);
    SetWaitableTimer(t->h, &due, 0, NULL, NULL, FALSE);
    WaitForSingleObject(t->h, INFINITE);
}
#else
typedef struct { int open, precise; } plat_timer;
static inline int plat_timer_open(plat_timer* t) { t->open = t->precise = 1; return 1; }
static inline int plat_timer_is_open(const plat_timer* t) { return t->open; }
static inline void plat_timer_sleep_us(plat_timer* t, uint64_t us) { (void)t; plat_sleep_us(us); }
#endif

/* The precise sleep, written once: until `us` microseconds after `start` (a
 * plat_clock_now reading), to the clock's tick. It sleeps on the thread's
 * timer for all but the last `tail_us`, then spins to the deadline; the tail
 * covers the timer's wake latency. (Windows' high-resolution timer fires on
 * ~0.5 ms boundaries: with a 500 us tail an 800 us sleep ends at ~830 us and
 * costs ~13% of a core, where spinning it all cost 50%.) It only spins when
 * tail_us < 0, the timer is not precise, or `us` is tail_us + 100 or less.
 * Between clock reads the spin hands the core to another ready thread, or
 * with in_place pauses on this one: a hand-over can cost a whole quantum.
 * Returns the clock reading that ended it. Meant for short sleeps: `us`
 * times the clock's frequency must fit 64 bits (hours, at 1 GHz). */
static inline int64_t plat_sleep_precise(plat_timer* t, int64_t start, uint64_t us,
                                         int tail_us, int in_place)
{
    int64_t deadline, now;
    if (tail_us >= 0 && t->precise && us > (uint64_t)tail_us + 100)
        plat_timer_sleep_us(t, us - (uint64_t)tail_us);
    deadline = start + (int64_t)((us * (uint64_t)plat_clock_freq()) / 1000000ULL);
    do {
        if (in_place) plat_relax();
        else plat_yield();
        now = plat_clock_now();
    } while (now < deadline);
    return now;
}

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_PLAT_H */
