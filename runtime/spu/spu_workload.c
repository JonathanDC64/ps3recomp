/* spu_workload.c — SPU workload / task dispatch registry (see spu_workload.h).
 *
 * Maps a registered SPU image (by FNV-1a-64 content fingerprint) to its
 * pre-lifted native entry, loads the image into a 256 KB local store, and runs
 * it with the SPURS task ABI. cellSpurs's AddWorkload/CreateTask call
 * spu_workload_dispatch(); the registry is populated by the title's lifted set.
 */
#include "spu_workload.h"
#include "spu_lifted_job.h"   /* spu_run_lifted_job */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* ---- fingerprint ------------------------------------------------------- */

uint64_t spu_workload_fingerprint(const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ULL;          /* FNV offset basis */
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;                     /* FNV prime */
    }
    return h;
}

/* ---- registry ---------------------------------------------------------- */

#ifndef SPU_WORKLOAD_MAX
#define SPU_WORKLOAD_MAX 256
#endif

typedef struct {
    uint64_t            fp;
    spu_lifted_entry_fn fn;
    int                 image_id;
    const char*         name;
} spu_workload_entry;

static spu_workload_entry s_registry[SPU_WORKLOAD_MAX];
static unsigned           s_registry_count = 0;

void spu_workload_register_img(uint64_t fingerprint, spu_lifted_entry_fn fn,
                               int image_id, const char* name)
{
    if (!fn) return;
    for (unsigned i = 0; i < s_registry_count; i++) {
        if (s_registry[i].fp == fingerprint) {     /* idempotent on fingerprint */
            s_registry[i].fn       = fn;
            s_registry[i].image_id = image_id;
            s_registry[i].name     = name;
            return;
        }
    }
    if (s_registry_count >= SPU_WORKLOAD_MAX) {
        fprintf(stderr, "[spu_workload] registry full (%u); dropping '%s'\n",
                SPU_WORKLOAD_MAX, name ? name : "?");
        return;
    }
    s_registry[s_registry_count].fp       = fingerprint;
    s_registry[s_registry_count].fn       = fn;
    s_registry[s_registry_count].image_id = image_id;
    s_registry[s_registry_count].name     = name;
    s_registry_count++;
}

void spu_workload_register(uint64_t fingerprint, spu_lifted_entry_fn fn,
                           const char* name)
{
    spu_workload_register_img(fingerprint, fn, 0, name);
}

spu_lifted_entry_fn spu_workload_find(uint64_t fingerprint)
{
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fingerprint)
            return s_registry[i].fn;
    return NULL;
}

unsigned spu_workload_count(void) { return s_registry_count; }

/* ---- SPU ELF loader (32-bit big-endian) -------------------------------- */

static uint16_t rd_be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

