/*
 * ps3recomp - the portable renderer's GPU layer: the null backend
 *
 * gpu.h's device functions and gpu_cmd_ops.h's commands with no GPU under
 * them, for headless CI on every system: everything is recorded and checked
 * against the contract, and nothing is drawn. gpu_null.h says what it keeps
 * and how the operation stream is hashed; gpu_null_internal.h has the types;
 * the commands are gpu_null_cmd.h's static inline ones, which call
 * gpu_null_cmd_<name> here.
 *
 * Two kinds of check:
 *   - as a command is recorded: its handles, its arguments, and what the
 *     recording has bound (a recording's bindings are its own on D3D12 and
 *     Vulkan alike);
 *   - as it runs, at submit, in submission order: the tracked state of each
 *     subresource, with D3D12's promotion from COMMON and decay back to it;
 *     objects freed since it was recorded; the bytes the copies move.
 * Fences: the highest fence the caller has seen pass (gpu_fence_completed,
 * gpu_fence_wait, gpu_wait_idle) is what it may take as finished on a real
 * GPU, so frees, table writes, slot reuse and readback maps are checked
 * against it, though the null GPU itself is done at once.
 *
 * Plain C99 with no system or GPU headers: Windows, Linux and macOS.
 */
#if defined(PS3RECOMP_RSX_HAL)

#include "gpu_null_internal.h"
#include "gpu_null_cmd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Atomics and the lock (shaders and pipelines are made from any thread)
 * -------------------------------------------------------------------------*/

