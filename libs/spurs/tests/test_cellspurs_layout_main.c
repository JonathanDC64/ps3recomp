/* Standalone unit test for the real CellSpurs SPU/event-substrate layout (cellspurs_layout.c).
 * Verifies cellspurs_write_substrate lands each field at the exact RPCS3 offset, big-endian.
 * Build: clang -std=c11 -O2 -I .. test_cellspurs_layout_main.c ../cellspurs_layout.c -o t && ./t
 */
#include "../cellspurs_layout.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Mock guest memory + BE accessors (host is little-endian; guest is big-endian). */
static uint8_t g_mem[0x4000];
uint32_t vm_read32(uint64_t ea) {
    const uint8_t* p = g_mem + (uint32_t)ea;
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
void vm_write8(uint64_t ea, uint8_t v) { g_mem[(uint32_t)ea] = v; }
void vm_write32(uint64_t ea, uint32_t v) {
    uint8_t* p = g_mem + (uint32_t)ea;
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}

static int fails = 0;
#define CHECK(c,m) do{ if(!(c)){ printf("  FAIL: %s\n", m); fails++; } }while(0)

int main(void) {
    memset(g_mem, 0, sizeof g_mem);
    const uint32_t SP = 0x100;                 /* CellSpurs base in mock mem */
    const uint32_t spus[2] = { 0x00000021u, 0x00000022u };
    cellspurs_write_substrate(SP, /*spuTG*/0x00000011u, spus, /*nSpus*/2,
                              /*spuPort*/1, /*eventQueue*/1, /*eventPort*/5);

    /* Fields land at the exact RPCS3 offsets, big-endian. */
    CHECK(vm_read32(SP + 0xD30) == 0x11u, "spuTG @0xD30");
    CHECK(vm_read32(SP + 0xD34) == 0x21u, "spus[0] @0xD34");
    CHECK(vm_read32(SP + 0xD38) == 0x22u, "spus[1] @0xD38");
    CHECK(g_mem[SP + 0xC9] == 1u,         "spuPort @0xC9 (u8)");
    CHECK(vm_read32(SP + 0xD5C) == 1u,    "eventQueue @0xD5C");
    CHECK(vm_read32(SP + 0xD60) == 5u,    "eventPort @0xD60");
    CHECK(g_mem[SP + 0xD64] == 0u,        "handlerDirty @0xD64 cleared");

    /* Big-endian byte order check on spuTG (0x11 -> 00 00 00 11). */
    CHECK(g_mem[SP+0xD30]==0x00 && g_mem[SP+0xD31]==0x00 &&
          g_mem[SP+0xD32]==0x00 && g_mem[SP+0xD33]==0x11, "spuTG stored big-endian");

    /* spus[2..7] left zero (we wrote only 2). */
    CHECK(vm_read32(SP + 0xD34 + 2*4) == 0u, "spus[2] untouched (0)");

    /* Offsets don't collide: spuPort (0xC9) is below spuTG (0xD30); no overlap. */
    CHECK(0xC9u + 1 <= 0xD30u, "spuPort precedes the thread-group block");

    if (!fails) printf("  PASS: CellSpurs substrate layout (offsets + big-endian) correct.\n");
    return fails ? 1 : 0;
}
