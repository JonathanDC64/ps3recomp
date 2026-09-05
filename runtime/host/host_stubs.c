/*
 * Symbols the HLE library expects from the PPU loader sources.
 *
 * runtime/ppu/ and runtime/host/ are both excluded from the library target
 * (CMakeLists.txt), so a host that runs no lifted game still has to satisfy the
 * handful of out-of-line references the HLE modules make into the PPU loader.
 * These are inert but correct: a game-less host has no reservation state to
 * track, and the guest stores still have to be big-endian.
 */
#include <stdint.h>
#include <string.h>

extern uint8_t* vm_base;

/* ppu_loader.cpp - PPU reservation bookkeeping. The ppu_memory.h inlines call
 * these on every store so lwarx/stwcx reservations can be broken; with no PPU
 * threads there is never a live reservation. */
int  g_resv_store_active = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }

/* ppu_loader.cpp - out-of-line big-endian guest stores. */
void vm_write8(uint64_t addr, uint8_t v)
{
    vm_base[(uint32_t)addr] = v;
}

void vm_write32(uint64_t addr, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(vm_base + (uint32_t)addr, &v, 4);
}

/* ppu_fs.cpp - VFS root consulted by cellGame/cellFs path mapping. */
const char* ppu_vfs_root = ".";

/* Inline write-watch hooks. ppu_memory.h expands every vm_write* into a range
 * test against g_ww_lo/g_ww_hi plus a call to ps3_ww_report_inline, and those
 * live in runtime/ppu/ppu_loader.cpp -- which CMake excludes from the runtime
 * library because it is compiled per-game. A game link therefore supplies them
 * and this host does not, so it failed to link with three undefined symbols.
 *
 * lo == hi leaves the watch permanently empty, so the test is a compare that
 * never fires and the reporter is never reached. */
unsigned int g_ww_lo = 0, g_ww_hi = 0;

void ps3_ww_report_inline(unsigned int addr, unsigned long long val, int width)
{
    (void)addr; (void)val; (void)width;
}
