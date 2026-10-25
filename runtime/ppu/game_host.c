/*
 * ps3recomp - the host's half of a split build (ps3emu/game_module.h).
 *
 * Loads the game module, hands it the runtime functions the lifted code calls,
 * and moves the host's pointers (ps3emu/game_split.h) onto the globals and
 * trampolines the module owns. boot_main calls ps3_game_load once vm_base is
 * reserved and before anything reads the function table.
 *
 * The module is PS3_GAME_MODULE_NAME next to the exe, or the file named by
 * the PS3_GAME_MODULE environment variable. Its export is
 * PS3_GAME_MODULE_INIT. A title may supply ps3_game_title_fn, which the module
 * asks for the title's own functions (instruction hooks, overrides) by name,
 * and ps3_game_title_loaded, which resolves the lifted functions the title's
 * own code calls.
 *
 * This file declares the runtime functions as void(void) only to take their
 * addresses, so it must not include the headers that declare them properly.
 * The C library functions a freestanding module calls (PS3_GAME_HOST_CRT_FUNCS)
 * are this exe's own, declared by the C runtime's headers.
 *
 * Windows loads a DLL, Linux an ELF shared object with dlopen. What differs
 * is five small functions below (gh_exe_dir, gh_open, gh_symbol, gh_error,
 * gh_ms); ps3_game_load is the same on both.
 */
#if defined(PS3_GAME_SPLIT)
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#endif
#include "ps3emu/ps3_knob.h"
#include "ps3emu/game_module.h"
#include "ps3emu/game_split.h"   /* force-included as well; the guard makes that harmless */

#ifndef PS3_GAME_MODULE_NAME
#ifdef _WIN32
#define PS3_GAME_MODULE_NAME "game.dll"
#else
#define PS3_GAME_MODULE_NAME "game.so"
#endif
#endif
#ifndef PS3_GAME_MODULE_INIT
#define PS3_GAME_MODULE_INIT "game_init"
#endif

/* --- What differs per platform -------------------------------------------- */
#define PS3_GH_PATH_MAX 4096

#ifdef _WIN32
/* The exe's folder, with its separator at the end; 0 if unknown. */
static int gh_exe_dir(char* out, size_t size)
{
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    char* slash = strrchr(path, 92);
    if (slash) slash[1] = 0; else path[0] = 0;
    return snprintf(out, size, "%s", path) < (int)size;
}
/* The module by its full path; what it needs comes from its own folder, the exe's or System32, never from the
 * working folder or PATH. */
