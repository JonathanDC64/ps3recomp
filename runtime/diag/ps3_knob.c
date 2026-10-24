/* Live diagnostic knobs: see include/ps3emu/ps3_knob.h. */
#include <stdlib.h>
#include "ps3emu/ps3_knob.h"

/* Limits of the lifted PPU code's inline guest-memory fast paths
 * (ppu_recomp.h, kept by ppu_loader.cpp): an access at ea takes the inline
 * path when ea - 0x10000 < lim, and 0 sends every access through the call.
 * Here, not in ppu_loader.cpp, so a console setenv can drop them: a knob it
 * sets may be one the accessors must see, and the calls set them again. */
#ifndef PS3_GAME_SPLIT
unsigned g_vm_inl_rd_lim, g_vm_inl_wr_lim, g_vm_inl_rd_hole;
#else
/* Split build: the game module's once it loads (ps3emu/game_split.h). */
PS3_SPLIT_DEFINE(unsigned, g_vm_inl_rd_lim, 0);
PS3_SPLIT_DEFINE(unsigned, g_vm_inl_wr_lim, 0);
PS3_SPLIT_DEFINE(unsigned, g_vm_inl_rd_hole, 0);
volatile unsigned* ps3_split_game_env_gen;
#endif

void spu_coh_stk_eval(int allow);   /* runtime/spu/spu_coherency.c */
void ps3_env_bump(void)
{
    g_vm_inl_rd_lim = 0; g_vm_inl_wr_lim = 0; spu_coh_stk_eval(0);
    ps3_env_gen++;   /* only the debug console writes it */
#ifdef PS3_GAME_SPLIT
    if (ps3_split_game_env_gen) *ps3_split_game_env_gen = ps3_env_gen;   /* the module's copy */
#endif
}

