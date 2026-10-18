/* ps3recomp - Kernel condition variables.
 *
 * The kernel commits a waiter before releasing its guest mutex. A host CV
 * alone does not retain a signal delivered between syscall entry and parking.
 * Keep explicit wait records under a separate lock; signals never need the
 * guest mutex, and a later waiter cannot steal an earlier waiter's wakeup.
 */
#include "sys_cond.h"
#include "ps3emu/ps3_knob.h"
#include "../memory/vm.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

struct sys_cond_waiter {
    struct sys_cond_waiter* next;
    int signalled;
};

sys_cond_info g_sys_conds[SYS_COND_MAX];
uint32_t g_cond_id_addr[SYS_COND_MAX + 1];

/* Sync history: a ring of the last 64K cond and mutex lifecycle events
 * (create/destroy) and cond waits and signals, with the guest thread, the
 * caller and the result. Always on -- one store per event -- and read by the
 * debug console's `synchist <id>` verb, which answers "who created, signalled
 * and destroyed the cond this thread is parked on" after the fact. Ids are
 * recycled, so read a cond's history from its last create forward. */
sync_hist_ent g_sync_hist[SYNC_HIST_N];
volatile long g_sync_hist_n;
extern unsigned long long ps3_ms_now(void);
void sync_hist(char op, uint32_t id, const ppu_context* ctx, uint32_t extra, int32_t res)
{
    const long i = __sync_add_and_fetch(&g_sync_hist_n, 1) - 1;
    sync_hist_ent* e = &g_sync_hist[(unsigned long)i & (SYNC_HIST_N - 1)];
    e->t_ms = (uint32_t)ps3_ms_now(); e->op = (uint8_t)op; e->id = id;
    e->tid = ctx ? (uint32_t)ctx->thread_id : 0; e->lr = ctx ? (uint32_t)ctx->lr : 0;
    e->extra = extra; e->res = res;
}

#ifdef _WIN32
static SRWLOCK s_table_lock = SRWLOCK_INIT;
static void table_lock(void) { AcquireSRWLockExclusive(&s_table_lock); }
static void table_unlock(void) { ReleaseSRWLockExclusive(&s_table_lock); }
static void cond_lock(sys_cond_info* c) { EnterCriticalSection(&c->signal_lock); }
static void cond_unlock(sys_cond_info* c) { LeaveCriticalSection(&c->signal_lock); }
static void cond_wake(sys_cond_info* c) { WakeAllConditionVariable(&c->cv); }
static void guest_lock(sys_mutex_info* m) { EnterCriticalSection(&m->cs); }
static void guest_unlock(sys_mutex_info* m) { LeaveCriticalSection(&m->cs); }
#else
static pthread_mutex_t s_table_lock = PTHREAD_MUTEX_INITIALIZER;
static void table_lock(void) { pthread_mutex_lock(&s_table_lock); }
static void table_unlock(void) { pthread_mutex_unlock(&s_table_lock); }
static void cond_lock(sys_cond_info* c) { pthread_mutex_lock(&c->signal_lock); }
static void cond_unlock(sys_cond_info* c) { pthread_mutex_unlock(&c->signal_lock); }
static void cond_wake(sys_cond_info* c) { pthread_cond_broadcast(&c->cv); }
static void guest_lock(sys_mutex_info* m) { pthread_mutex_lock(&m->mtx); }
static void guest_unlock(sys_mutex_info* m) { pthread_mutex_unlock(&m->mtx); }
#endif

/* Lock ordering is table -> condition. No caller holds the condition lock
 * while acquiring a guest mutex. Native locks survive slot recycling. */
static sys_cond_info* acquire_cond(uint32_t id)
{
    if (!id || id > SYS_COND_MAX) return NULL;
    table_lock();
    sys_cond_info* c = &g_sys_conds[id - 1];
    if (!c->active) { table_unlock(); return NULL; }
    cond_lock(c);
    table_unlock();
    return c;
}

