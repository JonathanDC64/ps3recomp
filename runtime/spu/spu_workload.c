/* spu_workload.c — SPU workload / task dispatch registry (see spu_workload.h).
 *
 * Maps a registered SPU image (by FNV-1a-64 content fingerprint) to its
 * pre-lifted native entry, loads the image into a 256 KB local store, and runs
 * it with the SPURS task ABI. cellSpurs's AddWorkload/CreateTask call
 * spu_workload_dispatch(); the registry is populated by the title's lifted set.
 */
#include "spu_workload.h"
#include "ps3emu/ps3_knob.h"
#include "spu_lifted_job.h"   /* spu_run_lifted_job */
#include "../../libs/spurs/spurs_taskset.h"   /* taskset bitset layout + atomic set/clear */
#include "../ps3_log.h"      /* ps3_log_verbose */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <errno.h>
#include "../platform/win32_compat.h"   /* LONG, InterlockedExchange, Sleep */
#endif

/* Set by the MFC DMA engine (spu_dma.h) when the cri task (image 22) issues a
 * real video-payload GET (>256B from a non-context EA) = it actually decoded,
 * as opposed to the 64-byte context handshake DMAs. The dispatcher's
 * YDKJ_CRI_RESUME poll-loop watches this to know real work arrived. */
int g_cri_video_dma = 0;

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

spu_lifted_entry_fn spu_workload_find_img(uint64_t fingerprint, int* image_id_out)
{
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fingerprint) {
            if (image_id_out) *image_id_out = s_registry[i].image_id;
            return s_registry[i].fn;
        }
    if (image_id_out) *image_id_out = 0;
    return NULL;
}

unsigned spu_workload_count(void) { return s_registry_count; }

/* ---- SPU ELF loader (32-bit big-endian) -------------------------------- */

static uint16_t rd_be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

int spu_elf_load_to_ls(const uint8_t* image, size_t image_size, uint8_t* ls, uint32_t* entry_out)
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

/* Runs per lifted image id (index = image id, 0 = unknown), counted at each
 * dispatch HIT on the sync, job and async paths. Read by the debug console's
 * `spu` verb: shows which SPU programs a title is running right now. */
volatile unsigned long long g_spu_image_runs[64];
static void spu_count_run(int image_id)
{
    g_spu_image_runs[(unsigned)image_id < 64 ? image_id : 0]++;
}

/* SPU history: a ring of the last 64K task dispatches, starts, returns, thread
 * ends, signals, WAIT_SIGNAL parks and SPU->PPU user events. Always on -- one
 * store per event -- and read by the debug console's `spuhist [image] [n]`,
 * which answers "which run of this task never finished, or never told the PPU"
 * after a hang, without a log that perturbs the timing that caused it. */
spu_hist_ent g_spu_hist[SPU_HIST_N];
volatile long g_spu_hist_n;
void spu_hist(char op, int image, uint32_t a, uint32_t b, uint32_t c)
{
    extern unsigned long long ps3_ms_now(void);
    const long i = __sync_add_and_fetch(&g_spu_hist_n, 1) - 1;
    spu_hist_ent* e = &g_spu_hist[(unsigned long)i & (SPU_HIST_N - 1)];
    e->t_ms = (uint32_t)ps3_ms_now(); e->op = (uint8_t)op; e->image = (uint8_t)image;
    e->a = a; e->b = b; e->c = c; e->htid = (uint32_t)GetCurrentThreadId();
}

uint32_t spu_hist_line(void)
{
    static int s_init; static uint32_t s_ea;
    if (!s_init) { const char* e = getenv("SPU_HIST_LINE");
                   s_ea = e ? (uint32_t)strtoul(e, 0, 16) : 0; s_init = 1; }
    return s_ea;
}

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
        fprintf(stderr, "[spu_workload] dispatch fp=0x%016llX: not a valid SPU ELF\n", (unsigned long long)fp);
        free(ls);
        return 0;
    }

    spu_count_run(image_id);
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
    uint32_t            taskset_ea;   /* captured race-free at dispatch (globals get clobbered) */
    uint32_t            taskid;
    LONG                gen;          /* taskset generation when the job was queued */
} spu_async_job;

/* ---- Persistent per-taskset local store (LBP_PERSIST_LS) ------------------
 * Real SPURS time-multiplexes a taskset's tasks onto SPUs, saving/restoring a
 * shared taskset context in LS. Our default model runs each task ONCE on a
 * fresh calloc'd LS, so state one task establishes (e.g. FMOD's SPU DSP handler
 * table at LS 0x39Dxx, built by the init task) is invisible to the next task of
 * the same taskset, which reads the uninitialised image sentinel and
 * branch-to-0's. Give each taskset a PERSISTENT LS reused across its task
 * dispatches (image loaded once, state retained) and SERIALIZE its tasks (one
 * at a time) so they observe each other's setup without racing the shared
 * store -- a cooperative approximation of the kernel's context save/restore.
 * With LBP_WS_DRAIN (a task parked in WAIT_SIGNAL resumes + releases the
 * taskset, letting the next task run), the FMOD init + DSP tasks share one LS
 * and the handler table is populated when the DSP dispatch reads it. Opt-in;
 * default keeps per-dispatch fresh LS. */
typedef struct {
    uint32_t taskset_ea;   /* 0 = free slot */
    uint32_t taskid;       /* slot key is (taskset, task): on real SPURS each
                            * task has a PRIVATE LS context (the kernel saves/
                            * restores per task); one shared slot per TASKSET
                            * serialized siblings behind one lock, and FMOD's
                            * persistent mixer (task 0, never returns) starved
                            * its producer (task 1) forever -- the mixer then
                            * DMA-polled a NULL control EA for the whole run. */
    uint8_t* ls;           /* persistent LS (NULL until first task loads it) */
    int      loaded;       /* image loaded into ls */
#ifdef _WIN32
    SRWLOCK  lock;         /* serializes re-dispatches of this ONE task */
#else
    pthread_mutex_t lock;
#endif
} spu_ts_ls_slot;

#define SPU_TS_LS_MAX 24
static spu_ts_ls_slot s_ts_ls[SPU_TS_LS_MAX];
#ifdef _WIN32
static SRWLOCK s_ts_ls_dir = SRWLOCK_INIT;
#else
static pthread_mutex_t s_ts_ls_dir = PTHREAD_MUTEX_INITIALIZER;
#endif

static int spu_persist_ls_enabled(void)
{ static int _p = -1; PS3_KNOB(_p, getenv("LBP_PERSIST_LS") ? 1 : 0); return _p; }

/* ---- Global SPU execution serialization (LBP_SPU_SERIAL) ------------------
 * Default runs each SPU task on its own detached host thread, so N tasks race:
 * timing jitter sends each boot down a different path to a different dead end
 * (one run hangs on the FMOD DSP null, the next on an unrelated PPU init spin),
 * which makes any single failure impossible to reproduce and debug. Serialize:
 * only ONE SPU task executes at a time (global lock), so the SPU stage becomes
 * a fixed, repeatable sequence. A task that parks in WAIT_SIGNAL RELEASES the
 * lock so another task can run (and possibly signal it) -- cooperative, mirrors
 * a single SPU's task scheduling; with LBP_WS_DRAIN a genuinely unsignalled
 * task drains and releases. Opt-in; default keeps concurrent per-thread SPU. */
static int spu_serial_enabled(void)
{ static int _s = -1; PS3_KNOB(_s, getenv("LBP_SPU_SERIAL") ? 1 : 0); return _s; }
#ifdef _WIN32
static SRWLOCK s_spu_serial = SRWLOCK_INIT;
void spu_serial_acquire(void){ if (spu_serial_enabled()) AcquireSRWLockExclusive(&s_spu_serial); }
void spu_serial_release(void){ if (spu_serial_enabled()) ReleaseSRWLockExclusive(&s_spu_serial); }
#else
static pthread_mutex_t s_spu_serial = PTHREAD_MUTEX_INITIALIZER;
void spu_serial_acquire(void){ if (spu_serial_enabled()) pthread_mutex_lock(&s_spu_serial); }
void spu_serial_release(void){ if (spu_serial_enabled()) pthread_mutex_unlock(&s_spu_serial); }
#endif

/* Find-or-create the slot for a (taskset, task) pair (directory lock held
 * briefly). Per-task keying is load-bearing: see spu_ts_ls_slot.taskid. */
static spu_ts_ls_slot* ts_ls_get(uint32_t taskset_ea, uint32_t taskid)
{
    spu_ts_ls_slot* slot = NULL;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_ts_ls_dir);
#else
    pthread_mutex_lock(&s_ts_ls_dir);
#endif
    for (int i = 0; i < SPU_TS_LS_MAX && !slot; i++)
        if (s_ts_ls[i].taskset_ea == taskset_ea && s_ts_ls[i].taskid == taskid)
            slot = &s_ts_ls[i];
    for (int i = 0; i < SPU_TS_LS_MAX && !slot; i++)
        if (s_ts_ls[i].taskset_ea == 0) {
            s_ts_ls[i].taskset_ea = taskset_ea;
            s_ts_ls[i].taskid = taskid;
#ifdef _WIN32
            InitializeSRWLock(&s_ts_ls[i].lock);
#else
            pthread_mutex_init(&s_ts_ls[i].lock, NULL);
#endif
            slot = &s_ts_ls[i];
        }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_ts_ls_dir);
#else
    pthread_mutex_unlock(&s_ts_ls_dir);
#endif
    return slot;
}

/* Minimal taskset scheduling for tasks the PPU never created.
 *
 * SPU code can create tasks itself, or mark existing ones ready, and rely on
 * the taskset policy module to schedule them. Tasks created from the PPU are
 * dispatched by our cellSpursCreateTask, but nothing looked at the taskset
 * afterwards, so SPU-scheduled work never ran. Guitar Hero III's Havok task
 * hands the rest of each physics step to other tasks and EXITs, and the step
 * then waits forever.
 *
 * Runs when a task of the taskset returns (our runner returns on the EXIT
 * syscall; waiting tasks park instead). The returning task's ready, pending
 * and enabled bits are cleared, as the real policy does on exit, and every
 * other enabled task that is pending or ready and not already running here is
 * started from its ELF.
 *
 * Taskset layout: bitsets of 16 bytes at +0x10 ready, +0x20 pending,
 * +0x30 enabled (task 0 = MSB of byte 0); task_info[t] at +0x80 + 0x30*t with
 * the ELF EA at +0x14 and the context save EA at +0x1C. */
#define TS_TRACK 32
static struct { uint32_t ea; uint8_t running[16]; } s_ts_run[TS_TRACK];
static volatile LONG s_ts_lock;
/* The lines s_ts_run holds, as a 256-way filter on the line address (set,
 * never cleared, like the entries): spu_taskset_line_written runs for every
 * committed SPU lock-line and looks in s_ts_run only on a hit. */
static volatile uint8_t s_ts_lines[256];
static uint8_t* ts_running(uint32_t taskset_ea)
{
    for (int i = 0; i < TS_TRACK; i++) if (s_ts_run[i].ea == taskset_ea) return s_ts_run[i].running;
    for (int i = 0; i < TS_TRACK; i++) if (!s_ts_run[i].ea) {
        s_ts_lines[(taskset_ea >> 7) & 255u] = 1;   /* before the entry: see above */
        s_ts_run[i].ea = taskset_ea; return s_ts_run[i].running;
    }
    return NULL;
}
static void ts_lock(void)   { while (InterlockedExchange(&s_ts_lock, 1)) Sleep(0); }
static void ts_unlock(void) { InterlockedExchange(&s_ts_lock, 0); }

static void spu_taskset_mark(uint32_t taskset_ea, uint32_t t, int on)
{
    if (!taskset_ea || t >= 128) return;
    ts_lock();
    uint8_t* r = ts_running(taskset_ea);
    uint8_t m = (uint8_t)(0x80u >> (t & 7));
    if (r) { if (on) r[t / 8] |= m; else r[t / 8] &= (uint8_t)~m; }
    ts_unlock();
}

