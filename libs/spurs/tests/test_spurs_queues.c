/*
 * test_spurs_queues: our own LFQueue and SpursQueue (LIBSRE_OWN,
 * libs/spurs/spurs_queues.h) against a stand-in for each queue's SPU side.
 *
 * The stand-ins edit the queue line the way the port's docs/133 (section 9)
 * says the SPU side does, under the same lock-line lock an SPU PUTLLC takes,
 * and send their events through the SPURS port table, as an SPU task's
 * send_event does. They are written from that spec only.
 *
 *  1. LFQueue: initialize, its argument checks, attach and detach, and the line
 *     each leaves.
 *  2. LFQueue: non-blocking pushes up to the depth, then AGAIN; a push that
 *     finds a waiting consumer in the ring signals that task.
 *  3. LFQueue under load: a blocking pusher and a consumer thread, a queue of
 *     depth 4: every entry arrives once, in order, and no wake-up is lost.
 *  4. SpursQueue: initialize, attach, an empty pop (AGAIN), then a blocking
 *     popper and a try-push "SPU" thread, the same checks.
 *
 * A lost wake-up shows as a pusher or popper asleep past the recheck time
 * with its queue ready: g_sq_stats.lost_wakeups counts them.
 */
#include "../spurs_queues.h"
#include "../spurs_taskset.h"
#include "../../../runtime/syscalls/sys_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the runtime library expects from a game's loader (the rest is
 * runtime/host/host_stubs.c). */
uint8_t* vm_base = NULL;
const uint64_t function_table[2] = { 0, 0 };
const uint64_t function_table_count = 0;

extern uint32_t spurs_port_queue(uint32_t port);

