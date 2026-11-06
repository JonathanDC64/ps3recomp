/*
 * ps3recomp - the platform layer on Linux and macOS (see plat.h)
 *
 * The calls that take more than a line. Where the two systems differ (the
 * stack range, the clock a timed wait uses) the difference is here and
 * nowhere else. Empty on Windows, where plat.h has everything inline.
 */
#ifndef _WIN32

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* pthread_getattr_np, sched_setaffinity, CPU_AND */
#endif

#include "plat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
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

/* ---- A core for one thread (Linux) ---------------------------------------- */

#if defined(__linux__)
static cpu_set_t s_core_cpus;   /* the kept core's CPUs */
static cpu_set_t s_core_rest;   /* the process's other CPUs */
static int  s_core_cpu = -1;
static long s_core_tid;
static long s_core_threads;
static unsigned s_core_calls;

static long read_number(const char* path)
{
    FILE* f = fopen(path, "r");
    long v = -1;
    if (f) {
        if (fscanf(f, "%ld", &v) != 1) v = -1;
        fclose(f);
    }
    return v;
}

/* A sysfs CPU list ("2-3", "2,6", "0-1,8-9"). */
static int read_cpu_list(const char* path, cpu_set_t* set)
{
    CPU_ZERO(set);
    char buf[256];
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    const int got = fgets(buf, sizeof buf, f) != NULL;
    fclose(f);
    if (!got) return 0;
    for (char* p = buf; *p >= '0' && *p <= '9';) {
        char* e;
        const long lo = strtol(p, &e, 10);
        long hi = lo;
        if (*e == '-') {
            p = e + 1;
            hi = strtol(p, &e, 10);
            if (e == p || hi < lo) return 0;
        }
        for (long c = lo; c <= hi && c < CPU_SETSIZE; c++) CPU_SET((int)c, set);
        p = *e == ',' ? e + 1 : e;
    }
    return CPU_COUNT(set) > 0;
}

/* /proc/self/stat's 20th field. The name (field 2) can hold spaces and
 * brackets, so the count starts after its last ')'. */
static long count_threads(void)
{
    char buf[1024];
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return -1;
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char* p = strrchr(buf, ')');
    if (!p) return -1;
    for (int field = 2; field < 20 && p; field++) p = strchr(p + 1, ' ');
    return p ? strtol(p + 1, NULL, 10) : -1;
}

static void core_move(pid_t tid)
{
    cpu_set_t m, on, to;
    if (sched_getaffinity(tid, sizeof m, &m) != 0) return;   /* it has ended */
    CPU_AND(&on, &m, &s_core_cpus);
    if (!CPU_COUNT(&on)) return;
    CPU_AND(&to, &m, &s_core_rest);
    if (!CPU_COUNT(&to)) to = s_core_rest;
    sched_setaffinity(tid, sizeof to, &to);
}

static void core_sweep(void)
{
    DIR* d = opendir("/proc/self/task");
    if (!d) return;
    for (struct dirent* e; (e = readdir(d)) != NULL;) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const long tid = strtol(e->d_name, NULL, 10);
        if (tid != s_core_tid) core_move((pid_t)tid);
    }
    closedir(d);
}

int plat_core_reserve(int mode)
{
    if (s_core_cpu >= 0 || mode <= 0) return s_core_cpu;
    cpu_set_t allowed, seen, sib, in;
    if (sched_getaffinity(0, sizeof allowed, &allowed) != 0) return -1;
    CPU_ZERO(&seen);
    int best = -1, best_first = 0, cores = 0, smt = 0;
    long best_freq = -1, best_perf = -1;
    char path[112];
    /* Each core once, by its first CPU; only a core whose CPUs are all the
     * process's is a candidate. */
    for (int c = 0; c < CPU_SETSIZE; c++) {
        if (!CPU_ISSET(c, &allowed) || CPU_ISSET(c, &seen)) continue;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", c);
        if (!read_cpu_list(path, &sib)) return -1;
        CPU_OR(&seen, &seen, &sib);
        CPU_AND(&in, &sib, &allowed);
        if (!CPU_EQUAL(&in, &sib)) continue;
        cores++;
        if (CPU_COUNT(&sib) > 1) smt = 1;
        const int first = cores == 1;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        const long freq = read_number(path);
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/acpi_cppc/highest_perf", c);
        const long perf = read_number(path);
        if (best < 0 || freq > best_freq ||
            (freq == best_freq && (best_first > first || (best_first == first && perf > best_perf)))) {
            best = c;
            best_first = first;
            best_freq = freq;
            best_perf = perf;
        }
    }
    if (best < 0 || (mode == 2 && (!smt || cores < 4))) return -1;
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", best);
    if (!read_cpu_list(path, &s_core_cpus)) return -1;
    CPU_ZERO(&s_core_rest);
    for (int c = 0; c < CPU_SETSIZE; c++)
        if (CPU_ISSET(c, &allowed) && !CPU_ISSET(c, &s_core_cpus)) CPU_SET(c, &s_core_rest);
    if (!CPU_COUNT(&s_core_rest)) return -1;
    cpu_set_t self;
    CPU_ZERO(&self);
    CPU_SET(best, &self);
    if (sched_setaffinity(0, sizeof self, &self) != 0) return -1;
    s_core_cpu = best;
    s_core_tid = (long)syscall(SYS_gettid);
    core_sweep();
    s_core_threads = count_threads();
    return best;
}

void plat_core_keep(void)
{
    if (s_core_cpu < 0) return;
    const long n = count_threads();
    if (n == s_core_threads && (++s_core_calls & 15u)) return;
    s_core_threads = n;
    core_sweep();
}
#endif

#else
typedef int plat_posix_off;   /* (a file with no declaration is not ISO C) */
#endif /* !_WIN32 */
