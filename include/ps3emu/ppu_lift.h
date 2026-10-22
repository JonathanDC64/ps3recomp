/*
 * ps3recomp - the fixed part of a PPU lift's header.
 *
 * tools/ppu_lifter.py writes a ppu_recomp.h beside each lift. Most of it is
 * the same for every title: ppu_context, which the lifted code and the
 * runtime hand each other, the guest memory accessors, the syscall entry,
 * the thread-local qualifier, the function table's type and declaration, and
 * the shadow call stack's guard. That part is this header, and this is its
 * only copy. A lift's ppu_recomp.h includes it, then declares the title's own
 * names: a prototype for each lifted function and call target, and the
 * function overrides.
 *
 * The host's sources include this header, not a lift's: the boot scaffold in
 * runtime/ppu/ and a port's own code. They use nothing of a lift's own names,
 * so a host exe compiles with no lift at all (a CI build). A host source that
 * calls a lifted function by name declares it itself, the way a lift's header
 * does: void func_<address>(ppu_context* ctx);
 *
 * DS_SHADOW_STACK: a lift made with --shadow-stack defines it in its
 * ppu_recomp.h, ahead of the include, unless DS_NO_SHADOW_STACK is defined.
 * A host source does not see that line, so its build defines DS_SHADOW_STACK
 * itself when it wants the same DS_FRAME.
 *
 * A lift made before this header carries the same text inline in its
 * ppu_recomp.h, with only #pragma once around it. It needs no new lift: the
 * lifted sources include only that header and the host's sources only this
 * one, so each translation unit has one definition of ppu_context, and the
 * two are the same text. A translation unit must not include both, as the
 * inline copy cannot see this guard. A lift's header written since includes
 * this one, so it and this header can meet in either order.
 *
 * A --symbol-prefix lift (a system module such as libsre, linked beside the
 * title's own lift) carries the text inline too, with function_table and
 * function_table_count renamed to the prefixed names its source defines. So
 * its header is what it always was, and compiles with only its own folder on
 * the include path. ppu_lifter.py cuts that text out of this file at the
 * ppu_lifter:<part>:begin and :end lines below, which is why every
 * declaration and macro here belongs inside one of those parts, and why the
 * marker lines stay as they are (tools/test_ppu_lift_header.py checks both).
 */
#ifndef PS3EMU_PPU_LIFT_H
#define PS3EMU_PPU_LIFT_H

/* ppu_lifter:preamble:begin */
#include <stdint.h>

/* ppu_lifter:fence:begin */
/* Guest memory barriers: the lowering of sync/lwsync/eieio/isync/ptesync.
 *
 * These used to be no-ops, which x86-64 forgave -- its store order is total,
 * so the ordering a guest asked for was already there. A weakly ordered host
 * does not give it away. The gcm command buffer is the shape that breaks
 * first: the SDK writes its command words, separates them with eieio or
 * lwsync, then writes the FIFO put pointer, and with the barrier dropped the
 * pointer can become visible before the words it publishes. Every lock-free
 * handoff in a title has the same exposure.
 *
 * The macro lives in the header so the lifted code, the boot scaffold and the
 * smoke title all spell a fence the same way. On x86-64 the acquire, release
 * and acq_rel forms emit no instruction at all and cost only the reordering
 * the compiler no longer does; only seq_cst becomes a barrier instruction
 * (mfence, or the locked op LLVM prefers to it). On arm64 they are one dmb
 * each, around half a nanosecond. */
#ifndef PPU_FENCE
#  ifdef __cplusplus
#    include <atomic>
#    define PPU_FENCE(o) std::atomic_thread_fence(std::memory_order_##o)
#  else
#    include <stdatomic.h>
#    define PPU_FENCE(o) atomic_thread_fence(memory_order_##o)
#  endif
#endif
/* ppu_lifter:fence:end */

/* 128-bit vector register for VMX/AltiVec */
typedef union {
    uint8_t  b[16];
    uint16_t h[8];
    uint32_t w[4];
    uint64_t d[2];
    float    f[4];
} ppu_vr;

/* Field order and types match runtime/ppu/ppu_context.h exactly, so the
 * same pointer can flow between recompiled code and runtime syscall
 * handlers without layout drift. Keep the two in sync. */
typedef struct ppu_context {
    uint64_t gpr[32];   /* General-purpose registers  */
    double   fpr[32];   /* Floating-point registers   */
#ifdef _MSC_VER
    __declspec(align(16)) ppu_vr vr[32];  /* VMX/AltiVec vector regs */
#else
    ppu_vr   vr[32] __attribute__((aligned(16)));
#endif
    uint8_t  crf[8];    /* Condition register: one byte per field, CR0..CR7 */
    uint64_t lr;        /* Link register               */
    uint64_t ctr;       /* Count register              */
    uint32_t xer;       /* Fixed-point exception reg   */
    uint32_t fpscr;     /* FP status/control register  */
    uint32_t vscr;      /* Vector status/control reg   */
    uint64_t cia;       /* Current instruction address */
    uint64_t thread_id;     /* PPU thread ID              */
    uint64_t reserve_addr;  /* lwarx/stwcx. reservation address */
    uint64_t reserve_value; /* lwarx/stwcx. reservation value   */
    int      reserve_valid; /* lwarx/stwcx. reservation flag    */
} ppu_context;

