/*
 * ps3recomp - the game module split: what a host exe and a game DLL hand
 * each other.
 *
 * In a split build the lifted code (PPU chunks, SPU images, a lifted system
 * PRX and their glue) is a DLL, and the runtime, the port code and the UI are
 * the exe. The lifted sources compile exactly as for a single exe. The DLL's
 * glue (runtime/ppu/game_module.c) defines what they expect from outside:
 *
 *   - a jump thunk for each runtime function they call (PS3_GAME_HOST_FUNCS),
 *     through a slot the host fills at load: the cost of an import-table call;
 *   - the globals their inline fast paths read (the guest-memory limits, the
 *     SPU coherency bitmap, the SPU dispatch flags). They live in the DLL and
 *     the host writes them through pointers (ps3emu/game_split.h);
 *   - the two trampoline thread-locals. Windows cannot share a thread-local
 *     across modules, so they live in the DLL too, and the host reaches the
 *     current thread's copy through a pointer it caches per thread.
 *
 * The host (runtime/ppu/game_host.c) loads the DLL, passes a ps3_host_api to
 * its one export and gets a ps3_game_api back. Both sides check abi and size.
 *
 * A freestanding module (PS3_GAME_FREESTANDING, ps3emu/game_freestanding.h)
 * has no C runtime of its own. It reaches the host's for the few C library
 * functions its sources call (PS3_GAME_HOST_CRT_FUNCS), through jump thunks
 * like the runtime's; runtime/ppu/game_libc.c defines the rest (memcpy and
 * friends, _fltused, the TLS directory). The host always passes the list; a
 * module with a C runtime of its own ignores it.
 *
 * The same split builds on x86-64 Linux: the module is an ELF shared object
 * (des_game.so) that the host opens with dlopen. The ABI, the two lists and
 * the structs are the same; what differs is in the thunk macro below, the
 * host's loader (game_host.c) and the module's own pieces (game_module.c,
 * game_libc.c, game_freestanding.h). There the module is always freestanding:
 * built with clang's own headers only and no C library, so the player's PC
 * needs no libc headers to build it.
 */
#ifndef PS3EMU_GAME_MODULE_H
#define PS3EMU_GAME_MODULE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3_GAME_ABI 2

/* The runtime functions the lifted code calls by name, in slot order. A new
 * one goes at the end, with PS3_GAME_ABI raised. */
#define PS3_GAME_HOST_FUNCS(X)                                                   \
    X(vm_read8) X(vm_read16) X(vm_read32) X(vm_read64)                           \
    X(vm_write8) X(vm_write16) X(vm_write32) X(vm_write64)                       \
    X(ppu_stwcx32) X(ppu_stdcx64) X(lv2_syscall) X(ppu_timebase_now)             \
    X(ps3_indirect_call) X(ps3_hle_call) X(ppu_register_function)                \
    X(prx_load_module) X(prx_image_load_file)                                    \
    X(spu_rdch) X(spu_wrch) X(spu_rchcnt) X(spu_stop) X(spu_halt)                \
    X(spu_indirect_branch) X(spu_drain_call) X(spu_begin_image)                  \
    X(spu_register_function) X(spu_workload_register_img) X(spu_ls_watch_slow)   \
    X(spu_hook_ring_fetch) X(spu_hook_put_wait) X(spu_hook_inflate_ls)           \
    X(spu_hook_ring_post) X(spu_hook_state_diff)                                 \
    X(spu_registering_image) X(spu_bi_switch_verify)

#define PS3_GAME_HOST_FN_INDEX_(n) PS3_GH_##n,
enum { PS3_GAME_HOST_FUNCS(PS3_GAME_HOST_FN_INDEX_) PS3_GH_COUNT };

/* The C library functions a freestanding module calls by name, in slot order:
 * X(the name in the module, the host's function). The host passes its own C
 * runtime's, so a FILE* or a getenv result is the host's, and the math gives
 * the same bits as in a single exe (sqrt and sqrtf are only called when the
 * inline instruction's result is a NaN: the errno path). The UCRT's fprintf
 * is inline in its header, so the host passes a function of its own
 * (game_host.c). fputc, fputs and fwrite are what the compiler rewrites some
 * fprintf calls into. A new one goes at the end, with PS3_GAME_ABI raised.
 *
 * The module's stdin, stdout and stderr are __acrt_iob_func(0, 1, 2), as in
 * the UCRT (ps3emu/game_libc/stdio.h). glibc's are variables, which a module
 * with no C library cannot import, so on Linux the host passes
 * ps3_gh_iob (game_host.c), which returns them. */
#ifdef _WIN32
#define PS3_GAME_HOST_IOB __acrt_iob_func
#else
#define PS3_GAME_HOST_IOB ps3_gh_iob
#endif
#define PS3_GAME_HOST_CRT_FUNCS(X)                                               \
    X(fma, fma) X(sqrt, sqrt) X(sqrtf, sqrtf) X(ldexp, ldexp) X(exp2, exp2)      \
    X(getenv, getenv) X(atoi, atoi) X(strtoul, strtoul)                          \
    X(__acrt_iob_func, PS3_GAME_HOST_IOB) X(fprintf, ps3_gh_fprintf)             \
    X(fputc, fputc) X(fputs, fputs) X(fwrite, fwrite) X(fflush, fflush)

