/*
 * ps3recomp - the spv target's text edits and SPIR-V passes (gpu_shader_spv.h)
 *
 * Plain C written as C++, beside gpu_shader_spv.cpp (glslang): nothing here
 * needs glslang or SPIRV-Tools, so the passes read and write SPIR-V words
 * themselves. Every pass is deterministic: the same input gives the same
 * bytes.
 *
 * The idioms E2 rewrites, and the ones it leaves (the decompilers emit no
 * isnan, isinf or isfinite):
 *   - rsx_fp_decompiler.c, the colour output's NaN guard:
 *       (_o == _o) ? _o : (float4)0   and, per target, (_oN == _oN) ? _oN : ...
 *     A NaN test by self-compare, which a fast-math pass folds to true.
 *     -> gpu_spv_isnum(_o), (asuint(x) & 0x7FFFFFFF) <= 0x7F800000.
 *   - rsx_vp_decompiler.c, vp_mul1 (RSX's 0 * anything = 0, Inf and NaN too):
 *       (a == 0.0 || b == 0.0) ? 0.0 : a * b
 *     Folded to a * b by a pass that assumes no Inf or NaN.
 *     -> gpu_spv_iszero(a) || gpu_spv_iszero(b), (asuint(x) << 1) == 0.
 *   - rsx_fp_decompiler.c, DIVSQ (0 where the numerator is 0, so 0 / 0 gives
 *     no NaN):
 *       (abs(X) > 0.0 ? (X) / sqrt(abs((Y).x)) : (float4)0.0)
 *     -> gpu_spv_nonzero(X): not zero and not NaN, as abs(X) > 0.0 says.
 *   Left as they are: LIF's (a).y > 0.0 ? exp2(..) : 0.0 and LIT's x > 0.0
 *   (a compare choosing between two values, nothing to fold); the condition
 *   code select (_p.x != 0.0) ? .. (_p is exactly 0 or 1); the alpha test and
 *   SLT/SGE/SEQ... (plain compares, kept IEEE by the float controls); RSQ/LOG's
 *   max(abs(x), 1e-10), RCC's clamp and saturate, whose NaN case glslang's
 *   nan-clamp option makes D3D's (the non-NaN operand).
 * The bit tests are the IEEE meaning of the float compares under the module's
 * DenormPreserve: a denormal is neither zero nor NaN.
 *
 * The SPIR-V passes: gpu_spv_apply_layout (a module's sets and bindings from
 * the registers its bindings say, by the layout) and gpu_spv_post (the
 * post-passes, on a module as glslang made it: the cache's, never one they
 * already ran on, as the base vertex pass would subtract twice).
 *
 * Built with PS3RECOMP_RSX_HAL and PS3RECOMP_RSX_HAL_SPV.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(PS3RECOMP_RSX_HAL_SPV)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_shader_spv.h"

namespace {

/* ---- a growing text ---- */

struct Text {
    char* p;
    size_t n, cap;
    bool bad;
};

void text_put(Text* t, const char* s, size_t n)
{
    if (t->bad) return;
    if (t->n + n + 1 > t->cap) {
        size_t c = t->cap ? t->cap : 4096;
        while (t->n + n + 1 > c) c *= 2;
        char* g = (char*)realloc(t->p, c);
        if (!g) { t->bad = true; return; }
        t->p = g;
        t->cap = c;
    }
    memcpy(t->p + t->n, s, n);
    t->n += n;
    t->p[t->n] = 0;
}
void text_puts(Text* t, const char* s) { text_put(t, s, strlen(s)); }

/* The text as a fresh Text (src need not end in NUL). */
bool text_from(Text* t, const char* src, size_t n)
{
    memset(t, 0, sizeof *t);
    text_put(t, src, n);
    if (!t->p) text_put(t, "", 0);
    return !t->bad;
}

