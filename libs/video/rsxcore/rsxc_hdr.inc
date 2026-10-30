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
 *     after that program, are made FP16 (rt_dxgi). They still go back to
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
/* DISPLAYCONFIG_SDR_WHITE_LEVEL, declared here: wingdi.h has it only for a
 * newer target Windows version than this file asks for. */
typedef struct { DISPLAYCONFIG_DEVICE_INFO_HEADER header; ULONG level; } HdrSdrWhite;

/* Windows' SDR white level ("SDR content brightness") on the display with
 * this GDI device name, in nits; 0 when not known. */
static float hdr_sdr_white(const WCHAR* gdi)
{
    UINT32 np = 0, nm = 0;
    float nits = 0.0f;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return 0.0f;
    DISPLAYCONFIG_PATH_INFO* pi = (DISPLAYCONFIG_PATH_INFO*)calloc(np, sizeof *pi);
    DISPLAYCONFIG_MODE_INFO* mi = (DISPLAYCONFIG_MODE_INFO*)calloc(nm ? nm : 1, sizeof *mi);
    if (pi && mi && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, pi, &nm, mi, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < np && nits == 0.0f; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
            memset(&sn, 0, sizeof sn);
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof sn;
            sn.header.adapterId = pi[i].sourceInfo.adapterId;
            sn.header.id = pi[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, gdi))
                continue;
            HdrSdrWhite wl;
            memset(&wl, 0, sizeof wl);
            wl.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)11;   /* ..._GET_SDR_WHITE_LEVEL */
            wl.header.size = sizeof wl;
            wl.header.adapterId = pi[i].targetInfo.adapterId;
            wl.header.id = pi[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS) nits = (float)wl.level / 1000.0f * 80.0f;
        }
    }
    free(pi);
    free(mi);
    return nits;
}

/* What the display the window is on reports. A new factory each time: one
 * keeps describing the outputs as they were when it was made. */
static IDXGIFactory1* s_hdr_factory;
static HMONITOR s_hdr_mon;
static void hdr_display_query(void)
{
    s_hdr.disp_hdr = 0;
    s_hdr.disp_peak = s_hdr.disp_white = 0.0f;
    s_hdr.failed = 0;
    if (s_hdr_factory) s_hdr_factory->lpVtbl->Release(s_hdr_factory);
    s_hdr_factory = NULL;
    s_hdr_mon = MonitorFromWindow(s_d3d.hwnd, MONITOR_DEFAULTTONEAREST);
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&s_hdr_factory))) {
        s_hdr_factory = NULL;
        return;
    }
    IDXGIAdapter1* a = NULL;
    for (UINT i = 0; s_hdr_factory->lpVtbl->EnumAdapters1(s_hdr_factory, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        IDXGIOutput* o = NULL;
        for (UINT k = 0; a->lpVtbl->EnumOutputs(a, k, &o) != DXGI_ERROR_NOT_FOUND; k++) {
            DXGI_OUTPUT_DESC d;
            IDXGIOutput6* o6 = NULL;
            if (SUCCEEDED(o->lpVtbl->GetDesc(o, &d)) && d.Monitor == s_hdr_mon &&
                SUCCEEDED(o->lpVtbl->QueryInterface(o, &IID_IDXGIOutput6, (void**)&o6))) {
                DXGI_OUTPUT_DESC1 d1;
                if (SUCCEEDED(o6->lpVtbl->GetDesc1(o6, &d1))) {
                    s_hdr.disp_hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    s_hdr.disp_peak = d1.MaxLuminance;
                    s_hdr.disp_white = hdr_sdr_white(d1.DeviceName);
                }
                o6->lpVtbl->Release(o6);
            }
            o->lpVtbl->Release(o);
        }
        a->lpVtbl->Release(a);
    }
}

/* Between frames, with RSX_HDR on: asks again when the window moved to
 * another display, the display setup changed (the factory is no longer
 * current: HDR switched in Windows, a display added), or the window was
 * activated (the SDR white level may have moved meanwhile). */
