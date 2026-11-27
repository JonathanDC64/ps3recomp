/*
 * ps3recomp - the C runtime pieces a freestanding game module defines itself
 * (PS3_GAME_FREESTANDING, ps3emu/game_freestanding.h).
 *
 * A module linked with lld-link /nodefaultlib /noentry has no C runtime. The
 * library functions its sources call by name are jump thunks to the host's C
 * runtime (PS3_GAME_HOST_CRT_FUNCS, game_module.c). This file has the rest:
 *
 *   - memcpy, memmove, memset, memcmp and strlen, which the compiler may call
 *     on its own for a large or variable-size copy. no_builtin keeps it from
 *     turning their bodies back into calls to themselves;
 *   - _fltused, which every object that uses floating point references;
 *   - the run of the static constructors, which the CRT's DllMain does in a
 *     module with a C runtime;
 *   - the TLS directory, which the CRT's tlssup supplies to a module with
 *     thread-locals (here the two trampolines in game_module.c): _tls_used,
 *     _tls_index and the two ends of the template. The compiler puts the
 *     thread-locals in .tls$; the linker sorts .tls, .tls$ and .tls$ZZZ in
 *     that order and merges them into one .tls section. The loader writes the
 *     module's TLS slot into _tls_index. It starts at PS3_GAME_TLS_INDEX_UNSET,
 *     so ps3_game_module_init refuses a module whose directory the loader did
 *     not process, instead of letting the trampolines alias another module's
 *     thread-locals (slot 0 is the exe's);
 *   - one import, kernel32's GetCurrentThreadId, never called. Windows sets
 *     up a DLL's TLS only when the image has an import table: the same DLL
 *     with no import keeps _tls_index unset, entry point or not (measured on
 *     Windows 11, 10-22). The import library comes from
 *     game_libc_kernel32.def, so the link needs no Windows SDK.
 *
 * On Linux (an ELF shared object linked with ld.lld -nostdlib) only the
 * memory and string functions are needed. Floating point needs no marker, the
 * dynamic loader sets up the module's thread-locals itself, and there is no
 * TLS directory or import to add. Static constructors are the one difference
 * in kind: the dynamic loader runs a module's .init_array at the dlopen,
 * before ps3_game_module_init fills the C library slots, so one that called
 * the C library would crash there. The lift has none (docs/84 measured
 * Windows' objects), so ps3_game_libc_run_ctors has nothing to run.
 *
 * On arm64 macOS (a Mach-O dylib linked with ld64.lld -nostdlib) the same as
 * on Linux, with the copies written as plain loops (there is no rep movsb),
 * and memset_pattern16, which the compiler calls there on its own as well.
 * dyld runs the module's initialisers at the dlopen, as Linux's loader does.
 * Mach-O names a thread-local through a descriptor that points at dyld's
 * __tlv_bootstrap until dyld fills it in, so the link reads
 * game_libc_system.tbd: a stub of libSystem with that one name, so it needs
 * no SDK either.
 *
 * Compiled only into a freestanding module; elsewhere it is empty.
 */
#if defined(PS3_GAME_FREESTANDING) && \
    (defined(_WIN64) || (defined(__x86_64__) && defined(__ELF__)) || (defined(__aarch64__) && defined(__APPLE__)))
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "ps3emu/game_module.h"

/* --- Memory and strings ------------------------------------------------- */
#define PS3_GL_FN __attribute__((no_builtin))

#if defined(__x86_64__) || defined(_M_X64)
PS3_GL_FN void* memcpy(void* dst, const void* src, size_t n)
{
    void* d = dst;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dst;
}

