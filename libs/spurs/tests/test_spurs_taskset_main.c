/* SPURS taskset-PM layout (Option B) -- big-endian layout unit tests.
 *
 * These pin the BE-sensitive struct writes that feed REAL lifted SPU code. Every
 * field/bit is read back as RAW BYTES and checked for the correct big-endian
 * pattern + offset, so a byte-swap or offset mistake fails loudly here rather than
 * silently making the SPU read garbage. Offsets cross-referenced vs RPCS3
 * cellSpurs.h (CellSpursTaskset / TaskInfo / atomic_tasks_bitset::get_bit).
 *
 * Build (standalone): clang -std=c11 -I <libs/spurs> test_spurs_taskset_main.c -o ...
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Provide the SAME big-endian semantics as runtime/ppu/ppu_loader.cpp so the test
 * exercises real BE behaviour (vm_write32 stores MSB-first). */
static uint8_t g_mem[0x4000];
static uint32_t bswap32(uint32_t v){ return (v>>24)|((v>>8)&0xFF00)|((v<<8)&0xFF0000)|(v<<24); }
static uint64_t bswap64(uint64_t v){ return ((uint64_t)bswap32((uint32_t)v)<<32)|bswap32((uint32_t)(v>>32)); }
uint32_t vm_read32(uint64_t ea){ uint32_t v; memcpy(&v, g_mem+(uint32_t)ea, 4); return bswap32(v); }
uint64_t vm_read64(uint64_t ea){ uint64_t v; memcpy(&v, g_mem+(uint32_t)ea, 8); return bswap64(v); }
void vm_write32(uint64_t ea, uint32_t v){ v=bswap32(v); memcpy(g_mem+(uint32_t)ea,&v,4); }
void vm_write64(uint64_t ea, uint64_t v){ v=bswap64(v); memcpy(g_mem+(uint32_t)ea,&v,8); }

#include "spurs_taskset.h"

static int fails = 0;
#define CHECK(cond, msg) do { if(!(cond)){ printf("  FAIL: %s\n", msg); fails++; } } while(0)
#define CHECK_BYTES(ea, b0,b1,b2,b3, msg) do { \
    if(!(g_mem[ea]==(b0)&&g_mem[(ea)+1]==(b1)&&g_mem[(ea)+2]==(b2)&&g_mem[(ea)+3]==(b3))){ \
        printf("  FAIL: %s: got %02X %02X %02X %02X\n", msg, g_mem[ea],g_mem[(ea)+1],g_mem[(ea)+2],g_mem[(ea)+3]); fails++; } } while(0)