static void hdr_display_check(void)
{
    if (!InterlockedExchange(&s_hdr_requery, 0) && s_hdr_factory &&
        MonitorFromWindow(s_d3d.hwnd, MONITOR_DEFAULTTONEAREST) == s_hdr_mon &&
        s_hdr_factory->lpVtbl->IsCurrent(s_hdr_factory))
        return;
    hdr_display_query();
    printf("[D3D12] HDR: the window's display has HDR %s (peak %.0f nits, SDR white %.0f nits)\n",
           s_hdr.disp_hdr ? "on" : "off", s_hdr.disp_peak, s_hdr.disp_white);
}

/* The swap chain's colour space: scRGB for FP16 buffers (hdr), sRGB for RGBA8
 * ones. 0 if it was refused. */
static int swap_color_space(int hdr)
{
    const DXGI_COLOR_SPACE_TYPE cs = hdr ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                         : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT sup = 0;
    return SUCCEEDED(s_d3d.swap_chain->lpVtbl->CheckColorSpaceSupport(s_d3d.swap_chain, cs, &sup)) &&
           (sup & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) &&
           SUCCEEDED(s_d3d.swap_chain->lpVtbl->SetColorSpace1(s_d3d.swap_chain, cs));
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

/* The swap chain's buffers and their RTVs (init and after ResizeBuffers). */
static int swap_buffers_get(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.swap_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.swap_rtv_heap, &h);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        HRESULT hr = s_d3d.swap_chain->lpVtbl->GetBuffer(
            s_d3d.swap_chain, i, &IID_ID3D12Resource, (void**)&s_d3d.swap_buffers[i]);
        if (FAILED(hr)) return -1;
        s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_d3d.swap_buffers[i], NULL, h);
        h.ptr += s_d3d.rtv_descriptor_size;
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
static struct { ID3D12RootSignature* rs; ID3D12PipelineState* pso; int failed; } s_of;