bool is_id(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool starts(const char* s, const char* end, const char* lit)
{
    const size_t n = strlen(lit);
    return (size_t)(end - s) >= n && !memcmp(s, lit, n);
}

/* ---- E1: locations by semantic ---- */

/* The location of a varying or vertex input semantic, -1 for any other. */
int semantic_location(gpu_sc_stage stage, const char* s, size_t n)
{
    size_t k = 0;
    char name[16];
    while (k < n && k < sizeof name - 1 && ((s[k] >= 'a' && s[k] <= 'z') || (s[k] >= 'A' && s[k] <= 'Z'))) {
        name[k] = (char)(s[k] >= 'a' ? s[k] - 32 : s[k]);
        k++;
    }
    name[k] = 0;
    if (k == n || !(s[k] >= '0' && s[k] <= '9')) {
        if (k != n) return -1;
    }
    int idx = 0, digits = 0;
    for (size_t j = k; j < n; j++) {
        if (s[j] < '0' || s[j] > '9' || digits > 2) return -1;
        idx = idx * 10 + (s[j] - '0');
        digits++;
    }
    if (!strcmp(name, "COLOR")) return idx <= 1 ? idx : -1;
    if (!strcmp(name, "FOG")) return idx == 0 ? 2 : -1;
    if (!strcmp(name, "TEXCOORD")) return idx <= 9 ? 3 + idx : -1;
    if (!strcmp(name, "UVBOUND")) return idx <= 7 ? 13 + idx : -1;
    if (!strcmp(name, "ATTR")) return stage == GPU_SC_VS && idx <= 15 ? idx : -1;
    return -1;
}

/* Every `name : SEMANTIC` followed by ';', ',' or ')' gets [[vk::location(n)]]
 * in front of its declaration (after the '{', ';', '(' or ',' before it),
 * unless that declaration already has an attribute. */
bool edit_e1(gpu_sc_stage stage, const Text* in, Text* out)
{
    const char* s = in->p;
    const char* end = s + in->n;
    const char* copied = s;
    memset(out, 0, sizeof *out);
    for (const char* c = s; c < end; c++) {
        if (*c != ':' || (c + 1 < end && c[1] == ':') || (c > s && c[-1] == ':')) continue;
        /* the semantic */
        const char* a = c + 1;
        while (a < end && is_space(*a)) a++;
        const char* b = a;
        while (b < end && is_id(*b)) b++;
        if (b == a) continue;
        const char* f = b;
        while (f < end && is_space(*f)) f++;
        if (f == end || (*f != ';' && *f != ',' && *f != ')')) continue;
        const int loc = semantic_location(stage, a, (size_t)(b - a));
        if (loc < 0) continue;
        /* the declared name, then the start of its declaration */
        const char* e = c;
        while (e > s && is_space(e[-1])) e--;
        if (e > s && e[-1] == ']') {
            int depth = 0;
            while (e > s) {
                e--;
                if (*e == ']') depth++;
                else if (*e == '[' && --depth == 0) break;
            }
            while (e > s && is_space(e[-1])) e--;
        }
        const char* id = e;
        while (id > s && is_id(id[-1])) id--;
        if (id == e) continue;
        const char* d = id;
        while (d > s && !strchr("{};(,", d[-1])) d--;
        while (d < id && is_space(*d)) d++;
        bool has_attr = false;
        for (const char* q = d; q + 1 < id; q++)
            if (q[0] == '[' && q[1] == '[') { has_attr = true; break; }
        if (has_attr || d < copied) continue;
        char attr[40];
        snprintf(attr, sizeof attr, "[[vk::location(%d)]] ", loc);
        text_put(out, copied, (size_t)(d - copied));
        text_puts(out, attr);
        copied = d;
    }
    text_put(out, copied, (size_t)(end - copied));
    return !out->bad;
}

/* ---- E2: NaN and zero guards as bit tests ---- */

size_t id_len(const char* s, const char* end)
{
    const char* p = s;
    while (p < end && is_id(*p)) p++;
    return (s < end && !(*s >= '0' && *s <= '9')) ? (size_t)(p - s) : 0;
}

/* The ')' that closes the '(' at open, or NULL. */
const char* close_of(const char* open, const char* end)
{
    int depth = 0;
    for (const char* p = open; p < end; p++) {
        if (*p == '(') depth++;
        else if (*p == ')' && --depth == 0) return p;
    }
    return NULL;
}

enum { E2_ISNUM = 1, E2_ISZERO = 2, E2_NONZERO = 4 };

bool edit_e2(const Text* in, Text* out)
{
    const char* s = in->p;
    const char* end = s + in->n;
    const char* copied = s;
    unsigned used = 0;
    Text body;
    memset(&body, 0, sizeof body);
    for (const char* c = s; c < end; c++) {
        if (*c != '(') continue;
        const char* x = c + 1;
        /* (X == X) ? X : */
        size_t xl = id_len(x, end);
        if (xl) {
            const char* p = x + xl;
            if (starts(p, end, " == ") && (size_t)(end - (p + 4)) > 2 * xl + 7 && !memcmp(p + 4, x, xl) &&
                starts(p + 4 + xl, end, ") ? ") && !memcmp(p + 4 + xl + 4, x, xl) &&
                starts(p + 4 + xl + 4 + xl, end, " : ") && (c == s || !is_id(c[-1]))) {
                text_put(&body, copied, (size_t)(c - copied));
                text_puts(&body, "gpu_spv_isnum(");
                text_put(&body, x, xl);
                text_puts(&body, ")");
                copied = p + 4 + xl + 1;   /* past ")" */
                c = copied - 1;
                used |= E2_ISNUM;
                continue;
            }
            /* (A == 0.0 || B == 0.0) ? 0.0 : A * B */
            if (starts(p, end, " == 0.0 || ")) {
                const char* y = p + 11;
                const size_t yl = id_len(y, end);
                const char* q = y + yl;
                if (yl && starts(q, end, " == 0.0) ? 0.0 : ") &&
                    (size_t)(end - (q + 17)) >= xl + 3 + yl && !memcmp(q + 17, x, xl) &&
                    !memcmp(q + 17 + xl, " * ", 3) && !memcmp(q + 17 + xl + 3, y, yl) &&
                    (q + 17 + xl + 3 + yl == end || !is_id(q[17 + xl + 3 + yl]))) {
                    text_put(&body, copied, (size_t)(c - copied));
                    text_puts(&body, "(gpu_spv_iszero(");
                    text_put(&body, x, xl);
                    text_puts(&body, ") || gpu_spv_iszero(");
                    text_put(&body, y, yl);
                    text_puts(&body, "))");
                    copied = q + 8;   /* past " == 0.0)" */
                    c = copied - 1;
                    used |= E2_ISZERO;
                    continue;
                }
            }
        }
        /* (abs(X) > 0.0 ? (X) / sqrt(abs(( */
        if (starts(x, end, "abs(")) {
            const char* open = x + 3;
            const char* cl = close_of(open, end);
            if (!cl) continue;
            const char* X = open + 1;
            const size_t n = (size_t)(cl - X);
            const char* p = cl + 1;
            if (starts(p, end, " > 0.0 ? (") && (size_t)(end - (p + 10)) > n && !memcmp(p + 10, X, n) &&
                starts(p + 10 + n, end, ") / sqrt(abs((")) {
                text_put(&body, copied, (size_t)(x - copied));
                text_puts(&body, "gpu_spv_nonzero(");
                text_put(&body, X, n);
                text_puts(&body, ")");
                copied = p + 6;   /* past " > 0.0" */
                c = copied - 1;
                used |= E2_NONZERO;
                continue;
            }
        }
    }
    text_put(&body, copied, (size_t)(end - copied));
    memset(out, 0, sizeof *out);
    if (used & E2_ISNUM)
        text_puts(out, "bool4 gpu_spv_isnum(float4 x) { return (asuint(x) & 0x7FFFFFFFu) <= 0x7F800000u; }\n");
    if (used & E2_ISZERO)
        text_puts(out, "bool gpu_spv_iszero(float x) { return (asuint(x) << 1) == 0u; }\n");
    if (used & E2_NONZERO)
        text_puts(out,
            "bool gpu_spv_nonzero(float x) { return (asuint(x) & 0x7FFFFFFFu) - 1u < 0x7F800000u; }\n"
            "bool4 gpu_spv_nonzero(float4 x) { return (asuint(x) & 0x7FFFFFFFu) - 1u < 0x7F800000u; }\n");
    text_put(out, body.p ? body.p : "", body.n);
    free(body.p);
    return !out->bad && !body.bad;
}

/* ---- E3: the vertex fetch's loads kept inside t20 ---- */

/* glslang makes a ByteAddressBuffer's GetDimensions OpArrayLength: its
 * length in uints, where D3D12's is in bytes. So the bounds are in words
 * (version 1 took them as bytes and read 0 past a quarter of t20's range:
 * the far half of a Demon's Souls frame's fetch ring). */
bool edit_e3(const Text* in, Text* out)
{
    static const char decl[] = "ByteAddressBuffer vf_raw : register(t20);\n";
    static const char helpers[] =
        "uint gpu_spv_vf_ld(uint a) { uint n; vf_raw.GetDimensions(n); uint w = a >> 2;\n"
        "    return (n >= 1u && w <= n - 1u) ? vf_raw.Load(min(w, max(n, 1u) - 1u) << 2) : 0u; }\n"
        "uint4 gpu_spv_vf_ld4(uint a) { uint n; vf_raw.GetDimensions(n); uint w = a >> 2;\n"
        "    return (n >= 4u && w <= n - 4u) ? vf_raw.Load4(min(w, max(n, 4u) - 4u) << 2) : (uint4)0; }\n";
    const char* s = in->p;
    const char* end = s + in->n;
    const char* at = strstr(s, decl);
    memset(out, 0, sizeof *out);
    if (!at) {
        text_put(out, s, in->n);
        return !out->bad;
    }
    const char* after = at + sizeof decl - 1;
    text_put(out, s, (size_t)(after - s));
    text_puts(out, helpers);
    const char* copied = after;
    for (const char* c = after; c < end; c++) {
        if (starts(c, end, "vf_raw.Load4(")) {
            text_put(out, copied, (size_t)(c - copied));
            text_puts(out, "gpu_spv_vf_ld4(");
            copied = c + 13;
            c = copied - 1;
        } else if (starts(c, end, "vf_raw.Load(")) {
            text_put(out, copied, (size_t)(c - copied));
            text_puts(out, "gpu_spv_vf_ld(");
            copied = c + 12;
            c = copied - 1;
        }
    }
    text_put(out, copied, (size_t)(end - copied));
    return !out->bad;
}

/* ---- E4: the layout's CONST32 cbuffers as one push constant block ---- */

/* Past spaces and comments. */
const char* skip_ws(const char* p, const char* end)
{
    for (;;) {
        while (p < end && is_space(*p)) p++;
        if (starts(p, end, "//")) {
            while (p < end && *p != '\n') p++;
        } else if (starts(p, end, "/*")) {
            const char* q = p + 2;
            while (q + 1 < end && !(q[0] == '*' && q[1] == '/')) q++;
            p = q + 1 < end ? q + 2 : end;
        } else {
            return p;
        }
    }
}

/* A member type's components, 1-4, for float, int, uint and bool (and their
 * vectors); 0 for any other. */
int member_comps(const char* t, size_t n)
{
    static const char* const base[] = { "float", "int", "uint", "bool" };
    for (const char* b : base) {
        const size_t bl = strlen(b);
        if (n < bl || memcmp(t, b, bl)) continue;
        if (n == bl) return 1;
        if (n == bl + 1 && t[bl] >= '1' && t[bl] <= '4') return t[bl] - '0';
    }
    return 0;
}

/* 1 if the identifier name appears in [s, end) outside [skip0, skip1). */
bool named_outside(const char* s, const char* end, const char* name, size_t nl, const char* skip0,
                   const char* skip1)
{
    for (const char* p = s; p + nl <= end; p++) {
        if (p >= skip0 && p < skip1) { p = skip1 - 1; continue; }
        if (*p != *name || memcmp(p, name, nl)) continue;
        if ((p > s && is_id(p[-1])) || (p + nl < end && is_id(p[nl]))) continue;
        return true;
    }
    return false;
}

struct PushMember {
    const char* type;
    size_t type_len;
    const char* name;
    size_t name_len;
    unsigned array;    /* elements; 0: not an array */
    unsigned offset;   /* in the block */
};

/* A `cbuffer <name> : register(b<reg>) {` at c: its register, the body's
 * '{' and '}', and the end of the declaration (past its ';'). false for
 * any other form. */
bool cbuffer_at(const char* s, const char* c, const char* end, unsigned* reg, const char** body0,
                const char** body1, const char** decl_end)
{
    if (!starts(c, end, "cbuffer") || (c > s && is_id(c[-1])) || c + 7 >= end || !is_space(c[7])) return false;
    const char* p = skip_ws(c + 7, end);
    const size_t nl = id_len(p, end);
    if (!nl) return false;
    p = skip_ws(p + nl, end);
    if (p >= end || *p != ':') return false;
    p = skip_ws(p + 1, end);
    if (!starts(p, end, "register(")) return false;
    p = skip_ws(p + 9, end);
    if (p >= end || *p != 'b') return false;
    p++;
    unsigned r = 0;
    const char* d = p;
    while (p < end && *p >= '0' && *p <= '9' && p - d < 3) r = r * 10u + (unsigned)(*p++ - '0');
    if (p == d) return false;
    p = skip_ws(p, end);
    if (p >= end || *p != ')') return false;
    p = skip_ws(p + 1, end);
    if (p >= end || *p != '{') return false;
    const char* b0 = p;
    const char* b1 = b0 + 1;
    while (b1 < end && *b1 != '}' && *b1 != '{') b1++;
    if (b1 >= end || *b1 != '}') return false;
    p = skip_ws(b1 + 1, end);
    if (p < end && *p == ';') p++;
    *reg = r;
    *body0 = b0;
    *body1 = b1;
    *decl_end = p;
    return true;
}

/* A cbuffer body's members, at their HLSL offsets from base: a vector does
 * not cross a 16-byte row, an array starts a row and takes one per element. */
bool cbuffer_members(const char* b0, const char* b1, unsigned base, unsigned cap_bytes, unsigned reg,
                     PushMember* m, int* nm, int max, char* err, size_t err_cap)
{
    unsigned off = 0;
    const char* p = skip_ws(b0 + 1, b1);
    while (p < b1) {
        const size_t tl = id_len(p, b1);
        const int comps = tl ? member_comps(p, tl) : 0;
        if (!comps) {
            if (err && err_cap) snprintf(err, err_cap, "E4: b%u: a member of type %.*s", reg, (int)(tl ? tl : 1), p);
            return false;
        }
        const char* type = p;
        p = skip_ws(p + tl, b1);
        for (;;) {
            const size_t nl = id_len(p, b1);
            if (!nl || *nm >= max) {
                if (err && err_cap) snprintf(err, err_cap, "E4: b%u: a declaration E4 does not know", reg);
                return false;
            }
            PushMember* pm = &m[(*nm)++];
            pm->type = type;
            pm->type_len = tl;
            pm->name = p;
            pm->name_len = nl;
            pm->array = 0;
            p = skip_ws(p + nl, b1);
            if (p < b1 && *p == '[') {
                const char* d = p = skip_ws(p + 1, b1);
                while (p < b1 && *p >= '0' && *p <= '9' && p - d < 4) pm->array = pm->array * 10u + (unsigned)(*p++ - '0');
                p = skip_ws(p, b1);
                if (p == d || p >= b1 || *p != ']' || !pm->array) {
                    if (err && err_cap) snprintf(err, err_cap, "E4: b%u: %.*s's size", reg, (int)nl, pm->name);
                    return false;
                }
                p = skip_ws(p + 1, b1);
            }
            const unsigned size = 4u * (unsigned)comps;
            if (pm->array) {
                off = (off + 15u) & ~15u;
                pm->offset = base + off;
                off += 16u * (pm->array - 1u) + size;
            } else {
                if (off / 16u != (off + size - 1u) / 16u) off = (off + 15u) & ~15u;
                pm->offset = base + off;
                off += size;
            }
            if (p < b1 && *p == ',') { p = skip_ws(p + 1, b1); continue; }
            if (p < b1 && *p == ';') { p = skip_ws(p + 1, b1); break; }
            if (err && err_cap) snprintf(err, err_cap, "E4: b%u: %.*s has a qualifier E4 does not know", reg, (int)nl,
                                         pm->name);
            return false;
        }
    }
    if (off > cap_bytes) {
        if (err && err_cap) snprintf(err, err_cap, "E4: b%u's members take %u bytes, its slot %u", reg, off, cap_bytes);
        return false;
    }
    return true;
}

/* Every cbuffer at a CONST32 slot's register becomes a member list of one
 * [[vk::push_constant]] block, at the place of the first, each member at
 * packoffset(4 x the counts of the CONST32 slots before its own + its HLSL
 * offset). A member the text never names elsewhere is padding and is left
 * out. glslang lays the block out by std430 and takes packoffset only where
 * std430 aligns too (a vector of 3 or 4 at 16 bytes): E4 fails on a member
 * it would move, and on an array of fewer than 4 components (HLSL's stride
 * is 16 bytes, std430's the element). */
bool edit_e4(const Text* in, Text* out, const gpu_layout_desc* L, char* err, size_t err_cap)
{
    enum { MAX_M = 64, MAX_CB = GPU_MAX_SLOTS };
    const char* s = in->p;
    const char* end = s + in->n;
    memset(out, 0, sizeof *out);
    unsigned base[MAX_CB], cap[MAX_CB], regs[MAX_CB];
    int nslot = 0;
    unsigned at = 0;
    for (uint32_t i = 0; i < L->nslots && i < GPU_MAX_SLOTS; i++) {
        if (L->slot[i].kind != GPU_SLOT_CONST32) continue;
        regs[nslot] = L->slot[i].reg;
        base[nslot] = at;
        cap[nslot] = 4u * L->slot[i].count;
        at += cap[nslot];
        nslot++;
    }
    /* The declarations, in slot order. */
    const char* d0[MAX_CB] = { 0 };
    const char* d1[MAX_CB] = { 0 };
    const char* bb0[MAX_CB] = { 0 };
    const char* bb1[MAX_CB] = { 0 };
    for (const char* c = s; nslot && c < end; c++) {
        unsigned reg;
        const char *b0, *b1, *de;
        if (*c != 'c' || !cbuffer_at(s, c, end, &reg, &b0, &b1, &de)) continue;
        for (int k = 0; k < nslot; k++) {
            if (regs[k] != reg) continue;
            if (d0[k]) {
                if (err && err_cap) snprintf(err, err_cap, "E4: b%u declared twice", reg);
                return false;
            }
            d0[k] = c;
            d1[k] = de;
            bb0[k] = b0;
            bb1[k] = b1;
        }
        c = de - 1;
    }
    PushMember m[MAX_M];
    int nm = 0;
    const char* first = NULL;
    for (int k = 0; k < nslot; k++) {
        if (!d0[k]) continue;
        if (!first || d0[k] < first) first = d0[k];
        const int from = nm;
        if (!cbuffer_members(bb0[k], bb1[k], base[k], cap[k], regs[k], m, &nm, MAX_M, err, err_cap)) return false;
        /* padding out, then std430's alignment */
        int keep = from;
        for (int j = from; j < nm; j++) {
            if (!named_outside(s, end, m[j].name, m[j].name_len, d0[k], d1[k])) continue;
            const int comps = member_comps(m[j].type, m[j].type_len);
            const unsigned align = comps == 1 ? 4u : comps == 2 ? 8u : 16u;
            if ((m[j].array && comps != 4) || (m[j].offset % align)) {
                if (err && err_cap)
                    snprintf(err, err_cap, "E4: b%u: %.*s at %u is not where std430 can put it", regs[k],
                             (int)m[j].name_len, m[j].name, m[j].offset);
                return false;
            }
            m[keep++] = m[j];
        }
        nm = keep;
    }
    if (!first) {
        text_put(out, s, in->n);
        return !out->bad;
    }
    const char* copied = s;
    for (;;) {
        /* the next declaration after copied */
        int k = -1;
        for (int j = 0; j < nslot; j++)
            if (d0[j] && d0[j] >= copied && (k < 0 || d0[j] < d0[k])) k = j;
        if (k < 0) break;
        text_put(out, copied, (size_t)(d0[k] - copied));
        if (d0[k] == first && nm) {
            text_puts(out, "[[vk::push_constant]] cbuffer gpu_spv_push {\n");
            for (int j = 0; j < nm; j++) {
                char line[192];
                if (m[j].array)
                    snprintf(line, sizeof line, "    %.*s %.*s[%u] : packoffset(c%u.%c);\n", (int)m[j].type_len,
                             m[j].type, (int)m[j].name_len, m[j].name, m[j].array, m[j].offset / 16u,
                             "xyzw"[(m[j].offset % 16u) / 4u]);
                else
                    snprintf(line, sizeof line, "    %.*s %.*s : packoffset(c%u.%c);\n", (int)m[j].type_len,
                             m[j].type, (int)m[j].name_len, m[j].name, m[j].offset / 16u,
                             "xyzw"[(m[j].offset % 16u) / 4u]);
                text_puts(out, line);
            }
            text_puts(out, "};");
        }
        copied = d1[k];
    }
    text_put(out, copied, (size_t)(end - copied));
    return !out->bad;
}

/* ---- E5: texture and sampler arrays as one declaration per unit ---- */

/* `<type> <name>[N] : register(<r>B);` becomes N declarations
 * `<type> <name><i> : register(<r>B+i);`, and each `<name>[i]` `<name><i>`. */
bool edit_e5_one(const Text* in, Text* out, const char* name, char err_reg, char* err, size_t err_cap)
{
    const size_t nl = strlen(name);
    const char* s = in->p;
    const char* end = s + in->n;
    const char* copied = s;
    memset(out, 0, sizeof *out);
    for (const char* c = s; c < end; c++) {
        if (!starts(c, end, name) || c[nl] != '[' || (c > s && is_id(c[-1]))) continue;
        const char* d = c + nl + 1;
        unsigned idx = 0;
        const char* q = d;
        while (q < end && *q >= '0' && *q <= '9' && q - d < 4) idx = idx * 10u + (unsigned)(*q++ - '0');
        if (q == d || q >= end || *q != ']') {
            if (err && err_cap) snprintf(err, err_cap, "E5: %s indexed by an expression", name);
            return false;
        }
        const char* r = q + 1;
        while (r < end && is_space(*r)) r++;
        if (r < end && *r == ':') {
            /* the declaration: its type is the word before the name */
            const char* t1 = c;
            while (t1 > s && is_space(t1[-1])) t1--;
            const char* t0 = t1;
            while (t0 > s && (is_id(t0[-1]) || t0[-1] == '<' || t0[-1] == '>')) t0--;
            const char* reg = strstr(r, "register(");
            const char* semi = strchr(r, ';');
            if (t0 == t1 || !reg || !semi || reg > semi || reg[9] != err_reg) {
                if (err && err_cap) snprintf(err, err_cap, "E5: %s's declaration has an unknown form", name);
                return false;
            }
            const unsigned base = (unsigned)strtoul(reg + 10, NULL, 10);
            if (idx == 0 || idx > 64) {
                if (err && err_cap) snprintf(err, err_cap, "E5: %s[%u]", name, idx);
                return false;
            }
            text_put(out, copied, (size_t)(t0 - copied));
            for (unsigned i = 0; i < idx; i++) {
                char line[128];
                snprintf(line, sizeof line, "%.*s %s%u : register(%c%u);%s", (int)(t1 - t0), t0, name, i, err_reg,
                         base + i, i + 1 < idx ? "\n" : "");
                text_puts(out, line);
            }
            copied = semi + 1;
            c = semi;
            continue;
        }
        /* a use */
        char repl[48];
        snprintf(repl, sizeof repl, "%s%u", name, idx);
        text_put(out, copied, (size_t)(c - copied));
        text_puts(out, repl);
        copied = q + 1;
        c = q;
    }
    text_put(out, copied, (size_t)(end - copied));
    return !out->bad;
}

bool edit_e5(const Text* in, Text* out, char* err, size_t err_cap)
{
    Text mid;
    if (!edit_e5_one(in, &mid, "rsx_tex", 't', err, err_cap)) { free(mid.p); return false; }
    const bool ok = edit_e5_one(&mid, out, "rsx_samp", 's', err, err_cap);
    free(mid.p);
    return ok;
}

} // namespace

