/*
 * ps3recomp - threads, locks and counters for setup's shader pass
 * (rsx_shader_precompile.c), on every OS.
 *
 * On Windows each name is the Win32 call itself, as a macro, so the pass
 * compiles to the code it had before this header. Elsewhere they are
 * pthreads and the compiler's __atomic built-ins (built with the portable
 * renderer, PS3RECOMP_RSX_HAL, for the spv target).
 *
 *   rsx_pc_lock      a lock, RSX_PC_LOCK_INIT; rsx_pc_lock_x/_unlock_x hold it
 *                    alone, rsx_pc_lock_s/_unlock_s shared (alone on pthreads)
 *   rsx_pc_cond      a condition variable, RSX_PC_COND_INIT; rsx_pc_cond_wait
 *                    with the lock held alone, rsx_pc_wake_one, rsx_pc_wake_all
 *   rsx_pc_counter   a volatile counter for rsx_pc_inc and rsx_pc_dec (the new
 *                    value) and rsx_pc_xchg (the old one); a plain read of one
 *                    is a single load
 *   rsx_pc_thread    a thread running a function declared
 *                    RSX_PC_THREAD_FN(name, arg), which returns 0;
 *                    rsx_pc_thread_start(fn) gives 0 when it cannot start one,
 *                    rsx_pc_thread_join_all(t, n) waits for n of them
 *                    (n <= 64; again later returns at once), and
 *                    rsx_pc_thread_close(t) lets one go after that
 *
 * Small on purpose: the platform module's threads (runtime/platform/plat.h,
 * ps3recomp #180) take this over once they are in, and the pass moves onto
 * plat.h then.
 */
#ifndef PS3RECOMP_RSX_PC_SYNC_H
#define PS3RECOMP_RSX_PC_SYNC_H

#ifdef _WIN32

#include <windows.h>

#define rsx_pc_lock                  SRWLOCK
#define RSX_PC_LOCK_INIT             SRWLOCK_INIT
#define rsx_pc_lock_x                AcquireSRWLockExclusive
#define rsx_pc_unlock_x              ReleaseSRWLockExclusive
#define rsx_pc_lock_s                AcquireSRWLockShared
#define rsx_pc_unlock_s              ReleaseSRWLockShared
#define rsx_pc_cond                  CONDITION_VARIABLE
#define RSX_PC_COND_INIT             CONDITION_VARIABLE_INIT
#define rsx_pc_cond_wait(c, l)       SleepConditionVariableSRW(c, l, INFINITE, 0)
#define rsx_pc_wake_one              WakeConditionVariable
#define rsx_pc_wake_all              WakeAllConditionVariable
#define rsx_pc_counter               LONG
#define rsx_pc_inc                   InterlockedIncrement
#define rsx_pc_dec                   InterlockedDecrement
#define rsx_pc_xchg                  InterlockedExchange
#define rsx_pc_thread                HANDLE
#define RSX_PC_THREAD_FN(name, arg)  DWORD WINAPI name(LPVOID arg)
#define rsx_pc_thread_start(fn)      CreateThread(NULL, 0, fn, NULL, 0, NULL)
#define rsx_pc_thread_join_all(t, n) WaitForMultipleObjects((DWORD)n, t, TRUE, INFINITE)
#define rsx_pc_thread_close          CloseHandle

#else /* pthreads */

#include <pthread.h>
#include <stdlib.h>

typedef pthread_mutex_t rsx_pc_lock;
#define RSX_PC_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void rsx_pc_lock_x(rsx_pc_lock* l)   { pthread_mutex_lock(l); }
static inline void rsx_pc_unlock_x(rsx_pc_lock* l) { pthread_mutex_unlock(l); }
static inline void rsx_pc_lock_s(rsx_pc_lock* l)   { pthread_mutex_lock(l); }
static inline void rsx_pc_unlock_s(rsx_pc_lock* l) { pthread_mutex_unlock(l); }

typedef pthread_cond_t rsx_pc_cond;
#define RSX_PC_COND_INIT PTHREAD_COND_INITIALIZER
static inline void rsx_pc_cond_wait(rsx_pc_cond* c, rsx_pc_lock* l) { pthread_cond_wait(c, l); }
static inline void rsx_pc_wake_one(rsx_pc_cond* c) { pthread_cond_signal(c); }
static inline void rsx_pc_wake_all(rsx_pc_cond* c) { pthread_cond_broadcast(c); }

typedef long rsx_pc_counter;
static inline long rsx_pc_inc(volatile rsx_pc_counter* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline long rsx_pc_dec(volatile rsx_pc_counter* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline long rsx_pc_xchg(volatile rsx_pc_counter* p, long v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

/* A thread is a small block of its own, so that 0 can mean none and a join
 * can be asked for twice. */
typedef struct rsx_pc_thread_s { pthread_t t; int joined; }* rsx_pc_thread;
#define RSX_PC_THREAD_FN(name, arg) void* name(void* arg)

static inline rsx_pc_thread rsx_pc_thread_start(void* (*fn)(void*))
{
    rsx_pc_thread h = (rsx_pc_thread)calloc(1, sizeof *h);
    if (h && pthread_create(&h->t, NULL, fn, NULL) != 0) { free(h); h = NULL; }
    return h;
}
static inline void rsx_pc_thread_join_all(rsx_pc_thread* t, int n)
{
    for (int i = 0; i < n; i++)
        if (t[i] && !t[i]->joined) { pthread_join(t[i]->t, NULL); t[i]->joined = 1; }
}
static inline void rsx_pc_thread_close(rsx_pc_thread t)
{
    if (!t) return;
    if (!t->joined) pthread_join(t->t, NULL);
    free(t);
}

#endif /* _WIN32 */
#endif /* PS3RECOMP_RSX_PC_SYNC_H */
