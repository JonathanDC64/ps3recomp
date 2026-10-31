/*
 * ps3recomp - the GPU layer's Vulkan backend test (CMake target test_gpu_vk,
 * built with PS3RECOMP_RSX_HAL=ON and PS3RECOMP_RSX_HAL_VULKAN; ctest -R
 * gpu_vk)
 *
 * test_gpu_d3d12's cases on Vulkan, with the validation layer on: any message
 * it gives fails the test (GPU_TEST_VALIDATION=0 runs without it). It runs on
 * a CPU device (lavapipe, in CI) when there is one, else on the GPU;
 * GPU_TEST_HW=1 picks the GPU (GPU_TEST_LOW_POWER=1 too: the integrated
 * one). RSX_VK_MASK (GPU_CAP_* bits) turns features
 * off to run their fallbacks, and each check follows the caps the device
 * ends with. The shaders are the test's own: test_gpu_vk_shaders.hlsl, made
 * into test_gpu_vk_spv.h.
 *   - buffers: upload, GPU and readback memory round trip; host import;
 *   - textures (RGBA8, BC1, RGBA16F, one placed in a heap) through
 *     copy_buf_to_tex and copy_tex_to_buf; texture to texture copies; the
 *     footprints against D3D12's (WARP, on Windows);
 *   - clears of a colour and a depth target, today's zeta surface;
 *   - a draw on the guest pipelines' six-slot layout: its pixels, which way
 *     up it is, an empty entry, a swizzled one, depth clamp, a geometry
 *     shader, a read-only depth target, occlusion counts; a post pass with a
 *     static sampler;
 *   - the table memo, sampler and view caching, fences and a wait's timeout;
 *     t20 switched between buffers from draw to draw (the set-0 memo);
 *   - the pipeline cache across two devices, a swap chain on a hidden window
 *     (Windows), breadcrumbs and the fault report.
 */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE   /* clock_gettime, setenv, MAP_ANONYMOUS under -std=c99 */
#endif
#if defined(PS3RECOMP_RSX_HAL) && defined(PS3RECOMP_RSX_HAL_VULKAN)

#include "gpu_vk_internal.h"
#include "gpu_vk_cmd.h"
#include "test_gpu_vk_spv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

/* Guest memory, which host_stubs.c (the runtime's host side) names; no guest
 * runs here. */
uint8_t* vm_base = NULL;

static int g_fail, g_checks;

#define CHECK(cond)                                                              \
    do { g_checks++;                                                             \
         if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
                        g_fail++; } } while (0)

static gpu_device* g_d;
static gpu_vk_device* g_vk;
static uint32_t g_flags;   /* the device's GPU_DEVICE_* */

static int has(uint32_t cap) { return (g_d->caps.bits & cap) != 0; }

/* ---- Helpers ---- */

static int64_t ms_now(void)
{
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
#endif
}

static void env_set(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static int env_is(const char* name, const char* value)
{
    const char* v = getenv(name);
    return v && !strcmp(v, value);
}

static void temp_path(char* out, size_t cap, const char* name)
{
#ifdef _WIN32
    char dir[MAX_PATH];
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (!n || n >= MAX_PATH) dir[0] = 0;
    snprintf(out, cap, "%s%s_%lu", dir, name, (unsigned long)GetCurrentProcessId());
#else
    const char* t = getenv("TMPDIR");
    snprintf(out, cap, "%s/%s_%ld", t && t[0] ? t : "/tmp", name, (long)getpid());
#endif
}

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

/* Ends, submits and waits for a recording. 1 when the GPU finished. */
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
    gpu_buffer     buf;
    gpu_footprint  fp;
    uint32_t       rows;
    uint64_t       row_bytes, total;
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
    if (GPU_VALID(rb->buf)) gpu_buffer_destroy(g_d, rb->buf, 0);
    memset(rb, 0, sizeof *rb);
}

static const uint8_t* px(const readback* rb, uint32_t x, uint32_t y)
{
    return rb->p + (uint64_t)y * rb->fp.row_pitch + (uint64_t)x * 4;
}

static int close_to(int a, int b) { return abs(a - b) <= 2; }

static int px_is(const readback* rb, uint32_t x, uint32_t y, int r, int g, int b, int a, const char* what)
{
    if (!rb->p) return 0;
    const uint8_t* p = px(rb, x, y);
    const int ok = close_to(p[0], r) && close_to(p[1], g) && close_to(p[2], b) && close_to(p[3], a);
    if (!ok) printf("  %s: pixel (%u, %u) is %u %u %u %u, not %d %d %d %d\n", what, x, y, p[0], p[1], p[2], p[3], r, g,
                    b, a);
    return ok;
}

static gpu_shader sh_make(gpu_device* d, uint32_t stage, const uint32_t* code, size_t size)
{
    gpu_shader_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.stage = stage;
    sd.code = GPU_CODE_SPIRV;
    sd.bytes = code;
    sd.size = size;
    return gpu_shader_create(d, &sd);
}
#define SH(d, stage, arr) sh_make((d), (stage), (arr), sizeof(arr))

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

static void* host_alloc(size_t n)
{
#ifdef _WIN32
    return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static void host_free(void* p, size_t n)
{
#ifdef _WIN32
    (void)n;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, n);
#endif
}

static void test_host_import(void)
{
    const size_t n = 1u << 20;
    uint8_t* host = (uint8_t*)host_alloc(n);
    CHECK(host != NULL);
    if (!host) return;
    if (!has(GPU_CAP_HOST_IMPORT)) {   /* the fallback: refused, so the core copies */
        CHECK(!GPU_VALID(gpu_buffer_import_host(g_d, host, n)));
        printf("  host import: none (refused, as it should be)\n");
        host_free(host, n);
        return;
    }
    const uint64_t al = g_d->caps.host_import_align;
    CHECK(al && !(al & (al - 1)));
    if (al && (((uintptr_t)host & (al - 1)) || (n & (al - 1)))) {
        printf("  host import: skipped (alignment %llu)\n", (unsigned long long)al);
        host_free(host, n);
        return;
    }
    CHECK(!GPU_VALID(gpu_buffer_import_host(g_d, host + 8, n - al)));   /* off its alignment */
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
    host_free(host, n);
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
    if (has(GPU_CAP_BC)) {
        tex_roundtrip("BC1", GPU_FORMAT_BC1_UNORM, 64, 32);
    } else {   /* the fallback: refused, so the core decodes them */
        const gpu_texture_desc td = tex_desc(64, 32, GPU_FORMAT_BC1_UNORM, GPU_USE_SAMPLED | GPU_USE_COPY_DST,
                                             GPU_STATE_COPY_DEST);
        CHECK(!GPU_VALID(gpu_texture_create(g_d, &td)));
        CHECK(!(gpu_format_support(g_d, GPU_FORMAT_BC1_UNORM, 1) & GPU_FMT_SAMPLED));
    }
    tex_roundtrip("RGBA16F", GPU_FORMAT_R16G16B16A16_FLOAT, 32, 16);
    tex_roundtrip("B4G4R4A4", GPU_FORMAT_B4G4R4A4_UNORM, 16, 8);

    /* Placed, as today's texture pool places them: a 4 MB heap for sampled
     * textures, the size and alignment texture_size gives. */
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

#ifdef _WIN32
/* The footprints are D3D12's (GetCopyableFootprints), so the core's upload
 * code is the same on both: each against a WARP device's. */
static void footprints_same(gpu_device* w, const char* name, gpu_texture_desc td, uint32_t nsub, uint64_t base)
{
    gpu_footprint a[16], b[16];
    uint32_t ra[16], rb[16];
    uint64_t ba[16], bb[16];
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    const uint64_t ta = gpu_texture_footprints(g_d, &td, 0, nsub, base, a, ra, ba);
    const uint64_t tb = gpu_texture_footprints(w, &td, 0, nsub, base, b, rb, bb);
    int same = ta == tb;
    for (uint32_t i = 0; same && i < nsub; i++)
        same = a[i].offset == b[i].offset && a[i].format == b[i].format && a[i].width == b[i].width &&
               a[i].height == b[i].height && a[i].depth == b[i].depth && a[i].row_pitch == b[i].row_pitch &&
               ra[i] == rb[i] && ba[i] == bb[i];
    if (!same) {
        printf("  footprints of %s differ: total %llu (D3D12 %llu)\n", name, (unsigned long long)ta,
               (unsigned long long)tb);
        for (uint32_t i = 0; i < nsub; i++)
            printf("    %u: off %llu/%llu fmt %u/%u %ux%ux%u/%ux%ux%u pitch %u/%u rows %u/%u bytes %llu/%llu\n", i,
                   (unsigned long long)a[i].offset, (unsigned long long)b[i].offset, a[i].format, b[i].format,
                   a[i].width, a[i].height, a[i].depth, b[i].width, b[i].height, b[i].depth, a[i].row_pitch,
                   b[i].row_pitch, ra[i], rb[i], (unsigned long long)ba[i], (unsigned long long)bb[i]);
    }
    CHECK(same);
}

static void footprints_on(gpu_device* w)
{
    const uint32_t s = GPU_USE_SAMPLED | GPU_USE_COPY_DST;
    gpu_texture_desc td = tex_desc(64, 64, GPU_FORMAT_R8G8B8A8_UNORM, s, GPU_STATE_COPY_DEST);
    td.mips = 7;
    footprints_same(w, "RGBA8 64x64, 7 mips", td, 7, 0);
    footprints_same(w, "RGBA8 64x64, 7 mips, from 1024", td, 7, 1024);
    td = tex_desc(64, 32, GPU_FORMAT_BC1_UNORM, s, GPU_STATE_COPY_DEST);
    td.mips = 6;
    footprints_same(w, "BC1 64x32, 6 mips", td, 6, 0);
    td = tex_desc(20, 12, GPU_FORMAT_BC3_UNORM, s, GPU_STATE_COPY_DEST);
    footprints_same(w, "BC3 20x12", td, 1, 0);
    td = tex_desc(32, 32, GPU_FORMAT_R16G16B16A16_FLOAT, s, GPU_STATE_COPY_DEST);
    td.layers = 6;
    footprints_same(w, "RGBA16F cube 32", td, 6, 0);
    td = tex_desc(3, 5, GPU_FORMAT_R8_UNORM, s, GPU_STATE_COPY_DEST);
    td.mips = 3;
    footprints_same(w, "R8 3x5, 3 mips", td, 3, 0);
    td = tex_desc(30, 7, GPU_FORMAT_B4G4R4A4_UNORM, s, GPU_STATE_COPY_DEST);
    footprints_same(w, "B4G4R4A4 30x7", td, 1, 0);
    td = tex_desc(16, 16, GPU_FORMAT_R32_TYPELESS, GPU_USE_DS | GPU_USE_COPY_SRC, GPU_STATE_DEPTH_WRITE);
    footprints_same(w, "R32 depth", td, 1, 0);
    td = tex_desc(40, 24, GPU_FORMAT_R24G8_TYPELESS, GPU_USE_DS | GPU_USE_SAMPLED, GPU_STATE_DEPTH_WRITE);
    footprints_same(w, "D24S8 depth and stencil planes", td, 2, 0);
    td.width = 16;
    td.height = 16;
    footprints_same(w, "D24S8 16x16", td, 2, 0);
    td.width = 300;
    td.height = 7;
    td.mips = 2;
    footprints_same(w, "D24S8 300x7, 2 mips", td, 4, 0);
    td.width = 128;
    td.height = 64;
    footprints_same(w, "D24S8 128x64, 2 mips", td, 4, 512);
    td = tex_desc(40, 24, GPU_FORMAT_R32G8X24_TYPELESS, GPU_USE_DS, GPU_STATE_DEPTH_WRITE);
    footprints_same(w, "D32S8 depth and stencil planes", td, 2, 0);
}

/* Against WARP's, and the GPU's D3D12 device's with GPU_TEST_HW=1 (the
 * runtime's rules, which should be the same). */
static void test_footprints(void)
{
    for (int hw = 0; hw < 2; hw++) {
        if (hw && !env_is("GPU_TEST_HW", "1")) break;
        gpu_device_desc dd;
        memset(&dd, 0, sizeof dd);
        dd.flags = hw ? 0u : GPU_DEVICE_WARP;
        dd.cmd_slots = 1;
        gpu_device* w = gpu_d3d12_create(&dd);
        if (!w) {
            printf("  footprints: no D3D12 %s device\n", hw ? "GPU" : "WARP");
            continue;
        }
        footprints_on(w);
        gpu_destroy(w);
    }
}
#endif

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
    CHECK(px_is(&rb, 0, 0, 255, 0, 0, 255, "clear") && px_is(&rb, 31, 31, 0, 0, 255, 255, "clear"));
    CHECK(px_is(&rb, 15, 31, 255, 0, 0, 255, "clear") && px_is(&rb, 16, 0, 0, 0, 255, 255, "clear"));
    rb_done(&rb);

    /* A view that is not bound, cleared outside any pass (its own rendering). */
    c = gpu_cmd_begin(g_d, 0);
    gpu_cmd_barrier(c, t, GPU_STATE_COPY_SOURCE, GPU_STATE_RENDER_TARGET);
    gpu_cmd_clear_color(c, rtv, red, NULL);
    gpu_cmd_barrier(c, t, GPU_STATE_RENDER_TARGET, GPU_STATE_COPY_SOURCE);
    CHECK(run(g_d, c));
    CHECK(tex_read(t, &td, &rb));
    CHECK(px_is(&rb, 31, 31, 255, 0, 0, 255, "unbound clear"));
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
     * read-only one (cleared, not read back: its two planes). Without D24S8
     * targets it is D32S8, as gpu.h's cap says. */
    gpu_texture_desc zd = tex_desc(16, 16, GPU_FORMAT_R24G8_TYPELESS, GPU_USE_DS | GPU_USE_SAMPLED,
                                   GPU_STATE_DEPTH_WRITE);
    zd.clear_format = GPU_FORMAT_D24_UNORM_S8_UINT;
    zd.clear_depth = 1.0f;
    const gpu_texture zt = gpu_texture_create(g_d, &zd);
    CHECK(GPU_VALID(zt));
    if (!GPU_VALID(zt)) return;
    CHECK(g_vk->tex[GPU_HANDLE_INDEX(zt.id)].vkfmt ==
          (has(GPU_CAP_D24S8) ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT));
    memset(&vd, 0, sizeof vd);
    vd.texture = zt;
    vd.kind = GPU_VIEW_DSV;
    vd.format = GPU_FORMAT_D24_UNORM_S8_UINT;
    const gpu_view zv = gpu_view_get(g_d, &vd);
    vd.flags = GPU_VIEW_READ_ONLY_DEPTH | GPU_VIEW_READ_ONLY_STENCIL;
    const gpu_view zr = gpu_view_get(g_d, &vd);
    CHECK(GPU_VALID(zv) && GPU_VALID(zr) && zv.id != zr.id);
    c = gpu_cmd_begin(g_d, 0);
    memset(&pass, 0, sizeof pass);
    pass.ds = zv;
    gpu_cmd_pass_begin(c, &pass);
    gpu_cmd_clear_depth(c, zv, GPU_CLEAR_DEPTH | GPU_CLEAR_STENCIL, 1.0f, 0, NULL);
    /* Its depth plane (subresource 0) to be read in a shader and back: on
     * Vulkan one plane's barrier moves both. */
    gpu_barrier pb;
    memset(&pb, 0, sizeof pb);
    pb.texture = zt;
    pb.sub = 0;
    pb.before = GPU_STATE_DEPTH_WRITE;
    pb.after = GPU_STATE_PIXEL_SHADER_RESOURCE;
    gpu_cmd_barrier_n(c, 1, &pb);
    pb.before = GPU_STATE_PIXEL_SHADER_RESOURCE;
    pb.after = GPU_STATE_DEPTH_WRITE;
    gpu_cmd_barrier_n(c, 1, &pb);
    CHECK(run(g_d, c));
    gpu_texture_destroy(g_d, zt, 0);
}

