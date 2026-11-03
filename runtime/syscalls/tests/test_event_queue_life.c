/*
 * test_event_queue_life: event queues made and destroyed while host threads
 * push to them (#311).
 *
 * Two threads find a queue by its key and push to it, as cellAudio's mixer
 * does every audio period, while the main thread makes and destroys queues
 * with that key, through the guest's syscalls and through
 * sys_event_queue_create_direct. Before the fix a pusher could enter the lock
 * of a queue that was marked active but whose lock was not made yet, or was
 * already deleted: an access violation writing near 0x24, or a hang.
 *
 * Then: a live queue takes pushes up to its size, a destroyed one none.
 */
#include "../sys_event.h"

#include <stdio.h>
#include <string.h>

/* What the runtime library expects from a game's loader (the rest is
 * runtime/host/host_stubs.c). No guest memory is used: no id is written back. */
uint8_t* vm_base = NULL;
const uint64_t function_table[2] = { 0, 0 };
const uint64_t function_table_count = 0;

#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE th_t;
  static th_t th_start(DWORD (WINAPI* fn)(LPVOID), void* arg) { return CreateThread(NULL, 0, fn, arg, 0, NULL); }
  static void th_join(th_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
  #define TH_FN(name) static DWORD WINAPI name(LPVOID arg)
  #define TH_RET return 0
#else
  #include <pthread.h>
  typedef pthread_t th_t;
  static th_t th_start(void* (*fn)(void*), void* arg) { th_t t; pthread_create(&t, NULL, fn, arg); return t; }
  static void th_join(th_t t) { pthread_join(t, NULL); }
  #define TH_FN(name) static void* name(void* arg)
  #define TH_RET return NULL
#endif

#define KEY 0x8000CAFE02460300ull   /* (the notify queue's key in #311's log) */

static volatile int s_stop;
static volatile long s_pushed[2];

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

TH_FN(pusher)
{
    const int me = arg ? 1 : 0;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        const uint32_t qid = sys_event_find_queue_by_key(KEY);
        if (qid && sys_event_queue_push_by_id(qid, KEY, 0, 0, 0) == 0) s_pushed[me]++;
    }
    TH_RET;
}

static ppu_context s_ctx;

/* sys_event_queue_create as the guest calls it (no id written back: the
 * queue is found by its key), or sys_event_queue_create_direct. */
static uint32_t make_queue(int direct, int32_t size)
{
    if (direct) return sys_event_queue_create_direct(KEY, size);
    memset(&s_ctx, 0, sizeof s_ctx);
    s_ctx.gpr[3] = 0; s_ctx.gpr[4] = 0; s_ctx.gpr[5] = KEY; s_ctx.gpr[6] = (uint64_t)size;
    if (sys_event_queue_create(&s_ctx) != CELL_OK) return 0;
    return sys_event_find_queue_by_key(KEY);
}

static int64_t drop_queue(uint32_t qid)
{
    memset(&s_ctx, 0, sizeof s_ctx);
    s_ctx.gpr[3] = qid;
    return sys_event_queue_destroy(&s_ctx);
}

int main(void)
{
    /* The creates log a line each; not for the race's thousands. */
#ifdef _WIN32
    FILE* quiet = freopen("NUL", "w", stderr);
#else
    FILE* quiet = freopen("/dev/null", "w", stderr);
#endif
    (void)quiet;

    th_t a = th_start(pusher, NULL);
    th_t b = th_start(pusher, (void*)1);
    const int rounds = 20000;
    int made = 0;
    for (int i = 0; i < rounds; i++) {
        const uint32_t qid = make_queue(i & 1, 2);
        if (!qid) continue;
        made++;
        for (volatile int spin = 0; spin < (i % 64); spin++) { }
        CHECK(drop_queue(qid) == CELL_OK, "destroy of queue %u failed", qid);
    }
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
    th_join(a);
    th_join(b);
    CHECK(made == rounds, "%d of %d queues made", made, rounds);
    printf("[evq] %d queues made and destroyed under two pushers (%ld pushes landed)\n",
           made, s_pushed[0] + s_pushed[1]);

    /* A live queue of 2 takes 2 pushes and refuses the third; destroyed, it
     * takes none and its key finds nothing. */
    const uint32_t qid = make_queue(0, 2);
    CHECK(qid != 0, "no queue");
    CHECK(sys_event_queue_push_by_id(qid, KEY, 1, 0, 0) == 0, "first push refused");
    CHECK(sys_event_queue_push_by_id(qid, KEY, 2, 0, 0) == 0, "second push refused");
    CHECK(sys_event_queue_push_by_id(qid, KEY, 3, 0, 0) == -1, "a full queue took a third push");
    CHECK(drop_queue(qid) == CELL_OK, "destroy failed");
    CHECK(sys_event_queue_push_by_id(qid, KEY, 4, 0, 0) == -1, "a destroyed queue took a push");
    CHECK(sys_event_find_queue_by_key(KEY) == 0, "a destroyed queue is still found by its key");

    printf(fails ? "[evq] FAIL: %d check(s)\n" : "[evq] PASS\n", fails);
    return fails ? 1 : 0;
}
