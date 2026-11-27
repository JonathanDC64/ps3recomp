/*
 * ps3recomp - the host half of the split test (run.sh, ps3emu/game_module.h).
 *
 * Built as a split host is (PS3_GAME_SPLIT, ps3emu/game_split.h force-
 * included) with runtime/ppu/game_host.c, and with stand-ins for the runtime:
 * each function the lifted code calls returns 3 * a + its slot, and the
 * globals game_host.c moves are defined here as the runtime defines them. It
 * loads the module beside it (game.dylib or game.so), as des_host does, and
 * checks what crosses the split:
 *
 *   - the jump thunks, and the thunk address the host compares function
 *     pointers with;
 *   - a global the host sets before the load and through its pointer after;
 *   - the module's thread-locals as the host reaches them (the trampolines),
 *     on two threads at once;
 *   - the C library calls into the host's (fprintf's variable arguments
 *     among them), and the guest setjmp and longjmp through their thunks;
 *   - the module's own memory functions against the host's, overlaps both
 *     ways and every small size, and what the compiler lowers to calls.
 */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/game_module.h"

/* --- The runtime, stood in for ---------------------------------------------- */
#define HOST_FN_(n) unsigned long long n(unsigned a) { return 3ull * a + PS3_GH_##n; }
PS3_GAME_HOST_FUNCS(HOST_FN_)

uint8_t*    vm_base;
const void* g_extra_symtab;
uint64_t    g_extra_symtab_count;
PS3_SPLIT_DEFINE(unsigned, g_vm_inl_rd_lim, 0);
PS3_SPLIT_DEFINE(unsigned, g_vm_inl_wr_lim, 0);
PS3_SPLIT_DEFINE(unsigned, g_vm_inl_rd_hole, 0);
PS3_SPLIT_DEFINE(volatile int, g_spu_goto_slow, 1);
PS3_SPLIT_DEFINE(volatile int, g_yz_lockstep_on, -1);
PS3_SPLIT_DEFINE(void* volatile, g_pm_flow_ctx, 0);
PS3_SPLIT_DEFINE(int, g_spu_ls_watch_n, -1);
PS3_SPLIT_DEFINE(int, g_spu_ch_diag, 0);
PS3_SPLIT_DEFINE(unsigned, g_spu_ch_diag_gen, ~0u);
PS3_SPLIT_DEFINE(volatile int, g_spu_bi_switch, 0);
PS3_SPLIT_DEFINE(int, g_spu_task_regs, 0);
static unsigned char s_coh_bitmap[(1u << (32 - 7)) / 8];
static int s_coh_armed;
unsigned char*     ps3_split_coh_bitmap = s_coh_bitmap;
int*               ps3_split_coh_armed = &s_coh_armed;
volatile unsigned* ps3_split_game_env_gen;
extern const void*     ps3_split_p_function_table;        /* game_host.c */
extern const uint64_t* ps3_split_p_function_table_count;

