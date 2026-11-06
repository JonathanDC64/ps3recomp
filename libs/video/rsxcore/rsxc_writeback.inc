/* ---------------------------------------------------------------------------
 * Colour write-back: offscreen RTs -> guest memory.
 *
 * On the RSX a render target IS guest memory, and titles read it back with
 * the CPU, SPUs or the RSX's own transfer engines. Demon's Souls computes its
 * exposure on the GPU into the first texel of rotating 16x16 F_X32 targets and
 * feeds it to every level shader's tone-map constant (global parameter 0x87).
 * Rendering only into D3D12 resources left those bytes as stale heap data
 * (fp16 NaN, old fragment-program words) and the constant at 0; with the
 * write-back it carries the value the luminance chain computed (0.2018).
 *
 * Every RT targeted by a frame is copied to a per-slot readback buffer at the
 * end of that frame and, once the GPU is done, converted to the guest surface
 * layout (big-endian, linear pitch or swizzled) and stored at its offset.
 * RPCS3's equivalent is "Write Color Buffers". RSX_WRITE_COLOR=0 turns it off
 * (it costs a GPU wait plus a CPU conversion per frame).
 * -----------------------------------------------------------------------*/
static void off_rt_transition(int slot, gpu_state to);
static int write_color_on(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("RSX_WRITE_COLOR", 1));
    return on;
}

/* RSX_WRITE_COLOR_DEFER (default 1): store the frame's write-back copies at the
 * start of the next frame, after the GPU wait that frame makes anyway, instead
 * of waiting for the GPU between submit and Present. That wait held up both the
 * Present and the FIFO drain on this thread for most of the GPU's frame time.
 * Guest reads of render targets (Demon's Souls's exposure) see the values one
 * frame later. 0 restores the store before Present. */
static int write_color_defer(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("RSX_WRITE_COLOR_DEFER", 1));
    return on;
}
static double s_fl_wait_ms, s_fl_store_ms;   /* FRAME_LOG: frame-start wait, store */

static inline __attribute__((always_inline)) void rt_pitch_record(D3D12DrawRecord* dr)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st) return;
    int a = st->color_target == 2 ? 1 : 0;
    dr->rt_pitch[0] = st->surface_color_pitch[a];
    dr->rt_dma[0]   = st->surface_color_dma[a];
    for (int i = 1; i < 4; i++) {
        dr->rt_pitch[i] = st->surface_color_pitch[i];
        dr->rt_dma[i]   = st->surface_color_dma[i];
    }
    dr->zeta = st->surface_zeta_offset;
    dr->ctgt = st->color_target;
    dr->zfmt = (st->surface_format >> 5) & 7u;
}

static u32 rt_guest_bpp(u32 dxgi)
{
    return dxgi == GPU_FORMAT_R16G16B16A16_FLOAT ? 8u :
           dxgi == GPU_FORMAT_R32G32B32A32_FLOAT ? 16u : 4u;
}

/* Step C5b: the textures made here (the downscale copies, the snapshots, the
 * screen copy) are the GPU layer's, held by their handles (step C8a). One
 * dropped while a frame is recorded may still be used by that frame's list,
 * so it goes into the grave (rc_grave_tex, rsxc_targets.inc), where today's
 * off_rt_defer_release put it. */

/* RSX_SCALE: the write-back of a scaled target.
 *
 * A target drawn at a multiple of its guest size is drawn down to a guest-size
 * copy of the same format (one texel of each guest texel's block, Load), and
 * that copy is read back like any other target. Without it the guest memory
 * under a scaled target kept whatever was there before, and a target created
 * later at that offset was filled from it: the Profile List showed coloured
 * noise below its footer, where it draws nothing over a recreated target. */
static gpu_layout            s_down_layout;   /* (step C3b) */
static gpu_pipeline          s_down_pso[8];   /* the GPU layer's (step C3b), bound as handles (step C5b) */
static u32                   s_down_pso_fmt[8];   /* format, | 1 << 31 for guest byte order */
/* (The copies' RTVs are gpu_view_get's, kept with each copy: step C5b.) */
static gpu_shader            s_down_vs;
static gpu_shader            s_down_ps;
static gpu_shader            s_down_ps_argb;   /* A,R,G,B bytes: the guest's A8R8G8B8 */

static int down_init(void)
{
    static int state = 0;   /* 0 not tried, 1 ready, -1 failed */
    if (state) return state > 0;
    state = -1;
    static const char vs[] =
        "float4 main(uint id : SV_VertexID) : SV_Position {\n"
        "    float2 t = float2((id << 1) & 2, id & 2);\n"
        "    return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "}\n";
    static const char ps[] =
        "Texture2D<float4> src : register(t0);\n"
        "cbuffer K : register(b0) { float2 ratio; };\n"
        "float4 main(float4 p : SV_Position) : SV_Target {\n"
        "    return src.Load(int3(int2(p.xy * ratio), 0));\n"
        "}\n";
    static const char ps_argb[] =
        "Texture2D<float4> src : register(t0);\n"
        "cbuffer K : register(b0) { float2 ratio; };\n"
        "float4 main(float4 p : SV_Position) : SV_Target {\n"
        "    return src.Load(int3(int2(p.xy * ratio), 0)).argb;\n"
        "}\n";
    /* The GPU layer's layout: the t0 table, b0 = 2 constants. (Call map G12:
     * today's range has offset 0, the GPU layer's OFFSET_APPEND; the first
     * range of its table, it lands at 0 either way, so only the serialized
     * bytes differ.) The shaders are compiled for it (step VC1). */
    gpu_layout_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.nslots = 2;
    ld.slot[0] = lay_slot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
    ld.slot[1] = lay_slot(GPU_SLOT_CONST32, 0, 2, GPU_VIS_PIXEL);
    /* The shaders are the GPU layer's (step C3b), kept for down_pso's later
     * formats: never freed. */
    s_down_vs = sc_shader_for(&ld, GPU_SC_VS, vs, sizeof vs - 1, "down_vs", NULL, GPU_SC_NO_CACHE);
    if (GPU_VALID(s_down_vs))
        s_down_ps = sc_shader_for(&ld, GPU_SC_PS, ps, sizeof ps - 1, "down_ps", NULL, GPU_SC_NO_CACHE);
    if (GPU_VALID(s_down_ps))
        s_down_ps_argb = sc_shader_for(&ld, GPU_SC_PS, ps_argb, sizeof ps_argb - 1, "down_ps_argb", NULL,
                                       GPU_SC_NO_CACHE);
    if (!GPU_VALID(s_down_ps_argb)) {
        if (t_sc_msg[0]) fprintf(stderr, "[D3D12] write-back downscale: %s%c", t_sc_msg, 10);
        return 0;
    }
    s_down_layout = lay_make(&ld);
    if (!GPU_VALID(s_down_layout)) return 0;
    {   /* the GPU layer's table pool, a 1-entry table each (step C4): shader
         * visible, 2 x MAX_OFF_RTS (frame parity) */
        gpu_table_pool_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.kind = GPU_TABLE_TEX;
        pd.count = 2 * MAX_OFF_RTS;
        pd.nregions = 1;
        pd.region[0].first = 0; pd.region[0].tables = 2 * MAX_OFF_RTS; pd.region[0].size = 1;
        s_rc_down_pool = gpu_table_pool_create(s_rc_dev, &pd);
        if (!GPU_VALID(s_rc_down_pool)) return 0;
    }
    state = 1;
    return 1;
}

