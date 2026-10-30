/* --- Guest blend state -> packed PSO key ----------------------------------
 * RSX blend factors/equation carry GL enums; each SFACTOR/DFACTOR/EQUATION
 * word packs (alpha<<16)|colour. The key packs D3D12 enums so the PSO cache
 * distinguishes real blend modes: bit0 enable, [5:1] srcC, [10:6] dstC,
 * [15:11] srcA, [20:16] dstA, [23:21] opC, [26:24] opA. DeferredShading's
 * light accumulation is additive (ONE,ONE) -- the old hardcoded straight-
 * alpha PSO turned each light pass into a screen-blanking flash. */
static u8 gl_blend_factor_d3d(u32 f, int is_alpha)
{
    switch (f & 0xFFFF) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return is_alpha ? D3D12_BLEND_SRC_ALPHA     : D3D12_BLEND_SRC_COLOR;
    case 0x0301: return is_alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return is_alpha ? D3D12_BLEND_DEST_ALPHA     : D3D12_BLEND_DEST_COLOR;
    case 0x0307: return is_alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_COLOR   */
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_ALPHA ~ */
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return D3D12_BLEND_ONE;
    }
}
static u8 gl_blend_op_d3d(u32 e)
{
    switch (e & 0xFFFF) {
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    default:     return D3D12_BLEND_OP_ADD;   /* 0x8006 FUNC_ADD / unset */
    }
}
/* Pack the guest's face-culling state for the PSO key.
 * bit0 = cull enabled, bit1 = cull FRONT (else BACK), bit2 = front face is CCW.
 * RSX/GL enums: CULL_FACE FRONT=0x0404 BACK=0x0405 FRONT_AND_BACK=0x0408;
 * FRONT_FACE CW=0x0900 CCW=0x0901. rsx_commands seeds the reset state, BACK
 * and CCW. */
static u32 rsx_cull_key(const rsx_state* st)
{
    if (!st || !st->cull_face_enable) return 0;
    u32 k = 1u;
    if (st->cull_face == 0x0404u || st->cull_face == 0x0408u) k |= 2u;  /* FRONT */
    if (st->front_face == 0x0901u) k |= 4u;                             /* CCW */
    return k;
}

/* Pack the guest's depth state for the PSO key: bit0 test enabled, bit1
 * writes on, bits 2-4 the compare function (GL enum 0x0200 NEVER .. 0x0207
 * ALWAYS, in the same order as D3D12_COMPARISON_FUNC minus one). */
static u32 rsx_depth_key(const rsx_state* st)
{
    if (!st || !st->depth_test_enable) return 0;
    return 1u | (st->depth_mask ? 2u : 0u) | ((st->depth_func & 7u) << 2);
}

static u32 rsx_blend_key(const rsx_state* st, int enable)
{
    { static int _bd = -1; PS3_KNOB(_bd, getenv("BLENDDBG") ? 1 : 0);
      if (_bd) { static u32 seen[32]; static int ns=0;
        u32 sk = (enable?0x80000000u:0u) | (st ? (st->blend_sfactor & 0xFFFFu) : 0u);
        int f=0; for (int k2=0;k2<ns;k2++) if (seen[k2]==sk) f=1;
        if (!f && ns<32) { seen[ns++]=sk;
            fprintf(stderr, "[BLEND] enable=%d sfactor=0x%X dfactor=0x%X%c", enable,
                    st?st->blend_sfactor:0, st?st->blend_dfactor:0, 10); } } }
    if (!enable) return 0;
    /* Factors never programmed: keep the legacy straight-alpha behaviour
     * (dbgfont-style text enables blending without setting factors). */
    if (!st || (st->blend_sfactor == 0 && st->blend_dfactor == 0))
        return 1u
             | ((u32)D3D12_BLEND_SRC_ALPHA     << 1)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 6)
             | ((u32)D3D12_BLEND_ONE           << 11)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 16)
             | ((u32)D3D12_BLEND_OP_ADD << 21) | ((u32)D3D12_BLEND_OP_ADD << 24);
    return 1u
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor,       0) << 1)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor,       0) << 6)
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor >> 16, 1) << 11)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor >> 16, 1) << 16)
         | ((u32)gl_blend_op_d3d(st->blend_equation)       << 21)
         | ((u32)gl_blend_op_d3d(st->blend_equation >> 16) << 24);
}

