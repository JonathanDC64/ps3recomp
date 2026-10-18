/*
 * ps3recomp - Kernel mutex syscalls (implementation)
 */

#include "sys_mutex.h"
#include "../platform/darwin_compat.h"
#include "sys_timer.h"   /* lv2_usec_deadline: sub-ms timed waits */
#include "../memory/vm.h"
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_mutex_info g_sys_mutexes[SYS_MUTEX_MAX];

#ifdef _WIN32
static CRITICAL_SECTION s_mtx_table_lock;
static int              s_mtx_table_lock_init = 0;
#else
static pthread_mutex_t  s_mtx_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void mtx_table_lock(void)
{
#ifdef _WIN32
    if (!s_mtx_table_lock_init) {
        InitializeCriticalSection(&s_mtx_table_lock);
        s_mtx_table_lock_init = 1;
    }
    EnterCriticalSection(&s_mtx_table_lock);
#else
    pthread_mutex_lock(&s_mtx_table_lock);
#endif
}

static void mtx_table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_mtx_table_lock);
#else
    pthread_mutex_unlock(&s_mtx_table_lock);
#endif
}

static int mtx_find_free(void)
{
    for (int i = 0; i < SYS_MUTEX_MAX; i++) {
        if (!g_sys_mutexes[i].active)
            return i;
    }
    return -1;
}

/* The host lock without waiting: 1 if this thread now holds it (also when it
 * already did, on a recursive host lock), 0 if another thread holds it. */
static int host_trylock(sys_mutex_info* m)
{
#ifdef _WIN32
    return TryEnterCriticalSection(&m->cs) ? 1 : 0;
#else
    return pthread_mutex_trylock(&m->mtx) == 0;
#endif
}

static void host_unlock(sys_mutex_info* m)
{
#ifdef _WIN32
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->mtx);
#endif
}

/* Byteswap helper for writing u32 to guest memory */
static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * sys_mutex_create
 *
 * r3 = pointer to receive mutex ID (u32*)
 * r4 = pointer to attribute struct
 * -----------------------------------------------------------------------*/
int64_t sys_mutex_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr   = LV2_ARG_PTR(ctx, 1);

    mtx_table_lock();

    int slot = mtx_find_free();
    if (slot < 0) {
        mtx_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_mutex_info* m = &g_sys_mutexes[slot];
    /* No memset: on Windows the CRITICAL_SECTION is made once per slot and
     * kept. A thread that read this slot's id just before the last destroy
     * can still be on its way into the lock; it must find a valid one, and
     * sys_mutex_lock then sees the slot inactive (or the new mutex). */
    memset(m->name, 0, sizeof(m->name));
    m->protocol = 0;
    m->recursive = 0;
    m->owner_tid = 0;
    m->lock_count = 0;
    m->cond_count = 0;
    m->relock_pending = 0;

    /* Read attributes from guest memory */
    if (attr_addr != 0) {
        uint8_t* attr_raw = (uint8_t*)vm_to_host(attr_addr);
        /* protocol is at offset 0 (u32 BE) */
        uint32_t proto_be;
        memcpy(&proto_be, attr_raw + 0, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        proto_be = ((proto_be >> 24) & 0xFF) | ((proto_be >> 8) & 0xFF00) |
                   ((proto_be << 8) & 0xFF0000) | ((proto_be << 24) & 0xFF000000u);
#endif
        m->protocol = proto_be;

        /* recursive flag at offset 4 (u32 BE) */
        uint32_t rec_be;
        memcpy(&rec_be, attr_raw + 4, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        rec_be = ((rec_be >> 24) & 0xFF) | ((rec_be >> 8) & 0xFF00) |
                 ((rec_be << 8) & 0xFF0000) | ((rec_be << 24) & 0xFF000000u);
#endif
        m->recursive = (rec_be == SYS_SYNC_RECURSIVE) ? 1 : 0;

        /* name at offset 24 */
        memcpy(m->name, attr_raw + 24, 8);
    }

    /* Initialize host mutex */
#ifdef _WIN32
    if (!m->initialized) {
        InitializeCriticalSection(&m->cs);
        m->initialized = 1;
    }
    m->wait_handle = CreateEventA(NULL, FALSE, TRUE, NULL);
#else
    {
        pthread_mutexattr_t mattr;
        pthread_mutexattr_init(&mattr);
        if (m->recursive) {
            pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
        } else {
            pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_ERRORCHECK);
        }
        pthread_mutex_init(&m->mtx, &mattr);
        pthread_mutexattr_destroy(&mattr);
    }
#endif
    m->active = 1;   /* after the host lock is ready */

    /* Write mutex ID (slot + 1) to guest */
    uint32_t mutex_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, mutex_id);
    }

    mtx_table_unlock();
    { extern void sync_hist(char, uint32_t, const ppu_context*, uint32_t, int32_t);
      sync_hist('M', mutex_id, ctx, id_out_addr, 0); }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mutex_destroy
 *
 * r3 = mutex_id
 * -----------------------------------------------------------------------*/