#if defined(_MSC_VER)
#define SPU_WL_TLS __declspec(thread)
#else
#define SPU_WL_TLS __thread
#endif
/* Per-thread taskset/task handoff for spu_workload_dispatch_async (see there). */
static SPU_WL_TLS uint32_t t_dispatch_ts, t_dispatch_id;
static SPU_WL_TLS int t_dispatch_valid;
/* The image this task thread runs, for spu_hist entries made below it. */
static SPU_WL_TLS int t_hist_image;
void spu_workload_set_dispatch_task(uint32_t taskset_ea, uint32_t taskid)
{
    t_dispatch_ts = taskset_ea; t_dispatch_id = taskid; t_dispatch_valid = 1;
}

/* Taskset lifetime: a generation per taskset EA, and a count of task threads
 * still alive for it.
 *
 * cellSpursShutdownTaskset means the kernel never resumes a waiting task of the
 * taskset again, and cellSpursJoinTaskset returns once none is running. Our
 * task threads parked in WAIT_SIGNAL outlived both. Demon's Souls creates a
 * Core.Res taskset, shuts it down, joins it and creates the next one AT THE
 * SAME EA; the stale image-1 tasks kept serving the re-initialised LFQueue and
 * libsync asserted in _cellSyncLFQueueCompletePushPointer.
 *
 * Every task thread records the generation it was started under. Shutdown (and
 * a re-create at the same EA) bumps the generation; a task that waits under an
 * older generation is stopped instead of resumed. */
#define TS_LIFE_MAX 32
static struct { uint32_t ea; volatile LONG gen; volatile LONG live; uint64_t ids; uint32_t htid[64]; } s_ts_life[TS_LIFE_MAX];
static SPU_WL_TLS uint32_t t_job_ts;
static SPU_WL_TLS LONG t_job_gen;

static int ts_life_idx(uint32_t taskset_ea)   /* ts_lock held */
{
    for (int i = 0; i < TS_LIFE_MAX; i++) if (s_ts_life[i].ea == taskset_ea) return i;
    for (int i = 0; i < TS_LIFE_MAX; i++)
        if (!s_ts_life[i].ea) { s_ts_life[i].ea = taskset_ea; return i; }
    return -1;
}

static LONG spu_taskset_gen(uint32_t taskset_ea)
{
    ts_lock();
    int i = ts_life_idx(taskset_ea);
    LONG g = i >= 0 ? s_ts_life[i].gen : 0;
    ts_unlock();
    return g;
}

/* Generation of a taskset without taking ts_lock (callers may hold it). An EA
 * with no entry yet is generation 0, which is what ts_life_idx creates. */
static LONG spu_taskset_gen_peek(uint32_t taskset_ea)
{
    if (!taskset_ea) return 0;
    for (int i = 0; i < TS_LIFE_MAX; i++)
        if (s_ts_life[i].ea == taskset_ea) return s_ts_life[i].gen;
    return 0;
}

/* Task thread start/end bookkeeping (called on the task's own thread). */
static void spu_taskset_job_begin(uint32_t taskset_ea, uint32_t taskid)
{
    t_job_ts = taskset_ea;
    if (!taskset_ea) return;
    ts_lock();
    int i = ts_life_idx(taskset_ea);
    if (i >= 0) {
        t_job_gen = s_ts_life[i].gen; s_ts_life[i].live++;
        if (taskid < 64) { s_ts_life[i].ids |= 1ull << taskid;
                           s_ts_life[i].htid[taskid] = (uint32_t)GetCurrentThreadId(); }
    }
    ts_unlock();
}

static void spu_taskset_job_end(uint32_t taskset_ea, uint32_t taskid)
{
    if (!taskset_ea) return;
    ts_lock();
    int i = ts_life_idx(taskset_ea);
    if (i >= 0 && s_ts_life[i].live > 0) s_ts_life[i].live--;
    if (i >= 0 && taskid < 64) s_ts_life[i].ids &= ~(1ull << taskid);
    ts_unlock();
}

/* 1 if the calling task thread belongs to a taskset that was shut down (or
 * re-created) after the task started. */
int spu_taskset_job_stale(void)
{
    return t_job_ts && spu_taskset_gen(t_job_ts) != t_job_gen;
}

/* SPURS_TS_TRACE=<hex taskset ea> (or 1 for every taskset): log each task
 * start, exit, signal and WAIT_SIGNAL of that taskset, timestamped, without
 * PS3_VERBOSE -- which is too slow to reach a level. Answers "did the tasks
 * that feed this PPU wait still run, and what did the last one do". */
static int ts_trace(uint32_t taskset_ea)
{
    static int init = 0; static uint32_t want = 0;
    if (!init) { const char* e = getenv("SPURS_TS_TRACE");
                 want = e ? (uint32_t)strtoul(e, NULL, 16) : 0; init = 1; }
    return want && taskset_ea && (want == 1 || want == taskset_ea);
}
#define TS_TRACE(ea, ...) do { if (ts_trace(ea)) { \
    extern unsigned long long ps3_ms_now(void); \
    fprintf(stderr, "[ts-trace] t=%llu ts=%08X ", ps3_ms_now(), (unsigned)(ea)); \
    fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while (0)

/* Start task `t` of a taskset from its task_info ELF (ts_lock held). */
static void ts_start_task_locked(uint32_t taskset_ea, uint8_t* ts, uint8_t* run,
                                 uint32_t t, const char* why)
{
    extern uint8_t* vm_base;
    extern uint32_t g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
    const uint8_t m = (uint8_t)(0x80u >> (t & 7));
    const uint8_t* ti = ts + 0x80 + 0x30 * t;
    uint32_t elf = ((uint32_t)ti[0x14] << 24) | ((uint32_t)ti[0x15] << 16) |
                   ((uint32_t)ti[0x16] << 8) | ti[0x17];
    uint32_t ctx = ((uint32_t)ti[0x1C] << 24) | ((uint32_t)ti[0x1D] << 16) |
                   ((uint32_t)ti[0x1E] << 8) | ti[0x1F];
    if (!elf) return;
    size_t sz = spu_elf_image_size(vm_base + elf, 2u * 1024 * 1024);
    if (!sz) return;
    spurs_bitset_clear(taskset_ea + CSTS_PENDING_READY, t);   /* pending -> started */
    run[t / 8] |= m;
    if (ps3_log_verbose()) fprintf(stderr, "[taskset] start task %u of taskset 0x%08X elf=0x%08X (%s)\n",
                                   t, taskset_ea, elf, why);
    TS_TRACE(taskset_ea, "start task %u elf=0x%08X (%s)\n", t, elf, why);
    g_ydkj_real_taskset_ea = taskset_ea; g_ydkj_real_taskid = t;
    spu_workload_set_dispatch_task(taskset_ea, t);
    spu_workload_dispatch_async(vm_base + elf, (uint32_t)sz, ctx);
}

static void spu_taskset_task_exited(uint32_t taskset_ea, uint32_t done_task)
{
    extern uint8_t* vm_base;
    TS_TRACE(taskset_ea, "task %u exited%s\n", done_task,
             spu_taskset_job_stale() ? " (stale taskset)" : "");
    if (spu_taskset_job_stale()) return;   /* its taskset is gone: start nothing */
    ts_lock();
    uint8_t* ts = vm_base + taskset_ea;
    uint8_t* run = ts_running(taskset_ea);
    if (done_task < 128) {
        uint8_t m = (uint8_t)(0x80u >> (done_task & 7));
        /* Atomic against the SPU (spurs_bitset_clear): the same byte holds
         * tasks 0-7, and an SPU may be enabling one of them right now. */
        spurs_bitset_clear(taskset_ea + CSTS_READY, done_task);
        spurs_bitset_clear(taskset_ea + CSTS_PENDING_READY, done_task);
        spurs_bitset_clear(taskset_ea + CSTS_ENABLED, done_task);
        if (run) run[done_task / 8] &= (uint8_t)~m;
    }
    for (uint32_t t = 0; t < 128 && run; t++) {
        uint8_t m = (uint8_t)(0x80u >> (t & 7));
        if (!(ts[0x30 + t / 8] & m) || (run[t / 8] & m)) continue;
        if (!((ts[0x10 + t / 8] | ts[0x20 + t / 8]) & m)) continue;
        ts_start_task_locked(taskset_ea, ts, run, t, "after a task exited");
    }
    ts_unlock();
}

/* cellSpursCreateTask dispatches its task itself. Mark it running HERE, before
 * its thread exists: the thread marks itself only once it starts, and a task
 * of the same taskset exiting in between started every ready task not yet
 * marked -- this one a second time. Under load that repeats (Havok's taskset
 * reached 1009 task threads for 128 ids). */
void spu_taskset_mark_running(uint32_t taskset_ea, uint32_t taskid, int on)
{
    spu_taskset_mark(taskset_ea, taskid, on);
}

/* Make a taskset known before any of its tasks has run, so an SPU's write to
 * its header is recognised (spu_taskset_line_written). cellSpursCreateTaskset. */
void spu_taskset_created(uint32_t taskset_ea)
{
    if (!taskset_ea) return;
    ts_lock();
    (void)ts_running(taskset_ea);
    ts_unlock();
}

/* Tasks an SPU creates. SPU code can add a task to a taskset itself: Demon's
 * Souls' images 3 and 4 set a task's enabled bit and then its pending_ready bit
 * with two PUTLLCs on the taskset header -- what the PPU's cellSpursCreateTask
 * plus task start do. On hardware the taskset policy module then schedules it.
 * Nothing here did: pending tasks were started only when another task of the
 * same taskset exited, so these never ran, never exited, and held their ids
 * until every one of the 128 was taken ("no free task id").
 *
 * Called (from spu_taskset_line_committed) after a committed SPU lock-line
 * write. If the line is a known
 * taskset's header (the bitsets live in its first 128 bytes), start every
 * enabled task that is pending_ready and not running. Only PENDING tasks: a
 * ready one came from our PPU-side CreateTask, which dispatched it already. */
static void spu_taskset_line_written(uint32_t line_ea)
{
    extern uint8_t* vm_base;
    if (!s_ts_lines[(line_ea >> 7) & 255u]) return;
    int known = 0;
    for (int i = 0; i < TS_TRACK; i++) if (s_ts_run[i].ea == line_ea) { known = 1; break; }
    if (!known || !vm_base) return;
    uint8_t* ts = vm_base + line_ea;
    uint64_t p[2];
    memcpy(p, ts + 0x20, 16);
    if (!(p[0] | p[1])) return;
    ts_lock();
    uint8_t* run = ts_running(line_ea);
    for (uint32_t t = 0; t < 128 && run; t++) {
        uint8_t m = (uint8_t)(0x80u >> (t & 7));
        if (!(ts[0x30 + t / 8] & m) || !(ts[0x20 + t / 8] & m) || (run[t / 8] & m)) continue;
        ts_start_task_locked(line_ea, ts, run, t, "an SPU set it pending");
    }
    ts_unlock();
}

