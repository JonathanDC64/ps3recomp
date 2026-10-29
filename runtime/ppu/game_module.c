/*
 * ps3recomp - the game module's half of a split build (ps3emu/game_module.h).
 *
 * Linked into the game DLL beside the lifted sources, which compile exactly
 * as they do for a single exe. This file defines everything they expect from
 * outside the lift:
 *
 *   - a jump thunk per runtime function (PS3_GAME_HOST_FUNCS), through a slot
 *     ps3_game_module_init fills from the host's table;
 *   - the globals their inline fast paths read, with the values the runtime
 *     gives them (the host copies its current values in at the load, then
 *     writes them through pointers);
 *   - the two trampoline thread-locals, and functions that return the calling
 *     thread's copy to the host;
 *   - vm_base, set once from the host (the guest space is reserved before the
 *     load and never moves);
 *   - g_extra_symtab, which a lifted system PRX's glue fills; the host copies
 *     it after running that glue;
 *   - in a freestanding module (PS3_GAME_FREESTANDING), a jump thunk per C
 *     library function (PS3_GAME_HOST_CRT_FUNCS) into the host's C runtime.
 *
 * Compiled with PS3_GAME_MODULE defined and without PS3_GAME_SPLIT.
 *
 * On Linux (an ELF shared object) everything here is hidden, as the forced
 * include ps3emu/game_freestanding.h makes every name in the module, so the
 * module's globals are read relative to the instruction pointer and nothing
 * binds to the host's names. The two trampolines are initial-exec
 * thread-locals (the module is compiled with -ftls-model=initial-exec): one
 * %fs-relative access, as cheap as the DLL's. glibc keeps some static TLS
 * spare for a library opened later; these two pointers take 16 bytes of it,
 * and if none were left the dlopen would fail with glibc's message.
 */
#ifndef PS3_GAME_MODULE
#define PS3_GAME_MODULE 1
#endif
#include <stdint.h>
#include <stdio.h>
#include "ps3emu/ps3_knob.h"     /* ps3_env_gen: this module's own copy */
#include "ps3emu/game_module.h"

#if !defined(_WIN64) && !(defined(__x86_64__) && defined(__ELF__))
#error "game_module.c: the split build is for Windows x64 and x86-64 Linux"
#endif

/* --- Runtime functions: one slot and one thunk each ----------------------- */
#define PS3_GM_SLOT_(n) void* ps3_gt_##n;
PS3_GAME_HOST_FUNCS(PS3_GM_SLOT_)
#define PS3_GM_THUNK_(n) PS3_GAME_THUNK(n)
PS3_GAME_HOST_FUNCS(PS3_GM_THUNK_)
#define PS3_GM_SLOT_ADDR_(n) &ps3_gt_##n,
static void** const s_host_slots[PS3_GH_COUNT] = { PS3_GAME_HOST_FUNCS(PS3_GM_SLOT_ADDR_) };
/* The thunks' addresses, for the host's function-pointer comparisons. Declared
 * void(void) only to take their addresses (this file includes no header that
 * declares them properly). */
#define PS3_GM_THUNK_DECL_(n) extern void n(void);
PS3_GAME_HOST_FUNCS(PS3_GM_THUNK_DECL_)
#define PS3_GM_THUNK_ADDR_(n) (void*)&n,
static void* const s_host_thunks[PS3_GH_COUNT] = { PS3_GAME_HOST_FUNCS(PS3_GM_THUNK_ADDR_) };

/* --- C library functions (freestanding module): a slot and a thunk each --- */
#ifdef PS3_GAME_FREESTANDING
#define PS3_GM_CRT_SLOT_(n, h) void* ps3_gt_##n;
PS3_GAME_HOST_CRT_FUNCS(PS3_GM_CRT_SLOT_)
#define PS3_GM_CRT_THUNK_(n, h) PS3_GAME_THUNK(n)
PS3_GAME_HOST_CRT_FUNCS(PS3_GM_CRT_THUNK_)
#define PS3_GM_CRT_SLOT_ADDR_(n, h) &ps3_gt_##n,
static void** const s_crt_slots[PS3_GC_COUNT] = { PS3_GAME_HOST_CRT_FUNCS(PS3_GM_CRT_SLOT_ADDR_) };
int ps3_game_libc_run_ctors(void);   /* game_libc.c: the C++ dynamic initialisers, if any */

/* Whether the loader has set up this module's thread-locals. Windows: the loader writes the module's TLS slot into
 * _tls_index (game_libc.c's TLS directory), and leaves it unset for a DLL with no import table. Linux: the two
 * initial-exec thread-locals are part of the static TLS block, without which the dlopen itself fails. */
#ifdef _WIN64
extern unsigned int _tls_index;
static int tls_ready(void) { return _tls_index != PS3_GAME_TLS_INDEX_UNSET; }
#else
static int tls_ready(void) { return 1; }
#endif
#endif

/* --- Globals the lifted code reads -------------------------------------- */
/* Initial values as the runtime defines them (spu_drain.c, spu_channels.c,
 * spu_lockstep.c, ps3_knob.c, spu_coherency.c). */