#if defined(__GNUC__) || defined(__clang__)
static inline uint32_t at_load32(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void at_store32(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline uint32_t at_xchg32(uint32_t* p, uint32_t v) { return __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL); }
static inline void at_inc32(uint32_t* p) { (void)__atomic_fetch_add(p, 1u, __ATOMIC_ACQ_REL); }
static inline uint64_t at_load64(const uint64_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void at_store64(uint64_t* p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline void at_max64(uint64_t* p, uint64_t v)
{
    uint64_t cur = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    while (cur < v && !__atomic_compare_exchange_n(p, &cur, v, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { }
}
#elif defined(_MSC_VER)
#include <intrin.h>
static inline uint32_t at_load32(const uint32_t* p) { return (uint32_t)_InterlockedOr((volatile long*)p, 0); }
static inline void at_store32(uint32_t* p, uint32_t v) { (void)_InterlockedExchange((volatile long*)p, (long)v); }
static inline uint32_t at_xchg32(uint32_t* p, uint32_t v)
{
    return (uint32_t)_InterlockedExchange((volatile long*)p, (long)v);
}
static inline void at_inc32(uint32_t* p) { (void)_InterlockedIncrement((volatile long*)p); }
static inline uint64_t at_load64(const uint64_t* p) { return (uint64_t)_InterlockedOr64((volatile __int64*)p, 0); }
static inline void at_store64(uint64_t* p, uint64_t v)
{
    (void)_InterlockedExchange64((volatile __int64*)p, (__int64)v);
}
static inline void at_max64(uint64_t* p, uint64_t v)
{
    uint64_t cur = at_load64(p);
    while (cur < v) {
        const uint64_t was = (uint64_t)_InterlockedCompareExchange64((volatile __int64*)p, (__int64)v, (__int64)cur);
        if (was == cur) break;
        cur = was;
    }
}
#else   /* another compiler: one thread only */
static inline uint32_t at_load32(const uint32_t* p) { return *p; }
static inline void at_store32(uint32_t* p, uint32_t v) { *p = v; }
static inline uint32_t at_xchg32(uint32_t* p, uint32_t v) { const uint32_t o = *p; *p = v; return o; }
static inline void at_inc32(uint32_t* p) { ++*p; }
static inline uint64_t at_load64(const uint64_t* p) { return *p; }
static inline void at_store64(uint64_t* p, uint64_t v) { *p = v; }
static inline void at_max64(uint64_t* p, uint64_t v) { if (*p < v) *p = v; }
#endif

/* A spin lock: held for a table lookup at most. */
static void nl_lock(gpu_null_device* nd)
{
    while (at_xchg32(&nd->lock, 1u))
        while (at_load32(&nd->lock)) { }
}

static void nl_unlock(gpu_null_device* nd)
{
    at_store32(&nd->lock, 0u);
}

#if defined(__GNUC__) || defined(__clang__)
#define NL_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define NL_PRINTF(f, a)
#endif

#define LLU(x) ((unsigned long long)(x))
#define NL_IDX(id) GPU_HANDLE_INDEX(id)
#define NL_ADDR_MASK ((1ull << GPU_NULL_ADDR_BITS) - 1u)

/* ---------------------------------------------------------------------------
 * Names
 * -------------------------------------------------------------------------*/

#define NL_CMD_NAME(name, form, ret, params, args) #name,
static const char* const k_cmd_name[GPU_CMD_OP_COUNT] = { GPU_CMD_OPS(NL_CMD_NAME) };
#undef NL_CMD_NAME
#define NL_DEV_NAME(name) #name,
static const char* const k_dev_name[GPU_NULL_OP_END - GPU_NULL_OP_DEV_FIRST - 1] = {
    GPU_NULL_DEV_CALLS(NL_DEV_NAME)
};
#undef NL_DEV_NAME

static const char* const k_err_name[GPU_NULL_ERR_COUNT] = {
    "handle", "freed", "barrier", "state", "draw", "use", "align", "bounds", "query", "table", "fence",
    "recorder", "swap"
};

static const char* op_name(uint64_t op)
{
    if (op < GPU_CMD_OP_COUNT) return k_cmd_name[op];
    if (op > GPU_NULL_OP_DEV_FIRST && op < GPU_NULL_OP_END) return k_dev_name[op - GPU_NULL_OP_DEV_FIRST - 1];
    return "?";
}

static const char* tag_name(uint8_t tag)
{
    switch (tag) {
    case GPU_NULL_T_BUF:    return "buf";
    case GPU_NULL_T_TEX:    return "tex";
    case GPU_NULL_T_VIEW:   return "view";
    case GPU_NULL_T_SAMP:   return "samp";
    case GPU_NULL_T_LAYOUT: return "layout";
    case GPU_NULL_T_QPOOL:  return "qpool";
    case GPU_NULL_T_TPOOL:  return "tpool";
    case GPU_NULL_T_HEAP:   return "heap";
    case GPU_NULL_T_SWAP:   return "swap";
    case GPU_NULL_T_SHADER: return "shader";
    case GPU_NULL_T_PIPE:   return "pipe";
    case GPU_NULL_T_ADDR:   return "addr";
    case GPU_NULL_T_TABLE:  return "table";
    default:                return "arg";
    }
}

static const char* slot_kind_name(uint32_t kind)
{
    static const char* const n[] = { "CB", "RAW", "CONST32", "TABLE_TEX", "TABLE_SAMPLER" };
    return kind < 5 ? n[kind] : "?";
}

static const char* rec_state_name(uint32_t s)
{
    static const char* const n[] = { "not begun", "recording", "ended", "submitted" };
    return s < 4 ? n[s] : "?";
}

/* ---------------------------------------------------------------------------
 * Records: built into a vector of tagged arguments
 * -------------------------------------------------------------------------*/

static gpu_null_arg* vec_add(gpu_null_vec* v)
{
    if (v->n == v->cap) {
        const uint32_t cap = v->cap ? v->cap * 2 : 1024;
        gpu_null_arg* a = (gpu_null_arg*)realloc(v->a, (size_t)cap * sizeof *a);
        if (!a) return NULL;
        v->a = a;
        v->cap = cap;
    }
    return &v->a[v->n++];
}

typedef struct nl_rb {
    gpu_null_device* nd;
    gpu_null_vec*    v;
    uint32_t         head;
    int              ok;
} nl_rb;

static void rb_begin(nl_rb* b, gpu_null_device* nd, gpu_null_vec* v, uint32_t op)
{
    b->nd = nd;
    b->v = v;
    b->head = v->n;
    gpu_null_arg* a = vec_add(v);
    b->ok = a != NULL;
    if (a) {
        memset(a, 0, sizeof *a);
        a->v = op;
    }
}

static void rb_put(nl_rb* b, uint8_t tag, uint32_t raw, uint64_t value)
{
    if (!b->ok) return;
    gpu_null_arg* a = vec_add(b->v);
    if (!a) {
        b->ok = 0;
        return;
    }
    memset(a, 0, sizeof *a);
    a->tag = tag;
    a->raw = raw;
    a->v = value;
    b->v->a[b->head].raw++;
}

/* The finished record, or NULL without memory (then it is dropped, and the
 * stream is no longer the run's: said once). */
static gpu_null_arg* rb_end(nl_rb* b)
{
    if (b->ok) return &b->v->a[b->head];
    b->v->n = b->head;
    if (!at_xchg32(&b->nd->oom, 1u))
        fprintf(stderr, "[GPU null] out of memory: records dropped; the stream hash is not the run's\n");
    return NULL;
}

static void rb_drop(nl_rb* b)
{
    b->v->n = b->head;
}

/* A command's record in c's recording; a device call's in the stream (ev),
 * or outside it (scratch: only for a check's message). */
#define NL_BEGIN(op)  nl_rb b_; nl_rb* const b = &b_; rb_begin(b, c->nd, &c->rec, GPU_CMD_OP_##op)
#define NL_EV(op)     nl_rb b_; nl_rb* const b = &b_; rb_begin(b, nd, &nd->ev, GPU_NULL_OP_##op)
#define NL_CALL(op)   nl_rb b_; nl_rb* const b = &b_; nd->scratch.n = 0; rb_begin(b, nd, &nd->scratch, GPU_NULL_OP_##op)

/* ---------------------------------------------------------------------------
 * Handle slots, as the D3D12 backend keeps them
 * -------------------------------------------------------------------------*/

static int slot_valid(const gpu_null_slots* s, uint32_t id)
{
    const uint32_t i = NL_IDX(id);
    return id && i < s->used && s->live[i] && s->gen[i] == GPU_HANDLE_GEN(id);
}

/* The slot arrays and the objects' array (*arrp, elem bytes each) to cap
 * slots, zeroed. The objects' pointer is read and written with memcpy, so
 * any array type goes through one function. */
static int slots_resize(gpu_null_slots* s, uint32_t cap, void* arrp, size_t elem)
{
    uint32_t* f = (uint32_t*)realloc(s->free, (size_t)cap * sizeof *f);
    if (!f) return 0;
    s->free = f;
    uint8_t* g = (uint8_t*)realloc(s->gen, cap);
    if (!g) return 0;
    memset(g + s->cap, 0, cap - s->cap);
    s->gen = g;
    uint8_t* l = (uint8_t*)realloc(s->live, cap);
    if (!l) return 0;
    memset(l + s->cap, 0, cap - s->cap);
    s->live = l;
    if (arrp) {
        void* a;
        memcpy(&a, arrp, sizeof a);
        char* na = (char*)realloc(a, (size_t)cap * elem);
        if (!na) return 0;
        memset(na + (size_t)s->cap * elem, 0, (size_t)(cap - s->cap) * elem);
        memcpy(arrp, &na, sizeof na);
    }
    s->cap = cap;
    return 1;
}

static int slots_init(gpu_null_slots* s, uint32_t cap, uint32_t max, void* arrp, size_t elem)
{
    memset(s, 0, sizeof *s);
    s->max = max;
    return slots_resize(s, cap, arrp, elem);
}

static void slots_fini(gpu_null_slots* s)
{
    free(s->free);
    free(s->gen);
    free(s->live);
    memset(s, 0, sizeof *s);
}

/* A new slot's id (0 when full), growing the objects' array with the slots. */
static uint32_t obj_alloc(gpu_null_slots* s, void* arrp, size_t elem)
{
    uint32_t i;
    if (s->nfree) {
        i = s->free[--s->nfree];
    } else {
        if (s->used == s->cap) {
            if (s->cap >= s->max) return 0;
            uint32_t cap = s->cap * 2;
            if (cap > s->max) cap = s->max;
            if (!slots_resize(s, cap, arrp, elem)) return 0;
        }
        i = s->used++;
    }
    s->live[i] = 1;
    s->made++;
    return GPU_HANDLE_MAKE(i, s->gen[i]);
}

/* A slot at generation 255 is retired, not reused (gpu.h). */
static void slot_free(gpu_null_slots* s, uint32_t id)
{
    const uint32_t i = NL_IDX(id);
    s->live[i] = 0;
    if (s->gen[i] == 255) return;
    s->gen[i]++;
    s->free[s->nfree++] = i;
}

static const char* why_bad(const gpu_null_slots* s, uint32_t id)
{
    const uint32_t i = NL_IDX(id);
    if (!s || i >= s->used) return "never made";
    if (!s->live[i]) return "freed";
    return "stale (its slot holds a newer object)";
}

static int pipe_valid(gpu_null_device* nd, uint32_t raw)
{
    return raw && raw <= at_load32(&nd->npipe);
}

static gpu_null_tex* tex_at(gpu_null_device* nd, uint32_t id)
{
    return slot_valid(&nd->tex_slots, id) ? &nd->tex[NL_IDX(id)] : NULL;
}

static gpu_null_buf* buf_at(gpu_null_device* nd, uint32_t id)
{
    return slot_valid(&nd->buf_slots, id) ? &nd->buf[NL_IDX(id)] : NULL;
}

static gpu_null_view* view_at(gpu_null_device* nd, uint32_t id)
{
    return slot_valid(&nd->view_slots, id) ? &nd->view[NL_IDX(id)] : NULL;
}

static gpu_null_layout* layout_at(gpu_null_device* nd, uint32_t id)
{
    return slot_valid(&nd->layout_slots, id) ? &nd->layout[NL_IDX(id)] : NULL;
}

static gpu_null_tpool* tpool_at(gpu_null_device* nd, uint32_t id)
{
    return slot_valid(&nd->tpool_slots, id) ? &nd->tpool[NL_IDX(id)] : NULL;
}

static gpu_null_qpool* qpool_at(gpu_null_device* nd, uint32_t id)
{
    return slot_valid(&nd->qpool_slots, id) ? &nd->qpool[NL_IDX(id)] : NULL;
}

static gpu_null_swap* swap_at(gpu_null_device* nd, uint32_t id)
{
    return slot_valid(&nd->swap_slots, id) ? &nd->swap[NL_IDX(id)] : NULL;
}

/* gpu_addr: the buffer's slot + 1 at bit 40, its generation at bit 56. */
static uint32_t addr_buf_id(gpu_addr a)
{
    const uint32_t idx1 = (uint32_t)(a >> GPU_NULL_ADDR_BITS) & 0xFFFFu;
    return idx1 ? GPU_HANDLE_MAKE(idx1 - 1u, (uint32_t)(a >> 56)) : 0u;
}

/* ---------------------------------------------------------------------------
 * Arguments
 * -------------------------------------------------------------------------*/

static void put_u(nl_rb* b, uint64_t v) { rb_put(b, GPU_NULL_T_U, 0, v); }
static void put_i(nl_rb* b, int64_t v) { rb_put(b, GPU_NULL_T_I, 0, (uint64_t)v); }
static void put_x(nl_rb* b, uint64_t v) { rb_put(b, GPU_NULL_T_X, 0, v); }
static void put_flag(nl_rb* b, int on) { rb_put(b, GPU_NULL_T_FLAG, 0, on ? 1u : 0u); }

static void put_f(nl_rb* b, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    rb_put(b, GPU_NULL_T_F, 0, bits);
}

static void put_handle(nl_rb* b, uint8_t tag, const gpu_null_slots* s, const uint32_t* order, size_t stride,
                       uint32_t id)
{
    uint32_t no = 0;
    if (slot_valid(s, id)) memcpy(&no, (const char*)order + (size_t)NL_IDX(id) * stride, sizeof no);
    rb_put(b, tag, id, no);
}

/* Every object struct starts with its order number. */
#define NL_PUT(b, tag, slots, arr, id) \
    put_handle((b), (tag), &(b)->nd->slots, (b)->nd->arr ? &(b)->nd->arr[0].order : NULL, sizeof (b)->nd->arr[0], (id))

static void put_tex(nl_rb* b, gpu_texture t)       { NL_PUT(b, GPU_NULL_T_TEX, tex_slots, tex, t.id); }
static void put_buf(nl_rb* b, gpu_buffer x)        { NL_PUT(b, GPU_NULL_T_BUF, buf_slots, buf, x.id); }
static void put_view(nl_rb* b, gpu_view v)         { NL_PUT(b, GPU_NULL_T_VIEW, view_slots, view, v.id); }
static void put_samp(nl_rb* b, gpu_sampler s)      { NL_PUT(b, GPU_NULL_T_SAMP, samp_slots, samp, s.id); }
static void put_layout(nl_rb* b, gpu_layout l)     { NL_PUT(b, GPU_NULL_T_LAYOUT, layout_slots, layout, l.id); }
static void put_heap(nl_rb* b, gpu_heap h)         { NL_PUT(b, GPU_NULL_T_HEAP, heap_slots, heap, h.id); }

static void put_qpool(nl_rb* b, gpu_query_pool q)
{
    const gpu_null_qpool* p = qpool_at(b->nd, q.id);
    rb_put(b, GPU_NULL_T_QPOOL, q.id, p ? p->order : 0u);
}

static void put_tpool(nl_rb* b, gpu_table_pool t)
{
    const gpu_null_tpool* p = tpool_at(b->nd, t.id);
    rb_put(b, GPU_NULL_T_TPOOL, t.id, p ? p->order : 0u);
}

static void put_swap(nl_rb* b, gpu_swapchain s)
{
    const gpu_null_swap* p = swap_at(b->nd, s.id);
    rb_put(b, GPU_NULL_T_SWAP, s.id, p ? p->order : 0u);
}

static void put_addr(nl_rb* b, gpu_addr a)
{
    const uint32_t id = addr_buf_id(a);
    const gpu_null_buf* bf = buf_at(b->nd, id);
    rb_put(b, GPU_NULL_T_ADDR, id, ((uint64_t)(bf ? bf->order : 0u) << GPU_NULL_ADDR_BITS) | (a & NL_ADDR_MASK));
}

/* gpu_table: its pool's id times 2^32 plus its first entry. */
static void put_table(nl_rb* b, gpu_table t)
{
    const uint32_t id = (uint32_t)(t.v >> 32);
    const gpu_null_tpool* p = tpool_at(b->nd, id);
    rb_put(b, GPU_NULL_T_TABLE, id, ((uint64_t)(p ? p->order : 0u) << 32) | (uint32_t)t.v);
}

static void put_pipe(nl_rb* b, gpu_pipeline p)
{
    const uint32_t raw = p.v <= 0xFFFFFFFFu ? (uint32_t)p.v : 0xFFFFFFFFu;
    rb_put(b, GPU_NULL_T_PIPE, raw, pipe_valid(b->nd, raw) ? b->nd->pipe[raw - 1].name : 0u);
}

static void put_rect(nl_rb* b, const gpu_rect* r)
{
    put_flag(b, r != NULL);
    put_i(b, r ? r->left : 0);
    put_i(b, r ? r->top : 0);
    put_i(b, r ? r->right : 0);
    put_i(b, r ? r->bottom : 0);
}

static void put_box(nl_rb* b, const gpu_box* x)
{
    put_flag(b, x != NULL);
    put_u(b, x ? x->left : 0);
    put_u(b, x ? x->top : 0);
    put_u(b, x ? x->front : 0);
    put_u(b, x ? x->right : 0);
    put_u(b, x ? x->bottom : 0);
    put_u(b, x ? x->back : 0);
}

static void put_fp(nl_rb* b, const gpu_footprint* f)
{
    put_flag(b, f != NULL);
    put_u(b, f ? f->offset : 0);
    put_u(b, f ? f->format : 0);
    put_u(b, f ? f->width : 0);
    put_u(b, f ? f->height : 0);
    put_u(b, f ? f->depth : 0);
    put_u(b, f ? f->row_pitch : 0);
}

/* Text (a marker's), up to 255 bytes, eight to an argument. Not hashed. */
static void put_text(nl_rb* b, const char* text, uint32_t len)
{
    const uint32_t n = text ? (len < 255 ? len : 255) : 0;
    rb_put(b, GPU_NULL_T_TEXT, 0, n);
    for (uint32_t k = 0; k < n; k += 8) {
        uint64_t w = 0;
        for (uint32_t j = 0; j < 8 && k + j < n; j++) w |= (uint64_t)(uint8_t)text[k + j] << (8 * j);
        rb_put(b, GPU_NULL_T_CHARS, 0, w);
    }
}

/* A record's footprint argument (put_fp's six after the flag) back. */
static void get_fp(const gpu_null_arg* a, gpu_footprint* f)
{
    f->offset = a[0].v;
    f->format = (gpu_format)a[1].v;
    f->width = (uint32_t)a[2].v;
    f->height = (uint32_t)a[3].v;
    f->depth = (uint32_t)a[4].v;
    f->row_pitch = (uint32_t)a[5].v;
}

/* ---------------------------------------------------------------------------
 * The text of a record, for the dump and the checks' messages
 * -------------------------------------------------------------------------*/

static void cat(char* out, size_t cap, size_t* k, const char* fmt, ...) NL_PRINTF(4, 5);
static void cat(char* out, size_t cap, size_t* k, const char* fmt, ...)
{
    if (*k + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(out + *k, cap - *k, fmt, ap);
    va_end(ap);
    if (w > 0) *k += (size_t)w < cap - *k ? (size_t)w : cap - *k - 1;
}

static void fmt_rec(const gpu_null_arg* r, char* out, size_t cap)
{
    size_t k = 0;
    out[0] = 0;
    cat(out, cap, &k, "%s", op_name(r->v));
    const uint32_t n = r->raw;
    for (uint32_t i = 1; i <= n; i++) {
        const gpu_null_arg* a = &r[i];
        switch (a->tag) {
        case GPU_NULL_T_U:    cat(out, cap, &k, " %llu", LLU(a->v)); break;
        case GPU_NULL_T_I:    cat(out, cap, &k, " %lld", (long long)(int64_t)a->v); break;
        case GPU_NULL_T_X:    cat(out, cap, &k, " 0x%llx", LLU(a->v)); break;
        case GPU_NULL_T_FLAG: cat(out, cap, &k, " ?%llu", LLU(a->v)); break;
        case GPU_NULL_T_F: {
            const uint32_t bits = (uint32_t)a->v;
            float f;
            memcpy(&f, &bits, sizeof f);
            cat(out, cap, &k, " %.9g", (double)f);
            break;
        }
        case GPU_NULL_T_PIPE: cat(out, cap, &k, " pipe:%016llx", LLU(a->v)); break;
        case GPU_NULL_T_ADDR:
            cat(out, cap, &k, " buf#%llu+%llu", LLU(a->v >> GPU_NULL_ADDR_BITS), LLU(a->v & NL_ADDR_MASK));
            break;
        case GPU_NULL_T_TABLE:
            cat(out, cap, &k, " tbl#%llu[%llu]", LLU(a->v >> 32), LLU(a->v & 0xFFFFFFFFu));
            break;
        case GPU_NULL_T_TEXT: {
            const uint32_t len = (uint32_t)a->v;
            cat(out, cap, &k, " \"");
            for (uint32_t j = 0; j < len && i + 1 + j / 8 <= n; j++) {
                const char ch = (char)(r[i + 1 + j / 8].v >> (8 * (j % 8)));
                cat(out, cap, &k, "%c", ch >= 32 && ch < 127 && ch != '"' ? ch : '?');
            }
            cat(out, cap, &k, "\"");
            i += (len + 7) / 8;
            break;
        }
        case GPU_NULL_T_CHARS: break;
        default:
            cat(out, cap, &k, " %s#%llu", tag_name(a->tag), LLU(a->v));
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Checks: counted by kind, the first of each printed
 * -------------------------------------------------------------------------*/

static void nl_verr(gpu_null_device* nd, int kind, const gpu_null_arg* r, const char* fmt, va_list ap) NL_PRINTF(4, 0);
static void nl_verr(gpu_null_device* nd, int kind, const gpu_null_arg* r, const char* fmt, va_list ap)
{
    at_inc32(&nd->err[kind]);
    if (at_xchg32(&nd->shown[kind], 1u)) return;
    char what[1024], why[512];
    if (r) fmt_rec(r, what, sizeof what);
    else snprintf(what, sizeof what, "-");
    vsnprintf(why, sizeof why, fmt, ap);
    fprintf(stderr, "[GPU null] check failed (%s): %s: %s\n", k_err_name[kind], what, why);
    fflush(stderr);
}

static void nl_err(gpu_null_device* nd, int kind, const gpu_null_arg* r, const char* fmt, ...) NL_PRINTF(4, 5);
static void nl_err(gpu_null_device* nd, int kind, const gpu_null_arg* r, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nl_verr(nd, kind, r, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------------------
 * The stream: hashed and dumped in order
 * -------------------------------------------------------------------------*/

/* gpu_null.h's bytes: the name and a zero, the argument count (32 bits),
 * then each argument's tag and value (64 bits), little-endian; text is
 * left out. */
static void hash_rec(gpu_null_device* nd, const gpu_null_arg* r)
{
    const char* name = op_name(r->v);
    uint64_t h = gpu_null_fnv(nd->hash, name, strlen(name) + 1);
    uint32_t n = 0;
    for (uint32_t i = 1; i <= r->raw; i++)
        if (r[i].tag != GPU_NULL_T_TEXT && r[i].tag != GPU_NULL_T_CHARS) n++;
    uint8_t le[9];
    for (int j = 0; j < 4; j++) le[j] = (uint8_t)(n >> (8 * j));
    h = gpu_null_fnv(h, le, 4);
    for (uint32_t i = 1; i <= r->raw; i++) {
        if (r[i].tag == GPU_NULL_T_TEXT || r[i].tag == GPU_NULL_T_CHARS) continue;
        le[0] = r[i].tag;
        for (int j = 0; j < 8; j++) le[1 + j] = (uint8_t)(r[i].v >> (8 * j));
        h = gpu_null_fnv(h, le, 9);
    }
    nd->hash = h;
}

static void emit(gpu_null_device* nd, const gpu_null_arg* r, int indent)
{
    hash_rec(nd, r);
    if (nd->dump) {
        char line[4096];
        fmt_rec(r, line, sizeof line);
        fprintf((FILE*)nd->dump, "%s%s\n", indent ? "  " : "", line);
    }
}

/* The device calls made since the last flush, into the stream. */
static void flush_events(gpu_null_device* nd)
{
    for (uint32_t i = 0; i < nd->ev.n; ) {
        const gpu_null_arg* r = &nd->ev.a[i];
        emit(nd, r, 0);
        i += 1 + r->raw;
    }
    nd->ev.n = 0;
}

/* ---------------------------------------------------------------------------
 * Formats
 * -------------------------------------------------------------------------*/

/* Bytes a texel, or a 4x4 block (*block set) for the BC formats; 0 for a
 * number this table does not know. */
static uint32_t fmt_bytes(gpu_format f, int* block)
{
    *block = 0;
    if (f >= 1 && f <= 4) return 16;
    if (f >= 5 && f <= 8) return 12;
    if (f >= 9 && f <= 22) return 8;
    if (f >= 23 && f <= 47) return 4;
    if (f >= 48 && f <= 59) return 2;
    if (f >= 60 && f <= 65) return 1;
    if (f >= 67 && f <= 69) return 4;
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) { *block = 1; return 8; }
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) { *block = 1; return 16; }
    if (f == 85 || f == 86 || f == 115) return 2;
    if (f >= 87 && f <= 93) return 4;
    return 0;
}

/* D3D12's planar depth-stencil formats: the stencil is plane 1. */
static uint32_t fmt_planes(gpu_format f)
{
    return (f == GPU_FORMAT_R24G8_TYPELESS || f == GPU_FORMAT_D24_UNORM_S8_UINT ||
            f == GPU_FORMAT_R32G8X24_TYPELESS || f == GPU_FORMAT_D32_FLOAT_S8X24_UINT) ? 2u : 1u;
}

/* A plane's own format for copies: a planar depth's depth as R32_TYPELESS
 * and its stencil as R8_TYPELESS (D3D12's footprints for these formats). */
static gpu_format plane_format(gpu_format f, uint32_t plane)
{
    if (fmt_planes(f) == 1) return f;
    return plane ? (gpu_format)60 /* R8_TYPELESS */ : GPU_FORMAT_R32_TYPELESS;
}

static int fmt_is_depth(gpu_format f)
{
    return f == GPU_FORMAT_D32_FLOAT || f == GPU_FORMAT_D24_UNORM_S8_UINT || f == GPU_FORMAT_D16_UNORM ||
           f == GPU_FORMAT_D32_FLOAT_S8X24_UINT;
}

static int fmt_is_int(gpu_format f)
{
    switch (f) {
    case 3: case 4: case 7: case 8: case 12: case 14: case 17: case 18: case 25: case 30: case 32: case 36:
    case 38: case 42: case 43: case 50: case 52: case 57: case 59: case 62: case 64:
        return 1;
    default:
        return 0;
    }
}

static int fmt_is_typeless(gpu_format f)
{
    switch (f) {
    case 1: case 5: case 9: case 15: case 19: case 23: case 27: case 33: case 39: case 44: case 48:
    case 53: case 60: case 70: case 73: case 76: case 79: case 82: case 90: case 92: case 94: case 97:
        return 1;
    default:
        return 0;
    }
}

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) / a * a;
}

/* GetCopyableFootprints as D3D12 lays out the formats the renderer uses:
 * rows of row_pitch (a multiple of 256) from offsets that are multiples of
 * 512, whole blocks for BC; the last row is not padded in the total. */
static uint64_t footprints(const gpu_texture_desc* desc, uint32_t first, uint32_t n, uint64_t base,
                           gpu_footprint* out, uint32_t* rows, uint64_t* row_bytes, uint64_t* padded)
{
    const uint32_t mips = desc->mips ? desc->mips : 1u, layers = desc->layers ? desc->layers : 1u;
    uint64_t off = base, end = base;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t s = first + i, mip = s % mips, plane = s / (mips * layers);
        uint32_t w = desc->width >> mip, h = desc->height >> mip;
        if (!w) w = 1;
        if (!h) h = 1;
        const gpu_format pf = plane_format(desc->format, plane);
        int block;
        uint32_t bytes = fmt_bytes(pf, &block);
        if (!bytes) bytes = 4;
        uint32_t nrows = h;
        uint64_t rb = (uint64_t)w * bytes;
        if (block) {
            w = (w + 3) & ~3u;
            h = (h + 3) & ~3u;
            nrows = h / 4;
            rb = (uint64_t)(w / 4) * bytes;
        }
        const uint64_t pitch = align_up(rb, 256);
        off = align_up(off, 512);
        if (out) {
            out[i].offset = off;
            out[i].format = pf;
            out[i].width = w;
            out[i].height = h;
            out[i].depth = 1;
            out[i].row_pitch = (uint32_t)pitch;
        }
        if (rows) rows[i] = nrows;
        if (row_bytes) row_bytes[i] = rb;
        end = off + (uint64_t)(nrows - 1) * pitch + rb;
        off += (uint64_t)nrows * pitch;
    }
    if (padded) *padded = off - base;
    return n ? end - base : 0;
}

/* A footprint's extent from its offset, and its rows' bytes; 0 when a row is
 * longer than its pitch (*bad). */
static uint64_t fp_extent(const gpu_footprint* fp, uint64_t* row_bytes, uint32_t* nrows, int* bad)
{
    int block;
    uint32_t bytes = fmt_bytes(fp->format, &block);
    if (!bytes) bytes = 4;
    const uint32_t r = block ? (fp->height + 3) / 4 : fp->height;
    const uint64_t rb = block ? (uint64_t)((fp->width + 3) / 4) * bytes : (uint64_t)fp->width * bytes;
    *row_bytes = rb;
    *nrows = r * fp->depth;
    *bad = rb > fp->row_pitch;
    if (!*nrows || !rb) return 0;
    return (uint64_t)(*nrows - 1) * fp->row_pitch + rb;
}

/* A subresource's size in texels (whole blocks for BC) and its texel or
 * block bytes. */
static void sub_size(const gpu_null_tex* t, uint32_t sub, uint32_t* w, uint32_t* h, uint32_t* bytes)
{
    const uint32_t mip = sub % t->mips, plane = sub / ((uint32_t)t->mips * t->layers);
    uint32_t ww = t->width >> mip, hh = t->height >> mip;
    if (!ww) ww = 1;
    if (!hh) hh = 1;
    int block;
    const uint32_t by = fmt_bytes(plane_format(t->format, plane), &block);
    if (block) {
        ww = (ww + 3) & ~3u;
        hh = (hh + 3) & ~3u;
    }
    *w = ww;
    *h = hh;
    if (bytes) *bytes = by;
}

/* ---------------------------------------------------------------------------
 * Handles in a record
 * -------------------------------------------------------------------------*/

static const gpu_null_slots* tag_slots(const gpu_null_device* nd, uint8_t tag)
{
    switch (tag) {
    case GPU_NULL_T_TEX:    return &nd->tex_slots;
    case GPU_NULL_T_BUF:
    case GPU_NULL_T_ADDR:   return &nd->buf_slots;
    case GPU_NULL_T_VIEW:   return &nd->view_slots;
    case GPU_NULL_T_SAMP:   return &nd->samp_slots;
    case GPU_NULL_T_LAYOUT: return &nd->layout_slots;
    case GPU_NULL_T_QPOOL:  return &nd->qpool_slots;
    case GPU_NULL_T_TPOOL:
    case GPU_NULL_T_TABLE:  return &nd->tpool_slots;
    case GPU_NULL_T_HEAP:   return &nd->heap_slots;
    case GPU_NULL_T_SWAP:   return &nd->swap_slots;
    case GPU_NULL_T_SHADER: return &nd->shader_slots;
    default:                return NULL;
    }
}

static int arg_live(gpu_null_device* nd, const gpu_null_arg* a)
{
    if (a->tag == GPU_NULL_T_PIPE) return pipe_valid(nd, a->raw);
    const gpu_null_slots* s = tag_slots(nd, a->tag);
    return !s || slot_valid(s, a->raw);
}

/* The texture a handle argument names (a view's own), or NULL. */
static gpu_null_tex* arg_tex(gpu_null_device* nd, const gpu_null_arg* a)
{
    if (a->tag == GPU_NULL_T_TEX) return tex_at(nd, a->raw);
    if (a->tag == GPU_NULL_T_VIEW) {
        const gpu_null_view* v = view_at(nd, a->raw);
        return v ? tex_at(nd, v->desc.texture.id) : NULL;
    }
    return NULL;
}

/* As a call is made or a command recorded: every handle is live, and a swap
 * chain's image is only used while acquired. Else the first bad one is
 * counted, and every bad one's raw cleared: the record is not run on it. */
static int handles_ok(gpu_null_device* nd, gpu_null_arg* r)
{
    int ok = 1;
    for (uint32_t i = 1; i <= r->raw; i++) {
        gpu_null_arg* a = &r[i];
        if (!a->raw) continue;
        if (!arg_live(nd, a)) {
            if (ok)
                nl_err(nd, GPU_NULL_ERR_HANDLE, r, "argument %u: %s handle 0x%08x is %s", i, tag_name(a->tag),
                       a->raw, a->tag == GPU_NULL_T_PIPE ? "never made" : why_bad(tag_slots(nd, a->tag), a->raw));
            a->raw = 0;
            ok = 0;
            continue;
        }
        const gpu_null_tex* t = arg_tex(nd, a);
        if (t && t->swap) {
            const gpu_null_swap* s = &nd->swap[t->swap - 1];
            if (!s->acquired || s->next != t->image) {
                if (ok)
                    nl_err(nd, GPU_NULL_ERR_SWAP, r, "argument %u: image %u of swap chain #%u is not acquired "
                           "(swap_acquire before using it; none after its present)", i, t->image, s->order);
                a->raw = 0;
                ok = 0;
            }
        }
    }
    return ok;
}

/* handles_ok, and the arguments in `required` (a bit each, from bit 1) not
 * none: a device call on the zero handle. */
static int handles_req(gpu_null_device* nd, gpu_null_arg* r, uint32_t required)
{
    if (!handles_ok(nd, r)) return 0;
    for (uint32_t i = 1; i <= r->raw && i < 32; i++)
        if (((required >> i) & 1u) && !r[i].raw) {
            nl_err(nd, GPU_NULL_ERR_HANDLE, r, "argument %u: no %s (the zero handle)", i, tag_name(r[i].tag));
            return 0;
        }
    return 1;
}

#define NL_ARG1 (1u << 1)

/* ---------------------------------------------------------------------------
 * Freeing, now or once the timeline reaches a fence
 * -------------------------------------------------------------------------*/

static void tex_free_now(gpu_null_device* nd, uint32_t id)
{
    gpu_null_tex* const t = &nd->tex[NL_IDX(id)];
    for (uint32_t v = t->views; v; ) {
        const uint32_t j = v - 1;
        v = nd->view[j].next;
        slot_free(&nd->view_slots, GPU_HANDLE_MAKE(j, nd->view_slots.gen[j]));
    }
    free(t->state);
    memset(t, 0, sizeof *t);
    slot_free(&nd->tex_slots, id);
}

static void buf_free_now(gpu_null_device* nd, uint32_t id)
{
    gpu_null_buf* const bf = &nd->buf[NL_IDX(id)];
    if (bf->memory != GPU_MEM_HOST) free(bf->mem);
    memset(bf, 0, sizeof *bf);
    slot_free(&nd->buf_slots, id);
}

static void free_now(gpu_null_device* nd, uint32_t kind, uint32_t id)
{
    if (kind == GPU_NULL_DEFER_TEXTURE) {
        if (slot_valid(&nd->tex_slots, id)) tex_free_now(nd, id);
    } else if (slot_valid(&nd->buf_slots, id)) {
        buf_free_now(nd, id);
    }
}

/* What the timeline has reached (all of it with all set). */
static void collect(gpu_null_device* nd, int all)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < nd->ndefer; i++) {
        const gpu_null_defer e = nd->defer[i];
        if (all || e.after <= nd->fence) free_now(nd, e.kind, e.id);
        else nd->defer[k++] = e;
    }
    nd->ndefer = k;
}

static void free_after(gpu_null_device* nd, uint32_t kind, uint32_t id, gpu_fence after)
{
    if (!after || after <= nd->fence) {
        free_now(nd, kind, id);
        return;
    }
    if (nd->ndefer == nd->defer_cap) {
        const uint32_t cap = nd->defer_cap ? nd->defer_cap * 2 : 256;
        gpu_null_defer* n = (gpu_null_defer*)realloc(nd->defer, cap * sizeof *n);
        if (!n) return;   /* (no memory: kept for good) */
        nd->defer = n;
        nd->defer_cap = cap;
    }
    nd->defer[nd->ndefer].kind = kind;
    nd->defer[nd->ndefer].id = id;
    nd->defer[nd->ndefer].after = after;
    nd->ndefer++;
}

/* A free of something the work ending at fence last_use used: with `after`
 * 0 the caller must have seen that fence pass; else `after` must be at or
 * past it. */
static void free_check(gpu_null_device* nd, const gpu_null_arg* r, uint64_t last_use, gpu_fence after)
{
    const uint64_t seen = at_load64(&nd->seen);
    if (!after && last_use > seen)
        nl_err(nd, GPU_NULL_ERR_FREED, r, "freed now, but the work using it ends at fence %llu, "
               "and the last fence seen to pass is %llu", LLU(last_use), LLU(seen));
    else if (after && after < last_use)
        nl_err(nd, GPU_NULL_ERR_FREED, r, "freed after fence %llu, but the work using it ends at fence %llu",
               LLU(after), LLU(last_use));
}

/* ---------------------------------------------------------------------------
 * Device: create, destroy, caps
 * -------------------------------------------------------------------------*/

static void nl_destroy(gpu_device* d);

/* Feature bits: what the D3D12 backend has on a desktop GPU, so the core
 * takes the same paths; cap_mask turns bits off to try a fallback. No
 * pipeline cache, present timing or fault report: there is nothing to keep
 * or show. */
static void caps_set(gpu_null_device* nd, const gpu_device_desc* desc)
{
    gpu_caps* const c = &nd->base.caps;
    uint32_t b = GPU_CAP_GS | GPU_CAP_D24S8 | GPU_CAP_DEPTH_CLAMP | GPU_CAP_MIRROR_ONCE | GPU_CAP_BORDER |
                 GPU_CAP_ANISO16 | GPU_CAP_SWIZZLE | GPU_CAP_BC | GPU_CAP_RGBA32F_BLEND | GPU_CAP_RGBA32F_FILTER |
                 GPU_CAP_OCCLUSION_PRECISE | GPU_CAP_HOST_IMPORT | GPU_CAP_NULL_DESCRIPTOR | GPU_CAP_TEARING |
                 GPU_CAP_PRESENT_MODE_SWITCH | GPU_CAP_HDR_SCRGB | GPU_CAP_HDR_METADATA |
                 GPU_CAP_UNUSED_ATTACHMENTS;
    b &= ~desc->cap_mask;
    c->api = GPU_API_NULL;
    c->bits = b;
    c->cb_align = 256;
    c->max_aniso = (b & GPU_CAP_ANISO16) ? 16u : 8u;
    c->host_import_align = (b & GPU_CAP_HOST_IMPORT) ? GPU_NULL_HOST_ALIGN : 0u;
}

static int tables_init(gpu_null_device* nd)
{
    int ok = slots_init(&nd->tex_slots, 256, 1u << 24, &nd->tex, sizeof *nd->tex) &&
             slots_init(&nd->buf_slots, 256, GPU_NULL_MAX_BUFFERS, &nd->buf, sizeof *nd->buf) &&
             slots_init(&nd->view_slots, 256, 1u << 24, &nd->view, sizeof *nd->view) &&
             slots_init(&nd->samp_slots, 64, 1u << 16, &nd->samp, sizeof *nd->samp) &&
             slots_init(&nd->shader_slots, GPU_NULL_MAX_SHADERS, GPU_NULL_MAX_SHADERS, &nd->shader,
                        sizeof *nd->shader) &&
             slots_init(&nd->layout_slots, 256, 256, &nd->layout, sizeof *nd->layout) &&
             slots_init(&nd->qpool_slots, GPU_NULL_MAX_QPOOLS, GPU_NULL_MAX_QPOOLS, NULL, 0) &&
             slots_init(&nd->heap_slots, 64, 1u << 16, &nd->heap, sizeof *nd->heap) &&
             slots_init(&nd->tpool_slots, GPU_NULL_MAX_TPOOLS, GPU_NULL_MAX_TPOOLS, NULL, 0) &&
             slots_init(&nd->swap_slots, GPU_NULL_MAX_SWAPS, GPU_NULL_MAX_SWAPS, NULL, 0);
    if (!ok) return 0;
    nd->samp_hash_cap = 1u << 17;
    nd->samp_hash = (uint32_t*)calloc(nd->samp_hash_cap, sizeof *nd->samp_hash);
    nd->pipe = (gpu_null_pipe*)calloc(GPU_NULL_MAX_PIPELINES, sizeof *nd->pipe);
    nd->pipe_hash = (uint32_t*)calloc(2u * GPU_NULL_MAX_PIPELINES, sizeof *nd->pipe_hash);
    return nd->samp_hash && nd->pipe && nd->pipe_hash;
}

gpu_device* gpu_null_create(const gpu_device_desc* desc);

/* ---------------------------------------------------------------------------
 * Buffers
 * -------------------------------------------------------------------------*/

static gpu_buffer buf_add(gpu_null_device* nd, uint64_t size, uint32_t use, uint32_t memory, gpu_state state,
                          uint8_t* mem)
{
    gpu_buffer r = {0};
    r.id = obj_alloc(&nd->buf_slots, &nd->buf, sizeof *nd->buf);
    if (!r.id) return r;
    gpu_null_buf* const bf = &nd->buf[NL_IDX(r.id)];
    memset(bf, 0, sizeof *bf);
    bf->order = nd->buf_slots.made;
    bf->size = size;
    bf->use = use;
    bf->memory = memory;
    bf->mem = mem;
    bf->state = memory == GPU_MEM_UPLOAD ? GPU_STATE_GENERIC_READ
              : memory == GPU_MEM_GPU ? state : GPU_STATE_COPY_DEST;
    return r;
}

/* Real memory, zeroed: maps and copies between buffers work. */
static gpu_buffer nl_buffer_create(gpu_device* d, const gpu_buffer_desc* desc)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    if (desc->memory < GPU_MEM_GPU || desc->memory > GPU_MEM_READBACK || !desc->size ||
        desc->size > NL_ADDR_MASK)
        return (gpu_buffer){0};
    NL_EV(buffer_create);
    put_buf(b, (gpu_buffer){0});
    put_u(b, desc->size);
    put_x(b, desc->use);
    put_u(b, desc->memory);
    put_x(b, desc->state);
    gpu_null_arg* const r = rb_end(b);
    uint8_t* mem = (uint8_t*)calloc(1, (size_t)desc->size);
    const gpu_buffer x = mem ? buf_add(nd, desc->size, desc->use, desc->memory, desc->state, mem) : (gpu_buffer){0};
    if (!x.id) {
        free(mem);
        if (r) rb_drop(b);
        return x;
    }
    if (!r) return x;
    r[1].raw = x.id;
    r[1].v = nd->buf[NL_IDX(x.id)].order;
    if ((desc->memory == GPU_MEM_UPLOAD && desc->state != GPU_STATE_GENERIC_READ) ||
        (desc->memory == GPU_MEM_READBACK && desc->state != GPU_STATE_COPY_DEST))
        nl_err(nd, GPU_NULL_ERR_STATE, r, "an upload buffer starts in GENERIC_READ (0xac3) and a readback one in "
               "COPY_DEST (0x400)");
    return x;
}

/* A buffer over the caller's memory, in COPY_DEST: copies into it land
 * there. Its address and size must be multiples of host_import_align. */
static gpu_buffer nl_buffer_import_host(gpu_device* d, void* host, uint64_t size)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    if (!(d->caps.bits & GPU_CAP_HOST_IMPORT) || !host || !size || size > NL_ADDR_MASK) return (gpu_buffer){0};
    NL_EV(buffer_import_host);
    put_buf(b, (gpu_buffer){0});
    put_u(b, size);
    gpu_null_arg* const r = rb_end(b);
    if (((uintptr_t)host % GPU_NULL_HOST_ALIGN) || (size % GPU_NULL_HOST_ALIGN)) {
        nl_err(nd, GPU_NULL_ERR_ALIGN, r, "host memory's address and size must be multiples of %u",
               GPU_NULL_HOST_ALIGN);
        if (r) rb_drop(b);
        return (gpu_buffer){0};
    }
    const gpu_buffer x = buf_add(nd, size, GPU_USE_COPY_DST, GPU_MEM_HOST, GPU_STATE_COPY_DEST, (uint8_t*)host);
    if (!r) return x;
    if (!x.id) {
        rb_drop(b);
        return x;
    }
    r[1].raw = x.id;
    r[1].v = nd->buf[NL_IDX(x.id)].order;
    return x;
}

/* The zero handle is nothing to free, as on D3D12; a stale one is a double
 * free. */
static void nl_buffer_destroy(gpu_device* d, gpu_buffer x, gpu_fence after)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    if (!x.id) return;
    NL_EV(buffer_destroy);
    put_buf(b, x);
    put_u(b, after);
    gpu_null_arg* const r = rb_end(b);
    if (!r) return;
    if (!handles_req(nd, r, NL_ARG1)) {
        rb_drop(b);
        return;
    }
    free_check(nd, r, nd->buf[NL_IDX(x.id)].last_use, after);
    free_after(nd, GPU_NULL_DEFER_BUFFER, x.id, after);
}

/* GPU memory has no CPU pointer, as on D3D12 and on Vulkan's device-local
 * memory. A readback read before the fence of the work writing it is a race
 * on a real GPU. */
static void* nl_buffer_map(gpu_device* d, gpu_buffer x, uint64_t read_offset, uint64_t read_size)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    NL_CALL(buffer_map);
    put_buf(b, x);
    put_u(b, read_offset);
    put_u(b, read_size);
    gpu_null_arg* const r = rb_end(b);
    if (!r || !handles_req(nd, r, NL_ARG1)) return NULL;
    gpu_null_buf* const bf = &nd->buf[NL_IDX(x.id)];
    if (bf->memory == GPU_MEM_GPU) {
        nl_err(nd, GPU_NULL_ERR_USE, r, "buffer #%u is GPU memory: no CPU pointer", bf->order);
        return NULL;
    }
    if (read_offset > bf->size || read_size > bf->size - read_offset)
        nl_err(nd, GPU_NULL_ERR_BOUNDS, r, "reads past buffer #%u's %llu bytes", bf->order, LLU(bf->size));
    else if (read_size && bf->last_write > at_load64(&nd->seen))
        nl_err(nd, GPU_NULL_ERR_FENCE, r, "reads buffer #%u before the fence of the work writing it (%llu) "
               "was seen to pass (%llu)", bf->order, LLU(bf->last_write), LLU(at_load64(&nd->seen)));
    return bf->mem;
}

