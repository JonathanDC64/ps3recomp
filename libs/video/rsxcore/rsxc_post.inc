/* ---------------------------------------------------------------------------
 * Post anti-aliasing (RSX_AA, docs/43)
 *
 * RSX_AA=1 runs an FXAA-style filter (rsx_fxaa.h) over the frame's scene
 * target before the HUD is drawn into it: the scene's edges are smoothed and
 * HUD text stays sharp. RSX_AA=2 runs SMAA 1x (rsx_smaa.h, third_party/smaa)
 * there instead: three passes that find edges, weigh them by their shape and
 * blend along them, so textures stay sharper than under FXAA. If SMAA's
 * pipelines cannot be made, FXAA runs. Live. 0, the default, leaves every
 * frame as it was.
 *
 * Where the pass goes is decided per batch from its draw records alone (guest
 * state, not timing), before the replay starts (aa_pick). Batches that present
 * and hold at least AA_MIN_SCENE records are looked at:
 *   1. The display copy: the last draw into a display buffer that writes
 *      colour and samples an offscreen target. The largest target it samples
 *      is T, the scene target. No display copy: no pass.
 *   2. Fewer than AA_MIN_SCENE depth-tested draws into T: a 2D frame (title,
 *      loading screen, movie), no pass.
 *   3. The last post op: before the display copy, the last draw into T that
 *      writes colour, has blending and the depth test off, and samples a
 *      target or a depth surface. The pass runs right after it. The HUD comes
 *      after: Demon's Souls draws it blended, into T, after its post chain.
 * The pass filters a copy of T (off_rt_snapshot) back into T, RGB only: alpha
 * can hold data (Demon's Souls keeps its depth-of-field mask there). T is
 * RGBA8, or FP16 while HDR keeps the game's highlights in it (RSX_HDR,
 * RSX_HDR_SCENE_FP): the pass has a pipeline for each, and the filter works
 * on values above 1 as it does below.
 *
 * RSX_AA_BEFORE_FP=<fp>[,<fp>...]: run the pass before the first draw with one
 * of these fragment programs (SET_SHADER_PROGRAM values in hex, as the F9 draw
 * list prints them) instead, where the rule picks the wrong place.
 * RSX_AA_AT=present: filter the finished display buffer instead, HUD and all.
 * RSX_AA_LOG: 0 quiet, 1 (default) a line when the choice changes, 2 a line
 * every batch.
 * -----------------------------------------------------------------------*/
#include "rsx_fxaa.h"
#define AA_SRV_SLOT  21   /* SRV heap: the pass's source (slots 21-31 were spare) */
#define AA_MIN_SCENE 16   /* depth-tested draws into T that make a 3D frame */
#define AA_FP_MAX    8
#define AA_LOG_MAX   100  /* change lines per run at RSX_AA_LOG=1 */

static int knob_aa(void)     { static int v = -1; PS3_KNOB(v, ps3_env_int("RSX_AA", 0)); return v; }
static int knob_aa_log(void) { static int v = -1; PS3_KNOB(v, ps3_env_int("RSX_AA_LOG", 1)); return v; }
/* RSX_AA's filter: 1 FXAA, 2 SMAA, 0 none (any other value too). */
static int aa_mode(void)     { const int m = knob_aa(); return m == 1 || m == 2 ? m : 0; }
static int aa_at_present_env(void)
{
    const char* e = getenv("RSX_AA_AT");
    return e && !strcmp(e, "present");
}
static int knob_aa_at_present(void) { static int v = -1; PS3_KNOB(v, aa_at_present_env()); return v; }

enum { AA_RUN, AA_NO_COPY, AA_SMALL, AA_FORMAT, AA_NO_SCENE, AA_NO_POST };
static const char* const s_aa_why[] = {
    "", "no display copy", "the display copy's source is small",
    "the scene target is neither RGBA8 nor FP16", "no 3D scene", "no post pass before the display copy",
};

static struct {
    gpu_layout            layout;     /* (step C3b) */
    gpu_pipeline          pso;
    gpu_pipeline          pso16;      /* the same into an FP16 target (HDR's scene target) */
    int                   failed;     /* the pipeline could not be made: stay off */
    gpu_texture           disp_tex;   /* RSX_AA_AT=present: copy of the display buffer (step C6) */
    u32                   disp_w, disp_h;
    D3D12_RESOURCE_STATES disp_st;
    u32                   fp[AA_FP_MAX];   /* RSX_AA_BEFORE_FP */
    int                   fp_n;
    unsigned              fp_gen;
    u32                   log_key[4];      /* the choice last logged */
    int                   log_valid, log_n;
    int                   present_logged;  /* the filter RSX_AA_AT=present's line named, 0 none yet */
} s_aa;

