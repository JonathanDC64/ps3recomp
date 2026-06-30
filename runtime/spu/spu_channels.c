/*
 * ps3recomp - SPU channel + indirect-branch runtime glue
 *
 * Implements the externs the SPU lifter (tools/spu_lifter.py) emits:
 *   - spu_rdch / spu_rchcnt / spu_wrch : SPU channel access. MFC channels are
 *     routed to the DMA engine (spu_dma.h); mailboxes, signal notification,
 *     events and the decrementer use the spu_context channel fields.
 *   - spu_indirect_branch : resolves ctx->pc to a lifted spu_func_* via a
 *     registry that generated code populates by calling spu_recomp_register().
 *
 * The MFC engine state is kept per spu_context here (spu_context.h does not
 * embed one), in a small lazily-populated registry.
 */

#include "spu_dma.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv (SPU_DISPATCH_LOG diagnostic) */
#include <setjmp.h>

/* SPU_QLOG diagnostic: -1 = env not yet read, 0 = off, 1 = on. Read lazily in
 * mfc_do_transfer (spu_dma.h) to trace the leaf's command-queue DMA sources. */
int g_spu_qlog = -1;
int g_qlog7_budget = 60;   /* cap image-7 DMA log lines (it transfers a lot) */
int g_qlog7_prov   = 2;    /* cap image-7 bad-EA GPR provenance dumps */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Clean SPU job abort (longjmp). The `br .` halt idiom and other terminal
 * spins can't be escaped by setting a status flag (lifted code never checks
 * it), so spu_halt() longjmps back to spu_run_with_halt() in the dispatcher.
 * -----------------------------------------------------------------------*/
#if defined(_MSC_VER)
#  define SPU_TLS __declspec(thread)
#else
#  define SPU_TLS __thread
#endif
static SPU_TLS jmp_buf s_spu_halt_env;
static SPU_TLS int     s_spu_halt_armed = 0;

void spu_halt(spu_context* ctx)
{
    (void)ctx;
    if (s_spu_halt_armed) { s_spu_halt_armed = 0; longjmp(s_spu_halt_env, 1); }
}

/* Run a lifted SPU entry with a halt landing pad. Returns 1 if the job halted
 * (via spu_halt), 0 if it returned normally. */
int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx)
{
    int halted = 0;
    s_spu_halt_armed = 1;
    if (setjmp(s_spu_halt_env) != 0) halted = 1;   /* came back via longjmp */
    else                             entry(ctx);    /* run the job          */
    s_spu_halt_armed = 0;
    return halted;
}

/* ===========================================================================
 * Per-context MFC engine registry
 * ===========================================================================*/
#define SPU_MAX_CONTEXTS 8

typedef struct {
    spu_context* ctx;
    mfc_engine   mfc;
} spu_mfc_slot;

static spu_mfc_slot s_mfc_slots[SPU_MAX_CONTEXTS];

static mfc_engine* mfc_for(spu_context* ctx)
{
    spu_mfc_slot* free_slot = NULL;
    for (int i = 0; i < SPU_MAX_CONTEXTS; i++) {
        if (s_mfc_slots[i].ctx == ctx)
            return &s_mfc_slots[i].mfc;
        if (!free_slot && s_mfc_slots[i].ctx == NULL)
            free_slot = &s_mfc_slots[i];
    }
    if (free_slot) {
        free_slot->ctx = ctx;
        mfc_engine_init(&free_slot->mfc);
        return &free_slot->mfc;
    }
    /* Out of slots: fall back to a shared engine (correct for single-SPU). */
    static mfc_engine fallback;
    static int fallback_init = 0;
    if (!fallback_init) { mfc_engine_init(&fallback); fallback_init = 1; }
    return &fallback;
}

/* ===========================================================================
 * Atomic reservation (GETLLAR / PUTLLC / PUTLLUC) -- real lock-line semantics
 *
 * Multiple SPU kernel threads (the SPURS workload runtime runs several SPUs on
 * one shared lock-free queue) issue GETLLAR/PUTLLC on the SAME 128-byte lines.
 * Without honoring the reservation, two SPUs both "claim" the same slot and the
 * queue corrupts (observed: the 2nd claim returns garbage [1,1,1,1] -> the SPU
 * traps). PUTLLC must FAIL when the line changed since GETLLAR. We implement the
 * compare-and-swap under one global lock across all SPU host threads.
 * ===========================================================================*/
