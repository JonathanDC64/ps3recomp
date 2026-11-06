/*
 * ps3recomp - the SpursQueue family on our own code (LIBSRE_OWN, spurs_queues.h)
 *
 * _cellSpursQueueInitialize, cellSpursQueueAttachLv2EventQueue,
 * cellSpursQueuePopBody and cellSpursQueueDetachLv2EventQueue, for a queue an
 * SPU task pushes to and a PPU thread pops from (Demon's Souls' HighGraphics
 * queue). Also the line transaction and the event plumbing both families use.
 *
 * The line (the port's docs/133, section 9.2; big-endian):
 *   0x00 s32 pop count, its complement while a PPU popper sleeps
 *   0x04 s32 push count, its complement while an SPU push is in flight
 *   0x08 u32 entry size          0x0C u32 depth
 *   0x10 u64 buffer              0x18 u32 event port, 0xFFFFFFFF unattached
 *   0x1C u32 direction           0x30, 0x50 rings of waiting SPU pushers
 *   0x60 u64 taskset             0x68 u64 the taskset's SPURS instance
 *   0x74 u32 the event queue a sleeping popper waits on
 * Counts run modulo twice the depth; count n is in slot n mod depth.
 */
#include "spurs_queues.h"
#include "spurs_taskset.h"                      /* CSTS_SPURS */
#include "../../runtime/ppu/ppu_memory.h"      /* vm_base */
#include "../../runtime/ppu/ppu_context.h"
#include "../../runtime/spu/spu_coherency.h"   /* the lock-line lock, spu_coh_notify_write */
#include "../../runtime/syscalls/sys_event.h"
#include "../../runtime/platform/plat.h"
#include "ps3emu/ps3_knob.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SQ_LINE 128u
#define SQ_NO_PORT 0xFFFFFFFFu

enum { Q_POP = 0x00, Q_PUSH = 0x04, Q_SIZE = 0x08, Q_DEPTH = 0x0C, Q_BUFFER = 0x10,
       Q_PORT = 0x18, Q_DIR = 0x1C, Q_RING1 = 0x30, Q_RING2 = 0x50, Q_TASKSET = 0x60,
       Q_SPURS = 0x68, Q_EQ = 0x74 };
enum { DIR_SPU2SPU = 0, DIR_SPU2PPU = 1, DIR_PPU2SPU = 2 };

sq_stats_t g_sq_stats;

/* The hooks an SPU PUTLLUC runs around its store (spu_channels.c): the PPU
 * store-conditional locks of the line, its PPU reservations, and the
 * runtime's own watchers of committed lines. */
extern void (*g_ppu_resv_line_enter)(uint32_t line);
extern void (*g_ppu_resv_line_break)(uint32_t line);
extern void (*g_ppu_resv_line_leave)(uint32_t line);
extern void (*g_spu_line_commit_hook)(uint32_t line);
void spu_taskset_line_committed(uint32_t line);
extern uint32_t spurs_hle_connect_port_mask(uint32_t queue, uint64_t mask, uint8_t* port_out);
extern uint32_t spurs_hle_disconnect_port(uint32_t port);

/* ---- LIBSRE_OWN ------------------------------------------------------- */

int spurs_own_families(void)
{
    static volatile int s_own = -1;
    int v = s_own;
    if (v >= 0) return v;
    const char* e = getenv("LIBSRE_OWN");
    v = 0;
    if (e && (!strcmp(e, "1") || !strcmp(e, "all"))) v = SQ_LIBSRE_OWN_LF | SQ_LIBSRE_OWN_QUEUE;
    else if (e && !strcmp(e, "lf"))                    v = SQ_LIBSRE_OWN_LF;
    else if (e && !strcmp(e, "queue"))                 v = SQ_LIBSRE_OWN_QUEUE;
    else if (e && *e && strcmp(e, "0"))
        fprintf(stderr, "[spurs-queues] LIBSRE_OWN=%s: not 0, 1, all, lf or queue; off\n", e);
    if (v)
        fprintf(stderr, "[spurs-queues] LIBSRE_OWN: the %s on ps3recomp's own code\n",
                v == 3 ? "LFQueue and SpursQueue families" : v == 1 ? "LFQueue family" : "SpursQueue family");
    s_own = v;
    return v;
}