/* ---- Layouts ---- */

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

/* Today's blit root signature: one texture, a static linear clamp. */
static gpu_layout blit_layout(gpu_device* d)
{
    gpu_layout_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.nslots = 1;
    ld.slot[0] = slot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
    ld.nstatic = 1;
    ld.statics[0] = clamp_sampler(GPU_FILTER(1, 1, 1, 0), 0);
    return gpu_layout_create(d, &ld);
}

static void test_layouts(void)
{
    /* Where each slot lands (gpu_vk_internal.h's rules, which the SPIR-V
     * target follows). */
    const gpu_layout guest = guest_layout(g_d);
    CHECK(GPU_VALID(guest) && gpu_native(g_d, GPU_NATIVE_LAYOUT, guest.id) != NULL);
    if (GPU_VALID(guest)) {
        const gpu_vk_layout* l = &g_vk->layout[GPU_HANDLE_INDEX(guest.id)];
        CHECK(l->nsets == 3 && l->first == 0 && l->ndyn == 3 && l->push_size == 4);
        CHECK(l->slot[1].set == 1 && l->slot[3].set == 2);
        CHECK(l->dyn_binding[l->slot[0].dyn] == 0 && l->dyn_binding[l->slot[2].dyn] == 1 &&
              l->dyn_binding[l->slot[4].dyn] == 36 && l->dyn_raw[l->slot[4].dyn] == 1);
        CHECK(l->slot[5].push_off == 0 && l->slot[5].push_size == 4);
    }

    const gpu_layout blit = blit_layout(g_d);
    CHECK(GPU_VALID(blit));
    if (GPU_VALID(blit)) {
        const gpu_vk_layout* l = &g_vk->layout[GPU_HANDLE_INDEX(blit.id)];
        CHECK(l->nsets == 2 && l->ndyn == 0 && l->set0_fixed != VK_NULL_HANDLE && l->nstatics == 1);
    }

    gpu_layout_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.nslots = 2;
    ld.slot[0] = slot(GPU_SLOT_TABLE_TEX, 0, 3, GPU_VIS_PIXEL);
    ld.slot[1] = slot(GPU_SLOT_CONST32, 0, 4, GPU_VIS_ALL);
    ld.nstatic = 2;
    ld.statics[0] = clamp_sampler(GPU_FILTER(1, 1, 0, 0), 0);
    ld.statics[1] = clamp_sampler(GPU_FILTER(0, 0, 0, 0), 1);
    const gpu_layout smaa = gpu_layout_create(g_d, &ld);
    CHECK(GPU_VALID(smaa));
    if (GPU_VALID(smaa)) CHECK(g_vk->layout[GPU_HANDLE_INDEX(smaa.id)].push_size == 16);

    /* Two CONST32 slots (today's HDR blit): one block, in slot order. */
    memset(&ld, 0, sizeof ld);
    ld.nslots = 3;
    ld.slot[0] = slot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
    ld.slot[1] = slot(GPU_SLOT_CONST32, 0, 4, GPU_VIS_PIXEL);
    ld.slot[2] = slot(GPU_SLOT_CONST32, 1, 2, GPU_VIS_PIXEL);
    const gpu_layout hdr = gpu_layout_create(g_d, &ld);
    CHECK(GPU_VALID(hdr));
    if (GPU_VALID(hdr)) {
        const gpu_vk_layout* l = &g_vk->layout[GPU_HANDLE_INDEX(hdr.id)];
        CHECK(l->push_size == 24 && l->slot[2].push_off == 16 && l->first == 1);
    }

    /* A table from another register than 0 (step VM: the menu's t1): its
     * entry 0 is that register, so it binds a pool's sets as one from 0. */
    memset(&ld, 0, sizeof ld);
    ld.nslots = 2;
    ld.slot[0] = slot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
    ld.slot[1] = slot(GPU_SLOT_TABLE_TEX, 4, 2, GPU_VIS_PIXEL);
    const gpu_layout t4 = gpu_layout_create(g_d, &ld);
    CHECK(GPU_VALID(t4));
    if (GPU_VALID(t4)) {
        const gpu_vk_layout* l = &g_vk->layout[GPU_HANDLE_INDEX(t4.id)];
        CHECK(l->slot[0].set != l->slot[1].set && l->slot[1].set < l->nsets);
    }
}

/* ---- A draw on the guest layout ---- */

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
    gpu_layout        layout;
    gpu_shader        vs, ps, vs_gs, gs;
    gpu_pipeline      pso, pso_clip, pso_gs, pso_depth, pso_zeta;
    gpu_pipeline_desc desc;
    gpu_texture       rt, rt2, texel;
    gpu_view          rtv, rtv2;
    gpu_table_pool    tex_pool, samp_pool;
    gpu_sampler       samp;
    gpu_buffer        vb, ib, cb, raw;
    uint32_t          cb_step;   /* cb_align: b0 at 0, b1 at one step, b0 with z 1.5 at two */
    gpu_query_pool    queries;
} scene;