static void nl_buffer_unmap(gpu_device* d, gpu_buffer x, uint64_t written_offset, uint64_t written_size)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    NL_CALL(buffer_unmap);
    put_buf(b, x);
    put_u(b, written_offset);
    put_u(b, written_size);
    gpu_null_arg* const r = rb_end(b);
    if (!r || !handles_req(nd, r, NL_ARG1)) return;
    const gpu_null_buf* const bf = &nd->buf[NL_IDX(x.id)];
    if (written_offset > bf->size || written_size > bf->size - written_offset)
        nl_err(nd, GPU_NULL_ERR_BOUNDS, r, "writes past buffer #%u's %llu bytes", bf->order, LLU(bf->size));
}

gpu_addr gpu_null_fast_buffer_addr(gpu_device* d, gpu_buffer x)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    if (slot_valid(&nd->buf_slots, x.id))
        return ((uint64_t)GPU_HANDLE_GEN(x.id) << 56) | ((uint64_t)(NL_IDX(x.id) + 1u) << GPU_NULL_ADDR_BITS);
    NL_CALL(buffer_addr);
    put_buf(b, x);
    gpu_null_arg* const r = rb_end(b);
    if (r) handles_req(nd, r, NL_ARG1);
    return 0;
}

static gpu_addr nl_buffer_addr(gpu_device* d, gpu_buffer x)
{
    return gpu_fast_buffer_addr(d, x);
}

/* ---------------------------------------------------------------------------
 * Heaps and textures
 * -------------------------------------------------------------------------*/

static gpu_heap nl_heap_create(gpu_device* d, const gpu_heap_desc* desc)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_heap h = {0};
    if (!desc->size) return h;
    h.id = obj_alloc(&nd->heap_slots, &nd->heap, sizeof *nd->heap);
    if (!h.id) return h;
    gpu_null_heap* const p = &nd->heap[NL_IDX(h.id)];
    p->order = nd->heap_slots.made;
    p->size = desc->size;
    p->flags = desc->flags;
    NL_EV(heap_create);
    put_heap(b, h);
    put_u(b, desc->size);
    put_u(b, desc->memory);
    put_x(b, desc->flags);
    rb_end(b);
    return h;
}

/* D3D12's: the subresources' footprints, 64 KB aligned (4 MB with MSAA). */
static void nl_texture_size(gpu_device* d, const gpu_texture_desc* desc, uint64_t* size, uint64_t* align)
{
    (void)d;
    const uint32_t mips = desc->mips ? desc->mips : 1u, layers = desc->layers ? desc->layers : 1u;
    const uint32_t samples = desc->samples ? desc->samples : 1u;
    uint64_t padded = 0;
    footprints(desc, 0, mips * layers * fmt_planes(desc->format), 0, NULL, NULL, NULL, &padded);
    const uint64_t a = samples > 1 ? 4194304u : 65536u;
    if (size) *size = align_up(padded * samples, a);
    if (align) *align = a;
}

static uint64_t nl_texture_footprints(gpu_device* d, const gpu_texture_desc* desc, uint32_t first_sub, uint32_t n,
                                      uint64_t base_offset, gpu_footprint* out, uint32_t* rows, uint64_t* row_bytes)
{
    (void)d;
    return footprints(desc, first_sub, n, base_offset, out, rows, row_bytes, NULL);
}

/* A texture's record: no memory, each subresource's state. */
static gpu_texture tex_make(gpu_null_device* nd, const gpu_texture_desc* desc, uint32_t swap, uint32_t image)
{
    gpu_texture r = {0};
    const uint32_t mips = desc->mips ? desc->mips : 1u, layers = desc->layers ? desc->layers : 1u;
    const uint32_t planes = fmt_planes(desc->format);
    const uint64_t nsub = (uint64_t)mips * layers * planes;
    if (!desc->width || !desc->height || nsub > (1u << 20)) return r;
    uint32_t* st = (uint32_t*)malloc((size_t)nsub * sizeof *st);
    if (!st) return r;
    for (uint64_t k = 0; k < nsub; k++) st[k] = desc->state;
    r.id = obj_alloc(&nd->tex_slots, &nd->tex, sizeof *nd->tex);
    if (!r.id) {
        free(st);
        return r;
    }
    gpu_null_tex* const t = &nd->tex[NL_IDX(r.id)];
    memset(t, 0, sizeof *t);
    t->order = nd->tex_slots.made;
    t->width = desc->width;
    t->height = desc->height;
    t->layers = (uint16_t)layers;
    t->mips = (uint16_t)mips;
    t->format = desc->format;
    t->samples = desc->samples ? desc->samples : 1u;
    t->use = desc->use;
    t->planes = planes;
    t->nsub = (uint32_t)nsub;
    t->state = st;
    t->swap = swap;
    t->image = image;
    return r;
}

static gpu_texture nl_texture_create(gpu_device* d, const gpu_texture_desc* desc)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    NL_EV(texture_create);
    put_tex(b, (gpu_texture){0});
    put_u(b, desc->width);
    put_u(b, desc->height);
    put_u(b, desc->layers);
    put_u(b, desc->mips);
    put_u(b, desc->format);
    put_u(b, desc->samples);
    put_x(b, desc->use);
    put_x(b, desc->state);
    put_heap(b, desc->heap);
    put_u(b, desc->heap_offset);
    put_u(b, desc->clear_format);
    gpu_null_arg* const r = rb_end(b);
    if (!r) return tex_make(nd, desc, 0, 0);
    if (GPU_VALID(desc->heap)) {   /* placed: inside its heap, at its alignment */
        if (!handles_ok(nd, r)) {
            rb_drop(b);
            return (gpu_texture){0};
        }
        const gpu_null_heap* const h = &nd->heap[NL_IDX(desc->heap.id)];
        uint64_t size, align;
        nl_texture_size(d, desc, &size, &align);
        if (desc->heap_offset % align) {
            nl_err(nd, GPU_NULL_ERR_ALIGN, r, "placed at %llu, not a multiple of %llu", LLU(desc->heap_offset),
                   LLU(align));
            rb_drop(b);
            return (gpu_texture){0};
        }
        if (desc->heap_offset > h->size || size > h->size - desc->heap_offset) {
            nl_err(nd, GPU_NULL_ERR_BOUNDS, r, "%llu bytes at %llu: heap #%u holds %llu", LLU(size),
                   LLU(desc->heap_offset), h->order, LLU(h->size));
            rb_drop(b);
            return (gpu_texture){0};
        }
        if ((h->flags & GPU_HEAP_SAMPLED_TEXTURES) && (desc->use & (GPU_USE_RT | GPU_USE_DS))) {
            nl_err(nd, GPU_NULL_ERR_USE, r, "a target in heap #%u, which holds only sampled textures", h->order);
            rb_drop(b);
            return (gpu_texture){0};
        }
    }
    const gpu_texture t = tex_make(nd, desc, 0, 0);
    if (!t.id) {
        rb_drop(b);
        return t;
    }
    r[1].raw = t.id;
    r[1].v = nd->tex[NL_IDX(t.id)].order;
    return t;
}

/* A swap chain's image is its swap chain's to free, as on D3D12. */
static void nl_texture_destroy(gpu_device* d, gpu_texture t, gpu_fence after)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    const gpu_null_tex* const tx = tex_at(nd, t.id);
    if (!t.id || (tx && tx->swap)) return;
    NL_EV(texture_destroy);
    put_tex(b, t);
    put_u(b, after);
    gpu_null_arg* const r = rb_end(b);
    if (!r) return;
    if (!handles_req(nd, r, NL_ARG1)) {
        rb_drop(b);
        return;
    }
    free_check(nd, r, tx->last_use, after);
    free_after(nd, GPU_NULL_DEFER_TEXTURE, t.id, after);
}

/* A target view: once per description, kept with its texture. Its texture
 * must be made for that kind of target. */
static gpu_view nl_view_get(gpu_device* d, const gpu_view_desc* desc)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_view v = {0};
    if (desc->kind != GPU_VIEW_RTV && desc->kind != GPU_VIEW_DSV) return v;
    gpu_null_tex* const t = tex_at(nd, desc->texture.id);
    if (t)
        for (uint32_t k = t->views; k; k = nd->view[k - 1].next)
            if (!memcmp(&nd->view[k - 1].desc, desc, sizeof *desc)) {
                v.id = GPU_HANDLE_MAKE(k - 1, nd->view_slots.gen[k - 1]);
                return v;
            }
    NL_EV(view_create);
    put_view(b, v);
    put_tex(b, desc->texture);
    put_u(b, desc->kind);
    put_u(b, desc->dim);
    put_x(b, desc->flags);
    put_u(b, desc->format);
    put_u(b, desc->mip);
    put_u(b, desc->layer);
    gpu_null_arg* const r = rb_end(b);
    if (r && !t) {
        nl_err(nd, GPU_NULL_ERR_HANDLE, r, "texture handle 0x%08x is %s", desc->texture.id,
               why_bad(&nd->tex_slots, desc->texture.id));
        rb_drop(b);
        return v;
    }
    if (!t) return v;
    const uint32_t need = desc->kind == GPU_VIEW_RTV ? GPU_USE_RT : GPU_USE_DS;
    if (r && !(t->use & need))
        nl_err(nd, GPU_NULL_ERR_USE, r, "texture #%u has no GPU_USE_%s", t->order,
               desc->kind == GPU_VIEW_RTV ? "RT" : "DS");
    else if (r && (desc->mip >= t->mips || desc->layer >= t->layers))
        nl_err(nd, GPU_NULL_ERR_BOUNDS, r, "mip %u layer %u: texture #%u has %u and %u", desc->mip, desc->layer,
               t->order, t->mips, t->layers);
    v.id = obj_alloc(&nd->view_slots, &nd->view, sizeof *nd->view);
    if (!v.id) {
        if (r) rb_drop(b);
        return v;
    }
    gpu_null_tex* const t2 = tex_at(nd, desc->texture.id);   /* (the views' array may have moved; textures not) */
    gpu_null_view* const w = &nd->view[NL_IDX(v.id)];
    w->order = nd->view_slots.made;
    w->desc = *desc;
    w->next = t2->views;
    t2->views = NL_IDX(v.id) + 1;
    if (r) {
        r[1].raw = v.id;
        r[1].v = w->order;
    }
    return v;
}

/* ---------------------------------------------------------------------------
 * Samplers and tables
 * -------------------------------------------------------------------------*/

static gpu_sampler nl_sampler_get(gpu_device* d, const gpu_sampler_desc* desc)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_sampler s = {0};
    const uint32_t mask = nd->samp_hash_cap - 1;
    uint32_t i = (uint32_t)gpu_null_fnv(GPU_NULL_FNV_BASIS, desc, sizeof *desc) & mask;
    for (;; i = (i + 1) & mask) {
        const uint32_t id = nd->samp_hash[i];
        if (!id) break;
        if (!memcmp(&nd->samp[NL_IDX(id)].desc, desc, sizeof *desc)) {
            s.id = id;
            return s;
        }
    }
    s.id = obj_alloc(&nd->samp_slots, &nd->samp, sizeof *nd->samp);
    if (!s.id) return s;
    gpu_null_samp* const p = &nd->samp[NL_IDX(s.id)];
    p->order = nd->samp_slots.made;
    p->desc = *desc;
    nd->samp_hash[i] = s.id;
    NL_EV(sampler_create);
    put_samp(b, s);
    put_x(b, desc->filter);
    put_u(b, desc->address_u);
    put_u(b, desc->address_v);
    put_u(b, desc->address_w);
    put_f(b, desc->mip_lod_bias);
    put_u(b, desc->max_aniso);
    put_u(b, desc->compare);
    for (int k = 0; k < 4; k++) put_f(b, desc->border[k]);
    put_f(b, desc->min_lod);
    put_f(b, desc->max_lod);
    gpu_null_arg* const r = rb_end(b);
    if (r && GPU_FILTER_IS_ANISO(desc->filter) && (desc->max_aniso < 1 || desc->max_aniso > d->caps.max_aniso))
        nl_err(nd, GPU_NULL_ERR_BOUNDS, r, "anisotropy %u: 1 to %u", desc->max_aniso, d->caps.max_aniso);
    return s;
}

/* A new pool's texture entries are empty R8_UNORM ones (the D3D12 backend's
 * null_r8); its sampler entries none. */
static gpu_table_pool nl_table_pool_create(gpu_device* d, const gpu_table_pool_desc* desc)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_table_pool r = {0};
    if (!desc->count || (desc->kind != GPU_TABLE_TEX && desc->kind != GPU_TABLE_SAMPLER)) return r;
    const int samp = desc->kind == GPU_TABLE_SAMPLER;
    void* memo = calloc(desc->count, samp ? sizeof(uint32_t) : sizeof(gpu_view_desc));
    uint64_t* last = (uint64_t*)calloc(desc->count, sizeof *last);
    uint16_t* pend = (uint16_t*)calloc(desc->count, sizeof *pend);
    if (memo && last && pend) r.id = obj_alloc(&nd->tpool_slots, NULL, 0);
    if (!r.id) {
        free(memo);
        free(last);
        free(pend);
        return r;
    }
    gpu_null_tpool* const p = &nd->tpool[NL_IDX(r.id)];
    memset(p, 0, sizeof *p);
    p->order = nd->tpool_slots.made;
    p->kind = desc->kind;
    p->count = desc->count;
    p->desc = *desc;
    p->memo = memo;
    p->last_fence = last;
    p->pending = pend;
    if (!samp) {
        gpu_view_desc* const m = (gpu_view_desc*)memo;
        for (uint32_t k = 0; k < desc->count; k++) {
            m[k].mips = 1;
            m[k].format = GPU_FORMAT_R8_UNORM;
            m[k].swizzle = GPU_SWIZZLE_IDENTITY;
        }
    }
    NL_EV(table_pool_create);
    put_tpool(b, r);
    put_u(b, desc->kind);
    put_u(b, desc->count);
    put_u(b, desc->nregions);
    for (uint32_t k = 0; k < GPU_MAX_TABLE_REGIONS; k++) {
        const int on = k < desc->nregions;
        put_u(b, on ? desc->region[k].first : 0);
        put_u(b, on ? desc->region[k].tables : 0);
        put_u(b, on ? desc->region[k].size : 0);
    }
    gpu_null_arg* const rec = rb_end(b);
    if (desc->nregions > GPU_MAX_TABLE_REGIONS) {
        nl_err(nd, GPU_NULL_ERR_TABLE, rec, "%u regions: at most %u", desc->nregions, (uint32_t)GPU_MAX_TABLE_REGIONS);
        p->desc.nregions = GPU_MAX_TABLE_REGIONS;
    }
    for (uint32_t k = 0; k < p->desc.nregions; k++) {
        const gpu_table_region* g = &desc->region[k];
        if ((uint64_t)g->first + (uint64_t)g->tables * g->size > desc->count) {
            nl_err(nd, GPU_NULL_ERR_TABLE, rec, "region %u (%u tables of %u from %u) is outside the pool's %u",
                   k, g->tables, g->size, g->first, desc->count);
            break;
        }
    }
    return r;
}

