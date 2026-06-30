/* SPURS leaf-task EXIT -- host (PPU-side) harness.
 *
 * Runs the lifted task from gen_test_taskexit.py (sets r3 = exit code, then
 * `stop 0` = CELL_SPURS_TASK_SYSCALL_EXIT) and verifies the runtime helper
 * spu_spurs_task_write_exit_code() detects the EXIT and writes the code into the
 * exit-code container -- the task-exit-code completion mechanism func_00A31158
 * (DeS boot) depends on.
 */
#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spurs_task.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint8_t g_mem[4096];
uint8_t* vm_base = g_mem;

/* The task uses no channels/branches; provide stubs so the link is self-contained. */
u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return spu_zero(); }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }
void spu_wrch(spu_context* ctx, uint32_t ch, u128 v) { (void)ctx; (void)ch; (void)v; }
void spu_indirect_branch(spu_context* ctx) { (void)ctx; fprintf(stderr, "FAIL: unexpected indirect branch\n"); }
void spu_register_function(uint32_t a, void (*f)(spu_context*)) { (void)a; (void)f; }

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

int main(void) {
    memset(g_mem, 0, sizeof(g_mem));
    const uint32_t kContainer = 0x100;     /* exit-code container guest EA */
    const uint32_t kExpect    = 0x00001234;

    spu_context ctx;
    spu_context_init(&ctx, 0);
    spu_func_00000000(&ctx);               /* run the task -> stop 0 */

    int exited  = spu_spurs_task_write_exit_code(&ctx, kContainer);
    uint32_t got_code  = be32(&g_mem[kContainer + 0]);
    uint32_t got_valid = be32(&g_mem[kContainer + 4]);

    int stop_ok  = (ctx.status == SPU_STATUS_STOPPED_BY_STOP && ctx.stop_code == 0);
    int write_ok = (exited && got_code == kExpect && got_valid == 1);

    printf("  [stop]    status=0x%X stop_code=0x%X  %s\n",
           ctx.status, ctx.stop_code, stop_ok ? "OK (EXIT)" : "FAIL");
    printf("  [exit]    container[0]=0x%08X valid=%u (expected 0x%08X,1)  %s\n",
           got_code, got_valid, kExpect, write_ok ? "OK" : "FAIL");

    if (stop_ok && write_ok)
        printf("  PASS: leaf-task stop 0 -> exit code written to container.\n");
    return (stop_ok && write_ok) ? 0 : 1;
}
