/* ---------------------------------------------------------------------------
 * HDR output (RSX_HDR)
 *
 * RSX_HDR=1 (live; default 0): while the display the window is on has HDR on
 * in Windows, the swap chain is FP16 scRGB (linear, 1.0 = 80 nits) and the
 * blit maps the game's picture into it: decoded with gamma 2.2
 * (RSX_HDR_GAMMA=1: the sRGB curve, as Windows shows SDR windows), white at
 * RSX_HDR_WHITE nits (0, the default: Windows' SDR white level on that
 * display), and anything brighter rolled off towards RSX_HDR_PEAK nits (0: the
 * display's peak). Otherwise the swap chain is RGBA8 and the blit is the one
 * it always was.
 *
 * The game's own highlights: RSX_HDR_SCENE_FP=<fp>[:<limit>][,...] names the
 * program that clamps the finished scene to white (Demon's Souls: its
 * composite, which doubles the half-brightness scene and clamps it before
 * adding bloom; the port names it). While HDR is on and one is named:
 *   - T, the scene target (the largest target the frame's display copy
 *     samples, as RSX_AA finds it), and each target a draw copies T into
 *     after that program, are made FP16 (rt_target_format). They still go back to
 *     guest memory in the format the title set (OffRT.gdxgi).
 *   - That program, drawing into T, clamps at <limit> instead of 1 (dr_hdrv;
 *     with no limit, not at all).
 *   - A draw into any other target reads T and the copies clamped to 1, as
 *     their own format held them (Demon's Souls' display copy looks T up in a
 *     table whose sampler wraps). Draws into them read them whole.
 *   - The blit adds what T holds above 1 to the display copy's picture, which
 *     read T clamped to 1.
 * The targets come from the frame's draws (guest state). What one batch finds
 * applies from the next, as formats are chosen at a batch's start.
 *
 * RSX_HDR_SNAP=<file> (live, once per value): the swap chain buffer after the
 * blit, raw (an "HDRP w h" line, then RGBA halves), with a summary line.
 * -------------------------------------------------------------------------*/
static struct {
    int   disp_hdr;              /* the window's display has HDR on */
    float disp_peak, disp_white; /* its peak, Windows' SDR white level there; nits, 0 = not known */
    int   active;                /* the swap chain is FP16 scRGB */
    int   failed;                /* refused there: RGBA8 until the display is asked again */
    int   stage_b;               /* active, and a scene program is named */
    u32   t_off, x_off[4];       /* the targets promoted in this batch */
    u32   next_t, next_x[4];     /* what hdr_mark_scan found, for the next batch */
    int   frame_t;               /* T's slot for this batch's blit, -1: none */
} s_hdr = { .frame_t = -1 };

#include "rsxc_display.h"

static int knob_hdr(void)       { static int v = -1; PS3_KNOB(v, ps3_env_int("RSX_HDR", 0)); return v; }
static int knob_hdr_white(void) { static int v = -1; PS3_KNOB(v, ps3_env_int("RSX_HDR_WHITE", 0)); return v; }
static int knob_hdr_peak(void)  { static int v = -1; PS3_KNOB(v, ps3_env_int("RSX_HDR_PEAK", 0)); return v; }
static int knob_hdr_gamma(void) { static int v = -1; PS3_KNOB(v, ps3_env_int("RSX_HDR_GAMMA", 0)); return v; }

/* Paper white and the peak, in nits, as the blit uses them. */
static float hdr_white_nits(void)
{
    const int k = knob_hdr_white();
    if (k > 0) return (float)k;
    return s_hdr.disp_white >= 80.0f ? s_hdr.disp_white : 200.0f;
}
static float hdr_peak_nits(void)
{
    const int k = knob_hdr_peak();
    if (k > 0) return (float)k;
    return s_hdr.disp_peak >= 100.0f ? s_hdr.disp_peak : 1000.0f;
}

#ifdef _WIN32
/* What the display the window is on reports (step C8b: the system's
 * display code, rsxc_display_win.c, which reads DXGI's outputs for every
 * backend on Windows; with no window, as on a headless backend, no HDR). */
static void hdr_display_query(void)
{
    rsxc_display_hdr dh;
    rsxc_display_hdr_query(s_d3d.hwnd, &dh);
    s_hdr.disp_hdr = dh.hdr;
    s_hdr.disp_peak = dh.peak;
    s_hdr.disp_white = dh.white;
    s_hdr.failed = 0;
}

/* Between frames, with RSX_HDR on: asks again when the window moved to
 * another display, the display setup changed (HDR switched in Windows, a
 * display added), or the window was activated (the SDR white level may
 * have moved meanwhile). */