int main(void) {
    /* ---- BE field write: vm_write32 stores big-endian (MSB first) ---- */
    memset(g_mem, 0, sizeof g_mem);
    vm_write32(0x100, 0x12345678u);
    CHECK_BYTES(0x100, 0x12,0x34,0x56,0x78, "vm_write32 BE order");
    CHECK(vm_read32(0x100) == 0x12345678u, "vm_read32 round-trip");

    /* ---- bitset: bit N at word N/32, mask (1<<31)>>(N%32), BE byte order ----
     * RPCS3 atomic_tasks_bitset::get_bit -> bit 0 = MSB of word 0. */
    memset(g_mem, 0, sizeof g_mem);
    uint32_t bs = 0x200;
    spurs_bitset_set(bs, 0);                 /* bit 0 -> 0x80000000 -> bytes 80 00 00 00 */
    CHECK_BYTES(bs+0, 0x80,0x00,0x00,0x00, "bitset bit0 = MSB word0");
    CHECK(spurs_bitset_test(bs, 0), "bitset_test bit0");
    spurs_bitset_set(bs, 5);                 /* bit 5 -> 0x04000000 -> 84 00 00 00 */
    CHECK_BYTES(bs+0, 0x84,0x00,0x00,0x00, "bitset bit5 in word0");
    spurs_bitset_set(bs, 32);                /* bit 32 -> word1 bit0 -> word1 = 80 00 00 00 */
    CHECK_BYTES(bs+4, 0x80,0x00,0x00,0x00, "bitset bit32 = MSB word1");
    CHECK(spurs_bitset_test(bs, 32), "bitset_test bit32");
    CHECK(!spurs_bitset_test(bs, 1), "bitset bit1 still clear");
    spurs_bitset_clear(bs, 5);
    CHECK_BYTES(bs+0, 0x80,0x00,0x00,0x00, "bitset clear bit5 leaves bit0");

    /* ---- TaskInfo EA math (cross-ref: TaskInfo is 48 bytes, array @0x80) ---- */
    uint32_t ts = 0x45B4B300u;
    CHECK(spurs_taskset_taskinfo_ea(ts,0) == ts+0x80, "taskinfo[0] @+0x80");
    CHECK(spurs_taskset_taskinfo_ea(ts,1) == ts+0x80+48, "taskinfo[1] @+0x80+48");

    /* ---- a TaskInfo write round-trips BE (elf @+0x10, context @+0x18) ---- */
    memset(g_mem, 0, sizeof g_mem);
    uint32_t ti = spurs_taskset_taskinfo_ea(0, 3);   /* small EA for the buffer */
    vm_write64(ti + TI_ELF, 0x0177C200ull);
    vm_write64(ti + TI_CONTEXT, 0x03100000ull);
    CHECK(vm_read64(ti + TI_ELF) == 0x0177C200ull, "taskinfo elf round-trip");
    CHECK(vm_read64(ti + TI_CONTEXT) == 0x03100000ull, "taskinfo context round-trip");
    CHECK(g_mem[ti+TI_ELF+7] == 0x00 && g_mem[ti+TI_ELF+5] == 0x77, "elf stored big-endian");

    /* ---- B2 create path: spurs_taskset_init + add_task build the real layout ---- */
    memset(g_mem, 0, sizeof g_mem);
    uint32_t TS = 0x800;                              /* taskset base in the test buffer */
    spurs_taskset_init(TS, /*spurs*/0x01B92E00u, /*args*/0xDEAD0000ull, /*wid*/7,
                       /*size*/10496, /*evf1*/3, /*evf2*/0);
    CHECK(vm_read64(TS + CSTS_SPURS) == 0x01B92E00ull, "init: spurs ptr @0x60");
    CHECK(vm_read32(TS + CSTS_WID) == 7, "init: wid @0x74");
    CHECK(vm_read32(TS + CSTS_SIZE_FIELD) == 10496, "init: size @0x1890");
    CHECK(vm_read32(TS + CSTS_EVENT_FLAG_ID1) == 3, "init: event_flag_id1 @0x1898");
    CHECK(vm_read32(TS + CSTS_READY) == 0 && vm_read32(TS + CSTS_ENABLED) == 0,
          "init: bitsets cleared");

    const uint32_t arg[4]   = {0x00A662C8u, 0x019B5040u, 0x00A663A0u, 0x019B5040u};
    spurs_taskset_add_task(TS, /*taskId*/0, /*elf*/0x0177C200ull, /*ctx*/0x03100000ull,
                           arg, /*ls_pattern*/0);
    uint32_t ti0 = spurs_taskset_taskinfo_ea(TS, 0);
    CHECK(vm_read32(ti0 + TI_ARGS + 0) == 0x00A662C8u, "add_task: arg[0] @taskinfo+0");
    CHECK(vm_read64(ti0 + TI_ELF) == 0x0177C200ull, "add_task: elf @taskinfo+0x10");
    CHECK(vm_read64(ti0 + TI_CONTEXT) == 0x03100000ull, "add_task: context @+0x18");
    CHECK(spurs_bitset_test(TS + CSTS_ENABLED, 0), "add_task: enabled bit set");
    CHECK(spurs_bitset_test(TS + CSTS_READY, 0), "add_task: ready bit set");
    /* a second task at id 1 sets its own bit without disturbing id 0 */
    spurs_taskset_add_task(TS, 1, 0x0177CF00ull, 0, 0, 0);
    CHECK(spurs_bitset_test(TS + CSTS_READY, 0) && spurs_bitset_test(TS + CSTS_READY, 1),
          "add_task: id0 and id1 both ready");
    CHECK(vm_read64(spurs_taskset_taskinfo_ea(TS,1) + TI_ELF) == 0x0177CF00ull,
          "add_task: task1 elf at its own slot");

    if (!fails) printf("  PASS: SPURS taskset BE layout + create-path build (init/add_task) correct.\n");
    return fails ? 1 : 0;
}
