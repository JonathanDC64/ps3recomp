/*
 * ps3recomp - integrated PPU boot harness.
 *
 * Links the whole PPU runtime half into one executable and starts executing a
 * recompiled program's entry point:
 *
 *   lifted code (ppu_recomp.c) + loader (ppu_loader.cpp) + HLE bridge
 *   (ppu_hle.cpp + generated NID table) + HLE libs (cellGcmSys, rsx_commands)
 *
 * It loads the PPU ELF named on the command line, registers the lifted
 * functions and the HLE NID handlers, starts the frame clock, then dispatches
 * the entry OPD. Execution runs real guest code until it reaches a function
 * outside the lifted subset (logged by the unlifted stub), an unimplemented
 * firmware import (logged by ps3_hle_call), or an lv2 syscall (logged by
 * lv2_syscall) -- telling us exactly what to implement next.
 *
 * Nothing here is title-specific: the ELF path, the VFS root derived from it
 * and the window title all come from the command line or the environment. Two
 * things drive it today -- a game port, which supplies its own lifted
 * ppu_recomp.c and its own HLE overrides, and the smoke title in
 * tests/smoke/, which is the same harness with a synthetic program in it and
 * is what proves this path runs at all on POSIX.
 */
#include <stdarg.h>
#include "ppu_recomp.h"
#include "ps3emu/milestone.h"   /* ps3_ms / ps3_ms_kv -- boot milestone log */
/* PPU_THREAD_LOCAL only -- NOT ppu_context.h, which would redefine the struct
 * a generated ppu_recomp.h already declares. Ports generated before the
 * qualifier existed do not define it, so the scaffold must carry its own
 * reachable definition. */
#include "../ppu_tls.h"
#include "../../platform/win32_compat.h"      /* Sleep, GetTickCount64, threads on POSIX */
#include "../../platform/win32_backtrace.h"   /* RtlCaptureStackBackTrace / GetModuleHandleA on POSIX */
/* The lifted ppu_recomp.h already defines `struct ppu_context` (identical to
 * runtime/ppu/ppu_context.h by design -- "keep the two in sync"). A syscall
 * header included later (sys_ppu_thread.h -> lv2_syscall_table.h -> ppu_context.h)
 * would redefine it, so claim ppu_context.h's guard now: this TU has the struct,
 * and boot_main uses none of that header's PPU_CR or XER bit macros. */
#ifndef PPU_CONTEXT_H
#define PPU_CONTEXT_H
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
uint32_t ppu_load_elf(const char* path);
void     ppu_recomp_register(void);
void     ppu_hle_init(void);
uint32_t ps3_hle_count(void);   /* handlers actually registered, for the milestone log */
void     ppu_sysprx_register(void);
void     ppu_fs_register(void);
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
extern const char* ppu_vfs_root;   /* host dir that PS3 mount points map into */
/* The status a guest handed to sys_process_exit, published by
 * libs/system/sysPrxForUser.c before it calls the host exit(). A title that
 * exits that way never returns through ppu_run, so this is only read on the
 * path where the guest fell out of its entry function instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;
void     cellGame_init_from_paramsfo(const char* sfo_path);  /* libs/system/cellGame.c */
/* Optional hook: load real system PRX modules (libsre = cellSpurs/cellSync) into
 * guest RAM and register their exports. Weak default is a no-op; a title that
 * links a lifted PRX defines a strong version. Called after the lifted function
 * table is registered and vm_base is live, before the game runs. */
void     ps3_load_prx_modules(void) __attribute__((weak));
void     ps3_load_prx_modules(void) {}
/* Optional hook: a title's own host-side options (user-facing knobs that act on
 * the game's state, not workarounds). Weak default is a no-op. Called once the
 * lifted function table is registered, so it may re-register addresses. */
void     ps3_title_options_init(void) __attribute__((weak));
void     ps3_title_options_init(void) {}
}

#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include "ps3emu/ps3_knob.h"

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>   /* _ReturnAddress */
/* timeBeginPeriod: windows.h is included with WIN32_LEAN_AND_MEAN, which
 * excludes the multimedia timer API, so it must be asked for by name -- and
 * AFTER windows.h, since timeapi.h uses UINT and friends. */
#include <timeapi.h>

/* Process-wide getenv cache. The runtime has ~700 `if (getenv("X"))` diagnostic
 * gates, many on per-draw / per-syscall paths, and the UCRT getenv takes the
 * environment lock and scans every variable each time: profiled at ~30% of
 * GH3's main PPU thread and ~35% of the RSX thread in gameplay. Defining the
 * import slot here (an object in the link beats the import library's lazy
 * definition) routes every dllimport'ed getenv call through this cache.
 * Only the debug console changes the environment after startup, through
 * cached_setenv below; otherwise a value is looked up once and the pointer
 * stays valid forever. Reads are lock-free:
 * a slot's key is published last, after its value.
 * ponytail: 1024 open-addressed slots, fixed; names past that fall back to the OS lookup. */
namespace {
struct EnvSlot { const char* volatile key; const char* val; };
EnvSlot  s_env[1024];
SRWLOCK  s_env_lock = SRWLOCK_INIT;

unsigned env_hash(const char* s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s)) * 16777619u;
    return h;
}

const char* env_lookup_os(const char* name)
{
    DWORD n = GetEnvironmentVariableA(name, NULL, 0);
    if (!n) return NULL;
    char* v = (char*)malloc(n);
    if (!v || GetEnvironmentVariableA(name, v, n) >= n) { free(v); return NULL; }
    return v;
}

char* __cdecl cached_getenv(const char* name)
{
    if (!name || !*name) return NULL;
    const unsigned mask = 1023, h = env_hash(name);
    for (unsigned i = h & mask, n = 0; n <= mask; n++, i = (i + 1) & mask) {
        const char* k = s_env[i].key;
        MemoryBarrier();
        if (!k) break;
        if (!_stricmp(k, name)) {
            /* GETENV_STATS=1: which gate is being polled on a hot path. */
            static int st = -1;
            if (st < 0) st = env_lookup_os("GETENV_STATS") ? 1 : 0;
            if (st) {
                static unsigned cnt[1024]; static void* ra[1024]; static unsigned long long tot;
                cnt[i]++; ra[i] = _ReturnAddress();
                if ((++tot & ((1u << 22) - 1)) == 0) {
                    const uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL);
                    for (int r = 0; r < 5; r++) {
                        unsigned b = 0;
                        for (unsigned j = 1; j <= mask; j++) if (cnt[j] > cnt[b]) b = j;
                        if (!cnt[b]) break;
                        fprintf(stderr, "[getenv-stats] %-28s %u calls  caller rva=0x%llX%c",
                                s_env[b].key, cnt[b], (unsigned long long)((uintptr_t)ra[b] - exe), 10);
                        cnt[b] = 0;
                    }
                    memset(cnt, 0, sizeof cnt);
                }
            }
            return (char*)s_env[i].val;
        }
    }
    AcquireSRWLockExclusive(&s_env_lock);
    const char* val = NULL; int found = 0;
    unsigned i = h & mask, n = 0;
    for (; n <= mask; n++, i = (i + 1) & mask) {
        if (!s_env[i].key) break;
        if (!_stricmp(s_env[i].key, name)) { val = s_env[i].val; found = 1; break; }
    }
    if (!found) {
        val = env_lookup_os(name);
        if (n <= mask) {
            s_env[i].val = val;
            MemoryBarrier();
            s_env[i].key = _strdup(name);
        }
    }
    ReleaseSRWLockExclusive(&s_env_lock);
    return (char*)val;
}

/* The console's setenv: set the variable and replace its cached value, so the
 * next getenv sees it. The old value is leaked on purpose, because a reader may
 * still hold it. */
void cached_setenv(const char* name, const char* value)
{
    _putenv_s(name, value);
    const char* v = (value && *value) ? _strdup(value) : NULL;
    AcquireSRWLockExclusive(&s_env_lock);
    unsigned i = env_hash(name) & 1023, n = 0;
    for (; n <= 1023; n++, i = (i + 1) & 1023) {
        if (!s_env[i].key) {
            s_env[i].val = v;
            MemoryBarrier();
            s_env[i].key = _strdup(name);
            break;
        }
        if (!_stricmp(s_env[i].key, name)) { s_env[i].val = v; break; }
    }
    ReleaseSRWLockExclusive(&s_env_lock);
    ps3_env_bump();   /* PS3_KNOB statics re-read on their next use */
}
}
extern "C" char* (__cdecl* __imp_getenv)(const char*) = cached_getenv;

/* Last-chance crash reporter: vm_base accesses are bounds-guarded, so a real
 * access violation means a HOST pointer deref (e.g. a bad function pointer or a
 * runtime-struct walk). Print the faulting address and the RIP as a module
 * offset (RVA) so it can be symbolized with llvm-symbolizer against the PDB. */
extern "C" uint32_t    g_last_hle_nid;    /* ppu_hle.cpp breadcrumb */
extern "C" const char* g_last_hle_name;
extern "C" volatile unsigned long long g_spu_image_runs[64];  /* spu_workload.c */
extern "C" ppu_context* volatile g_ppu_main_ctx;            /* ppu_loader.cpp: main thread ctx */

extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
static LONG WINAPI ydkj_crash_filter(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    fprintf(stderr, "\n[CRASH] code=0x%08lX rip=%p\n",
            (unsigned long)er->ExceptionCode, er->ExceptionAddress);
    fprintf(stderr, "[CRASH] last HLE NID 0x%08X (%s)\n",
            g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, "[CRASH] %s fault address 0x%llX\n",
                er->ExceptionInformation[0] ? "write" : "read",
                (unsigned long long)er->ExceptionInformation[1]);
    if (g_active_ctx) fprintf(stderr, "[CRASH] guest ctr=0x%08X lr=0x%08X r3=0x%08X\n",
          (uint32_t)g_active_ctx->ctr, (uint32_t)g_active_ctx->lr, (uint32_t)g_active_ctx->gpr[3]);
    HMODULE mod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)er->ExceptionAddress, &mod);
    fprintf(stderr, "[CRASH] module=%p rva=0x%llX  (llvm-symbolizer --obj=ydkj_boot.exe 0x%llX)\n",
            (void*)mod, (unsigned long long)((char*)er->ExceptionAddress - (char*)mod),
            (unsigned long long)((char*)er->ExceptionAddress - (char*)mod));
    /* Host call stack (RVAs) so the lifted caller can be symbolized. */
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, frames, NULL);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        if (m == mod)
            fprintf(stderr, "[CRASH]   #%-2u rva=0x%llX\n", i,
                    (unsigned long long)((char*)frames[i] - (char*)m));
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#ifdef _WIN32
/* abort()/exit(3) reporter: the recompiled CRT (or a failed invariant) can call
 * abort() — Windows turns that into exit code 3 with no message. Capture a host
 * backtrace (RVAs) + the last HLE NID so the aborting caller can be symbolized. */
static void ydkj_abort_handler(int)
{
    fprintf(stderr, "\n[ABORT] SIGABRT raised; last HLE NID 0x%08X (%s)\n",
            g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    void* frames[32];
    USHORT n = RtlCaptureStackBackTrace(0, 32, frames, NULL);
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ydkj_abort_handler, &self);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        if (m == self)
            fprintf(stderr, "[ABORT]   #%-2u rva=0x%llX\n", i,
                    (unsigned long long)((char*)frames[i] - (char*)m));
    }
    fflush(stderr);
    _exit(3);
}
#endif

/* Derive the VFS root (the dir containing PS3_GAME) from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf  -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];
static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }
    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip three trailing components: EBOOT.elf / USRDIR / PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* Host-provided symbols the runtime + HLE libs need. */