static void hdr_display_check(void)
{
    if (!InterlockedExchange(&s_hdr_requery, 0) && rsxc_display_hdr_current(s_d3d.hwnd))
        return;
    hdr_display_query();
    printf("[D3D12] HDR: the window's display has HDR %s (peak %.0f nits, SDR white %.0f nits)\n",
           s_hdr.disp_hdr ? "on" : "off", s_hdr.disp_peak, s_hdr.disp_white);
}

/* The swap chain's colour space: scRGB for FP16 buffers (hdr), sRGB for RGBA8
 * ones. 0 if it was refused. */
static int swap_color_space(int hdr)
{
    return gpu_swap_hdr(s_rc_dev, s_rc_swap, hdr ? GPU_COLOR_SCRGB : GPU_COLOR_SRGB, NULL);
}
#else
/* Off Windows no display reports HDR here: the swap chain stays RGBA8. */
static void hdr_display_check(void) { s_hdr.disp_hdr = 0; }
static int swap_color_space(int hdr) { return !hdr; }
#endif

static int hdr_blit_ready(void);

/* The swap chain's format from the next frame: FP16 scRGB while RSX_HDR is on
 * and the display has HDR on, unless that was refused there. */
static int hdr_want(void)
{
    if (!knob_hdr()) return 0;
    hdr_display_check();
    return s_hdr.disp_hdr && !s_hdr.failed && hdr_blit_ready();
}

/* The swap chain's buffers and their RTVs (init and after a resize). The
 * buffers are the swap chain's images (step C8a: by their handles only). The
 * RTVs are views of the images (step C5a, today's NULL description), which
 * a resize drops with the images, so they are made again here. (Step C6: the
 * images by index are gpu.h's gpu_swap_image, the bridge's no longer.) */
static int swap_buffers_get(void)
{
    memset(s_rc_swap_rtv, 0, sizeof s_rc_swap_rtv);
    /* Every image the swap chain has (step VC1: FRAME_COUNT on D3D12, as
     * many as the driver made on Vulkan). */
    s_rc_swap_n = gpu_swap_image_count(s_rc_dev, s_rc_swap);
    if (s_rc_swap_n > RC_SWAP_MAX) s_rc_swap_n = RC_SWAP_MAX;
    if (s_rc_swap_n < FRAME_COUNT) return -1;
    for (u32 i = 0; i < s_rc_swap_n; i++) {
        s_rc_swap_image[i] = gpu_swap_image(s_rc_dev, s_rc_swap, i);
        if (!GPU_VALID(s_rc_swap_image[i])) return -1;
        s_rc_swap_rtv[i] = rc_target_view(s_rc_swap_image[i], GPU_VIEW_RTV, 0, 0);
    }
    return 0;
}

/* RSX_OUTFILTER (live): the filter blit_to_window scales the game's frame
 * into the window with. 0, the default, is the single bilinear tap it has
 * always taken. 1 filters each axis by its ratio: where the frame is bigger
 * than the viewport (a render resolution above the window's), an area filter,
 * so every window pixel averages the texels under it and nothing shimmers at
 * ratios other than 1:1 and 2:1; where it is smaller, a Catmull-Rom bicubic,
 * sharper than bilinear. The pipeline (rsx_outfilter.h) is made on first use;
 * if that fails, the bilinear blit stays. */
#include "rsx_outfilter.h"
static int knob_outfilter(void) { static int v = -1; PS3_KNOB(v, ps3_env_int("RSX_OUTFILTER", 0)); return v; }
static struct { gpu_layout layout; gpu_pipeline pso; int failed; } s_of;

static int of_ready(void)
{
    if (s_of.pso.v) return 1;
    if (s_of.failed || !s_rc_dev) return 0;
    s_of.failed = 1;   /* until it is made */
    /* The GPU layer's layout (step C3b): the t0 table, b0 = 8 constants.
     * The shaders are compiled for it (step VC1). */
    gpu_layout_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.nslots = 2;
    ld.slot[0] = lay_slot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
    ld.slot[1] = lay_slot(GPU_SLOT_CONST32, 0, 8, GPU_VIS_PIXEL);
    gpu_shader vb = sc_shader_for(&ld, GPU_SC_VS, rsx_outfilter_vs, sizeof rsx_outfilter_vs - 1, "rsx_outfilter_vs",
                                  NULL, GPU_SC_NO_CACHE | GPU_SC_OPTIMIZE);
    gpu_shader pb = {0};
    if (GPU_VALID(vb))
        pb = sc_shader_for(&ld, GPU_SC_PS, rsx_outfilter_ps, sizeof rsx_outfilter_ps - 1, "rsx_outfilter_ps",
                           NULL, GPU_SC_NO_CACHE | GPU_SC_OPTIMIZE);
    if (!GPU_VALID(pb))
        printf("[D3D12] RSX_OUTFILTER shader compile failed: %s\n", sc_msg("?"));
    else
        s_of.layout = lay_make(&ld);
    if (GPU_VALID(s_of.layout)) {
        gpu_pipeline_desc pd;
        pass_desc(&pd, s_of.layout, vb, pb, GPU_FORMAT_R8G8B8A8_UNORM, GPU_WRITE_ALL, 1);
        s_of.pso = rc_pipeline(&pd, NULL);
    }
    sc_shader_drop(&vb);
    sc_shader_drop(&pb);
    if (!s_of.pso.v) {
        printf("[D3D12] RSX_OUTFILTER pipeline failed (0x%08lX): the bilinear blit stays\n", (long)E_FAIL);
        return 0;
    }
    s_of.failed = 0;
    return 1;
}