/* Bound colour target count for a draw record: 1 (target A) + the contiguous
 * prefix of MRT B/C/D offsets. */
/* RSX depth texture formats (base, LN/UN stripped): DEPTH24_D8, its float
 * variant, DEPTH16, DEPTH16_FLOAT. They sample a depth surface. */
static int rsx_fmt_is_depth(u32 basef) { return basef >= 0x90 && basef <= 0x93; }

/* A unit samples with a depth comparison (a shadow map) when it holds a depth
 * format and its TEXTURE_ADDRESS zfunc [31:28] is a real test -- NEVER (0)
 * and ALWAYS (7) give constant results, so RSX reads the depth instead. */
static int dr_unit_shadow(const D3D12DrawRecord* dr, int u)
{
    if (!dr || !dr->tex[u].set || !rsx_fmt_is_depth(dr->tex[u].fmt & 0x9F)) return 0;
    const u32 zf = (dr->tex[u].addr >> 28) & 0xF;
    return zf > 0 && zf < 7;
}

static u32 dr_shadow_mask(const D3D12DrawRecord* dr)
{
    u32 m = 0;
    for (int u = 0; u < RSX_FP_UNITS; u++) if (dr_unit_shadow(dr, u)) m |= 1u << u;
    return m;
}

/* Colour targets bound for a draw: 0 for a depth-only pass (colour target
 * NONE, as Demon's Souls's shadow cascades), else A plus any MRTs. */
static int dr_num_rts(const D3D12DrawRecord* dr)
{
    if (dr->ctgt == 0) return 0;
    int n = 1;
    while (n < 4 && dr->rt_mrt[n - 1]) n++;
    return n;
}

/* Which of a draw's texture units are cube textures, as a bit mask; bits
 * 16-31 mark the units sampled with a depth comparison (dr_shadow_mask). The
 * decompiler takes both from this one word, and it is part of the PSO key. */
static u32 dr_cube_mask(const D3D12DrawRecord* dr)
{
    /* ON by default (RSX_CUBE_TEX=0 to disable). This was off while the cube path
     * cost 2262 PSO misses per 20 frames and 0.38 fps -- both caused by inline
     * fragment-program constants baking into the shader source. With those
     * constants hoisted into b1 the cube path costs 0 PSO misses, and with the
     * per-face stride fixed (each face is a whole mip pyramid, not one mip-0
     * image) all six faces decode as clean environment art. */
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("RSX_CUBE_TEX", 1));
    const u32 sh = dr_shadow_mask(dr) << 16;
    if (!en) return sh;
    u32 m = 0;
    for (int u = 0; u < RSX_FP_UNITS; u++) if (dr->tex[u].set && dr->tex[u].cube) m |= 1u << u;
    return m | sh;
}

/* The guest-FP PSO cache is looked up for every draw, and holds thousands of
 * keys once a level has been played: a scan of the whole array per draw was a
 * measurable part of the render thread's frame at 6k draws. Bucketed by
 * fp_addr. */
#define VP_FP_BUCKETS 4096
static int s_vp_fp_head[VP_FP_BUCKETS];
static u32 vp_fp_bucket(u32 fp_addr) { return ((fp_addr >> 4) * 2654435761u) >> 20; }

/* Append a finished (or failed: pso NULL) key to the guest-FP PSO cache. */
static ID3D12PipelineState* vp_fp_insert(const VPFPEntry* e)
{
    if (s_d3d.vp_fp_n >= s_d3d.vp_fp_cap) {
        const int cap = s_d3d.vp_fp_cap ? s_d3d.vp_fp_cap * 2 : VP_FP_CACHE_INIT;
        VPFPEntry* grown = (VPFPEntry*)realloc(s_d3d.vp_fp, sizeof(VPFPEntry) * (size_t)cap);
        if (!grown) return e->pso;              /* uncached, still usable */
        s_d3d.vp_fp = grown; s_d3d.vp_fp_cap = cap;
    }
    const int idx = s_d3d.vp_fp_n++;
    s_d3d.vp_fp[idx] = *e;
    VPFPEntry* n = &s_d3d.vp_fp[idx];
    int* head = &s_vp_fp_head[vp_fp_bucket(e->fp_addr)];
    n->next = *head - 1;     /* heads are stored +1 so 0 means empty */
    *head = idx + 1;
    return e->pso;
}