extern "C" uint8_t* vm_base = nullptr;
extern "C" uint32_t ppu_vm_size;   /* defined in ppu_loader.cpp (OOB guard) */
extern "C" void lv2_init_syscalls(void);   /* runtime/syscalls/lv2_register.c */
extern "C" void np_psnr_setup(const char* username, const char* server);  /* libs/network/np_psnr.c */

/* Guest-callback dispatch + RSX vblank/flip driver.
 *
 * g_ps3_guest_caller (defined NULL by libs/system/cellSysutil.c) is the hook the
 * HLE runtime uses to call back into recompiled code -- cellSysutil events and
 * the GCM vblank/flip handlers. ppu_guest_call (ppu_loader.cpp) does the OPD ->
 * dispatch. On real hardware the RSX fires a vblank interrupt ~60x/s that drives
 * the game's frame loop; with no RSX we synthesize it from a host timer thread
 * calling cellGcmTickVBlank()/TickFlip(), which invoke the registered handlers.
 * Without this the game inits, registers its handlers, and then waits forever
 * for a vblank that never comes. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" ps3_guest_caller_fn g_ps3_guest_caller;        /* libs/system/cellSysutil.c */
extern "C" uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" void cellGcmTickVBlank(void);
extern "C" void cellGcmTickFlip(void);
extern "C" int  cellGcm_take_flip_pending(void);  /* hoisted to file scope: clang-cl rejects block-scope extern "C" */

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{ ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7); }

/* RSX present backend. D3D12 on Windows, Metal on Apple, and the null
 * backend's headless software path anywhere else -- the same selection
 * runtime/host/host_posix.c makes, so the ticker below is backend-agnostic
 * and a POSIX host gets a frame clock instead of no clock at all. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#endif
extern "C" void cellGcm_rsx_process_fifo(void);   /* cellGcmSys.c: drain get->put */
extern "C" void cellGcm_fifo_kick_wait(unsigned ms); /* idle wait, woken by a kick */
extern "C" unsigned cellGcm_flip_request_count(void);
extern "C" unsigned cellGcmGetCurrentDisplayBufferId(void);
extern "C" int sys_event_queue_inject(unsigned int, unsigned long long, unsigned long long, unsigned long long, unsigned long long);

/* Live NV4097->D3D12 engine (libs/video/rsx_live_draw.c, from caner /
 * canersaka's Yakuza: Dead Souls port). Opt-in with RSX_LIVE_DRAW=1: the null
 * backend opens the window, the engine binds its swap chain to that HWND and
 * owns presentation, so rsx_d3d12_backend is left out entirely rather than run
 * alongside it. Unset, every path below is exactly what it was. */
extern "C" int   rsx_live_draw_enabled(void);
extern "C" int   rsx_live_draw_init(void* hwnd, uint32_t w, uint32_t h,
                                    const uint8_t* (*guest_ptr)(void*, uint32_t,
                                                                uint32_t, uint32_t),
                                    void* user);
extern "C" void  rsx_live_draw_present(uint32_t buffer_id);
extern "C" uint32_t rsx_live_draw_get_frames(void);
extern "C" uint32_t rsx_live_draw_get_last_draws(void);
extern "C" int   rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" int   rsx_null_backend_pump_messages(void);
extern "C" void* rsx_null_backend_get_hwnd(void);
extern "C" void  rsx_null_backend_suppress_present(int on);
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);
extern "C" uint32_t cellGcmResolveIO(uint32_t offset);

static int s_rsx_live = 0;   /* live engine selected AND up */

/* Resolve (location, offset) to host memory for the engine. location 0 is RSX
 * local VRAM, 1 is main/IO memory; the engine promises its callers the whole
 * min_bytes span is readable, so validate the interval, not just its start.
 * The location comes straight from the RSX DMA context selector and is
 * authoritative -- cellGcmResolveOffset()'s heuristic prefers VRAM for any page
 * the guest ever derived from a local EA, which maps a title's MAIN-memory
 * textures into local memory where they read as garbage (caner hit exactly
 * that in the Yakuza port; the geometry came out flat white). */
static const uint8_t* rsx_live_guest_ptr(void* user, uint32_t location,
                                         uint32_t offset, uint32_t min_bytes)
{
    (void)user;
    if (!vm_base) return nullptr;
    uint32_t ea;
    if (location == 0) {
        ea = cellGcmResolveLocated(1, offset);          /* RSX local VRAM */
    } else {
        ea = cellGcmResolveIO(offset);                  /* main, via the IO table */
        if (!ea) ea = cellGcmResolveLocated(0, offset); /* unmapped: old behaviour */
    }
    if (!ea || ea == 0xFFFFFFFFu ||
        (uint64_t)ea + min_bytes > 0x100000000ull) return nullptr;
    return (const uint8_t*)vm_base + ea;
}

/* Present / pump through whichever backend is live. */
static void rsx_present_frame(void)
{
    /* Present the buffer the guest actually flipped to, not buffer 0.
     *
     * rsx_live_draw_present() looks up the surface registered for the display
     * buffer it is given, so hardcoding 0 presented buffer 0's surface for
     * EVERY flip. A double-buffered title flips 0,1,0,1..., so half its frames
     * showed the previous image instead of the one just drawn -- content
     * appearing and vanishing at ~30 Hz, and black wherever buffer 0 had not
     * been drawn yet. The Simpsons Arcade Game flips 0,1 alternately and that
     * is exactly what it looked like.
     *
     * cellGcmGetCurrentDisplayBufferId() is set by cellGcmSetFlipCommand,
     * which the FIFO walker calls on this same thread before setting the
     * pending flag we are responding to, so it is current here. */
    if (s_rsx_live) rsx_live_draw_present(cellGcmGetCurrentDisplayBufferId());
    else            rsx_backend_present();   /* platform backend: D3D12 / Metal / null */
}
static int rsx_pump_messages(void)
{ return s_rsx_live ? rsx_null_backend_pump_messages() : rsx_backend_pump(); }

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished, which is the number a port means by "is anything
 * reaching the screen". The presents the ticker makes before the guest's first
 * flip (so a fresh window is not left white through a long boot) are
 * deliberately not counted: they carry no guest frame. */
static volatile LONG g_frames_presented = 0;
extern "C" unsigned ppu_boot_frames_presented(void) { return (unsigned)g_frames_presented; }
static void present_guest_frame(void)
{
    rsx_present_frame();
    /* The ticker increments, a guest thread reads: interlocked rather than a
     * volatile ++, which is neither atomic nor a fence on arm64. */
    InterlockedIncrement(&g_frames_presented);
}

/* ---- Guest-PC sampling profiler (PS3_GUEST_PROF=1) -----------------------
 * Samples every guest thread's ctx.cia every ~5ms and dumps the top sites
 * every 5s. cia is refreshed at every syscall, and guest spin loops issue a
 * syscall per iteration (yield/usleep), so a thread stuck in a wait loop
 * shows its loop's PC dominating the histogram. This is the tool that
 * answers "where does thread X spend its wall time" without a debugger. */
#include "../../syscalls/sys_ppu_thread.h"
#include "../../syscalls/sys_cond.h"          /* sync history ring (console synchist) */
#include "../ppu_waits.h"                     /* console waits */
static DWORD WINAPI guest_prof_thread(LPVOID)
{
    struct Site { uint32_t tid, cia; uint32_t hits; };
    static Site sites[256];
    int nsites = 0;
    ULONGLONG t0 = GetTickCount64();
    unsigned total = 0;
    for (;;) {
        Sleep(5);
        for (int i = 0; i < PPU_THREAD_MAX; i++) {
            ppu_thread_info* t = &g_ppu_threads[i];
            if (t->state != 1 /*RUNNING-ish; harmless if enum differs*/ && t->state != 2) continue;
            uint32_t cia = (uint32_t)t->ctx.cia;
            if (!cia) continue;
            uint32_t tid = (uint32_t)(i + 1);
            total++;
            int f = -1;
            for (int k = 0; k < nsites; k++)
                if (sites[k].tid == tid && sites[k].cia == cia) { f = k; break; }
            if (f < 0 && nsites < 256) { f = nsites++; sites[f].tid = tid; sites[f].cia = cia; sites[f].hits = 0; }
            if (f >= 0) sites[f].hits++;
        }
        ULONGLONG now = GetTickCount64();
        if (now - t0 >= 5000) {
            /* top 10 by hits */
            fprintf(stderr, "[gprof] %u samples/5s; top sites:\n", total);
            for (int rank = 0; rank < 10; rank++) {
                int best = -1; uint32_t bh = 0;
                for (int k = 0; k < nsites; k++)
                    if (sites[k].hits > bh) { bh = sites[k].hits; best = k; }
                if (best < 0 || bh == 0) break;
                fprintf(stderr, "[gprof]   tid=%-3u cia=0x%08X  %5.1f%%  (%s)\n",
                        sites[best].tid, sites[best].cia,
                        100.0 * sites[best].hits / (total ? total : 1),
                        g_ppu_threads[sites[best].tid - 1].name);
                sites[best].hits = 0;   /* consume */
            }
            fflush(stderr);
            nsites = 0; total = 0; t0 = now;
        }
    }
}

extern "C" const char* cellGame_get_title(void);   /* PARAM.SFO TITLE, for the caption */