/* The scale pass: a full-screen triangle sampling the game's frame with
 * linear filtering, into a viewport that keeps its aspect ratio. */
static void blit_srvs_write(void);
static int blit_init(void)
{
    static const char vs[] =
        "struct O { float4 p : SV_POSITION; float2 uv : TEXCOORD; };\n"
        "O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";
    static const char ps[] =
        "Texture2D t : register(t0); SamplerState s : register(s0);\n"
        "float4 main(float4 p : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {\n"
        "  return float4(t.Sample(s, uv).rgb, 1); }\n";
    /* The GPU layer's layout and pipeline (step C3b): the t0 table and a
     * linear clamp at s0. The shaders are compiled for it (step VC1). */
    gpu_layout_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.nslots = 1;
    ld.slot[0] = lay_slot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
    ld.nstatic = 1;
    ld.statics[0] = pass_sampler(GPU_FILTER(1, 1, 1, 0), 0);
    gpu_shader vb = sc_shader_for(&ld, GPU_SC_VS, vs, sizeof vs - 1, "blit_vs", NULL, GPU_SC_NO_CACHE);
    gpu_shader pb = {0};
    if (GPU_VALID(vb)) pb = sc_shader_for(&ld, GPU_SC_PS, ps, sizeof ps - 1, "blit_ps", NULL, GPU_SC_NO_CACHE);
    if (!GPU_VALID(pb)) {
        printf("[D3D12] blit shader compile failed: %s\n", sc_msg("?"));
        sc_shader_drop(&vb);
        return -1;
    }
    s_d3d.blit_layout = lay_make(&ld);
    HRESULT hr = GPU_VALID(s_d3d.blit_layout) ? S_OK : E_FAIL;
    if (SUCCEEDED(hr)) {
        gpu_pipeline_desc pd;
        pass_desc(&pd, s_d3d.blit_layout, vb, pb, GPU_FORMAT_R8G8B8A8_UNORM, GPU_WRITE_ALL, 1);
        s_d3d.blit_pso = rc_pipeline(&pd, NULL);
        hr = s_d3d.blit_pso.v ? S_OK : E_FAIL;
    }
    if (SUCCEEDED(hr)) {   /* the GPU layer's table pool: frame fi's table at 2 fi (step C4) */
        gpu_table_pool_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.kind = GPU_TABLE_TEX;
        pd.count = FRAME_COUNT * 2;   /* the frame and T (blit_srvs_write) */
        pd.nregions = 1;
        pd.region[0].first = 0; pd.region[0].tables = FRAME_COUNT; pd.region[0].size = 2;
        s_rc_blit_pool = gpu_table_pool_create(s_rc_dev, &pd);
        if (!GPU_VALID(s_rc_blit_pool)) hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) blit_srvs_write();
    sc_shader_drop(&vb);
    sc_shader_drop(&pb);
    if (FAILED(hr)) printf("[D3D12] blit pipeline failed (0x%08lX)\n", (long)hr);
    return FAILED(hr) ? -1 : 0;
}

/* The blit into an FP16 scRGB swap chain (see HDR output): the bilinear one,
 * and the Sharp filter (RSX_OUTFILTER=1) with the same mapping after it. Made
 * on first use; if that fails, HDR stays off (hdr_want). t0 is the game's
 * frame and t1 T (or nothing), in one table; b0 is the Sharp filter's
 * constants and b1 the mapping's (blit_to_window). */
#define HDR_MAP_HLSL \
    "cbuffer HDR : register(b1) { float hw_white; float hw_ratio; float hw_knee; float hw_srgb;\n" \
    "                             float hw_use_t; float3 hw_pad; };\n" \
    "Texture2D<float4> hw_t : register(t1);\n" \
    "SamplerState hw_s : register(s0);\n" \
    "// The frame's colour (with what T holds above 1) to scRGB: decoded, white at\n" \
    "// paper white, and brighter values rolled off towards the peak above a knee.\n" \
    "float3 hw_map(float3 c, float2 uv) {\n" \
    "    if (hw_use_t > 0.5) c += max(hw_t.Sample(hw_s, uv).rgb - 1.0, 0.0);\n" \
    "    c = max(c, 0.0);\n" \
    "    float3 l = hw_srgb > 0.5 ? (c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)) : pow(c, 2.2);\n" \
    "    float m = max(l.r, max(l.g, l.b));\n" \
    "    if (m > hw_knee) {\n" \
    "        float r = hw_ratio - hw_knee;\n" \
    "        float m2 = r > 0.0 ? hw_knee + r * (1.0 - exp((hw_knee - m) / r)) : hw_ratio;\n" \
    "        l *= m2 / m;\n" \
    "    }\n" \
    "    return l * hw_white;\n" \
    "}\n"
