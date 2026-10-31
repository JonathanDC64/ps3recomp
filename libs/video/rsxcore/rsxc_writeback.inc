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
static void off_rt_transition(int slot, D3D12_RESOURCE_STATES to);
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
static int s_wb_pending;   /* copies recorded, store not yet done */
static double s_fl_wait_ms, s_fl_store_ms;   /* FRAME_LOG: frame-start wait, store */

static void rt_pitch_record(D3D12DrawRecord* dr)
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
    return dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u :
           dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16u : 4u;
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
    /* The shaders are the GPU layer's (step C3b), kept for down_pso's later
     * formats: never freed. */
    s_down_vs = sc_shader(GPU_SC_VS, vs, sizeof vs - 1, "down_vs", NULL, GPU_SC_NO_CACHE);
    if (GPU_VALID(s_down_vs))
        s_down_ps = sc_shader(GPU_SC_PS, ps, sizeof ps - 1, "down_ps", NULL, GPU_SC_NO_CACHE);
    if (GPU_VALID(s_down_ps))
        s_down_ps_argb = sc_shader(GPU_SC_PS, ps_argb, sizeof ps_argb - 1, "down_ps_argb", NULL, GPU_SC_NO_CACHE);
    if (!GPU_VALID(s_down_ps_argb)) {
        if (t_sc_msg[0]) fprintf(stderr, "[D3D12] write-back downscale: %s%c", t_sc_msg, 10);
        return 0;
    }
    /* The GPU layer's layout: the t0 table, b0 = 2 constants. (Call map G12:
     * today's range has offset 0, the GPU layer's OFFSET_APPEND; the first
     * range of its table, it lands at 0 either way, so only the serialized
     * bytes differ.) */
    gpu_layout_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.nslots = 2;
    ld.slot[0] = lay_slot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
    ld.slot[1] = lay_slot(GPU_SLOT_CONST32, 0, 2, GPU_VIS_PIXEL);
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

static gpu_pipeline down_pso(DXGI_FORMAT f, int argb)
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
    const gpu_pipeline pso = down_pso((DXGI_FORMAT)r->gdxgi, argb);
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
        r->down_st = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    gpu_cmd* const c = s_rc_cmd;
    off_rt_transition(i, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (r->down_st != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        gpu_cmd_barrier(c, r->down_tex, (gpu_state)r->down_st, GPU_STATE_RENDER_TARGET);
        r->down_st = D3D12_RESOURCE_STATE_RENDER_TARGET;
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
    r->down_st = D3D12_RESOURCE_STATE_COPY_SOURCE;
    return r->down_tex;
}

/* RSX_WB_GPU (live, default 1; Windows): the write-back of a linear A8R8G8B8
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
 * per mismatching target, and a count every 1024 stores). */
#define WBG_BASE 0xC0000000u   /* RSX local memory (cellGcm's localAddress) */
#define WBG_SIZE 0x10000000u
/* state: 0 untried, 1 ready, -1 off. buf: the GPU layer's buffer over RSX
 * local memory (step C5b: gpu_buffer_import_host, today's heap opened on the
 * guest's pages and the buffer placed on it). */
static struct { int state; gpu_buffer buf; } s_wbg;
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
     * C5b), in COPY_DEST. Without GPU_CAP_HOST_IMPORT (no ID3D12Device3) it
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
    return 0;   /* the heap needs RSX local memory as an allocation of its own (Windows placeholders) */
#endif
}