static void spu_async_run(spu_async_job* j)
{
    spu_taskset_job_begin(j->taskset_ea, j->taskid);
    /* The job belongs to the generation it was QUEUED under. A shutdown that
     * lands between the queueing and this thread starting would otherwise be
     * missed: the task would adopt the new generation, park in WAIT_SIGNAL and
     * hold up cellSpursJoinTaskset forever. Such a job never runs. */
    t_job_gen = j->gen;
    t_hist_image = j->image_id;
    if (spu_taskset_job_stale()) {
        spu_hist('N', j->image_id, j->taskset_ea, j->taskid, (uint32_t)j->gen);
        fprintf(stderr, "[spu_workload] task %u (taskset 0x%08X) not started: "
                "taskset was shut down after it was queued\n", j->taskid, j->taskset_ea);
        spu_taskset_job_end(j->taskset_ea, j->taskid);
        free(j);
        return;
    }
    spu_taskset_mark(j->taskset_ea, j->taskid, 1);
    spu_hist('S', j->image_id, j->taskset_ea, j->taskid, j->args_ea);
    TS_TRACE(j->taskset_ea, "task %u running image=%d\n", j->taskid, j->image_id);
    /* Persistent per-taskset LS: acquire (+serialize) the taskset's LS, or fall
     * back to a fresh per-dispatch LS when disabled / no taskset. */
    spu_ts_ls_slot* ts_slot = NULL;
    int did_load = 0;           /* whether THIS dispatch (re)loaded the image */
    if (spu_persist_ls_enabled() && j->taskset_ea && j->image_id != 22)
        ts_slot = ts_ls_get(j->taskset_ea, j->taskid);
    uint8_t* ls;
    if (ts_slot) {
#ifdef _WIN32
        AcquireSRWLockExclusive(&ts_slot->lock);   /* serialize this taskset */
#else
        pthread_mutex_lock(&ts_slot->lock);
#endif
        if (!ts_slot->ls) ts_slot->ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
        ls = ts_slot->ls;
        { static int _n = 0; if (_n++ < 24)
            fprintf(stderr, "[persist-ls] taskset=0x%08X task=%u image=%d %s\n",
                    j->taskset_ea, j->taskid, j->image_id,
                    ts_slot->loaded ? "REUSE (state retained)" : "first load"); fflush(stderr); }
    } else {
        ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    }
    if (ls) {
        uint32_t entry = 0;
        /* Load the image only on a taskset's FIRST task (or every dispatch when
         * not persisting) -- reusing preserves the prior task's LS state. */
        int loaded = (ts_slot && ts_slot->loaded)
                   ? 1
                   : (did_load = 1, spu_elf_load_to_ls(j->image, j->image_size, ls, &entry));
        if (ts_slot && did_load && loaded) ts_slot->loaded = 1;
        if (!loaded)
            fprintf(stderr, "[spu_workload] async image=%d ELF LOAD FAILED\n",
                    j->image_id), fflush(stderr);
        if (loaded) {
            /* YDKJ_CRI_POLICY (cri build experiment): the cri SPU task
             * (image 22) calls the SPURS task-API via a jump table at LS 0x2700
             * and reads its task descriptor at LS 0x2FB0 — both POLICY-provided,
             * not in the task ELF (so it branch-to-0's at func_00026DE0). Preload
             * the policy module (libsre @0x30021480 -> LS 0xA00) and write the
             * task descriptor {0xFFFFFFFF, 0x400, 0x2700, 0x3000} at 0x2FB0, then
             * run the cri task, to see how much further it gets. Diagnostic only. */
            if (j->image_id == 22 && getenv("YDKJ_CRI_TASKSET")) {
                /* cri build: load the TASKSET POLICY module (libsre 0x23680 ->
                 * LS 0xA00) alongside the cri task (already at 0x3000), then RUN
                 * THE POLICY ENTRY (tsp_spu_func_00000A00) under image 23. The
                 * policy builds the 0x2700 task-API table + dispatches the cri
                 * task. Needs the SpursKernelContext at LS[0x1C0] (instance ptr)
                 * + r80=0x100 context base, mirroring the kernel->policy handoff. */
                extern uint8_t* vm_base;
                extern void tsp_spu_func_00000A00(spu_context*);
                if (vm_base) memcpy(ls + 0xA00, vm_base + 0x30023680, 0x2200);
                /* Build the REAL SpursTasksetContext at LS 0x2700 (taskset ptr @0x27B8,
                 * TaskInfo @0x2780, syscallAddr @0x27C4) from the actual game taskset,
                 * so the policy's DMAs read valid data instead of garbage. The instance
                 * ptr (LS[0x1C0]) was a hardcoded 0x40009D00 that mismatched the game's
                 * real 0x40009F00 -> policy DMA'd garbage -> null branches. */
                extern uint64_t spurs_pm_build_context(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t);
                extern uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
                if (g_ydkj_real_taskset_ea)
                    spurs_pm_build_context(ls, g_ydkj_real_taskset_ea, g_ydkj_real_taskid, 0, 0);
                uint32_t inst = g_ydkj_real_spurs_ea ? g_ydkj_real_spurs_ea : 0x40009D00u;
                uint8_t* p = ls + 0x1C0;
                p[0]=0; p[1]=0; p[2]=0; p[3]=0;                 /* hi32 of u64 */
                p[4]=(uint8_t)(inst>>24); p[5]=(uint8_t)(inst>>16);
                p[6]=(uint8_t)(inst>>8);  p[7]=(uint8_t)inst;   /* lo32 */
                fprintf(stderr, "[cri] YDKJ_CRI_TASKSET: loaded taskset policy@0xA00, LS[0x1C0]=inst, running policy entry (img23)\n");
                fflush(stderr);
                /* Run the taskset policy entry instead of the cri task entry.
                 * (Declared by spu_lifted_job.h -- a local extern re-declaration
                 * here conflicts with the header's static inline under clang.) */
                int32_t prc = spu_run_lifted_job_abi(tsp_spu_func_00000A00, ls,
                                                     j->args_ea, 23, 1, j->have_r3 ? j->r3 : 0, 0);
                fprintf(stderr, "[cri] taskset policy RETURNED rc=%d\n", prc);
                fflush(stderr);
                free(ls); spu_taskset_job_end(j->taskset_ea, j->taskid); free(j); return;
            }
            /* YDKJ HLE cri-task path (image 22, cri_mpvps3spurs.elf): the real
             * SPURS kernel/policy would build the task's SpursTasksetContext (LS
             * 0x2700) before dispatch. In the HLE-cellSpurs path we dispatch the
             * task ELF directly, so LS 0x2700 is empty and the task branch-to-0's:
             * cri func_00026DE0 reads syscallAddr@0x27C4 (=0) and branches there.
             * Plant a minimal context so the task-API call lands on the HLE syscall
             * trampoline (LS 0xA70, intercepted in spu_channels.c) + the task
             * descriptor @0x2FB0. Adopted from JonathanDC64/ps3recomp (aaea4158).
             * Gate: default on for image 22 unless YDKJ_NO_CRI_CTX. */
            if (j->image_id == 22 && !getenv("YDKJ_NO_CRI_CTX")) {
                extern uint64_t spurs_pm_build_context(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t);
                extern uint32_t g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
                #define LSBE32(o,v) do{uint32_t _v=(v);ls[(o)+0]=(uint8_t)(_v>>24);ls[(o)+1]=(uint8_t)(_v>>16);ls[(o)+2]=(uint8_t)(_v>>8);ls[(o)+3]=(uint8_t)_v;}while(0)
                if (g_ydkj_real_taskset_ea && !getenv("YDKJ_MINIMAL_CTX")) {
                    /* REAL taskset context: build the SpursTasksetContext at LS 0x2700
                     * from the actual BE CellSpursTaskset (spurs ptr, args, TaskInfo)
                     * so the cri leaf reads valid data instead of my planted guesses. */
                    uint64_t elf = spurs_pm_build_context(ls, g_ydkj_real_taskset_ea, g_ydkj_real_taskid, 0, 0);
                    /* --- Golden-reference context fields for the CRI task, recovered from a
                     * WORKING RPCS3 SPU-LS snapshot via caner's rpcs3-guest-memory-dumper fork
                     * (github.com/canersaka/rpcs3-guest-memory-dumper): YDKJ BLUS30569, SPU0
                     * "CellSpursKernel0" mid-cri-decode. These are what the cri task's context
                     * validator (func_00026E80/F18/FC4) reads; our values differed and tripped
                     * its 0x80410911 error path. CRI-SCOPED (image 22 only) -- overriding these
                     * in the shared build_context breaks the audio SPURS task. */
                    LSBE32(0x2840, 0x53505552u); LSBE32(0x2844, 0x53544153u); /* moduleId "SPURSTASK MODULE" */
                    LSBE32(0x2848, 0x4B204D4Fu); LSBE32(0x284C, 0x44554C45u);
                    /* TI_LS_PATTERN = full-coverage (all-ones). The prior "golden fix"
                     * set this to 0 from the decode-time RPCS3 LS dump, but that region
                     * is post-validation (also zero) so it wasn't authoritative. Runtime
                     * probe YDKJ_CRI_GATE1TRACE proved 0 makes the validator func_00026E80
                     * return 0x8041090F (busy-spin poll); all-ones makes it return 0 -- the
                     * task then passes and reaches the taskset-state getllar (func_00027028).
                     * Verified by direct r3 read (YDKJ_G1_LSPAT=0xFFFFFFFF -> r3=0). */
                    LSBE32(0x27A0, 0xFFFFFFFFu); LSBE32(0x27A4, 0xFFFFFFFFu);
                    LSBE32(0x27A8, 0xFFFFFFFFu); LSBE32(0x27AC, 0xFFFFFFFFu);
                    LSBE32(0x27D0, 0x1F);                       /* dmaTagId = 0x1F (build_context wrote 0) */
                    /* still plant the cri-specific task descriptor @0x2FB0 that build_context
                     * doesn't cover (cri func_00026DE0 reads it). */
                    LSBE32(0x2FB0, 0xFFFFFFFFu); LSBE32(0x2FB4, 0x400);
                    LSBE32(0x2FB8, 0x2700);      LSBE32(0x2FBC, 0x3000);
                    /* GATE1 SWEEP (YDKJ_G1_*): the validator func_00026E80 reads the
                     * TI_CONTEXT(0x2798)/TI_LS_PATTERN(0x27A0) region; YDKJ_CRI_GATE1TRACE
                     * proved r16=rotqby(LS[0x2798],8)=0 forces its 0x8041090F return. The
                     * decode-time RPCS3 dump has this region zero (post-validation), so it
                     * can't be diffed -- sweep candidate values here and read the probe's
                     * r3. Overrides the plants above so one build tests many values. */
                    { const char* _e;
                      if ((_e=getenv("YDKJ_G1_LSPAT"))) { uint32_t v=(uint32_t)strtoul(_e,0,0);
                          LSBE32(0x27A0,v); LSBE32(0x27A4,v); LSBE32(0x27A8,v); LSBE32(0x27AC,v); }
                      if ((_e=getenv("YDKJ_G1_CTX")))   { uint32_t v=(uint32_t)strtoul(_e,0,0);
                          LSBE32(0x2798,v); LSBE32(0x279C,v); } }
                    fprintf(stderr, "[cri] REAL SpursTasksetContext built from taskset 0x%08X task %u (elf=0x%llX)\n",
                            g_ydkj_real_taskset_ea, g_ydkj_real_taskid, (unsigned long long)elf);
                } else {
                    LSBE32(0x27C0, 0x100);     /* kernelMgmtAddr -> SPURS kernel ctx @LS 0x100 */
                    LSBE32(0x27C4, 0xA70);     /* syscallAddr -> HLE PM syscall trampoline (0xA70) */
                    LSBE32(0x2FB0, 0xFFFFFFFFu); LSBE32(0x2FB4, 0x400);
                    LSBE32(0x2FB8, 0x2700);      LSBE32(0x2FBC, 0x3000);
                    fprintf(stderr, "[cri] HLE cri-task ctx (minimal plant): syscallAddr@0x27C4=0xA70\n");
                }
                #undef LSBE32
                fflush(stderr);
            }
            /* Dump the 64-byte decode context the cri task will DMA from eaContext
             * (r3.word1 = args_ea). This is the job the game's cri_mpv PPU layer is
             * supposed to populate (video-data EA, output buffer). If it's zero/garbage
             * the PPU layer never wrote a real job -> the task decodes nothing. */
            if (j->image_id == 22 && j->args_ea) {
                extern uint8_t* vm_base;
                const uint8_t* c = vm_base + (j->args_ea & 0x0FFFFFFFu);
                fprintf(stderr, "[cri] eaContext=0x%08X 64B decode-context:", j->args_ea);
                for (int i = 0; i < 64; i++) { if ((i&15)==0) fprintf(stderr,"\n     +%02X:", i); fprintf(stderr, " %02X", c[i]); }
                fprintf(stderr, "\n"); fflush(stderr);
            }
            /* Async dispatch is the SPURS-task path: the entry expects the SPURS
             * task kernel ABI in r3 ({0x40 marker, eaContext, queue EA, ...}),
             * captured at dispatch time (j->r3) so it doesn't race the PPU
             * overwriting the stack-allocated context. */
            /* Generic SPURS taskset task (LBP's audio SPEEX/MultiStream tasks,
             * image 6/7 — anything but the cri image 22 handled above): the real
             * kernel runs the task UNDER the taskset policy, which plants a
             * SpursTasksetContext at LS 0x2700 (taskset header, TaskInfo, and
             * syscallAddr=0xA70) and enters it with r3 = task args, r4 = {spurs,
             * taskset args}. We dispatch the task ELF directly, so without this it
             * runs with r3=r4=0 and no context and SPINS FOREVER (observed: image
             * 6/7 ENTER + RUN, never RETURN, 0 DMA, PPU blocks on EventFlag 0x0100).
             * Build the context here from the real taskset CreateTask recorded;
             * spu_run_lifted_job_abi then sets r3/r4 off the 0xA70 sentinel.
             * Env-gated while bringing this up — default leaves every path as-is. */
            /* Timing probe: a real SPURS task is dispatched when work is signaled.
             * We dispatch immediately at CreateTask. If LBP fills the task's work
             * buffer slightly LATER, delaying the run lets it see real data (=>
             * timing bug, fix = defer/re-dispatch on signal); if it still loops on
             * a null base, the buffer is never filled (=> an audio HLE gap). */
            if (j->image_id != 22) {
                const char* d = getenv("LBP_TASK_DELAY");
                if (d && *d) {
#ifdef _WIN32
                    Sleep((unsigned)(atoi(d) * 1000));
#endif
                }
            }
            /* A generic SPURS taskset task (image != cri 22) MUST get its
             * SpursTasksetContext planted at LS 0x2700 -- the real kernel builds
             * it from the taskset before entry, and the task reads its args and
             * every DMA base pointer out of it. Without it the task runs with a
             * null base and DMAs EVERYTHING from EA 0 (observed on LBP's FMOD
             * mixer: 105k GETs from ea=0x0, degenerate handler targets ->
             * branch-to-0, and the PPU pump blocks forever on EventFlag 0x0100).
             * Planting it makes the mixer read the real FMOD control block
             * (0x0094F5xx) and the EventFlag handshake completes. Default ON;
             * LBP_NO_TASKSET restores the old direct-dispatch for comparison. */
            if (j->image_id != 22 && !getenv("LBP_NO_TASKSET")) {
                extern uint64_t spurs_pm_build_context(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t);
                /* Use the taskset+taskid captured for THIS job at dispatch (not the
                 * globals, which the next CreateTask clobbers -- the race that made
                 * both audio tasks run task 1's descriptor). */
                if (j->taskset_ea) {
                    spurs_pm_build_context(ls, j->taskset_ea, j->taskid, 0, 0);
                    if (ps3_log_verbose()) fprintf(stderr, "[taskset] built SpursTasksetContext image=%d "
                            "taskset=0x%08X task=%u\n", j->image_id,
                            j->taskset_ea, j->taskid); fflush(stderr);
                }
            }
            /* ---- Bink-layer probe + real-policy A/B (image 3 = binkspu) ----
             * (1) dump binkspu's Bink work-descriptor at its arg EA so we can see
             *     the output-plane EAs and the plane+0x50 "needs-decode" gate that
             *     gates the frame-output PUT (all prior PUTs went to the 0x927E80
             *     control block, never a plane). This tells us whether the missing
             *     output EA is a Bink/PPU-layer field (this descriptor) or a SPURS
             *     taskset-context field (LS 0x2700, below).
             * (2) LBP_REAL_POLICY: run the REAL lifted taskset policy to build its
             *     SpursTasksetContext and diff it against the C reimpl build_context
             *     planted in `ls` -- shows exactly which fields differ. */
            /* Image gate: 3 is LBP's binkspu. Other titles reach the same
             * taskset-policy A/B through their own image -- Demon's Souls's
             * event-flag producer is image 12 -- so make it selectable instead of
             * hardcoded. REAL_POLICY_IMG=<n> overrides; default 3 keeps LBP's
             * behaviour exactly. (fork: DeS needs 12) */
            int _rp_img = 3;
            { static int _rpi = -2;
              if (_rpi == -2) { const char* e = getenv("REAL_POLICY_IMG");
                                _rpi = e ? atoi(e) : -1; }
              if (_rpi >= 0) _rp_img = _rpi; }
            if (j->image_id == _rp_img) {
                extern uint8_t* vm_base;
                uint32_t arg = j->args_ea;
                /* SPU DMA maps EAs as vm_base + (uint32_t)ea with NO masking
                 * (spu_dma.h:186); vm_base spans the full 4GB so VRAM 0xC0000000+
                 * is directly addressable. (The earlier &0x0FFFFFFF mask read main
                 * code at 0x10B88 instead of the real VRAM descriptor.) */
                #define RDBE32(base, off) (((uint32_t)(base)[(off)]<<24)|((uint32_t)(base)[(off)+1]<<16)| \
                                           ((uint32_t)(base)[(off)+2]<<8)|(base)[(off)+3])
                if (vm_base && arg >= 0x10000) {
                    const uint8_t* d = vm_base + arg;         /* FULL EA, no mask */
                    fprintf(stderr, "[bink-desc] arg=0x%08X 256B work-descriptor:", arg);
                    uint32_t words[64];
                    for (int i = 0; i < 256; i += 16) {
                        fprintf(stderr, "\n   +%03X:", i);
                        for (int k = 0; k < 16; k += 4) {
                            uint32_t w = RDBE32(d, i+k); words[(i+k)/4] = w;
                            fprintf(stderr, " %08X", w);
                        }
                    }
                    fprintf(stderr, "\n");
                    /* Follow each plausible pointer one level: dump +0x00..+0x60 so
                     * the output-plane EA + the plane+0x50 "needs-decode" gate show. */
                    for (int wi = 0; wi < 64; wi++) {
                        uint32_t p = words[wi];
                        if (p < 0x10000 || (p >= 0x50000000u && p < 0xC0000000u)) continue;
                        const uint8_t* q = vm_base + p;
                        fprintf(stderr, "  [desc+%02X]=0x%08X -> +00:%08X +40:%08X +50:%08X(gate) +54:%08X\n",
                                wi*4, p, RDBE32(q,0), RDBE32(q,0x40), RDBE32(q,0x50), RDBE32(q,0x54));
                    }
                    fflush(stderr);
                }
                if (getenv("LBP_REAL_POLICY") && j->taskset_ea) {
                    extern int spurs_run_taskset_policy_probe(uint32_t,uint32_t,uint32_t,
                                                              uint64_t,uint32_t,uint8_t*,uint32_t);
                    /* spurs EA from the taskset header lo32 @ +0x64 (be64 @0x60). */
                    uint32_t ts = j->taskset_ea;
                    uint32_t spurs_ea = 0;
                    if (vm_base) { const uint8_t* t = vm_base + ts;
                        spurs_ea = RDBE32(t, 0x64);
                        /* Why did the policy exit without dispatching? Dump the task
                         * bitsets it reads (task N = bit 127-N; task 2 = bit 125). */
                        fprintf(stderr, "[taskset-bits] RUNNING=%08X%08X READY=%08X%08X ENABLED=%08X%08X SIGNALLED=%08X%08X WAITING=%08X%08X\n",
                                RDBE32(t,0x00),RDBE32(t,0x04), RDBE32(t,0x10),RDBE32(t,0x14),
                                RDBE32(t,0x30),RDBE32(t,0x34), RDBE32(t,0x40),RDBE32(t,0x44),
                                RDBE32(t,0x50),RDBE32(t,0x54)); }
                    uint8_t real2700[0x180];
                    int st = spurs_run_taskset_policy_probe(ts, j->taskid, spurs_ea,
                                                            (uint64_t)ts, 3, real2700, sizeof real2700);
                    fprintf(stderr, "[real-pm] status=0x%X -- LS 0x2700 diff (build_context vs real policy):\n", st);
                    for (uint32_t o = 0; o < sizeof real2700; o += 4) {
                        uint32_t bc = ((uint32_t)ls[0x2700+o]<<24)|((uint32_t)ls[0x2700+o+1]<<16)|
                                      ((uint32_t)ls[0x2700+o+2]<<8)|ls[0x2700+o+3];
                        uint32_t rp = ((uint32_t)real2700[o]<<24)|((uint32_t)real2700[o+1]<<16)|
                                      ((uint32_t)real2700[o+2]<<8)|real2700[o+3];
                        if (bc != rp)
                            fprintf(stderr, "   0x%04X: build=%08X real=%08X\n", 0x2700+o, bc, rp);
                    }
                    /* EMPIRICAL TEST (LBP_POLICY_CTX): overlay the REAL policy's
                     * SpursTasksetContext onto binkspu's LS 0x2700, replacing the C
                     * reimpl, then let binkspu run with it. If the movie plane fills
                     * (not green) the policy's fuller ctx is the fix (H1); if still
                     * green, the frame-output gate is Bink-layer (H2). Keeps the
                     * build_context 0x100 kernel ctx (moduleId "TK") intact. */
                    if (getenv("LBP_POLICY_CTX")) {
                        memcpy(ls + 0x2700, real2700, sizeof real2700);
                        fprintf(stderr, "[real-pm] OVERLAID policy ctx onto binkspu LS 0x2700 (LBP_POLICY_CTX)\n");
                    }
                    fflush(stderr);
                }
                #undef RDBE32
            }
            spu_serial_acquire();       /* one SPU task runs at a time (LBP_SPU_SERIAL) */
            int32_t rc = spu_run_lifted_job_abi(j->fn, ls, j->args_ea, j->image_id,
                                                1, j->have_r3 ? j->r3 : 0, 0);
            spu_hist('R', j->image_id, j->taskset_ea, j->taskid, (uint32_t)rc);
            /* YDKJ_CRI_RESUME: a real SPURS task is PERSISTENT -- on yield (num=0)
             * the kernel re-enters it when work is signaled. Our HLE runs it once,
             * so it polls the (concurrently PPU-updated) eaContext, finds no work,
             * yields and exits before the PPU marks work-ready. Approximate the
             * resume by re-running the task (fresh context re-read from eaContext)
             * in a bounded loop until it actually DECODES (a >256B video GET sets
             * g_cri_video_dma) or we time out. This does NOT fake data: the task
             * only decodes if the PPU genuinely populated real work meanwhile. */
            if (j->image_id == 22 && getenv("YDKJ_CRI_RESUME") && !g_cri_video_dma) {
                for (int attempt = 0; attempt < 400 && !g_cri_video_dma; attempt++) {
#ifdef _WIN32
                    Sleep(3);
#else
                    /* The pause is the point: it is the window in which the
                     * PPU marks work ready. Without it the 400 attempts are
                     * spent before the producer runs at all. */
                    { struct timespec _d = { 0, 3 * 1000 * 1000 }; nanosleep(&_d, NULL); }
#endif
                    memset(ls, 0, SPU_LS_SIZE);
                    uint32_t e2 = 0;
                    if (!spu_elf_load_to_ls(j->image, j->image_size, ls, &e2)) break;
                    rc = spu_run_lifted_job_abi(j->fn, ls, j->args_ea, j->image_id,
                                                1, j->have_r3 ? j->r3 : 0, 0);
                    if (g_cri_video_dma) {
                        fprintf(stderr, "[cri] RESUME: cri task DECODED real video (attempt %d)\n", attempt);
                        break;
                    }
                }
                if (!g_cri_video_dma)
                    fprintf(stderr, "[cri] RESUME: no work-ready after 400 polls (PPU never marked work)\n");
                fflush(stderr);
            }
            /* SPU_TASK_RESUME=<img>[:<attempts>[:<ms>]] -- generalised persistent
             * re-entry, the same idea as YDKJ_CRI_RESUME above but not hardwired
             * to image 22.
             *
             * A real SPURS task is PERSISTENT: the kernel re-enters it whenever
             * work is signalled. Our HLE dispatches it ONCE at cellSpursCreateTask,
             * so a task created before the PPU has anything to give it finds an
             * empty queue and exits, and nothing ever runs it again.
             *
             * Demon's Souls's graphics dispatcher (image 2) is exactly that: it
             * reads its 256-byte descriptor at 0x01B51000 and the command buffer,
             * finds the work list empty (its entries are self-pointers), exits --
             * and HighGraphics then submits a command block and spins ~930k times
             * at func_00ACAFA8+0xF4 waiting for a completion word no one will
             * write, because the only task that could write it has already exited.
             *
             * Re-running reloads the ELF into a fresh local store and re-reads the
             * task context from guest memory, so a re-entry sees whatever the PPU
             * has submitted in the meantime. It fabricates nothing: the task only
             * does work if the PPU genuinely queued some. Bounded, and off unless
             * set, so the default build is unchanged. */
            { static int s_tr = -2; static int s_tr_img, s_tr_att = 400, s_tr_ms = 3;
              if (s_tr == -2) { const char* e = getenv("SPU_TASK_RESUME");
                  s_tr = 0;
                  if (e && *e) { char* end = 0;
                      s_tr_img = (int)strtoul(e, &end, 0); s_tr = 1;
                      if (end && *end == ':') { s_tr_att = (int)strtoul(end + 1, &end, 0);
                          if (end && *end == ':') s_tr_ms = (int)strtoul(end + 1, 0, 0); } } }
              if (s_tr && s_tr_img == j->image_id) {
                  fprintf(stderr, "[task-resume] image=%d: re-entering up to %dx every %dms\n",
                          j->image_id, s_tr_att, s_tr_ms);
                  for (int attempt = 0; attempt < s_tr_att; attempt++) {
#ifdef _WIN32
                      Sleep((DWORD)s_tr_ms);
#endif
                      memset(ls, 0, SPU_LS_SIZE);
                      uint32_t e2 = 0;
                      if (!spu_elf_load_to_ls(j->image, j->image_size, ls, &e2)) break;
                      rc = spu_run_lifted_job_abi(j->fn, ls, j->args_ea, j->image_id,
                                                  1, j->have_r3 ? j->r3 : 0, 0);
                      /* Publish the completion word after EACH re-entry, not just
                       * once at the end: the PPU waiter clears it and re-submits
                       * every iteration, so a single publish only ever buys one
                       * iteration. (SPU_DONE_WORD below documents the word.) */
                      { extern uint8_t* vm_base;
                        const char* de = getenv("SPU_DONE_WORD");
                        if (de && *de && vm_base) {
                            uint32_t dea = (uint32_t)strtoul(de, 0, 16);
                            if (dea) { const uint8_t* d = vm_base + dea;
                                uint32_t endp = ((uint32_t)d[0]<<24)|((uint32_t)d[1]<<16)|
                                                ((uint32_t)d[2]<<8)|(uint32_t)d[3];
                                uint8_t* w = vm_base + dea + 0x0C;
                                w[0]=(uint8_t)(endp>>24); w[1]=(uint8_t)(endp>>16);
                                w[2]=(uint8_t)(endp>>8);  w[3]=(uint8_t)endp; } } }
                  }
                  fprintf(stderr, "[task-resume] image=%d: done re-entering\n", j->image_id);
                  fflush(stderr);
              } }

            /* YDKJ_CRI_WAKE probe: on cri task (image 22) completion, wake any PPU
             * completion-waiter (the SPU->PPU cellSpursEventFlag completion isn't
             * propagated yet). Tests whether the game then advances to draw content. */
            if (j->image_id == 22 && getenv("YDKJ_CRI_WAKE")) {
                extern void ydkj_wake_all_event_flags(void);
                ydkj_wake_all_event_flags();
                fprintf(stderr, "[cri] YDKJ_CRI_WAKE: woke all event-flag waiters on cri completion\n");
            }
            /* SPU_DONE_WORD=<descEA hex>[:<img>] -- DIAGNOSTIC BISECT, NOT A FIX.
             *
             * Demon's Souls's HighGraphics thread submits a command block, then
             * spins usleep(30) at func_00ACAFA8+0xF4 (over a MILLION times)
             * waiting for a completion word to go non-zero:
             *
             *   desc = 0x01B50FC4:  +0x00 = end pointer (0x45A4B320)
             *                       +0x0C = completion word (stays 0)
             *
             * The SPU graphics dispatcher (image 2) demonstrably READS the work --
             * it GETs the 256-byte descriptor at 0x01B51000 and the command buffer
             * at 0x45A4B280/0x45A4B290 -- and writes the descriptor back, but never
             * writes that completion word. Publishing "consumed up to the end
             * pointer" on task completion tests whether that word is the whole
             * remaining gate for the graphics path, exactly as the taskset
             * round-notify was for the audio path. Off unless set.
             * Example: SPU_DONE_WORD=01B50FC4:2 */
            { static int s_dw = -2; static uint32_t s_dw_ea; static int s_dw_img = -1;
              if (s_dw == -2) { const char* e = getenv("SPU_DONE_WORD");
                  s_dw = 0;
                  if (e && *e) { char* end = 0;
                      s_dw_ea = (uint32_t)strtoul(e, &end, 16);
                      if (end && *end == ':') s_dw_img = (int)strtoul(end + 1, 0, 0);
                      s_dw = s_dw_ea ? 1 : 0; } }
              extern uint8_t* vm_base;
              if (s_dw && (s_dw_img < 0 || s_dw_img == j->image_id) && vm_base) {
                  const uint8_t* d = vm_base + s_dw_ea;
                  uint32_t endp = ((uint32_t)d[0]<<24)|((uint32_t)d[1]<<16)|
                                  ((uint32_t)d[2]<<8)|(uint32_t)d[3];
                  uint8_t* w = vm_base + s_dw_ea + 0x0C;
                  w[0]=(uint8_t)(endp>>24); w[1]=(uint8_t)(endp>>16);
                  w[2]=(uint8_t)(endp>>8);  w[3]=(uint8_t)endp;
                  static int _n = 0; if (_n++ < 12)
                      fprintf(stderr, "[done-word] image=%d done -> [0x%08X+0x0C]=0x%08X (bisect)\n",
                              j->image_id, s_dw_ea, endp);
              } }

            /* The PM's other completion duty: signal the lv2 event queues SPURS
             * attached, so a PPU thread blocked in sys_event_queue_receive waiting
             * for this SPU work actually wakes. Only fires for queues with a
             * blocked receiver (see spurs_notify_task_completion). */
            { extern void spurs_notify_task_completion(void);
              if (!spu_taskset_job_stale()) spurs_notify_task_completion(); }

            /* SPU_LS_DUMP=<prefix>: write this job's whole local store when it
             * ends. A reference dump from real hardware is a full local store,
             * so the only way to ask "what ELSE is different" -- rather than
             * checking one guessed word at a time -- is to diff all 256 KB. */
            { const char* pfx = getenv("SPU_LS_DUMP");
              if (pfx) { char path[512];
                  snprintf(path, sizeof path, "%s_img%d.bin", pfx, j->image_id);
                  FILE* f = fopen(path, "wb");
                  if (f) { fwrite(ls, 1, SPU_LS_SIZE, f); fclose(f);
                      fprintf(stderr, "[spu_workload] wrote local store -> %s\n", path); } } }
            if (ps3_log_verbose()) fprintf(stderr, "[spu_workload] async image=%d RETURNED rc=%d "
                    "(job ran to completion, did not loop)\n", j->image_id, rc);
            spu_serial_release();
            if (j->taskset_ea) spu_taskset_task_exited(j->taskset_ea, j->taskid);
        }
        /* Persistent LS is retained in its taskset slot for the next task; a
         * per-dispatch LS is freed. */
        if (!ts_slot) free(ls);
    }
    if (ts_slot) {
#ifdef _WIN32
        ReleaseSRWLockExclusive(&ts_slot->lock);
#else
        pthread_mutex_unlock(&ts_slot->lock);
#endif
    }
    spu_taskset_job_end(j->taskset_ea, j->taskid);
    spu_hist('X', j->image_id, j->taskset_ea, j->taskid, 0);
    free(j);
}