static int of_ready(void)
{
    if (s_of.pso) return 1;
    if (s_of.failed || !s_d3d.device) return 0;
    s_of.failed = 1;   /* until it is made */
    ID3DBlob *vb = NULL, *pb = NULL, *rsb = NULL, *err = NULL;
    HRESULT hr = sc_compile(GPU_SC_VS, rsx_outfilter_vs, sizeof rsx_outfilter_vs - 1, "rsx_outfilter_vs",
                            NULL, GPU_SC_NO_CACHE | GPU_SC_OPTIMIZE, &vb, &err);
    if (SUCCEEDED(hr))
        hr = sc_compile(GPU_SC_PS, rsx_outfilter_ps, sizeof rsx_outfilter_ps - 1, "rsx_outfilter_ps",
                        NULL, GPU_SC_NO_CACHE | GPU_SC_OPTIMIZE, &pb, &err);
    if (FAILED(hr))
        printf("[D3D12] RSX_OUTFILTER shader compile failed: %s\n",
               err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
                                        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        D3D12_ROOT_PARAMETER rp[2] = {0};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 1;
        rp[0].DescriptorTable.pDescriptorRanges = &range;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[1].Constants.Num32BitValues = 8;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {2, rp, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err);
        if (SUCCEEDED(hr))
            hr = s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0,
                rsb->lpVtbl->GetBufferPointer(rsb), rsb->lpVtbl->GetBufferSize(rsb),
                &IID_ID3D12RootSignature, (void**)&s_of.rs);
    }
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = s_of.rs;
        pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb);
        pd.VS.BytecodeLength  = vb->lpVtbl->GetBufferSize(vb);
        pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb);
        pd.PS.BytecodeLength  = pb->lpVtbl->GetBufferSize(pb);
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd,
            &IID_ID3D12PipelineState, (void**)&s_of.pso);
    }
    if (vb) vb->lpVtbl->Release(vb);
    if (pb) pb->lpVtbl->Release(pb);
    if (rsb) rsb->lpVtbl->Release(rsb);
    if (err) err->lpVtbl->Release(err);
    if (FAILED(hr) || !s_of.pso) {
        printf("[D3D12] RSX_OUTFILTER pipeline failed (0x%08lX): the bilinear blit stays\n", (long)hr);
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
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL, *rsb = NULL;
    HRESULT hr = sc_compile(GPU_SC_VS, vs, sizeof vs - 1, "blit_vs", NULL, GPU_SC_NO_CACHE, &vb, &err);
    if (SUCCEEDED(hr)) hr = sc_compile(GPU_SC_PS, ps, sizeof ps - 1, "blit_ps", NULL, GPU_SC_NO_CACHE, &pb, &err);
    if (FAILED(hr)) {
        printf("[D3D12] blit shader compile failed: %s\n", err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
        return -1;
    }
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12_ROOT_PARAMETER param = {0};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC smp = {0};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {1, &param, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err);
    if (SUCCEEDED(hr))
        hr = s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0, rsb->lpVtbl->GetBufferPointer(rsb),
                                                        rsb->lpVtbl->GetBufferSize(rsb),
                                                        &IID_ID3D12RootSignature, (void**)&s_d3d.blit_rs);
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = s_d3d.blit_rs;
        pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb);
        pd.VS.BytecodeLength  = vb->lpVtbl->GetBufferSize(vb);
        pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb);
        pd.PS.BytecodeLength  = pb->lpVtbl->GetBufferSize(pb);
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState,
                                                               (void**)&s_d3d.blit_pso);
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = FRAME_COUNT * 2;   /* the frame and T (blit_srvs_write) */
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd, &IID_ID3D12DescriptorHeap,
                                                        (void**)&s_d3d.blit_srv_heap);
    }
    if (SUCCEEDED(hr)) blit_srvs_write();
    if (vb) vb->lpVtbl->Release(vb);
    if (pb) pb->lpVtbl->Release(pb);
    if (rsb) rsb->lpVtbl->Release(rsb);
    if (err) err->lpVtbl->Release(err);
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
static struct { ID3D12RootSignature* rs; ID3D12PipelineState* pso[2]; int failed; } s_hdr_blit;

static int hdr_blit_ready(void)
{
    if (s_hdr_blit.pso[0] && s_hdr_blit.pso[1]) return 1;
    if (s_hdr_blit.failed || !s_d3d.device) return 0;
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
    ID3DBlob *vb[2] = {NULL, NULL}, *pb[2] = {NULL, NULL}, *rsb = NULL, *err = NULL;
    HRESULT hr = sc_compile(GPU_SC_VS, vs, sizeof vs - 1, "hdr_blit_vs", NULL, GPU_SC_NO_CACHE, &vb[0], &err);
    if (SUCCEEDED(hr))
        hr = sc_compile(GPU_SC_PS, ps, sizeof ps - 1, "hdr_blit_ps", NULL, GPU_SC_NO_CACHE, &pb[0], &err);
    if (SUCCEEDED(hr))
        hr = sc_compile(GPU_SC_VS, rsx_outfilter_vs, sizeof rsx_outfilter_vs - 1, "rsx_outfilter_vs", NULL,
                        GPU_SC_NO_CACHE | GPU_SC_OPTIMIZE, &vb[1], &err);
    if (SUCCEEDED(hr))
        hr = sc_compile(GPU_SC_PS, sharp, strlen(sharp), "hdr_sharp_ps", NULL,
                        GPU_SC_NO_CACHE | GPU_SC_OPTIMIZE, &pb[1], &err);
    if (FAILED(hr))
        printf("[D3D12] HDR blit shader compile failed: %s\n",
               err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0,
                                        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        D3D12_ROOT_PARAMETER rp[3] = {0};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 1;
        rp[0].DescriptorTable.pDescriptorRanges = &range;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        for (int i = 1; i <= 2; i++) {
            rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[i].Constants.ShaderRegister = (UINT)(i - 1);
            rp[i].Constants.Num32BitValues = 8;
            rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_STATIC_SAMPLER_DESC smp = {0};
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.MaxLOD = D3D12_FLOAT32_MAX;
        smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {3, rp, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err);
        if (SUCCEEDED(hr))
            hr = s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0,
                rsb->lpVtbl->GetBufferPointer(rsb), rsb->lpVtbl->GetBufferSize(rsb),
                &IID_ID3D12RootSignature, (void**)&s_hdr_blit.rs);
    }
    for (int i = 0; i < 2 && SUCCEEDED(hr); i++) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = s_hdr_blit.rs;
        pd.VS.pShaderBytecode = vb[i]->lpVtbl->GetBufferPointer(vb[i]);
        pd.VS.BytecodeLength  = vb[i]->lpVtbl->GetBufferSize(vb[i]);
        pd.PS.pShaderBytecode = pb[i]->lpVtbl->GetBufferPointer(pb[i]);
        pd.PS.BytecodeLength  = pb[i]->lpVtbl->GetBufferSize(pb[i]);
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count = 1;
        hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd,
            &IID_ID3D12PipelineState, (void**)&s_hdr_blit.pso[i]);
    }
    for (int i = 0; i < 2; i++) {
        if (vb[i]) vb[i]->lpVtbl->Release(vb[i]);
        if (pb[i]) pb[i]->lpVtbl->Release(pb[i]);
    }
    if (rsb) rsb->lpVtbl->Release(rsb);
    if (err) err->lpVtbl->Release(err);
    if (FAILED(hr) || !s_hdr_blit.pso[0] || !s_hdr_blit.pso[1]) {
        printf("[D3D12] HDR blit pipeline failed (0x%08lX): HDR stays off\n", (long)hr);
        return 0;
    }
    s_hdr_blit.failed = 0;
    return 1;
}