/* Target i straight into guest memory, if it can go that way: 1 recorded. */
static int wbg_record(int i)
{
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    OffRT* r = &s_d3d.off_rt[i];
    if (r->gdxgi != DXGI_FORMAT_R8G8B8A8_UNORM || ((r->gfmt >> 8) & 0xF) == 2 ||
        r->gdma == CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER || !wbg_ready()) return 0;
    { static int lg = -1; PS3_KNOB(lg, ps3_env_int("RSX_WRITE_COLOR_LOG", 0));
      if (lg) return 0; }   /* the log compares the CPU store's bytes */
    const u32 gp = r->gpitch, dst = cellGcmResolveLocated(1, r->off);
    if (dst < WBG_BASE || dst - WBG_BASE >= WBG_SIZE || ((dst - WBG_BASE) & 511u) || (gp & 255u) ||
        gp < r->w * 4u || gp > 0x10000 || !r->h ||
        (u64)(dst - WBG_BASE) + (u64)gp * (r->h - 1) + r->w * 4u > WBG_SIZE) return 0;
    D3D12_RESOURCE_STATES prior = r->st;
    const gpu_texture from = off_rt_downscale(i, 1);
    if (!GPU_VALID(from)) return 0;
    gpu_footprint to;   /* the target's place in the buffer over RSX local memory */
    memset(&to, 0, sizeof to);
    to.offset    = dst - WBG_BASE;
    to.format    = GPU_FORMAT_R8G8B8A8_UNORM;
    to.width     = r->w;
    to.height    = r->h;
    to.depth     = 1;
    to.row_pitch = gp;
    gpu_cmd_copy_tex_to_buf(s_rc_cmd, s_wbg.buf, &to, from, 0, NULL);
    off_rt_transition(i, prior);
    r->gpu_dst = dst;
    r->used = 3;   /* stored by the GPU: the store only takes its checksum */
    return 1;
}