extern uint8_t* vm_base;

/* Global spinlock guarding all atomic line ops. _InterlockedExchange is a
 * clang-cl/MSVC intrinsic (no runtime library symbol needed). */
#include <intrin.h>
static volatile long g_resv_lock = 0;
static void resv_lock(void)   { while (_InterlockedExchange(&g_resv_lock, 1)) { } }
static void resv_unlock(void) { _InterlockedExchange(&g_resv_lock, 0); }

/* Cross-agent lock-line reservation registry. A SPURS task that getllar's a control
 * line and waits on SPU_EVENT_LR (0x400) only wakes when ANOTHER agent (the PPU, or
 * another SPU's putllc) writes that line. To deliver LR we track which SPU contexts
 * hold a reservation, and the write paths (PPU vm_write*, SPU putllc/putlluc) call
 * spu_reservation_notify_write(ea): any context reserving that 128 B line gets
 * event_status |= SPU_EVENT_LR and its reservation invalidated. g_spu_resv_count is a
 * non-static fast-path gate so the hot PPU vm_write path skips the call when no SPU
 * holds a reservation. */
#define SPU_MAX_RESV 8
static spu_context* s_resv_ctx[SPU_MAX_RESV];
volatile long       g_spu_resv_count = 0;   /* read by the PPU vm_write hook */

static void resv_register(spu_context* ctx)
{
    for (int i = 0; i < SPU_MAX_RESV; i++) if (s_resv_ctx[i] == ctx) return;   /* already */
    for (int i = 0; i < SPU_MAX_RESV; i++)
        if (!s_resv_ctx[i]) { s_resv_ctx[i] = ctx; _InterlockedIncrement(&g_spu_resv_count); return; }
}
static void resv_unregister(spu_context* ctx)
{
    for (int i = 0; i < SPU_MAX_RESV; i++)
        if (s_resv_ctx[i] == ctx) { s_resv_ctx[i] = NULL; _InterlockedDecrement(&g_spu_resv_count); return; }
}

/* Notify reservers that the 128 B line containing `ea` was written -> deliver LR.
 * Must be called WITHOUT g_resv_lock held (it takes it). */
void spu_reservation_notify_write(uint32_t ea)
{
    if (g_spu_resv_count == 0) return;                 /* fast path: no reservations */
    uint32_t line = ea & ~(uint32_t)(MFC_ATOMIC_LINE - 1);
    resv_lock();
    for (int i = 0; i < SPU_MAX_RESV; i++) {
        spu_context* c = s_resv_ctx[i];
        if (c && c->resv_valid && c->resv_ea == line) {
            c->event_status |= SPU_EVENT_LR;           /* RdEventStat masks by event_mask */
            c->resv_valid = 0;
            s_resv_ctx[i] = NULL;
            _InterlockedDecrement(&g_spu_resv_count);
            { const char* e = getenv("SPU_RESV_LOG"); static int n = 0;
              if (e && e[0] != '0' && n < 30) { n++;
                fprintf(stderr, "[resv] LR fired: line 0x%08X written -> image=%d woken\n",
                        line, c->image_id); fflush(stderr); } }
        }
    }
    resv_unlock();
}