static DWORD WINAPI vblank_ticker(LPVOID)
{
    /* Window caption: $PS3_TITLE wins, else the TITLE field PARAM.SFO already
     * gave cellGame, else a neutral name. It used to fall back to a specific
     * other title, so every port announced itself as that game. */
    const char* _title = getenv("PS3_TITLE");
    if (!_title || !*_title) {
        const char* sfo = cellGame_get_title();
        if (sfo && *sfo && strcmp(sfo, "Unknown Title") != 0) _title = sfo;
    }
    if (!_title || !*_title) _title = "ps3recomp";
    /* The live engine's RT-as-backbuffer rescue only treats a surface as the
     * backbuffer when its clip EQUALS the backend size, so a title that renders
     * into something other than 1280x720 must say so: RSX_W / RSX_H. */
    uint32_t rsx_w = 1280, rsx_h = 720;
    if (const char* e = getenv("RSX_W")) rsx_w = (uint32_t)strtoul(e, 0, 0);
    if (const char* e = getenv("RSX_H")) rsx_h = (uint32_t)strtoul(e, 0, 0);
    int rsx_ok;
    if (rsx_live_draw_enabled()) {
        rsx_ok = (rsx_null_backend_init(rsx_w, rsx_h, _title) == 0);
        if (rsx_ok && rsx_live_draw_init(rsx_null_backend_get_hwnd(), rsx_w, rsx_h,
                                         rsx_live_guest_ptr, nullptr) == 0) {
            s_rsx_live = 1;
            rsx_null_backend_suppress_present(1);
            fprintf(stderr, "[rsx] live-draw engine up (D3D12); GDI present suppressed\n");
        } else {
            fprintf(stderr, "[rsx] live-draw init FAILED -- falling back to the D3D12 backend\n");
            rsx_ok = (rsx_backend_init(rsx_w, rsx_h, _title) == 0);
        }
    } else {
        rsx_ok = (rsx_backend_init(rsx_w, rsx_h, _title) == 0);
    }
    fprintf(stderr, "[rsx] backend init %s\n", rsx_ok ? "OK -- window open" : "FAILED");
    unsigned last_flip = 0;
    /* The game's frame pacing (vblank/flip handlers -> display frame counter) must
     * advance at ~60Hz regardless of how long present() blocks. On a hidden/occluded
     * window DXGI Present throttles hard, which previously stalled these ticks and
     * paced the game's main loop to ~0.5fps. Drive the ticks off REAL elapsed time
     * and catch up in a bounded burst so present latency never slows the game. */
    /* Vblanks on the performance counter at the PS3's 59.94 Hz. They were
     * GetTickCount64 + 16 ms: 62.5 Hz, so a title flipping every other
     * vblank ran at 31.25/s and a 29.97 fps movie showed a frame twice
     * about once a second; and GetTickCount64 moves in ~15.6 ms steps, so
     * ticks came in uneven bursts. VBLANK_MHZ (millihertz, default 59940)
     * sets the rate; 60000 is NTSC-exact 60 Hz. */
    LARGE_INTEGER qpf, qpc;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qpc);
    const int vblank_mhz = ps3_env_int("VBLANK_MHZ", 59940);
    const long long tick_qpc = qpf.QuadPart * 1000 / (vblank_mhz > 1000 ? vblank_mhz : 59940);
    long long next_tick = qpc.QuadPart;
    for (;;) {
        /* Was Sleep(4). The FIFO kick event existed, was signalled on a dry
         * fence queue, and had no waiter -- so a guest starved for `get` or a
         * fence still paid the whole tick. Waiting on it drains on demand
         * when something kicks. FIFO_KICK_MS (live) is the ceiling when
         * nothing does: a title that writes `put` directly never kicks, and
         * each wait is latency on its frame's critical path (the real RSX
         * fetches continuously). 4 -> 1 took Demon's Souls from ~13.7 to
         * ~15.8 presents/s for ~15% of a core on this thread. */
        { static int kms = -1;
          PS3_KNOB(kms, ps3_env_int("FIFO_KICK_MS", 1));
          cellGcm_fifo_kick_wait(kms >= 1 ? (unsigned)kms : 1u); }
        ULONGLONG now = GetTickCount64();
        QueryPerformanceCounter(&qpc);
        int fired = 0;
        while (qpc.QuadPart - next_tick >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining further: the flip fires
             * at a get==put frame boundary (guest thread), so the batch held
             * right now is exactly the completed frame. Presenting on a raw
             * flip-count change after the drain raced the guest's next-frame
             * writes and showed empty or mixed batches. */
            {
                if (rsx_ok && cellGcm_take_flip_pending()) {
                    present_guest_frame();
                    last_flip = cellGcm_flip_request_count();
                }
            }
            /* Drain the game's GCM FIFO every tick -- this writes the RSX sync-fence
             * labels (e.g. @0x03000410) that the game's per-frame logic blocks on.
             * Doing it here (not after present) keeps those fences advancing at 60Hz
             * even when present() throttles on a hidden/occluded window. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += tick_qpc;
            fired++;
        }
        if (fired >= 240) next_tick = qpc.QuadPart;   /* fell too far behind -> resync */
        /* Drain at the outer ~4ms cadence too (not just the 16ms tick):
         * titles fence EVERY render pass on an RSX label the drain writes;
         * at 16ms per fence wave's six passes paced the guest to ~7 fps.
         * The real RSX writes those fences in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            cellGcm_rsx_process_fifo();
        }
        /* YDKJ_INJECT_Q3: the main thread polls q3 (load/state-complete) each frame but
         * it's never posted (producer signals a condvar, never enqueues). Inject a q3
         * event periodically to test whether delivering the completion advances the
         * game's state machine to instantiate + display the (force-parsed) menu movie. */
        if (getenv("YDKJ_INJECT_Q3")) {
            static int s_q3=0; const char* qe=getenv("YDKJ_INJECT_Q3"); uint32_t qid=(uint32_t)strtoul(qe,0,0); if(qid==0||qid==1) qid=3;
            if(++s_q3 % 8 == 0){ int r=sys_event_queue_inject(qid, 0x1234, 0, 0, 0);
              static int _n=0; if(_n++<12) fprintf(stderr,"[INJQ3] injected q%u event rc=%d\n",qid,r); } }
        if (rsx_ok) {
            /* The player closed the window: that is quitting the game. Only
             * stopping the presents left the title running headless, a
             * process with no window that never ended. Terminate rather than
             * ExitProcess: guest threads are still running and may hold CRT
             * locks the DLL-detach path would wait on. */
            if (rsx_pump_messages() != 0) {
                printf("[boot] window closed -- exiting%c", 10);
                fflush(stdout); fflush(stderr);
                TerminateProcess(GetCurrentProcess(), 0);
            }
            if (getenv("YDKJ_PACETRACE")) {
                static ULONGLONG s_win=0; static int s_pf=0, s_pres=0; static ULONGLONG s_presms=0;
                s_pf += fired; s_pres++;
                ULONGLONG t0=GetTickCount64(); rsx_present_frame(); ULONGLONG t1=GetTickCount64();
                s_presms += (t1-t0);
                if (s_win==0) s_win=now;
                if (now - s_win >= 1000) {
                    fprintf(stderr,"[PACE] process_fifo=%d/s  present=%d/s  present_total=%llums/s (avg %llums)\n",
                            s_pf, s_pres, s_presms, s_pres? s_presms/s_pres:0);
                    s_pf=0; s_pres=0; s_presms=0; s_win=now;
                }
            } else {
                /* Present only on a guest flip (frame boundary). A fixed-clock
                 * present can catch the drain mid-frame -- notably while the
                 * guest is blocked in the FIFO-wrap recycle callback -- and
                 * flash a partial frame (clear + a few draws). Before the
                 * first flip present freely so the window isn't stuck white
                 * during boot. */
                unsigned fc = cellGcm_flip_request_count();
                if (fc != last_flip || fc == 0) {
                    if (fc) present_guest_frame(); else rsx_present_frame();
                    last_flip = fc;
                } else if (fc == 0) {
                    rsx_backend_present();
                }
            }
        }
    }
    return 0;
}

extern "C" uint32_t    g_last_hle_nid;
extern "C" const char* g_last_hle_name;

/* Windows-only from here to the end of hang_watchdog. Snapshotting every
 * OTHER thread and reading its instruction pointer needs tlhelp32 plus
 * SuspendThread/GetThreadContext; POSIX has no equivalent that does not
 * amount to writing a debugger. The vblank ticker above, which is what the
 * frame loop actually depends on, is deliberately outside this. */
extern "C" void (*g_ds_register_shadow_hook)(unsigned long long);
extern "C" void ps3_exit_now(const char* tag);   /* ppu_hle.cpp */
#ifdef _WIN32
#include <tlhelp32.h>
/* When the boot wedges, snapshot every other thread's instruction pointer as a
 * module RVA (symbolize with llvm-symbolizer) so a guest spin/wait is pinned to
 * an exact lifted function -- the HLE breadcrumb only covers HLE calls. */
/* Snapshot every other thread's RIP. For threads in the boot module (lifted
 * guest code) print the RVA (symbolizable) + a couple of stack-return RVAs;
 * for threads parked in a DLL (OS waits / FMOD) print the module name so they
 * are not mistaken for guest spins. Called twice so the caller can diff which
 * guest thread is genuinely parked (same RIP) vs. still progressing. */
/* Defined with the debug console below; writes to stderr and, when the
 * console is servicing a command, also to its response file. */
static void dbg_printf(const char* fmt, ...);
extern "C" void ds_print_all_shadows(void (*)(const char*, ...), unsigned) __attribute__((weak));
extern "C" void ppu_guard_page_rearm(uint32_t guest_ea, int noaccess);   /* ppu_loader.cpp */
/* While dump_threads holds a thread suspended, dbg_printf formats into this
 * buffer instead of a stream: the suspended thread may own stderr's CRT lock,
 * and writing then deadlocks the watchdog -- and with it every thread that
 * logs. GH3 froze solid (0% CPU, log silent mid-boot) exactly that way. */
static __declspec(thread) char   s_dbg_capbuf[16384];
static __declspec(thread) char*  s_dbg_cap = NULL;
static __declspec(thread) size_t s_dbg_capn = 0;

extern "C" const char* g_hle_inflight[];   /* ppu_hle.cpp; 64 entries */
extern "C" void ppu_report_guest_lrs(void); /* ppu_loader.cpp */

static void dump_threads(const char* label, HMODULE self)
{
    dbg_printf( "[WATCHDOG] %s; last HLE call = 0x%08X (%s)\n",
            label, g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");

    /* Which HLE each guest thread is INSIDE right now.
     *
     * ppu_hle.cpp has maintained this array for a while and nothing ever read
     * it. It is the only way to see a thread parked in an HLE wait:
     * sys_lwmutex_lock and sys_lwcond_wait are sysPrxForUser handlers, not lv2
     * syscalls, so PS3_SCBLOCK_PROF cannot see them block, the usleep
     * histogram cannot see them (they do not usleep), and the sampler just
     * reports ntdll. A thread stuck and invisible to all three is exactly the
     * case this answers. */
    { int any = 0;
      for (unsigned t = 0; t < 64u; t++) {
          const char* n = g_hle_inflight[t];
          if (!n) continue;
          if (!any) { dbg_printf("[WATCHDOG] guest threads inside an HLE:\n"); any = 1; }
          dbg_printf("[WATCHDOG]   guest-tid %-3u in %s\n", t, n);
      }
      if (!any) dbg_printf("[WATCHDOG] no guest thread is inside an HLE\n"); }
    dbg_printf("[WATCHDOG] guest threads, by guest lr:\n");
    ppu_report_guest_lrs();
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;
            s_dbg_capn = 0; s_dbg_capbuf[0] = 0; s_dbg_cap = s_dbg_capbuf;
            SuspendThread(th);
            CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &ctx)) {
                HMODULE m = NULL;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)ctx.Rip, &m);
                if (m == self) {
                    dbg_printf( "[WATCHDOG]   tid %5lu BOOT rip rva=0x%llX\n",
                            (unsigned long)te.th32ThreadID,
                            (unsigned long long)((char*)ctx.Rip - (char*)self));
                } else {
                    char path[MAX_PATH] = "?";
                    if (m) GetModuleFileNameA(m, path, sizeof path);
                    const char* base = strrchr(path, '\\');
                    dbg_printf( "[WATCHDOG]   tid %5lu in %s\n",
                            (unsigned long)te.th32ThreadID, base ? base + 1 : path);
                }
                /* Scan the suspended thread's stack for boot-module return
                 * addresses to reconstruct the lifted call chain — done for ALL
                 * threads (even when RIP is parked in ntdll inside a CriticalSection
                 * call), since that's exactly where the busy-spin's lwmutex churn
                 * lands the main thread. Map RVAs -> func_ names via flow.map.
                 * (some false positives expected — these are stack-scan hits.) */
                {
                    uint64_t* sp = (uint64_t*)ctx.Rsp;
                    /* Bound the scan to the committed stack region so we never read
                     * past the guard page. If the query fails or the region is not
                     * committed+readable (thread mid-create, guard page, garbage
                     * Rsp), skip the scan entirely -- a diagnostic AV here gets
                     * caught by the crash filter and kills the whole run. */
                    MEMORY_BASIC_INFORMATION mbi;
                    uint64_t region_end = (uint64_t)sp;
                    if (VirtualQuery((LPCVOID)sp, &mbi, sizeof mbi) &&
                        mbi.State == MEM_COMMIT &&
                        !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                                        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                        region_end = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
                    int maxk = (int)((region_end - (uint64_t)sp) / 8);
                    if (maxk > 0x200000 / 8) maxk = 0x200000 / 8;   /* lifted SPU frames are big */
                    int found = 0;
                    for (int k = 0; k < maxk && found < 40; k++) {
                        uint64_t v = sp[k];
                        if (v < (uint64_t)self) continue;
                        HMODULE mm = NULL;
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCSTR)v, &mm);
                        if (mm == self) {
                            /* Resolve to the guest function containing this return
                             * address: the function_table entry with the largest host
                             * pointer <= it. A raw RVA needs symbols a release build
                             * does not have, and "ret rva=0x519F66" says nothing about
                             * where a hung title actually is. Same mapping the
                             * unresolved-call dump uses. */
                            uint32_t bestGuest = 0;
                            uintptr_t bestHost = 0, nextHost = 0, maxHost = 0;
                            for (uint64_t q = 0; q < function_table_count; q++) {
                                uintptr_t h = (uintptr_t)function_table[q].func;
                                if (h > maxHost) maxHost = h;
                                if (h <= (uintptr_t)v && h > bestHost) {
                                    bestHost = h; bestGuest = function_table[q].addr;
                                }
                            }
                            for (uint64_t q = 0; q < function_table_count; q++) {
                                uintptr_t h = (uintptr_t)function_table[q].func;
                                if (h > bestHost && (nextHost == 0 || h < nextHost)) nextHost = h;
                            }
                            /* The highest-addressed lifted function has no next, so its
                             * extent is unknown -- and EVERY runtime/CRT address above it
                             * was attributed to it. Refuse to guess there. A confident
                             * wrong frame is worse than no frame. */
                            if (bestHost == maxHost ||
                                (uintptr_t)v - bestHost > 0x4000u) bestGuest = 0;
                            dbg_printf( bestGuest
                                    ? "[WATCHDOG]       tid %5lu ret rva=0x%llX  func_%08X+0x%llX\n"
                                    : "[WATCHDOG]       tid %5lu ret rva=0x%llX\n",
                                    (unsigned long)te.th32ThreadID,
                                    (unsigned long long)(v - (uint64_t)self),
                                    bestGuest, (unsigned long long)((uintptr_t)v - bestHost));
                            found++;
                        }
                    }
                }
            }
            ResumeThread(th);
            s_dbg_cap = NULL;
            if (s_dbg_capn) dbg_printf("%s", s_dbg_capbuf);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    fflush(stderr);
}

