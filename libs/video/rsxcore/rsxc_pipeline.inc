/* A guest-FP pipeline's description, from its shaders and the draw state
 * vp_get_fp_pso keys it by. A draw's and the pipeline index's (pxi_build_one)
 * come from this one recipe, so the two give a pipeline the same name. Step
 * C3b: the GPU layer's description; the D3D12 description the layer makes of
 * it is today's, byte for byte but the pointers (call map F, G8), so the
 * manifest's names (pso_desc_key of that) are today's too. */
static void vp_fp_desc(gpu_pipeline_desc* pd, gpu_shader vs, int fetch, gpu_shader ps, gpu_shader gs,
                       u32 blend, int nrt, gpu_format rtfmt, u32 cmask, u32 cull, u32 depth)
{
    memset(pd, 0, sizeof *pd);   /* zeroed whole: the GPU layer names the pipeline by its bytes */
    pd->layout = s_d3d.vp_layout;
    pd->vs = vs;
    pd->ps = ps;
    pd->gs = gs;
    if (!fetch) {   /* (fetch: SV_VertexID only, no input layout) */
        pd->vertex.count = 16;
        pd->vertex.stride = 256;
        for (int _i = 0; _i < 16; _i++) {
            strcpy(pd->vertex.attr[_i].semantic, "ATTR");
            pd->vertex.attr[_i].semantic_index = (u32)_i;
            pd->vertex.attr[_i].format = GPU_FORMAT_R32G32B32A32_FLOAT;
            pd->vertex.attr[_i].offset = (u32)(_i * 16);
        }
    }
    /* Guest face culling. CULL_OFF=1 forces double-sided, the old
     * unconditional behaviour -- geometry that vanishes under culling is a
     * winding/front-face problem, not a missing draw. */
    { static int _co = -1; PS3_KNOB(_co, getenv("CULL_OFF") ? 1 : 0);
      pd->cull = (!_co && (cull & 1u)) ? ((cull & 2u) ? GPU_CULL_FRONT : GPU_CULL_BACK) : GPU_CULL_NONE;
      /* CULL_FLIP=1: invert the front-face convention. Our VP path may hand
       * D3D the opposite winding from the guest's, in which case honouring
       * CULL_FACE culls exactly the faces it should keep. */
      static int _cfl = -1; PS3_KNOB(_cfl, getenv("CULL_FLIP") ? 1 : 0);
      pd->front_ccw = (((cull & 4u) ? 1 : 0) ^ _cfl) ? 1 : 0; }
    pd->depth_clip = (cull & 8u) ? 0 : 1;   /* rsx_cull_key */
    /* Blend per the guest's packed key (see rsx_blend_key): dbgfont text needs
     * straight alpha; demosaic's effect passes blend OFF; DeferredShading's
     * light accumulation is additive ONE,ONE. Every target takes it (the GPU
     * layer writes it into each of the nrt targets: zero-init left
     * RenderTargetWriteMask 0 on secondary targets, masking every MRT-B write). */
    pd->blend.enable    = (blend & 1) ? 1 : 0;
    pd->blend.src       = (blend & 1) ? (u8)((blend >> 1)  & 0x1F) : (u8)GPU_BLEND_ONE;
    pd->blend.dst       = (blend & 1) ? (u8)((blend >> 6)  & 0x1F) : (u8)GPU_BLEND_ZERO;
    pd->blend.op        = (blend & 1) ? (u8)((blend >> 21) & 0x7)  : (u8)GPU_BLEND_OP_ADD;
    pd->blend.src_alpha = (blend & 1) ? (u8)((blend >> 11) & 0x1F) : (u8)GPU_BLEND_ONE;
    pd->blend.dst_alpha = (blend & 1) ? (u8)((blend >> 16) & 0x1F) : (u8)GPU_BLEND_ZERO;
    pd->blend.op_alpha  = (blend & 1) ? (u8)((blend >> 24) & 0x7)  : (u8)GPU_BLEND_OP_ADD;
    pd->sample_mask = UINT_MAX;
    pd->topology_type = GPU_TOPOLOGY_TYPE_TRIANGLE;
    /* Strip draws (vf_strip_native) cut at 0xFFFFFFFF; list draws never hold it. */
    pd->strip_cut = GPU_STRIP_CUT_FFFFFFFF;
    pd->nrt = (u32)nrt;
    for (int _r = 0; _r < nrt && _r < 4; _r++) pd->rt_format[_r] = (gpu_format)rtfmt;
    /* Guest colour write mask (RGBA nibble, already D3D-ordered). cmask is 0xF
     * by default (unset colour_mask register decodes to all-on); a literal 0
     * therefore means the guest EXPLICITLY masked every channel -- a depth-only
     * pass (e.g. DeferredShading's shadow-map generation). Honour it: forcing 0
     * back to 0xF splatters the depth pass's fragment colour onto the target. */
    /* CMASK_FORCE=1: ignore the guest colour mask (write RGBA on every draw).
     * A mis-decoded mask makes geometry rasterize correctly and write nothing,
     * which is indistinguishable from "the draw never happened". */
    { static int _cf = -1; PS3_KNOB(_cf, getenv("CMASK_FORCE") ? 1 : 0);
      if (_cf) cmask = 0xF; }
    /* With no colour target the mask writes nothing; it is every channel
     * there, as the D3D12 description's target 0 has it, so that one D3D12
     * description is one name in the GPU layer's library (pso_rec_desc). */
    pd->blend.write_mask = nrt ? (u8)(cmask & 0xF) : (u8)GPU_WRITE_ALL;
    pd->ds_format = rc_ds_format();   /* (step V5: D24S8, or D32S8) */
    /* Guest depth state (rsx_depth_key). With the test off RSX neither tests
     * nor writes depth, which is D3D's DepthEnable FALSE as well. */
    pd->depth_test = (depth & 1u) ? 1 : 0;
    pd->depth_write = (depth & 2u) ? 1 : 0;
    pd->depth_func = (u8)(((depth >> 2) & 7u) + 1u);
    /* DEPTH_OFF=1: drop the depth test for guest-FP draws, so submission order
     * alone decides what is on top. Paired with DRAW_LAST_TEX this puts one
     * object in front of everything and answers "is it merely occluded?" --
     * which reordering alone cannot, because a relocated draw still fails the
     * depth test against whatever is already in the buffer. Diagnostic. */
    { static int doff = -1;
      PS3_KNOB(doff, ps3_env_int("DEPTH_OFF", 0));
      if (doff) { pd->depth_test = 0;
                  pd->depth_write = 0; } }
    pd->samples = 1;
}