extern "C" const gpu_layout_desc* gpu_spv_guest_layout(void)
{
    /* gpu.h: 0 CB b0 (vertex), 1 TABLE_TEX t0-t15 (pixel), 2 CB b1 (pixel),
     * 3 TABLE_SAMPLER s0-s15 (pixel), 4 RAW t20 (vertex), 5 CONST32 b2 x1 (vertex). */
    static const gpu_layout_desc guest = {
        GPU_LAYOUT_VERTEX_INPUT, 6,
        { { GPU_SLOT_CB, 0, 0, GPU_VIS_VERTEX }, { GPU_SLOT_TABLE_TEX, 0, 16, GPU_VIS_PIXEL },
          { GPU_SLOT_CB, 1, 0, GPU_VIS_PIXEL }, { GPU_SLOT_TABLE_SAMPLER, 0, 16, GPU_VIS_PIXEL },
          { GPU_SLOT_RAW, 20, 0, GPU_VIS_VERTEX }, { GPU_SLOT_CONST32, 2, 1, GPU_VIS_VERTEX } },
        0, {}
    };
    return &guest;
}

extern "C" char* gpu_spv_edit(gpu_sc_stage stage, const char* src, size_t n, unsigned edits,
                              const gpu_layout_desc* layout, size_t* len, char* err, size_t err_cap)
{
    if (err && err_cap) err[0] = 0;
    if (len) *len = 0;
    if (!src) return NULL;
    if (!layout) layout = gpu_spv_guest_layout();
    Text cur;
    if (!text_from(&cur, src, n)) { free(cur.p); return NULL; }
    for (unsigned bit = 1; bit <= GPU_SPV_E5; bit <<= 1) {
        if (!(edits & bit)) continue;
        Text next;
        memset(&next, 0, sizeof next);
        bool ok = false;
        switch (bit) {
        case GPU_SPV_E1: ok = edit_e1(stage, &cur, &next); break;
        case GPU_SPV_E2: ok = edit_e2(&cur, &next); break;
        case GPU_SPV_E3: ok = edit_e3(&cur, &next); break;
        case GPU_SPV_E4: ok = edit_e4(&cur, &next, layout, err, err_cap); break;
        case GPU_SPV_E5: ok = edit_e5(&cur, &next, err, err_cap); break;
        default: break;
        }
        free(cur.p);
        if (!ok) { free(next.p); return NULL; }
        if (!next.p) text_put(&next, "", 0);
        cur = next;
    }
    if (len) *len = cur.n;
    return cur.p;
}

