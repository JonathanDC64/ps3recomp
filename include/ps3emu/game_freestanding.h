/*
 * ps3recomp - force-included (/FI, -include) into every source of a
 * freestanding game module (PS3_GAME_FREESTANDING).
 *
 * A freestanding module is the game DLL of a split build (game_module.h)
 * compiled with clang's own headers plus the declarations in
 * include/ps3emu/game_libc/ only (clang-cl /X, a hosted compile: not
 * -ffreestanding, which turns inline memcpy and sqrt into calls), and linked
 * with no C runtime (lld-link /nodefaultlib /noentry). The C library functions
 * it calls are jump thunks to the host's C runtime (PS3_GAME_HOST_CRT_FUNCS);
 * runtime/ppu/game_libc.c has the rest.
 *
 * This header sets three things the lifted sources take from the MSVC headers
 * in other builds, before any of those sources' own includes:
 *
 *   - PPU_FENCE without <atomic>. The lowering is the MSVC STL's own for
 *     std::atomic_thread_fence on x64 (its <atomic>, _Atomic_thread_fence):
 *     a compiler barrier, and for seq_cst a locked increment of a stack word
 *     between two. So the fences compile to what they compile to with the STL.
 *   - PPU_THREAD_LOCAL and SPU_THREAD_LOCAL as __thread. clang treats
 *     __declspec(thread) as a thread-local that may need dynamic
 *     initialisation (MSVC 2015 and later allow it), in C as well as C++, and
 *     guards every access to one defined in another file with the CRT's
 *     __tls_guard and __dyn_tls_on_demand_init. __thread has to be constant
 *     initialised, so it gets no guard. The two trampolines are pointers that
 *     start at 0 (game_module.c defines them).
 *   - INT_MIN and the other limits, which the lifted PPU code gets through
 *     <atomic> and <stdlib.h> in other builds.
 *
 * PS3_GAME_FREESTANDING itself also leaves out ps3types.h's C++ part (be_t,
 * vm::ptr, through <type_traits>): the SPU workload table includes it for
 * the C types only.
 *
 * On Linux the module is an ELF shared object, compiled with clang
 * -nostdlibinc (clang's own headers and game_libc/ only) and linked with
 * ld.lld -nostdlib. There this header sets:
 *
 *   - PPU_FENCE as the compiler's __atomic_thread_fence, which is what
 *     libstdc++'s std::atomic_thread_fence and clang's <stdatomic.h> both
 *     are, so the fences compile as in the single Linux exe;
 *   - __cdecl as nothing, for game_libc/'s declarations (a Windows keyword);
 *   - hidden visibility for every name declared or defined after it: see
 *     game_module.h. The module's one export, the title's init, says
 *     visibility("default") itself.
 */
#ifndef PS3EMU_GAME_FREESTANDING_H
#define PS3EMU_GAME_FREESTANDING_H

#ifndef PS3_GAME_FREESTANDING
#define PS3_GAME_FREESTANDING 1
#endif

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#define PPU_THREAD_LOCAL __thread
#define SPU_THREAD_LOCAL __thread

#ifndef _WIN32
#define __cdecl
#pragma GCC visibility push(hidden)
#define PPU_FENCE(o) __atomic_thread_fence(PS3_GFS_ORDER_##o)
#define PS3_GFS_ORDER_relaxed __ATOMIC_RELAXED
#define PS3_GFS_ORDER_consume __ATOMIC_CONSUME
#define PS3_GFS_ORDER_acquire __ATOMIC_ACQUIRE
#define PS3_GFS_ORDER_release __ATOMIC_RELEASE
#define PS3_GFS_ORDER_acq_rel __ATOMIC_ACQ_REL
#define PS3_GFS_ORDER_seq_cst __ATOMIC_SEQ_CST
#else
#include <intrin0.h>   /* clang's: _ReadWriteBarrier (_InterlockedIncrement is a builtin) */

#define PPU_FENCE(o) PS3_GFS_FENCE_##o()
#define PS3_GFS_FENCE_relaxed() ((void)0)
#define PS3_GFS_FENCE_consume() _ReadWriteBarrier()
#define PS3_GFS_FENCE_acquire() _ReadWriteBarrier()
#define PS3_GFS_FENCE_release() _ReadWriteBarrier()
#define PS3_GFS_FENCE_acq_rel() _ReadWriteBarrier()
#define PS3_GFS_FENCE_seq_cst() ps3_gfs_fence_seq_cst()
static __inline void ps3_gfs_fence_seq_cst(void)
{
    volatile long guard;   /* not initialised, as in the STL: only the locked op matters */
    _ReadWriteBarrier();
    (void)_InterlockedIncrement(&guard);
    _ReadWriteBarrier();
}
#endif /* _WIN32 */

#endif /* PS3EMU_GAME_FREESTANDING_H */
