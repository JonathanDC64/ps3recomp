/*
 * ps3recomp - the RSX shader texts, for the portable renderer (rsxcore)
 *
 * The HLSL the D3D12 renderer (rsx_d3d12_backend.c) compiles for a guest
 * program, built here without D3D12: the same functions, copied line for
 * line, so the same program and draw state give the same text and the same
 * shader cache key (copy_manifest.txt lists the ranges). The static names
 * are the original's; what other files call is exported as rsxc_*.
 * libs/video/tests/test_shader_text_identity.c holds the copies to today's
 * exports byte for byte.
 *
 * Left out: the debug rewrites of a draw's fragment text (FP_ONE, FP_SHOW,
 * FP_IDCOLOR, FP_TEX, FP_KILL, FP_FORCE, FP_SUB) and the dumps (VP_DUMP,
 * RSX_DBG_VP, FP_DUMP, FP_PICK, FP_LIST, CUBEKEYDBG); the draw-state parts
 * (the VS cache and its memo, dr_uvclamp, dr_hdrv and the HDR scene marks),
 * which go with the draw record.
 */
#if defined(PS3RECOMP_RSX_HAL)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/ps3_knob.h"
#include "../rsx_commands.h"        /* rsx_state */
#include "../rsx_fp_decompiler.h"
#include "../gpu/gpu_shader.h"      /* the cache directory and key */
#include "rsxc_shader.h"

#ifndef MAX_PATH
#define MAX_PATH 260   /* Windows' value, which the copied buffers are sized by */
#endif
#ifndef RSX_FP_UNITS
#define RSX_FP_UNITS      16
#endif
#ifndef FPCB_KMAX
#define FPCB_KMAX         128
#endif

/* The shader cache's directory and key are the compile service's
 * (gpu_shader.c holds the copies of shader_cache_dir and shader_key). */
static const char* shader_cache_dir(void) { return gpu_sc_cache_dir(); }
static u64 shader_key(const char* src, size_t n, const char* profile)
{
    return gpu_sc_key(src, n, profile);
}

static u64 fnv_bytes(u64 h, const void* p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= ((const u8*)p)[i]; h *= 1099511628211ull; }
    return h;
}

/* The debug rewrites below (FP_ONE, FP_SHOW, FP_IDCOLOR, FP_TEX, FP_KILL) edit
 * the colour-0 output of a translated program. Its spelling depends on the
 * decompiler version: "_po.c0 = X;" (older), "float4 _o = X;" (one target) or
 * "float4 _o0 = X;" (several). They anchored on the first only and silently did
 * nothing with the current decompiler. Returns the start of X (or of the whole
 * statement with want_stmt) and sets *end to its ';', or NULL. */
static char* fp_out_anchor(char* hlsl, char** end, int want_stmt)
{
    static const char* const pats[] = { "_po.c0 = ", "float4 _o = ", "float4 _o0 = " };
    for (int i = 0; i < 3; i++) {
        char* rp = strstr(hlsl, pats[i]);
        char* semi = rp ? strchr(rp, ';') : NULL;
        if (semi) { *end = semi; return want_stmt ? rp : rp + strlen(pats[i]); }
    }
    return NULL;
}

/* Replace the colour-0 expression with expr. Returns 1 if it was replaced. */
static int fp_set_out(char* hlsl, size_t cap, const char* expr)
{
    char* semi; char* rp = fp_out_anchor(hlsl, &semi, 0);
    if (!rp) return 0;
    size_t newlen = strlen(expr), tail = strlen(semi) + 1;
    if ((size_t)(rp - hlsl) + newlen + tail >= cap) return 0;
    memmove(rp + newlen, semi, tail);
    memcpy(rp, expr, newlen);
    return 1;
}

/* Insert `ins` in front of the ')' that closes the call whose '(' is at open.
 * Returns the position just past the inserted text, or NULL. */
static char* hlsl_insert_before_close(char* hlsl, size_t cap, char* open, const char* ins)
{
    int depth = 0;
    char* p = open;
    for (; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') { if (--depth == 0) break; }
    }
    if (*p != ')') return NULL;
    const size_t il = strlen(ins), used = strlen(hlsl);
    if (used + il + 1 > cap) return NULL;
    memmove(p + il, p, used - (size_t)(p - hlsl) + 1);
    memcpy(p, ins, il);
    return p + il;
}

/* Wire a decompiled program to this backend's per-draw b1 slot (see
 * VP_FPCB_STRIDE and vp_record_cb):
 *  - its cbuffer declaration becomes the b1 layout (texel scales, alpha test,
 *    inline constants);
 *  - each 2D Sample() coordinate is multiplied by rsx_texscale[unit], which is
 *    1/size for a texture addressed in texels (format bit 0x40) and 1 otherwise;
 *  - the alpha test runs on colour target 0 before returning (D3D12 has no
 *    fixed-function one). Its state is per draw, so the pipeline key does not
 *    need it.
 * The previous decompiler emitted the scale and test itself; the current one
 * leaves them to the backend, and this path had lost both. Returns 0 on
 * failure (buffer too small or an unexpected program shape). */