static int scene_make(scene* s)
{
    memset(s, 0, sizeof *s);
    s->layout = guest_layout(g_d);
    s->vs = SH(g_d, GPU_STAGE_VS, k_spv_guest_vs);
    s->ps = SH(g_d, GPU_STAGE_PS, k_spv_guest_ps);
    s->vs_gs = SH(g_d, GPU_STAGE_VS, k_spv_guest_vs_gs);
    s->gs = SH(g_d, GPU_STAGE_GS, k_spv_guest_gs);
    CHECK(GPU_VALID(s->layout) && GPU_VALID(s->vs) && GPU_VALID(s->ps) && GPU_VALID(s->vs_gs));
    CHECK(GPU_VALID(s->gs) == has(GPU_CAP_GS));   /* without geometry shaders, refused */
    if (!GPU_VALID(s->layout) || !GPU_VALID(s->vs) || !GPU_VALID(s->ps)) return 0;
    draw_desc(&s->desc, s->layout, s->vs, s->ps);
    uint32_t cached = 7;
    s->pso = gpu_pipeline_create(g_d, &s->desc, 0, &cached);
    /* the device's cache, empty before it; with the cache masked (RSX_VK_MASK), compiled only */
    CHECK(s->pso.v != 0 && cached == (has(GPU_CAP_PIPELINE_CACHE) ? GPU_PIPELINE_STORED : GPU_PIPELINE_COMPILED));
    gpu_pipeline_desc pd = s->desc;
    pd.depth_clip = 1;
    s->pso_clip = gpu_pipeline_create(g_d, &pd, 0, NULL);
    if (GPU_VALID(s->gs)) {
        pd = s->desc;
        pd.vs = s->vs_gs;
        pd.gs = s->gs;
        s->pso_gs = gpu_pipeline_create(g_d, &pd, 0, NULL);
        CHECK(s->pso_gs.v != 0);
    }
    pd = s->desc;
    pd.depth_test = 1;
    pd.depth_write = 1;
    pd.ds_format = GPU_FORMAT_D32_FLOAT;
    s->pso_depth = gpu_pipeline_create(g_d, &pd, 0, NULL);
    /* Today's zeta pipelines: D24S8 with stencil, which the D32S8 fallback must take too. */
    pd.ds_format = GPU_FORMAT_D24_UNORM_S8_UINT;
    pd.depth_func = GPU_CMP_LESS_EQUAL;
    pd.stencil_test = 1;
    pd.stencil_read_mask = pd.stencil_write_mask = 0xFF;
    pd.stencil_front.fail = pd.stencil_front.depth_fail = GPU_STENCIL_KEEP;
    pd.stencil_front.pass = GPU_STENCIL_REPLACE;
    pd.stencil_front.func = GPU_CMP_ALWAYS;
    pd.stencil_back = pd.stencil_front;
    s->pso_zeta = gpu_pipeline_create(g_d, &pd, 0, NULL);
    CHECK(s->pso_clip.v != 0 && s->pso_depth.v != 0 && s->pso_zeta.v != 0);

    gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, GPU_USE_RT | GPU_USE_COPY_SRC,
                                   GPU_STATE_RENDER_TARGET);
    s->rt = gpu_texture_create(g_d, &td);
    s->rt2 = gpu_texture_create(g_d, &td);
    gpu_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.texture = s->rt;
    vd.kind = GPU_VIEW_RTV;
    s->rtv = gpu_view_get(g_d, &vd);
    vd.texture = s->rt2;
    s->rtv2 = gpu_view_get(g_d, &vd);

    /* One texel, (128, 255, 255, 255), sampled in the middle. */
    td = tex_desc(1, 1, GPU_FORMAT_R8G8B8A8_UNORM, GPU_USE_SAMPLED | GPU_USE_COPY_DST, GPU_STATE_COPY_DEST);
    s->texel = gpu_texture_create(g_d, &td);
    gpu_footprint fp;
    const uint64_t total = gpu_texture_footprints(g_d, &td, 0, 1, 0, &fp, NULL, NULL);
    const gpu_buffer up = buf_new(g_d, total, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    const uint8_t texel[4] = {128, 255, 255, 255};
    memcpy(buf_ptr(up), texel, 4);

    /* Three 16-entry tables from entry 16 (today's ring of tables). */
    gpu_table_pool_desc pd2;
    memset(&pd2, 0, sizeof pd2);
    pd2.kind = GPU_TABLE_TEX;
    pd2.count = 64;
    pd2.nregions = 1;
    pd2.region[0].first = 16;
    pd2.region[0].tables = 3;
    pd2.region[0].size = 16;
    s->tex_pool = gpu_table_pool_create(g_d, &pd2);
    pd2.kind = GPU_TABLE_SAMPLER;
    pd2.count = 32;
    pd2.region[0].tables = 1;
    s->samp_pool = gpu_table_pool_create(g_d, &pd2);
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
    vd.texture = s->texel;   /* every field at its default */
    gpu_fast_table_write_tex(g_d, s->tex_pool, 16, &vd);
    for (uint32_t u = 1; u < 16; u++) gpu_fast_table_write_tex(g_d, s->tex_pool, 16 + u, NULL);
    /* The second table: the texel swizzled (A, 0, 1, B); its t1 a new pool's empty entry. */
    vd.swizzle = GPU_SWIZZLE(GPU_SWZ_A, GPU_SWZ_ZERO, GPU_SWZ_ONE, GPU_SWZ_B);
    gpu_fast_table_write_tex(g_d, s->tex_pool, 32, &vd);
    gpu_fast_table_write_sampler(g_d, s->samp_pool, 16, s->samp);

    /* A triangle over the whole target, its colour (1, 0.5, 0.25, 1); then
     * one over the top left corner only. */
    const float verts[6][8] = {
        {-1, -1, 0, 1, 1, 0.5f, 0.25f, 1}, {-1, 3, 0, 1, 1, 0.5f, 0.25f, 1}, {3, -1, 0, 1, 1, 0.5f, 0.25f, 1},
        {-1, 1, 0, 1, 1, 0.5f, 0.25f, 1},  {0, 1, 0, 1, 1, 0.5f, 0.25f, 1},  {-1, 0, 0, 1, 1, 0.5f, 0.25f, 1}};
    const uint32_t idx[3] = {0, 1, 2};
    const float rawv[2] = {0.0f, 1.0f};   /* b2's k = 1 reads 1.0 */
    s->cb_step = g_d->caps.cb_align;
    CHECK(s->cb_step >= 256 && !(s->cb_step & (s->cb_step - 1)));
    s->vb = buf_new(g_d, sizeof verts, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    s->ib = buf_new(g_d, sizeof idx, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    s->cb = buf_new(g_d, 3ull * s->cb_step, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    s->raw = buf_new(g_d, 256, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    memcpy(buf_ptr(s->vb), verts, sizeof verts);
    memcpy(buf_ptr(s->ib), idx, sizeof idx);
    {
        uint8_t* p = buf_ptr(s->cb);
        const float offs0[4] = {0, 0, 0, 0}, tint[4] = {1, 1, 1, 1}, far_z[4] = {0, 0, 1.5f, 0};
        memset(p, 0, 3ull * s->cb_step);
        memcpy(p, offs0, sizeof offs0);
        memcpy(p + s->cb_step, tint, sizeof tint);
        memcpy(p + 2ull * s->cb_step, far_z, sizeof far_z);
    }
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

/* Records today's per-draw binds for one draw of three vertices from vb_off,
 * b0 at b0_off. */
static void scene_binds(gpu_cmd* c, const scene* s, gpu_pipeline pso, gpu_table tex_table, uint32_t vb_off,
                        uint32_t b0_off)
{
    const gpu_viewport vp = {0, 0, 16, 16, 0, 1};
    const gpu_rect sc = {0, 0, 16, 16};
    const float bf[4] = {1, 1, 1, 1};
    gpu_cmd_set_table_pools(c, s->tex_pool, s->samp_pool);
    gpu_cmd_set_layout(c, s->layout);
    gpu_cmd_set_pipeline(c, pso);
    gpu_cmd_set_topology(c, GPU_TOPO_TRIANGLELIST);
    gpu_cmd_set_viewport(c, &vp);
    gpu_cmd_set_scissor(c, &sc);
    gpu_cmd_set_stencil_ref(c, 0);
    gpu_cmd_set_blend_factor(c, bf);
    gpu_cmd_bind_cb(c, 0, gpu_fast_buffer_addr(g_d, s->cb) + b0_off);
    gpu_cmd_bind_table(c, 1, tex_table);
    gpu_cmd_bind_cb(c, 2, gpu_fast_buffer_addr(g_d, s->cb) + s->cb_step);
    gpu_cmd_bind_table(c, 3, gpu_fast_table_ref(g_d, s->samp_pool, 16));
    gpu_cmd_bind_raw(c, 4, gpu_fast_buffer_addr(g_d, s->raw));
    gpu_cmd_push_const1(c, 5, 1, 0);
    gpu_cmd_set_vb(c, 0, gpu_fast_buffer_addr(g_d, s->vb) + vb_off, 96, 32);
}

/* Draws into the cleared target and reads all of it back. */
static int scene_draw(const scene* s, gpu_pipeline pso, gpu_table tex_table, uint32_t vb_off, uint32_t b0_off,
                      readback* rb)
{
    const float zero[4] = {0, 0, 0, 0};
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = s->rtv;
    gpu_cmd* c = gpu_cmd_begin(g_d, 0);
    gpu_cmd_pass_begin(c, &pass);
    gpu_cmd_clear_color(c, s->rtv, zero, NULL);
    scene_binds(c, s, pso, tex_table, vb_off, b0_off);
    gpu_cmd_draw(c, 3, 1, 0, 0);
    gpu_cmd_barrier(c, s->rt, GPU_STATE_RENDER_TARGET, GPU_STATE_COPY_SOURCE);
    int ok = run(g_d, c);
    const gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0);
    memset(rb, 0, sizeof *rb);
    if (ok) ok = tex_read(s->rt, &td, rb);
    c = gpu_cmd_begin(g_d, 0);
    gpu_cmd_barrier(c, s->rt, GPU_STATE_COPY_SOURCE, GPU_STATE_RENDER_TARGET);
    run(g_d, c);
    return ok;
}

static void test_draw(scene* s)
{
    const gpu_table t16 = gpu_fast_table_ref(g_d, s->tex_pool, 16);
    const gpu_table t32 = gpu_fast_table_ref(g_d, s->tex_pool, 32);
    readback rb;

    /* The draw: (1, 0.5, 0.25, 1) * the texel, plus the empty t1's zero. */
    CHECK(scene_draw(s, s->pso, t16, 0, 0, &rb) && px_is(&rb, 8, 8, 128, 128, 64, 255, "draw"));
    rb_done(&rb);
    /* Which way up: the top left corner's triangle (the target's row 0 is its top, as on D3D12). */
    CHECK(scene_draw(s, s->pso, t16, 96, 0, &rb) && px_is(&rb, 1, 1, 128, 128, 64, 255, "top left") &&
          px_is(&rb, 1, 14, 0, 0, 0, 0, "bottom left"));
    rb_done(&rb);
    /* The swizzled entry: (255, 0, 255, 255) read, so (255, 0, 64, 255). */
    CHECK(scene_draw(s, s->pso, t32, 0, 0, &rb) && px_is(&rb, 8, 8, 255, 0, 64, 255, "swizzle"));
    rb_done(&rb);
    /* Depth clamp: z 1.5 is drawn by the guest pipelines (depth_clip 0) when
     * the device clamps, else clipped (the core's vertex-shader clamp); a
     * depth_clip pipeline clips it either way. */
    CHECK(scene_draw(s, s->pso, t16, 0, 2 * s->cb_step, &rb) &&
          (has(GPU_CAP_DEPTH_CLAMP) ? px_is(&rb, 8, 8, 128, 128, 64, 255, "depth clamp")
                                    : px_is(&rb, 8, 8, 0, 0, 0, 0, "no depth clamp")));
    rb_done(&rb);
    CHECK(scene_draw(s, s->pso_clip, t16, 0, 2 * s->cb_step, &rb) && px_is(&rb, 8, 8, 0, 0, 0, 0, "depth clip"));
    rb_done(&rb);
    /* A geometry shader: the same pixels, and the same way up (it flips y). */
    if (s->pso_gs.v) {
        CHECK(scene_draw(s, s->pso_gs, t16, 0, 0, &rb) && px_is(&rb, 8, 8, 128, 128, 64, 255, "geometry shader"));
        rb_done(&rb);
        CHECK(scene_draw(s, s->pso_gs, t16, 96, 0, &rb) && px_is(&rb, 1, 1, 128, 128, 64, 255, "gs top left") &&
              px_is(&rb, 1, 14, 0, 0, 0, 0, "gs bottom left"));
        rb_done(&rb);
    }

    /* A read-only depth target: depth writes off whatever the pipeline says. */
    {
        gpu_texture_desc zd = tex_desc(16, 16, GPU_FORMAT_R32_TYPELESS, GPU_USE_DS | GPU_USE_COPY_SRC,
                                       GPU_STATE_DEPTH_WRITE);
        const gpu_texture z = gpu_texture_create(g_d, &zd);
        gpu_view_desc vd;
        memset(&vd, 0, sizeof vd);
        vd.texture = z;
        vd.kind = GPU_VIEW_DSV;
        const gpu_view dsv = gpu_view_get(g_d, &vd);
        vd.flags = GPU_VIEW_READ_ONLY_DEPTH;
        const gpu_view ro = gpu_view_get(g_d, &vd);
        const float zero[4] = {0, 0, 0, 0};
        gpu_pass pass;
        memset(&pass, 0, sizeof pass);
        pass.nrt = 1;
        pass.rt[0] = s->rtv;
        pass.ds = ro;
        gpu_cmd* c = gpu_cmd_begin(g_d, 0);
        gpu_cmd_clear_depth(c, dsv, GPU_CLEAR_DEPTH, 0.375f, 0, NULL);
        gpu_cmd_barrier(c, z, GPU_STATE_DEPTH_WRITE, GPU_STATE_DEPTH_READ);
        gpu_cmd_pass_begin(c, &pass);
        gpu_cmd_clear_color(c, s->rtv, zero, NULL);
        scene_binds(c, s, s->pso_depth, t16, 0, 0);
        gpu_cmd_draw(c, 3, 1, 0, 0);
        gpu_cmd_pass_end(c);
        gpu_barrier bs[2];
        memset(bs, 0, sizeof bs);
        bs[0].texture = s->rt;
        bs[0].before = GPU_STATE_RENDER_TARGET;
        bs[1].texture = z;
        bs[1].before = GPU_STATE_DEPTH_READ;
        bs[0].sub = bs[1].sub = GPU_SUB_ALL;
        bs[0].after = bs[1].after = GPU_STATE_COPY_SOURCE;
        gpu_cmd_barrier_n(c, 2, bs);
        CHECK(run(g_d, c));
        const gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0);
        CHECK(tex_read(s->rt, &td, &rb) && px_is(&rb, 8, 8, 128, 128, 64, 255, "read-only depth"));
        rb_done(&rb);
        CHECK(tex_read(z, &zd, &rb));
        if (rb.p) {
            float v;
            memcpy(&v, px(&rb, 8, 8), sizeof v);
            CHECK(v == 0.375f);   /* the draw's depth 0 was not written */
        }
        rb_done(&rb);
        c = gpu_cmd_begin(g_d, 0);
        gpu_cmd_barrier(c, s->rt, GPU_STATE_COPY_SOURCE, GPU_STATE_RENDER_TARGET);
        CHECK(run(g_d, c));
        gpu_texture_destroy(g_d, z, 0);
    }

    /* Today's zeta surface as the depth target (D24S8, or D32S8 without it):
     * depth 0 passes LESS_EQUAL against the clear's 1. */
    {
        gpu_texture_desc zd = tex_desc(16, 16, GPU_FORMAT_R24G8_TYPELESS, GPU_USE_DS | GPU_USE_SAMPLED,
                                       GPU_STATE_DEPTH_WRITE);
        zd.clear_format = GPU_FORMAT_D24_UNORM_S8_UINT;
        zd.clear_depth = 1.0f;
        const gpu_texture z = gpu_texture_create(g_d, &zd);
        gpu_view_desc vd;
        memset(&vd, 0, sizeof vd);
        vd.texture = z;
        vd.kind = GPU_VIEW_DSV;
        vd.format = GPU_FORMAT_D24_UNORM_S8_UINT;
        const gpu_view zv = gpu_view_get(g_d, &vd);
        CHECK(GPU_VALID(z) && GPU_VALID(zv));
        const float zero[4] = {0, 0, 0, 0};
        gpu_pass pass;
        memset(&pass, 0, sizeof pass);
        pass.nrt = 1;
        pass.rt[0] = s->rtv;
        pass.ds = zv;
        gpu_cmd* c = gpu_cmd_begin(g_d, 0);
        gpu_cmd_pass_begin(c, &pass);
        gpu_cmd_clear_color(c, s->rtv, zero, NULL);
        gpu_cmd_clear_depth(c, zv, GPU_CLEAR_DEPTH | GPU_CLEAR_STENCIL, 1.0f, 0, NULL);
        scene_binds(c, s, s->pso_zeta, t16, 0, 0);
        gpu_cmd_set_stencil_ref(c, 0x5A);
        gpu_cmd_draw(c, 3, 1, 0, 0);
        gpu_cmd_barrier(c, s->rt, GPU_STATE_RENDER_TARGET, GPU_STATE_COPY_SOURCE);
        CHECK(run(g_d, c));
        const gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0);
        CHECK(tex_read(s->rt, &td, &rb) && px_is(&rb, 8, 8, 128, 128, 64, 255, "zeta"));
        rb_done(&rb);
        c = gpu_cmd_begin(g_d, 0);
        gpu_cmd_barrier(c, s->rt, GPU_STATE_COPY_SOURCE, GPU_STATE_RENDER_TARGET);
        CHECK(run(g_d, c));
        gpu_texture_destroy(g_d, z, 0);
    }

    /* Two targets bound, a pipeline writing one: only with unused attachments. */
    if (has(GPU_CAP_UNUSED_ATTACHMENTS)) {
        const float zero[4] = {0, 0, 0, 0};
        gpu_pass pass;
        memset(&pass, 0, sizeof pass);
        pass.nrt = 2;
        pass.rt[0] = s->rtv;
        pass.rt[1] = s->rtv2;
        gpu_cmd* c = gpu_cmd_begin(g_d, 0);
        gpu_cmd_pass_begin(c, &pass);
        gpu_cmd_clear_color(c, s->rtv, zero, NULL);
        gpu_cmd_clear_color(c, s->rtv2, zero, NULL);
        scene_binds(c, s, s->pso, t16, 0, 0);
        gpu_cmd_draw(c, 3, 1, 0, 0);
        gpu_cmd_barrier(c, s->rt, GPU_STATE_RENDER_TARGET, GPU_STATE_COPY_SOURCE);
        CHECK(run(g_d, c));
        const gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0);
        CHECK(tex_read(s->rt, &td, &rb) && px_is(&rb, 8, 8, 128, 128, 64, 255, "unused attachment"));
        rb_done(&rb);
        c = gpu_cmd_begin(g_d, 0);
        gpu_cmd_barrier(c, s->rt, GPU_STATE_COPY_SOURCE, GPU_STATE_RENDER_TARGET);
        CHECK(run(g_d, c));
    }

    /* Occlusion counts: the whole 16x16 target from a draw and an indexed
     * draw, then a 4x4 scissor. Without precise queries, not zero. */
    const gpu_buffer qrb = buf_new(g_d, 4 * sizeof(uint64_t), GPU_MEM_READBACK, GPU_STATE_COPY_DEST);
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = s->rtv;
    gpu_cmd* c = gpu_cmd_begin(g_d, 0);
    gpu_cmd_pass_begin(c, &pass);
    scene_binds(c, s, s->pso, t16, 0, 0);
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
        const int good = has(GPU_CAP_OCCLUSION_PRECISE) ? q[0] == 256 && q[1] == 256 && q[2] == 16
                                                        : q[0] != 0 && q[1] != 0 && q[2] != 0;
        if (!good)
            printf("  occlusion counts %llu %llu %llu\n", (unsigned long long)q[0], (unsigned long long)q[1],
                   (unsigned long long)q[2]);
        CHECK(good);
    }
    gpu_buffer_unmap(g_d, qrb, 0, 0);
    gpu_buffer_destroy(g_d, qrb, 0);

    uint32_t cached = 7;
    const gpu_pipeline again = gpu_pipeline_create(g_d, &s->desc, 0, &cached);
    CHECK(again.v != 0);
}