static gpu_pipeline down_pso(gpu_format f, int argb)
{
    const gpu_pipeline none = {0};
    const u32 key = (u32)f | (argb ? 1u << 31 : 0u);
    for (int i = 0; i < 8; i++)
        if (s_down_pso[i].v && s_down_pso_fmt[i] == key) return s_down_pso[i];
    int slot = -1;
    for (int i = 0; i < 8; i++) if (!s_down_pso[i].v) { slot = i; break; }
    if (slot < 0) return none;
    gpu_pipeline_desc pd;
    pass_desc(&pd, s_down_layout, s_down_vs, argb ? s_down_ps_argb : s_down_ps, (gpu_format)f, GPU_WRITE_ALL, 0);
    s_down_pso[slot] = rc_pipeline(&pd, NULL);   /* (the GPU layer's: step C3b) */
    if (!s_down_pso[slot].v) return none;
    s_down_pso_fmt[slot] = key;
    return s_down_pso[slot];
}

/* Draw scaled target i down to its guest size; the copy is left in COPY_SOURCE.
 * `argb`: in the guest's byte order (an R8G8B8A8 copy holding A,R,G,B).
 * Returns none if that cannot be done (the target is then not written back).
 * The copy, its view and the pass are the GPU layer's (step C5b), recorded on
 * the frame's recorder in today's order. */
static gpu_texture off_rt_downscale(int i, int argb)
{
    const gpu_texture none = {0};
    OffRT* r = &s_d3d.off_rt[i];
    if (!down_init()) return none;
    const gpu_pipeline pso = down_pso((gpu_format)r->gdxgi, argb);
    if (!pso.v) return none;
    if (GPU_VALID(r->down_tex) && (r->down_w != r->w || r->down_h != r->h || r->down_dxgi != r->gdxgi))
        rc_grave_tex(&r->down_tex);   /* the open list may still use it */
    if (!GPU_VALID(r->down_tex)) {
        /* Today's: committed, ALLOW_RENDER_TARGET, in RENDER_TARGET, no clear
         * value. */
        gpu_texture_desc td;
        memset(&td, 0, sizeof td);
        td.width = r->w; td.height = r->h; td.layers = 1; td.mips = 1;
        td.format = (gpu_format)r->gdxgi; td.samples = 1;
        td.use = GPU_USE_RT | GPU_USE_COPY_SRC;
        td.state = GPU_STATE_RENDER_TARGET;
        r->down_tex = gpu_texture_create(s_rc_dev, &td);
        if (!GPU_VALID(r->down_tex)) return none;
        r->down_w = r->w; r->down_h = r->h; r->down_dxgi = r->gdxgi;
        r->down_st = GPU_STATE_RENDER_TARGET;
    }
    gpu_cmd* const c = s_rc_cmd;
    off_rt_transition(i, GPU_STATE_PIXEL_SHADER_RESOURCE);
    if (r->down_st != GPU_STATE_RENDER_TARGET) {
        gpu_cmd_barrier(c, r->down_tex, r->down_st, GPU_STATE_RENDER_TARGET);
        r->down_st = GPU_STATE_RENDER_TARGET;
    }
    /* The source's default view (today's NULL description) in the down pool,
     * and its table (step C4). */
    const u32 di = (u32)(s_d3d.frame_count & 1u) * MAX_OFF_RTS + (u32)i;
    {
        gpu_view_desc sv;
        memset(&sv, 0, sizeof sv);
        sv.texture = r->tex;
        sv.kind = GPU_VIEW_SRV;
        gpu_table_write_tex(s_rc_dev, s_rc_down_pool, di, &sv);
    }
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    {   /* the copy's default RTV (today's NULL description), made once */
        gpu_view_desc rv;
        memset(&rv, 0, sizeof rv);
        rv.texture = r->down_tex;
        rv.kind = GPU_VIEW_RTV;
        pass.nrt = 1;
        pass.rt[0] = gpu_view_get(s_rc_dev, &rv);
        if (!GPU_VALID(pass.rt[0])) return none;
    }
    const gpu_table_pool no_pool = {0};
    gpu_cmd_pass_begin(c, &pass);
    gpu_cmd_set_layout(c, s_down_layout);
    gpu_cmd_set_pipeline(c, pso);
    gpu_cmd_set_table_pools(c, s_rc_down_pool, no_pool);
    gpu_cmd_bind_table(c, 0, gpu_table_ref(s_rc_dev, s_rc_down_pool, di));
    const float ratio[2] = {(float)r->hw / (float)r->w, (float)r->hh / (float)r->h};
    gpu_cmd_push_const(c, 1, 2, ratio, 0);
    const gpu_viewport vp = {0, 0, (float)r->w, (float)r->h, 0.0f, 1.0f};
    const gpu_rect rc = {0, 0, (int32_t)r->w, (int32_t)r->h};
    gpu_cmd_set_viewport(c, &vp);
    gpu_cmd_set_scissor(c, &rc);
    gpu_cmd_set_topology(c, GPU_TOPO_TRIANGLELIST);
    gpu_cmd_draw(c, 3, 1, 0, 0);
    gpu_cmd_barrier(c, r->down_tex, GPU_STATE_RENDER_TARGET, GPU_STATE_COPY_SOURCE);
    r->down_st = GPU_STATE_COPY_SOURCE;
    return r->down_tex;
}

/* RSX_WB_GPU (live, default 1): the write-back of a linear A8R8G8B8
 * target in RSX local memory goes from the GPU straight into guest memory,
 * through a heap opened on the guest's own pages: a draw puts the target in
 * the guest's byte order and a copy puts it in place. The CPU store was 13 MB
 * a frame in the deep Palace (three 1280x720 targets and a dozen smaller), a
 * read and a write of every byte on the RSX thread. The boot reserves RSX
 * local memory as an allocation of its own for this (boot_main.cpp,
 * vm_reserve); without that, other formats, swizzled targets and offsets the
 * copy cannot address (512-byte offsets, 256-byte pitches) store on the CPU
 * as before. 0: every target on the CPU. 2: as 1, and the CPU store is also
 * made, into a scratch copy, and compared with what the GPU stored (a line
 * per mismatching target, and a count every 1024 stores).
 * Off Windows there is no heap to open: the guest's space is one anonymous
 * mapping (boot_main.cpp), and each target gets a buffer over its own pages
 * (wbg_place). Not one over all 256 MB: amdgpu checks an imported buffer's
 * pages one by one at every submit (amdgpu_cs_parser_bos), and they stay
 * resident. */
#define WBG_BASE 0xC0000000u   /* RSX local memory (cellGcm's localAddress) */
#define WBG_SIZE 0x10000000u
/* state: 0 untried, 1 ready, -1 off. buf: the GPU layer's buffer over RSX
 * local memory (step C5b: gpu_buffer_import_host, today's heap opened on the
 * guest's pages and the buffer placed on it). */