/* ── Debug console ───────────────────────────────────────────────────────────
 * PS3_DEBUG=<path> opens a file-based command channel, so a title that is
 * already running can be asked what it is doing without a rebuild.
 *
 * Write one command into <path>; the console executes it, appends the answer
 * to <path>.out, and truncates <path> ready for the next one.
 *
 *     echo threads    > dbg.txt     # symbolised stack of every guest thread
 *     echo "mem 10200 64" > dbg.txt # hexdump of guest memory
 *     echo stat       > dbg.txt     # flips, HLE breadcrumb, uptime
 *
 * A file and not a socket on purpose: no listening port inside a game
 * process, nothing for a firewall to prompt about, and it drives from a shell
 * script exactly the way PAD_FILE already does.
 *
 * Read-only by design apart from poke32. Changing a diagnostic knob while the
 * title runs is NOT possible here: the large majority are read once at first
 * use and cached in a function-local static (see docs/DIAGNOSTICS.md), so they
 * are launch-time settings. `knobs` reports which ones this run was started
 * with, which is the useful half of that.
 */
static FILE* s_dbg_out = NULL;

static void dbg_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (s_dbg_cap) {             /* a thread is suspended: format only, no stream lock */
        int n = vsnprintf(s_dbg_cap + s_dbg_capn, sizeof s_dbg_capbuf - s_dbg_capn, fmt, ap);
        if (n > 0) s_dbg_capn += (size_t)n;
        if (s_dbg_capn >= sizeof s_dbg_capbuf) s_dbg_capn = sizeof s_dbg_capbuf - 1;
        va_end(ap);
        return;
    }
    if (s_dbg_out) { va_list c; va_copy(c, ap); vfprintf(s_dbg_out, fmt, c); va_end(c); }
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void dbg_mem(uint32_t ea, uint32_t len)
{
    if (!vm_base) { dbg_printf("  vm_base not live yet%c", 10); return; }
    if (len == 0 || len > 4096) len = 64;
    for (uint32_t off = 0; off < len; off += 16) {
        char asc[17];
        dbg_printf("  0x%08X ", ea + off);
        for (uint32_t i = 0; i < 16; i++) {
            if (off + i < len) {
                uint8_t b = vm_base[(size_t)(ea + off + i)];
                dbg_printf("%02X ", b);
                asc[i] = (b >= 32 && b < 127) ? (char)b : '.';
            } else { dbg_printf("   "); asc[i] = ' '; }
        }
        asc[16] = 0;
        dbg_printf(" |%s|%c", asc, 10);
    }
}

/* find <start> <len> <hex bytes>: every place a byte pattern sits in guest
 * memory (capped at 32), e.g. the source of a value the title copies into a
 * GPU buffer. Only committed host pages are read, so a sparse range is safe. */
static void dbg_find(uint32_t ea, uint32_t len, const char* hex)
{
    if (!vm_base) { dbg_printf("  vm_base not live yet%c", 10); return; }
    uint8_t pat[64]; size_t pn = 0;
    for (const char* h = hex; h[0] && h[1] && pn < sizeof pat; h += 2) {
        unsigned v; if (sscanf(h, "%2x", &v) != 1) break; pat[pn++] = (uint8_t)v;
    }
    if (!pn) { dbg_printf("  usage: find <start> <len> <hexbytes>%c", 10); return; }
    int hits = 0;
    uint64_t end = (uint64_t)ea + len;
    for (uint64_t at = ea; at < end && hits < 32; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + at, &mi, sizeof mi)) break;
        uint64_t rend = (uint64_t)((uint8_t*)mi.BaseAddress + mi.RegionSize - vm_base);
        if (rend > end) rend = end;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            for (uint64_t i = at; i + pn <= rend && hits < 32; i += 4)
                if (!memcmp(vm_base + i, pat, pn)) {
                    dbg_printf("  hit 0x%08X%c", (uint32_t)i, 10); hits++;
                }
        }
        at = rend;
    }
    dbg_printf("  %d hit(s)%c", hits, 10);
}

static void dbg_knobs(const char* prefix)
{
    /* Which diagnostics this run was actually started with. */
    LPCH env = GetEnvironmentStringsA();
    if (!env) return;
    int n = 0;
    for (LPCH p = env; *p; p += strlen(p) + 1) {
        const char* eq = strchr(p, '=');
        if (!eq || eq == p) continue;
        /* Only the runtime's own namespaces, not the whole shell environment. */
        static const char* known[] = {
            "PS3_", "SPU_", "SPURS_", "GCM_", "RSX_", "LD_", "YZ_", "YDKJ_",
            "LBP_", "FLOW_", "RD_", "TEX_", "FP_", "VP_", "PPU_", "PS1_",
            "CELLMARK_", "RTT_", "PAD_", "FS_", "WATCHDOG_", "SYNC_", NULL };
        int match = 0;
        for (int k = 0; known[k]; k++)
            if (strncmp(p, known[k], strlen(known[k])) == 0) { match = 1; break; }
        if (!match) continue;
        if (prefix && *prefix && strncmp(p, prefix, strlen(prefix)) != 0) continue;
        dbg_printf("  %s%c", p, 10);
        n++;
    }
    FreeEnvironmentStringsA(env);
    dbg_printf("  (%d set; docs/DIAGNOSTICS.md lists all of them)%c", n, 10);
}

/* "prof <host-tid> [sec]": sample ONE thread at ~1 kHz and unwind it through
 * .pdata (exact, unlike dump_threads' stack scan), then print the hottest
 * functions exclusive (leaf) and inclusive (anywhere on the stack), keyed by
 * function start. exe entries print as rva=; resolve against the link map.
 * Only lock-free work happens while the thread is suspended.
 * ponytail: fixed 4096-slot tables; a thread touching more functions drops the rest. */
#define PROF_SLOTS 4096
static uint64_t s_prof_key[3][PROF_SLOTS];
static uint32_t s_prof_ct[3][PROF_SLOTS];
static void prof_hit(int tab, uint64_t key)
{
    for (uint32_t i = (uint32_t)((key >> 4) * 2654435761u) % PROF_SLOTS, n = 0; n < PROF_SLOTS;
         n++, i = (i + 1) % PROF_SLOTS) {
        if (s_prof_key[tab][i] == key) { s_prof_ct[tab][i]++; return; }
        if (!s_prof_key[tab][i]) { s_prof_key[tab][i] = key; s_prof_ct[tab][i] = 1; return; }
    }
}
/* Name exe RVAs from the link map beside the exe (des_boot.map): the nearest
 * symbol at or below each. One pass over the map, only when asked. */
static void map_names(const uint64_t* rva, int n, char (*name)[48])
{
    uint64_t best[64] = {0};
    for (int i = 0; i < n; i++) snprintf(name[i], 48, "rva=0x%llX", (unsigned long long)rva[i]);
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, sizeof path);
    if (!len || len < 4 || len >= sizeof path) return;
    memcpy(path + len - 4, ".map", 5);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char ln[512], sym[256];
    unsigned long long va, base = 0x140000000ull;
    while (fgets(ln, sizeof ln, f)) {
        if (strstr(ln, "Preferred load address is")) { sscanf(strstr(ln, "is") + 2, "%llx", &base); continue; }
        if (sscanf(ln, " %*x:%*x %255s %llx", sym, &va) != 2 || va < base) continue;
        const uint64_t r = va - base;
        for (int i = 0; i < n && i < 64; i++)
            if (r <= rva[i] && r >= best[i]) {
                best[i] = r;
                if (rva[i] - r < 0x100000)
                    snprintf(name[i], 48, "%.40s%s", sym, rva[i] == r ? "" : "+");
            }
    }
    fclose(f);
}

/* cpu [sec]: CPU per host thread over the last sec seconds (default 5), with
 * the guest thread name (or the host thread's start function), split by where
 * the time went:
 *   app  the exe: lifted guest code and the runtime
 *   os   ntdll/kernel32/KernelBase: yields, spin-waits and syscalls -- time a
 *        waiting thread burns instead of blocking shows up here
 *   dll  everything else (graphics driver, D3D12, audio)
 * and the exe function with the most time ("hot"; for an os/dll sample, the
 * exe function that called out, so a spin-wait names its caller). Each thread
 * is sampled every ~5 ms and the cycles it ran since the last sample are
 * credited to where it is now, so a blocked thread costs nothing.
 * ponytail: at most 256 threads; 16 hot functions per thread. */
