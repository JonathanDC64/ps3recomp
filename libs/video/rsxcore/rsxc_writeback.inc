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

/* RSX_SCALE: the write-back of a scaled target.
 *
 * A target drawn at a multiple of its guest size is drawn down to a guest-size
 * copy of the same format (one texel of each guest texel's block, Load), and
 * that copy is read back like any other target. Without it the guest memory
 * under a scaled target kept whatever was there before, and a target created
 * later at that offset was filled from it: the Profile List showed coloured
 * noise below its footer, where it draws nothing over a recreated target. */
static ID3D12RootSignature*  s_down_rs;
static ID3D12PipelineState*  s_down_pso[8];
static u32                   s_down_pso_fmt[8];   /* format, | 1 << 31 for guest byte order */
static ID3D12DescriptorHeap* s_down_srv;   /* shader visible, 2 x MAX_OFF_RTS (frame parity) */
static ID3D12DescriptorHeap* s_down_rtv;
static ID3DBlob*             s_down_vs;
static ID3DBlob*             s_down_ps;
static ID3DBlob*             s_down_ps_argb;   /* A,R,G,B bytes: the guest's A8R8G8B8 */

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
    ID3DBlob* err = NULL;
    if (FAILED(sc_compile(GPU_SC_VS, vs, sizeof vs - 1, "down_vs", NULL, GPU_SC_NO_CACHE, &s_down_vs, &err)) ||
        FAILED(sc_compile(GPU_SC_PS, ps, sizeof ps - 1, "down_ps", NULL, GPU_SC_NO_CACHE, &s_down_ps, &err)) ||
        FAILED(sc_compile(GPU_SC_PS, ps_argb, sizeof ps_argb - 1, "down_ps_argb", NULL, GPU_SC_NO_CACHE,
                          &s_down_ps_argb, &err))) {
        if (err) { fprintf(stderr, "[D3D12] write-back downscale: %s%c", (const char*)err->lpVtbl->GetBufferPointer(err), 10);
                   err->lpVtbl->Release(err); }
        return 0;
    }
    D3D12_DESCRIPTOR_RANGE range = {0};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp[2] = {0};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &range;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 2;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    rd.NumParameters = 2; rd.pParameters = rp;
    ID3DBlob* sig = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        if (err) err->lpVtbl->Release(err);
        return 0;
    }
    HRESULT hr = s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0,
        sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
        &IID_ID3D12RootSignature, (void**)&s_down_rs);
    sig->lpVtbl->Release(sig);
    if (FAILED(hr)) return 0;
    {   /* the GPU layer's table pool, a 1-entry table each (step C4) */
        gpu_table_pool_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.kind = GPU_TABLE_TEX;
        pd.count = 2 * MAX_OFF_RTS;
        pd.nregions = 1;
        pd.region[0].first = 0; pd.region[0].tables = 2 * MAX_OFF_RTS; pd.region[0].size = 1;
        s_rc_down_pool = gpu_table_pool_create(s_rc_dev, &pd);
        s_down_srv = gpu_d3d12_table_pool_heap(s_rc_dev, s_rc_down_pool);
        if (!s_down_srv) return 0;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = MAX_OFF_RTS;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd,
            &IID_ID3D12DescriptorHeap, (void**)&s_down_rtv))) return 0;
    state = 1;
    return 1;
}

static ID3D12PipelineState* down_pso(DXGI_FORMAT f, int argb)
{
    const u32 key = (u32)f | (argb ? 1u << 31 : 0u);
    for (int i = 0; i < 8; i++)
        if (s_down_pso[i] && s_down_pso_fmt[i] == key) return s_down_pso[i];
    int slot = -1;
    for (int i = 0; i < 8; i++) if (!s_down_pso[i]) { slot = i; break; }
    if (slot < 0) return NULL;
    ID3DBlob* const ps = argb ? s_down_ps_argb : s_down_ps;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_down_rs;
    pd.VS.pShaderBytecode = s_down_vs->lpVtbl->GetBufferPointer(s_down_vs);
    pd.VS.BytecodeLength  = s_down_vs->lpVtbl->GetBufferSize(s_down_vs);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    pd.PS.BytecodeLength  = ps->lpVtbl->GetBufferSize(ps);
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = f;
    pd.SampleDesc.Count = 1;
    if (FAILED(s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd,
            &IID_ID3D12PipelineState, (void**)&s_down_pso[slot]))) { s_down_pso[slot] = NULL; return NULL; }
    s_down_pso_fmt[slot] = key;
    return s_down_pso[slot];
}

