/* cellspurs_layout.c -- write the CellSpurs SPU/event substrate at the real BE offsets.
 * See cellspurs_layout.h. RPCS3-cross-referenced (docs/15). */
#include "cellspurs_layout.h"

void cellspurs_write_substrate(uint32_t spurs_ea, uint32_t spuTG,
                               const uint32_t* spus, uint32_t nSpus,
                               uint8_t spuPort, uint32_t eventQueue, uint32_t eventPort)
{
    if (nSpus > CS_MAX_SPUS) nSpus = CS_MAX_SPUS;
    vm_write32(spurs_ea + CS_SPU_TG, spuTG);
    for (uint32_t i = 0; i < nSpus; i++)
        vm_write32(spurs_ea + CS_SPUS + i * 4u, spus ? spus[i] : 0u);
    vm_write8 (spurs_ea + CS_SPU_PORT,    spuPort);
    vm_write32(spurs_ea + CS_EVENT_QUEUE, eventQueue);
    vm_write32(spurs_ea + CS_EVENT_PORT,  eventPort);
    vm_write8 (spurs_ea + CS_HANDLER_DIRTY, 0);
}
