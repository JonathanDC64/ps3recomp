/* spurs_task.c -- SPURS leaf-task lifecycle helpers (HLE of the taskset PM bits
 * we need without running the real policy module).
 *
 * A SPURS leaf task signals the kernel by executing `stop <code>`:
 *   CELL_SPURS_TASK_SYSCALL_EXIT = 0  (the task is done; its exit code is read by
 *   the PPU via cellSpursTaskExitCodeTryGet / cellSpursJoinTask).
 * When we dispatch a task ELF directly (no policy module), nothing emulates the
 * PM's "on task exit" step. This helper does the minimal piece: on `stop 0`, write
 * the task's exit code into the exit-code container the game registered via
 * cellSpursTaskAttributeSetExitCodeContainer.
 *
 * Container layout (CellSpursTaskExitCode, 128 B): the PPU side polls a "code
 * present" marker then reads the code. We write the code at +0 and a non-zero
 * "valid" word at +0x80-... no -- keep it minimal + match what cellSpurs.c's
 * TryGet/Join read: code at +0, validity flag at +4 (1 = exited). Adjust when the
 * real DeS poll layout is confirmed.
 *
 * The exit-code REGISTER convention (which GPR holds the code at `stop 0`) is not
 * yet confirmed against the real DeS task; we read the preferred slot of r3 (the
 * conventional result register) -- a parameterizable assumption the unit test pins.
 */
#include "spu_context.h"
#include <stdint.h>

extern uint8_t* vm_base;   /* host-provided guest memory base */

/* Returns 1 if the task EXITed (`stop 0`) and the exit code was written to the
 * container; 0 if it stopped for another reason (different stop code / halt). */
int spu_spurs_task_write_exit_code(spu_context* ctx, uint32_t exitcode_ea)
{
    if (!ctx) return 0;
    if (ctx->status != SPU_STATUS_STOPPED_BY_STOP) return 0;
    if (ctx->stop_code != 0u) return 0;        /* not CELL_SPURS_TASK_SYSCALL_EXIT */

    uint32_t code = ctx->gpr[3]._u32[0];        /* assumed exit-code register */
    if (exitcode_ea && vm_base) {
        uint8_t* c = vm_base + exitcode_ea;
        c[0] = (uint8_t)(code >> 24); c[1] = (uint8_t)(code >> 16);
        c[2] = (uint8_t)(code >> 8);  c[3] = (uint8_t)code;        /* exit code (BE) */
        c[4] = 0; c[5] = 0; c[6] = 0; c[7] = 1;                    /* valid = 1 (BE) */
    }
    return 1;
}