/* The entry with exactly this key, or NULL. Entries are only appended, so a
 * bucket's chain stays valid as the array grows. */
static VPFPEntry* vp_fp_find(const VPFPEntry* k)
{
    for (int i = s_vp_fp_head[vp_fp_bucket(k->fp_addr)] - 1; i >= 0; i = s_d3d.vp_fp[i].next) {
        VPFPEntry* q = &s_d3d.vp_fp[i];
        if (q->fp_addr == k->fp_addr && q->vs_idx == k->vs_idx &&
            q->vs_hash == k->vs_hash && q->gen == k->gen &&
            q->blend == k->blend && q->nrt == k->nrt &&
            q->rtfmt == k->rtfmt && q->exp32 == k->exp32 &&
            q->ucode_hash == k->ucode_hash && q->cmask == k->cmask &&
            q->cull == k->cull && q->depth == k->depth &&
            q->cube_mask == k->cube_mask && q->d2c_mask == k->d2c_mask &&
            q->uvclamp == k->uvclamp && q->hdrv == k->hdrv)
            return q;
    }
    return NULL;
}

/* PSO_THREADS=<n> (live; default half the processors less one, at most 7;
 * 0 = off): the pipelines a frame's draw pre-pass finds missing are built on
 * n worker threads and the render thread together, at the end of the
 * pre-pass (psj_finish), instead of one after another as each draw asks. A
 * miss costs about 2 ms on a PC whose driver has seen the shaders (7-20 ms
 * when it has not), and on a first start a camera turn brings tens at once:
 * 32 in one Tutorial frame took 55 ms in a row. The frames are the same,
 * since every pipeline exists before the draw loop records anything either
 * way; only the wait is shorter. The decompile and the shader cache stay on
 * the render thread (guest memory, static buffers); the workers build the
 * pipeline, load it from the library or store it there, and add it to the
 * manifest (pso_create_cached). */
#define PSJ_MAX 256
#define PSJ_THREADS_MAX 15
typedef struct {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    D3D12_INPUT_ELEMENT_DESC il[16];
    ID3DBlob *vs, *ps, *gs;             /* held until psj_finish */
    int entry;                          /* its VPFPEntry */
    ID3D12PipelineState* pso;
} PsjJob;
static PsjJob s_psj[PSJ_MAX];
static int s_psj_n;                     /* jobs this frame (render thread) */
static int s_psj_defer;                 /* set during the draw pre-pass */
static int s_psj_last = -1;             /* vp_get_fp_pso: the pending entry it returned NULL for */
static struct { u32 draw; int entry; } s_psj_fix[MAX_DRAWS];   /* draws to fill in */
static int s_psj_fix_n;
static struct {
    SRWLOCK            lk;
    CONDITION_VARIABLE cv;              /* workers: a new batch */
    CONDITION_VARIABLE done;            /* the render thread: the last job is built */
    u32                gen;             /* one per batch */
    LONG               n;               /* jobs; 0 while they are rewritten */
    int                want, started;
} s_psjp = { SRWLOCK_INIT, CONDITION_VARIABLE_INIT, CONDITION_VARIABLE_INIT };   /* gen..want under lk */
static volatile LONG s_psj_next, s_psj_left, s_psj_busy;

static int psj_threads(void)
{
    static int want = -2;
    PS3_KNOB(want, ps3_env_int("PSO_THREADS", -1));
    if (want >= 0) return want < PSJ_THREADS_MAX ? want : PSJ_THREADS_MAX;
    static int dflt = -1;
    if (dflt < 0) {
        SYSTEM_INFO si; GetSystemInfo(&si);
        dflt = (int)si.dwNumberOfProcessors / 2 - 1;
        if (dflt < 1) dflt = 1;
        if (dflt > 7) dflt = 7;
    }
    return dflt;
}

/* Builds jobs from the batch of n until none is left. */
static void psj_take(LONG n)
{
    for (LONG i; (i = InterlockedIncrement(&s_psj_next) - 1) < n; ) {
        PsjJob* j = &s_psj[i];
        if (FAILED(pso_create_cached(&j->pd, &j->pso))) j->pso = NULL;
        if (InterlockedDecrement(&s_psj_left) == 0) {
            AcquireSRWLockExclusive(&s_psjp.lk);   /* the render thread waits or has not looked yet */
            ReleaseSRWLockExclusive(&s_psjp.lk);
            WakeAllConditionVariable(&s_psjp.done);
        }
    }
}