#ifdef _WIN32
/* Async job workers.
 *
 * Every async dispatch used to create a host thread and let it exit with the
 * job. Demon's Souls runs ~650 such jobs a second (Havok image 7, image 10,
 * images 3-6), so that was ~650 CreateThread calls a second on the
 * dispatching PPU threads, a fresh stack to fault in for each job's 260 KB
 * stack-local spu_context, and every job starting on cold caches.
 *
 * A finished worker now parks on its own event and the next dispatch hands it
 * a job. There is no queue and no cap: a dispatch with no idle worker creates
 * one, exactly as before, so a job never waits for another to finish (tasks
 * block in WAIT_SIGNAL on each other). Idle workers exit after 5 s, or at
 * once beyond 64 idle. SPU_ASYNC_POOL=0 creates a thread per job again.
 * Image 22 (the cri task) always gets its own: its EXIT handling counts calls
 * in a thread-local. */
typedef struct spu_async_worker {
    HANDLE ev;                        /* auto-reset: a job was handed over */
    spu_async_job* job;
    struct spu_async_worker* next;    /* idle list */
} spu_async_worker;
static SRWLOCK s_aw_lock = SRWLOCK_INIT;
static spu_async_worker* s_aw_idle;
static int s_aw_idle_n;
#define SPU_AW_MAX_IDLE 64
#define SPU_AW_IDLE_MS  5000

