/*
 * ps3recomp - Semaphore syscalls (implementation)
 */

#include "sys_semaphore.h"
#include "../memory/vm.h"
#include <string.h>
#include <stdlib.h>   /* getenv/strtol (FORCE_SEMA probe; avoid ptr-truncating implicit decls) */

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_semaphore_info g_sys_semaphores[SYS_SEMAPHORE_MAX];

#ifdef _WIN32
static CRITICAL_SECTION s_sem_table_lock;
static int              s_sem_table_lock_init = 0;
#else
static pthread_mutex_t  s_sem_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void sem_table_lock(void)
{
#ifdef _WIN32
    if (!s_sem_table_lock_init) {
        InitializeCriticalSection(&s_sem_table_lock);
        s_sem_table_lock_init = 1;
    }
    EnterCriticalSection(&s_sem_table_lock);
#else
    pthread_mutex_lock(&s_sem_table_lock);
#endif
}

static void sem_table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_sem_table_lock);
#else
    pthread_mutex_unlock(&s_sem_table_lock);
#endif
}

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
 * sys_semaphore_create
 *
 * r3 = pointer to receive semaphore ID (u32*)
 * r4 = pointer to attribute struct
 * r5 = initial value
 * r6 = max value
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_create(ppu_context* ctx)
{
    uint32_t id_out_addr  = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr    = LV2_ARG_PTR(ctx, 1);
    int32_t  initial      = LV2_ARG_S32(ctx, 2);
    int32_t  max_val      = LV2_ARG_S32(ctx, 3);

    if (max_val <= 0 || initial < 0 || initial > max_val)
        return (int64_t)(int32_t)CELL_EINVAL;

    sem_table_lock();

    int slot = -1;
    for (int i = 0; i < SYS_SEMAPHORE_MAX; i++) {
        if (!g_sys_semaphores[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        sem_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_semaphore_info* s = &g_sys_semaphores[slot];
    memset(s, 0, sizeof(*s));
    s->active    = 1;
    s->value     = initial;
    s->max_value = max_val;

    if (attr_addr != 0) {
        uint8_t* attr_raw = (uint8_t*)vm_to_host(attr_addr);
        uint32_t proto_be;
        memcpy(&proto_be, attr_raw, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        proto_be = ((proto_be >> 24) & 0xFF) | ((proto_be >> 8) & 0xFF00) |
                   ((proto_be << 8) & 0xFF0000) | ((proto_be << 24) & 0xFF000000u);
#endif
        s->protocol = proto_be;
        /* name at offset 8 */
        memcpy(s->name, attr_raw + 8, 8);
    }

#ifdef _WIN32
    s->sem_handle = CreateSemaphoreA(NULL, initial, max_val, NULL);
    InitializeCriticalSection(&s->value_lock);
    if (s->sem_handle == NULL) {
        s->active = 0;
        sem_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
#else
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cv, NULL);
#endif

    uint32_t sem_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, sem_id);
    }

    sem_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_destroy
 *
 * r3 = sem_id
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_destroy(ppu_context* ctx)
{
    uint32_t sem_id = LV2_ARG_U32(ctx, 0);

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sem_table_lock();

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active) {
        sem_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

#ifdef _WIN32
    CloseHandle(s->sem_handle);
    DeleteCriticalSection(&s->value_lock);
#else
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mtx);
#endif

    s->active = 0;
    sem_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_wait
 *
 * r3 = sem_id
 * r4 = timeout_usec (0 = infinite)
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_wait(ppu_context* ctx)
{
    { extern void spu_pending_flush(void); spu_pending_flush(); }  /* run deferred SPURS tasks (docs/13) */
    { static int _st=-1; if(_st<0)_st=getenv("YDKJ_SYNCTRACE")?1:0; if(_st) fprintf(stderr,"[SYNC] tid=%lu SEMA_WAIT id=0x%X\n",(unsigned long)GetCurrentThreadId(),(unsigned)(uint32_t)ctx->gpr[3]); }
    uint32_t sem_id     = LV2_ARG_U32(ctx, 0);
    uint64_t timeout_us = LV2_ARG_U64(ctx, 1);
    fprintf(stderr, "[WAIT] semaphore_wait(sem=%u timeout=%llu)\n", sem_id, (unsigned long long)timeout_us);
    /* One-shot: capture the guest caller (LR) of the sema-4 frame wait so we can
     * decompile the exact condition-loop that HighGraphics parks in. */
    { static int _n4 = 0;
      if (sem_id == 4 && _n4 < 2) { _n4++;
        fprintf(stderr, "[WAIT4] sema-4 r3..r6=%08X %08X %08X %08X host-bt:\n",
                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
                (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]);
#ifdef _WIN32
        void* frames[48];
        unsigned short nfr = RtlCaptureStackBackTrace(0, 48, frames, 0);
        HMODULE self = 0;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&sys_semaphore_wait, &self);
        for (unsigned short i = 0; i < nfr; i++) {
            HMODULE m = 0;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &m);
            if (m == self)
                fprintf(stderr, "[WAIT4]   rva=0x%llX\n",
                        (unsigned long long)((char*)frames[i] - (char*)self));
        }
#endif
      } }

    /* EXPERIMENT (FORCE_SEMA=N): make sys_semaphore_wait on sema N return immediately
     * (probe whether a never-posted semaphore, e.g. SLSession's sema 2, gates the
     * cellSaveDataAutoLoad2 dialog). */
    { static int fs = -2; if (fs == -2) { const char* e = getenv("FORCE_SEMA"); fs = e ? (int)strtol(e,0,0) : -1; }
      if (fs >= 0 && sem_id == (uint32_t)fs) return CELL_OK; }

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    { extern void thrdiag_wait(const char*, uint32_t); thrdiag_wait("sema", sem_id); }
    /* SLSESSION_BT: one-shot guest backtrace at the SLSession gate wait (the only
     * infinite wait on this id), so we can find its functor + the condition it loops on. */
    { static int bt=-1; if(bt<0){const char* e=getenv("SLSESSION_BT"); bt=e?atoi(e):0;}
      if (bt>0 && sem_id==(uint32_t)bt && timeout_us==0) { static int once=0; if(once++<3){
          fprintf(stderr,"[slsess-bt] wait sem=%u cia=0x%08X lr=0x%08X\n",sem_id,(uint32_t)ctx->cia,(uint32_t)ctx->lr);
          extern void ds_dump_shadow(void); ds_dump_shadow(); fflush(stderr);} } }
#ifdef _WIN32
    DWORD ms = (timeout_us == 0) ? INFINITE : (DWORD)(timeout_us / 1000);
    if (ms == 0 && timeout_us > 0) ms = 1;

    int is_timed = (timeout_us > 0);
    if (is_timed) InterlockedIncrement(&s->timed_waiters);
    DWORD result = WaitForSingleObject(s->sem_handle, ms);
    if (is_timed) InterlockedDecrement(&s->timed_waiters);
    { extern void thrdiag_wake(void); thrdiag_wake(); }
    if (result == WAIT_TIMEOUT) {
        return (int64_t)(int32_t)CELL_ETIMEDOUT;
    }
    if (result != WAIT_OBJECT_0) {
        return (int64_t)(int32_t)CELL_EINVAL;
    }

    EnterCriticalSection(&s->value_lock);
    s->value--;
    LeaveCriticalSection(&s->value_lock);
#else
    pthread_mutex_lock(&s->mtx);

    if (timeout_us == 0) {
        while (s->value <= 0) {
            pthread_cond_wait(&s->cv, &s->mtx);
        }
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeout_us / 1000000);
        ts.tv_nsec += (long)((timeout_us % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (s->value <= 0) {
            int rc = pthread_cond_timedwait(&s->cv, &s->mtx, &ts);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&s->mtx);
                return (int64_t)(int32_t)CELL_ETIMEDOUT;
            }
        }
    }

    s->value--;
    pthread_mutex_unlock(&s->mtx);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_trywait
 *
 * r3 = sem_id
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_trywait(ppu_context* ctx)
{
    uint32_t sem_id = LV2_ARG_U32(ctx, 0);

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    DWORD result = WaitForSingleObject(s->sem_handle, 0);
    if (result == WAIT_TIMEOUT) {
        return (int64_t)(int32_t)CELL_EBUSY;
    }
    if (result != WAIT_OBJECT_0) {
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    EnterCriticalSection(&s->value_lock);
    s->value--;
    LeaveCriticalSection(&s->value_lock);
#else
    pthread_mutex_lock(&s->mtx);
    if (s->value <= 0) {
        pthread_mutex_unlock(&s->mtx);
        return (int64_t)(int32_t)CELL_EBUSY;
    }
    s->value--;
    pthread_mutex_unlock(&s->mtx);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_post
 *
 * r3 = sem_id
 * r4 = count (number to post)
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_post(ppu_context* ctx)
{
    { static int _st=-1; if(_st<0)_st=getenv("YDKJ_SYNCTRACE")?1:0; if(_st) fprintf(stderr,"[SYNC] tid=%lu SEMA_POST id=0x%X\n",(unsigned long)GetCurrentThreadId(),(unsigned)(uint32_t)ctx->gpr[3]); }
    uint32_t sem_id = LV2_ARG_U32(ctx, 0);
    int32_t  count  = LV2_ARG_S32(ctx, 1);

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (count <= 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    EnterCriticalSection(&s->value_lock);
    if (s->value + count > s->max_value) {
        LeaveCriticalSection(&s->value_lock);
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    s->value += count;
    LeaveCriticalSection(&s->value_lock);

    ReleaseSemaphore(s->sem_handle, count, NULL);
#else
    pthread_mutex_lock(&s->mtx);
    if (s->value + count > s->max_value) {
        pthread_mutex_unlock(&s->mtx);
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    s->value += count;
    /* Wake waiters */
    for (int i = 0; i < count; i++) {
        pthread_cond_signal(&s->cv);
    }
    pthread_mutex_unlock(&s->mtx);
#endif

    return CELL_OK;
}

/* Host-callable post by id (no ppu_context). Used to emulate the RSX vblank
 * interrupt: on real HW _gcm_intr_thread receives an RSX interrupt event and
 * posts the GCM vblank semaphore that HighGraphics waits on each frame. We have
 * no RSX interrupt, so the synthetic vblank ticker calls this at ~60Hz. Safe if
 * the sema is full (won't exceed max_value). Returns 1 if it posted. */
int lv2_semaphore_post_by_id(uint32_t sem_id, int count)
{
    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX || count <= 0) return 0;
    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active) return 0;
#ifdef _WIN32
    EnterCriticalSection(&s->value_lock);
    if (s->value + count > s->max_value) { LeaveCriticalSection(&s->value_lock); return 0; }
    s->value += count;
    LeaveCriticalSection(&s->value_lock);
    ReleaseSemaphore(s->sem_handle, count, NULL);
#else
    pthread_mutex_lock(&s->mtx);
    if (s->value + count > s->max_value) { pthread_mutex_unlock(&s->mtx); return 0; }
    s->value += count;
    for (int i = 0; i < count; i++) pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mtx);
#endif
    return 1;
}

/* lv2_semaphore_post_frame -- emulate the libgcm RSX vblank interrupt handler
 * (_gcm_intr_thread) posting the GCM frame semaphore each vblank so HighGraphics
 * advances a frame (RPCS3: sema 0x9604d100, get_value-then-post). We don't run
 * _gcm_intr_thread; the host vblank ticker calls this with the exact sema id that
 * HighGraphics is currently blocked on (via thrdiag).
 *
 * Guards (this is a SPECIFIC id, but stay defensive): post only if the sema is
 * binary (max_value==1), drained (value==0), and has a FINITE-timeout waiter
 * (timed_waiters>0). HighGraphics's frame wait is timed (100ms); its init waits
 * are infinite (timeout=0, timed_waiters==0) so they are never posted here.
 * Returns 1 if posted. */
int lv2_semaphore_post_frame(uint32_t sem_id)
{
    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX) return 0;
    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active || s->max_value != 1 || s->timed_waiters <= 0) return 0;
#ifdef _WIN32
    EnterCriticalSection(&s->value_lock);
    int do_post = (s->value == 0);
    if (do_post) s->value += 1;
    LeaveCriticalSection(&s->value_lock);
    if (do_post) { ReleaseSemaphore(s->sem_handle, 1, NULL); return 1; }
#else
    pthread_mutex_lock(&s->mtx);
    int do_post = (s->value == 0);
    if (do_post) { s->value += 1; pthread_cond_signal(&s->cv); }
    pthread_mutex_unlock(&s->mtx);
    if (do_post) return 1;
#endif
    return 0;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_get_value
 *
 * r3 = sem_id
 * r4 = pointer to receive value (s32*)
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_get_value(ppu_context* ctx)
{
    uint32_t sem_id   = LV2_ARG_U32(ctx, 0);
    uint32_t out_addr = LV2_ARG_PTR(ctx, 1);

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    int32_t val;
#ifdef _WIN32
    EnterCriticalSection(&s->value_lock);
    val = s->value;
    LeaveCriticalSection(&s->value_lock);
#else
    pthread_mutex_lock(&s->mtx);
    val = s->value;
    pthread_mutex_unlock(&s->mtx);
#endif

    if (out_addr != 0) {
        write_be32(out_addr, (uint32_t)val);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_semaphore_init(lv2_syscall_table* tbl)
{
    memset(g_sys_semaphores, 0, sizeof(g_sys_semaphores));

#ifdef _WIN32
    if (!s_sem_table_lock_init) {
        InitializeCriticalSection(&s_sem_table_lock);
        s_sem_table_lock_init = 1;
    }
#endif

    lv2_syscall_register(tbl, SYS_SEMAPHORE_CREATE,    sys_semaphore_create);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_DESTROY,   sys_semaphore_destroy);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_WAIT,      sys_semaphore_wait);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_TRYWAIT,   sys_semaphore_trywait);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_POST,      sys_semaphore_post);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_GET_VALUE, sys_semaphore_get_value);
}