/* Returns 1 if `cmd` is an atomic line op and was handled here, else 0. */
static int spu_mfc_atomic(spu_context* ctx, uint32_t cmd)
{
    uint32_t ea  = ctx->mfc_eal & ~(uint32_t)(MFC_ATOMIC_LINE - 1);
    uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK;
    uint8_t* ls  = &ctx->ls[lsa];
    uint8_t* mem = vm_base + ea;

    switch (cmd) {
    case MFC_GETLLAR_CMD:
        /* Diagnostic (SPU_RESV_LOG): which main-memory line the coordinator watches. */
        { const char* e = getenv("SPU_RESV_LOG");
          if (e && e[0] != '0') { static uint32_t last = 0; static int n = 0;
            if (ea != last && n < 30) { last = ea; n++;
              const uint8_t* m = vm_base + ea;
              fprintf(stderr, "[resv] image=%d GETLLAR line=0x%08X content:", ctx->image_id, ea);
              for (int b = 0; b < 16; b++) fprintf(stderr, " %02X", m[b]);
              /* LS queue pointers that drive the getllar EA computation */
              fprintf(stderr, " | LS[0x13400]:");
              for (int b = 0; b < 16; b++) fprintf(stderr, " %02X", ctx->ls[0x13400 + b]);
              fprintf(stderr, " LS[0x13480]:");
              for (int b = 0; b < 16; b++) fprintf(stderr, " %02X", ctx->ls[0x13480 + b]);
              fprintf(stderr, "\n"); fflush(stderr); } } }
        resv_lock();
        memcpy(ls, mem, MFC_ATOMIC_LINE);              /* line -> local store */
        memcpy(ctx->resv_line, mem, MFC_ATOMIC_LINE);  /* snapshot for compare */
        ctx->resv_ea = ea; ctx->resv_valid = 1; ctx->atomic_stat = 0;
        resv_register(ctx);                            /* track for LR delivery */
        resv_unlock();
        return 1;

    case MFC_PUTLLC_CMD: {
        int committed;
        resv_lock();
        if (ctx->resv_valid && ctx->resv_ea == ea &&
            memcmp(mem, ctx->resv_line, MFC_ATOMIC_LINE) == 0) {
            memcpy(mem, ls, MFC_ATOMIC_LINE);          /* commit local store */
            ctx->atomic_stat = 0;                      /* PUTLLC_SUCCESS */
            committed = 1;
        } else {
            ctx->atomic_stat = 1;                      /* PUTLLC_FAILURE -> retry */
            committed = 0;
        }
        ctx->resv_valid = 0;                           /* reservation consumed */
        resv_unregister(ctx);
        resv_unlock();
        if (committed) spu_reservation_notify_write(ea);  /* lost others' reservations */
        return 1;
    }

    case MFC_PUTLLUC_CMD:
    case MFC_PUTQLLUC_CMD:
        resv_lock();
        memcpy(mem, ls, MFC_ATOMIC_LINE);              /* unconditional store */
        ctx->resv_valid = 0; ctx->atomic_stat = 0;
        resv_unregister(ctx);
        resv_unlock();
        spu_reservation_notify_write(ea);              /* lost others' reservations */
        return 1;

    default:
        return 0;
    }
}

static int channel_is_mfc(uint32_t ch)
{
    switch (ch) {
    case MFC_WrMSSyncReq: case MFC_RdTagMask:  case MFC_LSA:
    case MFC_EAH:         case MFC_EAL:         case MFC_Size:
    case MFC_TagID:       case MFC_Cmd:         case MFC_WrTagMask:
    case MFC_WrTagUpdate: case MFC_RdTagStat:   case MFC_RdListStallStat:
    case MFC_WrListStallAck: case MFC_RdAtomicStat:
        return 1;
    default:
        return 0;
    }
}

/* ===========================================================================
 * Channel write
 * ===========================================================================*/
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    uint32_t v = value._u32[0];  /* channel writes use the preferred slot */

    if (channel_is_mfc(channel)) {
        /* Atomic line ops (GETLLAR/PUTLLC/...) need real reservation semantics,
         * not the plain GET/PUT the DMA engine would do. */
        if (channel == MFC_Cmd && spu_mfc_atomic(ctx, v))
            return;
        mfc_channel_write(mfc_for(ctx), ctx, channel, v);
        return;
    }

    switch (channel) {
    case SPU_WrOutMbox:      spu_channel_write(&ctx->ch_out_mbox, v);       break;
    case SPU_WrOutIntrMbox:  spu_channel_write(&ctx->ch_out_intr_mbox, v);  break;
    case SPU_WrDec:          ctx->decrementer = v;                          break;
    case SPU_WrEventMask:    ctx->event_mask = v;                           break;
    case SPU_WrEventAck:     ctx->event_status &= ~v;                       break;
    case SPU_WrSRR0:         ctx->srr0 = v;                                 break;
    default:
        /* Unknown / unhandled channel write -- ignore (matches a no-op SPU). */
        break;
    }
}