static struct { int state; gpu_buffer buf; } s_wbg;
#ifndef _WIN32
#define WBG_SPANS 32   /* more than MAX_OFF_RTS: targets move between areas; past it, the CPU store */
static struct { u32 base, size; gpu_buffer buf; } s_wbg_span[WBG_SPANS];
static int s_wbg_spans;
#endif
static int wbg_mode(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("RSX_WB_GPU", 1));
    return on;
}
static int wbg_ready(void)
{
    const int on = wbg_mode();
    if (!on || s_wbg.state < 0) return 0;
    if (s_wbg.state) return 1;
    s_wbg.state = -1;
#ifdef _WIN32
    extern uint8_t* vm_base;
    u8* const win = vm_base + WBG_BASE;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(win, &mi, sizeof mi) || mi.AllocationBase != (void*)win) {
        fprintf(stderr, "[D3D12] write-back: RSX local memory is not an allocation of its own; stores on the CPU%c", 10);
        return 0;
    }
    /* The heap maps the whole allocation, so all of it is committed (the guest
     * sees zeros in pages it never wrote, as it did on first touch). */
    if (!VirtualAlloc(win, WBG_SIZE, MEM_COMMIT, PAGE_READWRITE)) return 0;
    /* The heap over it and the buffer placed on it are the GPU layer's (step
     * C5b), in COPY_DEST. Without GPU_CAP_HOST_IMPORT (on D3D12: no Device3) it
     * says nothing, as today; otherwise its own line says which call failed. */
    s_wbg.buf = gpu_buffer_import_host(s_rc_dev, win, WBG_SIZE);
    if (!GPU_VALID(s_wbg.buf)) {
        if (s_rc_dev->caps.bits & GPU_CAP_HOST_IMPORT)
            fprintf(stderr, "[D3D12] write-back: no buffer on guest memory; stores on the CPU%c", 10);
        return 0;
    }
    s_wbg.state = 1;
    fprintf(stderr, "[D3D12] write-back: from the GPU into guest memory%c", 10);
    return 1;
#else
    if (!(s_rc_dev->caps.bits & GPU_CAP_HOST_IMPORT)) return 0;
    s_wbg.state = 1;
    fprintf(stderr, "[rsx] write-back: from the GPU into guest memory%c", 10);
    return 1;
#endif
}

/* The buffer a copy to [dst, dst + len) of guest memory goes through, and the
 * offset in it. 0: none, so that target stores on the CPU. */
static int wbg_place(u32 dst, u64 len, gpu_buffer* buf, u64* offset)
{
#ifdef _WIN32
    (void)len;
    *buf = s_wbg.buf;
    *offset = dst - WBG_BASE;
    return 1;
#else
    extern uint8_t* vm_base;
    for (int i = 0; i < s_wbg_spans; i++)
        if (dst >= s_wbg_span[i].base && dst + len <= (u64)s_wbg_span[i].base + s_wbg_span[i].size) {
            *buf = s_wbg_span[i].buf;
            *offset = dst - s_wbg_span[i].base;
            return 1;
        }
    if (s_wbg_spans == WBG_SPANS) return 0;
    const u64 a = s_rc_dev->caps.host_import_align;
    if (!a || a > 0x10000u) return 0;
    /* The host address is what must be aligned: the guest space's mapping is
     * only page-aligned, and the alignment can be larger (D3D12's 64 KB). The
     * span stays inside the guest space, which runs well past RSX local
     * memory on both sides. */
    const uintptr_t at = (uintptr_t)(vm_base + dst);
    const uintptr_t lo = at & ~(uintptr_t)(a - 1), hi = (at + len + a - 1) & ~(uintptr_t)(a - 1);
    const gpu_buffer b = gpu_buffer_import_host(s_rc_dev, (void*)lo, hi - lo);
    if (!GPU_VALID(b)) {
        static int said;
        if (!said) { said = 1; fprintf(stderr, "[rsx] write-back: no buffer on guest memory at 0x%08X; stores on the CPU%c", dst, 10); }
        return 0;
    }
    const u64 base = dst - (u64)(at - lo), end = base + (u64)(hi - lo);
    s_wbg_span[s_wbg_spans].base = (u32)base;
    s_wbg_span[s_wbg_spans].size = (u32)(end - base);
    s_wbg_span[s_wbg_spans].buf = b;
    s_wbg_spans++;
    *buf = b;
    *offset = dst - base;
    return 1;
#endif
}

/* A write-back copy rt_writeback_record made, as the target was then. Its
 * store waits until the GPU is done with the batch (wb_store_half): with
 * frames in flight that can come after the next batch's pre-pass has moved
 * the target on, so the store works from these fields. The pre-pass stores
 * the copy first when it would drop the target or read the guest memory the
 * copy lands in (wb_land_target, wb_land_range); one whose target was
 * dropped anyway (gen) is not stored. */
enum { WB_CPU, WB_GPU, WB_CHECK };   /* stored by the CPU; by the GPU (wbg_record); both, compared */
typedef struct {
    int        slot, kind;
    u32        gen, serial;   /* the target's generation, and the batch's s_rf_serial */
    u32        off, w, h, gdxgi, gfmt, gpitch, gdma;
    u32        gpu_dst;       /* WB_GPU, WB_CHECK: where the GPU stored it */
    gpu_buffer rb;            /* WB_CPU, WB_CHECK: the readback copy */
    u32        dst, len;      /* the guest memory it lands in (len 0: none) */
} WbCopy;
/* The copies of each parity half's last batch: at most one per target. */
static struct { WbCopy c[MAX_OFF_RTS]; int n; } s_wb_q[2];

static int wb_swizzled(u32 gfmt) { return ((gfmt >> 8) & 0xF) == 2; }

/* The guest bytes copy c covers at its guest address: the swizzled
 * surface's whole power-of-two square, or its rows at the guest's pitch.
 * 0 for a swizzled size past what RSX can have (log2 above 12). */
static u32 wb_guest_len(const WbCopy* c)
{
    if (!wb_swizzled(c->gfmt)) return c->gpitch * c->h;
    const u32 lw = (c->gfmt >> 16) & 0xFF, lh = (c->gfmt >> 24) & 0xFF;
    if (lw > 12 || lh > 12) return 0;
    return ((1u << lw) << lh) * rt_guest_bpp(c->gdxgi);
}

static void wb_queue(int i, int kind, u32 gpu_dst, gpu_buffer rb)
{
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    OffRT* r = &s_d3d.off_rt[i];
    WbCopy* c = &s_wb_q[s_rf_par].c[s_wb_q[s_rf_par].n++];
    c->slot = i; c->kind = kind; c->gen = r->gen; c->serial = s_rf_serial;
    c->off = r->off; c->w = r->w; c->h = r->h; c->gdxgi = r->gdxgi;
    c->gfmt = r->gfmt; c->gpitch = r->gpitch; c->gdma = r->gdma;
    c->gpu_dst = gpu_dst; c->rb = rb;
    /* Where wb_store_cpu will put it (the GPU's own address for the rest). */
    c->dst = kind != WB_CPU ? gpu_dst
                            : cellGcmResolveLocated(c->gdma != CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER, c->off);
    c->len = c->dst && c->dst != 0xFFFFFFFFu ? wb_guest_len(c) : 0;
    r->wb_queued++;
}

/* Target i straight into guest memory, if it can go that way: its guest
 * address, or 0 when it goes through a readback copy. */
static u32 wbg_record(int i)
{
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    OffRT* r = &s_d3d.off_rt[i];
    if (r->gdxgi != GPU_FORMAT_R8G8B8A8_UNORM || ((r->gfmt >> 8) & 0xF) == 2 ||
        r->gdma == CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER || !wbg_ready()) return 0;
    { static int lg = -1; PS3_KNOB(lg, ps3_env_int("RSX_WRITE_COLOR_LOG", 0));
      if (lg) return 0; }   /* the log compares the CPU store's bytes */
    const u32 gp = r->gpitch, dst = cellGcmResolveLocated(1, r->off);
    if (dst < WBG_BASE || dst - WBG_BASE >= WBG_SIZE || ((dst - WBG_BASE) & 511u) || (gp & 255u) ||
        gp < r->w * 4u || gp > 0x10000 || !r->h ||
        (u64)(dst - WBG_BASE) + (u64)gp * (r->h - 1) + r->w * 4u > WBG_SIZE) return 0;
    gpu_buffer into;
    u64 into_at;
    if (!wbg_place(dst, (u64)gp * (r->h - 1) + r->w * 4u, &into, &into_at)) return 0;
    gpu_state prior = r->st;
    const gpu_texture from = off_rt_downscale(i, 1);
    if (!GPU_VALID(from)) return 0;
    gpu_footprint to;   /* the target's place in the buffer over RSX local memory */
    memset(&to, 0, sizeof to);
    to.offset    = into_at;
    to.format    = GPU_FORMAT_R8G8B8A8_UNORM;
    to.width     = r->w;
    to.height    = r->h;
    to.depth     = 1;
    to.row_pitch = gp;
    gpu_cmd_copy_tex_to_buf(s_rc_cmd, into, &to, from, 0, NULL);
    off_rt_transition(i, prior);
    return dst;   /* stored by the GPU: the store only takes its checksum */
}

