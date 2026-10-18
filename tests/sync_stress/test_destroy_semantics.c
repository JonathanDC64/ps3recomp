/*
 * lv2 mutex/cond destroy and trylock results, checked against RPCS3's
 * lv2 code as oracle:
 *   mutex_busy  sys_mutex_destroy of an owned mutex is EBUSY, for the owner
 *               and for anyone else; afterwards the id is gone (ESRCH).
 *   mutex_conds sys_mutex_destroy while a cond is bound to the mutex is
 *               EPERM; a cond cannot be made on a destroyed mutex (ESRCH).
 *   trylock     the owner's trylock of a non-recursive mutex is EDEADLK and
 *               takes no second count; a recursive one counts up.
 *   relock      a cond waiter that has left its cond but not yet taken the
 *               mutex back still owns it: sys_mutex_destroy is EBUSY.
 * Windows only, like test_cond_semantics: the waiter is a host thread that
 * the test parks, and for relock suspends, at a chosen point.
 */
#include "runtime/platform/win32_compat.h"
#include "runtime/syscalls/sys_cond.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
static int failures;
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
typedef struct { uint32_t mid, cid; int32_t rc; volatile LONG done; } waiter_args;

static DWORD WINAPI waiter(void* raw)
{
    waiter_args* a = (waiter_args*)raw;
    ppu_context c = {0}; c.thread_id = 7;
    if (call(sys_mutex_lock, &c, a->mid, 0, 0) == CELL_OK) {
        a->rc = call(sys_cond_wait, &c, a->cid, 0, 0);
        call(sys_mutex_unlock, &c, a->mid, 0, 0);
    }
    InterlockedExchange(&a->done, 1);
    return 0;
}

/* Wait until the waiter is parked: its record is on the cond and it has let
 * go of the cond's lock, which it holds from entry until it sleeps. */
static void wait_parked(sys_cond_info* ci)
{
    for (int i = 0; i < 5000 && !*(struct sys_cond_waiter* volatile*)&ci->waiters; i++) Sleep(1);
    CHECK(ci->waiters != NULL);
    EnterCriticalSection(&ci->signal_lock);
    LeaveCriticalSection(&ci->signal_lock);
}

static void mode_relock(void)
{
    ppu_context c1 = {0}; c1.thread_id = 1;
    uint32_t mid = make_mutex(&c1, 0, 0x200);
    uint32_t cid = make_cond(&c1, mid, 0x204);
    sys_cond_info* ci = &g_sys_conds[cid - 1];
    sys_mutex_info* mi = &g_sys_mutexes[mid - 1];
    waiter_args a = {mid, cid, -1, 0};
    HANDLE h = CreateThread(NULL, 0, waiter, &a, 0, NULL);
    CHECK(h != NULL);
    wait_parked(ci);

    /* Signal while holding the mutex: the waiter can leave the cond but not
     * get the mutex back. Freeze it there, out of the cond's lock. */
    CHECK(call(sys_mutex_lock, &c1, mid, 0, 0) == CELL_OK);
    CHECK(call(sys_cond_signal, &c1, cid, 0, 0) == CELL_OK);
    for (int i = 0; i < 5000 && !mi->relock_pending; i++) Sleep(1);
    CHECK(mi->relock_pending == 1);
    EnterCriticalSection(&ci->signal_lock);
    LeaveCriticalSection(&ci->signal_lock);
    CHECK(SuspendThread(h) != (DWORD)-1);

    /* Nobody holds the mutex now, but the waiter is owed it. */
    CHECK(call(sys_mutex_unlock, &c1, mid, 0, 0) == CELL_OK);
    CHECK(call(sys_cond_destroy, &c1, cid, 0, 0) == CELL_OK);
    CHECK(call(sys_mutex_destroy, &c1, mid, 0, 0) == (int32_t)CELL_EBUSY);

    CHECK(ResumeThread(h) != (DWORD)-1);
    CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
    CloseHandle(h);
    CHECK(a.done && a.rc == CELL_OK);
    CHECK(mi->relock_pending == 0);
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
    else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }
    printf("destroy %s: %s\n", mode, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
