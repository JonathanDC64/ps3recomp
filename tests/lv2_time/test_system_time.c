/*
 * ps3recomp - tests/lv2_time
 *
 * lv2_system_time_usec (runtime/syscalls/sys_timer.c) is the clock that
 * sys_time_get_system_time returns and cellGcmGetLastFlipTime reports. This
 * checks that it counts microseconds. Each reading is bracketed by host
 * performance-counter reads, so the allowed range is exact: the test does
 * not depend on how long the sleep really took.
 */
#include "runtime/platform/win32_compat.h"
#include "runtime/syscalls/sys_timer.h"
#include "runtime/syscalls/sys_event.h"
#include <stdio.h>

/* What sys_timer.c refers to outside itself. None of it runs here. */
uint8_t* vm_base;
uint32_t ppu_vm_size;
sys_event_queue_info g_sys_event_queues[SYS_EVENT_QUEUE_MAX];
unsigned long long ps3_qpc_us(void) { return 0; }
void ppu_log_host_chain(const char* tag) { (void)tag; }

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static uint64_t qpc_us(LARGE_INTEGER from, LARGE_INTEGER to, LARGE_INTEGER freq)
{
    uint64_t d = (uint64_t)(to.QuadPart - from.QuadPart);
    uint64_t q = (uint64_t)freq.QuadPart;
    return (d / q) * 1000000ull + (d % q) * 1000000ull / q;
}

int main(void)
{
    LARGE_INTEGER f, q0, q1, q2, q3;
    QueryPerformanceFrequency(&f);

    QueryPerformanceCounter(&q0);
    const uint64_t a = lv2_system_time_usec();
    QueryPerformanceCounter(&q1);
    Sleep(50);
    QueryPerformanceCounter(&q2);
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
#ifdef _WIN32
    /* On Windows it counts from its first call, as sys_time_get_system_time
     * always has. */
    CHECK(a == 0);
#endif

    printf("lv2 system time: a=%llu b=%llu (bracket %llu..%llu us): %s\n",
           (unsigned long long)a, (unsigned long long)b,
           (unsigned long long)inner, (unsigned long long)outer,
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
