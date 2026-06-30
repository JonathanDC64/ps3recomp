/* SPU lock-line reservation-lost (SPU_EVENT_LR) delivery -- unit test.
 *
 * The SPURS coordinator task getllar's a control line, arms SPU_EVENT_LR (0x400),
 * and sleeps on RdEventStat until another agent (the PPU) writes that line. This
 * test drives the real channel API (spu_channels.c) directly:
 *   getllar a line + arm LR -> RdEventStat == 0
 *   spu_reservation_notify_write(line) [PPU write] -> RdEventStat == SPU_EVENT_LR
 *   write to an UNRELATED line -> no LR
 * No lifted SPU program needed; this exercises the runtime primitive in isolation.
 *
 * Build (standalone): clang -std=c11 -I <runtime/spu> test_lr_event_main.c \
 *                       <runtime/spu>/spu_channels.c -o test_lr_event.exe
 */
#include "spu_context.h"
#include "spu_dma.h"
#include <stdio.h>
#include <string.h>

/* Channel ABI (normally in the lifter-generated spu_recomp.h; declare here for the
 * standalone test). Implemented by spu_channels.c. */
u128 spu_rdch(spu_context* ctx, uint32_t channel);
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
extern void spu_reservation_notify_write(uint32_t ea);

static uint8_t g_mem[16384];
uint8_t* vm_base = g_mem;

static u128 v32(uint32_t x) { u128 r; memset(&r, 0, sizeof r); r._u32[0] = x; return r; }
static uint32_t rd_evt(spu_context* c) { return spu_rdch(c, SPU_RdEventStat)._u32[0]; }

/* issue a GETLLAR on the 128 B line at `ea` (LS dst `lsa`) via the real MFC path */
static void getllar(spu_context* c, uint32_t lsa, uint32_t ea) {
    spu_wrch(c, MFC_LSA, v32(lsa));
    spu_wrch(c, MFC_EAL, v32(ea));
    spu_wrch(c, MFC_Cmd, v32(MFC_GETLLAR_CMD));
}

int main(void) {
    memset(g_mem, 0, sizeof g_mem);
    spu_context ctx;
    spu_context_init(&ctx, 0);

    spu_wrch(&ctx, SPU_WrEventMask, v32(SPU_EVENT_LR));   /* arm LR */

    getllar(&ctx, 0x100, 0x200);                          /* reserve line 0x200 */
    uint32_t e0 = rd_evt(&ctx);                           /* expect 0 (no event yet) */

    spu_reservation_notify_write(0x240);                  /* PPU write within the 0x200 line */
    uint32_t e1 = rd_evt(&ctx);                           /* expect SPU_EVENT_LR */

    /* negative: re-reserve, then a write to a DIFFERENT line must NOT fire LR */
    spu_wrch(&ctx, SPU_WrEventAck, v32(SPU_EVENT_LR));    /* clear */
    getllar(&ctx, 0x100, 0x200);
    spu_reservation_notify_write(0x800);                  /* unrelated line */
    uint32_t e2 = rd_evt(&ctx);                           /* expect 0 */

    int ok = (e0 == 0) && (e1 == SPU_EVENT_LR) && (e2 == 0);
    printf("  e0=0x%X e1=0x%X e2=0x%X (expect 0, 0x%X, 0)  %s\n",
           e0, e1, e2, SPU_EVENT_LR, ok ? "OK" : "FAIL");
    if (ok) printf("  PASS: PPU write to a getllar'd line delivers SPU_EVENT_LR.\n");
    return ok ? 0 : 1;
}