#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE th_t;
  static th_t th_start(DWORD (WINAPI* fn)(LPVOID), void* arg) { return CreateThread(NULL, 0, fn, arg, 0, NULL); }
  static void th_join(th_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
  static void th_yield(void) { SwitchToThread(); }
  #define TH_FN(name) static DWORD WINAPI name(LPVOID arg)
  #define TH_RET return 0
#else
  #include <pthread.h>
  #include <sched.h>
  typedef pthread_t th_t;
  static th_t th_start(void* (*fn)(void*), void* arg) { th_t t; pthread_create(&t, NULL, fn, arg); return t; }
  static void th_join(th_t t) { pthread_join(t, NULL); }
  static void th_yield(void) { sched_yield(); }
  #define TH_FN(name) static void* name(void* arg)
  #define TH_RET return NULL
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Guest addresses in the test's arena. */
#define ARENA    0x00100000u
#define TASKSET  0x00010000u   /* 128-aligned; its signalled set is at +CSTS_SIGNALLED */
#define LFQ      0x00020000u
#define LFQ_BUF  0x00020080u
#define SQ       0x00030000u
#define SQ_BUF   0x00030080u
#define ENTRY    0x00040000u   /* a push's source, a pop's destination */
#define SPU_LS   0x00050000u   /* the stand-in SPU's own copy */

#define NO_PORT 0xFFFFFFFFu

static void stand_in_send(uint32_t port)
{
    /* As an SPU task's send_event on a port attached to the SPURS instance. */
    const uint32_t eq = spurs_port_queue(port);
    if (eq) (void)sys_event_queue_push_by_id(eq, port, 0, (uint64_t)port << 32, 0);
}

/* ---- 1, 2: the LFQueue's line ------------------------------------------- */

static void test_lfq_line(void)
{
    memset(vm_base + LFQ, 0, 128);
    CHECK(spurs_lfq_init(TASKSET, 0, LFQ_BUF, 0x20, 16, 2) == SQ_ERR_NULL_POINTER, "null queue");
    CHECK(spurs_lfq_init(TASKSET, LFQ, LFQ_BUF, 0x18, 16, 2) == SQ_ERR_INVAL, "size not a multiple of 16");
    CHECK(spurs_lfq_init(TASKSET, LFQ, LFQ_BUF, 0x20, 0, 2) == SQ_ERR_INVAL, "depth 0");
    CHECK(spurs_lfq_init(TASKSET, LFQ, LFQ_BUF, 0x20, 16, 4) == SQ_ERR_INVAL, "direction 4");
    CHECK(spurs_lfq_init(TASKSET, LFQ + 0x40, LFQ_BUF, 0x20, 16, 2) == SQ_ERR_ALIGN, "queue not 128-aligned");
    CHECK(spurs_lfq_init(TASKSET, LFQ, LFQ_BUF + 8, 0x20, 16, 2) == SQ_ERR_ALIGN, "buffer not 16-aligned");

    CHECK(spurs_lfq_init(TASKSET, LFQ, LFQ_BUF, 0x20, 16, 2) == 0, "initialize");
    CHECK(sq_rd32(LFQ + 0x10) == 0x20 && sq_rd32(LFQ + 0x14) == 16, "size, depth");
    CHECK(sq_rd64(LFQ + 0x18) == LFQ_BUF, "buffer");
    CHECK(sq_rd32(LFQ + 0x20) == NO_PORT, "no port yet");
    CHECK(sq_rd32(LFQ + 0x24) == 2 && sq_rd32(LFQ + 0x2C) == 2, "direction, init state");
    CHECK(sq_rd64(LFQ + 0x70) == TASKSET, "signal target");
    CHECK(sq_rd64(LFQ + 0x00) == 0 && sq_rd64(LFQ + 0x08) == 0, "counter words 0");
    CHECK(spurs_lfq_init(TASKSET, LFQ, LFQ_BUF, 0x20, 16, 2) == 0, "the same initialize again");
    CHECK(spurs_lfq_init(TASKSET, LFQ, LFQ_BUF, 0x20, 8, 2) == SQ_ERR_INVAL, "a different one");
    {
        uint8_t keep[128];
        memcpy(keep, vm_base + LFQ, 128);
        memset(vm_base + LFQ, 0, 128);
        vm_base[LFQ + 0x40] = 1;
        CHECK(spurs_lfq_init(TASKSET, LFQ, LFQ_BUF, 0x20, 16, 2) == SQ_ERR_STAT, "a line not zeroed");
        memcpy(vm_base + LFQ, keep, 128);
    }

    CHECK(spurs_lfq_push(LFQ, ENTRY, 3) == SQ_ERR_STAT, "a blocking push before attach");
    CHECK(spurs_lfq_detach(LFQ) == SQ_ERR_STAT, "detach before attach");
    CHECK(spurs_lfq_attach(LFQ) == 0, "attach");
    const uint32_t port = sq_rd32(LFQ + 0x20), eq = sq_rd32(LFQ + 0x7C);
    CHECK(port >= 16 && port < 64, "port %u in 16..63", port);
    CHECK(eq != 0 && spurs_port_queue(port) == eq, "port %u routes to queue %u", port, eq);
    CHECK(spurs_lfq_attach(LFQ) == SQ_ERR_STAT, "attach twice");
    CHECK(spurs_lfq_detach(LFQ) == 0, "detach");
    CHECK(sq_rd32(LFQ + 0x20) == NO_PORT && spurs_port_queue(port) == 0, "port released");
    CHECK(sq_rd32(LFQ + 0x7C) == eq, "the queue id stays");
    CHECK(spurs_lfq_attach(LFQ) == 0, "attach again");

    /* 2: non-blocking pushes. */
    for (uint32_t i = 0; i < 16; i++) {
        memset(vm_base + ENTRY, (int)(0x40 + i), 0x20);
        CHECK(spurs_lfq_push(LFQ, ENTRY, 0) == 0, "push %u", i);
    }
    CHECK(spurs_lfq_push(LFQ, ENTRY, 0) == SQ_ERR_AGAIN, "push into a full queue");
    CHECK(sq_rd16(LFQ + 0x08) == 16 && sq_rd16(LFQ + 0x0E) == 16, "push done, next = 16");
    CHECK(sq_rd16(LFQ + 0x0A) == 0, "done mask empty");
    CHECK(vm_base[LFQ_BUF + 5 * 0x20] == 0x45 && vm_base[LFQ_BUF + 15 * 0x20 + 31] == 0x4F, "slots hold the entries");

    /* A consumer took two entries and one more waits in the ring: the next
     * push signals it (task 7) and moves the ring's read counter. */
    sq_wr16(LFQ + 0x06, 2); sq_wr16(LFQ + 0x00, 2);
    sq_wr16(LFQ + 0x32, 7);
    sq_wr16(LFQ + 0x30, 1);                         /* read 0, resumed 0, write 1 */
    memset(vm_base + TASKSET, 0, 256);
    CHECK(spurs_lfq_push(LFQ, ENTRY, 0) == 0, "push with a waiter");
    CHECK(spurs_bitset_test(TASKSET + CSTS_SIGNALLED, 7), "task 7 signalled");
    CHECK(sq_rd16(LFQ + 0x30) == ((1u << 10) | 1u), "ring read moved to 1 (ring 0x%04X)", sq_rd16(LFQ + 0x30));
    CHECK(sq_rd16(LFQ + 0x08) == 17 && sq_rd16(LFQ + 0x0E) == 17, "push done, next = 17");
    CHECK(vm_base[LFQ_BUF + 0] == 0x4F, "slot 16 is slot 0 again");

    CHECK(spurs_lfq_detach(LFQ) == 0, "detach");
}

/* ---- 3: LFQueue under load ------------------------------------------------ */

#define LOAD_N 40000u
static volatile int s_lfq_bad;

/* The SPU consumer of a PPU-to-SPU LFQueue: take the entry at pop.next, copy
 * it out, then advance pop.done; finishing a pop while the pusher waits
 * (push waiting = 3), set waiting = 1 and send an event on the port. */
TH_FN(lfq_consumer)
{
    (void)arg;
    const uint32_t depth = sq_rd32(LFQ + 0x14), span = depth * 2, size = sq_rd32(LFQ + 0x10);
    uint32_t want = 0;
    while (want < LOAD_N) {
        sq_line_lock(LFQ);
        const uint32_t pop_done = sq_rd16(LFQ + 0x00), pop_next = sq_rd16(LFQ + 0x06);
        const uint32_t push_done = sq_rd16(LFQ + 0x08);
        if (pop_next != pop_done || push_done == pop_next) { sq_line_unlock(LFQ, 0); th_yield(); continue; }
        sq_wr16(LFQ + 0x06, (uint16_t)((pop_next + 1) % span));
        sq_line_unlock(LFQ, 1);

        memcpy(vm_base + SPU_LS, vm_base + LFQ_BUF + (pop_next % depth) * size, size);
        uint32_t got;
        memcpy(&got, vm_base + SPU_LS, 4);
        if (got != want) { s_lfq_bad++; if (s_lfq_bad < 4) printf("FAIL lfq: entry %u, wanted %u\n", got, want); }
        want++;

        sq_line_lock(LFQ);
        sq_wr16(LFQ + 0x00, (uint16_t)((pop_done + 1) % span));
        const int wake = sq_rd16(LFQ + 0x0C) == 3;
        if (wake) sq_wr16(LFQ + 0x0C, 1);
        const uint32_t port = sq_rd32(LFQ + 0x20);
        sq_line_unlock(LFQ, 1);
        if (wake) stand_in_send(port);
    }
    TH_RET;
}

static void test_lfq_load(void)
{
    memset(vm_base + LFQ, 0, 128);
    CHECK(spurs_lfq_init(TASKSET, LFQ, LFQ_BUF, 0x10, 4, 2) == 0, "initialize (depth 4)");
    CHECK(spurs_lfq_attach(LFQ) == 0, "attach");
    const long slept0 = g_sq_stats.lfq_push_slept, lost0 = g_sq_stats.lost_wakeups;
    th_t t = th_start(lfq_consumer, NULL);
    for (uint32_t i = 0; i < LOAD_N; i++) {
        memset(vm_base + ENTRY, 0, 0x10);
        memcpy(vm_base + ENTRY, &i, 4);
        const uint32_t rc = spurs_lfq_push(LFQ, ENTRY, 3);
        if (rc) { CHECK(rc == 0, "blocking push %u: 0x%08X", i, rc); break; }
    }
    th_join(t);
    CHECK(s_lfq_bad == 0, "%d entries out of order", s_lfq_bad);
    CHECK(g_sq_stats.lost_wakeups == lost0, "%ld lost wake-ups", g_sq_stats.lost_wakeups - lost0);
    printf("lfq load: %u pushes, %ld slept on a full queue\n", LOAD_N, g_sq_stats.lfq_push_slept - slept0);
    CHECK(g_sq_stats.lfq_push_slept > slept0, "the pusher never slept: the test did not test the wait");
    CHECK(spurs_lfq_detach(LFQ) == 0, "detach");
}

/* ---- 4: SpursQueue ---------------------------------------------------------- */

/* The SPU pusher of an SPU-to-PPU SpursQueue, a try-push in a loop: claim the
 * push by storing the complement of the push count, write the entry, store
 * push + 1, and if the pop word is negative (a popper sleeps) send an event. */
TH_FN(sq_pusher)
{
    (void)arg;
    const uint32_t depth = sq_rd32(SQ + 0x0C), span = depth * 2, size = sq_rd32(SQ + 0x08);
    uint32_t i = 0;
    while (i < LOAD_N) {
        sq_line_lock(SQ);
        const int32_t popw = (int32_t)sq_rd32(SQ + 0x00);
        const uint32_t pop = (uint32_t)(popw < 0 ? ~popw : popw);
        const uint32_t push = sq_rd32(SQ + 0x04);
        if ((push + span - pop) % span >= depth) { sq_line_unlock(SQ, 0); th_yield(); continue; }
        sq_wr32(SQ + 0x04, ~push);
        sq_line_unlock(SQ, 1);

        memset(vm_base + SQ_BUF + (push % depth) * size, 0, size);
        memcpy(vm_base + SQ_BUF + (push % depth) * size, &i, 4);
        i++;

        sq_line_lock(SQ);
        sq_wr32(SQ + 0x04, (push + 1) % span);
        const int wake = (int32_t)sq_rd32(SQ + 0x00) < 0;
        const uint32_t port = sq_rd32(SQ + 0x18);
        sq_line_unlock(SQ, 1);
        if (wake) stand_in_send(port);
        /* Now and then let the popper find the queue empty. */
        if ((i & 63) == 0) for (int k = 0; k < 50; k++) th_yield();
    }
    TH_RET;
}

static void test_sq(void)
{
    /* The taskset's SPURS instance, at its +0x60. */
    sq_wr64(TASKSET + CSTS_SPURS, 0x00070000u);
    memset(vm_base + SQ, 0, 128);
    CHECK(spurs_q_init(0, TASKSET, SQ + 0x40, SQ_BUF, 0x10, 8, 1) == SQ_ERR_ALIGN, "queue not 128-aligned");
    CHECK(spurs_q_init(0, TASKSET, SQ, SQ_BUF, 0x10, 0, 1) == SQ_ERR_INVAL, "depth 0");
    CHECK(spurs_q_init(0, TASKSET, SQ, SQ_BUF, 0x10, 8, 1) == 0, "initialize");
    CHECK(sq_rd32(SQ + 0x08) == 0x10 && sq_rd32(SQ + 0x0C) == 8, "size, depth");
    CHECK(sq_rd64(SQ + 0x10) == SQ_BUF && sq_rd32(SQ + 0x18) == NO_PORT && sq_rd32(SQ + 0x1C) == 1,
          "buffer, no port, direction");
    CHECK(sq_rd64(SQ + 0x60) == TASKSET && sq_rd64(SQ + 0x68) == 0x00070000u, "taskset, its SPURS");

    CHECK(spurs_q_pop(SQ, ENTRY, 1) == SQ_ERR_STAT, "a blocking pop before attach");
    CHECK(spurs_q_detach(SQ) == SQ_ERR_STAT, "detach before attach");
    CHECK(spurs_q_attach(SQ) == 0, "attach");
    const uint32_t port = sq_rd32(SQ + 0x18);
    CHECK(port >= 16 && port < 64 && spurs_port_queue(port) == sq_rd32(SQ + 0x74), "port %u, event queue", port);
    CHECK(spurs_q_pop(SQ, ENTRY, 0) == SQ_ERR_AGAIN, "an empty pop");
    CHECK(sq_rd32(SQ + 0x00) == 0, "an empty non-blocking pop leaves the line");

    const long slept0 = g_sq_stats.q_pop_slept, lost0 = g_sq_stats.lost_wakeups;
    th_t t = th_start(sq_pusher, NULL);
    int bad = 0;
    for (uint32_t i = 0; i < LOAD_N; i++) {
        const uint32_t rc = spurs_q_pop(SQ, ENTRY, 1);
        if (rc) { CHECK(rc == 0, "blocking pop %u: 0x%08X", i, rc); break; }
        uint32_t got;
        memcpy(&got, vm_base + ENTRY, 4);
        if (got != i && bad++ < 4) printf("FAIL sq: entry %u, wanted %u\n", got, i);
    }
    th_join(t);
    CHECK(bad == 0, "%d entries out of order", bad);
    CHECK((int32_t)sq_rd32(SQ + 0x00) >= 0, "no popper marked asleep at the end");
    CHECK(g_sq_stats.lost_wakeups == lost0, "%ld lost wake-ups", g_sq_stats.lost_wakeups - lost0);
    printf("sq load: %u pops, %ld slept on an empty queue\n", LOAD_N, g_sq_stats.q_pop_slept - slept0);
    CHECK(g_sq_stats.q_pop_slept > slept0, "the popper never slept: the test did not test the wait");
    CHECK(spurs_q_pop(SQ, ENTRY, 0) == SQ_ERR_AGAIN, "empty again");
    CHECK(spurs_q_detach(SQ) == 0, "detach");
    CHECK(sq_rd32(SQ + 0x18) == NO_PORT && spurs_port_queue(port) == 0, "port released");
}

int main(void)
{
    /* No [WAIT] line for each of the thousands of sleeps below. */
#ifdef _WIN32
    _putenv("PS3_VERBOSE=0");
#else
    setenv("PS3_VERBOSE", "0", 1);
#endif
    vm_base = (uint8_t*)calloc(1, ARENA);
    if (!vm_base) { printf("FAIL: no arena\n"); return 1; }
    test_lfq_line();
    test_lfq_load();
    test_sq();
    if (fails) { printf("spurs_queues: %d failed\n", fails); return 1; }
    printf("spurs_queues: all passed\n");
    return 0;
}