int64_t sys_cond_create(ppu_context* ctx)
{
    uint32_t out = LV2_ARG_PTR(ctx, 0);
    uint32_t mid = LV2_ARG_U32(ctx, 1);
    uint32_t attr = LV2_ARG_PTR(ctx, 2);
    /* Check the mutex and count this cond on it in one step under the mutex
     * table lock: checking .active alone raced sys_mutex_destroy, which could
     * then leave a cond on a dead mutex (RPCS3 PR #8340 fixed the same race).
     * Every failure below must give the count back. */
    if (!sys_mutex_bind_cond(mid))
        return (int32_t)CELL_ESRCH;
    table_lock();
    for (unsigned i = 0; i < SYS_COND_MAX; ++i) {
        sys_cond_info* c = &g_sys_conds[i];
        if (c->active) continue;
        if (!c->initialized) {
#ifdef _WIN32
            InitializeCriticalSection(&c->signal_lock);
            InitializeConditionVariable(&c->cv);
#else
            int rc = pthread_mutex_init(&c->signal_lock, NULL);
            if (rc) { table_unlock(); sys_mutex_unbind_cond(mid); return (int32_t)CELL_EAGAIN; }
            rc = pthread_cond_init(&c->cv, NULL);
            if (rc) {
                pthread_mutex_destroy(&c->signal_lock);
                table_unlock(); sys_mutex_unbind_cond(mid); return (int32_t)CELL_EAGAIN;
            }
#endif
            c->initialized = 1;
        }
        c->mutex_id = mid;
        c->waiters = NULL;
        memset(c->name, 0, sizeof(c->name));
        if (attr) memcpy(c->name, (uint8_t*)vm_to_host(attr) + 8, 8);
        c->active = 1;
        g_cond_id_addr[i + 1] = out;
        if (out) {
            uint8_t* p = vm_to_host(out);
            uint32_t id = i + 1;
            p[0] = (uint8_t)(id >> 24); p[1] = (uint8_t)(id >> 16);
            p[2] = (uint8_t)(id >> 8); p[3] = (uint8_t)id;
        }
        sync_hist('C', i + 1, ctx, mid, (int32_t)out);
        if (getenv("PS3_SYNCLOG") || getenv("YDKJ_SYNCLOG")) {
            char name[9]; memcpy(name, c->name, 8); name[8] = 0;
            fprintf(stderr, "[SYNC] cond_create id=%u name='%s' mutex=%u id_at=0x%08X lr=0x%08X\n",
                    i + 1, name, mid, out, (uint32_t)ctx->lr);
        }
        table_unlock();
        return CELL_OK;
    }
    table_unlock();
    sys_mutex_unbind_cond(mid);
    return (int32_t)CELL_EAGAIN;
}

int64_t sys_cond_destroy(ppu_context* ctx)
{
    uint32_t id = LV2_ARG_U32(ctx, 0);
    if (!id || id > SYS_COND_MAX) return (int32_t)CELL_ESRCH;
    table_lock();
    sys_cond_info* c = &g_sys_conds[id - 1];
    if (!c->active) { table_unlock(); return (int32_t)CELL_ESRCH; }
    cond_lock(c);
    int32_t result = CELL_OK;
    uint32_t mid = 0;
    if (c->waiters) result = (int32_t)CELL_EBUSY;
    else { mid = c->mutex_id; c->active = 0; g_cond_id_addr[id] = 0; }
    cond_unlock(c);
    table_unlock();
    if (result == CELL_OK) sys_mutex_unbind_cond(mid);
    sync_hist('D', id, ctx, 0, result);
    return result;
}

extern unsigned long long ps3_ms_now(void);
static int cond_traced(uint32_t id);