static struct { gpu_layout layout; gpu_pipeline pso[2]; int failed; } s_hdr_blit;

static int hdr_blit_ready(void)
{
    if (s_hdr_blit.pso[0].v && s_hdr_blit.pso[1].v) return 1;
    if (s_hdr_blit.failed || !s_rc_dev) return 0;
    s_hdr_blit.failed = 1;   /* until it is made */
    static const char vs[] =
        "struct O { float4 p : SV_POSITION; float2 uv : TEXCOORD; };\n"
        "O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";
    static const char ps[] =
        "Texture2D t : register(t0);\n" HDR_MAP_HLSL
        "float4 main(float4 p : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {\n"
        "  return float4(hw_map(t.Sample(hw_s, uv).rgb, uv), 1); }\n";
    static const char sharp_tail[] =
        "#undef main\n" HDR_MAP_HLSL
        "float4 main(float4 pos : SV_Position) : SV_Target {\n"
        "  return float4(hw_map(of_main(pos).rgb, (pos.xy - vp_org) * scale / src_size), 1); }\n";
    static char sharp[sizeof rsx_outfilter_ps + sizeof sharp_tail + 64];
    snprintf(sharp, sizeof sharp, "#define main of_main\n%s\n%s", rsx_outfilter_ps, sharp_tail);
    /* The GPU layer's layout (step C3b): the t0-t1 table, b0 and b1 = 8
     * constants each, a linear clamp at s0. The shaders are compiled for it
     * (step VC1). */
    gpu_layout_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.nslots = 3;
    ld.slot[0] = lay_slot(GPU_SLOT_TABLE_TEX, 0, 2, GPU_VIS_PIXEL);
    for (int i = 1; i <= 2; i++) ld.slot[i] = lay_slot(GPU_SLOT_CONST32, (u8)(i - 1), 8, GPU_VIS_PIXEL);
    ld.nstatic = 1;
    ld.statics[0] = pass_sampler(GPU_FILTER(1, 1, 1, 0), 0);
    gpu_shader vb[2] = {{0}, {0}}, pb[2] = {{0}, {0}};
    vb[0] = sc_shader_for(&ld, GPU_SC_VS, vs, sizeof vs - 1, "hdr_blit_vs", NULL, GPU_SC_NO_CACHE);
    if (GPU_VALID(vb[0]))
        pb[0] = sc_shader_for(&ld, GPU_SC_PS, ps, sizeof ps - 1, "hdr_blit_ps", NULL, GPU_SC_NO_CACHE);
    if (GPU_VALID(pb[0]))
        vb[1] = sc_shader_for(&ld, GPU_SC_VS, rsx_outfilter_vs, sizeof rsx_outfilter_vs - 1, "rsx_outfilter_vs",
                              NULL, GPU_SC_NO_CACHE | GPU_SC_OPTIMIZE);
    if (GPU_VALID(vb[1]))
        pb[1] = sc_shader_for(&ld, GPU_SC_PS, sharp, strlen(sharp), "hdr_sharp_ps", NULL,
                              GPU_SC_NO_CACHE | GPU_SC_OPTIMIZE);
    if (!GPU_VALID(pb[1]))
        printf("[D3D12] HDR blit shader compile failed: %s\n", sc_msg("?"));
    else
        s_hdr_blit.layout = lay_make(&ld);
    for (int i = 0; i < 2 && GPU_VALID(s_hdr_blit.layout); i++) {
        gpu_pipeline_desc pd;
        pass_desc(&pd, s_hdr_blit.layout, vb[i], pb[i], GPU_FORMAT_R16G16B16A16_FLOAT, GPU_WRITE_ALL, 1);
        s_hdr_blit.pso[i] = rc_pipeline(&pd, NULL);
        if (!s_hdr_blit.pso[i].v) break;
    }
    for (int i = 0; i < 2; i++) {
        sc_shader_drop(&vb[i]);
        sc_shader_drop(&pb[i]);
    }
    if (!s_hdr_blit.pso[0].v || !s_hdr_blit.pso[1].v) {
        printf("[D3D12] HDR blit pipeline failed (0x%08lX): HDR stays off\n", (long)E_FAIL);
        return 0;
    }
    s_hdr_blit.failed = 0;
    return 1;
}