/* --- The checks --------------------------------------------------------------- */
static int s_fail, s_pass;
#define CHECK(c, ...)                                                          \
    do {                                                                       \
        if (c) {                                                               \
            __atomic_add_fetch(&s_pass, 1, __ATOMIC_RELAXED);                  \
        } else {                                                               \
            __atomic_add_fetch(&s_fail, 1, __ATOMIC_RELAXED);                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static void* fn(const char* name)
{
    const struct { uint64_t addr; void* func; const char* name; }* t = ps3_split_p_function_table;
    for (uint64_t i = 0; i < *ps3_split_p_function_table_count; i++)
        if (strcmp(t[i].name, name) == 0) return t[i].func;
    printf("FAIL: the module has no %s\n", name);
    exit(1);
}

/* Two threads meet here (macOS has no pthread barrier). */
static pthread_mutex_t s_bar_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_bar_cv = PTHREAD_COND_INITIALIZER;
static int s_bar_n, s_bar_gen;
static void bar_wait(void)
{
    pthread_mutex_lock(&s_bar_mu);
    const int gen = s_bar_gen;
    if (++s_bar_n == 2) {
        s_bar_n = 0;
        s_bar_gen++;
        pthread_cond_broadcast(&s_bar_cv);
    } else {
        while (gen == s_bar_gen) pthread_cond_wait(&s_bar_cv, &s_bar_mu);
    }
    pthread_mutex_unlock(&s_bar_mu);
}

/* Each thread sets the module's trampoline, reads it back from both sides, and waits for the other in between. */
static void* tls_thread(void* arg)
{
    void (*set)(void*) = (void (*)(void*))fn("t_tls_set");
    void* (*get)(void) = (void* (*)(void))fn("t_tls_get");
    set(arg);
    bar_wait();
    CHECK(get() == arg, "the module's trampoline on this thread is %p, set %p", get(), arg);
    CHECK(*ps3_split_ppu_tramp() == arg, "the host reads %p from the module's slot, set %p", *ps3_split_ppu_tramp(), arg);
    void* mine = (char*)arg + 1;
    g_trampoline_fn = (void (*)(void*))mine;   /* game_split.h: the module's, through the host's cached slot */
    bar_wait();
    CHECK(get() == mine, "the host's write reads %p in the module, wrote %p", get(), mine);
    return 0;
}

int main(void)
{
    static unsigned char space[1 << 16];
    vm_base = space;
    g_vm_inl_rd_lim = 0x1234;   /* the host's, before the load */
    setenv("PS3_SPLIT_TEST", "17", 1);
    unsetenv("PS3_GAME_MODULE");   /* the module beside the exe, as des_host finds it */
    if (ps3_game_load() != 0) { printf("FAIL: ps3_game_load\n"); return 1; }

    unsigned long long (*thunks)(unsigned) = (unsigned long long (*)(unsigned))fn("t_thunks");
    CHECK(thunks(10) == (3ull * 10 + PS3_GH_vm_read32) + (3ull * 11 + PS3_GH_spu_stop), "thunks: %llu", thunks(10));
    void* addr = ((void* (*)(void))fn("t_thunk_addr"))();
    CHECK(addr == ps3_split_module_spu_indirect_branch, "the module's spu_indirect_branch is %p, the host has %p",
          addr, ps3_split_module_spu_indirect_branch);
    CHECK(((unsigned long long (*)(unsigned))addr)(9) == 3ull * 9 + PS3_GH_spu_indirect_branch,
          "a call through the thunk's address");

    unsigned (*rd_lim)(void) = (unsigned (*)(void))fn("t_rd_lim");
    CHECK(rd_lim() == 0x1234, "the module's copy at the load is %#x", rd_lim());
    g_vm_inl_rd_lim = 0x5678;   /* now the module's, through the pointer */
    CHECK(rd_lim() == 0x5678, "after the host's write the module reads %#x", rd_lim());

    static char a, b;
    pthread_t ta, tb;
    pthread_create(&ta, 0, tls_thread, &a);
    pthread_create(&tb, 0, tls_thread, &b);
    pthread_join(ta, 0);
    pthread_join(tb, 0);

    FILE* f = tmpfile();
    const int n = ((int (*)(FILE*))fn("t_print"))(f);
    char text[128] = { 0 };
    rewind(f);
    if (fread(text, 1, sizeof text - 1, f) == 0) text[0] = 0;
    fclose(f);
    char want_text[128];
    const int want_n = snprintf(want_text, sizeof want_text, "%d %s %.2f %c %llu|", 42, "str", 1.5, 'x', 1ull << 40);
    strcat(want_text, "end!w");
    CHECK(n == want_n && strcmp(text, want_text) == 0, "printed %d: '%s', the host's %d: '%s'", n, text, want_n,
          want_text);
    CHECK(((unsigned long (*)(const char*))fn("t_env"))("PS3_SPLIT_TEST") == 17 + 255, "getenv, atoi, strtoul");
    CHECK(((int (*)(int))fn("t_sjlj"))(4) == 4 * 100 + 5, "setjmp and longjmp: %d", ((int (*)(int))fn("t_sjlj"))(4));

    void (*mcpy)(void*, const void*, size_t) = (void (*)(void*, const void*, size_t))fn("t_memcpy");
    void (*mmov)(void*, const void*, size_t) = (void (*)(void*, const void*, size_t))fn("t_memmove");
    void (*mset)(void*, int, size_t) = (void (*)(void*, int, size_t))fn("t_memset");
    int (*mcmp)(const void*, const void*, size_t) = (int (*)(const void*, const void*, size_t))fn("t_memcmp");
    size_t (*slen)(const char*) = (size_t (*)(const char*))fn("t_strlen");
    void (*zero)(unsigned char*, size_t) = (void (*)(unsigned char*, size_t))fn("t_zero");
    void (*pat)(uint32_t*, size_t) = (void (*)(uint32_t*, size_t))fn("t_pattern");
    enum { N = 200 };
    unsigned char x[N], y[N], src[N];
    for (int i = 0; i < N; i++) src[i] = (unsigned char)(i * 7 + 3);
    int bad = 0;
    for (size_t len = 0; len <= 70; len++) {
        for (int off = 0; off <= 40; off++) {
            /* a copy into another buffer, a move down and a move up by off, and a fill */
            memcpy(x, src, N); memcpy(y, src, N);
            mcpy(x + off, src + 100, len); memcpy(y + off, src + 100, len);
            bad += memcmp(x, y, N) != 0;
            memcpy(x, src, N); memcpy(y, src, N);
            mmov(x + 50, x + 50 + off, len); memmove(y + 50, y + 50 + off, len);
            bad += memcmp(x, y, N) != 0;
            memcpy(x, src, N); memcpy(y, src, N);
            mmov(x + 50 + off, x + 50, len); memmove(y + 50 + off, y + 50, len);
            bad += memcmp(x, y, N) != 0;
            memcpy(x, src, N); memcpy(y, src, N);
            mset(x + off, 0xA5 + off, len); memset(y + off, 0xA5 + off, len);
            bad += memcmp(x, y, N) != 0;
            memcpy(x, src, N); memcpy(y, src, N);
            zero(x + off, len); memset(y + off, 0, len);
            bad += memcmp(x, y, N) != 0;
            memcpy(x, src, N); memcpy(y, src, N);
            if (len) { x[off + len - 1] ^= 0x80; y[off + len - 1] ^= 0x80; }
            const int r1 = mcmp(x + off, src + off, len), r2 = memcmp(y + off, src + off, len);
            bad += (r1 > 0) != (r2 > 0) || (r1 < 0) != (r2 < 0);
        }
    }
    CHECK(bad == 0, "%d of the memory function cases differ from the host's", bad);
    CHECK(slen("") == 0 && slen("seventeen chars!!") == 17, "strlen");
    uint32_t words[40] = { 0 };
    pat(words + 1, 37);
    CHECK(words[0] == 0 && words[1] == 0x01020304u && words[37] == 0x01020304u && words[38] == 0, "the pattern loop");
    CHECK(((unsigned (*)(unsigned))fn("t_bigframe"))(77) == 78, "a 256 KB frame");
    const double m = ((double (*)(double))fn("t_math"))(2.5);
    const double want = sqrt(2.5) + floor(2.5) + trunc(2.5) + ceil(2.5) + nearbyint(2.5) + fma(2.5, 2.5, 2.5) +
                        ldexp(2.5, 3) + exp2(2.5);
    CHECK(memcmp(&m, &want, sizeof m) == 0, "the math: %.17g, the host's %.17g", m, want);

    printf("game split: %d checks passed, %d failed\n", s_pass, s_fail);
    return s_fail != 0;
}