static int fp_bind_b1(char* hlsl, size_t cap, u32 cube_mask, int buffered)
{
    static const char decl[] =
        "cbuffer PSConstants : register(b1) {\n"
        "    float4 rsx_texscale[16];\n"
        "    float4 rsx_alphatest;   /* x enable, y reference, z function - 0x200 */\n"
        "    float4 fp_constants[128];\n"
        "};\n"
        "bool rsx_alpha_pass(float a) {\n"
        "    const float r = rsx_alphatest.y; const int f = (int)rsx_alphatest.z;\n"
        "    return f == 1 ? a < r : f == 2 ? a == r : f == 3 ? a <= r :\n"
        "           f == 4 ? a > r : f == 5 ? a != r : f == 6 ? a >= r : f == 7;\n"
        "}\n";
    /* Replace the decompiler's declaration, or add ours in front of main. */
    char* at = strstr(hlsl, "cbuffer PSConstants : register(b1) {");
    char* end = at ? strstr(at, "};\n") : NULL;
    if (at && !end) return 0;
    if (!buffered && at) return 0;
    if (!at) {
        at = strstr(hlsl, "float4 main(");
        if (!at) at = strstr(hlsl, "struct PSOutput {");
        if (!at) return 0;
        end = at;
    } else {
        end += 3;
    }
    {
        const size_t dl = sizeof(decl) - 1, used = strlen(hlsl);
        const size_t old = (size_t)(end - at);
        if (used - old + dl + 1 > cap) return 0;
        memmove(at + dl, end, used - (size_t)(end - hlsl) + 1);
        memcpy(at, decl, dl);
    }
    /* Texel scale on every 2D Sample() of each unit. */
    for (u32 u = 0; u < RSX_FP_UNITS; u++) {
        if ((cube_mask >> u) & 1u) continue;
        /* Every spelling the decompiler emits for a 2D unit: a program with a
         * shadow unit declares its samplers one by one (rsx_sampN). */
        char needle[3][48], ins[32];
        snprintf(needle[0], sizeof needle[0], "rsx_tex[%u].Sample(rsx_samp[%u],", u, u);
        snprintf(needle[1], sizeof needle[1], "rsx_tex%u.Sample(rsx_samp[%u],", u, u);
        snprintf(needle[2], sizeof needle[2], "rsx_tex%u.Sample(rsx_samp%u,", u, u);
        snprintf(ins, sizeof ins, " * rsx_texscale[%u].xy", u);
        for (int k = 0; k < 3; k++) {
            char* from = hlsl;
            char* hit;
            while ((hit = strstr(from, needle[k])) != NULL) {
                char* open = strchr(hit, '(');
                from = open ? hlsl_insert_before_close(hlsl, cap, open, ins) : NULL;
                if (!from) return 0;
            }
        }
    }
    /* Alpha test on target 0, just before the final return. */
    {
        char* ret = NULL;
        for (char* p = strstr(hlsl, "    return "); p; p = strstr(p + 1, "    return "))
            ret = p;
        if (!ret) return 0;
        const char* test = strstr(hlsl, "struct PSOutput")
            ? "    if (rsx_alphatest.x > 0.5 && !rsx_alpha_pass(_out.t0.a)) discard;\n"
            : "    if (rsx_alphatest.x > 0.5 && !rsx_alpha_pass(_o.a)) discard;\n";
        if (!strstr(hlsl, "float4 _o = ") && !strstr(hlsl, "PSOutput _out;")) return 0;
        const size_t tl = strlen(test), used = strlen(hlsl);
        if (used + tl + 1 > cap) return 0;
        memmove(ret + tl, ret, used - (size_t)(ret - hlsl) + 1);
        memcpy(ret, test, tl);
    }
    return 1;
}

/* The HLSL the draw path compiles for a fragment program: decompiled with its
 * inline constants read from b1 (baked in when there are more than b1 holds),
 * then given the b1 layout. The offline precompiler builds the same text, so
 * its disk-cache entries are the ones a draw looks up. ucode must be readable
 * for RSX_FP_MAX_BYTES. Returns the instruction count, <= 0 on failure;
 * fp_addr is only for the log. */
/* Fragment-program variants this title has compiled, by code hash, in
 * <shader cache>/fp_variants.txt ("hash exp32 cube d2c" per line, hex). The
 * texture kinds a program is bound with (cube, shadow, depth-as-colour) are
 * draw state that a Cg binary does not record, so the precompiler reads this
 * list to rebuild every variant a run has needed: a cache emptied by a
 * decompiler or driver update is refilled whole before the next boot. */