/* The blit's descriptors: for each frame fi, the game's frame at 2 fi and T
 * or nothing at 2 fi + 1 (written by blit_to_window). */
static void blit_srvs_write(void)
{
    const u32 inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.blit_srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.blit_srv_heap, &h);
    D3D12_SHADER_RESOURCE_VIEW_DESC none = {0};
    none.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    none.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    none.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    none.Texture2D.MipLevels = 1;
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.render_targets[i], NULL, h);
        h.ptr += inc;
        s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, NULL, &none, h);
        h.ptr += inc;
    }
}

static void off_rt_transition(int slot, D3D12_RESOURCE_STATES to);

/* Records the scale of the game's frame fi into the window's buffer fi: the
 * largest rectangle with the frame's aspect ratio (RSX_ASPECT's, when set),
 * centred, black around it. Both resources are in PRESENT before and after. */
static void blit_to_window(u32 fi)
{
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.Subresource = b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.pResource   = s_d3d.render_targets[fi];
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[1].Transition.pResource   = s_d3d.swap_buffers[fi];
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cl->lpVtbl->ResourceBarrier(cl, 2, b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    s_d3d.swap_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.swap_rtv_heap, &rtv);
    rtv.ptr += (u64)fi * s_d3d.rtv_descriptor_size;
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);

    const float ww = (float)s_d3d.win_w, wh = (float)s_d3d.win_h;
    const float gh = (float)s_d3d.height, ar = rsx_aspect();
    const float gw = ar > 0.0f ? gh * ar : (float)s_d3d.width;   /* RSX_ASPECT: stretched to that shape */
    const float k = ww / gw < wh / gh ? ww / gw : wh / gh;
    const float vw = floorf(gw * k + 0.5f), vh = floorf(gh * k + 0.5f);
    D3D12_VIEWPORT vp = {floorf((ww - vw) * 0.5f), floorf((wh - vh) * 0.5f), vw, vh, 0.0f, 1.0f};
    if (vw != ww || vh != wh) {
        static const float black[4] = {0, 0, 0, 1};
        cl->lpVtbl->ClearRenderTargetView(cl, rtv, black, 0, NULL);
    }
    D3D12_RECT sc = {0, 0, (LONG)s_d3d.win_w, (LONG)s_d3d.win_h};
    /* RSX_OUTFILTER=1: the area / bicubic pass instead of the bilinear tap.
     * With HDR on (the swap chain is FP16), the HDR pipelines (hdr_blit_ready). */
    const int hdr = s_hdr.active;
    const int sharp = knob_outfilter() == 1 && vw >= 1.0f && vh >= 1.0f && (hdr || of_ready());
    if (hdr) {
        cl->lpVtbl->SetGraphicsRootSignature(cl, s_hdr_blit.rs);
        cl->lpVtbl->SetPipelineState(cl, s_hdr_blit.pso[sharp]);
    } else {
        cl->lpVtbl->SetGraphicsRootSignature(cl, sharp ? s_of.rs : s_d3d.blit_rs);
        cl->lpVtbl->SetPipelineState(cl, sharp ? s_of.pso : s_d3d.blit_pso);
    }
    const u32 inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    int use_t = 0;
    if (hdr) {   /* T's highlights at 2 fi + 1, for this frame only */
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        s_d3d.blit_srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.blit_srv_heap, &h);
        h.ptr += (SIZE_T)(2 * fi + 1) * inc;
        const int t = s_hdr.frame_t;
        if (t >= 0 && s_d3d.off_rt[t].res && s_d3d.off_rt[t].dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            off_rt_transition(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.off_rt[t].res, NULL, h);
            use_t = 1;
        } else {
            D3D12_SHADER_RESOURCE_VIEW_DESC none = {0};
            none.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            none.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            none.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            none.Texture2D.MipLevels = 1;
            s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, NULL, &none, h);
        }
    }
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_d3d.blit_srv_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    s_d3d.blit_srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.blit_srv_heap, &g);
    g.ptr += (u64)(2 * fi) * inc;
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, g);
    if (sharp) {   /* viewport origin, frame texels per window pixel, frame size */
        const float of[8] = {vp.TopLeftX, vp.TopLeftY, (float)s_d3d.host_w / vw, (float)s_d3d.host_h / vh,
                             (float)s_d3d.host_w, (float)s_d3d.host_h, 0.0f, 0.0f};
        cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 8, of, 0);
    }
    if (hdr) {   /* paper white in scRGB units, peak / paper white, the knee, the curve, T */
        const float white = hdr_white_nits(), ratio = hdr_peak_nits() / white;
        const float knee = ratio <= 1.25f ? 0.8f * ratio : (0.6f * ratio > 1.0f ? 0.6f * ratio : 1.0f);
        const float hc[8] = {white / 80.0f, ratio, knee, knob_hdr_gamma() ? 1.0f : 0.0f,
                             (float)use_t, 0.0f, 0.0f, 0.0f};
        cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 2, 8, hc, 0);
    }
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);

    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    cl->lpVtbl->ResourceBarrier(cl, 2, b);
}