extern "C" int gpu_spv_text_has_bindings(const char* src, size_t n)
{
    static const char lit[] = "vk::binding";
    if (!src || n < sizeof lit - 1) return 0;
    for (size_t i = 0; i + sizeof lit - 1 <= n; i++)
        if (src[i] == 'v' && !memcmp(src + i, lit, sizeof lit - 1)) return 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * SPIR-V
 * -------------------------------------------------------------------------*/

namespace {

/* SPIR-V's numbers (spirv.h), the few used here. */
enum : uint32_t {
    OpSourceContinued = 2, OpSource = 3, OpSourceExtension = 4, OpName = 5, OpMemberName = 6, OpString = 7,
    OpExtension = 10, OpExtInstImport = 11, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16,
    OpCapability = 17, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeArray = 28,
    OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpFunction = 54, OpVariable = 59,
    OpLoad = 61, OpDecorate = 71, OpMemberDecorate = 72, OpDecorationGroup = 73, OpGroupDecorate = 74,
    OpGroupMemberDecorate = 75, OpFAdd = 129, OpISub = 130, OpFSub = 131, OpFMul = 133, OpFDiv = 136,
    OpFRem = 140, OpFMod = 141, OpVectorTimesScalar = 142, OpMatrixTimesScalar = 143,
    OpVectorTimesMatrix = 144, OpMatrixTimesVector = 145, OpMatrixTimesMatrix = 146, OpOuterProduct = 147,
    OpDot = 148, OpModuleProcessed = 330, OpExecutionModeId = 331, OpDecorateId = 332,
    OpDecorateString = 5632, OpMemberDecorateString = 5633,

    DecBlock = 2, DecBufferBlock = 3, DecBuiltIn = 11, DecInvariant = 18, DecBinding = 33,
    DecDescriptorSet = 34, DecNoContraction = 42,
    BuiltInPosition = 0, BuiltInVertexIndex = 42, BuiltInBaseVertex = 4424,
    CapDrawParameters = 4427, CapDenormPreserve = 4464, CapSignedZeroInfNanPreserve = 4466,
    ModeDenormPreserve = 4459, ModeSignedZeroInfNanPreserve = 4461,
    StorageUniformConstant = 0, StorageInput = 1, StorageUniform = 2, StorageOutput = 3,
    StoragePushConstant = 9, StorageStorageBuffer = 12,
    DimBuffer = 5,
    SpvMagic = 0x07230203u
};

/* The logical layout's sections, in order (SPIR-V 2.4). */
enum Section {
    SecCapability = 1, SecExtension, SecExtInstImport, SecMemoryModel, SecEntryPoint, SecExecutionMode,
    SecDebug, SecAnnotation, SecGlobal, SecFunction
};

int section_of(uint32_t op, bool in_functions)
{
    if (in_functions) return SecFunction;
    switch (op) {
    case OpCapability: return SecCapability;
    case OpExtension: return SecExtension;
    case OpExtInstImport: return SecExtInstImport;
    case OpMemoryModel: return SecMemoryModel;
    case OpEntryPoint: return SecEntryPoint;
    case OpExecutionMode: case OpExecutionModeId: return SecExecutionMode;
    case OpSourceContinued: case OpSource: case OpSourceExtension: case OpName: case OpMemberName:
    case OpString: case OpModuleProcessed: return SecDebug;
    case OpDecorate: case OpMemberDecorate: case OpDecorationGroup: case OpGroupDecorate:
    case OpGroupMemberDecorate: case OpDecorateId: case OpDecorateString: case OpMemberDecorateString:
        return SecAnnotation;
    case OpFunction: return SecFunction;
    default: return SecGlobal;
    }
}

/* A module's instructions, checked: each starts at an offset with its word
 * count. */
struct Module {
    const uint32_t* w;
    size_t n;
    uint32_t bound;
};

bool module_check(const uint32_t* w, size_t n, Module* m)
{
    if (!w || n < 5 || w[0] != SpvMagic) return false;
    for (size_t i = 5; i < n; ) {
        const uint32_t wc = w[i] >> 16;
        if (wc == 0 || i + wc > n) return false;
        i += wc;
    }
    m->w = w;
    m->n = n;
    m->bound = w[3];
    return true;
}

/* A word vector. */
struct Words {
    uint32_t* p;
    size_t n, cap;
    bool bad;
};
void words_put(Words* v, const uint32_t* w, size_t n)
{
    if (v->bad) return;
    if (v->n + n > v->cap) {
        size_t c = v->cap ? v->cap : 1024;
        while (v->n + n > c) c *= 2;
        uint32_t* g = (uint32_t*)realloc(v->p, c * sizeof *g);
        if (!g) { v->bad = true; return; }
        v->p = g;
        v->cap = c;
    }
    memcpy(v->p + v->n, w, n * sizeof *w);
    v->n += n;
}
void words_op(Words* v, uint32_t op, const uint32_t* operands, size_t n)
{
    const uint32_t head = ((uint32_t)(n + 1) << 16) | op;
    words_put(v, &head, 1);
    words_put(v, operands, n);
}
void words_string_op(Words* v, uint32_t op, const char* s)
{
    uint32_t buf[16];
    const size_t len = strlen(s), nw = len / 4 + 1;
    memset(buf, 0, sizeof buf);
    memcpy(buf, s, len);
    words_op(v, op, buf, nw);
}

/* A table indexed by id (the module's bound is its size). */
struct IdTable {
    uint32_t* v;
};
bool idt_init(IdTable* t, uint32_t bound)
{
    t->v = (uint32_t*)calloc(bound ? bound : 1, sizeof *t->v);
    return t->v != NULL;
}

/* The instruction defining each type id (its offset), and for each variable
 * its pointer type and storage class. */
struct Defs {
    IdTable at;          /* id -> offset of its OpType* / OpVariable, 0 = none */
};

uint32_t pointee_base(const Module* m, const Defs* d, uint32_t ptr_type)
{
    uint32_t t = 0;
    if (ptr_type < m->bound && d->at.v[ptr_type]) {
        const uint32_t* p = m->w + d->at.v[ptr_type];
        if ((p[0] & 0xFFFF) == OpTypePointer) t = p[3];
    }
    for (int guard = 0; guard < 8 && t && t < m->bound && d->at.v[t]; guard++) {
        const uint32_t* p = m->w + d->at.v[t];
        const uint32_t op = p[0] & 0xFFFF;
        if (op == OpTypeArray || op == OpTypeRuntimeArray) t = p[2];
        else return t;
    }
    return t;
}

bool defs_build(const Module* m, Defs* d)
{
    if (!idt_init(&d->at, m->bound)) return false;
    for (size_t i = 5; i < m->n; i += m->w[i] >> 16) {
        const uint32_t op = m->w[i] & 0xFFFF, wc = m->w[i] >> 16;
        if (op == OpFunction) break;
        uint32_t id = 0;
        if ((op >= 19 && op <= 39) && wc >= 2) id = m->w[i + 1];   /* OpTypeVoid .. OpTypeForwardPointer */
        else if (op == OpVariable && wc >= 4) id = m->w[i + 2];
        if (id && id < m->bound) d->at.v[id] = (uint32_t)i;
    }
    return true;
}

uint32_t op_at(const Module* m, const Defs* d, uint32_t id)
{
    return (id < m->bound && d->at.v[id]) ? (m->w[d->at.v[id]] & 0xFFFF) : 0;
}

bool is_float_arith(uint32_t op)
{
    switch (op) {
    case OpFAdd: case OpFSub: case OpFMul: case OpFDiv: case OpFRem: case OpFMod:
    case OpVectorTimesScalar: case OpMatrixTimesScalar: case OpVectorTimesMatrix: case OpMatrixTimesVector:
    case OpMatrixTimesMatrix: case OpOuterProduct: case OpDot:
        return true;
    default:
        return false;
    }
}

/* The name OpName gives an id, for messages. */
void name_of(const Module* m, uint32_t id, char* buf, size_t cap)
{
    snprintf(buf, cap, "%%%u", id);
    for (size_t i = 5; i < m->n; i += m->w[i] >> 16) {
        const uint32_t op = m->w[i] & 0xFFFF, wc = m->w[i] >> 16;
        if (op == OpFunction) break;
        if (op == OpName && wc >= 3 && m->w[i + 1] == id) {
            const size_t room = (wc - 2) * 4;
            const char* s = (const char*)(m->w + i + 2);
            size_t l = 0;
            while (l < room && s[l]) l++;
            snprintf(buf, cap, "%.*s", (int)l, s);
            return;
        }
    }
}

} // namespace

extern "C" int gpu_spv_apply_layout(uint32_t* words, size_t nwords, const gpu_layout_desc* L, char* err,
                                    size_t err_cap)
{
    if (err && err_cap) err[0] = 0;
    Module m;
    if (!L || !module_check(words, nwords, &m)) {
        if (err && err_cap) snprintf(err, err_cap, "layout: malformed module");
        return -1;
    }
    Defs d;
    if (!defs_build(&m, &d)) return -1;
    IdTable bind_at, set_at, block;   /* (offset of the decoration's value) + 1; a struct's Block (1) or BufferBlock (2) */
    if (!idt_init(&bind_at, m.bound) || !idt_init(&set_at, m.bound) || !idt_init(&block, m.bound)) {
        free(bind_at.v); free(set_at.v); free(d.at.v);
        return -1;
    }
    for (size_t i = 5; i < nwords; i += words[i] >> 16) {
        const uint32_t op = words[i] & 0xFFFF, wc = words[i] >> 16;
        if (op == OpFunction) break;
        if (op != OpDecorate || wc < 3 || words[i + 1] >= m.bound) continue;
        const uint32_t id = words[i + 1], dec = words[i + 2];
        if (dec == DecBinding && wc >= 4) bind_at.v[id] = (uint32_t)(i + 3) + 1u;
        if (dec == DecDescriptorSet && wc >= 4) set_at.v[id] = (uint32_t)(i + 3) + 1u;
        if (dec == DecBlock) block.v[id] = 1;
        if (dec == DecBufferBlock) block.v[id] = 2;
    }
    int r = 0;
    char nm[64];
    for (size_t i = 5; i < nwords && !r; i += words[i] >> 16) {
        const uint32_t op = words[i] & 0xFFFF, wc = words[i] >> 16;
        if (op == OpFunction) break;
        if (op != OpVariable || wc < 4) continue;
        const uint32_t ptr = words[i + 1], var = words[i + 2], sc = words[i + 3];
        if (sc != StorageUniformConstant && sc != StorageUniform && sc != StorageStorageBuffer) continue;
        /* the pointee, and whether it is an array */
        uint32_t t = 0;
        bool array = false;
        if (op_at(&m, &d, ptr) == OpTypePointer) t = words[d.at.v[ptr] + 3];
        const uint32_t top = op_at(&m, &d, t);
        if (top == OpTypeArray || top == OpTypeRuntimeArray) { array = true; t = words[d.at.v[t] + 2]; }
        const uint32_t bop = op_at(&m, &d, t);
        /* its class: 'b' a constant buffer, 'r' a raw buffer, 't' a texture, 's' a sampler */
        char cls = 0;
        if (sc == StorageUniform && bop == OpTypeStruct) cls = block.v[t] == 2 ? 'r' : 'b';
        else if (sc == StorageStorageBuffer && bop == OpTypeStruct) cls = 'r';
        else if (bop == OpTypeImage && words[d.at.v[t] + 7] == 1u && words[d.at.v[t] + 3] != DimBuffer) cls = 't';
        else if (bop == OpTypeSampler) cls = 's';
        name_of(&m, var, nm, sizeof nm);
        if ((!nm[0] || nm[0] == '%') && t) name_of(&m, t, nm, sizeof nm);   /* a block's instance: its type's name */
        if (!cls) {
            if (err && err_cap) snprintf(err, err_cap, "layout: %s is a resource the model has no place for", nm);
            r = -1;
            break;
        }
        if (array) {
            if (err && err_cap) snprintf(err, err_cap, "layout: %s is an array of descriptors", nm);
            r = -1;
            break;
        }
        if (!bind_at.v[var] || !set_at.v[var]) {
            if (err && err_cap) snprintf(err, err_cap, "layout: %s has no register", nm);
            r = -1;
            break;
        }
        const uint32_t b = words[bind_at.v[var] - 1u];
        const uint32_t shift = cls == 'b' ? 0u : cls == 's' ? 48u : 16u;
        const uint32_t reg = b - shift;
        uint32_t set = ~0u, binding = 0;
        uint32_t tables = 0;
        for (uint32_t k = 0; k < L->nslots && k < GPU_MAX_SLOTS && set == ~0u; k++) {
            const gpu_layout_slot* s = &L->slot[k];
            const bool table = s->kind == GPU_SLOT_TABLE_TEX || s->kind == GPU_SLOT_TABLE_SAMPLER;
            if (b < shift) break;
            if (cls == 'b' && s->kind == GPU_SLOT_CB && s->reg == reg && reg < 16) { set = 0; binding = reg; }
            if (cls == 'b' && s->kind == GPU_SLOT_CONST32 && s->reg == reg) {
                if (err && err_cap) snprintf(err, err_cap, "layout: %s (b%u) is a CONST32 slot's, not in the push constant block", nm, reg);
                r = -1;
                break;
            }
            if (cls == 'r' && s->kind == GPU_SLOT_RAW && s->reg == reg && reg < 32) { set = 0; binding = 16 + reg; }
            if (cls == 't' && s->kind == GPU_SLOT_TABLE_TEX && reg >= s->reg && reg - s->reg < s->count &&
                reg - s->reg < 32) { set = 1 + tables; binding = 16 + (reg - s->reg); }
            if (cls == 's' && s->kind == GPU_SLOT_TABLE_SAMPLER && reg >= s->reg && reg - s->reg < s->count &&
                reg - s->reg < 16) { set = 1 + tables; binding = 48 + (reg - s->reg); }
            if (table) tables++;
        }
        if (r) break;
        if (cls == 's' && set == ~0u && b >= shift)
            for (uint32_t k = 0; k < L->nstatic && k < GPU_MAX_STATIC_SAMPLERS; k++)
                if (L->statics[k].reg == reg && reg < 16) { set = 0; binding = 48 + reg; }
        if (set == ~0u) {
            if (err && err_cap)
                snprintf(err, err_cap, "layout: %s (%c%u) is not in the layout", nm, cls == 'r' ? 't' : cls,
                         b >= shift ? reg : b);
            r = -1;
            break;
        }
        words[bind_at.v[var] - 1u] = binding;
        words[set_at.v[var] - 1u] = set;
    }
    free(block.v);
    free(set_at.v);
    free(bind_at.v);
    free(d.at.v);
    return r;
}

extern "C" int gpu_spv_post(gpu_sc_stage stage, const uint32_t* words, size_t nwords, unsigned opts,
                            uint32_t** out, size_t* out_n)
{
    if (out) *out = NULL;
    if (out_n) *out_n = 0;
    Module m;
    if (!out || !out_n || !module_check(words, nwords, &m)) return GPU_SC_FAILED;
    const uint32_t version = words[1];
    Defs d;
    if (!defs_build(&m, &d)) return GPU_SC_FAILED;
    IdTable deco;   /* bit 0 NoContraction, bit 1 Invariant, bit 2 BuiltIn Position, bit 3 BuiltIn VertexIndex */
    if (!idt_init(&deco, m.bound)) { free(d.at.v); return GPU_SC_FAILED; }

    /* What the module has. */
    bool cap_szinp = false, cap_denorm = false, cap_draw = false, ext_fc = false;
    uint32_t entry_fn = 0;
    size_t entry_at = 0;
    uint32_t pos_struct = 0, pos_member = 0;   /* a block member Position, when there is one */
    bool pos_member_inv = false;
    bool mode_szinp = false, mode_denorm = false;
    for (size_t i = 5; i < nwords; i += words[i] >> 16) {
        const uint32_t op = words[i] & 0xFFFF, wc = words[i] >> 16;
        if (op == OpCapability && wc >= 2) {
            cap_szinp |= words[i + 1] == CapSignedZeroInfNanPreserve;
            cap_denorm |= words[i + 1] == CapDenormPreserve;
            cap_draw |= words[i + 1] == CapDrawParameters;
        } else if (op == OpExtension && wc >= 2) {
            ext_fc |= !strncmp((const char*)(words + i + 1), "SPV_KHR_float_controls", (wc - 1) * 4);
        } else if (op == OpEntryPoint && wc >= 3 && !entry_at) {
            entry_at = i;
            entry_fn = words[i + 2];
        } else if (op == OpExecutionMode && wc >= 4 && words[i + 1] == entry_fn) {
            mode_szinp |= words[i + 2] == ModeSignedZeroInfNanPreserve && words[i + 3] == 32;
            mode_denorm |= words[i + 2] == ModeDenormPreserve && words[i + 3] == 32;
        } else if (op == OpDecorate && wc >= 3 && words[i + 1] < m.bound) {
            const uint32_t id = words[i + 1];
            if (words[i + 2] == DecNoContraction) deco.v[id] |= 1;
            if (words[i + 2] == DecInvariant) deco.v[id] |= 2;
            if (words[i + 2] == DecBuiltIn && wc >= 4 && words[i + 3] == BuiltInPosition) deco.v[id] |= 4;
            if (words[i + 2] == DecBuiltIn && wc >= 4 && words[i + 3] == BuiltInVertexIndex) deco.v[id] |= 8;
        } else if (op == OpMemberDecorate && wc >= 4 && words[i + 1] < m.bound) {
            if (words[i + 3] == DecBuiltIn && wc >= 5 && words[i + 4] == BuiltInPosition) {
                pos_struct = words[i + 1];
                pos_member = words[i + 2];
            }
        }
    }
    if (pos_struct) {
        for (size_t i = 5; i < nwords; i += words[i] >> 16) {
            const uint32_t op = words[i] & 0xFFFF, wc = words[i] >> 16;
            if (op == OpMemberDecorate && wc >= 4 && words[i + 1] == pos_struct && words[i + 2] == pos_member &&
                words[i + 3] == DecInvariant)
                pos_member_inv = true;
        }
    }
    if (!entry_at) { free(deco.v); free(d.at.v); return GPU_SC_FAILED; }

    /* What to add. */
    const bool fc = (opts & (GPU_SC_SPV_FLOAT_PRESERVE | GPU_SC_SPV_DENORM_PRESERVE)) != 0;
    const bool add_szinp = (opts & GPU_SC_SPV_FLOAT_PRESERVE) != 0;
    const bool add_denorm = (opts & GPU_SC_SPV_DENORM_PRESERVE) != 0;
    const bool invariant = (opts & GPU_SC_SPV_INVARIANT) && (stage == GPU_SC_VS || stage == GPU_SC_GS);
    /* The output variables holding Position (not an input's), and VertexIndex. */
    uint32_t vi_var = 0, vi_ptr = 0;
    bool pos_struct_out = false;
    for (size_t i = 5; i < nwords; i += words[i] >> 16) {
        const uint32_t op = words[i] & 0xFFFF, wc = words[i] >> 16;
        if (op == OpFunction) break;
        if (op != OpVariable || wc < 4) continue;
        const uint32_t id = words[i + 2], sc = words[i + 3];
        if (sc == StorageInput && (deco.v[id] & 8) && stage == GPU_SC_VS) { vi_var = id; vi_ptr = words[i + 1]; }
        if (sc == StorageOutput && pos_struct && pointee_base(&m, &d, words[i + 1]) == pos_struct)
            pos_struct_out = true;
        if (sc != StorageOutput) deco.v[id] &= ~4u;   /* a GS's Position input stays */
    }
    const bool base_vertex = (opts & GPU_SC_SPV_BASE_VERTEX) && vi_var != 0;
    uint32_t bound = m.bound;
    const uint32_t bv_var = base_vertex ? bound++ : 0;
    /* Each load of VertexIndex: two new ids (its value before the
     * subtraction, BaseVertex's value). */
    size_t vi_loads = 0;
    if (base_vertex)
        for (size_t i = 5; i < nwords; i += words[i] >> 16)
            if ((words[i] & 0xFFFF) == OpLoad && (words[i] >> 16) >= 4 && words[i + 3] == vi_var) vi_loads++;
    if ((uint64_t)bound + 2u * vi_loads >= 0x3FFFFFu) { free(deco.v); free(d.at.v); return GPU_SC_FAILED; }

    Words v;
    memset(&v, 0, sizeof v);
    {
        uint32_t head[5] = { words[0], words[1], words[2], 0, words[4] };
        words_put(&v, head, 5);
    }
    bool in_fn = false;
    int flushed = 0;   /* the last section whose additions are in */
    auto flush_to = [&](int sec) {
        for (int s = flushed + 1; s < sec; s++) {
            switch (s) {
            case SecCapability:
                if (add_szinp && !cap_szinp) { const uint32_t c = CapSignedZeroInfNanPreserve; words_op(&v, OpCapability, &c, 1); }
                if (add_denorm && !cap_denorm) { const uint32_t c = CapDenormPreserve; words_op(&v, OpCapability, &c, 1); }
                if (base_vertex && !cap_draw) { const uint32_t c = CapDrawParameters; words_op(&v, OpCapability, &c, 1); }
                break;
            case SecExtension:
                if (fc && !ext_fc && version < 0x00010400u) words_string_op(&v, OpExtension, "SPV_KHR_float_controls");
                if (base_vertex && !cap_draw && version < 0x00010300u)
                    words_string_op(&v, OpExtension, "SPV_KHR_shader_draw_parameters");
                break;
            case SecExecutionMode:
                if (add_szinp && !mode_szinp) {
                    const uint32_t e[3] = { entry_fn, ModeSignedZeroInfNanPreserve, 32 };
                    words_op(&v, OpExecutionMode, e, 3);
                }
                if (add_denorm && !mode_denorm) {
                    const uint32_t e[3] = { entry_fn, ModeDenormPreserve, 32 };
                    words_op(&v, OpExecutionMode, e, 3);
                }
                break;
            case SecAnnotation:
                if (invariant) {
                    for (uint32_t id = 1; id < m.bound; id++)
                        if ((deco.v[id] & 4) && !(deco.v[id] & 2)) {
                            const uint32_t e[2] = { id, DecInvariant };
                            words_op(&v, OpDecorate, e, 2);
                        }
                    if (pos_struct_out && !pos_member_inv) {
                        const uint32_t e[3] = { pos_struct, pos_member, DecInvariant };
                        words_op(&v, OpMemberDecorate, e, 3);
                    }
                }
                if (base_vertex) {
                    const uint32_t e[3] = { bv_var, DecBuiltIn, BuiltInBaseVertex };
                    words_op(&v, OpDecorate, e, 3);
                }
                if (opts & GPU_SC_SPV_NO_CONTRACT) {
                    bool fns = false;
                    for (size_t i = 5; i < nwords; i += words[i] >> 16) {
                        const uint32_t op = words[i] & 0xFFFF, wc = words[i] >> 16;
                        if (op == OpFunction) fns = true;
                        if (fns && is_float_arith(op) && wc >= 3 && words[i + 2] < m.bound &&
                            !(deco.v[words[i + 2]] & 1)) {
                            const uint32_t e[2] = { words[i + 2], DecNoContraction };
                            words_op(&v, OpDecorate, e, 2);
                        }
                    }
                }
                break;
            case SecGlobal:
                if (base_vertex) {
                    const uint32_t e[3] = { vi_ptr, bv_var, StorageInput };
                    words_op(&v, OpVariable, e, 3);
                }
                break;
            default:
                break;
            }
            flushed = s;
        }
    };
    uint32_t next_id = bound;
    for (size_t i = 5; i < nwords; ) {
        const uint32_t op = words[i] & 0xFFFF, wc = words[i] >> 16;
        if (op == OpFunction) in_fn = true;
        const int sec = section_of(op, in_fn);
        flush_to(sec);
        if (op == OpEntryPoint && i == entry_at && base_vertex) {
            /* the interface gains BaseVertex */
            const uint32_t head = ((wc + 1) << 16) | op;
            words_put(&v, &head, 1);
            words_put(&v, words + i + 1, wc - 1);
            words_put(&v, &bv_var, 1);
        } else if (op == OpLoad && base_vertex && wc >= 4 && words[i + 3] == vi_var) {
            /* %r = VertexIndex - BaseVertex, as D3D12's SV_VertexID */
            const uint32_t type = words[i + 1], r = words[i + 2];
            const uint32_t raw = next_id++, base = next_id++;
            const uint32_t l1[3] = { type, raw, vi_var };
            const uint32_t l2[3] = { type, base, bv_var };
            const uint32_t sub[4] = { type, r, raw, base };
            words_op(&v, OpLoad, l1, 3);
            words_op(&v, OpLoad, l2, 3);
            words_op(&v, OpISub, sub, 4);
        } else {
            words_put(&v, words + i, wc);
        }
        i += wc;
    }
    flush_to(SecFunction + 1);
    free(deco.v);
    free(d.at.v);
    if (v.bad) { free(v.p); return GPU_SC_FAILED; }
    v.p[3] = next_id;
    *out = v.p;
    *out_n = v.n;
    return GPU_SC_OK;
}

#else
typedef int gpu_shader_spv_edit_off;
#endif /* PS3RECOMP_RSX_HAL && PS3RECOMP_RSX_HAL_SPV */
