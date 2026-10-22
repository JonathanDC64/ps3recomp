/*
 * ps3recomp - the host side of the game module split (ps3emu/game_module.h).
 *
 * Force-included (/FI) into every host source of a split build: the runtime
 * library built with PS3RECOMP_GAME_SPLIT=ON, and the exe's own sources. It
 * does nothing unless PS3_GAME_SPLIT is defined, so the single-exe build and
 * the game module's sources never see it.
 *
 * The game module owns the globals the lifted code reads on its fast paths
 * and the two trampoline thread-locals. Here those names become accesses
 * through pointers. Until the module loads they point at host copies, defined
 * where the single-exe build defines the globals; game_host.c moves them onto
 * the module's at the load. A declaration of one of these names elsewhere
 * turns into a declaration of its pointer and keeps compiling
 * (extern int g_spu_ch_diag; -> extern int (*ps3_split_p_g_spu_ch_diag);).
 * A definition must not: each one is under #ifndef PS3_GAME_SPLIT, with
 * PS3_SPLIT_DEFINE in the #else.
 */
#ifndef PS3EMU_GAME_SPLIT_H
#define PS3EMU_GAME_SPLIT_H
#ifdef PS3_GAME_SPLIT

#ifdef __cplusplus
extern "C" {
#endif

/* The guest-memory fast-path limits (ps3_knob.c, ppu_loader.cpp) and the SPU
 * dispatch flags (spu_drain.c, spu_channels.c, spu_lockstep.c). */
extern unsigned*       ps3_split_p_g_vm_inl_rd_lim;
extern unsigned*       ps3_split_p_g_vm_inl_wr_lim;
extern unsigned*       ps3_split_p_g_vm_inl_rd_hole;
extern volatile int*   ps3_split_p_g_spu_goto_slow;
extern volatile int*   ps3_split_p_g_yz_lockstep_on;
extern void* volatile* ps3_split_p_g_pm_flow_ctx;
extern int*            ps3_split_p_g_spu_ls_watch_n;
extern int*            ps3_split_p_g_spu_ch_diag;
extern unsigned*       ps3_split_p_g_spu_ch_diag_gen;
extern volatile int*   ps3_split_p_g_spu_bi_switch;
extern int*            ps3_split_p_g_spu_task_regs;
#define g_vm_inl_rd_lim    (*ps3_split_p_g_vm_inl_rd_lim)
#define g_vm_inl_wr_lim    (*ps3_split_p_g_vm_inl_wr_lim)
#define g_vm_inl_rd_hole   (*ps3_split_p_g_vm_inl_rd_hole)
#define g_spu_goto_slow    (*ps3_split_p_g_spu_goto_slow)
#define g_yz_lockstep_on   (*ps3_split_p_g_yz_lockstep_on)
#define g_pm_flow_ctx      (*ps3_split_p_g_pm_flow_ctx)
#define g_spu_ls_watch_n   (*ps3_split_p_g_spu_ls_watch_n)
#define g_spu_ch_diag      (*ps3_split_p_g_spu_ch_diag)
#define g_spu_ch_diag_gen  (*ps3_split_p_g_spu_ch_diag_gen)
#define g_spu_bi_switch    (*ps3_split_p_g_spu_bi_switch)
#define g_spu_task_regs    (*ps3_split_p_g_spu_task_regs)

/* The SPU coherency bitmap and its armed flag. Their two users
 * (spu_coherency.c, ppu_loader.cpp) name these directly. */
extern unsigned char* ps3_split_coh_bitmap;
extern int*           ps3_split_coh_armed;

/* The module's own ps3_env_gen: ps3_env_bump sets it with the host's, so the
 * lifted code's knob checks and its channel latch see a console setenv. 0
 * until the module loads. */
extern volatile unsigned* ps3_split_game_env_gen;

/* The lift's function table, in the module. Not declared here:
 * ps3emu/ppu_lift.h (the fixed part of the lift's ppu_recomp.h) and
 * lv2_register.c declare it with their own entry types, and those
 * declarations become declarations of these pointers (game_host.c). */
#define function_table        (*ps3_split_p_function_table)
#define function_table_count  (*ps3_split_p_function_table_count)

/* The trampolines: the calling thread's slot in the module, cached per thread
 * on first use after the load. Before the load (no lifted code has run) a
 * host slot stands in, and nothing is cached. */
#if defined(_MSC_VER)
#define PS3_SPLIT_TLS __declspec(thread)
#else
#define PS3_SPLIT_TLS __thread
#endif
extern PS3_SPLIT_TLS void** ps3_split_t_ppu_tramp;
extern PS3_SPLIT_TLS void** ps3_split_t_spu_tramp;
void** ps3_split_ppu_tramp_init(void);
void** ps3_split_spu_tramp_init(void);
static inline void** ps3_split_ppu_tramp(void)
{
    void** p = ps3_split_t_ppu_tramp;
    return p ? p : ps3_split_ppu_tramp_init();
}
static inline void** ps3_split_spu_tramp(void)
{
    void** p = ps3_split_t_spu_tramp;
    return p ? p : ps3_split_spu_tramp_init();
}
struct spu_context;
#define g_trampoline_fn      (*(void (**)(void*))ps3_split_ppu_tramp())
#define g_spu_trampoline_fn  (*(void (**)(struct spu_context*))ps3_split_spu_tramp())

/* The module's address for spu_indirect_branch: the jump thunk its lifted SPU
 * code stores in g_spu_trampoline_fn (SPU_RET), which is not this
 * spu_indirect_branch's address. spu_drain_call accepts either. 0 until the
 * module loads. */
extern void* ps3_split_module_spu_indirect_branch;

/* The host copy of a module-owned global, and the pointer to it. The name is
 * only used next to ##, so the macros above do not expand it. */
#define PS3_SPLIT_DEFINE(type, name, init)                                     \
    static type ps3_split_host_##name = init;                                  \
    type* ps3_split_p_##name = &ps3_split_host_##name

/* runtime/ppu/game_host.c: load the module (0 = loaded), then run its system
 * PRX glue in place of ps3_load_prx_modules. */
int  ps3_game_load(void);
void ps3_game_load_prx_modules(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3_GAME_SPLIT */
#endif /* PS3EMU_GAME_SPLIT_H */