int spurs_own_nid(uint32_t nid)
{
    switch (nid) {
    case 0x011EE38Bu: case 0x1656D49Fu: case 0x8A85674Du: case 0x73E06F91u:   /* LFQueue */
        return (spurs_own_families() & SQ_LIBSRE_OWN_LF) != 0;
    case 0x082BFB09u: case 0xE5443BE7u: case 0x039D70B7u: case 0x91066667u:   /* SpursQueue */
        return (spurs_own_families() & SQ_LIBSRE_OWN_QUEUE) != 0;
    default:
        return 0;
    }
}

const char* spurs_own_name(uint32_t nid)
{
    switch (nid) {
    case 0x011EE38Bu: return "_cellSpursLFQueueInitialize";
    case 0x1656D49Fu: return "cellSpursLFQueueAttachLv2EventQueue";
    case 0x8A85674Du: return "_cellSpursLFQueuePushBody";
    case 0x73E06F91u: return "cellSpursLFQueueDetachLv2EventQueue";
    case 0x082BFB09u: return "_cellSpursQueueInitialize";
    case 0xE5443BE7u: return "cellSpursQueueAttachLv2EventQueue";
    case 0x039D70B7u: return "cellSpursQueueDetachLv2EventQueue";
    case 0x91066667u: return "cellSpursQueuePopBody";
    default:          return "?";
    }
}

uint32_t spurs_own_call(uint32_t nid, const uint64_t* gpr)
{
    const uint32_t a0 = (uint32_t)gpr[3], a1 = (uint32_t)gpr[4], a2 = (uint32_t)gpr[5],
                   a3 = (uint32_t)gpr[6], a4 = (uint32_t)gpr[7], a5 = (uint32_t)gpr[8],
                   a6 = (uint32_t)gpr[9];
    switch (nid) {
    case 0x011EE38Bu: return spurs_lfq_init(a0, a1, a2, a3, a4, a5);
    case 0x1656D49Fu: return spurs_lfq_attach(a0);
    case 0x8A85674Du: return spurs_lfq_push(a0, a1, a2);
    case 0x73E06F91u: return spurs_lfq_detach(a0);
    case 0x082BFB09u: return spurs_q_init(a0, a1, a2, a3, a4, a5, a6);
    case 0xE5443BE7u: return spurs_q_attach(a0);
    case 0x039D70B7u: return spurs_q_detach(a0);
    case 0x91066667u: return spurs_q_pop(a0, a1, a2);
    default:          return SQ_ERR_INVAL;
    }
}

/* ---- The line transaction --------------------------------------------- */

/* Lock a queue's line as an SPU PUTLLUC does: the PPU store-conditional locks
 * first, then the lock-line lock. Between lock and unlock nobody else reads
 * or writes the line consistently, so a decision and the store it leads to
 * are one step. */
void sq_line_lock(uint32_t line)
{
    if (g_ppu_resv_line_enter) g_ppu_resv_line_enter(line);
    spu_lockline_lock_ea(line, SQ_LINE);
}

/* stored: the line was written. Then every reservation on it is lost (an
 * SPU's PUTLLC in flight fails and reads the line again, a reserving SPU
 * takes its LR event, a PPU stwcx. fails), and the runtime's line watchers
 * run, as after an SPU commit. */
void sq_line_unlock(uint32_t line, int stored)
{
    if (stored) {
        if (g_ppu_resv_line_break) g_ppu_resv_line_break(line);
        spu_coh_notify_write(line);
    }
    spu_lockline_unlock_ea(line, SQ_LINE);
    if (g_ppu_resv_line_leave) g_ppu_resv_line_leave(line);
    if (stored) {
        spu_taskset_line_committed(line);
        if (g_spu_line_commit_hook) g_spu_line_commit_hook(line);
    }
}