/* ===========================================================================
 * Channel read (returns value in the preferred word slot)
 * ===========================================================================*/
u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    uint32_t v = 0;
    { static uint32_t last = 0xFFFFFFFFu, n = 0;
      if (channel == last) { if (++n == 4000000u) {
          fprintf(stderr, "[SPU] spinning on rdch channel %u (img-task waiting for kernel feed)\n", channel);
          n = 0; } } else { last = channel; n = 0; } }

    if (channel_is_mfc(channel)) {
        v = mfc_channel_read(mfc_for(ctx), ctx, channel);
        return spu_make_preferred_u32(v);
    }

    switch (channel) {
    case SPU_RdInMbox:      v = spu_channel_read(&ctx->ch_in_mbox);     break;
    case SPU_RdSigNotify1:  v = spu_channel_read(&ctx->ch_sig_notify[0]); break;
    case SPU_RdSigNotify2:  v = spu_channel_read(&ctx->ch_sig_notify[1]); break;
    case SPU_RdDec:         v = ctx->decrementer;                       break;
    case SPU_RdEventMask:   v = ctx->event_mask;                        break;
    case SPU_RdEventStat:   v = ctx->event_status & ctx->event_mask;    break;
    case SPU_RdMachStat:    v = (ctx->status == SPU_STATUS_RUNNING) ? 1 : 0; break;
    case SPU_RdSRR0:        v = ctx->srr0;                              break;
    default:
        v = 0;
        break;
    }
    return spu_make_preferred_u32(v);
}

/* ===========================================================================
 * Channel count (rchcnt) -- how many entries can be read/written right now
 * ===========================================================================*/
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel)
{
    /* Spin detector: a SPURS task busy-polling a channel count (e.g. waiting for
     * the SPURS kernel to push work to SPU_RdInMbox / a signal) reads the same
     * channel millions of times with no progress. Log it so we know what the job
     * is waiting on (the kernel work-feed our HLE doesn't yet provide). */
    { static uint32_t last = 0xFFFFFFFFu, n = 0;
      if (channel == last) { if (++n == 4000000u) {
          fprintf(stderr, "[SPU] spinning on rchcnt channel %u (img-task waiting for kernel feed)\n", channel);
          n = 0; } } else { last = channel; n = 0; } }
    switch (channel) {
    case SPU_RdInMbox:       return ctx->ch_in_mbox.count;                 /* readable */
    case SPU_WrOutMbox:      return SPU_MBOX_DEPTH - ctx->ch_out_mbox.count; /* free slots */
    case SPU_WrOutIntrMbox:  return SPU_INTR_MBOX_DEPTH - ctx->ch_out_intr_mbox.count;
    case SPU_RdSigNotify1:   return ctx->ch_sig_notify[0].count;
    case SPU_RdSigNotify2:   return ctx->ch_sig_notify[1].count;
    case MFC_Cmd:            return MFC_QUEUE_DEPTH - mfc_for(ctx)->queue_count;
    case MFC_RdTagStat:      return 1;  /* synchronous: status always ready */
    default:                 return 1;  /* default: channel ready */
    }
}

/* ===========================================================================
 * Indirect-branch dispatch + function registry
 * ===========================================================================*/
typedef void (*spu_fn)(spu_context*);

typedef struct {
    uint32_t addr;
    spu_fn   fn;
    int      image_id;   /* which recompiled image this function belongs to */
} spu_reg_entry;

#define SPU_FN_REGISTRY_MAX 65536
static spu_reg_entry s_registry[SPU_FN_REGISTRY_MAX];
static uint32_t s_registry_count = 0;

/* Image currently being registered. SPURS images (kernel/policy/job) overlap in
 * LS, so each registers under a distinct id via spu_begin_image() before calling
 * its (prefixed) spu_recomp_register(). Single-image callers leave it 0. */