typedef struct { u32 hash, exp32, cube, d2c; } FPVariant;
static FPVariant* s_fpv = NULL;
static int s_fpv_n = 0, s_fpv_cap = 0, s_fpv_loaded = 0;
static int fpv_find(u32 hash, u32 exp32, u32 cube, u32 d2c)
{
    for (int i = 0; i < s_fpv_n; i++)
        if (s_fpv[i].hash == hash && s_fpv[i].exp32 == exp32 && s_fpv[i].cube == cube &&
            s_fpv[i].d2c == d2c) return 1;
    return 0;
}
static void fpv_add(u32 hash, u32 exp32, u32 cube, u32 d2c)
{
    if (s_fpv_n == s_fpv_cap) {
        const int cap = s_fpv_cap ? s_fpv_cap * 2 : 256;
        FPVariant* g = (FPVariant*)realloc(s_fpv, sizeof(FPVariant) * (size_t)cap);
        if (!g) return;
        s_fpv = g; s_fpv_cap = cap;
    }
    s_fpv[s_fpv_n].hash = hash; s_fpv[s_fpv_n].exp32 = exp32;
    s_fpv[s_fpv_n].cube = cube; s_fpv[s_fpv_n].d2c = d2c; s_fpv_n++;
}
static void fpv_load(void)
{
    if (s_fpv_loaded) return;
    s_fpv_loaded = 1;
    if (!shader_cache_dir()[0]) return;
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s/fp_variants.txt", shader_cache_dir());
    FILE* f = fopen(path, "r");
    if (!f) return;
    unsigned h, e, c, d;
    while (fscanf(f, "%x %x %x %x", &h, &e, &c, &d) == 4)
        if (!fpv_find(h, e, c, d)) fpv_add(h, e, c, d);
    fclose(f);
}
static void fpv_note(const u8* ucode, u32 exp32, u32 cube, u32 d2c)
{
    fpv_load();
    const u32 hash = rsx_fp_code_hash(ucode, RSX_FP_MAX_BYTES);
    if (!shader_cache_dir()[0] || fpv_find(hash, exp32, cube, d2c)) return;
    fpv_add(hash, exp32, cube, d2c);
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s/fp_variants.txt", shader_cache_dir());
    FILE* f = fopen(path, "a");
    if (f) { fprintf(f, "%08x %x %x %x\n", hash, exp32, cube, d2c); fclose(f); }
}

/* fp_build_hlsl's last program had more constants than b1 holds, so they
 * are in its text: a pipeline the pipeline index cannot make again from
 * the game's files, which hold other values. */
static int s_fp_baked;
static int fp_build_hlsl(const u8* ucode, int exp32, u32 cube_mask, u32 d2c_mask,
                         char* hlsl, size_t cap, u32 fp_addr, int note)
{
    /* Only the units the program samples: the masks come from whatever the
     * draw has bound, and a cube or depth texture left on an unused unit
     * would otherwise change the text (and the cache entry) for nothing. */
    { const u32 used = rsx_fp_texture_units(ucode, RSX_FP_MAX_BYTES);
      cube_mask &= used | (used << 16); d2c_mask &= used; }
    if (note) fpv_note(ucode, exp32 ? 1u : 0u, cube_mask, d2c_mask);
    u32 kcount = 0;
    int n = rsx_fp_decompile_buffered_ex2(ucode, RSX_FP_MAX_BYTES, exp32 ? 0x40u : 0u, cube_mask,
                                          d2c_mask, hlsl, (u32)cap, &kcount);
    int buffered = n > 0 && kcount <= FPCB_KMAX;
    s_fp_baked = n > 0 && !buffered;
    if (n > 0 && !buffered) {
        /* More constants than b1 holds: bake them, as before. */
        static int said = 0;
        if (said < 4 && ++said)
            fprintf(stderr, "[FP] fp=0x%X has %u inline constants (b1 holds %d); "
                            "compiled with them baked in%c", fp_addr, kcount, FPCB_KMAX, 10);
        n = rsx_fp_decompile_ex2(ucode, RSX_FP_MAX_BYTES, exp32 ? 0x40u : 0u, cube_mask,
                                 d2c_mask, hlsl, (u32)cap);
    }
    if (n <= 0) { static int _e=0; if(_e < 16 && ++_e) printf("[FP] decompile fail (fp=0x%08X)\n", fp_addr); return n; }
    if (!fp_bind_b1(hlsl, cap, cube_mask, buffered)) {
        static int _e=0; if(_e < 16 && ++_e) printf("[FP] b1 binding failed (fp=0x%08X)\n", fp_addr);
        return 0;
    }
    return n;
}

/* RSX_WRITE_COLOR_LOG: guest ranges of the fragment programs drawn with,
 * so a write-back that lands on live ucode is reported. */
static u32 s_fp_seen[512][2]; static int s_fp_seen_n;
static void fp_seen_note(u32 ea, u32 len)
{
    for (int i = 0; i < s_fp_seen_n; i++) if (s_fp_seen[i][0] == ea) return;
    if (s_fp_seen_n < 512) { s_fp_seen[s_fp_seen_n][0] = ea; s_fp_seen[s_fp_seen_n][1] = len; s_fp_seen_n++; }
}

/* rsx_fp_code_hash of the program at guest offset `off`, and its size. The
 * PSO lookup needs it for every draw, and hashing (a byte-serial FNV) plus
 * sizing the program was ~12% of the render thread at Boletarian Palace's
 * 6k draws. A program is re-hashed only when its bytes differ from the copy
 * kept with the last hash: equal bytes up to the old size mean the same END
 * instruction, so the same size and the same hash. RSX_FP_HASH_MEMO=0 hashes
 * every time. */
