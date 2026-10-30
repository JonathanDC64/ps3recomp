/*
 * ps3recomp - the GPU layer's D3D12 backend test (CMake target
 * test_gpu_d3d12, built with PS3RECOMP_RSX_HAL=ON on Windows; ctest -R
 * gpu_d3d12)
 *
 * Runs on WARP, so it needs no GPU and runs in CI; GPU_TEST_HW=1 runs it on
 * the adapter the renderer would pick. Each case records through
 * gpu_d3d12_cmd.h (the draw loop's path), waits for the GPU and reads the
 * result back:
 *   - buffers: upload, GPU and readback memory round trip; host import;
 *   - textures (RGBA8, BC1, RGBA16F, one placed in a heap) through
 *     copy_buf_to_tex and copy_tex_to_buf; texture to texture copies;
 *   - clears of a colour and a depth target;
 *   - a draw on the guest pipelines' six-slot layout (FXC through the
 *     compile service), its pixels, and occlusion counts;
 *   - the table memo, sampler and view caching, fences and a wait's timeout;
 *   - root signatures and pipeline descriptions against today's, byte for
 *     byte (its recipes are copied here: rsx_d3d12_backend.c);
 *   - the pipeline cache across two devices, pipelines made only for it
 *     (GPU_PIPELINE_STORE_ONLY) and a device without one, a swap chain on a
 *     hidden window, and the conversion's bridges (gpu_d3d12_bridge.h);
 *   - DRED (GPU_DEVICE_BREADCRUMBS) and, on WARP, a device lost on purpose.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(_WIN32)

#include "gpu_d3d12_internal.h"
#include "gpu_d3d12_cmd.h"
#include "gpu_d3d12_bridge.h"
#include "gpu_shader.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "user32.lib")

/* Guest memory, which host_stubs.c (the runtime's host side) names; no guest
 * runs here. */
uint8_t* vm_base = NULL;

static int g_fail, g_checks;

#define CHECK(cond)                                                              \
    do { g_checks++;                                                             \
         if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
                        g_fail++; } } while (0)

static gpu_device* g_d;
static gpu_d3d12_device* g_dd;
static uint32_t g_flags;   /* the device's GPU_DEVICE_* */

/* ---- Helpers ---- */

static gpu_buffer buf_new(gpu_device* d, uint64_t size, uint32_t memory, gpu_state state)
{
    gpu_buffer_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = size;
    bd.use = GPU_USE_VERTEX | GPU_USE_INDEX | GPU_USE_CONSTANT | GPU_USE_RAW;
    bd.memory = memory;
    bd.state = state;
    return gpu_buffer_create(d, &bd);
}

static uint8_t* buf_ptr(gpu_buffer b)
{
    return (uint8_t*)gpu_buffer_map(g_d, b, 0, 0);
}

/* Ends, submits and waits for a recording on slot 0. 1 when the GPU finished. */
static int run(gpu_device* d, gpu_cmd* c)
{
    gpu_submit_desc how;
    memset(&how, 0, sizeof how);
    how.flags = GPU_SUBMIT_SIGNAL;
    gpu_cmd_end(d, c);
    const gpu_fence f = gpu_submit(d, &c, 1, &how);
    return f && gpu_fence_wait(d, f, 20000) == 0;
}

static gpu_texture_desc tex_desc(uint32_t w, uint32_t h, gpu_format f, uint32_t use, gpu_state state)
{
    gpu_texture_desc td;
    memset(&td, 0, sizeof td);
    td.width = w;
    td.height = h;
    td.layers = 1;
    td.mips = 1;
    td.format = f;
    td.samples = 1;
    td.use = use;
    td.state = state;
    return td;
}

/* A texture's subresource 0, read back: t must be in COPY_SOURCE. */
typedef struct readback {
    gpu_buffer    buf;
    gpu_footprint fp;
    uint32_t      rows;
    uint64_t      row_bytes, total;
    const uint8_t* p;
} readback;

static int tex_read(gpu_texture t, const gpu_texture_desc* td, readback* rb)
{
    memset(rb, 0, sizeof *rb);
    rb->total = gpu_texture_footprints(g_d, td, 0, 1, 0, &rb->fp, &rb->rows, &rb->row_bytes);
    rb->buf = buf_new(g_d, rb->total, GPU_MEM_READBACK, GPU_STATE_COPY_DEST);
    gpu_cmd* c = gpu_cmd_begin(g_d, 0);
    gpu_cmd_copy_tex_to_buf(c, rb->buf, &rb->fp, t, 0, NULL);
    if (!run(g_d, c)) return 0;
    rb->p = (const uint8_t*)gpu_buffer_map(g_d, rb->buf, 0, rb->total);
    return rb->p != NULL;
}

static void rb_done(readback* rb)
{
    if (rb->p) gpu_buffer_unmap(g_d, rb->buf, 0, 0);
    gpu_buffer_destroy(g_d, rb->buf, 0);
    memset(rb, 0, sizeof *rb);
}

static const uint8_t* px(const readback* rb, uint32_t x, uint32_t y)
{
    return rb->p + (uint64_t)y * rb->fp.row_pitch + (uint64_t)x * 4;
}

static int close_to(int a, int b) { return abs(a - b) <= 2; }

static gpu_shader sh_make(gpu_device* d, gpu_sc_stage st, uint32_t stage, const char* src, const char* name)
{
    gpu_shader none = {0};
    gpu_sc_blob b;
    char err[1024];
    err[0] = 0;
    const int r = gpu_sc_compile(GPU_SC_DXBC, st, src, strlen(src), name, GPU_SC_NO_CACHE, &b, err, sizeof err);
    if (r != GPU_SC_OK) {
        printf("  shader %s: %d %s\n", name, r, err);
        return none;
    }
    gpu_shader_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.stage = stage;
    sd.code = GPU_CODE_DXBC;
    sd.bytes = b.data;
    sd.size = b.size;
    const gpu_shader s = gpu_shader_create(d, &sd);
    gpu_sc_blob_release(&b);
    return s;
}

/* ---- Buffers ---- */

