/*
 * ps3recomp - <setjmp.h> for a freestanding game module (PS3_GAME_FREESTANDING,
 * ps3emu/game_freestanding.h).
 *
 * Only the type, as MSVC's header defines it on x64, for the declaration of
 * _setjmp in clang's <intrin.h>. The module never calls setjmp or longjmp: the
 * runtime's longjmp (thread exit, spu_halt) unwinds through the module's
 * frames with their unwind tables, which the compiler emits anyway.
 */
#ifndef PS3_GAME_LIBC_SETJMP_H
#define PS3_GAME_LIBC_SETJMP_H

typedef __declspec(align(16)) struct _SETJMP_FLOAT128 {
    unsigned __int64 Part[2];
} SETJMP_FLOAT128;

#define _JBLEN 16
typedef SETJMP_FLOAT128 _JBTYPE;
typedef _JBTYPE jmp_buf[_JBLEN];

#endif /* PS3_GAME_LIBC_SETJMP_H */