/* The blit's descriptors: for each frame fi, the game's frame at 2 fi and T
 * or nothing at 2 fi + 1 (written by blit_to_window). The frame's is the
 * default view (today's NULL description); nothing is an empty RGBA16F
 * entry. */
static void blit_srv_none(gpu_view_desc* v)
{
    memset(v, 0, sizeof *v);
    v->kind = GPU_VIEW_SRV;
    v->dim = GPU_DIM_2D;
    v->mips = 1;
    v->format = GPU_FORMAT_R16G16B16A16_FLOAT;
}
static void blit_srvs_write(void)
{
    gpu_view_desc fr, none;
    blit_srv_none(&none);
    memset(&fr, 0, sizeof fr);
    fr.kind = GPU_VIEW_SRV;
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        fr.texture = s_rc_rt_tex[i];
        gpu_table_write_tex(s_rc_dev, s_rc_blit_pool, 2 * i, &fr);
        gpu_table_write_tex(s_rc_dev, s_rc_blit_pool, 2 * i + 1, &none);
    }
}

static void off_rt_transition(int slot, gpu_state to);

/* Records the scale of the game's frame fi into the window's buffer fi: the
 * largest rectangle with the frame's aspect ratio (RSX_ASPECT's, when set),
 * centred, black around it. Both resources are in PRESENT before and after.
 * On the frame's recorder (step C6): s_rc_rt_tex[fi] is render_targets[fi]'s
 * handle, s_rc_swap_image[fi] swap_buffers[fi]'s, s_rc_swap_rtv[fi] its view;
 * the pipelines and layouts bound are the passes' handles. */
static void blit_to_window(u32 fi)
{
    if (!s_rc_have_image) return;   /* (step VC2: no image acquired, rc_acquire) */
    gpu_cmd* const c = s_rc_cmd;
    gpu_barrier b[2];
    memset(b, 0, sizeof b);
    b[0].type    = GPU_BARRIER_TRANSITION;
    b[0].texture = s_rc_rt_tex[fi];
    b[0].sub     = GPU_SUB_ALL;
    b[0].before  = GPU_STATE_PRESENT;
    b[0].after   = GPU_STATE_PIXEL_SHADER_RESOURCE;
    b[1] = b[0];
    b[1].texture = s_rc_swap_image[s_rc_image];   /* (the acquired image: fi on D3D12, step VC1) */
    b[1].after   = GPU_STATE_RENDER_TARGET;
    gpu_cmd_barrier_n(c, 2, b);

    const gpu_view rtv = s_rc_swap_rtv[s_rc_image];   /* its view (step C5a) */
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = rtv;
    gpu_cmd_pass_begin(c, &pass);

    const float ww = (float)s_d3d.win_w, wh = (float)s_d3d.win_h;
    const float gh = (float)s_d3d.height, ar = rsx_aspect();
    const float gw = ar > 0.0f ? gh * ar : (float)s_d3d.width;   /* RSX_ASPECT: stretched to that shape */
    const float k = ww / gw < wh / gh ? ww / gw : wh / gh;
    const float vw = floorf(gw * k + 0.5f), vh = floorf(gh * k + 0.5f);
    const gpu_viewport vp = {floorf((ww - vw) * 0.5f), floorf((wh - vh) * 0.5f), vw, vh, 0.0f, 1.0f};
    if (vw != ww || vh != wh) {
        static const float black[4] = {0, 0, 0, 1};
        gpu_cmd_clear_color(c, rtv, black, NULL);
    }
    const gpu_rect sc = {0, 0, (int32_t)s_d3d.win_w, (int32_t)s_d3d.win_h};
    /* RSX_OUTFILTER=1: the area / bicubic pass instead of the bilinear tap.
     * With HDR on (the swap chain is FP16), the HDR pipelines (hdr_blit_ready). */
    const int hdr = s_hdr.active;
    const int sharp = knob_outfilter() == 1 && vw >= 1.0f && vh >= 1.0f && (hdr || of_ready());
    if (hdr) {
        gpu_cmd_set_layout(c, s_hdr_blit.layout);
        gpu_cmd_set_pipeline(c, s_hdr_blit.pso[sharp]);
    } else {
        gpu_cmd_set_layout(c, sharp ? s_of.layout : s_d3d.blit_layout);
        gpu_cmd_set_pipeline(c, sharp ? s_of.pso : s_d3d.blit_pso);
    }
    int use_t = 0;
    if (hdr) {   /* T's highlights at 2 fi + 1, for this frame only */
        const int t = s_hdr.frame_t;
        gpu_view_desc v;
        blit_srv_none(&v);
        if (t >= 0 && GPU_VALID(s_d3d.off_rt[t].tex) && s_d3d.off_rt[t].dxgi == GPU_FORMAT_R16G16B16A16_FLOAT) {
            off_rt_transition(t, GPU_STATE_PIXEL_SHADER_RESOURCE);
            memset(&v, 0, sizeof v);   /* the default view */
            v.texture = s_d3d.off_rt[t].tex;
            use_t = 1;
        }
        gpu_table_write_tex(s_rc_dev, s_rc_blit_pool, 2 * fi + 1, &v);
    }
    const gpu_table_pool no_pool = {0};
    gpu_cmd_set_table_pools(c, s_rc_blit_pool, no_pool);
    gpu_cmd_bind_table(c, 0, gpu_table_ref(s_rc_dev, s_rc_blit_pool, 2 * fi));   /* frame fi's table */
    if (sharp) {   /* viewport origin, frame texels per window pixel, frame size */
        const float of[8] = {vp.x, vp.y, (float)s_d3d.host_w / vw, (float)s_d3d.host_h / vh,
                             (float)s_d3d.host_w, (float)s_d3d.host_h, 0.0f, 0.0f};
        gpu_cmd_push_const(c, 1, 8, of, 0);
    }
    if (hdr) {   /* paper white in scRGB units, peak / paper white, the knee, the curve, T */
        const float white = hdr_white_nits(), ratio = hdr_peak_nits() / white;
        const float knee = ratio <= 1.25f ? 0.8f * ratio : (0.6f * ratio > 1.0f ? 0.6f * ratio : 1.0f);
        const float hc[8] = {white / 80.0f, ratio, knee, knob_hdr_gamma() ? 1.0f : 0.0f,
                             (float)use_t, 0.0f, 0.0f, 0.0f};
        gpu_cmd_push_const(c, 2, 8, hc, 0);
    }
    gpu_cmd_set_viewport(c, &vp);
    gpu_cmd_set_scissor(c, &sc);
    gpu_cmd_set_topology(c, GPU_TOPO_TRIANGLELIST);
    gpu_cmd_draw(c, 3, 1, 0, 0);

    b[0].before = GPU_STATE_PIXEL_SHADER_RESOURCE;
    b[0].after  = GPU_STATE_PRESENT;
    b[1].before = GPU_STATE_RENDER_TARGET;
    b[1].after  = GPU_STATE_PRESENT;
    gpu_cmd_barrier_n(c, 2, b);
}

