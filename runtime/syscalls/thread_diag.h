/* thread_diag.h -- lightweight per-thread name + current-wait registry.
 *
 * Purpose: at a boot wedge, answer "which thread is the stalled main-logic
 * thread and exactly what wait object is it blocked on?". Each guest PPU thread
 * tags itself with its guest name on start; each blocking wait syscall records
 * (wait-type, object id) before it blocks and clears it on wake. thrdiag_dump()
 * prints every registered thread with its name + current wait state, so the
 * watchdog (or any caller) can snapshot the whole thread graph. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Tag the calling thread with a guest name (idempotent; registers a slot). */
void thrdiag_set_name(const char* name);
/* Record that the calling thread is about to block on (type, obj). */
void thrdiag_wait(const char* type, uint32_t obj);
/* Record that the calling thread woke from its last wait. */
void thrdiag_wake(void);
/* Record the most recent HLE function the calling thread entered (by name). */
void thrdiag_hle(const char* name);
/* Print every registered thread: name, tid, running/WAITING, wait type+obj, wait count. */
void thrdiag_dump(void);

#ifdef __cplusplus
}
#endif
