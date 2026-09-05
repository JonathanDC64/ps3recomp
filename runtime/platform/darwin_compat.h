/*
 * ps3recomp - POSIX functions absent on Darwin
 *
 * macOS ships neither pthread_mutex_timedlock (POSIX 2001) nor sem_timedwait
 * (POSIX realtime). Both are emulated by polling the non-blocking variant
 * against the caller's absolute CLOCK_REALTIME deadline. Contended waits here
 * are rare and short (guest sync primitives with millisecond timeouts), so the
 * poll interval costs less than pulling in a full futex-style reimplementation.
 */
#ifndef PS3RECOMP_DARWIN_COMPAT_H
#define PS3RECOMP_DARWIN_COMPAT_H

#if defined(__APPLE__)

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

/* Poll granularity. 200 us keeps worst-case overshoot well under the 1 ms
 * resolution the guest timeout APIs actually express. */
#define PS3_TIMEDWAIT_POLL_NS  200000L

static inline int ps3__deadline_passed(const struct timespec* abs)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec  != abs->tv_sec) return now.tv_sec > abs->tv_sec;
    return now.tv_nsec >= abs->tv_nsec;
}

static inline void ps3__poll_sleep(void)
{
    struct timespec d = { 0, PS3_TIMEDWAIT_POLL_NS };
    nanosleep(&d, NULL);
}

static inline int pthread_mutex_timedlock(pthread_mutex_t* m,
                                          const struct timespec* abstime)
{
    for (;;) {
        int rc = pthread_mutex_trylock(m);
        if (rc != EBUSY) return rc;              /* acquired, or a real error */
        if (ps3__deadline_passed(abstime)) return ETIMEDOUT;
        ps3__poll_sleep();
    }
}

static inline int ps3_sem_timedwait(sem_t* s, const struct timespec* abstime)
{
    for (;;) {
        if (sem_trywait(s) == 0) return 0;
        if (errno != EAGAIN) return -1;          /* propagate the real error */
        if (ps3__deadline_passed(abstime)) { errno = ETIMEDOUT; return -1; }
        ps3__poll_sleep();
    }
}
#define sem_timedwait ps3_sem_timedwait

#endif /* __APPLE__ */
#endif /* PS3RECOMP_DARWIN_COMPAT_H */