/* Target i's readback copy, into its buffer for this batch's half: 1 recorded. */
static int wb_record_copy(int i)
{
    OffRT* r = &s_d3d.off_rt[i];
    /* A scaled target, or a promoted one (HDR), goes through the draw down
     * to the guest's size and format. */
    const int scaled = r->hw != r->w || r->hh != r->h || r->dxgi != r->gdxgi;
    const u32 pitch = (r->w * rt_guest_bpp(r->gdxgi) + 255) & ~255u;
    const u64 need = (u64)pitch * r->h;
    /* The readback buffer is the GPU layer's (step C5b; rb its resource,
     * borrowed). One too small goes with the last batch's fence: its last
     * copy was the batch before last's, stored by now (wb_store_done). */
    gpu_buffer* const rb = &r->rb_buf[s_rf_par];
    if (GPU_VALID(*rb) && r->rb_size[s_rf_par] < need) {
        const gpu_texture none = {0};
        rc_grave_put(none, *rb);
        rb->id = 0;
    }
    if (!GPU_VALID(*rb)) {
        gpu_buffer_desc bd;
        memset(&bd, 0, sizeof bd);
        bd.size = need;
        bd.use = GPU_USE_COPY_DST;
        bd.memory = GPU_MEM_READBACK;
        bd.state = GPU_STATE_COPY_DEST;
        *rb = gpu_buffer_create(s_rc_dev, &bd);
        if (!GPU_VALID(*rb)) return 0;
        r->rb_size[s_rf_par] = need;
    }
    gpu_state prior = r->st;
    gpu_texture from = r->tex;
    if (scaled) { if (!GPU_VALID(from = off_rt_downscale(i, 0))) return 0; }
    else off_rt_transition(i, GPU_STATE_COPY_SOURCE);
    gpu_footprint fp;
    memset(&fp, 0, sizeof fp);
    fp.format    = (gpu_format)r->gdxgi;
    fp.width     = r->w;
    fp.height    = r->h;
    fp.depth     = 1;
    fp.row_pitch = pitch;
    gpu_cmd_copy_tex_to_buf(s_rc_cmd, *rb, &fp, from, 0, NULL);
    off_rt_transition(i, prior);
    return 1;
}

/* Record the copies (before the frame's command list closes), each queued
 * for its store in this batch's half. */
static void rt_writeback_record(void)
{
    if (!write_color_on()) return;
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!GPU_VALID(r->tex) || !r->used) continue;
        const u32 gpu_dst = wbg_record(i);
        const gpu_buffer none = {0};
        if (gpu_dst && wbg_mode() != 2) { wb_queue(i, WB_GPU, gpu_dst, none); continue; }
        if (!wb_record_copy(i)) continue;   /* (RSX_WB_GPU=2: the CPU store's copy too, to compare) */
        wb_queue(i, gpu_dst ? WB_CHECK : WB_CPU, gpu_dst, r->rb_buf[s_rf_par]);
    }
}

/* Morton index for a swizzled RSX surface of 2^lw x 2^lh texels: x and y bits
 * interleave (x lowest) until the shorter side runs out. */
static u32 rsx_swz_index(u32 x, u32 y, u32 lw, u32 lh)
{
    u32 o = 0, b = 0;
    for (u32 i = 0; i < 16 && (i < lw || i < lh); i++) {
        if (i < lw) o |= ((x >> i) & 1u) << b++;
        if (i < lh) o |= ((y >> i) & 1u) << b++;
    }
    return o;
}

/* n words from a readback row to guest memory. kind 0: RGBA8 to A,R,G,B
 * (a rotate), 1: FP16 (swap each half's bytes), 2: 32-bit lanes (bswap). */
static void wb_conv_row(u8* d, const u8* s, u32 n, int kind)
{
    /* One loop per kind, so each vectorises. */
    #define WB_LOOP(EXPR) for (u32 i = 0; i < n; i++) { \
        u32 v; memcpy(&v, s + (u64)i * 4, 4); v = (EXPR); memcpy(d + (u64)i * 4, &v, 4); }
    if (kind == 0)      WB_LOOP((v << 8) | (v >> 24))
    else if (kind == 1) WB_LOOP(((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu))
    else                WB_LOOP(__builtin_bswap32(v))
    #undef WB_LOOP
}

/* RSX_WB_GPU=2: the CPU store of a target the GPU stored, made into a scratch
 * copy and compared with guest memory. */
static void wbg_check(const WbCopy* c, OffRT* r)
{
    extern uint8_t* vm_base;
    static u8* tmp; static size_t tmp_n;
    static unsigned long long n_ok, n_bad;
    const u32 pitch = (c->w * 4u + 255) & ~255u, gp = c->gpitch, row = c->w * 4u;
    if (tmp_n < (size_t)row) { free(tmp); tmp = (u8*)malloc(row); tmp_n = tmp ? row : 0; }
    void* mp = NULL;   /* (the GPU layer's map: today's Map(0, {0, n}), step C5b) */
    if (!tmp || !(mp = gpu_buffer_map(s_rc_dev, c->rb, 0, (u64)pitch * c->h))) return;
    u32 bad_y = ~0u, bad_x = 0;
    for (u32 y = 0; y < c->h && bad_y == ~0u; y++) {
        wb_conv_row(tmp, (const u8*)mp + (u64)y * pitch, c->w, 0);
        const u8* g = vm_base + c->gpu_dst + (u64)y * gp;
        if (memcmp(tmp, g, row))
            for (u32 x = 0; x < row; x++) if (tmp[x] != g[x]) { bad_y = y; bad_x = x; break; }
    }
    gpu_buffer_unmap(s_rc_dev, c->rb, 0, 0);   /* nothing written: Unmap(0, {0, 0}) */
    if (bad_y == ~0u) n_ok++;
    else {
        n_bad++;
        static int lines = 0;
        if (lines < 40 && ++lines) {
            const u8* g = vm_base + c->gpu_dst + (u64)bad_y * gp + (bad_x & ~3u);
            const u8* t = tmp + (bad_x & ~3u);
            fprintf(stderr, "[wb-gpu] 0x%08X %ux%u differs at row %u byte %u: gpu %02X%02X%02X%02X cpu %02X%02X%02X%02X%c",
                    c->gpu_dst, c->w, c->h, bad_y, bad_x, g[0], g[1], g[2], g[3], t[0], t[1], t[2], t[3], 10);
        }
    }
    if ((n_ok + n_bad) % 1024u == 0)
        fprintf(stderr, "[wb-gpu] check: %llu targets equal, %llu differ%c", n_ok, n_bad, 10);
    r->gaddr = c->gpu_dst;
    r->glen = gp * c->h;
    r->gsum_new = 1;
}

