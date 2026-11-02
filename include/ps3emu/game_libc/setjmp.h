/*
 * ps3recomp - <setjmp.h> for a freestanding game module (PS3_GAME_FREESTANDING,
 * ps3emu/game_freestanding.h).
 *
 * The type as the host's C library defines it (MSVC's header on x64, glibc's on
 * x86-64 Linux), and _setjmp and longjmp, which are jump thunks to the host's
 * (PS3_GAME_HOST_CRT_FUNCS). The lift calls them for the guest setjmp and
 * longjmp (ppu_lifter.py, SJLJ_SUPPORT). The runtime's own longjmp (thread
 * exit, spu_halt) unwinds through the module's frames with their unwind tables,
 * which the compiler emits anyway. On Windows the declaration of _setjmp is the
 * one in clang's <intrin.h>, which includes this file.
 */
#ifndef PS3_GAME_LIBC_SETJMP_H
#define PS3_GAME_LIBC_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
typedef __declspec(align(16)) struct _SETJMP_FLOAT128 {
    unsigned __int64 Part[2];
} SETJMP_FLOAT128;

#define _JBLEN 16
typedef SETJMP_FLOAT128 _JBTYPE;
typedef _JBTYPE jmp_buf[_JBLEN];

int __cdecl _setjmp(jmp_buf);
__declspec(noreturn) void __cdecl longjmp(jmp_buf, int);
#else
/* glibc's, x86-64: the registers, then whether a signal mask was saved and the
 * mask (a sigset_t, 1024 bits). _setjmp saves none. */
typedef struct __jmp_buf_tag {
    long __jmpbuf[8];
    int __mask_was_saved;
    unsigned long __saved_mask[16];
} jmp_buf[1];

int _setjmp(jmp_buf) __attribute__((returns_twice));
void longjmp(jmp_buf, int) __attribute__((noreturn));
#endif

#define setjmp(env) _setjmp(env)

#ifdef __cplusplus
}
#endif

#endif /* PS3_GAME_LIBC_SETJMP_H */
