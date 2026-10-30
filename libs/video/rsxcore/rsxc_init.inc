/* ---------------------------------------------------------------------------
 * D3D12 initialization
 * -----------------------------------------------------------------------*/

static void pso_lib_open(void);   /* pipeline library, defined with the shader cache */
static void shader_cache_warm(void);   /* the shader cache read ahead, defined with it */
static u64 s_vp_root_hash;        /* vp_root_sig's serialized bytes, part of every pipeline's name */
static u64 fnv_bytes(u64 h, const void* p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= ((const u8*)p)[i]; h *= 1099511628211ull; }
    return h;
}

/* Recording on a frame slot's recorder (the GPU layer's, one allocator and
 * list per slot): today's allocator and list Reset. s_rc_cmd is the recorder
 * for converted code; s_d3d.cmd_list becomes its list, so code that records
 * into s_d3d.cmd_list records into the one open now. */
static void frame_cmd_begin(u32 slot)
{
    s_rc_cmd = gpu_cmd_begin(s_rc_dev, slot);
    s_d3d.cmd_list = (ID3D12GraphicsCommandList*)gpu_native(s_rc_dev, GPU_NATIVE_CMD, (u64)(uintptr_t)s_rc_cmd);
}

/* Today's Close and ExecuteCommandLists of the open recorder. */
static void frame_cmd_submit(void)
{
    gpu_cmd_end(s_rc_dev, s_rc_cmd);
    gpu_submit(s_rc_dev, &s_rc_cmd, 1, NULL);
}

/* The display buffers and the shared depth buffer, at RSX_SCALE's size: made
 * by init_d3d12, and again by rsx_scale_apply when the scale changes. Their
 * descriptor heaps already exist. */
static int display_targets_make(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);

    /* The game's frame: textures of their own (see render_targets), in the
     * PRESENT (= COMMON) state between frames as the swap chain's were, so
     * every transition and readback of them is unchanged. */
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = s_d3d.host_w;
        rd.Height           = s_d3d.host_h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        HRESULT hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PRESENT, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.render_targets[i]);
        if (FAILED(hr)) return -1;
        rc_tex_set(&s_rc_rt_tex[i], s_d3d.render_targets[i]);   /* step C4: the blit's SRVs */

        s_d3d.device->lpVtbl->CreateRenderTargetView(
            s_d3d.device, s_d3d.render_targets[i], NULL, rtv_handle);
        rtv_handle.ptr += s_d3d.rtv_descriptor_size;
    }
    return 0;
}

static int depth_buffer_make(void)
{
    D3D12_HEAP_PROPERTIES heap_props = {0};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depth_desc = {0};
    depth_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    /* Sized to cover the LARGEST render target, not just the window:
     * D3D12 clips rasterization to the smallest bound attachment, and
     * this DSV is shared by every pass -- with a window-sized depth
     * buffer, draws into larger offscreen RTs (wave's 1920x1080 input
     * image) were silently cropped to the window rect. */
    depth_desc.Width              = rt_host_dim(2048, 2048, 2048);
    depth_desc.Height             = rt_host_dim(2048, 2048, 2048);
    depth_desc.DepthOrArraySize   = 1;
    depth_desc.MipLevels          = 1;
    depth_desc.Format             = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count   = 1;
    depth_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depth_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depth_clear = {0};
    depth_clear.Format                       = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_clear.DepthStencil.Depth           = 1.0f;
    depth_clear.DepthStencil.Stencil         = 0;

    HRESULT hr = d3d_create_committed(
        s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
        &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
        &IID_ID3D12Resource, (void**)&s_d3d.depth_buffer);
    if (FAILED(hr)) {
        printf("[D3D12] Depth buffer creation failed (0x%08lX)\n", hr);
        return -1;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
    dsv_desc.Format         = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_desc.ViewDimension  = D3D12_DSV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);
    s_d3d.device->lpVtbl->CreateDepthStencilView(
        s_d3d.device, s_d3d.depth_buffer, &dsv_desc, dsv_handle);
    return 0;
}