/* Record the copies (before the frame's command list closes). */
static void rt_writeback_record(void)
{
    if (!write_color_on()) return;
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!GPU_VALID(r->tex) || !r->used) continue;
        r->gpu_check = 0;
        if (wbg_record(i)) {
            if (wbg_mode() != 2) continue;
            r->gpu_check = 1;   /* and the CPU store's copy, to compare */
        }
        /* A scaled target, or a promoted one (HDR), goes through the draw down
         * to the guest's size and format. */
        const int scaled = r->hw != r->w || r->hh != r->h || r->dxgi != r->gdxgi;
        u32 pitch = (r->w * rt_guest_bpp(r->gdxgi) + 255) & ~255u;
        u64 need = (u64)pitch * r->h;
        /* The readback buffer is the GPU layer's (step C5b; rb its resource,
         * borrowed). One too small goes now, as today: the GPU and the last
         * store are done with it (render_frame's wait came first). */
        if (GPU_VALID(r->rb_buf) && r->rb_size < need) {
            gpu_buffer_destroy(s_rc_dev, r->rb_buf, 0);
            r->rb_buf.id = 0;
        }
        if (!GPU_VALID(r->rb_buf)) {
            gpu_buffer_desc bd;
            memset(&bd, 0, sizeof bd);
            bd.size = need;
            bd.use = GPU_USE_COPY_DST;
            bd.memory = GPU_MEM_READBACK;
            bd.state = GPU_STATE_COPY_DEST;
            r->rb_buf = gpu_buffer_create(s_rc_dev, &bd);
            if (!GPU_VALID(r->rb_buf)) continue;
            r->rb_size = need;
        }
        D3D12_RESOURCE_STATES prior = r->st;
        gpu_texture from = r->tex;
        if (scaled) { if (!GPU_VALID(from = off_rt_downscale(i, 0))) continue; }
        else off_rt_transition(i, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu_footprint fp;
        memset(&fp, 0, sizeof fp);
        fp.format    = (gpu_format)r->gdxgi;
        fp.width     = r->w;
        fp.height    = r->h;
        fp.depth     = 1;
        fp.row_pitch = pitch;
        gpu_cmd_copy_tex_to_buf(s_rc_cmd, r->rb_buf, &fp, from, 0, NULL);
        off_rt_transition(i, prior);
        r->used = 2;   /* copy pending */
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
static void wbg_check(OffRT* r)
{
    extern uint8_t* vm_base;
    static u8* tmp; static size_t tmp_n;
    static unsigned long long n_ok, n_bad;
    const u32 pitch = (r->w * 4u + 255) & ~255u, gp = r->gpitch, row = r->w * 4u;
    if (tmp_n < (size_t)row) { free(tmp); tmp = (u8*)malloc(row); tmp_n = tmp ? row : 0; }
    void* mp = NULL;   /* (the GPU layer's map: today's Map(0, {0, n}), step C5b) */
    if (!tmp || !(mp = gpu_buffer_map(s_rc_dev, r->rb_buf, 0, (u64)pitch * r->h))) return;
    u32 bad_y = ~0u, bad_x = 0;
    for (u32 y = 0; y < r->h && bad_y == ~0u; y++) {
        wb_conv_row(tmp, (const u8*)mp + (u64)y * pitch, r->w, 0);
        const u8* g = vm_base + r->gpu_dst + (u64)y * gp;
        if (memcmp(tmp, g, row))
            for (u32 x = 0; x < row; x++) if (tmp[x] != g[x]) { bad_y = y; bad_x = x; break; }
    }
    gpu_buffer_unmap(s_rc_dev, r->rb_buf, 0, 0);   /* nothing written: Unmap(0, {0, 0}) */
    if (bad_y == ~0u) n_ok++;
    else {
        n_bad++;
        static int lines = 0;
        if (lines < 40 && ++lines) {
            const u8* g = vm_base + r->gpu_dst + (u64)bad_y * gp + (bad_x & ~3u);
            const u8* c = tmp + (bad_x & ~3u);
            fprintf(stderr, "[wb-gpu] 0x%08X %ux%u differs at row %u byte %u: gpu %02X%02X%02X%02X cpu %02X%02X%02X%02X%c",
                    r->gpu_dst, r->w, r->h, bad_y, bad_x, g[0], g[1], g[2], g[3], c[0], c[1], c[2], c[3], 10);
        }
    }
    if ((n_ok + n_bad) % 1024u == 0)
        fprintf(stderr, "[wb-gpu] check: %llu targets equal, %llu differ%c", n_ok, n_bad, 10);
    r->gaddr = r->gpu_dst;
    r->glen = gp * r->h;
    r->gsum_new = 1;
}

/* Store the copies in guest memory (after the GPU finished the frame). */
static u8* s_wc_shadow; static u32 s_wc_len;   /* RSX_WRITE_COLOR_LOG */
static void rt_writeback_store(void)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (!write_color_on() || !vm_base) return;
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (GPU_VALID(r->tex) && r->used == 3) {   /* the GPU stored it (wbg_record) */
            r->used = 1;
            r->gaddr = r->gpu_dst;
            r->glen = r->gpitch * r->h;
            r->gsum_new = 1;
            continue;
        }
        if (!GPU_VALID(r->tex) || !GPU_VALID(r->rb_buf) || r->used != 2) continue;
        r->used = 1;
        if (r->gpu_check) { wbg_check(r); continue; }
        u32 bpp = rt_guest_bpp(r->gdxgi);
        u32 pitch = (r->w * bpp + 255) & ~255u;
        u32 swz = ((r->gfmt >> 8) & 0xF) == 2;
        u32 lw = (r->gfmt >> 16) & 0xFF, lh = (r->gfmt >> 24) & 0xFF;
        u32 gp = r->gpitch;
        if (swz) { if ((1u << lw) < r->w || (1u << lh) < r->h) continue; }
        else if (gp < r->w * bpp || gp > 0x10000) continue;
        /* The surface's context DMA says which memory the offset is in: local
         * and IO offsets are overlapping number spaces, so the offset alone
         * cannot place a surface the title keeps in main memory. */
        u32 dst = cellGcmResolveLocated(r->gdma != CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER, r->off);
        if (!dst || dst == 0xFFFFFFFFu) continue;
        { static int lg = -1; PS3_KNOB(lg, ps3_env_int("RSX_WRITE_COLOR_LOG", 0));
          if (lg > 0) { lg--;
            /* Did anything else write the surface since our last store? */
            static u8* shadow[MAX_OFF_RTS]; static u32 shoff[MAX_OFF_RTS], shlen[MAX_OFF_RTS];
            u32 len = swz ? ((1u << lw) << lh) * bpp : gp * r->h;
            u32 ndiff = 0, first = 0;
            if (shadow[i] && shoff[i] == r->off && shlen[i] == len)
                for (u32 k = 0; k < len; k++)
                    if (shadow[i][k] != vm_base[dst + k]) { if (!ndiff) first = k; ndiff++; }
            fprintf(stderr, "[WRITE-COLOR] off=0x%X -> 0x%08X %ux%u fmt=0x%08X pitch=%u dma=0x%X%s foreign=%u first=+0x%X%c",
                    r->off, dst, r->w, r->h, r->gfmt, gp, r->gdma, swz ? " swz" : "", ndiff, first, 10);
            /* The guest ranges of the fragment programs drawn with (rsxc_fp_seen),
             * so a write-back that lands on live ucode is reported. */
            for (int q = 0, nq = rsxc_fp_seen(-1, NULL, NULL); q < nq; q++) {
                u32 fa = 0, fl = 0;
                rsxc_fp_seen(q, &fa, &fl);
                if (fa < dst + len && dst < fa + fl)
                    fprintf(stderr, "[WRITE-COLOR] !! surface 0x%X [0x%08X,+0x%X) overlaps FP ucode 0x%08X+0x%X%c",
                            r->off, dst, len, fa, fl, 10);
            }
            if (!shadow[i] || shlen[i] != len) { free(shadow[i]); shadow[i] = (u8*)malloc(len); shlen[i] = len; }
            shoff[i] = r->off;
            s_wc_shadow = shadow[i]; s_wc_len = len;
          } }
        /* The GPU layer's map and unmap (step C5b): today's Map(0, {0, n}) and
         * Unmap(0, {0, 0}). */
        void* mp = gpu_buffer_map(s_rc_dev, r->rb_buf, 0, (u64)pitch * r->h);
        if (!mp) continue;
        /* A word at a time: every format here is whole 32-bit words per
         * texel, converted the same way. RGBA8 R,G,B,A -> A,R,G,B is a rotate,
         * FP16 swaps the bytes of each half, 32-bit lanes swap all four.
         * Byte by byte, this was 3.5-6 ms of each Demon's Souls frame (about
         * 25 MB: a 1280x720 target several times per frame, and others). */
        const int kind = r->gdxgi == DXGI_FORMAT_R8G8B8A8_UNORM ? 0 :
                         r->gdxgi == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1 : 2;
        const u32 wpt = bpp / 4;                     /* words per texel */
        if (!swz) {
            for (u32 y = 0; y < r->h; y++)
                wb_conv_row(vm_base + dst + (u64)y * gp, (const u8*)mp + (u64)y * pitch,
                            r->w * wpt, kind);
        } else {
            /* rsx_swz_index(x, y) is the x bits' part OR the y bits' part. */
            static u32* xpart; static u32 xpart_n;
            if (xpart_n < r->w) {
                u32* nx = (u32*)realloc(xpart, (size_t)r->w * sizeof *nx);
                if (!nx) { gpu_buffer_unmap(s_rc_dev, r->rb_buf, 0, 0); continue; }
                xpart = nx; xpart_n = r->w;
            }
            for (u32 x = 0; x < r->w; x++) xpart[x] = rsx_swz_index(x, 0, lw, lh);
            for (u32 y = 0; y < r->h; y++) {
                const u32 ypart = rsx_swz_index(0, y, lw, lh);
                const u8* srow = (const u8*)mp + (u64)y * pitch;
                for (u32 x = 0; x < r->w; x++)
                    wb_conv_row(vm_base + dst + (u64)(xpart[x] | ypart) * bpp,
                                srow + (u64)x * bpp, wpt, kind);
            }
        }
        gpu_buffer_unmap(s_rc_dev, r->rb_buf, 0, 0);
        if (s_wc_shadow) { memcpy(s_wc_shadow, vm_base + dst, s_wc_len); s_wc_shadow = NULL; }
        r->gaddr = dst;
        r->glen = swz ? ((1u << lw) << lh) * bpp : gp * r->h;
        r->gsum_new = 1;
    }
    /* Checksums after every store, so targets that share memory do not
     * count each other's write-back as a foreign write. */
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!r->gsum_new) continue;
        r->gsum_new = 0;
        r->gsum = tex_csum(vm_base + r->gaddr, r->glen);
        r->gsum_ok = 1;
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
 * left as it is. The heap is only written after the GPU has finished the
 * previous frame (render_frame waits first), so skipping a write is safe.
 * The memo compares the whole view, the texture by its handle, and a handle
 * names one texture for the device's life, so no stamp is needed: today's
 * memo rewrote every window after any resource was made, and those rewrites
 * of an unchanged view are the only ones skipped as well. It is always on
 * (RSX_SRV_MEMO=0 is gone). Inline: the compare is gpu_d3d12_cmd.h's, only
 * a write calls out. Through the pool's writer, found once at init (step
 * C8a): the 16 calls of a draw look nothing up before the memo. */
