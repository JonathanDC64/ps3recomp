/*
 * ps3recomp - the platform layer on Linux and macOS (see plat.h)
 *
 * The calls that take more than a line. Where the two systems differ (the
 * stack range, the clock a timed wait uses) the difference is here and
 * nowhere else. Empty on Windows, where plat.h has everything inline.
 */
#ifndef _WIN32

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* pthread_getattr_np */
#endif

#include "plat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- A CPU-time sampler, for test runs ----------------------------------- */

/* PS3_SAMPLE=<file>,<start s>,<seconds>: where the process's CPU time goes on
 * a machine with no profiler to hand (a Steam Deck). <start s> after the call,
 * for <seconds>, each tick that finds the process on a CPU notes the thread,
 * its instruction and the word on top of its stack (a leaf's caller, usually).
 * Then <file> gets the memory map ("M " lines), the notes ("S tid pc" and the
 * top 8 words of the stack), the thread names ("T tid name") and the nearest
 * exported name of each instruction seen ("P pc name"), for
 * tools/des/sample_read.py. Off unless set. */
#if defined(__linux__) && defined(__x86_64__)
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>

typedef struct { uint64_t pc, w[8]; uint32_t tid; } plat_sample;
#define PLAT_SAMPLE_CAP (1u << 18)
static plat_sample* s_samples;
static atomic_uint s_sample_n;
static char s_sample_file[1024];
static unsigned s_sample_start, s_sample_secs;

static void sample_on_tick(int sig, siginfo_t* si, void* uc_)
{
    (void)sig; (void)si;
    const int saved = errno;
    const ucontext_t* uc = (const ucontext_t*)uc_;
    const unsigned i = atomic_fetch_add_explicit(&s_sample_n, 1, memory_order_relaxed);
    if (i < PLAT_SAMPLE_CAP) {
        s_samples[i].pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
        const uint64_t* sp = (const uint64_t*)uc->uc_mcontext.gregs[REG_RSP];
        for (int k = 0; k < 8; k++) s_samples[i].w[k] = sp[k];
        s_samples[i].tid = (uint32_t)syscall(SYS_gettid);
    }
    errno = saved;
}

static int sample_pc_order(const void* a, const void* b)
{
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

static void sample_write_names(FILE* out, unsigned n)
{
    uint64_t* pcs = (uint64_t*)malloc((n ? n : 1) * sizeof *pcs);
    if (pcs) {
        for (unsigned i = 0; i < n; i++) pcs[i] = s_samples[i].pc;
        qsort(pcs, n, sizeof *pcs, sample_pc_order);
        for (unsigned i = 0; i < n; i++) {
            if (i && pcs[i] == pcs[i - 1]) continue;
            Dl_info di;
            if (dladdr((void*)pcs[i], &di) && di.dli_sname)
                fprintf(out, "P %llx %s\n", (unsigned long long)pcs[i], di.dli_sname);
        }
        free(pcs);
    }
    DIR* d = opendir("/proc/self/task");
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            char path[64], name[32] = "";
            if (e->d_name[0] == '.' || strlen(e->d_name) > 16) continue;
            snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
            FILE* f = fopen(path, "r");
            if (!f) continue;
            if (fgets(name, sizeof name, f)) fprintf(out, "T %s %s", e->d_name, name);
            fclose(f);
        }
        closedir(d);
    }
}

static void* sample_thread(void* arg)
{
    (void)arg;
    sigset_t only;
    sigemptyset(&only);
    sigaddset(&only, SIGPROF);
    pthread_sigmask(SIG_BLOCK, &only, NULL);
    sleep(s_sample_start);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sample_on_tick;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0) return NULL;
    struct itimerval every = {{0, 1000}, {0, 1000}}, off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_PROF, &every, NULL);
    sleep(s_sample_secs);
    setitimer(ITIMER_PROF, &off, NULL);
    FILE* out = fopen(s_sample_file, "w");
    if (!out) return NULL;
    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[512];
        while (fgets(line, sizeof line, maps)) fprintf(out, "M %s", line);
        fclose(maps);
    }
    unsigned n = atomic_load(&s_sample_n);
    if (n > PLAT_SAMPLE_CAP) n = PLAT_SAMPLE_CAP;
    for (unsigned i = 0; i < n; i++) {
        fprintf(out, "S %u %llx", s_samples[i].tid, (unsigned long long)s_samples[i].pc);
        for (int k = 0; k < 8; k++) fprintf(out, " %llx", (unsigned long long)s_samples[i].w[k]);
        fputc('\n', out);
    }
    sample_write_names(out, n);
    fclose(out);
    fprintf(stderr, "[sample] %u samples in %s\n", n, s_sample_file);
    return NULL;
}