static DWORD WINAPI psj_worker(LPVOID arg)
{
    const int me = (int)(intptr_t)arg;
    u32 seen = 0;
    for (;;) {
        AcquireSRWLockExclusive(&s_psjp.lk);
        while (s_psjp.gen == seen)
            SleepConditionVariableSRW(&s_psjp.cv, &s_psjp.lk, INFINITE, 0);
        seen = s_psjp.gen;
        const LONG n = me < s_psjp.want ? s_psjp.n : 0;
        InterlockedIncrement(&s_psj_busy);   /* under lk: see psj_finish */
        ReleaseSRWLockExclusive(&s_psjp.lk);
        if (n) psj_take(n);
        InterlockedDecrement(&s_psj_busy);
    }
}

/* vp_get_fp_pso in the pre-pass, on a miss: the key goes into the cache as
 * pending and the pipeline into this frame's batch, which takes the caller's
 * reference to ps. The entry's index, or -1 (full: build it now). */
static int psj_add(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, const D3D12_INPUT_ELEMENT_DESC* il,
                   ID3DBlob* vs, ID3DBlob* ps, ID3DBlob* gs, const VPFPEntry* key)
{
    if (s_psj_n >= PSJ_MAX) return -1;
    const int at = s_d3d.vp_fp_n;
    VPFPEntry e = *key;
    e.pso = NULL; e.pending = 1;
    vp_fp_insert(&e);
    if (s_d3d.vp_fp_n != at + 1) return -1;   /* the cache could not grow */
    PsjJob* j = &s_psj[s_psj_n++];
    j->pd = *pd;
    memcpy(j->il, il, sizeof j->il);
    j->pd.InputLayout.pInputElementDescs = j->il;
    j->vs = vs; vs->lpVtbl->AddRef(vs);
    j->ps = ps;
    j->gs = gs; if (gs) gs->lpVtbl->AddRef(gs);
    j->entry = at;
    j->pso = NULL;
    return at;
}

/* End of the draw pre-pass: build the batch, then give its pipelines to
 * their cache entries and to the draws that asked for them. */
static void psj_finish(void)
{
    const LONG n = s_psj_n;
    if (!n) { s_psj_fix_n = 0; return; }
    const double t0 = perf_now();
    int use = psj_threads();
    if (use > n - 1) use = n - 1;           /* the render thread builds one too */
    while (s_psjp.started < use) {
        HANDLE h = CreateThread(NULL, 0, psj_worker, (LPVOID)(intptr_t)s_psjp.started, 0, NULL);
        if (!h) break;
        CloseHandle(h);
        s_psjp.started++;
    }
    if (use > s_psjp.started) use = s_psjp.started;
    /* The last batch's workers are done building, but one may still be in
     * its job loop, and one that wakes late must find no jobs: a worker
     * raises busy under lk, as it reads n. */
    AcquireSRWLockExclusive(&s_psjp.lk);
    s_psjp.n = 0;
    ReleaseSRWLockExclusive(&s_psjp.lk);
    while (s_psj_busy) YieldProcessor();
    s_psj_next = 0;
    s_psj_left = n;
    AcquireSRWLockExclusive(&s_psjp.lk);
    s_psjp.want = use; s_psjp.n = n; s_psjp.gen++;
    ReleaseSRWLockExclusive(&s_psjp.lk);
    if (use > 0) WakeAllConditionVariable(&s_psjp.cv);
    psj_take(n);
    AcquireSRWLockExclusive(&s_psjp.lk);
    while (s_psj_left > 0) SleepConditionVariableSRW(&s_psjp.done, &s_psjp.lk, INFINITE, 0);
    ReleaseSRWLockExclusive(&s_psjp.lk);
    int failed = 0;
    for (LONG i = 0; i < n; i++) {
        PsjJob* j = &s_psj[i];
        VPFPEntry* e = &s_d3d.vp_fp[j->entry];
        e->pso = j->pso;
        e->pending = 0;
        if (!j->pso) {
            static int said = 0;
            failed++;
            if (said < 16 && ++said) printf("[FP] PSO FAIL (fp=0x%08X)\n", e->fp_addr);
        }
        j->vs->lpVtbl->Release(j->vs);
        j->ps->lpVtbl->Release(j->ps);
        if (j->gs) j->gs->lpVtbl->Release(j->gs);
    }
    for (int k = 0; k < s_psj_fix_n; k++)
        s_d3d.draws[s_psj_fix[k].draw].pso = s_d3d.vp_fp[s_psj_fix[k].entry].pso;
    const double ms = (perf_now() - t0) * 1000.0;
    if (perf_on()) { s_perf_cpso += ms / 1000.0; s_perf_cpso_n += (int)n; }
    { static int said = 0;
      if (said < 3 && ++said)
          printf("[D3D12] %ld missing pipelines built on %d threads in %.1f ms%s (PSO_THREADS)\n",
                 (long)n, use + 1, ms, failed ? ", some failed" : ""); }
    s_psj_n = 0;
    s_psj_fix_n = 0;
}

