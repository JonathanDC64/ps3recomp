/* cellspurs_layout.h -- real CellSpurs SPU/event-substrate field offsets + BE writer.
 *
 * BIG-ENDIAN WARNING: the guest (PPC) is big-endian; the SPURS service (game code) reads
 * the CellSpurs object at the REAL offsets below. Our previous simplified CellSpurs struct
 * (initialized/nSpus/flags/prefix) put fields at the WRONG offsets, so the service read
 * garbage for spuTG/spus[]/spuPort/eventQueue. Always write these via the BE accessors at
 * the exact offsets (RPCS3 cellSpurs.h cross-reference, docs/15). The game allocates the
 * (large) CellSpurs buffer; we only populate the substrate fields it needs.
 */
#ifndef CELLSPURS_LAYOUT_H
#define CELLSPURS_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Real CellSpurs offsets (RPCS3 cellSpurs.h). */
enum {
    CS_WKL_READY_COUNT1 = 0x00,   /* atomic u8[16] -- ready count, workloads 0-15 */
    CS_WKL_STATE1       = 0x80,   /* u8[16] -- workload state, 0-15 */
    CS_SPU_PORT         = 0xC9,   /* u8     -- SPU event port number (event selector source) */
    CS_PPU0             = 0xD20,  /* u64    -- handler thread id */
    CS_PPU1             = 0xD28,  /* u64    -- event-helper thread id */
    CS_SPU_TG           = 0xD30,  /* u32    -- SPU thread-group id */
    CS_SPUS             = 0xD34,  /* u32[8] -- SPU thread lv2 ids (service registers these) */
    CS_EVENT_QUEUE      = 0xD5C,  /* u32    -- lv2 event queue id */
    CS_EVENT_PORT       = 0xD60,  /* u32    -- lv2 event port id */
    CS_HANDLER_DIRTY    = 0xD64,  /* u8 (atomic) -- handler needs wakeup */
    CS_MAX_SPUS         = 8,
};

/* vm_* byte-swapping guest-memory accessors (defined in runtime/ppu/ppu_loader.cpp; the
 * unit test provides its own over a buffer). */
extern uint32_t vm_read32(uint64_t ea);
extern void     vm_write8 (uint64_t ea, uint8_t  v);
extern void     vm_write32(uint64_t ea, uint32_t v);

/* Write the SPU/event substrate fields into the guest CellSpurs at `spurs_ea` (BE), so the
 * SPURS service finds the SPU thread group, the per-SPU lv2 ids, the SPU port, and the event
 * queue/port. `spus` holds `nSpus` (<= CS_MAX_SPUS) lv2 thread ids. */
void cellspurs_write_substrate(uint32_t spurs_ea, uint32_t spuTG,
                               const uint32_t* spus, uint32_t nSpus,
                               uint8_t spuPort, uint32_t eventQueue, uint32_t eventPort);

#ifdef __cplusplus
}
#endif

#endif /* CELLSPURS_LAYOUT_H */
