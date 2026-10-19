/*
 * lv2 mutex/cond destroy and trylock results, checked against RPCS3's
 * lv2 code as oracle:
 *   mutex_busy  sys_mutex_destroy of an owned mutex is EBUSY, for the owner
 *               and for anyone else; afterwards the id is gone (ESRCH).
 *   mutex_conds sys_mutex_destroy while a cond is bound to the mutex is
 *               EPERM; a cond cannot be made on a destroyed mutex (ESRCH).
 *   trylock     the owner's trylock of a non-recursive mutex is EDEADLK and
 *               takes no second count; a recursive one counts up.
 *   relock      a signalled cond waiter that has not yet taken the mutex
 *               back still owns it: sys_mutex_destroy is EBUSY.
 *   signal      a signal takes the woken thread off the cond, as lv2's
 *               does: sys_cond_destroy is EBUSY only while a thread is
 *               still waiting, and succeeds right after the last signal.
 *   timeout     a timed-out waiter leaves the cond and the mutex's count
 *               as it found them.
 * Windows only, like test_cond_semantics: the waiters are host threads that
 * the test parks, and for relock freezes, at a chosen point.
 */
#include "runtime/platform/win32_compat.h"
#include "runtime/syscalls/sys_cond.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
static int failures;

/* sys_cond.c's wait record, completed here to count a cond's waiters. */
struct sys_cond_waiter { struct sys_cond_waiter* next; int signalled; };
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static int32_t call(int64_t (*f)(ppu_context*), ppu_context* c, uint64_t a, uint64_t b, uint64_t d)
{
    c->gpr[3] = a; c->gpr[4] = b; c->gpr[5] = d;
    return (int32_t)f(c);
}

