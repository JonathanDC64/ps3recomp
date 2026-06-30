/* spu_lifted_job.h — run a lifted SPU job with the SPURS task ABI.
 *
 * The bridge between the lv2 SPU-thread-group layer (runtime/syscalls/lv2_register.c,
 * which runs SPU threads as registered PPU-fallbacks) and the lifted-execution layer
 * (a lifted spu_func from spu_lifter). A SPURS task / SPU thread receives its argument
 * (the job/task descriptor effective address) in r3 and runs against its 256 KB local
 * store; this helper sets that up, runs the lifted entry, and bridges the local store.
 *
 * Wiring into lv2: register `spu_lifted_fallback` for an SPU image's entry point with
 * `user` = the lifted entry fn; lv2's spu_fallback_thread_proc then calls it with
 * (tid, args_ea, args_size, user), and we run the lifted job on that thread's LS.
 */
#ifndef SPU_LIFTED_JOB_H
#define SPU_LIFTED_JOB_H

#include "spu_context.h"
#include <string.h>
#include <stdio.h>

typedef void (*spu_lifted_entry_fn)(spu_context*);

/* Run a lifted SPU job. `local_store` (256 KB, may be NULL) is the SPU thread's
 * local store; it is brought into the context, the job's arg EA is placed in r3
 * (the SPURS task ABI), the lifted entry runs, and the LS is written back.
 * The caller links the channel ABI (spu_wrch/spu_rdch/...) that the lifted code
 * uses to reach DMA / mailboxes / events. Returns the job's exit code (0). */
/* spurs_task_abi: when nonzero, set up r3 per the SPURS task kernel ABI instead
 * of the simple-job ABI. A SPURS task entry expects r3 (128-bit) = a pair:
 *   word0 = {high half 0x40 (kernel marker), low half = DMA tag}
 *   word1 = eaContext  (the task's context save area; the entry DMAs it in)
 * The simple-job ABI just puts the single arg EA in word0. (Verified against the
 * lifted SPURS entry: it checks (r3.word0 >> 16) == 0x40 then DMAs 64 bytes from
 * r3.word1, tag = r3.word0 & 0xFFFF — see spu_func_00003E68/00003ED8.) */
/* r3_override (optional, 4 words, big-endian/native lane values): when non-NULL
 * and spurs_task_abi is set, the full 128-bit r3 the SPURS kernel hands the task
 * is supplied by the caller (captured race-free at dispatch time from the game's
 * eaContext+0x10 descriptor: {0x40-marker handle, eaContext, queue/lock EA,
 * ...}). The claim CAS computes its atomic EA from r3.word2/3 & 0xFFFFFF80, so a
 * zero there locks address 0 and the runtime finds "no ready task". */
/* exitcode_ea: when nonzero and the task EXITs (`stop 0`), its exit code is
 * written to this guest EA (the CellSpursTaskExitCode container the game
 * registered). See spurs_task.c. */