static void test_buffers(void)
{
    enum { N = 65536 };
    const gpu_buffer up = buf_new(g_d, N, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    const gpu_buffer mid = buf_new(g_d, N, GPU_MEM_GPU, GPU_STATE_COPY_DEST);
    const gpu_buffer rbk = buf_new(g_d, N, GPU_MEM_READBACK, GPU_STATE_COPY_DEST);
    CHECK(GPU_VALID(up) && GPU_VALID(mid) && GPU_VALID(rbk));
    uint8_t* p = buf_ptr(up);
    CHECK(p != NULL);
    if (!p) return;
    for (int i = 0; i < N; i++) p[i] = (uint8_t)(i * 7 + 3);
    CHECK(gpu_buffer_addr(g_d, up) != 0 && gpu_buffer_addr(g_d, up) == gpu_fast_buffer_addr(g_d, up));
    CHECK(gpu_buffer_map(g_d, mid, 0, 0) == NULL);   /* GPU memory has no CPU pointer */
    gpu_cmd* c = gpu_cmd_begin(g_d, 0);
    CHECK(c != NULL);
    gpu_cmd_copy_buffer(c, mid, 0, up, 0, N);
    gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.type = GPU_BARRIER_TRANSITION;
    b.buffer = mid;
    b.sub = GPU_SUB_ALL;
    b.before = GPU_STATE_COPY_DEST;
    b.after = GPU_STATE_COPY_SOURCE;
    gpu_cmd_barrier_n(c, 1, &b);
    gpu_cmd_copy_buffer(c, rbk, 0, mid, 0, N);
    CHECK(run(g_d, c));
    const uint8_t* q = (const uint8_t*)gpu_buffer_map(g_d, rbk, 0, N);
    CHECK(q && !memcmp(p, q, N));
    gpu_buffer_unmap(g_d, rbk, 0, 0);
    gpu_buffer_destroy(g_d, up, 0);
    gpu_buffer_destroy(g_d, mid, 0);
    gpu_buffer_destroy(g_d, rbk, 0);
    CHECK(gpu_native(g_d, GPU_NATIVE_BUFFER, up.id) == NULL);   /* freed at once: after 0 */
}

static void test_host_import(void)
{
    if (!(g_d->caps.bits & GPU_CAP_HOST_IMPORT)) {
        printf("  host import: skipped (no ID3D12Device3)\n");
        return;
    }
    CHECK(g_d->caps.host_import_align == 65536);
    const size_t n = 1u << 20;
    uint8_t* host = (uint8_t*)VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_READWRITE);   /* the import commits it */
    CHECK(host != NULL);
    if (!host) return;
    const gpu_buffer hb = gpu_buffer_import_host(g_d, host, n);
    CHECK(GPU_VALID(hb));
    if (GPU_VALID(hb)) {
        CHECK(gpu_buffer_map(g_d, hb, 0, 0) == host);
        const gpu_buffer up = buf_new(g_d, 4096, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
        uint8_t* p = buf_ptr(up);
        for (int i = 0; i < 4096; i++) p[i] = (uint8_t)(i ^ 0x5A);
        gpu_cmd* c = gpu_cmd_begin(g_d, 0);
        gpu_cmd_copy_buffer(c, hb, 8192, up, 0, 4096);
        CHECK(run(g_d, c));
        CHECK(!memcmp(host + 8192, p, 4096));   /* the GPU wrote the caller's memory */
        CHECK(host[0] == 0 && host[n - 1] == 0);
        gpu_buffer_destroy(g_d, up, 0);
        gpu_buffer_destroy(g_d, hb, 0);
    }
    VirtualFree(host, 0, MEM_RELEASE);
}

/* ---- Textures ---- */

/* Bytes into t (in COPY_DEST) and back, row by row. */
static void tex_roundtrip_on(const char* name, gpu_texture t, const gpu_texture_desc* td, int aliased)
{
    gpu_footprint fp;
    uint32_t rows = 0;
    uint64_t row_bytes = 0;
    const uint64_t total = gpu_texture_footprints(g_d, td, 0, 1, 0, &fp, &rows, &row_bytes);
    CHECK(total > 0 && fp.row_pitch % 256 == 0 && fp.format == td->format && fp.width == td->width);
    const gpu_buffer up = buf_new(g_d, total, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    uint8_t* p = buf_ptr(up);
    for (uint32_t r = 0; r < rows; r++)
        for (uint64_t k = 0; k < row_bytes; k++)
            p[(uint64_t)r * fp.row_pitch + k] = (uint8_t)(r * 31u + k * 17u + td->format);
    gpu_cmd* c = gpu_cmd_begin(g_d, 0);
    if (aliased) {   /* the memory may have held another texture (today's texture pool) */
        gpu_barrier a;
        memset(&a, 0, sizeof a);
        a.type = GPU_BARRIER_ALIASING;
        a.texture = t;
        gpu_cmd_barrier_n(c, 1, &a);
    }
    gpu_cmd_copy_buf_to_tex(c, t, 0, 0, 0, 0, up, &fp);
    gpu_cmd_barrier(c, t, GPU_STATE_COPY_DEST, GPU_STATE_COPY_SOURCE);
    CHECK(run(g_d, c));
    readback rb;
    CHECK(tex_read(t, td, &rb));
    int same = rb.p != NULL && rb.rows == rows;
    for (uint32_t r = 0; same && r < rows; r++)
        if (memcmp(p + (uint64_t)r * fp.row_pitch, rb.p + (uint64_t)r * rb.fp.row_pitch, (size_t)row_bytes)) same = 0;
    if (!same) printf("  %s: the bytes read back differ\n", name);
    CHECK(same);
    rb_done(&rb);
    gpu_buffer_destroy(g_d, up, 0);
}

static void tex_roundtrip(const char* name, gpu_format f, uint32_t w, uint32_t h)
{
    const gpu_texture_desc td = tex_desc(w, h, f, GPU_USE_SAMPLED | GPU_USE_COPY_SRC | GPU_USE_COPY_DST,
                                         GPU_STATE_COPY_DEST);
    const gpu_texture t = gpu_texture_create(g_d, &td);
    CHECK(GPU_VALID(t));
    if (!GPU_VALID(t)) return;
    tex_roundtrip_on(name, t, &td, 0);
    gpu_texture_destroy(g_d, t, 0);
}

static void test_textures(void)
{
    tex_roundtrip("RGBA8", GPU_FORMAT_R8G8B8A8_UNORM, 64, 64);
    tex_roundtrip("BC1", GPU_FORMAT_BC1_UNORM, 64, 32);
    tex_roundtrip("RGBA16F", GPU_FORMAT_R16G16B16A16_FLOAT, 32, 16);

    /* Placed, as today's texture pool places them: a 4 MB heap for sampled
     * textures, the size and alignment GetResourceAllocationInfo gives. */
    gpu_heap_desc hd;
    memset(&hd, 0, sizeof hd);
    hd.size = 4u << 20;
    hd.memory = GPU_MEM_GPU;
    hd.flags = GPU_HEAP_SAMPLED_TEXTURES;
    const gpu_heap heap = gpu_heap_create(g_d, &hd);
    CHECK(GPU_VALID(heap));
    gpu_texture_desc td = tex_desc(64, 64, GPU_FORMAT_R8G8B8A8_UNORM,
                                   GPU_USE_SAMPLED | GPU_USE_COPY_SRC | GPU_USE_COPY_DST, GPU_STATE_COPY_DEST);
    uint64_t size = 0, align = 0;
    gpu_texture_size(g_d, &td, &size, &align);
    CHECK(size >= 64 * 64 * 4 && size != UINT64_MAX && align && align <= 65536);
    td.heap = heap;
    td.heap_offset = align;
    const gpu_texture t = gpu_texture_create(g_d, &td);
    CHECK(GPU_VALID(t));
    if (GPU_VALID(t)) {
        tex_roundtrip_on("placed RGBA8", t, &td, 1);

        /* Texture to texture: a box of t to (4, 4) of u, and all of t to v. */
        gpu_texture_desc ud = td;
        ud.heap.id = 0;
        ud.heap_offset = 0;
        const gpu_texture u = gpu_texture_create(g_d, &ud);
        const gpu_texture v = gpu_texture_create(g_d, &ud);
        CHECK(GPU_VALID(u) && GPU_VALID(v));
        readback a;
        CHECK(tex_read(t, &td, &a));   /* t is in COPY_SOURCE */
        gpu_cmd* c = gpu_cmd_begin(g_d, 0);
        const gpu_box box = {0, 0, 0, 8, 8, 1};
        gpu_cmd_copy_texture(c, u, 0, 4, 4, 0, t, 0, &box);
        gpu_cmd_copy_texture_whole(c, v, t);
        gpu_barrier bs[2];
        memset(bs, 0, sizeof bs);
        bs[0].texture = u;
        bs[1].texture = v;
        for (int i = 0; i < 2; i++) {
            bs[i].sub = GPU_SUB_ALL;
            bs[i].before = GPU_STATE_COPY_DEST;
            bs[i].after = GPU_STATE_COPY_SOURCE;
        }
        gpu_cmd_barrier_n(c, 2, bs);
        CHECK(run(g_d, c));
        readback ru, rv;
        CHECK(tex_read(u, &ud, &ru) && tex_read(v, &ud, &rv));
        if (a.p && ru.p && rv.p) {
            CHECK(!memcmp(px(&ru, 4, 4), px(&a, 0, 0), 4) && !memcmp(px(&ru, 11, 11), px(&a, 7, 7), 4));
            CHECK(!memcmp(px(&rv, 63, 63), px(&a, 63, 63), 4) && !memcmp(px(&rv, 0, 0), px(&a, 0, 0), 4));
        }
        rb_done(&a);
        rb_done(&ru);
        rb_done(&rv);
        gpu_texture_destroy(g_d, u, 0);
        gpu_texture_destroy(g_d, v, 0);
        gpu_texture_destroy(g_d, t, 0);
    }
}

/* ---- Clears ---- */

static void test_clears(void)
{
    gpu_texture_desc td = tex_desc(32, 32, GPU_FORMAT_R8G8B8A8_UNORM, GPU_USE_RT | GPU_USE_COPY_SRC,
                                   GPU_STATE_RENDER_TARGET);
    td.clear_format = GPU_FORMAT_R8G8B8A8_UNORM;
    td.clear_color[2] = td.clear_color[3] = 1.0f;
    const gpu_texture t = gpu_texture_create(g_d, &td);
    gpu_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.texture = t;
    vd.kind = GPU_VIEW_RTV;
    const gpu_view rtv = gpu_view_get(g_d, &vd);
    CHECK(GPU_VALID(t) && GPU_VALID(rtv));
    const float blue[4] = {0, 0, 1, 1}, red[4] = {1, 0, 0, 1};
    const gpu_rect left = {0, 0, 16, 32};
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = rtv;
    gpu_cmd* c = gpu_cmd_begin(g_d, 0);
    gpu_cmd_pass_begin(c, &pass);
    gpu_cmd_clear_color(c, rtv, blue, NULL);
    gpu_cmd_clear_color(c, rtv, red, &left);
    gpu_cmd_pass_end(c);
    gpu_cmd_barrier(c, t, GPU_STATE_RENDER_TARGET, GPU_STATE_COPY_SOURCE);
    CHECK(run(g_d, c));
    readback rb;
    CHECK(tex_read(t, &td, &rb));
    if (rb.p) {
        const uint8_t* a = px(&rb, 0, 0);
        const uint8_t* b = px(&rb, 31, 31);
        const uint8_t* e = px(&rb, 15, 31);
        const uint8_t* f = px(&rb, 16, 0);
        CHECK(a[0] == 255 && a[1] == 0 && a[2] == 0 && a[3] == 255);
        CHECK(b[0] == 0 && b[1] == 0 && b[2] == 255 && b[3] == 255);
        CHECK(e[0] == 255 && f[2] == 255 && f[0] == 0);
    }
    rb_done(&rb);
    gpu_texture_destroy(g_d, t, 0);

    /* Depth: a D32 resource made TYPELESS, its DSV's format from it. */
    gpu_texture_desc dd = tex_desc(16, 16, GPU_FORMAT_R32_TYPELESS, GPU_USE_DS | GPU_USE_COPY_SRC,
                                   GPU_STATE_DEPTH_WRITE);
    dd.clear_format = GPU_FORMAT_D32_FLOAT;
    dd.clear_depth = 1.0f;
    const gpu_texture z = gpu_texture_create(g_d, &dd);
    memset(&vd, 0, sizeof vd);
    vd.texture = z;
    vd.kind = GPU_VIEW_DSV;
    const gpu_view dsv = gpu_view_get(g_d, &vd);
    vd.flags = GPU_VIEW_READ_ONLY_DEPTH;
    const gpu_view ro = gpu_view_get(g_d, &vd);
    CHECK(GPU_VALID(z) && GPU_VALID(dsv) && GPU_VALID(ro) && dsv.id != ro.id);
    c = gpu_cmd_begin(g_d, 0);
    gpu_cmd_clear_depth(c, dsv, GPU_CLEAR_DEPTH, 0.375f, 0, NULL);
    gpu_cmd_barrier(c, z, GPU_STATE_DEPTH_WRITE, GPU_STATE_COPY_SOURCE);
    CHECK(run(g_d, c));
    CHECK(tex_read(z, &dd, &rb));
    if (rb.p) {
        float v;
        memcpy(&v, px(&rb, 5, 5), sizeof v);
        CHECK(v == 0.375f);
    }
    rb_done(&rb);
    gpu_texture_destroy(g_d, z, 0);

    /* Today's zeta surface: R24G8 with a D24S8 clear value, a DSV and a
     * read-only one (cleared, not read back: its two planes). */
    gpu_texture_desc zd = tex_desc(16, 16, GPU_FORMAT_R24G8_TYPELESS, GPU_USE_DS | GPU_USE_SAMPLED,
                                   GPU_STATE_DEPTH_WRITE);
    zd.clear_format = GPU_FORMAT_D24_UNORM_S8_UINT;
    zd.clear_depth = 1.0f;
    const gpu_texture zt = gpu_texture_create(g_d, &zd);
    memset(&vd, 0, sizeof vd);
    vd.texture = zt;
    vd.kind = GPU_VIEW_DSV;
    vd.format = GPU_FORMAT_D24_UNORM_S8_UINT;
    const gpu_view zv = gpu_view_get(g_d, &vd);
    vd.flags = GPU_VIEW_READ_ONLY_DEPTH | GPU_VIEW_READ_ONLY_STENCIL;
    const gpu_view zr = gpu_view_get(g_d, &vd);
    CHECK(GPU_VALID(zt) && GPU_VALID(zv) && GPU_VALID(zr) && zv.id != zr.id);
    c = gpu_cmd_begin(g_d, 0);
    memset(&pass, 0, sizeof pass);
    pass.ds = zv;
    gpu_cmd_pass_begin(c, &pass);
    gpu_cmd_clear_depth(c, zv, GPU_CLEAR_DEPTH | GPU_CLEAR_STENCIL, 1.0f, 0, NULL);
    CHECK(run(g_d, c));
    gpu_texture_destroy(g_d, zt, 0);
}

/* ---- Layouts: today's root signatures, byte for byte ---- */

static uint64_t blob_hash(const D3D12_ROOT_SIGNATURE_DESC* rd)
{
    ID3DBlob* sig = NULL;
    ID3DBlob* err = NULL;
    if (FAILED(D3D12SerializeRootSignature(rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        if (err) err->lpVtbl->Release(err);
        return 0;
    }
    const uint64_t h = gpu_d3d12_fnv(GPU_D3D12_FNV_BASIS, sig->lpVtbl->GetBufferPointer(sig),
                                     sig->lpVtbl->GetBufferSize(sig));
    sig->lpVtbl->Release(sig);
    return h;
}

/* init_d3d12's vp_root_sig (the guest pipelines'), as it builds it. */
static uint64_t legacy_vp_root_hash(void)
{
    D3D12_DESCRIPTOR_RANGE srv_range = {0};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 16;
    srv_range.BaseShaderRegister = 0;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER rp[6] = {0};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[0].Descriptor.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &srv_range;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[2].Descriptor.ShaderRegister = 1;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE samp_range = {0};
    samp_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samp_range.NumDescriptors = 16;
    samp_range.BaseShaderRegister = 0;
    samp_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[3].DescriptorTable.NumDescriptorRanges = 1;
    rp[3].DescriptorTable.pDescriptorRanges = &samp_range;
    rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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
    return blob_hash(&rd);
}

/* blit_init's blit_rs: one SRV and a static linear clamp sampler. */
static uint64_t legacy_blit_root_hash(void)
{
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
    return blob_hash(&rsd);
}

/* smaa_ready's: three SRVs, four constants for every stage, two static samplers. */
static uint64_t legacy_smaa_root_hash(void)
{
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0,
                                    D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12_ROOT_PARAMETER rp[2] = {0};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &range;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 4;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC smp[2] = {{0}};
    for (int k = 0; k < 2; k++) {
        smp[k].Filter = k ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        smp[k].AddressU = smp[k].AddressV = smp[k].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp[k].MaxLOD = D3D12_FLOAT32_MAX;
        smp[k].ShaderRegister = (UINT)k;
        smp[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = {2, rp, 2, smp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    return blob_hash(&rsd);
}

static gpu_layout_slot slot(uint8_t kind, uint8_t reg, uint8_t count, uint8_t vis)
{
    gpu_layout_slot s;
    s.kind = kind;
    s.reg = reg;
    s.count = count;
    s.visibility = vis;
    return s;
}

static gpu_static_sampler clamp_sampler(uint32_t filter, uint32_t reg)
{
    gpu_static_sampler s;
    memset(&s, 0, sizeof s);
    s.desc.filter = filter;
    s.desc.address_u = s.desc.address_v = s.desc.address_w = GPU_ADDRESS_CLAMP;
    s.desc.max_lod = GPU_LOD_MAX;
    s.reg = reg;
    s.visibility = GPU_VIS_PIXEL;
    return s;
}

/* The guest pipelines' layout (gpu.h's comment). */
static gpu_layout guest_layout(gpu_device* d)
{
    gpu_layout_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.flags = GPU_LAYOUT_VERTEX_INPUT;
    ld.nslots = 6;
    ld.slot[0] = slot(GPU_SLOT_CB, 0, 0, GPU_VIS_VERTEX);
    ld.slot[1] = slot(GPU_SLOT_TABLE_TEX, 0, 16, GPU_VIS_PIXEL);
    ld.slot[2] = slot(GPU_SLOT_CB, 1, 0, GPU_VIS_PIXEL);
    ld.slot[3] = slot(GPU_SLOT_TABLE_SAMPLER, 0, 16, GPU_VIS_PIXEL);
    ld.slot[4] = slot(GPU_SLOT_RAW, 20, 0, GPU_VIS_VERTEX);
    ld.slot[5] = slot(GPU_SLOT_CONST32, 2, 1, GPU_VIS_VERTEX);
    return gpu_layout_create(d, &ld);
}

static void test_layouts(void)
{
    const gpu_layout guest = guest_layout(g_d);
    CHECK(GPU_VALID(guest));
    CHECK(gpu_d3d12_layout_hash(g_d, guest) == legacy_vp_root_hash());   /* today's s_vp_root_hash */

    gpu_layout_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.nslots = 1;
    ld.slot[0] = slot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
    ld.nstatic = 1;
    ld.statics[0] = clamp_sampler(GPU_FILTER(1, 1, 1, 0), 0);
    const gpu_layout blit = gpu_layout_create(g_d, &ld);
    CHECK(GPU_VALID(blit) && gpu_d3d12_layout_hash(g_d, blit) == legacy_blit_root_hash());

    memset(&ld, 0, sizeof ld);
    ld.nslots = 2;
    ld.slot[0] = slot(GPU_SLOT_TABLE_TEX, 0, 3, GPU_VIS_PIXEL);
    ld.slot[1] = slot(GPU_SLOT_CONST32, 0, 4, GPU_VIS_ALL);
    ld.nstatic = 2;
    ld.statics[0] = clamp_sampler(GPU_FILTER(1, 1, 0, 0), 0);
    ld.statics[1] = clamp_sampler(GPU_FILTER(0, 0, 0, 0), 1);
    const gpu_layout smaa = gpu_layout_create(g_d, &ld);
    CHECK(GPU_VALID(smaa) && gpu_d3d12_layout_hash(g_d, smaa) == legacy_smaa_root_hash());
    CHECK(gpu_native(g_d, GPU_NATIVE_LAYOUT, guest.id) != NULL);
}

/* ---- Pipeline descriptions: today's recipes against the conversion ---- */

/* vp_fp_desc as today's renderer has it, with its diagnostic knobs
 * (CULL_OFF, CULL_FLIP, CMASK_FORCE, DEPTH_OFF) off. */
static void legacy_vp_fp_desc(D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, D3D12_INPUT_ELEMENT_DESC il[16],
                              ID3D12RootSignature* rs, const void* vs, SIZE_T vs_len, int fetch,
                              const void* ps, SIZE_T ps_len, uint32_t blend, int nrt, DXGI_FORMAT rtfmt,
                              uint32_t cmask, uint32_t cull, uint32_t depth)
{
    for (int _i = 0; _i < 16; _i++) {
        il[_i].SemanticName = "ATTR"; il[_i].SemanticIndex = _i;
        il[_i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; il[_i].InputSlot = 0;
        il[_i].AlignedByteOffset = (UINT)(_i * 16);
        il[_i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_i].InstanceDataStepRate = 0;
    }
    memset(pd, 0, sizeof *pd);
    pd->pRootSignature = rs;
    pd->VS.pShaderBytecode = vs; pd->VS.BytecodeLength = vs_len;
    pd->PS.pShaderBytecode = ps; pd->PS.BytecodeLength = ps_len;
    pd->InputLayout.pInputElementDescs = il; pd->InputLayout.NumElements = 16;
    if (fetch) pd->InputLayout.NumElements = 0;
    pd->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd->RasterizerState.CullMode = (cull & 1u) ? ((cull & 2u) ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK)
                                               : D3D12_CULL_MODE_NONE;
    pd->RasterizerState.FrontCounterClockwise = (cull & 4u) ? TRUE : FALSE;
    pd->BlendState.RenderTarget[0].BlendEnable    = (blend & 1) ? TRUE : FALSE;
    pd->BlendState.RenderTarget[0].SrcBlend       = (blend & 1) ? (D3D12_BLEND)((blend >> 1)  & 0x1F) : D3D12_BLEND_ONE;
    pd->BlendState.RenderTarget[0].DestBlend      = (blend & 1) ? (D3D12_BLEND)((blend >> 6)  & 0x1F) : D3D12_BLEND_ZERO;
    pd->BlendState.RenderTarget[0].BlendOp        = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 21) & 0x7) : D3D12_BLEND_OP_ADD;
    pd->BlendState.RenderTarget[0].SrcBlendAlpha  = (blend & 1) ? (D3D12_BLEND)((blend >> 11) & 0x1F) : D3D12_BLEND_ONE;
    pd->BlendState.RenderTarget[0].DestBlendAlpha = (blend & 1) ? (D3D12_BLEND)((blend >> 16) & 0x1F) : D3D12_BLEND_ZERO;
    pd->BlendState.RenderTarget[0].BlendOpAlpha   = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 24) & 0x7) : D3D12_BLEND_OP_ADD;
    pd->BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd->SampleMask = UINT_MAX;
    pd->PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd->IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
    pd->NumRenderTargets = (UINT)nrt;
    for (int _r = 0; _r < nrt; _r++) {
        pd->RTVFormats[_r] = rtfmt;
        pd->BlendState.RenderTarget[_r] = pd->BlendState.RenderTarget[0];
    }
    for (int _r = 0; _r < nrt; _r++)
        pd->BlendState.RenderTarget[_r].RenderTargetWriteMask = (UINT8)(cmask & 0xF);
    pd->DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd->DepthStencilState.DepthEnable = (depth & 1u) ? TRUE : FALSE;
    pd->DepthStencilState.DepthWriteMask = (depth & 2u) ? D3D12_DEPTH_WRITE_MASK_ALL
                                                        : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd->DepthStencilState.DepthFunc = (D3D12_COMPARISON_FUNC)(((depth >> 2) & 7u) + 1u);
    pd->DepthStencilState.StencilEnable = FALSE;
    pd->SampleDesc.Count = 1;
}

/* The same pipeline as the conversion describes it (call_map section F). */
static void hal_vp_fp_desc(gpu_pipeline_desc* d, gpu_layout layout, gpu_shader vs, int fetch, gpu_shader ps,
                           uint32_t blend, int nrt, gpu_format rtfmt, uint32_t cmask, uint32_t cull, uint32_t depth)
{
    memset(d, 0, sizeof *d);
    d->layout = layout;
    d->vs = vs;
    d->ps = ps;
    if (!fetch) {
        d->vertex.count = 16;
        d->vertex.stride = 256;
        for (uint32_t i = 0; i < 16; i++) {
            memcpy(d->vertex.attr[i].semantic, "ATTR", 5);
            d->vertex.attr[i].semantic_index = i;
            d->vertex.attr[i].format = GPU_FORMAT_R32G32B32A32_FLOAT;
            d->vertex.attr[i].offset = i * 16;
        }
    }
    d->topology_type = GPU_TOPOLOGY_TYPE_TRIANGLE;
    d->cull = (cull & 1u) ? ((cull & 2u) ? GPU_CULL_FRONT : GPU_CULL_BACK) : GPU_CULL_NONE;
    d->front_ccw = (cull & 4u) ? 1 : 0;
    d->depth_test = (uint8_t)(depth & 1u);
    d->depth_write = (uint8_t)((depth >> 1) & 1u);
    d->depth_func = (uint8_t)(((depth >> 2) & 7u) + 1u);
    const int on = (blend & 1) != 0;
    d->blend.enable = (uint8_t)on;
    d->blend.src = (uint8_t)(on ? (blend >> 1) & 0x1F : GPU_BLEND_ONE);
    d->blend.dst = (uint8_t)(on ? (blend >> 6) & 0x1F : GPU_BLEND_ZERO);
    d->blend.op = (uint8_t)(on ? (blend >> 21) & 0x7 : GPU_BLEND_OP_ADD);
    d->blend.src_alpha = (uint8_t)(on ? (blend >> 11) & 0x1F : GPU_BLEND_ONE);
    d->blend.dst_alpha = (uint8_t)(on ? (blend >> 16) & 0x1F : GPU_BLEND_ZERO);
    d->blend.op_alpha = (uint8_t)(on ? (blend >> 24) & 0x7 : GPU_BLEND_OP_ADD);
    d->blend.write_mask = (uint8_t)(cmask & 0xF);
    d->nrt = (uint32_t)nrt;
    for (int r = 0; r < nrt; r++) d->rt_format[r] = rtfmt;
    d->ds_format = GPU_FORMAT_D24_UNORM_S8_UINT;
    d->samples = 1;
    d->sample_mask = 0xFFFFFFFFu;
    d->strip_cut = GPU_STRIP_CUT_FFFFFFFF;
}

/* Equal but for where the pointers point: the shaders' bytes, the input
 * layouts' fields and names, everything else byte for byte. */
static int same_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* a, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* b)
{
    if (a->pRootSignature != b->pRootSignature) return 0;
    const D3D12_SHADER_BYTECODE* sa[3] = {&a->VS, &a->GS, &a->PS};
    const D3D12_SHADER_BYTECODE* sb[3] = {&b->VS, &b->GS, &b->PS};
    for (int k = 0; k < 3; k++) {
        if (sa[k]->BytecodeLength != sb[k]->BytecodeLength) return 0;
        if (sa[k]->BytecodeLength && memcmp(sa[k]->pShaderBytecode, sb[k]->pShaderBytecode, sa[k]->BytecodeLength))
            return 0;
    }
    if (a->InputLayout.NumElements != b->InputLayout.NumElements) return 0;
    for (UINT i = 0; i < a->InputLayout.NumElements; i++) {
        D3D12_INPUT_ELEMENT_DESC x = a->InputLayout.pInputElementDescs[i];
        D3D12_INPUT_ELEMENT_DESC y = b->InputLayout.pInputElementDescs[i];
        if (strcmp(x.SemanticName, y.SemanticName)) return 0;
        x.SemanticName = y.SemanticName = NULL;
        if (memcmp(&x, &y, sizeof x)) return 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC ca, cb;
    memcpy(&ca, a, sizeof ca);
    memcpy(&cb, b, sizeof cb);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC* c2[2] = {&ca, &cb};
    for (int k = 0; k < 2; k++) {
        c2[k]->pRootSignature = NULL;
        c2[k]->VS.pShaderBytecode = c2[k]->GS.pShaderBytecode = c2[k]->PS.pShaderBytecode = NULL;
        c2[k]->InputLayout.pInputElementDescs = NULL;
    }
    return !memcmp(&ca, &cb, sizeof ca);
}

static void test_pipeline_descs(gpu_layout layout, gpu_shader vs, gpu_shader ps)
{
    const gpu_d3d12_shader* v = &g_dd->shader[GPU_HANDLE_INDEX(vs.id)];
    const gpu_d3d12_shader* p = &g_dd->shader[GPU_HANDLE_INDEX(ps.id)];
    ID3D12RootSignature* rs = (ID3D12RootSignature*)gpu_native(g_d, GPU_NATIVE_LAYOUT, layout.id);
    /* blend keys: off; SRC_ALPHA / INV_SRC_ALPHA ADD (straight alpha); ONE / ONE ADD (additive) */
    const uint32_t alpha = 1u | (5u << 1) | (6u << 6) | (1u << 21) | (2u << 11) | (6u << 16) | (1u << 24);
    const uint32_t add = 1u | (2u << 1) | (2u << 6) | (1u << 21) | (2u << 11) | (2u << 16) | (1u << 24);
    static const struct { uint32_t blend; int nrt; gpu_format f; uint32_t cmask, cull, depth; int fetch; } k[] = {
        {0, 1, GPU_FORMAT_R8G8B8A8_UNORM, 0xF, 0, 1u | 2u | (3u << 2), 0},
        {0, 2, GPU_FORMAT_R16G16B16A16_FLOAT, 0x7, 1u | 4u, 0, 1},
        {0, 4, GPU_FORMAT_R8G8B8A8_UNORM, 0x0, 3u, 1u | (6u << 2), 0},
        {0, 0, GPU_FORMAT_UNKNOWN, 0xF, 1u, 1u | 2u, 0},   /* depth only */
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        const uint32_t blend = i == 1 ? alpha : i == 2 ? add : k[i].blend;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC a, b;
        D3D12_INPUT_ELEMENT_DESC ila[16], ilb[16];
        memset(ila, 0, sizeof ila);
        legacy_vp_fp_desc(&a, ila, rs, v->code, (SIZE_T)v->size, k[i].fetch, p->code, (SIZE_T)p->size, blend,
                          k[i].nrt, (DXGI_FORMAT)k[i].f, k[i].cmask, k[i].cull, k[i].depth);
        gpu_pipeline_desc hd;
        hal_vp_fp_desc(&hd, layout, vs, k[i].fetch, ps, blend, k[i].nrt, k[i].f, k[i].cmask, k[i].cull, k[i].depth);
        gpu_d3d12_pipeline_d3d_desc(g_d, &hd, &b, ilb);
        if (!same_pso(&a, &b)) printf("  guest pipeline description %u differs from vp_fp_desc's\n", (unsigned)i);
        CHECK(same_pso(&a, &b));
    }
    /* A post pass (of_ready, blit_init): DepthClip on, RT0 writes all, RGBA8. */
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC a = {0}, b;
        D3D12_INPUT_ELEMENT_DESC il[16];
        a.pRootSignature = rs;
        a.VS.pShaderBytecode = v->code; a.VS.BytecodeLength = (SIZE_T)v->size;
        a.PS.pShaderBytecode = p->code; a.PS.BytecodeLength = (SIZE_T)p->size;
        a.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        a.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        a.RasterizerState.DepthClipEnable = TRUE;
        a.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        a.SampleMask = UINT_MAX;
        a.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        a.NumRenderTargets = 1;
        a.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        a.SampleDesc.Count = 1;
        a.InputLayout.pInputElementDescs = il;   /* (none: the comparison skips it) */
        gpu_pipeline_desc hd;
        memset(&hd, 0, sizeof hd);
        hd.layout = layout;
        hd.vs = vs;
        hd.ps = ps;
        hd.topology_type = GPU_TOPOLOGY_TYPE_TRIANGLE;
        hd.cull = GPU_CULL_NONE;
        hd.depth_clip = 1;
        hd.blend.write_mask = GPU_WRITE_ALL;
        hd.nrt = 1;
        hd.rt_format[0] = GPU_FORMAT_R8G8B8A8_UNORM;
        hd.samples = 1;
        hd.sample_mask = 0xFFFFFFFFu;
        gpu_d3d12_pipeline_d3d_desc(g_d, &hd, &b, il);
        CHECK(same_pso(&a, &b));
    }
}

/* ---- A draw on the guest layout ---- */

static const char k_vs[] =
    "cbuffer C0 : register(b0) { float4 offs; };\n"
    "cbuffer C2 : register(b2) { uint k; };\n"
    "ByteAddressBuffer raw : register(t20);\n"
    "struct VI { float4 p : ATTR0; float4 c : ATTR1; };\n"
    "struct VO { float4 p : SV_Position; float4 c : COLOR0; };\n"
    "VO main(VI i) { VO o; o.p = i.p + offs; o.c = i.c * asfloat(raw.Load(k * 4)); return o; }\n";
static const char k_ps[] =
    "cbuffer C1 : register(b1) { float4 tint; };\n"
    "Texture2D t0 : register(t0);\n"
    "SamplerState s0 : register(s0);\n"
    "float4 main(float4 p : SV_Position, float4 c : COLOR0) : SV_Target {\n"
    "    return c * tint * t0.Sample(s0, float2(0.5, 0.5)); }\n";

static void draw_desc(gpu_pipeline_desc* pd, gpu_layout layout, gpu_shader vs, gpu_shader ps)
{
    memset(pd, 0, sizeof *pd);
    pd->layout = layout;
    pd->vs = vs;
    pd->ps = ps;
    pd->vertex.count = 2;
    pd->vertex.stride = 32;
    for (uint32_t i = 0; i < 2; i++) {
        memcpy(pd->vertex.attr[i].semantic, "ATTR", 5);
        pd->vertex.attr[i].semantic_index = i;
        pd->vertex.attr[i].format = GPU_FORMAT_R32G32B32A32_FLOAT;
        pd->vertex.attr[i].offset = i * 16;
    }
    pd->topology_type = GPU_TOPOLOGY_TYPE_TRIANGLE;
    pd->cull = GPU_CULL_NONE;
    pd->depth_func = GPU_CMP_ALWAYS;
    pd->blend.src = pd->blend.src_alpha = GPU_BLEND_ONE;
    pd->blend.dst = pd->blend.dst_alpha = GPU_BLEND_ZERO;
    pd->blend.op = pd->blend.op_alpha = GPU_BLEND_OP_ADD;
    pd->blend.write_mask = GPU_WRITE_ALL;
    pd->nrt = 1;
    pd->rt_format[0] = GPU_FORMAT_R8G8B8A8_UNORM;
    pd->samples = 1;
    pd->sample_mask = 0xFFFFFFFFu;
}

/* What a draw binds, made once. */
typedef struct scene {
    gpu_layout     layout;
    gpu_shader     vs, ps;
    gpu_pipeline   pso;
    gpu_pipeline_desc desc;
    gpu_texture    rt, texel;
    gpu_view       rtv;
    gpu_table_pool tex_pool, samp_pool;
    gpu_sampler    samp;
    gpu_buffer     vb, ib, cb, raw;
    gpu_query_pool queries;
} scene;

static int scene_make(scene* s)
{
    memset(s, 0, sizeof *s);
    s->layout = guest_layout(g_d);
    s->vs = sh_make(g_d, GPU_SC_VS, GPU_STAGE_VS, k_vs, "test_vs");
    s->ps = sh_make(g_d, GPU_SC_PS, GPU_STAGE_PS, k_ps, "test_ps");
    CHECK(GPU_VALID(s->layout) && GPU_VALID(s->vs) && GPU_VALID(s->ps));
    if (!GPU_VALID(s->vs) || !GPU_VALID(s->ps)) return 0;
    draw_desc(&s->desc, s->layout, s->vs, s->ps);
    uint32_t cached = 7;
    s->pso = gpu_pipeline_create(g_d, &s->desc, 0, &cached);
    CHECK(s->pso.v != 0 && cached == GPU_PIPELINE_COMPILED);   /* no library yet */

    gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, GPU_USE_RT | GPU_USE_COPY_SRC,
                                   GPU_STATE_RENDER_TARGET);
    s->rt = gpu_texture_create(g_d, &td);
    gpu_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.texture = s->rt;
    vd.kind = GPU_VIEW_RTV;
    s->rtv = gpu_view_get(g_d, &vd);

    /* One texel, (128, 255, 255, 255), sampled in the middle. */
    td = tex_desc(1, 1, GPU_FORMAT_R8G8B8A8_UNORM, GPU_USE_SAMPLED | GPU_USE_COPY_DST, GPU_STATE_COPY_DEST);
    s->texel = gpu_texture_create(g_d, &td);
    gpu_footprint fp;
    const uint64_t total = gpu_texture_footprints(g_d, &td, 0, 1, 0, &fp, NULL, NULL);
    const gpu_buffer up = buf_new(g_d, total, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    const uint8_t texel[4] = {128, 255, 255, 255};
    memcpy(buf_ptr(up), texel, 4);

    gpu_table_pool_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.kind = GPU_TABLE_TEX;
    pd.count = 64;
    pd.nregions = 1;
    pd.region[0].first = 16;
    pd.region[0].tables = 3;
    pd.region[0].size = 16;
    s->tex_pool = gpu_table_pool_create(g_d, &pd);
    pd.kind = GPU_TABLE_SAMPLER;
    pd.count = 32;
    pd.region[0].tables = 1;
    s->samp_pool = gpu_table_pool_create(g_d, &pd);
    gpu_sampler_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.filter = GPU_FILTER(0, 0, 0, 0);
    sd.address_u = sd.address_v = sd.address_w = GPU_ADDRESS_CLAMP;
    sd.max_aniso = 1;
    sd.compare = GPU_CMP_NEVER;
    sd.max_lod = GPU_LOD_MAX;
    s->samp = gpu_sampler_get(g_d, &sd);
    CHECK(GPU_VALID(s->tex_pool) && GPU_VALID(s->samp_pool) && GPU_VALID(s->samp));
    memset(&vd, 0, sizeof vd);
    vd.texture = s->texel;   /* every field at its default: written with a NULL description */
    gpu_fast_table_write_tex(g_d, s->tex_pool, 16, &vd);
    for (uint32_t u = 1; u < 16; u++) gpu_fast_table_write_tex(g_d, s->tex_pool, 16 + u, NULL);
    gpu_fast_table_write_sampler(g_d, s->samp_pool, 16, s->samp);

    /* A triangle over the whole target, its colour (1, 0.5, 0.25, 1). */
    const float verts[3][8] = {
        {-1, -1, 0, 1, 1, 0.5f, 0.25f, 1}, {-1, 3, 0, 1, 1, 0.5f, 0.25f, 1}, {3, -1, 0, 1, 1, 0.5f, 0.25f, 1}};
    const uint32_t idx[3] = {0, 1, 2};
    const float consts[2][64] = {{0}, {1, 1, 1, 1}};   /* b0 offs 0 at 0; b1 tint at 256 */
    const float rawv[2] = {0.0f, 1.0f};               /* b2's k = 1 reads 1.0 */
    s->vb = buf_new(g_d, sizeof verts, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    s->ib = buf_new(g_d, sizeof idx, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    s->cb = buf_new(g_d, sizeof consts, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    s->raw = buf_new(g_d, 256, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    memcpy(buf_ptr(s->vb), verts, sizeof verts);
    memcpy(buf_ptr(s->ib), idx, sizeof idx);
    memcpy(buf_ptr(s->cb), consts, sizeof consts);
    memcpy(buf_ptr(s->raw), rawv, sizeof rawv);
    s->queries = gpu_query_pool_create(g_d, GPU_QUERY_OCCLUSION, 4);
    CHECK(GPU_VALID(s->queries));

    gpu_cmd* c = gpu_cmd_begin(g_d, 0);
    gpu_cmd_copy_buf_to_tex(c, s->texel, 0, 0, 0, 0, up, &fp);
    gpu_cmd_barrier(c, s->texel, GPU_STATE_COPY_DEST, GPU_STATE_PIXEL_SHADER_RESOURCE);
    const int ok = run(g_d, c);
    CHECK(ok);
    gpu_buffer_destroy(g_d, up, 0);
    return ok && s->pso.v;
}

/* Records today's per-draw binds for one draw of the triangle. */
static void scene_binds(gpu_cmd* c, const scene* s, gpu_pipeline pso, gpu_layout layout, gpu_table tex_table)
{
    const gpu_viewport vp = {0, 0, 16, 16, 0, 1};
    const gpu_rect sc = {0, 0, 16, 16};
    gpu_cmd_set_table_pools(c, s->tex_pool, s->samp_pool);
    gpu_cmd_set_layout(c, layout);
    gpu_cmd_set_pipeline(c, pso);
    gpu_cmd_set_topology(c, GPU_TOPO_TRIANGLELIST);
    gpu_cmd_set_viewport(c, &vp);
    gpu_cmd_set_scissor(c, &sc);
    gpu_cmd_bind_cb(c, 0, gpu_fast_buffer_addr(g_d, s->cb));
    gpu_cmd_bind_table(c, 1, tex_table);
    gpu_cmd_bind_cb(c, 2, gpu_fast_buffer_addr(g_d, s->cb) + 256);
    gpu_cmd_bind_table(c, 3, gpu_fast_table_ref(g_d, s->samp_pool, 16));
    gpu_cmd_bind_raw(c, 4, gpu_fast_buffer_addr(g_d, s->raw));
    gpu_cmd_push_const1(c, 5, 1, 0);
    gpu_cmd_set_vb(c, 0, gpu_fast_buffer_addr(g_d, s->vb), 96, 32);
}

/* Draws into the target, reads back its middle pixel: 1 when it is
 * (1, 0.5, 0.25, 1) * the texel. */
static int scene_draw_check(const scene* s, gpu_pipeline pso, gpu_layout layout, gpu_table tex_table,
                            gpu_table_pool tex_pool)
{
    scene t = *s;
    t.tex_pool = tex_pool;
    const float zero[4] = {0, 0, 0, 0};
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = s->rtv;
    gpu_cmd* c = gpu_cmd_begin(g_d, 0);
    gpu_cmd_pass_begin(c, &pass);
    gpu_cmd_clear_color(c, s->rtv, zero, NULL);
    scene_binds(c, &t, pso, layout, tex_table);
    gpu_cmd_draw(c, 3, 1, 0, 0);
    gpu_cmd_barrier(c, s->rt, GPU_STATE_RENDER_TARGET, GPU_STATE_COPY_SOURCE);
    const int ok = run(g_d, c);
    const gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0);
    readback rb;
    int good = 0;
    if (ok && tex_read(s->rt, &td, &rb)) {
        const uint8_t* p = px(&rb, 8, 8);
        good = close_to(p[0], 128) && close_to(p[1], 128) && close_to(p[2], 64) && p[3] == 255;
        if (!good) printf("  pixel (8, 8) is %u %u %u %u\n", p[0], p[1], p[2], p[3]);
    }
    rb_done(&rb);
    c = gpu_cmd_begin(g_d, 0);
    gpu_cmd_barrier(c, s->rt, GPU_STATE_COPY_SOURCE, GPU_STATE_RENDER_TARGET);
    run(g_d, c);
    return good;
}

static void test_draw(scene* s)
{
    /* The draw, then occlusion counts: the whole 16x16 target from a draw and
     * an indexed draw, then a 4x4 scissor. */
    CHECK(scene_draw_check(s, s->pso, s->layout, gpu_fast_table_ref(g_d, s->tex_pool, 16), s->tex_pool));
    const gpu_buffer qrb = buf_new(g_d, 4 * sizeof(uint64_t), GPU_MEM_READBACK, GPU_STATE_COPY_DEST);
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = s->rtv;
    gpu_cmd* c = gpu_cmd_begin(g_d, 0);
    gpu_cmd_pass_begin(c, &pass);
    scene_binds(c, s, s->pso, s->layout, gpu_fast_table_ref(g_d, s->tex_pool, 16));
    gpu_cmd_marker(c, "test draw 0", 11);
    gpu_cmd_query_reset(c, s->queries, 0, 3);
    gpu_cmd_query_begin(c, s->queries, 0);
    gpu_cmd_draw(c, 3, 1, 0, 0);
    gpu_cmd_query_end(c, s->queries, 0);
    gpu_cmd_set_ib(c, gpu_fast_buffer_addr(g_d, s->ib), 12, GPU_FORMAT_R32_UINT);
    gpu_cmd_query_begin(c, s->queries, 1);
    gpu_cmd_draw_indexed(c, 3, 1, 0, 0, 0);
    gpu_cmd_query_end(c, s->queries, 1);
    const gpu_rect corner = {0, 0, 4, 4};
    gpu_cmd_set_scissor(c, &corner);
    gpu_cmd_query_begin(c, s->queries, 2);
    gpu_cmd_draw(c, 3, 1, 0, 0);
    gpu_cmd_query_end(c, s->queries, 2);
    gpu_cmd_pass_end(c);
    gpu_cmd_query_resolve(c, s->queries, 0, 3, qrb, 0);
    CHECK(run(g_d, c));
    const uint64_t* q = (const uint64_t*)gpu_buffer_map(g_d, qrb, 0, 3 * sizeof(uint64_t));
    CHECK(q != NULL);
    if (q) {
        if (q[0] != 256 || q[1] != 256 || q[2] != 16)
            printf("  occlusion counts %llu %llu %llu\n", (unsigned long long)q[0], (unsigned long long)q[1],
                   (unsigned long long)q[2]);
        CHECK(q[0] == 256 && q[1] == 256 && q[2] == 16);
    }
    gpu_buffer_unmap(g_d, qrb, 0, 0);
    gpu_buffer_destroy(g_d, qrb, 0);

    /* The same description again: the pipeline is the library's when the
     * device has one (none until gpu_pipeline_cache_load). */
    uint32_t cached = 7;
    const gpu_pipeline again = gpu_pipeline_create(g_d, &s->desc, 0, &cached);
    CHECK(again.v != 0 && cached == GPU_PIPELINE_COMPILED);
}

/* ---- The memo, caches, fences ---- */

static void test_memo(const scene* s)
{
    gpu_table_pool_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.kind = GPU_TABLE_TEX;
    pd.count = 16;
    const gpu_table_pool pool = gpu_table_pool_create(g_d, &pd);
    CHECK(GPU_VALID(pool));
    const LONG n0 = g_dd->n_srv_writes;
    gpu_view_desc a;
    memset(&a, 0, sizeof a);
    a.texture = s->texel;
    a.mips = 1;
    a.format = GPU_FORMAT_R8G8B8A8_UNORM;
    gpu_fast_table_write_tex(g_d, pool, 3, &a);   CHECK(g_dd->n_srv_writes == n0 + 1);
    gpu_fast_table_write_tex(g_d, pool, 3, &a);   CHECK(g_dd->n_srv_writes == n0 + 1);   /* the same: kept */
    gpu_table_write_tex(g_d, pool, 3, &a);        CHECK(g_dd->n_srv_writes == n0 + 1);   /* through the table too */
    a.swizzle = GPU_SWIZZLE_IDENTITY;
    gpu_fast_table_write_tex(g_d, pool, 3, &a);   CHECK(g_dd->n_srv_writes == n0 + 1);   /* 0 is the identity */
    a.swizzle = GPU_SWIZZLE(GPU_SWZ_A, GPU_SWZ_ZERO, GPU_SWZ_ONE, GPU_SWZ_B);
    gpu_fast_table_write_tex(g_d, pool, 3, &a);   CHECK(g_dd->n_srv_writes == n0 + 2);   /* changed: written */
    gpu_fast_table_write_tex(g_d, pool, 4, &a);   CHECK(g_dd->n_srv_writes == n0 + 3);   /* another entry */
    gpu_view_desc e;
    memset(&e, 0, sizeof e);
    e.mips = 1;
    e.format = GPU_FORMAT_R8_UNORM;
    gpu_fast_table_write_tex(g_d, pool, 5, &e);   CHECK(g_dd->n_srv_writes == n0 + 3);   /* a new pool's empty entry */
    gpu_fast_table_write_tex(g_d, pool, 5, NULL); CHECK(g_dd->n_srv_writes == n0 + 4);
    gpu_fast_table_write_tex(g_d, pool, 5, NULL); CHECK(g_dd->n_srv_writes == n0 + 4);
    e.format = GPU_FORMAT_R8G8B8A8_UNORM;
    gpu_fast_table_write_tex(g_d, pool, 5, &e);   CHECK(g_dd->n_srv_writes == n0 + 4);   /* NULL's own */

    gpu_table_pool_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.kind = GPU_TABLE_SAMPLER;
    sd.count = 16;
    const gpu_table_pool sp = gpu_table_pool_create(g_d, &sd);
    gpu_sampler_desc smp;
    memset(&smp, 0, sizeof smp);
    smp.filter = GPU_FILTER_ANISO(0);
    smp.address_u = smp.address_v = smp.address_w = GPU_ADDRESS_WRAP;
    smp.max_aniso = 8;
    smp.compare = GPU_CMP_NEVER;
    smp.max_lod = GPU_LOD_MAX;
    const gpu_sampler s2 = gpu_sampler_get(g_d, &smp);
    const LONG m0 = g_dd->n_sampler_writes;
    gpu_fast_table_write_sampler(g_d, sp, 2, s->samp); CHECK(g_dd->n_sampler_writes == m0 + 1);
    gpu_fast_table_write_sampler(g_d, sp, 2, s->samp); CHECK(g_dd->n_sampler_writes == m0 + 1);
    gpu_table_write_sampler(g_d, sp, 2, s2);           CHECK(g_dd->n_sampler_writes == m0 + 2);

    /* Tables: today's start + index * increment, through both paths. */
    CHECK(gpu_table_ref(g_d, pool, 4).v - gpu_table_ref(g_d, pool, 3).v == g_dd->srv_inc);
    CHECK(gpu_fast_table_ref(g_d, pool, 9).v == gpu_table_ref(g_d, pool, 9).v);
}

static void test_caches(void)
{
    gpu_sampler_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.filter = GPU_FILTER(1, 1, 1, 0);
    sd.address_u = sd.address_v = sd.address_w = GPU_ADDRESS_MIRROR;
    sd.max_aniso = 1;
    sd.compare = GPU_CMP_NEVER;
    sd.max_lod = GPU_LOD_MAX;
    const gpu_sampler a = gpu_sampler_get(g_d, &sd);
    const gpu_sampler b = gpu_sampler_get(g_d, &sd);
    sd.mip_lod_bias = 0.5f;
    const gpu_sampler c = gpu_sampler_get(g_d, &sd);
    CHECK(GPU_VALID(a) && a.id == b.id && c.id != a.id);

    const gpu_texture_desc td = tex_desc(8, 8, GPU_FORMAT_R8G8B8A8_TYPELESS, GPU_USE_RT, GPU_STATE_RENDER_TARGET);
    const gpu_texture t = gpu_texture_create(g_d, &td);
    gpu_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.texture = t;
    vd.kind = GPU_VIEW_RTV;
    vd.format = GPU_FORMAT_R8G8B8A8_UNORM;
    const gpu_view v1 = gpu_view_get(g_d, &vd);
    const gpu_view v2 = gpu_view_get(g_d, &vd);
    vd.format = GPU_FORMAT_R8G8B8A8_UNORM_SRGB;
    const gpu_view v3 = gpu_view_get(g_d, &vd);
    CHECK(GPU_VALID(v1) && v1.id == v2.id && GPU_VALID(v3) && v3.id != v1.id);
    gpu_texture_destroy(g_d, t, 0);   /* its views go with it */
    CHECK(!gpu_d3d12_slot_valid(&g_dd->view_slots, v1.id) && !gpu_d3d12_slot_valid(&g_dd->view_slots, v3.id));
    CHECK(gpu_view_get(g_d, &vd).id == 0);   /* a stale texture has no views */
}

static void test_fences(void)
{
    const gpu_fence f = gpu_signal(g_d);
    CHECK(f != 0 && gpu_fence_wait(g_d, f, 5000) == 0 && gpu_fence_completed(g_d) >= f);
    const ULONGLONG t0 = GetTickCount64();
    CHECK(gpu_fence_wait(g_d, f + 1000000, 100) == 1);   /* never signalled: the timeout */
    const ULONGLONG ms = GetTickCount64() - t0;
    CHECK(ms >= 80 && ms < 5000);
    CHECK(gpu_lost(g_d) == 0);

    /* Freed once the GPU passes the fence: one it has passed (now), one it
     * will pass (at the next collection), one it never will (at gpu_destroy). */
    const gpu_buffer a = buf_new(g_d, 256, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    const gpu_buffer b = buf_new(g_d, 256, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    const gpu_buffer c = buf_new(g_d, 256, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    gpu_buffer_destroy(g_d, a, f);
    CHECK(gpu_native(g_d, GPU_NATIVE_BUFFER, a.id) == NULL);
    gpu_buffer_destroy(g_d, b, f + 1);   /* the next signal's */
    gpu_buffer_destroy(g_d, c, f + 1000000);
    gpu_wait_idle(g_d);
    CHECK(gpu_native(g_d, GPU_NATIVE_BUFFER, b.id) == NULL);
    CHECK(gpu_native(g_d, GPU_NATIVE_BUFFER, c.id) != NULL);
}

/* ---- The pipeline cache, across two devices ---- */

static void test_pipeline_cache(void)
{
    char path[MAX_PATH];
    const DWORD n = GetTempPathA(MAX_PATH - 40, path);
    if (!n || n >= MAX_PATH - 40) { CHECK(!"no temporary directory"); return; }
    snprintf(path + n, MAX_PATH - n, "ps3r_test_gpu_d3d12_%lu.lib", (unsigned long)GetCurrentProcessId());
    DeleteFileA(path);
    gpu_device_desc dd;
    memset(&dd, 0, sizeof dd);
    dd.flags = g_flags;
    dd.cmd_slots = 1;
    for (int run_no = 0; run_no < 2; run_no++) {
        gpu_device* d = gpu_d3d12_create(&dd);
        CHECK(d != NULL);
        if (!d) break;
        const int loaded = gpu_pipeline_cache_load(d, path);
        if (loaded == GPU_PIPELINE_CACHE_NONE) {
            CHECK(GPU_D3D12_DEV(d)->lib == NULL);
            printf("  pipeline cache: skipped (no pipeline library on this device)\n");
            gpu_destroy(d);
            break;
        }
        /* none the first time, the first device's the second */
        CHECK(loaded == (run_no ? GPU_PIPELINE_CACHE_LOADED : GPU_PIPELINE_CACHE_NEW));
        CHECK(GPU_D3D12_DEV(d)->lib != NULL);
        /* The layout and shaders made again on each device: the name is their
         * content's, not their handles'. */
        const gpu_layout l = guest_layout(d);
        const gpu_shader vs = sh_make(d, GPU_SC_VS, GPU_STAGE_VS, k_vs, "test_vs");
        const gpu_shader ps = sh_make(d, GPU_SC_PS, GPU_STAGE_PS, k_ps, "test_ps");
        gpu_pipeline_desc pd, pd2;
        draw_desc(&pd, l, vs, ps);
        pd2 = pd;
        pd2.depth_clip = 1;
        const gpu_d3d12_device* const g = GPU_D3D12_DEV(d);
        const uint32_t kept = g->npso;
        /* Made only for the library (today's startup builds): stored, or
         * loaded, then released; no handle, nothing kept. */
        uint32_t how = 7;
        CHECK(gpu_pipeline_create(d, &pd2, GPU_PIPELINE_STORE_ONLY, &how).v == 0);
        CHECK(how == (run_no ? GPU_PIPELINE_LOADED : GPU_PIPELINE_STORED) && g->npso == kept);
        how = 7;
        CHECK(gpu_pipeline_create(d, &pd2, GPU_PIPELINE_STORE_ONLY, &how).v == 0);
        CHECK(how == GPU_PIPELINE_LOADED && g->npso == kept);   /* the library has it */
        /* A draw's is kept, loaded from the library the build filled. */
        how = 7;
        const gpu_pipeline p0 = gpu_pipeline_create(d, &pd2, 0, &how);
        CHECK(p0.v != 0 && how == GPU_PIPELINE_LOADED && g->npso == kept + 1);
        CHECK(gpu_native(d, GPU_NATIVE_PIPELINE, p0.v) == (void*)(uintptr_t)p0.v);
        uint32_t cached = 7;
        const gpu_pipeline p1 = gpu_pipeline_create(d, &pd, 0, &cached);
        CHECK(p1.v != 0 && cached == (run_no ? GPU_PIPELINE_LOADED : GPU_PIPELINE_STORED));
        const gpu_pipeline p2 = gpu_pipeline_create(d, &pd, 0, &cached);
        CHECK(p2.v != 0 && cached == GPU_PIPELINE_LOADED);   /* stored by the first */
        CHECK(g->npso == kept + 3);
        gpu_shader_destroy(d, vs);          /* pipelines outlive their shaders */
        gpu_shader_destroy(d, ps);
        if (run_no == 0) CHECK(gpu_pipeline_cache_save(d, path) == 1);
        gpu_destroy(d);
    }
    /* No library (the cap masked, as a fallback would): pipelines are
     * compiled and never stored; a store-only one is compiled and released. */
    dd.cap_mask = GPU_CAP_PIPELINE_CACHE;
    gpu_device* const d = gpu_d3d12_create(&dd);
    CHECK(d != NULL);
    if (d) {
        CHECK(gpu_pipeline_cache_load(d, path) == GPU_PIPELINE_CACHE_NONE && GPU_D3D12_DEV(d)->lib == NULL);
        const gpu_layout l = guest_layout(d);
        const gpu_shader vs = sh_make(d, GPU_SC_VS, GPU_STAGE_VS, k_vs, "test_vs");
        const gpu_shader ps = sh_make(d, GPU_SC_PS, GPU_STAGE_PS, k_ps, "test_ps");
        gpu_pipeline_desc pd;
        draw_desc(&pd, l, vs, ps);
        uint32_t how = 7;
        CHECK(gpu_pipeline_create(d, &pd, GPU_PIPELINE_STORE_ONLY, &how).v == 0);
        CHECK(how == GPU_PIPELINE_COMPILED && GPU_D3D12_DEV(d)->npso == 0);
        how = 7;
        CHECK(gpu_pipeline_create(d, &pd, 0, &how).v != 0 && how == GPU_PIPELINE_COMPILED);
        CHECK(gpu_pipeline_cache_save(d, path) == 0);
        gpu_destroy(d);
    }
    DeleteFileA(path);
}

/* ---- A swap chain on a hidden window ---- */

static void test_swapchain(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ps3r_test_gpu_d3d12";
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, wc.lpszClassName, "test_gpu_d3d12", WS_OVERLAPPEDWINDOW, 0, 0, 160, 120,
                             NULL, NULL, wc.hInstance, NULL);   /* never shown */
    CHECK(w != NULL);
    if (!w) return;
    gpu_swapchain_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.window = w;
    sd.width = 64;
    sd.height = 48;
    sd.format = GPU_FORMAT_R8G8B8A8_UNORM;
    sd.buffers = 2;
    sd.colour_space = GPU_COLOR_SRGB;
    sd.flags = (g_d->caps.bits & GPU_CAP_TEARING) ? GPU_SWAP_TEARING : 0;
    const gpu_swapchain sc = gpu_swap_create(g_d, &sd);
    CHECK(GPU_VALID(sc) && gpu_native(g_d, GPU_NATIVE_SWAPCHAIN, sc.id) != NULL);
    if (GPU_VALID(sc)) {
        gpu_texture img;
        const uint32_t i = gpu_swap_acquire(g_d, sc, &img);
        CHECK(i < 2 && GPU_VALID(img));
        /* The bridge's image by index: the current one is acquire's; the other is another; none past 2. */
        CHECK(gpu_d3d12_swap_image(g_d, sc, i).id == img.id && GPU_VALID(gpu_d3d12_swap_image(g_d, sc, 1 - i)) &&
              gpu_d3d12_swap_image(g_d, sc, 1 - i).id != img.id && !GPU_VALID(gpu_d3d12_swap_image(g_d, sc, 2)));
        gpu_view_desc vd;
        memset(&vd, 0, sizeof vd);
        vd.texture = img;
        vd.kind = GPU_VIEW_RTV;
        const gpu_view rtv = gpu_view_get(g_d, &vd);
        const float grey[4] = {0.5f, 0.5f, 0.5f, 1};
        gpu_cmd* c = gpu_cmd_begin(g_d, 0);
        gpu_cmd_barrier(c, img, GPU_STATE_PRESENT, GPU_STATE_RENDER_TARGET);
        gpu_cmd_clear_color(c, rtv, grey, NULL);
        gpu_cmd_barrier(c, img, GPU_STATE_RENDER_TARGET, GPU_STATE_PRESENT);
        CHECK(run(g_d, c));
        CHECK(gpu_swap_present(g_d, sc, 1, 0) == 0);
        if (sd.flags & GPU_SWAP_TEARING) CHECK(gpu_swap_present(g_d, sc, 0, GPU_PRESENT_TEARING) == 0);
        gpu_present_stats st;
        memset(&st, 0, sizeof st);
        const int sr = gpu_swap_stats(g_d, sc, &st);
        printf("  swap chain: statistics %s (presents %u), sRGB colour space %s\n", sr ? "not yet" : "read",
               st.last_present_count, gpu_swap_hdr(g_d, sc, GPU_COLOR_SRGB, NULL) ? "taken" : "refused");
        gpu_texture_destroy(g_d, img, 0);   /* the swap chain's: not freed */
        CHECK(gpu_d3d12_slot_valid(&g_dd->tex_slots, img.id));
        gpu_wait_idle(g_d);
        CHECK(gpu_swap_resize(g_d, sc, 96, 64, GPU_FORMAT_R8G8B8A8_UNORM) == 0);
        CHECK(!gpu_d3d12_slot_valid(&g_dd->tex_slots, img.id) && !gpu_d3d12_slot_valid(&g_dd->view_slots, rtv.id));
        gpu_texture img2;
        const uint32_t i2 = gpu_swap_acquire(g_d, sc, &img2);
        CHECK(GPU_VALID(img2) && img2.id != img.id && gpu_d3d12_swap_image(g_d, sc, i2).id == img2.id);
        ID3D12Resource* r = (ID3D12Resource*)gpu_native(g_d, GPU_NATIVE_TEXTURE, img2.id);
        CHECK(r != NULL);
        if (r) {
            D3D12_RESOURCE_DESC rd;
            r->lpVtbl->GetDesc(r, &rd);
            CHECK(rd.Width == 96 && rd.Height == 64);
        }
        /* Every image by index (gpu_swap_image): the current one is acquire's, the other another, none
         * past the count, which is the buffers asked. */
        {
            gpu_texture cur;
            const uint32_t ci = gpu_swap_acquire(g_d, sc, &cur);
            const uint32_t n = gpu_swap_image_count(g_d, sc);
            CHECK(n == 2 && ci < n && gpu_swap_image(g_d, sc, ci).id == cur.id);
            CHECK(GPU_VALID(gpu_swap_image(g_d, sc, 1 - ci)) && gpu_swap_image(g_d, sc, 1 - ci).id != cur.id &&
                  !GPU_VALID(gpu_swap_image(g_d, sc, n)));
        }
        gpu_swap_destroy(g_d, sc);
        CHECK(!gpu_d3d12_slot_valid(&g_dd->tex_slots, img2.id));
    }
    DestroyWindow(w);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
}

/* ---- The conversion's bridges ---- */

static ID3D12Resource* native_committed(const D3D12_RESOURCE_DESC* rd, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES st)
{
    ID3D12Device* dev = (ID3D12Device*)gpu_native(g_d, GPU_NATIVE_DEVICE, 0);
    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof hp);
    hp.Type = heap;
    ID3D12Resource* r = NULL;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, rd, st, NULL,
                                                    &IID_ID3D12Resource, (void**)&r)))
        return NULL;
    return r;
}

static void test_bridges(const scene* s)
{
    ID3D12Device* dev = (ID3D12Device*)gpu_native(g_d, GPU_NATIVE_DEVICE, 0);

    /* A texture the old code made. */
    {
        D3D12_RESOURCE_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 16; rd.Height = 8; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
        ID3D12Resource* r = native_committed(&rd, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        CHECK(r != NULL);
        const gpu_texture_desc td = tex_desc(16, 8, GPU_FORMAT_R8G8B8A8_UNORM,
                                             GPU_USE_SAMPLED | GPU_USE_COPY_SRC | GPU_USE_COPY_DST,
                                             GPU_STATE_COPY_DEST);
        const gpu_texture t = gpu_d3d12_adopt_texture(g_d, r, &td);
        CHECK(GPU_VALID(t) && gpu_native(g_d, GPU_NATIVE_TEXTURE, t.id) == r);
        const ULONG refs = r->lpVtbl->AddRef(r);
        r->lpVtbl->Release(r);
        CHECK(refs == 3);   /* the old code's, the handle's, this one */
        r->lpVtbl->Release(r);   /* the old code lets go first */
        if (GPU_VALID(t)) tex_roundtrip_on("adopted texture", t, &td, 0);
        gpu_texture_destroy(g_d, t, 0);
    }
    /* An upload buffer the old code made and maps. */
    {
        D3D12_RESOURCE_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* r = native_committed(&bd, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        gpu_buffer_desc gd;
        memset(&gd, 0, sizeof gd);
        gd.size = 256;
        gd.memory = GPU_MEM_UPLOAD;
        gd.state = GPU_STATE_GENERIC_READ;
        const gpu_buffer b = gpu_d3d12_adopt_buffer(g_d, r, &gd);
        uint8_t* p = buf_ptr(b);
        CHECK(GPU_VALID(b) && p != NULL);
        if (r) r->lpVtbl->Release(r);
        if (p) {
            for (int i = 0; i < 256; i++) p[i] = (uint8_t)(255 - i);
            const gpu_buffer rb = buf_new(g_d, 256, GPU_MEM_READBACK, GPU_STATE_COPY_DEST);
            gpu_cmd* c = gpu_cmd_begin(g_d, 0);
            gpu_cmd_copy_buffer(c, rb, 0, b, 0, 256);
            CHECK(run(g_d, c));
            const uint8_t* q = (const uint8_t*)gpu_buffer_map(g_d, rb, 0, 256);
            CHECK(q && !memcmp(p, q, 256));
            gpu_buffer_unmap(g_d, rb, 0, 0);
            gpu_buffer_destroy(g_d, rb, 0);
        }
        gpu_buffer_destroy(g_d, b, 0);
    }
    /* A pipeline and a root signature the old code made, from the same
     * description: the draw is the same. */
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
        D3D12_INPUT_ELEMENT_DESC il[GPU_MAX_VERTEX_ATTRS];
        gpu_d3d12_pipeline_d3d_desc(g_d, &s->desc, &pd, il);
        ID3D12PipelineState* pso = NULL;
        CHECK(SUCCEEDED(dev->lpVtbl->CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState,
                                                                 (void**)&pso)));
        const gpu_pipeline p = gpu_d3d12_adopt_pipeline(g_d, pso);
        if (pso) pso->lpVtbl->Release(pso);
        ID3D12RootSignature* rs = (ID3D12RootSignature*)gpu_native(g_d, GPU_NATIVE_LAYOUT, s->layout.id);
        const gpu_layout l = gpu_d3d12_adopt_layout(g_d, rs, gpu_d3d12_layout_hash(g_d, s->layout));
        CHECK(p.v != 0 && GPU_VALID(l) && gpu_d3d12_layout_hash(g_d, l) == gpu_d3d12_layout_hash(g_d, s->layout));
        if (p.v && GPU_VALID(l))
            CHECK(scene_draw_check(s, p, l, gpu_fast_table_ref(g_d, s->tex_pool, 16), s->tex_pool));
    }
    /* A descriptor heap the old code made and writes, and a table from one
     * of its GPU handles. */
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        memset(&hd, 0, sizeof hd);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 32;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12DescriptorHeap* heap = NULL;
        CHECK(SUCCEEDED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&heap)));
        gpu_table_pool_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.kind = GPU_TABLE_TEX;
        pd.count = 32;
        const gpu_table_pool ap = gpu_d3d12_adopt_table_pool(g_d, heap, &pd);
        CHECK(GPU_VALID(ap));
        const LONG n0 = g_dd->n_srv_writes;
        gpu_fast_table_write_tex(g_d, ap, 2, NULL);   CHECK(g_dd->n_srv_writes == n0 + 1);   /* unknown: written */
        gpu_fast_table_write_tex(g_d, ap, 2, NULL);   CHECK(g_dd->n_srv_writes == n0 + 1);
        gpu_view_desc vd;
        memset(&vd, 0, sizeof vd);
        vd.texture = s->texel;
        gpu_fast_table_write_tex(g_d, ap, 0, &vd);
        for (uint32_t u = 1; u < 16; u++) gpu_fast_table_write_tex(g_d, ap, u, NULL);
        D3D12_GPU_DESCRIPTOR_HANDLE g;
        heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &g);
        const gpu_table wrapped = gpu_d3d12_wrap_table(g);
        CHECK(wrapped.v == gpu_table_ref(g_d, ap, 0).v);
        g.ptr += 2u * g_dd->srv_inc;
        CHECK(gpu_d3d12_wrap_table(g).v == gpu_table_ref(g_d, ap, 2).v);
        heap->lpVtbl->Release(heap);   /* the pool keeps its own reference */
        CHECK(scene_draw_check(s, s->pso, s->layout, wrapped, ap));
    }
    /* Recording into the old code's open list. */
    {
        ID3D12CommandAllocator* alloc = NULL;
        ID3D12GraphicsCommandList* cl = NULL;
        CHECK(SUCCEEDED(dev->lpVtbl->CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            &IID_ID3D12CommandAllocator, (void**)&alloc)));
        CHECK(SUCCEEDED(dev->lpVtbl->CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                                       &IID_ID3D12GraphicsCommandList, (void**)&cl)));
        gpu_cmd* c = gpu_d3d12_cmd_wrap(g_d, cl);
        CHECK(c != NULL && gpu_native(g_d, GPU_NATIVE_CMD, (uint64_t)(uintptr_t)c) == cl);
        if (c) {
            const float green[4] = {0, 1, 0, 1};
            gpu_cmd_clear_color(c, s->rtv, green, NULL);
            gpu_cmd_barrier(c, s->rt, GPU_STATE_RENDER_TARGET, GPU_STATE_COPY_SOURCE);
            cl->lpVtbl->Close(cl);
            ID3D12CommandQueue* q = (ID3D12CommandQueue*)gpu_native(g_d, GPU_NATIVE_QUEUE, 0);
            ID3D12CommandList* lists[1] = {(ID3D12CommandList*)cl};
            q->lpVtbl->ExecuteCommandLists(q, 1, lists);
            CHECK(gpu_fence_wait(g_d, gpu_signal(g_d), 20000) == 0);
            readback rb;
            const gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0);
            CHECK(tex_read(s->rt, &td, &rb));
            if (rb.p) CHECK(px(&rb, 3, 3)[1] == 255 && px(&rb, 3, 3)[0] == 0);
            rb_done(&rb);
            gpu_d3d12_cmd_unwrap(g_d, c);
            gpu_cmd* back = gpu_cmd_begin(g_d, 0);
            gpu_cmd_barrier(back, s->rt, GPU_STATE_COPY_SOURCE, GPU_STATE_RENDER_TARGET);
            run(g_d, back);
        }
        if (cl) cl->lpVtbl->Release(cl);
        if (alloc) alloc->lpVtbl->Release(alloc);
    }
}