void plat_sampler_start(void)
{
    const char* v = getenv("PS3_SAMPLE");
    if (!v || !*v || s_samples) return;
    const char* c1 = strchr(v, ',');
    const char* c2 = c1 ? strchr(c1 + 1, ',') : NULL;
    if (!c2 || (size_t)(c1 - v) >= sizeof s_sample_file) return;
    memcpy(s_sample_file, v, (size_t)(c1 - v));
    s_sample_start = (unsigned)atoi(c1 + 1);
    s_sample_secs = (unsigned)atoi(c2 + 1);
    s_samples = (plat_sample*)malloc(PLAT_SAMPLE_CAP * sizeof *s_samples);
    if (!s_samples) return;
    pthread_t t;
    if (pthread_create(&t, NULL, sample_thread, NULL) == 0) pthread_detach(t);
}
#else
void plat_sampler_start(void) {}
#endif

/* PS3_TIMER_SLACK_US=<n> (test, Linux): how late a sleep may wake. The kernel's
 * default is 50 us. Threads inherit it, so it's set before any of them start. */
#if defined(__linux__)
#include <stdlib.h>
#include <sys/prctl.h>
__attribute__((constructor)) static void plat_timer_slack_knob(void)
{
    const char* v = getenv("PS3_TIMER_SLACK_US");
    if (v && *v) prctl(PR_SET_TIMERSLACK, (unsigned long)atoi(v) * 1000ul, 0, 0, 0);
}
#endif

/* ---- Threads ------------------------------------------------------------ */