static inline int32_t spu_run_lifted_job_abi(spu_lifted_entry_fn entry,
                                             uint8_t* local_store,
                                             uint32_t args_ea,
                                             int image_id,
                                             int spurs_task_abi,
                                             const uint32_t* r3_override,
                                             uint32_t exitcode_ea)
{
    if (!entry) return -1;
    spu_context ctx;
    spu_context_init(&ctx, 0);
    ctx.image_id = image_id;     /* select this image's indirect-branch table */
    /* Initialize the SPU stack pointer to the top of local store (the SPU ABI
     * expects r1 = top-16, 16-byte aligned, with a NULL back-chain). Without
     * this it is 0 from spu_context_init, so the first `r1 -= frame` wraps
     * negative -> garbage stack -> null function pointers -> branch to LS 0. */
    ctx.gpr[1]._u32[0] = SPU_LS_SIZE - 0x10;   /* 0x3FFF0 for a 256KB LS */
    if (local_store) memcpy(ctx.ls, local_store, SPU_LS_SIZE);  /* job's LS in */
    if (spurs_task_abi == 2) {
        /* SPURS task-START ABI (what the taskset policy module hands a fresh leaf
         * task): r3 = the 16-byte CellSpursTaskArgument (verbatim, big-endian
         * lanes); r4 = {taskset->args (doubleword 0), taskset->spurs EA (doubleword
         * 1)}. The task body reads r3 as its argument quadword; passing 0 makes it
         * loop. r4 is built from the taskset HEADER the PM copied into LS 0x2700. */
        if (r3_override) {
            ctx.gpr[3]._u32[0] = r3_override[0];
            ctx.gpr[3]._u32[1] = r3_override[1];
            ctx.gpr[3]._u32[2] = r3_override[2];
            ctx.gpr[3]._u32[3] = r3_override[3];
        }
        /* r4 = {taskset->args (d0, bytes 0-7), taskset->spurs EA (d1, bytes 8-15)},
         * read from the header spurs_pm_build_context placed at LS 0x2700 (+0x68 args,
         * +0x60 spurs, both big-endian u64). RPCS3 spursTasksetStartTask does exactly
         * this; the leaf uses r4's SPURS base to find what to DMA, so the bare tasksetEA
         * we used before made it DMA from a bad address. Our gpr lanes are big-endian
         * order: _u32[0]=bytes0-3 ... _u32[3]=bytes12-15. */
        {
            const uint8_t* ts = ctx.ls + 0x2700;
            ctx.gpr[4]._u32[0] = ((uint32_t)ts[0x68]<<24)|((uint32_t)ts[0x69]<<16)|((uint32_t)ts[0x6A]<<8)|ts[0x6B]; /* args hi */
            ctx.gpr[4]._u32[1] = ((uint32_t)ts[0x6C]<<24)|((uint32_t)ts[0x6D]<<16)|((uint32_t)ts[0x6E]<<8)|ts[0x6F]; /* args lo */
            ctx.gpr[4]._u32[2] = ((uint32_t)ts[0x60]<<24)|((uint32_t)ts[0x61]<<16)|((uint32_t)ts[0x62]<<8)|ts[0x63]; /* spurs hi */
            ctx.gpr[4]._u32[3] = ((uint32_t)ts[0x64]<<24)|((uint32_t)ts[0x65]<<16)|((uint32_t)ts[0x66]<<8)|ts[0x67]; /* spurs lo */
        }
        (void)args_ea;
    } else if (spurs_task_abi) {
        if (r3_override) {
            ctx.gpr[3]._u32[0] = r3_override[0];   /* 0x40-marker handle      */
            ctx.gpr[3]._u32[1] = args_ea;          /* eaContext (DMA'd first) */
            ctx.gpr[3]._u32[2] = r3_override[2];   /* queue/lock EA           */
            ctx.gpr[3]._u32[3] = r3_override[3];
        } else {
            ctx.gpr[3]._u32[0] = 0x00400000u;   /* 0x40 marker (>>16==64), DMA tag 0 */
            ctx.gpr[3]._u32[1] = args_ea;       /* eaContext -> r3.word1 */
        }
    } else {
        ctx.gpr[3]._u32[0] = args_ea;                           /* simple-job arg -> r3 */
    }
    { extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
      spu_run_with_halt(entry, &ctx); }                         /* run with halt pad   */
    if (local_store) memcpy(local_store, ctx.ls, SPU_LS_SIZE);  /* LS back out */
    /* SPURS task EXIT: if the task stopped via `stop 0`, hand its exit code to the
     * registered container (emulating the taskset PM's on-task-exit step). */
    if (exitcode_ea) {
        extern int spu_spurs_task_write_exit_code(spu_context*, uint32_t);
        if (spu_spurs_task_write_exit_code(&ctx, exitcode_ea))
            return (int32_t)ctx.gpr[3]._u32[0];                 /* exit code */
    }
    return 0;
}

static inline int32_t spu_run_lifted_job_img(spu_lifted_entry_fn entry,
                                             uint8_t* local_store,
                                             uint32_t args_ea,
                                             int image_id)
{
    return spu_run_lifted_job_abi(entry, local_store, args_ea, image_id, 0, 0, 0);
}

static inline int32_t spu_run_lifted_job(spu_lifted_entry_fn entry,
                                         uint8_t* local_store,
                                         uint32_t args_ea)
{
    return spu_run_lifted_job_img(entry, local_store, args_ea, 0);
}

/* lv2 PPU-fallback wrapper: signature matches spu_ppu_fallback_fn so it can be
 * registered via spu_register_ppu_fallback(entry_point, spu_lifted_fallback, fn).
 * Defined where spu_thread_get_local_store() is available (the lv2 TU); declared
 * here for callers. (uint32_t tid, uint32_t args_ea, uint32_t args_size, void* user) */
int32_t spu_lifted_fallback(uint32_t tid, uint32_t args_ea,
                            uint32_t args_size, void* user);

#endif /* SPU_LIFTED_JOB_H */