typedef struct { u32 off, size, hash, units; u8* bytes; } FpHashMemo;
static FpHashMemo s_fph[1024];
static u32 fp_hash_at(u32 off, u32* size_out)
{
    extern uint8_t* vm_base;
    const u8* up = vm_base + off;
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("RSX_FP_HASH_MEMO", 1));
    FpHashMemo* m = &s_fph[((off >> 4) * 2654435761u) >> 22];
    if (en && m->bytes && m->off == off && !memcmp(m->bytes, up, m->size)) {
        *size_out = m->size;
        return m->hash;
    }
    u32 usz = rsx_fp_program_size(up, RSX_FP_MAX_BYTES);
    if (usz == 0) usz = 64;
    const u32 h = rsx_fp_code_hash(up, RSX_FP_MAX_BYTES);
    fp_seen_note(off, usz);
    if (en) {
        u8* b = (m->bytes && m->size >= usz) ? m->bytes : (u8*)realloc(m->bytes, usz);
        if (b) { memcpy(b, up, usz); m->bytes = b; m->off = off; m->size = usz; m->hash = h;
                 m->units = rsx_fp_texture_units(up, RSX_FP_MAX_BYTES); }
    }
    *size_out = usz;
    return h;
}
/* The texture units the program at guest offset `off` samples, from
 * fp_hash_at's memo: render_frame binds only those. Every draw looked up all
 * sixteen units' textures (and render targets, and depth surfaces), ~14 a
 * draw at the Palace where its programs sample two or three, and that was
 * most of the frame's SRV time. */
static u32 fp_units_at(u32 off)
{
    extern uint8_t* vm_base;
    u32 usz;
    (void)fp_hash_at(off, &usz);
    const FpHashMemo* m = &s_fph[((off >> 4) * 2654435761u) >> 22];
    if (m->bytes && m->off == off && !memcmp(m->bytes, vm_base + off, m->size))
        return m->units;
    return rsx_fp_texture_units(vm_base + off, RSX_FP_MAX_BYTES);
}
/* RSX_SCALE: sprite edges sampled as at 1x.
 *
 * The HUD and menus are built from quads cut out of texture atlases. At 1x a
 * quad drawn 1:1 puts each pixel centre on a texel centre, so linear filtering
 * never reaches past the sprite's edge. On a target drawn at 2x the outermost
 * pixel centre is a quarter texel from the edge, and the filter blends in the
 * atlas's next sprite: thin bright or dark lines along panel edges, between
 * the pieces of a framed window, around icons.
 *
 * For a draw without a depth test on a scaled target, the pipeline gets a
 * geometry shader that hands each triangle's texture-coordinate bounds to the
 * pixel shader (flat), for triangles whose vertices all have w = 1 (drawn in
 * screen space, as 2D is; for others the bounds are unlimited). The pixel
 * shader then keeps each coordinate half a guest pixel inside those bounds,
 * which is where the 1x pixel centre nearest the edge samples. Half a guest
 * pixel in texture units is half the coordinate's change over one guest pixel:
 * fwidth over a host pixel times the scale. A quad split into two triangles
 * gives both the quad's bounds, so its diagonal is not clamped. Interiors are
 * untouched. RSX_SCALE_UVCLAMP=0 turns it off.
 *
 * The scale reaches the shader as a constant (rsx_alphatest.w, vp_record_cb),
 * not in its text: one shader and one pipeline serve every scale, so a
 * precompile at any scale (rsx_shader_precompile, RSX_PRECOMPILE_2D) builds the
 * shader a scaled draw looks up, and a change of scale compiles nothing. */
/* (dr_uvclamp, which picks the draws, goes with the draw record.) */
/* The geometry shader's source, as uvclamp_gs compiles it (gs_5_0, "main",
 * flags 0, not through the disk cache). */