static int s_reg_image = 0;
void spu_begin_image(int image_id) { s_reg_image = image_id; }

void spu_register_function(uint32_t addr, spu_fn fn)
{
    if (s_registry_count < SPU_FN_REGISTRY_MAX) {
        s_registry[s_registry_count].addr = addr;
        s_registry[s_registry_count].fn = fn;
        s_registry[s_registry_count].image_id = s_reg_image;
        s_registry_count++;
    }
}

static spu_fn spu_lookup(uint32_t addr, int image_id)
{
    /* Linear scan is fine for the small per-image tables. Match the context's
     * active image; image_id 0 (context or entry) matches any, for back-compat
     * with single-image contexts. */
    for (uint32_t i = 0; i < s_registry_count; i++)
        if (s_registry[i].addr == addr &&
            (image_id == 0 || s_registry[i].image_id == 0 ||
             s_registry[i].image_id == image_id))
            return s_registry[i].fn;
    return NULL;
}

/* spu_indirect_branch lifts a `bisl`/`bi rX` to a real nested host C call
 * (fn(ctx)). If a guest TAIL-call or a loop-back `bi` is mis-lifted as a CALL
 * (instead of a jump/goto), each "iteration" nests another host frame and the
 * stack grows without bound -> the whole process dies on stack overflow with no
 * log. Guard the nesting depth: a chain thousands deep is always such a bug, so
 * pin the recursive target (LS addr + link reg) and halt the job cleanly
 * (longjmp out) instead of crashing the process. */
static SPU_TLS int s_ind_depth = 0;

/* HLE of the taskset Policy Module's task-syscall entry (CELL_SPURS_TASKSET_PM_SYSCALL_ADDR
 * = LS 0xA70). A SPURS task branches here (via the SpursTasksetContext syscallAddr that
 * spurs_pm_build_context plants) to perform a task syscall. RPCS3 runs the real PM code
 * here; we don't have it resident, so we HLE: read syscallNum (r3 preferred slot) + args
 * (r4), act, and either resume the task (return -> the lifted caller continues at its link)
 * or end it (EXIT -> halt; the post-run exit-code write + completion then fire). The 0x10
 * bit selects the "2" variant (no implicit wait); we mask it off for the dispatch. */
static void spu_spurs_taskset_syscall(spu_context* ctx)
{
    uint32_t raw  = ctx->gpr[3]._u32[0];
    uint32_t args = ctx->gpr[4]._u32[0];
    uint32_t num  = raw & 0x0F;
    fprintf(stderr, "[spu] SPURS taskset syscall num=%u (raw=0x%X args=0x%08X) image=%d "
            "link/r0=0x%05X\n", num, raw, args, ctx->image_id, ctx->gpr[0]._u32[0] & SPU_LS_MASK);
    fflush(stderr);
    switch (num) {
    case 0: /* CELL_SPURS_TASK_SYSCALL_EXIT: the task is done. Mark a clean `stop 0` and
             * halt; spu_run_lifted_job_abi's post-run path writes the exit code + the
             * completion handler runs. */
        ctx->stop_code = 0;
        ctx->status = SPU_STATUS_STOPPED_BY_STOP;
        spu_halt(ctx);          /* longjmp out to spu_run_with_halt */
        return;
    default: /* YIELD(1)/WAIT_SIGNAL(2)/POLL(3)/RECV_WKL_FLAG(4): for our single-task model
              * report success and resume (return -> lifted caller continues at its link).
              * Refine per-syscall (e.g. real WAIT_SIGNAL blocking) when a task needs it. */
        ctx->gpr[3]._u32[0] = 0;
        return;
    }
}