/* The fragment programs' texts are rsxc_shader.c's (step C3a): the b1
 * binding, the variants list (fp_variants.txt), the program memo, the
 * sprite-edge clamp's pixel half and the HDR variants. These keep the names
 * the draw path calls them by. The debug rewrites (FP_ONE, FP_SHOW,
 * FP_IDCOLOR, FP_TEX, FP_KILL) edit the colour-0 output: fp_out_anchor finds
 * it (with want_stmt, the whole statement) and sets *end to its ';';
 * fp_set_out replaces it (1 if it did). */
static char* fp_out_anchor(char* hlsl, char** end, int want_stmt) { return rsxc_fp_out_anchor(hlsl, end, want_stmt); }
static int fp_set_out(char* hlsl, size_t cap, const char* expr) { return rsxc_fp_set_out(hlsl, cap, expr); }

/* Knobs baked into a pipeline when it is built. The cache keys don't hold
 * them, so after the console changes one of them every pipeline is rebuilt
 * (vp_gen); other setenvs leave the cache alone. */
static void vp_pso_knobs_check(void)
{
    static unsigned seen_gen = 0, sig = 0;
    if (seen_gen == ps3_env_gen) return;
    seen_gen = ps3_env_gen;
    static const char* const names[] = {
        "CULL_OFF", "CULL_FLIP", "DEPTH_OFF", "CMASK_FORCE", "FP_CONSTBUF", "FP_FORCE",
        "FP_IDCOLOR", "FP_KILL", "FP_OFF", "FP_ONE", "FP_PICK", "FP_SHOW", "FP_SUB",
        "FP_TEX", "FP_TEX_FP", "FP_TEX_UV", NULL };
    unsigned h = 2166136261u;
    for (int i = 0; names[i]; i++) {
        const char* v = getenv(names[i]);
        for (const char* c = v ? v : ""; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
        h = (h ^ 0xFFu) * 16777619u;
    }
    if (sig && h != sig) {
        s_d3d.vp_gen++;
        fprintf(stderr, "[D3D12] pipeline knobs changed: rebuilding pipelines%c", 10);
    }
    sig = h;
}

/* The HLSL the draw path compiles for a fragment program, its variant noted
 * in fp_variants.txt with note = 1 (rsxc_fp_build_hlsl; rsxc_fp_baked() says
 * whether its constants are in its text). */
static int fp_build_hlsl(const u8* ucode, int exp32, u32 cube_mask, u32 d2c_mask,
                         char* hlsl, size_t cap, u32 fp_addr, int note)
{
    return rsxc_fp_build_hlsl(ucode, exp32, cube_mask, d2c_mask, hlsl, cap, fp_addr, note);
}

/* rsx_fp_code_hash of the program at guest offset `off` and its size, and
 * the texture units it samples, memoised (rsxc_fp_hash_at, rsxc_fp_units_at):
 * the PSO lookup and the SRV binding need them for every draw. */
static u32 fp_hash_at(u32 off, u32* size_out) { return rsxc_fp_hash_at(off, size_out); }
static u32 fp_units_at(u32 off) { return rsxc_fp_units_at(off); }

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
static u32 dr_uvclamp(const D3D12DrawRecord* dr)
{
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("RSX_SCALE_UVCLAMP", 1));
    const u32 pct = rsx_scale_pct();
    if (!en || pct == 100 || (dr->depth & 1u)) return 0;
    if (dr->rt_off) {   /* an offscreen target: scaled unless it is small (rt_host_dim) */
        const u32 w = dr->rt_w ? dr->rt_w : s_d3d.width, h = dr->rt_h ? dr->rt_h : s_d3d.height;
        if (rt_host_dim(w, h, w) == w && rt_host_dim(w, h, h) == h) return 0;
    }
    return pct;
}

