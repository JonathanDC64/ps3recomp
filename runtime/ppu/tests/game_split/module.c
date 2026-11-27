/*
 * ps3recomp - the module half of the split test (run.sh, ps3emu/game_module.h).
 *
 * Functions in the shape of lifted ones, built freestanding as a game module
 * is, and the module's one export. host.c calls them through the function
 * table and checks each result. The prototypes of the runtime functions are
 * the test's own: the host's stand-ins return 3 * a + their slot.
 *
 * Some functions are here for what the compiler makes of them: a zeroing and
 * a pattern loop of unknown size (Darwin lowers them to bzero and
 * memset_pattern), a large frame (a stack probe on some targets) and the
 * rounding functions (one instruction each, with SSE4.1 or on arm64). If one
 * of them needs a function the module does not have, the link fails or
 * run.sh's check of the module's imports does.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <math.h>
#include "ps3emu/game_module.h"

unsigned long long vm_read32(unsigned addr);
unsigned long long spu_stop(unsigned code);
unsigned long long spu_indirect_branch(unsigned v);

extern unsigned g_vm_inl_rd_lim;                    /* game_module.c */
extern __thread void (*g_trampoline_fn)(void*);     /* game_module.c */

static unsigned long long t_thunks(unsigned a) { return vm_read32(a) + spu_stop(a + 1); }
static void* t_thunk_addr(void) { return (void*)&spu_indirect_branch; }
static unsigned t_rd_lim(void) { return g_vm_inl_rd_lim; }
static void t_tls_set(void* v) { g_trampoline_fn = (void (*)(void*))v; }
static void* t_tls_get(void) { return (void*)g_trampoline_fn; }

static int t_print(FILE* f)
{
    const int n = fprintf(f, "%d %s %.2f %c %llu|", 42, "str", 1.5, 'x', 1ull << 40);
    fputs("end", f);
    fputc('!', f);
    fwrite("w", 1, 1, f);
    fflush(f);
    return n;
}

static unsigned long t_env(const char* name)
{
    const char* v = getenv(name);
    return (v ? (unsigned long)atoi(v) : 0) + strtoul("ff", 0, 16);
}

static int t_sjlj(int v)
{
    jmp_buf jb;
    volatile int n = 0;
    const int r = setjmp(jb);
    if (r == 0) {
        n = v;
        longjmp(jb, v + 1);
    }
    return n * 100 + r;
}

static void   t_memcpy(void* d, const void* s, size_t n)  { memcpy(d, s, n); }
static void   t_memmove(void* d, const void* s, size_t n) { memmove(d, s, n); }
static void   t_memset(void* d, int c, size_t n)          { memset(d, c, n); }
static int    t_memcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }
static size_t t_strlen(const char* s)                     { return strlen(s); }
static void   t_zero(unsigned char* p, size_t n)          { memset(p, 0, n); }

static void t_pattern(uint32_t* p, size_t n)
{
    for (size_t i = 0; i < n; i++) p[i] = 0x01020304u;
}

static unsigned t_bigframe(unsigned i)
{
    volatile unsigned char big[1u << 18];
    big[i & (sizeof big - 1)] = (unsigned char)i;
    big[0] = 1;
    return big[i & (sizeof big - 1)] + big[0];
}

static double t_math(double x)
{
    return sqrt(x) + floor(x) + trunc(x) + ceil(x) + nearbyint(x) + fma(x, x, x) + ldexp(x, 3) + exp2(x);
}

#define T_(f) { 0, (void*)&f, #f }
const struct { uint64_t addr; void* func; const char* name; } function_table[] = {
    T_(t_thunks), T_(t_thunk_addr), T_(t_rd_lim), T_(t_tls_set), T_(t_tls_get), T_(t_print), T_(t_env),
    T_(t_sjlj), T_(t_memcpy), T_(t_memmove), T_(t_memset), T_(t_memcmp), T_(t_strlen), T_(t_zero),
    T_(t_pattern), T_(t_bigframe), T_(t_math),
};
const uint64_t function_table_count = sizeof function_table / sizeof function_table[0];

__attribute__((visibility("default"))) int game_init(const ps3_host_api* host, ps3_game_api* game)
{
    return ps3_game_module_init(host, game);
}