static const char* uvclamp_gs_src(size_t* len)
{
    static const char src[] =
        "struct V {\n"
        "    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
        "    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
        "    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n"
        "};\n"
        "struct G {\n"
        "    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
        "    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
        "    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n"
        "    nointerpolation float4 b0:UVBOUND0; nointerpolation float4 b1:UVBOUND1;\n"
        "    nointerpolation float4 b2:UVBOUND2; nointerpolation float4 b3:UVBOUND3;\n"
        "    nointerpolation float4 b4:UVBOUND4; nointerpolation float4 b5:UVBOUND5;\n"
        "    nointerpolation float4 b6:UVBOUND6; nointerpolation float4 b7:UVBOUND7;\n"
        "};\n"
        "float4 bnd(float2 a, float2 b, float2 c, bool on) {\n"
        "    return on ? float4(min(min(a, b), c), max(max(a, b), c)) : float4(-1e30, -1e30, 1e30, 1e30);\n"
        "}\n"
        "[maxvertexcount(3)]\n"
        "void main(triangle V v[3], inout TriangleStream<G> s) {\n"
        "    const bool on = v[0].pos.w == 1.0 && v[1].pos.w == 1.0 && v[2].pos.w == 1.0;\n"
        "    G o;\n"
        "    o.b0 = bnd(v[0].t0.xy, v[1].t0.xy, v[2].t0.xy, on); o.b1 = bnd(v[0].t1.xy, v[1].t1.xy, v[2].t1.xy, on);\n"
        "    o.b2 = bnd(v[0].t2.xy, v[1].t2.xy, v[2].t2.xy, on); o.b3 = bnd(v[0].t3.xy, v[1].t3.xy, v[2].t3.xy, on);\n"
        "    o.b4 = bnd(v[0].t4.xy, v[1].t4.xy, v[2].t4.xy, on); o.b5 = bnd(v[0].t5.xy, v[1].t5.xy, v[2].t5.xy, on);\n"
        "    o.b6 = bnd(v[0].t6.xy, v[1].t6.xy, v[2].t6.xy, on); o.b7 = bnd(v[0].t7.xy, v[1].t7.xy, v[2].t7.xy, on);\n"
        "    [unroll] for (int i = 0; i < 3; i++) {\n"
        "        o.pos = v[i].pos; o.col0 = v[i].col0; o.col1 = v[i].col1; o.fog = v[i].fog;\n"
        "        o.t0 = v[i].t0; o.t1 = v[i].t1; o.t2 = v[i].t2; o.t3 = v[i].t3; o.t4 = v[i].t4;\n"
        "        o.t5 = v[i].t5; o.t6 = v[i].t6; o.t7 = v[i].t7; o.t8 = v[i].t8; o.t9 = v[i].t9;\n"
        "        s.Append(o);\n"
        "    }\n"
        "}\n";
    if (len) *len = sizeof src - 1;
    return src;
}

/* The pixel-shader half of the above, on a translated program's HLSL: the
 * bounds as inputs after TEXCOORD9 (declared here if the program did not read
 * it, so the inputs line up with the geometry shader's outputs), and the clamp
 * at the top of main, half the scale from rsx_alphatest.w. Returns 0, leaving
 * hlsl as it was, if an anchor is missing or the buffer is too small. */
static int fp_inject_uvclamp(char* hlsl, size_t cap)
{
    static const char tc7[] =
        "float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n";
    char* in = strstr(hlsl, tc7);
    char* m = strstr(hlsl, "main(PSInput input)");
    char* body = m ? strchr(m, '{') : NULL;
    if (!in || !body || body < in) return 0;
    /* The program's own TEXCOORD8/9, if it reads them, come right after TEXCOORD7. */
    static const char tc9[] = "float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;\n";
    char* after_in = in + sizeof tc7 - 1;
    const char* next = after_in + strspn(after_in, " ");
    const int has9 = !strncmp(next, tc9, sizeof tc9 - 1);
    if (has9) after_in = (char*)next + sizeof tc9 - 1;
    char decl[1024];
    snprintf(decl, sizeof decl, "%s"
        "    nointerpolation float4 uvb0:UVBOUND0; nointerpolation float4 uvb1:UVBOUND1;\n"
        "    nointerpolation float4 uvb2:UVBOUND2; nointerpolation float4 uvb3:UVBOUND3;\n"
        "    nointerpolation float4 uvb4:UVBOUND4; nointerpolation float4 uvb5:UVBOUND5;\n"
        "    nointerpolation float4 uvb6:UVBOUND6; nointerpolation float4 uvb7:UVBOUND7;\n",
        has9 ? "" : "    float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;\n");
    char code[2048];
    int n = snprintf(code, sizeof code, "\n    { const float k = rsx_alphatest.w;   /* half the scale: half a guest pixel, in host pixels */\n");
    for (int i = 0; i < 8; i++)
        n += snprintf(code + n, sizeof code - (size_t)n,
            "      { const float2 h = k * fwidth(input.tc%d.xy);"
            " input.tc%d.xy = clamp(input.tc%d.xy, input.uvb%d.xy + h, input.uvb%d.zw - h); }\n",
            i, i, i, i, i);
    snprintf(code + n, sizeof code - (size_t)n, "    }\n");
    const size_t len = strlen(hlsl), dl = strlen(decl), cl = strlen(code);
    if (len + dl + cl + 1 > cap) return 0;
    const size_t body_off = (size_t)(body + 1 - hlsl) + dl;
    memmove(after_in + dl, after_in, len - (size_t)(after_in - hlsl) + 1);
    memcpy(after_in, decl, dl);
    char* b = hlsl + body_off;
    memmove(b + cl, b, strlen(b) + 1);
    memcpy(b, code, cl);
    return 1;
}

/* The HDR variant of a draw (PSO key): a named scene program drawing into T
 * (its clamp limit's float bits), or a draw into an unpromoted target that
 * samples a promoted one (HDRV_SAT and the units). */
#define HDRV_SAT 0x80000000u

/* The variant's text: each clamp to [0, 1] becomes a clamp to [0, limit], or
 * none for the largest half float. Returns how many it changed. */