static int init_d3d12(u32 width, u32 height)
{
    HRESULT hr;

    /* The device, its queue, a recorder per frame slot and the fence: the GPU
     * layer's D3D12 backend, with today's choices. D3D12_DRED=1: Device
     * Removed Extended Data (auto-breadcrumbs, page faults, the draws'
     * markers), which gpu_fault_report prints once the device is lost.
     * CELLMARK_IGPU: the low-power adapter (for A/B testing a suspected
     * iGPU-driver stall); else the high-performance one, software adapters
     * skipped, the default adapter last. The debug layer is never enabled on
     * this route (D3D12_DBG and debug builds included). */
    {
        gpu_device_desc dd;
        memset(&dd, 0, sizeof dd);
        if (getenv("D3D12_DRED")) dd.flags |= GPU_DEVICE_BREADCRUMBS;
        if (getenv("CELLMARK_IGPU")) dd.flags |= GPU_DEVICE_LOW_POWER;
        dd.cmd_slots = FRAME_COUNT;
        s_rc_dev = gpu_d3d12_create(&dd);
        if (!s_rc_dev) return -1;
        s_d3d.device = (ID3D12Device*)gpu_native(s_rc_dev, GPU_NATIVE_DEVICE, 0);
        s_d3d.cmd_queue = (ID3D12CommandQueue*)gpu_native(s_rc_dev, GPU_NATIVE_QUEUE, 0);
    }

    pso_lib_open();
    shader_cache_warm();

    /* The swap chain: flip model, FRAME_COUNT buffers, Alt+Enter left to the
     * window. A windowed flip-model swap chain presenting with sync interval 0
     * is still paced by the compositor at the display's refresh unless it may
     * tear: it is made with ALLOW_TEARING when the system has it, and each such
     * Present passes DXGI_PRESENT_ALLOW_TEARING (RSX_VSYNC=0, swap_present). A
     * system without it (no variable refresh support in the driver or OS)
     * presents with vsync. */
    s_d3d.tearing = (s_rc_dev->caps.bits & GPU_CAP_TEARING) ? 1 : 0;
    {
        gpu_swapchain_desc sd;
        memset(&sd, 0, sizeof sd);
        sd.window = s_d3d.hwnd;
        sd.width = width;
        sd.height = height;
        sd.format = GPU_FORMAT_R8G8B8A8_UNORM;
        sd.buffers = FRAME_COUNT;
        sd.colour_space = GPU_COLOR_SRGB;
        sd.flags = s_d3d.tearing ? GPU_SWAP_TEARING : 0;
        s_rc_swap = gpu_swap_create(s_rc_dev, &sd);
        if (!GPU_VALID(s_rc_swap)) return -1;
        s_d3d.swap_chain = (IDXGISwapChain3*)gpu_native(s_rc_dev, GPU_NATIVE_SWAPCHAIN, s_rc_swap.id);
    }

    s_d3d.frame_index = gpu_swap_acquire(s_rc_dev, s_rc_swap, NULL);

    /* Create RTV descriptor heap */
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
    rtv_heap_desc.NumDescriptors = FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
        s_d3d.device, &rtv_heap_desc, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rtv_heap);
    if (FAILED(hr)) return -1;

    s_d3d.rtv_descriptor_size = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (display_targets_make() != 0) return -1;

    /* The window's buffers, with RTVs of their own. */
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.NumDescriptors = FRAME_COUNT;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
            s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.swap_rtv_heap);
        if (FAILED(hr)) return -1;
    }
    s_d3d.win_w = width;
    s_d3d.win_h = height;
    if (swap_buffers_get() != 0) return -1;
    if (blit_init() != 0) return -1;

    /* ---------------------------------------------------------------
     * Depth/stencil buffer
     * 24-bit depth + 8-bit stencil (DXGI_FORMAT_D24_UNORM_S8_UINT).
     * One shared depth texture across both frames — RSX games on PS3
     * typically use a single zeta surface.
     * ---------------------------------------------------------------*/
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {0};
        dsv_heap_desc.NumDescriptors = 1;
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
            s_d3d.device, &dsv_heap_desc, &IID_ID3D12DescriptorHeap,
            (void**)&s_d3d.dsv_heap);
        if (FAILED(hr)) {
            printf("[D3D12] DSV heap creation failed\n");
            return -1;
        }

        if (depth_buffer_make() != 0) return -1;

        printf("[D3D12] Depth buffer created (%ux%u D24S8)\n", width, height);
    }

    /* The recorders and the fence are the device's (one fence, the device's
     * timeline: s_rc_frame_fence holds each frame slot's point on it). Until
     * the first frame, s_d3d.cmd_list is slot 0's list, closed, as today's one
     * list is after init. */
    frame_cmd_begin(0);
    if (!s_rc_cmd) return -1;
    gpu_cmd_end(s_rc_dev, s_rc_cmd);
    memset(s_rc_frame_fence, 0, sizeof s_rc_frame_fence);

    /* ---------------------------------------------------------------
     * Create root signature with 16 root constants (one mat4 MVP at b0).
     * Visible only to the vertex shader — pixel shader doesn't need it.
     * ---------------------------------------------------------------*/
    {
        /* param 0: mat4 MVP as 16 root constants at b0 (vertex shader).
         * param 1: one SRV (t0) descriptor table for the atlas texture (pixel).
         * static sampler s0: linear clamp. The plain (untextured) PSO simply
         * never references t0/s0, so it ignores them. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors     = 1;
        srv_range.BaseShaderRegister = 0;   /* t0 */
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER root_params[2] = {0};
        root_params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_params[0].Constants.ShaderRegister = 0;   /* b0 */
        root_params[0].Constants.RegisterSpace  = 0;
        root_params[0].Constants.Num32BitValues = 16;  /* mat4 */
        root_params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
        root_params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_params[1].DescriptorTable.NumDescriptorRanges = 1;
        root_params[1].DescriptorTable.pDescriptorRanges   = &srv_range;
        root_params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = {0};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;   /* s0 */
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
        rs_desc.NumParameters     = 2;
        rs_desc.pParameters       = root_params;
        rs_desc.NumStaticSamplers = 1;
        rs_desc.pStaticSamplers   = &samp;
        rs_desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ID3DBlob* signature_blob = NULL;
        ID3DBlob* error_blob = NULL;
        hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &signature_blob, &error_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature serialization failed: %s\n",
                   error_blob ? (const char*)error_blob->lpVtbl->GetBufferPointer(error_blob) : "?");
            if (error_blob) error_blob->lpVtbl->Release(error_blob);
            return -1;
        }

        hr = s_d3d.device->lpVtbl->CreateRootSignature(
            s_d3d.device, 0,
            signature_blob->lpVtbl->GetBufferPointer(signature_blob),
            signature_blob->lpVtbl->GetBufferSize(signature_blob),
            &IID_ID3D12RootSignature, (void**)&s_d3d.root_signature);
        signature_blob->lpVtbl->Release(signature_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature creation failed\n");
            return -1;
        }
    }

    /* ---------------------------------------------------------------
     * Compile shaders and create PSO
     * ---------------------------------------------------------------*/
    {
        /* Basic vertex-colored shader.
         * The MVP matrix arrives via root constants as 4 vec4 columns
         * (PS3/OpenGL column-major convention). We multiply explicitly so
         * we don't depend on HLSL's matrix packing — matches PS3 semantics
         * `gl_Position = MVP * vec4(pos, 1.0)`. */
        static const char vs_hlsl[] =
            "cbuffer cb0 : register(b0) {\n"
            "    float4 mvp_col0;\n"
            "    float4 mvp_col1;\n"
            "    float4 mvp_col2;\n"
            "    float4 mvp_col3;\n"
            "};\n"
            "struct VSInput  { float3 pos : POSITION; float4 col : COLOR; };\n"
            "struct VSOutput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "VSOutput main(VSInput i) {\n"
            "    VSOutput o;\n"
            "    float4 p = float4(i.pos, 1.0);\n"
            "    o.pos = mvp_col0 * p.x + mvp_col1 * p.y + mvp_col2 * p.z + mvp_col3 * p.w;\n"
            "    o.col = i.col;\n"
            "    return o;\n"
            "}\n";
        static const char ps_hlsl[] =
            "struct PSInput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "float4 main(PSInput i) : SV_TARGET { return i.col; }\n";

        ID3DBlob* vs_blob = NULL;
        ID3DBlob* ps_blob = NULL;
        ID3DBlob* err = NULL;

        hr = sc_compile(GPU_SC_VS, vs_hlsl, sizeof(vs_hlsl) - 1, "vs_basic", NULL,
                        GPU_SC_NO_CACHE, &vs_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] VS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        hr = sc_compile(GPU_SC_PS, ps_hlsl, sizeof(ps_hlsl) - 1, "ps_basic", NULL,
                        GPU_SC_NO_CACHE, &ps_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] PS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        if (vs_blob && ps_blob) {
            D3D12_INPUT_ELEMENT_DESC input_layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
            pso_desc.pRootSignature = s_d3d.root_signature;
            pso_desc.VS.pShaderBytecode = vs_blob->lpVtbl->GetBufferPointer(vs_blob);
            pso_desc.VS.BytecodeLength = vs_blob->lpVtbl->GetBufferSize(vs_blob);
            pso_desc.PS.pShaderBytecode = ps_blob->lpVtbl->GetBufferPointer(ps_blob);
            pso_desc.PS.BytecodeLength = ps_blob->lpVtbl->GetBufferSize(ps_blob);
            pso_desc.InputLayout.pInputElementDescs = input_layout;
            pso_desc.InputLayout.NumElements = 3;
            pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;
            pso_desc.SampleMask = UINT_MAX;
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso_desc.NumRenderTargets = 1;
            pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            /* Depth test enabled, write enabled, LESS func.
             * Games with depth_test_enable=0 in RSX state can still render —
             * LESS just means new-z must be less than existing — but future
             * work should mirror RSX depth state into a PSO cache. */
            pso_desc.DepthStencilState.DepthEnable    = TRUE;
            pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pso_desc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            pso_desc.DepthStencilState.StencilEnable  = FALSE;
            pso_desc.SampleDesc.Count = 1;

            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state);
            if (SUCCEEDED(hr)) {
                s_d3d.pipeline_ready = 1;
                printf("[D3D12] Pipeline state created (triangle class)\n");
            } else {
                printf("[D3D12] PSO TRIANGLE creation failed (0x%08lX)\n", hr);
            }

            /* Line-class PSO — same shader, LINE topology type. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_lines);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (line class)\n");
            else printf("[D3D12] PSO LINE creation failed (0x%08lX)\n", hr);

            /* Point-class PSO. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_points);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (point class)\n");
            else printf("[D3D12] PSO POINT creation failed (0x%08lX)\n", hr);

            vs_blob->lpVtbl->Release(vs_blob);
            ps_blob->lpVtbl->Release(ps_blob);
        }

        /* -----------------------------------------------------------------
         * Textured triangle PSO (dbgfont / 2D atlas quads). Same MVP VS but
         * carrying UV; the PS samples the single-channel atlas as coverage
         * and modulates the vertex color, with straight alpha blending so
         * glyph edges composite over the framebuffer. Depth off (2D overlay).
         * ----------------------------------------------------------------*/
        {
            static const char vs_tex[] =
                "cbuffer cb0 : register(b0){float4 c0;float4 c1;float4 c2;float4 c3;};\n"
                "struct VSIn { float3 pos:POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "struct VSOut{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "VSOut main(VSIn i){ VSOut o; float4 p=float4(i.pos,1.0);\n"
                "  o.pos=c0*p.x+c1*p.y+c2*p.z+c3*p.w; o.col=i.col; o.uv=i.uv; return o; }\n";
            static const char ps_tex[] =
                "Texture2D tex : register(t0);\n"
                "SamplerState smp : register(s0);\n"
                "struct PSIn{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "float4 main(PSIn i):SV_TARGET{\n"
                /* dbgfont texcoords are compressed ~10x (U) / ~8x (V) vs. the
                 * atlas; scale to recover glyph cells. Exact per-glyph offset
                 * still being calibrated. */
                "  float2 uv2 = float2(i.uv.x*10.0, i.uv.y*8.0 + 0.59);\n"
                "  float cov = tex.Sample(smp, uv2).r;\n"
                "  return float4(i.col.rgb, i.col.a * cov); }\n";

            ID3DBlob *vtb = NULL, *ptb = NULL, *e2 = NULL;
            hr = sc_compile(GPU_SC_VS, vs_tex, sizeof(vs_tex) - 1, "vs_tex", NULL, GPU_SC_NO_CACHE, &vtb, &e2);
            if (FAILED(hr)) printf("[D3D12] VS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");
            hr = sc_compile(GPU_SC_PS, ps_tex, sizeof(ps_tex) - 1, "ps_tex", NULL, GPU_SC_NO_CACHE, &ptb, &e2);
            if (FAILED(hr)) printf("[D3D12] PS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");

            if (vtb && ptb) {
                D3D12_INPUT_ELEMENT_DESC il[] = {
                    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                };
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
                pd.pRootSignature = s_d3d.root_signature;
                pd.VS.pShaderBytecode = vtb->lpVtbl->GetBufferPointer(vtb);
                pd.VS.BytecodeLength  = vtb->lpVtbl->GetBufferSize(vtb);
                pd.PS.pShaderBytecode = ptb->lpVtbl->GetBufferPointer(ptb);
                pd.PS.BytecodeLength  = ptb->lpVtbl->GetBufferSize(ptb);
                pd.InputLayout.pInputElementDescs = il;
                pd.InputLayout.NumElements = 3;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
                pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
                pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = UINT_MAX;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
                pd.DepthStencilState.DepthEnable = FALSE;
                pd.DepthStencilState.StencilEnable = FALSE;
                pd.SampleDesc.Count = 1;
                hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                    s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_tex);
                if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (textured class)\n");
                else printf("[D3D12] PSO TEX creation failed (0x%08lX)\n", hr);
            }
            if (vtb) vtb->lpVtbl->Release(vtb);
            if (ptb) ptb->lpVtbl->Release(ptb);
        }

        /* SRV descriptor heap (shader-visible). Layout: slot 0 = legacy atlas
         * (dbgfont / textured 2D path), slots 1-4 = per-draw VP textures,
         * 5-20 = offscreen render targets (render-to-texture), rest spare.
         * All slots start as null SRVs so any 4-wide table window (the
         * root signature binds t0-t3) is valid on tier-1 hardware. */
        /* The GPU layer's table pool, its entries made as these R8_UNORM
         * null SRVs (step C4). The regions name its tables for Vulkan: the
         * per-draw windows, the AA source (AA_SRV_SLOT, rsxc_post.inc) and
         * SMAA's two 3-wide windows (SMAA_SRV_SLOT + 0 and + 2). */
        {
            gpu_table_pool_desc pd;
            memset(&pd, 0, sizeof pd);
            pd.kind = GPU_TABLE_TEX;
            pd.count = SRV_HEAP_SIZE;
            pd.nregions = 4;
            pd.region[0].first = DRAW_SRV_BASE; pd.region[0].tables = MAX_DRAWS; pd.region[0].size = RSX_FP_UNITS;
            pd.region[1].first = 21; pd.region[1].tables = 1; pd.region[1].size = 1;
            pd.region[2].first = 22; pd.region[2].tables = 1; pd.region[2].size = 3;
            pd.region[3].first = 24; pd.region[3].tables = 1; pd.region[3].size = 3;
            s_rc_srv_pool = gpu_table_pool_create(s_rc_dev, &pd);
            s_d3d.srv_heap = gpu_d3d12_table_pool_heap(s_rc_dev, s_rc_srv_pool);
            if (!s_d3d.srv_heap) printf("[D3D12] SRV heap creation failed\n");
            else
                s_d3d.srv_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                    s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        /* RTV heap for offscreen render targets (CPU-visible only). */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = MAX_OFF_RTS;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rt_rtv_heap);
            if (FAILED(hr)) printf("[D3D12] offscreen RTV heap creation failed (0x%08lX)\n", hr);
        }
    }

    /* ---------------------------------------------------------------
     * Create dynamic vertex buffer (upload heap, 4MB)
     * ---------------------------------------------------------------*/
    {
        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC buf_desc = {0};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = MAX_VERTICES * VERTEX_STRIDE;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = d3d_create_committed(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.vertex_buffer);
        if (SUCCEEDED(hr)) {
            D3D12_RANGE read_range = {0, 0};
            s_d3d.vertex_buffer->lpVtbl->Map(
                s_d3d.vertex_buffer, 0, &read_range, &s_d3d.vb_mapped);
            s_d3d.vb_view.BufferLocation =
                s_d3d.vertex_buffer->lpVtbl->GetGPUVirtualAddress(s_d3d.vertex_buffer);
            s_d3d.vb_view.StrideInBytes = VERTEX_STRIDE;
            s_d3d.vb_view.SizeInBytes = MAX_VERTICES * VERTEX_STRIDE;
            printf("[D3D12] Vertex buffer created (%u KB)\n",
                   (MAX_VERTICES * VERTEX_STRIDE) / 1024);
        }
    }

    /* ---------------------------------------------------------------
     * Real vertex-program path resources: root signature (CBV b0 for the
     * vp_c[] bank + SRV t0 + static sampler s0), a raw-float4 vertex buffer,
     * and the constant-bank buffer. The PSO itself is built lazily once the
     * game uploads its VP microcode (render_frame).
     * ---------------------------------------------------------------*/
    {
        /* SRV table t0-t15 so decompiled fragment programs can sample every
         * RSX texture unit; the hardcoded atlas/colour PSs use only t0 and are
         * unaffected. Matching sampler table s0-s15. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = RSX_FP_UNITS;
        srv_range.BaseShaderRegister = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rp[6] = {0};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b0 = vp_c bank */
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &srv_range;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b1 = FP texscale */
        rp[2].Descriptor.ShaderRegister = 1;
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        /* s0-s15 come from a per-draw sampler table built from the guest's
         * TEXTURE_ADDRESS/TEXTURE_FILTER (vp_sampler_table). Fixed static
         * samplers sampled a 256x1 gamma LUT with WRAP, so u=0 blended texel 0
         * with texel 255 and black came out mid-grey. */
        D3D12_DESCRIPTOR_RANGE samp_range = {0};
        samp_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samp_range.NumDescriptors = RSX_FP_UNITS;
        samp_range.BaseShaderRegister = 0;
        samp_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[3].DescriptorTable.NumDescriptorRanges = 1;
        rp[3].DescriptorTable.pDescriptorRanges = &samp_range;
        rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        {
            /* The GPU layer's table pool: table t at t*16 (step C4). */
            gpu_table_pool_desc sh;
            memset(&sh, 0, sizeof sh);
            sh.kind = GPU_TABLE_SAMPLER;
            sh.count = VP_SAMP_TABLES * RSX_FP_UNITS;
            sh.nregions = 1;
            sh.region[0].first = 0; sh.region[0].tables = VP_SAMP_TABLES; sh.region[0].size = RSX_FP_UNITS;
            s_rc_samp_pool = gpu_table_pool_create(s_rc_dev, &sh);
            s_d3d.samp_heap = gpu_d3d12_table_pool_heap(s_rc_dev, s_rc_samp_pool);
            if (!s_d3d.samp_heap)
                printf("[D3D12] sampler heap creation failed\n");
            s_d3d.samp_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }

        /* GPU vertex fetch: t20 = the frame's raw guest vertex bytes (vf_buf),
         * b2 = one constant, the draw's attribute records' offset in it. */
        rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[4].Descriptor.ShaderRegister = 20;
        rp[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[5].Constants.ShaderRegister = 2;
        rp[5].Constants.Num32BitValues = 1;
        rp[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rd = {0};
        rd.NumParameters = 6; rd.pParameters = rp;
        rd.NumStaticSamplers = 0; rd.pStaticSamplers = NULL;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (SUCCEEDED(hr)) {
            s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0,
                sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                &IID_ID3D12RootSignature, (void**)&s_d3d.vp_root_sig);
            s_vp_root_hash = fnv_bytes(14695981039346656037ull, sig->lpVtbl->GetBufferPointer(sig),
                                       sig->lpVtbl->GetBufferSize(sig));
            sig->lpVtbl->Release(sig);
        } else if (err) { printf("[D3D12] VP root sig: %s\n", (const char*)err->lpVtbl->GetBufferPointer(err)); err->lpVtbl->Release(err); }

        /* raw float4 vertex buffer + constant bank (both UPLOAD, persistently mapped) */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE nr = {0,0};

        /* RSX_VP_BUFFERS=0: none of the four per-frame buffers below (about
         * 1.2 GB of upload memory at the defaults), for a process that runs no
         * game and draws only its overlay, such as a title's setup screens
         * while a compile beside them needs the memory. Only a frame with
         * guest draws uses them: a game started this way skips render_frame's
         * vertex-program pass, so it draws none of its geometry. */
        if (!ps3_env_int("RSX_VP_BUFFERS", 1)) {
            fprintf(stderr, "[VPVB] no vertex-program buffers (RSX_VP_BUFFERS=0)%c", 10);
            goto vp_buffers_done;
        }

        /* VP_VB_VERTS=<n>: start the buffer at n vertices rather than
         * MAX_VERTICES, for a title known to need more (it grows on demand,
         * but the growth costs a frame; see vp_vb_grow). */
        { u64 v = (u64)ps3_env_int("VP_VB_VERTS", 0);
          if (v > MAX_VERTICES && v <= (64ull << 20)) s_vp_cap = (u32)v; }
        bd.Width = (u64)s_vp_cap * 256 * 2;  /* generic VP vertex: 16 float4 slots,
                                          * DOUBLE-buffered by frame parity so a
                                          * new frame's upload never overwrites
                                          * vertices the in-flight GPU frame is
                                          * still reading (was: torn/missing
                                          * triangles mixing stale+new verts) */
        /* Failure here leaves vp_vb_mapped NULL and every VP draw silently
         * declines to upload -- the scene just does not render, with no error
         * anywhere. Report the size too: this buffer is
         * MAX_VERTICES * 256 * 2 bytes and grows fast. */
        fprintf(stderr, "[VPVB] per-frame vertex buffer: %llu MB (%u verts, x2 parity)%c",
                (unsigned long long)(bd.Width >> 20), (unsigned)s_vp_cap, 10);
        if (SUCCEEDED(d3d_create_committed(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_vb)))
            s_d3d.vp_vb->lpVtbl->Map(s_d3d.vp_vb, 0, &nr, &s_d3d.vp_vb_mapped);
        else
            fprintf(stderr, "[VPVB] ALLOCATION FAILED -- no VP geometry will render%c", 10);

        bd.Width = (u64)VP_CB_STRIDE * MAX_DRAWS * 2;   /* per-draw constant slots, x2 parity */
        /* A failure here is silent and catastrophic: vp_record_cb returns early,
         * every draw loses its constants, and the whole scene transforms by
         * zeros -- indistinguishable from a broken vertex program. Say so. */
        fprintf(stderr, "[VPCB] per-draw constant buffer: %llu MB (%d draws)%c",
                (unsigned long long)(bd.Width >> 20), MAX_DRAWS, 10);
        if (SUCCEEDED(d3d_create_committed(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_cb)))
            s_d3d.vp_cb->lpVtbl->Map(s_d3d.vp_cb, 0, &nr, &s_d3d.vp_cb_mapped);

        bd.Width = (u64)VP_FPCB_STRIDE * MAX_DRAWS * 2;  /* per-draw FP slots (b1), x2 parity */
        if (SUCCEEDED(d3d_create_committed(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_fpcb)))
            s_d3d.vp_fpcb->lpVtbl->Map(s_d3d.vp_fpcb, 0, &nr, &s_d3d.vp_fpcb_mapped);

        /* GPU vertex fetch ring: each fetch draw's guest vertex bytes and
         * attribute records, x2 parity like vp_vb. VP_FETCH_RING_MB (read
         * once, default 64) per half; a draw that does not fit takes the CPU
         * path. */
        { int mb = ps3_env_int("VP_FETCH_RING_MB", 64);
          if (mb < 4) mb = 4; if (mb > 1024) mb = 1024;
          s_vf_cap = (u32)mb << 20;
          bd.Width = (u64)s_vf_cap * 2;
          if (SUCCEEDED(d3d_create_committed(s_d3d.device, &hp,
                  D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                  &IID_ID3D12Resource, (void**)&s_d3d.vf_buf)))
              s_d3d.vf_buf->lpVtbl->Map(s_d3d.vf_buf, 0, &nr, &s_d3d.vf_mapped);
          else s_vf_cap = 0; }

    vp_buffers_done:
        printf("[D3D12] VP path resources: rootsig=%p vb=%p cb=%p\n",
               (void*)s_d3d.vp_root_sig, (void*)s_d3d.vp_vb, (void*)s_d3d.vp_cb);
    }

    printf("[D3D12] Initialization complete (%ux%u, %u buffers, pipeline=%s)\n",
           width, height, FRAME_COUNT,
           s_d3d.pipeline_ready ? "ready" : "NOT ready");
    return 0;
}