/* ---- The pipeline index --------------------------------------------------
 * The port keeps, for each game version, a list of the pipelines players'
 * games have asked for: each one's two guest programs as code fingerprints,
 * and the draw state vp_get_fp_pso keys a pipeline by. A fingerprint cannot
 * be turned back into a program, so nothing in the list is the game's.
 *
 * Recording: every pipeline a draw asks for (vp_get_fp_pso's miss path, once
 * per key per run) is a row, and the rows this cache has not seen are added
 * to pso_index_seen.tsv in the shader cache, off the render thread. The
 * port's tools merge those files into its list.
 *
 * Building: the startup shader pass (rsx_shader_precompile_index) finds each
 * row's programs in the game's files by their fingerprints, makes their
 * shader texts as a draw would, puts them in the cache and hands the row
 * here with the texts' cache keys (rc_pso_index_add), then says the
 * list is complete
 * (rc_pso_index_ready). Once the manifest's startup build is done,
 * worker threads build the rows neither the manifest nor the library has,
 * below normal priority, in the list's order. A draw that asks for one then
 * loads it from the library instead of waiting on the driver, 7-20 ms for a
 * pipeline the driver has not seen. The rows built go into the manifest like
 * any other pipeline, and a build that got through the whole list writes the
 * list's hash to pso_index.built, so the next start's pass skips the list
 * (until the list changes, or a new library removes the file: pso_lib_open).
 *
 * A row, tab-separated, in hex:
 *   vp vstart vfetch fp exp32 cube d2c clamp blend nrt rtfmt cmask cull depth [hdrv]
 * vp: FNV-1a 64 of the vertex program's words up to its END instruction, as
 *   the transform program store holds them (little-endian); vstart: the
 *   transform program start it was decompiled at; vfetch: 1 for the fetch
 *   form.
 * fp: rsx_fp_code_hash64 (the instructions without their inline constants);
 *   exp32, cube, d2c: as fp_build_hlsl takes them, the masks cut to the units
 *   the program samples; clamp: 1 for the sprite-edge clamp form.
 * The rest are vp_get_fp_pso's arguments. hdrv, only in a row with HDR on
 * (RSX_HDR), when it is not 0: the program's HDR variant (dr_hdrv), a read
 * clamp's mask cut to the units the program samples. A row without it builds
 * the plain text, as every row did before the field.
 * A program whose constants are in its text (rsxc_fp_baked) is not recorded.
 * PS3_PSO_INDEX=0 turns recording and building off. */
static int pso_index_on(void)
{
    static int on = -1;
    if (on < 0) on = ps3_env_int("PS3_PSO_INDEX", 1) != 0;
    return on;
}

static SRWLOCK s_pxr_lock = SRWLOCK_INIT;        /* the rows below */
static SRWLOCK s_pxr_file_lock = SRWLOCK_INIT;   /* pso_index_seen.tsv */
static u64*    s_pxr_seen;                       /* rows this cache has, as hashes (open addressing, 0 = empty) */
static u32     s_pxr_seen_n, s_pxr_seen_cap;
static char*   s_pxr_pend;                       /* rows not written yet */
static size_t  s_pxr_pend_n, s_pxr_pend_cap;
static int     s_pxr_loaded;
static u32     s_pxr_quiet;                      /* presents since the last new row */
static char    s_pxr_game[64];                   /* the game and its version, for the file's first line */

static void rc_pso_index_game(const char* game)
{
    snprintf(s_pxr_game, sizeof s_pxr_game, "%s", game ? game : "");
}

/* 1 if h is new. With s_pxr_lock. */
static int pxr_seen_add(u64 h)
{
    if (!h) h = 1;
    if ((s_pxr_seen_n + 1) * 2 > s_pxr_seen_cap) {
        const u32 cap = s_pxr_seen_cap ? s_pxr_seen_cap * 2 : 1024;
        u64* t = (u64*)calloc(cap, sizeof *t);
        if (!t) return 1;   /* out of memory: write it again rather than lose it */
        for (u32 i = 0; i < s_pxr_seen_cap; i++)
            if (s_pxr_seen[i]) {
                u32 j = (u32)s_pxr_seen[i] & (cap - 1);
                while (t[j]) j = (j + 1) & (cap - 1);
                t[j] = s_pxr_seen[i];
            }
        free(s_pxr_seen);
        s_pxr_seen = t; s_pxr_seen_cap = cap;
    }
    u32 j = (u32)h & (s_pxr_seen_cap - 1);
    while (s_pxr_seen[j]) {
        if (s_pxr_seen[j] == h) return 0;
        j = (j + 1) & (s_pxr_seen_cap - 1);
    }
    s_pxr_seen[j] = h; s_pxr_seen_n++;
    return 1;
}

/* The rows the file holds already. With s_pxr_lock. */
static void pxr_load(void)
{
    s_pxr_loaded = 1;
    char path[MAX_PATH + 32];
    pso_path(path, sizeof path, "pso_index_seen.tsv");
    long n = 0;
    char* t = (char*)pso_read_file(path, &n);
    if (!t) return;
    for (long i = 0; i < n; ) {
        long e = i;
        while (e < n && t[e] != '\n') e++;
        if (e > i && t[i] != '#') pxr_seen_add(fnv_bytes(14695981039346656037ull, t + i, (size_t)(e - i)));
        i = e + 1;
    }
    free(t);
}