static void spu_async_run_one(spu_async_job* j)
{
    spu_async_run(j);
    /* Nothing on this stack may stay in the lock-line reserver set: the
     * job's stack-local contexts are gone once it returns. */
    { ULONG_PTR lo, hi; GetCurrentThreadStackLimits(&lo, &hi);
      extern void spu_coh_forget_range(uintptr_t, uintptr_t);
      spu_coh_forget_range((uintptr_t)lo, (uintptr_t)hi); }
}

/* Take an idle worker off the list, or NULL. */
static spu_async_worker* spu_aw_take(void)
{
    AcquireSRWLockExclusive(&s_aw_lock);
    spu_async_worker* w = s_aw_idle;
    if (w) { s_aw_idle = w->next; s_aw_idle_n--; }
    ReleaseSRWLockExclusive(&s_aw_lock);
    return w;
}

/* Park a worker that finished its job. Returns 0 if it should exit instead. */
static int spu_aw_park(spu_async_worker* w)
{
    AcquireSRWLockExclusive(&s_aw_lock);
    if (s_aw_idle_n >= SPU_AW_MAX_IDLE) { ReleaseSRWLockExclusive(&s_aw_lock); return 0; }
    w->next = s_aw_idle; s_aw_idle = w; s_aw_idle_n++;
    ReleaseSRWLockExclusive(&s_aw_lock);
    if (WaitForSingleObject(w->ev, SPU_AW_IDLE_MS) == WAIT_OBJECT_0) return 1;
    /* Timed out: leave the list, unless a dispatcher took us off it in the
     * meantime -- then its job is on the way and the event will fire. */
    int removed = 0;
    AcquireSRWLockExclusive(&s_aw_lock);
    for (spu_async_worker** pp = &s_aw_idle; *pp; pp = &(*pp)->next)
        if (*pp == w) { *pp = w->next; s_aw_idle_n--; removed = 1; break; }
    ReleaseSRWLockExclusive(&s_aw_lock);
    if (removed) return 0;
    WaitForSingleObject(w->ev, INFINITE);
    return 1;
}

static DWORD WINAPI spu_async_thread(LPVOID p) {
    /* Reserve handler headroom so a stack overflow in lifted SPU code (e.g. a
     * runaway brsl recursion) can actually reach the STACKOVERFLOW reporter
     * instead of killing the process silently with 0x80000001. */
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }
    spu_async_worker* w = (spu_async_worker*)p;
    for (;;) {
        spu_async_job* j = w->job;
        w->job = NULL;
        const int pooled = j->image_id != 22;
        spu_async_run_one(j);
        if (!pooled || !spu_aw_park(w)) break;
    }
    CloseHandle(w->ev);
    free(w);
    return 0;
}