uint32_t sq_rd32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint16_t sq_rd16(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}
uint64_t sq_rd64(uint32_t ea) { return ((uint64_t)sq_rd32(ea) << 32) | sq_rd32(ea + 4); }
void sq_wr32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
void sq_wr16(uint32_t ea, uint16_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
void sq_wr64(uint32_t ea, uint64_t v) { sq_wr32(ea, (uint32_t)(v >> 32)); sq_wr32(ea + 4, (uint32_t)v); }

/* ---- Events ------------------------------------------------------------ */

uint32_t sq_attach_events(uint32_t* eq, uint32_t* port)
{
    /* A local queue (key 0) of one event, as libsre makes it. */
    const uint32_t id = sys_event_queue_create_direct(0, 1);
    if (!id) return 0x80010001u;                       /* CELL_EAGAIN: no free queue */
    uint8_t p = 0;
    const uint32_t rc = spurs_hle_connect_port_mask(id, 0xFFFFFFFFFFFF0000ull, &p);
    if (rc) {
        sq_detach_events(id, SQ_NO_PORT);
        return rc;
    }
    *eq = id; *port = p;
    return 0;
}

void sq_detach_events(uint32_t eq, uint32_t port)
{
    if (port != SQ_NO_PORT) spurs_hle_disconnect_port(port);
    if (eq) {
        ppu_context c;
        memset(&c, 0, sizeof c);
        c.gpr[3] = eq; c.gpr[4] = 1;                    /* SYS_EVENT_QUEUE_DESTROY_FORCE */
        (void)sys_event_queue_destroy(&c);
    }
}

uint32_t sq_sleep(uint32_t eq)
{
    static int s_ms = -1;
    PS3_KNOB(s_ms, ps3_env_int("LIBSRE_OWN_RECHECK_MS", 1000));
    extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
    /* A copy of the caller's context, so the wait is listed under its thread
     * and the wait diagnostics see its stack; only r3 to r7 are ours. */
    ppu_context c;
    if (g_active_ctx) c = *g_active_ctx;
    else memset(&c, 0, sizeof c);
    c.gpr[3] = eq; c.gpr[4] = 0;
    c.gpr[5] = s_ms > 0 ? (uint64_t)s_ms * 1000u : 0;  /* microseconds, 0 = for ever */
    const uint32_t rc = (uint32_t)sys_event_queue_receive(&c);
    if (rc != 0x8001000Bu) return rc;                  /* woken, or the queue is gone */
    /* CELL_ETIMEDOUT. An event that came in just as the time ran out is a
     * wake-up all the same, not a lost one. */
    extern int sys_event_queue_receive_host(uint32_t queue_id, sys_event_t* out, unsigned timeout_ms);
    sys_event_t late;
    return sys_event_queue_receive_host(eq, &late, 0) == 1 ? 0 : 1;
}

void sq_count_lost_wakeup(const char* what, uint32_t q)
{
    const long n = __atomic_add_fetch(&g_sq_stats.lost_wakeups, 1, __ATOMIC_RELAXED);
    if (n <= 8 || (n & (n - 1)) == 0)
        fprintf(stderr, "[spurs-queues] lost wake-up: a %s on queue 0x%08X slept past the recheck time "
                        "with its queue ready (%ld so far)\n", what, q, n);
}

/* ---- The SpursQueue ---------------------------------------------------- */

uint32_t spurs_q_init(uint32_t spurs, uint32_t taskset, uint32_t q, uint32_t buffer,
                      uint32_t size, uint32_t depth, uint32_t direction)
{
    if (!q || (!taskset && !spurs) || (size && !buffer)) return SQ_ERR_NULL_POINTER;
    if (size > 0x4000u || (size & 15u) || !depth || depth > 0x7FFFu || direction > DIR_PPU2SPU)
        return SQ_ERR_INVAL;
    if ((q & (SQ_LINE - 1)) || (buffer & 15u) || (taskset & (SQ_LINE - 1))) return SQ_ERR_ALIGN;

    /* The SPURS instance of the taskset (its +0x60), or the one passed. */
    const uint64_t owner = taskset ? sq_rd64(taskset + CSTS_SPURS) : spurs;
    sq_line_lock(q);
    memset(vm_base + q, 0, SQ_LINE);
    sq_wr32(q + Q_SIZE, size);
    sq_wr32(q + Q_DEPTH, depth);
    sq_wr64(q + Q_BUFFER, buffer);
    sq_wr32(q + Q_PORT, SQ_NO_PORT);
    sq_wr32(q + Q_DIR, direction);
    sq_wr64(q + Q_TASKSET, taskset);
    sq_wr64(q + Q_SPURS, owner);
    sq_line_unlock(q, 1);
    return 0;
}

uint32_t spurs_q_attach(uint32_t q)
{
    if (!q) return SQ_ERR_NULL_POINTER;
    if (q & (SQ_LINE - 1)) return SQ_ERR_ALIGN;
    if (sq_rd32(q + Q_PORT) != SQ_NO_PORT) return SQ_ERR_STAT;
    uint32_t eq = 0, port = 0;
    const uint32_t rc = sq_attach_events(&eq, &port);
    if (rc) return rc;
    sq_line_lock(q);
    sq_wr32(q + Q_PORT, port);
    sq_wr32(q + Q_EQ, eq);
    sq_line_unlock(q, 1);
    return 0;
}

uint32_t spurs_q_detach(uint32_t q)
{
    if (!q) return SQ_ERR_NULL_POINTER;
    if (q & (SQ_LINE - 1)) return SQ_ERR_ALIGN;
    const uint32_t port = sq_rd32(q + Q_PORT);
    if (port == SQ_NO_PORT) return SQ_ERR_STAT;
    sq_detach_events(sq_rd32(q + Q_EQ), port);
    sq_line_lock(q);
    sq_wr32(q + Q_PORT, SQ_NO_PORT);
    sq_line_unlock(q, 1);
    return 0;
}

static int32_t sq_normal(uint32_t w) { return (int32_t)w < 0 ? (int32_t)~w : (int32_t)w; }

uint32_t spurs_q_pop(uint32_t q, uint32_t dest, uint32_t blocking)
{
    if (!q || !dest) return SQ_ERR_NULL_POINTER;
    if ((q & (SQ_LINE - 1)) || (dest & 15u)) return SQ_ERR_ALIGN;
    if (sq_rd32(q + Q_DIR) != DIR_SPU2PPU) return SQ_ERR_PERM;
    const uint32_t size = sq_rd32(q + Q_SIZE), depth = sq_rd32(q + Q_DEPTH);
    const uint32_t buffer = (uint32_t)sq_rd64(q + Q_BUFFER);
    if (!depth || depth > 0x7FFFu) return SQ_ERR_STAT;
    if (blocking && sq_rd32(q + Q_PORT) == SQ_NO_PORT) return SQ_ERR_STAT;
    const uint32_t span = depth * 2u;
    int rechecked = 0;
    __atomic_add_fetch(&g_sq_stats.q_pop, 1, __ATOMIC_RELAXED);
    for (;;) {
        sq_line_lock(q);
        const uint32_t pop  = (uint32_t)sq_normal(sq_rd32(q + Q_POP));
        const uint32_t push = (uint32_t)sq_normal(sq_rd32(q + Q_PUSH));
        const uint32_t ready = (push + span - pop) % span;
        if (ready) {
            memcpy(vm_base + dest, vm_base + buffer + (pop % depth) * size, size);
            sq_wr32(q + Q_POP, (pop + 1u) % span);
            /* An SPU pusher waiting for room would be named in a ring at +0x30 or
             * +0x50. Demon's Souls' pushers try again instead and never wait;
             * waking one is not done here. */
            const int waiter = vm_base[q + Q_RING1] || vm_base[q + Q_RING2];
            sq_line_unlock(q, 1);
            if (rechecked) sq_count_lost_wakeup("pop", q);
            if (waiter) {
                static int s_said;
                if (!s_said) { s_said = 1;
                    fprintf(stderr, "[spurs-queues] queue 0x%08X: an SPU pusher waits for room; "
                                    "LIBSRE_OWN does not wake one\n", q); }
            }
            return 0;
        }
        if (!blocking) {
            sq_line_unlock(q, 0);
            __atomic_add_fetch(&g_sq_stats.q_again, 1, __ATOMIC_RELAXED);
            return SQ_ERR_AGAIN;
        }
        /* Empty: mark the popper asleep. An SPU push that commits after this
         * store sees the mark and sends an event; one that read the line
         * before it loses its reservation here and reads it again. */
        sq_wr32(q + Q_POP, ~pop);
        const uint32_t eq = sq_rd32(q + Q_EQ);
        sq_line_unlock(q, 1);
        __atomic_add_fetch(&g_sq_stats.q_pop_slept, 1, __ATOMIC_RELAXED);
        const uint32_t rc = sq_sleep(eq);
        if (rc > 1) return rc;
        rechecked = rc == 1;
    }
}
