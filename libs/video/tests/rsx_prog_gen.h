/*
 * rsx_prog_gen.h -- a seeded generator of RSX fragment and vertex programs,
 * for the shader tests (test_shader_text_identity.c, test_gpu_sc_spv.c).
 *
 * The programs come from the NV40 encodings (the decompilers' field and
 * opcode tables: ALU, texture, kill and flow-control instructions, inline
 * constants, condition codes), so the same seed gives the same programs in
 * every test. Nothing here comes from a game. Include it in one file of a
 * test; every function is static.
 */
#ifndef PS3RECOMP_RSX_PROG_GEN_H
#define PS3RECOMP_RSX_PROG_GEN_H

#include <stdint.h>
#include <string.h>

#include "ps3emu/ps3types.h"

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

#endif /* PS3RECOMP_RSX_PROG_GEN_H */
