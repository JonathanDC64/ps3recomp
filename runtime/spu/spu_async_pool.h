/* spu_async_pool.h -- host threads for async SPU jobs (spu_async_pool.c).
 *
 * spu_workload.c runs each async SPU job on a host thread of its own, so the
 * PPU caller is not blocked. These are those threads: a worker that finished
 * its job waits for the next one instead of exiting. Every platform uses it.
 */
#ifndef PS3RECOMP_SPU_ASYNC_POOL_H
#define PS3RECOMP_SPU_ASYNC_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spu_async_pool_fn)(void* arg);

/* Run fn(arg) on an idle worker, or on a new thread. own_thread: always a new
 * thread, which exits after the job instead of waiting for another.
 * Returns 0 if no thread could be made; fn has not run then. */
int spu_async_pool_start(spu_async_pool_fn fn, void* arg, int own_thread);

/* Workers waiting for a job now (for tests and diagnostics). */
int spu_async_pool_idle(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SPU_ASYNC_POOL_H */