static void pso_index_note(const VPVSEntry* vs, const u8* fp_ucode, int exp32, u32 cube, u32 d2c, int clamp,
                           u32 blend, int nrt, gpu_format rtfmt, u32 cmask, u32 cull, u32 depth, u32 hdrv)
{
    if (!pso_index_on() || !vs->code64 || rsxc_fp_baked() || !shader_cache_dir()[0]) return;
    const u32 used = rsx_fp_texture_units(fp_ucode, RSX_FP_MAX_BYTES);
    cube &= used | (used << 16); d2c &= used;
    if ((hdrv & HDRV_SAT) && !(hdrv & used)) hdrv = 0;   /* no promoted target sampled: the plain text */
    else if (hdrv & HDRV_SAT) hdrv &= HDRV_SAT | used;
    char row[192];
    int n = snprintf(row, sizeof row, "%016llx\t%x\t%d\t%016llx\t%d\t%x\t%x\t%d\t%x\t%d\t%x\t%x\t%x\t%x",
                     (unsigned long long)vs->code64, vs->start, vs->fetch ? 1 : 0,
                     (unsigned long long)rsx_fp_code_hash64(fp_ucode, RSX_FP_MAX_BYTES),
                     exp32 ? 1 : 0, cube, d2c, clamp ? 1 : 0, blend, nrt, (u32)rtfmt, cmask & 0xFu,
                     cull, depth);
    if (n > 0 && n < (int)sizeof row && hdrv) n += snprintf(row + n, sizeof row - (size_t)n, "\t%x", hdrv);
    if (n <= 0 || n >= (int)sizeof row) return;
    AcquireSRWLockExclusive(&s_pxr_lock);
    if (!s_pxr_loaded) pxr_load();
    if (pxr_seen_add(fnv_bytes(14695981039346656037ull, row, (size_t)n))) {
        if (s_pxr_pend_n + (size_t)n + 1 > s_pxr_pend_cap) {
            const size_t cap = s_pxr_pend_cap ? s_pxr_pend_cap * 2 : 16384;
            char* g = (char*)realloc(s_pxr_pend, cap);
            if (g) { s_pxr_pend = g; s_pxr_pend_cap = cap; }
        }
        if (s_pxr_pend_n + (size_t)n + 1 <= s_pxr_pend_cap) {
            memcpy(s_pxr_pend + s_pxr_pend_n, row, (size_t)n);
            s_pxr_pend_n += (size_t)n;
            s_pxr_pend[s_pxr_pend_n++] = '\n';
        }
        s_pxr_quiet = 0;
    }
    ReleaseSRWLockExclusive(&s_pxr_lock);
}

/* The rows not written yet, to the end of the file. */
static void pso_index_flush(void)
{
    AcquireSRWLockExclusive(&s_pxr_file_lock);
    AcquireSRWLockExclusive(&s_pxr_lock);
    char* t = s_pxr_pend;
    const size_t n = s_pxr_pend_n;
    s_pxr_pend = NULL; s_pxr_pend_n = s_pxr_pend_cap = 0;
    ReleaseSRWLockExclusive(&s_pxr_lock);
    if (t) {
        char path[MAX_PATH + 32];
        pso_path(path, sizeof path, "pso_index_seen.tsv");
        const int fresh = GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES;
        FILE* f = fopen(path, "ab");
        if (f) {
            if (fresh) fprintf(f, "# pso_index 1 game=%s\n", s_pxr_game[0] ? s_pxr_game : "unknown");
            fwrite(t, 1, n, f);
            fclose(f);
        }
        free(t);
    }
    ReleaseSRWLockExclusive(&s_pxr_file_lock);
}

static DWORD WINAPI pso_index_flush_thread(LPVOID unused)
{
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    pso_index_flush();
    InterlockedExchange(&s_pxr_writing, 0);
    return 0;
}

/* Once per present (pso_lib_tick): new rows are written once 240 presents
 * have passed without another. */
static void pso_index_tick(void)
{
    if (!s_pxr_pend_n || ++s_pxr_quiet < 240) return;
    s_pxr_quiet = 0;
    if (InterlockedCompareExchange(&s_pxr_writing, 1, 0) == 0) {
        HANDLE t = CreateThread(NULL, 0, pso_index_flush_thread, NULL, 0, NULL);
        if (t) CloseHandle(t); else InterlockedExchange(&s_pxr_writing, 0);
    }
}

/* A row to build: its shaders by their cache keys (the pass has put them in
 * the cache) and its draw state. */
typedef struct { u64 vs_key, ps_key; u32 fetch, clamp, blend, nrt, rtfmt, cmask, cull, depth; } PxiItem;
static PxiItem*      s_pxb;
static u32           s_pxb_n, s_pxb_cap;
static SRWLOCK       s_pxb_lock = SRWLOCK_INIT;   /* the list, until the build starts */
static volatile LONG s_pxb_ready;                 /* the pass has handed over the whole list */
static u64           s_pxb_hash;                  /* the list's, for pso_index.built (0: do not write it) */
static volatile LONG s_pxb_next, s_pxb_built, s_pxb_had, s_pxb_failed;
static int           s_pxb_started;
static double        s_pxb_t0;

static void rc_pso_index_add(int vs_fetch, u64 vs_key, int clamp, u64 ps_key,
                             u32 blend, int nrt, u32 rtfmt, u32 cmask, u32 cull, u32 depth)
{
    if (!pso_index_on()) return;
    PxiItem it;
    memset(&it, 0, sizeof it);
    it.vs_key = vs_key;
    it.ps_key = ps_key;
    it.fetch = vs_fetch ? 1u : 0u; it.clamp = clamp ? 1u : 0u;
    it.blend = blend; it.nrt = (u32)nrt; it.rtfmt = rtfmt; it.cmask = cmask; it.cull = cull; it.depth = depth;
    AcquireSRWLockExclusive(&s_pxb_lock);
    if (!s_pxb_started) {
        if (s_pxb_n == s_pxb_cap) {
            const u32 cap = s_pxb_cap ? s_pxb_cap * 2 : 1024;
            PxiItem* g = (PxiItem*)realloc(s_pxb, sizeof *g * cap);
            if (g) { s_pxb = g; s_pxb_cap = cap; }
        }
        if (s_pxb_n < s_pxb_cap) s_pxb[s_pxb_n++] = it;
    }
    ReleaseSRWLockExclusive(&s_pxb_lock);
}

/* The pass is done; hash: the list's (0 when the pass was stopped early). */
static void rc_pso_index_ready(u64 hash)
{
    AcquireSRWLockExclusive(&s_pxb_lock);
    s_pxb_hash = hash;
    ReleaseSRWLockExclusive(&s_pxb_lock);
    InterlockedExchange(&s_pxb_ready, 1);
}

/* Step P3S: why a row or record was not built (pxi_build_one's -1 to -3). */
static const char* const k_pxi_why[3] = {
    "a shader is not in this target's cache", "the sprite-edge clamp's other form", "the driver refused it"
};
static int pso_log_on(void) { static int lg = -1; PS3_KNOB(lg, ps3_env_int("PS3_PSO_LOG", 0)); return lg; }
/* A neutral manifest record as a row to build. */
static PxiItem gm_item(const gm_rec* r)
{
    PxiItem it;
    it.vs_key = r->vs_key; it.ps_key = r->ps_key;
    it.fetch = r->fetch; it.clamp = r->clamp; it.blend = r->blend; it.nrt = r->nrt; it.rtfmt = r->rtfmt;
    it.cmask = r->cmask; it.cull = r->cull; it.depth = r->depth;
    return it;
}
/* 1 built, 0 there already, -1 not built: a shader missing from the cache,
 * -2 a clamp row of the form this run does not draw (step V5; the index's
 * rows are the GS form's; step P3S: the neutral manifest's clamp 2 is kind
 * 4's), -3 the driver refused it. The shaders are the GPU layer's, made for
 * the row and freed after; gs is the uvclamp GS (never freed). A row built,
 * or there already, goes into the neutral manifest (step P3S). */