/* The size of the table starting at index, from the pool's regions: Vulkan
 * has a descriptor set only there. 0 when no table starts at index. */
static uint32_t table_size_at(const gpu_null_tpool* p, uint32_t index)
{
    for (uint32_t k = 0; k < p->desc.nregions && k < GPU_MAX_TABLE_REGIONS; k++) {
        const gpu_table_region* g = &p->desc.region[k];
        if (!g->size || index < g->first) continue;
        const uint32_t off = index - g->first;
        if (off % g->size == 0 && off / g->size < g->tables) return g->size;
    }
    return 0;
}

/* An entry is written only once the GPU is done with it (gpu.h): no
 * unsubmitted draw reads it, and the fence of the last work that read it was
 * seen to pass. */
static void entry_free_check(gpu_null_device* nd, const gpu_null_arg* r, const gpu_null_tpool* p, uint32_t index)
{
    const uint64_t seen = at_load64(&nd->seen);
    if (p->pending[index])
        nl_err(nd, GPU_NULL_ERR_TABLE, r, "entry %u is read by draws recorded and not yet submitted "
               "(recorder bits 0x%x)", index, p->pending[index]);
    else if (p->last_fence[index] > seen)
        nl_err(nd, GPU_NULL_ERR_TABLE, r, "entry %u is read by work ending at fence %llu; the last fence seen "
               "to pass is %llu", index, LLU(p->last_fence[index]), LLU(seen));
}

/* The memo first (the D3D12 backend's, so the same writes reach the stream):
 * an entry already holding this description is left as it is. */
void gpu_null_fast_table_write_tex(gpu_device* d, gpu_table_pool pool, uint32_t index, const gpu_view_desc* srv)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_view_desc v;
    if (srv) {
        v = *srv;
    } else {
        memset(&v, 0, sizeof v);
        v.mips = 1;
        v.format = GPU_FORMAT_R8G8B8A8_UNORM;
    }
    if (!v.swizzle) v.swizzle = GPU_SWIZZLE_IDENTITY;
    gpu_null_tpool* const p = tpool_at(nd, pool.id);
    if (p && p->kind == GPU_TABLE_TEX && index < p->count &&
        !memcmp(&((const gpu_view_desc*)p->memo)[index], &v, sizeof v))
        return;
    NL_EV(table_write_tex);
    put_tpool(b, pool);
    put_u(b, index);
    put_tex(b, v.texture);
    put_u(b, v.kind);
    put_u(b, v.dim);
    put_x(b, v.flags);
    put_u(b, v.mips);
    put_u(b, v.format);
    put_x(b, v.swizzle);
    put_u(b, v.mip);
    put_u(b, v.layer);
    gpu_null_arg* const r = rb_end(b);
    if (!r) return;
    if (!p || p->kind != GPU_TABLE_TEX) {
        if (handles_req(nd, r, NL_ARG1)) nl_err(nd, GPU_NULL_ERR_HANDLE, r, "pool #%u holds samplers", p->order);
        rb_drop(b);
        return;
    }
    if (index >= p->count) {
        nl_err(nd, GPU_NULL_ERR_TABLE, r, "entry %u: pool #%u has %u", index, p->order, p->count);
        rb_drop(b);
        return;
    }
    if (handles_ok(nd, r)) {
        const gpu_null_tex* const t = tex_at(nd, v.texture.id);
        if (t && !(t->use & GPU_USE_SAMPLED))
            nl_err(nd, GPU_NULL_ERR_USE, r, "texture #%u has no GPU_USE_SAMPLED", t->order);
        else if (t && (v.mip >= t->mips || (v.mips && (uint32_t)v.mip + v.mips > t->mips) || v.layer >= t->layers))
            nl_err(nd, GPU_NULL_ERR_BOUNDS, r, "mips %u+%u layer %u: texture #%u has %u mips and %u layers",
                   v.mip, v.mips, v.layer, t->order, t->mips, t->layers);
        else
            entry_free_check(nd, r, p, index);
    } else {
        v.texture.id = 0;   /* (counted: a draw reading it is not counted again) */
    }
    ((gpu_view_desc*)p->memo)[index] = v;
    nd->counts.table_writes++;
}

void gpu_null_fast_table_write_sampler(gpu_device* d, gpu_table_pool pool, uint32_t index, gpu_sampler s)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_null_tpool* const p = tpool_at(nd, pool.id);
    if (p && p->kind == GPU_TABLE_SAMPLER && index < p->count && ((const uint32_t*)p->memo)[index] == s.id)
        return;
    NL_EV(table_write_sampler);
    put_tpool(b, pool);
    put_u(b, index);
    put_samp(b, s);
    gpu_null_arg* const r = rb_end(b);
    if (!r) return;
    if (!handles_req(nd, r, NL_ARG1 | (1u << 3))) {
        rb_drop(b);
        return;
    }
    if (p->kind != GPU_TABLE_SAMPLER || index >= p->count) {
        if (p->kind != GPU_TABLE_SAMPLER) nl_err(nd, GPU_NULL_ERR_HANDLE, r, "pool #%u holds textures", p->order);
        else nl_err(nd, GPU_NULL_ERR_TABLE, r, "entry %u: pool #%u has %u", index, p->order, p->count);
        rb_drop(b);
        return;
    }
    entry_free_check(nd, r, p, index);
    ((uint32_t*)p->memo)[index] = s.id;
    nd->counts.table_writes++;
}

gpu_table gpu_null_fast_table_ref(gpu_device* d, gpu_table_pool pool, uint32_t index)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_table t;
    t.v = ((uint64_t)pool.id << 32) | index;
    const gpu_null_tpool* const p = tpool_at(nd, pool.id);
    if (p && index < p->count && table_size_at(p, index)) return t;
    NL_CALL(table_ref);
    put_tpool(b, pool);
    put_u(b, index);
    gpu_null_arg* const r = rb_end(b);
    if (!r || !handles_req(nd, r, NL_ARG1)) return t;
    if (index >= p->count)
        nl_err(nd, GPU_NULL_ERR_TABLE, r, "entry %u: pool #%u has %u", index, p->order, p->count);
    else
        nl_err(nd, GPU_NULL_ERR_TABLE, r, "no table of pool #%u's regions starts at entry %u (Vulkan has "
               "a descriptor set only there)", p->order, index);
    return t;
}

static void nl_table_write_tex(gpu_device* d, gpu_table_pool pool, uint32_t index, const gpu_view_desc* srv)
{
    gpu_fast_table_write_tex(d, pool, index, srv);
}

static void nl_table_write_sampler(gpu_device* d, gpu_table_pool pool, uint32_t index, gpu_sampler s)
{
    gpu_fast_table_write_sampler(d, pool, index, s);
}

static gpu_table nl_table_ref(gpu_device* d, gpu_table_pool pool, uint32_t index)
{
    return gpu_fast_table_ref(d, pool, index);
}

/* ---------------------------------------------------------------------------
 * Shaders, layouts, pipelines, queries
 * -------------------------------------------------------------------------*/

/* No shader target of its own: it runs none. */
static int nl_compile_hook(gpu_device* d, const void* request, void* result)
{
    (void)d; (void)request; (void)result;
    return 0;
}

/* Any code is taken (nothing runs it); its hash names the pipelines. Thread-safe. */
static gpu_shader nl_shader_create(gpu_device* d, const gpu_shader_desc* desc)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_shader s = {0};
    if (!desc->bytes || !desc->size || desc->stage > GPU_STAGE_PS) return s;
    const uint64_t h = gpu_null_fnv(GPU_NULL_FNV_BASIS, desc->bytes, (size_t)desc->size);
    nl_lock(nd);
    s.id = obj_alloc(&nd->shader_slots, &nd->shader, sizeof *nd->shader);
    if (s.id) {
        gpu_null_shader* const p = &nd->shader[NL_IDX(s.id)];
        p->hash = h;
        p->stage = desc->stage;
        p->code = desc->code;
    }
    nl_unlock(nd);
    return s;
}

static void nl_shader_destroy(gpu_device* d, gpu_shader s)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    nl_lock(nd);
    if (slot_valid(&nd->shader_slots, s.id)) {
        memset(&nd->shader[NL_IDX(s.id)], 0, sizeof nd->shader[0]);
        slot_free(&nd->shader_slots, s.id);
    }
    nl_unlock(nd);
}

static uint64_t layout_hash(const gpu_layout_desc* l, uint32_t n, uint32_t ns)
{
    uint64_t h = gpu_null_fnv(GPU_NULL_FNV_BASIS, &l->flags, sizeof l->flags);
    h = gpu_null_fnv(h, &n, sizeof n);
    h = gpu_null_fnv(h, l->slot, n * sizeof l->slot[0]);
    h = gpu_null_fnv(h, &ns, sizeof ns);
    return gpu_null_fnv(h, l->statics, ns * sizeof l->statics[0]);
}

static gpu_layout nl_layout_create(gpu_device* d, const gpu_layout_desc* desc)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_layout r = {0};
    const uint32_t n = desc->nslots < GPU_MAX_SLOTS ? desc->nslots : GPU_MAX_SLOTS;
    const uint32_t ns = desc->nstatic < GPU_MAX_STATIC_SAMPLERS ? desc->nstatic : GPU_MAX_STATIC_SAMPLERS;
    const uint64_t h = layout_hash(desc, n, ns);
    nl_lock(nd);   /* pipeline workers read layouts */
    r.id = obj_alloc(&nd->layout_slots, &nd->layout, sizeof *nd->layout);
    if (r.id) {
        gpu_null_layout* const p = &nd->layout[NL_IDX(r.id)];
        p->order = nd->layout_slots.made;
        p->hash = h;
        p->desc = *desc;
        p->desc.nslots = n;
        p->desc.nstatic = ns;
    }
    nl_unlock(nd);
    if (!r.id) return r;
    NL_EV(layout_create);
    put_layout(b, r);
    put_x(b, desc->flags);
    put_u(b, n);
    for (uint32_t k = 0; k < n; k++) {
        const gpu_layout_slot* s = &desc->slot[k];
        put_x(b, (uint32_t)s->kind | (uint32_t)s->reg << 8 | (uint32_t)s->count << 16 | (uint32_t)s->visibility << 24);
    }
    put_u(b, ns);
    put_x(b, h);
    gpu_null_arg* const rec = rb_end(b);
    if (desc->nslots > GPU_MAX_SLOTS || desc->nstatic > GPU_MAX_STATIC_SAMPLERS)
        nl_err(nd, GPU_NULL_ERR_BOUNDS, rec, "%u slots and %u static samplers: at most %u and %u", desc->nslots,
               desc->nstatic, (uint32_t)GPU_MAX_SLOTS, (uint32_t)GPU_MAX_STATIC_SAMPLERS);
    for (uint32_t k = 0; k < n; k++)
        if (desc->slot[k].kind > GPU_SLOT_TABLE_SAMPLER ||
            (desc->slot[k].kind >= GPU_SLOT_CONST32 && !desc->slot[k].count)) {
            nl_err(nd, GPU_NULL_ERR_BOUNDS, rec, "slot %u: kind %u, count %u", k, desc->slot[k].kind,
                   desc->slot[k].count);
            break;
        }
    return r;
}

/* As the D3D12 backend names a pipeline for its library: the description's
 * bytes with the shaders' code hashes and the layout's in place of their
 * handles. Under the lock. */
static uint64_t pipe_name(gpu_null_device* nd, const gpu_pipeline_desc* desc)
{
    gpu_pipeline_desc c;
    memcpy(&c, desc, sizeof c);
    c.layout.id = c.vs.id = c.gs.id = c.ps.id = 0;
    uint64_t h = gpu_null_fnv(GPU_NULL_FNV_BASIS, &c, sizeof c);
    const gpu_shader sh[3] = { desc->vs, desc->gs, desc->ps };
    for (int k = 0; k < 3; k++) {
        const uint64_t s = slot_valid(&nd->shader_slots, sh[k].id) ? nd->shader[NL_IDX(sh[k].id)].hash : 0;
        h = gpu_null_fnv(h, &s, sizeof s);
    }
    const uint64_t l = slot_valid(&nd->layout_slots, desc->layout.id) ? nd->layout[NL_IDX(desc->layout.id)].hash : 0;
    return gpu_null_fnv(h, &l, sizeof l);
}

/* One pipeline per contents: the same description again is the same
 * pipeline (always GPU_PIPELINE_COMPILED: there is no cache). A
 * GPU_PIPELINE_STORE_ONLY one is checked as any other, counted, and keeps
 * nothing: no entry, so no handle can name it (a bind of one is a never-made
 * pipeline, a HANDLE failure). Thread-safe. */
static gpu_pipeline nl_pipeline_create(gpu_device* d, const gpu_pipeline_desc* desc, uint32_t flags, uint32_t* how)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_pipeline r = {0};
    if (how) *how = GPU_PIPELINE_FAILED;
    const char* bad = NULL;
    nl_lock(nd);
    const gpu_shader sh[3] = { desc->vs, desc->gs, desc->ps };
    const uint32_t stage[3] = { GPU_STAGE_VS, GPU_STAGE_GS, GPU_STAGE_PS };
    if (!slot_valid(&nd->layout_slots, desc->layout.id)) bad = "its layout is not a live layout";
    else if (!GPU_VALID(desc->vs)) bad = "no vertex shader";
    for (int k = 0; k < 3 && !bad; k++) {
        if (!GPU_VALID(sh[k])) continue;
        if (!slot_valid(&nd->shader_slots, sh[k].id)) bad = "a shader handle is stale or freed";
        else if (nd->shader[NL_IDX(sh[k].id)].stage != stage[k]) bad = "a shader is of another stage";
    }
    if (!bad && desc->nrt > 4) bad = "more than 4 colour targets";
    uint32_t idx1 = 0;
    if (!bad && (flags & GPU_PIPELINE_STORE_ONLY)) {
        nd->stored_only++;
        nl_unlock(nd);
        if (how) *how = GPU_PIPELINE_COMPILED;
        return r;
    }
    if (!bad) {
        const uint64_t name = pipe_name(nd, desc);
        const uint32_t mask = 2u * GPU_NULL_MAX_PIPELINES - 1u;
        uint32_t i = (uint32_t)name & mask;
        for (;; i = (i + 1) & mask) {
            const uint32_t k = nd->pipe_hash[i];
            if (!k) break;
            if (nd->pipe[k - 1].name == name) { idx1 = k; break; }
        }
        if (!idx1 && nd->npipe < GPU_NULL_MAX_PIPELINES) {
            gpu_null_pipe* const p = &nd->pipe[nd->npipe];
            p->name = name;
            p->layout_hash = nd->layout[NL_IDX(desc->layout.id)].hash;
            p->topology_type = desc->topology_type;
            p->depth_write = desc->depth_write;
            p->vertex_count = desc->vertex.count;
            idx1 = nd->npipe + 1;
            nd->pipe_hash[i] = idx1;
            at_store32(&nd->npipe, idx1);   /* last: set_pipeline reads the entry without the lock */
        }
    }
    nl_unlock(nd);
    if (bad) {
        gpu_null_vec v = {0};   /* (a worker's own: the device's scratch is the render thread's) */
        nl_rb b_;
        nl_rb* const b = &b_;
        rb_begin(b, nd, &v, GPU_NULL_OP_pipeline_create);
        rb_put(b, GPU_NULL_T_LAYOUT, desc->layout.id, 0);
        rb_put(b, GPU_NULL_T_SHADER, desc->vs.id, 0);
        rb_put(b, GPU_NULL_T_SHADER, desc->gs.id, 0);
        rb_put(b, GPU_NULL_T_SHADER, desc->ps.id, 0);
        put_u(b, desc->nrt);
        nl_err(nd, GPU_NULL_ERR_HANDLE, rb_end(b), "%s", bad);
        free(v.a);
        return r;
    }
    if (idx1 && how) *how = GPU_PIPELINE_COMPILED;
    r.v = idx1;
    return r;
}

static int nl_pipeline_cache_load(gpu_device* d, const char* path)
{
    (void)d; (void)path;
    return GPU_PIPELINE_CACHE_NONE;
}

static int nl_pipeline_cache_save(gpu_device* d, const char* path)
{
    (void)d; (void)path;
    return 0;
}

static gpu_query_pool nl_query_pool_create(gpu_device* d, uint32_t type, uint32_t count)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_query_pool r = {0};
    if ((type != GPU_QUERY_OCCLUSION && type != GPU_QUERY_BINARY_OCCLUSION) || !count) return r;
    r.id = obj_alloc(&nd->qpool_slots, NULL, 0);
    if (!r.id) return r;
    gpu_null_qpool* const q = &nd->qpool[NL_IDX(r.id)];
    q->order = nd->qpool_slots.made;
    q->type = type;
    q->count = count;
    NL_EV(query_pool_create);
    put_qpool(b, r);
    put_u(b, type);
    put_u(b, count);
    rb_end(b);
    return r;
}

/* ---------------------------------------------------------------------------
 * Recorders
 * -------------------------------------------------------------------------*/

/* Draws that were recorded and never submitted read nothing now. */
static void pending_clear(gpu_null_device* nd, gpu_cmd* c)
{
    const uint16_t bit = (uint16_t)(1u << c->slot);
    for (uint32_t i = 0; i < c->ntuse; i++) {
        gpu_null_tpool* const p = tpool_at(nd, c->tuse[i].pool);
        if (!p) continue;
        for (uint32_t k = 0; k < c->tuse[i].count; k++) p->pending[c->tuse[i].first + k] &= (uint16_t)~bit;
    }
    c->ntuse = 0;
}

static gpu_cmd* nl_cmd_begin(gpu_device* d, uint32_t slot)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    if (slot >= nd->nslots) return NULL;
    gpu_cmd* const c = &nd->slot[slot];
    NL_CALL(cmd_begin);
    put_u(b, slot);
    gpu_null_arg* const r = rb_end(b);
    if (c->state == GPU_NULL_REC_OPEN)
        nl_err(nd, GPU_NULL_ERR_RECORDER, r, "slot %u is still recording (no cmd_end): its commands are dropped",
               slot);
    const uint64_t seen = at_load64(&nd->seen);
    if (c->fence > seen)
        nl_err(nd, GPU_NULL_ERR_FENCE, r, "slot %u's last work ends at fence %llu, and the last fence seen to "
               "pass is %llu (gpu.h: a slot the GPU has finished with)", slot, LLU(c->fence), LLU(seen));
    if (c->state == GPU_NULL_REC_OPEN || c->state == GPU_NULL_REC_ENDED) pending_clear(nd, c);
    c->ntuse = 0;
    c->rec.n = 0;
    memset(&c->b, 0, sizeof c->b);
    c->b.depth_write = 2;
    for (uint32_t i = 0; i < GPU_NULL_MAX_QPOOLS; i++)
        if (c->q[i]) memset(c->q[i], 0, c->qcount[i]);
    c->qactive = 0;
    c->state = GPU_NULL_REC_OPEN;
    collect(nd, 0);
    return c;
}

static void nl_cmd_end(gpu_device* d, gpu_cmd* c)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    if (!c) return;
    NL_CALL(cmd_end);
    put_u(b, c->slot);
    gpu_null_arg* const r = rb_end(b);
    if (c->state != GPU_NULL_REC_OPEN) {
        nl_err(nd, GPU_NULL_ERR_RECORDER, r, "slot %u is %s: nothing to end", c->slot, rec_state_name(c->state));
        return;
    }
    if (c->qactive)
        nl_err(nd, GPU_NULL_ERR_QUERY, r, "%u queries begun and not ended in the recording (D3D12 and Vulkan end "
               "a query in the recording that began it)", c->qactive);
    c->state = GPU_NULL_REC_ENDED;
}

/* A recorded command, checked: NULL when it is dropped (the recorder is not
 * recording) or names a bad handle; then nothing else is checked. */
static gpu_null_arg* rec_open(gpu_cmd* c, nl_rb* b)
{
    gpu_null_arg* const r = rb_end(b);
    if (!r) return NULL;
    if (c->state != GPU_NULL_REC_OPEN) {
        nl_err(c->nd, GPU_NULL_ERR_RECORDER, r, "recorder %u is %s: the command is dropped", c->slot,
               rec_state_name(c->state));
        rb_drop(b);
        return NULL;
    }
    return handles_ok(c->nd, r) ? r : NULL;
}

static const gpu_null_layout* bound_layout(gpu_cmd* c)
{
    return layout_at(c->nd, c->b.layout);
}