int64_t sys_cond_wait(ppu_context* ctx)
{
    uint64_t timeout = LV2_ARG_U64(ctx, 1);
    /* Preserve upstream's opt-in wait diagnostics across the waiter rewrite. */
    const char* peek = getenv("PS3_COND_PEEK");
    if (peek) {
        uint32_t ea = (uint32_t)strtoul(peek, NULL, 16);
        if (ea) {
            const uint8_t* p = vm_to_host(ea);
            uint32_t value = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                             ((uint32_t)p[2] << 8) | p[3];
            fprintf(stderr, "[PEEK] [0x%08X]=%u (0x%08X) at cond_wait(cond=%u)\n",
                    ea, value, value, LV2_ARG_U32(ctx, 0));
        }
    }
#ifdef _WIN32
    const char* spurious = getenv("PS3_COND_SPURIOUS_MS");
    long probe_ms = spurious ? atol(spurious) : 0;
    if (probe_ms > 0 && !timeout) timeout = (uint64_t)probe_ms * 1000;
#endif
    if (timeout > ((1ull << 48) - 1)) timeout = (1ull << 48) - 1;
    if (cond_traced(LV2_ARG_U32(ctx, 0)))
        fprintf(stderr, "[COND_TRACE] t=%llu wait cond=%u by tid=%llu lr=0x%08llX%c", ps3_ms_now(), LV2_ARG_U32(ctx, 0),
                (unsigned long long)ctx->thread_id, (unsigned long long)ctx->lr, 10);
    sys_cond_info* c = acquire_cond(LV2_ARG_U32(ctx, 0));
    if (!c) return (int32_t)CELL_ESRCH;
    sys_mutex_info* m = &g_sys_mutexes[c->mutex_id - 1];
    if (!m->active) { cond_unlock(c); return (int32_t)CELL_ESRCH; }
    if (m->owner_tid != ctx->thread_id || m->lock_count <= 0) {
        cond_unlock(c); return (int32_t)CELL_EPERM;
    }
    sync_hist('W', LV2_ARG_U32(ctx, 0), ctx, c->mutex_id, 0);
    struct sys_cond_waiter waiter = {NULL, 0};
    struct sys_cond_waiter** tail = &c->waiters;
    while (*tail) tail = &(*tail)->next;
    *tail = &waiter;
    int depth = m->lock_count;
    m->owner_tid = 0;
    m->lock_count = 0;
    for (int i = 0; i < depth; ++i) guest_unlock(m);

    int32_t result = CELL_OK;
#ifdef _WIN32
    ULONGLONG start = GetTickCount64();
    uint64_t duration = (timeout + 999) / 1000;
    /* COND_SHADOW=1: a wait still blocked after 3 s prints the cond, the
     * waiter's cia/lr and its guest call stack (shadow stack, --shadow-stack
     * lift), once per wait. Names what a parked thread is waiting for when the
     * host stack only shows ntdll. */
    static int s_cs = -1;
    PS3_KNOB(s_cs, getenv("COND_SHADOW") ? 1 : 0);
    int cs_said = !s_cs;
    while (!waiter.signalled) {
        DWORD ms = INFINITE;
        if (timeout) {
            uint64_t elapsed = GetTickCount64() - start;
            if (elapsed >= duration) { result = (int32_t)CELL_ETIMEDOUT; break; }
            uint64_t remaining = duration - elapsed;
            ms = remaining >= INFINITE ? INFINITE - 1 : (DWORD)remaining;
        }
        if (!cs_said) {
            if (GetTickCount64() - start >= 3000) {
                extern void (*g_shadow_stack_dump_hook)(void);
                cs_said = 1;
                fprintf(stderr, "[COND_SHADOW] tid=%llu blocked >3s on cond=%u (mutex=%u) cia=0x%08llX lr=0x%08llX%c",
                        (unsigned long long)ctx->thread_id, LV2_ARG_U32(ctx, 0), c->mutex_id,
                        (unsigned long long)ctx->cia, (unsigned long long)ctx->lr, 10);
                if (g_shadow_stack_dump_hook) g_shadow_stack_dump_hook();
            } else if (ms > 3000) ms = 3000;
        }
        BOOL ok = SleepConditionVariableCS(&c->cv, &c->signal_lock, ms);
        if (!ok && GetLastError() != ERROR_TIMEOUT && !waiter.signalled) {
            result = (int32_t)CELL_EFAULT; break;
        }
    }
#else
    struct timespec deadline;
    if (timeout) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)(timeout / 1000000);
        deadline.tv_nsec += (long)((timeout % 1000000) * 1000);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++; deadline.tv_nsec -= 1000000000L;
        }
    }
    while (!waiter.signalled) {
        int rc = timeout ? pthread_cond_timedwait(&c->cv, &c->signal_lock, &deadline)
                         : pthread_cond_wait(&c->cv, &c->signal_lock);
        if (rc && !waiter.signalled) {
            result = (int32_t)(rc == ETIMEDOUT ? CELL_ETIMEDOUT : CELL_EFAULT);
            break;
        }
    }
