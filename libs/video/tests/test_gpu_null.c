/*
 * ps3recomp - the GPU layer's null backend test (CMake target test_gpu_null,
 * built with PS3RECOMP_RSX_HAL=ON; ctest -R gpu_null)
 *
 * Needs no GPU and no window, so it runs on every system CI builds on. Each
 * case records through gpu_null_cmd.h, the draw loop's path:
 *   - three frames of the guest pipelines' commands on two recorders and a
 *     swap chain, made as the renderer makes them: no check fails, two
 *     devices give the same stream hash and the same dump, the hash is the
 *     golden below, a change to one draw changes it and a marker's text
 *     does not;
 *   - buffers' bytes from upload to GPU to readback memory, a host import, a
 *     texture's readback (zeroes), footprints and sizes;
 *   - fences, the caps, and a lost device;
 *   - a case for each kind of check, each on its own device, which fails
 *     that check the number of times it should and no other. The first
 *     failure of each kind is printed: those lines are expected.
 */
#if defined(PS3RECOMP_RSX_HAL)

#include "gpu_null_cmd.h"
#include "gpu_null.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(cond)                                                              \
    do { g_checks++;                                                             \
         if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
                        g_fail++; } } while (0)

/* The three frames' stream hash. The hashed bytes are the same on every
 * system and compiler, so this is CI's golden there all; a change to what
 * the frames record, or to the stream's encoding (gpu_null.h), changes it:
 * then read the dump (GPU_NULL_DUMP) and update it. */
#define K_GOLDEN 0x680018cd611f8e1aull

/* ---- Helpers ---- */

static gpu_device* dev_new(uint32_t cap_mask)
{
    gpu_device_desc dd;
    memset(&dd, 0, sizeof dd);
    dd.cmd_slots = 2;
    dd.cap_mask = cap_mask;
    gpu_device* const d = gpu_null_create(&dd);
    CHECK(d != NULL);
    return d;
}

static gpu_buffer buf_new(gpu_device* d, uint64_t size, uint32_t use, uint32_t memory, gpu_state state)
{
    gpu_buffer_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = size;
    bd.use = use;
    bd.memory = memory;
    bd.state = state;
    const gpu_buffer b = gpu_buffer_create(d, &bd);
    CHECK(GPU_VALID(b));
    return b;
}

static gpu_buffer upload_new(gpu_device* d, uint64_t size, uint32_t use)
{
    return buf_new(d, size, use, GPU_MEM_UPLOAD, GPU_STATE_GENERIC_READ);
}