/* A post pass: no vertices, a one-entry table, a static linear clamp. Its
 * corners are the 2x2 texture's, the right way up. */
static void test_post(const scene* s)
{
    const gpu_layout blit = blit_layout(g_d);
    const gpu_shader vs = SH(g_d, GPU_STAGE_VS, k_spv_post_vs);
    const gpu_shader ps = SH(g_d, GPU_STAGE_PS, k_spv_post_ps);
    CHECK(GPU_VALID(blit) && GPU_VALID(vs) && GPU_VALID(ps));
    gpu_pipeline_desc pd;
    draw_desc(&pd, blit, vs, ps);
    memset(&pd.vertex, 0, sizeof pd.vertex);
    const gpu_pipeline pso = gpu_pipeline_create(g_d, &pd, 0, NULL);
    CHECK(pso.v != 0);

    const gpu_texture_desc td = tex_desc(2, 2, GPU_FORMAT_R8G8B8A8_UNORM, GPU_USE_SAMPLED | GPU_USE_COPY_DST,
                                         GPU_STATE_COPY_DEST);
    const gpu_texture quad = gpu_texture_create(g_d, &td);
    gpu_footprint fp;
    const uint64_t total = gpu_texture_footprints(g_d, &td, 0, 1, 0, &fp, NULL, NULL);
    const gpu_buffer up = buf_new(g_d, total, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    const uint8_t row0[8] = {255, 0, 0, 255, 0, 255, 0, 255}, row1[8] = {0, 0, 255, 255, 255, 255, 255, 255};
    memcpy(buf_ptr(up), row0, 8);
    memcpy(buf_ptr(up) + fp.row_pitch, row1, 8);

    gpu_table_pool_desc tp;
    memset(&tp, 0, sizeof tp);
    tp.kind = GPU_TABLE_TEX;
    tp.count = 4;
    tp.nregions = 1;
    tp.region[0].tables = 4;
    tp.region[0].size = 1;
    const gpu_table_pool pool = gpu_table_pool_create(g_d, &tp);
    gpu_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.texture = quad;
    gpu_table_write_tex(g_d, pool, 2, &vd);

    const gpu_viewport vp = {0, 0, 16, 16, 0, 1};
    const gpu_rect sc = {0, 0, 16, 16};
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = s->rtv;
    gpu_table_pool none = {0};
    gpu_cmd* c = gpu_cmd_begin(g_d, 1);   /* the other recorder */
    gpu_cmd_copy_buf_to_tex(c, quad, 0, 0, 0, 0, up, &fp);
    gpu_cmd_barrier(c, quad, GPU_STATE_COPY_DEST, GPU_STATE_PIXEL_SHADER_RESOURCE);
    gpu_cmd_pass_begin(c, &pass);
    gpu_cmd_set_table_pools(c, pool, none);
    gpu_cmd_set_layout(c, blit);
    gpu_cmd_set_pipeline(c, pso);
    gpu_cmd_set_topology(c, GPU_TOPO_TRIANGLELIST);
    gpu_cmd_set_viewport(c, &vp);
    gpu_cmd_set_scissor(c, &sc);
    gpu_cmd_bind_table(c, 0, gpu_table_ref(g_d, pool, 2));
    gpu_cmd_draw(c, 3, 1, 0, 0);
    gpu_cmd_barrier(c, s->rt, GPU_STATE_RENDER_TARGET, GPU_STATE_COPY_SOURCE);
    CHECK(run(g_d, c));
    readback rb;
    const gpu_texture_desc rd = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0);
    CHECK(tex_read(s->rt, &rd, &rb));
    CHECK(px_is(&rb, 0, 0, 255, 0, 0, 255, "post") && px_is(&rb, 15, 0, 0, 255, 0, 255, "post") &&
          px_is(&rb, 0, 15, 0, 0, 255, 255, "post") && px_is(&rb, 15, 15, 255, 255, 255, 255, "post"));
    rb_done(&rb);
    c = gpu_cmd_begin(g_d, 0);
    gpu_cmd_barrier(c, s->rt, GPU_STATE_COPY_SOURCE, GPU_STATE_RENDER_TARGET);
    CHECK(run(g_d, c));
    gpu_buffer_destroy(g_d, up, 0);
    gpu_texture_destroy(g_d, quad, 0);
}

/* ---- The memo, caches, fences ---- */