static int pxi_build_one(const PxiItem* it, gpu_shader gs)
{
    if (it->clamp && (it->clamp == 2) != (s_uvc_vs != 0)) return -2;
    gpu_shader vs = sc_load(GPU_SC_VS, it->vs_key);
    gpu_shader ps = sc_load(GPU_SC_PS, it->ps_key);
    int r = -1;
    if (GPU_VALID(vs) && GPU_VALID(ps) && (it->clamp != 1 || GPU_VALID(gs))) {
        const gpu_shader none = {0};
        gpu_pipeline_desc pd;
        vp_fp_desc(&pd, vs, (int)it->fetch, ps, it->clamp == 1 ? gs : none,
                   it->blend, (int)it->nrt, (gpu_format)it->rtfmt, it->cmask, it->cull, it->depth);
        r = -3;
        /* Its manifest name (the instance file's: on D3D12 today's,
         * pso_desc_key of the D3D12 description the backend makes of it). */
        const u64 key = rc_be_pso_name(s_rc_dev, &pd, s_vp_root_hash);
        AcquireSRWLockShared(&s_pso_man_lock);
        const int known = pso_names_has(key);   /* the manifest's: its startup build saw to it */
        ReleaseSRWLockShared(&s_pso_man_lock);
        if (known) r = 0;
        else {
            /* Only into the library (step C3c): today's releases the pipeline
             * once stored, and a draw loads it from there. */
            const uint32_t how = rc_pipeline_store(&pd);
            if (how == GPU_PIPELINE_LOADED) r = 0;
            else if (how != GPU_PIPELINE_FAILED) {
                if (how == GPU_PIPELINE_STORED) InterlockedIncrement(&s_pso_lib_new);   /* stored in the library */
                pso_manifest_add(&pd);
                r = 1;
            }
        }
        if (r >= 0) {
            gm_rec rec;
            memset(&rec, 0, sizeof rec);
            rec.magic = GM_REC_MAGIC; rec.size = sizeof rec;
            rec.vs_key = it->vs_key; rec.ps_key = it->ps_key;
            rec.fetch = it->fetch; rec.clamp = it->clamp; rec.blend = it->blend; rec.nrt = it->nrt;
            rec.rtfmt = it->rtfmt; rec.cmask = it->cmask; rec.cull = it->cull; rec.depth = it->depth;
            gm_add(&rec);
        }
    }
    sc_shader_drop(&vs);
    sc_shader_drop(&ps);
    return r;
}

/* Step P3S: a row or record not built, with PS3_PSO_LOG=1: which, and why. */
static volatile LONG s_pxb_why[3], s_gmb_why[3];   /* the index's, the neutral manifest's */
static void pxi_not_built(volatile LONG* why, const char* list, u32 i, const PxiItem* it, int r)
{
    InterlockedIncrement(&why[-r - 1]);
    if (pso_log_on())
        printf("[D3D12] %s %u not built: %s (vs %016llx, ps %016llx, clamp %u)\n", list, i, k_pxi_why[-r - 1],
               (unsigned long long)it->vs_key, (unsigned long long)it->ps_key, it->clamp);
}
/* The reasons' counts, when some were not built. */
static void pxi_why_print(const volatile LONG* why, const char* list, long failed)
{
    if (failed)
        printf("[D3D12] %s: not built: %ld as %s, %ld as %s, %ld as %s\n", list, (long)why[0], k_pxi_why[0],
               (long)why[1], k_pxi_why[1], (long)why[2], k_pxi_why[2]);
}

static DWORD WINAPI pxi_build_thread(LPVOID gs_id)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    gpu_shader gs;
    gs.id = (u32)(uintptr_t)gs_id;
    for (;;) {
        const u32 i = (u32)(InterlockedIncrement(&s_pxb_next) - 1);
        if (i >= s_pxb_n || s_pso_pre_abort) break;
        const int r = pxi_build_one(&s_pxb[i], gs);
        InterlockedIncrement(r > 0 ? &s_pxb_built : r == 0 ? &s_pxb_had : &s_pxb_failed);
        if (r < 0) pxi_not_built(s_pxb_why, "pipeline index row", i, &s_pxb[i], r);
    }
    if (InterlockedDecrement(&s_pxb_run) == 0) {
        const u32 done = (u32)(s_pxb_built + s_pxb_had + s_pxb_failed);
        printf("[D3D12] pipeline index: %ld built, %ld there already, %ld not built, of %u, in %.0f ms\n",
               s_pxb_built, s_pxb_had, s_pxb_failed, s_pxb_n, 1000.0 * (perf_now() - s_pxb_t0));
        pxi_why_print(s_pxb_why, "pipeline index", s_pxb_failed);
        if (done == s_pxb_n && s_pxb_hash) {
            char path[MAX_PATH + 32], txt[24];
            pso_lib_path(path, sizeof path, "index.built");   /* (the GPU layer's library's own) */
            pso_write_file(path, txt, (size_t)snprintf(txt, sizeof txt, "%016llx\n",
                                                       (unsigned long long)s_pxb_hash));
        }
    }
    InterlockedDecrement(&s_pxb_live);
    return 0;
}

/* The uvclamp GS for a list's builds, if a row needs it (step V5: none in
 * the vertex-shader form). Here: it compiles on its first call. */
static gpu_shader pxi_gs_for(int any_gs_row)
{
    const gpu_shader none = {0};
    return any_gs_row && !s_uvc_vs ? uvclamp_gs() : none;
}
/* Threads for a start's build (as the manifest's). */
static int pxi_threads(void)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    const int nt = (int)si.dwNumberOfProcessors / 4;
    return nt < 1 ? 1 : nt > 4 ? 4 : nt;
}

/* pso_lib_tick: the build, once the pass has handed over the list; on as many
 * threads as the manifest's. */