static int spu_async_pool_on(void)
{
    static int s_on = -1;
    PS3_KNOB(s_on, ps3_env_int("SPU_ASYNC_POOL", 1));
    return s_on;
}

/* Run j on an idle worker, or on a new thread. 0 if no thread could be made. */
static int spu_async_start(spu_async_job* j)
{
    spu_async_worker* w = (j->image_id != 22 && spu_async_pool_on()) ? spu_aw_take() : NULL;
    if (w) { w->job = j; SetEvent(w->ev); return 1; }
    w = (spu_async_worker*)calloc(1, sizeof *w);
    if (!w) return 0;
    w->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!w->ev) { free(w); return 0; }
    w->job = j;
    /* 256MB RESERVE (not commit): the Bink SPU decoder's per-macroblock brsl
     * chains are deep host call chains with fat lifted frames -- a 1MB stack
     * blew the guard page (STATUS_GUARD_PAGE_VIOLATION 0x80000001, silent
     * process death) the moment the real decode path first executed. */
    HANDLE th = CreateThread(NULL, 256u << 20, spu_async_thread, w,
                             STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!th) { CloseHandle(w->ev); free(w); return 0; }
    CloseHandle(th);   /* detached */
    return 1;
}
#else
static void* spu_async_thread(void* p) { spu_async_run((spu_async_job*)p); return NULL; }
#endif

int spu_workload_dispatch_job(const uint8_t* image, uint32_t image_size,
                              uint32_t job_ea, uint32_t job_desc_size)
{
    if (!image || image_size == 0) return 0;

    uint64_t fp = spu_workload_fingerprint(image, image_size);
    spu_lifted_entry_fn fn = NULL;
    int image_id = 0;
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fp) { fn = s_registry[i].fn; image_id = s_registry[i].image_id; break; }
    if (!fn) {
        /* Capped: an unlifted job a title dispatches every frame (Tornado
         * Outbreak's Wwise audio jobs) wrote 2 GB of these in three minutes,
         * and the logging itself distorts guest timing. */
        { static unsigned long long miss_n = 0;
          if (++miss_n <= 32 || miss_n % 100000 == 0)
            fprintf(stderr, "[spurs-job] dispatch MISS fp=0x%016llX size=%u job=0x%08X (#%llu)\n",
                    (unsigned long long)fp, image_size, job_ea, miss_n); }
        /* SPU_DUMP_MISS=<dir>: write the unrecognised image out so it can be
         * lifted and registered. SPURS job binaries are raw code+data blobs
         * the title loads from its own data files -- unlike sys_spu_image
         * programs they are NOT embedded ELFs in the EBOOT, so
         * tools/extract_spu_images.py cannot find them and this is the only
         * place the bytes exist. One file per fingerprint. */
        { const char* _dir = getenv("SPU_DUMP_MISS");
          if (_dir && *_dir) {
              static uint64_t _seen[64]; static unsigned _nseen = 0;
              unsigned _k = 0;
              for (; _k < _nseen; _k++) if (_seen[_k] == fp) break;
              if (_k == _nseen && _nseen < 64) {
                  char _path[512];
                  _seen[_nseen++] = fp;
                  snprintf(_path, sizeof(_path), "%s/spujob_%016llX_%u.bin",
                           _dir, (unsigned long long)fp, image_size);
                  FILE* _f = fopen(_path, "wb");
                  if (_f) {
                      fwrite(image, 1, image_size, _f);
                      fclose(_f);
                      fprintf(stderr, "[spurs-job] dumped -> %s\n", _path);
                  }
              }
          } }
        return 0;
    }
    /* Two lines plus two explicit flushes PER JOB. A title running its audio
     * chain does ~1500 jobs/s, so this alone was ~6k writes/s through one FILE
     * lock, and it starved the guest threads competing for that lock badly
     * enough to change what the title does (see ps3_log.h). Milestones stay;
     * the per-job pair is verbose-only. */
    spu_count_run(image_id);
    int verbose = ps3_log_verbose();
    if (verbose) {
        fprintf(stderr, "[spurs-job] dispatch HIT fp=0x%016llX image=%d job=0x%08X\n",
                (unsigned long long)fp, image_id, job_ea);
        fflush(stderr);
    }
    int rc = spu_run_spurs_job(fn, image_id, job_ea, job_desc_size);
    if (verbose) {
        fprintf(stderr, "[spurs-job] job 0x%08X RETURNED rc=%d\n", job_ea, rc);
        fflush(stderr);
    }
    return 1;
}

/* ---- SPURS task signal channel (real WAIT_SIGNAL semantics) --------------
 *
 * Mirrors the kernel/RPCS3 contract (spursTasksetProcessSyscall):
 *   - _cellSpursSendSignal / cellSpursEventFlagSet mark the task's bit in the
 *     taskset's `signalled` bitset (BE, in the GUEST CellSpursTaskset) and
 *     wake it if sleeping.
 *   - The WAIT_SIGNAL taskset syscall consumes a pending signal, or sleeps
 *     the task until one arrives (POLL_SIGNAL-then-wait, no lost wakeups:
 *     the guest bit is set under the same lock the waiter checks it).
 * The bitset is guest state (SPU-visible); the host lock/condvar exist only
 * to block and wake the task's host thread. */

#ifdef _WIN32
static SRWLOCK            s_sig_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_sig_cv;   /* zero-init == CONDITION_VARIABLE_INIT */
#else
static pthread_mutex_t s_sig_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_sig_cv   = PTHREAD_COND_INITIALIZER;
#endif

/* Which (taskset, task) pairs are currently parked in WAIT_SIGNAL.
 *
 * A PPU->SPU event-flag Set must be able to reach a task that parked WITHOUT
 * registering a wait slot in the flag: our Set path only signalled tasks listed
 * in the flag`s `used` bitmask, so such a task slept forever while the producer`s
 * memory looked perfectly intact. Same failure class canersaka documented for
 * SPURS queues in Yakuza Dead Souls ("the HLE previously woke only its host
 * condvar, which lifted SPU consumer tasks never wait on").
 *
 * ponytail: deliberately lock-free and racy. A missed entry costs one 1s poll
 * timeout in the waiter; a stale entry costs one spurious signal, which is safe
 * because signals are LATCHED in guest state (CSTS_SIGNALLED) and every wait
 * loop re-checks its own predicate. Add a lock only if this ever needs to be
 * authoritative rather than a hint. */
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);  /* defined below */

#define SPU_PARKED_MAX 64
static volatile uint64_t s_parked[SPU_PARKED_MAX];   /* (taskset<<32)|(taskId+1), 0 = free */
static volatile uint32_t s_parked_obj[SPU_PARKED_MAX];  /* LS[0x2FD0] wait object, 0 = unknown */

/* The lines holding a parked task's signalled bitset, counted in a 256-way
 * filter on the line address, so a PPU store-conditional (every guest lock
 * takes one) tests one word before it looks at the parked list. */
volatile long g_spu_parked_lines[256];
static unsigned parked_line_slot(uint32_t taskset_ea)
{
    return ((taskset_ea + CSTS_SIGNALLED) >> 7) & 255u;
}

void spu_taskset_parked_add(uint32_t taskset_ea, uint32_t taskId, uint32_t wait_obj)
{
    uint64_t key = ((uint64_t)taskset_ea << 32) | (uint64_t)(taskId + 1);
    for (int i = 0; i < SPU_PARKED_MAX; i++)
        if (s_parked[i] == 0) {
            __sync_add_and_fetch(&g_spu_parked_lines[parked_line_slot(taskset_ea)], 1);
            s_parked_obj[i] = wait_obj; s_parked[i] = key; return;
        }
}

void spu_taskset_parked_del(uint32_t taskset_ea, uint32_t taskId)
{
    uint64_t key = ((uint64_t)taskset_ea << 32) | (uint64_t)(taskId + 1);
    for (int i = 0; i < SPU_PARKED_MAX; i++)
        if (s_parked[i] == key) {
            s_parked[i] = 0;
            __sync_sub_and_fetch(&g_spu_parked_lines[parked_line_slot(taskset_ea)], 1);
            return;
        }
}

/* Event bits owed to a task, keyed by the OBJECT it waits on (flag - 0x80).
 *
 * Writing the bits at Set time loses a race: the guest reads them from
 * object+0x30 immediately on resuming, and a task that took the latched-wake
 * path was never parked for the Set to target. So the Set only RECORDS what is
 * owed, and the WAIT_SIGNAL handler writes it into guest memory just before it
 * hands control back -- at which point the guest cannot yet have read it. */
#define SPU_EFBITS_MAX 16
static volatile uint32_t s_efbits_obj[SPU_EFBITS_MAX];
static volatile uint32_t s_efbits_val[SPU_EFBITS_MAX];

void spu_ef_bits_post(uint32_t obj, uint16_t bits)
{
    if (!obj) return;
    for (int i = 0; i < SPU_EFBITS_MAX; i++)
        if (s_efbits_obj[i] == obj) { s_efbits_val[i] |= bits; return; }
    for (int i = 0; i < SPU_EFBITS_MAX; i++)
        if (s_efbits_obj[i] == 0) { s_efbits_val[i] = bits; s_efbits_obj[i] = obj; return; }
}

/* Take (and clear) whatever is owed to `obj`. 0 = nothing pending. */
uint16_t spu_ef_bits_take(uint32_t obj)
{
    if (!obj) return 0;
    for (int i = 0; i < SPU_EFBITS_MAX; i++)
        if (s_efbits_obj[i] == obj) {
            uint16_t v = (uint16_t)s_efbits_val[i];
            s_efbits_obj[i] = 0; s_efbits_val[i] = 0;
            return v;
        }
    return 0;
}

/* A direction-2 Set that finds NOBODY parked yet is a lost wakeup: the task
 * parks a moment later and sleeps through the signal it was meant to get. Latch
 * one pending wake per taskset; the next task to park consumes it instead of
 * sleeping. One bit, not a counter -- SPURS signals are latched, not queued, so
 * two Sets before a park must not buy two passes through the wait. */
#define SPU_WAKE_LATCH_MAX 16
static volatile uint32_t s_wake_latch[SPU_WAKE_LATCH_MAX];   /* taskset EA, 0 = free */

void spu_taskset_latch_wake(uint32_t taskset_ea)
{
    if (!taskset_ea) return;
    for (int i = 0; i < SPU_WAKE_LATCH_MAX; i++)
        if (s_wake_latch[i] == taskset_ea) return;          /* already latched */
    for (int i = 0; i < SPU_WAKE_LATCH_MAX; i++)
        if (s_wake_latch[i] == 0) { s_wake_latch[i] = taskset_ea; return; }
}

/* Consume a latched wake for this taskset. Returns 1 if one was pending. */
int spu_taskset_consume_wake(uint32_t taskset_ea)
{
    if (!taskset_ea) return 0;
    for (int i = 0; i < SPU_WAKE_LATCH_MAX; i++)
        if (s_wake_latch[i] == taskset_ea) { s_wake_latch[i] = 0; return 1; }
    return 0;
}

/* Signal every task of `taskset_ea` currently parked. Returns how many. */
/* Wake only the task parked on `wait_obj` (0 = any task of the taskset).
 * The guest names its wait object in the WAIT_SIGNAL argument, and for YDKJ`s
 * CRI tasks the flag the PPU sets is exactly wait_obj + 0x80 -- so a Set can
 * address the one task that was waiting for it instead of every parked task. */
int spu_taskset_signal_parked_obj(uint32_t taskset_ea, uint32_t wait_obj)
{
    if (!taskset_ea) return 0;
    int n = 0;
    for (int i = 0; i < SPU_PARKED_MAX; i++) {
        uint64_t k = s_parked[i];
        if (!k || (uint32_t)(k >> 32) != taskset_ea) continue;
        if (wait_obj && s_parked_obj[i] && s_parked_obj[i] != wait_obj) continue;
        spu_taskset_signal_task(taskset_ea, (uint32_t)(k & 0xFFFFFFFFu) - 1u);
        n++;
    }
    return n;
}