/* rc_overlay_repaint: the last game frame shown, as blit_to_window
 * read it (render_targets[fi] is cleared by the next batch, a frame shown or
 * not). Render thread. */
static ID3D12Resource* s_hold;
static u32 s_hold_w, s_hold_h;
static int s_hold_valid;            /* s_hold has the last frame shown */
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

/* Records the copy of the game's frame fi into s_hold. render_targets[fi] is
 * in PRESENT before and after. */
static void hold_copy(u32 fi)
{
    if (s_hold && (s_hold_w != s_d3d.host_w || s_hold_h != s_d3d.host_h)) {   /* RSX_SCALE moved */
        s_hold->lpVtbl->Release(s_hold);
        s_hold = NULL;
    }
    if (!s_hold) {
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
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL,
                &IID_ID3D12Resource, (void**)&s_hold))) {
            s_hold = NULL;
            s_hold_valid = 0;
            return;
        }
        s_hold_w = s_d3d.host_w;
        s_hold_h = s_d3d.host_h;
    }
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.Subresource = b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.pResource   = s_d3d.render_targets[fi];
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource   = s_hold;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    cl->lpVtbl->ResourceBarrier(cl, 2, b);
    cl->lpVtbl->CopyResource(cl, s_hold, s_d3d.render_targets[fi]);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    cl->lpVtbl->ResourceBarrier(cl, 2, b);
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
    DXGI_FRAME_STATISTICS fs;
    memset(&fs, 0, sizeof fs);
    if (FAILED(s_d3d.swap_chain->lpVtbl->GetFrameStatistics(s_d3d.swap_chain, &fs)) ||
        !fs.SyncQPCTime.QuadPart || !fs.SyncRefreshCount)
        return;
    const long long q = fs.SyncQPCTime.QuadPart;
    const UINT c = fs.SyncRefreshCount;
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
 * Said once, with Windows' reason; rc_device_lost() hands the reason
 * to the host (its problem report). */
