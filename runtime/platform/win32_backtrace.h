/*
 * ps3recomp - Win32 host-backtrace API on POSIX
 *
 * The PPU boot scaffold is dense with host-side backtrace diagnostics: when a
 * guest store lands somewhere impossible, or an OPD resolves to nothing, the
 * only useful question is which lifted function did it, and the answer is the
 * host call stack at that moment. Those probes are written against
 * RtlCaptureStackBackTrace and GetModuleHandleA, and they account for the
 * single largest block of Windows-only symbols in runtime/ppu/ -- around sixty
 * uses, none of them load-bearing for actually running a game.
 *
 * Fencing every one off behind #ifdef _WIN32 would compile, and would also
 * mean the platform being brought up is the one platform with no way to
 * diagnose it. So, following runtime/platform/win32_compat.h: supply the names
 * on POSIX and let the call sites stand.
 *
 * The mapping is exact enough for what the probes print, which is always an
 * RVA -- a frame address minus the image base -- to be fed back through
 * llvm-symbolizer or addr2line:
 *
 *   RtlCaptureStackBackTrace -> backtrace(3)
 *   GetModuleHandleA(NULL)   -> dladdr(3) on this image, giving its load base
 *   GetModuleHandleExA(..FROM_ADDRESS..) -> dladdr(3) on the queried address
 *
 * A returned HMODULE is therefore a load base, not a Win32 module handle, and
 * is only ever valid to subtract. That is all the scaffold does with it.
 *
 * Kept out of win32_compat.h deliberately. That header is included by library
 * translation units (cellSpurs.c, cellGcmSys.c, spu_channels.c, lv2_register.c)
 * and <execinfo.h> does not exist on musl, so folding these in would put a
 * needless portability floor under the whole runtime for the sake of a
 * debug-only facility the library never calls.
 */
#ifndef PS3RECOMP_WIN32_BACKTRACE_H
#define PS3RECOMP_WIN32_BACKTRACE_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#else /* !_WIN32 */

#include <stddef.h>
#include <stdint.h>   /* uintptr_t, for the function-pointer -> void* cast */

/* backtrace(3) is a glibc/Apple extension; musl ships no <execinfo.h>. Where it
 * is missing the shims stay, but report an empty stack -- the diagnostics then
 * print nothing useful instead of failing to build, which is the right trade
 * for a debug path. */
#if defined(__GLIBC__) || defined(__APPLE__)
#  include <execinfo.h>
#  define PS3_HAVE_BACKTRACE 1
#else
#  define PS3_HAVE_BACKTRACE 0
#endif
#include <dlfcn.h>

#ifndef PS3_WIN32_TYPES_USHORT
#define PS3_WIN32_TYPES_USHORT
typedef unsigned short USHORT;
#endif
typedef void*       HMODULE;
typedef const char* LPCSTR;

/* Only the two flags the scaffold passes. Both are honoured implicitly:
 * dladdr always resolves from an address and never takes a reference. */
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS       0x00000004

/* Load base of the mapped object containing `addr`, or NULL. */
static inline void* ps3__image_base_of(const void* addr)
{
    Dl_info info;
    if (addr && dladdr(addr, &info) && info.dli_fbase)
        return info.dli_fbase;
    return NULL;
}

/* GetModuleHandleA(NULL) means "the image I am running in". The scaffold never
 * asks for any other module, so a named lookup would be dead code; resolving
 * this header's own address gives the right answer and needs no name. */
static inline HMODULE GetModuleHandleA(LPCSTR name)
{
    (void)name;
    return (HMODULE)ps3__image_base_of((const void*)(uintptr_t)&ps3__image_base_of);
}

static inline int GetModuleHandleExA(unsigned long flags, LPCSTR addr,
                                     HMODULE* out)
{
    (void)flags;
    if (!out) return 0;
    *out = (HMODULE)ps3__image_base_of((const void*)addr);
    return *out != NULL;   /* nonzero == success, as on Win32 */
}

/* Win32 counts `skip` from the CALLER of RtlCaptureStackBackTrace; backtrace(3)
 * starts at itself, so one extra frame comes off the top for this shim. */
static inline USHORT RtlCaptureStackBackTrace(unsigned long skip,
                                             unsigned long count,
                                             void** frames,
                                             unsigned long* hash)
{
    if (hash) *hash = 0;
    if (!frames || count == 0) return 0;
#if PS3_HAVE_BACKTRACE
    enum { PS3_BT_CAP = 160 };   /* deepest in-tree request is 62 + skip */
    void* raw[PS3_BT_CAP];
    unsigned long want = skip + count + 1;   /* +1: this frame */
    if (want > PS3_BT_CAP) want = PS3_BT_CAP;

    int got = backtrace(raw, (int)want);
    if (got <= 0) return 0;

    unsigned long first = skip + 1;
    if (first >= (unsigned long)got) return 0;

    unsigned long n = (unsigned long)got - first;
    if (n > count) n = count;
    for (unsigned long i = 0; i < n; i++) frames[i] = raw[first + i];
    return (USHORT)n;
#else
    (void)skip; (void)count;
    return 0;
#endif
}

#endif /* _WIN32 */
#endif /* PS3RECOMP_WIN32_BACKTRACE_H */