static int fp_hdr_unclamp(char* hlsl, size_t cap, u32 hdrv)
{
    static const char sat[] = " _v = saturate(_v);";
    float l;
    memcpy(&l, &hdrv, 4);
    char with[64] = "";
    if (l < 65504.0f) snprintf(with, sizeof with, " _v = clamp(_v, 0.0, %.9g);", l);
    const size_t sl = sizeof sat - 1, wl = strlen(with);
    int k = 0;
    for (char* q = strstr(hlsl, sat); q; q = strstr(q + wl, sat), k++) {
        if (strlen(hlsl) - sl + wl + 1 > cap) break;
        memmove(q + wl, q + sl, strlen(q + sl) + 1);
        memcpy(q, with, wl);
    }
    return k;
}

/* The variant of a draw into an unpromoted target (HDRV_SAT): each sample of
 * the units in mask, promoted targets, clamped to [0, 1]. Returns how many. */
static int fp_hdr_sat_units(char* hlsl, size_t cap, u32 mask)
{
    int k = 0;
    for (u32 u = 0; u < RSX_FP_UNITS; u++) {
        if (!((mask >> u) & 1u)) continue;
        char needle[3][48];
        snprintf(needle[0], sizeof needle[0], "rsx_tex[%u].Sample(rsx_samp[%u],", u, u);
        snprintf(needle[1], sizeof needle[1], "rsx_tex%u.Sample(rsx_samp[%u],", u, u);
        snprintf(needle[2], sizeof needle[2], "rsx_tex%u.Sample(rsx_samp%u,", u, u);
        for (int i = 0; i < 3; i++) {
            char* from = hlsl;
            char* hit;
            while ((hit = strstr(from, needle[i])) != NULL) {
                char* close = hlsl_insert_before_close(hlsl, cap, strchr(hit, '('), "");
                const size_t used = strlen(hlsl);
                if (!close || used + 10 + 1 > cap) return k;
                memmove(close + 2, close + 1, used - (size_t)(close + 1 - hlsl) + 1);   /* ")" after it */
                close[1] = ')';
                memmove(hit + 9, hit, used + 1 - (size_t)(hit - hlsl) + 1);            /* "saturate(" before */
                memcpy(hit, "saturate(", 9);
                from = close + 11;
                k++;
            }
        }
    }
    return k;
}

/* A fragment program's text in HDR variant hdrv (dr_hdrv), as vp_get_fp_pso
 * makes it; the pipeline index's build (rsx_shader_precompile.c) makes its
 * rows' texts with it too. Returns how many clamps or samples it changed. */
int rsxc_shader_hdr(char* hlsl, u32 cap, u32 hdrv)
{
    if (!hlsl || !hdrv) return 0;
    return (hdrv & HDRV_SAT) ? fp_hdr_sat_units(hlsl, cap, hdrv & ~HDRV_SAT) : fp_hdr_unclamp(hlsl, cap, hdrv);
}

/* Hash + compile the CURRENT rsx_state vertex program into the VS cache;
 * returns the cache slot (or -1). Called at draw-record time so each draw
 * carries the VP that was loaded when it was submitted. */
static u32 vp_hash_ucode(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1u;
}

/* vp_get_vs_mode (rsx_d3d12_backend.c) without its cache, in two halves:
 * vp_vs_locate finds the program the state has loaded at its start and keys
 * it, which is all the cache's lookup needs; vp_vs_text makes the text a
 * miss compiles. VP_START_OFF, VP_W_ONE and VP_BYPASS read as they do there.
 * The VS cache, its memo (RSX_VP_MEMO) and the dumps (VP_DUMP, RSX_DBG_VP)
 * stay with the caller. */
static int vp_vs_locate(const rsx_state* st, int fetch, rsxc_vs_program* vp)
{
    extern u32 rsx_vp_program_size_instrs(const u8*, u32);
    if (!st || st->vp_ucode_bytes < 16) return -1;
    /* Start at the instruction SET_TRANSFORM_PROGRAM_START selects, not at 0.
     * The microcode store holds every resident program -- this title keeps its
     * scene, fluid, caustics and droplet programs in it at once -- so
     * decompiling from 0 ran the scene's transform for every draw. That is why
     * the fluid's 114300 vertices rasterized nothing: they were being pushed
     * through the wrong program.
     * VP_START_OFF=1 restores the old always-from-zero behaviour. */
    u32 vstart = 0;
    { static int off = -1; PS3_KNOB(off, getenv("VP_START_OFF") ? 1 : 0);
      if (!off) vstart = st->transform_program_start * 16u; }
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u32 avail = st->vp_ucode_bytes - vstart;
    const u8* vuc = st->vp_ucode + vstart;
    u32 vlen = avail;
    /* Key the program by its own instructions, up to its end bit. The store
     * holds every resident program, so hashing to the end of the store made
     * each program's key change whenever any program after it was uploaded. */
    int vend = 0;
    { const u32 instrs = rsx_vp_program_size_instrs(vuc, vlen);
      if (instrs) { vlen = instrs * 16u; vend = 1; } }
    u32 hash = vp_hash_ucode(vuc, vlen);
    if (fetch) hash ^= 0x9E3779B9u;
    vp->code = vuc;
    vp->vstart = vstart;
    vp->vlen = vlen;
    vp->end = vend;
    vp->hash = hash;
    return 0;
}

