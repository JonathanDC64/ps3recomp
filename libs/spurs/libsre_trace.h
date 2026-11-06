/*
 * ps3recomp - a trace of the SPURS queue calls (LIBSRE_TRACE)
 *
 * LIBSRE_TRACE=<file>: write one record for each call of the SPURS queue
 * functions (the LFQueue and SpursQueue families, and cellSyncLFQueueGetDirection)
 * to <file>, whichever side serves the call: the loaded libsre (LIBSRE_LLE) or
 * the HLE. A record holds the arguments, the return value, the time the call
 * took, the queue's 128-byte line before the call and what changed in it by
 * the end, and for a push or a pop the entry and the buffer slot it matches.
 * While such a call runs, the lv2 syscalls and the other imports its thread
 * makes are recorded too, and so is every SPU PUTLLC or PUTLLUC that changes
 * one of the queue lines seen so far. The format is in libsre_trace.c.
 *
 * It is a developer's tool: off by default, and one load and branch per HLE
 * call while off. LIBSRE_TRACE_MAX=<n> caps the records (default 2000000).
 */
#ifndef LIBSRE_TRACE_H
#define LIBSRE_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -1 until the knob is read, then 0 (off) or 1 (on). */
extern volatile int g_libsre_trace_on;
int libsre_trace_init(void);

static inline int libsre_trace_active(void)
{
    const int v = g_libsre_trace_on;
    return v < 0 ? libsre_trace_init() : v;
}

/* Around one HLE dispatch (ps3_hle_call). enter returns a token, or NULL when
 * the call is neither a traced function nor made inside one. gpr is the
 * calling thread's r0..r31. lle marks that the loaded libsre served the call.
 * leave takes the call's r3 at its end. */
void* libsre_trace_enter(uint32_t nid, const uint64_t* gpr, uint32_t tid, uint32_t lr);
void  libsre_trace_lle(void* tok);
void  libsre_trace_leave(void* tok, uint64_t r3);

#ifdef __cplusplus
}
#endif

#endif /* LIBSRE_TRACE_H */