static gpu_buffer readback_new(gpu_device* d, uint64_t size, uint32_t use)
{
    return buf_new(d, size, use, GPU_MEM_READBACK, GPU_STATE_COPY_DEST);
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

static gpu_texture tex_new(gpu_device* d, uint32_t w, uint32_t h, gpu_format f, uint32_t use, gpu_state state)
{
    const gpu_texture_desc td = tex_desc(w, h, f, use, state);
    const gpu_texture t = gpu_texture_create(d, &td);
    CHECK(GPU_VALID(t));
    return t;
}

static gpu_texture rgba_new(gpu_device* d, uint32_t use, gpu_state state)
{
    return tex_new(d, 16, 16, GPU_FORMAT_R8G8B8A8_UNORM, use, state);
}

static gpu_view target(gpu_device* d, gpu_texture t, uint8_t kind, gpu_format f)
{
    gpu_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.texture = t;
    vd.kind = kind;
    vd.format = f;
    const gpu_view v = gpu_view_get(d, &vd);
    CHECK(GPU_VALID(v));
    return v;
}

static gpu_view_desc srv(gpu_texture t)
{
    gpu_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.texture = t;
    vd.kind = GPU_VIEW_SRV;
    return vd;
}

/* Ends and submits one recorder. */
static gpu_fence submit(gpu_device* d, gpu_cmd* c, uint32_t flags, gpu_swapchain sc)
{
    gpu_submit_desc how;
    memset(&how, 0, sizeof how);
    how.flags = flags;
    how.swapchain = sc;
    gpu_cmd_end(d, c);
    return gpu_submit(d, &c, 1, &how);
}

/* Ends, submits and waits. 1 when the fence passed. */
static int run(gpu_device* d, gpu_cmd* c)
{
    const gpu_swapchain none = {0};
    const gpu_fence f = submit(d, c, GPU_SUBMIT_SIGNAL, none);
    return f && gpu_fence_wait(d, f, 1000) == 0;
}

/* Any bytes are a shader to the null backend: they name the pipelines. */
static gpu_shader shader(gpu_device* d, uint32_t stage, const char* text)
{
    gpu_shader_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.stage = stage;
    sd.code = GPU_CODE_DXBC;
    sd.bytes = text;
    sd.size = strlen(text);
    const gpu_shader s = gpu_shader_create(d, &sd);
    CHECK(GPU_VALID(s));
    return s;
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
    const gpu_layout l = gpu_layout_create(d, &ld);
    CHECK(GPU_VALID(l));
    return l;
}

/* A blit's: one texture and one sampler. */
static gpu_layout blit_layout(gpu_device* d)
{
    gpu_layout_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.nslots = 2;
    ld.slot[0] = slot(GPU_SLOT_TABLE_TEX, 0, 1, GPU_VIS_PIXEL);
    ld.slot[1] = slot(GPU_SLOT_TABLE_SAMPLER, 0, 1, GPU_VIS_PIXEL);
    const gpu_layout l = gpu_layout_create(d, &ld);
    CHECK(GPU_VALID(l));
    return l;
}

/* attrs RGBA32F "ATTR" attributes (0: the vertices are fetched by the
 * shader), one RGBA8 target, a depth target of format ds (0: none). */
static gpu_pipeline pipe_new(gpu_device* d, gpu_layout layout, gpu_shader vs, gpu_shader ps, uint32_t attrs,
                             gpu_format ds)
{
    gpu_pipeline_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.layout = layout;
    pd.vs = vs;
    pd.ps = ps;
    pd.vertex.count = attrs;
    pd.vertex.stride = attrs * 16;
    for (uint32_t i = 0; i < attrs; i++) {
        memcpy(pd.vertex.attr[i].semantic, "ATTR", 5);
        pd.vertex.attr[i].semantic_index = i;
        pd.vertex.attr[i].format = GPU_FORMAT_R32G32B32A32_FLOAT;
        pd.vertex.attr[i].offset = i * 16;
    }
    pd.topology_type = GPU_TOPOLOGY_TYPE_TRIANGLE;
    pd.cull = GPU_CULL_NONE;
    pd.depth_test = pd.depth_write = ds ? 1 : 0;
    pd.depth_func = ds ? GPU_CMP_LESS_EQUAL : GPU_CMP_ALWAYS;
    pd.blend.src = pd.blend.src_alpha = GPU_BLEND_ONE;
    pd.blend.dst = pd.blend.dst_alpha = GPU_BLEND_ZERO;
    pd.blend.op = pd.blend.op_alpha = GPU_BLEND_OP_ADD;
    pd.blend.write_mask = GPU_WRITE_ALL;
    pd.nrt = 1;
    pd.rt_format[0] = GPU_FORMAT_R8G8B8A8_UNORM;
    pd.ds_format = ds;
    pd.samples = 1;
    pd.sample_mask = 0xFFFFFFFFu;
    uint32_t cached = 7;
    const gpu_pipeline p = gpu_pipeline_create(d, &pd, &cached);
    CHECK(p.v != 0 && cached == 0);
    return p;
}

static gpu_table_pool pool_new(gpu_device* d, uint32_t kind, uint32_t count, uint32_t tables, uint32_t size)
{
    gpu_table_pool_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.kind = kind;
    pd.count = count;
    pd.nregions = 1;
    pd.region[0].first = 0;
    pd.region[0].tables = tables;
    pd.region[0].size = size;
    const gpu_table_pool p = gpu_table_pool_create(d, &pd);
    CHECK(GPU_VALID(p));
    return p;
}

static gpu_sampler sampler(gpu_device* d, uint32_t linear, uint32_t address)
{
    gpu_sampler_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.filter = GPU_FILTER(linear, linear, linear, 0);
    sd.address_u = sd.address_v = sd.address_w = address;
    sd.max_aniso = 1;
    sd.compare = GPU_CMP_NEVER;
    sd.max_lod = GPU_LOD_MAX;
    const gpu_sampler s = gpu_sampler_get(d, &sd);
    CHECK(GPU_VALID(s));
    return s;
}

static gpu_footprint footprint(gpu_device* d, const gpu_texture_desc* td, uint32_t sub, uint64_t base)
{
    gpu_footprint fp;
    memset(&fp, 0, sizeof fp);
    gpu_texture_footprints(d, td, sub, 1, base, &fp, NULL, NULL);
    return fp;
}

static uint8_t* read_file(const char* path, size_t* size)
{
    *size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    uint8_t* b = (uint8_t*)malloc(cap);
    for (;;) {
        if (!b) break;
        n += fread(b + n, 1, cap - n, f);
        if (n < cap) break;
        uint8_t* nb = (uint8_t*)realloc(b, cap * 2);
        if (!nb) {
            free(b);
            b = NULL;
            break;
        }
        b = nb;
        cap *= 2;
    }
    fclose(f);
    *size = n;
    return b;
}

/* ---- Three frames, as the renderer records them ---- */

typedef struct world {
    gpu_device*    d;
    gpu_swapchain  sc;
    gpu_layout     guest, blit;
    gpu_pipeline   draw, fetch, copy;
    gpu_texture    colour, depth, tex[4], bc;
    gpu_view       rtv, dsv;
    gpu_table_pool tex_pool, samp_pool;
    gpu_sampler    samp[2];
    gpu_buffer     ring, vb, ib, raw, qrb;
    gpu_query_pool queries;
    gpu_fence      fence[3];
} world;

enum { W_PLAIN = 0, W_MORE_VERTICES = 1, W_OTHER_MARKERS = 2 };

/* The device, its objects, and the sampled textures uploaded. */
static int world_make(world* w, const char* dump)
{
    memset(w, 0, sizeof *w);
    w->d = dev_new(0);
    if (!w->d) return 0;
    gpu_device* const d = w->d;
    if (dump) CHECK(gpu_null_dump(d, dump));

    gpu_swapchain_desc scd;
    memset(&scd, 0, sizeof scd);
    scd.width = 320;
    scd.height = 180;
    scd.format = GPU_FORMAT_R8G8B8A8_UNORM;
    scd.buffers = 2;
    scd.colour_space = GPU_COLOR_SRGB;
    w->sc = gpu_swap_create(d, &scd);
    CHECK(GPU_VALID(w->sc));

    w->guest = guest_layout(d);
    w->blit = blit_layout(d);
    const gpu_shader vs = shader(d, GPU_STAGE_VS, "guest vertex program 1");
    const gpu_shader vsf = shader(d, GPU_STAGE_VS, "guest vertex program 2, fetching its vertices");
    const gpu_shader ps = shader(d, GPU_STAGE_PS, "guest fragment program 1");
    const gpu_shader bvs = shader(d, GPU_STAGE_VS, "the blit's vertex shader");
    const gpu_shader bps = shader(d, GPU_STAGE_PS, "the blit's pixel shader");
    w->draw = pipe_new(d, w->guest, vs, ps, 16, GPU_FORMAT_D24_UNORM_S8_UINT);
    w->fetch = pipe_new(d, w->guest, vsf, ps, 0, GPU_FORMAT_D24_UNORM_S8_UINT);
    w->copy = pipe_new(d, w->blit, bvs, bps, 0, 0);
    /* The same description again is the same pipeline; pipelines outlive
     * their shaders' code. */
    CHECK(pipe_new(d, w->guest, vs, ps, 16, GPU_FORMAT_D24_UNORM_S8_UINT).v == w->draw.v);
    CHECK(w->draw.v != w->fetch.v && w->fetch.v != w->copy.v);
    gpu_shader_destroy(d, vs);
    gpu_shader_destroy(d, vsf);
    gpu_shader_destroy(d, ps);
    gpu_shader_destroy(d, bvs);
    gpu_shader_destroy(d, bps);

    w->colour = tex_new(d, 320, 180, GPU_FORMAT_R8G8B8A8_UNORM, GPU_USE_RT | GPU_USE_SAMPLED | GPU_USE_COPY_SRC,
                        GPU_STATE_PIXEL_SHADER_RESOURCE);
    w->rtv = target(d, w->colour, GPU_VIEW_RTV, 0);
    w->depth = tex_new(d, 320, 180, GPU_FORMAT_R24G8_TYPELESS, GPU_USE_DS | GPU_USE_SAMPLED, GPU_STATE_DEPTH_WRITE);
    w->dsv = target(d, w->depth, GPU_VIEW_DSV, GPU_FORMAT_D24_UNORM_S8_UINT);
    CHECK(target(d, w->colour, GPU_VIEW_RTV, 0).id == w->rtv.id);   /* kept */

    /* Four RGBA8 textures and a BC1 one with four mips, from one upload. */
    const gpu_texture_desc t16 = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, GPU_USE_SAMPLED | GPU_USE_COPY_DST,
                                          GPU_STATE_COPY_DEST);
    gpu_texture_desc tbc = tex_desc(32, 32, GPU_FORMAT_BC1_UNORM, GPU_USE_SAMPLED | GPU_USE_COPY_DST,
                                    GPU_STATE_COPY_DEST);
    tbc.mips = 4;
    gpu_footprint fp[8];
    uint64_t at = 0;
    for (int k = 0; k < 4; k++) {
        w->tex[k] = gpu_texture_create(d, &t16);
        CHECK(GPU_VALID(w->tex[k]));
        at += gpu_texture_footprints(d, &t16, 0, 1, at, &fp[k], NULL, NULL);
    }
    w->bc = gpu_texture_create(d, &tbc);
    CHECK(GPU_VALID(w->bc));
    at += gpu_texture_footprints(d, &tbc, 0, 4, at, &fp[4], NULL, NULL);
    const gpu_buffer up = upload_new(d, at, 0);
    uint8_t* const p = (uint8_t*)gpu_buffer_map(d, up, 0, 0);
    CHECK(p != NULL);
    if (!p) return 0;
    for (uint64_t i = 0; i < at; i++) p[i] = (uint8_t)(i * 7u);
    gpu_buffer_unmap(d, up, 0, at);
    gpu_cmd* c = gpu_cmd_begin(d, 0);
    gpu_barrier bar[5];
    memset(bar, 0, sizeof bar);
    for (uint32_t k = 0; k < 5; k++) {
        if (k < 4) gpu_cmd_copy_buf_to_tex(c, w->tex[k], 0, 0, 0, 0, up, &fp[k]);
        else for (uint32_t m = 0; m < 4; m++) gpu_cmd_copy_buf_to_tex(c, w->bc, m, 0, 0, 0, up, &fp[4 + m]);
        bar[k].type = GPU_BARRIER_TRANSITION;
        bar[k].texture = k < 4 ? w->tex[k] : w->bc;
        bar[k].sub = GPU_SUB_ALL;
        bar[k].before = GPU_STATE_COPY_DEST;
        bar[k].after = GPU_STATE_PIXEL_SHADER_RESOURCE;
    }
    gpu_cmd_barrier_n(c, 5, bar);
    const gpu_swapchain none = {0};
    const gpu_fence f = submit(d, c, GPU_SUBMIT_SIGNAL, none);
    CHECK(f == 1);
    gpu_buffer_destroy(d, up, f);   /* once the copies are done */
    CHECK(gpu_fence_wait(d, f, 1000) == 0);

    /* Sixteen tables of sixteen textures; four of sixteen samplers. */
    w->tex_pool = pool_new(d, GPU_TABLE_TEX, 256, 16, 16);
    w->samp_pool = pool_new(d, GPU_TABLE_SAMPLER, 64, 4, 16);
    w->samp[0] = sampler(d, 0, GPU_ADDRESS_CLAMP);
    w->samp[1] = sampler(d, 1, GPU_ADDRESS_WRAP);
    CHECK(sampler(d, 0, GPU_ADDRESS_CLAMP).id == w->samp[0].id);   /* kept */

    /* Constants for two frames in flight, a triangle, the fetch's buffer,
     * and the occlusion results of two frames. */
    w->ring = upload_new(d, 2 * 4096, GPU_USE_CONSTANT);
    w->vb = upload_new(d, 3 * 256, GPU_USE_VERTEX);
    w->ib = upload_new(d, 12, GPU_USE_INDEX);
    w->raw = upload_new(d, 4096, GPU_USE_RAW);
    w->qrb = readback_new(d, 64, GPU_USE_QUERY);
    w->queries = gpu_query_pool_create(d, GPU_QUERY_OCCLUSION, 4);
    CHECK(GPU_VALID(w->queries));
    float* const v = (float*)gpu_buffer_map(d, w->vb, 0, 0);
    uint32_t* const ix = (uint32_t*)gpu_buffer_map(d, w->ib, 0, 0);
    CHECK(v && ix);
    if (!v || !ix) return 0;
    for (uint32_t i = 0; i < 3 * 64; i++) v[i] = (float)(i % 7) * 0.25f;
    for (uint32_t i = 0; i < 3; i++) ix[i] = i;
    gpu_buffer_unmap(d, w->vb, 0, 3 * 256);
    gpu_buffer_unmap(d, w->ib, 0, 12);
    return 1;
}