/* The bound layout's slot, of a kind in `kinds` (a bit each); NULL counted. */
static const gpu_layout_slot* slot_of(gpu_cmd* c, const gpu_null_arg* r, uint32_t slot, uint32_t kinds)
{
    const gpu_null_layout* const l = bound_layout(c);
    if (!l) {
        nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "no layout set (set_layout) to bind into");
        return NULL;
    }
    if (slot >= l->desc.nslots) {
        nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "slot %u: layout #%u has %u", slot, l->order, l->desc.nslots);
        return NULL;
    }
    const gpu_layout_slot* const s = &l->desc.slot[slot];
    if (!(kinds & (1u << s->kind))) {
        nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "slot %u of layout #%u is a %s slot", slot, l->order,
               slot_kind_name(s->kind));
        return NULL;
    }
    return s;
}

/* A bind's address: in a buffer made for the use, with bytes from it. */
static const gpu_null_buf* addr_check(gpu_cmd* c, const gpu_null_arg* r, gpu_addr addr, uint32_t use,
                                      const char* use_name, uint64_t bytes)
{
    gpu_null_device* const nd = c->nd;
    const gpu_null_buf* const bf = buf_at(nd, addr_buf_id(addr));
    if (!bf) {
        nl_err(nd, GPU_NULL_ERR_USE, r, "address 0x%016llx is in no buffer (gpu_buffer_addr plus an offset)",
               LLU(addr));
        return NULL;
    }
    const uint64_t off = addr & NL_ADDR_MASK;
    if (!(bf->use & use)) {
        nl_err(nd, GPU_NULL_ERR_USE, r, "buffer #%u has no GPU_USE_%s", bf->order, use_name);
        return NULL;
    }
    if (off >= bf->size || bytes > bf->size - off) {
        nl_err(nd, GPU_NULL_ERR_USE, r, "%llu bytes at offset %llu: buffer #%u has %llu", LLU(bytes), LLU(off),
               bf->order, LLU(bf->size));
        return NULL;
    }
    return bf;
}

/* Vulkan ends the pass at a barrier, copy, resolve or query reset; a query
 * begun in the pass cannot outlive it. */
static int pass_breaks(gpu_cmd* c, const gpu_null_arg* r)
{
    if (!c->qactive) return 0;
    nl_err(c->nd, GPU_NULL_ERR_QUERY, r, "%u queries are active: on Vulkan this ends the pass they were begun in",
           c->qactive);
    return 1;
}

void gpu_null_cmd_pass_begin(gpu_cmd* c, const gpu_pass* pass)
{
    NL_BEGIN(pass_begin);
    const uint32_t nrt = pass ? pass->nrt : 0;
    put_u(b, nrt);
    for (uint32_t i = 0; i < 4; i++) {
        gpu_view v = {0};
        if (pass && i < nrt) v = pass->rt[i];
        put_view(b, v);
    }
    put_view(b, pass ? pass->ds : (gpu_view){0});
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    gpu_null_device* const nd = c->nd;
    if (!pass) {
        nl_err(nd, GPU_NULL_ERR_DRAW, r, "no targets (NULL)");
        return;
    }
    if (nrt > 4) {
        nl_err(nd, GPU_NULL_ERR_DRAW, r, "%u colour targets: at most 4", nrt);
        return;
    }
    for (uint32_t i = 0; i < nrt; i++) {
        const gpu_null_view* const v = view_at(nd, r[2 + i].raw);
        if (!v) {
            nl_err(nd, GPU_NULL_ERR_DRAW, r, "colour target %u is none", i);
            return;
        }
        if (v->desc.kind != GPU_VIEW_RTV) {
            nl_err(nd, GPU_NULL_ERR_HANDLE, r, "colour target %u is a depth view", i);
            return;
        }
    }
    const gpu_null_view* const ds = view_at(nd, r[6].raw);
    if (ds && ds->desc.kind != GPU_VIEW_DSV) {
        nl_err(nd, GPU_NULL_ERR_HANDLE, r, "the depth target is a colour view");
        return;
    }
    c->b.pass = 1;
}

void gpu_null_cmd_pass_end(gpu_cmd* c)
{
    NL_BEGIN(pass_end);
    rec_open(c, b);
}

static int rect_bad(const gpu_rect* rect)
{
    return rect && (rect->left < 0 || rect->top < 0 || rect->left > rect->right || rect->top > rect->bottom);
}

void gpu_null_cmd_clear_color(gpu_cmd* c, gpu_view rtv, const float rgba[4], const gpu_rect* rect)
{
    NL_BEGIN(clear_color);
    put_view(b, rtv);
    for (int k = 0; k < 4; k++) put_f(b, rgba ? rgba[k] : 0.0f);
    put_rect(b, rect);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    const gpu_null_view* const v = view_at(c->nd, r[1].raw);
    if (!v) nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "no view");
    else if (v->desc.kind != GPU_VIEW_RTV) nl_err(c->nd, GPU_NULL_ERR_HANDLE, r, "a depth view");
    else if (!rgba) nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "no colour (NULL)");
    else if (rect_bad(rect)) nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "a negative or inside-out rectangle");
}

void gpu_null_cmd_clear_depth(gpu_cmd* c, gpu_view dsv, uint32_t flags, float depth, uint8_t stencil,
                              const gpu_rect* rect)
{
    NL_BEGIN(clear_depth);
    put_view(b, dsv);
    put_x(b, flags);
    put_f(b, depth);
    put_u(b, stencil);
    put_rect(b, rect);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    gpu_null_device* const nd = c->nd;
    const gpu_null_view* const v = view_at(nd, r[1].raw);
    if (!v) {
        nl_err(nd, GPU_NULL_ERR_DRAW, r, "no view");
        return;
    }
    const gpu_null_tex* const t = tex_at(nd, v->desc.texture.id);
    if (v->desc.kind != GPU_VIEW_DSV) nl_err(nd, GPU_NULL_ERR_HANDLE, r, "a colour view");
    else if (!flags || (flags & ~3u))
        nl_err(nd, GPU_NULL_ERR_DRAW, r, "flags 0x%x: GPU_CLEAR_DEPTH and/or _STENCIL", flags);
    else if ((flags & GPU_CLEAR_STENCIL) && t && t->planes < 2)
        nl_err(nd, GPU_NULL_ERR_DRAW, r, "a stencil clear of texture #%u, whose format has no stencil", t->order);
    else if (!(depth >= 0.0f && depth <= 1.0f))
        nl_err(nd, GPU_NULL_ERR_DRAW, r, "depth %g is outside 0-1", (double)depth);
    else if (rect_bad(rect)) nl_err(nd, GPU_NULL_ERR_DRAW, r, "a negative or inside-out rectangle");
}

/* Forgets every slot's binding, as SetGraphicsRootSignature does. */
void gpu_null_cmd_set_layout(gpu_cmd* c, gpu_layout layout)
{
    NL_BEGIN(set_layout);
    put_layout(b, layout);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    c->b.layout = r[1].raw;
    c->b.bound = 0;
    memset(c->b.table, 0, sizeof c->b.table);
}

void gpu_null_cmd_set_pipeline(gpu_cmd* c, gpu_pipeline pipeline)
{
    NL_BEGIN(set_pipeline);
    put_pipe(b, pipeline);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    c->b.pipeline = r[1].raw;
}

void gpu_null_cmd_set_topology(gpu_cmd* c, uint32_t topology)
{
    NL_BEGIN(set_topology);
    put_u(b, topology);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    if (topology < GPU_TOPO_POINTLIST || topology > GPU_TOPO_TRIANGLESTRIP) {
        nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "topology %u is not a GPU_TOPO_*", topology);
        return;
    }
    c->b.topology = topology;
}

void gpu_null_cmd_set_depth_write(gpu_cmd* c, uint32_t on)
{
    NL_BEGIN(set_depth_write);
    put_u(b, on);
    if (rec_open(c, b)) c->b.depth_write = on ? 1u : 0u;
}

void gpu_null_cmd_set_viewport(gpu_cmd* c, const gpu_viewport* vp)
{
    NL_BEGIN(set_viewport);
    put_flag(b, vp != NULL);
    put_f(b, vp ? vp->x : 0.0f);
    put_f(b, vp ? vp->y : 0.0f);
    put_f(b, vp ? vp->width : 0.0f);
    put_f(b, vp ? vp->height : 0.0f);
    put_f(b, vp ? vp->min_depth : 0.0f);
    put_f(b, vp ? vp->max_depth : 0.0f);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    if (!vp) nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "no viewport (NULL)");
    else if (!(vp->width >= 0.0f && vp->height >= 0.0f && vp->min_depth >= 0.0f && vp->min_depth <= 1.0f &&
               vp->max_depth >= 0.0f && vp->max_depth <= 1.0f))
        nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "a negative size, or depths outside 0-1");
    else c->b.viewport = 1;
}

void gpu_null_cmd_set_scissor(gpu_cmd* c, const gpu_rect* rect)
{
    NL_BEGIN(set_scissor);
    put_rect(b, rect);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    if (!rect) nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "no rectangle (NULL)");
    else if (rect_bad(rect)) nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "a negative or inside-out rectangle");
    else c->b.scissor = 1;
}

/* Tables bound from a pool no longer set are unbound (their descriptors are
 * not in the heaps the GPU reads). */
void gpu_null_cmd_set_table_pools(gpu_cmd* c, gpu_table_pool tex, gpu_table_pool samplers)
{
    NL_BEGIN(set_table_pools);
    put_tpool(b, tex);
    put_tpool(b, samplers);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    gpu_null_device* const nd = c->nd;
    const gpu_null_tpool* const t = tpool_at(nd, r[1].raw);
    const gpu_null_tpool* const s = tpool_at(nd, r[2].raw);
    if (t && t->kind != GPU_TABLE_TEX) {
        nl_err(nd, GPU_NULL_ERR_HANDLE, r, "the texture pool #%u holds samplers", t->order);
        return;
    }
    if (s && s->kind != GPU_TABLE_SAMPLER) {
        nl_err(nd, GPU_NULL_ERR_HANDLE, r, "the sampler pool #%u holds textures", s->order);
        return;
    }
    const gpu_null_layout* const l = bound_layout(c);
    for (uint32_t k = 0; l && k < l->desc.nslots; k++) {
        const uint32_t kind = l->desc.slot[k].kind;
        const uint32_t was = kind == GPU_SLOT_TABLE_TEX ? c->b.tex_pool
                           : kind == GPU_SLOT_TABLE_SAMPLER ? c->b.samp_pool : 0;
        const uint32_t now = kind == GPU_SLOT_TABLE_TEX ? r[1].raw : r[2].raw;
        if (was && was != now) c->b.bound &= ~(1u << k);
    }
    c->b.tex_pool = r[1].raw;
    c->b.samp_pool = r[2].raw;
}

void gpu_null_cmd_bind_cb(gpu_cmd* c, uint32_t slot, gpu_addr addr)
{
    NL_BEGIN(bind_cb);
    put_u(b, slot);
    put_addr(b, addr);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || !slot_of(c, r, slot, 1u << GPU_SLOT_CB)) return;
    c->b.bound |= 1u << slot;
    if (!addr_check(c, r, addr, GPU_USE_CONSTANT, "CONSTANT", 1)) return;
    if ((addr & NL_ADDR_MASK) % c->nd->base.caps.cb_align)
        nl_err(c->nd, GPU_NULL_ERR_ALIGN, r, "offset %llu is not a multiple of cb_align (%u)", LLU(addr & NL_ADDR_MASK),
               c->nd->base.caps.cb_align);
}

void gpu_null_cmd_bind_raw(gpu_cmd* c, uint32_t slot, gpu_addr addr)
{
    NL_BEGIN(bind_raw);
    put_u(b, slot);
    put_addr(b, addr);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || !slot_of(c, r, slot, 1u << GPU_SLOT_RAW)) return;
    c->b.bound |= 1u << slot;
    if (!addr_check(c, r, addr, GPU_USE_RAW, "RAW", 4)) return;
    if ((addr & NL_ADDR_MASK) % 4)
        nl_err(c->nd, GPU_NULL_ERR_ALIGN, r, "a raw buffer's offset %llu is not a multiple of 4",
               LLU(addr & NL_ADDR_MASK));
}

void gpu_null_cmd_bind_table(gpu_cmd* c, uint32_t slot, gpu_table table)
{
    NL_BEGIN(bind_table);
    put_u(b, slot);
    put_table(b, table);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    const gpu_layout_slot* const s = slot_of(c, r, slot, (1u << GPU_SLOT_TABLE_TEX) | (1u << GPU_SLOT_TABLE_SAMPLER));
    if (!s) return;
    gpu_null_device* const nd = c->nd;
    const gpu_null_tpool* const p = tpool_at(nd, r[2].raw);
    const uint32_t index = (uint32_t)table.v;
    if (!p) {
        nl_err(nd, GPU_NULL_ERR_DRAW, r, "no table (gpu_table_ref)");
        return;
    }
    const int tex = s->kind == GPU_SLOT_TABLE_TEX;
    if (tex != (p->kind == GPU_TABLE_TEX)) {
        nl_err(nd, GPU_NULL_ERR_DRAW, r, "a %s table in a %s slot", tex ? "sampler" : "texture",
               slot_kind_name(s->kind));
        return;
    }
    if (r[2].raw != (tex ? c->b.tex_pool : c->b.samp_pool)) {
        nl_err(nd, GPU_NULL_ERR_DRAW, r, "pool #%u is not the one set_table_pools set", p->order);
        return;
    }
    c->b.bound |= 1u << slot;
    c->b.table[slot] = table.v;
    if ((uint64_t)index + s->count > p->count) {
        nl_err(nd, GPU_NULL_ERR_TABLE, r, "entries %u-%u: pool #%u has %u", index, index + s->count - 1, p->order,
               p->count);
        return;
    }
    const uint32_t size = table_size_at(p, index);
    if (size && size < s->count)
        nl_err(nd, GPU_NULL_ERR_TABLE, r, "the slot reads %u entries; pool #%u's table at %u holds %u", s->count,
               p->order, index, size);
}

void gpu_null_cmd_push_const(gpu_cmd* c, uint32_t slot, uint32_t count, const void* values, uint32_t first)
{
    NL_BEGIN(push_const);
    put_u(b, slot);
    put_u(b, count);
    put_u(b, first);
    for (uint32_t k = 0; values && k < count && k < 256; k++) {
        uint32_t w;
        memcpy(&w, (const char*)values + 4u * k, sizeof w);
        put_x(b, w);
    }
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    const gpu_layout_slot* const s = slot_of(c, r, slot, 1u << GPU_SLOT_CONST32);
    if (!s) return;
    c->b.bound |= 1u << slot;
    if (!values && count) nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "no values (NULL)");
    else if ((uint64_t)first + count > s->count)
        nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "values %u-%u: the slot holds %u", first, first + count - 1, s->count);
}

void gpu_null_cmd_push_const1(gpu_cmd* c, uint32_t slot, uint32_t value, uint32_t first)
{
    NL_BEGIN(push_const1);
    put_u(b, slot);
    put_x(b, value);
    put_u(b, first);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    const gpu_layout_slot* const s = slot_of(c, r, slot, 1u << GPU_SLOT_CONST32);
    if (!s) return;
    c->b.bound |= 1u << slot;
    if (first >= s->count) nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "value %u: the slot holds %u", first, s->count);
}

void gpu_null_cmd_set_vb(gpu_cmd* c, uint32_t slot, gpu_addr addr, uint32_t size, uint32_t stride)
{
    NL_BEGIN(set_vb);
    put_u(b, slot);
    put_addr(b, addr);
    put_u(b, size);
    put_u(b, stride);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    if (slot) {
        nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "slot %u: a pipeline reads one vertex buffer, slot 0", slot);
        return;
    }
    c->b.vb = 1;
    if (!addr_check(c, r, addr, GPU_USE_VERTEX, "VERTEX", size ? size : 1)) return;
    if ((addr & NL_ADDR_MASK) % 4)
        nl_err(c->nd, GPU_NULL_ERR_ALIGN, r, "offset %llu is not a multiple of 4", LLU(addr & NL_ADDR_MASK));
}

void gpu_null_cmd_set_ib(gpu_cmd* c, gpu_addr addr, uint32_t size, gpu_format format)
{
    NL_BEGIN(set_ib);
    put_addr(b, addr);
    put_u(b, size);
    put_u(b, format);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    if (format != GPU_FORMAT_R16_UINT && format != GPU_FORMAT_R32_UINT) {
        nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "format %u: R16_UINT or R32_UINT", format);
        return;
    }
    c->b.ib = 1;
    if (!addr_check(c, r, addr, GPU_USE_INDEX, "INDEX", size ? size : 1)) return;
    const uint32_t a = format == GPU_FORMAT_R16_UINT ? 2u : 4u;
    if ((addr & NL_ADDR_MASK) % a)
        nl_err(c->nd, GPU_NULL_ERR_ALIGN, r, "offset %llu is not a multiple of the index size (%u)",
               LLU(addr & NL_ADDR_MASK), a);
}

static uint32_t topology_type_of(uint32_t topology)
{
    return topology == GPU_TOPO_POINTLIST ? GPU_TOPOLOGY_TYPE_POINT
         : topology <= GPU_TOPO_LINESTRIP ? GPU_TOPOLOGY_TYPE_LINE : GPU_TOPOLOGY_TYPE_TRIANGLE;
}

/* What a draw needs bound; the first thing missing is counted. */
static const gpu_null_layout* draw_check(gpu_cmd* c, const gpu_null_arg* r, int indexed)
{
    gpu_null_device* const nd = c->nd;
    const gpu_null_bind* const bd = &c->b;
    const gpu_null_layout* const l = bound_layout(c);
    const char* why = NULL;
    uint32_t n = 0;
    if (!l) why = "no layout set (set_layout)";
    else if (!pipe_valid(nd, bd->pipeline)) why = "no pipeline set (set_pipeline)";
    else if (nd->pipe[bd->pipeline - 1].layout_hash != l->hash) why = "the pipeline was made for another layout";
    else if (!bd->topology) why = "no topology set (set_topology)";
    else if (topology_type_of(bd->topology) != nd->pipe[bd->pipeline - 1].topology_type)
        why = "the topology is not of the pipeline's kind (points, lines or triangles)";
    else if (!bd->viewport) why = "no viewport set";
    else if (!bd->scissor) why = "no scissor set";
    else if (!bd->pass) why = "no pass_begin in this recording (Vulkan draws into the targets it names)";
    if (why) {
        nl_err(nd, GPU_NULL_ERR_DRAW, r, "%s", why);
        return NULL;
    }
    const gpu_null_pipe* const p = &nd->pipe[bd->pipeline - 1];
    const uint32_t missing = ((1u << l->desc.nslots) - 1u) & ~bd->bound;
    if (missing) {
        while (!(missing & (1u << n))) n++;
        nl_err(nd, GPU_NULL_ERR_DRAW, r, "slot %u (%s) of layout #%u is not bound since set_layout", n,
               slot_kind_name(l->desc.slot[n].kind), l->order);
        return NULL;
    }
    if (p->vertex_count && !bd->vb) why = "the pipeline reads vertices and no vertex buffer is set";
    else if (bd->depth_write == 1 && !p->depth_write) why = "depth writes turned on, and the pipeline writes none";
    else if (indexed && !bd->ib) why = "no index buffer set (set_ib)";
    if (why) {
        nl_err(nd, GPU_NULL_ERR_DRAW, r, "%s", why);
        return NULL;
    }
    return l;
}

/* The tables a checked draw reads: pending for this recorder until it is
 * submitted (gpu_null_fast_table_write_tex checks it). */
static void tables_read(gpu_cmd* c, const gpu_null_layout* l)
{
    gpu_null_device* const nd = c->nd;
    const uint16_t bit = (uint16_t)(1u << c->slot);
    for (uint32_t s = 0; s < l->desc.nslots; s++) {
        if (l->desc.slot[s].kind != GPU_SLOT_TABLE_TEX && l->desc.slot[s].kind != GPU_SLOT_TABLE_SAMPLER) continue;
        const uint32_t pool = (uint32_t)(c->b.table[s] >> 32), first = (uint32_t)c->b.table[s];
        gpu_null_tpool* const p = tpool_at(nd, pool);
        if (!p || first >= p->count) continue;
        uint32_t count = l->desc.slot[s].count;
        if (count > p->count - first) count = p->count - first;
        int seen = 0;
        for (uint32_t k = c->ntuse > 2 ? c->ntuse - 2 : 0; k < c->ntuse; k++)
            if (c->tuse[k].pool == pool && c->tuse[k].first == first && c->tuse[k].count == count) seen = 1;
        if (seen) continue;
        if (c->ntuse == c->tuse_cap) {
            const uint32_t cap = c->tuse_cap ? c->tuse_cap * 2 : 256;
            gpu_null_tuse* t = (gpu_null_tuse*)realloc(c->tuse, cap * sizeof *t);
            if (!t) continue;
            c->tuse = t;
            c->tuse_cap = cap;
        }
        c->tuse[c->ntuse].pool = pool;
        c->tuse[c->ntuse].first = first;
        c->tuse[c->ntuse].count = count;
        c->ntuse++;
        for (uint32_t k = 0; k < count; k++) p->pending[first + k] |= bit;
    }
}

