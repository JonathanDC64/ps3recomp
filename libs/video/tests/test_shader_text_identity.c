/*
 * test_shader_text_identity -- the portable renderer's shader texts and
 * compile service against today's D3D12 renderer, byte for byte.
 *
 *   test_shader_text_identity [--seed N] [--fp N] [--vp N] [--put N] [--keep] [--verbose]
 *
 * libs/video/rsxcore/rsxc_shader.c copies the D3D12 renderer's shader text
 * builders, and libs/video/gpu/gpu_shader.c its shader disk cache, so the new
 * path's texts, cache keys and .dxbc files are today's. This holds them to it:
 *
 *  - a seeded generator writes RSX fragment and vertex programs from the NV40
 *    encodings (the decompilers' field and opcode tables: ALU, texture, kill
 *    and flow-control instructions, inline constants, condition codes);
 *  - for each program and each kind 0-3, with both export widths and the
 *    cube / shadow / depth-as-colour masks, rsx_d3d12_shader_hlsl and
 *    rsxc_shader_hlsl must give the same result and text; so must _hdr for
 *    the HDR variants, _key (and gpu_sc_key) for every text, and
 *    _program_hash64 and _fp_variants for every program; some calls get a
 *    buffer just too small, so the failure paths are compared too;
 *  - the draw path's vertex text (rsxc_vs_locate / rsxc_vs_text, from an
 *    rsx_state) must be the export's text, key and fingerprint, with
 *    VP_W_ONE, VP_BYPASS and VP_START_OFF acting as in vp_get_vs_mode; the
 *    fragment memo (rsxc_fp_hash_at) must give the decompiler's hash, size
 *    and units, before and after the program changes;
 *  - for a sample of texts, gpu_sc_put and rsx_d3d12_shader_cache_put each
 *    fill their own new cache directory: the same results, file names and
 *    bytes (PS3_SHADER_CACHE_HLSL=1, so the kept texts too).
 *
 * Nothing here comes from a game. Exit 0 when everything matches. Windows
 * only: it links the runtime library, whose D3D12 renderer has the exports.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ps3emu/ps3_knob.h"        /* ps3_env_gen: a knob's next read sees a setenv */
#include "rsx_d3d12_backend.h"     /* today's exports */
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"
#include "rsxcore/rsxc_shader.h"
#include "gpu/gpu_shader.h"

/* What the runtime library expects from a game's loader. */
uint8_t* vm_base = NULL;
const uint64_t function_table[2] = { 0, 0 };
const uint64_t function_table_count = 0;

#define CAP (256u * 1024u)
#define HDRV_SAT 0x80000000u

static int g_verbose;
static unsigned long long g_checks, g_fail;
static char g_ctx[160];

static void fail_at(const char* what, const char* a, const char* b)
{
    g_fail++;
    if (g_fail > 12) return;
    printf("MISMATCH %s [%s]\n", what, g_ctx);
    if (a && b) {
        size_t i = 0;
        while (a[i] && a[i] == b[i]) i++;
        const size_t from = i > 60 ? i - 60 : 0;
        printf("  at byte %u (lengths %u, %u)\n  today: %.120s\n  copy:  %.120s\n", (unsigned)i,
               (unsigned)strlen(a), (unsigned)strlen(b), a + from, b + from);
    }
}
static void check_int(const char* what, long long a, long long b)
{
    g_checks++;
    if (a != b) { char m[64]; snprintf(m, sizeof m, "%lld vs %lld", a, b); fail_at(what, m, NULL); printf("  %s\n", m); }
}
static void check_str(const char* what, const char* a, const char* b)
{
    g_checks++;
    if (strcmp(a, b)) fail_at(what, a, b);
}

/* ---- random --------------------------------------------------------------- */
typedef struct { uint64_t s; } Rng;
static uint64_t rnext(Rng* r)
{
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static u32 rn(Rng* r, u32 n) { return n ? (u32)(rnext(r) % n) : 0; }
static int chance(Rng* r, u32 one_in) { return rn(r, one_in) == 0; }
static Rng rng_for(uint64_t seed, uint64_t stream, uint64_t index)
{
    Rng r = { seed ^ (stream * 0xD1B54A32D192ED03ull) ^ (index * 0x8CB92BA72F3D8DD7ull) };
    rnext(&r);
    return r;
}

/* ---- fragment programs ------------------------------------------------------
 * The NV40 fragment encoding (rsx_fp_decompiler.c): four words, each stored
 * big-endian with its 16-bit halves swapped; a CONST source pulls the next 16
 * bytes as an inline constant. */
static void fp_put(u8* p, u32 h)
{
    const u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16); p[2] = (u8)(be >> 8); p[3] = (u8)be;
}

static const u8 k_fp_alu[] = {
    0x01, 0x01, 0x02, 0x03, 0x04, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
    0x0E, 0x0F, 0x10, 0x11, 0x13, 0x14, 0x15, 0x16, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2E, 0x36, 0x38, 0x39, 0x3A,
    0x3B, 0x3C, 0x3D, 0x3E, 0x00 };
static const u8 k_fp_tex[] = { 0x17, 0x17, 0x17, 0x18, 0x18, 0x2F, 0x31, 0x19, 0x33, 0x34 };

static u32 fp_src(Rng* r, int allow_const)
{
    u32 t = rn(r, 100);
    u32 type = t < 50 ? 0u : t < 85 ? 1u : (allow_const ? 2u : 0u);
    u32 w = type;
    w |= (chance(r, 4) ? rn(r, 48) : rn(r, 6)) << 2;
    if (chance(r, 3)) w |= 1u << 8;                          /* half */
    w |= (chance(r, 3) ? rn(r, 256) : 0xE4u) << 9;          /* swizzle (identity 0xE4) */
    if (chance(r, 6)) w |= 1u << 17;                         /* negate */
    return w;
}