static int vp_vs_text(const rsxc_vs_program* vp, int fetch, char* hlsl, u32 cap, int* uses_c03)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    extern int rsx_vp_decompile_fetch(const uint8_t*, u32, char*, u32);
    const u8* vuc = vp->code;
    const u32 vlen = vp->vlen;
    int ni = fetch ? rsx_vp_decompile_fetch(vuc, vlen, hlsl, cap)
                   : rsx_vp_decompile(vuc, vlen, hlsl, cap);
    if (ni <= 0) return ni;
    /* VP_BYPASS=1: replace the guest transform with a direct map of attribute 0
     * into clip space. If geometry appears with this on, everything from the
     * input layout through raster/depth/present is sound and the fault is the
     * transform (constants or the decompiled program); if it stays blank, the
     * fault is upstream of the shader -- the attribute binding itself. Patched
     * in place, space-padded to the original length so offsets stay valid. */
    /* VP_W_ONE=1: keep the guest transform but force the clip-space w to 1.
     * Different from VP_BYPASS, which replaces the whole position: this tests
     * only the hypothesis that w is wrong. This title's VP ends
     *   { float4 _v = (float4)((vp_c[467]).xxxx); o[0].w = _v.w; }
     * with c467 = (-1, 1, 0, 0), so w = -1 and the clipper discards every
     * vertex. Patched in place, space-padded so offsets stay valid. */
    if (getenv("VP_W_ONE")) {
        static const char anchor[] = "o[0].w = _v.w;";
        static const char repl[]   = "o[0].w = 1.0;";
        char* at = hlsl;
        while ((at = strstr(at, anchor)) != NULL) {
            size_t n = sizeof(anchor) - 1, m = sizeof(repl) - 1;
            memcpy(at, repl, m);
            memset(at + m, ' ', n - m);
            at += n;
        }
    }
    if (getenv("VP_BYPASS")) {
        static const char anchor[] =
            "Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);";
        static const char repl[] = "Out.pos = float4(v[0].xy * 0.5, 0.5, 1.0);";
        char* at = hlsl;
        while ((at = strstr(at, anchor)) != NULL) {
            size_t n = sizeof(anchor) - 1, m = sizeof(repl) - 1;
            memcpy(at, repl, m);
            memset(at + m, ' ', n - m);
            at += n;
        }
    }
    if (uses_c03)
        *uses_c03 =
        (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    return ni;
}

/* Offline warm-up of the shader disk cache (rsx_shader_precompile). */
int rsxc_shader_hlsl(int fragment, const u8* ucode, u32 ucode_len, u32 vp_start_slot,
                          int exp32, u32 cube_mask, u32 d2c_mask, char* out, u32 cap)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    extern int rsx_vp_decompile_fetch(const uint8_t*, u32, char*, u32);
    extern u32 rsx_vp_program_size_instrs(const u8*, u32);
    extern void rsx_vp_set_branch_base(u32);
    if (!ucode || !out || cap < 2) return 0;
    if (fragment == 1 || fragment == 3) {
        /* The decompiler may read up to RSX_FP_MAX_BYTES, as it does from
         * guest memory; give it a zero tail. */
        u8* buf = (u8*)calloc(1, RSX_FP_MAX_BYTES);
        if (!buf) return 0;
        memcpy(buf, ucode, ucode_len < RSX_FP_MAX_BYTES ? ucode_len : RSX_FP_MAX_BYTES);
        int n = fp_build_hlsl(buf, exp32, cube_mask, d2c_mask, out, cap, 0, 0);
        free(buf);
        /* 3: as a draw without a depth test on a scaled target has it (the
         * sprite-edge clamp, vp_get_fp_pso). */
        if (fragment == 3 && n > 0 && !fp_inject_uvclamp(out, cap)) n = 0;
        return n;
    }
    /* The binary holds big-endian words; rsx_state keeps the words
     * SET_TRANSFORM_PROGRAM delivered, little-endian. Then as vp_get_vs_mode:
     * trimmed to the END instruction, branch targets made relative to the
     * slot the program is loaded at. */
    u32 vlen = ucode_len & ~3u;
    u8* w = (u8*)malloc(vlen ? vlen : 4);
    if (!w) return 0;
    for (u32 i = 0; i < vlen; i += 4) {
        w[i] = ucode[i + 3]; w[i + 1] = ucode[i + 2]; w[i + 2] = ucode[i + 1]; w[i + 3] = ucode[i];
    }
    { const u32 instrs = rsx_vp_program_size_instrs(w, vlen);
      if (instrs) vlen = instrs * 16u; }
    rsx_vp_set_branch_base(vp_start_slot);
    const int n = fragment == 2 ? rsx_vp_decompile_fetch(w, vlen, out, cap)
                                : rsx_vp_decompile(w, vlen, out, cap);
    rsx_vp_set_branch_base(0);
    free(w);
    return n;
}