void gpu_null_cmd_draw(gpu_cmd* c, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                       uint32_t first_instance)
{
    NL_BEGIN(draw);
    put_u(b, vertices);
    put_u(b, instances);
    put_u(b, first_vertex);
    put_u(b, first_instance);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    const gpu_null_layout* const l = draw_check(c, r, 0);
    if (l) tables_read(c, l);
}

void gpu_null_cmd_draw_indexed(gpu_cmd* c, uint32_t indices, uint32_t instances, uint32_t first_index,
                               int32_t base_vertex, uint32_t first_instance)
{
    NL_BEGIN(draw_indexed);
    put_u(b, indices);
    put_u(b, instances);
    put_u(b, first_index);
    put_i(b, base_vertex);
    put_u(b, first_instance);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r) return;
    const gpu_null_layout* const l = draw_check(c, r, 1);
    if (l) tables_read(c, l);
}

void gpu_null_cmd_barrier(gpu_cmd* c, gpu_texture texture, gpu_state before, gpu_state after)
{
    NL_BEGIN(barrier);
    put_tex(b, texture);
    put_x(b, before);
    put_x(b, after);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || pass_breaks(c, r)) return;
    if (!r[1].raw) nl_err(c->nd, GPU_NULL_ERR_BARRIER, r, "no texture");
}

void gpu_null_cmd_barrier_n(gpu_cmd* c, uint32_t n, const gpu_barrier* list)
{
    NL_BEGIN(barrier_n);
    put_u(b, list ? n : 0);
    for (uint32_t i = 0; list && i < n; i++) {
        put_u(b, list[i].type);
        put_tex(b, list[i].texture);
        put_buf(b, list[i].buffer);
        put_x(b, list[i].sub);
        put_x(b, list[i].before);
        put_x(b, list[i].after);
    }
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || pass_breaks(c, r)) return;
    if (!list && n) {
        nl_err(c->nd, GPU_NULL_ERR_BARRIER, r, "%u barriers and no list (NULL)", n);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        const int t = GPU_VALID(list[i].texture), bu = GPU_VALID(list[i].buffer);
        if (list[i].type == GPU_BARRIER_ALIASING ? !t : (t == bu)) {
            nl_err(c->nd, GPU_NULL_ERR_BARRIER, r, "barrier %u: a transition names a texture or a buffer, an "
                   "aliasing barrier a texture", i);
            return;
        }
    }
}

/* Copies: whole subresources of the textures, the right use bits (Vulkan's
 * transfer usage), boxes and footprints inside what they name. */
static int copy_use(gpu_cmd* c, const gpu_null_arg* r, const gpu_null_tex* dst, const gpu_null_tex* src)
{
    if (dst && !(dst->use & GPU_USE_COPY_DST)) {
        nl_err(c->nd, GPU_NULL_ERR_USE, r, "texture #%u has no GPU_USE_COPY_DST", dst->order);
        return 0;
    }
    if (src && !(src->use & GPU_USE_COPY_SRC)) {
        nl_err(c->nd, GPU_NULL_ERR_USE, r, "texture #%u has no GPU_USE_COPY_SRC", src->order);
        return 0;
    }
    return 1;
}

/* Upload memory is a copy's source and readback or host memory its
 * destination by what it is; other buffers say so in their use. */
static int buf_copy_use(gpu_cmd* c, const gpu_null_arg* r, const gpu_null_buf* bf, int dst)
{
    const uint32_t implied = dst ? (bf->memory == GPU_MEM_READBACK || bf->memory == GPU_MEM_HOST)
                                 : bf->memory == GPU_MEM_UPLOAD;
    if (implied || (bf->use & (dst ? GPU_USE_COPY_DST : GPU_USE_COPY_SRC))) return 1;
    nl_err(c->nd, GPU_NULL_ERR_USE, r, "buffer #%u has no GPU_USE_COPY_%s", bf->order, dst ? "DST" : "SRC");
    return 0;
}

static int sub_check(gpu_cmd* c, const gpu_null_arg* r, const gpu_null_tex* t, uint32_t sub)
{
    if (sub < t->nsub) return 1;
    nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "subresource %u: texture #%u has %u", sub, t->order, t->nsub);
    return 0;
}

void gpu_null_cmd_copy_texture(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
                               gpu_texture src, uint32_t src_sub, const gpu_box* box)
{
    NL_BEGIN(copy_texture);
    put_tex(b, dst);
    put_u(b, dst_sub);
    put_u(b, x);
    put_u(b, y);
    put_u(b, z);
    put_tex(b, src);
    put_u(b, src_sub);
    put_box(b, box);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || pass_breaks(c, r)) return;
    const gpu_null_tex* const d = tex_at(c->nd, r[1].raw);
    const gpu_null_tex* const s = tex_at(c->nd, r[6].raw);
    if (!d || !s) {
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "no %s texture", d ? "source" : "destination");
        return;
    }
    if (!copy_use(c, r, d, s) || !sub_check(c, r, d, dst_sub) || !sub_check(c, r, s, src_sub)) return;
    uint32_t dw, dh, db, sw, sh, sb;
    sub_size(d, dst_sub, &dw, &dh, &db);
    sub_size(s, src_sub, &sw, &sh, &sb);
    const char* why = NULL;
    uint32_t w = sw, h = sh;
    if (d->samples != s->samples) why = "the sample counts differ";
    else if (db != sb) why = "the texel sizes differ";
    else if (box && (box->left > box->right || box->top > box->bottom || box->right > sw || box->bottom > sh ||
                     box->front != 0 || box->back != 1))
        why = "the box is outside the source subresource";
    if (box && !why) {
        w = box->right - box->left;
        h = box->bottom - box->top;
    }
    if (!why && (z != 0 || x > dw || y > dh || w > dw - x || h > dh - y)) why = "the region is outside the destination";
    if (why)
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "%s (destination %ux%u, source %ux%u)", why, dw, dh, sw, sh);
}

void gpu_null_cmd_copy_texture_whole(gpu_cmd* c, gpu_texture dst, gpu_texture src)
{
    NL_BEGIN(copy_texture_whole);
    put_tex(b, dst);
    put_tex(b, src);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || pass_breaks(c, r)) return;
    const gpu_null_tex* const d = tex_at(c->nd, r[1].raw);
    const gpu_null_tex* const s = tex_at(c->nd, r[2].raw);
    if (!d || !s) {
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "no %s texture", d ? "source" : "destination");
        return;
    }
    if (!copy_use(c, r, d, s)) return;
    int bd, bs;
    if (d->width != s->width || d->height != s->height || d->layers != s->layers || d->mips != s->mips ||
        d->samples != s->samples || d->planes != s->planes || fmt_bytes(d->format, &bd) != fmt_bytes(s->format, &bs))
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "textures #%u and #%u differ in size, subresources or texel size",
               d->order, s->order);
}

/* A footprint against a texture's subresource (at x, y, z; box NULL: the
 * whole of it) and a buffer. */
static void fp_check(gpu_cmd* c, const gpu_null_arg* r, const gpu_footprint* fp, const gpu_null_tex* t, uint32_t sub,
                     uint32_t x, uint32_t y, uint32_t z, const gpu_box* box, const gpu_null_buf* bf)
{
    uint32_t w, h, tb;
    sub_size(t, sub, &w, &h, &tb);
    int block;
    const uint32_t fb = fmt_bytes(fp->format, &block);
    uint64_t rb;
    uint32_t nrows;
    int bad;
    const uint64_t ext = fp_extent(fp, &rb, &nrows, &bad);
    if (fp->offset % 512 || fp->row_pitch % 256) {
        nl_err(c->nd, GPU_NULL_ERR_ALIGN, r, "footprint offset %llu (a multiple of 512) or row pitch %u (of 256)",
               LLU(fp->offset), fp->row_pitch);
        return;
    }
    const char* why = NULL;
    if (fb && tb && fb != tb) why = "the footprint's texel size is not the texture's";
    else if (bad) why = "a row is longer than the row pitch";
    else if (fp->offset > bf->size || ext > bf->size - fp->offset) why = "the footprint runs past the buffer";
    else if (box) {
        if (box->left > box->right || box->top > box->bottom || box->right > w || box->bottom > h ||
            box->front != 0 || box->back != 1)
            why = "the box is outside the subresource";
        else if (box->right - box->left > fp->width || box->bottom - box->top > fp->height)
            why = "the box is larger than the footprint";
    } else if (r->v == GPU_CMD_OP_copy_tex_to_buf) {
        if (fp->width != w || fp->height != h || fp->depth != 1)
            why = "with no box the footprint is the whole subresource";
    } else if (z != 0 || fp->depth != 1 || x > w || y > h || fp->width > w - x || fp->height > h - y) {
        why = "the footprint at x, y, z runs outside the subresource";
    }
    if (why)
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "%s (subresource %ux%u, footprint %ux%ux%u at %llu, pitch %u; buffer "
               "#%u holds %llu)", why, w, h, fp->width, fp->height, fp->depth, LLU(fp->offset), fp->row_pitch,
               bf->order, LLU(bf->size));
}

void gpu_null_cmd_copy_buf_to_tex(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
                                  gpu_buffer src, const gpu_footprint* from)
{
    NL_BEGIN(copy_buf_to_tex);
    put_tex(b, dst);
    put_u(b, dst_sub);
    put_u(b, x);
    put_u(b, y);
    put_u(b, z);
    put_buf(b, src);
    put_fp(b, from);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || pass_breaks(c, r)) return;
    const gpu_null_tex* const t = tex_at(c->nd, r[1].raw);
    const gpu_null_buf* const bf = buf_at(c->nd, r[6].raw);
    if (!t || !bf || !from) {
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "no %s", !t ? "texture" : !bf ? "buffer" : "footprint");
        return;
    }
    if (!copy_use(c, r, t, NULL) || !buf_copy_use(c, r, bf, 0) || !sub_check(c, r, t, dst_sub)) return;
    fp_check(c, r, from, t, dst_sub, x, y, z, NULL, bf);
}

void gpu_null_cmd_copy_tex_to_buf(gpu_cmd* c, gpu_buffer dst, const gpu_footprint* to, gpu_texture src,
                                  uint32_t src_sub, const gpu_box* box)
{
    NL_BEGIN(copy_tex_to_buf);
    put_buf(b, dst);
    put_fp(b, to);
    put_tex(b, src);
    put_u(b, src_sub);
    put_box(b, box);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || pass_breaks(c, r)) return;
    const gpu_null_buf* const bf = buf_at(c->nd, r[1].raw);
    const gpu_null_tex* const t = tex_at(c->nd, r[9].raw);
    if (!t || !bf || !to) {
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "no %s", !t ? "texture" : !bf ? "buffer" : "footprint");
        return;
    }
    if (!copy_use(c, r, NULL, t) || !buf_copy_use(c, r, bf, 1) || !sub_check(c, r, t, src_sub)) return;
    fp_check(c, r, to, t, src_sub, 0, 0, 0, box, bf);
}

void gpu_null_cmd_copy_buffer(gpu_cmd* c, gpu_buffer dst, uint64_t dst_offset, gpu_buffer src, uint64_t src_offset,
                              uint64_t size)
{
    NL_BEGIN(copy_buffer);
    put_buf(b, dst);
    put_u(b, dst_offset);
    put_buf(b, src);
    put_u(b, src_offset);
    put_u(b, size);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || pass_breaks(c, r)) return;
    const gpu_null_buf* const d = buf_at(c->nd, r[1].raw);
    const gpu_null_buf* const s = buf_at(c->nd, r[3].raw);
    if (!d || !s) {
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "no %s buffer", d ? "source" : "destination");
        return;
    }
    if (!buf_copy_use(c, r, d, 1) || !buf_copy_use(c, r, s, 0)) return;
    if (dst_offset > d->size || size > d->size - dst_offset || src_offset > s->size || size > s->size - src_offset)
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "%llu bytes: buffer #%u holds %llu, buffer #%u %llu", LLU(size),
               d->order, LLU(d->size), s->order, LLU(s->size));
    else if (d == s && dst_offset < src_offset + size && src_offset < dst_offset + size)
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "the ranges overlap in buffer #%u", d->order);
}

void gpu_null_cmd_resolve(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, gpu_texture src, uint32_t src_sub,
                          gpu_format format)
{
    NL_BEGIN(resolve);
    put_tex(b, dst);
    put_u(b, dst_sub);
    put_tex(b, src);
    put_u(b, src_sub);
    put_u(b, format);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || pass_breaks(c, r)) return;
    const gpu_null_tex* const d = tex_at(c->nd, r[1].raw);
    const gpu_null_tex* const s = tex_at(c->nd, r[3].raw);
    if (!d || !s) {
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "no %s texture", d ? "source" : "destination");
        return;
    }
    if (!copy_use(c, r, d, s) || !sub_check(c, r, d, dst_sub) || !sub_check(c, r, s, src_sub)) return;
    uint32_t dw, dh, sw, sh;
    sub_size(d, dst_sub, &dw, &dh, NULL);
    sub_size(s, src_sub, &sw, &sh, NULL);
    if (s->samples < 2 || d->samples != 1 || dw != sw || dh != sh || fmt_is_typeless(format))
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "a resolve is from MSAA to one sample, the same size, in a typed format "
               "(%u samples %ux%u to %u samples %ux%u)", s->samples, sw, sh, d->samples, dw, dh);
}

/* This recording's states of a pool's queries. */
static uint8_t* qstates(gpu_cmd* c, uint32_t pool)
{
    const uint32_t i = NL_IDX(pool);
    const gpu_null_qpool* const q = &c->nd->qpool[i];
    if (!c->q[i] || c->qcount[i] != q->count) {
        free(c->q[i]);
        c->q[i] = (uint8_t*)calloc(q->count, 1);
        c->qcount[i] = c->q[i] ? q->count : 0;
    }
    return c->q[i];
}

static int query_range(gpu_cmd* c, const gpu_null_arg* r, uint32_t first, uint32_t count)
{
    const gpu_null_qpool* const q = qpool_at(c->nd, r[1].raw);
    if (q && first < q->count && count <= q->count - first) return 1;
    nl_err(c->nd, GPU_NULL_ERR_QUERY, r, "queries %u+%u: pool #%u holds %u", first, count, q ? q->order : 0u,
           q ? q->count : 0u);
    return 0;
}

void gpu_null_cmd_query_reset(gpu_cmd* c, gpu_query_pool pool, uint32_t first, uint32_t count)
{
    NL_BEGIN(query_reset);
    put_qpool(b, pool);
    put_u(b, first);
    put_u(b, count);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || pass_breaks(c, r) || !query_range(c, r, first, count)) return;
    uint8_t* const q = qstates(c, pool.id);
    if (!q) return;
    for (uint32_t k = 0; k < count; k++) q[first + k] = GPU_NULL_Q_RESET;
}

void gpu_null_cmd_query_begin(gpu_cmd* c, gpu_query_pool pool, uint32_t index)
{
    NL_BEGIN(query_begin);
    put_qpool(b, pool);
    put_u(b, index);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || !query_range(c, r, index, 1)) return;
    uint8_t* const q = qstates(c, pool.id);
    if (!q) return;
    if (q[index] & GPU_NULL_Q_ACTIVE) {
        nl_err(c->nd, GPU_NULL_ERR_QUERY, r, "query %u is begun twice", index);
        return;
    }
    if (!(q[index] & GPU_NULL_Q_RESET))
        nl_err(c->nd, GPU_NULL_ERR_QUERY, r, "query %u is begun %s (Vulkan needs a query_reset before each use in "
               "a recording)", index, (q[index] & GPU_NULL_Q_ENDED) ? "again without a reset" : "with no reset");
    q[index] = GPU_NULL_Q_ACTIVE;
    c->qactive++;
}

void gpu_null_cmd_query_end(gpu_cmd* c, gpu_query_pool pool, uint32_t index)
{
    NL_BEGIN(query_end);
    put_qpool(b, pool);
    put_u(b, index);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || !query_range(c, r, index, 1)) return;
    uint8_t* const q = qstates(c, pool.id);
    if (!q) return;
    if (!(q[index] & GPU_NULL_Q_ACTIVE)) {
        nl_err(c->nd, GPU_NULL_ERR_QUERY, r, "query %u is ended and was not begun", index);
        return;
    }
    q[index] = GPU_NULL_Q_ENDED;
    c->qactive--;
}

void gpu_null_cmd_query_resolve(gpu_cmd* c, gpu_query_pool pool, uint32_t first, uint32_t count, gpu_buffer dst,
                                uint64_t offset)
{
    NL_BEGIN(query_resolve);
    put_qpool(b, pool);
    put_u(b, first);
    put_u(b, count);
    put_buf(b, dst);
    put_u(b, offset);
    gpu_null_arg* const r = rec_open(c, b);
    if (!r || pass_breaks(c, r) || !query_range(c, r, first, count)) return;
    const gpu_null_buf* const bf = buf_at(c->nd, r[4].raw);
    if (!bf) {
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "no buffer");
        return;
    }
    if (!(bf->use & GPU_USE_QUERY)) nl_err(c->nd, GPU_NULL_ERR_USE, r, "buffer #%u has no GPU_USE_QUERY", bf->order);
    else if (offset % 8) nl_err(c->nd, GPU_NULL_ERR_ALIGN, r, "offset %llu is not a multiple of 8", LLU(offset));
    else if (offset > bf->size || (uint64_t)count * 8u > bf->size - offset)
        nl_err(c->nd, GPU_NULL_ERR_BOUNDS, r, "%u results at %llu: buffer #%u holds %llu bytes", count, LLU(offset),
               bf->order, LLU(bf->size));
}

void gpu_null_cmd_set_stencil_ref(gpu_cmd* c, uint32_t ref)
{
    NL_BEGIN(set_stencil_ref);
    put_u(b, ref);
    rec_open(c, b);
}

void gpu_null_cmd_set_blend_factor(gpu_cmd* c, const float rgba[4])
{
    NL_BEGIN(set_blend_factor);
    for (int k = 0; k < 4; k++) put_f(b, rgba ? rgba[k] : 0.0f);
    gpu_null_arg* const r = rec_open(c, b);
    if (r && !rgba) nl_err(c->nd, GPU_NULL_ERR_DRAW, r, "no factor (NULL)");
}

void gpu_null_cmd_marker(gpu_cmd* c, const char* text, uint32_t len)
{
    NL_BEGIN(marker);
    put_text(b, text, len);
    rec_open(c, b);
}

/* ---------------------------------------------------------------------------
 * Running a recording at submit
 * -------------------------------------------------------------------------*/

typedef struct nl_ex {
    gpu_null_device*    nd;
    const gpu_null_arg* rec;
    uint32_t            mask;     /* kinds counted for this record: each once */
    uint64_t            fence;    /* the fence that ends this submit's work */
    uint32_t            nrt, rt[4], ds;
    uint32_t            layout;
    uint64_t            table[GPU_MAX_SLOTS];   /* pool id << 32 | first entry */
    uint32_t            swaps;    /* swap chains whose images it used, a bit each */
} nl_ex;

static void ex_err(nl_ex* ex, int kind, const char* fmt, ...) NL_PRINTF(3, 4);
static void ex_err(nl_ex* ex, int kind, const char* fmt, ...)
{
    if (ex->mask & (1u << kind)) return;
    ex->mask |= 1u << kind;
    va_list ap;
    va_start(ap, fmt);
    nl_verr(ex->nd, kind, ex->rec, fmt, ap);
    va_end(ap);
}

static void list_add(uint32_t** a, uint32_t* n, uint32_t* cap, uint32_t v)
{
    if (*n == *cap) {
        const uint32_t nc = *cap ? *cap * 2 : 256;
        uint32_t* na = (uint32_t*)realloc(*a, nc * sizeof *na);
        if (!na) return;
        *a = na;
        *cap = nc;
    }
    (*a)[(*n)++] = v;
}

static void tex_used(nl_ex* ex, gpu_null_tex* t)
{
    t->last_use = ex->fence;
    if (t->swap) ex->swaps |= 1u << (t->swap - 1);
}

/* Handles freed since the command was recorded: a recording that uses an
 * object holds it until its work is done. Else every object it names is in
 * use until this submit's fence. */
static int handles_run(nl_ex* ex, gpu_null_arg* r)
{
    gpu_null_device* const nd = ex->nd;
    for (uint32_t i = 1; i <= r->raw; i++)
        if (r[i].raw && !arg_live(nd, &r[i])) {
            ex_err(ex, GPU_NULL_ERR_FREED, "argument %u: its %s was freed after the command was recorded", i,
                   tag_name(r[i].tag));
            return 0;
        }
    for (uint32_t i = 1; i <= r->raw; i++) {
        if (!r[i].raw) continue;
        gpu_null_tex* const t = arg_tex(nd, &r[i]);
        if (t) tex_used(ex, t);
        if (r[i].tag == GPU_NULL_T_BUF || r[i].tag == GPU_NULL_T_ADDR) nd->buf[NL_IDX(r[i].raw)].last_use = ex->fence;
    }
    return 1;
}

static int read_state(uint32_t need)
{
    return need && !(need & ~(uint32_t)(GPU_STATE_COPY_SOURCE | GPU_STATE_PIXEL_SHADER_RESOURCE |
                                        GPU_STATE_NON_PIXEL_SHADER_RESOURCE));
}

