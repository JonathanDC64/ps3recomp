/* Our host vertex layout for the fallback path: position (xyz) + color (rgba)
 * + texcoord (uv), 36 bytes. (The real VP path feeds raw float4 attrib0.) */
typedef struct { float x, y, z; float r, g, b, a; float u, v; } BasicVertex;


/* Read one RSX vertex (by absolute vertex index) from guest memory into our
 * host layout. Position is attrib 0 (float3+), color is attrib 3 (ubyte4 or
 * float4); missing attribs default to opaque white. RSX stores each 32-bit
 * component big-endian, so every lane is byte-swapped. */
static void read_rsx_vertex(const rsx_state* state, u32 vindex, BasicVertex* out)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    out->x = out->y = out->z = 0.0f;
    out->r = out->g = out->b = out->a = 1.0f;
    out->u = out->v = 0.0f;
    if (!state || !vm_base) return;

    const rsx_vertex_attrib* pos = &state->vertex_attribs[0];
    if (pos->enabled && pos->type == 2 /* float */ && pos->size >= 2) {
        u8* src = vm_base + cellGcmResolveOffset(pos->offset + vindex * pos->stride);
        out->x = rsx_rd_bef(src);
        out->y = rsx_rd_bef(src + 4);
        if (pos->size >= 3) out->z = rsx_rd_bef(src + 8);

        /* dbgfont (and similar 2D overlays) store screen positions biased far
         * outside clip space; their vertex program folds them back to clip
         * space using transform constants we don't execute. When a position is
         * well outside NDC, recover the fractional part as a normalized [0,1]
         * screen coord and map it to NDC (screen Y-down -> clip Y-up). In this
         * layout the attribute is [posX, posY, U, V] (size 4), so the last two
         * components are the atlas texcoords, not depth.
         * TODO: execute the real vertex-program / RSX viewport transform so
         * this is not needed. */
        if (out->x > 2.0f || out->x < -2.0f || out->y > 2.0f || out->y < -2.0f) {
            float sx = out->x - floorf(out->x);
            float sy = out->y - floorf(out->y);
            out->x = sx * 2.0f - 1.0f;
            out->y = 1.0f - sy * 2.0f;
            out->z = 0.0f;
            if (pos->size >= 4) {
                out->u = rsx_rd_bef(src + 8);   /* U */
                out->v = rsx_rd_bef(src + 12);  /* V */
            }
        }
    }

    const rsx_vertex_attrib* col = &state->vertex_attribs[3];
    if (col->enabled && col->size >= 3) {
        u8* src = vm_base + cellGcmResolveOffset(col->offset + vindex * col->stride);
        if (col->type == 4 /* ubyte */) {
            out->r = src[0] / 255.0f;
            out->g = src[1] / 255.0f;
            out->b = src[2] / 255.0f;
            out->a = (col->size >= 4) ? src[3] / 255.0f : 1.0f;
        } else if (col->type == 2 /* float */) {
            u32 fr, fg, fb, fa;
            memcpy(&fr, src,     4); fr = ((fr>>24)&0xFF)|((fr>>8)&0xFF00)|((fr<<8)&0xFF0000)|((fr<<24)&0xFF000000);
            memcpy(&fg, src + 4, 4); fg = ((fg>>24)&0xFF)|((fg>>8)&0xFF00)|((fg<<8)&0xFF0000)|((fg<<24)&0xFF000000);
            memcpy(&fb, src + 8, 4); fb = ((fb>>24)&0xFF)|((fb>>8)&0xFF00)|((fb<<8)&0xFF0000)|((fb<<24)&0xFF000000);
            memcpy(&out->r, &fr, 4); memcpy(&out->g, &fg, 4); memcpy(&out->b, &fb, 4);
            if (col->size >= 4) {
                memcpy(&fa, src + 12, 4); fa = ((fa>>24)&0xFF)|((fa>>8)&0xFF00)|((fa<<8)&0xFF0000)|((fa<<24)&0xFF000000);
                memcpy(&out->a, &fa, 4);
            }
        }
    }
}

/* Upload `count` sequential vertices [first, first+count). Returns the count
 * actually written (clamped to the remaining per-frame buffer). */
static u32 upload_vertices_from_rsx(u32 first, u32 count)
{
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (count > max_verts) count = max_verts;
    const rsx_state* state = s_d3d.current_rsx_state;
    for (u32 i = 0; i < count; i++)
        read_rsx_vertex(state, first + i, &verts[i]);
    s_d3d.vb_offset += count * sizeof(BasicVertex);
    return count;
}

/* Upload RSX QUADS (prim 8) as a triangle list: each 4-vertex quad v0..v3
 * (perimeter winding) splits into triangles (v0,v1,v2) and (v0,v2,v3).
 * D3D12 has no quad topology, so this expansion is how quads render at all.
 * Returns the number of triangle-list vertices emitted (6 per quad). */
static u32 upload_quads_from_rsx(u32 first, u32 count)
{
    const rsx_state* state = s_d3d.current_rsx_state;
    u32 quads = count / 4;
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (quads * 6 > max_verts) quads = max_verts / 6;
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        BasicVertex c[4];
        for (u32 k = 0; k < 4; k++)
            read_rsx_vertex(state, first + q * 4 + k, &c[k]);
        verts[o++] = c[0]; verts[o++] = c[1]; verts[o++] = c[2];
        verts[o++] = c[0]; verts[o++] = c[2]; verts[o++] = c[3];
    }
    s_d3d.vb_offset += o * sizeof(BasicVertex);
    return o;
}

/* Upload RSX QUADS as raw float4 attrib0 (byte-swapped) into vp_vb, expanded
 * to a triangle list (6 verts/quad). The decompiled vertex shader does the
 * transform. Returns emitted vertex count. */
/* Generic VP vertex: all 16 RSX vertex attributes, each converted to a float4
 * slot -- 256 bytes/vertex, input layout ATTRi @ i*16. Apps place attributes at
 * arbitrary indices (tiny3d: pos=a0 colour=a3 tex=a8; SDK gcm samples: pos=a0
 * colour=a1 tex=a2; dbgfont: a0..a2), so a hardcoded pos+colour pair can't
 * cover them: every enabled attrib is fetched from guest memory and converted
 * by its RSX type; disabled slots read (0,0,0,1). */
typedef struct { float v[4]; } VPSlot;
#define VP_VERT_STRIDE (16 * sizeof(VPSlot))   /* 256 */


static void rc_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n);

/* Fill all 16 attribute slots for one vertex. The per-attribute work lives in
 * rsx_vertex_fetch.c so the Metal and null backends read guest vertices the
 * same way this one does, rather than each porting the logic again. */
/* VP_FETCH_FAST (live, default 1): each VP upload starts with
 * vp_plan_begin, which builds the draw's fetch plan (rsx_fetch_plan_init) and
 * opens a new generation of the vertex cache. 0 = the per-attribute
 * rsx_fetch_attrib path and no cache, for A/B. */
static rsx_fetch_plan s_vp_plan;
static float s_vp_tmp[16][4];   /* read_vp_vertex's vertex, see there */
static int s_vp_fast = 1;
#define VP_VC_N 256u
static u32 s_vc_key[VP_VC_N], s_vc_tag[VP_VC_N], s_vc_gen;
static VPSlot s_vc_v[VP_VC_N][16];
/* For indexed uploads: where this draw's vertices went in vp_vb, keyed like
 * the vertex cache (and in the same generation). */
static u32 s_vx_key[VP_VC_N], s_vx_tag[VP_VC_N], s_vx_out[VP_VC_N];
/* Vertices the draw being extended already has (d3d12_draw_indexed), 0 for
 * a new draw: the indices a merged range writes count from there, and the
 * vertex cache carries over, so the vertices two ranges share are written
 * once. */
static u32 s_strip_base;

/* RSX_LATE_CHECK, vertices: these are read when the draw is recorded, so the
 * question is the other way round from textures -- did the title write a
 * vertex array after issuing the draw that reads it? Each VP upload notes the
 * guest bytes of every enabled array over the vertex range it fetched, and
 * late_check hashes them again when the batch renders. A replay reads its
 * snapshot from the end of the frame, so a late write shows live only. */
#define LATE_VTX_MAX 16384
typedef struct { const u8* p; u32 len, sum, fp, draw, slot, off; } LateVtx;
static LateVtx* s_late_vtx;
static u32 s_late_vtx_n, s_lv_lo, s_lv_hi, s_lv_draw, s_lv_klo, s_lv_khi, s_lv_idma, s_lv_ioff;
static int s_lv_open;
static rsx_fetch_plan s_lv_plan;
static u32 lv_elem_bytes(u32 type, u32 n)
{
    switch (type) { case 4: case 7: return n; case 1: case 3: case 5: return 2 * n;
                    default: return 4 * n; }
}
static void lv_close(void)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int, u32);
    if (!s_lv_open) return;
    s_lv_open = 0;
    if (s_lv_lo > s_lv_hi || !vm_base) return;
    for (u32 k = 0; k < s_lv_plan.n_on && s_late_vtx_n < LATE_VTX_MAX; k++) {
        const rsx_fetch_slot* sl = &s_lv_plan.on[k];
        u32 lo = s_lv_lo, hi = s_lv_hi;
        if (sl->freq) { if (sl->modulo) { lo = 0; hi = sl->freq - 1; }
                        else { lo /= sl->freq; hi /= sl->freq; } }
        const u32 off = sl->off + lo * sl->stride;
        u64 len = (u64)(hi - lo) * sl->stride + lv_elem_bytes(sl->type, sl->n);
        if (len > (16u << 20)) len = 16u << 20;
        const u32 ga = sl->main ? cellGcmResolveLocated(0, off) : sl->local_addr + off;
        if (ga == 0xFFFFFFFFu) continue;
        LateVtx* e = &s_late_vtx[s_late_vtx_n++];
        e->p = vm_base + ga; e->len = (u32)len; e->off = sl->off;
        e->sum = tex_csum_full(e->p, e->len);
        e->fp = s_lv_plan.st ? s_lv_plan.st->shader_program : 0;
        e->draw = s_lv_draw; e->slot = sl->idx;
    }
    /* The index range, too. */
    if (s_lv_klo <= s_lv_khi && s_late_vtx_n < LATE_VTX_MAX) {
        const u32 isz = ((s_lv_idma >> 4) & 0xF) == 1 ? 2 : 4;
        const u32 base = cellGcmResolveLocated((s_lv_idma & 0xF) == 0, s_lv_ioff);
        if (base != 0xFFFFFFFFu) {
            LateVtx* e = &s_late_vtx[s_late_vtx_n++];
            e->p = vm_base + base + (u64)s_lv_klo * isz;
            e->len = (s_lv_khi - s_lv_klo + 1) * isz; e->off = s_lv_ioff;
            e->sum = tex_csum_full(e->p, e->len);
            e->fp = s_lv_plan.st ? s_lv_plan.st->shader_program : 0;
            e->draw = s_lv_draw; e->slot = 16;
        }
    }
}
static void lv_open(const rsx_state* state)
{
    lv_close();
    if (!late_on()) return;
    if (!s_late_vtx && !(s_late_vtx = calloc(LATE_VTX_MAX, sizeof *s_late_vtx))) return;
    rsx_fetch_plan_init(state, &s_lv_plan);
    s_lv_lo = 0xFFFFFFFFu; s_lv_hi = 0; s_lv_open = 1;
    s_lv_draw = s_d3d.draw_count;
    s_lv_klo = 0xFFFFFFFFu; s_lv_khi = 0;
    s_lv_idma = state->index_array_dma; s_lv_ioff = state->index_array_offset;
}
static void late_vtx_check(void)
{
    lv_close();
    if (!s_late_vtx || !s_late_vtx_n) return;
    static int said = 0;
    u32 nchg = 0;
    for (u32 i = 0; i < s_late_vtx_n; i++) {
        const LateVtx* e = &s_late_vtx[i];
        if (tex_csum_full(e->p, e->len) == e->sum) continue;
        nchg++;
        if (said < 60) { said++;
            fprintf(stderr, "[LATE] frame %llu draw %u fp 0x%X %s%u array 0x%X (%u bytes) "
                    "written after the draw read it%c", (unsigned long long)s_d3d.frame_count,
                    e->draw, e->fp, e->slot == 16 ? "index" : "a", e->slot == 16 ? 0 : e->slot,
                    e->off, e->len, 10); }
    }
    if (nchg) {
        static int n = 0;
        if (n < 200 && ++n) fprintf(stderr, "[LATE] frame %llu: %u of %u vertex reads changed "
                               "between draw and render%c", (unsigned long long)s_d3d.frame_count,
                               nchg, s_late_vtx_n, 10);
    }
    s_late_vtx_n = 0;
}