/* The pipeline index's fingerprint of a program, from its ucode as a Cg
 * binary holds it (big-endian words): what pso_index_note takes from the
 * guest. A fragment program: rsx_fp_code_hash64. A vertex program: FNV-1a 64
 * of its words as the transform program store holds them (little-endian), up
 * to its END instruction. */
u64 rsxc_program_hash64(int fragment, const u8* ucode, u32 ucode_len)
{
    extern u32 rsx_vp_program_size_instrs(const u8*, u32);
    if (!ucode || !ucode_len) return 0;
    if (fragment) {
        u8* buf = (u8*)calloc(1, RSX_FP_MAX_BYTES);
        if (!buf) return 0;
        memcpy(buf, ucode, ucode_len < RSX_FP_MAX_BYTES ? ucode_len : RSX_FP_MAX_BYTES);
        const u64 h = rsx_fp_code_hash64(buf, RSX_FP_MAX_BYTES);
        free(buf);
        return h;
    }
    u32 vlen = ucode_len & ~3u;
    u8* w = (u8*)malloc(vlen ? vlen : 4);
    if (!w) return 0;
    for (u32 i = 0; i < vlen; i += 4) {
        w[i] = ucode[i + 3]; w[i + 1] = ucode[i + 2]; w[i + 2] = ucode[i + 1]; w[i + 3] = ucode[i];
    }
    { const u32 instrs = rsx_vp_program_size_instrs(w, vlen);
      if (instrs) vlen = instrs * 16u; }
    const u64 h = fnv_bytes(14695981039346656037ull, w, vlen);
    free(w);
    return h;
}

u64 rsxc_shader_key(int fragment, const char* hlsl)
{
    const char* profile = fragment ? "ps_5_0" : "vs_5_0";
    return hlsl ? shader_key(hlsl, strlen(hlsl), profile) : 0;
}

int rsxc_shader_fp_variants(const u8* ucode, u32 ucode_len, u32* out, int max)
{
    u8* buf = (u8*)calloc(1, RSX_FP_MAX_BYTES);
    if (!buf) return 0;
    memcpy(buf, ucode, ucode_len < RSX_FP_MAX_BYTES ? ucode_len : RSX_FP_MAX_BYTES);
    const u32 hash = rsx_fp_code_hash(buf, RSX_FP_MAX_BYTES);
    free(buf);
    fpv_load();
    int n = 0;
    for (int i = 0; i < s_fpv_n && n < max; i++)
        if (s_fpv[i].hash == hash) {
            out[n * 3 + 0] = s_fpv[i].exp32; out[n * 3 + 1] = s_fpv[i].cube;
            out[n * 3 + 2] = s_fpv[i].d2c; n++;
        }
    return n;
}

/* The draw path's entry points (rsxc_shader.h), over the copies above. */
int rsxc_fp_build_hlsl(const u8* ucode, int exp32, u32 cube_mask, u32 d2c_mask,
                       char* hlsl, size_t cap, u32 fp_addr, int note)
{
    return fp_build_hlsl(ucode, exp32, cube_mask, d2c_mask, hlsl, cap, fp_addr, note);
}
int rsxc_fp_baked(void) { return s_fp_baked; }
int rsxc_fp_inject_uvclamp(char* hlsl, size_t cap) { return fp_inject_uvclamp(hlsl, cap); }
const char* rsxc_uvclamp_gs_hlsl(size_t* len) { return uvclamp_gs_src(len); }
char* rsxc_fp_out_anchor(char* hlsl, char** end, int want_stmt) { return fp_out_anchor(hlsl, end, want_stmt); }
int rsxc_fp_set_out(char* hlsl, size_t cap, const char* expr) { return fp_set_out(hlsl, cap, expr); }
u32 rsxc_fp_hash_at(u32 off, u32* size_out) { return fp_hash_at(off, size_out); }
u32 rsxc_fp_units_at(u32 off) { return fp_units_at(off); }
int rsxc_fp_seen(int i, u32* ea, u32* len)
{
    if (i < 0 || i >= s_fp_seen_n) return s_fp_seen_n;
    if (ea) *ea = s_fp_seen[i][0];
    if (len) *len = s_fp_seen[i][1];
    return s_fp_seen_n;
}
int rsxc_vs_locate(const struct rsx_state* st, int fetch, rsxc_vs_program* vp)
{
    return vp ? vp_vs_locate(st, fetch, vp) : -1;
}
int rsxc_vs_text(const rsxc_vs_program* vp, int fetch, char* hlsl, u32 cap, int* uses_c03)
{
    return vp && vp->code && hlsl ? vp_vs_text(vp, fetch, hlsl, cap, uses_c03) : -1;
}
u64 rsxc_vs_code64(const rsxc_vs_program* vp)
{
    return vp && vp->code ? fnv_bytes(14695981039346656037ull, vp->code, vp->vlen) : 0;
}

#else
typedef int rsxc_shader_off;   /* (a file with no declaration is not ISO C) */
#endif /* PS3RECOMP_RSX_HAL */