static void test_memo(const scene* s)
{
    gpu_table_pool_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.kind = GPU_TABLE_TEX;
    pd.count = 16;
    pd.nregions = 2;   /* overlapping: tables at 0 and 8, and one at 4 */
    pd.region[0].tables = 2;
    pd.region[0].size = 8;
    pd.region[1].first = 4;
    pd.region[1].tables = 1;
    pd.region[1].size = 8;
    const gpu_table_pool pool = gpu_table_pool_create(g_d, &pd);
    CHECK(GPU_VALID(pool));
    if (!GPU_VALID(pool)) return;
    const uint32_t n0 = g_vk->n_srv_writes;
    gpu_view_desc a;
    memset(&a, 0, sizeof a);
    a.texture = s->texel;
    a.mips = 1;
    a.format = GPU_FORMAT_R8G8B8A8_UNORM;
    gpu_fast_table_write_tex(g_d, pool, 3, &a);   CHECK(g_vk->n_srv_writes == n0 + 1);
    gpu_fast_table_write_tex(g_d, pool, 3, &a);   CHECK(g_vk->n_srv_writes == n0 + 1);   /* the same: kept */
    gpu_table_write_tex(g_d, pool, 3, &a);        CHECK(g_vk->n_srv_writes == n0 + 1);   /* through the table too */
    a.swizzle = GPU_SWIZZLE_IDENTITY;
    gpu_fast_table_write_tex(g_d, pool, 3, &a);   CHECK(g_vk->n_srv_writes == n0 + 1);   /* 0 is the identity */
    a.swizzle = GPU_SWIZZLE(GPU_SWZ_A, GPU_SWZ_ZERO, GPU_SWZ_ONE, GPU_SWZ_B);
    gpu_fast_table_write_tex(g_d, pool, 3, &a);   CHECK(g_vk->n_srv_writes == n0 + 2);   /* changed: written */
    gpu_fast_table_write_tex(g_d, pool, 5, &a);   CHECK(g_vk->n_srv_writes == n0 + 3);   /* in two tables */
    gpu_view_desc e;
    memset(&e, 0, sizeof e);
    e.mips = 1;
    e.format = GPU_FORMAT_R8_UNORM;
    gpu_fast_table_write_tex(g_d, pool, 6, &e);   CHECK(g_vk->n_srv_writes == n0 + 3);   /* a new pool's empty entry */
    gpu_fast_table_write_tex(g_d, pool, 6, NULL); CHECK(g_vk->n_srv_writes == n0 + 4);
    gpu_fast_table_write_tex(g_d, pool, 6, NULL); CHECK(g_vk->n_srv_writes == n0 + 4);
    e.format = GPU_FORMAT_R8G8B8A8_UNORM;
    gpu_fast_table_write_tex(g_d, pool, 6, &e);   CHECK(g_vk->n_srv_writes == n0 + 4);   /* NULL's own */
    /* Through a writer (the pool found once): the same memo. */
    const gpu_table_writer w = gpu_table_writer_get(g_d, pool);
    CHECK(w.v != 0);
    gpu_fast_table_writer_tex(g_d, w, 5, &a);     CHECK(g_vk->n_srv_writes == n0 + 4);   /* entry 5 holds a */
    gpu_table_writer_tex(g_d, w, 5, &a);          CHECK(g_vk->n_srv_writes == n0 + 4);   /* through the table too */
    gpu_fast_table_writer_tex(g_d, w, 6, NULL);   CHECK(g_vk->n_srv_writes == n0 + 4);
    a.swizzle = 0;
    gpu_fast_table_writer_tex(g_d, w, 5, &a);     CHECK(g_vk->n_srv_writes == n0 + 5);   /* changed: written */
    gpu_fast_table_write_tex(g_d, pool, 5, &a);   CHECK(g_vk->n_srv_writes == n0 + 5);   /* the pool's memo has it */

    gpu_table_pool_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.kind = GPU_TABLE_SAMPLER;
    sd.count = 16;
    sd.nregions = 1;
    sd.region[0].tables = 1;
    sd.region[0].size = 16;
    const gpu_table_pool sp = gpu_table_pool_create(g_d, &sd);
    gpu_sampler_desc smp;
    memset(&smp, 0, sizeof smp);
    smp.filter = GPU_FILTER_ANISO(0);
    smp.address_u = smp.address_v = smp.address_w = GPU_ADDRESS_WRAP;
    smp.max_aniso = 16;   /* past the device's without anisotropy: clamped */
    smp.compare = GPU_CMP_NEVER;
    smp.max_lod = GPU_LOD_MAX;
    const gpu_sampler s2 = gpu_sampler_get(g_d, &smp);
    CHECK(GPU_VALID(sp) && GPU_VALID(s2));
    const uint32_t m0 = g_vk->n_sampler_writes;
    gpu_fast_table_write_sampler(g_d, sp, 2, s->samp); CHECK(g_vk->n_sampler_writes == m0 + 1);
    gpu_fast_table_write_sampler(g_d, sp, 2, s->samp); CHECK(g_vk->n_sampler_writes == m0 + 1);
    gpu_table_write_sampler(g_d, sp, 2, s2);           CHECK(g_vk->n_sampler_writes == m0 + 2);

    /* Tables: a set at each table's start, none elsewhere; both paths agree. */
    const gpu_table t0 = gpu_table_ref(g_d, pool, 0), t4 = gpu_table_ref(g_d, pool, 4),
                    t8 = gpu_table_ref(g_d, pool, 8);
    CHECK(t0.v && t4.v && t8.v && t0.v != t4.v && t4.v != t8.v && t0.v != t8.v);
    CHECK(gpu_table_ref(g_d, pool, 3).v == 0 && gpu_fast_table_ref(g_d, pool, 9).v == 0);
    CHECK(gpu_fast_table_ref(g_d, pool, 8).v == t8.v);
}

/* Step V5v: t20 switched between two buffers and offsets from draw to draw,
 * as the core's vertex-shader clamp does (its table's ring, then vf_buf):
 * every draw reads its own, and the recorder's set-0 memo finds a set it had
 * without the cache. The vertex shader scales the colour by raw[off + 4]. */
static void test_raw_switch(scene* s)
{
    /* From 0: 0 (black); from 256 (the largest minStorageBufferOffsetAlignment): 1. */
    float other[66];
    memset(other, 0, sizeof other);
    other[0] = other[65] = 1.0f;
    const gpu_buffer b = buf_new(g_d, 512, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
    CHECK(GPU_VALID(b));
    if (!GPU_VALID(b)) return;
    memcpy(buf_ptr(b), other, sizeof other);
    const gpu_addr k_at[3] = {gpu_fast_buffer_addr(g_d, s->raw), gpu_fast_buffer_addr(g_d, b),
                              gpu_fast_buffer_addr(g_d, b) + 256};
    const int k_lit[3] = {1, 0, 1};
    /* Each recorder draws with s->raw first, then with seq[i]: the memo holds
     * that pair, so a buffer comes from the cache (miss) when it is new or
     * fell out of the memo (1 after 2), and not when it is still in it. */
    static const int seq[] = {0, 1, 1, 0, 2, 2, 1, 1};
    static const int miss[] = {0, 1, 0, 0, 1, 0, 1, 0};
    const gpu_table t16 = gpu_fast_table_ref(g_d, s->tex_pool, 16);
    const float zero[4] = {0, 0, 0, 0};
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = s->rtv;
    int ok = 1;
    for (uint32_t i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        readback rb;
        gpu_cmd* c = gpu_cmd_begin(g_d, 0);
        gpu_cmd_pass_begin(c, &pass);
        gpu_cmd_clear_color(c, s->rtv, zero, NULL);
        scene_binds(c, s, s->pso, t16, 0, 0);
        gpu_cmd_draw(c, 3, 1, 0, 0);   /* s->raw's set: in the memo from the draw before */
        gpu_cmd_bind_raw(c, 4, k_at[seq[i]]);
        const uint32_t n0 = g_vk->n_set0_gets;
        gpu_cmd_draw(c, 3, 1, 0, 0);
        const uint32_t looked = g_vk->n_set0_gets - n0;
        gpu_cmd_barrier(c, s->rt, GPU_STATE_RENDER_TARGET, GPU_STATE_COPY_SOURCE);
        const int ran = run(g_d, c);
        const gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0);
        char what[32];
        snprintf(what, sizeof what, "raw switch %u", i);
        const int lit = k_lit[seq[i]];
        ok &= ran && tex_read(s->rt, &td, &rb) &&
              px_is(&rb, 8, 8, lit ? 128 : 0, lit ? 128 : 0, lit ? 64 : 0, lit ? 255 : 0, what);
        rb_done(&rb);
        c = gpu_cmd_begin(g_d, 0);
        gpu_cmd_barrier(c, s->rt, GPU_STATE_COPY_SOURCE, GPU_STATE_RENDER_TARGET);
        run(g_d, c);
        if (i && looked != (uint32_t)miss[i]) {
            printf("  raw switch %u: %u set-0 lookups, not %d\n", i, looked, miss[i]);
            ok = 0;
        }
    }
    CHECK(ok);
    gpu_buffer_destroy(g_d, b, 0);
    printf("  t20 switched between buffers and offsets: %u draws, each read its own; the memo held two\n",
           (unsigned)(sizeof seq / sizeof seq[0]));
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
    /* MIRROR_ONCE and BORDER: their own, or the fallbacks without the cap. */
    sd.address_u = GPU_ADDRESS_MIRROR_ONCE;
    sd.address_v = GPU_ADDRESS_BORDER;
    CHECK(GPU_VALID(gpu_sampler_get(g_d, &sd)));

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
    CHECK(!gpu_vk_slot_valid(&g_vk->view_slots, v1.id) && !gpu_vk_slot_valid(&g_vk->view_slots, v3.id));
    CHECK(gpu_view_get(g_d, &vd).id == 0);   /* a stale texture has no views */
}

static void test_fences(void)
{
    const gpu_fence f = gpu_signal(g_d);
    CHECK(f != 0 && gpu_fence_wait(g_d, f, 5000) == 0 && gpu_fence_completed(g_d) >= f);
    const int64_t t0 = ms_now();
    CHECK(gpu_fence_wait(g_d, f + 1000000, 100) == 1);   /* never signalled: the timeout */
    const int64_t ms = ms_now() - t0;
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

/* Step VC2: the file's checks. A bad file (path holds one) is ignored and the
 * next save replaces it; then every way of spoiling that good file is
 * refused before the driver sees it: one byte short or long, our header's
 * version, its length, the driver, a byte of the data (its sum), and
 * Vulkan's own header's vendor (with the sum made right again). The layout
 * (gpu_vk_pipe.c's pcache_head): magic 0, version 8, header length 12,
 * vendor 16, device 20, driver 24, UUID 28, data length 48, sum 56, data
 * from 64. */
static int pcache_load_on_new(const gpu_device_desc* dd, const char* path)
{
    gpu_device* d = gpu_vk_create(dd);
    if (!d) return -2;
    const int r = gpu_pipeline_cache_load(d, path);
    CHECK(GPU_VK_DEV(d)->n_validation == 0);
    gpu_destroy(d);
    return r;
}

static void pcache_write(const char* path, const uint8_t* p, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(p, 1, n, f);
        fclose(f);
    }
}

static void test_pipeline_cache_file(const gpu_device_desc* dd, const char* path)
{
    gpu_device* d = gpu_vk_create(dd);
    CHECK(d != NULL);
    if (!d) return;
    CHECK(gpu_pipeline_cache_load(d, path) == GPU_PIPELINE_CACHE_NEW);   /* (the bad file: ignored) */
    const gpu_layout l = guest_layout(d);
    const gpu_shader vs = SH(d, GPU_STAGE_VS, k_spv_guest_vs);
    const gpu_shader ps = SH(d, GPU_STAGE_PS, k_spv_guest_ps);
    gpu_pipeline_desc pd;
    draw_desc(&pd, l, vs, ps);
    uint32_t how = 7;
    CHECK(gpu_pipeline_create(d, &pd, 0, &how).v != 0);
    CHECK(gpu_pipeline_cache_save(d, path) == 1);   /* replaces it */
    gpu_destroy(d);
    CHECK(pcache_load_on_new(dd, path) == GPU_PIPELINE_CACHE_LOADED);

    FILE* f = fopen(path, "rb");
    uint8_t* good = NULL;
    long n = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        good = n > 64 ? (uint8_t*)malloc((size_t)n + 1) : NULL;
        if (good && fread(good, 1, (size_t)n, f) != (size_t)n) n = 0;
        fclose(f);
    }
    uint64_t size = 0;
    if (good) memcpy(&size, good + 48, 8);
    CHECK(good && n > 64 && size == (uint64_t)n - 64 && !memcmp(good, "PS3VKPC", 7));
    if (!good || size != (uint64_t)n - 64) {
        free(good);
        return;
    }
    uint8_t* bad = (uint8_t*)malloc((size_t)n + 1);
    static const struct { const char* what; int at; } k[] = {
        {"one byte short", -1}, {"one byte long", -2}, {"the version", 8}, {"the header's length", 12},
        {"the driver", 24}, {"a byte of the data", -3}, {"Vulkan's header's vendor", 64 + 8},
    };
    int refused = 0;
    for (size_t i = 0; bad && i < sizeof k / sizeof k[0]; i++) {
        memcpy(bad, good, (size_t)n);
        size_t len = (size_t)n;
        if (k[i].at == -1) len--;
        else if (k[i].at == -2) bad[len++] = 0;
        else if (k[i].at == -3) bad[len - 1] ^= 0x5A;
        else bad[k[i].at] ^= 0x01;
        if (k[i].at >= 64) {   /* the sum made right again: only Vulkan's header can refuse it */
            const uint64_t fnv = gpu_vk_fnv(GPU_VK_FNV_BASIS, bad + 64, (size_t)size);
            memcpy(bad + 56, &fnv, 8);
        }
        pcache_write(path, bad, len);
        const int r = pcache_load_on_new(dd, path);
        if (r != GPU_PIPELINE_CACHE_NEW) printf("  pipeline cache file: %s was not refused (%d)\n", k[i].what, r);
        else refused++;
    }
    CHECK(refused == (int)(sizeof k / sizeof k[0]));
    printf("  pipeline cache file: %ld bytes; a bad one replaced; %d spoilt ones refused\n", n, refused);
    free(bad);
    free(good);
}

