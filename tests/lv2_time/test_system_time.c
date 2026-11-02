/*
 * ps3recomp - tests/lv2_time
 *
 * lv2_system_time_usec (runtime/syscalls/sys_timer.c) is the clock that
 * sys_time_get_system_time returns and cellGcmGetLastFlipTime reports. This
 * checks that it counts microseconds. Each reading is bracketed by host
 * performance-counter reads, so the allowed range is exact: the test does
 * not depend on how long the sleep really took. The guest timebase (mftb)
 * is checked the same way, in 79.8 MHz ticks, and both for their origin:
 * each counts from its first reading, on every platform.
 *
 * Then sys_timer_usleep itself, the guest's usleep: requested against
 * measured, p50 and p99, for one-off sleeps (the precise sleep) and for a
 * streak of back-to-back short ones (the idle poll, which sleeps on the
 * thread's timer). The precise ones must never end early.
 */
#include "runtime/platform/win32_compat.h"
#include "runtime/syscalls/sys_timer.h"
#include "runtime/syscalls/sys_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What sys_timer.c refers to outside itself. None of it runs here. */
uint8_t* vm_base;
uint32_t ppu_vm_size;
sys_event_queue_info g_sys_event_queues[SYS_EVENT_QUEUE_MAX];
unsigned long long ps3_qpc_us(void) { return 0; }
void ppu_log_host_chain(const char* tag) { (void)tag; }

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static uint64_t qpc_scaled(LARGE_INTEGER from, LARGE_INTEGER to, LARGE_INTEGER freq, uint64_t per_sec)
{
    uint64_t d = (uint64_t)(to.QuadPart - from.QuadPart);
    uint64_t q = (uint64_t)freq.QuadPart;
    return (d / q) * per_sec + (d % q) * per_sec / q;
}

static uint64_t qpc_us(LARGE_INTEGER from, LARGE_INTEGER to, LARGE_INTEGER freq)
{
    return qpc_scaled(from, to, freq, 1000000ull);
}

static int cmp_double(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

#define REPS 100

/* REPS guest usleeps of `us`, each timed; prints requested against measured.
 * With `streak`, 70 back-to-back ones go first, so these are the idle poll's
 * sleeps on the timer; without, a gap of 1 ms before each keeps them one-off.
 * Returns how many ended before `us`. */
static int run_usleep(uint64_t us, int streak, LARGE_INTEGER f)
{
    static ppu_context ctx;
    static double v[REPS];
    int early = 0;
    if (streak) {
        for (int i = 0; i < 70; i++) { ctx.gpr[3] = us; sys_timer_usleep(&ctx); }
    }
    for (int i = 0; i < REPS; i++) {
        LARGE_INTEGER q0, q1;
        if (!streak) Sleep(1);
        ctx.gpr[3] = us;
        QueryPerformanceCounter(&q0);
        CHECK(sys_timer_usleep(&ctx) == CELL_OK);
        QueryPerformanceCounter(&q1);
        const uint64_t ns = qpc_scaled(q0, q1, f, 1000000000ull);
        v[i] = (double)ns / 1000.0;
        if (ns < us * 1000u) early++;
    }
    qsort(v, REPS, sizeof v[0], cmp_double);
    printf("  usleep %5llu us %-8s p50 %8.1f  p99 %8.1f  max %8.1f us  (%d early)\n",
           (unsigned long long)us, streak ? "streak" : "one-off",
           v[REPS / 2], v[REPS * 99 / 100], v[REPS - 1], early);
    return early;
}

int main(void)
{
    LARGE_INTEGER f, q0, q1, q2, q3;
    QueryPerformanceFrequency(&f);
    setvbuf(stdout, NULL, _IONBF, 0);

    QueryPerformanceCounter(&q0);
    const uint64_t a = lv2_system_time_usec();
    const uint64_t ta = ppu_timebase_now();
    QueryPerformanceCounter(&q1);
    Sleep(50);
    QueryPerformanceCounter(&q2);
    const uint64_t tb = ppu_timebase_now();
    const uint64_t b = lv2_system_time_usec();
    QueryPerformanceCounter(&q3);

    /* The time between the two readings lies between the inner and the
     * outer bracket; one microsecond of slack covers the rounding down of
     * each reading. Nanoseconds or milliseconds land 1000 times outside. */
    const uint64_t inner = qpc_us(q1, q2, f);
    const uint64_t outer = qpc_us(q0, q3, f);
    CHECK(b >= a);
    CHECK(b - a + 1 >= inner);
    CHECK(b - a <= outer + 1);
    CHECK(inner >= 40000);   /* the sleep happened, so the range means something */
    /* Both clocks count from their first call, as sys_time_get_system_time
     * always has on Windows; Linux and macOS counted from the host's boot. */
    CHECK(a == 0);
    CHECK(ta == 0);

    /* The timebase in 79.8 MHz ticks, bracketed the same way (one tick of
     * rounding each). */
    const uint64_t tinner = qpc_scaled(q1, q2, f, PS3_TIMEBASE_FREQ);
    const uint64_t touter = qpc_scaled(q0, q3, f, PS3_TIMEBASE_FREQ);
    CHECK(tb >= ta);
    CHECK(tb - ta + 1 >= tinner);
    CHECK(tb - ta <= touter + 1);

    printf("lv2 system time: a=%llu b=%llu (bracket %llu..%llu us)\n",
           (unsigned long long)a, (unsigned long long)b,
           (unsigned long long)inner, (unsigned long long)outer);
    printf("ppu timebase:    a=%llu b=%llu (bracket %llu..%llu ticks)\n",
           (unsigned long long)ta, (unsigned long long)tb,
           (unsigned long long)tinner, (unsigned long long)touter);

    /* The guest's usleep. The one-off sleeps under a millisecond spin to
     * their deadline, so none ends early; the streak's sleep on the timer and
     * the plain sleep of a millisecond or more are figures only. */
    printf("sys_timer_usleep (%d each, measured on the host's counter):\n", REPS);
    {   /* The first 60 usleeps print a [WAIT] line; not inside a timing. */
        static ppu_context warm;
        for (int i = 0; i < 60; i++) { warm.gpr[3] = 1; sys_timer_usleep(&warm); }
    }
    static const uint64_t oneoff[] = { 1, 10, 30, 100, 500, 800 };
    for (size_t i = 0; i < sizeof oneoff / sizeof oneoff[0]; i++)
        CHECK(run_usleep(oneoff[i], 0, f) == 0);
    run_usleep(30, 1, f);
    run_usleep(2000, 0, f);

    printf("lv2_time: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