#define PS3_GM_COH_BITMAP_SZ ((1u << (32 - 7)) / 8)   /* spu_coherency.c: one bit per 128-byte line */
uint8_t*          vm_base;
unsigned          g_vm_inl_rd_lim, g_vm_inl_wr_lim, g_vm_inl_rd_hole;
unsigned char     g_spu_coh_bitmap[PS3_GM_COH_BITMAP_SZ];
/* v: the armed flag; stk_lim, stk_run, st_run: the store runs' limits (ps3emu/ppu_lift.h), the host's to set */
__attribute__((aligned(64))) struct { int v; unsigned stk_lim, stk_run, st_run; char pad_[64 - 4 * sizeof(int)]; } g_spu_coh_armed_line;
volatile int      g_spu_goto_slow = 1;
volatile int      g_yz_lockstep_on = -1;
void* volatile    g_pm_flow_ctx = 0;
int               g_spu_ls_watch_n = -1;
int               g_spu_ch_diag;
unsigned          g_spu_ch_diag_gen = ~0u;
volatile int      g_spu_bi_switch = 0;
int               g_spu_task_regs = 0;
const void*       g_extra_symtab;
uint64_t          g_extra_symtab_count;

/* --- The trampolines ------------------------------------------------------ */
/* (__thread and __attribute__ above: clang's spellings, which clang-cl takes as well) */
__thread void (*g_trampoline_fn)(void*);
__thread void (*g_spu_trampoline_fn)(void*);   /* (spu_context*) in the runtime */
static void** ppu_tramp_slot(void) { return (void**)&g_trampoline_fn; }
static void** spu_tramp_slot(void) { return (void**)&g_spu_trampoline_fn; }

/* --- The lift's function table (ppu_recomp.h declares it) ------------------ */
extern const struct ps3_gm_func_entry { uint64_t addr; void* func; const char* name; } function_table[];
extern const uint64_t function_table_count;

int ps3_game_module_init(const ps3_host_api* host, ps3_game_api* game)
{
    if (!host || !game) return 1;
    if (host->abi != PS3_GAME_ABI || host->size != sizeof(ps3_host_api)) {
#ifndef PS3_GAME_FREESTANDING   /* (a freestanding module cannot print yet; the host reports it) */
        fprintf(stderr, "[game] host ABI %u/%u, module %u/%u: refusing to run%c",
                host->abi, host->size, PS3_GAME_ABI, (unsigned)sizeof(ps3_host_api), 10);
#endif
        return 2;
    }
#ifdef PS3_GAME_FREESTANDING
    /* The C library first: everything below may print. A missing one cannot
     * be printed, so the host reports the code. */
    for (int i = 0; i < PS3_GC_COUNT; i++) {
        if (!host->crt[i]) return 3;
        *s_crt_slots[i] = host->crt[i];
    }
    if (!tls_ready()) {
        fprintf(stderr, "[game] the loader did not set up this module's thread-locals "
                        "(it needs a TLS directory and an import table): refusing to run%c", 10);
        return 5;
    }
    ps3_game_libc_run_ctors();
#endif
    for (int i = 0; i < PS3_GH_COUNT; i++) {
        if (!host->fn[i]) {
            fprintf(stderr, "[game] host function %d missing%c", i, 10);
            return 3;
        }
        *s_host_slots[i] = host->fn[i];
    }
    vm_base     = host->vm_base;
    ps3_env_gen = host->env_gen;

    game->abi                  = PS3_GAME_ABI;
    game->size                 = sizeof(ps3_game_api);
    game->func_table           = function_table;
    game->func_table_count     = &function_table_count;
    game->ppu_tramp_slot       = ppu_tramp_slot;
    game->spu_tramp_slot       = spu_tramp_slot;
    game->host_fn_thunks       = s_host_thunks;
    game->vm_inl_rd_lim       = &g_vm_inl_rd_lim;
    game->vm_inl_wr_lim        = &g_vm_inl_wr_lim;
    game->vm_inl_rd_hole       = &g_vm_inl_rd_hole;
    game->coh_bitmap           = g_spu_coh_bitmap;
    game->coh_bitmap_size      = (uint32_t)sizeof g_spu_coh_bitmap;
    game->coh_armed            = &g_spu_coh_armed_line.v;
    game->spu_goto_slow        = &g_spu_goto_slow;
    game->yz_lockstep_on       = &g_yz_lockstep_on;
    game->pm_flow_ctx          = &g_pm_flow_ctx;
    game->spu_ls_watch_n       = &g_spu_ls_watch_n;
    game->spu_ch_diag          = &g_spu_ch_diag;
    game->spu_ch_diag_gen      = &g_spu_ch_diag_gen;
    game->spu_bi_switch        = &g_spu_bi_switch;
    game->spu_task_regs        = &g_spu_task_regs;
    game->env_gen              = &ps3_env_gen;
    game->extra_symtab         = &g_extra_symtab;
    game->extra_symtab_count   = &g_extra_symtab_count;
    game->register_spu         = 0;   /* the title's init sets these */
    game->load_prx_modules     = 0;
    return 0;
}