static void test_pipeline_cache(void)
{
    char path[512];
    temp_path(path, sizeof path, "ps3r_test_gpu_vk.cache");
    remove(path);
    gpu_device_desc dd;
    memset(&dd, 0, sizeof dd);
    dd.flags = g_flags;
    dd.cmd_slots = 1;
    int hits = 1, cache = 1;
    for (int run_no = 0; run_no < 2; run_no++) {
        gpu_device* d = gpu_vk_create(&dd);
        CHECK(d != NULL);
        if (!d) break;
        /* With the cache masked (RSX_VK_MASK, as CI's run with every cap
         * off), only the no-cache case below applies. */
        if (!(d->caps.bits & GPU_CAP_PIPELINE_CACHE)) {
            printf("  pipeline cache: masked; the two-device round trip is skipped\n");
            gpu_destroy(d);
            cache = 0;
            break;
        }
        const int loaded = gpu_pipeline_cache_load(d, path);
        /* none the first time, the first device's the second */
        CHECK(loaded == (run_no ? GPU_PIPELINE_CACHE_LOADED : GPU_PIPELINE_CACHE_NEW));
        /* The layout and shaders made again on each device: the name is their
         * content's, not their handles'. */
        const gpu_layout l = guest_layout(d);
        const gpu_shader vs = SH(d, GPU_STAGE_VS, k_spv_guest_vs);
        const gpu_shader ps = SH(d, GPU_STAGE_PS, k_spv_guest_ps);
        gpu_pipeline_desc pd, pd2;
        draw_desc(&pd, l, vs, ps);
        pd2 = pd;
        pd2.depth_clip = 1;
        const gpu_vk_device* const g = GPU_VK_DEV(d);
        const uint32_t kept = g->npso;
        /* Made only for the cache (today's startup builds): made with it,
         * destroyed at once; no handle, no slot. Then a draw's, kept. */
        uint32_t c0 = 7, c3 = 7;
        CHECK(gpu_pipeline_create(d, &pd2, GPU_PIPELINE_STORE_ONLY, &c0).v == 0 && g->npso == kept);
        const gpu_pipeline p0 = gpu_pipeline_create(d, &pd2, 0, &c3);
        CHECK(p0.v != 0 && g->npso == kept + 1);
        CHECK(gpu_native(d, GPU_NATIVE_PIPELINE, p0.v) != NULL);
        uint32_t c1 = 7, c2 = 7;
        const gpu_pipeline p1 = gpu_pipeline_create(d, &pd, 0, &c1);
        const gpu_pipeline p2 = gpu_pipeline_create(d, &pd, 0, &c2);
        CHECK(p1.v != 0 && p2.v != 0);
        if (run_no == 0) {
            /* The first in an empty cache; the second description differs by
             * depth_clip alone, so a driver that caches per stage may find
             * it there (NVIDIA does). */
            CHECK(c0 == GPU_PIPELINE_STORED && (c1 == GPU_PIPELINE_STORED || c1 == GPU_PIPELINE_LOADED));
            hits = c3 == GPU_PIPELINE_LOADED;   /* the store-only one's, if the driver says (creation feedback) */
            if (!hits) printf("  pipeline cache: the driver does not say when it hits\n");
            else CHECK(c2 == GPU_PIPELINE_LOADED);
        } else if (hits) {
            CHECK(c0 == GPU_PIPELINE_LOADED && c1 == GPU_PIPELINE_LOADED);   /* from the file */
        }
        gpu_shader_destroy(d, vs);   /* pipelines outlive their shaders */
        gpu_shader_destroy(d, ps);
        if (run_no == 0) CHECK(gpu_pipeline_cache_save(d, path) == 1);
        CHECK(GPU_VK_DEV(d)->n_validation == 0);
        gpu_destroy(d);
    }
    /* A file from something else is not loaded. */
    if (cache) {
        FILE* f = fopen(path, "r+b");
        if (f) {
            fputc('X', f);
            fclose(f);
        }
        gpu_device* d = gpu_vk_create(&dd);
        if (d) {
            CHECK(gpu_pipeline_cache_load(d, path) == GPU_PIPELINE_CACHE_NEW);
            gpu_destroy(d);
        }
    }
    if (cache) test_pipeline_cache_file(&dd, path);
    /* No cache (the cap masked, as RSX_VK_MASK would): pipelines are made
     * without it and never stored; a store-only one is made and destroyed. */
    {
        dd.cap_mask = GPU_CAP_PIPELINE_CACHE;
        gpu_device* d = gpu_vk_create(&dd);
        CHECK(d != NULL);
        if (d) {
            CHECK(gpu_pipeline_cache_load(d, path) == GPU_PIPELINE_CACHE_NONE);
            const gpu_layout l = guest_layout(d);
            const gpu_shader vs = SH(d, GPU_STAGE_VS, k_spv_guest_vs);
            const gpu_shader ps = SH(d, GPU_STAGE_PS, k_spv_guest_ps);
            gpu_pipeline_desc pd;
            draw_desc(&pd, l, vs, ps);
            uint32_t how = 7;
            CHECK(gpu_pipeline_create(d, &pd, GPU_PIPELINE_STORE_ONLY, &how).v == 0);
            CHECK(how == GPU_PIPELINE_COMPILED && GPU_VK_DEV(d)->npso == 0);
            how = 7;
            CHECK(gpu_pipeline_create(d, &pd, 0, &how).v != 0 && how == GPU_PIPELINE_COMPILED);
            CHECK(gpu_pipeline_cache_save(d, path) == 0);
            CHECK(GPU_VK_DEV(d)->n_validation == 0);
            gpu_destroy(d);
        }
        dd.cap_mask = 0;
    }
    remove(path);
}

/* ---- A swap chain on a hidden window ---- */

#ifdef _WIN32
static int swap_frame(gpu_swapchain sc, const float rgba[4], uint32_t interval, uint32_t flags)
{
    gpu_texture img;
    const uint32_t i = gpu_swap_acquire(g_d, sc, &img);
    if (!GPU_VALID(img) || i >= gpu_swap_image_count(g_d, sc)) return -1;
    gpu_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.texture = img;
    vd.kind = GPU_VIEW_RTV;
    const gpu_view rtv = gpu_view_get(g_d, &vd);
    gpu_cmd* c = gpu_cmd_begin(g_d, 0);
    gpu_cmd_barrier(c, img, GPU_STATE_PRESENT, GPU_STATE_RENDER_TARGET);
    gpu_cmd_clear_color(c, rtv, rgba, NULL);
    gpu_cmd_barrier(c, img, GPU_STATE_RENDER_TARGET, GPU_STATE_PRESENT);
    gpu_cmd_end(g_d, c);
    gpu_submit_desc how;
    memset(&how, 0, sizeof how);
    how.swapchain = sc;
    how.flags = GPU_SUBMIT_ACQUIRE_WAIT | GPU_SUBMIT_PRESENT_SIGNAL | GPU_SUBMIT_SIGNAL;
    const gpu_fence f = gpu_submit(g_d, &c, 1, &how);
    const int r = gpu_swap_present(g_d, sc, interval, flags);
    if (!f || gpu_fence_wait(g_d, f, 20000)) return -2;
    return r;
}

static void test_swapchain(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ps3r_test_gpu_vk";
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, wc.lpszClassName, "test_gpu_vk", WS_OVERLAPPEDWINDOW, 0, 0, 160, 120, NULL, NULL,
                             wc.hInstance, NULL);   /* never shown */
    CHECK(w != NULL);
    if (!w) return;
    RECT cr;
    GetClientRect(w, &cr);
    gpu_swapchain_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.window = w;
    sd.width = 64;
    sd.height = 48;
    sd.format = GPU_FORMAT_R8G8B8A8_UNORM;
    sd.buffers = 2;
    sd.colour_space = GPU_COLOR_SRGB;
    sd.flags = has(GPU_CAP_TEARING) ? GPU_SWAP_TEARING : 0;
    const gpu_swapchain sc = gpu_swap_create(g_d, &sd);
    CHECK(GPU_VALID(sc) && gpu_native(g_d, GPU_NATIVE_SWAPCHAIN, sc.id) != NULL);
    if (GPU_VALID(sc)) {
        /* Every image by index: at least the buffers asked; acquire's is one of them. */
        const uint32_t n = gpu_swap_image_count(g_d, sc);
        gpu_texture img;
        const uint32_t i = gpu_swap_acquire(g_d, sc, &img);
        CHECK(n >= 2 && n <= GPU_VK_MAX_IMAGES && i < n && GPU_VALID(img));
        CHECK(gpu_swap_image(g_d, sc, i).id == img.id && !GPU_VALID(gpu_swap_image(g_d, sc, n)));
        for (uint32_t k = 0; k < n; k++)
            if (k != i) CHECK(GPU_VALID(gpu_swap_image(g_d, sc, k)) && gpu_swap_image(g_d, sc, k).id != img.id);
        /* The window's size, which the surface sets (not the size asked). */
        const gpu_vk_tex* t = &g_vk->tex[GPU_HANDLE_INDEX(img.id)];
        CHECK(t->width == (uint32_t)(cr.right - cr.left) && t->height == (uint32_t)(cr.bottom - cr.top));
        gpu_texture_destroy(g_d, img, 0);   /* the swap chain's: not freed */
        CHECK(gpu_vk_slot_valid(&g_vk->tex_slots, img.id));

        const float grey[4] = {0.5f, 0.5f, 0.5f, 1}, dark[4] = {0.1f, 0.1f, 0.1f, 1};
        CHECK(swap_frame(sc, grey, 1, 0) == 0);
        /* With tearing: interval 0, IMMEDIATE (switched per present, or the
         * swap chain made again without swapchain_maintenance1). */
        if (has(GPU_CAP_TEARING)) CHECK(swap_frame(sc, dark, 0, GPU_PRESENT_TEARING) == 0);
        CHECK(swap_frame(sc, grey, 1, 0) == 0);
        gpu_present_stats st;
        memset(&st, 0, sizeof st);
        gpu_wait_idle(g_d);
        const int sr = gpu_swap_stats(g_d, sc, &st);
        if (!has(GPU_CAP_PRESENT_WAIT)) CHECK(sr != 0);
        printf("  swap chain: %u images, %ux%u, statistics %s (presents %u, last on the display %u), "
               "sRGB colour space %s\n",
               n, t->width, t->height, sr ? "not yet" : "read", st.last_present_count, st.present_count,
               gpu_swap_hdr(g_d, sc, GPU_COLOR_SRGB, NULL) ? "taken" : "refused");
        CHECK(gpu_swap_hdr(g_d, sc, GPU_COLOR_SCRGB, NULL) == 0);   /* an RGBA8 swap chain */

        gpu_wait_idle(g_d);
        SetWindowPos(w, NULL, 0, 0, 200, 150, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        GetClientRect(w, &cr);
        CHECK(gpu_swap_resize(g_d, sc, 96, 64, GPU_FORMAT_R8G8B8A8_UNORM) == 0);
        CHECK(!gpu_vk_slot_valid(&g_vk->tex_slots, img.id));
        gpu_texture img2;
        gpu_swap_acquire(g_d, sc, &img2);
        CHECK(GPU_VALID(img2) && img2.id != img.id);
        if (GPU_VALID(img2)) {
            t = &g_vk->tex[GPU_HANDLE_INDEX(img2.id)];
            CHECK(t->width == (uint32_t)(cr.right - cr.left) && t->height == (uint32_t)(cr.bottom - cr.top));
        }
        CHECK(swap_frame(sc, grey, 1, 0) == 0);   /* acquire's image again, until the present */
        gpu_wait_idle(g_d);
        gpu_swap_destroy(g_d, sc);
        CHECK(!gpu_vk_slot_valid(&g_vk->tex_slots, img2.id));
    }
    DestroyWindow(w);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
}
#endif