/* rc_overlay_repaint: the last game frame shown, as blit_to_window
 * read it (render_targets[fi] is cleared by the next batch, a frame shown or
 * not). Render thread. */
static gpu_texture s_hold_tex;     /* the GPU layer's (step C6) */
static u32 s_hold_w, s_hold_h;
static int s_hold_valid;            /* s_hold_tex has the last frame shown */
static u32 s_shown_fi = ~0u;        /* render_targets[] index of the last frame shown */
static long long s_last_present_qpc;   /* the last Present, a repaint's too */
static long long s_game_frame_qpc;     /* the last game frame shown (hold_frame) */
static u32 s_repaints;                 /* repaints made (GCM_RATES) */

static int repaint_knob(void)
{
    static int v = -1;
    PS3_KNOB(v, ps3_env_int("RSX_OVERLAY_REPAINT", 1));
    return v;
}

/* The hold's and frame fi's whole moves between the states given, in one
 * barrier list: the display target s_rc_rt_tex[fi] first, the hold second. */
static void hold_barriers(u32 fi, gpu_state rt_before, gpu_state rt_after, gpu_state hold_before,
                          gpu_state hold_after)
{
    gpu_barrier b[2];
    memset(b, 0, sizeof b);
    b[0].type    = GPU_BARRIER_TRANSITION;
    b[0].texture = s_rc_rt_tex[fi];
    b[0].sub     = GPU_SUB_ALL;
    b[0].before  = rt_before;
    b[0].after   = rt_after;
    b[1] = b[0];
    b[1].texture = s_hold_tex;
    b[1].before  = hold_before;
    b[1].after   = hold_after;
    gpu_cmd_barrier_n(s_rc_cmd, 2, b);
}

/* Records the copy of the game's frame fi into the hold. s_rc_rt_tex[fi] is
 * in PRESENT before and after. The hold is the GPU layer's (step C6):
 * today's committed RGBA8 texture with no flags, in COMMON. */
