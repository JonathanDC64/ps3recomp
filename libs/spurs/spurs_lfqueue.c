/*
 * ps3recomp - the LFQueue family on our own code (LIBSRE_OWN, spurs_queues.h)
 *
 * _cellSpursLFQueueInitialize, cellSpursLFQueueAttachLv2EventQueue,
 * _cellSpursLFQueuePushBody and cellSpursLFQueueDetachLv2EventQueue: the PPU
 * end of a CellSyncLFQueue whose consumers are SPU tasks (Demon's Souls'
 * resource decompressor feeds its zlib task this way).
 *
 * The line (the port's docs/133, section 9.1; big-endian):
 *   0x00 pop:  u16 done, u16 done mask, u16 -, u16 next
 *   0x08 push: u16 done, u16 done mask, u16 waiting, u16 next
 *   0x10 u32 entry size      0x14 u32 depth       0x18 u64 buffer
 *   0x20 u32 event port (0xFFFFFFFF unattached)   0x24 u32 direction
 *   0x2C u32 init state (2 = initialised)
 *   0x30 u16 the push side's ring of waiting SPU consumers, three 5-bit
 *        counters: read (bits 10-14), resumed (5-9), write (0-4); the task
 *        ids follow as u16[15] at 0x32
 *   0x50 the pop side's ring, the same way
 *   0x70 u64 the signal target (the owner: a taskset)
 *   0x7C u32 the event queue a pusher sleeps on while the queue is full
 * Pointers run modulo twice the depth; pointer n is in slot n mod depth.
 * RPCS3's cellSync LFQueue documents the same layout; this code follows the
 * spec in docs/133, not that code.
 */
#include "spurs_queues.h"
#include "../../runtime/ppu/ppu_memory.h"      /* vm_base */
#include "../../runtime/platform/plat.h"       /* plat_yield */
#include <stdio.h>
#include <string.h>

#define LQ_LINE    128u
#define LQ_NO_PORT 0xFFFFFFFFu

enum { L_POP_DONE = 0x00, L_POP_MASK = 0x02, L_POP_NEXT = 0x06,
       L_PUSH_DONE = 0x08, L_PUSH_MASK = 0x0A, L_PUSH_WAIT = 0x0C, L_PUSH_NEXT = 0x0E,
       L_SIZE = 0x10, L_DEPTH = 0x14, L_BUFFER = 0x18, L_PORT = 0x20, L_DIR = 0x24,
       L_V1 = 0x28, L_INIT = 0x2C, L_PUSH_RING = 0x30, L_PUSH_IDS = 0x32,
       L_POP_RING = 0x50, L_SIGNAL = 0x70, L_V2 = 0x78, L_EQ = 0x7C };
enum { DIR_SPU2SPU = 0, DIR_SPU2PPU = 1, DIR_PPU2SPU = 2, DIR_ANY2ANY = 3 };
enum { INIT_DONE = 2 };
#define RING_SPAN 30u   /* a ring's counters run modulo 30 over 15 id slots */

void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);

uint32_t spurs_lfq_init(uint32_t owner, uint32_t q, uint32_t buffer, uint32_t size,
                        uint32_t depth, uint32_t direction)
{
    if (!q || (size && !buffer)) return SQ_ERR_NULL_POINTER;
    if (size > 0x4000u || (size & 15u) || !depth || depth > 0x7FFFu || direction > DIR_ANY2ANY)
        return SQ_ERR_INVAL;
    if ((q & (LQ_LINE - 1)) || (buffer & 15u)) return SQ_ERR_ALIGN;

    const uint64_t buf_word = (uint64_t)buffer | (direction == DIR_ANY2ANY ? 1u : 0u);
    sq_line_lock(q);
    const uint32_t init = sq_rd32(q + L_INIT);
    if (init) {
        /* Initialised before: the same queue again is fine, anything else is
         * someone else's. */
        uint32_t rc = 0;
        if (init != INIT_DONE) rc = SQ_ERR_STAT;
        else if (sq_rd32(q + L_SIZE) != size || sq_rd32(q + L_DEPTH) != depth ||
                 sq_rd64(q + L_BUFFER) != buf_word || sq_rd64(q + L_SIGNAL) != owner ||
                 sq_rd32(q + L_DIR) != direction)
            rc = SQ_ERR_INVAL;
        sq_line_unlock(q, 0);
        return rc;
    }
    for (uint32_t o = 0; o < LQ_LINE; o++)
        if (vm_base[q + o]) { sq_line_unlock(q, 0); return SQ_ERR_STAT; }

    /* From the zeroed line: the fields, both counter words at 0, both rings
     * empty, no port yet. */
    sq_wr32(q + L_SIZE, size);
    sq_wr32(q + L_DEPTH, depth);
    sq_wr64(q + L_BUFFER, buf_word);
    sq_wr32(q + L_DIR, direction);
    sq_wr64(q + L_SIGNAL, owner);
    if (direction == DIR_ANY2ANY) {
        /* The any-to-any layout: two port bytes unset, the rings marked unused. */
        vm_base[q + L_PORT] = 0xFF; vm_base[q + L_PORT + 1] = 0xFF;
        sq_wr32(q + L_V1, 0xFFFFFFFFu);
        sq_wr16(q + L_PUSH_RING, 0xFFFF);
        sq_wr16(q + L_POP_RING, 0xFFFF);
    } else {
        sq_wr32(q + L_PORT, LQ_NO_PORT);
    }
    sq_wr32(q + L_INIT, INIT_DONE);
    sq_line_unlock(q, 1);
    return 0;
}