/* Copy c's readback into guest memory, converted to the guest's layout: the
 * guest address it went to, or 0 when it could not go. */
static u8* s_wc_shadow; static u32 s_wc_len;   /* RSX_WRITE_COLOR_LOG */
static u32 wb_store_cpu(const WbCopy* c)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    const int i = c->slot;
    u32 bpp = rt_guest_bpp(c->gdxgi);
    u32 pitch = (c->w * bpp + 255) & ~255u;
    u32 swz = ((c->gfmt >> 8) & 0xF) == 2;
    u32 lw = (c->gfmt >> 16) & 0xFF, lh = (c->gfmt >> 24) & 0xFF;
    u32 gp = c->gpitch;
    if (swz) { if (!wb_guest_len(c) || (1u << lw) < c->w || (1u << lh) < c->h) return 0; }
    else if (gp < c->w * bpp || gp > 0x10000) return 0;
    /* The surface's context DMA says which memory the offset is in: local
     * and IO offsets are overlapping number spaces, so the offset alone
     * cannot place a surface the title keeps in main memory. */
    u32 dst = cellGcmResolveLocated(c->gdma != CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER, c->off);
    if (!dst || dst == 0xFFFFFFFFu) return 0;
    { static int lg = -1; PS3_KNOB(lg, ps3_env_int("RSX_WRITE_COLOR_LOG", 0));
      if (lg > 0) { lg--;
        /* Did anything else write the surface since our last store? */
        static u8* shadow[MAX_OFF_RTS]; static u32 shoff[MAX_OFF_RTS], shlen[MAX_OFF_RTS];
        u32 len = swz ? ((1u << lw) << lh) * bpp : gp * c->h;
        u32 ndiff = 0, first = 0;
        if (shadow[i] && shoff[i] == c->off && shlen[i] == len)
            for (u32 k = 0; k < len; k++)
                if (shadow[i][k] != vm_base[dst + k]) { if (!ndiff) first = k; ndiff++; }
        fprintf(stderr, "[WRITE-COLOR] off=0x%X -> 0x%08X %ux%u fmt=0x%08X pitch=%u dma=0x%X%s foreign=%u first=+0x%X%c",
                c->off, dst, c->w, c->h, c->gfmt, gp, c->gdma, swz ? " swz" : "", ndiff, first, 10);
        /* The guest ranges of the fragment programs drawn with (rsxc_fp_seen),
         * so a write-back that lands on live ucode is reported. */
        for (int q = 0, nq = rsxc_fp_seen(-1, NULL, NULL); q < nq; q++) {
            u32 fa = 0, fl = 0;
            rsxc_fp_seen(q, &fa, &fl);
            if (fa < dst + len && dst < fa + fl)
                fprintf(stderr, "[WRITE-COLOR] !! surface 0x%X [0x%08X,+0x%X) overlaps FP ucode 0x%08X+0x%X%c",
                        c->off, dst, len, fa, fl, 10);
        }
        if (!shadow[i] || shlen[i] != len) { free(shadow[i]); shadow[i] = (u8*)malloc(len); shlen[i] = len; }
        shoff[i] = c->off;
        s_wc_shadow = shadow[i]; s_wc_len = len;
      } }
    /* The GPU layer's map and unmap (step C5b): today's Map(0, {0, n}) and
     * Unmap(0, {0, 0}). */
    void* mp = gpu_buffer_map(s_rc_dev, c->rb, 0, (u64)pitch * c->h);
    if (!mp) { s_wc_shadow = NULL; return 0; }
    /* A word at a time: every format here is whole 32-bit words per
     * texel, converted the same way. RGBA8 R,G,B,A -> A,R,G,B is a rotate,
     * FP16 swaps the bytes of each half, 32-bit lanes swap all four.
     * Byte by byte, this was 3.5-6 ms of each Demon's Souls frame (about
     * 25 MB: a 1280x720 target several times per frame, and others). */
    const int kind = c->gdxgi == GPU_FORMAT_R8G8B8A8_UNORM ? 0 :
                     c->gdxgi == GPU_FORMAT_R16G16B16A16_FLOAT ? 1 : 2;
    const u32 wpt = bpp / 4;                     /* words per texel */
    if (!swz) {
        for (u32 y = 0; y < c->h; y++)
            wb_conv_row(vm_base + dst + (u64)y * gp, (const u8*)mp + (u64)y * pitch,
                        c->w * wpt, kind);
    } else {
        /* rsx_swz_index(x, y) is the x bits' part OR the y bits' part. */
        static u32* xpart; static u32 xpart_n;
        if (xpart_n < c->w) {
            u32* nx = (u32*)realloc(xpart, (size_t)c->w * sizeof *nx);
            if (!nx) { gpu_buffer_unmap(s_rc_dev, c->rb, 0, 0); s_wc_shadow = NULL; return 0; }
            xpart = nx; xpart_n = c->w;
        }
        for (u32 x = 0; x < c->w; x++) xpart[x] = rsx_swz_index(x, 0, lw, lh);
        for (u32 y = 0; y < c->h; y++) {
            const u32 ypart = rsx_swz_index(0, y, lw, lh);
            const u8* srow = (const u8*)mp + (u64)y * pitch;
            for (u32 x = 0; x < c->w; x++)
                wb_conv_row(vm_base + dst + (u64)(xpart[x] | ypart) * bpp,
                            srow + (u64)x * bpp, wpt, kind);
        }
    }
    gpu_buffer_unmap(s_rc_dev, c->rb, 0, 0);
    if (s_wc_shadow) { memcpy(s_wc_shadow, vm_base + dst, s_wc_len); s_wc_shadow = NULL; }
    return dst;
}

/* Is copy c still to be stored, its target the one it was made from? */
static int wb_copy_live(const WbCopy* c)
{
    const OffRT* r = &s_d3d.off_rt[c->slot];
    return GPU_VALID(r->tex) && r->gen == c->gen;
}

/* Store half par's queued copies in guest memory, once the GPU has finished
 * their batch (it waits for that). A copy of a target dropped since is not
 * stored: the title may have put something else in that memory. The
 * pre-pass lands a copy before it drops the target (wb_land_target), so
 * that is a bug, said in a "[wb] not stored" line (test_rsx_synth fails on
 * it). */
static void wb_store_half(u32 par)
{
    extern uint8_t* vm_base;
    if (!s_wb_q[par].n) return;
    batch_wait(par);
    for (int k = 0; k < s_wb_q[par].n; k++) {
        const WbCopy* c = &s_wb_q[par].c[k];
        OffRT* r = &s_d3d.off_rt[c->slot];
        if (!wb_copy_live(c)) {
            static int n = 0;
            if (n < 20 && ++n) fprintf(stderr, "[wb] not stored: 0x%X (%ux%u), its target dropped first%c",
                                  c->off, c->w, c->h, 10);
            continue;
        }
        r->wb_queued--;
        if (!write_color_on() || !vm_base) continue;
        if (c->kind == WB_CHECK) { wbg_check(c, r); continue; }
        const u32 dst = c->kind == WB_GPU ? c->gpu_dst : wb_store_cpu(c);
        if (!dst) continue;
        r->gaddr = dst;
        r->glen = wb_guest_len(c);
        r->gsum_new = 1;
    }
    s_wb_q[par].n = 0;
    /* Checksums after every store, so targets that share memory do not
     * count each other's write-back as a foreign write. */
    if (!vm_base) return;
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!r->gsum_new) continue;
        r->gsum_new = 0;
        r->gsum = tex_csum(vm_base + r->gaddr, r->glen);
        r->gsum_ok = 1;
    }
}