static void aa_before_fp_load(void)
{
    if (s_aa.fp_gen == ps3_env_gen) return;
    s_aa.fp_gen = ps3_env_gen;
    s_aa.fp_n = 0;
    const char* e = getenv("RSX_AA_BEFORE_FP");
    while (e && *e && s_aa.fp_n < AA_FP_MAX) {
        char* end;
        const unsigned long v = strtoul(e, &end, 16);
        if (end == e) break;
        s_aa.fp[s_aa.fp_n++] = (u32)v;
        e = *end == ',' ? end + 1 : end;
    }
}

/* The filter's root signature (t0 table, b0 = 4 constants, a bilinear clamp
 * static sampler) and pipeline (RGBA8, RGB written, no depth), made on first
 * use. */
static int aa_ready(void)
{
    if (s_aa.pso.v) return 1;
    if (s_aa.failed || !s_rc_dev || !GPU_VALID(s_rc_srv_pool)) return 0;
    s_aa.failed = 1;   /* until it is made */
    gpu_shader vb = sc_shader(GPU_SC_VS, rsx_fxaa_vs, sizeof rsx_fxaa_vs - 1, "rsx_fxaa_vs", NULL,
                              GPU_SC_NO_CACHE | GPU_SC_OPTIMIZE);
    gpu_shader pb = {0};
    if (GPU_VALID(vb))
        pb = sc_shader(GPU_SC_PS, rsx_fxaa_ps, sizeof rsx_fxaa_ps - 1, "rsx_fxaa_ps", NULL,
                       GPU_SC_NO_CACHE | GPU_SC_OPTIMIZE);
    if (!GPU_VALID(pb))
        printf("[AA] shader compile failed: %s\n", sc_msg("?"));
    else {
        /* The GPU layer's layout and pipelines (step C3b): the t0 table, b0 =
         * 4 constants, a linear clamp at s0. */
        gpu_layout_desc ld;
        memset(&ld, 0, sizeof ld);
        ld.nslots = 2;
        ld.slot[0] = lay_slot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
        ld.slot[1] = lay_slot(GPU_SLOT_CONST32, 0, 4, GPU_VIS_PIXEL);
        ld.nstatic = 1;
        ld.statics[0] = pass_sampler(GPU_FILTER(1, 1, 1, 0), 0);
        s_aa.layout = lay_make(&ld);
    }
    if (GPU_VALID(s_aa.layout)) {
        gpu_pipeline_desc pd;
        pass_desc(&pd, s_aa.layout, vb, pb, GPU_FORMAT_R8G8B8A8_UNORM,
                  GPU_WRITE_R | GPU_WRITE_G | GPU_WRITE_B, 1);
        s_aa.pso = rc_pipeline(&pd, NULL);
        pd.rt_format[0] = GPU_FORMAT_R16G16B16A16_FLOAT;   /* without it, FP16 targets go unfiltered */
        if (s_aa.pso.v) s_aa.pso16 = rc_pipeline(&pd, NULL);
    }
    sc_shader_drop(&vb);
    sc_shader_drop(&pb);
    if (!s_aa.pso.v) {
        printf("[AA] pipeline failed (0x%08lX): anti-aliasing stays off\n", (long)E_FAIL);
        return 0;
    }
    s_aa.failed = 0;
    return 1;
}

/* The one colour target rtv, no depth (OMSetRenderTargets(1, ..., NULL)). */
static void aa_target(gpu_view rtv)
{
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = rtv;
    gpu_cmd_pass_begin(s_rc_cmd, &pass);
}

/* One pass: src (a copy of the target in format fmt, RGBA8 or FP16, in
 * PIXEL_SHADER_RESOURCE) filtered into the target behind rtv, w x h, in
 * RENDER_TARGET. Binds its own root signature, pipeline, target, viewport and
 * topology; the shader-visible heap must be srv_heap. The backend waits for
 * the GPU before each batch, so the one descriptor at AA_SRV_SLOT is free to
 * rewrite. src is the copy's handle (step C4: the SRV pool's write). On the
 * frame's recorder (step C6): rtv is the target's view, the table the SRV
 * pool's at AA_SRV_SLOT. */