static void frame(world* w, uint32_t f, int variant)
{
    gpu_device* const d = w->d;
    const uint32_t win = f % 2;   /* this frame's half of the ring and of the tables */
    if (f >= 2) CHECK(gpu_fence_wait(d, w->fence[f - 2], 1000) == 0);
    float* const ring = (float*)gpu_buffer_map(d, w->ring, 0, 0);
    CHECK(ring != NULL);
    if (!ring) return;
    for (uint32_t i = 0; i < 4 * 128; i++) ring[win * 1024 + i] = (float)(f * 1000 + i);
    gpu_buffer_unmap(d, w->ring, win * 4096, 4096);

    gpu_cmd* const c = gpu_cmd_begin(d, win);
    CHECK(c != NULL);
    if (!c) return;
    const gpu_viewport vp = {0, 0, 320, 180, 0, 1};
    const gpu_rect sc = {0, 0, 320, 180};
    const float black[4] = {0, 0, 0, 1};
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = w->rtv;
    pass.ds = w->dsv;
    gpu_cmd_barrier(c, w->colour, GPU_STATE_PIXEL_SHADER_RESOURCE, GPU_STATE_RENDER_TARGET);
    gpu_cmd_pass_begin(c, &pass);
    gpu_cmd_clear_color(c, w->rtv, black, NULL);
    gpu_cmd_clear_depth(c, w->dsv, GPU_CLEAR_DEPTH | GPU_CLEAR_STENCIL, 1.0f, 0, NULL);
    gpu_cmd_set_table_pools(c, w->tex_pool, w->samp_pool);
    gpu_cmd_set_layout(c, w->guest);
    gpu_cmd_query_reset(c, w->queries, 0, 4);
    gpu_fast_table_write_sampler(d, w->samp_pool, 0, w->samp[0]);
    gpu_fast_table_write_sampler(d, w->samp_pool, 1, w->samp[1]);
    for (uint32_t i = 0; i < 4; i++) {
        /* The draw's table: a texture that changes with the frame, the BC
         * one, and empty entries (memoised after the first frame). */
        const uint32_t table = (win * 8 + i) * 16;
        gpu_view_desc t = srv(w->tex[(i + f) % 4]);
        gpu_fast_table_write_tex(d, w->tex_pool, table, &t);
        t = srv(w->bc);
        gpu_fast_table_write_tex(d, w->tex_pool, table + 1, &t);
        for (uint32_t k = 2; k < 16; k++) gpu_fast_table_write_tex(d, w->tex_pool, table + k, NULL);

        const int fetch = i % 2;
        char text[32];
        const int n = snprintf(text, sizeof text, "%s %u draw %u", variant == W_OTHER_MARKERS ? "picture" : "frame",
                               f, i);
        gpu_cmd_marker(c, text, (uint32_t)n);
        gpu_cmd_set_pipeline(c, fetch ? w->fetch : w->draw);
        gpu_cmd_set_topology(c, GPU_TOPO_TRIANGLELIST);
        gpu_cmd_set_viewport(c, &vp);
        gpu_cmd_set_scissor(c, &sc);
        const gpu_addr cb = gpu_fast_buffer_addr(d, w->ring) + win * 4096 + i * 512;
        gpu_cmd_bind_cb(c, 0, cb);
        gpu_cmd_bind_table(c, 1, gpu_fast_table_ref(d, w->tex_pool, table));
        gpu_cmd_bind_cb(c, 2, cb + 256);
        gpu_cmd_bind_table(c, 3, gpu_fast_table_ref(d, w->samp_pool, 0));
        gpu_cmd_bind_raw(c, 4, gpu_fast_buffer_addr(d, w->raw));
        gpu_cmd_push_const1(c, 5, i, 0);
        gpu_cmd_query_begin(c, w->queries, i);
        if (fetch) {
            gpu_cmd_draw(c, (variant == W_MORE_VERTICES && f == 1 && i == 1) ? 6 : 3, 1, 0, 0);
        } else {
            gpu_cmd_set_vb(c, 0, gpu_fast_buffer_addr(d, w->vb), 3 * 256, 256);
            gpu_cmd_set_ib(c, gpu_fast_buffer_addr(d, w->ib), 12, GPU_FORMAT_R32_UINT);
            gpu_cmd_draw_indexed(c, 3, 1, 0, 0, 0);
        }
        gpu_cmd_query_end(c, w->queries, i);
    }
    gpu_cmd_pass_end(c);
    gpu_cmd_query_resolve(c, w->queries, 0, 4, w->qrb, win * 32);
    gpu_cmd_barrier(c, w->colour, GPU_STATE_RENDER_TARGET, GPU_STATE_PIXEL_SHADER_RESOURCE);

    /* The picture onto the swap chain's image. */
    gpu_texture img;
    const uint32_t idx = gpu_swap_acquire(d, w->sc, &img);
    CHECK(idx == f % 2 && GPU_VALID(img));
    const gpu_view out = target(d, img, GPU_VIEW_RTV, 0);
    const gpu_view_desc t = srv(w->colour);
    gpu_fast_table_write_tex(d, w->tex_pool, 240, &t);
    gpu_fast_table_write_sampler(d, w->samp_pool, 16, w->samp[1]);
    gpu_cmd_barrier(c, img, GPU_STATE_PRESENT, GPU_STATE_RENDER_TARGET);
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = out;
    gpu_cmd_pass_begin(c, &pass);
    gpu_cmd_set_layout(c, w->blit);
    gpu_cmd_set_pipeline(c, w->copy);
    gpu_cmd_set_topology(c, GPU_TOPO_TRIANGLELIST);
    gpu_cmd_set_viewport(c, &vp);
    gpu_cmd_set_scissor(c, &sc);
    gpu_cmd_bind_table(c, 0, gpu_fast_table_ref(d, w->tex_pool, 240));
    gpu_cmd_bind_table(c, 1, gpu_fast_table_ref(d, w->samp_pool, 16));
    gpu_cmd_draw(c, 3, 1, 0, 0);
    gpu_cmd_pass_end(c);
    gpu_cmd_barrier(c, img, GPU_STATE_RENDER_TARGET, GPU_STATE_PRESENT);
    w->fence[f] = submit(d, c, GPU_SUBMIT_ACQUIRE_WAIT | GPU_SUBMIT_PRESENT_SIGNAL | GPU_SUBMIT_SIGNAL, w->sc);
    CHECK(w->fence[f] == f + 2);
    CHECK(gpu_swap_present(d, w->sc, 1, 0) == 0);
}

