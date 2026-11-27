/*
 * ps3recomp - the first thread on macOS (see plat.h, plat_main)
 *
 * AppKit runs windows and their events on a process's first thread only. The
 * program's body moves to a thread of its own, and the first thread runs the
 * work handed to it (the game window's thread), and its run loop in between.
 * Empty off macOS, where plat.h has plat_main inline.
 */
#if defined(__APPLE__)

#include "plat.h"

#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

enum { FIRST_JOBS = 8 };
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { void (*fn)(void*); void* arg; } s_jobs[FIRST_JOBS];
static int s_jobs_n;
static CFRunLoopRef volatile s_loop;   /* the first thread's, while plat_main serves it */

typedef struct { int (*body)(int, char**); int argc; char** argv; } body_args;

static void* body_thread(void* p)
{
    const body_args* a = (const body_args*)p;
    exit(a->body(a->argc, a->argv));   /* (the first thread serves its work until now) */
}

int plat_main(int (*body)(int, char**), int argc, char** argv)
{
    if (!pthread_main_np()) return body(argc, argv);
    static body_args a;
    a.body = body;
    a.argc = argc;
    a.argv = argv;
    /* The first thread's stack size (RLIMIT_STACK: 8 MB unless the shell
     * asked for more), in whole 16 KB pages, which Darwin wants. */
    size_t stack = (size_t)8 << 20;
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && (size_t)rl.rlim_cur > stack)
        stack = (size_t)rl.rlim_cur;
    stack = (stack + 16383u) & ~(size_t)16383u;
    pthread_attr_t attr;
    pthread_t t;
    int ok = pthread_attr_init(&attr) == 0;
    if (ok) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, stack);
        s_loop = CFRunLoopGetCurrent();   /* (before the body can hand anything here) */
        ok = pthread_create(&t, &attr, body_thread, &a) == 0;
        pthread_attr_destroy(&attr);
    }
    if (!ok) {
        s_loop = NULL;
        fprintf(stderr, "[plat] no thread for the program: it runs on the first thread, and opens no window\n");
        return body(argc, argv);
    }
    for (;;) {
        pthread_mutex_lock(&s_lock);
        if (s_jobs_n) {
            void (*const fn)(void*) = s_jobs[0].fn;
            void* const arg = s_jobs[0].arg;
            s_jobs_n--;
            memmove(&s_jobs[0], &s_jobs[1], (size_t)s_jobs_n * sizeof s_jobs[0]);
            pthread_mutex_unlock(&s_lock);
            fn(arg);
            continue;
        }
        pthread_mutex_unlock(&s_lock);
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
    }
}

int plat_on_first_thread(void (*fn)(void*), void* arg)
{
    const CFRunLoopRef loop = s_loop;
    if (!loop || !fn) return 0;
    pthread_mutex_lock(&s_lock);
    const int room = s_jobs_n < FIRST_JOBS;
    if (room) {
        s_jobs[s_jobs_n].fn = fn;
        s_jobs[s_jobs_n].arg = arg;
        s_jobs_n++;
    }
    pthread_mutex_unlock(&s_lock);
    if (room) CFRunLoopStop(loop);   /* (its wait in the run loop ends now, not at the next quarter second) */
    return room;
}

#else
typedef int plat_first_thread_apple_off;   /* (a file with no declaration is not ISO C) */
#endif /* __APPLE__ */
