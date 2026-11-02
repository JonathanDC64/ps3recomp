/* spu_async_pool.c -- host threads for async SPU jobs (see spu_async_pool.h).
 *
 * Every async dispatch used to create a host thread and let it exit with the
 * job. Demon's Souls runs ~650 such jobs a second (Havok image 7, image 10,
 * images 3-6), so that was ~650 thread starts a second on the dispatching PPU
 * threads, a fresh stack to fault in for each job's 260 KB stack-local
 * spu_context, and every job starting on cold caches.
 *
 * A finished worker now parks on its own event and the next dispatch hands it
 * a job. There is no queue and no cap: a dispatch with no idle worker creates
 * one, so a job never waits for another to finish (tasks block in WAIT_SIGNAL
 * on each other). Idle workers exit after 5 s, or at once beyond 64 idle.
 * SPU_ASYNC_POOL=0 (live) creates a thread per job again.
 *
 * Written once on plat.h. Windows had this alone until it moved here; Linux
 * and macOS created a thread per job.
 */
#include "spu_async_pool.h"
#include "ps3emu/ps3_knob.h"
#include "../platform/plat.h"
#include <stdlib.h>

/* A test builds this file with shorter limits. */
#ifndef SPU_ASYNC_POOL_MAX_IDLE
#define SPU_ASYNC_POOL_MAX_IDLE 64
#endif
#ifndef SPU_ASYNC_POOL_IDLE_MS
#define SPU_ASYNC_POOL_IDLE_MS  5000
#endif

/* 256 MB of address space, committed as it is touched: the Bink SPU decoder's
 * per-macroblock brsl chains are deep host call chains with fat lifted frames,
 * and a 1 MB stack blew the guard page (STATUS_GUARD_PAGE_VIOLATION
 * 0x80000001, silent process death) the moment the real decode path first
 * executed. The default is 512 KB on Darwin. */
#define SPU_ASYNC_STACK     (256u << 20)
#define SPU_ASYNC_GUARANTEE (256u * 1024)

typedef struct spu_async_worker {
    plat_event ev;                    /* auto-reset: a job was handed over */
    spu_async_pool_fn fn;
    void* arg;
    int own;                          /* own thread: exit after the job */
    struct spu_async_worker* next;    /* idle list */
} spu_async_worker;
static plat_mutex s_aw_lock = PLAT_MUTEX_INIT;
static spu_async_worker* s_aw_idle;
static int s_aw_idle_n;

/* Take an idle worker off the list, or NULL. */
static spu_async_worker* spu_aw_take(void)
{
    plat_mutex_lock(&s_aw_lock);
    spu_async_worker* w = s_aw_idle;
    if (w) { s_aw_idle = w->next; s_aw_idle_n--; }
    plat_mutex_unlock(&s_aw_lock);
    return w;
}

/* Park a worker that finished its job. Returns 0 if it should exit instead. */
static int spu_aw_park(spu_async_worker* w)
{
    plat_mutex_lock(&s_aw_lock);
    if (s_aw_idle_n >= SPU_ASYNC_POOL_MAX_IDLE) { plat_mutex_unlock(&s_aw_lock); return 0; }
    w->next = s_aw_idle; s_aw_idle = w; s_aw_idle_n++;
    plat_mutex_unlock(&s_aw_lock);
    if (plat_event_wait(&w->ev, SPU_ASYNC_POOL_IDLE_MS)) return 1;
    /* Timed out: leave the list, unless a dispatcher took us off it in the
     * meantime -- then its job is on the way and the event will fire. */
    int removed = 0;
    plat_mutex_lock(&s_aw_lock);
    for (spu_async_worker** pp = &s_aw_idle; *pp; pp = &(*pp)->next)
        if (*pp == w) { *pp = w->next; s_aw_idle_n--; removed = 1; break; }
    plat_mutex_unlock(&s_aw_lock);
    if (removed) return 0;
    plat_event_wait(&w->ev, PLAT_WAIT_FOREVER);
    return 1;
}

/* The worker. Its name is what tools and the debug console's thread list show
 * for these threads (they name a thread by its start address). */
static plat_thread_ret PLAT_THREAD_CALL spu_async_thread(void* p)
{
    /* Reserve handler headroom so a stack overflow in lifted SPU code (e.g. a
     * runaway brsl recursion) can actually reach the STACKOVERFLOW reporter
     * instead of killing the process silently with 0x80000001. */
    plat_thread_stack_guarantee(SPU_ASYNC_GUARANTEE);
    spu_async_worker* w = (spu_async_worker*)p;
    for (;;) {
        spu_async_pool_fn fn = w->fn;
        void* arg = w->arg;
        w->fn = NULL;
        fn(arg);
        if (w->own || !spu_aw_park(w)) break;
    }
    plat_event_destroy(&w->ev);
    free(w);
    return 0;
}

static int spu_async_pool_on(void)
{
    static int s_on = -1;
    PS3_KNOB(s_on, ps3_env_int("SPU_ASYNC_POOL", 1));
    return s_on;
}

int spu_async_pool_start(spu_async_pool_fn fn, void* arg, int own_thread)
{
    spu_async_worker* w = (!own_thread && spu_async_pool_on()) ? spu_aw_take() : NULL;
    if (w) { w->fn = fn; w->arg = arg; plat_event_set(&w->ev); return 1; }
    w = (spu_async_worker*)calloc(1, sizeof *w);
    if (!w) return 0;
    if (!plat_event_init(&w->ev)) { free(w); return 0; }
    w->fn = fn;
    w->arg = arg;
    w->own = own_thread;
    if (!plat_thread_start_detached(spu_async_thread, w, SPU_ASYNC_STACK)) {
        plat_event_destroy(&w->ev);
        free(w);
        return 0;
    }
    return 1;
}

int spu_async_pool_idle(void)
{
    plat_mutex_lock(&s_aw_lock);
    const int n = s_aw_idle_n;
    plat_mutex_unlock(&s_aw_lock);
    return n;
}