/* Both halves' copies, the older batch's first. */
static void wb_store_queued(void)
{
    const u32 older = s_wb_q[0].n && s_wb_q[1].n && s_wb_q[1].c[0].serial < s_wb_q[0].c[0].serial;
    wb_store_half(older);
    wb_store_half(older ^ 1u);
}

/* At render_frame's start (s_rf_par set): the batch before last's copies,
 * its GPU work done (render_frame's end waited for it), then the last
 * batch's if the GPU is done with it too. Without frames in flight the GPU
 * is idle here and both are stored. */
static void wb_store_done(void)
{
    wb_store_half(s_rf_par);
    if (batch_done(s_rf_par ^ 1u)) wb_store_half(s_rf_par ^ 1u);
}

/* Every queued copy stored, the GPU idle first: before what drops the
 * targets or needs the write-back current (RSX_SCALE, RSX_WRITE_COLOR_DEFER=0). */
static void wb_store_all(void)
{
    wait_for_gpu();
    wb_store_queued();
}

/* The last batch's copies, stored now (waiting for that batch first): the
 * pre-pass is about to drop a target or read memory one of them lands in.
 * The other half holds only this batch's own copies, recorded at its end,
 * after the pre-pass: wb_store_done stored the batch before last's. The
 * texture workers stop first (tcj_stop): they read guest memory. */
static void wb_land_last(void)
{
    tcj_stop();
    wb_store_half(s_rf_par ^ 1u);
}

static void wb_land_target(int slot)
{
    if (!s_d3d.off_rt[slot].wb_queued) return;
    const u32 h = s_rf_par ^ 1u;
    for (int k = 0; k < s_wb_q[h].n; k++) {
        const WbCopy* c = &s_wb_q[h].c[k];
        if (c->slot == slot && wb_copy_live(c)) { wb_land_last(); return; }
    }
}

static void wb_land_range(u32 addr, u64 len)
{
    const u32 h = s_rf_par ^ 1u;
    for (int k = 0; k < s_wb_q[h].n; k++) {
        const WbCopy* c = &s_wb_q[h].c[k];
        if (c->len && c->dst < (u64)addr + len && addr < (u64)c->dst + c->len && wb_copy_live(c)) {
            wb_land_last();
            return;
        }
    }
}

/* Drop offscreen targets whose guest memory the title rewrote after our last
 * write-back: it streamed a texture or copied an image there. A texture bound
 * at that offset then reads the guest bytes, and a later draw into it starts
 * a new target from them. Sampling the old target instead is a live-only
 * error -- a replay starts with no targets and reads the guest bytes. */
static void off_rt_drop_rewritten(void)
{
    extern uint8_t* vm_base;
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("RSX_RT_REWRITTEN", 1));
    if (!en || !vm_base) return;
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!GPU_VALID(r->tex) || !r->gsum_ok) continue;
        if (r->wb_queued) continue;   /* our own write-back may still be landing there */
        if (tex_csum(vm_base + r->gaddr, r->glen) == r->gsum) continue;
        { static int n = 0;
          if (n < 40 && ++n) fprintf(stderr, "[RT] 0x%X (%ux%u) rewritten by the title: "
                                "dropped (frame %llu)%c", r->off, r->w, r->h,
                                (unsigned long long)s_d3d.frame_count, 10); }
        off_rt_drop_target(r);   /* the target and its staging (step C5a) */
        rc_grave_tex(&r->snap_tex);   /* step C5b */
        rc_grave_tex(&r->down_tex);
        r->gsum_ok = 0;
    }
}

/* Write a texture SRV (or a null SRV when tex is none) at an absolute SRV
 * heap slot. Used to fill per-draw t0-t3 descriptor windows. */
/* What each descriptor last held is the SRV pool's memo (step C4; it was
 * SrvMemo here, for the windows only). A draw at the same index usually
 * binds the same textures as it did the frame before, and at 5-6k draws a
 * frame (Demon's Souls's Boletarian Palace) re-creating all 16 views of
 * every window was ~11 ms of the render thread's frame. An unchanged view is
 * left as it is. A descriptor a batch writes is the GPU's again only after
 * its last reader is done (the windows and the AA and SMAA sets are a set per
 * parity half, and render_frame's end waited for the half's last batch), so
 * skipping a write is safe.
 * The memo compares the whole view, the texture by its handle, and a handle
 * names one texture for the device's life, so no stamp is needed: today's
 * memo rewrote every window after any resource was made, and those rewrites
 * of an unchanged view are the only ones skipped as well. It is always on
 * (RSX_SRV_MEMO=0 is gone). Inline: the compare is the backend's command
 * header's (the instance includes it), only
 * a write calls out. Through the pool's writer, found once at init (step
 * C8a): the 16 calls of a draw look nothing up before the memo. */
static void srv_write_ex(u32 heap_slot, gpu_texture tex, gpu_format fmt,
                         u32 swizzle, int cube)
{
    gpu_view_desc sv;
    sv.texture = tex;
    sv.kind = GPU_VIEW_SRV;
    sv.dim = cube ? GPU_DIM_CUBE : GPU_DIM_2D;   /* a cube must match the TextureCube declaration in the HLSL */
    sv.flags = 0;
    sv.mips = 1;
    sv.format = fmt;
    sv.swizzle = swizzle;
    sv.mip = 0;
    sv.layer = 0;
    gpu_fast_table_writer_tex(s_rc_dev, s_rc_srv_w, heap_slot, &sv);
}

static void srv_write(u32 heap_slot, gpu_texture tex, gpu_format fmt, u32 swizzle)
{
    srv_write_ex(heap_slot, tex, fmt, swizzle, 0);
}

/* On the frame's recorder (step C5b). Inline (the backend's command header), so a draw's
 * transition is today's one call. */
static void off_rt_transition(int slot, gpu_state to)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (r->st == to) return;
    gpu_cmd_barrier(s_rc_cmd, r->tex, r->st, to);
    r->st = to;
}

/* A draw that samples a surface it is also rendering to. RSX allows it, and
 * Demon's Souls relies on it: its exposure adaptation (a 16x16 F_X32 target)
 * reads the previous exposure from the target it overwrites, each pixel its
 * own texel. D3D12 cannot have one resource bound as RTV and SRV at once; that
 * read returned 0, so the exposure never moved past one adaptation step from 0
 * and the level drew almost black. Copy the target just before the draw and
 * return the copy for the draw to sample: the contents RSX would have read.
 * (The copy's handle, r->snap_tex; none when there is none.) */
static gpu_texture off_rt_snapshot(int slot)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (!GPU_VALID(r->tex)) return k_rc_no_tex;
    if (GPU_VALID(r->snap_tex) && (r->snap_w != r->hw || r->snap_h != r->hh || r->snap_dxgi != r->dxgi))
        rc_grave_tex(&r->snap_tex);   /* the open list may still use it */
    if (!GPU_VALID(r->snap_tex)) {
        /* The GPU layer's (step C5b), sampled by the draws (the SRV writes).
         * Today's: committed, no flags, in COPY_DEST. */
        gpu_texture_desc td;
        memset(&td, 0, sizeof td);
        td.width = r->hw; td.height = r->hh; td.layers = 1; td.mips = 1;
        td.format = (gpu_format)r->dxgi; td.samples = 1;
        td.use = GPU_USE_SAMPLED | GPU_USE_COPY_DST;
        td.state = GPU_STATE_COPY_DEST;
        r->snap_tex = gpu_texture_create(s_rc_dev, &td);
        if (!GPU_VALID(r->snap_tex)) return k_rc_no_tex;
        r->snap_w = r->hw; r->snap_h = r->hh; r->snap_dxgi = r->dxgi;
        r->snap_st = GPU_STATE_COPY_DEST;
    }
    const gpu_state back = r->st;
    off_rt_transition(slot, GPU_STATE_COPY_SOURCE);
    if (r->snap_st != GPU_STATE_COPY_DEST)
        gpu_cmd_barrier(s_rc_cmd, r->snap_tex, r->snap_st, GPU_STATE_COPY_DEST);
    gpu_cmd_copy_texture_whole(s_rc_cmd, r->snap_tex, r->tex);
    gpu_cmd_barrier(s_rc_cmd, r->snap_tex, GPU_STATE_COPY_DEST, GPU_STATE_PIXEL_SHADER_RESOURCE);
    r->snap_st = GPU_STATE_PIXEL_SHADER_RESOURCE;
    off_rt_transition(slot, back);
    return r->snap_tex;
}