static void vp_plan_begin(const rsx_state* state)
{
    lv_open(state);
    static int fast = -1;
    PS3_KNOB(fast, ps3_env_int("VP_FETCH_FAST", 1));
    s_vp_fast = fast != 0;
    if (s_vp_fast) {
        rsx_fetch_plan_init(state, &s_vp_plan);
        memcpy(s_vp_tmp, s_vp_plan.konst, sizeof s_vp_tmp);
    }
    if (s_strip_base) return;   /* continuing the last draw: its vertices stay */
    if (++s_vc_gen == 0) { memset(s_vc_tag, 0, sizeof s_vc_tag);
                           memset(s_vx_tag, 0, sizeof s_vx_tag); s_vc_gen = 1; }
}

static void read_vp_vertex(const rsx_state* state, u32 vi, VPSlot* out16)
{
    if (s_lv_open) { if (vi < s_lv_lo) s_lv_lo = vi; if (vi > s_lv_hi) s_lv_hi = vi; }
    if (s_vp_fast && s_vp_plan.st == state) {
        /* Built in s_vp_tmp, then one copy: out16 is usually the upload
         * heap, which is write-combined. s_vp_tmp holds the draw's constant
         * attributes from vp_plan_begin on, and only the components the
         * arrays supply change from vertex to vertex. */
        s_vs_fetch++; s_vs_attr += s_vp_plan.n_on;
        rsx_fetch_vertex_arrays(&s_vp_plan, vi, s_vp_tmp);
        memcpy(out16, s_vp_tmp, sizeof s_vp_tmp);
    } else {
        for (int i = 0; i < 16; i++)
            rsx_fetch_attrib(state, i, vi, out16[i].v);
    }
    { const rsx_vertex_attrib* a = &state->vertex_attribs[0];
      u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
      rc_vtx_pos_dbg(state, out16[0].v, n); }
}

/* For uploads that emit the same vertex more than once (strips and fans
 * expanded to lists, indexed draws): a direct-mapped cache of this draw's
 * fetched vertices. A guest vertex cannot change within one draw, so a hit is
 * exactly what a fetch would return. */
static void read_vp_vertex_c(const rsx_state* state, u32 vi, VPSlot* out16)
{
    if (!s_vp_fast) { read_vp_vertex(state, vi, out16); return; }
    const u32 h = (vi ^ (vi >> 8)) & (VP_VC_N - 1);
    if (s_vc_tag[h] != s_vc_gen || s_vc_key[h] != vi) {
        read_vp_vertex(state, vi, s_vc_v[h]);
        s_vc_key[h] = vi; s_vc_tag[h] = s_vc_gen;
    }
    memcpy(out16, s_vc_v[h], VP_VERT_STRIDE);
}

/* VP_FETCH_THREADS=<n> (read once; default 1; 0 = off): convert the vertices
 * of indexed strip uploads on worker threads. The FIFO thread only walks the
 * indices, numbers the draw's vertices and queues them (vpj_queue); the
 * workers fetch and convert them straight into vp_vb. In a Demon's Souls level
 * frame the conversion was about a third of the FIFO thread, and HighGraphics
 * waits on that thread's labels before each flip.
 *
 * One worker, whatever the machine: the conversion only has to overlap the
 * FIFO walk, and one keeps up. More cost the frame instead -- Palace, 60 fps
 * mode with VBLANK_MHZ=120000: 1 worker 63-64 fps, 2 about 61, 4 (the old
 * default of a quarter of the logical CPUs) 57-59, with each of the four at
 * ~55% of a core, half of it in the kernel going to sleep and waking. Waking
 * one worker per publish instead of all of them did not change that. The
 * threads the frame waits on (HighGraphics, the image-2 SPU thread, the FIFO
 * walker) run slower beside them. 0 workers: ~46 fps at a 60 cap.
 *
 * A queued vertex is read from guest memory later than the FIFO reached the
 * draw, so everything the title can observe of the RSX's progress waits for
 * the queue first (vpj_sync, g_rsx_sync_hook): labels, semaphores, reports,
 * the reference, the 2D engine's writes, `get`, and the frame's render.
 *
 * The GPU fetch path's span copies (vf_copy) go to the same queue, as jobs of
 * no vertices, under the same rule. */
typedef struct {
    u32 n;                   /* vertices; 0: a span copy, clen bytes from guest csrc to cdst */
    u32 clen, csrc;
    u8* cdst;
    const u32* vis;          /* guest vertex indices, n of them */
    VPSlot* out;             /* n vertices of VP_VERT_STRIDE */
    rsx_fetch_plan plan;     /* copied: the FIFO moves on before it is used */
} VPJob;
#define VPJ_JOBS 8192u
#define VPJ_VIS  (1u << 20)
static VPJob* s_vpj;
static u32* s_vpj_vis;
static u32 s_vpj_vis_n;
/* The shared counters, each on its own cache line: the workers claim from
 * `next` and count into `done` while the FIFO thread publishes into `pub`,
 * and on one line every publish waited for the workers' writes. */
typedef struct { volatile LONG v; char pad[60]; } VPJCounter;
static _Alignas(64) VPJCounter s_vpj_ctr[4];
#define s_vpj_pub      (s_vpj_ctr[0].v)   /* jobs the workers may take */
#define s_vpj_next     (s_vpj_ctr[1].v)
#define s_vpj_done     (s_vpj_ctr[2].v)
#define s_vpj_sleepers (s_vpj_ctr[3].v)
/* FIFO thread only: jobs queued (published or not), and the vertices queued
 * since the last publish. Jobs are published in batches of VPJ_BATCH
 * vertices (read once, default 1024): one publish per draw was a locked
 * increment of a line the workers spin on, ~6% of the FIFO thread. */
static LONG s_vpj_q;
static u32 s_vpj_qv;
static SRWLOCK s_vpj_lk = SRWLOCK_INIT;
static CONDITION_VARIABLE s_vpj_cv = CONDITION_VARIABLE_INIT;

static void vpj_run(LONG j)
{
    const VPJob* J = &s_vpj[j];
    if (!J->n) {
        extern uint8_t* vm_base;
        memcpy(J->cdst, vm_base + J->csrc, J->clen);
        return;
    }
    float tmp[16][4];
    memcpy(tmp, J->plan.konst, sizeof tmp);
    for (u32 i = 0; i < J->n; i++) {
        rsx_fetch_vertex_arrays(&J->plan, J->vis[i], tmp);
        memcpy(&J->out[i * 16], tmp, sizeof tmp);
    }
}
static LONG vpj_claim(void)
{
    for (;;) {
        const LONG n = s_vpj_next;
        if (n >= s_vpj_pub) return -1;
        if (InterlockedCompareExchange(&s_vpj_next, n + 1, n) == n) return n;
    }
}
static DWORD WINAPI vpj_worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        LONG j = vpj_claim();
        if (j >= 0) { vpj_run(j); InterlockedIncrement(&s_vpj_done); continue; }
        for (int k = 0; k < 2000 && s_vpj_next >= s_vpj_pub; k++) YieldProcessor();
        if (s_vpj_next < s_vpj_pub) continue;
        AcquireSRWLockExclusive(&s_vpj_lk);
        InterlockedIncrement(&s_vpj_sleepers);   /* before the check: see vpj_publish */
        while (s_vpj_next >= s_vpj_pub)
            SleepConditionVariableSRW(&s_vpj_cv, &s_vpj_lk, INFINITE, 0);
        InterlockedDecrement(&s_vpj_sleepers);
        ReleaseSRWLockExclusive(&s_vpj_lk);
    }
}
/* Let the workers take everything queued so far. The exchange is also the
 * full barrier the sleeper check needs (see vpj_worker). */
static void vpj_publish(void)
{
    s_vpj_qv = 0;
    if (s_vpj_q == s_vpj_pub) return;
    InterlockedExchange(&s_vpj_pub, s_vpj_q);
    /* A worker counts itself a sleeper before it checks for work, so either
     * it sees these jobs or this sees it. */
    if (s_vpj_sleepers) {
        AcquireSRWLockExclusive(&s_vpj_lk);
        WakeAllConditionVariable(&s_vpj_cv);
        ReleaseSRWLockExclusive(&s_vpj_lk);
    }
}
/* Wait for every queued vertex, converting what no worker has taken yet. */
static void vpj_sync(void)
{
    vpj_publish();
    if (!s_vpj_pub) return;
    LONG j;
    while ((j = vpj_claim()) >= 0) { vpj_run(j); InterlockedIncrement(&s_vpj_done); }
    while (s_vpj_done < s_vpj_pub) YieldProcessor();
    InterlockedExchange(&s_vpj_pub, 0);   /* first: a stale claim then sees nothing */
    InterlockedExchange(&s_vpj_next, 0);
    InterlockedExchange(&s_vpj_done, 0);
    s_vpj_q = 0;
    s_vpj_vis_n = 0;
}
static void vpj_sync_hook(void) { vpj_sync(); }
static int vpj_threads(void)
{
    static int n = -1;
    if (n >= 0) return n;
    n = ps3_env_int("VP_FETCH_THREADS", 1);
    if (n > 16) n = 16;
    if (n > 0) {
        s_vpj = (VPJob*)malloc(sizeof(VPJob) * VPJ_JOBS);
        s_vpj_vis = (u32*)malloc(sizeof(u32) * VPJ_VIS);
        if (!s_vpj || !s_vpj_vis) { n = 0; return n; }
        for (int i = 0; i < n; i++) {
            HANDLE h = CreateThread(NULL, 0, vpj_worker, NULL, 0, NULL);
            if (h) CloseHandle(h);
        }
        extern void (*g_rsx_sync_hook)(void);
        g_rsx_sync_hook = vpj_sync_hook;
        fprintf(stderr, "[VPJ] %d vertex fetch threads%c", n, 10);
    }
    return n;
}
/* Can this draw's vertices go to the workers? Every array type must be one
 * rsx_fetch_vertex_arrays converts itself (its fallback reads the live
 * state), and no debug knob may want to see the vertices as they upload. */
