/* cellspurs_layout.c -- write the CellSpurs SPU/event substrate at the real BE offsets.
 * See cellspurs_layout.h. RPCS3-cross-referenced (docs/15). */
#include "cellspurs_layout.h"

/* Exposed to the SPU completion path (spu_workload.c): the real lv2 SPU thread
 * ids the P2 substrate registered, so the SPURS USER event carries the id the
 * service's handler B looks up (docs/15 §3, event data1 = sending SPU lv2 id).
 * Hardcoding data1=1 misses the lookup -> handler recovery -> no dispatch. */
uint32_t g_cs_spu_lv2[CS_MAX_SPUS] = {0};
uint32_t g_cs_nspus = 0;
uint32_t g_cs_spurs_ea = 0;
uint8_t  g_cs_spu_port = 0;

/* Set/clear a taskId bit in a 128-bit taskset bitset (MSB-first, 4x u32 BE). */
static void ts_setbit(uint32_t ts_ea, uint32_t off, uint32_t taskId, int set)
{
    if (taskId >= 128) return;
    uint32_t a = ts_ea + off + (taskId >> 5) * 4u;
    uint32_t w = vm_read32(a);
    uint32_t bit = 0x80000000u >> (taskId & 31u);
    if (set) w |= bit; else w &= ~bit;
    vm_write32(a, w);
}

void spurs_taskset_task_start(uint32_t taskset_ea, uint32_t taskId)
{
    if (!taskset_ea) return;
    ts_setbit(taskset_ea, TS_RUNNING, taskId, 1);
    uint32_t wid   = vm_read32(taskset_ea + TS_WID);
    uint32_t spurs = vm_read32(taskset_ea + TS_SPURS);
    if (spurs && wid < 16) {
        uint32_t a = spurs + CS_WKL_CONTENTION + wid;
        uint8_t c = vm_read8(a); if (c < 255) vm_write8(a, (uint8_t)(c + 1));
    }
}

void spurs_taskset_task_done(uint32_t taskset_ea, uint32_t taskId)
{
    if (!taskset_ea) return;
    ts_setbit(taskset_ea, TS_RUNNING, taskId, 0);
    ts_setbit(taskset_ea, TS_ENABLED, taskId, 0);   /* DONE marker (frees the slot) */
    uint32_t wid   = vm_read32(taskset_ea + TS_WID);
    uint32_t spurs = vm_read32(taskset_ea + TS_SPURS);
    if (spurs && wid < 16) {
        uint32_t a = spurs + CS_WKL_CONTENTION + wid;
        uint8_t c = vm_read8(a); if (c > 0) vm_write8(a, (uint8_t)(c - 1));
    }
}

void cellspurs_write_substrate(uint32_t spurs_ea, uint32_t spuTG,
                               const uint32_t* spus, uint32_t nSpus,
                               uint8_t spuPort, uint32_t eventQueue, uint32_t eventPort)
{
    if (nSpus > CS_MAX_SPUS) nSpus = CS_MAX_SPUS;
    g_cs_nspus = nSpus; g_cs_spurs_ea = spurs_ea; g_cs_spu_port = spuPort;
    for (uint32_t i = 0; i < nSpus; i++) g_cs_spu_lv2[i] = spus ? spus[i] : 0u;
    vm_write32(spurs_ea + CS_SPU_TG, spuTG);
    for (uint32_t i = 0; i < nSpus; i++)
        vm_write32(spurs_ea + CS_SPUS + i * 4u, spus ? spus[i] : 0u);
    vm_write8 (spurs_ea + CS_SPU_PORT,    spuPort);
    vm_write32(spurs_ea + CS_EVENT_QUEUE, eventQueue);
    vm_write32(spurs_ea + CS_EVENT_PORT,  eventPort);
    vm_write8 (spurs_ea + CS_HANDLER_DIRTY, 0);
}