static void hold_copy(u32 fi)
{
    if (GPU_VALID(s_hold_tex) && (s_hold_w != s_d3d.host_w || s_hold_h != s_d3d.host_h)) {   /* RSX_SCALE moved */
        gpu_texture_destroy(s_rc_dev, s_hold_tex, 0);   /* at once, as today's Release */
        s_hold_tex.id = 0;
    }
    if (!GPU_VALID(s_hold_tex)) {
        gpu_texture_desc td;
        memset(&td, 0, sizeof td);
        td.width = s_d3d.host_w; td.height = s_d3d.host_h; td.layers = 1; td.mips = 1;
        td.format = GPU_FORMAT_R8G8B8A8_UNORM; td.samples = 1;
        td.use = GPU_USE_COPY_SRC | GPU_USE_COPY_DST;
        td.state = GPU_STATE_COMMON;
        s_hold_tex = gpu_texture_create(s_rc_dev, &td);
        if (!GPU_VALID(s_hold_tex)) {
            s_hold_valid = 0;
            return;
        }
        s_hold_w = s_d3d.host_w;
        s_hold_h = s_d3d.host_h;
    }
    hold_barriers(fi, GPU_STATE_PRESENT, GPU_STATE_COPY_SOURCE, GPU_STATE_COMMON, GPU_STATE_COPY_DEST);
    gpu_cmd_copy_texture_whole(s_rc_cmd, s_hold_tex, s_rc_rt_tex[fi]);
    hold_barriers(fi, GPU_STATE_COPY_SOURCE, GPU_STATE_PRESENT, GPU_STATE_COPY_DEST, GPU_STATE_COMMON);
    s_hold_valid = 1;
}

/* The copy above for each frame shown while repaints are on. */
static void hold_frame(u32 fi)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    s_game_frame_qpc = q.QuadPart;
    s_shown_fi = fi;
    if (!s_overlay_repaint || !repaint_knob()) {
        s_hold_valid = 0;
        return;
    }
    hold_copy(fi);
}

/* The window's display, for rc_display_timing: its refresh period and
 * the time of one of its vblanks, in performance counter ticks. The swap
 * chain's statistics give both for the display the frames reach. After a
 * present, at most every 100 ms: the vblank the last frame shown went up at,
 * and, over a second or more of those, the period. */
static volatile long long s_disp_period, s_disp_vblank, s_disp_at;
static void display_note(long long now)
{
    static long long freq, last, base_q;
    static UINT base_c;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    if (now - last < freq / 10) return;
    last = now;
    gpu_present_stats st;
    memset(&st, 0, sizeof st);
    if (gpu_swap_stats(s_rc_dev, s_rc_swap, &st) || !st.sync_qpc || !st.sync_refresh_count)
        return;
    const long long q = st.sync_qpc;
    const UINT c = st.sync_refresh_count;
    if (!base_q || c < base_c || q < base_q) {   /* (the first, or the statistics started over) */
        base_q = q;
        base_c = c;
    } else if (q - base_q >= freq && c > base_c) {
        const long long p = (q - base_q) / (long long)(c - base_c);
        if (p >= freq / 1000 && p <= freq / 20) InterlockedExchange64(&s_disp_period, p);
        base_q = q;
        base_c = c;
    }
    InterlockedExchange64(&s_disp_vblank, q);
    InterlockedExchange64(&s_disp_at, now);
}

/* Present, at the refresh or (RSX_VSYNC=0, live, default 1) at once with
 * tearing, so the title's own vblank is the frame limit and a game set above
 * the display's rate is not held to it. Without tearing support (swap chain
 * creation above) a sync interval of 0 would still be paced at the refresh
 * by the compositor, so it presents with vsync and says so once.
 * RSX_VSYNC=2 to 4 holds each frame that many refreshes at least: a title
 * whose frames come every n refreshes (ps3_vblank_lock_display) shows each
 * for n, and one that comes a little late moves the frames after it a
 * refresh later instead of being shown for n + 1 refreshes while the next is
 * shown for n - 1. A repaint (game_frame 0: the overlay over a held frame)
 * waits one refresh at most, so the overlay keeps the display's rate. */
static void dred_report(const char* where);

/* The device is lost when the driver resets the GPU (a TDR: Windows' answer
 * to a GPU that has not finished its work in about 2 s) or the GPU goes away.
 * Nothing draws from then on while the game plays on: the picture stays on
 * its last frame (Demon's Souls issue #205) and the overlay can make no
 * textures. Every fence reads as done at once, so no wait notices it, and
 * Present's result was never looked at: the only trace was the overlay's.
 * Said once, with Windows' reason (the instance file's rc_be_lost_text)
 * and its breadcrumbs knob (rc_be_lost_hint); rc_device_lost() hands the
 * reason to the host (its problem report). */
static volatile LONG s_device_lost;      /* GetDeviceRemovedReason, 0 while the device works */
static volatile LONG64 s_device_lost_qpc;

/* Step VC2: the line, the report's reason and the breadcrumbs for a loss of
 * reason rr: the device's (check_device), or the window's surface's, which
 * nothing reaches again either (the instance file's rc_be_surface_lost:
 * Vulkan's VK_ERROR_SURFACE_LOST_KHR, when the backend could not make it
 * again; gpu.h's GPU_PRESENT_SURFACE_LOST). */