/* Draw scaled target i down to its guest size; the copy is left in COPY_SOURCE.
 * `argb`: in the guest's byte order (an R8G8B8A8 copy holding A,R,G,B).
 * Returns NULL if that cannot be done (the target is then not written back). */
static ID3D12Resource* off_rt_downscale(int i, int argb)
{
    OffRT* r = &s_d3d.off_rt[i];
    if (!down_init()) return NULL;
    ID3D12PipelineState* pso = down_pso((DXGI_FORMAT)r->gdxgi, argb);
    if (!pso) return NULL;
    if (r->down && (r->down_w != r->w || r->down_h != r->h || r->down_dxgi != r->gdxgi)) {
        off_rt_defer_release(r->down); r->down = NULL;
    }
    if (!r->down) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = r->w; td.Height = r->h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = (DXGI_FORMAT)r->gdxgi; td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(d3d_create_committed(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                &IID_ID3D12Resource, (void**)&r->down)) || !r->down) { r->down = NULL; return NULL; }
        r->down_w = r->w; r->down_h = r->h; r->down_dxgi = r->gdxgi;
        r->down_st = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    off_rt_transition(i, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (r->down_st != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r->down;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = r->down_st;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->lpVtbl->ResourceBarrier(cl, 1, &b);
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
    D3D12_GPU_DESCRIPTOR_HANDLE sg;
    sg.ptr = gpu_table_ref(s_rc_dev, s_rc_down_pool, di).v;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    s_down_rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_down_rtv, &rtv);
    rtv.ptr += (SIZE_T)i * s_d3d.rtv_descriptor_size;
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, r->down, NULL, rtv);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_down_rs);
    cl->lpVtbl->SetPipelineState(cl, pso);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_down_srv);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, sg);
    const float ratio[2] = {(float)r->hw / (float)r->w, (float)r->hh / (float)r->h};
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 2, ratio, 0);
    D3D12_VIEWPORT vp = {0, 0, (float)r->w, (float)r->h, 0.0f, 1.0f};
    D3D12_RECT rc = {0, 0, (LONG)r->w, (LONG)r->h};
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &rc);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r->down;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->lpVtbl->ResourceBarrier(cl, 1, &b);
    r->down_st = D3D12_RESOURCE_STATE_COPY_SOURCE;
    return r->down;
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
static struct { int state; ID3D12Heap* heap; ID3D12Resource* buf; } s_wbg;   /* state: 0 untried, 1 ready, -1 off */
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
    ID3D12Device3* d3 = NULL;
    if (FAILED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12Device3, (void**)&d3)) || !d3)
        return 0;
    HRESULT hr = d3->lpVtbl->OpenExistingHeapFromAddress(d3, win, &IID_ID3D12Heap, (void**)&s_wbg.heap);
    d3->lpVtbl->Release(d3);
    if (FAILED(hr) || !s_wbg.heap) {
        fprintf(stderr, "[D3D12] write-back: no heap on guest memory (0x%08lX); stores on the CPU%c", (long)hr, 10);
        s_wbg.heap = NULL;
        return 0;
    }
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = WBG_SIZE; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    hr = s_d3d.device->lpVtbl->CreatePlacedResource(s_d3d.device, s_wbg.heap, 0, &bd,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_wbg.buf);
    if (FAILED(hr) || !s_wbg.buf) {
        fprintf(stderr, "[D3D12] write-back: no buffer on guest memory (0x%08lX); stores on the CPU%c", (long)hr, 10);
        s_wbg.buf = NULL; s_wbg.heap->lpVtbl->Release(s_wbg.heap); s_wbg.heap = NULL;
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
    ID3D12Resource* from = off_rt_downscale(i, 1);
    if (!from) return 0;
    D3D12_TEXTURE_COPY_LOCATION to = {0}, src = {0};
    to.pResource = s_wbg.buf;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Offset = dst - WBG_BASE;
    to.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    to.PlacedFootprint.Footprint.Width    = r->w;
    to.PlacedFootprint.Footprint.Height   = r->h;
    to.PlacedFootprint.Footprint.Depth    = 1;
    to.PlacedFootprint.Footprint.RowPitch = gp;
    src.pResource = from;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &to, 0, 0, 0, &src, NULL);
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
        if (!r->res || !r->used) continue;
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
        if (r->rb && r->rb_size < need) { r->rb->lpVtbl->Release(r->rb); r->rb = NULL; }
        if (!r->rb) {
            D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd = {0};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = need; rd.Height = 1; rd.DepthOrArraySize = 1;
            rd.MipLevels = 1; rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(d3d_create_committed(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                    &IID_ID3D12Resource, (void**)&r->rb)) || !r->rb) {
                r->rb = NULL; continue;
            }
            r->rb_size = need;
        }
        D3D12_RESOURCE_STATES prior = r->st;
        ID3D12Resource* from = r->res;
        if (scaled) { if (!(from = off_rt_downscale(i, 0))) continue; }
        else off_rt_transition(i, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = r->rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format   = (DXGI_FORMAT)r->gdxgi;
        dst.PlacedFootprint.Footprint.Width    = r->w;
        dst.PlacedFootprint.Footprint.Height   = r->h;
        dst.PlacedFootprint.Footprint.Depth    = 1;
        dst.PlacedFootprint.Footprint.RowPitch = pitch;
        src.pResource = from;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
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
    void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)pitch * r->h};
    if (!tmp || FAILED(r->rb->lpVtbl->Map(r->rb, 0, &rr, &mp)) || !mp) return;
    u32 bad_y = ~0u, bad_x = 0;
    for (u32 y = 0; y < r->h && bad_y == ~0u; y++) {
        wb_conv_row(tmp, (const u8*)mp + (u64)y * pitch, r->w, 0);
        const u8* g = vm_base + r->gpu_dst + (u64)y * gp;
        if (memcmp(tmp, g, row))
            for (u32 x = 0; x < row; x++) if (tmp[x] != g[x]) { bad_y = y; bad_x = x; break; }
    }
    D3D12_RANGE nw = {0, 0};
    r->rb->lpVtbl->Unmap(r->rb, 0, &nw);
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
        if (r->res && r->used == 3) {   /* the GPU stored it (wbg_record) */
            r->used = 1;
            r->gaddr = r->gpu_dst;
            r->glen = r->gpitch * r->h;
            r->gsum_new = 1;
            continue;
        }
        if (!r->res || !r->rb || r->used != 2) continue;
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
        void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)pitch * r->h};
        if (FAILED(r->rb->lpVtbl->Map(r->rb, 0, &rr, &mp)) || !mp) continue;
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
                if (!nx) { D3D12_RANGE nw0 = {0, 0}; r->rb->lpVtbl->Unmap(r->rb, 0, &nw0); continue; }
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
        D3D12_RANGE nw = {0, 0};
        r->rb->lpVtbl->Unmap(r->rb, 0, &nw);
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
        if (!r->res || !r->gsum_ok) continue;
        if (tex_csum(vm_base + r->gaddr, r->glen) == r->gsum) continue;
        { static int n = 0;
          if (n < 40 && ++n) fprintf(stderr, "[RT] 0x%X (%ux%u) rewritten by the title: "
                                "dropped (frame %llu)%c", r->off, r->w, r->h,
                                (unsigned long long)s_d3d.frame_count, 10); }
        off_rt_defer_release(r->res);  r->res = NULL;
        off_rt_defer_release(r->up);   r->up = NULL;
        off_rt_defer_release(r->snap); r->snap = NULL;
        off_rt_defer_release(r->down); r->down = NULL;
        rc_tex_drop(&r->tex); rc_tex_drop(&r->snap_tex);   /* step C4 */
        r->gsum_ok = 0;
    }
}