static volatile LONG s_device_lost;      /* GetDeviceRemovedReason, 0 while the device works */
static volatile LONG64 s_device_lost_qpc;

static const char* removed_reason_text(HRESULT rr)
{
    switch ((unsigned long)rr) {
    case 0x887A0006ul: return "the GPU stopped responding and the driver was reset (DXGI_ERROR_DEVICE_HUNG)";
    case 0x887A0005ul: return "the GPU was removed or its driver changed (DXGI_ERROR_DEVICE_REMOVED)";
    case 0x887A0007ul: return "the device was reset after a bad command (DXGI_ERROR_DEVICE_RESET)";
    case 0x887A0020ul: return "an internal driver error (DXGI_ERROR_DRIVER_INTERNAL_ERROR)";
    case 0x8007000Eul: return "the GPU ran out of memory (E_OUTOFMEMORY)";
    default: return "an unknown reason";
    }
}

static void check_device(const char* where)
{
    if (s_device_lost || !s_d3d.device) return;
    const HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
    if (rr == S_OK) return;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    InterlockedExchange64(&s_device_lost_qpc, q.QuadPart);
    if (InterlockedCompareExchange(&s_device_lost, (LONG)rr, 0) != 0) return;
    fprintf(stderr, "[D3D12] the graphics device was lost (seen at %s): %s, 0x%08lX. Nothing more can be"
            " drawn, so the picture stays as it is; the game needs a restart. D3D12_DRED=1 on the next run"
            " says where the GPU was.%c", where, removed_reason_text(rr), (unsigned long)rr, 10);
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
    const HRESULT hr = s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, interval, tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(hr)) check_device("present");
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
    for (u32 i = 0; i < FRAME_COUNT; i++)
        if (s_d3d.swap_buffers[i]) {
            s_d3d.swap_buffers[i]->lpVtbl->Release(s_d3d.swap_buffers[i]);
            s_d3d.swap_buffers[i] = NULL;
        }
    const UINT flags = s_d3d.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HRESULT hr = s_d3d.swap_chain->lpVtbl->ResizeBuffers(
        s_d3d.swap_chain, FRAME_COUNT, w, h,
        hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM, flags);
    if (SUCCEEDED(hr) && hdr != s_hdr.active) {
        /* A display that refuses scRGB keeps RGBA8. */
        if (!swap_color_space(hdr)) {
            if (hdr) {
                printf("[D3D12] HDR: the swap chain's scRGB colour space was refused; staying SDR\n");
                s_hdr.failed = 1;
                hr = s_d3d.swap_chain->lpVtbl->ResizeBuffers(s_d3d.swap_chain, FRAME_COUNT, w, h,
                                                             DXGI_FORMAT_R8G8B8A8_UNORM, flags);
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
    /* The back buffer index starts over; the GPU is idle, so every frame's
     * fence value moves to the highest one and the fence never steps back. */
    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);
    u64 fv = 0;
    for (u32 i = 0; i < FRAME_COUNT; i++) if (s_d3d.fence_values[i] > fv) fv = s_d3d.fence_values[i];
    for (u32 i = 0; i < FRAME_COUNT; i++) s_d3d.fence_values[i] = fv;
}