/* Condition register access for host code (insn hooks, HLE): ppu_cr_get and
 * ppu_cr_set read and write one field (0 = CR0 ... 7 = CR7; LT=8 GT=4 EQ=2
 * SO=1), ppu_cr_pack and ppu_cr_unpack the whole register as the guest sees
 * it (mfcr / mtcr: CR0 is bits 31:28, CR7 bits 3:0). runtime/ppu/ppu_context.h
 * has the same four. */
#ifndef PPU_CR_PACK_DEFINED
#define PPU_CR_PACK_DEFINED
static inline uint8_t ppu_cr_get(const ppu_context* ctx, int field)
{
    return (uint8_t)(ctx->crf[field] & 0xF);
}
static inline void ppu_cr_set(ppu_context* ctx, int field, uint8_t value)
{
    ctx->crf[field] = (uint8_t)(value & 0xF);
}
static inline uint32_t ppu_cr_pack(const ppu_context* ctx)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 4) | (ctx->crf[i] & 0xFu);
    return v;
}
static inline void ppu_cr_unpack(ppu_context* ctx, uint32_t v)
{
    for (int i = 0; i < 8; i++) ctx->crf[i] = (uint8_t)((v >> (28 - 4 * i)) & 0xFu);
}
#endif

/* Memory access helpers (provided by runtime) */
#ifdef __cplusplus
extern "C" {
#endif
uint8_t  vm_read8 (uint64_t addr);
uint16_t vm_read16(uint64_t addr);
uint32_t vm_read32(uint64_t addr);
uint64_t vm_read64(uint64_t addr);
void     vm_write8 (uint64_t addr, uint8_t  val);
void     vm_write16(uint64_t addr, uint16_t val);
void     vm_write32(uint64_t addr, uint32_t val);
void     vm_write64(uint64_t addr, uint64_t val);
/* stwcx./stdcx. store-conditional: atomically store `val` at `addr` iff the raw
 * big-endian guest word still equals `expected` (the value the paired lwarx/ldarx
 * loaded). Returns 1 if the reservation held (store applied), 0 otherwise. This
 * makes lock-free sequences (free-list CAS, refcounts) correct across host
 * threads -- a plain conditional write races and corrupts under real concurrency. */
int      ppu_stwcx32(uint64_t addr, uint32_t expected, uint32_t val);
int      ppu_stdcx64(uint64_t addr, uint64_t expected, uint64_t val);
#ifdef __cplusplus
}
#endif

/* Inline fast paths for the accessors above. The runtime keeps the limits
 * (ppu_loader.cpp): an access at ea is a plain big-endian load or store when
 * ea - 0x10000 < the limit (below the raw-SPU window, above the null page),
 * a load is not the one special word (g_vm_inl_rd_hole), and the line of a
 * store is not SPU-reserved. Anything else, and everything while a limit is 0
 * (a diagnostic is on, or not yet decided), calls the accessor, which also
 * sets the limits. The call was ~20% of Demon's Souls' HighGraphics thread.
 * Only for the lifted sources (their preamble defines PPU_VM_INLINE), not the
 * runtime that defines the accessors; PPU_VM_NO_INLINE always calls. */