/* The three frames on a new device: its stream hash, its failed checks. */
static uint64_t world_run(int variant, const char* dump, uint32_t* errors)
{
    world w;
    *errors = ~0u;
    if (!world_make(&w, dump)) {
        if (w.d) gpu_destroy(w.d);
        return 0;
    }
    gpu_device* const d = w.d;
    for (uint32_t f = 0; f < 3; f++) frame(&w, f, variant);
    gpu_wait_idle(d);
    const uint64_t* const q = (const uint64_t*)gpu_buffer_map(d, w.qrb, 0, 64);
    CHECK(q != NULL);
    if (q) for (int k = 0; k < 8; k++) CHECK(q[k] == 0);   /* nothing is drawn */
    gpu_buffer_unmap(d, w.qrb, 0, 0);
    gpu_null_counts n;
    gpu_null_counts_get(d, &n);
    CHECK(n.submits == 4 && n.draws == 15 && n.presents == 3);
    if (variant == W_PLAIN) {
        /* Frame 0: the 16 entries of its 4 tables and the blit's; frame 1:
         * its own 4 tables; frame 2: the 4 entries that change in frame 0's;
         * 3 sampler entries, once. */
        CHECK(n.table_writes == (64 + 1) + 64 + 4 + 3);
    }
    gpu_texture_destroy(d, w.colour, 0);
    gpu_buffer_destroy(d, w.ring, 0);
    *errors = gpu_null_errors(d, GPU_NULL_ERR_ALL);
    const uint64_t h = gpu_null_stream_hash(d);
    gpu_destroy(d);
    return h;
}

static void test_frames(void)
{
    static const char* const dump[3] = { "test_gpu_null_a.txt", "test_gpu_null_b.txt", "test_gpu_null_c.txt" };
    uint32_t e0, e1, e2, e3;
    const uint64_t h0 = world_run(W_PLAIN, dump[0], &e0);
    const uint64_t h1 = world_run(W_PLAIN, dump[1], &e1);
    const uint64_t h2 = world_run(W_OTHER_MARKERS, dump[2], &e2);
    const uint64_t h3 = world_run(W_MORE_VERTICES, NULL, &e3);
    printf("  three frames: stream hash 0x%016llx\n", (unsigned long long)h0);
    CHECK(e0 == 0 && e1 == 0 && e2 == 0 && e3 == 0);
    CHECK(h0 != 0 && h0 == h1);   /* the same on two devices */
    CHECK(h2 == h0);              /* markers' text is not hashed */
    CHECK(h3 != h0);              /* one draw's vertex count is */
    if (K_GOLDEN && h0 != K_GOLDEN)
        printf("  the golden is 0x%016llx: the stream changed (diff its dump, then update K_GOLDEN)\n",
               (unsigned long long)K_GOLDEN);
    CHECK(h0 == K_GOLDEN);

    size_t n0, n1, n2;
    uint8_t* const a = read_file(dump[0], &n0);
    uint8_t* const b = read_file(dump[1], &n1);
    uint8_t* const m = read_file(dump[2], &n2);
    CHECK(a && b && m && n0 > 1000);
    if (a && b && m) {
        CHECK(n0 == n1 && !memcmp(a, b, n0));   /* the dump too */
        CHECK(n0 != n2 || memcmp(a, m, n0));   /* which shows the markers */
        char last[64];
        snprintf(last, sizeof last, "hash 0x%016llx\n", (unsigned long long)h0);
        const size_t ln = strlen(last);
        CHECK(n0 > ln && !memcmp(a + n0 - ln, last, ln));
        const char* const want[] = {
            "\nsubmit 1 0x7 swap#1\n", "\n  draw_indexed 3 1 0 0 0\n", "\n  marker \"frame 2 draw 3\"\n",
            "\nswap_present swap#1 1 0x0\n", "\ntable_write_tex tpool#1 240 tex#3 "
        };
        for (size_t k = 0; k < sizeof want / sizeof want[0]; k++) {
            const size_t wl = strlen(want[k]);
            int found = 0;
            for (size_t i = 0; i + wl <= n0 && !found; i++) found = !memcmp(a + i, want[k], wl);
            if (!found) printf("  the dump has no \"%s\"\n", want[k]);
            CHECK(found);
        }
    }
    free(a);
    free(b);
    free(m);
    if (!g_fail)
        for (int k = 0; k < 3; k++) remove(dump[k]);
}

/* ---- Bytes, footprints, the device ---- */

static void test_bytes(void)
{
    gpu_device* const d = dev_new(0);
    if (!d) return;
    const gpu_buffer a = upload_new(d, 4096, 0);
    const gpu_buffer g = buf_new(d, 4096, GPU_USE_COPY_SRC | GPU_USE_COPY_DST, GPU_MEM_GPU, GPU_STATE_COMMON);
    const gpu_buffer r = readback_new(d, 4096, 0);
    uint8_t* const p = (uint8_t*)gpu_buffer_map(d, a, 0, 0);
    CHECK(p != NULL);
    if (!p) return;
    for (int i = 0; i < 4096; i++) p[i] = (uint8_t)(i * 13 + 5);
    gpu_buffer_unmap(d, a, 0, 4096);

    /* Upload to GPU memory to readback: the GPU buffer is promoted to
     * COPY_DEST and must be moved to COPY_SOURCE between the copies. */
    gpu_cmd* c = gpu_cmd_begin(d, 0);
    gpu_cmd_copy_buffer(c, g, 0, a, 0, 4096);
    gpu_barrier bar;
    memset(&bar, 0, sizeof bar);
    bar.buffer = g;
    bar.before = GPU_STATE_COPY_DEST;
    bar.after = GPU_STATE_COPY_SOURCE;
    gpu_cmd_barrier_n(c, 1, &bar);
    gpu_cmd_copy_buffer(c, r, 100, g, 0, 3000);
    CHECK(run(d, c));
    const uint8_t* const q = (const uint8_t*)gpu_buffer_map(d, r, 0, 4096);
    CHECK(q && !memcmp(q + 100, p, 3000) && q[0] == 0 && q[99] == 0 && q[3100] == 0);
    /* The submit left the GPU buffer in COMMON (buffers decay), so a copy
     * into it needs no barrier. */
    c = gpu_cmd_begin(d, 1);
    gpu_cmd_copy_buffer(c, g, 0, a, 0, 16);
    CHECK(run(d, c));

    /* A copy into host memory lands there. */
    static uint8_t host[3 * 65536];
    uint8_t* const h = (uint8_t*)(((uintptr_t)host + 65535u) & ~(uintptr_t)65535u);
    const gpu_buffer hb = gpu_buffer_import_host(d, h, 65536);
    CHECK(GPU_VALID(hb));
    c = gpu_cmd_begin(d, 1);
    gpu_cmd_copy_buffer(c, hb, 512, a, 0, 4096);
    CHECK(run(d, c));
    CHECK(!memcmp(h + 512, p, 4096) && h[511] == 0);

    /* A texture holds nothing: its readback is zeroes, rows of 64 bytes at a
     * pitch of 256 over what was there. */
    const gpu_texture t = rgba_new(d, GPU_USE_COPY_SRC | GPU_USE_COPY_DST, GPU_STATE_COPY_DEST);
    const gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0);
    const gpu_footprint fp = footprint(d, &td, 0, 0);
    c = gpu_cmd_begin(d, 0);
    gpu_cmd_copy_buf_to_tex(c, t, 0, 0, 0, 0, a, &fp);
    gpu_cmd_barrier(c, t, GPU_STATE_COPY_DEST, GPU_STATE_COPY_SOURCE);
    gpu_cmd_copy_buffer(c, r, 0, a, 0, 4096);
    gpu_cmd_copy_tex_to_buf(c, r, &fp, t, 0, NULL);
    CHECK(run(d, c));
    const uint8_t* const z = (const uint8_t*)gpu_buffer_map(d, r, 0, 4096);
    int zero = 1, kept = 1;
    for (uint32_t row = 0; z && row < 16; row++)
        for (uint32_t x = 0; x < 256; x++) {
            if (x < 64) zero &= z[row * 256 + x] == 0;
            else kept &= z[row * 256 + x] == p[row * 256 + x];
        }
    CHECK(z && zero && kept);
    gpu_buffer_unmap(d, r, 0, 0);

    gpu_buffer_destroy(d, hb, 0);
    gpu_texture_destroy(d, t, 0);
    CHECK(gpu_null_errors(d, GPU_NULL_ERR_ALL) == 0);
    gpu_destroy(d);
}