static void dbg_cpu(unsigned sec)
{
    enum { MAXT = 256, HOT = 16 };
    struct T {
        DWORD tid; HANDLE h; ULONG64 cyc, cat[3]; FILETIME k0, u0; double ms;
        uint64_t hk[HOT]; ULONG64 hc[HOT]; uint64_t start;
    };
    static T t[MAXT];
    int nt = 0;
    const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    const uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL);
    typedef LONG (WINAPI *NtQIT)(HANDLE, int, PVOID, ULONG, PULONG);
    NtQIT qit = (NtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me || nt >= MAXT) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                  FALSE, te.th32ThreadID);
            if (!h) continue;
            T* x = &t[nt++]; memset(x, 0, sizeof *x);
            x->tid = te.th32ThreadID; x->h = h;
            QueryThreadCycleTime(h, &x->cyc);
            FILETIME c0, e0; GetThreadTimes(h, &c0, &e0, &x->k0, &x->u0);
            PVOID sa = NULL;
            if (qit) qit(h, 9 /* ThreadQuerySetWin32StartAddress */, &sa, sizeof sa, NULL);
            x->start = (uint64_t)(uintptr_t)sa;
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);

    HMODULE os[3] = { GetModuleHandleA("ntdll.dll"), GetModuleHandleA("kernel32.dll"),
                      GetModuleHandleA("KernelBase.dll") };
    const ULONGLONG t0 = GetTickCount64(), end = t0 + sec * 1000ull;
    while (GetTickCount64() < end) {
        for (int i = 0; i < nt; i++) {
            T* x = &t[i];
            ULONG64 c = 0;
            if (!QueryThreadCycleTime(x->h, &c)) continue;
            const ULONG64 d = c - x->cyc; x->cyc = c;
            if (d < 1000) continue;                      /* did not run */
            if (SuspendThread(x->h) == (DWORD)-1) continue;
            CONTEXT ctx; ctx.ContextFlags = CONTEXT_FULL;
            int cat = 2; uint64_t hot = 0;
            if (GetThreadContext(x->h, &ctx)) {
                HMODULE m = NULL;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ctx.Rip, &m);
                cat = (uintptr_t)m == exe ? 0 : (m && (m == os[0] || m == os[1] || m == os[2])) ? 1 : 2;
                /* first exe frame: the leaf itself, or who called out of the exe */
                for (int dpt = 0; dpt < 16 && ctx.Rip; dpt++) {
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
                    if ((uintptr_t)(ctx.Rip - exe) < 0x40000000u) {
                        hot = rf ? base + rf->BeginAddress - exe : ctx.Rip - exe; break;
                    }
                    if (!rf) break;
                    void* hd; DWORD64 ef;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &ef, NULL);
                }
            }
            ResumeThread(x->h);
            x->cat[cat] += d;
            if (hot) {
                int k = 0;
                while (k < HOT && x->hk[k] && x->hk[k] != hot) k++;
                if (k < HOT) { x->hk[k] = hot; x->hc[k] += d; }
            }
        }
        Sleep(5);
    }
    const double wall = (double)(GetTickCount64() - t0);
    for (int i = 0; i < nt; i++) {
        FILETIME c1, e1, k1, u1; GetThreadTimes(t[i].h, &c1, &e1, &k1, &u1);
        ULONGLONG a = ((ULONGLONG)k1.dwHighDateTime << 32 | k1.dwLowDateTime) +
                      ((ULONGLONG)u1.dwHighDateTime << 32 | u1.dwLowDateTime);
        ULONGLONG b = ((ULONGLONG)t[i].k0.dwHighDateTime << 32 | t[i].k0.dwLowDateTime) +
                      ((ULONGLONG)t[i].u0.dwHighDateTime << 32 | t[i].u0.dwLowDateTime);
        t[i].ms = (double)(a - b) / 10000.0;
        CloseHandle(t[i].h);
    }
    /* busiest first; name the top 24 threads' start and hot functions */
    int ord[MAXT]; for (int i = 0; i < nt; i++) ord[i] = i;
    for (int i = 1; i < nt; i++) for (int j = i; j > 0 && t[ord[j]].ms > t[ord[j - 1]].ms; j--)
        { int s = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = s; }
    const int show = nt < 24 ? nt : 24;
    uint64_t rv[64]; char nm[64][48]; int nr = 0;
    int hot_of[24], start_of[24];
    for (int r = 0; r < show; r++) {
        T* x = &t[ord[r]];
        int hb = -1; for (int k = 0; k < HOT; k++) if (x->hk[k] && (hb < 0 || x->hc[k] > x->hc[hb])) hb = k;
        hot_of[r] = hb >= 0 ? nr : -1; if (hb >= 0) rv[nr++] = x->hk[hb];
        start_of[r] = (uintptr_t)(x->start - exe) < 0x40000000u ? nr : -1;
        if (start_of[r] >= 0) rv[nr++] = x->start - exe;
    }
    map_names(rv, nr, nm);
    double tot = 0; for (int i = 0; i < nt; i++) tot += t[i].ms;
    dbg_printf("  %u threads, %.2f cores busy over %.1f s (cpu%% = of one core)%c", (unsigned)nt,
               tot / wall, wall / 1000.0, 10);
    dbg_printf("  %-6s %6s %5s %5s %5s  %-28s %s%c", "host", "cpu%", "app", "os", "dll", "thread", "hot", 10);
    for (int r = 0; r < show; r++) {
        T* x = &t[ord[r]];
        if (x->ms < wall * 0.005) break;
        const ULONG64 cs = x->cat[0] + x->cat[1] + x->cat[2];
        char who[64] = "";
        for (int g = 0; g < PPU_THREAD_MAX; g++)
            if (g_ppu_threads[g].state != PPU_THREAD_STATE_FREE && g_ppu_threads[g].host_tid == x->tid)
                snprintf(who, sizeof who, "ppu %d \"%.20s\"", g + 1, g_ppu_threads[g].name);
        if (!who[0]) snprintf(who, sizeof who, "%s", start_of[r] >= 0 ? nm[start_of[r]] : "(dll thread)");
        dbg_printf("  %-6lu %6.1f %4.0f%% %4.0f%% %4.0f%%  %-28.28s %s%c", (unsigned long)x->tid,
                   100.0 * x->ms / wall,
                   cs ? 100.0 * x->cat[0] / cs : 0.0, cs ? 100.0 * x->cat[1] / cs : 0.0,
                   cs ? 100.0 * x->cat[2] / cs : 0.0, who, hot_of[r] >= 0 ? nm[hot_of[r]] : "-", 10);
    }
}

static void dbg_prof(DWORD tid, unsigned sec)
{
    /* Suspending ourselves never resumes: the console would hang for good. */
    if (tid == GetCurrentThreadId()) { dbg_printf("  tid %lu is the console%c", (unsigned long)tid, 10); return; }
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) { dbg_printf("  cannot open tid %lu%c", (unsigned long)tid, 10); return; }
    memset(s_prof_key, 0, sizeof s_prof_key); memset(s_prof_ct, 0, sizeof s_prof_ct);
    const uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL);
    unsigned samples = 0;
    const ULONGLONG end = GetTickCount64() + sec * 1000ull;
    while (GetTickCount64() < end) {
        if (SuspendThread(h) == (DWORD)-1) break;
        CONTEXT c; c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &c)) {
            samples++;
            uint64_t seen[24]; int ns = 0, got = 0;
            for (int d = 0; d < 24 && c.Rip; d++) {
                DWORD64 base = 0;
                PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                uint64_t key = rf ? base + rf->BeginAddress : c.Rip;
                if (d == 0) prof_hit(0, key);
                /* first exe frame of a sample whose leaf is in a DLL: who called out */
                if (!got && (uintptr_t)(key - exe) < 0x40000000u) { got = 1; if (d > 0) prof_hit(2, key); }
                int dup = 0; for (int k = 0; k < ns; k++) dup |= seen[k] == key;
                if (!dup) { seen[ns++] = key; prof_hit(1, key); }
                if (!rf) break;                       /* leaf without unwind data: stop */
                void* hd; DWORD64 ef;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &ef, NULL);
            }
        }
        ResumeThread(h);
        Sleep(1);
    }
    CloseHandle(h);
    dbg_printf("  prof tid %lu: %u samples over %us%c", (unsigned long)tid, samples, sec, 10);
    for (int tab = 0; tab < 3; tab++) {
        static const char* tn[3] = { "exclusive", "inclusive", "exe caller of DLL leaf" };
        dbg_printf("  --- %s ---%c", tn[tab], 10);
        for (int rank = 0; rank < 40; rank++) {
            uint32_t best = 0; int bi = -1;
            for (int k = 0; k < PROF_SLOTS; k++) if (s_prof_ct[tab][k] > best) { best = s_prof_ct[tab][k]; bi = k; }
            if (bi < 0) break;
            uint64_t key = s_prof_key[tab][bi];
            HMODULE m = NULL; char path[MAX_PATH] = "?";
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)key, &m);
            if (m) GetModuleFileNameA(m, path, sizeof path);
            const char* b = strrchr(path, 92); b = b ? b + 1 : path;
            if ((uintptr_t)m == exe) dbg_printf("  %5.1f%%  rva=0x%llX%c", 100.0 * best / (samples ? samples : 1),
                                                (unsigned long long)(key - exe), 10);
            else dbg_printf("  %5.1f%%  %s+0x%llX%c", 100.0 * best / (samples ? samples : 1), b,
                            (unsigned long long)(key - (uintptr_t)m), 10);
            s_prof_ct[tab][bi] = 0;
        }
    }
}

/* waits [s]: what guest threads spend their time inside, over s seconds.
 * Every lv2 syscall and HLE call is timed per (thread, call, r3); r3 is
 * usually the object waited on. A call already running when the window opens
 * is not seen, so a thread parked the whole time shows nothing: ctx/gstacks
 * answer that one. HLE handlers that make syscalls count at both levels. */
static const char* sc_name(uint32_t n)
{
    /* Numbers from lv2_syscall_table.h, so the labels cannot drift. */
    switch (n) {
    case SYS_EVENT_FLAG_WAIT:     return "event_flag_wait";
    case SYS_SEMAPHORE_WAIT:      return "semaphore_wait";
    case SYS_SEMAPHORE_TRYWAIT:   return "semaphore_trywait";
    case SYS_SEMAPHORE_POST:      return "semaphore_post";
    case SYS_LWMUTEX_LOCK:        return "lwmutex_lock";
    case SYS_LWMUTEX_UNLOCK:      return "lwmutex_unlock";
    case SYS_MUTEX_LOCK:          return "mutex_lock";
    case SYS_MUTEX_UNLOCK:        return "mutex_unlock";
    case SYS_COND_WAIT:           return "cond_wait";
    case SYS_COND_SIGNAL:         return "cond_signal";
    case SYS_COND_SIGNAL_ALL:     return "cond_signal_all";
    case SYS_LWCOND_WAIT:         return "lwcond_queue_wait";
    case SYS_EVENT_QUEUE_RECEIVE: return "event_queue_receive";
    case SYS_EVENT_PORT_SEND:     return "event_port_send";
    case SYS_TIMER_USLEEP:        return "timer_usleep";
    case SYS_TIMER_SLEEP:         return "timer_sleep";
    case SYS_PPU_THREAD_YIELD:    return "ppu_thread_yield";
    case SYS_PPU_THREAD_JOIN:     return "ppu_thread_join";
    case SYS_RWLOCK_RLOCK:        return "rwlock_rlock";
    case SYS_RWLOCK_WLOCK:        return "rwlock_wlock";
    case SYS_FS_OPEN:             return "fs_open";
    case SYS_FS_READ:             return "fs_read";
    case SYS_FS_LSEEK:            return "fs_lseek";
    default: return "";
    }
}