/* One subresource used in a state that needs `need`. With promote, a
 * subresource in COMMON is promoted to it (D3D12's implicit promotion: a
 * copy or shader-resource state for a texture), a promoted read state takes
 * more read states, and a promoted read decays at the end of the submit. */
static void sub_need(nl_ex* ex, gpu_null_tex* t, uint32_t ti, uint32_t sub, uint32_t need, int promote,
                     const char* what)
{
    uint32_t* const s = &t->state[sub];
    const uint32_t cur = *s & ~GPU_NULL_PROMOTED;
    if ((cur & need) == need) return;
    if (promote) {
        const int read = read_state(need);
        if (cur == GPU_STATE_COMMON && !(*s & GPU_NULL_PROMOTED)) {
            *s = need | (read ? GPU_NULL_PROMOTED : 0u);
            if (read) list_add(&ex->nd->decay_tex, &ex->nd->ndecay_tex, &ex->nd->decay_tex_cap, ti);
            return;
        }
        if ((*s & GPU_NULL_PROMOTED) && read) {
            *s |= need;
            return;
        }
    }
    ex_err(ex, GPU_NULL_ERR_STATE, "%s: texture #%u subresource %u is in 0x%x; it needs 0x%x", what, t->order, sub, cur,
           need);
}

/* A texture's subresource (or all of them) used in `need`. */
static void tex_need(nl_ex* ex, uint32_t id, uint32_t sub, uint32_t need, int promote, const char* what)
{
    gpu_null_tex* const t = tex_at(ex->nd, id);
    if (!t) return;
    const uint32_t ti = NL_IDX(id);
    if (sub == GPU_SUB_ALL) {
        for (uint32_t k = 0; k < t->nsub; k++) sub_need(ex, t, ti, k, need, promote, what);
    } else if (sub < t->nsub) {
        sub_need(ex, t, ti, sub, need, promote, what);
    }
}

static uint32_t view_sub(const gpu_null_tex* t, const gpu_view_desc* v, uint32_t plane)
{
    return (uint32_t)v->mip + (uint32_t)v->layer * t->mips + plane * (uint32_t)t->mips * t->layers;
}

/* A target view's subresources: a colour view's in RENDER_TARGET; a depth
 * view's planes in DEPTH_WRITE, or DEPTH_READ where it is read-only (for a
 * clear: the planes cleared, in DEPTH_WRITE). */
static void view_need(nl_ex* ex, uint32_t view_id, int clear, uint32_t clear_flags)
{
    gpu_null_device* const nd = ex->nd;
    const gpu_null_view* const v = view_at(nd, view_id);
    if (!v) return;
    gpu_null_tex* const t = tex_at(nd, v->desc.texture.id);
    if (!t || v->desc.mip >= t->mips || v->desc.layer >= t->layers) return;
    const uint32_t ti = NL_IDX(v->desc.texture.id);
    tex_used(ex, t);
    if (v->desc.kind == GPU_VIEW_RTV) {
        sub_need(ex, t, ti, view_sub(t, &v->desc, 0), GPU_STATE_RENDER_TARGET, 0, "a colour target");
        return;
    }
    for (uint32_t p = 0; p < t->planes; p++) {
        uint32_t need;
        if (clear) {
            if (!(clear_flags & (p ? GPU_CLEAR_STENCIL : GPU_CLEAR_DEPTH))) continue;
            need = GPU_STATE_DEPTH_WRITE;
        } else {
            const uint32_t ro = p ? GPU_VIEW_READ_ONLY_STENCIL : GPU_VIEW_READ_ONLY_DEPTH;
            need = (v->desc.flags & ro) ? GPU_STATE_DEPTH_READ : GPU_STATE_DEPTH_WRITE;
        }
        sub_need(ex, t, ti, view_sub(t, &v->desc, p), need, 0, p ? "a depth target's stencil" : "a depth target");
    }
}

/* The textures a draw's tables name: in a shader-resource state for the
 * stages that read them. */
static void tables_need(nl_ex* ex)
{
    gpu_null_device* const nd = ex->nd;
    const gpu_null_layout* const l = layout_at(nd, ex->layout);
    if (!l) return;
    for (uint32_t s = 0; s < l->desc.nslots; s++) {
        const gpu_layout_slot* const sl = &l->desc.slot[s];
        if (sl->kind != GPU_SLOT_TABLE_TEX) continue;
        const gpu_null_tpool* const p = tpool_at(nd, (uint32_t)(ex->table[s] >> 32));
        const uint32_t first = (uint32_t)ex->table[s];
        if (!p || p->kind != GPU_TABLE_TEX) continue;
        const uint32_t need = sl->visibility == GPU_VIS_VERTEX || sl->visibility == GPU_VIS_GEOMETRY
                            ? GPU_STATE_NON_PIXEL_SHADER_RESOURCE : GPU_STATE_PIXEL_SHADER_RESOURCE;
        for (uint32_t k = first; k < p->count && k - first < sl->count; k++) {
            const gpu_view_desc* const v = &((const gpu_view_desc*)p->memo)[k];
            if (!v->texture.id) continue;
            gpu_null_tex* const t = tex_at(nd, v->texture.id);
            if (!t) {
                ex_err(ex, GPU_NULL_ERR_FREED, "table entry %u of pool #%u names a texture freed since it was written",
                       k, p->order);
                continue;
            }
            tex_used(ex, t);
            const uint32_t ti = NL_IDX(v->texture.id);
            const gpu_format f = v->format;
            const uint32_t plane = t->planes > 1 && (f == GPU_FORMAT_X24_TYPELESS_G8_UINT ||
                                                     f == GPU_FORMAT_X32_TYPELESS_G8X24_UINT) ? 1u : 0u;
            const uint32_t m0 = v->mip, m1 = v->mips ? (uint32_t)v->mip + v->mips : t->mips;
            uint32_t l0 = 0, l1 = 1;
            if (v->dim == GPU_DIM_CUBE) l1 = t->layers;
            else if (v->dim == GPU_DIM_2D_ARRAY) { l0 = v->layer; l1 = t->layers; }
            for (uint32_t ly = l0; ly < l1 && ly < t->layers; ly++)
                for (uint32_t m = m0; m < m1 && m < t->mips; m++)
                    sub_need(ex, t, ti, m + ly * t->mips + plane * (uint32_t)t->mips * t->layers, need, 1,
                             "a texture in a table");
        }
    }
}

/* A buffer used in a state: upload memory is GENERIC_READ for good,
 * readback and host memory COPY_DEST; GPU memory is tracked, promoted from
 * COMMON by any use, and back in COMMON after a submit that used it, as
 * D3D12's buffers decay whatever state they were moved to. */
static void buf_need(nl_ex* ex, uint32_t id, uint32_t need, int write, const char* what)
{
    gpu_null_device* const nd = ex->nd;
    gpu_null_buf* const bf = buf_at(nd, id);
    if (!bf) return;
    bf->last_use = ex->fence;
    if (write) bf->last_write = ex->fence;
    const uint32_t cur = bf->state & ~GPU_NULL_PROMOTED;
    if (bf->memory != GPU_MEM_GPU) {
        if ((cur & need) != need)
            ex_err(ex, GPU_NULL_ERR_STATE, "%s: buffer #%u is %s memory, in 0x%x for good; it needs 0x%x", what,
                   bf->order, bf->memory == GPU_MEM_UPLOAD ? "upload" : "readback", cur, need);
        return;
    }
    list_add(&nd->decay_buf, &nd->ndecay_buf, &nd->decay_buf_cap, NL_IDX(id));
    if ((cur & need) == need) return;
    if (cur == GPU_STATE_COMMON && !(bf->state & GPU_NULL_PROMOTED)) {
        bf->state = need | (read_state(need) ? GPU_NULL_PROMOTED : 0u);
        return;
    }
    if ((bf->state & GPU_NULL_PROMOTED) && read_state(need)) {
        bf->state |= need;
        return;
    }
    ex_err(ex, GPU_NULL_ERR_STATE, "%s: buffer #%u is in 0x%x; it needs 0x%x", what, bf->order, cur, need);
}

/* A transition: every subresource it names is in `before`. */
static void transition(nl_ex* ex, uint32_t id, uint32_t sub, uint32_t before, uint32_t after)
{
    gpu_null_tex* const t = tex_at(ex->nd, id);
    if (!t) return;
    if (sub != GPU_SUB_ALL && sub >= t->nsub) {
        ex_err(ex, GPU_NULL_ERR_BARRIER, "subresource %u: texture #%u has %u", sub, t->order, t->nsub);
        return;
    }
    const uint32_t lo = sub == GPU_SUB_ALL ? 0 : sub, hi = sub == GPU_SUB_ALL ? t->nsub : sub + 1;
    for (uint32_t k = lo; k < hi; k++) {
        const uint32_t cur = t->state[k] & ~GPU_NULL_PROMOTED;
        if (cur != before)
            ex_err(ex, GPU_NULL_ERR_BARRIER, "texture #%u subresource %u is in 0x%x, not 0x%x", t->order, k, cur,
                   before);
        t->state[k] = after;
    }
}

static void buf_transition(nl_ex* ex, uint32_t id, uint32_t before, uint32_t after)
{
    gpu_null_device* const nd = ex->nd;
    gpu_null_buf* const bf = buf_at(nd, id);
    if (!bf) return;
    if (bf->memory != GPU_MEM_GPU) {
        ex_err(ex, GPU_NULL_ERR_BARRIER, "buffer #%u is upload or readback memory, which keeps its state", bf->order);
        return;
    }
    const uint32_t cur = bf->state & ~GPU_NULL_PROMOTED;
    if (cur != before) ex_err(ex, GPU_NULL_ERR_BARRIER, "buffer #%u is in 0x%x, not 0x%x", bf->order, cur, before);
    bf->state = after;
    list_add(&nd->decay_buf, &nd->ndecay_buf, &nd->decay_buf_cap, NL_IDX(id));
}

/* Zeroes where a footprint lies in a buffer: what a copy from a texture,
 * which holds nothing, writes. */
static void fp_zero(gpu_null_buf* bf, const gpu_footprint* fp)
{
    uint64_t rb;
    uint32_t nrows;
    int bad;
    const uint64_t ext = fp_extent(fp, &rb, &nrows, &bad);
    if (bad || !ext || fp->offset > bf->size || ext > bf->size - fp->offset) return;
    for (uint32_t k = 0; k < nrows; k++) memset(bf->mem + fp->offset + (uint64_t)k * fp->row_pitch, 0, (size_t)rb);
}

static void run_one(nl_ex* ex, gpu_null_arg* r)
{
    gpu_null_device* const nd = ex->nd;
    ex->rec = r;
    ex->mask = 0;
    nd->counts.commands++;
    if (!handles_run(ex, r)) return;
    switch ((uint32_t)r->v) {
    case GPU_CMD_OP_pass_begin:
        ex->nrt = r[1].v < 4 ? (uint32_t)r[1].v : 4u;
        for (uint32_t i = 0; i < 4; i++) ex->rt[i] = r[2 + i].raw;
        ex->ds = r[6].raw;
        break;
    case GPU_CMD_OP_clear_color:
        view_need(ex, r[1].raw, 1, 0);
        break;
    case GPU_CMD_OP_clear_depth:
        view_need(ex, r[1].raw, 1, (uint32_t)r[2].v);
        break;
    case GPU_CMD_OP_set_layout:
        ex->layout = r[1].raw;
        memset(ex->table, 0, sizeof ex->table);
        break;
    case GPU_CMD_OP_bind_table:
        if (r[1].v < GPU_MAX_SLOTS) ex->table[r[1].v] = ((uint64_t)r[2].raw << 32) | (uint32_t)r[2].v;
        break;
    case GPU_CMD_OP_bind_cb:
        buf_need(ex, r[2].raw, GPU_STATE_VERTEX_AND_CONSTANT_BUFFER, 0, "a constant buffer");
        break;
    case GPU_CMD_OP_bind_raw: {
        const gpu_null_layout* const l = layout_at(nd, ex->layout);
        const uint32_t vis = l && r[1].v < l->desc.nslots ? l->desc.slot[r[1].v].visibility : GPU_VIS_ALL;
        const uint32_t need = vis == GPU_VIS_PIXEL ? GPU_STATE_PIXEL_SHADER_RESOURCE
                            : vis == GPU_VIS_ALL ? GPU_STATE_PIXEL_SHADER_RESOURCE | GPU_STATE_NON_PIXEL_SHADER_RESOURCE
                            : GPU_STATE_NON_PIXEL_SHADER_RESOURCE;
        buf_need(ex, r[2].raw, need, 0, "a raw buffer");
        break;
    }
    case GPU_CMD_OP_set_vb:
        buf_need(ex, r[2].raw, GPU_STATE_VERTEX_AND_CONSTANT_BUFFER, 0, "a vertex buffer");
        break;
    case GPU_CMD_OP_set_ib:
        buf_need(ex, r[1].raw, GPU_STATE_INDEX_BUFFER, 0, "an index buffer");
        break;
    case GPU_CMD_OP_draw:
    case GPU_CMD_OP_draw_indexed:
        nd->counts.draws++;
        for (uint32_t i = 0; i < ex->nrt; i++) view_need(ex, ex->rt[i], 0, 0);
        view_need(ex, ex->ds, 0, 0);
        tables_need(ex);
        break;
    case GPU_CMD_OP_barrier:
        transition(ex, r[1].raw, GPU_SUB_ALL, (uint32_t)r[2].v, (uint32_t)r[3].v);
        break;
    case GPU_CMD_OP_barrier_n:
        for (uint32_t i = 0; i < r[1].v && 2 + 6 * i + 5 <= r->raw; i++) {
            const gpu_null_arg* const a = &r[2 + 6 * i];
            if (a[0].v == GPU_BARRIER_ALIASING) continue;
            if (a[1].raw) transition(ex, a[1].raw, (uint32_t)a[3].v, (uint32_t)a[4].v, (uint32_t)a[5].v);
            else if (a[2].raw) buf_transition(ex, a[2].raw, (uint32_t)a[4].v, (uint32_t)a[5].v);
        }
        break;
    case GPU_CMD_OP_copy_texture:
        tex_need(ex, r[1].raw, (uint32_t)r[2].v, GPU_STATE_COPY_DEST, 1, "a copy's destination");
        tex_need(ex, r[6].raw, (uint32_t)r[7].v, GPU_STATE_COPY_SOURCE, 1, "a copy's source");
        break;
    case GPU_CMD_OP_copy_texture_whole:
        tex_need(ex, r[1].raw, GPU_SUB_ALL, GPU_STATE_COPY_DEST, 1, "a copy's destination");
        tex_need(ex, r[2].raw, GPU_SUB_ALL, GPU_STATE_COPY_SOURCE, 1, "a copy's source");
        break;
    case GPU_CMD_OP_copy_buf_to_tex:
        tex_need(ex, r[1].raw, (uint32_t)r[2].v, GPU_STATE_COPY_DEST, 1, "a copy's destination");
        buf_need(ex, r[6].raw, GPU_STATE_COPY_SOURCE, 0, "a copy's source");
        break;
    case GPU_CMD_OP_copy_tex_to_buf: {
        tex_need(ex, r[9].raw, (uint32_t)r[10].v, GPU_STATE_COPY_SOURCE, 1, "a copy's source");
        buf_need(ex, r[1].raw, GPU_STATE_COPY_DEST, 1, "a copy's destination");
        gpu_null_buf* const bf = buf_at(nd, r[1].raw);
        if (bf && r[2].v) {
            gpu_footprint fp;
            get_fp(&r[3], &fp);
            fp_zero(bf, &fp);
        }
        break;
    }
    case GPU_CMD_OP_copy_buffer: {
        buf_need(ex, r[1].raw, GPU_STATE_COPY_DEST, 1, "a copy's destination");
        buf_need(ex, r[3].raw, GPU_STATE_COPY_SOURCE, 0, "a copy's source");
        gpu_null_buf* const d = buf_at(nd, r[1].raw);
        const gpu_null_buf* const s = buf_at(nd, r[3].raw);
        const uint64_t doff = r[2].v, soff = r[4].v, n = r[5].v;
        if (d && s && doff <= d->size && n <= d->size - doff && soff <= s->size && n <= s->size - soff)
            memmove(d->mem + doff, s->mem + soff, (size_t)n);
        break;
    }
    case GPU_CMD_OP_resolve:
        tex_need(ex, r[1].raw, (uint32_t)r[2].v, GPU_STATE_RESOLVE_DEST, 0, "a resolve's destination");
        tex_need(ex, r[3].raw, (uint32_t)r[4].v, GPU_STATE_RESOLVE_SOURCE, 0, "a resolve's source");
        break;
    case GPU_CMD_OP_query_resolve: {
        buf_need(ex, r[4].raw, GPU_STATE_COPY_DEST, 1, "the results' buffer");
        gpu_null_buf* const bf = buf_at(nd, r[4].raw);
        const uint64_t off = r[5].v, n = r[3].v * 8u;
        if (bf && off <= bf->size && n <= bf->size - off) memset(bf->mem + off, 0, (size_t)n);   /* nothing was drawn */
        break;
    }
    default:
        break;
    }
}

/* What a submit leaves: promoted reads and GPU-memory buffers decay to
 * COMMON. */
static void decay(gpu_null_device* nd)
{
    for (uint32_t i = 0; i < nd->ndecay_tex; i++) {
        const uint32_t ti = nd->decay_tex[i];
        if (ti >= nd->tex_slots.used || !nd->tex_slots.live[ti]) continue;
        gpu_null_tex* const t = &nd->tex[ti];
        for (uint32_t k = 0; k < t->nsub; k++)
            if (t->state[k] & GPU_NULL_PROMOTED) t->state[k] = GPU_STATE_COMMON;
    }
    nd->ndecay_tex = 0;
    for (uint32_t i = 0; i < nd->ndecay_buf; i++) {
        const uint32_t bi = nd->decay_buf[i];
        if (bi < nd->buf_slots.used && nd->buf_slots.live[bi] && nd->buf[bi].memory == GPU_MEM_GPU)
            nd->buf[bi].state = GPU_STATE_COMMON;
    }
    nd->ndecay_buf = 0;
}

/* A small record outside any vector (the submit's own and its "cmd" ones). */
static void small_rec(gpu_null_arg* a, uint32_t op, uint32_t n)
{
    memset(a, 0, (n + 1) * sizeof *a);
    a[0].v = op;
    a[0].raw = n;
}

static void run_cmd(gpu_null_device* nd, gpu_cmd* c, uint64_t fence, uint32_t* swaps)
{
    gpu_null_arg head[2];
    small_rec(head, GPU_NULL_OP_cmd, 1);
    head[1].tag = GPU_NULL_T_U;
    head[1].v = c->slot;
    emit(nd, head, 0);
    nl_ex ex;
    memset(&ex, 0, sizeof ex);
    ex.nd = nd;
    ex.fence = fence;
    for (uint32_t i = 0; i < c->rec.n; ) {
        gpu_null_arg* const r = &c->rec.a[i];
        run_one(&ex, r);
        emit(nd, r, 1);
        i += 1 + r->raw;
    }
    *swaps |= ex.swaps;
    const uint16_t bit = (uint16_t)(1u << c->slot);
    for (uint32_t i = 0; i < c->ntuse; i++) {
        gpu_null_tpool* const p = tpool_at(nd, c->tuse[i].pool);
        if (!p) continue;
        for (uint32_t k = 0; k < c->tuse[i].count; k++) {
            const uint32_t e = c->tuse[i].first + k;
            p->pending[e] &= (uint16_t)~bit;
            if (p->last_fence[e] < fence) p->last_fence[e] = fence;
        }
    }
    c->ntuse = 0;
}

static gpu_fence do_signal(gpu_null_device* nd)
{
    const uint64_t v = nd->fence + 1;
    at_store64(&nd->fence, v);
    NL_EV(signal);
    put_u(b, v);
    rb_end(b);
    collect(nd, 0);
    return v;
}

/* Vulkan's swap chain rules, which D3D12 does not need: the first work on an
 * acquired image waits for it (GPU_SUBMIT_ACQUIRE_WAIT), and the present
 * waits for the last (GPU_SUBMIT_PRESENT_SIGNAL). */
static void swap_submit(gpu_null_device* nd, const gpu_null_arg* sr, uint32_t swaps, uint32_t flags, gpu_swapchain sc)
{
    const uint32_t both = GPU_SUBMIT_ACQUIRE_WAIT | GPU_SUBMIT_PRESENT_SIGNAL;
    gpu_null_swap* const w = (flags & both) ? swap_at(nd, sc.id) : NULL;
    if ((flags & both) && !w)
        nl_err(nd, GPU_NULL_ERR_SWAP, sr, "GPU_SUBMIT_ACQUIRE_WAIT or _PRESENT_SIGNAL with no live swap chain");
    if (w && (flags & GPU_SUBMIT_ACQUIRE_WAIT)) {
        if (!w->acquired) nl_err(nd, GPU_NULL_ERR_SWAP, sr, "waits for swap chain #%u's image, and none is acquired",
                                 w->order);
        else w->waited = 1;
    }
    for (uint32_t s = 0; s < GPU_NULL_MAX_SWAPS; s++) {
        if (!(swaps & (1u << s))) continue;
        gpu_null_swap* const x = &nd->swap[s];
        if (x->acquired && !x->waited)
            nl_err(nd, GPU_NULL_ERR_SWAP, sr, "the first work on swap chain #%u's acquired image does not wait for "
                   "it (GPU_SUBMIT_ACQUIRE_WAIT)", x->order);
        x->used = 1;
        x->signalled = 0;
    }
    if (w && (flags & GPU_SUBMIT_PRESENT_SIGNAL)) w->signalled = 1;
}