static float k_fp_floats[] = { 0.0f, 1.0f, 0.5f, -1.0f, 2.0f, 0.25f, 255.0f, -0.125f, 1e-3f, 3.14159f };
static u32 fp_const_word(Rng* r)
{
    const u32 k = rn(r, 40);
    if (k == 0) return 0x7FC00000u;          /* NaN: sanitized by the decompiler */
    if (k == 1) return 0x7F800000u;          /* +Inf */
    if (k == 2) return 0x00000001u;          /* denormal */
    if (k == 3) return (u32)rnext(r);
    float f = k < 24 ? k_fp_floats[rn(r, 10)] : (float)((int)rn(r, 2001) - 1000) / 64.0f;
    u32 w; memcpy(&w, &f, 4);
    return w;
}

typedef struct { u32 w[4]; int has_k; u32 k[4]; int br, else_i, end_i; } FpIns;

/* A program into out (cap bytes); returns its length. */
static u32 gen_fp(Rng* r, u8* out, u32 cap)
{
    static FpIns ins[256];
    const u32 shape = rn(r, 100);
    u32 n;
    if (shape < 2) {                                  /* raw words, END last */
        n = 1 + rn(r, 24);
        u32 len = 0;
        for (u32 i = 0; i < n && len + 16 <= cap; i++, len += 16)
            for (int j = 0; j < 4; j++) {
                u32 v = (u32)rnext(r);
                if (j == 0) v = (v & ~1u) | (i + 1 == n);
                fp_put(out + len + 4 * j, v);
            }
        return len;
    }
    const int many_k = shape < 5;                      /* more constants than b1 holds */
    const int no_end = shape == 5;
    n = many_k ? 129 + rn(r, 40) : 1 + (chance(r, 6) ? rn(r, 120) : rn(r, 28));
    if (n > 256) n = 256;
    for (u32 i = 0; i < n; i++) {
        FpIns* q = &ins[i];
        memset(q, 0, sizeof *q);
        q->br = !many_k && i + 1 < n && chance(r, 14);
        if (q->br) {
            static const u8 bops[] = { 0x02, 0x02, 0x03, 0x04, 0x00, 0x00, 0x05, 0x06 };
            const u32 op = bops[rn(r, sizeof bops)];
            q->w[0] = op << 24;
            u32 ex = chance(r, 3) ? rn(r, 8) : 7u;
            q->w[1] = (ex << 18) | (rn(r, 256) << 21) | (chance(r, 2) ? 1u << 31 : 0);
            q->w[2] = 1u << 31;
            if (op == 0x03 || op == 0x04)
                q->w[2] |= (rn(r, 9) << 2) | (rn(r, 3) << 10) | (rn(r, 3) << 19);
            q->end_i = (int)(i + 1 + rn(r, n - i));
            q->else_i = (int)(i + 1 + rn(r, (u32)q->end_i - i));
            if (chance(r, 12)) q->end_i = (int)i;     /* malformed: end before it */
            continue;
        }
        const int tex = chance(r, 4);
        const u32 op = many_k ? (chance(r, 2) ? 0x01u : 0x03u)
                     : tex ? k_fp_tex[rn(r, sizeof k_fp_tex)]
                     : chance(r, 25) ? 0x12u : k_fp_alu[rn(r, sizeof k_fp_alu)];
        u32 w0 = op << 24;
        w0 |= (chance(r, 4) ? 1u + rn(r, 15) : 15u) << 9;           /* write mask */
        static const u8 dregs[] = { 0, 0, 2, 3, 4, 0, 4, 6, 8, 1 };
        const u32 dr = chance(r, 3) ? rn(r, 48) : dregs[rn(r, sizeof dregs)];
        w0 |= dr << 1;
        if (chance(r, 2)) w0 |= 1u << 7;                               /* half dest */
        if (chance(r, 12)) w0 |= 1u << 8;                              /* set_cond */
        w0 |= (chance(r, 2) ? 4u + rn(r, 8) : rn(r, 16)) << 13;        /* input */
        w0 |= (chance(r, 2) ? rn(r, 4) : rn(r, 16)) << 17;             /* texture unit */
        if (chance(r, 4)) w0 |= rn(r, 4) << 22;                        /* dest precision */
        if (chance(r, 30)) w0 |= 1u << 30;                             /* no dest */
        if (chance(r, 3)) w0 |= 1u << 31;                              /* saturate */
        u32 s0 = fp_src(r, 1), s1 = fp_src(r, 1), s2 = fp_src(r, 1);
        if (many_k) s0 = (s0 & ~3u) | 2u;
        const u32 ex = chance(r, 9) ? rn(r, 8) : 7u;
        u32 w1 = s0 | (ex << 18) | (rn(r, 256) << 21);
        if (chance(r, 8)) w1 |= 1u << 29;                              /* src0 abs */
        if (chance(r, 6)) w1 |= 1u << 30;
        if (chance(r, 6)) w1 |= 1u << 31;
        u32 w2 = s1;
        if (chance(r, 8)) w2 |= 1u << 18;
        if (chance(r, 4)) w2 |= rn(r, 512) << 19;                      /* precisions */
        if (chance(r, 8)) w2 |= rn(r, 8) << 28;                        /* result scale */
        w2 &= ~(1u << 31);
        u32 w3 = s2;
        if (chance(r, 8)) w3 |= 1u << 18;
        q->w[0] = w0; q->w[1] = w1; q->w[2] = w2; q->w[3] = w3;
        q->has_k = (s0 & 3u) == 2u || (s1 & 3u) == 2u || (s2 & 3u) == 2u;
        for (int j = 0; j < 4; j++) q->k[j] = fp_const_word(r);
    }
    /* Byte offsets, then the branches' targets (word offsets; a multiple of
     * four, so their low bits never read as a CONST source). */
    static u32 at[258];
    u32 len = 0;
    for (u32 i = 0; i < n; i++) { at[i] = len; len += ins[i].has_k ? 32u : 16u; }
    at[n] = len;
    if (len > cap) return 0;
    for (u32 i = 0; i < n; i++) {
        FpIns* q = &ins[i];
        if (q->br) {
            const u32 op = q->w[0] >> 24;
            if (op == 0x02) q->w[2] |= at[q->else_i] >> 2;
            q->w[3] = at[q->end_i] >> 2;
        }
        u32 w0 = q->w[0];
        if (i + 1 == n && !no_end) w0 |= 1u;
        fp_put(out + at[i] + 0, w0);
        fp_put(out + at[i] + 4, q->w[1]);
        fp_put(out + at[i] + 8, q->w[2]);
        fp_put(out + at[i] + 12, q->w[3]);
        if (q->has_k) for (int j = 0; j < 4; j++) fp_put(out + at[i] + 16 + 4 * j, q->k[j]);
    }
    return len;
}