int64_t sys_mutex_destroy(ppu_context* ctx)
{
    uint32_t mutex_id = LV2_ARG_U32(ctx, 0);

    if (mutex_id == 0 || mutex_id > SYS_MUTEX_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    mtx_table_lock();

    sys_mutex_info* m = &g_sys_mutexes[mutex_id - 1];
    if (!m->active) {
        mtx_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    /* lv2 refuses while the mutex has an owner (EBUSY), then while a cond is
     * still created on it (EPERM) -- RPCS3's sys_mutex_destroy, as oracle,
     * checks in that order. Destroying anyway deleted the CRITICAL_SECTION
     * under its holder and its waiters (undefined behaviour) and left the
     * conds pointing at a dead or reused id.
     * Owner test: the host lock cannot be taken while another thread holds
     * it, and the caller's own hold shows in lock_count. A cond waiter on
     * its way back to the mutex (relock_pending) is an owner too: lv2 gives
     * it the mutex, or queues it for it, in the same step that ends the
     * wait. */
    int32_t result = CELL_OK;
    if (!host_trylock(m)) {
        result = (int32_t)CELL_EBUSY;
    } else {
        if (m->lock_count > 0 || m->relock_pending > 0)
            result = (int32_t)CELL_EBUSY;
        else if (m->cond_count > 0)
            result = (int32_t)CELL_EPERM;
        else
            m->active = 0;
        host_unlock(m);
    }

    if (result == CELL_OK) {
#ifdef _WIN32
        /* The CRITICAL_SECTION stays for the slot's next mutex (see create). */
        if (m->wait_handle) { CloseHandle(m->wait_handle); m->wait_handle = NULL; }
#else
        pthread_mutex_destroy(&m->mtx);
#endif
    }

    mtx_table_unlock();
    { extern void sync_hist(char, uint32_t, const ppu_context*, uint32_t, int32_t);
      sync_hist('X', mutex_id, ctx, (uint32_t)m->owner_tid,
                result == CELL_OK ? m->lock_count : result); }
    return result;
}

/* ---------------------------------------------------------------------------
 * sys_mutex_lock
 *
 * r3 = mutex_id
 * r4 = timeout_usec (0 = infinite)
 * -----------------------------------------------------------------------*/
int64_t sys_mutex_lock(ppu_context* ctx)
{
    uint32_t mutex_id    = LV2_ARG_U32(ctx, 0);
    uint64_t timeout_us  = LV2_ARG_U64(ctx, 1);
    if (mutex_id == 0 || mutex_id > SYS_MUTEX_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mutex_info* m = &g_sys_mutexes[mutex_id - 1];
    if (!m->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    uint64_t caller_tid = ctx->thread_id;

    /* Check for deadlock on non-recursive mutex */
    if (!m->recursive && m->owner_tid == caller_tid && m->lock_count > 0) {
        return (int64_t)(int32_t)CELL_EDEADLK;
    }

    { extern void thrdiag_wait(const char*, uint32_t); thrdiag_wait("mutex", mutex_id); }
#ifdef _WIN32
    if (timeout_us == 0) {
        /* Infinite wait */
        EnterCriticalSection(&m->cs);
        { extern void thrdiag_wake(void); thrdiag_wake(); }
    } else {
        /* Timed lock via TryEnterCriticalSection to a QPC deadline. The old
         * GetTickCount/Sleep(1) loop measured elapsed time in ~15.6 ms ticks
         * and slept a full tick per retry, overshooting sub-ms timeouts by
         * orders of magnitude. Yield (not Sleep) for sub-ms timeouts so the
         * holder can run; Sleep(1) only for waits long enough to absorb it.
         * Safe to poll: TryEnterCriticalSection's success IS the atomic
         * acquire, so there's no separate signal to miss between polls. */
        int64_t deadline = lv2_usec_deadline(timeout_us);
        while (!TryEnterCriticalSection(&m->cs)) {
            if (lv2_deadline_passed(deadline)) {
                return (int64_t)(int32_t)CELL_ETIMEDOUT;
            }
            if (timeout_us < 1000) SwitchToThread();
            else                   Sleep(1);
        }
    }
#else
    if (timeout_us == 0) {
        pthread_mutex_lock(&m->mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeout_us / 1000000);
        ts.tv_nsec += (long)((timeout_us % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        int rc = pthread_mutex_timedlock(&m->mtx, &ts);
        if (rc == ETIMEDOUT) {
            return (int64_t)(int32_t)CELL_ETIMEDOUT;
        }
    }
#endif

    if (!m->active) {
        /* Destroyed while this thread was on its way in: destroy only takes
         * a free lock, so it can win the race to a lock just released. lv2
         * answers ESRCH for an id that is gone. */
        host_unlock(m);
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    m->owner_tid = caller_tid;
    m->lock_count++;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mutex_trylock
 *
 * r3 = mutex_id
 * -----------------------------------------------------------------------*/
int64_t sys_mutex_trylock(ppu_context* ctx)
{
    uint32_t mutex_id = LV2_ARG_U32(ctx, 0);

    if (mutex_id == 0 || mutex_id > SYS_MUTEX_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mutex_info* m = &g_sys_mutexes[mutex_id - 1];
    if (!m->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    uint64_t caller_tid = ctx->thread_id;

    /* The owner trying a non-recursive mutex again: lv2 answers EDEADLK, as
     * sys_mutex_lock does above. The host lock cannot say so: a
     * CRITICAL_SECTION is recursive, so TryEnter succeeded and the guest got
     * CELL_OK and a second count (and the POSIX error-checking mutex gave
     * EBUSY). */
    if (!m->recursive && m->owner_tid == caller_tid && m->lock_count > 0) {
        return (int64_t)(int32_t)CELL_EDEADLK;
    }

    /* Recursive re-entry. MUST still acquire the host primitive: unlock
     * calls LeaveCriticalSection unconditionally per guest unlock, so every
     * guest lock (including a recursive re-entry) must Enter exactly once
     * or the Enter/Leave pairing breaks: the paired unlock releases the CS
     * while the guest logically still holds the mutex, and the final
     * unlock calls Leave on a non-held CS (undefined behavior, corrupts
     * the CS and causes random forever-blocks on later Enters).
     * EnterCriticalSection by the current owner is itself recursive and
     * never blocks, so this is safe and cheap. */
    if (m->recursive && m->owner_tid == caller_tid && m->lock_count > 0) {
#ifdef _WIN32
        EnterCriticalSection(&m->cs);
#else
        /* Same reasoning, and it applies here: create() gives a recursive
         * guest mutex a PTHREAD_MUTEX_RECURSIVE host mutex, so the owner
         * re-locking it bumps the host recursion count and never blocks.
         * The comment this replaces claimed the host mutex was the default
         * non-recursive kind and skipped the lock, which left one host
         * acquisition behind two guest ones: the first guest unlock then
         * dropped the host mutex entirely and another thread could enter
         * while the guest still believed it held the mutex. */
        pthread_mutex_lock(&m->mtx);
#endif
        m->lock_count++;
        return CELL_OK;
    }

#ifdef _WIN32
    if (!TryEnterCriticalSection(&m->cs)) {
        return (int64_t)(int32_t)CELL_EBUSY;
    }
#else
    int rc = pthread_mutex_trylock(&m->mtx);
    if (rc != 0) {
        return (int64_t)(int32_t)CELL_EBUSY;
    }
#endif

    if (!m->active) {   /* destroyed meanwhile, as in sys_mutex_lock */
        host_unlock(m);
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    m->owner_tid = caller_tid;
    m->lock_count++;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Conds bound to a mutex (sys_cond.c)
 * -----------------------------------------------------------------------*/
int sys_mutex_bind_cond(uint32_t mutex_id)
{
    if (mutex_id == 0 || mutex_id > SYS_MUTEX_MAX)
        return 0;
    mtx_table_lock();
    sys_mutex_info* m = &g_sys_mutexes[mutex_id - 1];
    int ok = m->active;
    if (ok)
        m->cond_count++;
    mtx_table_unlock();
    return ok;
}

void sys_mutex_unbind_cond(uint32_t mutex_id)
{
    if (mutex_id == 0 || mutex_id > SYS_MUTEX_MAX)
        return;
    mtx_table_lock();
    sys_mutex_info* m = &g_sys_mutexes[mutex_id - 1];
    if (m->cond_count > 0)
        m->cond_count--;
    mtx_table_unlock();
}

/* ---------------------------------------------------------------------------
 * sys_mutex_unlock
 *
 * r3 = mutex_id
 * -----------------------------------------------------------------------*/
int64_t sys_mutex_unlock(ppu_context* ctx)
{
    uint32_t mutex_id = LV2_ARG_U32(ctx, 0);

    if (mutex_id == 0 || mutex_id > SYS_MUTEX_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mutex_info* m = &g_sys_mutexes[mutex_id - 1];
    if (!m->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    uint64_t caller_tid = ctx->thread_id;

    if (m->owner_tid != caller_tid) {
        /* Real lv2 returns EPERM for a non-owner unlock (RPCS3
         * sys_mutex.h's lv2_mutex::try_unlock, oracle, no code copied:
         * "if (it.owner != cpu.id) return CELL_EPERM;"), not a made up
         * CELL_EMUTEX_NOT_OWNED code that guest branches on this
         * contract would never match. */
        return (int64_t)(int32_t)CELL_EPERM;
    }

    m->lock_count--;
    if (m->lock_count == 0) {
        m->owner_tid = 0;
    }

#ifdef _WIN32
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->mtx);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_mutex_init(lv2_syscall_table* tbl)
{
    memset(g_sys_mutexes, 0, sizeof(g_sys_mutexes));

#ifdef _WIN32
    if (!s_mtx_table_lock_init) {
        InitializeCriticalSection(&s_mtx_table_lock);
        s_mtx_table_lock_init = 1;
    }
#endif

    lv2_syscall_register(tbl, SYS_MUTEX_CREATE,  sys_mutex_create);
    lv2_syscall_register(tbl, SYS_MUTEX_DESTROY, sys_mutex_destroy);
    lv2_syscall_register(tbl, SYS_MUTEX_LOCK,    sys_mutex_lock);
    lv2_syscall_register(tbl, SYS_MUTEX_TRYLOCK, sys_mutex_trylock);
    lv2_syscall_register(tbl, SYS_MUTEX_UNLOCK,  sys_mutex_unlock);
}
