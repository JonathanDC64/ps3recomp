/*
 * ps3recomp - integrated PPU boot harness (first-boot attempt).
 *
 * Links the whole PPU runtime half into one executable and starts executing
 * the recompiled game's entry point:
 *
 *   lifted code (ppu_recomp.c) + loader (ppu_loader.cpp) + HLE bridge
 *   (ppu_hle.cpp + generated NID table) + HLE libs (cellGcmSys, rsx_commands)
 *
 * It loads the real EBOOT image, registers the lifted functions and the HLE
 * NID handlers, then dispatches the entry. Execution runs real Uncharted boot
 * code until it reaches a function outside the lifted subset (logged by the
 * unlifted stub), an unimplemented firmware import (logged by ps3_hle_call),
 * or an lv2 syscall (logged by lv2_syscall) -- telling us exactly what to
 * implement next.
 *
 * This proves the integration builds + runs; a full-image build additionally
 * needs the lifter to split output into multiple TUs (88 MB single-file
 * otherwise).
 */
#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
uint32_t ppu_load_elf(const char* path);
void     ppu_recomp_register(void);
void     ppu_hle_init(void);
void     ppu_sysprx_register(void);
void     ppu_fs_register(void);
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
extern const char* ppu_vfs_root;   /* host dir that PS3 mount points map into */
/* Optional hook: load real system PRX modules (libsre = cellSpurs/cellSync) into
 * guest RAM and register their exports. Weak default is a no-op; a title that
 * links a lifted PRX defines a strong version. Called after the lifted function
 * table is registered and vm_base is live, before the game runs. */
void     ps3_load_prx_modules(void) __attribute__((weak));
void     ps3_load_prx_modules(void) {}
}

#include <string.h>
#include <stdlib.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
/* Last-chance crash reporter: vm_base accesses are bounds-guarded, so a real
 * access violation means a HOST pointer deref (e.g. a bad function pointer or a
 * runtime-struct walk). Print the faulting address and the RIP as a module
 * offset (RVA) so it can be symbolized with llvm-symbolizer against the PDB. */
extern "C" uint32_t    g_last_hle_nid;    /* ppu_hle.cpp breadcrumb */
extern "C" const char* g_last_hle_name;

extern "C" __declspec(thread) ppu_context* g_active_ctx;
/* atexit probe: the process is terminating via a clean exit() (not a crash or
 * abort) somewhere after the SPURS task dispatch, with no unwind log. Capture a
 * host backtrace at exit so the exit() caller (a guest sys_process_exit, a CRT
 * path, or cellGameExitToShelf) maps to a lifted func_XXXX / HLE symbol. */