static int vpj_usable(void)
{
    if (vpj_threads() <= 0 || s_lv_open) return 0;
    static int dbg = -1;
    PS3_KNOB(dbg, (getenv("RSX_DBG_VTX") || ps3_env_int("VTX_POS", 0)) ? 1 : 0);
    if (dbg) return 0;
    for (u32 k = 0; k < s_vp_plan.n_on; k++) {
        const u32 t = s_vp_plan.on[k].type;
        if (t < 1 || t > 7 || t == 6) return 0;
    }
    return 1;
}
/* Room for a draw of up to `nv` vertices, syncing to make it. */
static int vpj_room(u32 nv)
{
    if (nv > VPJ_VIS) return 0;
    if (s_vpj_q >= (LONG)VPJ_JOBS || s_vpj_vis_n + nv > VPJ_VIS) vpj_sync();
    return 1;
}
static u32 vpj_batch(void)
{
    static int batch = -1;
    if (batch < 0) batch = ps3_env_int("VPJ_BATCH", 1024);
    return (u32)batch;
}
static void vpj_queue(const u32* vis, u32 n, VPSlot* out)
{
    VPJob* J = &s_vpj[s_vpj_q];
    memcpy(&J->plan, &s_vp_plan, sizeof J->plan);
    J->vis = vis; J->n = n; J->out = out;
    s_vpj_vis_n += n;
    s_vpj_q++;
    s_vpj_qv += n;
    if (s_vpj_qv >= vpj_batch()) vpj_publish();
}
/* The GPU fetch path's span copies, about 29 MB a frame at Demon's Souls'
 * deep Palace and a sixth of the FIFO thread, as jobs for the vertex worker
 * (VF_COPY_WORKER, default 1; 0 copies on the FIFO thread, as before). A copy
 * that continues the last queued one, as a draw appending to its descriptor's
 * span does, extends that job. Each 256 bytes count as a vertex for
 * VPJ_BATCH. */
static void vf_copy(u8* dst, u32 src, u32 len)
{
    extern uint8_t* vm_base;
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("VF_COPY_WORKER", 1));
    if (!on || vpj_threads() <= 0) { memcpy(dst, vm_base + src, len); return; }
    /* Not published yet, so no worker has taken it. */
    VPJob* P = s_vpj_q > s_vpj_pub ? &s_vpj[s_vpj_q - 1] : NULL;
    if (P && !P->n && P->cdst + P->clen == dst && P->csrc + P->clen == src) P->clen += len;
    else {
        if (s_vpj_q >= (LONG)VPJ_JOBS) vpj_sync();
        VPJob* J = &s_vpj[s_vpj_q++];
        J->n = 0; J->clen = len; J->csrc = src; J->cdst = dst;
    }
    s_vpj_qv += len >> 8;
    if (s_vpj_qv >= vpj_batch()) vpj_publish();
}

/* VTX_POS=<N>: print the first fetched position of the first N draws. A guest
 * vertex array that resolves to the wrong memory reads as garbage/denormals, and
 * that is indistinguishable from "the shader is wrong" without seeing the input. */
static void rc_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n)
{
    static int cap = -1, seen = 0;
    PS3_KNOB(cap, ps3_env_int("VTX_POS", 0));
    if (!cap || seen >= cap) return;
    seen++;
    const rsx_vertex_attrib* a = &state->vertex_attribs[0];
    fprintf(stderr, "[VTXPOS] a0 off=0x%X stride=%u size=%u type=%u -> (%g, %g, %g, %g)\n",
            a->offset, a->stride, a->size, a->type,
            v[0], v[1], v[2], v[3]);   /* all four: masking by `size` hid whether w survived */
}

static void vp_attrs_dbg(const rsx_state* state)
{
    static int en = -1;   /* once: this runs for every draw */
    PS3_KNOB(en, getenv("VP_ATTRS") ? 1 : 0);
    if (!en) return;
    /* VP_ATTRS_FP=<hex shader_program>: dump only the draws that use one
     * fragment program. Without it the first six draws are whatever the frame
     * happens to start with, which is never the mesh being investigated. */
    { static const char* e = (const char*)1; static u32 want = 0;
      if (e == (const char*)1) { e = getenv("VP_ATTRS_FP");
          want = e ? (u32)strtoul(e, NULL, 16) : 0; }   /* hex, like FP_PICK */
      if (want && (!state || state->shader_program != want)) return;
      /* With a program picked, also write the vertex program its first draw
       * runs (vp_pick.hlsl): the attribute list says what is fed, this says
       * what is read and how the outputs are formed. */
      static int wrote = 0;
      if (want && !wrote && state->vp_ucode_bytes >= 16) {
          extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
          static char vh[262144];
          u32 vs0 = state->transform_program_start * 16u;
          if (vs0 >= state->vp_ucode_bytes) vs0 = 0;
          const int ni = rsx_vp_decompile(state->vp_ucode + vs0, state->vp_ucode_bytes - vs0,
                                          vh, sizeof vh);
          FILE* f = ni > 0 ? fopen("vp_pick.hlsl", "w") : NULL;
          if (f) { fprintf(f, "/* fp=0x%X start=%u */\n%s", want, vs0 / 16u, vh); fclose(f); }
          wrote = 1;
      } }
    /* VP_CONSTS=<i,j,...>: print those vertex constants whenever their values
     * change (capped). A VS that derives colour purely from constants renders
     * black if the guest's uploads never landed, which the attrib dump can't show. */
    { const char* e = getenv("VP_CONSTS");
      if (e) {
          static float last[16][4]; static int nl = -1, nprint = 0;
          int idx[16]; int n = 0; const char* p = e;
          while (*p && n < 16) { idx[n++] = (int)strtol(p, (char**)&p, 0); if (*p == ',') p++; else break; }
          int changed = (nl != n);
          for (int k = 0; k < n && !changed; k++)
              if (memcmp(last[k], state->vertex_constants[idx[k] & 511], 16)) changed = 1;
          if (changed && nprint < 60) {
              nprint++; nl = n;
              for (int k = 0; k < n; k++) {
                  const float* c = state->vertex_constants[idx[k] & 511];
                  memcpy(last[k], c, 16);
                  fprintf(stderr, "[VPCONST] fp=0x%X c[%d] = (%g, %g, %g, %g)%c",
                          state->shader_program, idx[k], c[0], c[1], c[2], c[3], 10);
              }
          }
      } }
    /* Dedupe by the set of enabled attributes rather than printing the first
     * few draws: one fragment program can be used by meshes with different
     * vertex layouts, and a mesh missing an attribute takes the constant
     * register instead -- which is exactly the case worth seeing. */
    { u32 mask = 0;
      for (int i = 0; i < 16; i++) if (state->vertex_attribs[i].enabled) mask |= 1u << i;
      static u32 seen[16]; static int ns = 0;
      for (int i = 0; i < ns; i++) if (seen[i] == mask) return;
      if (ns >= 16) return;
      seen[ns++] = mask; }
    fprintf(stderr, "[VPATTR] --- fp=0x%X ---%c", state->shader_program, 10);
    fprintf(stderr, "[VPATTR] divider_op=0x%08X\n", state->frequency_divider_op);
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &state->vertex_attribs[i];
        if (a->enabled) fprintf(stderr, "[VPATTR] a%d off=0x%X stride=%u size=%u type=%u freq=%u fmt=0x%08X\n",
                                i, a->offset, a->stride, a->size, a->type, a->frequency, a->format);
    }
    /* VP_ATTRS_DUMP=<attr>:<n>: read n entries of that attribute straight out of
     * guest memory. Answers "did the guest write these values, or did our fetch
     * mangle them" without inferring it from the rendered image. */
    { const char* e = getenv("VP_ATTRS_DUMP"); if (!e) return;
      int ai = 0, cnt = 8; sscanf(e, "%d:%d", &ai, &cnt);
      const rsx_vertex_attrib* a = &state->vertex_attribs[ai];
      if (!a->enabled || !a->stride) return;
      extern uint8_t* vm_base;
      extern u32 cellGcmResolveLocated(int, u32);
      u32 nbad = 0, nzero = 0;
      for (int v = 0; v < cnt; v++) {
          u32 aoff = (a->offset & 0x7FFFFFFFu) + (u32)v * a->stride;
          const u8* q = vm_base + ((a->offset & 0x80000000u)
                        ? cellGcmResolveLocated(0, aoff) : cellGcmResolveLocated(1, aoff));
          /* Also read the same offset through the OTHER context-DMA. An array
           * the guest put in main memory but whose offset we resolve as LOCAL
           * reads as untouched VRAM, i.e. all zeros -- indistinguishable from
           * "the producer never ran". */
          if (v == 0) {
              u32 la = cellGcmResolveLocated(1, aoff), ma = cellGcmResolveLocated(0, aoff);
              const u8* lp = vm_base + la; const u8* mp = vm_base + ma;
              const u8* rp = vm_base + (a->offset & 0x7FFFFFFFu);   /* raw, unresolved */
              fprintf(stderr, "[VPDUMP] a%d off=0x%X  LOCAL@0x%X=(%g,%g,%g)  MAIN@0x%X=(%g,%g,%g)"
                              "  RAW@0x%X=(%g,%g,%g)%c",
                      ai, aoff, la, rsx_rd_bef(lp), rsx_rd_bef(lp+4), rsx_rd_bef(lp+8),
                      ma, rsx_rd_bef(mp), rsx_rd_bef(mp+4), rsx_rd_bef(mp+8),
                      (a->offset & 0x7FFFFFFFu), rsx_rd_bef(rp), rsx_rd_bef(rp+4), rsx_rd_bef(rp+8), 10);
          }
          u32 nc = a->size ? a->size : 3; if (nc > 3) nc = 3;
          float f[3] = {0,0,0};
          for (u32 k = 0; k < nc; k++) f[k] = rsx_rd_bef(q + k * 4);
          if (v < 12)
              fprintf(stderr, "[VPDUMP] a%d[%d] = (%g, %g, %g)%c", ai, v, f[0], f[1], f[2], 10);
          int allz = 1; for (u32 k = 0; k < nc; k++) if (f[k] != 0.f) allz = 0;
          if (allz) nzero++;
          else if (f[2] < -0.9f && f[0] > -0.1f && f[0] < 0.1f) nbad++;
      }
      fprintf(stderr, "[VPDUMP] a%d over %d verts: %u zero, %u near (0,0,-1)%c",
              ai, cnt, nzero, nbad, 10);
      /* How many entries differ from the first? An all-identical position array
       * is a degenerate mesh: it rasterizes nothing, which looks the same as
       * "the draw never happened". */
      { float f0[3]; u32 o0 = (a->offset & 0x7FFFFFFFu);
        const u8* q0 = vm_base + ((a->offset & 0x80000000u)
                       ? cellGcmResolveLocated(0, o0) : cellGcmResolveLocated(1, o0));
        for (int k = 0; k < 3; k++) f0[k] = rsx_rd_bef(q0 + k * 4);
        u32 diff = 0;
        for (int v = 1; v < cnt; v++) {
            u32 ao = o0 + (u32)v * a->stride;
            const u8* q = vm_base + ((a->offset & 0x80000000u)
                          ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
            for (int k = 0; k < 3; k++)
                if (rsx_rd_bef(q + k * 4) != f0[k]) { diff++; break; }
        }
        fprintf(stderr, "[VPDUMP] a%d: %u/%d entries differ from entry 0 (%g,%g,%g)%c",
                ai, diff, cnt, f0[0], f0[1], f0[2], 10);
        /* Extent: an isosurface with little fluid is a speck, and a speck that
         * rasterizes nothing is correct, not a bug. */
        { float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (f < lo[k]) lo[k] = f; if (f > hi[k]) hi[k] = f; } }
          u32 nnan = 0, nfin = 0;
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              int bad = 0;
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (!(f == f) || f > 1e30f || f < -1e30f) bad = 1; }
              if (bad) nnan++; else nfin++;
          }
          fprintf(stderr, "[VPDUMP] a%d extent x[%g..%g] y[%g..%g] z[%g..%g]"
                          "  finite=%u nan/inf=%u%c",
                  ai, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], nfin, nnan, 10); } } }
}