PS3_GL_FN void* memmove(void* dst, const void* src, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (n == 0 || d == s) return dst;
    if ((uintptr_t)d - (uintptr_t)s >= n) {
        /* d below s, or at or past its end: a forward copy never reads a byte
         * it has already written. */
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {
        /* d inside [s, s + n): backwards, from the last byte. The direction
         * flag is clear again before the return, as the ABI requires. */
        d += n - 1;
        s += n - 1;
        __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
    return dst;
}

PS3_GL_FN void* memset(void* dst, int c, size_t n)
{
    void* d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}
#else
/* arm64: 16 bytes a step as two unaligned 8-byte words, both read before
 * either is written, then single bytes. */
typedef uint64_t __attribute__((aligned(1), may_alias)) ps3_gl_u64u;

PS3_GL_FN void* memcpy(void* dst, const void* src, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (; n >= 16; n -= 16, d += 16, s += 16) {
        const uint64_t a = ((const ps3_gl_u64u*)s)[0], b = ((const ps3_gl_u64u*)s)[1];
        ((ps3_gl_u64u*)d)[0] = a;
        ((ps3_gl_u64u*)d)[1] = b;
    }
    for (; n; n--) *d++ = *s++;
    return dst;
}

PS3_GL_FN void* memmove(void* dst, const void* src, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (n == 0 || d == s) return dst;
    if ((uintptr_t)d - (uintptr_t)s >= n) {
        /* d below s, or at or past its end: forwards. A step reads its 16
         * bytes before it writes, so it never reads a byte already written. */
        for (; n >= 16; n -= 16, d += 16, s += 16) {
            const uint64_t a = ((const ps3_gl_u64u*)s)[0], b = ((const ps3_gl_u64u*)s)[1];
            ((ps3_gl_u64u*)d)[0] = a;
            ((ps3_gl_u64u*)d)[1] = b;
        }
        for (; n; n--) *d++ = *s++;
    } else {
        /* d inside [s, s + n): backwards, from the end. */
        d += n;
        s += n;
        for (; n >= 16; n -= 16) {
            d -= 16;
            s -= 16;
            const uint64_t a = ((const ps3_gl_u64u*)s)[0], b = ((const ps3_gl_u64u*)s)[1];
            ((ps3_gl_u64u*)d)[0] = a;
            ((ps3_gl_u64u*)d)[1] = b;
        }
        for (; n; n--) *--d = *--s;
    }
    return dst;
}

PS3_GL_FN void* memset(void* dst, int c, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    const uint64_t v = 0x0101010101010101ull * (unsigned char)c;
    for (; n >= 16; n -= 16, d += 16) {
        ((ps3_gl_u64u*)d)[0] = v;
        ((ps3_gl_u64u*)d)[1] = v;
    }
    for (; n; n--) *d++ = (unsigned char)c;
    return dst;
}

#if defined(__APPLE__)
/* Darwin's: the compiler turns a loop that stores a repeating pattern of 16
 * bytes (or a shorter one, repeated to 16) into a call to this, and the lift
 * has some hundreds. n is in bytes; the last copy of the pattern may be cut
 * short. */
PS3_GL_FN void memset_pattern16(void* dst, const void* pattern, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    const uint64_t a = ((const ps3_gl_u64u*)pattern)[0], b = ((const ps3_gl_u64u*)pattern)[1];
    for (; n >= 16; n -= 16, d += 16) {
        ((ps3_gl_u64u*)d)[0] = a;
        ((ps3_gl_u64u*)d)[1] = b;
    }
    const unsigned char* p = (const unsigned char*)pattern;
    for (size_t i = 0; i < n; i++) d[i] = p[i];
}
#endif
#endif /* x86-64, arm64 */

PS3_GL_FN int memcmp(const void* a, const void* b, size_t n)
{
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++)
        if (p[i] != q[i]) return p[i] < q[i] ? -1 : 1;
    return 0;
}

PS3_GL_FN size_t strlen(const char* s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

#ifndef _WIN64
/* ELF and Mach-O: the dynamic loader runs any static constructor itself (see the top). */
int ps3_game_libc_run_ctors(void)
{
    return 0;
}
#else
/* --- Floating point ------------------------------------------------------ */
int _fltused = 0x9875;   /* the CRT's value; nothing reads it */

/* --- Static constructors ---------------------------------------------------- */
/* The compiler lists a C++ source's dynamic initialisers in .CRT$XCU, and the
 * linker sorts .CRT$XCA, .CRT$XCU and .CRT$XCZ in that order. With /noentry
 * no C runtime runs them (milestone 1's DLL did, in the CRT's DllMain). The
 * lift has none today (docs/84); ps3_game_module_init runs any there are,
 * once the C library slots are filled. A destructor would need atexit,
 * which is not in the list, so such a source fails to link instead. */
typedef void (__cdecl* ps3_gl_ctor)(void);
#pragma section(".CRT$XCA", read)
#pragma section(".CRT$XCZ", read)
__declspec(allocate(".CRT$XCA")) const ps3_gl_ctor ps3_gl_xc_a = 0;
__declspec(allocate(".CRT$XCZ")) const ps3_gl_ctor ps3_gl_xc_z = 0;

int ps3_game_libc_run_ctors(void)
{
    const ps3_gl_ctor* p = &ps3_gl_xc_a;
    const ps3_gl_ctor* end = &ps3_gl_xc_z;
    __asm__("" : "+r"(p), "+r"(end));   /* one run the linker laid out, not two objects */
    int n = 0;
    for (p++; p < end; p++) {
        if (*p) {   /* the linker may pad between the parts with zeros */
            (*p)();
            n++;
        }
    }
    return n;
}

/* --- The TLS directory ------------------------------------------------------ */
unsigned int _tls_index = PS3_GAME_TLS_INDEX_UNSET;
__attribute__((section(".tls")))     char _tls_start = 0;
__attribute__((section(".tls$ZZZ"))) char _tls_end   = 0;
static const void* const s_tls_callbacks[1] = { 0 };   /* none: no TLS initialiser runs */

/* IMAGE_TLS_DIRECTORY64. lld-link points the image's TLS data directory at the
 * symbol _tls_used, as link.exe does. */
typedef struct ps3_gl_tls_directory {
    const void* start;            /* StartAddressOfRawData */
    const void* end;              /* EndAddressOfRawData */
    const void* index;            /* AddressOfIndex */
    const void* callbacks;        /* AddressOfCallBacks: a zero-terminated list */
    uint32_t    zero_fill;        /* SizeOfZeroFill */
    uint32_t    characteristics;  /* the linker sets the alignment */
} ps3_gl_tls_directory;
__attribute__((section(".rdata$T")))
const ps3_gl_tls_directory _tls_used = { &_tls_start, &_tls_end, &_tls_index, s_tls_callbacks, 0, 0 };
/* Nothing in the code refers to it; keep it through /OPT:REF. */
#pragma comment(linker, "/include:_tls_used")
/* The import that makes the loader set up the TLS (see the top). */
#pragma comment(linker, "/include:__imp_GetCurrentThreadId")
#endif /* _WIN64 */

#endif /* PS3_GAME_FREESTANDING, Windows x64, x86-64 ELF or arm64 Mach-O */