static void srv_write_ex(u32 heap_slot, gpu_texture tex, DXGI_FORMAT fmt,
                         UINT mapping, int cube)
{
    gpu_view_desc sv;
    sv.texture = tex;
    sv.kind = GPU_VIEW_SRV;
    sv.dim = cube ? GPU_DIM_CUBE : GPU_DIM_2D;   /* a cube must match the TextureCube declaration in the HLSL */
    sv.flags = 0;
    sv.mips = 1;
    sv.format = (gpu_format)fmt;
    sv.swizzle = mapping;
    sv.mip = 0;
    sv.layer = 0;
    gpu_fast_table_writer_tex(s_rc_dev, s_rc_srv_w, heap_slot, &sv);
}

static void srv_write(u32 heap_slot, gpu_texture tex, DXGI_FORMAT fmt, UINT mapping)
{
    srv_write_ex(heap_slot, tex, fmt, mapping, 0);
}

/* On the frame's recorder (step C5b). Inline (gpu_d3d12_cmd.h), so a draw's
 * transition is today's one call. */
static void off_rt_transition(int slot, D3D12_RESOURCE_STATES to)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (r->st == to) return;
    gpu_cmd_barrier(s_rc_cmd, r->tex, (gpu_state)r->st, (gpu_state)to);
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
        r->snap_st = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    const D3D12_RESOURCE_STATES back = r->st;
    off_rt_transition(slot, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (r->snap_st != D3D12_RESOURCE_STATE_COPY_DEST)
        gpu_cmd_barrier(s_rc_cmd, r->snap_tex, (gpu_state)r->snap_st, GPU_STATE_COPY_DEST);
    gpu_cmd_copy_texture_whole(s_rc_cmd, r->snap_tex, r->tex);
    gpu_cmd_barrier(s_rc_cmd, r->snap_tex, GPU_STATE_COPY_DEST, GPU_STATE_PIXEL_SHADER_RESOURCE);
    r->snap_st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
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
    if (r->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM) {
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
    b[0].before  = (gpu_state)r->st;
    b[0].after   = GPU_STATE_COPY_SOURCE;
    b[1] = b[0];
    b[1].texture = s_rc_rt_tex[fi];
    b[1].before  = GPU_STATE_RENDER_TARGET;
    b[1].after   = GPU_STATE_COPY_DEST;
    gpu_cmd_barrier_n(s_rc_cmd, 2, b);

    const gpu_box box = {0, 0, 0, cw, ch, 1};
    gpu_cmd_copy_texture(s_rc_cmd, s_rc_rt_tex[fi], 0, 0, 0, 0, r->tex, 0, &box);

    b[0].before = GPU_STATE_COPY_SOURCE;
    b[0].after  = (gpu_state)r->st;
    b[1].before = GPU_STATE_COPY_DEST;
    b[1].after  = GPU_STATE_RENDER_TARGET;
    gpu_cmd_barrier_n(s_rc_cmd, 2, b);
}


static void guest_fb_present(u32 fi)
{
    static int inited = 0;
    static u32 off, gw, gh, gpitch, up_pitch;
    static gpu_buffer up_buf;   /* step C2: the upload buffer, mapped for its life */
    if (!inited) {
        inited = 1;
        const char* e = getenv("GCM_GUEST_FB");
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (!e || sscanf(e, "%i,%u,%u,%u", &a, &b, &c, &d) != 4 || !b || !c) return;
        off = a; gw = b; gh = c; gpitch = d;
        up_pitch = (gw * 4u + 255u) & ~255u;      /* D3D12 wants 256-byte rows */
        rc_buf_make((u64)up_pitch * gh, GPU_USE_COPY_SRC, GPU_MEM_UPLOAD, &up_buf, NULL);   /* none if none */
        fprintf(stderr, "[GUESTFB] presenting guest 0x%08X %ux%u pitch %u -> backbuffer%c",
                off, gw, gh, gpitch, 10);
    }
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


/* Grow the VP vertex buffer to hold a frame of `want` vertices (with
 * headroom). The frame that overflowed has already lost its tail; the next
 * ones fit. The old buffer may still be read by the frame in flight, so it is
 * destroyed with that frame's fence (rc_ring_retire). VP_VB_MAX_VERTS caps the
 * growth (default 2M vertices = 1 GB of upload heap at 256 bytes x 2
 * parities). */
/* Growing creates and maps a new upload buffer of hundreds of MB on the render
 * thread: 100-400 ms for Demon's Souls's level, one frame, whose overflowing
 * draws are dropped as well. A title that needs more starts bigger
 * (VP_VB_VERTS). */
static void vp_vb_grow(u64 want)
{
    static u64 limit = 0;
    if (!limit) { const char* e = getenv("VP_VB_MAX_VERTS");
                  limit = e ? strtoull(e, NULL, 0) : (2ull << 20);
                  if (limit < s_vp_cap) limit = s_vp_cap; }
    u64 cap = s_vp_cap;
    while (cap < want + want / 8 && cap < limit) cap *= 2;
    if (cap > limit) cap = limit;
    if (cap <= s_vp_cap) {
        static int warned = 0;
        if (!warned++) fprintf(stderr, "[VPVB] a frame needs %llu vertices; the buffer stays at"
                               " %u (VP_VB_MAX_VERTS); later draws are dropped%c",
                               (unsigned long long)want, s_vp_cap, 10);
        return;
    }
    const u64 bytes = cap * 256 * 2;   /* VP_VERT_STRIDE, two parity halves */
    gpu_buffer nb; void* mp = NULL;
    if (!rc_buf_make(bytes, GPU_USE_VERTEX, GPU_MEM_UPLOAD, &nb, &mp) || !mp) {
        if (GPU_VALID(nb)) gpu_buffer_destroy(s_rc_dev, nb, 0);   /* made, not mapped: never used */
        fprintf(stderr, "[VPVB] growing to %llu vertices failed; staying at %u%c",
                (unsigned long long)cap, s_vp_cap, 10);
        limit = s_vp_cap;
        return;
    }
    rc_ring_retire(0, s_rc_vp_vb);
    s_rc_vp_vb = nb;
    s_d3d.vp_vb_mapped = mp; s_vp_cap = (u32)cap;
    fprintf(stderr, "[VPVB] a frame asked for %llu vertices: buffer grown to %u (%llu MB)%c",
            (unsigned long long)want, s_vp_cap, (unsigned long long)(bytes >> 20), 10);
}

/* The VP index buffer (indexed strip uploads, upload_strip_vp_indexed): u32
 * indices, two parity halves like vp_vb. Created at the first frame end and
 * grown there as vp_vb is; until it exists, or once a frame fills it, the
 * uploads expand to unindexed lists instead. */
static void vp_ib_grow(u64 want)
{
    const u64 limit = 64ull << 20;          /* 512 MB of upload heap */
    u64 cap = s_vp_icap ? s_vp_icap : (1u << 20);
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