static void boot_atexit(void)
{
    fprintf(stderr, "\n[boot] *** exit() reached; last HLE NID 0x%08X (%s) ***\n",
            g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    if (g_active_ctx)
        fprintf(stderr, "[boot]   guest cia=0x%08X lr=0x%08X ctr=0x%08X r3=0x%08X\n",
                (uint32_t)g_active_ctx->cia, (uint32_t)g_active_ctx->lr,
                (uint32_t)g_active_ctx->ctr, (uint32_t)g_active_ctx->gpr[3]);
    void* frames[40];
    USHORT n = RtlCaptureStackBackTrace(0, 40, frames, NULL);
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&boot_atexit, &self);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        if (m == self)
            fprintf(stderr, "[boot-exit]   #%-2u rva=0x%llX\n", i,
                    (unsigned long long)((char*)frames[i] - (char*)m));
    }
    fflush(stderr);
}

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
    if (g_active_ctx) fprintf(stderr, "[CRASH] guest cia=0x%08X ctr=0x%08X lr=0x%08X r3=0x%08X\n",
          (uint32_t)g_active_ctx->cia, (uint32_t)g_active_ctx->ctr, (uint32_t)g_active_ctx->lr, (uint32_t)g_active_ctx->gpr[3]);
    HMODULE mod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)er->ExceptionAddress, &mod);
    fprintf(stderr, "[CRASH] module=%p rva=0x%llX  (llvm-symbolizer --obj=des_boot.exe 0x%llX)\n",
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
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" ps3_guest_caller_fn g_ps3_guest_caller;        /* libs/system/cellSysutil.c */
extern "C" uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" void thrdiag_set_name(const char*);   /* thread_diag.c */
extern "C" void thrdiag_dump(void);
extern "C" void ps3_hle_register_ctx(unsigned, const char*, void(*)(ppu_context*));  /* ppu_hle.cpp */
extern "C" void cellGcmTickVBlank(void);
extern "C" void cellGcmTickFlip(void);

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{ ppu_guest_call(opd, a0, a1, a2, a3); }

#ifdef _WIN32
/* RSX present backend (libs/video/rsx_d3d12_backend.c). Driven on the vblank
 * thread so the D3D12 device + window message pump live on one thread. */
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" void cellGcm_rsx_process_fifo(void);   /* cellGcmSys.c: drain get->put */
extern "C" int  lv2_semaphore_post_by_id(uint32_t, int);  /* sys_semaphore.c: vblank-sema post */
extern "C" int  lv2_semaphore_post_frame(uint32_t);       /* sys_semaphore.c: conditional frame-sema post */
extern "C" uint32_t thrdiag_wobj_of(const char*, const char*);  /* thread_diag.c: a thread's current wait obj */
extern "C" uint32_t watch_thread_pc(void);                      /* ppu_loader.cpp: WATCH_HDD live guest PC */
extern "C" void vm_write32(unsigned long long, unsigned int);  /* guest BE write */
extern "C" int  gcm_display_event_post(unsigned long long, unsigned long long, unsigned long long); /* sys_event.c: libgcm vblank/flip event */

static DWORD WINAPI vblank_ticker(LPVOID)
{
    int rsx_ok = (rsx_d3d12_backend_init(1280, 720, "Demon's Souls (ps3recomp)") == 0);
    fprintf(stderr, "[rsx] backend init %s\n", rsx_ok ? "OK -- window open" : "FAILED");
    /* Emulate the RSX vblank interrupt that on real HW wakes _gcm_intr_thread to
     * post the GCM vblank semaphore HighGraphics waits on each frame (RPCS3:
     * sema 0x9604d100 posted by _gcm_intr_thread). Without it HighGraphics blocks
     * -> never signals MAIN's frame cond -> boot wedges pre-title. VBLANK_SEMA=<id>
     * selects the guest semaphore id to post each tick. */
    long vbl_sema = 0; { const char* e = getenv("VBLANK_SEMA"); if (e) vbl_sema = strtol(e, 0, 0); }
    /* RSX vblank reference LABELS: on real HW the RSX advances flip/vblank-count
     * labels each vblank; HighGraphics polls them (index 255=0x03000FF0,
     * 129=0x03000810) and spins while they stay 0. RSX_TICK_LABELS="255,129"
     * -> write an incrementing count to those label slots each tick. */
    #define GCM_LABEL_BASE 0x03000000u
    #define GCM_LABEL_STRIDE 0x10u
    int lbl_idx[8]; int lbl_n = 0;
    { const char* e = getenv("RSX_TICK_LABELS");
      if (e) { char buf[64]; strncpy(buf, e, 63); buf[63]=0;
               for (char* t = strtok(buf, ","); t && lbl_n < 8; t = strtok(NULL, ","))
                   lbl_idx[lbl_n++] = (int)strtol(t, 0, 0); } }
    /* Conditional frame-sema post (default ON): emulate _gcm_intr_thread posting the
     * libgcm vblank frame semaphore each vblank. Safe by construction -- posts only a
     * binary sema with a finite-timeout waiter blocked (HighGraphics), never a t=0
     * one-shot barrier (SLSession). Set VBLANK_TICK=0 to disable. */
    int vbl_tick = 1; { const char* e = getenv("VBLANK_TICK"); if (e) vbl_tick = atoi(e); }
    /* libgcm display event (vblank/flip) post -- the real inter-frame trigger. Default ON. */
    int gcm_display_evt = 1; { const char* e = getenv("GCM_DISPLAY_EVT"); if (e) gcm_display_evt = atoi(e); }
    /* PROBE: one-shot post of a semaphore after the boot settles. POKE_SEMA=<id>
     * (internal id), POKE_AFTER=<ticks, default 240=~4s>. Used to test whether
     * releasing SLSession's gate sema (3) makes it call cellSaveDataAutoLoad2. */
    long poke_sema = 0; { const char* e = getenv("POKE_SEMA"); if (e) poke_sema = strtol(e,0,0); }
    long poke_after = 240; { const char* e = getenv("POKE_AFTER"); if (e) poke_after = strtol(e,0,0); }
    int poke_done = 0;
    unsigned vbl_count = 0;
    unsigned watch_n = 0; uint32_t watch_last = 0xFFFFFFFF; int watch_stable = 0;
    for (;;) {
        Sleep(16);            /* ~60 Hz */
        /* POKE_ONCE=1 -> single post; else post every poke_after ticks (pump probe). */
        if (poke_sema > 0 && (long)(++vbl_count) >= poke_after) {
            static int _pn = 0; static long _last = 0;
            int once = (getenv("POKE_ONCE") != 0);
            if ((!once || !poke_done) && (long)vbl_count - _last >= (once?0:30)) {
                _last = vbl_count; if (++_pn <= 40) fprintf(stderr, "[poke] post sema %ld (#%d)\n", poke_sema, _pn);
                lv2_semaphore_post_by_id((uint32_t)poke_sema, 1); poke_done = 1; } }
        /* WATCH_HDD sampler: log the watched (hung) thread's live guest PC ~2x/s; flag
         * when it stops advancing (a spin) so the stuck guest addr is obvious. */
        if ((++watch_n % 30) == 0) { uint32_t wp = watch_thread_pc();
            if (wp) { if (wp == watch_last) { if (++watch_stable == 3)
                        fprintf(stderr, "[watch] SPIN at guest PC 0x%08X (stable)\n", wp); }
                      else { watch_stable = 0; watch_last = wp;
                        fprintf(stderr, "[watch] guest PC 0x%08X\n", wp); } } }
        /* Post exactly the sema HighGraphics is currently blocked on (its frame
         * wait), if any -- the libgcm _gcm_intr vblank tick. Targeted by thread so
         * we never disturb other threads' init waits. */
        if (vbl_tick) { uint32_t hg = thrdiag_wobj_of("HighGraphics", "sema");
                        if (hg) lv2_semaphore_post_frame(hg); }
        if (vbl_sema > 0) lv2_semaphore_post_by_id((uint32_t)vbl_sema, 1);
        if (lbl_n) { ++vbl_count;
            for (int i = 0; i < lbl_n; i++)
                vm_write32(GCM_LABEL_BASE + (unsigned)lbl_idx[i] * GCM_LABEL_STRIDE, vbl_count); }
        cellGcmTickVBlank();
        cellGcmTickFlip();
        /* libgcm display-event source: post vblank(data2=0x2)+flip(data2=0x10) to
         * the game's fee1dead display port each tick, waking _gcm_intr_thread which
         * on real HW is driven by the RSX/vblank interrupt. This is the missing
         * inter-frame trigger: without it the render loop submits frame 1 then
         * blocks forever (RPCS3 fires ~1300 vblank + ~1200 flip events during boot).
         * GCM_DISPLAY_EVT=0 disables for A/B testing. */
        if (gcm_display_evt) {
            gcm_display_event_post(0, 0x2, 0);   /* vblank */
            gcm_display_event_post(0, 0x10, 0);  /* flip   */
        }
        cellGcm_rsx_process_fifo();          /* drain FIFO (get->put) + run commands;
                                              * needed even with no window so the title's
                                              * get==put FIFO waits complete */
        if (rsx_ok) {
            if (rsx_d3d12_backend_pump_messages() != 0) { rsx_ok = 0; }
            rsx_d3d12_backend_present();     /* present the frame */
        }
    }
    return 0;
}

extern "C" uint32_t    g_last_hle_nid;
extern "C" const char* g_last_hle_name;
#include <tlhelp32.h>
/* When the boot wedges, snapshot every other thread's instruction pointer as a
 * module RVA (symbolize with llvm-symbolizer) so a guest spin/wait is pinned to
 * an exact lifted function -- the HLE breadcrumb only covers HLE calls. */
/* Snapshot every other thread's RIP. For threads in the boot module (lifted
 * guest code) print the RVA (symbolizable) + a couple of stack-return RVAs;
 * for threads parked in a DLL (OS waits / FMOD) print the module name so they
 * are not mistaken for guest spins. Called twice so the caller can diff which
 * guest thread is genuinely parked (same RIP) vs. still progressing. */
static void dump_threads(const char* label, HMODULE self)
{
    fprintf(stderr, "[WATCHDOG] %s; last HLE call = 0x%08X (%s)\n",
            label, g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    thrdiag_dump();
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;
            SuspendThread(th);
            CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &ctx)) {
                HMODULE m = NULL;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)ctx.Rip, &m);
                char path[MAX_PATH] = "?";
                if (m) GetModuleFileNameA(m, path, sizeof path);
                const char* base = strrchr(path, '\\');
                fprintf(stderr, "[WATCHDOG]   tid %5lu in %s%s\n",
                        (unsigned long)te.th32ThreadID,
                        (m == self) ? "des_boot" : (base ? base + 1 : path),
                        (m == self) ? " (lifted/runtime)" : "");
                /* ALWAYS scan the suspended thread's stack for boot-module return
                 * addresses to reconstruct the lifted/runtime call chain -- even when
                 * the thread is currently parked in a Win32 wait (ntdll). Reveals where
                 * "running" workers (no tracked lv2 wait) are actually blocked.
                 * Some false positives expected (stale stack slots). */
                {
                    /* Bound the scan to the committed stack region containing Rsp --
                     * reading past it (into the guard page / unmapped memory) faults.
                     * VirtualQuery gives the region [base, base+size); scan up from Rsp
                     * (stack grows down, so higher addrs toward the stack base are
                     * committed) but never past region_end. */
                    uint64_t rsp = ctx.Rsp;
                    uint64_t scan_end = rsp + 0x10000;
                    MEMORY_BASIC_INFORMATION mbi;
                    if (VirtualQuery((LPCVOID)rsp, &mbi, sizeof mbi) &&
                        (mbi.State == MEM_COMMIT) &&
                        !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                        uint64_t region_end = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
                        if (region_end < scan_end) scan_end = region_end;
                    } else {
                        scan_end = rsp;   /* Rsp not in a readable committed region: skip */
                    }
                    int found = 0;
                    for (uint64_t a = rsp; a + 8 <= scan_end && found < 12; a += 8) {
                        uint64_t v = *(const uint64_t*)a;
                        if (v < (uint64_t)self) continue;
                        HMODULE mm = NULL;
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCSTR)v, &mm);
                        if (mm == self) {
                            fprintf(stderr, "[WATCHDOG]       ret rva=0x%llX\n",
                                    (unsigned long long)(v - (uint64_t)self));
                            found++;
                        }
                    }
                }
            }
            ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    fflush(stderr);
}