static int start_detached(plat_thread_fn fn, void* arg, size_t stack)
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return 0;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = 0;
    if (stack) {
        /* Darwin rejects a size that is not a page multiple; both reject one
         * under the minimum. */
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        if (page == 0) page = 4096;
        if (stack < (size_t)PTHREAD_STACK_MIN) stack = (size_t)PTHREAD_STACK_MIN;
        stack = (stack + page - 1) & ~(page - 1);
        rc = pthread_attr_setstacksize(&attr, stack);
    }
    pthread_t th;
    if (rc == 0) rc = pthread_create(&th, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

int plat_thread_start_detached(plat_thread_fn fn, void* arg, size_t stack_reserve)
{
    if (start_detached(fn, arg, stack_reserve)) return 1;
    return stack_reserve != 0 && start_detached(fn, arg, 0);
}

/* A thread's stack never moves, and asking is not free on Linux:
 * pthread_getattr_np also reads the thread's CPU affinity, a system call, and
 * for the main thread it reads /proc/self/maps. The pool asks after every
 * job, so each thread asks once. */
static _Thread_local uintptr_t t_stack_lo, t_stack_hi;

int plat_thread_stack_range(uintptr_t* lo, uintptr_t* hi)
{
    if (!t_stack_hi) {
#if defined(__APPLE__)
        /* The address Darwin gives is the top of the stack. */
        pthread_t self = pthread_self();
        const uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
        const size_t size = pthread_get_stacksize_np(self);
        if (top && size && size <= top) { t_stack_lo = top - size; t_stack_hi = top; }
#elif defined(__linux__)
        pthread_attr_t a;
        if (pthread_getattr_np(pthread_self(), &a) == 0) {
            void* addr = NULL;
            size_t size = 0;
            if (pthread_attr_getstack(&a, &addr, &size) == 0 && addr && size) {
                t_stack_lo = (uintptr_t)addr;
                t_stack_hi = (uintptr_t)addr + size;
            }
            pthread_attr_destroy(&a);
        }
#endif
    }
    *lo = t_stack_lo;
    *hi = t_stack_hi;
    return t_stack_hi != 0;
}

/* ---- Events --------------------------------------------------------------- */

/* A timed wait counts against the monotonic clock, so a change to the wall
 * clock neither ends it early nor holds it. Linux's condition variable takes
 * that clock as an attribute; Darwin has no such attribute, but waits for a
 * relative time, which is computed from the same clock. */

static void mono_now(struct timespec* t) { clock_gettime(CLOCK_MONOTONIC, t); }

#if defined(__APPLE__)
static int ts_before(const struct timespec* a, const struct timespec* b)
{
    return a->tv_sec != b->tv_sec ? a->tv_sec < b->tv_sec : a->tv_nsec < b->tv_nsec;
}
#endif

int plat_event_init(plat_event* e)
{
    e->set = 0;
    if (pthread_mutex_init(&e->m, NULL) != 0) return 0;
    pthread_condattr_t ca;
    if (pthread_condattr_init(&ca) != 0) { pthread_mutex_destroy(&e->m); return 0; }
#if !defined(__APPLE__)
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
    const int rc = pthread_cond_init(&e->c, &ca);
    pthread_condattr_destroy(&ca);
    if (rc != 0) { pthread_mutex_destroy(&e->m); return 0; }
    return 1;
}

void plat_event_destroy(plat_event* e)
{
    pthread_cond_destroy(&e->c);
    pthread_mutex_destroy(&e->m);
}

void plat_event_set(plat_event* e)
{
    pthread_mutex_lock(&e->m);
    e->set = 1;
    pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
}

int plat_event_wait(plat_event* e, unsigned ms)
{
    pthread_mutex_lock(&e->m);
    if (!e->set && ms == PLAT_WAIT_FOREVER) {
        while (!e->set) pthread_cond_wait(&e->c, &e->m);
    } else if (!e->set && ms != 0) {
        struct timespec end;
        mono_now(&end);
        end.tv_sec  += (time_t)(ms / 1000u);
        end.tv_nsec += (long)(ms % 1000u) * 1000000L;
        if (end.tv_nsec >= 1000000000L) { end.tv_sec++; end.tv_nsec -= 1000000000L; }
        while (!e->set) {
#if defined(__APPLE__)
            struct timespec now, rel;
            mono_now(&now);
            if (!ts_before(&now, &end)) break;
            rel.tv_sec  = end.tv_sec - now.tv_sec;
            rel.tv_nsec = end.tv_nsec - now.tv_nsec;
            if (rel.tv_nsec < 0) { rel.tv_sec--; rel.tv_nsec += 1000000000L; }
            pthread_cond_timedwait_relative_np(&e->c, &e->m, &rel);
#else
            if (pthread_cond_timedwait(&e->c, &e->m, &end) == ETIMEDOUT) break;
#endif
        }
    }
    const int got = e->set;
    e->set = 0;
    pthread_mutex_unlock(&e->m);
    return got;
}

/* ---- Time ----------------------------------------------------------------- */

void plat_sleep_us(uint64_t us)
{
    struct timespec d;
    d.tv_sec  = (time_t)(us / 1000000u);
    d.tv_nsec = (long)(us % 1000000u) * 1000L;
    /* A signal ends a sleep early; sleep the rest. */
    while (nanosleep(&d, &d) != 0 && errno == EINTR) {}
}

/* ---- A file mapped read-only ---------------------------------------------- */

/* The descriptor is closed once the file is mapped: the mapping keeps the
 * file, and a rename over its name leaves the mapped one as it was. */
int plat_file_map_read(const char* path, plat_file_map* m)
{
    m->p = NULL;
    m->n = 0;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    void* v = MAP_FAILED;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
        (uint64_t)st.st_size <= (uint64_t)SIZE_MAX)
        v = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (v == MAP_FAILED) return 0;
    m->p = v;
    m->n = (size_t)st.st_size;
    return 1;
}

void plat_file_unmap(plat_file_map* m)
{
    if (m->p) munmap((void*)m->p, m->n);
    m->p = NULL;
    m->n = 0;
}

#else
typedef int plat_posix_off;   /* (a file with no declaration is not ISO C) */
#endif /* !_WIN32 */