static D3D12_CPU_DESCRIPTOR_HANDLE off_rt_rtv(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    s_d3d.rt_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return rh;
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
 * a write calls out. */
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
    gpu_fast_table_write_tex(s_rc_dev, s_rc_srv_pool, heap_slot, &sv);
}

static void srv_write(u32 heap_slot, gpu_texture tex, DXGI_FORMAT fmt, UINT mapping)
{
    srv_write_ex(heap_slot, tex, fmt, mapping, 0);
}

static void off_rt_transition(int slot, D3D12_RESOURCE_STATES to)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (r->st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r->res;
    b.Transition.StateBefore = r->st;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    r->st = to;
}

/* A draw that samples a surface it is also rendering to. RSX allows it, and
 * Demon's Souls relies on it: its exposure adaptation (a 16x16 F_X32 target)
 * reads the previous exposure from the target it overwrites, each pixel its
 * own texel. D3D12 cannot have one resource bound as RTV and SRV at once; that
 * read returned 0, so the exposure never moved past one adaptation step from 0
 * and the level drew almost black. Copy the target just before the draw and
 * return the copy for the draw to sample: the contents RSX would have read. */
static ID3D12Resource* off_rt_snapshot(int slot)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (!r->res) return NULL;
    if (r->snap && (r->snap_w != r->hw || r->snap_h != r->hh || r->snap_dxgi != r->dxgi)) {
        off_rt_defer_release(r->snap);   /* the open list may still use it */
        r->snap = NULL;
        rc_tex_drop(&r->snap_tex);   /* step C4 */
    }
    if (!r->snap) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = r->hw; td.Height = r->hh; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = (DXGI_FORMAT)r->dxgi; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(d3d_create_committed(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void**)&r->snap)))
            return NULL;
        rc_tex_set(&r->snap_tex, r->snap);   /* step C4: sampled (the SRV writes) */
        r->snap_w = r->hw; r->snap_h = r->hh; r->snap_dxgi = r->dxgi;
        r->snap_st = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    const D3D12_RESOURCE_STATES back = r->st;
    off_rt_transition(slot, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (r->snap_st != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = r->snap;
        b.Transition.StateBefore = r->snap_st;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list, r->snap, r->res);
    {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = r->snap;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
        r->snap_st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    off_rt_transition(slot, back);
    return r->snap;
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

/* Copy the current backbuffer into s_screen_copy. Called mid-frame the moment
 * the reduced-viewport passes finish -- this title renders its reflection into
 * the TOP-LEFT REGION of the render target and the main scene then overwrites
 * it, so a snapshot taken at end of frame contains the scene, not the
 * reflection. Also called at end of frame as a fallback for frames with no
 * reduced-viewport pass. */
static void screen_copy_capture(u32 fi)
{
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("SCREEN_AS_TEX", 1));
    if (!en || !s_d3d.device) return;
            if (!s_screen_copy) {
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC td = {0};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = s_d3d.host_w; td.Height = s_d3d.host_h;
                td.DepthOrArraySize = 1; td.MipLevels = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
                td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                if (FAILED(d3d_create_committed(
                        s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                        &IID_ID3D12Resource, (void**)&s_screen_copy)))
                    s_screen_copy = NULL;
                else
                    rc_tex_set(&s_rc_screen_copy_tex, s_screen_copy);   /* step C4: sampled (the SRV writes) */
            }
            if (s_screen_copy) {
                D3D12_RESOURCE_BARRIER bb[2] = {0};
                bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb[0].Transition.pResource   = s_d3d.render_targets[fi];
                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb[1] = bb[0];
                bb[1].Transition.pResource   = s_screen_copy;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list,
                    s_screen_copy, s_d3d.render_targets[fi]);

                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                /* SCREENCOPY_DUMP=1: stage the captured image into the readback
                 * buffer so it can be written out and LOOKED AT. Claiming a
                 * capture happened is not the same as showing what is in it. */
                { static int sd = -1;
                  PS3_KNOB(sd, ps3_env_int("SCREENCOPY_DUMP", 0));
                  if (sd && s_d3d.readback_buf) {
                      D3D12_RESOURCE_BARRIER t = {0};
                      t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                      t.Transition.pResource   = s_screen_copy;
                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);

                      D3D12_TEXTURE_COPY_LOCATION cd = {0}, cs = {0};
                      cd.pResource = s_d3d.readback_buf;
                      cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                      cd.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
                      cd.PlacedFootprint.Footprint.Width    = s_d3d.host_w;
                      cd.PlacedFootprint.Footprint.Height   = s_d3d.host_h;
                      cd.PlacedFootprint.Footprint.Depth    = 1;
                      cd.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
                      cs.pResource = s_screen_copy;
                      cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                      s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cd, 0, 0, 0, &cs, NULL);

                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);
                      s_sc_dump_pending = 1;
                  } }
            }

}