uint32_t spurs_lfq_attach(uint32_t q)
{
    if (!q) return SQ_ERR_NULL_POINTER;
    if (q & (LQ_LINE - 1)) return SQ_ERR_ALIGN;
    const uint32_t dir = sq_rd32(q + L_DIR);
    if (dir != DIR_SPU2PPU && dir != DIR_PPU2SPU) return SQ_ERR_PERM;
    if (sq_rd32(q + L_PORT) != LQ_NO_PORT) return SQ_ERR_STAT;
    uint32_t eq = 0, port = 0;
    const uint32_t rc = sq_attach_events(&eq, &port);
    if (rc) return rc;
    sq_line_lock(q);
    sq_wr32(q + L_PORT, port);
    sq_wr32(q + L_EQ, eq);
    sq_line_unlock(q, 1);
    return 0;
}

uint32_t spurs_lfq_detach(uint32_t q)
{
    if (!q) return SQ_ERR_NULL_POINTER;
    if (q & (LQ_LINE - 1)) return SQ_ERR_ALIGN;
    const uint32_t port = sq_rd32(q + L_PORT);
    if (port == LQ_NO_PORT) return SQ_ERR_STAT;
    sq_detach_events(sq_rd32(q + L_EQ), port);
    /* The queue id stays at +0x7C, as libsre leaves it. */
    sq_line_lock(q);
    sq_wr32(q + L_PORT, LQ_NO_PORT);
    sq_line_unlock(q, 1);
    return 0;
}

/* The number of leading one bits of a 16-bit word. */
static unsigned lq_lead_ones16(unsigned m)
{
    unsigned n = 0;
    while (n < 16 && (m & (0x8000u >> n))) n++;
    return n;
}

/* Take a slot to write. 0 with *slot set, or the return code. */
static uint32_t lq_reserve(uint32_t q, uint32_t blocking, uint32_t* slot)
{
    const uint32_t depth = sq_rd32(q + L_DEPTH), span = depth * 2u;
    int resumed = 0, rechecked = 0;
    for (;;) {
        sq_line_lock(q);
        const uint32_t pop_done = sq_rd16(q + L_POP_DONE);
        const uint32_t done = sq_rd16(q + L_PUSH_DONE), next = sq_rd16(q + L_PUSH_NEXT);
        uint32_t waiting = sq_rd16(q + L_PUSH_WAIT);
        int stored = 0, sleep = 0;
        uint32_t rc = 0;
        if (blocking && sq_rd32(q + L_PORT) == LQ_NO_PORT) {
            rc = SQ_ERR_STAT;                          /* nobody could wake us */
        } else {
            /* Back from a sleep: we were the waiting pusher. */
            if (resumed && waiting) { waiting = 0; sq_wr16(q + L_PUSH_WAIT, 0); stored = 1; }
            if (done != next || waiting) {
                rc = SQ_ERR_BUSY;                      /* another push is under way */
            } else if ((next + span - pop_done) % span < depth) {
                *slot = next;
                sq_wr16(q + L_PUSH_NEXT, (uint16_t)(next + 1u == span ? 0u : next + 1u));
                stored = 1;
            } else if (!blocking) {
                rc = SQ_ERR_AGAIN;
            } else {
                /* Full: mark the pusher waiting. The consumer that frees a slot
                 * next sees the mark, clears it to 1 and sends an event. */
                sq_wr16(q + L_PUSH_WAIT, 3);
                stored = 1; sleep = 1;
            }
        }
        const uint32_t eq = sq_rd32(q + L_EQ);
        sq_line_unlock(q, stored);
        if (rc) return rc;
        if (!sleep) {
            if (rechecked) sq_count_lost_wakeup("push", q);
            return 0;
        }
        __atomic_add_fetch(&g_sq_stats.lfq_push_slept, 1, __ATOMIC_RELAXED);
        const uint32_t s = sq_sleep(eq);
        if (s > 1) return s;
        resumed = 1; rechecked = s == 1;
    }
}