static void test_footprints(void)
{
    gpu_device* const d = dev_new(0);
    if (!d) return;
    gpu_footprint fp[4];
    uint32_t rows[4];
    uint64_t rb[4];

    gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0);
    CHECK(gpu_texture_footprints(d, &td, 0, 1, 0, fp, rows, rb) == 15 * 256 + 64);
    CHECK(fp[0].offset == 0 && fp[0].row_pitch == 256 && fp[0].width == 16 && fp[0].height == 16 &&
          fp[0].depth == 1 && fp[0].format == GPU_FORMAT_R8G8B8A8_UNORM && rows[0] == 16 && rb[0] == 64);
    CHECK(gpu_texture_footprints(d, &td, 0, 1, 100, fp, NULL, NULL) == 412 + 15 * 256 + 64);   /* from 512 */
    CHECK(fp[0].offset == 512);
    uint64_t size = 0, align = 0;
    gpu_texture_size(d, &td, &size, &align);
    CHECK(size == 65536 && align == 65536);

    /* BC1: rows of 4x4 blocks of 8 bytes, the small mips a whole block. */
    td = tex_desc(8, 8, GPU_FORMAT_BC1_UNORM, 0, 0);
    td.mips = 4;
    CHECK(gpu_texture_footprints(d, &td, 0, 1, 0, fp, rows, rb) == 256 + 16);
    CHECK(rows[0] == 2 && rb[0] == 16 && fp[0].width == 8);
    CHECK(gpu_texture_footprints(d, &td, 0, 4, 0, fp, rows, rb) == 1536 + 8);
    CHECK(fp[1].offset == 512 && fp[2].offset == 1024 && fp[3].offset == 1536);
    CHECK(fp[1].width == 4 && fp[2].width == 4 && fp[3].width == 4 && fp[3].height == 4 && rows[3] == 1 && rb[3] == 8);

    /* D24S8: the depth plane as R32_TYPELESS, the stencil as R8_TYPELESS. */
    td = tex_desc(16, 16, GPU_FORMAT_R24G8_TYPELESS, 0, 0);
    CHECK(gpu_texture_footprints(d, &td, 0, 2, 0, fp, rows, rb) == 4096 + 15 * 256 + 16);
    CHECK(fp[0].format == GPU_FORMAT_R32_TYPELESS && rb[0] == 64 && fp[1].offset == 4096 && rb[1] == 16);
    CHECK(gpu_null_errors(d, GPU_NULL_ERR_ALL) == 0);
    gpu_destroy(d);
}

static void test_device(void)
{
    gpu_device* d = dev_new(0);
    if (!d) return;
    gpu_adapter_info info;
    gpu_adapter(d, &info);
    CHECK(!strncmp(info.name, "null", 4));
    CHECK(d->caps.api == GPU_API_NULL && d->caps.cb_align == 256 && (d->caps.bits & GPU_CAP_BC));
    CHECK(gpu_native(d, 0, 0) == NULL);
    const uint32_t rgba = gpu_format_support(d, GPU_FORMAT_R8G8B8A8_UNORM, 1);
    CHECK((rgba & GPU_FMT_SAMPLED) && (rgba & GPU_FMT_RT) && (rgba & GPU_FMT_DISPLAY));
    CHECK((gpu_format_support(d, GPU_FORMAT_BC1_UNORM, 1) & (GPU_FMT_SAMPLED | GPU_FMT_RT)) == GPU_FMT_SAMPLED);
    CHECK(gpu_format_support(d, GPU_FORMAT_D24_UNORM_S8_UINT, 1) & GPU_FMT_DS);

    /* The timeline: instant. */
    CHECK(gpu_fence_completed(d) == 0);
    const gpu_fence f = gpu_signal(d);
    CHECK(f == 1 && gpu_fence_completed(d) == 1 && gpu_fence_wait(d, f, 0) == 0);
    gpu_wait_idle(d);
    CHECK(gpu_fence_completed(d) == 2);
    gpu_cmd* const c = gpu_cmd_begin(d, 0);
    CHECK(c != NULL && gpu_cmd_begin(d, 2) == NULL);   /* two slots */
    const gpu_swapchain none = {0};
    CHECK(submit(d, c, 0, none) == 0);                 /* no fence without GPU_SUBMIT_SIGNAL */

    /* Lost: the fence reads UINT64_MAX and waits end at once. */
    CHECK(gpu_lost(d) == 0);
    gpu_null_test_lose(d, 0x887A0005u);
    CHECK(gpu_lost(d) == 0x887A0005u);
    CHECK(gpu_fence_completed(d) == UINT64_MAX);
    CHECK(gpu_fence_wait(d, 1000, 1000) == 0);
    CHECK(gpu_null_errors(d, GPU_NULL_ERR_ALL) == 0);
    gpu_destroy(d);

    /* cap_mask turns features off. */
    d = dev_new(GPU_CAP_BC | GPU_CAP_HOST_IMPORT);
    if (!d) return;
    CHECK(!(d->caps.bits & GPU_CAP_BC) && gpu_format_support(d, GPU_FORMAT_BC1_UNORM, 1) == 0);
    CHECK(d->caps.host_import_align == 0);
    gpu_destroy(d);
}

/* ---- One case per check ---- */

/* n failures of kind on d, and none of any other kind. */
static void expect(gpu_device* d, int kind, uint32_t n)
{
    const uint32_t got = gpu_null_errors(d, kind), all = gpu_null_errors(d, GPU_NULL_ERR_ALL);
    if (got != n || all != n)
        printf("  %s: %u failures, wanted %u; %u of every kind\n", gpu_null_error_name(kind), got, n, all);
    CHECK(got == n && all == n);
}

/* A draw with everything bound: one texture and one sampler. */
typedef struct mini {
    gpu_device*    d;
    gpu_layout     layout;
    gpu_pipeline   pipe;
    gpu_texture    rt, tex;
    gpu_view       rtv;
    gpu_table_pool tp, sp;
} mini;