static void dbg_vtx_dump(const rsx_state* state, const VPSlot* out, u32 first, u32 nverts)
{
    /* RSX_DBG_VTX=<hex tex0 raw> (live, re-armed by every setenv): for the
     * next 8 draws that bind that texture, print every enabled vertex
     * attribute of the first 4 vertices as the VP reads them, and the
     * non-zero constant range. RSX_DBG_VTX=fp:<hex>[:n] matches the fragment
     * program instead and prints vertex 0 of the next n draws (default 80):
     * one line per quad of a HUD. fp:<hex>:<n>:<v> prints v vertices of each.
     * For "drawn but invisible" and "drawn in the wrong place" questions. */
    { static unsigned gen; static u32 want = 0, by_fp = 0, fp_verts = 1; static int n = 0, lim = 8;
      if (gen != ps3_env_gen) { const char* dv = getenv("RSX_DBG_VTX");
          gen = ps3_env_gen; n = 0; by_fp = dv && strncmp(dv, "fp:", 3) == 0;
          if (by_fp) { char* e; want = (u32)strtoul(dv + 3, &e, 16);
                       lim = *e == ':' ? (int)strtol(e + 1, &e, 10) : 80;
                       fp_verts = *e == ':' ? (u32)atoi(e + 1) : 1; }
          else { want = dv ? (u32)strtoul(dv, NULL, 16) : 0; lim = 8; } }
      if (want && (by_fp ? state->shader_program == want : s_d3d.cur_texs[0].raw == want) && n < lim) { n++;
        if (by_fp && nverts > fp_verts) nverts = fp_verts;
        if (by_fp) fprintf(stderr, "[DBGVTX] #%d t0=0x%08X", n, s_d3d.cur_texs[0].raw);
        fprintf(stderr, "[DBGVTX] fp=0x%08X first=%u verts=%u", state->shader_program, first, nverts);
        { int lo = -1, hi = -1, nz = 0;
          for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
              const float* m = state->vertex_constants[r];
              if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
          }
          fprintf(stderr, " | constants: %d non-zero, slots %d..%d%c", nz, lo, hi, 10); }
        for (u32 k = 0; k < nverts && (by_fp || k < 4); k++) {
            fprintf(stderr, "  v%u", k);
            for (int a = 0; a < 16; a++)
                if (state->vertex_attribs[a].enabled)
                    fprintf(stderr, " a%d=(%.3f,%.3f,%.3f,%.3f)", a, out[k*16+a].v[0],
                            out[k*16+a].v[1], out[k*16+a].v[2], out[k*16+a].v[3]);
            fprintf(stderr, "%c", 10);
        }
      } }
}

static u32 upload_quads_vp(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    vp_plan_begin(state);
    u32 quads = count / 4;
    u32 maxv = (s_vp_cap * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    s_vp_want += (u64)quads * 6;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * s_vp_cap * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex(state, first + q*4 + k, c[k]);
        /* quad -> two triangles (perimeter winding) */
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (knob_vtx_dump()) { static int _n=0; if (_n < 4 && ++_n) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) { for (u32 k = 0; k < 4; k++)
                fprintf(f, "q%02d v%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", _n, k,
                    c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                    c[k][8].v[0],c[k][8].v[1]);
              fclose(f); } } }
    }
    dbg_vtx_dump(state, out, first, o);
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Straight triangle-list upload through the VP path (gcm/cube's cube draws
 * TRIANGLES, prim 5 -- no expansion needed). */
static u32 upload_tris_vp(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++; s_vp_want += count;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    vp_plan_begin(state);
    u32 maxv = (s_vp_cap * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && _n < 12 && ++_n)
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * s_vp_cap * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      for (u32 k = 0; k < count; k++)
          read_vp_vertex(state, first + k, &out[k*16]);
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; } }
    /* IDXDBG=<hex shader_program>: vertex range for that program's draws. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if (_n < 12 && ++_n)
          fprintf(stderr, "[IDXDBG] tris first=%u count=%u a0off=0x%X stride=%u%c",
                  first, count, state->vertex_attribs[0].offset,
                  state->vertex_attribs[0].stride, 10); } }
    if (knob_vtx_dump() && state->surface_color_offset[0] == 0xCC0000) {
        static int _n=0; if (_n < 1 && ++_n) {
        FILE* f = fopen("vtx_dump.txt", "w");
        if (f) { fprintf(f, "G-buffer draw surf0=0x%X count=%u first=%u\n",
                         state->surface_color_offset[0], count, first);
          for (u32 k = 0; k < count && k < 30; k++)
            fprintf(f, "v%02u a0=(%.3f,%.3f,%.3f) a2=(%.3f,%.3f,%.3f) a9=(%.4f,%.4f,%.4f,%.4f)\n", k,
                out[k*16].v[0],out[k*16].v[1],out[k*16].v[2],
                out[k*16+2].v[0],out[k*16+2].v[1],out[k*16+2].v[2],
                out[k*16+9].v[0],out[k*16+9].v[1],out[k*16+9].v[2],out[k*16+9].v[3]);
          fclose(f); } } }
    dbg_vtx_dump(state, out, first, count);
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Non-indexed TRIANGLE_STRIP (fan=0) / TRIANGLE_FAN (fan=1) through the VP
 * path, expanded to a triangle list. LBP's Bink movie draws its YUV quad as a
 * non-indexed 4-vertex strip (prim 6); without this it fell to the fixed-
 * function fallback with is_vp=0 (no vertex-program transform -> the quad
 * collapsed to a line) and textured=0 (the Y/U/V planes never sampled). */
static u32 upload_strip_vp(const rsx_state* state, u32 first, u32 count, int fan)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    vp_plan_begin(state);
    if (count < 3) return 0;
    u32 tris = count - 2;
    u32 maxv = (s_vp_cap * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    s_vp_want += (u64)tris * 3;
    if (tris * 3 > maxv) tris = maxv / 3;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * s_vp_cap * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 t = 0; t < tris; t++) {
        u32 i0 = fan ? 0 : t;
        /* strip winding alternates per triangle to keep facing consistent */
        u32 i1 = t + 1, i2 = t + 2;
        if (!fan && (t & 1)) { u32 tmp = i1; i1 = i2; i2 = tmp; }
        read_vp_vertex_c(state, first + i0, &out[o*16]); o++;
        read_vp_vertex_c(state, first + i1, &out[o*16]); o++;
        read_vp_vertex_c(state, first + i2, &out[o*16]); o++;
    }
    dbg_vtx_dump(state, out, first, o);
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Fetch index k from the guest index array (SET_INDEX_ARRAY_ADDRESS/_DMA:
 * dma [3:0] = location (0 local, 1 main), [7:4] = type (0 u32, 1 u16)).
 * Indices are big-endian in guest memory. */
static u32 read_guest_index(const rsx_state* st, u32 k)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (s_lv_open) { if (k < s_lv_klo) s_lv_klo = k; if (k > s_lv_khi) s_lv_khi = k; }
    int local = ((st->index_array_dma & 0xF) == 0);
    u32 base = cellGcmResolveLocated(local, st->index_array_offset);
    const u8* p = vm_base + base;
    if (((st->index_array_dma >> 4) & 0xF) == 1) {
        p += (u64)k * 2;
        return ((u32)p[0] << 8) | p[1];
    }
    p += (u64)k * 4;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* read_guest_index for a loop over one draw's indices: the array's base is
 * resolved once, not per index (cellGcmResolveLocated is out of line, in
 * cellGcmSys.c, and the strip walk reads millions of indices a second). The
 * caller notes the range for the live-vertex window itself. */
typedef struct { const u8* p; int w16; } GuestIdx;
/* A strip or fan split over several DRAW_INDEX_ARRAY ranges of one
 * BEGIN/END carries its state into the next range (upload_strip_vp_indexed,
 * upload_strip_vp_fetch). */
static struct { int valid, fan; u32 epoch, next, m, p0, p1; } s_strip_carry;
static GuestIdx guest_index_open(const rsx_state* st)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    GuestIdx g;
    g.p = vm_base + cellGcmResolveLocated((st->index_array_dma & 0xF) == 0,
                                          st->index_array_offset);
    g.w16 = ((st->index_array_dma >> 4) & 0xF) == 1;
    return g;
}
static inline u32 guest_index_at(const GuestIdx* g, u32 k)
{
    if (g->w16) {
        const u8* p = g->p + (u64)k * 2;
        return ((u32)p[0] << 8) | p[1];
    }
    const u8* p = g->p + (u64)k * 4;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Indexed variants of the VP-path uploads. */
static u32 upload_quads_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    vp_plan_begin(state);
    u32 quads = count / 4;
    u32 maxv = (s_vp_cap * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    s_vp_want += (u64)quads * 6;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * s_vp_cap * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex_c(state, read_guest_index(state, first + q*4 + k), c[k]);
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (knob_vtx_dump()) { static int _n=0; if (_n < 6 && ++_n) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) {
                const float* vs_ = state->viewport_scale;
                const float* vo_ = state->viewport_offset;
                fprintf(f, "iq%02d vp_scale=(%.1f,%.1f,%.3f) vp_off=(%.1f,%.1f,%.3f)\n", _n,
                        vs_[0], vs_[1], vs_[2], vo_[0], vo_[1], vo_[2]);
                for (int _c = 464; _c <= 467; _c++)
                    fprintf(f, "  c[%d]=(%.3f,%.3f,%.3f,%.3f)\n", _c,
                            state->vertex_constants[_c][0], state->vertex_constants[_c][1],
                            state->vertex_constants[_c][2], state->vertex_constants[_c][3]);
                for (u32 k = 0; k < 4; k++)
                    fprintf(f, "  v%u i%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", k,
                        read_guest_index(state, first + q*4 + k),
                        c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                        c[k][8].v[0],c[k][8].v[1]);
                fclose(f);
            } } }
    }
    dbg_vtx_dump(state, out, first, o);
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