static void pso_index_build_start(void)
{
    if (s_pxb_started || !s_pxb_ready) return;
    AcquireSRWLockExclusive(&s_pxb_lock);
    s_pxb_started = 1;
    ReleaseSRWLockExclusive(&s_pxb_lock);
    if (!s_pxb_n) return;
    int any = 0;
    for (u32 i = 0; i < s_pxb_n && !any; i++) any = s_pxb[i].clamp == 1;
    const gpu_shader gs = pxi_gs_for(any);
    /* (Step V5: in the vertex-shader form the rows with the clamp are not
     * built: their vertex shader is the program's kind 4, made at a draw.) */
    const int nt = pxi_threads();
    s_pxb_t0 = perf_now();
    s_pxb_live = s_pxb_run = nt;
    for (int t = 0; t < nt; t++) {
        HANDLE h = CreateThread(NULL, 0, pxi_build_thread, (LPVOID)(uintptr_t)gs.id, 0, NULL);
        if (h) CloseHandle(h); else { InterlockedDecrement(&s_pxb_run); InterlockedDecrement(&s_pxb_live); }
    }
}

/* Step P3S: the neutral manifest's build (rsxc_psolib.inc): the records
 * after the loaded library's count, as the index's rows. */
static DWORD WINAPI gm_build_thread(LPVOID gs_id)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    gpu_shader gs;
    gs.id = (u32)(uintptr_t)gs_id;
    const u32 n = s_gm_read - s_gm_have;
    for (;;) {
        const u32 i = (u32)(InterlockedIncrement(&s_gmb_next) - 1);
        if (i >= n || s_pso_pre_abort) break;
        const PxiItem it = gm_item(&s_gm[s_gm_have + i]);
        const int r = pxi_build_one(&it, gs);
        InterlockedIncrement(r > 0 ? &s_gmb_built : r == 0 ? &s_gmb_had : &s_gmb_failed);
        if (r < 0) pxi_not_built(s_gmb_why, "pipeline manifest record", s_gm_have + i, &it, r);
    }
    if (InterlockedDecrement(&s_gmb_run) == 0) {
        printf("[D3D12] pipeline manifest build: %ld built, %ld there already, %ld not built, of %u, in %.0f ms\n",
               (long)s_gmb_built, (long)s_gmb_had, (long)s_gmb_failed, n, 1000.0 * (perf_now() - s_gmb_t0));
        pxi_why_print(s_gmb_why, "pipeline manifest build", s_gmb_failed);
        /* (the count moves on only once every record is in the library) */
        s_gmb_complete = !s_pso_pre_abort && !s_gmb_failed && (u32)(s_gmb_built + s_gmb_had) == n;
    }
    InterlockedDecrement(&s_gmb_live);   /* (after s_gmb_complete: the save reads it once this is 0) */
    return 0;
}
static void gm_build_start(void)
{
    if (s_gmb_started) return;
    s_gmb_started = 1;
    const u32 n = s_gm_read - s_gm_have;
    if (!n || !ps3_env_int("PS3_PSO_PREBUILD", 1)) { s_gmb_complete = !n; return; }
    int any = 0;
    for (u32 i = s_gm_have; i < s_gm_read && !any; i++) any = s_gm[i].clamp == 1;
    const gpu_shader gs = pxi_gs_for(any);
    const int nt = pxi_threads();
    s_gmb_t0 = perf_now();
    s_gmb_live = s_gmb_run = nt;
    for (int t = 0; t < nt; t++) {
        HANDLE h = CreateThread(NULL, 0, gm_build_thread, (LPVOID)(uintptr_t)gs.id, 0, NULL);
        if (h) CloseHandle(h); else { InterlockedDecrement(&s_gmb_run); InterlockedDecrement(&s_gmb_live); }
    }
}

/* PS3_PSO_WAIT=1 (tests and replays, which end soon after they start): the
 * shutdown lets the start's builds run to their end, the index's too once
 * the pass has handed it over, instead of stopping them. */
static void pso_builds_finish(void)
{
    if (!s_pso_lib_on || !GPU_VALID(s_d3d.vp_layout)) return;
    for (;;) {
        pso_builds_step();
        if (!s_pso_pre_live && s_gmb_started && !s_gmb_live && (!s_pxb_ready || (s_pxb_started && !s_pxb_live)))
            break;
        Sleep(1);
    }
}