/* The geometry half: rsxc_uvclamp_gs_hlsl's source, compiled once (gs_5_0,
 * not through the disk cache). The pixel half is fp_inject_uvclamp's
 * (rsxc_fp_inject_uvclamp): the bounds as inputs after TEXCOORD9 and the
 * clamp at the top of main; 0 leaves the text as it was. */
static ID3DBlob* uvclamp_gs(void)
{
    static ID3DBlob* gs = NULL;
    static int tried = 0;
    if (tried) return gs;
    tried = 1;
    size_t n = 0;
    const char* src = rsxc_uvclamp_gs_hlsl(&n);
    ID3DBlob* err = NULL;
    if (FAILED(sc_compile(GPU_SC_GS, src, n, "uvclamp_gs", NULL, GPU_SC_NO_CACHE, &gs, &err))) {
        if (err) { fprintf(stderr, "[D3D12] RSX_SCALE_UVCLAMP: %s%c", (const char*)err->lpVtbl->GetBufferPointer(err), 10);
                   err->lpVtbl->Release(err); }
        gs = NULL;
    }
    return gs;
}
static int fp_inject_uvclamp(char* hlsl, size_t cap) { return rsxc_fp_inject_uvclamp(hlsl, cap); }

/* RSX_HDR_SCENE_FP (see HDR output): the clamp limit for this program, 0 if it
 * is not named; with fp 0, how many are named. */
#define HDR_SCENE_MAX 8
static int off_rt_find(u32 off);
static float hdr_scene_limit(u32 fp)
{
    static u32 s_fp[HDR_SCENE_MAX];
    static float s_lim[HDR_SCENE_MAX];
    static int n;
    static unsigned gen;
    if (gen != ps3_env_gen) {
        gen = ps3_env_gen;
        n = 0;
        const char* e = getenv("RSX_HDR_SCENE_FP");
        while (e && *e && n < HDR_SCENE_MAX) {
            char* end = NULL;
            const unsigned long v = strtoul(e, &end, 16);
            if (end == e) break;
            float l = 0.0f;
            if (*end == ':') l = strtof(end + 1, &end);
            if (v) { s_fp[n] = (u32)v; s_lim[n] = l > 0.0f ? l : 65504.0f; n++; }
            e = end;
            while (*e == ',' || *e == ' ') e++;
        }
    }
    if (!fp) return (float)n;
    for (int i = 0; i < n; i++) if (s_fp[i] == fp) return s_lim[i];
    return 0.0f;
}

/* The HDR variant of a draw (PSO key): a named scene program drawing into T
 * (its clamp limit's float bits), or a draw into an unpromoted target that
 * samples a promoted one (HDRV_SAT and the units). */
#define HDRV_SAT 0x80000000u
static u32 dr_hdrv(const D3D12DrawRecord* dr)
{
    if (!s_hdr.stage_b || !s_hdr.t_off) return 0;
    int promoted = dr->rt_off && dr->rt_off == s_hdr.t_off;
    for (int i = 0; i < 4; i++) promoted |= dr->rt_off && dr->rt_off == s_hdr.x_off[i];
    if (!promoted) {
        u32 mask = 0;
        for (int u = 0; u < RSX_FP_UNITS; u++) {
            const int k = dr->tex_rt[u];
            if (k >= 0 && s_d3d.off_rt[k].res && s_d3d.off_rt[k].dxgi != s_d3d.off_rt[k].gdxgi) mask |= 1u << u;
        }
        return mask ? HDRV_SAT | mask : 0;
    }
    if (dr->rt_off != s_hdr.t_off) return 0;
    const float l = hdr_scene_limit(dr->fp_addr);
    if (l <= 0.0f) return 0;
    u32 b;
    memcpy(&b, &l, 4);
    return b;
}

/* A fragment program's text in HDR variant hdrv (dr_hdrv), as vp_get_fp_pso
 * makes it (rsxc_shader_hdr); also the route's shader_hdr. Returns how many
 * clamps or samples it changed. */
static int rc_shader_hdr(char* hlsl, u32 cap, u32 hdrv) { return rsxc_shader_hdr(hlsl, cap, hdrv); }