/* Deliver a signal to a task (callable from any PPU/host thread). */
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId)
{
    if (!taskset_ea || taskId >= 128) return;
    spu_hist('G', t_hist_image, taskset_ea, taskId, 0);
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
    spurs_bitset_set(taskset_ea + CSTS_SIGNALLED, taskId);
    WakeAllConditionVariable(&s_sig_cv);
    ReleaseSRWLockExclusive(&s_sig_lock);
    TS_TRACE(taskset_ea, "signal task %u\n", taskId);
#else
    pthread_mutex_lock(&s_sig_lock);
    spurs_bitset_set(taskset_ea + CSTS_SIGNALLED, taskId);
    pthread_cond_broadcast(&s_sig_cv);
    pthread_mutex_unlock(&s_sig_lock);
#endif
    { static int _n = 0; if (_n++ < 24)
        fprintf(stderr, "[spu_workload] signal task %u (taskset 0x%08X)\n",
                taskId, taskset_ea); fflush(stderr); }
    /* SPU_SIG_STATS=1: signals per second per (taskset, task, caller). */
    { static int s_on = -1; PS3_KNOB(s_on, getenv("SPU_SIG_STATS") ? 1 : 0);
      if (s_on) {
          static uint32_t ts[32], tk[32]; static uintptr_t ca[32]; static unsigned cnt[32];
          static unsigned long long t0;
          extern unsigned long long ps3_ms_now(void);
          const uintptr_t c = (uintptr_t)__builtin_return_address(0);
          for (int i = 0; i < 32; i++) {
              if (cnt[i] && ts[i] == taskset_ea && tk[i] == taskId && ca[i] == c) { cnt[i]++; break; }
              if (!cnt[i] && !ts[i]) { ts[i] = taskset_ea; tk[i] = taskId; ca[i] = c; cnt[i] = 1; break; }
          }
          unsigned long long now = ps3_ms_now();
          if (!t0) t0 = now;
          if (now - t0 >= 5000) {
              for (int i = 0; i < 32 && ts[i]; i++) {
                  fprintf(stderr, "[sig-stats] taskset=0x%08X task=%u caller=%p %.1f/s\n",
                          ts[i], tk[i], (void*)ca[i], cnt[i] * 1000.0 / (now - t0));
                  cnt[i] = 0; ts[i] = 0;
              }
              t0 = now;
          }
      } }
}

/* A successful PUTLLC/PUTLLUC committed the 128-byte guest line `line`.
 *
 * A task signalling another task does it on the SPU, as libspurs does: a
 * GETLLAR/PUTLLC loop that sets the target's bit in the taskset's signalled
 * bitset. That updates the guest bit the waiter checks but never touches
 * s_sig_cv, so the parked task only saw it when its 1 s timed wait ran out.
 * Twisted Metal's post-FX pipeline chains its six tasks this way several
 * times a frame and took 4 s or more per frame. Wake the waiters whenever a
 * line holding a parked taskset's signalled bitset is committed; a commit
 * that did not signal them only costs a spurious wake, which the wait loop
 * re-checks. */
void spu_taskset_line_committed(uint32_t line)
{
    spu_taskset_line_written(line);
    /* Every committed SPU lock-line comes here; the parked list matters only
     * for a line g_spu_parked_lines counts. */
    if (!g_spu_parked_lines[(line >> 7) & 255u]) return;
    for (int i = 0; i < SPU_PARKED_MAX; i++) {
        const uint64_t k = s_parked[i];
        if (!k || (((uint32_t)(k >> 32) + CSTS_SIGNALLED) & ~127u) != line) continue;
#ifdef _WIN32
        AcquireSRWLockExclusive(&s_sig_lock);
        WakeAllConditionVariable(&s_sig_cv);
        ReleaseSRWLockExclusive(&s_sig_lock);
#else
        pthread_mutex_lock(&s_sig_lock);
        pthread_cond_broadcast(&s_sig_cv);
        pthread_mutex_unlock(&s_sig_lock);
#endif
        return;
    }
}

/* A PPU store-conditional committed at `ea` (ppu_stwcx32/ppu_stdcx64), on a
 * line the filter says may hold a parked task's signalled bitset.
 *
 * The PPU signals a task the same way an SPU does when the code doing it is
 * the real libsre rather than our cellSpursSendSignal: an lwarx/stwcx. loop
 * on the bitset. Demon's Souls's resource decompressor pushes each block to
 * its SPU task with libsre's cellSpursQueuePush (run as lifted code), which
 * signals the task that way. Nothing woke the parked task, so it saw its bit
 * only when some other signal broadcast the condition variable: the audio
 * mixer's, about every 10 ms. Every block of a level load waited for that.
 *
 * Wake only when a parked task's own bit is set. Other words share the line,
 * and the PPU takes store-conditionals on them all the time while a level is
 * running, each of which woke every parked task for nothing.
 * SPU_PPU_SIGNAL_WAKE=0 (live) turns the wake off, for A/B. */
void spu_taskset_ppu_committed(uint32_t ea)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("SPU_PPU_SIGNAL_WAKE", 1));
    if (!on) return;
    const uint32_t line = ea & ~127u;
    for (int i = 0; i < SPU_PARKED_MAX; i++) {
        const uint64_t k = s_parked[i];
        if (!k || (((uint32_t)(k >> 32) + CSTS_SIGNALLED) & ~127u) != line) continue;
        if (!spurs_bitset_test((uint32_t)(k >> 32) + CSTS_SIGNALLED, (uint32_t)k - 1u))
            continue;
#ifdef _WIN32
        AcquireSRWLockExclusive(&s_sig_lock);
        WakeAllConditionVariable(&s_sig_cv);
        ReleaseSRWLockExclusive(&s_sig_lock);
#else
        pthread_mutex_lock(&s_sig_lock);
        pthread_cond_broadcast(&s_sig_cv);
        pthread_mutex_unlock(&s_sig_lock);
#endif
        return;
    }
}

/* WAIT_SIGNAL from the task side (runs ON the task's host thread, called by
 * the 0xA70 taskset-syscall intercept). Consumes a pending signal or blocks
 * until one is delivered. Returns 0 (the syscall's success rc). */
int spu_taskset_wait_signal(uint32_t taskset_ea, uint32_t taskId)
{
    if (!taskset_ea || taskId >= 128) return 0;
    TS_TRACE(taskset_ea, "task %u WAIT_SIGNAL\n", taskId);
    spu_hist('W', t_hist_image, taskset_ea, taskId, 0);
    /* ran= is the wall time this task spent OUT of the wait (decode/mix work
     * per round) -- decisive for "is the Bink decode CPU-bound". TLS: each
     * task runs on its own host thread. */
    static _Thread_local unsigned long long s_wait_exit_ms;
    { extern unsigned long long ps3_ms_now(void);
      unsigned long long _now = ps3_ms_now();
      static int _n = 0; if (_n++ < 200 || (_n % 500) == 0)
        fprintf(stderr, "[spu_workload] WAIT_SIGNAL#%d enter task=%u taskset=0x%08X ran=%llums\n",
                _n, taskId, taskset_ea,
                s_wait_exit_ms ? (_now - s_wait_exit_ms) : 0ull); }
    /* LBP_SYNC_ACK: taskset sync-lane bookkeeping. On real SPURS the taskset
     * POLICY MODULE advances the per-SPU progress lanes (sync+0x40+16*row:
     * ticket u16 @+0, consumer lanes @+2..) as tasks are processed; our HLE
     * runs the task on a dedicated thread and re-runs the PM ~never, so the
     * lanes froze and the skip path's min-of-lanes barrier (sub_4D3730) hung
     * forever. A task ENTERING WAIT_SIGNAL has by definition drained all work
     * dispatched to it, so advancing the active lanes to the published ticket
     * here is semantically the PM's bookkeeping, batched. Validation gate. */
    if (getenv("LBP_SYNC_ACK")) {
        extern uint32_t g_barrier_sync_watch;
        uint32_t b = g_barrier_sync_watch;
        if (b) {
            for (int row = 0; row < 4; row++) {
                uint32_t p = b + 0x40 + 16u * (uint32_t)row;
                extern uint8_t* vm_base;
                uint16_t ticket = (uint16_t)((vm_base[p] << 8) | vm_base[p+1]);
                for (int lane = 0; lane < 7; lane++) {
                    uint32_t la = p + 2 + 2u * (uint32_t)lane;
                    uint16_t cur = (uint16_t)((vm_base[la] << 8) | vm_base[la+1]);
                    if (cur != 0xFFFF && cur < ticket) {
                        vm_base[la]   = (uint8_t)(ticket >> 8);
                        vm_base[la+1] = (uint8_t)ticket;
                    }
                }
            }
            { static int _a = 0; if (_a++ < 8)
                fprintf(stderr, "[sync-ack] lanes advanced to tickets (sync=0x%08X)\n", b); }
        }
    }
    unsigned secs = 0;
    /* LBP_WS_DRAIN=<secs>: EXPERIMENT (general work-drained approximation).
     * A task ENTERING WAIT_SIGNAL has drained the work dispatched to it. In our
     * one-shot SPU model the producer task ran to completion already, so the
     * signal never arrives and the consumer blocks forever (movie/audio/loading
     * deadlock). If set, after <secs> of no real signal we RETURN rc=0 as if the
     * kernel resumed the task with "no pending work" -- lets it run its post-wait
     * path (complete its loop / set completion flags) instead of hanging. 0/unset
     * keeps the block-forever behaviour. Reversible, opt-in. */
    unsigned drain = 0; { const char* e = getenv("LBP_WS_DRAIN"); if (e) { drain = (unsigned)atoi(e); if (!drain) drain = 1; } }
    int drained = 0, killed = 0;
    /* SPURS_ROUND_NOTIFY=<flagEA hex>[:<bits>] -- DIAGNOSTIC BISECT, NOT A FIX.
     *
     * A task entering WAIT_SIGNAL has by definition finished the round it was
     * signalled for. On real SPURS the taskset POLICY MODULE does the completion
     * bookkeeping at that point, including setting the event flag a PPU waiter is
     * blocked on. We HLE the taskset PM in C (spurs_pm_build_context) and never
     * run its notify logic, so nothing ever sets the flag -- established for
     * Demon's Souls: its SPU audio mixer (image 12) completes a full mixing round
     * and writes PCM to the cellAudio ring, yet issues NO atomic, NO mailbox and
     * NO taskset syscall other than WAIT_SIGNAL itself, while MOMixerThread stays
     * blocked in cellSpursEventFlagWait on 0x01CE7800 with bits=0x0000 forever.
     *
     * Setting the flag here tests exactly that diagnosis. It is a bisect because
     * the real PM decides WHICH flag and WHICH bits from taskset state we do not
     * model; this just applies the ones named in the env. Off unless set. */
    { static int s_rn = -1; static uint32_t s_rn_ea; static unsigned s_rn_bits;
      if (s_rn < 0) { const char* e = getenv("SPURS_ROUND_NOTIFY");
          s_rn = 0; s_rn_ea = 0; s_rn_bits = 1;
          if (e && *e) { char* end = 0;
              s_rn_ea = (uint32_t)strtoul(e, &end, 16);
              if (end && *end == ':') s_rn_bits = (unsigned)strtoul(end + 1, 0, 0);
              s_rn = s_rn_ea ? 1 : 0; } }
      if (s_rn) {
          extern void spurs_ef_set_from_spu(uint32_t, uint16_t);
          static int _n = 0; if (_n++ < 12)
              fprintf(stderr, "[round-notify] task %u finished a round -> set flag "
                      "0x%08X bits=0x%04X\n", taskId, s_rn_ea, (unsigned)s_rn_bits);
          spurs_ef_set_from_spu(s_rn_ea, (uint16_t)s_rn_bits);
      } else {
          /* Default path: let the event-flag layer work out WHICH flag and WHICH
           * bits from real guest state (flag->taskset binding, direction, and the
           * blocked waiter's own mask) instead of taking them from the env. This
           * is the taskset policy module's completion bookkeeping, which our C
           * HLE of the PM otherwise never performs. */
          extern void spurs_ef_notify_taskset_round(uint32_t);
          spurs_ef_notify_taskset_round(taskset_ea);
      } }

    /* Release the global SPU-serial lock while parked so another SPU task can
     * run (and possibly signal us); re-acquired before returning to resume. */
    spu_serial_release();
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
    while (!spurs_bitset_test(taskset_ea + CSTS_SIGNALLED, taskId)) {
        if (spu_taskset_job_stale()) { killed = 1; break; }
        if (!SleepConditionVariableSRW(&s_sig_cv, &s_sig_lock, 1000, 0)) {
            static int _n = 0;
            if (++secs && _n < 16) { _n++;
                fprintf(stderr, "[spu_workload] task %u (taskset 0x%08X) sleeping "
                        "%us in WAIT_SIGNAL\n", taskId, taskset_ea, secs); fflush(stderr); }
            if (drain && secs >= drain) { drained = 1; break; }
        }
    }
    if (!drained && !killed) spurs_bitset_clear(taskset_ea + CSTS_SIGNALLED, taskId);
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
    while (!spurs_bitset_test(taskset_ea + CSTS_SIGNALLED, taskId)) {
        if (spu_taskset_job_stale()) { killed = 1; break; }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
        if (pthread_cond_timedwait(&s_sig_cv, &s_sig_lock, &ts) != 0) {
            static int _n = 0;
            if (++secs && _n < 16) { _n++;
                fprintf(stderr, "[spu_workload] task %u (taskset 0x%08X) sleeping "
                        "%us in WAIT_SIGNAL\n", taskId, taskset_ea, secs); fflush(stderr); }
            if (drain && secs >= drain) { drained = 1; break; }
        }
    }
    if (!drained && !killed) spurs_bitset_clear(taskset_ea + CSTS_SIGNALLED, taskId);
    pthread_mutex_unlock(&s_sig_lock);
#endif
    if (drained) { static int _d = 0; if (_d++ < 24)
        fprintf(stderr, "[spu_workload] task %u (taskset 0x%08X) WS_DRAIN resume "
                "after %us (no signal)\n", taskId, taskset_ea, secs); fflush(stderr); }
    if (killed) {
        fprintf(stderr, "[spu_workload] task %u (taskset 0x%08X) stopped in WAIT_SIGNAL: "
                "taskset was shut down\n", taskId, taskset_ea); fflush(stderr);
        spu_serial_acquire();   /* the task's runner releases it on the way out */
        return -1;              /* caller halts the task */
    }
    spu_serial_acquire();       /* resume: re-take the serial lock */
    spu_hist('w', t_hist_image, taskset_ea, taskId, (uint32_t)secs);
    { extern unsigned long long ps3_ms_now(void);
      s_wait_exit_ms = ps3_ms_now(); }   /* stamp for the next entry's ran= */
    return 0;
}