/* rsx_topology -> D3D_PRIMITIVE_TOPOLOGY. Spelled out rather than relying on
 * the two enums happening to agree: the draw record carries the neutral value
 * so a second backend can read it, and this is the one place it becomes a D3D
 * one. */
static D3D12_PRIMITIVE_TOPOLOGY topo_to_d3d(u32 t)
{
    switch (t) {
    case RSX_TOPOLOGY_POINTS:         return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case RSX_TOPOLOGY_LINES:          return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case RSX_TOPOLOGY_LINE_STRIP:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case RSX_TOPOLOGY_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case RSX_TOPOLOGY_TRIANGLES:      return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:                          return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
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
    if (i < 0 || !s_d3d.off_rt[i].res) return;
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

    D3D12_RESOURCE_BARRIER b[2] = {0};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource   = r->res;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = r->st;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1] = b[0];
    b[1].Transition.pResource   = s_d3d.render_targets[fi];
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, b);

    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = s_d3d.render_targets[fi];
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
    sl.pResource = r->res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
    D3D12_BOX box = {0};
    box.left = 0; box.top = 0; box.front = 0;
    box.right = cw; box.bottom = ch; box.back = 1;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl, 0, 0, 0, &sl, &box);

    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter  = r->st;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, b);
}


static void guest_fb_present(u32 fi)
{
    static int inited = 0;
    static u32 off, gw, gh, gpitch, up_pitch;
    static ID3D12Resource* up = NULL;
    if (!inited) {
        inited = 1;
        const char* e = getenv("GCM_GUEST_FB");
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (!e || sscanf(e, "%i,%u,%u,%u", &a, &b, &c, &d) != 4 || !b || !c) return;
        off = a; gw = b; gh = c; gpitch = d;
        up_pitch = (gw * 4u + 255u) & ~255u;      /* D3D12 wants 256-byte rows */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width  = (u64)up_pitch * gh; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(d3d_create_committed(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&up)))
            up = NULL;
        fprintf(stderr, "[GUESTFB] presenting guest 0x%08X %ux%u pitch %u -> backbuffer%c",
                off, gw, gh, gpitch, 10);
    }
    if (!up) return;
    { extern u8* vm_base;
      if (!vm_base) return;
      /* RSX local memory is mapped at guest 0xC0000000. */
      const u8* srow = vm_base + 0xC0000000u + off;
      void* m = NULL; D3D12_RANGE nr = {0, 0};
      if (FAILED(up->lpVtbl->Map(up, 0, &nr, &m)) || !m) return;
      for (u32 y = 0; y < gh; y++) {
          const u8* s = srow + (size_t)y * gpitch;
          u8* dp = (u8*)m + (size_t)y * up_pitch;
          for (u32 x = 0; x < gw; x++) {   /* guest B,G,R,A -> backbuffer R,G,B,A */
              dp[x*4+0] = s[x*4+2]; dp[x*4+1] = s[x*4+1];
              dp[x*4+2] = s[x*4+0]; dp[x*4+3] = 0xFF;
          }
      }
      D3D12_RANGE wr = {0, (SIZE_T)up_pitch * gh};
      up->lpVtbl->Unmap(up, 0, &wr); }

    D3D12_RESOURCE_BARRIER gb = {0};
    gb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    gb.Transition.pResource   = s_d3d.render_targets[fi];
    gb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    gb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    gb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &gb);

    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = s_d3d.render_targets[fi];
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
    sl.pResource = up; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Offset = 0;
    sl.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    sl.PlacedFootprint.Footprint.Width    = gw;
    sl.PlacedFootprint.Footprint.Height   = gh;
    sl.PlacedFootprint.Footprint.Depth    = 1;
    sl.PlacedFootprint.Footprint.RowPitch = up_pitch;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl, 0, 0, 0, &sl, NULL);

    gb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    gb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &gb);
}


