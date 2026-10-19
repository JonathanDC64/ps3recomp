/*
 * Jump-table gotos for lifted SPU code (spu_lifter.py --bi-switch).
 *
 * A `bi` through a jump table normally leaves the lifted function and goes
 * through spu_indirect_branch, which looks the target up in the function
 * table. --bi-switch lifts such a `bi` with a switch ahead of that dispatch:
 * one SPU_RGOTO case per table target, so a target in the same region body is
 * a goto. Any other register value, and every value while spu_bi_switch_on
 * says no, takes the usual dispatch.
 *
 * A case is taken only where the dispatch would run the same lifted function
 * and do nothing else on the way:
 *   - the context runs this image's own code: no resident task, overlay or
 *     code slot (spu_indirect_branch looks those up first) and image_id is
 *     the image's own id, so its lookup is spu_lookup(pc, image_id);
 *   - no taskset task ELF or entry is registered (g_spu_task_regs), so the
 *     dispatch cannot adopt a task on the way;
 *   - no interrupt registers are saved (an iret target restores them);
 *   - nothing needs to see every hop (SPU_RGOTO's own tests);
 *   - spu_bi_switch_verify found, once, that spu_lookup gives each target's
 *     own function and that no target is an address the dispatch handles
 *     specially. Registration is over by then (it runs at startup), and a
 *     later registration cannot win a lookup an earlier one already wins.
 * SPU_BI_SWITCH=0 (live) turns the cases off.
 */
#ifndef SPU_BI_SWITCH_H
#define SPU_BI_SWITCH_H

#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t pc; void (*fn)(spu_context*); } spu_bi_target;
extern volatile int g_spu_bi_switch;   /* spu_drain.c: SPU_BI_SWITCH, 0 until read */
extern int g_spu_task_regs;            /* spu_channels.c */
int spu_registering_image(void);       /* spu_channels.c: the id spu_begin_image set */
int spu_bi_switch_verify(int self, const spu_bi_target* t, unsigned n);

static inline int spu_bi_switch_on(const spu_context* ctx, int self, int* ok,
                                   const spu_bi_target* t, unsigned n)
{
    if (!g_spu_bi_switch || g_spu_goto_slow || g_yz_lockstep_on || g_pm_flow_ctx ||
        g_spu_task_regs)
        return 0;
    if (ctx->image_id != self || ctx->resident_task || ctx->resident_ovl ||
        ctx->irq_saved || ctx->resident_code[0].image_id ||
        ctx->resident_code[1].image_id || ctx->resident_code[2].image_id ||
        ctx->resident_code[3].image_id)
        return 0;
    if (*ok < 0)
        *ok = spu_bi_switch_verify(self, t, n);
    return *ok;
}

#ifdef __cplusplus
}
#endif
#endif /* SPU_BI_SWITCH_H */