static DWORD WINAPI hang_watchdog(LPVOID)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&hang_watchdog, &self);
    Sleep(8000);
    dump_threads("8s sample", self);
    Sleep(7000);
    dump_threads("15s sample", self);
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
    if (argc < 2) { printf("usage: %s <EBOOT.elf>\n", argv[0]); return 2; }

    thrdiag_set_name("MAIN");

#ifdef _WIN32
    SetUnhandledExceptionFilter(ydkj_crash_filter);
    signal(SIGABRT, ydkj_abort_handler);
    atexit(boot_atexit);                /* catch clean exit() paths */
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
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;   /* full 32-bit space backed -> OOB guard unnecessary */
#else
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
#endif
    if (!vm_base) { printf("vm alloc failed\n"); return 1; }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) { printf("load failed\n"); return 1; }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* real system PRX (libsre) -> guest RAM + exports */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */

    /* EXPERIMENT: the engine (NexusRevolution Main) hammers unresolved sceNp NID
     * 0x36D0C2C5 forever (returns 0 by default) and never advances past pre-title
     * loading. Register a stub returning an env-configurable value (NP_36_RET) so
     * we can probe whether a different return unblocks the loading state. */
    ps3_hle_register_ctx(0x36D0C2C5u, "sceNp_0x36D0C2C5_probe", [](ppu_context* c){
        static long v = -2; if (v == -2) { const char* e = getenv("NP_36_RET"); v = e ? strtol(e,0,0) : 0; }
        static int n = 0; if (n < 4) { n++;
            fprintf(stderr, "[np36] call r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
                    (uint32_t)c->gpr[3], (uint32_t)c->gpr[4], (uint32_t)c->gpr[5], (uint32_t)c->gpr[6]); }
        /* NP_36_WRITE=0xVAL: if r3 looks like a guest pointer, write VAL (BE u32) there --
         * tests the "getter whose out-param we never fill" hypothesis. */
        { static long w = -2; static uint32_t wv = 0;
          if (w == -2) { const char* e = getenv("NP_36_WRITE"); w = e ? 1 : 0; if (e) wv = (uint32_t)strtoul(e,0,0); }
          if (w) { uint32_t p = (uint32_t)c->gpr[3];
              if (p >= 0x10000 && p < 0xE0000000u) { extern void vm_write32(uint64_t,uint32_t); vm_write32(p, wv); } } }
        c->gpr[3] = (uint64_t)(int64_t)v;
    });

    ppu_sysprx_register();   /* boot-critical CRT (sys_initialize_tls, ...) */
    ppu_fs_register();       /* cellFs VFS over the real game directory */
    lv2_init_syscalls();     /* real lv2 syscall table (semaphore/memory/fs/...) */

    /* Install the guest-callback hook and start the synthetic RSX vblank driver
     * so the game's frame loop advances (it no-ops until the game registers its
     * vblank/flip handlers during init). */
    g_ps3_guest_caller = harness_guest_caller;
#ifdef _WIN32
    CreateThread(NULL, 4u * 1024 * 1024, vblank_ticker, NULL, 0, NULL);
    CreateThread(NULL, 0, hang_watchdog, NULL, 0, NULL);
#endif

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n", entry, STACK_TOP);
    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);
    return 0;
}