/* Mark slot written, and wake the first waiting consumer if there is one. */
static void lq_complete(uint32_t q, uint32_t slot)
{
    const uint32_t depth = sq_rd32(q + L_DEPTH), span = depth * 2u;
    int task = -1;
    for (;;) {
        sq_line_lock(q);
        const uint32_t done = sq_rd16(q + L_PUSH_DONE), mask = sq_rd16(q + L_PUSH_MASK);
        /* Writes may finish out of order: done moves over the run of finished
         * slots that starts at it, and the mask keeps the ones after a gap. */
        const uint32_t off = (slot + span - done) % span;
        const uint32_t m = mask | (off <= 15u ? (0x8000u >> off) : 0u);
        const unsigned run = lq_lead_ones16(m);
        const uint32_t new_mask = run >= 16 ? 0u : (m << run) & 0xFFFFu;
        const uint32_t new_done = (done + run) % span;

        uint32_t ring = sq_rd16(q + L_PUSH_RING);
        uint32_t rd = (ring >> 10) & 31u;
        const uint32_t resumed = (ring >> 5) & 31u, wr = ring & 31u;
        if (rd != wr) {
            const uint32_t pops = (sq_rd16(q + L_POP_NEXT) + span - sq_rd16(q + L_POP_DONE)) % span;
            const uint32_t woken = (rd + RING_SPAN - resumed) % RING_SPAN;
            if (pops + woken > 15u) {
                /* Too many consumers on their way: wait for them to settle. */
                sq_line_unlock(q, 0);
                plat_yield();
                continue;
            }
            task = sq_rd16(q + L_PUSH_IDS + 2u * (rd >= 15u ? rd - 15u : rd));
            rd = rd + 1u == RING_SPAN ? 0u : rd + 1u;
            ring = (ring & 0x83FFu) | (rd << 10);
            sq_wr16(q + L_PUSH_RING, (uint16_t)ring);
        }
        sq_wr16(q + L_PUSH_DONE, (uint16_t)new_done);
        sq_wr16(q + L_PUSH_MASK, (uint16_t)new_mask);
        sq_line_unlock(q, 1);
        break;
    }
    if (task >= 0) {
        __atomic_add_fetch(&g_sq_stats.lfq_signals, 1, __ATOMIC_RELAXED);
        spu_taskset_signal_task((uint32_t)sq_rd64(q + L_SIGNAL), (uint32_t)task);
    }
}

uint32_t spurs_lfq_push(uint32_t q, uint32_t entry, uint32_t blocking)
{
    if (!q || !entry) return SQ_ERR_NULL_POINTER;
    if ((q & (LQ_LINE - 1)) || (entry & 15u)) return SQ_ERR_ALIGN;
    const uint32_t dir = sq_rd32(q + L_DIR);
    if (dir != DIR_PPU2SPU) {
        if (dir == DIR_ANY2ANY) {
            static int s_said;
            if (!s_said) { s_said = 1;
                fprintf(stderr, "[spurs-queues] LFQueue 0x%08X is any-to-any; LIBSRE_OWN pushes only "
                                "PPU-to-SPU queues\n", q); }
        }
        return SQ_ERR_PERM;
    }
    const uint32_t depth = sq_rd32(q + L_DEPTH), size = sq_rd32(q + L_SIZE);
    if (!depth || depth > 0x7FFFu || sq_rd32(q + L_INIT) != INIT_DONE) return SQ_ERR_STAT;
    __atomic_add_fetch(&g_sq_stats.lfq_push, 1, __ATOMIC_RELAXED);
    uint32_t slot = 0;
    const uint32_t rc = lq_reserve(q, blocking, &slot);
    if (rc) return rc;
    const uint32_t buffer = (uint32_t)(sq_rd64(q + L_BUFFER) & ~1ull);
    memcpy(vm_base + buffer + (slot % depth) * size, vm_base + entry, size);
    lq_complete(q, slot);
    return 0;
}