/* ---- step VC2: presenting as the core does, and a lost device ---- */

#ifdef _WIN32
/* A borderless window of w x h client pixels, never shown. */
static HWND vc2_window(int w, int h)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ps3r_test_gpu_vk_vc2";
    RegisterClassA(&wc);   /* (once: a second time fails, harmlessly) */
    return CreateWindowExA(0, wc.lpszClassName, "test_gpu_vk", WS_POPUP, 0, 0, w, h, NULL, NULL, wc.hInstance, NULL);
}

static gpu_swapchain vc2_swap(gpu_device* d, HWND w)
{
    gpu_swapchain_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.window = w;
    sd.width = 64;
    sd.height = 64;
    sd.format = GPU_FORMAT_R8G8B8A8_UNORM;
    sd.buffers = 2;
    sd.colour_space = GPU_COLOR_SRGB;
    sd.flags = (d->caps.bits & GPU_CAP_TEARING) ? GPU_SWAP_TEARING : 0;
    return gpu_swap_create(d, &sd);
}

static void vc2_submit(gpu_device* d, gpu_cmd* c, gpu_swapchain sc, uint32_t more)
{
    gpu_submit_desc how;
    memset(&how, 0, sizeof how);
    how.swapchain = sc;
    how.flags = GPU_SUBMIT_ACQUIRE_WAIT | GPU_SUBMIT_PRESENT_SIGNAL | more;   /* (on every submit, as the core) */
    gpu_cmd_end(d, c);
    (void)gpu_submit(d, &c, 1, &how);
}

static gpu_fence vc2_submit_signal(gpu_device* d, gpu_cmd* c, gpu_swapchain sc)
{
    gpu_submit_desc how;
    memset(&how, 0, sizeof how);
    how.swapchain = sc;
    how.flags = GPU_SUBMIT_ACQUIRE_WAIT | GPU_SUBMIT_PRESENT_SIGNAL | GPU_SUBMIT_SIGNAL;
    gpu_cmd_end(d, c);
    return gpu_submit(d, &c, 1, &how);
}

/* The window's image cleared to rgba, in PRESENT before and after: the blit. */
static void vc2_blit(gpu_device* d, gpu_cmd* c, gpu_texture img, const float rgba[4], const gpu_rect* rect)
{
    gpu_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.texture = img;
    vd.kind = GPU_VIEW_RTV;
    const gpu_view rtv = gpu_view_get(d, &vd);
    gpu_cmd_barrier(c, img, GPU_STATE_PRESENT, GPU_STATE_RENDER_TARGET);
    gpu_cmd_clear_color(c, rtv, rgba, rect);
    gpu_cmd_barrier(c, img, GPU_STATE_RENDER_TARGET, GPU_STATE_PRESENT);
}

/* Frames as the core presents them, on a device of their own with two frames
 * in flight and three recorders each: every submit says ACQUIRE_WAIT and
 * PRESENT_SIGNAL (the present semaphore: VC1's gap 4, on a real present).
 * The first submit has the frame's own work and, on even frames, the blit
 * into the window's image; the second, on odd frames, the blit (after a
 * submit that waited for the acquire), and a copy of the image (a capture's
 * readback); the third the overlay, a corner of the image. The present
 * modes go FIFO (interval 1), IMMEDIATE (interval 0 with tearing:
 * RSX_VSYNC=0), FIFO, then interval 2 (RSX_VSYNC=2, present_gate). Every
 * present is FIFO or IMMEDIATE, never MAILBOX. Validation, with
 * synchronization validation, says nothing. */
static void test_present_frames(void)
{
    gpu_device_desc dd;
    memset(&dd, 0, sizeof dd);
    dd.flags = g_flags;
    dd.cmd_slots = 6;
    gpu_device* d = gpu_vk_create(&dd);
    CHECK(d != NULL);
    if (!d) return;
    gpu_vk_device* const v = GPU_VK_DEV(d);
    HWND w = vc2_window(64, 64);
    CHECK(w != NULL);
    const gpu_swapchain sc = w ? vc2_swap(d, w) : (gpu_swapchain){0};
    CHECK(GPU_VALID(sc));
    if (!GPU_VALID(sc)) {
        if (w) DestroyWindow(w);
        gpu_destroy(d);
        return;
    }
    gpu_vk_swap* const s = &v->swap[GPU_HANDLE_INDEX(sc.id)];
    const int tear = (d->caps.bits & GPU_CAP_TEARING) != 0;

    /* The frame's own target, and the capture's buffer for the image. */
    gpu_texture_desc otd = tex_desc(64, 64, GPU_FORMAT_R8G8B8A8_UNORM, GPU_USE_RT, GPU_STATE_RENDER_TARGET);
    const gpu_texture off = gpu_texture_create(d, &otd);
    gpu_view_desc ovd;
    memset(&ovd, 0, sizeof ovd);
    ovd.texture = off;
    ovd.kind = GPU_VIEW_RTV;
    const gpu_view off_rtv = gpu_view_get(d, &ovd);
    gpu_texture_desc itd = tex_desc(s->width, s->height, GPU_FORMAT_R8G8B8A8_UNORM, 0, GPU_STATE_PRESENT);
    gpu_footprint fp;
    uint32_t rows = 0;
    uint64_t row_bytes = 0;
    const uint64_t total = gpu_texture_footprints(d, &itd, 0, 1, 0, &fp, &rows, &row_bytes);
    const gpu_buffer rb = buf_new(d, total, GPU_MEM_READBACK, GPU_STATE_COPY_DEST);
    CHECK(GPU_VALID(off) && GPU_VALID(rb));

    enum { N = 32 };
    const int64_t f0 = ms_now();
    gpu_fence fence[2] = {0, 0};
    uint32_t done = 0, gate0 = 0, gate_ok = 1;
    int64_t gate_t0 = 0, last_shown = 0, min_gap = INT64_MAX;
    uint8_t want[4] = {0, 0, 0, 0};
    for (uint32_t f = 0; f < N && GPU_VALID(off) && GPU_VALID(rb); f++) {
        const uint32_t half = f % 2, phase = f / 8;
        uint32_t interval = phase == 1 ? 0u : phase == 3 ? 2u : 1u;
        if (interval == 0 && !tear) interval = 1;   /* (the core's: no tearing, vsync) */
        const uint32_t flags = interval == 0 ? GPU_PRESENT_TEARING : 0;
        if (fence[half]) CHECK(gpu_fence_wait(d, fence[half], 5000) == 0);
        gpu_texture img;
        gpu_swap_acquire(d, sc, &img);
        CHECK(GPU_VALID(img));
        if (!GPU_VALID(img)) break;
        const float c8 = (float)(f * 8 % 256) / 255.0f;
        const float colour[4] = {c8, 1.0f - c8, 0.25f, 1.0f}, white[4] = {1, 1, 1, 1}, grey[4] = {0.5f, 0.5f, 0.5f, 1};
        const gpu_rect corner = {0, 0, 8, 8};

        gpu_cmd* c = gpu_cmd_begin(d, half * 3 + 0);
        gpu_cmd_clear_color(c, off_rtv, grey, NULL);
        if (!(f & 1)) vc2_blit(d, c, img, colour, NULL);
        vc2_submit(d, c, sc, 0);

        c = gpu_cmd_begin(d, half * 3 + 1);
        if (f & 1) vc2_blit(d, c, img, colour, NULL);
        gpu_cmd_barrier(c, img, GPU_STATE_PRESENT, GPU_STATE_COPY_SOURCE);
        gpu_cmd_copy_tex_to_buf(c, rb, &fp, img, 0, NULL);
        gpu_cmd_barrier(c, img, GPU_STATE_COPY_SOURCE, GPU_STATE_PRESENT);
        vc2_submit(d, c, sc, 0);

        c = gpu_cmd_begin(d, half * 3 + 2);
        vc2_blit(d, c, img, white, &corner);
        fence[half] = vc2_submit_signal(d, c, sc);

        if (phase == 3 && !gate_t0) {
            gate0 = s->gate_n;
            gate_t0 = (int64_t)s->gate_ticks;
        }
        const int pr = gpu_swap_present(d, sc, interval, flags);
        CHECK(pr == GPU_PRESENTED);
        if (pr == GPU_PRESENTED) done++;
        /* The mode the present had: per present with swapchain_maintenance1,
         * else from the next acquire (the swap chain made again). */
        const VkPresentModeKHR m = interval == 0 ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
        CHECK(s->mode == VK_PRESENT_MODE_FIFO_KHR || s->mode == VK_PRESENT_MODE_IMMEDIATE_KHR);
        if (s->modes_switch) CHECK(s->mode == m);
        else CHECK(s->mode == m || s->recreate);
        if (phase == 3 && s->gate_n > gate0 && s->done_qpc && s->done_qpc != last_shown) {
            if (last_shown && s->done_qpc - last_shown < min_gap) min_gap = s->done_qpc - last_shown;
            last_shown = s->done_qpc;
        }
        want[0] = (uint8_t)(c8 * 255.0f + 0.5f);
        want[1] = (uint8_t)((1.0f - c8) * 255.0f + 0.5f);
        want[2] = 64;
        want[3] = 255;
    }
    gpu_wait_idle(d);
    /* The last frame's capture: its blit's colour, the overlay not yet in it. */
    const uint8_t* p = (const uint8_t*)gpu_buffer_map(d, rb, 0, total);
    if (p) {
        const uint8_t* q = p + 32ull * fp.row_pitch + 32ull * 4;
        CHECK(close_to(q[0], want[0]) && close_to(q[1], want[1]) && close_to(q[2], want[2]) && q[3] == 255);
        q = p;   /* (the corner: the frame before's overlay is gone, the blit covered it) */
        CHECK(close_to(q[0], want[0]) && close_to(q[1], want[1]));
        gpu_buffer_unmap(d, rb, 0, 0);
    }
    CHECK(done == N);
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    const int64_t tps = (int64_t)qf.QuadPart;   /* (now_ticks' units on Windows) */
    const uint32_t gates = s->gate_n - gate0;
    if ((d->caps.bits & GPU_CAP_PRESENT_WAIT) && s->period) {
        /* Interval 2: each present waited (present_wait and the hold),
         * unless the window is not shown (the compositor never says). */
        if (s->gate_miss) printf("  present gate: the hidden window's presents are never seen; no gate\n");
        else {
            CHECK(gates >= 6);
            /* Each frame shown two refreshes after the one before, less the
             * gate's quarter (a display that shows a frame at once) and some. */
            if (min_gap != INT64_MAX) gate_ok = min_gap >= 2 * s->period - s->period / 2;
            CHECK(gate_ok);
        }
        printf("  present gate: %u waits, %.2f ms a frame, refresh %.2f ms, shortest time between frames %.2f ms\n",
               gates, gates ? 1000.0 * (double)(s->gate_ticks - gate_t0) / (double)tps / gates : 0.0,
               1000.0 * (double)s->period / (double)tps,
               min_gap != INT64_MAX ? 1000.0 * (double)min_gap / (double)tps : 0.0);
    } else {
        CHECK(gates == 0);
        printf("  present gate: no present_wait (or no refresh rate): interval 2 is FIFO's 1\n");
    }
    printf("  presents as the core does: %u frames, 3 submits each, in %lld ms; modes %s, tearing %s\n", done,
           (long long)(ms_now() - f0), s->modes_switch ? "switched per present" : "by a new swap chain",
           tear ? "IMMEDIATE" : "none (FIFO)");
    CHECK(v->n_validation == 0);
    gpu_buffer_destroy(d, rb, 0);
    gpu_swap_destroy(d, sc);
    gpu_destroy(d);
    DestroyWindow(w);
}
#endif

