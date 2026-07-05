/* spurs_runtime.c -- HLE substitute for the libsre SPURS PPU threads (docs/17).
 *
 * On real HW / RPCS3, cellSpursInitialize creates two PPU threads that live in the
 * libsre PRX: the handler (SpursHdlr0, workload dispatcher) and the event helper
 * (SpursHdlr1, a persistent sys_event_queue_receive loop). We do NOT lift libsre,
 * so our port creates neither -> SPU->PPU completion events have no consumer and
 * every SPURS-driven worker stays dormant. This reimplements the event helper as a
 * host thread (clean-room from the RPCS3 contract, no GPL copied). The handler /
 * workload side-effects (contention, wklF sems) are layered on incrementally.
 *
 * Gated behind SPURS_LV2THREADS (P2 substrate) so the default boot is unaffected.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern uint32_t spurs_rt_alloc_queue(int size);
extern int      spurs_rt_pop_blocking(uint32_t, uint64_t*, uint64_t*, uint64_t*, uint64_t*);
extern uint32_t vm_read32(uint64_t);
extern void     vm_write32(uint64_t, uint32_t);
extern uint64_t ppu_guest_call(uint32_t opd, uint64_t, uint64_t, uint64_t, uint64_t);
extern void     thrdiag_set_name(const char*);

/* CellSpurs offsets (docs/17 / cellSpurs.h) */
#define CS_EVENT_QUEUE   0xD5C
#define CS_EVENT_PORT    0xD60
#define CS_EVENTPORTMUX  0xF00   /* reqPending@0x00, spuPort@0x04, eventPort@0x10, handlerList@0x18 */

static uint32_t g_spurs_ea     = 0;
static uint32_t g_spurs_eventq = 0;
static volatile long g_helper_recv = 0;

uint32_t spurs_runtime_eventq(void) { return g_spurs_eventq; }

/* data0==3 path (cellSpurs.cpp invoke_event_handlers): if reqPending, walk the
 * handlerList and call each node->handler(mux, node->data) via a guest call.
 * Node layout {next@0x00, data@0x04, handler(OPD)@0x08}. The game registers these
 * nodes; if the list is empty this is a harmless no-op. */
static void spurs_invoke_event_handlers(uint32_t mux_ea)
{
    if (!vm_read32(mux_ea + 0x00)) return;      /* reqPending */
    vm_write32(mux_ea + 0x00, 0);
    uint32_t node = vm_read32(mux_ea + 0x18);   /* handlerList head */
    vm_write32(mux_ea + 0x18, 0);
    int guard = 0;
    while (node && guard++ < 128) {
        uint32_t next    = vm_read32(node + 0x00);
        uint32_t data    = vm_read32(node + 0x04);
        uint32_t handler = vm_read32(node + 0x08);
        if (getenv("SPU_MBOX_LOG"))
            fprintf(stderr, "[spurs-helper]   invoke handler=0x%08X data=0x%08X\n", handler, data);
        if (handler) ppu_guest_call(handler, (uint64_t)mux_ea, (uint64_t)data, 0, 0);
        node = next;
    }
}

static void spurs_event_helper_loop(void)
{
    thrdiag_set_name("HLE.SpursHdlr1");
    for (;;) {
        uint64_t src = 0, d1 = 0, d2 = 0, d3 = 0;
        if (spurs_rt_pop_blocking(g_spurs_eventq, &src, &d1, &d2, &d3) != 0) break;
        g_helper_recv++;
        uint32_t data0 = (uint32_t)(d2 & 0x00FFFFFFu);
        if (getenv("SPU_MBOX_LOG"))
            fprintf(stderr, "[spurs-helper] recv #%ld src=0x%llX data0=%u d1=0x%llX d3=0x%llX\n",
                    g_helper_recv, (unsigned long long)src, data0,
                    (unsigned long long)d1, (unsigned long long)d3);
        switch (data0) {
            case 0: /* workload-shutdown completion (d3 = wid bitmask) -> post wklF sems. TODO(P-C). */
                break;
            case 1: return;   /* helper exit (finalization only) */
            case 2: /* trace-update ack -> post semPrv. TODO. */
                break;
            case 3: spurs_invoke_event_handlers(g_spurs_ea + CS_EVENTPORTMUX); break;
            default: break;
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI spurs_helper_thread(LPVOID p) { (void)p; spurs_event_helper_loop(); return 0; }
#endif

/* Called from cellSpursInitialize (SPURS_LV2THREADS): create the SPURS event queue,
 * write it into the CellSpurs layout (@0xD5C), and spawn the host event-helper. */
uint32_t spurs_runtime_start(uint32_t spurs_ea)
{
    if (g_spurs_eventq) return g_spurs_eventq;   /* once */
    g_spurs_ea     = spurs_ea;
    g_spurs_eventq = spurs_rt_alloc_queue(0x2A);
    if (g_spurs_eventq) vm_write32(spurs_ea + CS_EVENT_QUEUE, g_spurs_eventq);
    fprintf(stderr, "[spurs-rt] event-helper started: eventQueue=%u (CellSpurs @0x%08X)\n",
            g_spurs_eventq, spurs_ea);
#ifdef _WIN32
    CreateThread(NULL, 0, spurs_helper_thread, NULL, 0, NULL);
#endif
    return g_spurs_eventq;
}