static void device_lost_say(const char* where, HRESULT rr);
static void check_device(const char* where)
{
    if (s_device_lost || !s_rc_dev) return;
    const HRESULT rr = (HRESULT)gpu_lost(s_rc_dev);
    if (rr == S_OK) return;
    device_lost_say(where, rr);
}
static void device_lost_say(const char* where, HRESULT rr)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    InterlockedExchange64(&s_device_lost_qpc, q.QuadPart);
    if (InterlockedCompareExchange(&s_device_lost, (LONG)rr, 0) != 0) return;
    fprintf(stderr, "[D3D12] the graphics device was lost (seen at %s): %s, 0x%08lX. Nothing more can be"
            " drawn, so the picture stays as it is; the game needs a restart.%s%c", where,
            rc_be_lost_text((u32)rr), (unsigned long)rr, rc_be_lost_hint, 10);
    dred_report(where);
}

static void swap_present(int game_frame)
{
    static int vsync = -1;
    PS3_KNOB(vsync, ps3_env_int("RSX_VSYNC", 1));
    if (!vsync && !s_d3d.tearing) {
        static int said = 0;
        if (!said++) printf("[D3D12] RSX_VSYNC=0: tearing is not supported on this system; presenting with vsync\n");
    }
    const int tear = !vsync && s_d3d.tearing;
    const UINT interval = tear ? 0 : vsync <= 1 || !game_frame ? 1 : vsync > 4 ? 4 : (UINT)vsync;
    const int pr = gpu_swap_present(s_rc_dev, s_rc_swap, interval, tear ? GPU_PRESENT_TEARING : 0);
    if (pr == GPU_PRESENT_SURFACE_LOST) {   /* (step VC2: gpu.h's codes; never on D3D12) */
        if (!s_device_lost) device_lost_say("present", (HRESULT)rc_be_surface_lost);
    } else if (pr) {
        check_device("present");
    }
    rc_be_presented(s_rc_dev);   /* (the instance file's: null prints the stream's hash) */
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    s_last_present_qpc = q.QuadPart;
    display_note(q.QuadPart);
}

static void wait_for_gpu(void);

/* Between frames, on the render thread: the swap chain follows the window's
 * client size (s_win_req). The GPU is idled first and the overlay drops its
 * views of the old buffers, or ResizeBuffers fails on the references. */
static void swap_resize(void)
{
    const LONG req = s_win_req;
    u32 w = (u32)req >> 16, h = (u32)req & 0xFFFF;
    if (!w || !h) { w = s_d3d.win_w; h = s_d3d.win_h; }
    const int hdr = hdr_want();   /* and its format follows RSX_HDR (see HDR output) */
    if (w == s_d3d.win_w && h == s_d3d.win_h && hdr == s_hdr.active) return;
    wait_for_gpu();
    if (s_overlay_on && s_overlay.release) s_overlay.release(s_overlay.user);
    /* The buffers are the swap chain's (borrowed): the resize drops them and
     * gets them again, with the buffer count and flags it was made with. */
    for (u32 i = 0; i < RC_SWAP_MAX; i++) s_rc_swap_image[i].id = 0;
    HRESULT hr = (HRESULT)gpu_swap_resize(s_rc_dev, s_rc_swap, w, h,
                                          hdr ? GPU_FORMAT_R16G16B16A16_FLOAT : GPU_FORMAT_R8G8B8A8_UNORM);
    if (SUCCEEDED(hr) && hdr != s_hdr.active) {
        /* A display that refuses scRGB keeps RGBA8. */
        if (!swap_color_space(hdr)) {
            if (hdr) {
                printf("[D3D12] HDR: the swap chain's scRGB colour space was refused; staying SDR\n");
                s_hdr.failed = 1;
                hr = (HRESULT)gpu_swap_resize(s_rc_dev, s_rc_swap, w, h, GPU_FORMAT_R8G8B8A8_UNORM);
            }
        } else {
            s_hdr.active = hdr;
            if (hdr) printf("[D3D12] HDR: on (paper white %.0f nits, peak %.0f nits, %s)\n", hdr_white_nits(),
                            hdr_peak_nits(), knob_hdr_gamma() ? "sRGB curve" : "gamma 2.2");
            else     printf("[D3D12] HDR: off\n");
        }
    }
    if (SUCCEEDED(hr)) {
        s_d3d.win_w = w;
        s_d3d.win_h = h;
    } else {
        printf("[D3D12] ResizeBuffers %ux%u failed (0x%08lX); keeping %ux%u\n", w, h, (long)hr,
               s_d3d.win_w, s_d3d.win_h);
        InterlockedExchange(&s_win_req, (LONG)((s_d3d.win_w << 16) | s_d3d.win_h));
    }
    if (swap_buffers_get() != 0) printf("[D3D12] swap chain buffers lost after a resize\n");
    /* The back buffer index starts over. The GPU is idle, so every frame
     * slot's fence is passed already, and the timeline only goes up: nothing
     * to move (today's code moved every frame's fence value to the highest). */
    rc_acquire();   /* (step VC1: the image, and the slot it falls in) */
}