static void dbg_waits(unsigned sec, unsigned rows)
{
    memset(g_waits, 0, sizeof(ppu_waits_ent) * PPU_WAITS_N);
    g_waits_dropped = 0;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const uint64_t a = ppu_waits_now();
    g_waits_armed = 1;
    Sleep(sec * 1000u);
    g_waits_armed = 0;
    const double win_ms = (double)(ppu_waits_now() - a) * 1000.0 / (double)f.QuadPart;
    Sleep(100);   /* calls in flight at the close land */
    const double tms = 1000.0 / (double)f.QuadPart;
    static ppu_waits_ent e[PPU_WAITS_N];
    memcpy(e, g_waits, sizeof e);
    unsigned used = 0;
    for (unsigned i = 0; i < PPU_WAITS_N; i++) used += e[i].n != 0;
    dbg_printf("  %u distinct (thread, call, r3)%c", used, 10);

    dbg_printf("  waits over %.0f ms (%% = share of the window), %u calls dropped (table full)%c",
               win_ms, (unsigned)g_waits_dropped, 10);
    /* per thread: time inside syscalls and inside HLE calls */
    for (uint32_t tid = 0; tid < 256; tid++) {
        double sc = 0, hl = 0; unsigned n = 0;
        for (unsigned i = 0; i < PPU_WAITS_N; i++)
            if (e[i].n && e[i].tid == tid) { (e[i].hle ? hl : sc) += e[i].ticks * tms; n += e[i].n; }
        if (!n) continue;
        const char* tn = tid >= 1 && tid <= PPU_THREAD_MAX ? g_ppu_threads[tid - 1].name : "?";
        dbg_printf("  tid %-3u %-28s syscalls %5.1f%%  hle %5.1f%%  %u calls%c", tid, tn,
                   100.0 * sc / win_ms, 100.0 * hl / win_ms, n, 10);
    }
    /* by (thread, call), r3 folded: HLE calls often take a different pointer each time */
    { static ppu_waits_ent g[4096]; unsigned ng = 0;
      for (unsigned i = 0; i < PPU_WAITS_N; i++) {
          if (!e[i].n) continue;
          unsigned k = 0;
          while (k < ng && (g[k].tid != e[i].tid || g[k].hle != e[i].hle || g[k].key != e[i].key)) k++;
          if (k == ng) { if (ng == 4096) continue; g[ng] = e[i]; g[ng].n = 0; g[ng].ticks = 0; g[ng].obj = 0; ng++; }
          g[k].n += e[i].n; g[k].ticks += e[i].ticks; g[k].obj++;
      }
      dbg_printf("  --- by thread and call (r3 folded; objs = distinct r3) ---%c", 10);
      for (unsigned rank = 0; rank < rows; rank++) {
          uint64_t best = 0; int bi = -1;
          for (unsigned i = 0; i < ng; i++) if (g[i].n && g[i].ticks > best) { best = g[i].ticks; bi = (int)i; }
          if (bi < 0) break;
          const ppu_waits_ent* w = &g[bi];
          const char* tn = w->tid >= 1 && w->tid <= PPU_THREAD_MAX ? g_ppu_threads[w->tid - 1].name : "?";
          char what[64];
          if (w->hle) snprintf(what, sizeof what, "%s", w->name ? w->name : "?");
          else snprintf(what, sizeof what, "sc%u %s", w->key, sc_name(w->key));
          const double ms = w->ticks * tms;
          dbg_printf("  %5.1f%% %8.1fms n=%-6u avg=%7.0fus  tid %-3u %-22.22s %-26.26s objs=%u%c",
                     100.0 * ms / win_ms, ms, w->n, 1000.0 * ms / w->n, w->tid, tn, what, w->obj, 10);
          g[bi].n = 0;
      } }
    dbg_printf("  --- top calls ---%c", 10);
    for (unsigned rank = 0; rank < rows + rows / 2; rank++) {
        uint64_t best = 0; int bi = -1;
        for (unsigned i = 0; i < PPU_WAITS_N; i++)
            if (e[i].n && e[i].ticks > best) { best = e[i].ticks; bi = (int)i; }
        if (bi < 0) break;
        const ppu_waits_ent* w = &e[bi];
        const char* tn = w->tid >= 1 && w->tid <= PPU_THREAD_MAX ? g_ppu_threads[w->tid - 1].name : "?";
        char what[64];
        if (w->hle) snprintf(what, sizeof what, "%s", w->name ? w->name : "?");
        else snprintf(what, sizeof what, "sc%u %s", w->key, sc_name(w->key));
        const double ms = w->ticks * tms;
        dbg_printf("  %5.1f%% %8.1fms n=%-6u avg=%7.0fus  tid %-3u %-22.22s %-26.26s r3=0x%08X%c",
                   100.0 * ms / win_ms, ms, w->n, 1000.0 * ms / w->n, w->tid, tn, what, w->obj, 10);
        e[bi].n = 0;
    }
}

