/*
 * ps3recomp - our own SPURS queue families (LIBSRE_OWN)
 *
 * The PPU half of the two SPURS queues whose other half is a title's own SPU
 * code: the LFQueue (initialize, attach, push, detach; spurs_lfqueue.c) and
 * the SpursQueue (initialize, attach, pop, detach; spurs_queue.c). A queue is
 * one 128-byte, 128-aligned guest line that the SPU side edits with
 * GETLLAR/PUTLLC, so the bytes of that line are the interface: these follow
 * the layout and protocol the loaded libsre was traced using (the port's
 * docs/133, section 9). Every change to a line is one transaction under the
 * runtime's lock-line lock that breaks the SPU and PPU reservations on it,
 * the host-side form of the PPU's lwarx/stwcx.
 *
 * LIBSRE_OWN picks them, read once:
 *   unset or 0   off: the HLE keeps its old handlers, and a loaded libsre
 *                (LIBSRE_LLE) serves what LIBSRE_LLE_ONLY lists, as before.
 *   1 or all     both families on this code, even where LIBSRE_LLE_ONLY
 *                lists them.
 *   lf           the LFQueue family only.
 *   queue        the SpursQueue family only.
 * LIBSRE_OWN_RECHECK_MS (default 1000; 0 waits as libsre does, for ever): a
 * push or pop asleep that long looks at its queue again, and if the queue
 * has what it waits for, takes it and counts a lost wake-up (logged).
 */
#ifndef SPURS_QUEUES_H
#define SPURS_QUEUES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The libsre error codes the queue functions return (the SPURS task range,
 * 0x804109xx; cellSpurs.h's CELL_SPURS_TASK_ERROR_* sit elsewhere). */
#define SQ_ERR_AGAIN        0x80410901u
#define SQ_ERR_INVAL        0x80410902u
#define SQ_ERR_PERM         0x80410909u
#define SQ_ERR_BUSY         0x8041090Au
#define SQ_ERR_STAT         0x8041090Fu
#define SQ_ERR_ALIGN        0x80410910u
#define SQ_ERR_NULL_POINTER 0x80410911u

#define SQ_LIBSRE_OWN_LF    1
#define SQ_LIBSRE_OWN_QUEUE 2

/* Which families LIBSRE_OWN puts on this code (SQ_LIBSRE_OWN_* bits). */
int spurs_own_families(void);
/* 1 when nid is one of the eight and LIBSRE_OWN covers its family. The HLE
 * dispatcher then sends it to spurs_own_call, ahead of a loaded libsre and of
 * the HLE table (whose old handlers stay as they were for LIBSRE_OWN off). */
int spurs_own_nid(uint32_t nid);
const char* spurs_own_name(uint32_t nid);
/* Run one of the eight with the caller's r3..r9 (gpr is r0..r31); the libsre
 * return code. */
uint32_t spurs_own_call(uint32_t nid, const uint64_t* gpr);

/* LFQueue (spurs_lfqueue.c). Guest addresses; the libsre return code. */
uint32_t spurs_lfq_init(uint32_t owner, uint32_t q, uint32_t buffer, uint32_t size,
                        uint32_t depth, uint32_t direction);
uint32_t spurs_lfq_attach(uint32_t q);
uint32_t spurs_lfq_detach(uint32_t q);
uint32_t spurs_lfq_push(uint32_t q, uint32_t entry, uint32_t blocking);

/* SpursQueue (spurs_queue.c). */
uint32_t spurs_q_init(uint32_t spurs, uint32_t taskset, uint32_t q, uint32_t buffer,
                      uint32_t size, uint32_t depth, uint32_t direction);
uint32_t spurs_q_attach(uint32_t q);
uint32_t spurs_q_detach(uint32_t q);
uint32_t spurs_q_pop(uint32_t q, uint32_t dest, uint32_t blocking);

/* What both use (spurs_queue.c). */
void     sq_line_lock(uint32_t line);
void     sq_line_unlock(uint32_t line, int stored);
uint32_t sq_rd32(uint32_t ea);
uint16_t sq_rd16(uint32_t ea);
uint64_t sq_rd64(uint32_t ea);
void     sq_wr32(uint32_t ea, uint32_t v);
void     sq_wr16(uint32_t ea, uint16_t v);
void     sq_wr64(uint32_t ea, uint64_t v);
/* An event queue for a queue's sleeper, connected to the SPURS SPU threads on
 * a port from 16 to 63, as libsre's attach does. 0 with *eq and *port set, or
 * an lv2 error. */
uint32_t sq_attach_events(uint32_t* eq, uint32_t* port);
void     sq_detach_events(uint32_t eq, uint32_t port);
/* Sleep on event queue eq. 0 woken by an event, 1 the recheck time passed,
 * or an lv2 error (the queue was destroyed or cancelled). */
uint32_t sq_sleep(uint32_t eq);
void     sq_count_lost_wakeup(const char* what, uint32_t q);

/* Counters, for tests and the log. */
typedef struct {
    volatile long lfq_push, lfq_push_slept, lfq_signals;
    volatile long q_pop, q_pop_slept, q_again;
    volatile long lost_wakeups;
} sq_stats_t;
extern sq_stats_t g_sq_stats;

#ifdef __cplusplus
}
#endif

#endif /* SPURS_QUEUES_H */