/* Compiled guest-FP pipeline for a draw (VS by cache slot + the FP at fp_addr). */
static gpu_pipeline vp_get_fp_pso(int vs_idx, u32 fp_addr, u32 blend, int nrt,
                                gpu_format rtfmt, int exp32, u32 cmask, u32 cull,
                                u32 depth, u32 cube_mask, u32 d2c_mask, u32 uvclamp,
                                u32 hdrv)
{
    s_psj_last = -1;
    s_fp_vs4 = 0;
    vp_pso_knobs_check();
    if (nrt < 0) nrt = 0; if (nrt > 4) nrt = 4;
    if (rtfmt == 0) rtfmt = GPU_FORMAT_R8G8B8A8_UNORM;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);
    if (!fp_addr || !vm_base) return k_rc_no_pipe;
    { static int _off=-1; if(_off<0)_off=getenv("FP_OFF")?1:0; if(_off) return k_rc_no_pipe; }
    /* Resolve the VS: the draw's cached per-VP shader, else the primary. */
    gpu_shader vsb = {0}; u32 vs_hash = 0;
    if (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) {
        vsb = s_d3d.vp_vs[vs_idx].vs; vs_hash = s_d3d.vp_vs[vs_idx].hash;
    }
    if (!GPU_VALID(vsb)) { vsb = s_d3d.vp_vs_shader; vs_idx = -1; }
    if (!GPU_VALID(vsb)) return k_rc_no_pipe;

    /* SET_SHADER_PROGRAM low bits = location+1 (same as textures): 1 = LOCAL
     * (VRAM), 2 = MAIN (gcm/cube emits 0x00B90001 for its VRAM-resident FP). */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    u32 off = cellGcmResolveLocated((fp_addr & 0x3u) == 1, fp_addr & ~0x3u);
    if (off == 0xFFFFFFFFu) return k_rc_no_pipe;

    /* Content hash: inline constants are patched in place per frame (wave's
     * stamp), so identity is the BYTES, not the address. */
    /* Hash the CODE, not the constants: with fp_k[] hoisted into the per-draw
     * constant buffer the compiled shader is invariant under constant changes,
     * so hashing them made the pipeline key move every draw. */
    u32 uhash;
    {
        static int cb = -1;
        PS3_KNOB(cb, ps3_env_int("FP_CONSTBUF", 1));
        u32 usz;
        uhash = fp_hash_at(off, &usz);
        s_perf_pso_hashbytes += (int)usz;
        if (!cb) {
            uhash = 2166136261u;
            const u8* up = vm_base + off;
            for (u32 i = 0; i < usz; i++) { uhash ^= up[i]; uhash *= 16777619u; }
        }
    }

    s_perf_pso_calls++;
    VPFPEntry key;
    memset(&key, 0, sizeof key);
    key.fp_addr = fp_addr; key.vs_idx = vs_idx; key.vs_hash = vs_hash;
    key.gen = s_d3d.vp_gen; key.blend = blend; key.nrt = nrt;
    key.rtfmt = (u32)rtfmt; key.exp32 = exp32; key.ucode_hash = uhash;
    key.cmask = cmask; key.cull = cull; key.depth = depth; key.cube_mask = cube_mask;
    key.d2c_mask = d2c_mask; key.uvclamp = uvclamp != 0; key.hdrv = hdrv;
    key.pso = k_rc_no_pipe;
    { const VPFPEntry* hit = vp_fp_find(&key);
      if (hit) {
          if (hit->pending) s_psj_last = (int)(hit - s_d3d.vp_fp);
          s_fp_vs4 = (int)hit->vs4;
          return hit->pso;
      } }
    s_perf_pso_miss++;      /* falls through to a full decompile + D3DCompile */
    /* PSOMISSDBG=1: on a miss, name the key field that differs from an existing
     * entry for the same program. "the cache misses" is not actionable; "it
     * misses on ucode_hash" is. */
    /* HASHDBG=1: how many DISTINCT ucode hashes each program produces. Inline
     * constants are patched into the shader bytes, so a program whose constants
     * animate yields a new hash -- and a new pipeline -- every time. */
    { static int hd = -1;
      PS3_KNOB(hd, ps3_env_int("HASHDBG", 0));
      if (hd) { static u32 fps[32]; static u32 cnt[32]; static int nf = 0;
        int idx = -1;
        for (int i = 0; i < nf; i++) if (fps[i] == fp_addr) idx = i;
        if (idx < 0 && nf < 32) { idx = nf++; fps[idx] = fp_addr; cnt[idx] = 0; }
        if (idx >= 0) { cnt[idx]++;
          if ((cnt[idx] % 200) == 0)
            fprintf(stderr, "[HASHDBG] fp=0x%X has produced %u distinct compiles%c",
                    fp_addr, cnt[idx], 10); } } }
    { static int md = -1;
      PS3_KNOB(md, ps3_env_int("PSOMISSDBG", 0));
      if (md) { static int n = 0;
        for (int i = 0; i < s_d3d.vp_fp_n && n < 12; i++) {
            VPFPEntry* q = &s_d3d.vp_fp[i];
            if (q->fp_addr != fp_addr) continue;
            n++;
            fprintf(stderr, "[PSOMISS] fp=0x%X differs:%s%s%s%s%s%s%s%s%s%c", fp_addr,
                    q->vs_idx != vs_idx        ? " vs_idx"     : "",
                    q->vs_hash != vs_hash      ? " vs_hash"    : "",
                    q->gen != s_d3d.vp_gen     ? " gen"        : "",
                    q->blend != blend          ? " blend"      : "",
                    q->nrt != nrt              ? " nrt"        : "",
                    q->rtfmt != (u32)rtfmt     ? " rtfmt"      : "",
                    q->ucode_hash != uhash     ? " ucode_hash" : "",
                    q->cube_mask != cube_mask  ? " cube_mask"  : "",
                    q->d2c_mask != d2c_mask    ? " d2c_mask"   : "", 10);
            break;
        } } }
    /* CUBEKEYDBG=1: which (program, cube mask) pairs are being compiled. If one
     * program shows several masks, the mask is unstable and is the reason the
     * pipeline cache thrashes with cube support on. */
    { static int ck = -1;
      PS3_KNOB(ck, ps3_env_int("CUBEKEYDBG", 0));
      if (ck) { static u32 seen[64][2]; static int ns = 0; int known = 0;
        for (int i = 0; i < ns; i++)
            if (seen[i][0] == fp_addr && seen[i][1] == cube_mask) known = 1;
        if (!known && ns < 64) { seen[ns][0] = fp_addr; seen[ns][1] = cube_mask; ns++;
            fprintf(stderr, "[CUBEKEY] fp=0x%X cube_mask=0x%X (distinct pairs=%d)%c",
                    fp_addr, cube_mask, ns, 10); } } }
    static char hlsl[RSX_FP_HLSL_BUFFER_SIZE];
    /* The third parameter is SET_SHADER_CONTROL. Only its 32_BITS_EXPORTS bit
     * matters to the decompiler, and the draw records it (exp32, also part of
     * the cache key). It decides whether the colour comes from r0 or h0; the
     * old inference ("r0 was written, so r0") picked a stale temp for programs
     * that use r0 as scratch and finish in h0 (Demon's Souls's level shaders).
     *
     * Buffered: inline constants become fp_constants[i] reads from b1, which
     * vp_record_cb refills from the guest ucode for every draw. The cache key
     * hashes the code without its constants, so a program compiled with its
     * constants baked in kept the values of its first draw for the rest of the
     * run while the title re-patched them per material (Demon's Souls's level
     * surfaces drew black or saturated). The cube mask goes to the decompiler
     * so cube units are declared and sampled as cubes. */
    int n = fp_build_hlsl(vm_base + off, exp32, cube_mask, d2c_mask, hlsl, sizeof hlsl, fp_addr, 1);
    if (n <= 0) return vp_fp_insert(&key);
    /* FP_LIST=1: every program the title actually compiles, with its size. A
     * fragment program that hangs the GPU shows up here as an implausible
     * instruction count long before it shows up as a TDR. */
    { static int fl = -1; PS3_KNOB(fl, getenv("FP_LIST") ? 1 : 0);
      if (fl) fprintf(stderr, "[FPLIST] fp=0x%08X instrs=%d hlsl=%u bytes%c",
                      fp_addr, n, (unsigned)strlen(hlsl), 10); }
    if (getenv("FP_DUMP")) { static int _d=0; if (_d < 4 && ++_d) {
        FILE* f = fopen("fp_dump.hlsl", _d==1 ? "w" : "a");
        if (f) {
            const u8* uc = vm_base + off;
            fprintf(f, "/* fp_addr=0x%08X vmoff=0x%08X raw:", fp_addr, off);
            for (int _b = 0; _b < 32; _b++) fprintf(f, " %02X", uc[_b]);
            fprintf(f, " */\n%s\n", hlsl); fclose(f);
        } } }

    /* FP_PICK=<hex fp_addr>: write just that program, whatever order it compiles
     * in. fp_dump.hlsl keeps only the first four, same trap as vp2_dump.hlsl. */
    { static const char* fpk = (const char*)1; static u32 wantf = 0; static int done = 0;
      if (fpk == (const char*)1) { fpk = getenv("FP_PICK");
                                   wantf = fpk ? (u32)strtoul(fpk, NULL, 16) : 0; }
      if (wantf && !done && fp_addr == wantf) { done = 1;
          FILE* f = fopen("duck_fp.hlsl", "w");
          if (f) { fprintf(f, "/* fp_addr=0x%08X, %d instrs */%s", fp_addr, n, hlsl);
                   fclose(f); }
          fprintf(stderr, "[DUCKFP] wrote duck_fp.hlsl (fp=0x%08X, %d instrs)%c",
                  fp_addr, n, 10);
      } }

    /* Debug FP_ONE=<hex fp_addr>: force that program's colour output to
     * all-ones (e.g. wave's colour-detect -> full mask -> island borders
     * everywhere -> the water sim must visibly radiate if it works). */
    { const char* f1 = getenv("FP_ONE");
      if (f1 && (u32)strtoul(f1, NULL, 16) == fp_addr) {
          /* Match the assignment, not one exact spelling of it: the final
           * register is r[N] for some programs and h[N] for others, and the old
           * fixed "= r[0];" anchor silently did nothing for every h[] shader --
           * so this switch appeared to have no effect and was useless for
           * identifying which program draws what. Rewrite the expression with a
           * proper length-changing splice. */
          fp_set_out(hlsl, sizeof hlsl, "(1.0).xxxx");
      } }
    /* FP_TEX=1: make every program output its unit-0 texture sample verbatim.
     * Separates "the texture is wrong" from "the shading tints it" in one run,
     * without having to identify which program draws which surface first. */
    /* FP_SHOW=<hlsl expr>: replace a program's colour output with an arbitrary
     * expression (e.g. FP_SHOW=h[1]) so an intermediate register can be looked
     * at directly. Restrict with FP_TEX_FP=<hex>. Reading a 69-instruction
     * shader to guess which term carries a bad value is slower and less certain
     * than displaying the terms. */
    /* FP_KILL=<hex fp_addr>: make that program discard every fragment, so what
     * it covers becomes visible. Distinguishes "this surface shades black" from
     * "this surface is correct and something else is missing behind it". */
    { const char* kf = getenv("FP_KILL");
      if (kf && (u32)strtoul(kf, NULL, 16) == fp_addr) {
          char* semi; char* rp = strstr(hlsl, "PSOut _po;");
          if (!rp) rp = strstr(hlsl, "PSOutput _out;");
          if (!rp) rp = fp_out_anchor(hlsl, &semi, 1);
          if (rp) {
              const char* ins = "discard; ";
              size_t nl = strlen(ins), tail = strlen(rp) + 1;
              if ((size_t)(rp - hlsl) + nl + tail < sizeof(hlsl)) {
                  memmove(rp + nl, rp, tail);
                  memcpy(rp, ins, nl);
                  fprintf(stderr, "[FPKILL] fp=0x%X discards%c", fp_addr, 10);
              }
          }
      } }
    { const char* sh = getenv("FP_SHOW");
      const char* only = getenv("FP_TEX_FP");
      if (sh && (!only || (u32)strtoul(only, NULL, 16) == fp_addr)) {
          char rep[256];
          snprintf(rep, sizeof rep, "float4((%s).xyz, 1.0)", sh);
          if (fp_set_out(hlsl, sizeof hlsl, rep))
              fprintf(stderr, "[FPSHOW] fp=0x%X output replaced with %s%c",
                      fp_addr, sh, 10);
      } }
    /* Cube units are declared and sampled as cubes by the decompiler itself
     * (it is given the mask). */
    if (cube_mask) {
        { static int _n = 0; if (_n < 4 && ++_n)
            fprintf(stderr, "[CUBE] fp=0x%X compiled with cube units mask 0x%X%c",
                    fp_addr, cube_mask, 10); }
    }
    /* FP_IDCOLOR=1: give every fragment program a distinct flat colour derived
     * from its address. One frame then shows which program paints which surface,
     * instead of one run per candidate to test them by elimination.
     * FP_IDCOLOR=h colours only half-precision-export programs: Demon's Souls
     * draws its scene with those and its full-screen post passes with 32-bit
     * exports, so the scene's colours survive the final composite. */
    if (getenv("FP_IDCOLOR") && (strcmp(getenv("FP_IDCOLOR"), "h") || !exp32)) {
        /* Mix properly: every fp address in a title tends to share low
         * bits (they all end 0x01/0x81 here), so a single multiply left
         * one channel constant across all programs and made distinct
         * shaders look like the same colour. */
        u32 hsh = fp_addr * 2654435761u;
        hsh ^= hsh >> 13; hsh *= 0x5BD1E995u; hsh ^= hsh >> 15;
        /* Force the top bit on in each channel: a hash that happens to
         * land near black is indistinguishable from "this surface was
         * never painted", which is the exact question the mode exists
         * to answer. */
        float cr = (float)(((hsh >> 16) & 0x7F) | 0x80) / 255.0f;
        float cg = (float)(((hsh >>  8) & 0x7F) | 0x80) / 255.0f;
        float cb = (float)(((hsh      ) & 0x7F) | 0x80) / 255.0f;
        char rep[128];
        snprintf(rep, sizeof rep, "float4(%.3f,%.3f,%.3f,1.0)", cr, cg, cb);
        if (fp_set_out(hlsl, sizeof hlsl, rep))
            fprintf(stderr, "[FPID] fp=0x%X -> rgb(%.0f,%.0f,%.0f)%c",
                    fp_addr, cr*255, cg*255, cb*255, 10);
    }
    /* FP_TEX=<unit>: output that unit's sample verbatim. FP_TEX_FP=<hex> limits
     * it to one program, so one surface can be inspected without repainting the
     * whole scene. */
    if (getenv("FP_TEX") && (!getenv("FP_TEX_FP") ||
        (u32)strtoul(getenv("FP_TEX_FP"), NULL, 16) == fp_addr)) {
        int _un = atoi(getenv("FP_TEX"));
        if (_un < 0 || _un >= RSX_FP_UNITS) _un = 0;
        /* The bank is an array unless a cube unit made the decompiler declare
         * each unit by name. */
        const int arr = strstr(hlsl, "rsx_tex[16]") != NULL;
        char tex[32];
        const int sarr = strstr(hlsl, "rsx_samp[16]") != NULL;
        if (arr) snprintf(tex, sizeof tex, "rsx_tex[%d].Sample(rsx_samp[%d]", _un, _un);
        else if (sarr) snprintf(tex, sizeof tex, "rsx_tex%d.Sample(rsx_samp[%d]", _un, _un);
        else     snprintf(tex, sizeof tex, "rsx_tex%d.Sample(rsx_samp%d", _un, _un);
        char rep[160];
        /* FP_TEX_UV=1: sample at a fixed centre UV instead of the
         * interpolated texcoord. If the texture is populated but the
         * texcoords never arrive, the normal path samples texel (0,0)
         * -- usually black -- and looks exactly like an empty texture.
         * This tells the two apart. */
        snprintf(rep, sizeof rep, "%s, %s)", tex,
                 getenv("FP_TEX_UV") ? "float2(0.5, 0.5)" : "input.tc0.xy");
        if (fp_set_out(hlsl, sizeof hlsl, rep))
            fprintf(stderr, "[FP_TEX] program 0x%X rewritten to sample unit %d%c",
                    fp_addr, _un, 10);
    }
    /* FP_FORCE=1: replace the translated body with solid magenta -- isolates
     * geometry/transform problems from texture/blend problems. */
    if (getenv("FP_FORCE")) {
        char* rp = strstr(hlsl, "return r[0];");
        if (rp) memcpy(rp, "return float4(1,0,1,1)", 23);
    }
    /* FP_SUB=<hex fp_addr>:<file>: compile that file instead of the translated
     * program. The decompiler's output shape changes between versions, so the
     * string-anchored rewrites above (FP_ONE/FP_TEX) silently miss; a whole
     * replacement always applies. The file must keep the PSInput/rsx_tex[]
     * declarations of the translated program (start from FP_PICK's dump). */
    { const char* fs = getenv("FP_SUB");
      if (fs && (u32)strtoul(fs, NULL, 16) == fp_addr && strchr(fs, ':')) {
          FILE* f = fopen(strchr(fs, ':') + 1, "rb");
          if (f) {
              size_t n2 = fread(hlsl, 1, sizeof(hlsl) - 1, f); hlsl[n2] = 0; fclose(f);
              fprintf(stderr, "[FP_SUB] fp=0x%X <- %s (%u bytes)%c", fp_addr,
                      strchr(fs, ':') + 1, (unsigned)n2, 10);
          }
      } }
    gpu_shader gsb = {0};
    if (uvclamp && !s_uvc_vs) gsb = uvclamp_gs();
    if (GPU_VALID(gsb) && !fp_inject_uvclamp(hlsl, sizeof hlsl)) gsb.id = 0;
    /* Step V5: the vertex-shader form (kind 4) in place of the geometry
     * shader: the program's clamp form, which reads its vertices itself (no
     * input layout), and the same pixel shader. */
    if (uvclamp && s_uvc_vs) {
        const gpu_shader v4 = uvc_vs4(vs_idx);
        if (GPU_VALID(v4) && fp_inject_uvclamp(hlsl, sizeof hlsl)) { vsb = v4; key.vs4 = 1; }
    }
    if (hdrv) {
        const int sat = (hdrv & HDRV_SAT) != 0;
        const int k = rc_shader_hdr(hlsl, sizeof hlsl, hdrv);
        static int said;
        if (said < 16 && ++said) printf("[D3D12] HDR: fp 0x%X's variant, %d %s\n", fp_addr, k,
                                        sat ? (k == 1 ? "sample clamped" : "samples clamped")
                                            : (k == 1 ? "clamp raised" : "clamps raised"));
    }
    gpu_shader pb = shader_compile_cached(hlsl, strlen(hlsl), "guest_fp", "ps_5_0");
    if (!GPU_VALID(pb)) {
        static int _e2=0; if (_e2 < 16 && ++_e2)
            printf("[FP] PS compile FAIL (fp=0x%08X): %s\n", fp_addr, sc_msg("?"));
        return vp_fp_insert(&key);
    }

    gpu_pipeline_desc pd;
    vp_fp_desc(&pd, vsb, key.vs4 || (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n && s_d3d.vp_vs[vs_idx].fetch),
               pb, gsb, blend, nrt, rtfmt, cmask, cull, depth);
    if (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n)   /* a row for the pipeline index (either clamp form's) */
        pso_index_note(&s_d3d.vp_vs[vs_idx], vm_base + off, exp32, cube_mask, d2c_mask,
                       GPU_VALID(gsb) || key.vs4, blend, nrt, rtfmt, cmask, cull, depth, hdrv);
    s_fp_vs4 = (int)key.vs4;
    /* Step P3S: its record in the neutral manifest: the two texts' keys and
     * vp_fp_desc's state (a draw on vp_vs_shader, which no key names, has
     * none). */
    gm_rec rec;
    memset(&rec, 0, sizeof rec);
    if (s_gm_on && vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) {
        const VPVSEntry* const ve = &s_d3d.vp_vs[vs_idx];
        rec.vs_key = key.vs4 ? ve->vs4_key : ve->vs_key;
        rec.ps_key = rsxc_shader_key(1, hlsl);
        rec.fetch = (key.vs4 || ve->fetch) ? 1u : 0u;
        rec.clamp = key.vs4 ? 2u : GPU_VALID(gsb) ? 1u : 0u;
        rec.blend = blend; rec.nrt = (u32)nrt; rec.rtfmt = (u32)rtfmt; rec.cmask = cmask & 0xFu;
        rec.cull = cull; rec.depth = depth;
        if (rec.vs_key && rec.ps_key) { rec.magic = GM_REC_MAGIC; rec.size = sizeof rec; }
    }

    /* In the draw pre-pass: built with the frame's other missing pipelines
     * (psj_finish), which has pb from here. */
    if (s_psj_defer && (s_psj_last = psj_add(&pd, pb, &rec, &key)) >= 0) return k_rc_no_pipe;
    const double cp_t0 = perf_on() ? perf_now() : 0.0;
    const gpu_pipeline pso = pso_create_cached(&pd, &rec);
    if (perf_on()) { s_perf_cpso += perf_now() - cp_t0; s_perf_cpso_n++; }
    sc_shader_drop(&pb);
    if (!pso.v) {
        static int _e3=0; if (_e3 < 16 && ++_e3) printf("[FP] PSO FAIL (fp=0x%08X, 0x%08lX)\n", fp_addr, (long)E_FAIL);
        return vp_fp_insert(&key);
    }
    { static int _ok=0; if (_ok < 32 && ++_ok) printf("[FP] guest FP pipeline ready (fp=0x%08X)\n", fp_addr); }

    key.pso = pso;
    return vp_fp_insert(&key);
}