static DWORD WINAPI debug_console(LPVOID param)
{
    const char* path = (const char*)param;
    char outpath[1024];
    snprintf(outpath, sizeof outpath, "%s.out", path);

    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&debug_console, &self);
    const DWORD t0 = GetTickCount();
    fprintf(stderr, "[dbg] console on '%s' -- write a command, read '%s'%c",
            path, outpath, 10);

    for (;;) {
        Sleep(100);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        char cmd[512] = {0};
        if (!fgets(cmd, sizeof cmd, f)) { fclose(f); continue; }
        fclose(f);
        char* nl = strpbrk(cmd, "\r\n"); if (nl) *nl = 0;
        if (!cmd[0]) continue;
        FILE* t = fopen(path, "wb"); if (t) fclose(t);   /* consume it */

        s_dbg_out = fopen(outpath, "ab");
        dbg_printf("%c[dbg] > %s%c", 10, cmd, 10);

        char verb[64] = {0};
        unsigned a = 0, b = 0;
        sscanf(cmd, "%63s", verb);

        if (!strcmp(verb, "help")) {
            dbg_printf("  threads          stacks of every guest thread, symbolised%c", 10);
            dbg_printf("  gstacks [n]      guest shadow stack of every thread, n frames%c", 10);
            dbg_printf("  prof <tid> [s]   sample one host thread ~1kHz, hottest functions%c", 10);
            dbg_printf("  cpu [s]          CPU per host thread over s seconds: app/os(spin, yield)/dll split%c", 10);
            dbg_printf("  waits [s] [rows] time per guest thread inside each syscall/HLE call (+r3) over s seconds%c", 10);
            dbg_printf("  hle              last HLE call the runtime dispatched%c", 10);
            dbg_printf("  stat             flips, HLE breadcrumb, uptime%c", 10);
            dbg_printf("  spu              SPU program runs per lifted image id%c", 10);
            dbg_printf("  ctx <tid>        a guest thread's live registers (lr = where it is parked)%c", 10);
            dbg_printf("  synchist <id> [n] cond/mutex history for an id (create, wait, signal, destroy)%c", 10);
            dbg_printf("  mem <hex> [len]  hexdump guest memory%c", 10);
            dbg_printf("  poke32 <hex> <v> write a guest u32%c", 10);
            dbg_printf("  knobs [prefix]   diagnostics this run was started with%c", 10);
            dbg_printf("  setenv K=V       set a diagnostic knob now (K= clears it)%c", 10);
            dbg_printf("  guard <hex> [rw] arm the page guard on a guest word now%c", 10);
        } else if (!strcmp(verb, "gstacks")) {
            /* Guest shadow stacks of every registered thread, innermost first
             * (needs a --shadow-stack lift and a title that registers them). */
            sscanf(cmd, "%*s %u", &a);
            if (ds_print_all_shadows) ds_print_all_shadows(dbg_printf, a ? a : 14);
            else dbg_printf("  no shadow stacks in this build%c", 10);
        } else if (!strcmp(verb, "threads")) {
            dump_threads("console", self);
        } else if (!strcmp(verb, "find32") && sscanf(cmd, "%*s %x", &a) == 1) {
            /* find32 <hex>: every aligned big-endian u32 equal to <hex> in committed
             * guest memory below 0xC0000000 (first 4096 hits). */
            const uint32_t be = ((a & 0xFF) << 24) | ((a & 0xFF00) << 8) | ((a >> 8) & 0xFF00) | (a >> 24);
            int hits = 0;
            for (uintptr_t p = 0; p < 0xC0000000u && hits < 4096 && vm_base; ) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(vm_base + p, &mi, sizeof mi)) break;
                const uintptr_t end = (uintptr_t)((uint8_t*)mi.BaseAddress + mi.RegionSize) - (uintptr_t)vm_base;
                if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                    for (uintptr_t q = p & ~3u; q + 4 <= end && q < 0xC0000000u && hits < 4096; q += 4)
                        if (*(const uint32_t*)(vm_base + q) == be) { dbg_printf("  0x%08X%c", (unsigned)q, 10); hits++; }
                p = end;
            }
            dbg_printf("  %d hit(s)%c", hits, 10);
        } else if (!strcmp(verb, "nzmap") && sscanf(cmd, "%*s %x %x", &a, &b) == 2) {
            /* nzmap <hex start> <hex len>: the runs of 64KB pages in the range
             * that hold any nonzero byte -- where data actually landed. */
            uint32_t run0 = 0; int in = 0, runs = 0;
            for (uint64_t p = a; p <= (uint64_t)a + b && vm_base && runs < 200; p += 0x10000) {
                int nz = 0;
                if (p < (uint64_t)a + b) {
                    MEMORY_BASIC_INFORMATION mi;
                    if (VirtualQuery(vm_base + p, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
                        !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                        const uint64_t* w = (const uint64_t*)(vm_base + p);
                        for (int i = 0; i < 0x10000 / 8; i++) if (w[i]) { nz = 1 + i; break; }
                    }
                }
                if (nz && !in) {
                    run0 = (uint32_t)p; in = 1;
                    const uint8_t* f = vm_base + p + (nz - 1) * 8;
                    dbg_printf("  first nz 0x%08X:", (unsigned)(p + (nz - 1) * 8));
                    for (int i = 0; i < 32; i++) dbg_printf(" %02X", f[i]);
                    dbg_printf("%c", 10);
                }
                if (!nz && in) { dbg_printf("  0x%08X-0x%08X%c", run0, (unsigned)p, 10); in = 0; runs++; }
            }
            dbg_printf("  %d run(s)%c", runs, 10);
        } else if (!strcmp(verb, "dumpbin") && sscanf(cmd, "%*s %x %x", &a, &b) == 2) {
            /* dumpbin <hex ea> <hex len> <file>: raw guest bytes to a host file
             * (uncommitted pages are written as zeros). */
            char path[260] = {0};
            sscanf(cmd, "%*s %*x %*x %259s", path);
            FILE* f = path[0] ? fopen(path, "wb") : NULL;
            uint32_t done = 0;
            for (uint32_t p = a; f && vm_base && p < a + b; ) {
                MEMORY_BASIC_INFORMATION mi;
                const uint32_t chunk_end = (p | 0xFFFu) + 1u;
                const uint32_t n2 = (chunk_end < a + b ? chunk_end : a + b) - p;
                static const uint8_t zeros[0x1000] = {0};
                const int ok = VirtualQuery(vm_base + p, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
                               !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
                fwrite(ok ? vm_base + p : zeros, 1, n2, f);
                done += n2; p += n2;
            }
            if (f) fclose(f);
            dbg_printf("  dumpbin 0x%08X +0x%X -> %s (%u bytes)%c", a, b, path[0] ? path : "(no file)", done, 10);
        } else if (!strcmp(verb, "findhex")) {
            /* findhex <hexbytes>: every 4-aligned occurrence of a byte string in
             * committed guest memory, RSX local memory (0xC0000000+) included
             * (first 256 hits). */
            uint8_t pat[64]; size_t n = 0;
            const char* h = cmd + 7;
            while (*h == ' ') h++;
            for (; h[0] && h[1] && n < sizeof pat; h += 2) {
                unsigned v; if (sscanf(h, "%2x", &v) != 1) break; pat[n++] = (uint8_t)v; }
            int hits = 0;
            for (uint64_t p = 0; n && p < 0x100000000ull && hits < 256 && vm_base; ) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(vm_base + p, &mi, sizeof mi)) break;
                const uint64_t end = (uint64_t)((uint8_t*)mi.BaseAddress + mi.RegionSize - vm_base);
                if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                    for (uint64_t q = (p + 3) & ~3ull; q + n <= end && hits < 256; q += 4)
                        if (!memcmp(vm_base + q, pat, n)) { dbg_printf("  0x%08X%c", (unsigned)q, 10); hits++; }
                p = end;
            }
            dbg_printf("  %d hit(s) for %u byte(s)%c", hits, (unsigned)n, 10);
        } else if (!strcmp(verb, "qbset") && sscanf(cmd, "%*s %x %x", &a, &b) == 2) {
            /* qbset <name-crc> <value>: set every integer QB struct member with
             * that name. A member is 16 bytes: xx 01|81 00 00 | name | value | next,
             * big-endian (GH3: bot_play, unlocked). Prints how many it set. */
            const uint32_t be_name = ((a & 0xFF) << 24) | ((a & 0xFF00) << 8) | ((a >> 8) & 0xFF00) | (a >> 24);
            const uint32_t be_val  = ((b & 0xFF) << 24) | ((b & 0xFF00) << 8) | ((b >> 8) & 0xFF00) | (b >> 24);
            unsigned n = 0;
            for (uintptr_t p = 0; p < 0xC0000000u && vm_base; ) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(vm_base + p, &mi, sizeof mi)) break;
                const uintptr_t end = (uintptr_t)((uint8_t*)mi.BaseAddress + mi.RegionSize) - (uintptr_t)vm_base;
                if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
                    for (uintptr_t q = (p + 4) & ~3u; q + 8 <= end; q += 4)
                        if (*(const uint32_t*)(vm_base + q) == be_name) {
                            /* type word: any flags byte, then 0x01 or 0x81 (integer), 00 00;
                             * only 0/1 values change -- an unlocked tag is a flag. */
                            const uint8_t* t = vm_base + q - 4;
                            const uint32_t v = *(const uint32_t*)(vm_base + q + 4);
                            if ((t[1] == 0x01 || t[1] == 0x81) && !t[2] && !t[3] &&
                                (v == 0 || v == 0x01000000u)) {
                                *(uint32_t*)(vm_base + q + 4) = be_val; n++;
                            }
                        }
                p = end;
            }
            dbg_printf("  qbset 0x%08X = %u: %u member(s)%c", a, b, n, 10);
        } else if (!strcmp(verb, "cpu")) {
            sscanf(cmd, "%*s %u", &a);
            dbg_cpu(a ? a : 5);
        } else if (!strcmp(verb, "waits")) {
            sscanf(cmd, "%*s %u %u", &a, &b);
            dbg_waits(a ? a : 3, b ? b : 40);
        } else if (!strcmp(verb, "prof") && sscanf(cmd, "%*s %u %u", &a, &b) >= 1) {
            dbg_prof((DWORD)a, b ? b : 5);
        } else if (!strcmp(verb, "ctx") && sscanf(cmd, "%*s %u", &a) == 1) {
            /* ctx <tid>: that guest thread's live registers. Read unlocked from
             * the thread's own context, so a running thread gives a torn view;
             * a parked one (the usual question) gives where it is parked: lr is
             * the return address into the function that made the blocking call. */
            if (a >= 1 && a <= PPU_THREAD_MAX && g_ppu_threads[a - 1].state != PPU_THREAD_STATE_FREE) {
                /* the main thread's context lives on the loader's frame */
                const ppu_context* c = (a == 1 && g_ppu_main_ctx) ? g_ppu_main_ctx : &g_ppu_threads[a - 1].ctx;
                dbg_printf("  tid %u \"%s\" cia=0x%08X lr=0x%08X ctr=0x%08X r1=0x%08X r2=0x%08X%c", a,
                           g_ppu_threads[a - 1].name, (unsigned)c->cia, (unsigned)c->lr, (unsigned)c->ctr,
                           (unsigned)c->gpr[1], (unsigned)c->gpr[2], 10);
                for (int r = 0; r < 32; r += 8)
                    dbg_printf("  r%-2d %08X %08X %08X %08X %08X %08X %08X %08X%c", r,
                               (unsigned)c->gpr[r], (unsigned)c->gpr[r + 1], (unsigned)c->gpr[r + 2],
                               (unsigned)c->gpr[r + 3], (unsigned)c->gpr[r + 4], (unsigned)c->gpr[r + 5],
                               (unsigned)c->gpr[r + 6], (unsigned)c->gpr[r + 7], 10);
            } else dbg_printf("  no thread %u%c", a, 10);
        } else if (!strcmp(verb, "synchist") && sscanf(cmd, "%*s %x", &a) == 1) {
            /* synchist <hex id> [n]: the last n (default 40) sync-history events
             * for that id, oldest first. Cond ops: C create (x=mutex, r=id
             * address), D destroy, W/w wait enter/return (x=mutex), S/A signal
             * (x=waiters then). Mutex ops (same id space, separate objects):
             * M create (x=id address), X destroy (x=owner, r=lock count). */
            unsigned want = 40; sscanf(cmd, "%*s %*x %u", &want);
            const long n = g_sync_hist_n;
            const long lo = n > SYNC_HIST_N ? n - SYNC_HIST_N : 0;
            static long hits[4096]; unsigned nh = 0;
            for (long i = n - 1; i >= lo && nh < want && nh < 4096; i--)
                if (g_sync_hist[i & (SYNC_HIST_N - 1)].id == a) hits[nh++] = i;
            while (nh--) {
                const sync_hist_ent* e = &g_sync_hist[hits[nh] & (SYNC_HIST_N - 1)];
                dbg_printf("  t=%u %c id=0x%X tid=%u lr=0x%08X x=0x%X r=0x%08X%c", e->t_ms, e->op,
                           e->id, e->tid, e->lr, e->extra, (unsigned)e->res, 10);
            }
        } else if (!strcmp(verb, "spu")) {
            /* SPU program runs per lifted image id since start (dispatch HITs). */
            for (int k = 0; k < 64; k++)
                if (g_spu_image_runs[k])
                    dbg_printf("  image %2d: %llu run(s)%c", k, g_spu_image_runs[k], 10);
        } else if (!strcmp(verb, "hle")) {
            dbg_printf("  last HLE = 0x%08X (%s)%c", g_last_hle_nid,
                       g_last_hle_name ? g_last_hle_name : "", 10);
        } else if (!strcmp(verb, "stat")) {
            dbg_printf("  uptime   %.1f s%c", (GetTickCount() - t0) / 1000.0, 10);
            dbg_printf("  flips    %u%c", cellGcm_flip_request_count(), 10);
            dbg_printf("  last HLE 0x%08X (%s)%c", g_last_hle_nid,
                       g_last_hle_name ? g_last_hle_name : "", 10);
            dbg_printf("  vm_base  %s%c", vm_base ? "live" : "not mapped", 10);
        } else if (!strcmp(verb, "mem") && sscanf(cmd, "%*s %x %u", &a, &b) >= 1) {
            dbg_mem(a, b);
        } else if (!strcmp(verb, "find")) {
            char hx[160] = {0};
            if (sscanf(cmd, "%*s %x %x %159s", &a, &b, hx) == 3) dbg_find(a, b, hx);
            else dbg_printf("  usage: find <start> <len> <hexbytes>%c", 10);
        } else if (!strcmp(verb, "poke32") && sscanf(cmd, "%*s %x %x", &a, &b) == 2) {
            if (vm_base) {
                uint32_t be = ((b & 0xFF) << 24) | ((b & 0xFF00) << 8) |
                              ((b >> 8) & 0xFF00) | ((b >> 24) & 0xFF);
                memcpy(vm_base + a, &be, 4);
                dbg_printf("  [0x%08X] = 0x%08X%c", a, b, 10);
            } else dbg_printf("  vm_base not live yet%c", 10);
        } else if (!strcmp(verb, "guard") && sscanf(cmd, "%*s %x", &a) == 1) {
            /* The PPU_GUARD_EA knob arms at load, which is too early for heap
             * memory the title allocates later. Arm it once the object exists. */
            ppu_guard_page_rearm(a, strstr(cmd, " rw") != NULL);
            dbg_printf("  guard on 0x%08X%c", a, 10);
        } else if (!strcmp(verb, "setenv")) {
            /* Turn a diagnostic on mid-run, e.g. a frame dump once a level is
             * up. Only knobs the code re-reads with getenv() see the change. */
            char kv[256] = {0};
            sscanf(cmd, "%*s %255s", kv);
            char* eq = strchr(kv, '=');
            if (eq) { *eq = 0; cached_setenv(kv, eq + 1); dbg_printf("  %s=%s%c", kv, eq + 1, 10); }
            else dbg_printf("  usage: setenv K=V%c", 10);
        } else if (!strcmp(verb, "knobs")) {
            char pfx[64] = {0};
            sscanf(cmd, "%*s %63s", pfx);
            dbg_knobs(pfx);
        } else {
            dbg_printf("  ? '%s' -- try 'help'%c", verb, 10);
        }

        /* End marker: a driver script waits for this rather than sleeping. */
        if (s_dbg_out) { fprintf(s_dbg_out, "[dbg] done%c", 10); fclose(s_dbg_out); s_dbg_out = NULL; }
    }
    return 0;
}

static DWORD WINAPI hang_watchdog(LPVOID)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&hang_watchdog, &self);
    /* ponytail: two fixed samples cover a boot wedge, but a hang that happens
     * minutes in (e.g. after a level load) needs them moved. WATCHDOG_AT="a,b"
     * gives the two sample times in seconds. */
    int t1 = 8, t2 = 15;
    { const char* e = getenv("WATCHDOG_AT"); int a = 0, b = 0;
      if (e && sscanf(e, "%d,%d", &a, &b) == 2 && a > 0 && b > a) { t1 = a; t2 = b; } }
    char lbl[64];
    Sleep((DWORD)t1 * 1000);
    snprintf(lbl, sizeof lbl, "%ds sample", t1); dump_threads(lbl, self);
    Sleep((DWORD)(t2 - t1) * 1000);
    snprintf(lbl, sizeof lbl, "%ds sample", t2); dump_threads(lbl, self);
    return 0;
}
#endif

/* The flat VM treats every address as valid RAM, so it must span every region
 * the PS3 memory map uses. The game's heap maps at 0x20000000+ and reaches
 * ~0x50000000, but sys_ppu_thread_create allocates thread stacks in the PS3
 * stack region at 0xD0000000-0xDFFFFFFF (vm.h: VM_STACK_BASE). Without covering
 * that, every spawned thread's stack access is OOB (reads 0 / writes dropped)
 * and the thread crashes. Size to include the stack region: ~3.75 GB, lazily
 * committed by the OS (only touched pages are backed). */
#define VM_SIZE    0x100010000ull /* full 32-bit guest space + 64K guard (top-edge reads), demand-committed */
#define STACK_TOP  0x0FF00000u   /* main-thread stack, below the 0x10000000 segment */

#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the full 4 GB guest space up front (no
 * commit cost) and commit each 64 KB page on first access. This makes EVERY
 * 32-bit guest offset valid -- a garbage guest pointer reads as zero instead of
 * crashing the process (essential now that the recompiled engine runs deep and
 * worker threads touch incomplete state). Out-of-arena faults fall through to
 * the crash reporter. */
/* Guard-page-violation reporter: STATUS_GUARD_PAGE_VIOLATION (0x80000001)
 * bypasses every ACCESS_VIOLATION-only handler and kills the process SILENTLY
 * (observed the moment the Bink SPU decoder first really ran). Log full context
 * before letting the chain continue. */