/* gpu.h's codes (step VC2): every VkResult a present or an acquire gives, and
 * what a lost device leaves, on a device of its own noted lost as a driver
 * reset would leave it. (A real loss would need a GPU hang: a TDR resets the
 * driver for every program on the machine, so the test does not make one.)
 * On Windows also the window's surface lost: the window destroyed under its
 * swap chain, which the backend cannot make a surface for again. */
static void test_lost(void)
{
    static const struct { VkResult r; int code; } k[] = {
        {VK_SUCCESS, GPU_PRESENTED},
        {VK_SUBOPTIMAL_KHR, GPU_PRESENTED},
        {VK_ERROR_OUT_OF_DATE_KHR, GPU_PRESENTED},
        {VK_ERROR_DEVICE_LOST, GPU_PRESENT_LOST},
        {VK_ERROR_SURFACE_LOST_KHR, GPU_PRESENT_SURFACE_LOST},
        {VK_ERROR_OUT_OF_HOST_MEMORY, GPU_PRESENT_FAILED},
        {VK_ERROR_OUT_OF_DEVICE_MEMORY, GPU_PRESENT_FAILED},
        {VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT, GPU_PRESENT_FAILED},
        {VK_ERROR_UNKNOWN, GPU_PRESENT_FAILED},
        {VK_TIMEOUT, GPU_PRESENT_FAILED},
        {VK_NOT_READY, GPU_PRESENT_FAILED},
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) CHECK(gpu_vk_present_code(k[i].r) == k[i].code);

    gpu_device_desc dd;
    memset(&dd, 0, sizeof dd);
    dd.flags = g_flags | GPU_DEVICE_BREADCRUMBS;
    dd.cmd_slots = 1;
    gpu_device* d = gpu_vk_create(&dd);
    CHECK(d != NULL);
    if (!d) return;
    gpu_vk_device* const v = GPU_VK_DEV(d);
#ifdef _WIN32
    gpu_swapchain sc = {0};
    HWND w = vc2_window(64, 64);
    if (w) sc = vc2_swap(d, w);
    CHECK(GPU_VALID(sc));
    if (GPU_VALID(sc)) {   /* an image acquired and drawn into, left for the present after the loss */
        gpu_texture img;
        gpu_swap_acquire(d, sc, &img);
        const float red[4] = {1, 0, 0, 1};
        gpu_cmd* c = gpu_cmd_begin(d, 0);
        gpu_cmd_marker(c, "before the loss", 15);
        vc2_blit(d, c, img, red, NULL);
        vc2_submit(d, c, sc, 0);
    }
#endif
    gpu_wait_idle(d);   /* (the GPU idle: a lost device's fences read as passed, and frees come at once) */
    CHECK(gpu_lost(d) == 0);
    gpu_vk_lost_note(v, VK_ERROR_OUT_OF_HOST_MEMORY);
    gpu_vk_lost_note(v, VK_ERROR_SURFACE_LOST_KHR);   /* (a surface's: the device works) */
    CHECK(gpu_lost(d) == 0);
    gpu_vk_lose_by_hand(v);   /* (gpu_vk_lost_note's VK_ERROR_DEVICE_LOST, the driver not asked) */
    CHECK(gpu_lost(d) == (uint32_t)VK_ERROR_DEVICE_LOST && v->lost_by_hand == 1);
    gpu_vk_lost_note(v, VK_ERROR_OUT_OF_DATE_KHR);
    gpu_vk_lost_note(v, VK_ERROR_DEVICE_LOST);
    CHECK(gpu_lost(d) == (uint32_t)VK_ERROR_DEVICE_LOST);   /* the first reason stays */
    CHECK(gpu_fence_completed(d) == UINT64_MAX);
    const int64_t t0 = ms_now();
    CHECK(gpu_fence_wait(d, gpu_signal(d) + 1000000, 2000) == 0);   /* never signalled, but lost: at once */
    CHECK(ms_now() - t0 < 1000);
#ifdef _WIN32
    if (GPU_VALID(sc)) CHECK(gpu_swap_present(d, sc, 1, 0) == GPU_PRESENT_LOST);
#endif
    gpu_fault_report(d, "test_gpu_vk's noted loss");   /* (the line, the checkpoints; device_fault not asked) */
    CHECK(v->fault_done == 1);
#ifdef _WIN32
    if (GPU_VALID(sc)) gpu_swap_destroy(d, sc);
    if (w) DestroyWindow(w);
#endif
    CHECK(v->n_validation == 0);
    gpu_destroy(d);

#ifdef _WIN32
    /* The surface: presents until the window's surface is given up on. */
    w = vc2_window(64, 64);
    sc = w ? vc2_swap(g_d, w) : (gpu_swapchain){0};
    CHECK(GPU_VALID(sc));
    if (!GPU_VALID(sc)) return;
    gpu_vk_swap* const s = &g_vk->swap[GPU_HANDLE_INDEX(sc.id)];
    int pr = GPU_PRESENTED, frames = 0;
    for (; frames < 6 && pr != GPU_PRESENT_SURFACE_LOST; frames++) {
        if (frames == 1) DestroyWindow(w);   /* after one frame shown */
        gpu_texture img;
        gpu_swap_acquire(g_d, sc, &img);
        if (GPU_VALID(img)) {
            const float blue[4] = {0, 0, 1, 1};
            gpu_cmd* c = gpu_cmd_begin(g_d, 0);
            vc2_blit(g_d, c, img, blue, NULL);
            const gpu_fence f = vc2_submit_signal(g_d, c, sc);
            pr = gpu_swap_present(g_d, sc, 1, 0);
            CHECK(f && gpu_fence_wait(g_d, f, 5000) == 0);
        } else {
            pr = gpu_swap_present(g_d, sc, 1, 0);   /* (no image: SURFACE_LOST once given up, else FAILED) */
            CHECK(pr == GPU_PRESENT_SURFACE_LOST || pr == GPU_PRESENT_FAILED);
        }
        if (frames == 0) CHECK(pr == GPU_PRESENTED);
    }
    if (pr == GPU_PRESENT_SURFACE_LOST) {
        CHECK(s->surface_dead && gpu_lost(g_d) == 0);   /* the device works on */
        gpu_texture img;
        gpu_swap_acquire(g_d, sc, &img);
        CHECK(!GPU_VALID(img));   /* no more images */
        printf("  surface lost: given up after %d frames (the window destroyed after the first)\n", frames);
    } else {
        printf("  surface lost: the driver went on presenting to a destroyed window (%d frames); not reached\n",
               frames);
    }
    gpu_swap_destroy(g_d, sc);
    CHECK(g_vk->n_validation == 0);
#endif
}

/* ---- Breadcrumbs and the fault report ---- */

static void test_breadcrumbs(void)
{
    gpu_device_desc dd;
    memset(&dd, 0, sizeof dd);
    dd.flags = g_flags | GPU_DEVICE_BREADCRUMBS;
    dd.cmd_slots = 1;
    gpu_device* d = gpu_vk_create(&dd);
    CHECK(d != NULL);
    if (!d) return;
    printf("  breadcrumbs: %s, device fault %s\n", (d->caps.bits & GPU_CAP_BREADCRUMBS) ? "checkpoints" : "none",
           (d->caps.bits & GPU_CAP_DEVICE_FAULT) ? "yes" : "no");
    gpu_cmd* c = gpu_cmd_begin(d, 0);
    gpu_cmd_marker(c, "a marker", 8);
    CHECK(run(d, c));
    gpu_fault_report(d, "a working device");   /* says nothing */
    CHECK(gpu_lost(d) == 0);
    CHECK(GPU_VK_DEV(d)->n_validation == 0);
    gpu_destroy(d);
}

int main(void)
{
    const int validate = !env_is("GPU_TEST_VALIDATION", "0");
    if (validate) env_set("RSX_VK_VALIDATION", "1");
    const int on_hw = env_is("GPU_TEST_HW", "1");
    const char* mask = getenv("RSX_VK_MASK");
    g_flags = on_hw ? 0u : GPU_DEVICE_WARP;
    if (on_hw && env_is("GPU_TEST_LOW_POWER", "1")) g_flags = GPU_DEVICE_LOW_POWER;   /* the integrated GPU */
    gpu_device_desc dd;
    memset(&dd, 0, sizeof dd);
    dd.flags = g_flags;
    dd.cmd_slots = 2;
    g_d = gpu_vk_create(&dd);
    if (!g_d && !on_hw) {   /* no CPU device here: the GPU */
        printf("GPU Vulkan: no CPU device, so the GPU\n");
        g_flags = dd.flags = 0;
        g_d = gpu_vk_create(&dd);
    }
    if (!g_d) {
        printf("GPU Vulkan: no device\n");
        return 1;
    }
    g_vk = GPU_VK_DEV(g_d);
    gpu_adapter_info ai;
    gpu_adapter(g_d, &ai);
    printf("GPU Vulkan test on %s (driver %s), caps 0x%08X, RSX_VK_MASK %s, validation %s\n", ai.name, ai.driver,
           g_d->caps.bits, mask && mask[0] ? mask : "none", g_vk->validation ? "on" : "off");
    CHECK(g_d->caps.api == GPU_API_VULKAN);
    if (validate && !g_vk->validation) {
        printf("GPU Vulkan: the validation layer is not here (GPU_TEST_VALIDATION=0 runs without it)\n");
        CHECK(!"the validation layer");
    }

    test_buffers();
    test_host_import();
    test_textures();
#ifdef _WIN32
    test_footprints();
#endif
    test_clears();
    test_layouts();
    scene s;
    if (scene_make(&s)) {
        test_draw(&s);
        test_post(&s);
        test_memo(&s);
        test_raw_switch(&s);
    }
    test_caches();
    test_fences();
#ifdef _WIN32
    test_swapchain();
    test_present_frames();
#else
    printf("  swap chain: skipped (Windows only here)\n");
#endif
    test_lost();
    CHECK(gpu_lost(g_d) == 0);
    CHECK(g_vk->n_validation == 0);
    gpu_destroy(g_d);
    test_pipeline_cache();
    test_breadcrumbs();
    if (gpu_vk_n_validation_all) printf("GPU Vulkan: %u validation messages\n", gpu_vk_n_validation_all);
    CHECK(gpu_vk_n_validation_all == 0);   /* those at a device's end too */

    if (g_fail) {
        printf("GPU Vulkan: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    printf("GPU Vulkan: OK (%d checks, %s, validation %s)\n", g_checks,
           (g_flags & GPU_DEVICE_WARP) ? "CPU device" : "GPU", validate ? "on" : "off");
    return 0;
}

#else
#include <stdio.h>
int main(void)
{
    printf("test_gpu_vk: built without PS3RECOMP_RSX_HAL and PS3RECOMP_RSX_HAL_VULKAN\n");
    return 1;
}
#endif
