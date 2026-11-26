/* tests/plat_threads -- runtime/platform/plat.h's core kept for one thread
 * (RSX_CORE).
 *
 * Mode 0 changes nothing, and so does mode 2 (auto) with fewer than 6 cores:
 * checked held to 4 cores with SMT where the machine has them, and as it is
 * when it has fewer than 6. On Linux, where the machine has a core to spare
 * (plat_core_reserve(1) gives a CPU): the calling thread runs on that CPU
 * alone; a thread started before runs off its core; a thread the reserved
 * thread starts after begins on its CPU and is off the core after
 * plat_core_keep. With no core to spare, or no topology to read, it gives -1
 * and the rest is skipped. Off Linux it always gives -1.
 *
 * Exit code 0 if every check passed; each failure prints a line.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* sched_getaffinity, CPU_ISSET */
#endif
#include "platform/plat.h"

#include <stdio.h>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

static int g_fail;
#define CHECK(c) do { if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#if defined(__linux__)
static volatile int g_stop;

static void* idle(void* arg)
{
    *(volatile long*)arg = (long)syscall(SYS_gettid);
    while (!g_stop) usleep(1000);
    return NULL;
}

static long start(pthread_t* t, volatile long* tid)
{
    *tid = 0;
    if (pthread_create(t, NULL, idle, (void*)tid) != 0) return 0;
    while (!*tid) usleep(100);
    return *tid;
}

/* The CPUs of cpu's core, from sysfs ("2-3" or "2,6"). */
static void core_of(int cpu, cpu_set_t* core)
{
    CPU_ZERO(core);
    CPU_SET(cpu, core);
    char path[112];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char buf[128];
    if (fgets(buf, sizeof buf, f)) {
        for (char* p = buf; *p >= '0' && *p <= '9';) {
            char* e;
            const long lo = strtol(p, &e, 10);
            long hi = lo;
            if (*e == '-') hi = strtol(e + 1, &e, 10);
            for (long c = lo; c <= hi && c < CPU_SETSIZE; c++) CPU_SET((int)c, core);
            p = *e == ',' ? e + 1 : e;
        }
    }
    fclose(f);
}

/* Up to `max` cores all of whose CPUs are in `allowed`, each once, into
 * `out`; *smt is 1 if one of them has two CPUs or more. Gives how many. */
static int cores_in(const cpu_set_t* allowed, int max, cpu_set_t* out, int* smt)
{
    cpu_set_t seen, sib, in;
    CPU_ZERO(&seen);
    CPU_ZERO(out);
    *smt = 0;
    int n = 0;
    for (int c = 0; c < CPU_SETSIZE && n < max; c++) {
        if (!CPU_ISSET(c, allowed) || CPU_ISSET(c, &seen)) continue;
        core_of(c, &sib);
        CPU_OR(&seen, &seen, &sib);
        CPU_AND(&in, &sib, allowed);
        if (!CPU_EQUAL(&in, &sib)) continue;
        CPU_OR(out, out, &sib);
        if (CPU_COUNT(&sib) > 1) *smt = 1;
        n++;
    }
    return n;
}

/* 1 if the thread's CPUs are some, and none of them on the core. */
static int off_core(long tid, const cpu_set_t* core)
{
    cpu_set_t m, both;
    if (sched_getaffinity((pid_t)tid, sizeof m, &m) != 0) return 0;
    CPU_AND(&both, &m, core);
    return CPU_COUNT(&m) > 0 && CPU_COUNT(&both) == 0;
}
#endif

int main(void)
{
    CHECK(plat_core_reserve(0) == -1);
#if defined(__linux__)
    {
        cpu_set_t was, set;
        int smt = 0;
        CHECK(sched_getaffinity(0, sizeof was, &was) == 0);
        if (cores_in(&was, 4, &set, &smt) == 4 && smt) {
            CHECK(sched_setaffinity(0, sizeof set, &set) == 0);
            CHECK(plat_core_reserve(2) == -1);
            CHECK(sched_setaffinity(0, sizeof was, &was) == 0);
            printf("plat_core: auto on 4 cores with SMT: left alone%c", 10);
        }
        if (cores_in(&was, CPU_SETSIZE, &set, &smt) < 6 || !smt) CHECK(plat_core_reserve(2) == -1);
    }
    pthread_t before, after;
    volatile long before_tid, after_tid;
    CHECK(start(&before, &before_tid));
    const int cpu = plat_core_reserve(1);
    if (cpu < 0) {
        printf("plat_core: no core to spare here; skipped%c", 10);
    } else {
        cpu_set_t core, m;
        core_of(cpu, &core);
        CHECK(sched_getaffinity(0, sizeof m, &m) == 0 && CPU_COUNT(&m) == 1 && CPU_ISSET(cpu, &m));
        CHECK(plat_core_reserve(1) == cpu);   /* once */
        CHECK(off_core(before_tid, &core));
        CHECK(start(&after, &after_tid));
        CHECK(sched_getaffinity((pid_t)after_tid, sizeof m, &m) == 0 && CPU_ISSET(cpu, &m));
        plat_core_keep();   /* one more thread than at the reserve: the list is read */
        CHECK(off_core(after_tid, &core));
        printf("plat_core: CPU %d kept, %d CPU(s) on its core%c", cpu, CPU_COUNT(&core), 10);
    }
    g_stop = 1;
    pthread_join(before, NULL);
    if (cpu >= 0) pthread_join(after, NULL);
#else
    CHECK(plat_core_reserve(1) == -1);
    plat_core_keep();
#endif
    return g_fail ? 1 : 0;
}