#if defined(PPU_VM_INLINE) && !defined(PPU_VM_NO_INLINE)
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t* vm_base;
extern unsigned g_vm_inl_rd_lim, g_vm_inl_wr_lim, g_vm_inl_rd_hole;
extern unsigned char g_spu_coh_bitmap[];
extern struct spu_coh_armed_line_t { int v; } g_spu_coh_armed_line;
#ifdef __cplusplus
}
#endif
static inline int vm_inl_rd(uint32_t ea) { return ea - 0x10000u < g_vm_inl_rd_lim; }
static inline int vm_inl_wr(uint32_t ea)
{
    if (!(ea - 0x10000u < g_vm_inl_wr_lim)) return 0;
    if (!*(volatile int*)&g_spu_coh_armed_line.v) return 1;
    const uint32_t line = ea >> 7;
    return !((g_spu_coh_bitmap[line >> 3] >> (line & 7)) & 1u);
}
static inline uint8_t vm_read8_inl(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (__builtin_expect(vm_inl_rd(ea), 1)) return vm_base[ea];
    return (vm_read8)(a);
}
static inline uint16_t vm_read16_inl(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (__builtin_expect(vm_inl_rd(ea), 1)) {
        uint16_t v; memcpy(&v, vm_base + ea, 2); return __builtin_bswap16(v);
    }
    return (vm_read16)(a);
}
static inline uint32_t vm_read32_inl(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (__builtin_expect(vm_inl_rd(ea) && ea != g_vm_inl_rd_hole, 1)) {
        uint32_t v; memcpy(&v, vm_base + ea, 4); return __builtin_bswap32(v);
    }
    return (vm_read32)(a);
}
static inline uint64_t vm_read64_inl(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (__builtin_expect(vm_inl_rd(ea), 1)) {
        uint64_t v; memcpy(&v, vm_base + ea, 8); return __builtin_bswap64(v);
    }
    return (vm_read64)(a);
}
static inline void vm_write8_inl(uint64_t a, uint8_t val)
{
    const uint32_t ea = (uint32_t)a;
    if (__builtin_expect(vm_inl_wr(ea), 1)) { vm_base[ea] = val; return; }
    (vm_write8)(a, val);
}
static inline void vm_write16_inl(uint64_t a, uint16_t val)
{
    const uint32_t ea = (uint32_t)a;
    if (__builtin_expect(vm_inl_wr(ea), 1)) {
        const uint16_t v = __builtin_bswap16(val); memcpy(vm_base + ea, &v, 2); return;
    }
    (vm_write16)(a, val);
}
static inline void vm_write32_inl(uint64_t a, uint32_t val)
{
    const uint32_t ea = (uint32_t)a;
    if (__builtin_expect(vm_inl_wr(ea), 1)) {
        const uint32_t v = __builtin_bswap32(val); memcpy(vm_base + ea, &v, 4); return;
    }
    (vm_write32)(a, val);
}
static inline void vm_write64_inl(uint64_t a, uint64_t val)
{
    const uint32_t ea = (uint32_t)a;
    if (__builtin_expect(vm_inl_wr(ea), 1)) {
        const uint64_t v = __builtin_bswap64(val); memcpy(vm_base + ea, &v, 8); return;
    }
    (vm_write64)(a, val);
}
#define vm_read8(a)      vm_read8_inl(a)
#define vm_read16(a)     vm_read16_inl(a)
#define vm_read32(a)     vm_read32_inl(a)
#define vm_read64(a)     vm_read64_inl(a)
#define vm_write8(a, v)  vm_write8_inl((a), (v))
#define vm_write16(a, v) vm_write16_inl((a), (v))
#define vm_write32(a, v) vm_write32_inl((a), (v))
#define vm_write64(a, v) vm_write64_inl((a), (v))
#endif

/* Syscall handler */
#ifdef __cplusplus
extern "C"
#endif
void lv2_syscall(ppu_context* ctx);

/* Thread-local storage qualifier. The runtime's boot scaffold declares the
 * same objects the generated code does (g_active_ctx, g_trampoline_fn), so
 * this has to live in the HEADER both sides include -- not in the generated
 * source, where only the lifted code could see it. */
#ifndef PPU_THREAD_LOCAL
#  ifdef _MSC_VER
#    define PPU_THREAD_LOCAL __declspec(thread)
#  else
#    define PPU_THREAD_LOCAL __thread
#  endif
#endif

/* Function table (defined in the generated source, sentinel-terminated).
 * The game project's indirect-call dispatcher builds its lookup from this. */
typedef struct { uint64_t addr; void (*func)(ppu_context*); const char* name; } func_entry;
#ifdef __cplusplus
extern "C" {
#endif
extern const func_entry function_table[];
extern const uint64_t  function_table_count;  /* entries, excluding sentinel */
#ifdef __cplusplus
}
#endif

/* ppu_lifter:preamble:end */

/* The shadow call stack. Every lifted function starts with DS_FRAME(its guest
 * address). With DS_SHADOW_STACK defined, that pushes the address onto a
 * thread-local stack for the length of the call: a guest backtrace where the
 * host's stack walk fails on the tail-call trampolines (ds_dump_shadow prints
 * it). Without it DS_FRAME is nothing. The title defines the stack itself,
 * g_ds_sp and g_ds_stk. */
/* ppu_lifter:shadow_stack:begin */
#ifdef DS_SHADOW_STACK
#if defined(_MSC_VER)
  #define DS_TLS __declspec(thread)
#else
  #define DS_TLS __thread
#endif
extern "C" { extern DS_TLS unsigned g_ds_sp; extern DS_TLS unsigned g_ds_stk[8192]; }
struct DsFrame { DsFrame(unsigned a){ if (g_ds_sp < 8192u) g_ds_stk[g_ds_sp] = a; ++g_ds_sp; }
                 ~DsFrame(){ if (g_ds_sp) --g_ds_sp; } };
#define DS_FRAME(a) DsFrame _dsf((a))
#else
#define DS_FRAME(a) ((void)0)
#endif
/* ppu_lifter:shadow_stack:end */

#endif /* PS3EMU_PPU_LIFT_H */