/* Grow the VP vertex buffer to hold a frame of `want` vertices (with
 * headroom). The frame that overflowed has already lost its tail; the next
 * ones fit. The old buffer may still be read by the frame in flight, so it is
 * released with the deferred RT resources. VP_VB_MAX_VERTS caps the growth
 * (default 2M vertices = 1 GB of upload heap at 256 bytes x 2 parities). */
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
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Width = cap * 256 * 2;          /* VP_VERT_STRIDE, two parity halves */
    ID3D12Resource* nb = NULL; void* mp = NULL; D3D12_RANGE nr = {0, 0};
    if (FAILED(d3d_create_committed(s_d3d.device, &hp,
            D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&nb)) ||
        FAILED(nb->lpVtbl->Map(nb, 0, &nr, &mp)) || !mp) {
        if (nb) nb->lpVtbl->Release(nb);
        fprintf(stderr, "[VPVB] growing to %llu vertices failed; staying at %u%c",
                (unsigned long long)cap, s_vp_cap, 10);
        limit = s_vp_cap;
        return;
    }
    off_rt_defer_release(s_d3d.vp_vb);
    s_d3d.vp_vb = nb; s_d3d.vp_vb_mapped = mp; s_vp_cap = (u32)cap;
    fprintf(stderr, "[VPVB] a frame asked for %llu vertices: buffer grown to %u (%llu MB)%c",
            (unsigned long long)want, s_vp_cap, (unsigned long long)(bd.Width >> 20), 10);
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
    if (s_d3d.vp_ib && cap <= s_vp_icap) return;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Width = cap * 4 * 2;
    ID3D12Resource* nb = NULL; void* mp = NULL; D3D12_RANGE nr = {0, 0};
    if (FAILED(d3d_create_committed(s_d3d.device, &hp,
            D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&nb)) ||
        FAILED(nb->lpVtbl->Map(nb, 0, &nr, &mp)) || !mp) {
        if (nb) nb->lpVtbl->Release(nb);
        static int said = 0;
        if (!said++) fprintf(stderr, "[VPIB] index buffer of %llu indices failed%c",
                             (unsigned long long)cap, 10);
        return;
    }
    if (s_d3d.vp_ib) off_rt_defer_release(s_d3d.vp_ib);
    s_d3d.vp_ib = nb; s_d3d.vp_ib_mapped = mp; s_vp_icap = (u32)cap;
    fprintf(stderr, "[VPIB] index buffer: %u indices (%llu MB)%c",
            s_vp_icap, (unsigned long long)(bd.Width >> 20), 10);
}