/* cellSpursShutdownTaskset / a re-create at the same EA: no waiting task of
 * the current generation is ever resumed again (see s_ts_life). */
void spu_taskset_shutdown(uint32_t taskset_ea)
{
    if (!taskset_ea) return;
    ts_lock();
    int i = ts_life_idx(taskset_ea);
    if (i >= 0) s_ts_life[i].gen++;
    uint8_t* r = ts_running(taskset_ea);
    if (r) memset(r, 0, 16);
    ts_unlock();
    spu_taskset_consume_wake(taskset_ea);
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
    WakeAllConditionVariable(&s_sig_cv);
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
    pthread_cond_broadcast(&s_sig_cv);
    pthread_mutex_unlock(&s_sig_lock);
#endif
}

/* cellSpursJoinTaskset: wait (bounded) until no task thread of the taskset is
 * still running. Returns how many are still alive at the deadline. */
/* Host-side view of a taskset for diagnostics: task threads alive, how many
 * of them are parked in WAIT_SIGNAL, and the ids (< 64) they run. */
void spu_taskset_report(uint32_t taskset_ea)
{
    ts_lock();
    int i = ts_life_idx(taskset_ea);
    LONG live = i >= 0 ? s_ts_life[i].live : 0;
    uint64_t ids = i >= 0 ? s_ts_life[i].ids : 0;
    ts_unlock();
    int parked = 0;
    for (int k = 0; k < SPU_PARKED_MAX; k++)
        if (s_parked[k] && (uint32_t)(s_parked[k] >> 32) == taskset_ea) parked++;
    fprintf(stderr, "[spu_workload] taskset 0x%08X: %ld task threads alive, %d parked, ids<64=0x%llX\n",
            taskset_ea, (long)live, parked, (unsigned long long)ids);
}

int spu_taskset_join(uint32_t taskset_ea, unsigned timeout_ms)
{
    extern unsigned long long ps3_ms_now(void);
    unsigned long long t0 = ps3_ms_now();
    for (;;) {
        ts_lock();
        int i = ts_life_idx(taskset_ea);
        LONG live = i >= 0 ? s_ts_life[i].live : 0;
        uint64_t ids = i >= 0 ? s_ts_life[i].ids : 0;
        uint32_t htid = 0;
        for (int k = 0; k < 64 && i >= 0; k++) if (ids >> k & 1) { htid = s_ts_life[i].htid[k]; break; }
        ts_unlock();
        unsigned long long el = ps3_ms_now() - t0;
        if (!live || el >= timeout_ms) {
            if (live || el >= 100) {
                int parked = 0;
                for (int k = 0; k < SPU_PARKED_MAX; k++)
                    if (s_parked[k] && (uint32_t)(s_parked[k] >> 32) == taskset_ea) parked++;
                fprintf(stderr, "[spu_workload] join taskset 0x%08X: %ld live (ids=0x%llX, "
                        "%d parked, host tid %u) after %llums\n", taskset_ea, (long)live,
                        (unsigned long long)ids, parked, htid, el);
            }
            return (int)live;
        }
#ifdef _WIN32
        Sleep(1);
#else
        { struct timespec d = { 0, 1000000 }; nanosleep(&d, NULL); }
#endif
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
        /* SPU_DUMP_MISS also applies here. The sync path has written the
         * unrecognised image out for a while and the async path did not, so a
         * workload policy module -- which is always dispatched async, being a
         * persistent worker -- could only ever report a fingerprint you had no
         * way to resolve. These blobs are not ELFs in the EBOOT, so this dump
         * is the only place their bytes exist. */
        { const char* _dir = getenv("SPU_DUMP_MISS");
          if (_dir && *_dir) {
              static uint64_t _seen[64]; static unsigned _nseen = 0;
              unsigned _k = 0;
              for (; _k < _nseen; _k++) if (_seen[_k] == fp) break;
              if (_k == _nseen && _nseen < 64) {
                  char _path[512];
                  _seen[_nseen++] = fp;
                  snprintf(_path, sizeof(_path), "%s/spujob_%016llX_%u.bin",
                           _dir, (unsigned long long)fp, image_size);
                  FILE* _f = fopen(_path, "wb");
                  if (_f) { fwrite(image, 1, image_size, _f); fclose(_f);
                      fprintf(stderr, "[spu_workload] wrote %s\n", _path); }
              }
          } }
        fprintf(stderr, "[spu_workload] async dispatch MISS fp=0x%016llX size=%u\n",
                (unsigned long long)fp, image_size);
        return 0;
    }

    spu_async_job* j = (spu_async_job*)malloc(sizeof(*j));
    if (!j) return 0;
    j->image = image; j->image_size = image_size; j->args_ea = args_ea;
    j->fn = fn; j->image_id = image_id;
    spu_count_run(image_id);
    /* Capture the taskset+taskid NOW (PPU thread, right after cellSpursCreateTask
     * set the globals for THIS task). Reading them later in the async thread races
     * the next CreateTask overwriting the single-slot globals -- with two audio
     * tasks that made both run task 1's descriptor. */
    { extern uint32_t g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
      j->taskset_ea = g_ydkj_real_taskset_ea; j->taskid = g_ydkj_real_taskid;
      /* ...but the globals are process-wide, and with several tasksets live
       * (Demon's Souls: Havok image 7 on one, image 1 on another) a PPU
       * CreateTask and an SPU thread starting the next task both write them
       * between another dispatcher's set and this read. That ran image 7 with
       * image 1's taskset context (misaligned GETs from 0x48BAxxxx). The two
       * dispatch sites hand THIS thread's pair over explicitly; prefer it. */
      if (t_dispatch_valid) { j->taskset_ea = t_dispatch_ts; j->taskid = t_dispatch_id;
                              t_dispatch_valid = 0; } }
    j->gen = spu_taskset_gen_peek(j->taskset_ea);
    spu_hist('D', image_id, j->taskset_ea, j->taskid, args_ea);
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

    if (ps3_log_verbose()) fprintf(stderr,
        "[spu_workload] dispatch HIT (async) fp=0x%016llX args=0x%08X image=%d -> spawning thread\n",
        (unsigned long long)fp, args_ea, image_id);
    if (args_ea) { extern uint8_t* vm_base; const uint8_t* c = vm_base + args_ea;
        static int _d=0; if (_d++ < 1) {
            /* Dump a larger window of the task context buffer + scan for any word
             * that looks like the LS[0xBEC0] target (i.e. a small LS-range value),
             * to see if the kernel-restored LS data lives here (real game data). */
            /* The policy module restores the task context from a save buffer into
             * LS 0xB200; LS[0xBEC0] = ctxbuf + 0xCC0. Check the heap-EA candidates
             * in the descriptor for a save buffer whose +0xCC0/+0xCC8 holds a
             * small LS pointer (a valid P for LS[0xBEC8]). */
            uint32_t cand[5];
            cand[0]=((uint32_t)c[0x14]<<24)|((uint32_t)c[0x15]<<16)|((uint32_t)c[0x16]<<8)|c[0x17];
            cand[1]=((uint32_t)c[0x1C]<<24)|((uint32_t)c[0x1D]<<16)|((uint32_t)c[0x1E]<<8)|c[0x1F];
            cand[2]=((uint32_t)c[0x98]<<24)|((uint32_t)c[0x99]<<16)|((uint32_t)c[0x9A]<<8)|c[0x9B];
            cand[3]=((uint32_t)c[0xB8]<<24)|((uint32_t)c[0xB9]<<16)|((uint32_t)c[0xBA]<<8)|c[0xBB];
            cand[4]=((uint32_t)c[0x16C]<<24)|((uint32_t)c[0x16D]<<16)|((uint32_t)c[0x16E]<<8)|c[0x16F];
            for (int ci=0; ci<5; ci++) {
                uint32_t ea=cand[ci]; if (!ea || ea>=0x10000000) continue;
                const uint8_t* b = vm_base + ea + 0xCC0;
                fprintf(stderr, "[ctxbuf cand 0x%08X +0xCC0]:", ea);
                for (int k=0;k<0x20;k+=4) {
                    uint32_t w=((uint32_t)b[k]<<24)|((uint32_t)b[k+1]<<16)|((uint32_t)b[k+2]<<8)|b[k+3];
                    fprintf(stderr, " %08X%s", w, (w>0&&w<0x40000)?"<LS":"");
                }
                fprintf(stderr, "\n");
            } } }

#ifdef _WIN32
    if (!spu_async_start(j)) { free(j); return 0; }
#else
    /* Same 256 MB reservation, for the same reason: pthread stacks are
     * mmap'd and committed on touch, so this reserves address space rather
     * than memory. The default is 512 KB on Darwin, half the 1 MB that blew
     * the guard page above. */
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int prc = EINVAL;
    if (pthread_attr_setstacksize(&attr, 256u << 20) == 0)
        prc = pthread_create(&th, &attr, spu_async_thread, j);
    pthread_attr_destroy(&attr);
    /* A host that will not reserve that much still gets the thread, on the
     * default stack: a shallow SPU job runs there, and a deep one dying
     * beats never dispatching at all. */
    if (prc != 0)
        prc = pthread_create(&th, NULL, spu_async_thread, j);
    if (prc != 0) { free(j); return 0; }
    pthread_detach(th);
#endif
    return 1;
}