static void* gh_open(const char* path)
{
    char full[PS3_GH_PATH_MAX];
    const DWORD n = GetFullPathNameA(path, (DWORD)sizeof full, full, NULL);
    if (n == 0 || n >= sizeof full) {
        if (n) SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return NULL;
    }
    return (void*)LoadLibraryExA(full, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}
static void* gh_symbol(void* module, const char* name)
{
    return (void*)GetProcAddress((HMODULE)module, name);
}
/* Why the last gh_open failed. */
static void gh_error(char* out, size_t size)
{
    snprintf(out, size, "error %lu", (unsigned long)GetLastError());
}
static uint64_t gh_ms(void)
{
    return (uint64_t)GetTickCount64();
}
#else
static int gh_exe_dir(char* out, size_t size)
{
    char path[PS3_GH_PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return 0;
    path[n] = 0;
    char* slash = strrchr(path, 47);
    if (slash) slash[1] = 0; else path[0] = 0;
    return snprintf(out, size, "%s", path) < (int)size;
}
/* RTLD_LOCAL: the module's names stay out of the global scope (it exports
 * only its init anyway). RTLD_NOW: an unresolved name fails here, not at a
 * first call in the middle of play. */
static void* gh_open(const char* path)
{
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static void* gh_symbol(void* module, const char* name)
{
    return dlsym(module, name);
}
static void gh_error(char* out, size_t size)
{
    const char* e = dlerror();
    snprintf(out, size, "%s", e ? e : "unknown error");
}
static uint64_t gh_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
/* stdin, stdout and stderr for the module (PS3_GAME_HOST_IOB, game_module.h). */
static FILE* ps3_gh_iob(unsigned index)
{
    return index == 0 ? stdin : index == 1 ? stdout : stderr;
}
#endif

/* The runtime functions, by address. */
#define PS3_GH_DECL_(n) extern void n(void);
PS3_GAME_HOST_FUNCS(PS3_GH_DECL_)

/* The UCRT's fprintf is inline in <stdio.h>, so a freestanding module's
 * fprintf thunk comes here. A jump thunk leaves the variable arguments as the
 * caller passed them. */
static int ps3_gh_fprintf(FILE* stream, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    const int r = vfprintf(stream, format, ap);
    va_end(ap);
    return r;
}

/* The title's own functions by name; the title defines the strong one. */
void* ps3_game_title_fn(const char* name) __attribute__((weak));
void* ps3_game_title_fn(const char* name) { (void)name; return 0; }
/* Called once the module is loaded and the host's pointers moved, with the
 * lift's function table: a title resolves the lifted functions its own code
 * calls by name here. Nonzero refuses the module. */
int ps3_game_title_loaded(const void* func_table, uint64_t count) __attribute__((weak));
int ps3_game_title_loaded(const void* func_table, uint64_t count) { (void)func_table; (void)count; return 0; }

extern uint8_t* vm_base;   /* boot_main.cpp */

/* The host's view of the function table until the module loads: empty. */
static const uint64_t s_no_count = 0;
static const uint64_t s_no_table[3];
const void*     ps3_split_p_function_table = s_no_table;
const uint64_t* ps3_split_p_function_table_count = &s_no_count;

/* Trampoline slots: the module's, cached per thread; the host's before the load. */
PS3_SPLIT_TLS void** ps3_split_t_ppu_tramp;
PS3_SPLIT_TLS void** ps3_split_t_spu_tramp;
static PS3_SPLIT_TLS void* t_ppu_tramp_host;
static PS3_SPLIT_TLS void* t_spu_tramp_host;

static ps3_game_api s_game;
static void*        s_module;
void* ps3_split_module_spu_indirect_branch;

void** ps3_split_ppu_tramp_init(void)
{
    if (!s_game.ppu_tramp_slot) return &t_ppu_tramp_host;
    return ps3_split_t_ppu_tramp = s_game.ppu_tramp_slot();
}

void** ps3_split_spu_tramp_init(void)
{
    if (!s_game.spu_tramp_slot) return &t_spu_tramp_host;
    return ps3_split_t_spu_tramp = s_game.spu_tramp_slot();
}

extern const void* g_extra_symtab;   /* ppu_loader.cpp */
extern uint64_t    g_extra_symtab_count;

int ps3_game_load(void)
{
    char path[PS3_GH_PATH_MAX * 2];
    const char* env = getenv("PS3_GAME_MODULE");
    if (env && *env) {
        snprintf(path, sizeof path, "%s", env);
    } else {
        char dir[PS3_GH_PATH_MAX];
        if (!gh_exe_dir(dir, sizeof dir)) { fprintf(stderr, "[game-host] no exe path%c", 10); return 1; }
        snprintf(path, sizeof path, "%s%s", dir, PS3_GAME_MODULE_NAME);
    }
    const uint64_t t0 = gh_ms();
    s_module = gh_open(path);
    if (!s_module) {
        char why[512];
        gh_error(why, sizeof why);
        fprintf(stderr, "[game-host] cannot load %s (%s)%c", path, why, 10);
        return 1;
    }
    ps3_game_init_fn init = (ps3_game_init_fn)gh_symbol(s_module, PS3_GAME_MODULE_INIT);
    if (!init) {
        fprintf(stderr, "[game-host] %s has no %s%c", path, PS3_GAME_MODULE_INIT, 10);
        return 1;
    }

    static ps3_host_api host;
    host.abi      = PS3_GAME_ABI;
    host.size     = sizeof host;
    host.vm_base  = vm_base;
    host.env_gen  = ps3_env_gen;
#define PS3_GH_FILL_(n) host.fn[PS3_GH_##n] = (void*)&n;
    PS3_GAME_HOST_FUNCS(PS3_GH_FILL_)
    host.title_fn = ps3_game_title_fn;
    /* getenv is boot_main's cached one, the lookup a single exe's lifted code
     * makes: with /MD, &getenv reads the import slot boot_main defines; with
     * a static runtime, getenv is boot_main's own function. */
#define PS3_GH_CRT_FILL_(n, h) host.crt[PS3_GC_##n] = (void*)&h;
    PS3_GAME_HOST_CRT_FUNCS(PS3_GH_CRT_FILL_)

    ps3_game_api game;
    memset(&game, 0, sizeof game);
    const int rc = init(&host, &game);
    if (rc != 0 || game.abi != PS3_GAME_ABI || game.size != sizeof game) {
        fprintf(stderr, "[game-host] %s refused: init %d, ABI %u/%u (host %u/%u)%c", path, rc,
                game.abi, game.size, PS3_GAME_ABI, (unsigned)sizeof game, 10);
        return 1;
    }
    if (game.coh_bitmap_size != (1u << (32 - 7)) / 8) {
        fprintf(stderr, "[game-host] %s: coherency bitmap %u bytes, host %u%c", path,
                game.coh_bitmap_size, (1u << (32 - 7)) / 8, 10);
        return 1;
    }
    if (!game.host_fn_thunks || !game.host_fn_thunks[PS3_GH_spu_indirect_branch]) {
        fprintf(stderr, "[game-host] %s: no thunk addresses%c", path, 10);
        return 1;
    }

    /* The module's globals take the host's current values (single-threaded
     * here: no guest or SPU thread runs yet), then the host's pointers move. */
    *game.vm_inl_rd_lim   = g_vm_inl_rd_lim;
    *game.vm_inl_wr_lim   = g_vm_inl_wr_lim;
    *game.vm_inl_rd_hole  = g_vm_inl_rd_hole;
    *game.spu_goto_slow   = g_spu_goto_slow;
    *game.yz_lockstep_on  = g_yz_lockstep_on;
    *game.pm_flow_ctx     = g_pm_flow_ctx;
    *game.spu_ls_watch_n  = g_spu_ls_watch_n;
    *game.spu_ch_diag     = g_spu_ch_diag;
    *game.spu_ch_diag_gen = g_spu_ch_diag_gen;
    *game.spu_bi_switch   = g_spu_bi_switch;
    *game.spu_task_regs   = g_spu_task_regs;
    if (*ps3_split_coh_armed) {   /* never before the load (no SPU has run); the bitmap is 4 MB */
        memcpy(game.coh_bitmap, ps3_split_coh_bitmap, game.coh_bitmap_size);
        *game.coh_armed = *ps3_split_coh_armed;
    }
    *game.env_gen         = ps3_env_gen;

    ps3_split_p_g_vm_inl_rd_lim   = game.vm_inl_rd_lim;
    ps3_split_p_g_vm_inl_wr_lim   = game.vm_inl_wr_lim;
    ps3_split_p_g_vm_inl_rd_hole  = game.vm_inl_rd_hole;
    ps3_split_p_g_spu_goto_slow   = game.spu_goto_slow;
    ps3_split_p_g_yz_lockstep_on  = game.yz_lockstep_on;
    ps3_split_p_g_pm_flow_ctx     = game.pm_flow_ctx;
    ps3_split_p_g_spu_ls_watch_n  = game.spu_ls_watch_n;
    ps3_split_p_g_spu_ch_diag     = game.spu_ch_diag;
    ps3_split_p_g_spu_ch_diag_gen = game.spu_ch_diag_gen;
    ps3_split_p_g_spu_bi_switch   = game.spu_bi_switch;
    ps3_split_p_g_spu_task_regs   = game.spu_task_regs;
    ps3_split_coh_bitmap          = game.coh_bitmap;
    ps3_split_coh_armed           = game.coh_armed;
    ps3_split_game_env_gen        = game.env_gen;
    ps3_split_p_function_table       = game.func_table;
    ps3_split_p_function_table_count = game.func_table_count;
    ps3_split_module_spu_indirect_branch = game.host_fn_thunks[PS3_GH_spu_indirect_branch];
    s_game = game;
    /* This thread may have used the host slots: drop nothing (they are empty
     * before any lifted code runs), but stop using them. */
    ps3_split_t_ppu_tramp = 0;
    ps3_split_t_spu_tramp = 0;

    if (ps3_game_title_loaded(game.func_table, *game.func_table_count) != 0) {
        fprintf(stderr, "[game-host] %s: the title refused it%c", path, 10);
        return 1;
    }
    if (s_game.register_spu) s_game.register_spu();   /* was a static constructor */

    fprintf(stderr, "[game-host] loaded %s: %llu functions, %llu ms%c", path,
            (unsigned long long)*game.func_table_count,
            (unsigned long long)(gh_ms() - t0), 10);
    return 0;
}

void ps3_game_load_prx_modules(void)
{
    if (!s_game.load_prx_modules) return;
    s_game.load_prx_modules();
    g_extra_symtab       = *s_game.extra_symtab;
    g_extra_symtab_count = *s_game.extra_symtab_count;
}
#endif /* PS3_GAME_SPLIT */