static void aa_draw(gpu_texture src, DXGI_FORMAT fmt, gpu_view rtv, u32 w, u32 h)
{
    gpu_cmd* const c = s_rc_cmd;
    srv_write(AA_SRV_SLOT, src, fmt, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
    const float k[4] = {1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h};
    const gpu_viewport vp = {0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    const gpu_rect sc = {0, 0, (int32_t)w, (int32_t)h};
    aa_target(rtv);
    gpu_cmd_set_viewport(c, &vp);
    gpu_cmd_set_scissor(c, &sc);
    gpu_cmd_set_layout(c, s_aa.layout);
    gpu_cmd_set_pipeline(c, fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? s_aa.pso16 : s_aa.pso);
    gpu_cmd_bind_table(c, 0, gpu_table_ref(s_rc_dev, s_rc_srv_pool, AA_SRV_SLOT));
    gpu_cmd_push_const(c, 1, 4, k, 0);
    gpu_cmd_set_topology(c, GPU_TOPO_TRIANGLELIST);
    gpu_cmd_draw(c, 3, 1, 0, 0);
}

/* SMAA 1x (RSX_AA=2): the three passes of rsx_smaa.h in place of aa_draw's
 * one. The lookup textures (AreaTex, SearchTex) are uploaded once; the edges
 * (R8G8) and weights (RGBA8) targets follow the size asked for. Descriptors,
 * all written again each run: SMAA_SRV_SLOT + 0 the colour, + 1 the weights,
 * + 2 the edges, + 3 AreaTex, + 4 SearchTex. A pass's t0-t2 is a window of
 * three of them: the edge and blend passes' starts at + 0, the weight pass's
 * at + 2. */
#include "rsx_smaa.h"
#include "rsx_smaa_src.h"
#include "../../third_party/smaa/AreaTex.h"
#include "../../third_party/smaa/SearchTex.h"
#define SMAA_SRV_SLOT 22   /* SRV heap slots 22-26, after AA_SRV_SLOT */

static struct {
    gpu_layout            layout;      /* (step C3b) */
    gpu_pipeline          pso[3];      /* edges, weights, blend into RGBA8 */
    gpu_pipeline          blend16;     /* the blend pass into FP16 (HDR's scene target) */
    int                   failed;      /* could not be made: FXAA runs instead */
    /* (The edges' and weights' RTVs are their views, edges_rtv and
     * weights_rtv: step C6, no RTV heap.) */
    gpu_texture           area_tex;    /* AreaTex, R8G8 160x560: the GPU layer's (step C6) */
    gpu_texture           search_tex;  /* SearchTex, R8 64x16: the GPU layer's (step C6) */
    int                   lut_loaded;  /* their upload is recorded */
    gpu_texture           edges_tex;   /* the GPU layer's (step C6) */
    gpu_view              edges_rtv;   /* its RTV (today's NULL description) */
    gpu_texture           weights_tex; /* the GPU layer's (step C6) */
    gpu_view              weights_rtv;
    D3D12_RESOURCE_STATES edges_st, weights_st;
    u32                   w, h;        /* of edges and weights */
} s_smaa;

/* On the frame's recorder (step C6). */
static void smaa_state(gpu_texture t, D3D12_RESOURCE_STATES* st, D3D12_RESOURCE_STATES to)
{
    if (*st == to) return;
    gpu_cmd_barrier(s_rc_cmd, t, (gpu_state)*st, (gpu_state)to);
    *st = to;
}

/* The shaders, root signature, pipelines and lookup textures, made on first
 * use. */
static int smaa_ready(void)
{
    if (s_smaa.pso[2].v) return 1;
    if (s_smaa.failed || !s_rc_dev || !GPU_VALID(s_rc_srv_pool)) return 0;
    s_smaa.failed = 1;   /* until it is made */
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
    static const char* const entry[6] = {
        "smaa_edge_vs", "smaa_edge_ps", "smaa_weight_vs", "smaa_weight_ps", "smaa_blend_vs", "smaa_blend_ps",
    };
    const size_t nh = sizeof rsx_smaa_head - 1, ns = sizeof rsx_smaa_src - 1, nt = sizeof rsx_smaa_tail - 1;
    char* text = (char*)malloc(nh + ns + nt);
    gpu_shader code[6] = {{0}};
    HRESULT hr = text ? S_OK : E_OUTOFMEMORY;
    if (text) {
        memcpy(text, rsx_smaa_head, nh);
        memcpy(text + nh, rsx_smaa_src, ns);
        memcpy(text + nh + ns, rsx_smaa_tail, nt);
    }
    for (int i = 0; i < 6 && SUCCEEDED(hr); i++) {
        code[i] = sc_shader((i & 1) ? GPU_SC_PS : GPU_SC_VS, text, nh + ns + nt, "rsx_smaa", entry[i],
                            GPU_SC_NO_CACHE | GPU_SC_OPTIMIZE);
        if (!GPU_VALID(code[i])) {
            hr = E_FAIL;
            printf("[AA] SMAA shader %s failed to compile: %s\n", entry[i], sc_msg("?"));
        }
    }
    free(text);
    if (SUCCEEDED(hr)) {
        /* The GPU layer's layout and pipelines (step C3b): a t0-t2 table, b0
         * = 4 constants (the vertex shaders use them too), a linear clamp at
         * s0 and a point clamp at s1. */
        gpu_layout_desc ld;
        memset(&ld, 0, sizeof ld);
        ld.nslots = 2;
        ld.slot[0] = lay_slot(GPU_SLOT_TABLE_TEX, 0, 3, GPU_VIS_PIXEL);
        ld.slot[1] = lay_slot(GPU_SLOT_CONST32, 0, 4, GPU_VIS_ALL);
        ld.nstatic = 2;
        ld.statics[0] = pass_sampler(GPU_FILTER(1, 1, 0, 0), 0);
        ld.statics[1] = pass_sampler(GPU_FILTER(0, 0, 0, 0), 1);
        s_smaa.layout = lay_make(&ld);
        if (!GPU_VALID(s_smaa.layout)) hr = E_FAIL;
    }
    for (int p = 0; p < 4 && SUCCEEDED(hr); p++) {   /* edges, weights, blend, blend16 */
        const int s = p < 3 ? p : 2;
        static const gpu_format fmt[4] = {GPU_FORMAT_R8G8_UNORM, GPU_FORMAT_R8G8B8A8_UNORM,
                                          GPU_FORMAT_R8G8B8A8_UNORM, GPU_FORMAT_R16G16B16A16_FLOAT};
        gpu_pipeline_desc pd;
        pass_desc(&pd, s_smaa.layout, code[2 * s], code[2 * s + 1], fmt[p],
                  s == 2 ? (u8)(GPU_WRITE_R | GPU_WRITE_G | GPU_WRITE_B) : (u8)GPU_WRITE_ALL, 1);
        gpu_pipeline* const out = p < 3 ? &s_smaa.pso[p] : &s_smaa.blend16;
        *out = rc_pipeline(&pd, NULL);
        hr = out->v ? S_OK : E_FAIL;
        if (p == 3) hr = S_OK;   /* without it, FP16 targets get FXAA */
    }
    /* (No RTV heap: the targets' RTVs are views, step C6.) The lookup
     * textures are the GPU layer's (step C6): today's committed textures with
     * no flags, in COPY_DEST, sampled by smaa_draw. */
    for (int k = 0; k < 2 && SUCCEEDED(hr); k++) {
        gpu_texture_desc td;
        memset(&td, 0, sizeof td);
        td.width  = k ? SEARCHTEX_WIDTH : AREATEX_WIDTH;
        td.height = k ? SEARCHTEX_HEIGHT : AREATEX_HEIGHT;
        td.layers = 1; td.mips = 1;
        td.format = k ? GPU_FORMAT_R8_UNORM : GPU_FORMAT_R8G8_UNORM;
        td.samples = 1;
        td.use = GPU_USE_SAMPLED | GPU_USE_COPY_DST;
        td.state = GPU_STATE_COPY_DEST;
        gpu_texture* const t = k ? &s_smaa.search_tex : &s_smaa.area_tex;
        *t = gpu_texture_create(s_rc_dev, &td);
        if (!GPU_VALID(*t)) hr = E_FAIL;
    }
    for (int i = 0; i < 6; i++) sc_shader_drop(&code[i]);
    if (FAILED(hr) || !s_smaa.pso[2].v) {
        printf("[AA] SMAA pipeline failed (0x%08lX): FXAA runs instead\n", (long)hr);
        return 0;
    }
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    printf("[AA] SMAA ready (%.0f ms to compile)\n", (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart);
    s_smaa.failed = 0;
    return 1;
}

/* Records the lookup textures' upload, once: from the texture staging
 * buffer, rows padded to D3D12's 256-byte pitch. The staging is the GPU
 * layer's buffer (tex_stage_alloc_buf), the copies and the barrier are on the
 * frame's recorder (step C6). */
static int smaa_luts_load(void)
{
    if (s_smaa.lut_loaded) return 1;
    const u32 ap = (AREATEX_PITCH + 255u) & ~255u, sp = (SEARCHTEX_PITCH + 255u) & ~255u;
    const u64 s_at = ((u64)ap * AREATEX_HEIGHT + 511u) & ~511ull;   /* a footprint's offset is 512-aligned */
    gpu_buffer up = {0};
    u64 at = 0;
    u8* m = tex_stage_alloc_buf(s_at + (u64)sp * SEARCHTEX_HEIGHT, &up, &at);
    if (!m) return 0;
    for (u32 y = 0; y < AREATEX_HEIGHT; y++)
        memcpy(m + (u64)y * ap, areaTexBytes + (size_t)y * AREATEX_PITCH, AREATEX_PITCH);
    for (u32 y = 0; y < SEARCHTEX_HEIGHT; y++)
        memcpy(m + s_at + (u64)y * sp, searchTexBytes + (size_t)y * SEARCHTEX_PITCH, SEARCHTEX_PITCH);
    gpu_cmd* const c = s_rc_cmd;
    gpu_barrier b[2];
    memset(b, 0, sizeof b);
    for (int k = 0; k < 2; k++) {
        gpu_footprint fp;
        memset(&fp, 0, sizeof fp);
        fp.offset    = at + (k ? s_at : 0);
        fp.format    = k ? GPU_FORMAT_R8_UNORM : GPU_FORMAT_R8G8_UNORM;
        fp.width     = k ? SEARCHTEX_WIDTH : AREATEX_WIDTH;
        fp.height    = k ? SEARCHTEX_HEIGHT : AREATEX_HEIGHT;
        fp.depth     = 1;
        fp.row_pitch = k ? sp : ap;
        const gpu_texture dst = k ? s_smaa.search_tex : s_smaa.area_tex;
        gpu_cmd_copy_buf_to_tex(c, dst, 0, 0, 0, 0, up, &fp);
        b[k].type    = GPU_BARRIER_TRANSITION;
        b[k].texture = dst;
        b[k].sub     = GPU_SUB_ALL;
        b[k].before  = GPU_STATE_COPY_DEST;
        b[k].after   = GPU_STATE_PIXEL_SHADER_RESOURCE;
    }
    gpu_cmd_barrier_n(c, 2, b);
    s_smaa.lut_loaded = 1;
    return 1;
}

/* The edges and weights targets at w x h, made again when the size changes.
 * The GPU layer's (step C6): today's committed textures that allow a render
 * target, in RENDER_TARGET, cleared to zero fastest, with their RTVs as
 * views. A pair dropped while a frame is recorded waits in the grave
 * (rc_grave_tex) and goes once the GPU is past that frame, as today's
 * off_rt_defer_release let it go. */
static int smaa_targets(u32 w, u32 h)
{
    if (GPU_VALID(s_smaa.edges_tex) && GPU_VALID(s_smaa.weights_tex) && s_smaa.w == w && s_smaa.h == h)
        return 1;
    rc_grave_tex(&s_smaa.edges_tex);     /* the open list may still use them (their views go with them) */
    rc_grave_tex(&s_smaa.weights_tex);
    s_smaa.edges_rtv.id = s_smaa.weights_rtv.id = 0;
    for (int k = 0; k < 2; k++) {
        gpu_texture_desc td;
        memset(&td, 0, sizeof td);
        td.width = w; td.height = h; td.layers = 1; td.mips = 1;
        td.format = k ? GPU_FORMAT_R8G8B8A8_UNORM : GPU_FORMAT_R8G8_UNORM;
        td.samples = 1;
        td.use = GPU_USE_RT | GPU_USE_SAMPLED;
        td.state = GPU_STATE_RENDER_TARGET;
        td.clear_format = td.format;   /* (colour 0) */
        gpu_texture* const t = k ? &s_smaa.weights_tex : &s_smaa.edges_tex;
        *t = gpu_texture_create(s_rc_dev, &td);
        if (!GPU_VALID(*t)) return 0;   /* the next run tries again */
        gpu_view_desc rv;   /* today's NULL description */
        memset(&rv, 0, sizeof rv);
        rv.texture = *t;
        rv.kind = GPU_VIEW_RTV;
        *(k ? &s_smaa.weights_rtv : &s_smaa.edges_rtv) = gpu_view_get(s_rc_dev, &rv);
    }
    s_smaa.edges_st = s_smaa.weights_st = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_smaa.w = w; s_smaa.h = h;
    return 1;
}

/* SMAA's three passes: src (as aa_draw's) into the target behind rtv, w x h,
 * in RENDER_TARGET. Needs smaa_targets(w, h) and smaa_luts_load() first. On
 * the frame's recorder (step C6): the tables are the SRV pool's windows at
 * SMAA_SRV_SLOT and SMAA_SRV_SLOT + 2 (its regions of three). */
static void smaa_draw(gpu_texture src, DXGI_FORMAT fmt, gpu_view rtv, u32 w, u32 h)
{
    gpu_cmd* const c = s_rc_cmd;
    const UINT map = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_write(SMAA_SRV_SLOT + 0, src, fmt, map);
    srv_write(SMAA_SRV_SLOT + 1, s_smaa.weights_tex, DXGI_FORMAT_R8G8B8A8_UNORM, map);
    srv_write(SMAA_SRV_SLOT + 2, s_smaa.edges_tex, DXGI_FORMAT_R8G8_UNORM, map);
    srv_write(SMAA_SRV_SLOT + 3, s_smaa.area_tex, DXGI_FORMAT_R8G8_UNORM, map);
    srv_write(SMAA_SRV_SLOT + 4, s_smaa.search_tex, DXGI_FORMAT_R8_UNORM, map);
    const gpu_table g0 = gpu_table_ref(s_rc_dev, s_rc_srv_pool, SMAA_SRV_SLOT);
    const gpu_table g2 = gpu_table_ref(s_rc_dev, s_rc_srv_pool, SMAA_SRV_SLOT + 2);
    const float k[4] = {1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h};
    const gpu_viewport vp = {0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    const gpu_rect sc = {0, 0, (int32_t)w, (int32_t)h};
    static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    gpu_cmd_set_viewport(c, &vp);
    gpu_cmd_set_scissor(c, &sc);
    gpu_cmd_set_layout(c, s_smaa.layout);
    gpu_cmd_push_const(c, 1, 4, k, 0);
    gpu_cmd_set_topology(c, GPU_TOPO_TRIANGLELIST);
    /* 1. Edges, into a cleared target: the pass discards pixels without one. */
    smaa_state(s_smaa.edges_tex, &s_smaa.edges_st, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gpu_cmd_clear_color(c, s_smaa.edges_rtv, zero, NULL);
    aa_target(s_smaa.edges_rtv);
    gpu_cmd_set_pipeline(c, s_smaa.pso[0]);
    gpu_cmd_bind_table(c, 0, g0);
    gpu_cmd_draw(c, 3, 1, 0, 0);
    /* 2. Blending weights: every pixel is written. */
    smaa_state(s_smaa.edges_tex, &s_smaa.edges_st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    smaa_state(s_smaa.weights_tex, &s_smaa.weights_st, D3D12_RESOURCE_STATE_RENDER_TARGET);
    aa_target(s_smaa.weights_rtv);
    gpu_cmd_set_pipeline(c, s_smaa.pso[1]);
    gpu_cmd_bind_table(c, 0, g2);
    gpu_cmd_draw(c, 3, 1, 0, 0);
    /* 3. The colour blended along the edges, into the target. */
    smaa_state(s_smaa.weights_tex, &s_smaa.weights_st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    aa_target(rtv);
    gpu_cmd_set_pipeline(c, fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? s_smaa.blend16 : s_smaa.pso[2]);
    gpu_cmd_bind_table(c, 0, g0);
    gpu_cmd_draw(c, 3, 1, 0, 0);
}

/* The filter a w x h target in format fmt (RGBA8 or FP16) gets now: 2 SMAA
 * (RSX_AA=2), 1 FXAA (RSX_AA=1, or SMAA could not be had), 0 none. What it
 * needs that is missing is made here, before the caller's snapshot. */
static int aa_filter_for(u32 w, u32 h, DXGI_FORMAT fmt)
{
    const int fp16 = fmt == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (aa_mode() == 2 && smaa_ready() && (!fp16 || s_smaa.blend16.v) && smaa_targets(w, h) && smaa_luts_load())
        return 2;
    if (!aa_ready() || (fp16 && !s_aa.pso16.v)) return 0;
    return 1;
}

static void aa_filter(int f, gpu_texture src, DXGI_FORMAT fmt, gpu_view rtv, u32 w, u32 h)
{
    if (f == 2) smaa_draw(src, fmt, rtv, w, h);
    else aa_draw(src, fmt, rtv, w, h);
}

/* The in-frame pass on offscreen target t. The caller puts its own state
 * back afterwards. (Step C6: into the target's view, r->rtv.) */
static int aa_run_rt(int t)
{
    OffRT* r = &s_d3d.off_rt[t];
    if (!GPU_VALID(r->tex) || (r->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM && r->dxgi != DXGI_FORMAT_R16G16B16A16_FLOAT))
        return 0;
    const int f = aa_filter_for(r->hw, r->hh, (DXGI_FORMAT)r->dxgi);
    if (!f) return 0;
    if (!GPU_VALID(off_rt_snapshot(t))) return 0;
    off_rt_transition(t, D3D12_RESOURCE_STATE_RENDER_TARGET);
    aa_filter(f, r->snap_tex, (DXGI_FORMAT)r->dxgi, r->rtv, r->hw, r->hh);
    return 1;
}

/* RSX_AA_AT=present: display buffer fi (in RENDER_TARGET, its view
 * s_rc_rt_rtv[fi]), finished, filtered from a copy of it. The copy is the
 * GPU layer's (step C6): today's committed RGBA8 texture with no flags, made
 * in COPY_DEST; one dropped while a frame is recorded waits in the grave,
 * as today's off_rt_defer_release. The barriers, the copy and the
 * pools bound are on the frame's recorder. */
static void aa_run_display(u32 fi)
{
    gpu_cmd* const c = s_rc_cmd;
    const u32 w = s_d3d.host_w, h = s_d3d.host_h;
    const int f = aa_filter_for(w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (!f) return;
    if (GPU_VALID(s_aa.disp_tex) && (s_aa.disp_w != w || s_aa.disp_h != h))
        rc_grave_tex(&s_aa.disp_tex);   /* the open list may still use it */
    if (!GPU_VALID(s_aa.disp_tex)) {
        gpu_texture_desc td;
        memset(&td, 0, sizeof td);
        td.width = w; td.height = h; td.layers = 1; td.mips = 1;
        td.format = GPU_FORMAT_R8G8B8A8_UNORM; td.samples = 1;
        td.use = GPU_USE_SAMPLED | GPU_USE_COPY_DST;
        td.state = GPU_STATE_COPY_DEST;
        s_aa.disp_tex = gpu_texture_create(s_rc_dev, &td);
        if (!GPU_VALID(s_aa.disp_tex)) return;
        s_aa.disp_w = w; s_aa.disp_h = h;
        s_aa.disp_st = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    gpu_barrier b[2];
    memset(b, 0, sizeof b);
    b[0].type    = GPU_BARRIER_TRANSITION;
    b[0].texture = s_rc_rt_tex[fi];
    b[0].sub     = GPU_SUB_ALL;
    b[0].before  = GPU_STATE_RENDER_TARGET;
    b[0].after   = GPU_STATE_COPY_SOURCE;
    b[1] = b[0];
    b[1].texture = s_aa.disp_tex;
    b[1].before  = (gpu_state)s_aa.disp_st;
    b[1].after   = GPU_STATE_COPY_DEST;
    gpu_cmd_barrier_n(c, s_aa.disp_st != D3D12_RESOURCE_STATE_COPY_DEST ? 2 : 1, b);
    gpu_cmd_copy_texture_whole(c, s_aa.disp_tex, s_rc_rt_tex[fi]);
    b[0].before = GPU_STATE_COPY_SOURCE;
    b[0].after  = GPU_STATE_RENDER_TARGET;
    b[1].before = GPU_STATE_COPY_DEST;
    b[1].after  = GPU_STATE_PIXEL_SHADER_RESOURCE;
    gpu_cmd_barrier_n(c, 2, b);
    s_aa.disp_st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    gpu_cmd_set_table_pools(c, s_rc_srv_pool, s_rc_samp_pool);   /* the sampler pool when there is one */
    aa_filter(f, s_aa.disp_tex, DXGI_FORMAT_R8G8B8A8_UNORM, s_rc_rt_rtv[fi], w, h);
}

/* Where this batch's pass goes (see the top of this section): returns T's
 * off_rt slot and sets *before to the op the pass runs before, or returns -1
 * and sets *why. Needs the pre-pass's tex_rt / tex_z. */
static int aa_pick(u32* before, int* why)
{
    const u32 n = s_d3d.draw_count < MAX_DRAWS ? s_d3d.draw_count : MAX_DRAWS;
    int t = -1;
    u32 dc = 0;
    for (u32 d = n; d-- > 0; ) {
        const D3D12DrawRecord* dr = &s_d3d.draws[d];
        if (!dr->is_vp || dr->is_clear || dr->rt_off || !(dr->cmask & 7u)) continue;
        u64 best = 0;
        for (int u = 0; u < RSX_FP_UNITS; u++) {
            const int s = dr->tex_rt[u];
            if (s < 0) continue;
            const u64 a = (u64)s_d3d.off_rt[s].w * s_d3d.off_rt[s].h;
            if (a > best) { best = a; t = s; }
        }
        if (t >= 0) { dc = d; break; }
    }
    if (t < 0) { *why = AA_NO_COPY; return -1; }
    const OffRT* T = &s_d3d.off_rt[t];
    if ((u64)T->w * T->h * 4u < (u64)s_d3d.width * s_d3d.height) { *why = AA_SMALL; return -1; }
    if (T->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM && T->dxgi != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        *why = AA_FORMAT; return -1;
    }
    aa_before_fp_load();
    u32 scene = 0;
    int post = -1, named = -1;
    for (u32 d = 0; d < dc; d++) {
        const D3D12DrawRecord* dr = &s_d3d.draws[d];
        if (!dr->is_vp || dr->is_clear) continue;
        for (int k = 0; named < 0 && k < s_aa.fp_n; k++)
            if (dr->fp_addr == s_aa.fp[k]) named = (int)d;
        if (dr->rt_off != T->off) continue;
        if (dr->depth & 1u) { scene++; continue; }
        if (dr->blend || !(dr->cmask & 7u)) continue;
        for (int u = 0; u < RSX_FP_UNITS; u++)
            if (dr->tex_rt[u] >= 0 || dr->tex_z[u] >= 0) { post = (int)d; break; }
    }
    if (scene < AA_MIN_SCENE) { *why = AA_NO_SCENE; return -1; }
    if (named >= 0) { *before = (u32)named; return t; }
    if (post < 0) { *why = AA_NO_POST; return -1; }
    *before = (u32)post + 1;
    return t;
}

/* The choice, logged when it changes (RSX_AA_LOG=1) or every batch (2). */
static void aa_log(int t, u32 before, int why)
{
    const int lv = knob_aa_log();
    if (lv <= 0) return;
    const u32 n = s_d3d.draw_count < MAX_DRAWS ? s_d3d.draw_count : MAX_DRAWS;
    const u32 fa = t >= 0 && before > 0 && before <= n ? s_d3d.draws[before - 1].fp_addr : 0;
    const u32 fb = t >= 0 && before < n ? s_d3d.draws[before].fp_addr : 0;
    const u32 key[4] = {t >= 0 ? s_d3d.off_rt[t].off : 0u, fa, fb, (u32)why | (u32)aa_mode() << 8};
    if (lv == 1) {
        if (s_aa.log_valid && !memcmp(key, s_aa.log_key, sizeof key)) return;
        memcpy(s_aa.log_key, key, sizeof key);
        s_aa.log_valid = 1;
        if (s_aa.log_n > AA_LOG_MAX) return;
        if (s_aa.log_n++ == AA_LOG_MAX) {
            printf("[AA] further changes not logged (RSX_AA_LOG=2 logs every batch)\n");
            return;
        }
    }
    if (t >= 0)
        printf("[AA] %s on T=0x%08X %ux%u before op %u of %u (after fp 0x%X, before fp 0x%X)\n",
               aa_mode() == 2 ? "SMAA" : "FXAA",
               s_d3d.off_rt[t].off, s_d3d.off_rt[t].w, s_d3d.off_rt[t].h, before, n, fa, fb);
    else
        printf("[AA] no pass: %s\n", s_aa_why[why]);
}