int spu_elf_load_to_ls(const uint8_t* image, size_t image_size, uint8_t* ls,
                       uint32_t* entry_out)
{
    if (!image || !ls || image_size < 0x34) return 0;

    /* ELF ident: 0x7F 'E' 'L' 'F', ELFCLASS32 (1), ELFDATA2MSB (2). */
    if (!(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F'))
        return 0;
    if (image[4] != 1 /*ELFCLASS32*/ || image[5] != 2 /*ELFDATA2MSB*/)
        return 0;

    uint32_t e_entry     = rd_be32(image + 0x18);
    uint32_t e_phoff     = rd_be32(image + 0x1C);
    uint16_t e_phentsize = rd_be16(image + 0x2A);
    uint16_t e_phnum     = rd_be16(image + 0x2C);
    if (e_phentsize < 0x20) e_phentsize = 0x20;

    for (uint16_t i = 0; i < e_phnum; i++) {
        size_t po = (size_t)e_phoff + (size_t)i * e_phentsize;
        if (po + 0x20 > image_size) break;
        const uint8_t* ph = image + po;

        uint32_t p_type   = rd_be32(ph + 0x00);
        if (p_type != 1 /*PT_LOAD*/) continue;
        uint32_t p_offset = rd_be32(ph + 0x04);
        uint32_t p_vaddr  = rd_be32(ph + 0x08);
        uint32_t p_filesz = rd_be32(ph + 0x10);
        uint32_t p_memsz  = rd_be32(ph + 0x14);

        /* bounds: segment must fit in local store and in the image */
        if ((uint64_t)p_vaddr + p_memsz > SPU_LS_SIZE)            return 0;
        if ((uint64_t)p_offset + p_filesz > image_size)          return 0;

        if (p_filesz) memcpy(ls + p_vaddr, image + p_offset, p_filesz);
        if (p_memsz > p_filesz)
            memset(ls + p_vaddr + p_filesz, 0, p_memsz - p_filesz);
    }
    if (entry_out) *entry_out = e_entry;
    return 1;
}

size_t spu_elf_image_size(const uint8_t* image, size_t max_avail)
{
    if (!image || max_avail < 0x34) return 0;
    if (!(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F'))
        return 0;
    if (image[4] != 1 || image[5] != 2) return 0;     /* ELFCLASS32, ELFDATA2MSB */

    uint32_t e_phoff     = rd_be32(image + 0x1C);
    uint32_t e_shoff     = rd_be32(image + 0x20);
    uint16_t e_phentsize = rd_be16(image + 0x2A);
    uint16_t e_phnum     = rd_be16(image + 0x2C);
    uint16_t e_shentsize = rd_be16(image + 0x2E);
    uint16_t e_shnum     = rd_be16(image + 0x30);

    uint64_t end = (uint64_t)e_shoff + (uint64_t)e_shnum * e_shentsize;

    for (uint16_t k = 0; k < e_phnum; k++) {           /* program headers */
        size_t po = (size_t)e_phoff + (size_t)k * e_phentsize;
        if (po + 0x14 > max_avail) break;
        uint32_t p_offset = rd_be32(image + po + 0x04);
        uint32_t p_filesz = rd_be32(image + po + 0x10);
        uint64_t e = (uint64_t)p_offset + p_filesz;
        if (e > end) end = e;
    }
    for (uint16_t k = 0; k < e_shnum; k++) {           /* section headers */
        size_t so = (size_t)e_shoff + (size_t)k * e_shentsize;
        if (so + 0x18 > max_avail) break;
        uint32_t sh_type   = rd_be32(image + so + 0x04);
        uint32_t sh_offset = rd_be32(image + so + 0x10);
        uint32_t sh_size   = rd_be32(image + so + 0x14);
        if (sh_type != 8 /*SHT_NOBITS*/) {
            uint64_t e = (uint64_t)sh_offset + sh_size;
            if (e > end) end = e;
        }
    }
    if (end > max_avail) end = max_avail;
    return (size_t)end;
}

/* ---- dispatch ---------------------------------------------------------- */

int spu_workload_dispatch(const uint8_t* image, uint32_t image_size,
                          uint32_t args_ea)
{
    if (!image || image_size == 0) return 0;

    uint64_t fp = spu_workload_fingerprint(image, image_size);
    spu_lifted_entry_fn fn = NULL;
    int image_id = 0;
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fp) { fn = s_registry[i].fn; image_id = s_registry[i].image_id; break; }
    if (!fn) {
        fprintf(stderr,
            "[spu_workload] dispatch MISS fp=0x%016llX size=%u "
            "(no lifted SPU binary registered for this image)\n",
            (unsigned long long)fp, image_size);
        return 0;
    }

    /* Load the SPU ELF into a fresh local store, then run the lifted entry with
     * the task arg in r3. 256 KB is heap-allocated (too large for the stack,
     * and spu_run_lifted_job already builds a full spu_context on its stack). */
    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (!ls) return 0;

    uint32_t entry = 0;
    if (!spu_elf_load_to_ls(image, image_size, ls, &entry)) {
        fprintf(stderr, "[spu_workload] dispatch fp=0x%016llX: not a valid SPU ELF\n",
                (unsigned long long)fp);
        free(ls);
        return 0;
    }

    fprintf(stderr,
        "[spu_workload] dispatch HIT fp=0x%016llX entry=0x%05X args=0x%08X image=%d -> running\n",
        (unsigned long long)fp, entry, args_ea, image_id);

    spu_run_lifted_job_img(fn, ls, args_ea, image_id);

    free(ls);
    return 1;
}

/* Async dispatch: run the SPU job on its OWN host thread so the PPU caller is
 * not blocked. SPURS service/worker tasks are persistent — they loop waiting on
 * PPU-side signals (DMA, event flags, event queues), so running them inline (as
 * spu_workload_dispatch does) deadlocks: the PPU can never deliver the signal
 * the SPU is waiting for because it is stuck inside the dispatch. Real SPUs run
 * concurrently with the PPU; a detached host thread models that. The image bytes
 * and args live in the shared guest arena (vm_base), so they stay valid. */
typedef struct {
    const uint8_t*      image;
    uint32_t            image_size;
    uint32_t            args_ea;
    spu_lifted_entry_fn fn;
    int                 image_id;
    uint32_t            r3[4];        /* captured race-free at dispatch time */
    int                 have_r3;
    int                 abi;          /* spu_run_lifted_job_abi mode: 1=kernel, 2=task-start */
    uint32_t            exitcode_ea;  /* CellSpursTaskExitCode container EA (0 = none) */
    uint32_t            taskset_ea;   /* CellSpursTaskset EA (Option B PM), 0 = none */
    uint32_t            taskId;       /* taskset slot for the PM SpursTasksetContext */
} spu_async_job;

/* Option B PM: build the SpursTasksetContext @LS 0x2700 for the selected task before
 * running it (spurs_pm.c). Returns the task ELF EA (low bits = flags). */
extern uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t taskset_ea, uint32_t taskId,
                                       uint32_t spuNum, uint32_t dmaTagId);

static void spu_async_run(spu_async_job* j)
{
    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (ls) {
        uint32_t entry = 0;
        if (spu_elf_load_to_ls(j->image, j->image_size, ls, &entry)) {
            /* Option B: before running the leaf, build the SpursTasksetContext at LS
             * 0x2700 (the reimplemented PM) so the leaf reads valid taskset/context
             * state instead of zeros. Requires the taskset's real layout (built in
             * cellSpursCreateTask) to be in place -- it is, by dispatch time. */
            if (j->taskset_ea) {
                spurs_pm_build_context(ls, j->taskset_ea, j->taskId, /*spuNum*/0, /*dmaTag*/0);
                fprintf(stderr, "[spu_workload] PM context built @LS 0x2700 (taskset=0x%08X "
                        "taskId=%u)\n", j->taskset_ea, j->taskId);
                fflush(stderr);
            }
            /* Async dispatch is the SPURS-task path: the entry expects the SPURS
             * task kernel ABI in r3 ({0x40 marker, eaContext, queue EA, ...}),
             * captured at dispatch time (j->r3) so it doesn't race the PPU
             * overwriting the stack-allocated context. */
            /* docs/19 P2 (A-R6): reproduce the SPU Taskset PM's bitset/contention
             * side-effects so the PPU-side SPURS scheduler sees a coherent world.
             * Gated with the substrate (SPURS_LV2THREADS). */
            { static int lv2t = -1; if (lv2t < 0) { const char* e = getenv("SPURS_LV2THREADS"); lv2t = (e && e[0] != '0') ? 1 : 0; }
              if (lv2t && j->taskset_ea) {
                  extern void spurs_taskset_task_start(uint32_t, uint32_t);
                  spurs_taskset_task_start(j->taskset_ea, j->taskId); } }
            fprintf(stderr, "[spu_workload] async image=%d ENTER run\n", j->image_id);
            fflush(stderr);
            int32_t rc = -999;
#if defined(_WIN32) && defined(_MSC_VER)
            __try {
                rc = spu_run_lifted_job_abi(j->fn, ls, j->args_ea, j->image_id,
                                            j->abi, j->have_r3 ? j->r3 : 0, j->exitcode_ea);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                fprintf(stderr, "[spu_workload] async image=%d *** SEH FAULT 0x%08lX *** "
                        "(SPU task crashed -- caught, not propagated)\n",
                        j->image_id, (unsigned long)GetExceptionCode());
                fflush(stderr);
            }
#else
            rc = spu_run_lifted_job_abi(j->fn, ls, j->args_ea, j->image_id,
                                        j->abi, j->have_r3 ? j->r3 : 0, j->exitcode_ea);
#endif
            fprintf(stderr, "[spu_workload] async image=%d RETURNED rc=%d\n", j->image_id, rc);
            fflush(stderr);
            /* docs/19 P2 (A-R6): on leaf completion, clear running+enabled + decrement
             * contention (the DONE marker + scheduler coherence). Gated. */
            { static int lv2t = -1; if (lv2t < 0) { const char* e = getenv("SPURS_LV2THREADS"); lv2t = (e && e[0] != '0') ? 1 : 0; }
              if (lv2t && j->taskset_ea) {
                  extern void spurs_taskset_task_done(uint32_t, uint32_t);
                  spurs_taskset_task_done(j->taskset_ea, j->taskId); } }

            /* EXPERIMENT (SPURS_DONE_EVQ=N): on task completion, post a SPURS USER
             * event to event queue N so the PPU completion handler (e.g. hkSpuUtil
             * Helper on evq 12) wakes and signals the barrier the game waits on.
             * Tests whether wiring SPU-completion -> PPU-event unblocks pre-title
             * loading. Params overridable: SPURS_DONE_SPUP, SPURS_DONE_DATA. */
            /* Completion bridging (SPURS_SIGNAL_COND=N[,TS]): on leaf completion,
             * signal SPURS barrier cond N once (the barrier waits N times, one wake
             * per completed task). Optional ,TS restricts to taskset_ea==TS. The leaf
             * has actually run (data ready), so this releases the barrier correctly --
             * unlike force-releasing the wait before the work runs. */
            { const char* se = getenv("SPURS_SIGNAL_COND");
              if (se) { extern void spurs_signal_cond_by_id(uint32_t);
                  char* end = 0; unsigned long cid = strtoul(se, &end, 0);
                  unsigned long ts = 0; if (end && *end == ',') ts = strtoul(end+1, 0, 0);
                  if (!ts || (uint32_t)ts == j->taskset_ea) {
                      extern volatile long g_spurs_release_pending;
                      g_spurs_release_pending++;   /* sticky: SPURS_RELEASE_COND consumes it */
                      fprintf(stderr, "[spu_workload] completion-bridge: signal cond %lu + mark "
                              "release-pending (image=%d taskset=0x%08X)\n", cid, j->image_id, j->taskset_ea);
                      spurs_signal_cond_by_id((uint32_t)cid);
                  } } }

            /* Completion event propagation. On real HW the SPU SPURS kernel posts a
             * completion event to a workload's event queue when its tasks finish; the
             * PPU-side handler (e.g. the Havok PPU Thread on evq 5) receives it and
             * releases the barrier the game's MAIN loop waits on. We run leaf tasks
             * directly (bypassing the kernel), so we post that event here.
             *
             * Default mapping (proven to unblock the Core.Res asset pipeline): the
             * Havok leaf image (image_id 7) -> evq 5. SPURS_DONE_EVQ=N overrides the
             * target queue for experiments; SPURS_DONE_SPUP / SPURS_DONE_DATA tune the
             * payload. */
            { extern int sys_spu_thread_post_user_event(uint32_t,uint64_t,uint32_t,uint32_t,uint64_t);
              uint32_t q = 0;
              const char* qe = getenv("SPURS_DONE_EVQ");
              if (qe) q = (uint32_t)strtoul(qe, 0, 0);
              else if (j->image_id == 7) q = 5;   /* Havok leaf completion -> Havok PPU Thread */
              if (q) {
                  uint32_t spup = 0; { const char* e = getenv("SPURS_DONE_SPUP"); if (e) spup = (uint32_t)strtoul(e,0,0); }
                  uint64_t d3 = (uint64_t)j->taskset_ea; { const char* e = getenv("SPURS_DONE_DATA"); if (e) d3 = strtoull(e,0,0); }
                  /* data1 = the SENDING SPU thread's lv2 id (docs/15 §3). Use the id the
                   * P2 substrate registered (g_cs_spu_lv2[0]) so handler B's list lookup
                   * HITS; fall back to 1 only if the substrate isn't up (SPURS_LV2THREADS
                   * off). Round-robin association could refine which spu id per task (P3/Q3). */
                  extern uint32_t g_cs_spu_lv2[]; extern uint32_t g_cs_nspus;
                  uint64_t spu_lv2 = (g_cs_nspus && g_cs_spu_lv2[0]) ? g_cs_spu_lv2[0] : 1u;
                  fprintf(stderr, "[spu_workload] POST completion USER event q=%u spup=%u spu_lv2=0x%llX data3=0x%llX (image=%d)\n",
                          q, spup, (unsigned long long)spu_lv2, (unsigned long long)d3, j->image_id);
                  int r = sys_spu_thread_post_user_event(q, spu_lv2, spup, 0, d3);
                  fprintf(stderr, "[spu_workload] post_user_event -> %d\n", r); } }
        }
        free(ls);
    }
    free(j);
}

#ifdef _WIN32
static DWORD WINAPI spu_async_thread(LPVOID p) { spu_async_run((spu_async_job*)p); return 0; }
#else
static void* spu_async_thread(void* p) { spu_async_run((spu_async_job*)p); return NULL; }
#endif

/* SPU task thread stack size (bytes). A lifted SPU job's guest `brsl` calls become
 * real nested host C calls. Guest LOOPS that branch to another lifted function are
 * forced tail calls (SPU_TAILCALL/musttail in the lifter) so they iterate in O(1)
 * host stack -- without that they overflowed and silently killed the process (a
 * stack-overflow SE can't run the unhandled filter; exit code reads 0). Genuine
 * call nesting is bounded by the guest's own stack discipline; 64 MB RESERVE
 * (committed on demand) is ample headroom. env SPU_STACK_MB overrides (also a probe
 * for runaway recursion: if crash time scales with this, something still nests). */
static size_t spu_task_stack_bytes(void)
{
    const char* e = getenv("SPU_STACK_MB");
    unsigned mb = e ? (unsigned)strtoul(e, NULL, 10) : 0;
    if (mb == 0) mb = 64;
    return (size_t)mb << 20;
}

/* Spawn a detached host thread to run job `j` (takes ownership of `j`). */
static int spu_spawn_job_thread(spu_async_job* j)
{
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, spu_task_stack_bytes(), spu_async_thread, j, 0, NULL);
    if (!th) { free(j); return 0; }
    CloseHandle(th);   /* detached */
#else
    pthread_t th;
    if (pthread_create(&th, NULL, spu_async_thread, j) != 0) { free(j); return 0; }
    pthread_detach(th);
#endif
    return 1;
}

/* ---- Deferred SPURS-task scheduling (docs/13 Part A) -----------------------
 * RPCS3 does NOT run a SPURS task at cellSpursCreateTask -- it marks it ready and
 * the SPU kernel schedules it LATER, after the game has populated per-task data.
 * We model that by REGISTERING created tasks as pending and FLUSHING them (spawning
 * their threads) the first time the creating PPU thread WAITS (event-queue receive,
 * cond/sema wait, or the timer-usleep poll loop) -- by which point setup is done.
 * Without this, a task races ahead of its data (image=7 read a NULL ptr from its
 * still-zero arg struct -> CELL_SPURS_TASK_ERROR_NULL_POINTER). */
#define SPU_PENDING_MAX 64
static spu_async_job* s_pending[SPU_PENDING_MAX];
static int            s_pending_count = 0;
#ifdef _WIN32
static CRITICAL_SECTION s_pending_cs;
static volatile long    s_pending_cs_init = 0;   /* 0=uninit, 1=initializing, 2=ready */
static void pending_lock(void)   {
    if (_InterlockedCompareExchange(&s_pending_cs_init, 1, 0) == 0) {
        InitializeCriticalSection(&s_pending_cs);
        _InterlockedExchange(&s_pending_cs_init, 2);
    } else {
        while (s_pending_cs_init != 2) { /* spin until the initializer finishes */ }
    }
    EnterCriticalSection(&s_pending_cs);
}
static void pending_unlock(void) { LeaveCriticalSection(&s_pending_cs); }
#else
static pthread_mutex_t s_pending_mx = PTHREAD_MUTEX_INITIALIZER;
static void pending_lock(void)   { pthread_mutex_lock(&s_pending_mx); }
static void pending_unlock(void) { pthread_mutex_unlock(&s_pending_mx); }
#endif

/* Register a created-but-not-yet-run task. Takes ownership of `j`. */
static int spu_pending_register(spu_async_job* j)
{
    pending_lock();
    int ok = (s_pending_count < SPU_PENDING_MAX);
    if (ok) s_pending[s_pending_count++] = j;
    pending_unlock();
    if (!ok) { fprintf(stderr, "[spu_workload] pending registry FULL -- running task eagerly\n");
               return spu_spawn_job_thread(j); }
    fprintf(stderr, "[spu_workload] task REGISTERED pending image=%d taskset=0x%08X taskId=%u "
            "(run deferred to next PPU wait)\n", j->image_id, j->taskset_ea, j->taskId);
    return 1;
}

/* Q2 diagnostic (SPU_QLOG): at flush time the PPU is blocked waiting for completion,
 * so the game's setup is done -- re-dump a pending task's arg-pointed structs to see
 * whether the data it will read is NOW populated (vs zero at create time). */
static void spu_pending_dump_args(const spu_async_job* j)
{
    if (!getenv("SPU_QLOG")) return;
    extern uint8_t* vm_base;
    for (int s = 0; s < 3; s++) {
        uint32_t p = j->r3[s];
        if (p < 0x00010000u || p >= 0x50000000u) continue;
        const uint8_t* d = vm_base + p;
        fprintf(stderr, "[flush-argdump] image=%d r3[%d]=0x%08X:", j->image_id, s, p);
        for (int b = 0; b < 32; b++) fprintf(stderr, "%s%02X", (b%4)?"":" ", d[b]);
        fprintf(stderr, "\n");
    }
}

/* Run all pending tasks now. Called from the PPU blocking/poll syscalls. Idempotent
 * (no-op when empty), so it is cheap to call on every wait. */
void spu_pending_flush(void)
{
    spu_async_job* batch[SPU_PENDING_MAX];
    int n = 0;
    pending_lock();
    n = s_pending_count; s_pending_count = 0;
    for (int i = 0; i < n; i++) batch[i] = s_pending[i];
    pending_unlock();
    if (n == 0) return;
    fprintf(stderr, "[spu_workload] pending FLUSH: running %d deferred task(s)\n", n);
    for (int i = 0; i < n; i++) {
        spu_pending_dump_args(batch[i]);     /* Q2: is the data populated now? */
        spu_spawn_job_thread(batch[i]);
    }
}

int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size,
                                uint32_t args_ea)
{
    if (!image || image_size == 0) return 0;

    uint64_t fp = spu_workload_fingerprint(image, image_size);
    spu_lifted_entry_fn fn = NULL;
    int image_id = 0;
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fp) { fn = s_registry[i].fn; image_id = s_registry[i].image_id; break; }
    if (!fn) {
        fprintf(stderr, "[spu_workload] async dispatch MISS fp=0x%016llX size=%u\n",
                (unsigned long long)fp, image_size);
        return 0;
    }

    spu_async_job* j = (spu_async_job*)malloc(sizeof(*j));
    if (!j) return 0;
    j->image = image; j->image_size = image_size; j->args_ea = args_ea;
    j->fn = fn; j->image_id = image_id; j->abi = 1;   /* kernel marker ABI */
    j->exitcode_ea = 0; j->taskset_ea = 0; j->taskId = 0;
    /* Capture the SPURS task r3 NOW (PPU thread, synchronous) from the game's
     * descriptor at eaContext+0x10 = {0x40-marker handle, workload EAs}; the
     * async SPU thread reading it later would race the PPU stack. word1 is
     * overridden to args_ea (eaContext) in spu_run_lifted_job_abi. */
    j->have_r3 = 0;
    if (args_ea) {
        extern uint8_t* vm_base;
        const uint8_t* c = vm_base + args_ea + 0x10;
        for (int k = 0; k < 4; k++)
            j->r3[k] = ((uint32_t)c[k*4]<<24)|((uint32_t)c[k*4+1]<<16)|
                       ((uint32_t)c[k*4+2]<<8)|c[k*4+3];
        if ((j->r3[0] >> 16) == 0x40) j->have_r3 = 1;   /* valid marker */
    }

    fprintf(stderr,
        "[spu_workload] dispatch HIT (async) fp=0x%016llX args=0x%08X image=%d -> spawning thread\n",
        (unsigned long long)fp, args_ea, image_id);

    return spu_spawn_job_thread(j);
}