static LONG WINAPI guard_report_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == 0x80000001u /*STATUS_GUARD_PAGE_VIOLATION*/) {
        static LONG s_n = 0;
        if (InterlockedIncrement(&s_n) <= 8) {
            char* mbase = (char*)GetModuleHandleA(NULL);
            fprintf(stderr, "\n[GUARDVIOLATION] tid=%lu %s fault=0x%llX rip-rva=0x%llX\n",
                    GetCurrentThreadId(),
                    ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                    (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
                    (unsigned long long)((char*)ep->ExceptionRecord->ExceptionAddress - mbase));
            void* fr[24]; USHORT n = RtlCaptureStackBackTrace(0, 24, fr, NULL);
            fprintf(stderr, "[GUARDVIOLATION] bt-rva:");
            for (USHORT i = 0; i < n; i++)
                fprintf(stderr, " %llX", (unsigned long long)((char*)fr[i] - mbase));
            fprintf(stderr, "\n"); fflush(stderr);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char** argv)
{
    /* <PPU ELF> [--username NAME] [--psnr host[:port]], flags in any order. */
    const char* elf = NULL;
    const char* username = NULL;
    const char* psnr = NULL;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char** flag = !strncmp(a, "--username", 10) ? &username
                          : !strncmp(a, "--psnr", 6)      ? &psnr : NULL;
        if (!flag) { if (!elf) elf = a; continue; }
        const char* eq = strchr(a, '=');
        if (eq) *flag = eq + 1;
        else if (i + 1 < argc) *flag = argv[++i];
    }
    if (!elf) {
        printf("usage: %s <PPU ELF> [--username NAME] [--psnr host[:port]]\n", argv[0]);
        return 2;
    }
    np_psnr_setup(username, psnr);

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    timeBeginPeriod(1);   /* 1ms timer resolution: the default ~15.6ms granularity
                           * inflates every sub-15ms wait (the game's event polls,
                           * usleeps) and throttled the whole title. */
    /* PS3_PRIORITY=below|idle|normal and PS3_AFFINITY=<hex cpu mask>: keep a
     * spinning or hung title from starving the rest of the machine during
     * test runs. Unset leaves the process as the OS started it. */
    { const char* pr = getenv("PS3_PRIORITY");
      if (pr && *pr) {
          DWORD cls = !_stricmp(pr, "idle")  ? IDLE_PRIORITY_CLASS :
                      !_stricmp(pr, "below") ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
          SetPriorityClass(GetCurrentProcess(), cls);
      }
      const char* af = getenv("PS3_AFFINITY");
      /* "all" is no limit. Parsed as hex it was 0xA (strtoull stops at the
       * 'l'): two logical CPUs, and every sleep in the title overran. */
      if (af && *af && _stricmp(af, "all")) {
          DWORD_PTR want = (DWORD_PTR)strtoull(af, 0, 16), proc = 0, sys = 0;
          if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && (want & sys))
              SetProcessAffinityMask(GetCurrentProcess(), want & sys);
      }
      if ((pr && *pr) || (af && *af))
          fprintf(stderr, "[boot] priority=%s affinity=%s\n", pr ? pr : "-", af ? af : "-"); }
    SetUnhandledExceptionFilter(ydkj_crash_filter);
    AddVectoredExceptionHandler(0 /*last*/, [](EXCEPTION_POINTERS* ep)->LONG{
        if (ep->ExceptionRecord->ExceptionCode == 0xC00000FDu /*STACK_OVERFLOW*/) {
            fprintf(stderr,"\n[STACKOVERFLOW] infinite recursion detected; backtrace (RVAs):\n");
            HMODULE mod=GetModuleHandleA(0); void* fr[62]; USHORT n=RtlCaptureStackBackTrace(0,62,fr,0);
            for(USHORT i=0;i<n;i++) fprintf(stderr," %llX",(unsigned long long)((char*)fr[i]-(char*)mod));
            fprintf(stderr,"\n"); fflush(stderr); ExitProcess(7);
        }
        return EXCEPTION_CONTINUE_SEARCH; });
    { ULONG g=256*1024; SetThreadStackGuarantee(&g); }  /* reserve stack so the SO handler can run */
    signal(SIGABRT, ydkj_abort_handler);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: don't lose prints on kill */
#endif

    /* Flat VM: one host buffer, guest addr -> vm_base + addr. This maps the
     * FULL 32-bit guest space uniformly (page 0, the 0x60000000..0xD0000000
     * range, everything) -- which native-VA mapping can't on Windows, because
     * the OS reserves the low 64 KB and DLLs occupy parts of the mid range.
     * On real PS3 those addresses are RAM, and the game writes to them (its
     * null-object inits land on page 0); calloc backs them so the game runs.
     * HLE functions that take guest pointers must translate via vm_base /
     * vm_write* (which also byte-swap) -- a raw *guest_ptr would deref the host
     * buffer's offset incorrectly. */
#ifdef _WIN32
    /* Reserve the full 4 GB guest space; pages commit on first touch via the VEH. */
    AddVectoredExceptionHandler(1, guard_report_veh);  /* diagnose 0x80000001 silent deaths */
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;   /* full 32-bit space backed -> OOB guard unnecessary */
#else
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
#endif
    if (!vm_base) { printf("vm alloc failed\n"); return 1; }

    uint32_t entry = ppu_load_elf(elf);
    if (!entry) { printf("load failed\n"); return 1; }

    derive_vfs_root(elf);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    /* Real title id, from the game's own PARAM.SFO. cellGame has been able to
     * read this since 2026-06-21, but only a title's own main() ever called it,
     * so every port that moved to this harness silently kept the BLES00000
     * placeholder -- and that id is what cellGame / cellSaveData / trophy build
     * their /dev_hdd0/game/<id> paths from, so saves landed in a directory
     * belonging to no title. Same shape as the live-draw engine before 1a050be:
     * a working facility with no caller in the shared harness. */
    {
        /* PARAM.SFO sits in a different place depending on how the VFS is rooted.
         * A disc-style root has it under PS3_GAME/; a title whose content is
         * opened by RELATIVE path needs the root AT USRDIR, and then PARAM.SFO is
         * one level up instead. Only the first layout was tried, so rooting at
         * USRDIR silently lost the title id and name -- which is how the window
         * caption ended up on its hardcoded fallback. Try each and take the
         * first that exists. */
        static const char* const kSfoLayouts[] = {
            "%s/PS3_GAME/PARAM.SFO",   /* vfs root = disc root      */
            "%s/../PARAM.SFO",         /* vfs root = .../USRDIR     */
            "%s/PARAM.SFO",            /* vfs root = the game dir   */
        };
        char sfo[1100];
        int found = 0;
        for (size_t i = 0; i < sizeof kSfoLayouts / sizeof kSfoLayouts[0]; i++) {
            snprintf(sfo, sizeof sfo, kSfoLayouts[i], ppu_vfs_root);
            FILE* f = fopen(sfo, "rb");
            if (f) { fclose(f); found = 1; break; }
        }
        if (!found)
            snprintf(sfo, sizeof sfo, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
        cellGame_init_from_paramsfo(sfo);
    }

    fprintf(stderr,"[boot-dbg] before ppu_recomp_register\n"); fflush(stderr);
    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_title_options_init(); /* title options (weak no-op by default) */
    fprintf(stderr,"[boot-dbg] after ppu_recomp_register; before ps3_load_prx_modules\n"); fflush(stderr);
    ps3_load_prx_modules();  /* real system PRX (libsre) -> guest RAM + exports */
    fprintf(stderr,"[boot-dbg] after prx; before ppu_hle_init\n"); fflush(stderr);
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT (sys_initialize_tls, ...) */
    ppu_fs_register();       /* cellFs VFS over the real game directory */
    fprintf(stderr,"[boot-dbg] before lv2_init_syscalls\n"); fflush(stderr);
    lv2_init_syscalls();     /* real lv2 syscall table (semaphore/memory/fs/...) */

    /* Two scalars that make a milestone diff interpretable: a lift that found
     * fewer functions, or a build that registered fewer HLE handlers, explains
     * a whole run going missing without anyone having to bisect the stream. */
    ps3_ms_kv("load:functions", (long long)function_table_count);
    ps3_ms_kv("load:hle_registered", (long long)ps3_hle_count());
    ps3_ms("boot:init_done");
    fprintf(stderr,"[boot-dbg] after lv2_init_syscalls\n"); fflush(stderr);

    /* Install the guest-callback hook and start the synthetic RSX vblank driver
     * so the game's frame loop advances (it no-ops until the game registers its
     * vblank/flip handlers during init). */
    g_ps3_guest_caller = harness_guest_caller;
    /* CreateThread is a pthread wrapper off Windows (win32_compat.h), so the
     * frame clock now runs everywhere. Without it a POSIX host never ticks
     * vblank or drains the FIFO, and the guest waits on fences forever. */
    CreateThread(NULL, 4u * 1024 * 1024, vblank_ticker, NULL, 0, NULL);
    if (getenv("PS3_GUEST_PROF"))
        CreateThread(NULL, 0, guest_prof_thread, NULL, 0, NULL);
#ifdef _WIN32
    CreateThread(NULL, 0, hang_watchdog, NULL, 0, NULL);
    /* PS3_DEBUG=<file>: ask a running title what it is doing. */
    { static char dbgpath[1024];
      const char* dp = getenv("PS3_DEBUG");
      if (dp && *dp) { snprintf(dbgpath, sizeof dbgpath, "%s", dp);
                       CreateThread(NULL, 0, debug_console, dbgpath, 0, NULL); } }   /* tlhelp32-based */
#endif

    ps3_ms("boot:entry");
#ifdef _WIN32
    { extern void ps3_sampler_start(void); ps3_sampler_start(); } /* PS3_SAMPLE=<ms> */
#endif
    /* PS3_MAIN_STACK_LV2=1: give the main thread its stack where lv2 does, in
     * the 0xD0000000 stack region (the first allocation there, which is also
     * what sys_ppu_thread_get_stack_information already reports for it).
     * Guest code can tell: Tornado Outbreak's SPU memcpy (func_000D60E0) copies
     * on the PPU when the destination shares the stack's top nibble, then waits
     * for an SPU reply only the SPU path sends -- with the stack at 0x0FF00000,
     * every low address "is on the stack" and the boot hangs. Opt-in until the
     * regression gate has run with it on. */
    uint32_t stack_top = STACK_TOP;
    { const char* ms = getenv("PS3_MAIN_STACK_LV2");
      if (ms && *ms && *ms != '0') {
          uint32_t base = vm_stack_allocate(&g_vm_stack_alloc, VM_PPU_STACK_SIZE);
          if (base) stack_top = base + VM_PPU_STACK_SIZE;
      } }
    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n", entry, stack_top);
#ifdef _WIN32
    fprintf(stderr, "[boot] MAIN guest thread tid=%lu\n", (unsigned long)GetCurrentThreadId());
#endif
    if (g_ds_register_shadow_hook) g_ds_register_shadow_hook(1);
    /* PS3_PIDFILE=<path>: append this process id, so a test script can stop
     * the instances it started without touching any other copy running. */
    if (const char* pf = getenv("PS3_PIDFILE")) {
        if (FILE* f = fopen(pf, "a")) { fprintf(f, "%lu\n", (unsigned long)GetCurrentProcessId()); fclose(f); }
    }
    /* EXIT_AFTER_MS=<n>: a hard stop for test runs, from inside the process,
     * so a probe does not need an external timeout to end. */
    if (const char* e = getenv("EXIT_AFTER_MS")) {
        static DWORD s_after; s_after = (DWORD)atoi(e);
        CloseHandle(CreateThread(NULL, 0, [](LPVOID) -> DWORD {
            Sleep(s_after);
            ps3_exit_now("EXIT_AFTER_MS");
            Sleep(INFINITE);
            return 0; }, NULL, 0, NULL));
    }
    int rc = ppu_run(entry, stack_top);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);
    /* A guest that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published one
     * -- returning a hardcoded 0 reported success for a run that never got
     * anywhere, which is exactly the kind of green a boot harness must not
     * produce. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