static gpu_fence nl_submit(gpu_device* d, gpu_cmd* const* cmds, uint32_t n, const gpu_submit_desc* how)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    const uint32_t flags = how ? how->flags : 0;
    gpu_swapchain sc = {0};
    if (how) sc = how->swapchain;
    flush_events(nd);
    gpu_null_arg sr[4];
    small_rec(sr, GPU_NULL_OP_submit, 3);
    sr[1].tag = GPU_NULL_T_U;
    sr[1].v = n;
    sr[2].tag = GPU_NULL_T_X;
    sr[2].v = flags;
    sr[3].tag = GPU_NULL_T_SWAP;
    sr[3].raw = sc.id;
    sr[3].v = swap_at(nd, sc.id) ? swap_at(nd, sc.id)->order : 0u;
    emit(nd, sr, 0);
    const uint64_t fence = nd->fence + 1;   /* the next signal ends this work */
    uint32_t swaps = 0;
    for (uint32_t i = 0; i < n; i++) {
        gpu_cmd* const c = cmds ? cmds[i] : NULL;
        if (!c || c->head.ops != &gpu_null_cmd_ops || c->nd != nd) {
            nl_err(nd, GPU_NULL_ERR_RECORDER, sr, "recorder %u is not one of this device's", i);
            continue;
        }
        if (c->state != GPU_NULL_REC_ENDED) {
            nl_err(nd, GPU_NULL_ERR_RECORDER, sr, "recorder %u (slot %u) is %s: only an ended recording is submitted, "
                   "once", i, c->slot, rec_state_name(c->state));
            continue;
        }
        run_cmd(nd, c, fence, &swaps);
        c->state = GPU_NULL_REC_SUBMITTED;
        c->fence = fence;
    }
    decay(nd);
    swap_submit(nd, sr, swaps, flags, sc);
    nd->counts.submits++;
    return (flags & GPU_SUBMIT_SIGNAL) ? do_signal(nd) : 0;
}

/* ---------------------------------------------------------------------------
 * The timeline: instant, so waits never block
 * -------------------------------------------------------------------------*/

static gpu_fence nl_signal(gpu_device* d)
{
    return do_signal(GPU_NULL_DEV(d));
}

/* Any thread. A lost device reads UINT64_MAX, as D3D12's fences do. */
static uint64_t nl_fence_completed(gpu_device* d)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    const uint64_t f = at_load32(&nd->lost) ? UINT64_MAX : at_load64(&nd->fence);
    at_max64(&nd->seen, f);
    return f;
}

/* A fence the timeline has not reached is never signalled by anything
 * already submitted: on a real GPU the wait would only time out. */
static int nl_fence_wait(gpu_device* d, gpu_fence f, uint32_t timeout_ms)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    (void)timeout_ms;
    if (at_load32(&nd->lost)) {
        at_max64(&nd->seen, UINT64_MAX);
        return 0;
    }
    const uint64_t now = at_load64(&nd->fence);
    if (f > now) {
        gpu_null_arg a[3];
        small_rec(a, GPU_NULL_OP_fence_wait, 2);
        a[1].tag = GPU_NULL_T_U;
        a[1].v = f;
        a[2].tag = GPU_NULL_T_U;
        a[2].v = timeout_ms;
        nl_err(nd, GPU_NULL_ERR_FENCE, a, "fence %llu is past the last one signalled (%llu): it cannot pass",
               LLU(f), LLU(now));
        return 1;
    }
    at_max64(&nd->seen, f);
    return 0;
}

/* As the D3D12 backend's: a signal, then everything is seen done. */
static void nl_wait_idle(gpu_device* d)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    const gpu_fence f = do_signal(nd);
    at_max64(&nd->seen, f);
}

static uint32_t nl_lost(gpu_device* d)
{
    return at_load32(&GPU_NULL_DEV(d)->lost);
}

/* ---------------------------------------------------------------------------
 * Swap chains: images with no window or display
 * -------------------------------------------------------------------------*/

static int images_make(gpu_null_device* nd, gpu_null_swap* s, uint32_t slot)
{
    gpu_texture_desc td;
    memset(&td, 0, sizeof td);
    td.width = s->width;
    td.height = s->height;
    td.layers = 1;
    td.mips = 1;
    td.format = s->format;
    td.samples = 1;
    td.use = GPU_USE_RT | GPU_USE_SAMPLED | GPU_USE_COPY_SRC | GPU_USE_COPY_DST;
    td.state = GPU_STATE_PRESENT;
    for (uint32_t i = 0; i < s->buffers; i++) {
        s->image[i] = tex_make(nd, &td, slot + 1, i);
        if (!GPU_VALID(s->image[i])) return 0;
    }
    return 1;
}

static void images_drop(gpu_null_device* nd, gpu_null_swap* s)
{
    for (uint32_t i = 0; i < GPU_NULL_MAX_IMAGES; i++)
        if (slot_valid(&nd->tex_slots, s->image[i].id)) tex_free_now(nd, s->image[i].id);
    memset(s->image, 0, sizeof s->image);
}

static gpu_swapchain nl_swap_create(gpu_device* d, const gpu_swapchain_desc* desc)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_swapchain r = {0};
    if (!desc->width || !desc->height || !desc->buffers || desc->buffers > GPU_NULL_MAX_IMAGES) return r;
    r.id = obj_alloc(&nd->swap_slots, NULL, 0);
    if (!r.id) return r;
    gpu_null_swap* const s = &nd->swap[NL_IDX(r.id)];
    memset(s, 0, sizeof *s);
    s->order = nd->swap_slots.made;
    s->width = desc->width;
    s->height = desc->height;
    s->buffers = desc->buffers;
    s->format = desc->format;
    s->flags = desc->flags;
    NL_EV(swap_create);
    put_swap(b, r);
    put_u(b, desc->width);
    put_u(b, desc->height);
    put_u(b, desc->format);
    put_u(b, desc->buffers);
    put_u(b, desc->colour_space);
    put_x(b, desc->flags);
    rb_end(b);
    if (!images_make(nd, s, NL_IDX(r.id))) {
        images_drop(nd, s);
        slot_free(&nd->swap_slots, r.id);
        r.id = 0;
    }
    return r;
}

static void nl_swap_destroy(gpu_device* d, gpu_swapchain sc)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_null_swap* const s = swap_at(nd, sc.id);
    if (!s) return;
    NL_EV(swap_destroy);
    put_swap(b, sc);
    rb_end(b);
    images_drop(nd, s);
    memset(s, 0, sizeof *s);
    slot_free(&nd->swap_slots, sc.id);
}

/* New images (new handles; the old ones are stale), the first acquired next.
 * gpu.h: the GPU idle and the images unused. */
static int nl_swap_resize(gpu_device* d, gpu_swapchain sc, uint32_t width, uint32_t height, gpu_format format)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    NL_EV(swap_resize);
    put_swap(b, sc);
    put_u(b, width);
    put_u(b, height);
    put_u(b, format);
    gpu_null_arg* const r = rb_end(b);
    gpu_null_swap* const s = swap_at(nd, sc.id);
    if (!s) {
        if (r) {
            handles_req(nd, r, NL_ARG1);
            rb_drop(b);
        }
        return -1;
    }
    const uint64_t seen = at_load64(&nd->seen);
    for (uint32_t i = 0; i < s->buffers; i++) {
        const gpu_null_tex* const t = tex_at(nd, s->image[i].id);
        if (t && t->last_use > seen) {
            nl_err(nd, GPU_NULL_ERR_SWAP, r, "image %u is used by work ending at fence %llu, and the last fence seen "
                   "to pass is %llu (gpu.h: the GPU idle)", i, LLU(t->last_use), LLU(seen));
            break;
        }
    }
    images_drop(nd, s);
    if (width) s->width = width;
    if (height) s->height = height;
    if (format) s->format = format;
    s->next = s->acquired = s->waited = s->used = s->signalled = 0;
    return images_make(nd, s, NL_IDX(sc.id)) ? 0 : -1;
}

/* The image to draw next: the same one until it is presented. */
static uint32_t nl_swap_acquire(gpu_device* d, gpu_swapchain sc, gpu_texture* image)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    gpu_null_swap* const s = swap_at(nd, sc.id);
    NL_EV(swap_acquire);
    put_swap(b, sc);
    put_u(b, s ? s->next : 0u);
    gpu_null_arg* const r = rb_end(b);
    if (!s) {
        if (r) {
            handles_req(nd, r, NL_ARG1);
            rb_drop(b);
        }
        if (image) image->id = 0;
        return 0;
    }
    if (!s->acquired) {
        s->acquired = 1;
        s->waited = s->used = s->signalled = 0;
    }
    if (image) *image = s->image[s->next];
    return s->next;
}

/* The images, as made: a query, not in the stream. Views of every image may be
 * made up front; drawing into one still needs its acquire. */
static uint32_t nl_swap_image_count(gpu_device* d, gpu_swapchain sc)
{
    const gpu_null_swap* const s = swap_at(GPU_NULL_DEV(d), sc.id);
    return s ? s->buffers : 0u;
}

static gpu_texture nl_swap_image(gpu_device* d, gpu_swapchain sc, uint32_t index)
{
    const gpu_null_swap* const s = swap_at(GPU_NULL_DEV(d), sc.id);
    gpu_texture r = {0};
    if (s && index < s->buffers) r = s->image[index];
    return r;
}

static int nl_swap_present(gpu_device* d, gpu_swapchain sc, uint32_t interval, uint32_t flags)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    NL_EV(swap_present);
    put_swap(b, sc);
    put_u(b, interval);
    put_x(b, flags);
    gpu_null_arg* const r = rb_end(b);
    gpu_null_swap* const s = swap_at(nd, sc.id);
    if (!s) {
        if (r) {
            handles_req(nd, r, NL_ARG1);
            rb_drop(b);
        }
        return -1;
    }
    if (!s->acquired) {
        nl_err(nd, GPU_NULL_ERR_SWAP, r, "no image is acquired (swap_acquire before drawing into it)");
    } else {
        const gpu_null_tex* const t = tex_at(nd, s->image[s->next].id);
        if (s->used && !s->signalled)
            nl_err(nd, GPU_NULL_ERR_SWAP, r, "the work on the image was submitted with no GPU_SUBMIT_PRESENT_SIGNAL "
                   "after it");
        for (uint32_t k = 0; t && k < t->nsub; k++)
            if (t->state[k] & ~GPU_NULL_PROMOTED) {
                nl_err(nd, GPU_NULL_ERR_STATE, r, "image %u is in 0x%x; a present needs PRESENT (0)", s->next,
                       t->state[k]);
                break;
            }
    }
    if ((flags & GPU_PRESENT_TEARING) && (interval || !(s->flags & GPU_SWAP_TEARING)))
        nl_err(nd, GPU_NULL_ERR_SWAP, r, "tearing needs interval 0 and a swap chain made with GPU_SWAP_TEARING");
    s->next = (s->next + 1) % s->buffers;
    s->acquired = s->waited = s->used = s->signalled = 0;
    s->presents++;
    nd->counts.presents++;
    return 0;
}

/* No display: no statistics. */
static int nl_swap_stats(gpu_device* d, gpu_swapchain sc, gpu_present_stats* out)
{
    (void)d; (void)sc; (void)out;
    return 1;
}

static int nl_swap_hdr(gpu_device* d, gpu_swapchain sc, uint32_t colour_space, const gpu_hdr_metadata* meta)
{
    (void)meta;
    if (!swap_at(GPU_NULL_DEV(d), sc.id)) return 0;
    if (colour_space == GPU_COLOR_SRGB) return 1;
    return (colour_space == GPU_COLOR_SCRGB || colour_space == GPU_COLOR_HDR10) && (d->caps.bits & GPU_CAP_HDR_SCRGB);
}

/* ---------------------------------------------------------------------------
 * The rest of the table
 * -------------------------------------------------------------------------*/

static void nl_adapter(gpu_device* d, gpu_adapter_info* out)
{
    (void)d;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "null (records and checks, draws nothing)");
    snprintf(out->driver, sizeof out->driver, "gpu_null");
}

/* What the D3D12 backend reports for the renderer's formats on a desktop
 * GPU, with the masked features (cap_mask) left out. */
static uint32_t nl_format_support(gpu_device* d, gpu_format f, uint32_t samples)
{
    int block;
    if (!fmt_bytes(f, &block)) return 0;
    const uint32_t caps = d->caps.bits;
    uint32_t r;
    if (fmt_is_depth(f)) r = (f == GPU_FORMAT_D24_UNORM_S8_UINT && !(caps & GPU_CAP_D24S8)) ? 0u : GPU_FMT_DS;
    else if (f == GPU_FORMAT_R24_UNORM_X8_TYPELESS || f == GPU_FORMAT_X24_TYPELESS_G8_UINT ||
             f == GPU_FORMAT_R32_FLOAT_X8X24_TYPELESS || f == GPU_FORMAT_X32_TYPELESS_G8X24_UINT) r = GPU_FMT_SAMPLED;
    else if (fmt_is_typeless(f)) r = 0;
    else if (block) r = (caps & GPU_CAP_BC) ? GPU_FMT_SAMPLED | GPU_FMT_FILTER : 0u;
    else if (fmt_is_int(f)) r = GPU_FMT_SAMPLED | GPU_FMT_RT;
    else r = GPU_FMT_SAMPLED | GPU_FMT_FILTER | GPU_FMT_RT | GPU_FMT_BLEND;
    if (f == GPU_FORMAT_R32G32B32A32_FLOAT) {
        if (!(caps & GPU_CAP_RGBA32F_BLEND)) r &= ~(uint32_t)GPU_FMT_BLEND;
        if (!(caps & GPU_CAP_RGBA32F_FILTER)) r &= ~(uint32_t)GPU_FMT_FILTER;
    }
    if (f == GPU_FORMAT_R8G8B8A8_UNORM || f == GPU_FORMAT_B8G8R8A8_UNORM || f == GPU_FORMAT_R10G10B10A2_UNORM ||
        f == GPU_FORMAT_R16G16B16A16_FLOAT)
        r |= GPU_FMT_DISPLAY;
    if ((r & (GPU_FMT_RT | GPU_FMT_DS)) && (samples <= 1 || samples == 2 || samples == 4 || samples == 8))
        r |= GPU_FMT_MSAA;
    return r;
}

static void nl_fault_report(gpu_device* d, const char* where)
{
    const uint32_t lost = at_load32(&GPU_NULL_DEV(d)->lost);
    if (lost) fprintf(stderr, "[GPU null] device lost at %s: reason 0x%08x (a test hook)\n", where ? where : "?", lost);
}

/* There are no API objects. */
static void* nl_native(gpu_device* d, uint32_t kind, uint64_t handle)
{
    (void)d; (void)kind; (void)handle;
    return NULL;
}

static void nl_destroy(gpu_device* d)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    if (!nd) return;
    flush_events(nd);
    if (nd->dump) {
        fprintf((FILE*)nd->dump, "hash 0x%016llx\n", LLU(nd->hash));
        fclose((FILE*)nd->dump);
    }
    uint32_t total = 0;
    for (int k = 0; k < GPU_NULL_ERR_COUNT; k++) total += nd->err[k];
    if (total) {
        fprintf(stderr, "[GPU null] %u checks failed:", total);
        for (int k = 0; k < GPU_NULL_ERR_COUNT; k++)
            if (nd->err[k]) fprintf(stderr, " %s %u", k_err_name[k], nd->err[k]);
        fprintf(stderr, "\n");
    }
    collect(nd, 1);
    for (uint32_t i = 0; i < nd->tex_slots.used; i++) free(nd->tex ? nd->tex[i].state : NULL);
    for (uint32_t i = 0; i < nd->buf_slots.used; i++)
        if (nd->buf_slots.live[i] && nd->buf[i].memory != GPU_MEM_HOST) free(nd->buf[i].mem);
    for (uint32_t i = 0; i < GPU_NULL_MAX_TPOOLS; i++) {
        free(nd->tpool[i].memo);
        free(nd->tpool[i].last_fence);
        free(nd->tpool[i].pending);
    }
    for (uint32_t i = 0; i < GPU_NULL_MAX_CMD_SLOTS; i++) {
        gpu_cmd* const c = &nd->slot[i];
        free(c->rec.a);
        free(c->tuse);
        for (uint32_t q = 0; q < GPU_NULL_MAX_QPOOLS; q++) free(c->q[q]);
    }
    slots_fini(&nd->tex_slots);
    slots_fini(&nd->buf_slots);
    slots_fini(&nd->view_slots);
    slots_fini(&nd->samp_slots);
    slots_fini(&nd->shader_slots);
    slots_fini(&nd->layout_slots);
    slots_fini(&nd->qpool_slots);
    slots_fini(&nd->heap_slots);
    slots_fini(&nd->tpool_slots);
    slots_fini(&nd->swap_slots);
    free(nd->tex);
    free(nd->buf);
    free(nd->view);
    free(nd->samp);
    free(nd->samp_hash);
    free(nd->shader);
    free(nd->layout);
    free(nd->pipe);
    free(nd->pipe_hash);
    free(nd->heap);
    free(nd->defer);
    free(nd->decay_tex);
    free(nd->decay_buf);
    free(nd->ev.a);
    free(nd->scratch.a);
    free(nd);
}

#define NL_VT_INIT(name, form, ret, params, args) nl_##name,
static const gpu_vt k_null_vt = { GPU_VT_FUNCS(NL_VT_INIT) };
#undef NL_VT_INIT

const gpu_cmd_ops gpu_null_cmd_ops = GPU_CMD_OPS_TABLE;

gpu_device* gpu_null_create(const gpu_device_desc* desc)
{
    gpu_device_desc none;
    if (!desc) {
        memset(&none, 0, sizeof none);
        desc = &none;
    }
    gpu_null_device* const nd = (gpu_null_device*)calloc(1, sizeof *nd);
    if (!nd) return NULL;
    nd->base.vt = &k_null_vt;
    nd->hash = GPU_NULL_FNV_BASIS;
    if (!tables_init(nd)) {
        nl_destroy(&nd->base);
        return NULL;
    }
    caps_set(nd, desc);
    nd->nslots = desc->cmd_slots ? desc->cmd_slots : 2u;
    if (nd->nslots > GPU_NULL_MAX_CMD_SLOTS) nd->nslots = GPU_NULL_MAX_CMD_SLOTS;
    for (uint32_t i = 0; i < GPU_NULL_MAX_CMD_SLOTS; i++) {
        gpu_cmd* const c = &nd->slot[i];
        c->head.ops = &gpu_null_cmd_ops;
        c->head.dev = &nd->base;
        c->nd = nd;
        c->slot = i;
        c->b.depth_write = 2;
    }
    const char* dump = getenv("GPU_NULL_DUMP");
    if (dump && dump[0]) gpu_null_dump(&nd->base, dump);
    return &nd->base;
}

/* ---------------------------------------------------------------------------
 * gpu_null.h
 * -------------------------------------------------------------------------*/

uint64_t gpu_null_stream_hash(gpu_device* d)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    flush_events(nd);
    return nd->hash;
}

int gpu_null_dump(gpu_device* d, const char* path)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    flush_events(nd);   /* what came before is not in the new file */
    if (nd->dump) fclose((FILE*)nd->dump);
    nd->dump = NULL;
    if (!path) return 1;
    nd->dump = fopen(path, "wb");   /* the same bytes on every system */
    if (!nd->dump) fprintf(stderr, "[GPU null] cannot write the stream to %s\n", path);
    return nd->dump != NULL;
}

uint32_t gpu_null_errors(gpu_device* d, int kind)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    if (kind >= 0 && kind < GPU_NULL_ERR_COUNT) return at_load32(&nd->err[kind]);
    uint32_t n = 0;
    if (kind == GPU_NULL_ERR_ALL)
        for (int k = 0; k < GPU_NULL_ERR_COUNT; k++) n += at_load32(&nd->err[k]);
    return n;
}

void gpu_null_errors_reset(gpu_device* d)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    for (int k = 0; k < GPU_NULL_ERR_COUNT; k++) {
        at_store32(&nd->err[k], 0u);
        at_store32(&nd->shown[k], 0u);
    }
}

const char* gpu_null_error_name(int kind)
{
    return kind >= 0 && kind < GPU_NULL_ERR_COUNT ? k_err_name[kind] : "all";
}

void gpu_null_counts_get(gpu_device* d, gpu_null_counts* out)
{
    gpu_null_device* const nd = GPU_NULL_DEV(d);
    *out = nd->counts;
    nl_lock(nd);
    out->pipelines = nd->npipe;
    out->stored_only = nd->stored_only;
    nl_unlock(nd);
}

void gpu_null_test_lose(gpu_device* d, uint32_t reason)
{
    at_store32(&GPU_NULL_DEV(d)->lost, reason ? reason : 1u);
}

#endif /* PS3RECOMP_RSX_HAL */