/* ---- step C4 ---- */
/* gpu_d3d12_adopt_texture_of (the description from the resource) and
 * gpu_d3d12_table_pool_heap (a pool's heap, borrowed). */
static void test_bridges_c4(const scene* s)
{
    {
        D3D12_RESOURCE_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 16; rd.Height = 8; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ID3D12Resource* r = native_committed(&rd, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        CHECK(r != NULL);
        const gpu_texture t = gpu_d3d12_adopt_texture_of(g_d, r);
        CHECK(GPU_VALID(t) && gpu_native(g_d, GPU_NATIVE_TEXTURE, t.id) == r);
        CHECK(!GPU_VALID(gpu_d3d12_adopt_texture_of(g_d, NULL)));
        if (GPU_VALID(t)) {
            const gpu_d3d12_tex* x = &g_dd->tex[GPU_HANDLE_INDEX(t.id)];
            CHECK(x->width == 16 && x->height == 8 && x->layers == 1 && x->mips == 1 && x->samples == 1 &&
                  x->format == GPU_FORMAT_R16G16B16A16_FLOAT &&
                  x->use == (GPU_USE_SAMPLED | GPU_USE_RT | GPU_USE_COPY_SRC | GPU_USE_COPY_DST));
            const gpu_texture_desc td = tex_desc(16, 8, GPU_FORMAT_R16G16B16A16_FLOAT,
                                                 GPU_USE_SAMPLED | GPU_USE_RT | GPU_USE_COPY_SRC |
                                                 GPU_USE_COPY_DST, GPU_STATE_COPY_DEST);
            if (r) r->lpVtbl->Release(r);
            tex_roundtrip_on("adopted from its description", t, &td, 0);
            gpu_texture_destroy(g_d, t, 0);
        } else if (r) {
            r->lpVtbl->Release(r);
        }
    }
    {
        ID3D12DescriptorHeap* h = gpu_d3d12_table_pool_heap(g_d, s->tex_pool);
        CHECK(h != NULL);
        if (h) {
            D3D12_GPU_DESCRIPTOR_HANDLE g;
            h->lpVtbl->GetGPUDescriptorHandleForHeapStart(h, &g);
            CHECK(gpu_d3d12_wrap_table(g).v == gpu_table_ref(g_d, s->tex_pool, 0).v);
        }
        CHECK(gpu_d3d12_table_pool_heap(g_d, s->samp_pool) != NULL &&
              gpu_d3d12_table_pool_heap(g_d, s->samp_pool) != h);
        const gpu_table_pool none = {0};
        CHECK(gpu_d3d12_table_pool_heap(g_d, none) == NULL);
    }
}

/* GPU_DEVICE_BREADCRUMBS (DRED, not the debug layer): its feature bits, a
 * marker, and, on WARP only, a device removed on purpose (as today's
 * RSX_LOSE_DEVICE_AT_FRAME removes it): lost, its fence passing every value,
 * the DRED report (on stderr), and a destroy that does not wait for good.
 * Last: DRED's settings stay on for the process's later devices. */
static void test_breadcrumbs(void)
{
    gpu_device_desc dd;
    memset(&dd, 0, sizeof dd);
    dd.flags = g_flags | GPU_DEVICE_BREADCRUMBS;
    dd.cmd_slots = 1;
    gpu_device* d = gpu_d3d12_create(&dd);
    CHECK(d != NULL);
    if (!d) return;
    CHECK((d->caps.bits & GPU_CAP_DEVICE_FAULT) != 0);
    gpu_cmd* c = gpu_cmd_begin(d, 0);
    gpu_cmd_marker(c, "a marker", 8);
    CHECK(run(d, c));
    gpu_fault_report(d, "a working device");   /* says nothing */
    CHECK(gpu_lost(d) == 0);
    if (g_flags & GPU_DEVICE_WARP) {
        ID3D12Device* dev = (ID3D12Device*)gpu_native(d, GPU_NATIVE_DEVICE, 0);
        ID3D12Device5* d5 = NULL;
        if (SUCCEEDED(dev->lpVtbl->QueryInterface(dev, &IID_ID3D12Device5, (void**)&d5)) && d5) {
            d5->lpVtbl->RemoveDevice(d5);
            d5->lpVtbl->Release(d5);
            CHECK(gpu_lost(d) != 0);
            CHECK(gpu_fence_completed(d) == UINT64_MAX && gpu_fence_wait(d, gpu_signal(d), 2000) == 0);
            gpu_fault_report(d, "the test");
        }
    }
    gpu_destroy(d);
}

int main(void)
{
    _putenv("PS3_SHADER_CACHE=0");   /* the compile service writes no cache */
    const char* hw = getenv("GPU_TEST_HW");
    const int on_hw = hw && hw[0] == '1';
    g_flags = on_hw ? 0u : GPU_DEVICE_WARP;
    gpu_device_desc dd;
    memset(&dd, 0, sizeof dd);
    dd.flags = g_flags;
    dd.cmd_slots = 2;
    g_d = gpu_d3d12_create(&dd);
    if (!g_d) {
        printf("GPU D3D12: no device (%s)\n", on_hw ? "hardware" : "WARP");
        return 1;
    }
    g_dd = GPU_D3D12_DEV(g_d);
    gpu_adapter_info ai;
    gpu_adapter(g_d, &ai);
    printf("GPU D3D12 test on %s (%s, driver %s), caps 0x%08X\n", ai.name, on_hw ? "hardware" : "WARP",
           ai.driver, g_d->caps.bits);

    test_buffers();
    test_host_import();
    test_textures();
    test_clears();
    test_layouts();
    scene s;
    if (scene_make(&s)) {
        test_pipeline_descs(s.layout, s.vs, s.ps);
        test_draw(&s);
        test_memo(&s);
        test_bridges(&s);
        test_bridges_c4(&s);
    }
    test_caches();
    test_fences();
    test_swapchain();
    CHECK(gpu_lost(g_d) == 0);
    gpu_destroy(g_d);
    test_pipeline_cache();
    test_breadcrumbs();

    if (g_fail) {
        printf("GPU D3D12: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    printf("GPU D3D12: OK (%d checks, %s)\n", g_checks, on_hw ? "hardware" : "WARP");
    return 0;
}

#else
#include <stdio.h>
int main(void)
{
    printf("test_gpu_d3d12: built without PS3RECOMP_RSX_HAL on Windows\n");
    return 1;
}
#endif
