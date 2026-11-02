/*
 * precompile_compare_today.c -- for precompile_compare built without the
 * portable renderer: today's pass (rsx_shader_precompile.c as that build
 * compiles it) inside the test, its two pipeline index calls renamed to the
 * test's recorder, which hands each on to the renderer. Built with
 * PS3RECOMP_RSX_HAL this file is empty: the test puts its recorder in the
 * pass's sink instead.
 */
#if !defined(PS3RECOMP_RSX_HAL)
#define rsx_d3d12_pso_index_add   pcc_index_add
#define rsx_d3d12_pso_index_ready pcc_index_ready
#include "../rsx_shader_precompile.c"
#else
typedef int precompile_compare_today_off;   /* (a file with no declaration is not ISO C) */
#endif