#define PS3_GAME_HOST_CRT_INDEX_(n, h) PS3_GC_##n,
enum { PS3_GAME_HOST_CRT_FUNCS(PS3_GAME_HOST_CRT_INDEX_) PS3_GC_COUNT };

/* A freestanding module's _tls_index before the loader writes it
 * (runtime/ppu/game_libc.c). */
#define PS3_GAME_TLS_INDEX_UNSET 0xFFFFFFFFu

typedef struct ps3_host_api {
    uint32_t abi;                  /* PS3_GAME_ABI */
    uint32_t size;                 /* sizeof(ps3_host_api) */
    uint8_t* vm_base;              /* the guest address space, reserved before the load */
    unsigned env_gen;              /* ps3_env_gen at the load */
    void*    fn[PS3_GH_COUNT];     /* PS3_GAME_HOST_FUNCS, in order */
    /* The title's own functions the lifted code calls (instruction hooks,
     * function overrides), by name; 0 when the host has no such function. */
    void*    (*title_fn)(const char* name);
    void*    crt[PS3_GC_COUNT];    /* PS3_GAME_HOST_CRT_FUNCS, in order */
} ps3_host_api;

typedef struct ps3_game_api {
    uint32_t abi;                  /* PS3_GAME_ABI */
    uint32_t size;                 /* sizeof(ps3_game_api) */
    /* The lift's function table: func_table_count entries of { uint64_t addr;
     * void (*func)(ppu_context*); const char* name; }. (Not named
     * function_table: game_split.h turns that name into the host's pointer.) */
    const void*     func_table;
    const uint64_t* func_table_count;
    /* The calling thread's g_trampoline_fn and g_spu_trampoline_fn. */
    void**   (*ppu_tramp_slot)(void);
    void**   (*spu_tramp_slot)(void);
    /* The module's address for each runtime function, in PS3_GAME_HOST_FUNCS
     * order: its jump thunk, which is what the lifted code stores when it
     * takes one's address (SPU_RET stores spu_indirect_branch). Where the
     * runtime compares a function pointer with one of its own functions, it
     * accepts this address too. */
    void* const* host_fn_thunks;
    /* Globals the lifted code reads on its fast paths. */
    unsigned*          vm_inl_rd_lim;
    unsigned*          vm_inl_wr_lim;
    unsigned*          vm_inl_rd_hole;
    unsigned char*     coh_bitmap;
    uint32_t           coh_bitmap_size;
    int*               coh_armed;
    volatile int*      spu_goto_slow;
    volatile int*      yz_lockstep_on;
    void* volatile*    pm_flow_ctx;
    int*               spu_ls_watch_n;
    int*               spu_ch_diag;
    unsigned*          spu_ch_diag_gen;
    volatile int*      spu_bi_switch;
    int*               spu_task_regs;
    volatile unsigned* env_gen;    /* the module's ps3_env_gen; the host's bump writes it too */
    /* What the lifted system PRX's glue publishes for the symbolizer. */
    const void**       extra_symtab;
    uint64_t*          extra_symtab_count;
    /* Startup work a single exe did in static constructors and hooks. */
    void (*register_spu)(void);        /* the title's SPU workloads; 0 if none */
    void (*load_prx_modules)(void);    /* the lifted system PRX; 0 if none */
} ps3_game_api;

/* The module's one export (the title names it), and the runtime's half of it,
 * which the title's export calls first. Both return 0 on success. */
typedef int (*ps3_game_init_fn)(const ps3_host_api* host, ps3_game_api* game);
int ps3_game_module_init(const ps3_host_api* host, ps3_game_api* game);

/* Module side: a function the lifted code calls by name, defined as a jump
 * through the slot ps3_gt_<name>, which init fills. Module-level assembly,
 * so the thunk does not need the function's prototype. AT&T syntax, said
 * explicitly: clang-cl parses module-level assembly as Intel syntax.
 *
 * On ELF the thunk is hidden, as everything in the module is but its export:
 * the host defines the same names (the runtime's vm_read32 and the rest), and
 * a default-visibility one in the module could be bound to the host's
 * instead. Hidden, the slot is read relative to the instruction pointer, as
 * in the DLL. */
#if defined(PS3_GAME_MODULE) && defined(_WIN64)
#define PS3_GAME_THUNK(name)                                                     \
    __asm__(".att_syntax prefix\n\t.text\n\t.globl " #name "\n"                    \
            "\t.def " #name "; .scl 2; .type 32; .endef\n"                          \
            "\t.p2align 4\n" #name ":\n\tjmpq *ps3_gt_" #name "(%rip)\n");
#elif defined(PS3_GAME_MODULE) && defined(__x86_64__) && defined(__ELF__)
#define PS3_GAME_THUNK(name)                                                     \
    __asm__(".text\n\t.globl " #name "\n\t.hidden " #name "\n"                     \
            "\t.type " #name ", @function\n"                                        \
            "\t.p2align 4\n" #name ":\n\tjmpq *ps3_gt_" #name "(%rip)\n"             \
            "\t.size " #name ", . - " #name "\n");
#endif

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_GAME_MODULE_H */