/* Set by the present entry point: 1 = this batch ends with a swapchain
 * Present, 0 = execute the recorded draws only. An OFFSCREEN-only batch (a
 * render-to-texture pass) must still run -- it is what fills the texture a
 * later batch samples -- but presenting it would show a half-built frame. */
static int s_present_this_frame = 1;

/* Resolve a DRAW_*_TEX value: a hex raw offset, or the literal "duck" for the
 * content-identified duck texture. */
static u32 draw_filter_tex(const char* e)
{
    if (!e) return 0;
    if (e[0] == 'd' && e[1] == 'u') return s_duck_raw;
    return (u32)strtoul(e, NULL, 16);
}

/* Copy the current backbuffer into the screen copy. Called mid-frame the moment
 * the reduced-viewport passes finish -- this title renders its reflection into
 * the TOP-LEFT REGION of the render target and the main scene then overwrites
 * it, so a snapshot taken at end of frame contains the scene, not the
 * reflection. Also called at end of frame as a fallback for frames with no
 * reduced-viewport pass. */
static void screen_copy_capture(u32 fi)
{
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("SCREEN_AS_TEX", 1));
    if (!en || !s_rc_dev) return;
            if (!GPU_VALID(s_rc_screen_copy_tex)) {
                /* The GPU layer's (step C5b), sampled by the draws (the SRV
                 * writes). Today's: committed, no flags, in
                 * PIXEL_SHADER_RESOURCE. */
                gpu_texture_desc td;
                memset(&td, 0, sizeof td);
                td.width = s_d3d.host_w; td.height = s_d3d.host_h; td.layers = 1; td.mips = 1;
                td.format = GPU_FORMAT_R8G8B8A8_UNORM; td.samples = 1;
                td.use = GPU_USE_SAMPLED | GPU_USE_COPY_DST | GPU_USE_COPY_SRC;
                td.state = GPU_STATE_PIXEL_SHADER_RESOURCE;
                s_rc_screen_copy_tex = gpu_texture_create(s_rc_dev, &td);
            }
            if (GPU_VALID(s_rc_screen_copy_tex)) {
                /* The barriers and the copy on the frame's recorder (step C5b);
                 * s_rc_rt_tex[fi] is the display target. */
                gpu_barrier bb[2];
                memset(bb, 0, sizeof bb);
                bb[0].type    = GPU_BARRIER_TRANSITION;
                bb[0].texture = s_rc_rt_tex[fi];
                bb[0].sub     = GPU_SUB_ALL;
                bb[0].before  = GPU_STATE_RENDER_TARGET;
                bb[0].after   = GPU_STATE_COPY_SOURCE;
                bb[1] = bb[0];
                bb[1].texture = s_rc_screen_copy_tex;
                bb[1].before  = GPU_STATE_PIXEL_SHADER_RESOURCE;
                bb[1].after   = GPU_STATE_COPY_DEST;
                gpu_cmd_barrier_n(s_rc_cmd, 2, bb);

                gpu_cmd_copy_texture_whole(s_rc_cmd, s_rc_screen_copy_tex, s_rc_rt_tex[fi]);

                bb[0].before = GPU_STATE_COPY_SOURCE;
                bb[0].after  = GPU_STATE_RENDER_TARGET;
                bb[1].before = GPU_STATE_COPY_DEST;
                bb[1].after  = GPU_STATE_PIXEL_SHADER_RESOURCE;
                gpu_cmd_barrier_n(s_rc_cmd, 2, bb);

                /* SCREENCOPY_DUMP=1: stage the captured image into the readback
                 * buffer so it can be written out and LOOKED AT. Claiming a
                 * capture happened is not the same as showing what is in it.
                 * (Step C8a: the copy into the dump's readback buffer is
                 * gpu_cmd_copy_tex_to_buf's, today's placed footprint at 0.) */
                { static int sd = -1;
                  PS3_KNOB(sd, ps3_env_int("SCREENCOPY_DUMP", 0));
                  if (sd && GPU_VALID(s_rc_readback_buf)) {
                      gpu_cmd_barrier(s_rc_cmd, s_rc_screen_copy_tex, GPU_STATE_PIXEL_SHADER_RESOURCE,
                                      GPU_STATE_COPY_SOURCE);

                      gpu_footprint cd;
                      memset(&cd, 0, sizeof cd);
                      cd.format    = GPU_FORMAT_R8G8B8A8_UNORM;
                      cd.width     = s_d3d.host_w;
                      cd.height    = s_d3d.host_h;
                      cd.depth     = 1;
                      cd.row_pitch = s_d3d.readback_pitch;
                      gpu_cmd_copy_tex_to_buf(s_rc_cmd, s_rc_readback_buf, &cd, s_rc_screen_copy_tex, 0, NULL);

                      gpu_cmd_barrier(s_rc_cmd, s_rc_screen_copy_tex, GPU_STATE_COPY_SOURCE,
                                      GPU_STATE_PIXEL_SHADER_RESOURCE);
                      s_sc_dump_pending = 1;
                  } }
            }

}

/* GCM_GUEST_FB=<off>,<w>,<h>,<pitch> -- present the guest display buffer.
 *
 * A title that composites its final image with the RSX 2D engine assembles it
 * in GUEST memory: nv3089_blit is a CPU copy and never touches D3D12, and the
 * draw pass below only runs when a recorded draw targets the display surface,
 * which for such a title never happens. The frame is finished, in the wrong
 * memory, and the swapchain shows only the clear. Copy it across.
 *
 * Off unless the offset and geometry are given, so nothing else changes. */
/* GPU-side composite of an offscreen RT onto the backbuffer.
 *
 * A title that finishes its frame with the RSX 2D engine blits a rendered
 * surface into the display buffer. That surface lives in guest RAM, which the
 * D3D12 path never writes -- the scene is on the GPU -- so the CPU copy in
 * nv3089_blit faithfully moves zeros. The backend already holds the same
 * surface as a texture, so do the composite here instead. Triggered from
 * cellGcmSys under GCM_COMPOSITE_RT. */
static u32 s_composite_src = 0;
static int s_last_drawn_slot = -1;
static u32 s_last_drawn_off  = 0;

static void rc_composite_from_offscreen(u32 raw_off) { s_composite_src = raw_off; }