void spu_indirect_branch(spu_context* ctx)
{
    /* Taskset PM task-syscall entry (LS 0xA70): HLE it instead of branching into
     * (absent) PM code. The task reaches here via the context syscallAddr. */
    if ((ctx->pc & SPU_LS_MASK) == 0xA70u) { spu_spurs_taskset_syscall(ctx); return; }

    /* Diagnostic (SPU_DISPATCH_LOG): trace the SPURS command dispatcher's handler
     * jumps (image 2, target in the 0x4804..0x5098 handler-table range) so we can
     * see whether the task processes a finite stream of real commands or loops on
     * garbage from an un-set-up queue. Capped so it can't flood. */
    if (ctx->image_id == 2 && ctx->pc >= 0x4804 && ctx->pc < 0x5098) {
        static int dl = 0;
        const char* e = getenv("SPU_DISPATCH_LOG");
        if (e && e[0] != '0' && dl < 80) {
            dl++;
            uint32_t qhead = *(uint32_t*)&ctx->ls[0x13400];
            uint32_t qtail = *(uint32_t*)&ctx->ls[0x13480];
            fprintf(stderr, "[spu_dispatch] #%d handler=0x%05X head@13400=0x%08X tail@13480=0x%08X\n",
                    dl, ctx->pc & SPU_LS_MASK, qhead, qtail);
            fflush(stderr);
        }
    }
    spu_fn fn = spu_lookup(ctx->pc, ctx->image_id);
    if (fn) {
        if (++s_ind_depth > 6000) {
            static int reported = 0;
            if (!reported) {
                reported = 1;
                fprintf(stderr, "[SPU] *** indirect-branch recursion depth %d at LS 0x%05X "
                        "(image %d, link/r0=0x%05X) -- a tail-call/loop `bi` lifted as a CALL; "
                        "halting job ***\n", s_ind_depth, ctx->pc & SPU_LS_MASK, ctx->image_id,
                        ctx->gpr[0]._u32[0] & SPU_LS_MASK);
                fflush(stderr);
            }
            s_ind_depth--;
            ctx->status = SPU_STATUS_STOPPED_BY_HALT;
            spu_halt(ctx);   /* longjmp back to spu_run_with_halt */
            return;
        }
        fn(ctx);
        s_ind_depth--;
        return;
    }
    fprintf(stderr, "[SPU] indirect branch to unknown LS address 0x%05X (image %d) "
            "link/r0=0x%05X r1(sp)=0x%05X r2=0x%08X r3=0x%08X r80=0x%08X\n",
            ctx->pc & SPU_LS_MASK, ctx->image_id, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
            ctx->gpr[1]._u32[0] & SPU_LS_MASK, ctx->gpr[2]._u32[0], ctx->gpr[3]._u32[0],
            ctx->gpr[80]._u32[0]);
    fflush(stderr);
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
}

/* ===========================================================================
 * Execution trace (for §3 validation: diff vs RPCS3 SPU interpreter)
 *
 * When the lifter is invoked with --trace, every emitted instruction is
 * surrounded by spu_trace_pc(ctx, PC) before execution and spu_trace_rt(
 * ctx, RT) after, for instructions whose destination is the rt slot. The
 * output is one line per event:
 *
 *     <PC-5hex>                          - PC about to execute
 *       r<rt> <hi-64hex> <lo-64hex>      - register written, post-state
 *
 * Direct to stderr by default; call spu_trace_init(path) once at startup
 * to redirect to a file. The format is intentionally minimal and stable
 * so a small converter can line it up against an RPCS3.log SPU trace.
 * ===========================================================================*/
static FILE* s_trace_fp = NULL;

void spu_trace_init(const char* path)
{
    if (!path || !*path) { s_trace_fp = stderr; return; }
    s_trace_fp = fopen(path, "w");
    if (!s_trace_fp) s_trace_fp = stderr;
}

void spu_trace_pc(spu_context* ctx, uint32_t pc)
{
    (void)ctx;
    if (!s_trace_fp) s_trace_fp = stderr;
    fprintf(s_trace_fp, "%05X\n", pc & SPU_LS_MASK);
}

void spu_trace_rt(spu_context* ctx, uint32_t rt)
{
    if (!s_trace_fp) s_trace_fp = stderr;
    u128 v = ctx->gpr[rt & 0x7F];
    fprintf(s_trace_fp, "  r%-3u %016llX %016llX\n",
            (unsigned)(rt & 0x7F),
            (unsigned long long)v._u64[0],
            (unsigned long long)v._u64[1]);
}

#ifdef __cplusplus
}
#endif