/* ---- vertex programs ----------------------------------------------------------
 * The NV40 vertex encoding (rsx_vp_decompiler.c): four words per
 * instruction, END in D3 bit 0. A Cg binary holds them big-endian (what the
 * export takes); the state's store holds them little-endian. */
static u32 vp_src(Rng* r)
{
    u32 s = 1u + rn(r, 3);                                   /* temp, input, const */
    if (chance(r, 16)) s = 0;
    s |= rn(r, chance(r, 4) ? 64 : 8) << 2;
    s |= (chance(r, 3) ? rn(r, 256) : 0x1Bu) << 8;           /* swizzle (identity 0x1B) */
    if (chance(r, 6)) s |= 1u << 16;
    return s;
}

/* A program of n instructions whose branch targets assume start slot base;
 * words (host order) into w; returns n. */
static u32 gen_vp(Rng* r, u32* w, u32 max_instr, u32 base, int with_end)
{
    u32 n = 1 + (chance(r, 6) ? rn(r, 160) : rn(r, 40));
    if (n > max_instr) n = max_instr;
    for (u32 i = 0; i < n; i++) {
        const u32 s0 = vp_src(r), s1 = vp_src(r), s2 = vp_src(r);
        const u32 vec = chance(r, 12) ? 0u : chance(r, 20) ? 0x19u : rn(r, 0x17);
        static const u8 sflow[] = { 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x11, 0x12 };
        const int flow = chance(r, 18);
        const u32 sca = flow ? sflow[rn(r, sizeof sflow)] : chance(r, 2) ? 0u : rn(r, 0x15);
        u32 d0 = rn(r, 4);                                     /* address swizzle */
        d0 |= rn(r, 256) << 2;                                 /* CC swizzle */
        d0 |= (chance(r, 6) ? rn(r, 8) : 7u) << 10;            /* cond */
        if (chance(r, 6)) d0 |= 1u << 13;                      /* cond test */
        if (chance(r, 10)) d0 |= (1u << 14) | (1u << 29);      /* CC update */
        d0 |= (chance(r, 2) ? 0x3Fu : rn(r, 32)) << 15;        /* vec temp dest */
        d0 |= rn(r, 8) << 21;                                  /* abs */
        if (chance(r, 4)) d0 |= 1u << 24;                      /* address register */
        if (chance(r, 4)) d0 |= 1u << 25;                      /* CC select */
        if (chance(r, 8)) d0 |= 1u << 26;                      /* saturate */
        if (chance(r, 2)) d0 |= 1u << 30;                      /* vec result to output */
        u32 d1 = (s0 >> 9) & 0xFFu;
        d1 |= rn(r, 16) << 8;                                  /* input */
        d1 |= (chance(r, 2) ? rn(r, 16) : rn(r, 512)) << 12;   /* constant */
        d1 |= vec << 22;
        d1 |= sca << 27;
        u32 d2 = (s2 >> 11) & 0x3Fu;
        d2 |= s1 << 6;
        d2 |= (s0 & 0x1FFu) << 23;
        u32 d3 = 0;
        if (chance(r, 16)) d3 |= 1u << 1;                      /* indexed constant */
        static const u8 outs[] = { 0, 0, 1, 2, 5, 6, 7, 8, 9, 14, 15, 0x1F, 0x1F };
        d3 |= (u32)outs[rn(r, sizeof outs)] << 2;
        d3 |= (chance(r, 2) ? 0x3Fu : rn(r, 32)) << 7;         /* scalar temp dest */
        d3 |= (chance(r, 4) ? rn(r, 16) : 15u) << 13;          /* vec write mask */
        if (sca) d3 |= (chance(r, 3) ? rn(r, 16) : 1u + rn(r, 15)) << 17;
        d3 |= (s2 & 0x7FFu) << 21;
        if (flow) {   /* D0[23] << 9 | D2[0:5] << 3 | D3[29:31], an absolute slot */
            const u32 t = base + rn(r, n + 2);
            d0 = (d0 & ~(1u << 23)) | (((t >> 9) & 1u) << 23);
            d2 = (d2 & ~0x3Fu) | ((t >> 3) & 0x3Fu);
            d3 = (d3 & ~(7u << 29)) | ((t & 7u) << 29);
        }
        if (with_end && i + 1 == n) d3 |= 1u; else d3 &= ~1u;
        w[i * 4 + 0] = d0; w[i * 4 + 1] = d1; w[i * 4 + 2] = d2; w[i * 4 + 3] = d3;
    }
    return n;
}
static void put_be(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void put_le(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

/* ---- the sample of texts for the cache test ---------------------------------- */
typedef struct { int fragment; char* text; } Sample;
static Sample* g_sample;
static int g_sample_n, g_sample_want, g_sample_of[2];
static void sample_add(int fragment, const char* text)
{
    /* Half vertex texts, half fragment ones. */
    if (g_sample_n >= g_sample_want || g_sample_of[fragment != 0] >= (g_sample_want + 1) / 2) return;
    for (int i = 0; i < g_sample_n; i++)
        if (g_sample[i].fragment == fragment && !strcmp(g_sample[i].text, text)) return;
    g_sample_of[fragment != 0]++;
    g_sample[g_sample_n].fragment = fragment;
    g_sample[g_sample_n].text = _strdup(text);
    g_sample_n++;
}

/* ---- the comparisons ---------------------------------------------------------- */
static char* A;
static char* B;
static unsigned long long g_texts, g_hdr_texts, g_hdr_changed, g_fp_calls, g_vp_calls, g_kind_ok[4];

static void compare_keys(int fragment, const char* a, const char* b)
{
    check_int("shader_key", (long long)rsx_d3d12_shader_key(fragment, a), (long long)rsxc_shader_key(fragment, b));
    check_int("gpu_sc_key", (long long)rsx_d3d12_shader_key(fragment, a),
              (long long)gpu_sc_key(b, strlen(b), fragment ? "ps_5_0" : "vs_5_0"));
}

/* Both builders on one input; cap bytes of output room. Returns the count. */
static int compare_hlsl(int kind, const u8* uc, u32 len, u32 slot, int exp32, u32 cube, u32 d2c, u32 cap)
{
    A[0] = 0; B[0] = 0;
    const int na = rsx_d3d12_shader_hlsl(kind, uc, len, slot, exp32, cube, d2c, A, cap);
    const int nb = rsxc_shader_hlsl(kind, uc, len, slot, exp32, cube, d2c, B, cap);
    check_int("shader_hlsl result", na, nb);
    check_str("shader_hlsl text", A, B);
    compare_keys(kind == 1 || kind == 3, A, B);
    if (kind == 1 || kind == 3) g_fp_calls++; else g_vp_calls++;
    if (na > 0) { g_texts++; g_kind_ok[kind & 3]++; }
    return na;
}

static void compare_hdr(const char* text, u32 hdrv, u32 cap)
{
    const size_t n = strlen(text) + 1;
    if (n > CAP) return;
    memcpy(A, text, n); memcpy(B, text, n);
    const int ka = rsx_d3d12_shader_hdr(A, cap, hdrv);
    const int kb = rsxc_shader_hdr(B, cap, hdrv);
    check_int("shader_hdr result", ka, kb);
    check_str("shader_hdr text", A, B);
    compare_keys(1, A, B);
    g_hdr_texts++;
    if (ka > 0) g_hdr_changed++;
    if (ka > 0 && ((hdrv ^ (u32)n) % 3) == 0) sample_add(1, B);
}

static u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }

static void compare_fp_program(Rng* r, const u8* uc, u32 len, u32 index)
{
    /* The masks the exports take, from the units the program samples (the
     * export keeps only those), plus a few that name unsampled units. */
    u32 units = 0;
    {
        u8* buf = (u8*)calloc(1, RSX_FP_MAX_BYTES);
        memcpy(buf, uc, len < RSX_FP_MAX_BYTES ? len : RSX_FP_MAX_BYTES);
        units = rsx_fp_texture_units(buf, RSX_FP_MAX_BYTES);
        free(buf);
    }
    const u32 rnd = (u32)rnext(r);
    const u32 masks[6][2] = {
        { 0, 0 },
        { units & rnd, 0 },                                     /* cube units */
        { (units & (rnd >> 8)) << 16, 0 },                      /* shadow units */
        { 0, units & (rnd >> 16) },                             /* depth as colour */
        { (units & rnd & 0x5555u) | ((units & 0x2222u) << 16), units & 0x8888u },
        { rnd, rnd >> 7 },                                      /* anything */
    };
    for (int kind = 1; kind <= 3; kind += 2)
        for (int exp32 = 0; exp32 <= 1; exp32++)
            for (int m = 0; m < 6; m++) {
                snprintf(g_ctx, sizeof g_ctx, "fp %u kind %d exp32 %d cube %08X d2c %04X", index, kind,
                         exp32, masks[m][0], masks[m][1]);
                const int n = compare_hlsl(kind, uc, len, 0, exp32, masks[m][0], masks[m][1], CAP);
                if (n <= 0) continue;
                static char text[CAP];
                memcpy(text, B, strlen(B) + 1);
                if (m == 0 && exp32 == (int)(index & 1)) sample_add(1, text);
                /* Just too small, and a little short, for the builders' own checks. */
                if (m == 0 && (index % 7) == 0) {
                    const u32 tl = (u32)strlen(text);
                    compare_hlsl(kind, uc, len, 0, exp32, 0, 0, tl);
                    compare_hlsl(kind, uc, len, 0, exp32, 0, 0, tl + 1);
                    compare_hlsl(kind, uc, len, 0, exp32, 0, 0, tl / 2 + 2);
                }
                if (m == 0 || m == 4) {
                    /* Limits that print the same at any precision, and ones
                     * that need all nine digits (0.1f is 0.100000001). */
                    const u32 hdrvs[] = { f2u(2.0f), f2u(64.0f), f2u(1000.5f), f2u(65504.0f),
                                          f2u(1e30f), f2u(0.1f), f2u(10.0f / 3.0f),
                                          f2u(1.0f + (float)(rnd % 60000u) / 7.0f),
                                          HDRV_SAT | units, HDRV_SAT | 0xFFFFu,
                                          HDRV_SAT | (units & rnd), 0 };
                    for (unsigned h = 0; h < sizeof hdrvs / sizeof hdrvs[0]; h++) {
                        snprintf(g_ctx, sizeof g_ctx, "fp %u kind %d exp32 %d mask %d hdrv %08X", index,
                                 kind, exp32, m, hdrvs[h]);
                        compare_hdr(text, hdrvs[h], CAP);
                        if ((index % 5) == 0) {
                            const u32 tl = (u32)strlen(text);
                            compare_hdr(text, hdrvs[h], tl + 1);
                            compare_hdr(text, hdrvs[h], tl + 12);
                        }
                    }
                }
            }
    /* The vertex kinds on the same bytes: a decompiler fed what it is not
     * meant to read must still give the same answer both ways. */
    snprintf(g_ctx, sizeof g_ctx, "fp %u as vertex", index);
    compare_hlsl(0, uc, len, 0, 0, 0, 0, CAP);
    compare_hlsl(2, uc, len, 0, 0, 0, 0, CAP);
    snprintf(g_ctx, sizeof g_ctx, "fp %u hash/variants", index);
    check_int("program_hash64 fp", (long long)rsx_d3d12_program_hash64(1, uc, len), (long long)rsxc_program_hash64(1, uc, len));
    check_int("program_hash64 fp as vp", (long long)rsx_d3d12_program_hash64(0, uc, len), (long long)rsxc_program_hash64(0, uc, len));
    u32 va[3 * 8], vb[3 * 8];
    memset(va, 0, sizeof va); memset(vb, 0, sizeof vb);
    const int nva = rsx_d3d12_shader_fp_variants(uc, len, va, 8);
    const int nvb = rsxc_shader_fp_variants(uc, len, vb, 8);
    check_int("fp_variants count", nva, nvb);
    check_int("fp_variants list", memcmp(va, vb, sizeof va), 0);
    check_int("fp_variants max 1", rsx_d3d12_shader_fp_variants(uc, len, va, 1), rsxc_shader_fp_variants(uc, len, vb, 1));
}

static rsx_state g_st;

static void compare_vp_program(Rng* r, u32 index)
{
    static u32 w[4 * 200];
    static u8 be[16 * 200];
    const u32 base = chance(r, 2) ? 0u : rn(r, 400);
    const int with_end = !chance(r, 40);
    const u32 n = gen_vp(r, w, 200, base, with_end);
    for (u32 i = 0; i < n * 4; i++) put_be(be + 4 * i, w[i]);
    const u32 len = n * 16;
    for (int kind = 0; kind <= 3; kind++) {
        const u32 slots[2] = { 0, base };
        for (int s = 0; s < (kind == 0 || kind == 2 ? 2 : 1); s++) {
            snprintf(g_ctx, sizeof g_ctx, "vp %u kind %d slot %u", index, kind, slots[s]);
            const int ni = compare_hlsl(kind, be, len, slots[s], (int)(index & 1), 0, 0, CAP);
            if ((kind == 0 || kind == 2) && ni > 0 && s == 0) sample_add(0, B);
            if ((kind == 0 || kind == 2) && ni > 0 && (index % 7) == 0) {
                const u32 tl = (u32)strlen(B);
                compare_hlsl(kind, be, len, slots[s], 0, 0, 0, tl);
                compare_hlsl(kind, be, len, slots[s], 0, 0, 0, tl / 2 + 256);
            }
        }
    }
    snprintf(g_ctx, sizeof g_ctx, "vp %u hash", index);
    check_int("program_hash64 vp", (long long)rsx_d3d12_program_hash64(0, be, len), (long long)rsxc_program_hash64(0, be, len));
    check_int("program_hash64 vp, odd length", (long long)rsx_d3d12_program_hash64(0, be, len - 3),
              (long long)rsxc_program_hash64(0, be, len - 3));
    check_int("program_hash64 vp as fp", (long long)rsx_d3d12_program_hash64(1, be, len), (long long)rsxc_program_hash64(1, be, len));

    /* The draw path: the same program in a state's store at a start slot,
     * between others, as SET_TRANSFORM_PROGRAM leaves it. */
    if (!with_end) return;
    const u32 start = rn(r, 1024 - n - 8);
    memset(&g_st, 0, sizeof g_st);
    for (u32 i = 0; i < start * 16; i++) g_st.vp_ucode[i] = (u8)rnext(r);
    for (u32 i = 0; i < n * 4; i++) put_le(g_st.vp_ucode + start * 16 + 4 * i, w[i]);
    const u32 tail = rn(r, 8) * 16;
    for (u32 i = 0; i < tail; i++) g_st.vp_ucode[(start + n) * 16 + i] = (u8)rnext(r);
    g_st.transform_program_start = start;
    g_st.vp_ucode_bytes = (start + n) * 16 + tail;
    for (int fetch = 0; fetch <= 1; fetch++) {
        snprintf(g_ctx, sizeof g_ctx, "vp %u draw path fetch %d start %u", index, fetch, start);
        rsxc_vs_program vp;
        check_int("vs_locate", rsxc_vs_locate(&g_st, fetch, &vp), 0);
        u32 h = 2166136261u;
        for (u32 i = 0; i < n * 16; i++) { h ^= g_st.vp_ucode[start * 16 + i]; h *= 16777619u; }
        h = h ? h : 1u;
        if (fetch) h ^= 0x9E3779B9u;
        check_int("vs_locate hash", (long long)h, (long long)vp.hash);
        check_int("vs_locate length", (long long)len, (long long)vp.vlen);
        check_int("vs_code64", (long long)rsx_d3d12_program_hash64(0, be, len), (long long)rsxc_vs_code64(&vp));
        A[0] = 0; B[0] = 0;
        const int na = rsx_d3d12_shader_hlsl(fetch ? 2 : 0, be, len, 0, 0, 0, 0, A, 262144);
        int c03 = -1;
        const int nb = rsxc_vs_text(&vp, fetch, B, 262144, &c03);
        check_int("vs_text result", na, nb);
        check_str("vs_text text", A, B);
        if (nb > 0)
            check_int("vs_text uses_c03", (strstr(A, "vp_c[0]") || strstr(A, "vp_c[1]") ||
                                           strstr(A, "vp_c[2]") || strstr(A, "vp_c[3]")) ? 1 : 0, c03);
        /* The draw path's knobs: VP_W_ONE and VP_BYPASS patch the text in
         * place, space-padded (vp_get_vs_mode); VP_START_OFF starts at 0. */
        if ((index % 4) == 0 && nb > 0) {
            static const char* const pat[2][2] = {
                { "o[0].w = _v.w;", "o[0].w = 1.0;" },
                { "Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);",
                  "Out.pos = float4(v[0].xy * 0.5, 0.5, 1.0);" } };
            for (int k = 0; k < 2; k++)
                for (char* at = A; (at = strstr(at, pat[k][0])) != NULL; at += strlen(pat[k][0])) {
                    memcpy(at, pat[k][1], strlen(pat[k][1]));
                    memset(at + strlen(pat[k][1]), ' ', strlen(pat[k][0]) - strlen(pat[k][1]));
                }
            _putenv_s("VP_W_ONE", "1");
            _putenv_s("VP_BYPASS", "1");
            check_int("vs_text VP_W_ONE/VP_BYPASS result", rsxc_vs_text(&vp, fetch, B, 262144, NULL), nb);
            check_str("vs_text VP_W_ONE/VP_BYPASS text", A, B);
            _putenv_s("VP_W_ONE", "");
            _putenv_s("VP_BYPASS", "");
            _putenv_s("VP_START_OFF", "1");
            ps3_env_gen++;
            rsxc_vs_program v0;
            check_int("vs_locate VP_START_OFF", rsxc_vs_locate(&g_st, fetch, &v0), 0);
            check_int("vs_locate VP_START_OFF start", v0.vstart, 0);
            _putenv_s("VP_START_OFF", "");
            ps3_env_gen++;
        }
    }
}

/* ---- the cache directories ---------------------------------------------------- */
static void dir_make(char* out, size_t cap, const char* tmp, const char* tag)
{
    snprintf(out, cap, "%sps3r_sti_%lu_%s", tmp, (unsigned long)GetCurrentProcessId(), tag);
    CreateDirectoryA(out, NULL);
}
typedef struct { char name[96]; } Name;
static int dir_list(const char* dir, Name* out, int max)
{
    char pat[MAX_PATH + 8];
    snprintf(pat, sizeof pat, "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int n = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n < max) snprintf(out[n++].name, sizeof out[0].name, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(out[j - 1].name, out[j].name) > 0; j--) {
            Name t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return n;
}
static u8* file_read(const char* path, long* n)
{
    FILE* f = fopen(path, "rb");
    *n = -1;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    u8* b = (u8*)malloc(*n > 0 ? (size_t)*n : 1);
    if (b && *n > 0 && fread(b, 1, (size_t)*n, f) != (size_t)*n) *n = -1;
    fclose(f);
    return b;
}
static void dir_remove(const char* dir)
{
    static Name names[8192];
    const int n = dir_list(dir, names, 8192);
    char p[MAX_PATH + 100];
    for (int i = 0; i < n; i++) { snprintf(p, sizeof p, "%s/%s", dir, names[i].name); DeleteFileA(p); }
    RemoveDirectoryA(dir);
}

/* fp_variants.txt for both directories: variants for some of the programs
 * the run will make (and lines for none), so the variant lists are not
 * empty. */
static void write_variants(const char* da, const char* db, uint64_t seed, u32 nfp)
{
    static u8 uc[16384];
    char pa[MAX_PATH + 32], pb[MAX_PATH + 32];
    snprintf(pa, sizeof pa, "%s/fp_variants.txt", da);
    snprintf(pb, sizeof pb, "%s/fp_variants.txt", db);
    FILE* fa = fopen(pa, "w");
    FILE* fb = fopen(pb, "w");
    if (!fa || !fb) { printf("cannot write %s\n", pa); exit(2); }
    u8* buf = (u8*)calloc(1, RSX_FP_MAX_BYTES);
    for (u32 i = 0; i < nfp; i += 3) {
        Rng r = rng_for(seed, 1, i);
        const u32 len = gen_fp(&r, uc, sizeof uc);
        memset(buf, 0, RSX_FP_MAX_BYTES);
        memcpy(buf, uc, len);
        const u32 h = rsx_fp_code_hash(buf, RSX_FP_MAX_BYTES);
        Rng q = rng_for(seed, 3, i);
        const u32 k = 1 + rn(&q, 10);
        for (u32 j = 0; j < k; j++) {
            char line[64];
            snprintf(line, sizeof line, "%08x %x %x %x\n", h, rn(&q, 2), (u32)rnext(&q) & 0xFFFF00FFu,
                     (u32)rnext(&q) & 0xFFFFu);
            fputs(line, fa); fputs(line, fb);
            if (chance(&q, 4)) { fputs(line, fa); fputs(line, fb); }   /* a repeat */
        }
    }
    fputs("0badf00d 1 0 0\n", fa); fputs("0badf00d 1 0 0\n", fb);
    free(buf);
    fclose(fa); fclose(fb);
}

static int compare_dirs(const char* da, const char* db, int* files)
{
    static Name na[8192], nb[8192];
    const int ca = dir_list(da, na, 8192), cb = dir_list(db, nb, 8192);
    snprintf(g_ctx, sizeof g_ctx, "cache files");
    check_int("cache file count", ca, cb);
    int bad = 0;
    for (int i = 0; i < ca && i < cb; i++) {
        check_str("cache file name", na[i].name, nb[i].name);
        char pa[MAX_PATH + 100], pb[MAX_PATH + 100];
        snprintf(pa, sizeof pa, "%s/%s", da, na[i].name);
        snprintf(pb, sizeof pb, "%s/%s", db, nb[i].name);
        long la, lb;
        u8* a = file_read(pa, &la);
        u8* b = file_read(pb, &lb);
        snprintf(g_ctx, sizeof g_ctx, "cache file %s", na[i].name);
        check_int("cache file size", la, lb);
        if (la == lb && la > 0) { g_checks++; if (memcmp(a, b, (size_t)la)) { bad++; fail_at("cache file bytes", NULL, NULL); } }
        free(a); free(b);
    }
    *files = ca;
    return bad;
}

static void cache_test(const char* da, const char* db)
{
    int put[3] = { 0, 0, 0 };
    for (int i = 0; i < g_sample_n; i++) {
        snprintf(g_ctx, sizeof g_ctx, "put %d (%s)", i, g_sample[i].fragment ? "ps" : "vs");
        const gpu_sc_stage st = g_sample[i].fragment ? GPU_SC_PS : GPU_SC_VS;
        const int ra = rsx_d3d12_shader_cache_put(g_sample[i].fragment, g_sample[i].text);
        const int rb = gpu_sc_put(GPU_SC_DXBC, st, g_sample[i].text);
        check_int("cache_put", ra, rb);
        if (ra >= 0 && ra <= 2) put[ra]++;
        check_int("cache_put again", rsx_d3d12_shader_cache_put(g_sample[i].fragment, g_sample[i].text),
                  gpu_sc_put(GPU_SC_DXBC, st, g_sample[i].text));
        char p[MAX_PATH + 64], q[MAX_PATH + 64];
        if (gpu_sc_cache_path(GPU_SC_DXBC, st, g_sample[i].text, strlen(g_sample[i].text), p, sizeof p)) {
            snprintf(q, sizeof q, "%s/%s_%016llx.dxbc", db, g_sample[i].fragment ? "ps_5_0" : "vs_5_0",
                     (unsigned long long)rsx_d3d12_shader_key(g_sample[i].fragment, g_sample[i].text));
            check_str("cache_path", q, p);
        }
    }
    /* A text FXC refuses. */
    snprintf(g_ctx, sizeof g_ctx, "put of a broken text");
    check_int("cache_put broken", rsx_d3d12_shader_cache_put(1, "float4 main( {"),
              gpu_sc_put(GPU_SC_DXBC, GPU_SC_PS, "float4 main( {"));
    check_int("cache_put NULL", rsx_d3d12_shader_cache_put(0, NULL), gpu_sc_put(GPU_SC_DXBC, GPU_SC_VS, NULL));
    int files = 0;
    const int bad = compare_dirs(da, db, &files);
    printf("cache: %d texts put (%d compiled, %d already there, %d failed both ways), %d files, %d differ\n",
           g_sample_n, put[2], put[1], put[0], files, bad);
    if (put[2] == 0) { g_fail++; printf("MISMATCH no text compiled: the cache test proved nothing\n"); }

    /* gpu_sc_compile: a cache hit gives the file's bytes; a file that is not
     * a whole program is compiled again and replaced, as today. */
    for (int i = 0; i < g_sample_n; i++) {
        const gpu_sc_stage st = g_sample[i].fragment ? GPU_SC_PS : GPU_SC_VS;
        char pa[MAX_PATH + 64], pb[MAX_PATH + 64];
        if (!gpu_sc_cache_path(GPU_SC_DXBC, st, g_sample[i].text, strlen(g_sample[i].text), pb, sizeof pb)) continue;
        snprintf(pa, sizeof pa, "%s%s", da, pb + strlen(db));
        long la;
        u8* a = file_read(pa, &la);
        if (!a || la <= 0) { free(a); continue; }
        snprintf(g_ctx, sizeof g_ctx, "compile %d", i);
        gpu_sc_blob blob;
        int r = gpu_sc_compile(GPU_SC_DXBC, st, g_sample[i].text, strlen(g_sample[i].text), "x", 0, &blob, NULL, 0);
        check_int("compile hit", r, GPU_SC_OK);
        check_int("compile hit bytes", blob.size == (size_t)la && !memcmp(blob.data, a, (size_t)la), 1);
        gpu_sc_blob_release(&blob);
        FILE* f = fopen(pb, "wb");                  /* a torn file: the size field disagrees */
        if (f) { fwrite(a, 1, (size_t)la / 2, f); fclose(f); }
        r = gpu_sc_compile(GPU_SC_DXBC, st, g_sample[i].text, strlen(g_sample[i].text),
                           g_sample[i].fragment ? "guest_fp" : "guest_vp2", 0, &blob, NULL, 0);
        check_int("compile over a torn file", r, GPU_SC_OK);
        check_int("compile over a torn file, bytes", blob.size == (size_t)la && !memcmp(blob.data, a, (size_t)la), 1);
        gpu_sc_blob_release(&blob);
        long lb;
        u8* b = file_read(pb, &lb);
        check_int("torn file replaced", lb == la && b && !memcmp(a, b, (size_t)la), 1);
        free(a); free(b);
        break;
    }

    /* The sprite-edge clamp's geometry shader, which is never cached. */
    {
        snprintf(g_ctx, sizeof g_ctx, "uvclamp gs");
        size_t gl = 0;
        const char* gs = rsxc_uvclamp_gs_hlsl(&gl);
        gpu_sc_blob blob;
        char err[512];
        const int r = gpu_sc_compile(GPU_SC_DXBC, GPU_SC_GS, gs, gl, "uvclamp_gs", GPU_SC_NO_CACHE, &blob, err, sizeof err);
        check_int("gs compile", r, GPU_SC_OK);
        if (r != GPU_SC_OK) printf("  %s\n", err);
        gpu_sc_blob_release(&blob);
        char p[MAX_PATH + 64];
        if (gpu_sc_cache_path(GPU_SC_DXBC, GPU_SC_GS, gs, gl, p, sizeof p))
            check_int("gs not cached", GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES, 1);
        int s2 = gpu_sc_compile(GPU_SC_DXBC, GPU_SC_PS, "float4 main( {", 14, "bad", 0, &blob, err, sizeof err);
        check_int("compile error", s2, GPU_SC_FAILED);
        check_int("compile error text", err[0] != 0, 1);
    }
    for (int t = GPU_SC_SPV; t < GPU_SC_TARGET_COUNT; t++) {
        gpu_sc_blob blob;
        snprintf(g_ctx, sizeof g_ctx, "target %d", t);
        check_int("target not available", gpu_sc_available((gpu_sc_target)t), 0);
        check_int("target compile", gpu_sc_compile((gpu_sc_target)t, GPU_SC_VS, "x", 1, "x", 0, &blob, NULL, 0),
                  GPU_SC_UNAVAILABLE);
        check_int("target put", gpu_sc_put((gpu_sc_target)t, GPU_SC_VS, "x"), 0);
    }
}

/* ---- the draw path's fragment memo ------------------------------------------ */
static void memo_test(uint64_t seed)
{
    const u32 span = 4u << 20;
    u8* mem = (u8*)calloc(1, span + RSX_FP_MAX_BYTES);
    if (!mem) return;
    vm_base = mem;
    u32 offs[64], n = 0, at = 0x1000;
    static u8 uc[16384];
    for (u32 i = 0; i < 64 && at + 16384 < span; i++) {
        Rng r = rng_for(seed, 5, i);
        const u32 len = gen_fp(&r, uc, sizeof uc);
        memcpy(mem + at, uc, len);
        offs[n++] = at;
        at = (at + len + 0x40 + rn(&r, 4096)) & ~0xFu;
    }
    for (int pass = 0; pass < 4; pass++)
        for (u32 i = 0; i < n; i++) {
            if (pass == 3 && i == 0) { _putenv_s("RSX_FP_HASH_MEMO", "0"); ps3_env_gen++; }
            snprintf(g_ctx, sizeof g_ctx, "fp memo pass %d program %u", pass, i);
            if (pass == 2 && (i & 1)) mem[offs[i] + 4] ^= 0x10;   /* a word changes */
            u32 sz = 0;
            const u32 h = rsxc_fp_hash_at(offs[i], &sz);
            u32 want = rsx_fp_program_size(mem + offs[i], RSX_FP_MAX_BYTES);
            if (!want) want = 64;
            check_int("fp_hash_at hash", h, rsx_fp_code_hash(mem + offs[i], RSX_FP_MAX_BYTES));
            check_int("fp_hash_at size", sz, want);
            check_int("fp_units_at", rsxc_fp_units_at(offs[i]), rsx_fp_texture_units(mem + offs[i], RSX_FP_MAX_BYTES));
        }
    _putenv_s("RSX_FP_HASH_MEMO", "");
    ps3_env_gen++;
    snprintf(g_ctx, sizeof g_ctx, "fp seen");
    check_int("fp_seen", rsxc_fp_seen(-1, NULL, NULL), (long long)n);
    vm_base = NULL;
    free(mem);
}

int main(int argc, char** argv)
{
    uint64_t seed = 0x5EEDu;
    u32 nfp = 2000, nvp = 2000;
    int keep = 0;
    g_sample_want = 48;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fp") && i + 1 < argc) nfp = (u32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--vp") && i + 1 < argc) nvp = (u32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--put") && i + 1 < argc) g_sample_want = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keep")) keep = 1;
        else if (!strcmp(argv[i], "--verbose")) g_verbose = 1;
        else { printf("usage: %s [--seed N] [--fp N] [--vp N] [--put N] [--keep] [--verbose]\n", argv[0]); return 2; }
    }
    A = (char*)malloc(CAP);
    B = (char*)malloc(CAP);
    g_sample = (Sample*)calloc((size_t)(g_sample_want > 0 ? g_sample_want : 1), sizeof *g_sample);
    if (!A || !B || !g_sample) return 2;

    /* Two new cache directories: today's renderer fills one, the compile
     * service the other. Each reads PS3_SHADER_CACHE once, at its first call. */
    char tmp[MAX_PATH], da[MAX_PATH + 64], db[MAX_PATH + 64];
    if (!GetTempPathA(sizeof tmp, tmp)) snprintf(tmp, sizeof tmp, ".\\");
    dir_make(da, sizeof da, tmp, "today");
    dir_make(db, sizeof db, tmp, "copy");
    write_variants(da, db, seed, nfp);
    _putenv_s("PS3_SHADER_CACHE_HLSL", "1");
    _putenv_s("PS3_SHADER_CACHE", da);
    const char* ca = rsx_d3d12_shader_cache_path();
    _putenv_s("PS3_SHADER_CACHE", db);
    const char* cb = gpu_sc_cache_dir();
    snprintf(g_ctx, sizeof g_ctx, "cache dirs");
    check_str("today's cache dir", da, ca);
    check_str("the copy's cache dir", db, cb);
    printf("seed 0x%llX: %u fragment and %u vertex programs\n", (unsigned long long)seed, nfp, nvp);

    const DWORD t0 = GetTickCount();
    static u8 uc[16384];
    for (u32 i = 0; i < nfp; i++) {
        Rng r = rng_for(seed, 1, i);
        const u32 len = gen_fp(&r, uc, sizeof uc);
        Rng q = rng_for(seed, 2, i);
        compare_fp_program(&q, uc, len, i);
        if (g_verbose && (i % 500) == 0) printf("  fp %u\n", i);
    }
    for (u32 i = 0; i < nvp; i++) {
        Rng r = rng_for(seed, 4, i);
        compare_vp_program(&r, i);
        if (g_verbose && (i % 500) == 0) printf("  vp %u\n", i);
    }
    snprintf(g_ctx, sizeof g_ctx, "null inputs");
    check_int("shader_hlsl NULL", rsx_d3d12_shader_hlsl(1, NULL, 0, 0, 0, 0, 0, A, CAP), rsxc_shader_hlsl(1, NULL, 0, 0, 0, 0, 0, B, CAP));
    check_int("shader_hlsl cap 1", rsx_d3d12_shader_hlsl(0, uc, 16, 0, 0, 0, 0, A, 1), rsxc_shader_hlsl(0, uc, 16, 0, 0, 0, 0, B, 1));
    check_int("shader_key NULL", (long long)rsx_d3d12_shader_key(1, NULL), (long long)rsxc_shader_key(1, NULL));
    check_int("shader_hdr NULL", rsx_d3d12_shader_hdr(NULL, 0, 1), rsxc_shader_hdr(NULL, 0, 1));
    check_int("program_hash64 empty", (long long)rsx_d3d12_program_hash64(0, uc, 0), (long long)rsxc_program_hash64(0, uc, 0));
    printf("texts: %llu fragment and %llu vertex builds, %llu texts (kind 0: %llu, 1: %llu, 2: %llu, 3: %llu), "
           "%llu HDR variants (%llu changed), %.1f s\n",
           g_fp_calls, g_vp_calls, g_texts, g_kind_ok[0], g_kind_ok[1], g_kind_ok[2], g_kind_ok[3],
           g_hdr_texts, g_hdr_changed, (GetTickCount() - t0) / 1000.0);

    memo_test(seed);
    cache_test(da, db);

    /* The draw path's own pieces, on one text. */
    {
        Rng r = rng_for(seed, 6, 0);
        u32 len = 0;
        for (int tries = 0; tries < 64 && len == 0; tries++) {
            len = gen_fp(&r, uc, sizeof uc);
            u8* buf = (u8*)calloc(1, RSX_FP_MAX_BYTES);
            memcpy(buf, uc, len);
            if (rsxc_fp_build_hlsl(buf, 0, 0, 0, B, CAP, 0x1234, 1) <= 0) len = 0;
            else {
                snprintf(g_ctx, sizeof g_ctx, "fp_build_hlsl");
                rsx_d3d12_shader_hlsl(1, uc, len, 0, 0, 0, 0, A, CAP);
                check_str("fp_build_hlsl text", A, B);
                u32 v[24];
                const int nv = rsxc_shader_fp_variants(uc, len, v, 8);
                int found = 0;
                for (int k = 0; k < nv; k++) found |= v[k * 3] == 0 && v[k * 3 + 1] == 0 && v[k * 3 + 2] == 0;
                check_int("fp_build_hlsl note", found, 1);
                check_int("fp_set_out", rsxc_fp_set_out(B, CAP, "float4(1,0,1,1)"), 1);
                check_int("fp_set_out text", strstr(B, " = float4(1,0,1,1);") != NULL, 1);
            }
            free(buf);
        }
    }

    if (!keep) { dir_remove(da); dir_remove(db); }
    else printf("cache directories kept: %s %s\n", da, db);
    printf("%llu comparisons, %llu mismatches: %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