static void composite_present(u32 fi)
{
    u32 want = s_composite_src;
    s_composite_src = 0;
    if (!want) return;
    /* want == 2 means "whatever this frame's draws targeted" -- the one thing
     * guaranteed to hold geometry, and immune to the offset collision above. */
    int i;
    if (want == 2u) { i = s_last_drawn_slot; want = s_last_drawn_off; }
    else            { i = off_rt_find(want); }
    if (i < 0 || !GPU_VALID(s_d3d.off_rt[i].tex)) return;
    OffRT* r = &s_d3d.off_rt[i];
    /* Same-format copy only; a half-float source would need a converting draw. */
    if (r->dxgi != GPU_FORMAT_R8G8B8A8_UNORM) {
        static int w1 = 0;
        if (w1 < 2 && ++w1)
            fprintf(stderr, "[COMPOSITE] RT 0x%08X is dxgi=%u, not R8G8B8A8 -- needs a draw%c",
                    want, r->dxgi, 10);
        return;
    }
    u32 cw = r->hw < s_d3d.host_w ? r->hw : s_d3d.host_w;
    u32 ch = r->hh < s_d3d.host_h ? r->hh : s_d3d.host_h;
    if (!cw || !ch) return;
    { static int n = 0;
      if (n < 3 && ++n) fprintf(stderr, "[COMPOSITE] RT 0x%08X %ux%u -> backbuffer%c",
                           want, cw, ch, 10); }

    /* On the frame's recorder (step C5b): r->tex is the target's handle,
     * s_rc_rt_tex[fi] the display target's. */
    gpu_barrier b[2];
    memset(b, 0, sizeof b);
    b[0].type    = GPU_BARRIER_TRANSITION;
    b[0].texture = r->tex;
    b[0].sub     = GPU_SUB_ALL;
    b[0].before  = r->st;
    b[0].after   = GPU_STATE_COPY_SOURCE;
    b[1] = b[0];
    b[1].texture = s_rc_rt_tex[fi];
    b[1].before  = GPU_STATE_RENDER_TARGET;
    b[1].after   = GPU_STATE_COPY_DEST;
    gpu_cmd_barrier_n(s_rc_cmd, 2, b);

    const gpu_box box = {0, 0, 0, cw, ch, 1};
    gpu_cmd_copy_texture(s_rc_cmd, s_rc_rt_tex[fi], 0, 0, 0, 0, r->tex, 0, &box);

    b[0].before = GPU_STATE_COPY_SOURCE;
    b[0].after  = r->st;
    b[1].before = GPU_STATE_COPY_DEST;
    b[1].after  = GPU_STATE_RENDER_TARGET;
    gpu_cmd_barrier_n(s_rc_cmd, 2, b);
}


static void guest_fb_present(u32 fi)
{
    static int inited = 0;
    static u32 off, gw, gh, gpitch, up_pitch;
    /* Step C2: the upload buffers, mapped for their life, one per parity
     * half (RSX_FRAMES_IN_FLIGHT: the last batch may still copy from its). */
    static gpu_buffer up_bufs[2];
    if (!inited) {
        inited = 1;
        const char* e = getenv("GCM_GUEST_FB");
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (!e || sscanf(e, "%i,%u,%u,%u", &a, &b, &c, &d) != 4 || !b || !c) return;
        off = a; gw = b; gh = c; gpitch = d;
        up_pitch = (gw * 4u + 255u) & ~255u;      /* D3D12 wants 256-byte rows */
        for (int k = 0; k < 2; k++)   /* none if none */
            rc_buf_make((u64)up_pitch * gh, GPU_USE_COPY_SRC, GPU_MEM_UPLOAD, &up_bufs[k], NULL);
        fprintf(stderr, "[GUESTFB] presenting guest 0x%08X %ux%u pitch %u -> backbuffer%c",
                off, gw, gh, gpitch, 10);
    }
    const gpu_buffer up_buf = up_bufs[s_rf_par];
    if (!GPU_VALID(up_buf)) return;
    { extern u8* vm_base;
      if (!vm_base) return;
      /* RSX local memory is mapped at guest 0xC0000000. */
      const u8* srow = vm_base + 0xC0000000u + off;
      void* m = gpu_buffer_map(s_rc_dev, up_buf, 0, 0);
      if (!m) return;
      for (u32 y = 0; y < gh; y++) {
          const u8* s = srow + (size_t)y * gpitch;
          u8* dp = (u8*)m + (size_t)y * up_pitch;
          for (u32 x = 0; x < gw; x++) {   /* guest B,G,R,A -> backbuffer R,G,B,A */
              dp[x*4+0] = s[x*4+2]; dp[x*4+1] = s[x*4+1];
              dp[x*4+2] = s[x*4+0]; dp[x*4+3] = 0xFF;
          }
      }
      gpu_buffer_unmap(s_rc_dev, up_buf, 0, (u64)up_pitch * gh); }

    /* The barriers and the copy on the frame's recorder (step C5b; the copy
     * from the upload buffer step C8a's: today's placed footprint at 0). */
    gpu_cmd_barrier(s_rc_cmd, s_rc_rt_tex[fi], GPU_STATE_RENDER_TARGET, GPU_STATE_COPY_DEST);

    gpu_footprint sl;
    memset(&sl, 0, sizeof sl);
    sl.format    = GPU_FORMAT_R8G8B8A8_UNORM;
    sl.width     = gw;
    sl.height    = gh;
    sl.depth     = 1;
    sl.row_pitch = up_pitch;
    gpu_cmd_copy_buf_to_tex(s_rc_cmd, s_rc_rt_tex[fi], 0, 0, 0, 0, up_buf, &sl);

    gpu_cmd_barrier(s_rc_cmd, s_rc_rt_tex[fi], GPU_STATE_COPY_DEST, GPU_STATE_RENDER_TARGET);
}


/* The VP index buffer (indexed strip uploads, upload_strip_vp_indexed): u32
 * indices, two parity halves like vp_vb. Created at the first frame end and
 * grown there as vp_vb is; until it exists, or once a frame fills it, the
 * uploads expand to unindexed lists instead.
 *
 * It starts at 4M indices (32 MB for both halves; VP_IB_INDICES sets it). A
 * load into the 1-2 bridge wants up to 3.7M in one frame, and the fetch path
 * needs room here too: at 1M (legacy's size) that frame's draws went down the
 * CPU path, unindexed, and vp_vb kept the 9 chunks they took (576 MB). */
static void vp_ib_grow(u64 want)
{
    const u64 limit = 64ull << 20;          /* 512 MB of upload heap */
    static int start = -1;
    PS3_KNOB(start, ps3_env_int("VP_IB_INDICES", 4 << 20));
    u64 cap = s_vp_icap ? s_vp_icap : (u64)(start > 0 ? start : 4 << 20);
    while (cap < want + want / 8 && cap < limit) cap *= 2;
    if (cap > limit) cap = limit;
    if (GPU_VALID(s_rc_vp_ib) && cap <= s_vp_icap) return;
    const u64 bytes = cap * 4 * 2;
    gpu_buffer nb; void* mp = NULL;
    if (!rc_buf_make(bytes, GPU_USE_INDEX, GPU_MEM_UPLOAD, &nb, &mp) || !mp) {
        if (GPU_VALID(nb)) gpu_buffer_destroy(s_rc_dev, nb, 0);   /* made, not mapped: never used */
        static int said = 0;
        if (!said++) fprintf(stderr, "[VPIB] index buffer of %llu indices failed%c",
                             (unsigned long long)cap, 10);
        return;
    }
    rc_ring_retire(1, s_rc_vp_ib);   /* (none the first time) */
    s_rc_vp_ib = nb;
    s_d3d.vp_ib_mapped = mp; s_vp_icap = (u32)cap;
    fprintf(stderr, "[VPIB] index buffer: %u indices (%llu MB)%c",
            s_vp_icap, (unsigned long long)(bytes >> 20), 10);
}