#endif
    struct sys_cond_waiter** link = &c->waiters;
    while (*link != &waiter) link = &(*link)->next;
    *link = waiter.next;
    /* Off the cond and not yet back on the mutex. lv2 gives a waiter the
     * mutex, or queues it for it, in the same step that ends its wait, so
     * sys_mutex_destroy cannot slip in here; count as an owner until the
     * relock is done. */
    __sync_add_and_fetch(&m->relock_pending, 1);
    cond_unlock(c);
    for (int i = 0; i < depth; ++i) guest_lock(m);
    m->owner_tid = ctx->thread_id;
    m->lock_count = depth;
    __sync_sub_and_fetch(&m->relock_pending, 1);
    sync_hist('w', LV2_ARG_U32(ctx, 0), ctx, c->mutex_id, result);
    return result;
}

/* COND_TRACE=<id>[,<id>...]: log every wait and signal on those cond ids, with
 * the caller's guest tid and lr and whether a waiter was there to take it. */
static int cond_traced(uint32_t id)
{
    static int n = -1; static uint32_t ids[16];
    if (n < 0) {
        n = 0;
        const char* e = getenv("COND_TRACE");
        while (e && *e && n < 16) {
            ids[n++] = (uint32_t)strtoul(e, (char**)&e, 0);
            while (*e == ',' || *e == ' ') e++;
        }
    }
    for (int i = 0; i < n; i++) if (ids[i] == id) return 1;
    return 0;
}

static int64_t signal_cond(uint32_t id, int all)
{
    sys_cond_info* c = acquire_cond(id);
    if (!c) return (int32_t)CELL_ESRCH;
    int selected = 0;
    for (struct sys_cond_waiter* w = c->waiters; w; w = w->next) {
        if (w->signalled) continue;
        w->signalled = 1;
        selected = 1;
        if (!all) break;
    }
    /* Broadcast the host CV: only selected records return to the guest.
     * Waking an arbitrary host waiter could leave the selected one asleep. */
    if (selected) cond_wake(c);
    cond_unlock(c);
    return CELL_OK;
}

static int64_t signal_cond_traced(ppu_context* ctx, int all)
{
    uint32_t id = LV2_ARG_U32(ctx, 0);
    { int waiting = 0;
      sys_cond_info* c = acquire_cond(id);
      if (c) { for (struct sys_cond_waiter* w = c->waiters; w; w = w->next) waiting += !w->signalled;
               cond_unlock(c); }
      sync_hist(all ? 'A' : 'S', id, ctx, (uint32_t)waiting, c ? 0 : (int32_t)CELL_ESRCH); }
    if (cond_traced(id)) {
        int waiting = 0;
        sys_cond_info* c = acquire_cond(id);
        if (c) { for (struct sys_cond_waiter* w = c->waiters; w; w = w->next) waiting += !w->signalled;
                 cond_unlock(c); }
        fprintf(stderr, "[COND_TRACE] t=%llu signal%s cond=%u by tid=%llu lr=0x%08llX waiting=%d%c",
                ps3_ms_now(), all ? "_all" : "", id, (unsigned long long)ctx->thread_id,
                (unsigned long long)ctx->lr, waiting, 10);
    }
    return signal_cond(id, all);
}

int64_t sys_cond_signal(ppu_context* ctx) { return signal_cond_traced(ctx, 0); }
int64_t sys_cond_signal_all(ppu_context* ctx) { return signal_cond_traced(ctx, 1); }

/* Preserve upstream's current broadcast fallback for signal_to. */
int64_t sys_cond_signal_to(ppu_context* ctx) { return signal_cond(LV2_ARG_U32(ctx, 0), 1); }

void sys_cond_init(lv2_syscall_table* tbl)
{
    /* Static storage starts empty. Registration must not memset live locks. */
    lv2_syscall_register(tbl, SYS_COND_CREATE, sys_cond_create);
    lv2_syscall_register(tbl, SYS_COND_DESTROY, sys_cond_destroy);
    lv2_syscall_register(tbl, SYS_COND_WAIT, sys_cond_wait);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL, sys_cond_signal);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL_ALL, sys_cond_signal_all);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL_TO, sys_cond_signal_to);
}
