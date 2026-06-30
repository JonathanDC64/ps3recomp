/*
 * ps3recomp - PPU HLE bridge (NID -> host function dispatch)
 *
 * Connects the recompiled game's firmware imports to our HLE C libraries.
 * The game calls an imported function through its import stub; the lifter
 * emits `ps3_hle_call(<NID>, ctx)` for those addresses (see ppu_lifter.py
 * --imports). This resolves the NID to a registered HLE handler and marshals
 * the PPC calling convention into a native C call.
 *
 * PPC64 ELFv1 integer/pointer ABI: arguments in r3..r10 (gpr[3..10]), return
 * value in r3. The generic adapter casts the handler to a uint64-in/uint64-out
 * function and passes the 8 GPR argument slots; this covers the large majority
 * of cellXxx APIs (integer args/handles, s32 return). Functions that take or
 * return *pointers* need host<->guest address translation and so require a
 * per-function wrapper -- the generic path passes the raw value through.
 *
 * Compiled as C++ (matches the lifted output). Game-agnostic.
 */
#include "ppu_recomp.h"   /* ppu_context */
#include "ps3emu/nid.h"   /* ps3_nid_table, ps3_nid_entry */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv (HLE trace toggle) */
#ifdef _WIN32
#include <windows.h>  /* RtlCaptureStackBackTrace / GetModuleHandleExA (fatal backtrace) */
#endif

/* Single flat NID -> handler table (all modules share it; resolution is by
 * NID which is globally unique). Sized for the firmware import surface. */
#define HLE_NID_CAP 4096
static ps3_nid_entry  g_hle_storage[HLE_NID_CAP];
static ps3_nid_table  g_hle_nids;
static int            g_hle_inited = 0;

extern "C" void ps3_hle_register(uint32_t nid, const char* name, void* handler)
{
    if (!g_hle_inited) { ps3_nid_table_init(&g_hle_nids, g_hle_storage, HLE_NID_CAP); g_hle_inited = 1; }
    ps3_nid_table_add(&g_hle_nids, nid, name, handler);
}

extern "C" uint32_t ps3_hle_count(void) { return g_hle_inited ? g_hle_nids.count : 0; }

/* Context-aware handlers: functions that need the full ppu_context (to read
 * args beyond the generic ABI, set registers like r13, touch memory, etc.).
 * Registered separately and dispatched before the generic table. */
typedef void (*hle_ctx_fn)(ppu_context*);
#define HLE_CTX_CAP 256
static struct { uint32_t nid; hle_ctx_fn fn; } g_ctx[HLE_CTX_CAP];
static uint32_t g_ctx_count = 0;

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn)
{
    (void)name;
    if (g_ctx_count < HLE_CTX_CAP) { g_ctx[g_ctx_count].nid = nid; g_ctx[g_ctx_count].fn = fn; g_ctx_count++; }
}

/* Generic PPC integer/pointer ABI adapter. */
typedef uint64_t (*hle_generic)(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t, uint64_t);

/* Host VM store (defined in ppu_loader.cpp) — used for the TOC save below. */
void vm_write64(uint64_t addr, uint64_t val);

/* Breadcrumb for the crash reporter: the last firmware import dispatched, so a
 * host AV inside an HLE handler names the culprit NID/function. */
extern "C" uint32_t    g_last_hle_nid  = 0;
extern "C" const char* g_last_hle_name = "";
extern "C" void thrdiag_hle(const char*);   /* thread_diag.c */

/* Real-PRX bridge: a loaded system PRX (libsre = cellSpurs/cellSync) may export
 * this NID. If so, dispatch into the REAL recompiled Sony code (its OPD -> our
 * indirect dispatcher -> the registered lifted libsre function) instead of the
 * HLE stub. prx_resolve_export returns 0 when no PRX exports the NID, so this is
 * a no-op when no PRX is loaded. */
extern "C" uint32_t prx_resolve_export(uint32_t nid);
extern "C" void     ps3_indirect_call(ppu_context* ctx);
extern "C" uint32_t vm_read32(uint64_t a);

extern "C" uint64_t vm_read64(uint64_t a);

extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx)
{
    g_last_hle_nid = nid;

    /* Fatal-path guest backtrace: when the game calls sys_process_exit
     * (nid 0xE6F2C1E7), walk the PPC64 ELFv1 stack (back-chain at 0(sp),
     * saved LR at 16(caller frame)) and print the guest return-address chain
     * so we can map the abort path back to the failing call site. */
    if (nid == 0xE6F2C1E7u) {
        fprintf(stderr, "[bt] sys_process_exit: guest backtrace (cur lr=0x%08X):\n",
                (uint32_t)ctx->lr);
        uint32_t fp = (uint32_t)ctx->gpr[1];
        for (int d = 0; d < 32 && fp; d++) {
            uint32_t lr = (uint32_t)vm_read64(fp + 16);
            uint32_t nx = (uint32_t)vm_read64(fp + 0);
            fprintf(stderr, "[bt]   #%2d fp=0x%08X  lr=0x%08X\n", d, fp, lr);
            if (nx <= fp) break;   /* stack grows down: caller frame is higher */
            fp = nx;
        }
#ifdef _WIN32
        /* Host backtrace: lifted `bl` calls are real nested C calls, so the host
         * stack holds the func_XXXX chain. Print RVAs to symbolize against the
         * PDB (llvm-symbolizer --obj=des_boot.exe <rva...>). */
        {
            void* frames[48];
            unsigned short n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
            HMODULE self = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)&vm_read64, &self);
            for (unsigned short i = 0; i < n; i++) {
                HMODULE m = nullptr;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)frames[i], &m);
                if (m == self)
                    fprintf(stderr, "[bt-host] rva=0x%llX\n",
                            (unsigned long long)((char*)frames[i] - (char*)self));
            }
        }
#endif
        fflush(stderr);
    }
    /* PPC64 ELFv1 cross-module ABI: the caller restores its TOC right after the
     * call with `ld r2, 0x28(r1)`, expecting the import stub to have saved the
     * caller's r2 into that slot. The real .lib.stub trampoline did this; the
     * lifted --hle-stubs body (ps3_hle_call) doesn't, so without this every
     * import call leaves the caller with a garbage r2 -> all later TOC-relative
     * loads (the C++ ctor list, globals, ...) read garbage -> boot corruption. */
    vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);

    /* Trace EVERY hle call (all resolution paths: prx / ctx / generic), not just
     * the generic one. Cached getenv. Resolve nid->name afterward if needed. */
    static int hletrace = -1;
    if (hletrace < 0) hletrace = getenv("YDKJ_HLETRACE") ? 1 : 0;
    if (hletrace)
        fprintf(stderr, "[hle] nid=0x%08X r3=%08X r4=%08X r5=%08X r6=%08X\n", nid,
                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
                (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]);

    /* Real libsre (loaded PRX) takes priority over the HLE stub. */
    {
        uint32_t opd = prx_resolve_export(nid);
        if (opd) {
            if (hletrace) fprintf(stderr, "[hle]   -> prx export\n");
            uint32_t code = vm_read32(opd);
            uint32_t toc  = vm_read32(opd + 4);
            ctx->gpr[2] = toc;            /* libsre's own TOC */
            ctx->ctr    = code;
            ps3_indirect_call(ctx);       /* -> registered lifted libsre fn; r3=ret */
            return;
        }
    }

    for (uint32_t i = 0; i < g_ctx_count; i++)
        if (g_ctx[i].nid == nid) {
            if (hletrace) fprintf(stderr, "[hle]   -> ctx handler\n");
            g_ctx[i].fn(ctx); return;
        }

    ps3_nid_entry* e = g_hle_inited ? ps3_nid_table_find(&g_hle_nids, nid) : nullptr;
    if (!e || !e->handler) {
        static int logged = 0;
        if (logged < 40) { fprintf(stderr, "[hle] unresolved NID 0x%08X\n", nid); logged++; }
        ctx->gpr[3] = 0;   /* CELL_OK-ish so the game keeps going */
        return;
    }
    g_last_hle_name = e->name;
    thrdiag_hle(e->name);
    hle_generic fn = (hle_generic)e->handler;
    int trace = (getenv("YDKJ_HLETRACE") != nullptr);
    if (trace)
        fprintf(stderr, "[hle] %-32s nid=0x%08X r3=%08X r4=%08X r5=%08X\n",
                e->name, nid, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
                (uint32_t)ctx->gpr[5]);
    uint64_t r = fn(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6],
                    ctx->gpr[7], ctx->gpr[8], ctx->gpr[9], ctx->gpr[10]);
    if (trace)
        fprintf(stderr, "[hle] %-32s -> r3=%08X\n", e->name, (uint32_t)r);
    ctx->gpr[3] = r;   /* PPC return value */
}

/* Populated by the generated registration unit (gen_hle_nids.py). Weak so a
 * build without it still links (no HLE registered -> imports log + return 0). */
extern "C" void ppu_hle_register_all(void) __attribute__((weak));
extern "C" void ppu_hle_register_all(void) {}

extern "C" void ppu_hle_init(void) { ppu_hle_register_all(); }