/* At a batch's start, before any target is made: the targets promoted in this
 * batch, as the last batch found them. */
static void hdr_batch_start(void)
{
    s_hdr.frame_t = -1;
    if (!s_hdr.active || hdr_scene_limit(0) <= 0.0f) {
        if (s_hdr.stage_b) printf("[D3D12] HDR: the game's highlights off\n");
        s_hdr.stage_b = 0;
        s_hdr.t_off = s_hdr.next_t = 0;
        memset(s_hdr.x_off, 0, sizeof s_hdr.x_off);
        memset(s_hdr.next_x, 0, sizeof s_hdr.next_x);
        return;
    }
    if (!s_hdr.stage_b) printf("[D3D12] HDR: the game's highlights on\n");
    s_hdr.stage_b = 1;
    if (s_hdr.t_off != s_hdr.next_t || memcmp(s_hdr.x_off, s_hdr.next_x, sizeof s_hdr.x_off)) {
        s_hdr.t_off = s_hdr.next_t;
        memcpy(s_hdr.x_off, s_hdr.next_x, sizeof s_hdr.x_off);
        printf("[D3D12] HDR: FP16 scene target 0x%X, copies 0x%X 0x%X 0x%X 0x%X\n", s_hdr.t_off,
               s_hdr.x_off[0], s_hdr.x_off[1], s_hdr.x_off[2], s_hdr.x_off[3]);
    }
}

/* After the pre-pass (draw records, tex_rt), before the replay: T and the
 * targets T is copied into after a named scene program, for the next batch,
 * and T's slot for this batch's blit. The display copy as aa_pick finds it:
 * the last draw into a display buffer that writes colour and samples a
 * target; T is the largest target it samples, at the display's size. */
static void hdr_mark_scan(void)
{
    if (!s_hdr.stage_b) return;
    const u32 n = s_d3d.draw_count < MAX_DRAWS ? s_d3d.draw_count : MAX_DRAWS;
    int t = -1;
    u32 dc = 0;
    for (u32 d = n; d-- > 0; ) {
        const D3D12DrawRecord* dr = &s_d3d.draws[d];
        if (!dr->is_vp || dr->is_clear || dr->rt_off || !(dr->cmask & 7u)) continue;
        u64 best = 0;
        for (int u = 0; u < RSX_FP_UNITS; u++) {
            const int k = dr->tex_rt[u];
            if (k < 0) continue;
            const u64 a = (u64)s_d3d.off_rt[k].w * s_d3d.off_rt[k].h;
            if (a > best) { best = a; t = k; }
        }
        if (t >= 0) { dc = d; break; }
    }
    if (t < 0) return;   /* not a frame that shows the scene: the marks stay */
    const OffRT* T = &s_d3d.off_rt[t];
    if (T->w != s_d3d.width || T->h != s_d3d.height || T->gdxgi != DXGI_FORMAT_R8G8B8A8_UNORM) return;
    if (T->dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) s_hdr.frame_t = t;
    u32 x[4] = {0, 0, 0, 0};
    int nx = 0, after = 0;
    for (u32 d = 0; d < dc; d++) {
        const D3D12DrawRecord* dr = &s_d3d.draws[d];
        if (!dr->is_vp || dr->is_clear) continue;
        if (!after) {
            if (dr->rt_off == T->off && hdr_scene_limit(dr->fp_addr) > 0.0f) after = 1;
            continue;
        }
        if (!dr->rt_off || dr->rt_off == T->off) continue;
        for (int u = 0; u < RSX_FP_UNITS; u++) {
            if (dr->tex_rt[u] != t) continue;
            const int k = off_rt_find(dr->rt_off);
            int seen = 0;
            for (int i = 0; i < nx; i++) seen |= x[i] == dr->rt_off;
            if (k >= 0 && !seen && nx < 4 && s_d3d.off_rt[k].w == T->w && s_d3d.off_rt[k].h == T->h &&
                s_d3d.off_rt[k].gdxgi == DXGI_FORMAT_R8G8B8A8_UNORM)
                x[nx++] = dr->rt_off;
            break;
        }
    }
    if (!after) return;   /* no named program in this frame: the marks stay */
    s_hdr.next_t = T->off;
    memcpy(s_hdr.next_x, x, sizeof x);
}