static gpu_device* mini_make(mini* m)
{
    memset(m, 0, sizeof *m);
    gpu_device* const d = m->d = dev_new(0);
    if (!d) return NULL;
    m->layout = blit_layout(d);
    m->pipe = pipe_new(d, m->layout, shader(d, GPU_STAGE_VS, "vs"), shader(d, GPU_STAGE_PS, "ps"), 0, 0);
    m->rt = tex_new(d, 64, 64, GPU_FORMAT_R8G8B8A8_UNORM, GPU_USE_RT | GPU_USE_COPY_SRC, GPU_STATE_RENDER_TARGET);
    m->rtv = target(d, m->rt, GPU_VIEW_RTV, 0);
    m->tex = rgba_new(d, GPU_USE_SAMPLED, GPU_STATE_PIXEL_SHADER_RESOURCE);
    m->tp = pool_new(d, GPU_TABLE_TEX, 64, 4, 16);
    m->sp = pool_new(d, GPU_TABLE_SAMPLER, 16, 1, 16);
    const gpu_view_desc v = srv(m->tex);
    gpu_fast_table_write_tex(d, m->tp, 0, &v);
    gpu_fast_table_write_sampler(d, m->sp, 0, sampler(d, 1, GPU_ADDRESS_CLAMP));
    return d;
}

static void mini_binds(const mini* m, gpu_cmd* c, uint32_t table)
{
    const gpu_viewport vp = {0, 0, 64, 64, 0, 1};
    const gpu_rect sc = {0, 0, 64, 64};
    gpu_cmd_set_table_pools(c, m->tp, m->sp);
    gpu_cmd_set_layout(c, m->layout);
    gpu_cmd_set_pipeline(c, m->pipe);
    gpu_cmd_set_topology(c, GPU_TOPO_TRIANGLESTRIP);
    gpu_cmd_set_viewport(c, &vp);
    gpu_cmd_set_scissor(c, &sc);
    gpu_cmd_bind_table(c, 0, gpu_fast_table_ref(m->d, m->tp, table));
    gpu_cmd_bind_table(c, 1, gpu_fast_table_ref(m->d, m->sp, 0));
}

static void mini_draw(const mini* m, gpu_cmd* c, uint32_t table)
{
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = m->rtv;
    gpu_cmd_pass_begin(c, &pass);
    mini_binds(m, c, table);
    gpu_cmd_draw(c, 4, 1, 0, 0);
}

/* Stale, freed, never-made and zero handles. */
static void neg_handle(void)
{
    gpu_device* const d = dev_new(0);
    if (!d) return;
    const gpu_texture t1 = rgba_new(d, GPU_USE_SAMPLED, GPU_STATE_PIXEL_SHADER_RESOURCE);
    gpu_texture_destroy(d, t1, 0);
    const gpu_texture t2 = rgba_new(d, GPU_USE_SAMPLED, GPU_STATE_PIXEL_SHADER_RESOURCE);
    CHECK(t1.id != t2.id && GPU_HANDLE_INDEX(t1.id) == GPU_HANDLE_INDEX(t2.id));   /* a newer generation */
    const gpu_buffer a = upload_new(d, 256, 0);
    gpu_buffer gone = upload_new(d, 256, 0);
    gpu_buffer_destroy(d, gone, 0);
    const gpu_buffer never = {0x00ABCDEFu};
    gpu_cmd* const c = gpu_cmd_begin(d, 0);
    gpu_cmd_barrier(c, t1, GPU_STATE_PIXEL_SHADER_RESOURCE, GPU_STATE_COPY_DEST);    /* 1: stale */
    gpu_cmd_copy_buffer(c, never, 0, a, 0, 16);                                       /* 2: never made */
    CHECK(run(d, c));
    CHECK(gpu_buffer_map(d, gone, 0, 0) == NULL);                                     /* 3: freed */
    gpu_texture_destroy(d, t1, 0);                                                    /* 4: freed twice */
    const gpu_shader vs = shader(d, GPU_STAGE_VS, "vs");
    gpu_shader_destroy(d, vs);
    gpu_pipeline_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.layout = blit_layout(d);
    pd.vs = vs;
    CHECK(gpu_pipeline_create(d, &pd, NULL).v == 0);                                  /* 5: a freed shader */
    gpu_buffer_destroy(d, (gpu_buffer){0}, 0);                                        /* (nothing) */
    expect(d, GPU_NULL_ERR_HANDLE, 5);
    gpu_destroy(d);
}

/* Freed before the fence of the work that uses it. */
static void neg_freed(void)
{
    gpu_device* const d = dev_new(0);
    if (!d) return;
    const gpu_swapchain none = {0};
    const gpu_buffer a = upload_new(d, 4096, 0);
    const gpu_buffer g = buf_new(d, 4096, GPU_USE_COPY_SRC | GPU_USE_COPY_DST, GPU_MEM_GPU, GPU_STATE_COMMON);
    const gpu_buffer r = readback_new(d, 65536, 0);
    const gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0);
    const gpu_footprint fp = footprint(d, &td, 0, 0);
    gpu_cmd* c = gpu_cmd_begin(d, 0);
    gpu_cmd_copy_buffer(c, g, 0, a, 0, 16);
    CHECK(submit(d, c, GPU_SUBMIT_SIGNAL, none) == 1);
    gpu_buffer_destroy(d, g, 0);                    /* 1: fence 1 not seen to pass */
    CHECK(gpu_fence_completed(d) == 1);
    gpu_buffer_destroy(d, a, 1);                    /* (after its fence: fine) */

    const gpu_texture t = rgba_new(d, GPU_USE_COPY_SRC, GPU_STATE_COPY_SOURCE);
    c = gpu_cmd_begin(d, 1);
    gpu_cmd_copy_tex_to_buf(c, r, &fp, t, 0, NULL);
    gpu_texture_destroy(d, t, 0);                   /* (not used yet: freed now) */
    CHECK(submit(d, c, GPU_SUBMIT_SIGNAL, none) == 2);   /* 2: the recording uses it */

    const gpu_texture t2 = rgba_new(d, GPU_USE_COPY_SRC, GPU_STATE_COPY_SOURCE);
    c = gpu_cmd_begin(d, 0);
    gpu_cmd_copy_tex_to_buf(c, r, &fp, t2, 0, NULL);
    CHECK(submit(d, c, GPU_SUBMIT_SIGNAL, none) == 3);
    gpu_texture_destroy(d, t2, 1);                  /* 3: after fence 1, used until 3 */
    expect(d, GPU_NULL_ERR_FREED, 3);
    gpu_destroy(d);
}

/* A barrier's before state against the tracked one, subresource by
 * subresource. */
static void neg_barrier(void)
{
    gpu_device* const d = dev_new(0);
    if (!d) return;
    gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, GPU_USE_SAMPLED | GPU_USE_COPY_SRC,
                                   GPU_STATE_PIXEL_SHADER_RESOURCE);
    td.mips = 2;
    const gpu_texture t = gpu_texture_create(d, &td);
    const gpu_buffer a = upload_new(d, 256, 0);
    gpu_cmd* const c = gpu_cmd_begin(d, 0);
    gpu_cmd_barrier(c, t, GPU_STATE_COPY_DEST, GPU_STATE_PIXEL_SHADER_RESOURCE);   /* 1: it is in PSR */
    gpu_barrier b[2];
    memset(b, 0, sizeof b);
    b[0].texture = t;
    b[0].sub = 1;
    b[0].before = GPU_STATE_PIXEL_SHADER_RESOURCE;
    b[0].after = GPU_STATE_COPY_SOURCE;
    gpu_cmd_barrier_n(c, 1, b);                                                    /* (mip 1 only) */
    gpu_cmd_barrier(c, t, GPU_STATE_PIXEL_SHADER_RESOURCE, GPU_STATE_COPY_SOURCE); /* 2: mip 1 is not in PSR */
    b[0].sub = 2;
    gpu_cmd_barrier_n(c, 1, b);                                                    /* 3: no subresource 2 */
    b[1].buffer = a;
    b[1].before = GPU_STATE_GENERIC_READ;
    b[1].after = GPU_STATE_COPY_DEST;
    gpu_cmd_barrier_n(c, 1, &b[1]);                                                /* 4: upload memory */
    CHECK(run(d, c));
    expect(d, GPU_NULL_ERR_BARRIER, 4);
    gpu_destroy(d);
}

