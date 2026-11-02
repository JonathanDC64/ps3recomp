/*
 * ps3recomp - the platform layer
 *
 * The few calls that really differ between Windows, Linux and macOS, so the
 * code above them is written once. It grows one piece at a time; this first
 * piece is what the SPU async job pool needs: detached threads with a large
 * stack reservation, the calling thread's stack range, an auto-reset event
 * with a timed wait, and a small mutex.
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

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_PLAT_H */