static u32 upload_tris_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++; s_vp_want += count;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    vp_plan_begin(state);
    u32 maxv = (s_vp_cap * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && _n < 12 && ++_n)
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * s_vp_cap * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      if (!state->restart_index_enable) {
          for (u32 k = 0; k < count; k++)
              read_vp_vertex_c(state, read_guest_index(state, first + k), &out[k*16]);
      } else {
          /* Primitive restart: the restart index drops the triangle in
           * progress and the list resumes after it.
           *
           * One BEGIN/END can carry several DRAW_INDEX_ARRAY ranges (at most
           * 256 indices each), and the RSX assembles them as one stream: a
           * triangle may start in one range and end in the next. Demon's
           * Souls' particles come as (0,32) (32,256) (288,12). Dropping the
           * two indices left over at the end of each range put the rest out
           * of phase, so sparks and smoke drew as slivers joining one
           * particle to the next, different every frame. The partial
           * triangle now carries into the next contiguous range of the same
           * BEGIN/END. */
          static struct { int valid; u32 epoch, next, n, tri[2]; } carry;
          u32 o = 0, n = 0, tri[3];
          if (carry.valid && carry.epoch == state->begin_epoch && carry.next == first) {
              n = carry.n; tri[0] = carry.tri[0]; tri[1] = carry.tri[1];
          }
          for (u32 k = 0; k < count; k++) {
              const u32 ix = read_guest_index(state, first + k);
              if (ix == state->restart_index) { n = 0; continue; }
              tri[n++] = ix;
              if (n == 3) {
                  for (int j = 0; j < 3; j++) read_vp_vertex_c(state, tri[j], &out[(o + j)*16]);
                  o += 3; n = 0;
              }
          }
          carry.valid = 1; carry.epoch = state->begin_epoch; carry.next = first + count;
          carry.n = n; carry.tri[0] = tri[0]; carry.tri[1] = tri[1];
          count = o;
      }
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; } }
    /* IDXDBG=<hex shader_program>: index range for that program's draws. An
     * index past the vertex array reads unmapped guest memory as zero, which
     * shows up as a spike triangle with a zero texcoord, not as a missing draw. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if (_n < 12 && ++_n) {
          u32 lo = 0xFFFFFFFFu, hi = 0;
          for (u32 k = 0; k < count; k++) { u32 ix = read_guest_index(state, first + k);
              if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
          fprintf(stderr, "[IDXDBG] first=%u count=%u idx=[%u..%u] dma=0x%X a0off=0x%X stride=%u%c",
                  first, count, lo, hi, state->index_array_dma,
                  state->vertex_attribs[0].offset, state->vertex_attribs[0].stride, 10); } } }
    /* DBG_LOCK tracking: projected centroid of this mesh, updated every draw. */
    { static const char* dl = (const char*)1; static u32 wantl = 0; static int mvpb = 256;
      if (dl == (const char*)1) { dl = getenv("RSX_DBG_VTX");
                                  wantl = dl ? (u32)strtoul(dl, NULL, 16) : 0;
                                  const char* mb = getenv("MVP_BASE");
                                  if (mb) mvpb = atoi(mb); }
      /* RSX_DBG_TRACK=1 tracks the content-identified duck without needing its
       * offset passed in (VRAM offsets move between runs). Re-read every call:
       * s_duck_raw is filled by the texture upload, which happens AFTER the
       * first draws, so caching it once left the tracker watching offset 0. */
      { static int dtrk = -1;
        PS3_KNOB(dtrk, ps3_env_int("RSX_DBG_TRACK", 0));
        if (dtrk && s_duck_raw) wantl = s_duck_raw; }
      /* RSX_DBG_PICK=<first>: track ONLY the draw with that starting index. The
       * mesh is one big vertex buffer sliced into 256-index chunks, so averaging
       * across all of them converges on the centre of the whole field -- which is
       * where there is nothing in particular. A single slice is a stable target
       * and can be magnified without drifting off it. */
      static int pick = -1;
      PS3_KNOB(pick, ps3_env_int("RSX_DBG_PICK", -2));
      if (wantl && s_d3d.cur_texs[0].raw == wantl && count
          && (pick == -2 || (int)first == pick)) {
          double sx = 0, sy = 0, sz = 0;
          for (u32 k = 0; k < count; k++) {
              sx += out[k*16].v[0]; sy += out[k*16].v[1]; sz += out[k*16].v[2];
          }
          float v[4] = { (float)(sx/count), (float)(sy/count), (float)(sz/count), 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[mvpb + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          if (clip[3] > 1e-6f) {
              float nx = clip[0]/clip[3], ny = clip[1]/clip[3];
              if (nx > -2.0f && nx < 2.0f && ny > -2.0f && ny < 2.0f) {
                  s_lock_sx += nx; s_lock_sy += ny; s_lock_n++;
                  if (!s_lock_valid) {        /* seed so frame 1 is usable */
                      s_lock_x = nx; s_lock_y = ny; s_lock_valid = 1;
                  }
              }
          }
      } }
    /* RSX_DBG_SCALE=<f>: multiply attribute-0 positions for the draws binding
     * RSX_DBG_VTX's texture. The duck's object-space bbox is ~0.2 units and its
     * draws rasterize only a few dozen pixels per frame -- sub-pixel. Growing it
     * about its own origin tests the rest of the chain (index buffer, texture,
     * per-draw transform): if a duck-shaped, duck-coloured object appears, only
     * the scale is wrong. Diagnostic. */
    { static const char* ds = (const char*)1; static float sc = 0.0f;
      if (ds == (const char*)1) { ds = getenv("RSX_DBG_SCALE");
                                  sc = ds ? (float)atof(ds) : 0.0f; }
      static const char* dv2 = (const char*)1; static u32 want2 = 0;
      if (dv2 == (const char*)1) { dv2 = getenv("RSX_DBG_VTX");
                                   want2 = dv2 ? (u32)strtoul(dv2, NULL, 16) : 0; }
      if (sc > 0.0f && want2 && s_d3d.cur_texs[0].raw == want2 && count) {
        /* Magnify about a FIXED point captured from the first tracked draw, not
         * about each chunk's own centre -- recentring per chunk would stack every
         * chunk of the mesh on top of the others. Combined with VP_BYPASS (which
         * maps attribute 0 straight to clip space) this renders the mesh's real
         * geometry and texture at a readable size. */
        /* RSX_DBG_RECENTER=1: recentre EVERY chunk on its own centroid, overlaying
         * them. If each chunk is one instance of the same mesh they align into a
         * single silhouette; if they are arbitrary slices of one big mesh they
         * smear. Either way the answer is visible at a glance. */
        static int recen = -1;
        PS3_KNOB(recen, ps3_env_int("RSX_DBG_RECENTER", 0));
        static int have_c = 0; static float ox = 0, oy = 0, oz = 0;
        if (!have_c || recen) {
            double sx = 0, sy = 0, sz = 0;
            for (u32 k = 0; k < count; k++) {
                sx += out[k*16].v[0]; sy += out[k*16].v[1]; sz += out[k*16].v[2];
            }
            ox = (float)(sx/count); oy = (float)(sy/count); oz = (float)(sz/count);
            have_c = 1;
        }
        for (u32 k = 0; k < count; k++) {
            /* RSX_DBG_SHIFT="dx,dy": nudge after scaling. VP_BYPASS applies the
             * guest's posoffset, so a recentred mesh does not land at screen
             * centre -- without this the magnified geometry sits clipped against
             * the top edge. */
            static int shf = -1; static float dx = 0.0f, dy = 0.0f;
            if (shf < 0) { const char* e = getenv("RSX_DBG_SHIFT");
                           if (e) { double a=0,b=0; sscanf(e, "%lf,%lf", &a, &b);
                                    dx = (float)a; dy = (float)b; }
                           shf = 1; }
            out[k*16].v[0] = (out[k*16].v[0] - ox) * sc + dx;
            out[k*16].v[1] = (out[k*16].v[1] - oy) * sc + dy;
            /* Leave z alone: under VP_BYPASS the object z maps straight to clip
             * z, so scaling it walks the geometry out of the depth range and
             * everything clips away. */
            (void)oz;
        }
      } }
    /* RSX_DBG_VTX=<hex tex0 offset>: attribute-0 positions for the draws binding
     * that texture (see upload_tris_vp). The duck's meshes are indexed, so this
     * is the copy that actually fires for it. */
    { static const char* dv = (const char*)1; static u32 want = 0; static int n = 0;
      if (dv == (const char*)1) { dv = getenv("RSX_DBG_VTX");
                                  want = dv ? (u32)strtoul(dv, NULL, 16) : 0; }
      if (want && s_d3d.cur_texs[0].raw == want && n < 8) { n++;
        float mnx=1e30f,mny=1e30f,mnz=1e30f,mxx=-1e30f,mxy=-1e30f,mxz=-1e30f;
        for (u32 k = 0; k < count; k++) {
            float x=out[k*16].v[0], y=out[k*16].v[1], z=out[k*16].v[2];
            if (x<mnx)mnx=x; if (y<mny)mny=y; if (z<mnz)mnz=z;
            if (x>mxx)mxx=x; if (y>mxy)mxy=y; if (z>mxz)mxz=z;
        }
        const rsx_vertex_attrib* _a3 = &state->vertex_attribs[3];
        fprintf(stderr, "[DUCKVTX] a3(en=%d type=%u size=%u stride=%u off=0x%08X freq=%u)"
                        " -> col0 v0=(%.3f %.3f %.3f %.3f)%c",
                _a3->enabled, _a3->type, _a3->size, _a3->stride, _a3->offset,
                _a3->frequency,
                out[3].v[0], out[3].v[1], out[3].v[2], out[3].v[3], 10);
        const rsx_vertex_attrib* _a0 = &state->vertex_attribs[0];
        fprintf(stderr, "[DUCKVTX] first=%u count=%u a0(type=%u size=%u stride=%u off=0x%08X)"
                        " obj-bbox x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f]%c",
                first, count, _a0->type, _a0->size, _a0->stride, _a0->offset,
                mnx,mxx, mny,mxy, mnz,mxz, 10);
        /* Project the object-space bbox corners through the first four transform
         * constants (the conventional MVP rows) and report NDC. Sub-pixel output
         * with correct vertices means the placement transform is the problem, and
         * the NDC extent says by how much. */
        { float cx=(mnx+mxx)*0.5f, cy=(mny+mxy)*0.5f, cz=(mnz+mxz)*0.5f;
          /* This title's Cg programs keep their MVP at c[256..259], not c[0..3]
           * (see the vp_uses_c03 note); projecting through c0 reported zeros. */
          static int MVP = -1;
          PS3_KNOB(MVP, ps3_env_int("MVP_BASE", 256));
          float v[4] = { cx, cy, cz, 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[MVP + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          float ex[4];
          { float e[4] = { mxx-cx, mxy-cy, mxz-cz, 0.0f };
            for (int r = 0; r < 4; r++) {
                const float* m = state->vertex_constants[MVP + r];
                ex[r] = m[0]*e[0] + m[1]*e[1] + m[2]*e[2];
            } }
          { int lo = -1, hi = -1, nz = 0;
            for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
                const float* m = state->vertex_constants[r];
                if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
            }
            fprintf(stderr, "[DUCKVTX]   constants: %d non-zero, slots %d..%d;", nz, lo, hi);
            for (int r = (lo < 0 ? 0 : lo); r < (lo < 0 ? 0 : lo) + 4 && r < RSX_MAX_VERTEX_CONSTANTS; r++)
                fprintf(stderr, " c%d=(%.3f %.3f %.3f %.3f)", r,
                        state->vertex_constants[r][0], state->vertex_constants[r][1],
                        state->vertex_constants[r][2], state->vertex_constants[r][3]);
            fprintf(stderr, "%c", 10); }
          fprintf(stderr, "[DUCKVTX]   mvp0=(%.3f %.3f %.3f %.3f) clip=(%.3f %.3f %.3f w=%.3f)",
                  state->vertex_constants[MVP][0], state->vertex_constants[MVP][1],
                  state->vertex_constants[MVP][2], state->vertex_constants[MVP][3],
                  clip[0], clip[1], clip[2], clip[3]);
          if (clip[3] > 1e-6f)
              fprintf(stderr, " ndc=(%.4f %.4f) half-extent-px=(%.2f %.2f)",
                      clip[0]/clip[3], clip[1]/clip[3],
                      ex[0]/clip[3]*640.0f, ex[1]/clip[3]*360.0f);
          fprintf(stderr, "%c", 10); }
      } }
    /* IDX_DBG=<N>: the first indices and the position they resolve to. An index
     * buffer read with the wrong element size or base yields huge indices and
     * degenerate triangles -- an INDEXED mesh vanishes while non-indexed
     * geometry in the same scene renders fine. */
    { static int cap = -1, n = 0;
      PS3_KNOB(cap, ps3_env_int("IDX_DBG", 0));
      if (cap && n < cap && count >= 3) { n++;
        extern u32 cellGcmResolveOffset(u32);
        const rsx_vertex_attrib* a0 = &state->vertex_attribs[0];
        u32 vbase = cellGcmResolveOffset(a0->offset & 0x7FFFFFFFu);
        u32 nz = 0; for (u32 q = 0; q < 0x1000u; q += 29) if (vm_base[vbase + q]) nz++;
        fprintf(stderr, "[IDX] idxoff=0x%X idx: %u %u %u | a0 off=0x%08X(->0x%08X nz=%u/142)"
                        " stride=%u size=%u type=%u  pos0=(%g %g %g)%c",
                state->index_array_offset,
                read_guest_index(state, first), read_guest_index(state, first+1),
                read_guest_index(state, first+2),
                a0->offset, vbase, nz, a0->stride, a0->size, a0->type,
                out[0].v[0], out[0].v[1], out[0].v[2], 10); } }
    dbg_vtx_dump(state, out, first, count);
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Indexed TRIANGLE_STRIP / TRIANGLE_FAN, expanded CPU-side into a triangle
 * list (the whole VP path draws lists). The guest's face culling is honoured,
 * so strip parity winding matters. DeferredShading's spline tube meshes -- the
 * octopus tentacles -- are indexed strips. */
/* The last indexed upload's indices (count 0 = it expanded to a list), for
 * d3d12_draw_indexed's record. */
static u32 s_up_icount, s_up_ifirst;

static u32 upload_strip_vp_indexed(const rsx_state* state, u32 first, u32 count, int fan)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    /* VP_SLOW_DRAW_NS=<ns> (live): spin that long per strip upload. A causal
     * test for whether the FIFO thread's draw cost sets the frame rate. */
    { static int slow = -1; PS3_KNOB(slow, ps3_env_int("VP_SLOW_DRAW_NS", 0));
      if (slow > 0) { static long long fq; LARGE_INTEGER t0, t;
          if (!fq) { QueryPerformanceFrequency(&t); fq = t.QuadPart; }
          QueryPerformanceCounter(&t0);
          do { YieldProcessor(); QueryPerformanceCounter(&t); }
          while ((t.QuadPart - t0.QuadPart) * 1000000000LL / fq < slow); } }
    vp_attrs_dbg(state);
    vp_plan_begin(state);
    u32 maxv = (s_vp_cap * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * s_vp_cap * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    /* Expand to a triangle list, one index at a time:
     *  - Strip triangles alternate winding; every odd one is emitted with its
     *    first two vertices swapped (as the non-indexed path does), or culling
     *    removes half of a culled strip.
     *  - Primitive restart (SET_RESTART_INDEX_ENABLE): an index equal to the
     *    restart index ends the strip or fan, and the next one starts after
     *    it. Without it the restart index was fetched as a vertex (past the
     *    end of the array) and stretched triangles across the screen.
     *  - One BEGIN/END can carry several DRAW_INDEX_ARRAY ranges of at most
     *    256 indices, and the RSX assembles them as one stream. Demon's Souls'
     *    level meshes are strips split that way; restarting the strip at each
     *    range lost the two triangles across every boundary (holes in the
     *    tunnel ceiling). The strip state carries into the next contiguous
     *    range of the same BEGIN/END, as for triangle lists. */
    const int restart = state->restart_index_enable != 0;
    const u32 ridx = state->restart_index;
#define carry s_strip_carry
    u32 m = 0, p0 = 0, p1 = 0;   /* m: indices into the current strip; p0/p1: last two (fan: centre, last) */
    if (carry.valid && carry.fan == fan && carry.epoch == state->begin_epoch && carry.next == first) {
        m = carry.m; p0 = carry.p0; p1 = carry.p1;
    }
    u32 o = 0, need = 0;   /* need: vertices the strip expands to, room or not */
    const GuestIdx gi = guest_index_open(state);
    if (s_lv_open && count) {
        if (first < s_lv_klo) s_lv_klo = first;
        if (first + count - 1 > s_lv_khi) s_lv_khi = first + count - 1;
    }
    /* VP_INDEXED (live, default 1): write each of the draw's vertices once and
     * the triangle list as u32 indices into them. A Demon's Souls level strip
     * expands to ~2.5 list vertices per guest vertex, and at 256 bytes each
     * the expanded list was ~350 MB a frame through the write-combined upload
     * heap, then read back by the GPU. 0 = expand to an unindexed list. */
    static int indexed = -1;
    PS3_KNOB(indexed, ps3_env_int("VP_INDEXED", 1));
    /* Room for the most indices the strip can make, or the list path. */
    const int want_ix = indexed && s_vp_fast;
    if (want_ix && s_d3d.vp_ib_mapped && s_vp_icap - s_d3d.vp_ib_offset >= (u64)count * 3) {
        u32* ib = (u32*)s_d3d.vp_ib_mapped + (u64)s_d3d.vp_parity * s_vp_icap + s_d3d.vp_ib_offset;
        const u32 iroom = s_vp_icap - s_d3d.vp_ib_offset;
        const u32 icap = iroom - iroom % 3;
        u32 ni = 0, nmiss = 0;
        u32 tri[3];
        const int par = vpj_usable() && vpj_room(count);
        u32* vis = par ? s_vpj_vis + s_vpj_vis_n : NULL;
        for (u32 k = 0; k < count; k++) {
            const u32 ix = guest_index_at(&gi, first + k);
            if (restart && ix == ridx) { m = 0; continue; }
            if (m >= 2) need += 3;
            if (m >= 2 && ni + 3 <= icap && o + 3 <= maxv) {
                tri[0] = p0; tri[1] = p1; tri[2] = ix;
                if (!fan && ((m - 2) & 1)) { tri[0] = p1; tri[1] = p0; }
                for (int t = 0; t < 3; t++) {
                    const u32 vi = tri[t];
                    const u32 h = (vi ^ (vi >> 8)) & (VP_VC_N - 1);
                    if (s_vx_tag[h] != s_vc_gen || s_vx_key[h] != vi) {
                        if (par) vis[o] = vi;
                        else     read_vp_vertex(state, vi, &out[o*16]);
                        s_vx_key[h] = vi; s_vx_tag[h] = s_vc_gen; s_vx_out[h] = s_strip_base + o++;
                        nmiss++;
                    }
                    ib[ni++] = s_vx_out[h];
                }
            }
            if (fan) { if (m == 0) p0 = ix; else p1 = ix; }
            else     { p0 = p1; p1 = ix; }
            m++;
        }
        carry.valid = 1; carry.fan = fan; carry.epoch = state->begin_epoch;
        carry.next = first + count; carry.m = m; carry.p0 = p0; carry.p1 = p1;
        s_vp_want += nmiss;
        s_vp_iwant += need;
        if (par && o) { vpj_queue(vis, o, out); s_vs_fetch += o; s_vs_attr += (u64)o * s_vp_plan.n_on; }
        if (!par) dbg_vtx_dump(state, out, first, o);
        s_up_icount = ni; s_up_ifirst = s_d3d.vp_ib_offset;
        s_d3d.vp_ib_offset += ni;
        s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
        return ni ? o : 0;
    }
    const u32 cap = maxv - maxv % 3;
    for (u32 k = 0; k < count; k++) {
        const u32 ix = guest_index_at(&gi, first + k);
        if (restart && ix == ridx) { m = 0; continue; }
        if (m >= 2) need += 3;
        if (m >= 2 && o + 3 <= cap) {
            u32 v0 = p0, v1 = p1;
            if (!fan && ((m - 2) & 1)) { v0 = p1; v1 = p0; }
            read_vp_vertex_c(state, v0, &out[o*16]); o++;
            read_vp_vertex_c(state, v1, &out[o*16]); o++;
            read_vp_vertex_c(state, ix, &out[o*16]); o++;
        }
        if (fan) { if (m == 0) p0 = ix; else p1 = ix; }
        else     { p0 = p1; p1 = ix; }
        m++;
    }
    carry.valid = 1; carry.fan = fan; carry.epoch = state->begin_epoch;
    carry.next = first + count; carry.m = m; carry.p0 = p0; carry.p1 = p1;
    if (want_ix) s_vp_iwant += need;   /* so the index buffer grows to fit */
    /* What the buffer grows to: (count-2)*3 counted the strip as unbroken,
     * twice the vertices a Demon's Souls frame emits once restarts split its
     * level strips, and grew the buffer to the 2M-vertex cap for nothing. */
    s_vp_want += need;
    { static int said = 0;
      if (restart && !said) { said = 1;
          fprintf(stderr, "[D3D12] primitive restart in use (index 0x%X)%c", ridx, 10); } }
    dbg_vtx_dump(state, out, first, o);
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
#undef carry
}
/* GPU vertex fetch (VP_GPU_FETCH, live, default 1). The CPU path above walks
 * a strip, numbers its vertices, converts each one to 16 float4s (256 bytes)
 * and writes them to vp_vb; in a Demon's Souls level frame that conversion
 * and its writes are most of the FIFO thread. Here the strip becomes a
 * triangle list of the guest's own indices, the guest bytes the indices
 * reach are copied as they are into vf_buf, and the vertex shader
 * (rsx_vp_decompile_fetch) reads and converts them itself.
 *
 * vf_buf's half for this frame holds, per descriptor: 16 attribute records
 * (32 bytes each, the layout rsx_vp_decompile_fetch documents), then the
 * guest byte spans they point into. A record's x is where vertex 0's
 * element would sit, so one descriptor serves every range of a strip whose
 * bytes it holds; a range reaching past the last span appends to it when
 * nothing was written after it. */
static u32 s_up_fdesc;
static u64 s_vf_stat[6];   /* new, reuse, append, sparse, type/addr, room */

static int vf_on(void)
{
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("VP_GPU_FETCH", 1));
    return en && s_d3d.vf_mapped && s_vf_cap && s_vp_fast && !late_on();
}

/* The guest address of the main-memory bytes [a,b), or ~0 when they are not
 * one linear run (IO pages are 1 MB). */
static u32 vf_resolve(const rsx_fetch_slot* sl, u64 a, u64 b)
{
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (!sl->main) return sl->local_addr + (u32)a;
    const u32 ga = cellGcmResolveLocated(0, (u32)a);
    if (ga == 0xFFFFFFFFu) return ga;
    for (u64 x = (a | 0xFFFFFu) + 1; x < b; x += 0x100000u)
        if (cellGcmResolveLocated(0, (u32)x) != ga + (u32)(x - a)) return 0xFFFFFFFFu;
    return ga;
}

/* A strip's triangle list from the guest's big-endian indices, for
 * upload_strip_vp_fetch: the same list, carry and [lo,hi] as its general
 * loop, with the index width a constant and the winding taken from m's low
 * bit. lo/hi cover every index a triangle uses: each new index, and p0/p1 at
 * the first triangle of a run or of the call (the indices before it, or the
 * carry); every later triangle's other two indices were new indices already.
 *
 * A Palace frame converts several hundred thousand strip indices on the FIFO
 * thread, and the general loop (per-index width test, fan test, six
 * compares) was 16% of that thread. Once the FIFO thread is on the frame's
 * path, as it is at ~105 fps, this is worth 100.2 -> 106.2 fps. Checked
 * against the general loop over 6.8 million draws with no difference. */
static inline __attribute__((always_inline)) u32 vf_strip_list_w(const u8* src, const int w16, u32 count,
                                         const int restart, const u32 ridx, u32* pm,
                                         u32* pp0, u32* pp1, u32* ib, u32* plo, u32* phi)
{
    u32 m = *pm, p0 = *pp0, p1 = *pp1, lo = *plo, hi = *phi, ni = 0;
    for (u32 k = 0; k < count; k++) {
        u32 ix;
        if (w16) { u16 v; memcpy(&v, src + (u64)k * 2, 2); ix = __builtin_bswap16(v); }
        else     { u32 v; memcpy(&v, src + (u64)k * 4, 4); ix = __builtin_bswap32(v); }
        if (restart && ix == ridx) { m = 0; continue; }
        if (m >= 2) {
            const u32 odd = m & 1u;   /* (m - 2) & 1 */
            ib[ni] = odd ? p1 : p0; ib[ni + 1] = odd ? p0 : p1; ib[ni + 2] = ix; ni += 3;
            if (m == 2 || ni == 3) {
                if (p0 < lo) lo = p0; if (p0 > hi) hi = p0;
                if (p1 < lo) lo = p1; if (p1 > hi) hi = p1;
            }
            lo = ix < lo ? ix : lo; hi = ix > hi ? ix : hi;
        }
        p0 = p1; p1 = ix; m++;
    }
    *pm = m; *pp0 = p0; *pp1 = p1; *plo = lo; *phi = hi;
    return ni;
}
static u32 vf_strip_list(const GuestIdx* gi, u32 first, u32 count, int restart, u32 ridx,
                         u32* pm, u32* pp0, u32* pp1, u32* ib, u32* plo, u32* phi)
{
    if (gi->w16) return vf_strip_list_w(gi->p + (u64)first * 2, 1, count, restart, ridx,
                                        pm, pp0, pp1, ib, plo, phi);
    return vf_strip_list_w(gi->p + (u64)first * 4, 0, count, restart, ridx,
                           pm, pp0, pp1, ib, plo, phi);
}

/* The strip as a strip (VF_NATIVE_STRIP, live, default 1): the draw is a
 * TRIANGLESTRIP and the guest's indices go into the index buffer as they are,
 * byte-swapped, with the restart index as D3D12's cut value (every VP PSO
 * sets IBStripCutValue to it). One index in, one out, no branches, against
 * the list's three out and a winding choice per triangle: the list walk was
 * ~15% of the FIFO thread at ~109 fps, with that thread on the frame's path.
 *
 * A range starts with a cut and then restarts the strip it continues from
 * the carry, with the winding parity the list would have used (triangle i
 * of a strip is (i, i+1, i+2) when i is even and (i+1, i, i+2) when odd):
 *   m == 1          -> p1
 *   m even          -> p0 p1
 *   m odd, m >= 3   -> p0 p0 p1   (one degenerate triangle shifts the parity)
 * That holds whether the range opens a draw or extends the last one. The
 * carry going out comes from the guest's last indices.
 *
 * Palace, same run: 108.9 -> 112.7 fps. Checked against the list walk, as
 * triangles with degenerates dropped, over 26 million ranges (Tutorial and
 * Palace) with no difference. VF_NATIVE_STRIP=0 goes back to the list. */
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif
#define VF_CUT 0xFFFFFFFFu
static inline __attribute__((always_inline)) u32 vf_ix(const u8* src, const int w16, u32 k)
{
    if (w16) { u16 v; memcpy(&v, src + (u64)k * 2, 2); return __builtin_bswap16(v); }
    u32 v; memcpy(&v, src + (u64)k * 4, 4); return __builtin_bswap32(v);
}
static inline __attribute__((always_inline)) u32 vf_strip_native_w(const u8* src, const int w16, u32 count,
                                           const int restart, const u32 ridx, u32* pm,
                                           u32* pp0, u32* pp1, u32* ib, u32* plo, u32* phi)
{
    u32 m = *pm, p0 = *pp0, p1 = *pp1, lo = *plo, hi = *phi, n = 0;
    ib[n++] = VF_CUT;
    if (m >= 1) {
        if (m >= 3 && (m & 1u)) ib[n++] = p0;
        if (m >= 2) { ib[n++] = p0; lo = p0 < lo ? p0 : lo; hi = p0 > hi ? p0 : hi; }
        ib[n++] = p1; lo = p1 < lo ? p1 : lo; hi = p1 > hi ? p1 : hi;
    }
    /* ib is an upload heap (write-combined): written in order, never read
     * back. A backward scan of it for the last restart was 29% of the FIFO
     * thread, so the loop keeps r, and the tail comes from the guest's bytes. */
    u32* b = ib + n;
    u32 r = 0;   /* where the strip after the range's last restart begins (0: none) */
    u32 k = 0;
#if defined(_M_X64) || defined(__x86_64__)
    /* 16-bit indices eight at a time, the same outputs as the loop below:
     * byte swap, restart lanes to the cut value, unsigned min/max over the
     * other lanes (SSE2 has only the signed ones, hence the 0x8000 bias),
     * and the last restart from the compare mask. The scalar loop was ~10%
     * of the FIFO thread, a cmov chain per index. */
    static int sse = -1;
    PS3_KNOB(sse, ps3_env_int("VF_STRIP_SSE", 1));
    if (w16 && count >= 8 && sse) {
        const __m128i bias = _mm_set1_epi16((short)0x8000);
        const __m128i rv = _mm_set1_epi16((short)ridx);
        const int rs = restart && ridx <= 0xFFFFu;
        __m128i vlo = _mm_set1_epi16(0x7FFF), vhi = _mm_set1_epi16((short)0x8000);
        __m128i real = _mm_setzero_si128();
        for (; k + 8 <= count; k += 8) {
            __m128i v = _mm_loadu_si128((const __m128i*)(src + (u64)k * 2));
            v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
            const __m128i c = rs ? _mm_cmpeq_epi16(v, rv) : _mm_setzero_si128();
            const __m128i o = _mm_or_si128(v, c);   /* restart lanes 0xFFFF */
            _mm_storeu_si128((__m128i*)(b + k), _mm_unpacklo_epi16(o, c));
            _mm_storeu_si128((__m128i*)(b + k + 4), _mm_unpackhi_epi16(o, c));
            vlo = _mm_min_epi16(vlo, _mm_xor_si128(o, bias));
            vhi = _mm_max_epi16(vhi, _mm_xor_si128(_mm_andnot_si128(c, v), bias));
            real = _mm_or_si128(real, _mm_xor_si128(c, _mm_set1_epi32(-1)));
            const int mk = _mm_movemask_epi8(c);
            if (mk) { const u32 top = 31u - (u32)__builtin_clz((unsigned)mk);   /* the highest set bit */
                      r = k + (top >> 1) + 1; }
        }
        if (_mm_movemask_epi8(real)) {
            u16 l8[8], h8[8];
            _mm_storeu_si128((__m128i*)l8, _mm_xor_si128(vlo, bias));
            _mm_storeu_si128((__m128i*)h8, _mm_xor_si128(vhi, bias));
            for (int j = 0; j < 8; j++) {
                lo = l8[j] < lo ? l8[j] : lo; hi = h8[j] > hi ? h8[j] : hi;
            }
        }
    }
#endif
#define VF_IX(k) vf_ix(src, w16, k)
    for (; k < count; k++) {
        const u32 ix = VF_IX(k);
        const int c = restart && ix == ridx;
        b[k] = c ? VF_CUT : ix;
        const u32 vl = c ? 0xFFFFFFFFu : ix, vh = c ? 0u : ix;
        lo = vl < lo ? vl : lo; hi = vh > hi ? vh : hi;
        r = c ? k + 1 : r;
    }
    const u32 tail = count - r;
    m = r ? tail : m + count;
    if (tail >= 2)      { p0 = VF_IX(count - 2); p1 = VF_IX(count - 1); }
    else if (tail == 1) { p0 = p1; p1 = VF_IX(count - 1); }
#undef VF_IX
    *pm = m; *pp0 = p0; *pp1 = p1; *plo = lo; *phi = hi;
    return n + count;
}
static u32 vf_strip_native(const GuestIdx* gi, u32 first, u32 count, int restart, u32 ridx,
                           u32* pm, u32* pp0, u32* pp1, u32* ib, u32* plo, u32* phi)
{
    if (gi->w16) return vf_strip_native_w(gi->p + (u64)first * 2, 1, count, restart, ridx,
                                          pm, pp0, pp1, ib, plo, phi);
    return vf_strip_native_w(gi->p + (u64)first * 4, 0, count, restart, ridx,
                             pm, pp0, pp1, ib, plo, phi);
}
static int s_up_strip;   /* the last upload_strip_vp_fetch wrote a strip */

/* Returns the indices written (0 is a strip that made no triangle), or -1
 * when the draw has to take the CPU path. ext: the fetch draw this range
 * continues, or NULL. */
static int upload_strip_vp_fetch(const rsx_state* state, u32 first, u32 count, int fan,
                                 const D3D12DrawRecord* ext)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_ib_mapped || !state->vertex_attribs[0].enabled) return -1;
    if ((u64)s_vp_icap - s_d3d.vp_ib_offset < (u64)count * 3 + 4) { s_vf_stat[5]++; return -1; }
    static rsx_fetch_plan pl;
    rsx_fetch_plan_init(state, &pl);
    for (u32 k = 0; k < pl.n_on; k++) {
        const u32 t = pl.on[k].type;
        if (t < 1 || t > 7 || t == 6) { s_vf_stat[4]++; return -1; }
    }
    /* The list, straight into the index buffer; it only counts once the
     * draw is accepted (vp_ib_offset, the carry). */
    const int restart = state->restart_index_enable != 0;
    const u32 ridx = state->restart_index;
    u32 m = 0, p0 = 0, p1 = 0;
    if (s_strip_carry.valid && s_strip_carry.fan == fan &&
        s_strip_carry.epoch == state->begin_epoch && s_strip_carry.next == first) {
        m = s_strip_carry.m; p0 = s_strip_carry.p0; p1 = s_strip_carry.p1;
    }
    u32* ib = (u32*)s_d3d.vp_ib_mapped + (u64)s_d3d.vp_parity * s_vp_icap + s_d3d.vp_ib_offset;
    const GuestIdx gi = guest_index_open(state);
    u32 ni = 0, lo = 0xFFFFFFFFu, hi = 0;
    static int nat = -1;
    PS3_KNOB(nat, ps3_env_int("VF_NATIVE_STRIP", 1));
    s_up_strip = !fan && nat;
    if (s_up_strip) {
        ni = vf_strip_native(&gi, first, count, restart, ridx, &m, &p0, &p1, ib, &lo, &hi);
        if (lo > hi) ni = 0;   /* restarts only: no vertex, so no triangle */
    }
    else if (!fan) ni = vf_strip_list(&gi, first, count, restart, ridx, &m, &p0, &p1, ib, &lo, &hi);
    else for (u32 k = 0; k < count; k++) {
        const u32 ix = guest_index_at(&gi, first + k);
        if (restart && ix == ridx) { m = 0; continue; }
        if (m >= 2) {
            ib[ni] = p0; ib[ni + 1] = p1; ib[ni + 2] = ix; ni += 3;
            if (p0 < lo) lo = p0; if (p0 > hi) hi = p0;
            if (p1 < lo) lo = p1; if (p1 > hi) hi = p1;
            if (ix < lo) lo = ix; if (ix > hi) hi = ix;
        }
        if (m == 0) p0 = ix; else p1 = ix;
        m++;
    }
    if (ni) {
        /* Each array's bytes over [lo,hi]. A range that reaches far more
         * vertices than it has indices copies more than the CPU path
         * converts: leave it to that path. VF_SPARSE (live, default 64):
         * how many vertices an index may reach. The CPU path writes 256
         * bytes a vertex and converts every attribute; the copy is the
         * guest's own bytes, tens a vertex, and the next ranges of the
         * mesh usually reuse it. At 4, the 256-index ranges of Demon's
         * Souls's level meshes left 64,000 vertices a frame to the CPU in
         * the deep Palace; at 64, 10,800. */
        { static int sx = -1;
          PS3_KNOB(sx, ps3_env_int("VF_SPARSE", 64));
          if ((u64)hi - lo + 1 > (u64)count * (u32)sx + 256) { s_vf_stat[3]++; return -1; } }
        u32 sg0[16], sg1[16], sx[16];
        for (u32 k = 0; k < pl.n_on; k++) {
            const rsx_fetch_slot* sl = &pl.on[k];
            u32 elo = lo, ehi = hi;
            if (sl->freq) { if (sl->modulo) { elo = 0; ehi = sl->freq - 1; }
                            else { elo /= sl->freq; ehi /= sl->freq; } }
            const u64 a = (u64)sl->off + (u64)elo * sl->stride;
            const u64 b = (u64)sl->off + (u64)ehi * sl->stride + lv_elem_bytes(sl->type, sl->n);
            if (b - a > (16u << 20)) { s_vf_stat[3]++; return -1; }
            const u32 ga = vf_resolve(sl, a, b);
            if (ga == 0xFFFFFFFFu || (u64)ga + (b - a) > 0xFFFFFFFFull) { s_vf_stat[4]++; return -1; }
            sg0[k] = ga; sg1[k] = ga + (u32)(b - a);
            sx[k] = elo * sl->stride;   /* x = ring(ga) - sx */
        }
        int done = 0;
        /* The draw being extended: its descriptor holds these bytes, or
         * can take them by appending to its one span. */
        if (ext && s_fx.valid && s_fx.fdesc == ext->fdesc && s_fx.epoch == state->begin_epoch) {
            u32 ng1 = 0; int ok = 1;
            for (u32 k = 0; k < pl.n_on && ok; k++) {
                int in = 0;
                for (u32 j = 0; j < s_fx.nsp; j++) {
                    const int last = (j + 1 == s_fx.nsp) && s_fx.end == s_vf_off;
                    if (sg0[k] >= s_fx.g0[j] && (sg1[k] <= s_fx.g1[j] || (last && sg0[k] <= s_fx.g1[j]))) {
                        if (s_fx.r0[j] + (sg0[k] - s_fx.g0[j]) - sx[k] != s_fx.x[pl.on[k].idx]) break;
                        if (sg1[k] > s_fx.g1[j] && sg1[k] > ng1) ng1 = sg1[k];
                        in = 1; break;
                    }
                }
                ok = in;
            }
            if (ok && ng1) {
                const u32 j = s_fx.nsp - 1;
                const u32 g1 = (ng1 + 3u) & ~3u, add = g1 - s_fx.g1[j];
                if ((u64)s_vf_off + add <= s_vf_cap) {
                    vf_copy((u8*)s_d3d.vf_mapped + (u64)s_d3d.vp_parity * s_vf_cap + s_vf_off,
                            s_fx.g1[j], add);
                    s_vf_off += add; s_fx.end = s_vf_off; s_fx.g1[j] = g1;
                    s_vf_stat[2]++;
                } else ok = 0;
            } else if (ok) s_vf_stat[1]++;
            done = ok;
        }
        if (!done) {
            /* A new descriptor: the spans, merged where they touch (an
             * interleaved array is one span for all its attributes). */
            u32 ord[16], n = pl.n_on;
            for (u32 k = 0; k < n; k++) ord[k] = k;
            for (u32 i = 1; i < n; i++)
                for (u32 j = i; j > 0 && sg0[ord[j]] < sg0[ord[j - 1]]; j--)
                    { const u32 t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t; }
            u32 g0[16], g1[16], nsp = 0;
            for (u32 i = 0; i < n; i++) {
                const u32 a = sg0[ord[i]] & ~3u, b = (sg1[ord[i]] + 3u) & ~3u;
                if (nsp && a <= g1[nsp - 1] + 256u) { if (b > g1[nsp - 1]) g1[nsp - 1] = b; }
                else { g0[nsp] = a; g1[nsp] = b; nsp++; }
            }
            u64 total = 512;
            for (u32 j = 0; j < nsp; j++) total += g1[j] - g0[j];
            if ((u64)s_vf_off + total > s_vf_cap) { s_vf_stat[5]++; s_vf_short += total; return -1; }
            u8* base = (u8*)s_d3d.vf_mapped + (u64)s_d3d.vp_parity * s_vf_cap;
            const u32 fdesc = s_vf_off;
            u32 r = fdesc + 512;
            for (u32 j = 0; j < nsp; j++) {
                vf_copy(base + r, g0[j], g1[j] - g0[j]);
                s_fx.g0[j] = g0[j]; s_fx.g1[j] = g1[j]; s_fx.r0[j] = r;
                r += g1[j] - g0[j];
            }
            struct { u32 x, y, z, w; float c[4]; } rec[16];
            for (u32 i = 0; i < 16; i++) {
                rec[i].x = rec[i].y = rec[i].z = rec[i].w = 0;
                memcpy(rec[i].c, pl.konst[i], 16);
                s_fx.x[i] = 0;
            }
            for (u32 k = 0; k < n; k++) {
                const rsx_fetch_slot* sl = &pl.on[k];
                u32 j = 0;
                while (j + 1 < nsp && !(sg0[k] >= g0[j] && sg0[k] < g1[j])) j++;
                const u32 x = s_fx.r0[j] + (sg0[k] - g0[j]) - sx[k];
                rec[sl->idx].x = x; rec[sl->idx].y = sl->stride;
                rec[sl->idx].z = sl->type | (sl->n << 4)
                               | ((sl->freq ? (sl->modulo ? 2u : 1u) : 0u) << 8);
                rec[sl->idx].w = sl->freq ? sl->freq : 1;
                s_fx.x[sl->idx] = x;
            }
            memcpy(base + fdesc, rec, sizeof rec);
            s_vf_off = r;
            s_fx.valid = 1; s_fx.epoch = state->begin_epoch; s_fx.fdesc = fdesc;
            s_fx.end = r; s_fx.nsp = nsp;
            s_vf_stat[0]++;
        }
        s_up_fdesc = s_fx.fdesc;
    }
    s_strip_carry.valid = 1; s_strip_carry.fan = fan; s_strip_carry.epoch = state->begin_epoch;
    s_strip_carry.next = first + count; s_strip_carry.m = m;
    s_strip_carry.p0 = p0; s_strip_carry.p1 = p1;
    s_vp_iwant += ni;
    s_up_icount = ni; s_up_ifirst = s_d3d.vp_ib_offset;
    s_d3d.vp_ib_offset += ni;
    { static int st = -1; static u32 calls;
      PS3_KNOB(st, ps3_env_int("VP_FETCH_STATS", 0));
      if (st && ++calls % 20000u == 0)
          fprintf(stderr, "[VF] new %llu reuse %llu append %llu | cpu: sparse %llu type/addr %llu room %llu | ring %u KB%c",
                  (unsigned long long)s_vf_stat[0], (unsigned long long)s_vf_stat[1],
                  (unsigned long long)s_vf_stat[2], (unsigned long long)s_vf_stat[3],
                  (unsigned long long)s_vf_stat[4], (unsigned long long)s_vf_stat[5],
                  s_vf_off >> 10, 10); }
    return (int)ni;
}