static uint32_t be_read(unsigned off)
{
    const uint8_t* p = vm_base + off;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Attributes at 0x100 (non-recursive) and 0x140 (recursive); ids from 0x200. */
static uint32_t make_mutex(ppu_context* c, int recursive, unsigned out)
{
    unsigned attr = recursive ? 0x140 : 0x100;
    vm_base[attr + 3] = SYS_SYNC_FIFO;
    vm_base[attr + 7] = recursive ? SYS_SYNC_RECURSIVE : SYS_SYNC_NOT_RECURSIVE;
    CHECK(call(sys_mutex_create, c, out, attr, 0) == CELL_OK);
    return be_read(out);
}

static uint32_t make_cond(ppu_context* c, uint32_t mid, unsigned out)
{
    CHECK(call(sys_cond_create, c, out, mid, 0) == CELL_OK);
    return be_read(out);
}

/* One syscall from another host thread: a guest thread is a host thread, and
 * the host lock is recursive per host thread. */
typedef struct { int64_t (*f)(ppu_context*); uint32_t a; int32_t rc; } op_args;

static DWORD WINAPI op_thread(void* raw)
{
    op_args* o = (op_args*)raw;
    ppu_context c = {0}; c.thread_id = 2;
    o->rc = call(o->f, &c, o->a, 0, 0);
    return 0;
}

static int32_t other_thread(int64_t (*f)(ppu_context*), uint32_t a)
{
    op_args o = {f, a, -1};
    HANDLE h = CreateThread(NULL, 0, op_thread, &o, 0, NULL);
    CHECK(h != NULL && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
    CloseHandle(h);
    return o.rc;
}

/* Another guest thread that holds a mutex until told to let go. */
typedef struct { uint32_t mid; HANDLE held, release; } holder_args;

static DWORD WINAPI holder(void* raw)
{
    holder_args* a = (holder_args*)raw;
    ppu_context c = {0}; c.thread_id = 3;
    CHECK(call(sys_mutex_lock, &c, a->mid, 0, 0) == CELL_OK);
    SetEvent(a->held);
    WaitForSingleObject(a->release, 5000);
    CHECK(call(sys_mutex_unlock, &c, a->mid, 0, 0) == CELL_OK);
    return 0;
}

static void mode_mutex_busy(void)
{
    ppu_context c1 = {0}; c1.thread_id = 1;
    uint32_t mid = make_mutex(&c1, 0, 0x200);
    CHECK(call(sys_mutex_lock, &c1, mid, 0, 0) == CELL_OK);
    CHECK(call(sys_mutex_destroy, &c1, mid, 0, 0) == (int32_t)CELL_EBUSY);
    CHECK(other_thread(sys_mutex_destroy, mid) == (int32_t)CELL_EBUSY);
    CHECK(g_sys_mutexes[mid - 1].owner_tid == 1 && g_sys_mutexes[mid - 1].lock_count == 1);
    CHECK(call(sys_mutex_unlock, &c1, mid, 0, 0) == CELL_OK);
    CHECK(other_thread(sys_mutex_destroy, mid) == CELL_OK);
    CHECK(call(sys_mutex_lock, &c1, mid, 0, 0) == (int32_t)CELL_ESRCH);
    CHECK(call(sys_mutex_destroy, &c1, mid, 0, 0) == (int32_t)CELL_ESRCH);

    /* The slot's next mutex works: its lock was kept, not made again. */
    uint32_t again = make_mutex(&c1, 0, 0x204);
    CHECK(again == mid);
    holder_args h = {again, CreateEventA(NULL, TRUE, FALSE, NULL), CreateEventA(NULL, TRUE, FALSE, NULL)};
    HANDLE t = CreateThread(NULL, 0, holder, &h, 0, NULL);
    CHECK(t != NULL && WaitForSingleObject(h.held, 5000) == WAIT_OBJECT_0);
    CHECK(call(sys_mutex_trylock, &c1, again, 0, 0) == (int32_t)CELL_EBUSY);
    CHECK(call(sys_mutex_destroy, &c1, again, 0, 0) == (int32_t)CELL_EBUSY);
    SetEvent(h.release);
    CHECK(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0);
    CloseHandle(t); CloseHandle(h.held); CloseHandle(h.release);
    CHECK(call(sys_mutex_destroy, &c1, again, 0, 0) == CELL_OK);
}

static void mode_mutex_conds(void)
{
    ppu_context c1 = {0}; c1.thread_id = 1;
    uint32_t mid = make_mutex(&c1, 0, 0x200);
    uint32_t cid1 = make_cond(&c1, mid, 0x204);
    uint32_t cid2 = make_cond(&c1, mid, 0x208);
    CHECK(call(sys_mutex_destroy, &c1, mid, 0, 0) == (int32_t)CELL_EPERM);
    CHECK(call(sys_cond_destroy, &c1, cid1, 0, 0) == CELL_OK);
    CHECK(call(sys_mutex_destroy, &c1, mid, 0, 0) == (int32_t)CELL_EPERM);
    CHECK(call(sys_cond_destroy, &c1, cid2, 0, 0) == CELL_OK);
    CHECK(call(sys_cond_destroy, &c1, cid2, 0, 0) == (int32_t)CELL_ESRCH);
    CHECK(call(sys_mutex_destroy, &c1, mid, 0, 0) == CELL_OK);
    CHECK(call(sys_cond_create, &c1, 0x20C, mid, 0) == (int32_t)CELL_ESRCH);
}

static void mode_trylock(void)
{
    ppu_context c1 = {0}; c1.thread_id = 1;
    uint32_t plain = make_mutex(&c1, 0, 0x200);
    CHECK(call(sys_mutex_lock, &c1, plain, 0, 0) == CELL_OK);
    CHECK(call(sys_mutex_trylock, &c1, plain, 0, 0) == (int32_t)CELL_EDEADLK);
    CHECK(g_sys_mutexes[plain - 1].lock_count == 1);
    CHECK(call(sys_mutex_unlock, &c1, plain, 0, 0) == CELL_OK);
    CHECK(call(sys_mutex_unlock, &c1, plain, 0, 0) == (int32_t)CELL_EPERM);

    uint32_t rec = make_mutex(&c1, 1, 0x204);
    CHECK(call(sys_mutex_lock, &c1, rec, 0, 0) == CELL_OK);
    CHECK(call(sys_mutex_trylock, &c1, rec, 0, 0) == CELL_OK);
    CHECK(g_sys_mutexes[rec - 1].lock_count == 2);
    CHECK(call(sys_mutex_unlock, &c1, rec, 0, 0) == CELL_OK);
    CHECK(call(sys_mutex_unlock, &c1, rec, 0, 0) == CELL_OK);
}

/* A guest thread that waits on the cond once and records the result. */
typedef struct { uint32_t mid, cid; uint64_t tid, timeout; int32_t rc; volatile LONG done; HANDLE h; } waiter_args;

static DWORD WINAPI waiter(void* raw)
{
    waiter_args* a = (waiter_args*)raw;
    ppu_context c = {0}; c.thread_id = a->tid;
    if (call(sys_mutex_lock, &c, a->mid, 0, 0) == CELL_OK) {
        a->rc = call(sys_cond_wait, &c, a->cid, a->timeout, 0);
        call(sys_mutex_unlock, &c, a->mid, 0, 0);
    }
    InterlockedExchange(&a->done, 1);
    return 0;
}

static void start_waiter(waiter_args* a, uint32_t mid, uint32_t cid, uint64_t tid, uint64_t timeout)
{
    waiter_args init = {mid, cid, tid, timeout, -1, 0, NULL};
    *a = init;
    a->h = CreateThread(NULL, 0, waiter, a, 0, NULL);
    CHECK(a->h != NULL);
}

static int32_t join_waiter(waiter_args* a)
{
    CHECK(WaitForSingleObject(a->h, 5000) == WAIT_OBJECT_0);
    CloseHandle(a->h);
    CHECK(a->done);
    return a->rc;
}

/* Wait until n waiters are parked: their records are on the cond and they
 * have let go of the cond's lock, which a waiter holds from entry until it
 * sleeps. */
static void wait_parked(sys_cond_info* ci, int n)
{
    int count = 0;
    for (int i = 0; i < 5000; i++) {
        EnterCriticalSection(&ci->signal_lock);
        count = 0;
        for (struct sys_cond_waiter* w = ci->waiters; w; w = w->next) count++;
        LeaveCriticalSection(&ci->signal_lock);
        if (count >= n) break;
        Sleep(1);
    }
    CHECK(count == n);
}

/* SuspendThread returns before the thread has stopped; GetThreadContext
 * waits until it has. */
static void freeze(HANDLE h)
{
    CHECK(SuspendThread(h) != (DWORD)-1);
    CONTEXT tc;
    memset(&tc, 0, sizeof(tc));
    tc.ContextFlags = CONTEXT_CONTROL;
    CHECK(GetThreadContext(h, &tc));
}

static void mode_relock(void)
{
    ppu_context c1 = {0}; c1.thread_id = 1;
    uint32_t mid = make_mutex(&c1, 0, 0x200);
    uint32_t cid = make_cond(&c1, mid, 0x204);
    sys_cond_info* ci = &g_sys_conds[cid - 1];
    sys_mutex_info* mi = &g_sys_mutexes[mid - 1];
    waiter_args a;
    start_waiter(&a, mid, cid, 7, 0);
    wait_parked(ci, 1);

    /* Signal while holding the cond's lock (recursive, so the syscalls
     * still take it): the woken waiter cannot get back out of its wait.
     * Freeze it there, then let go of everything. */
    EnterCriticalSection(&ci->signal_lock);
    CHECK(call(sys_mutex_lock, &c1, mid, 0, 0) == CELL_OK);
    CHECK(call(sys_cond_signal, &c1, cid, 0, 0) == CELL_OK);
    CHECK(mi->relock_pending == 1);
    freeze(a.h);
    LeaveCriticalSection(&ci->signal_lock);

    /* Nobody holds the mutex now, but the waiter is owed it. */
    CHECK(call(sys_mutex_unlock, &c1, mid, 0, 0) == CELL_OK);
    CHECK(call(sys_cond_destroy, &c1, cid, 0, 0) == CELL_OK);
    CHECK(call(sys_mutex_destroy, &c1, mid, 0, 0) == (int32_t)CELL_EBUSY);

    CHECK(ResumeThread(a.h) != (DWORD)-1);
    CHECK(join_waiter(&a) == CELL_OK);
    CHECK(mi->relock_pending == 0);
    CHECK(call(sys_mutex_destroy, &c1, mid, 0, 0) == CELL_OK);
}

static void mode_signal(void)
{
    ppu_context c1 = {0}; c1.thread_id = 1;
    uint32_t mid = make_mutex(&c1, 0, 0x200);
    uint32_t cid = make_cond(&c1, mid, 0x204);
    sys_cond_info* ci = &g_sys_conds[cid - 1];
    sys_mutex_info* mi = &g_sys_mutexes[mid - 1];
    waiter_args a, b;
    start_waiter(&a, mid, cid, 7, 0);
    start_waiter(&b, mid, cid, 8, 0);
    wait_parked(ci, 2);

    /* Hold the cond's lock to keep the woken waiters from running until the
     * destroys are done. */
    EnterCriticalSection(&ci->signal_lock);
    CHECK(call(sys_mutex_lock, &c1, mid, 0, 0) == CELL_OK);
    CHECK(call(sys_cond_signal, &c1, cid, 0, 0) == CELL_OK);
    CHECK(mi->relock_pending == 1);
    CHECK(call(sys_cond_destroy, &c1, cid, 0, 0) == (int32_t)CELL_EBUSY);   /* one still waits */
    CHECK(call(sys_cond_signal_all, &c1, cid, 0, 0) == CELL_OK);
    CHECK(mi->relock_pending == 2);
    CHECK(call(sys_cond_destroy, &c1, cid, 0, 0) == CELL_OK);
    CHECK(call(sys_mutex_unlock, &c1, mid, 0, 0) == CELL_OK);
    LeaveCriticalSection(&ci->signal_lock);

    CHECK(join_waiter(&a) == CELL_OK);
    CHECK(join_waiter(&b) == CELL_OK);
    CHECK(mi->relock_pending == 0);
    CHECK(call(sys_mutex_destroy, &c1, mid, 0, 0) == CELL_OK);
}

static void mode_timeout(void)
{
    ppu_context c1 = {0}; c1.thread_id = 1;
    uint32_t mid = make_mutex(&c1, 0, 0x200);
    uint32_t cid = make_cond(&c1, mid, 0x204);
    sys_mutex_info* mi = &g_sys_mutexes[mid - 1];
    waiter_args a;
    start_waiter(&a, mid, cid, 7, 1000);
    CHECK(join_waiter(&a) == (int32_t)CELL_ETIMEDOUT);
    CHECK(g_sys_conds[cid - 1].waiters == NULL);
    CHECK(mi->relock_pending == 0 && mi->lock_count == 0);
    CHECK(call(sys_cond_destroy, &c1, cid, 0, 0) == CELL_OK);
    CHECK(call(sys_mutex_destroy, &c1, mid, 0, 0) == CELL_OK);
}

int main(int argc, char** argv)
{
    vm_base = calloc(1, 65536);
    const char* mode = argc > 1 ? argv[1] : "mutex_busy";
    if (!strcmp(mode, "mutex_busy"))       mode_mutex_busy();
    else if (!strcmp(mode, "mutex_conds")) mode_mutex_conds();
    else if (!strcmp(mode, "trylock"))     mode_trylock();
    else if (!strcmp(mode, "relock"))      mode_relock();
    else if (!strcmp(mode, "signal"))      mode_signal();
    else if (!strcmp(mode, "timeout"))     mode_timeout();
    else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }
    printf("destroy %s: %s\n", mode, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
