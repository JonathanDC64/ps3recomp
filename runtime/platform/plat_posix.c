/*
 * ps3recomp - the platform layer on Linux and macOS (see plat.h)
 *
 * The calls that take more than a line. Where the two systems differ (the
 * stack range, the clock a timed wait uses) the difference is here and
 * nowhere else. Empty on Windows, where plat.h has everything inline.
 */
#ifndef _WIN32

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* pthread_getattr_np */
#endif

#include "plat.h"

#include <errno.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

/* ---- Threads ------------------------------------------------------------ */

static int start_detached(plat_thread_fn fn, void* arg, size_t stack)
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return 0;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = 0;
    if (stack) {
        /* Darwin rejects a size that is not a page multiple; both reject one
         * under the minimum. */
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        if (page == 0) page = 4096;
        if (stack < (size_t)PTHREAD_STACK_MIN) stack = (size_t)PTHREAD_STACK_MIN;
        stack = (stack + page - 1) & ~(page - 1);
        rc = pthread_attr_setstacksize(&attr, stack);
    }
    pthread_t th;
    if (rc == 0) rc = pthread_create(&th, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

int plat_thread_start_detached(plat_thread_fn fn, void* arg, size_t stack_reserve)
{
    if (start_detached(fn, arg, stack_reserve)) return 1;
    return stack_reserve != 0 && start_detached(fn, arg, 0);
}

/* A thread's stack never moves, and asking is not free on Linux:
 * pthread_getattr_np also reads the thread's CPU affinity, a system call, and
 * for the main thread it reads /proc/self/maps. The pool asks after every
 * job, so each thread asks once. */
static _Thread_local uintptr_t t_stack_lo, t_stack_hi;

int plat_thread_stack_range(uintptr_t* lo, uintptr_t* hi)
{
    if (!t_stack_hi) {
#if defined(__APPLE__)
        /* The address Darwin gives is the top of the stack. */
        pthread_t self = pthread_self();
        const uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
        const size_t size = pthread_get_stacksize_np(self);
        if (top && size && size <= top) { t_stack_lo = top - size; t_stack_hi = top; }
#elif defined(__linux__)
        pthread_attr_t a;
        if (pthread_getattr_np(pthread_self(), &a) == 0) {
            void* addr = NULL;
            size_t size = 0;
            if (pthread_attr_getstack(&a, &addr, &size) == 0 && addr && size) {
                t_stack_lo = (uintptr_t)addr;
                t_stack_hi = (uintptr_t)addr + size;
            }
            pthread_attr_destroy(&a);
        }
#endif
    }
    *lo = t_stack_lo;
    *hi = t_stack_hi;
    return t_stack_hi != 0;
}

/* ---- Events --------------------------------------------------------------- */

/* A timed wait counts against the monotonic clock, so a change to the wall
 * clock neither ends it early nor holds it. Linux's condition variable takes
 * that clock as an attribute; Darwin has no such attribute, but waits for a
 * relative time, which is computed from the same clock. */

static void mono_now(struct timespec* t) { clock_gettime(CLOCK_MONOTONIC, t); }

#if defined(__APPLE__)
static int ts_before(const struct timespec* a, const struct timespec* b)
{
    return a->tv_sec != b->tv_sec ? a->tv_sec < b->tv_sec : a->tv_nsec < b->tv_nsec;
}
#endif

int plat_event_init(plat_event* e)
{
    e->set = 0;
    if (pthread_mutex_init(&e->m, NULL) != 0) return 0;
    pthread_condattr_t ca;
    if (pthread_condattr_init(&ca) != 0) { pthread_mutex_destroy(&e->m); return 0; }
#if !defined(__APPLE__)
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
    const int rc = pthread_cond_init(&e->c, &ca);
    pthread_condattr_destroy(&ca);
    if (rc != 0) { pthread_mutex_destroy(&e->m); return 0; }
    return 1;
}

void plat_event_destroy(plat_event* e)
{
    pthread_cond_destroy(&e->c);
    pthread_mutex_destroy(&e->m);
}

void plat_event_set(plat_event* e)
{
    pthread_mutex_lock(&e->m);
    e->set = 1;
    pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
}

int plat_event_wait(plat_event* e, unsigned ms)
{
    pthread_mutex_lock(&e->m);
    if (!e->set && ms == PLAT_WAIT_FOREVER) {
        while (!e->set) pthread_cond_wait(&e->c, &e->m);
    } else if (!e->set && ms != 0) {
        struct timespec end;
        mono_now(&end);
        end.tv_sec  += (time_t)(ms / 1000u);
        end.tv_nsec += (long)(ms % 1000u) * 1000000L;
        if (end.tv_nsec >= 1000000000L) { end.tv_sec++; end.tv_nsec -= 1000000000L; }
        while (!e->set) {
#if defined(__APPLE__)
            struct timespec now, rel;
            mono_now(&now);
            if (!ts_before(&now, &end)) break;
            rel.tv_sec  = end.tv_sec - now.tv_sec;
            rel.tv_nsec = end.tv_nsec - now.tv_nsec;
            if (rel.tv_nsec < 0) { rel.tv_sec--; rel.tv_nsec += 1000000000L; }
            pthread_cond_timedwait_relative_np(&e->c, &e->m, &rel);
#else
            if (pthread_cond_timedwait(&e->c, &e->m, &end) == ETIMEDOUT) break;
#endif
        }
    }
    const int got = e->set;
    e->set = 0;
    pthread_mutex_unlock(&e->m);
    return got;
}

/* ---- Time ----------------------------------------------------------------- */

void plat_sleep_us(uint64_t us)
{
    struct timespec d;
    d.tv_sec  = (time_t)(us / 1000000u);
    d.tv_nsec = (long)(us % 1000000u) * 1000L;
    /* A signal ends a sleep early; sleep the rest. */
    while (nanosleep(&d, &d) != 0 && errno == EINTR) {}
}

#else
typedef int plat_posix_off;   /* (a file with no declaration is not ISO C) */
#endif /* !_WIN32 */