/* Textures and buffers used in a state that does not allow it. */
static void neg_state(void)
{
    mini m;
    gpu_device* const d = mini_make(&m);
    if (!d) return;
    const gpu_texture dst = tex_new(d, 64, 64, GPU_FORMAT_R8G8B8A8_UNORM, GPU_USE_COPY_DST, GPU_STATE_COPY_DEST);
    const gpu_texture fresh = rgba_new(d, GPU_USE_SAMPLED | GPU_USE_COPY_DST, GPU_STATE_COPY_DEST);
    const gpu_view_desc v = srv(fresh);
    gpu_fast_table_write_tex(d, m.tp, 16, &v);
    gpu_cmd* const c = gpu_cmd_begin(d, 0);
    gpu_cmd_copy_texture(c, dst, 0, 0, 0, 0, m.rt, 0, NULL);                          /* 1: a source in RT */
    mini_draw(&m, c, 16);                                                             /* 2: a table's in COPY_DEST */
    gpu_cmd_barrier(c, m.rt, GPU_STATE_RENDER_TARGET, GPU_STATE_PIXEL_SHADER_RESOURCE);
    mini_draw(&m, c, 0);                                                              /* 3: a target in PSR */
    CHECK(run(d, c));
    gpu_buffer_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = 256;
    bd.memory = GPU_MEM_UPLOAD;
    bd.state = GPU_STATE_COMMON;
    gpu_buffer_create(d, &bd);                                                        /* 4: upload in COMMON */
    expect(d, GPU_NULL_ERR_STATE, 4);
    gpu_destroy(d);
}

/* Draws without what they need. */
static void neg_draw(void)
{
    mini m;
    gpu_device* const d = mini_make(&m);
    if (!d) return;
    const gpu_pipeline other = pipe_new(d, guest_layout(d), shader(d, GPU_STAGE_VS, "guest vs"),
                                        shader(d, GPU_STAGE_PS, "guest ps"), 0, 0);
    gpu_cmd* c = gpu_cmd_begin(d, 0);
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = m.rtv;
    gpu_cmd_pass_begin(c, &pass);
    gpu_cmd_draw(c, 3, 1, 0, 0);                                   /* 1: no layout */
    gpu_cmd_set_layout(c, m.layout);
    gpu_cmd_draw(c, 3, 1, 0, 0);                                   /* 2: no pipeline */
    mini_binds(&m, c, 0);
    gpu_cmd_set_layout(c, m.layout);                               /* (forgets the bindings) */
    gpu_cmd_bind_table(c, 0, gpu_fast_table_ref(d, m.tp, 0));
    gpu_cmd_draw(c, 3, 1, 0, 0);                                   /* 3: slot 1 unbound */
    gpu_cmd_bind_table(c, 1, gpu_fast_table_ref(d, m.sp, 0));
    gpu_cmd_draw(c, 3, 1, 0, 0);                                   /* (fine) */
    gpu_cmd_set_pipeline(c, other);
    gpu_cmd_draw(c, 3, 1, 0, 0);                                   /* 4: another layout's pipeline */
    CHECK(run(d, c));
    c = gpu_cmd_begin(d, 1);
    mini_binds(&m, c, 0);
    gpu_cmd_draw(c, 3, 1, 0, 0);                                   /* 5: no pass_begin */
    CHECK(run(d, c));
    expect(d, GPU_NULL_ERR_DRAW, 5);
    gpu_destroy(d);
}

/* Binds of buffers without the use, or outside them; a map of GPU memory. */
static void neg_use(void)
{
    gpu_device* const d = dev_new(0);
    if (!d) return;
    const gpu_buffer vb = upload_new(d, 1024, GPU_USE_VERTEX);
    const gpu_buffer cb = upload_new(d, 1024, GPU_USE_CONSTANT);
    const gpu_buffer g = buf_new(d, 1024, GPU_USE_CONSTANT, GPU_MEM_GPU, GPU_STATE_COMMON);
    gpu_cmd* const c = gpu_cmd_begin(d, 0);
    gpu_cmd_set_layout(c, guest_layout(d));
    gpu_cmd_bind_cb(c, 0, gpu_fast_buffer_addr(d, vb));            /* 1: no GPU_USE_CONSTANT */
    gpu_cmd_bind_cb(c, 0, gpu_fast_buffer_addr(d, cb) + 4096);     /* 2: past its end */
    gpu_cmd_bind_raw(c, 4, gpu_fast_buffer_addr(d, cb));           /* 3: no GPU_USE_RAW */
    gpu_cmd_set_vb(c, 0, gpu_fast_buffer_addr(d, cb), 64, 16);     /* 4: no GPU_USE_VERTEX */
    gpu_cmd_bind_cb(c, 0, gpu_fast_buffer_addr(d, cb) + 768);      /* (fine) */
    CHECK(run(d, c));
    CHECK(gpu_buffer_map(d, g, 0, 0) == NULL);                     /* 5: no CPU pointer */
    expect(d, GPU_NULL_ERR_USE, 5);
    gpu_null_errors_reset(d);
    CHECK(gpu_null_errors(d, GPU_NULL_ERR_ALL) == 0);
    gpu_destroy(d);
}

/* Constant buffers off cb_align, and other alignments. */
static void neg_align(void)
{
    gpu_device* const d = dev_new(0);
    if (!d) return;
    const gpu_buffer cb = upload_new(d, 65536, GPU_USE_CONSTANT | GPU_USE_RAW);
    const gpu_texture t = rgba_new(d, GPU_USE_COPY_DST, GPU_STATE_COPY_DEST);
    const gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0);
    gpu_footprint fp = footprint(d, &td, 0, 0);
    gpu_cmd* const c = gpu_cmd_begin(d, 0);
    gpu_cmd_set_layout(c, guest_layout(d));
    gpu_cmd_bind_cb(c, 0, gpu_fast_buffer_addr(d, cb) + 16);       /* 1: cb_align is 256 */
    gpu_cmd_bind_raw(c, 4, gpu_fast_buffer_addr(d, cb) + 2);       /* 2: a raw buffer at 4 */
    fp.offset = 100;
    gpu_cmd_copy_buf_to_tex(c, t, 0, 0, 0, 0, cb, &fp);            /* 3: footprints at 512 */
    CHECK(run(d, c));
    expect(d, GPU_NULL_ERR_ALIGN, 3);
    gpu_destroy(d);
}

/* Copies outside what they name. */
static void neg_bounds(void)
{
    gpu_device* const d = dev_new(0);
    if (!d) return;
    const gpu_texture src = rgba_new(d, GPU_USE_COPY_SRC, GPU_STATE_COPY_SOURCE);
    const gpu_texture dst = rgba_new(d, GPU_USE_COPY_DST, GPU_STATE_COPY_DEST);
    const gpu_buffer up = upload_new(d, 1024, 0);
    const gpu_buffer r = readback_new(d, 65536, 0);
    const gpu_texture_desc td = tex_desc(16, 16, GPU_FORMAT_R8G8B8A8_UNORM, 0, 0);
    const gpu_footprint fp = footprint(d, &td, 0, 0);
    gpu_footprint small = fp;
    small.width = small.height = 8;
    const gpu_box box = {0, 0, 0, 32, 16, 1};
    const gpu_box half = {0, 0, 0, 8, 8, 1};
    gpu_cmd* const c = gpu_cmd_begin(d, 0);
    gpu_cmd_copy_buf_to_tex(c, dst, 0, 0, 0, 0, up, &fp);          /* 1: 3904 bytes from 1024 */
    gpu_cmd_copy_texture(c, dst, 0, 0, 0, 0, src, 0, &box);        /* 2: 32 wide from 16 */
    gpu_cmd_copy_tex_to_buf(c, r, &small, src, 0, NULL);           /* 3: no box: the whole 16x16 */
    gpu_cmd_copy_tex_to_buf(c, r, &small, src, 0, &half);          /* (fine) */
    gpu_cmd_copy_buffer(c, r, 65000, up, 0, 1024);                 /* 4: past the readback's end */
    gpu_cmd_copy_texture(c, dst, 0, 8, 8, 0, src, 0, &half);       /* (fine) */
    CHECK(run(d, c));
    expect(d, GPU_NULL_ERR_BOUNDS, 4);
    gpu_destroy(d);
}