/* Dispatch a SPURS leaf task with the task-START ABI (what the taskset policy
 * module hands a fresh task): r3 = the 16-byte task argument, r4 = tasksetEA.
 * Unlike dispatch_async (kernel-marker ABI), this passes the real task argument
 * the task body reads -- without it the body processes a null arg and loops. */
int spu_workload_dispatch_task(const uint8_t* image, uint32_t image_size,
                               const uint32_t arg[4], uint32_t taskset_ea,
                               uint32_t exitcode_ea, uint32_t taskId)
{
    if (!image || image_size == 0) return 0;
    uint64_t fp = spu_workload_fingerprint(image, image_size);
    spu_lifted_entry_fn fn = NULL; int image_id = 0;
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fp) { fn = s_registry[i].fn; image_id = s_registry[i].image_id; break; }
    if (!fn) {
        fprintf(stderr, "[spu_workload] task dispatch MISS fp=0x%016llX size=%u\n",
                (unsigned long long)fp, image_size);
        return 0;
    }
    spu_async_job* j = (spu_async_job*)malloc(sizeof(*j));
    if (!j) return 0;
    j->image = image; j->image_size = image_size; j->args_ea = taskset_ea;
    j->fn = fn; j->image_id = image_id; j->abi = 2;   /* task-start ABI */
    j->r3[0]=arg[0]; j->r3[1]=arg[1]; j->r3[2]=arg[2]; j->r3[3]=arg[3]; j->have_r3 = 1;
    j->exitcode_ea = exitcode_ea;     /* CellSpursTaskExitCode container (0 = none) */
    j->taskset_ea  = taskset_ea;      /* Option B: PM builds SpursTasksetContext from this */
    j->taskId      = taskId;
    fprintf(stderr, "[spu_workload] task dispatch (async) fp=0x%016llX "
            "r3={0x%08X,0x%08X,0x%08X,0x%08X} tasksetEA=0x%08X image=%d\n",
            (unsigned long long)fp, arg[0], arg[1], arg[2], arg[3], taskset_ea, image_id);
    /* Controlled diagnostic: SPU_TASK_OFF suppresses the actual SPU thread spawn
     * (the task is "created" but never runs) to isolate whether the concurrent SPU
     * task is what terminates the process after CreateTask. */
    { const char* off = getenv("SPU_TASK_OFF");
      if (off && off[0] != '0') { fprintf(stderr, "[spu_workload] SPU_TASK_OFF: thread NOT spawned\n");
                                  fflush(stderr); free(j); return 1; } }
    /* Diagnostic: SPU_INLINE runs the task on THIS (PPU) thread instead of a
     * worker, so the boot's hang_watchdog -- which samples every OTHER thread's
     * RIP -- pins exactly which lifted SPU function the task loops in. Deadlocks
     * the boot (the task can't get PPU-side signals), but that's fine for a probe. */
    { const char* inl = getenv("SPU_INLINE");
      if (inl && inl[0] != '0') {
          fprintf(stderr, "[spu_workload] SPU_INLINE: running task on PPU thread\n");
          fflush(stderr); spu_async_run(j); return 1; } }
    /* DEFERRED scheduling (docs/13 Part A): register the task as pending and run it
     * when the creating PPU thread next WAITS (spu_pending_flush in the wait syscalls),
     * by which point the game has populated per-task data. SPU_TASK_EAGER=1 forces the
     * old run-on-create behaviour for A/B comparison. */
    { const char* eager = getenv("SPU_TASK_EAGER");
      if (eager && eager[0] != '0') return spu_spawn_job_thread(j); }
    return spu_pending_register(j);
}
