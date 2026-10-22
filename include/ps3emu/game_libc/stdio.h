/*
 * ps3recomp - <stdio.h> for a freestanding game module (PS3_GAME_FREESTANDING,
 * ps3emu/game_freestanding.h).
 *
 * Declarations only. Each function is a jump thunk in the module to the host's
 * C runtime (PS3_GAME_HOST_CRT_FUNCS in ps3emu/game_module.h), so a FILE* here
 * is the host's own. Only the functions in that list are declared: a call to
 * any other one fails to compile instead of failing to link. fputc, fputs and
 * fwrite are there because the compiler rewrites some fprintf calls into them.
 */
#ifndef PS3_GAME_LIBC_STDIO_H
#define PS3_GAME_LIBC_STDIO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _iobuf FILE;

#ifndef EOF
#define EOF (-1)
#endif

FILE*  __cdecl __acrt_iob_func(unsigned index);
#define stdin  (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))

int    __cdecl fprintf(FILE* stream, const char* format, ...);
int    __cdecl fputc(int c, FILE* stream);
int    __cdecl fputs(const char* s, FILE* stream);
size_t __cdecl fwrite(const void* buffer, size_t size, size_t count, FILE* stream);
int    __cdecl fflush(FILE* stream);

#ifdef __cplusplus
}
#endif

#endif /* PS3_GAME_LIBC_STDIO_H */