/* Queries begun twice, ended unbegun, not reset, open across a barrier. */
static void neg_query(void)
{
    gpu_device* const d = dev_new(0);
    if (!d) return;
    const gpu_query_pool q = gpu_query_pool_create(d, GPU_QUERY_OCCLUSION, 4);
    const gpu_texture t = rgba_new(d, GPU_USE_SAMPLED, GPU_STATE_PIXEL_SHADER_RESOURCE);
    gpu_cmd* const c = gpu_cmd_begin(d, 0);
    gpu_cmd_query_reset(c, q, 0, 3);
    gpu_cmd_query_begin(c, q, 0);
    gpu_cmd_query_begin(c, q, 0);                                  /* 1: begun twice */
    gpu_cmd_query_end(c, q, 0);
    gpu_cmd_query_end(c, q, 1);                                    /* 2: ended unbegun */
    gpu_cmd_query_begin(c, q, 3);                                  /* 3: not reset */
    gpu_cmd_barrier(c, t, GPU_STATE_PIXEL_SHADER_RESOURCE, GPU_STATE_PIXEL_SHADER_RESOURCE);   /* 4: 3 is open */
    gpu_cmd_query_end(c, q, 3);
    CHECK(run(d, c));
    expect(d, GPU_NULL_ERR_QUERY, 4);
    gpu_destroy(d);
}

/* Table entries written while work that reads them is unfinished. */
static void neg_table(void)
{
    mini m;
    gpu_device* const d = mini_make(&m);
    if (!d) return;
    const gpu_swapchain none = {0};
    const gpu_texture other = rgba_new(d, GPU_USE_SAMPLED, GPU_STATE_PIXEL_SHADER_RESOURCE);
    const gpu_view_desc a = srv(other), b = srv(m.tex);
    gpu_cmd* const c = gpu_cmd_begin(d, 0);
    mini_draw(&m, c, 0);
    gpu_fast_table_write_tex(d, m.tp, 0, &a);                      /* 1: a recorded draw reads it */
    gpu_fast_table_write_tex(d, m.tp, 1, &a);                      /* (the slot reads entry 0 only) */
    CHECK(submit(d, c, GPU_SUBMIT_SIGNAL, none) == 1);
    gpu_fast_table_write_tex(d, m.tp, 0, &b);                      /* 2: fence 1 not seen to pass */
    gpu_fast_table_ref(d, m.tp, 5);                                /* 3: no table starts at 5 */
    CHECK(gpu_fence_wait(d, 1, 0) == 0);
    gpu_fast_table_write_tex(d, m.tp, 0, &a);                      /* (fine now) */
    expect(d, GPU_NULL_ERR_TABLE, 3);
    gpu_destroy(d);
}

/* Fences: one never reached, and work reused or read before its fence. */
static void neg_fence(void)
{
    gpu_device* const d = dev_new(0);
    if (!d) return;
    const gpu_swapchain none = {0};
    const gpu_buffer a = upload_new(d, 64, 0);
    const gpu_buffer r = readback_new(d, 64, 0);
    gpu_cmd* c = gpu_cmd_begin(d, 0);
    gpu_cmd_copy_buffer(c, r, 0, a, 0, 16);
    CHECK(submit(d, c, GPU_SUBMIT_SIGNAL, none) == 1);
    CHECK(gpu_buffer_map(d, r, 0, 16) != NULL);                    /* 1: read before fence 1 */
    gpu_buffer_unmap(d, r, 0, 0);
    c = gpu_cmd_begin(d, 0);                                       /* 2: slot 0 is fence 1's */
    submit(d, c, 0, none);
    CHECK(gpu_fence_wait(d, 99, 0) == 1);                          /* 3: never signalled */
    CHECK(gpu_fence_wait(d, 1, 0) == 0);
    CHECK(gpu_buffer_map(d, r, 0, 16) != NULL);                    /* (fine now) */
    expect(d, GPU_NULL_ERR_FENCE, 3);
    gpu_destroy(d);
}

/* Recorders used after their end, ended or submitted twice. */
static void neg_recorder(void)
{
    gpu_device* const d = dev_new(0);
    if (!d) return;
    gpu_cmd* const c = gpu_cmd_begin(d, 0);
    gpu_cmd_end(d, c);
    gpu_cmd_set_stencil_ref(c, 1);                                 /* 1: after its end */
    gpu_cmd* const c2 = gpu_cmd_begin(d, 1);
    CHECK(gpu_submit(d, (gpu_cmd* const*)&c2, 1, NULL) == 0);      /* 2: not ended */
    gpu_cmd_end(d, c2);
    gpu_cmd_end(d, c2);                                            /* 3: ended twice */
    gpu_submit(d, (gpu_cmd* const*)&c, 1, NULL);
    gpu_submit(d, (gpu_cmd* const*)&c, 1, NULL);                   /* 4: submitted twice */
    expect(d, GPU_NULL_ERR_RECORDER, 4);
    gpu_destroy(d);
}

/* Swap chain images used without acquire, presented unready. */
static void neg_swap(void)
{
    gpu_device* const d = dev_new(0);
    if (!d) return;
    gpu_swapchain_desc scd;
    memset(&scd, 0, sizeof scd);
    scd.width = 64;
    scd.height = 64;
    scd.format = GPU_FORMAT_R8G8B8A8_UNORM;
    scd.buffers = 2;
    const gpu_swapchain sc = gpu_swap_create(d, &scd);
    const gpu_swapchain none = {0};
    gpu_texture img;
    CHECK(gpu_swap_acquire(d, sc, &img) == 0);
    gpu_cmd* c = gpu_cmd_begin(d, 0);
    gpu_cmd_barrier(c, img, GPU_STATE_PRESENT, GPU_STATE_RENDER_TARGET);
    gpu_cmd_barrier(c, img, GPU_STATE_RENDER_TARGET, GPU_STATE_PRESENT);
    submit(d, c, GPU_SUBMIT_SIGNAL, none);                         /* 1: no GPU_SUBMIT_ACQUIRE_WAIT */
    CHECK(gpu_swap_present(d, sc, 1, 0) == 0);                     /* 2: no GPU_SUBMIT_PRESENT_SIGNAL */
    CHECK(gpu_fence_completed(d) == 1);
    c = gpu_cmd_begin(d, 0);
    gpu_cmd_barrier(c, img, GPU_STATE_PRESENT, GPU_STATE_RENDER_TARGET);   /* 3: image 0 is not acquired */
    submit(d, c, 0, none);
    gpu_swap_present(d, sc, 1, 0);                                 /* 4: nothing acquired */
    expect(d, GPU_NULL_ERR_SWAP, 4);
    gpu_swap_destroy(d, sc);
    gpu_destroy(d);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* in order with the backend's lines on stderr */
    printf("test_gpu_null: the GPU layer's null backend\n");
    test_frames();
    test_bytes();
    test_footprints();
    test_device();
    printf("  one case per check (each prints its first failure, as it should):\n");
    fflush(stdout);
    neg_handle();
    neg_freed();
    neg_barrier();
    neg_state();
    neg_draw();
    neg_use();
    neg_align();
    neg_bounds();
    neg_query();
    neg_table();
    neg_fence();
    neg_recorder();
    neg_swap();
    fflush(stderr);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

#else
#include <stdio.h>
int main(void)
{
    printf("test_gpu_null: built without PS3RECOMP_RSX_HAL\n");
    return 0;
}
#endif
