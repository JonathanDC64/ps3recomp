/* Upload the guest texture for a VP draw into a per-frame texture slot
 * (re-uploaded every frame: gcm/cube's plasma animates in guest memory).
 * Returns the slot index (SRV at heap VP_TEX_SRV_BASE+slot) or -1. Must run
 * while the command list is open, before the draw passes. */
/* NV4097 TEXTURE_CONTROL1 component remap -> D3D12 Shader4ComponentMapping.
 * The crossbar decode itself is RSX semantics and lives in
 * rsx_texture_layout.c; all that is left here is packing the four selectors
 * into D3D12's word. Its force-zero/force-one encodings are 4 and 5, which is
 * what RSX_REMAP_ZERO/ONE already are, so the selectors pass straight through.
 *
 * out[] arrives in the crossbar's field order A,R,G,B; D3D12 wants
 * destR | destG<<3 | destB<<6 | destA<<9 | valid<<12. */
static u32 rsx_remap_to_d3d(u32 c1, u32 basef)
{
    u8 out[4];
    rsx_texture_component_remap(c1, basef, out);
    /* TEX_REMAP_ID=<n>: override the derived crossbar with a fixed mapping, to
     * test channel order directly. The resource holds the guest's A8R8G8B8
     * bytes straight through, so its components are (R=A, G=R, B=G, A=B) and
     * the shader needs destR=1, destG=2, destB=3, destA=0 to see real RGBA.
     *   1 = identity   2 = rotate (the ARGB fix)   3 = BGRA swap */
    { static int fixed = -1;
      PS3_KNOB(fixed, ps3_env_int("TEX_REMAP_ID", 0));
      if (fixed == 1) return 0u | (1u<<3) | (2u<<6) | (3u<<9) | (1u<<12);
      if (fixed == 2) return 1u | (2u<<3) | (3u<<6) | (0u<<9) | (1u<<12);
      if (fixed == 3) return 2u | (1u<<3) | (0u<<6) | (3u<<9) | (1u<<12); }
    /* D3D12 mapping: destR | destG<<3 | destB<<6 | destA<<9 | valid bit */
    { static int _rd = -1; PS3_KNOB(_rd, getenv("REMAPDBG") ? 1 : 0);
      if (_rd) { static u32 seen[32]; static int ns=0;
        u32 k = (basef << 16) | (c1 & 0xFFFFu); int f=0;
        for (int i2=0;i2<ns;i2++) if (seen[i2]==k) f=1;
        if (!f && ns<32) { seen[ns++]=k;
            fprintf(stderr, "[REMAP] basef=0x%02X c1=0x%04X -> destR=%u destG=%u destB=%u destA=%u%c",
                    basef, c1 & 0xFFFFu, out[1], out[2], out[3], out[0], 10); } } }
    return out[1] | (out[2] << 3) | (out[3] << 6) | (out[0] << 9) | (1u << 12);
}

/* Morton/Z-order texel offset for RSX swizzled textures (LN bit clear).
 * Interleaves x (even bit positions) and y (odd) until the smaller dimension's
 * bits run out, then the larger dimension's remaining bits ride above -- the
 * NV40 layout (matches RPCS3 convert_linear_swizzle). POT dims only, which is
 * what the hardware requires for swizzled textures. */

/* Sparse FNV-1a over a texture's source bytes -- enough to notice an animated
 * surface changing, cheap enough to run on every bind. Kept a function rather
 * than a statement-expression macro so the file still compiles under MSVC;
 * ps3recomp builds with clang-cl, but titles like Tokyo Jungle drive the build
 * through the Visual Studio generator. */
static u32 tex_csum(const u8* base, u32 nbytes)
{
    u32 h = 2166136261u;
    u32 step = nbytes > 4096u ? nbytes / 1024u : 4u;
    /* The word read moves through each step (k*28 mod step): texture sizes
     * are powers of two, so a fixed stride always landed on the same bytes
     * of every compressed block -- a 128x128 DXT5 read only its alpha
     * endpoints, a DXT1 only its colour endpoints, never the indices. */
    u32 k = 0;
    for (u32 i = 0; i + 3 < nbytes; i += step, k++) {
        u32 at = step > 4u ? i + ((k * 28u) % step & ~3u) : i;
        if (at + 3 >= nbytes) at = i;
        u32 w32; memcpy(&w32, base + at, sizeof w32);
        h ^= w32; h *= 16777619u;
    }
    return h;
}

/* FNV-1a over every word, for TEX_VERIFY. */
static u32 tex_csum_full(const u8* base, u32 nbytes)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i + 3 < nbytes; i += 4) {
        u32 w32; memcpy(&w32, base + i, sizeof w32);
        h ^= w32; h *= 16777619u;
    }
    return h;
}

/* Does guest memory hold anything at [off, off+nbytes)? Sparse, about 1,000
 * reads spread over the whole range. A surface only the GPU rendered reads as
 * all zero here, because our backend never writes render targets back. */
static int tex_guest_written(u32 off, u32 nbytes)
{
    extern uint8_t* vm_base;
    if (!vm_base || !off) return 0;
    const u32 step = nbytes > 4096u ? (nbytes / 1024u) | 1u : 1u;
    for (u32 i = 0; i < nbytes; i += step)
        if (vm_base[off + i]) return 1;
    return 0;
}

/* The DXGI format of a decoded guest texture (rsx_texture_layout's class). The
 * upload creates the resource with it and the SRV views it with it. */
static DXGI_FORMAT rsx_texfmt_dxgi(rsx_texfmt f)
{
    switch (f) {
    case RSX_TEXFMT_R8G8B8A8:      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case RSX_TEXFMT_R8G8:          return DXGI_FORMAT_R8G8_UNORM;
    case RSX_TEXFMT_BC1:           return DXGI_FORMAT_BC1_UNORM;
    case RSX_TEXFMT_BC2:           return DXGI_FORMAT_BC2_UNORM;
    case RSX_TEXFMT_BC3:           return DXGI_FORMAT_BC3_UNORM;
    case RSX_TEXFMT_R16:           return DXGI_FORMAT_R16_UNORM;
    case RSX_TEXFMT_R16G16:        return DXGI_FORMAT_R16G16_UNORM;
    case RSX_TEXFMT_R16G16F:       return DXGI_FORMAT_R16G16_FLOAT;
    case RSX_TEXFMT_R16G16B16A16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case RSX_TEXFMT_R32F:          return DXGI_FORMAT_R32_FLOAT;
    case RSX_TEXFMT_R32G32B32A32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:                       return DXGI_FORMAT_R8_UNORM;
    }
}

/* Guest texture formats a VP draw samples from guest memory: every format
 * rsx_texture_layout decodes, except the depth formats. Without depth
 * write-back their guest bytes are not the depth the title rendered, and
 * they want a depth-compare sampler besides. Anything else gets a null SRV.
 * This used to be B8, A8R8G8B8, G8B8 and DXT only, so Demon's Souls's
 * D8R8G8B8 environment-light cube maps (units 11-14 of every level shader)
 * sampled 0 and the whole level rendered unlit. */
static int vp_tex_fmt_ok(u32 basef)
{
    switch (basef) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x8B: case 0x8C: case 0x8D: case 0x94: case 0x95: case 0x97:
    case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        return 1;
    default:
        return 0;
    }
}

/* Slot lookup by (bound offset, size, format, cube) through an open-
 * addressed index, rebuilt whenever a slot takes a different texture (new
 * uploads only, which cost far more than the rebuild). */
static short s_vp_tex_index[VP_TEX_HASH];   /* slot + 1, 0 = empty */
static int   s_vp_tex_index_dirty = 1;

static u32 vp_tex_hash(u32 key, u32 w, u32 h, u32 fmt, int cube)
{
    u32 x = key * 0x9E3779B1u ^ (w << 16 | h) * 0x85EBCA77u ^ (fmt << 1 | (u32)cube) * 0xC2B2AE3Du;
    return (x ^ (x >> 15)) & (VP_TEX_HASH - 1);
}

static int vp_tex_find(u32 key, u32 w, u32 h, u32 fmt, int cube)
{
    if (s_vp_tex_index_dirty) {
        memset(s_vp_tex_index, 0, sizeof s_vp_tex_index);
        for (int i = 0; i < VP_TEX_SLOTS; i++) {
            const VPTexSlot* c = &s_d3d.vp_tex[i];
            if (!GPU_VALID(c->tex)) continue;
            u32 b = vp_tex_hash(c->key, c->w, c->h, c->fmt, c->cube);
            while (s_vp_tex_index[b]) b = (b + 1) & (VP_TEX_HASH - 1);
            s_vp_tex_index[b] = (short)(i + 1);
        }
        s_vp_tex_index_dirty = 0;
    }
    for (u32 b = vp_tex_hash(key, w, h, fmt, cube); s_vp_tex_index[b]; b = (b + 1) & (VP_TEX_HASH - 1)) {
        const int i = s_vp_tex_index[b] - 1;
        const VPTexSlot* c = &s_d3d.vp_tex[i];
        if (GPU_VALID(c->tex) && c->key == key && c->w == w && c->h == h && c->fmt == fmt && c->cube == cube)
            return i;
    }
    return -1;
}

/* A slot not used this frame, round robin, so the textures a frame stops
 * using are the ones evicted, oldest first. -1 when all are in use. */
static int vp_tex_free_slot(void)
{
    static int cursor;
    for (int n = 0; n < VP_TEX_SLOTS; n++) {
        const int i = (cursor + n) % VP_TEX_SLOTS;
        if (!s_d3d.vp_tex[i].used) { cursor = (i + 1) % VP_TEX_SLOTS; return i; }
    }
    return -1;
}

/* TEX_CSUM_THREADS=<n> (live, default 2): worker threads that take the
 * sparse checksums the draw pre-pass is about to want. A held texture is
 * checked again on its first bind of every frame (vp_upload_tex_slot): about
 * a thousand reads spread over the whole texture, most of them cache misses,
 * 5.8% of the FIFO thread at the Boletarian Palace. At the start of the
 * pre-pass the workers sum the slots the previous frame bound, in the order
 * it first bound them, and a cache hit takes a worker's sum when it covers
 * the same bytes. A texture the last frame did not bind, or one no worker
 * has reached yet, is summed on the FIFO thread as before. 0 = all on the
 * FIFO thread. */
#define TCJ_THREADS_MAX 8
typedef struct { u32 off, nb; int slot; } TcjJob;
static TcjJob s_tcj_job[VP_TEX_SLOTS];
/* Per slot: the sum a worker took, as (render_frame serial << 32 | sum) in
 * one write, and the bytes its job covered (set by the FIFO thread). */
static volatile LONG64 s_tcj_pre[VP_TEX_SLOTS];
static u32 s_tcj_pre_off[VP_TEX_SLOTS], s_tcj_pre_nb[VP_TEX_SLOTS];
/* The slots this frame has bound so far, in first-bind order: the next
 * frame's jobs. */
static u16 s_tcj_order[VP_TEX_SLOTS];
static int s_tcj_order_n;
static struct {
    SRWLOCK            lk;
    CONDITION_VARIABLE cv;
    u32                gen;     /* one per dispatch */
    u32                serial;  /* the render_frame the jobs are for */
    int                want;    /* workers in use (the knob) */
    LONG               n;       /* jobs; 0 while they are rewritten */
    int                started; /* FIFO thread only */
} s_tcj = { SRWLOCK_INIT, CONDITION_VARIABLE_INIT };   /* gen..n under lk */
/* The workers' counters, each on its own cache line. */
typedef struct { volatile LONG v; char pad[60]; } TcjCounter;
static _Alignas(64) TcjCounter s_tcj_ctr[2];
#define s_tcj_next (s_tcj_ctr[0].v)   /* the next job to take */
#define s_tcj_busy (s_tcj_ctr[1].v)   /* workers inside the job loop */

static DWORD WINAPI tcj_worker(LPVOID arg)
{
    extern uint8_t* vm_base;
    const int me = (int)(intptr_t)arg;
    u32 seen = 0;
    for (;;) {
        AcquireSRWLockExclusive(&s_tcj.lk);
        while (s_tcj.gen == seen)
            SleepConditionVariableSRW(&s_tcj.cv, &s_tcj.lk, INFINITE, 0);
        seen = s_tcj.gen;
        const u64 tag = (u64)s_tcj.serial << 32;
        const LONG n = me < s_tcj.want ? s_tcj.n : 0;
        InterlockedIncrement(&s_tcj_busy);   /* under lk: see tcj_dispatch */
        ReleaseSRWLockExclusive(&s_tcj.lk);
        for (LONG j; n && (j = InterlockedIncrement(&s_tcj_next) - 1) < n; ) {
            const TcjJob* jb = &s_tcj_job[j];
            InterlockedExchange64(&s_tcj_pre[jb->slot],
                                  (LONG64)(tag | tex_csum(vm_base + jb->off, jb->nb)));
        }
        InterlockedDecrement(&s_tcj_busy);
    }
}

/* Start of the draw pre-pass: hand the previous frame's slots to the workers. */
static void tcj_dispatch(void)
{
    extern uint8_t* vm_base;
    static int want = -1;
    PS3_KNOB(want, ps3_env_int("TEX_CSUM_THREADS", 2));
    const int norder = s_tcj_order_n;
    s_tcj_order_n = 0;                    /* this frame's list starts here */
    const int use = want < TCJ_THREADS_MAX ? want : TCJ_THREADS_MAX;
    if (use <= 0 || !norder || !vm_base) return;
    /* The jobs are rewritten below. Last frame's workers finished long ago,
     * but one that wakes late must find no jobs, and none may still be in
     * the job loop: a worker raises busy under lk, as it reads n. */
    AcquireSRWLockExclusive(&s_tcj.lk);
    s_tcj.n = 0;
    ReleaseSRWLockExclusive(&s_tcj.lk);
    while (s_tcj_busy) YieldProcessor();
    LONG nj = 0;
    for (int k = 0; k < norder; k++) {
        const int i = s_tcj_order[k];
        const VPTexSlot* c = &s_d3d.vp_tex[i];
        if (!GPU_VALID(c->tex)) continue;
        rsx_tex_layout tl;
        rsx_texture_layout(c->fmt, c->w, c->h, &tl);
        /* vp_upload_tex_slot's byte count for a cache hit */
        const u32 nb = tl.compressed ? tl.row_bytes * tl.rows : c->w * c->h * tl.bytes_per_texel;
        s_tcj_pre_off[i] = c->off; s_tcj_pre_nb[i] = nb;
        s_tcj_job[nj].off = c->off; s_tcj_job[nj].nb = nb; s_tcj_job[nj].slot = i;
        nj++;
    }
    while (s_tcj.started < use) {
        HANDLE h = CreateThread(NULL, 0, tcj_worker, (LPVOID)(intptr_t)s_tcj.started, 0, NULL);
        if (!h) break;
        CloseHandle(h);
        s_tcj.started++;
    }
    AcquireSRWLockExclusive(&s_tcj.lk);
    s_tcj.serial = s_rf_serial; s_tcj.want = use; s_tcj_next = 0; s_tcj.n = nj; s_tcj.gen++;
    ReleaseSRWLockExclusive(&s_tcj.lk);
    WakeAllConditionVariable(&s_tcj.cv);
}

/* Slot i's first bind this frame: the next frame's workers sum it. */
static void tcj_order_add(int i)
{
    if (s_tcj_order_n < VP_TEX_SLOTS) s_tcj_order[s_tcj_order_n++] = (u16)i;
}

/* The checksum of slot i's source bytes [off, +nb): a worker's from this
 * render_frame when its job covered the same bytes, else taken here. */
static u32 tcj_sum(int i, u32 off, u32 nb)
{
    extern uint8_t* vm_base;
    const u64 pre = (u64)s_tcj_pre[i];
    if ((u32)(pre >> 32) == s_rf_serial && s_tcj_pre_off[i] == off && s_tcj_pre_nb[i] == nb)
        return (u32)pre;
    return tex_csum(vm_base + off, nb);
}

/* Texture staging: one upload buffer, persistently mapped, that every
 * texture upload of a frame decodes into, from the start again each frame
 * (render_frame waits for the GPU before the pre-pass uploads anything).
 *
 * Each texture slot used to own an upload buffer of its own. Creating it
 * was half of a texture's first upload: a camera turn in Demon's Souls'
 * Tutorial that brought 67 new textures into view spent 30 ms creating
 * resources and 6 ms decoding, and the frame missed the game's 36.3 ms
 * limit. The buffers also stayed allocated for as long as their slots.
 *
 * A frame that needs more than the buffer holds puts the rest into one-off
 * buffers (released the next frame), and the next frame starts with the
 * buffer grown to what that frame used, up to TEX_STAGE_MAX_MB (default
 * 512). TEX_STAGE_MB sets the starting size (default 32).
 *
 * The buffers are the GPU layer's upload buffers, mapped for their life
 * (step C4). One that a frame stops using (a one-off buffer, or the buffer
 * the next frame outgrew) is destroyed at the start of the frame after, once
 * the GPU is idle: when today's off_rt_defer_release let it go. */
static gpu_buffer      s_ts_buf;
static u8*             s_ts_map;
static u64             s_ts_cap, s_ts_off;
static gpu_buffer*     s_ts_grave;   /* destroyed by the next tex_stage_frame */
static int             s_ts_grave_n, s_ts_grave_cap;
static gpu_buffer tex_stage_create(u64 bytes, u8** map)
{
    gpu_buffer_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = bytes;
    bd.use = GPU_USE_COPY_SRC;
    bd.memory = GPU_MEM_UPLOAD;
    bd.state = GPU_STATE_GENERIC_READ;
    gpu_buffer b = gpu_buffer_create(s_rc_dev, &bd);
    if (!GPU_VALID(b)) return b;
    *map = (u8*)gpu_buffer_map(s_rc_dev, b, 0, 0);
    if (!*map) { gpu_buffer_destroy(s_rc_dev, b, 0); b.id = 0; }
    return b;
}
static void tex_stage_drop(gpu_buffer b)
{
    if (s_ts_grave_n == s_ts_grave_cap) {
        const int nc = s_ts_grave_cap ? s_ts_grave_cap * 2 : 64;
        gpu_buffer* g = (gpu_buffer*)realloc(s_ts_grave, (size_t)nc * sizeof *g);
        if (!g) { wait_for_gpu(); gpu_buffer_destroy(s_rc_dev, b, 0); return; }   /* best effort */
        s_ts_grave = g; s_ts_grave_cap = nc;
    }
    s_ts_grave[s_ts_grave_n++] = b;
}
/* Space for `bytes` of decoded texture, 512-byte aligned (a placed
 * footprint's offset must be). NULL if no buffer could be had. */
static u8* tex_stage_alloc_buf(u64 bytes, gpu_buffer* buf, u64* at)
{
    const u64 a = (s_ts_off + 511u) & ~511ull;
    s_ts_off = a + bytes;               /* counted either way: next frame's size */
    if (s_ts_map && a + bytes <= s_ts_cap) { *buf = s_ts_buf; *at = a; return s_ts_map + a; }
    u8* mp = NULL;
    const gpu_buffer b = tex_stage_create(bytes, &mp);
    if (!GPU_VALID(b)) return NULL;
    tex_stage_drop(b);                  /* read by this frame's copy; gone next frame */
    *buf = b; *at = 0;
    return mp;
}

/* Start of a frame's uploads; the GPU is idle. */
static void tex_stage_frame(void)
{
    for (int i = 0; i < s_ts_grave_n; i++) gpu_buffer_destroy(s_rc_dev, s_ts_grave[i], 0);
    s_ts_grave_n = 0;
    const u64 used = s_ts_off;
    s_ts_off = 0;
    static u64 limit = 0;
    if (!limit) limit = (u64)ps3_env_int("TEX_STAGE_MAX_MB", 512) << 20;
    u64 cap = s_ts_cap ? s_ts_cap : (u64)ps3_env_int("TEX_STAGE_MB", 32) << 20;
    while (cap < used + used / 4 && cap < limit) cap *= 2;
    if (cap > limit) cap = limit;
    if (GPU_VALID(s_ts_buf) && cap <= s_ts_cap) return;
    u8* mp = NULL;
    const gpu_buffer r = tex_stage_create(cap, &mp);
    if (!GPU_VALID(r)) {
        static int warned = 0;
        if (!warned++) fprintf(stderr, "[TEXSTAGE] no %llu MB staging buffer; staying at %llu MB%c",
                               (unsigned long long)(cap >> 20), (unsigned long long)(s_ts_cap >> 20), 10);
        limit = s_ts_cap ? s_ts_cap : 1;
        return;
    }
    if (GPU_VALID(s_ts_buf)) {
        fprintf(stderr, "[TEXSTAGE] a frame staged %llu MB of textures: buffer grown to %llu MB%c",
                (unsigned long long)(used >> 20), (unsigned long long)(cap >> 20), 10);
        tex_stage_drop(s_ts_buf);
    }
    s_ts_buf = r; s_ts_map = mp; s_ts_cap = cap;
}

/* Texture memory: placed resources in 64 MB heaps, in power-of-two blocks
 * from 64 KB to 64 MB with a free list per size, so a slot's texture
 * memory is reused when the slot takes another texture. A committed
 * resource per texture cost ~0.24 ms to create; 67 new textures on one
 * Tutorial camera turn were 16 ms of a 38 ms frame. A texture over 64 MB
 * (or TEX_PLACED=0) is still committed. Heaps are never released (the GPU
 * layer's heaps, step C4: they go with the device). */
#define TEX_POOL_CHUNK   (64ull << 20)
#define TEX_POOL_MIN     (64ull << 10)
#define TEX_POOL_CLASSES 11                  /* 64 KB << 0..10 = 64 MB */
typedef struct { gpu_heap heap; u64 off; } TexBlock;
static TexBlock*   s_tp_free[TEX_POOL_CLASSES];
static int         s_tp_free_n[TEX_POOL_CLASSES], s_tp_free_cap[TEX_POOL_CLASSES];
static gpu_heap    s_tp_chunk;
static u64         s_tp_chunk_off;
static void tex_pool_put(int k, gpu_heap heap, u64 off)
{
    if (s_tp_free_n[k] == s_tp_free_cap[k]) {
        const int nc = s_tp_free_cap[k] ? s_tp_free_cap[k] * 2 : 64;
        TexBlock* nb = (TexBlock*)realloc(s_tp_free[k], (size_t)nc * sizeof *nb);
        if (!nb) return;                     /* the block is lost, not reused */
        s_tp_free[k] = nb; s_tp_free_cap[k] = nc;
    }
    s_tp_free[k][s_tp_free_n[k]].heap = heap;
    s_tp_free[k][s_tp_free_n[k]++].off = off;
}
/* A block of class k: a freed one, or carved from the current heap. *reused
 * says the memory held another texture before. */
static int tex_pool_get(int k, gpu_heap* heap, u64* off, int* reused)
{
    if (s_tp_free_n[k]) {
        const TexBlock b = s_tp_free[k][--s_tp_free_n[k]];
        *heap = b.heap; *off = b.off; *reused = 1;
        return 1;
    }
    const u64 sz = TEX_POOL_MIN << k;
    if (!GPU_VALID(s_tp_chunk) || s_tp_chunk_off + sz > TEX_POOL_CHUNK) {
        gpu_heap_desc hd;
        memset(&hd, 0, sizeof hd);
        hd.size = TEX_POOL_CHUNK;
        hd.memory = GPU_MEM_GPU;
        hd.flags = GPU_HEAP_SAMPLED_TEXTURES;   /* any heap tier */
        const gpu_heap nh = gpu_heap_create(s_rc_dev, &hd);
        if (!GPU_VALID(nh))
            return 0;
        /* The old heap's tail goes to the free lists. Blocks are carved in
         * any order, so align each piece to its own size. */
        if (GPU_VALID(s_tp_chunk)) {
            u64 o = s_tp_chunk_off;
            while (o + TEX_POOL_MIN <= TEX_POOL_CHUNK) {
                int c = 0;
                while (c + 1 < TEX_POOL_CLASSES && !(o & ((TEX_POOL_MIN << (c + 1)) - 1)) &&
                       o + (TEX_POOL_MIN << (c + 1)) <= TEX_POOL_CHUNK) c++;
                tex_pool_put(c, s_tp_chunk, o);
                o += TEX_POOL_MIN << c;
            }
        }
        s_tp_chunk = nh; s_tp_chunk_off = 0;
    }
    /* Align to the block's own size (a 64 KB block may have left the
     * offset off a larger block's boundary); the gap goes to the lists. */
    while (s_tp_chunk_off & (sz - 1)) {
        int c = 0;
        while (TEX_POOL_MIN << (c + 1) <= sz &&
               !(s_tp_chunk_off & ((TEX_POOL_MIN << (c + 1)) - 1))) c++;
        tex_pool_put(c, s_tp_chunk, s_tp_chunk_off);
        s_tp_chunk_off += TEX_POOL_MIN << c;
    }
    if (s_tp_chunk_off + sz > TEX_POOL_CHUNK) return tex_pool_get(k, heap, off, reused);
    *heap = s_tp_chunk; *off = s_tp_chunk_off; *reused = 0;
    s_tp_chunk_off += sz;
    return 1;
}
/* The slot's texture, in COPY_DEST: the GPU layer's (step C4). 0 if it could
 * not be made. */
static int tex_res_create(VPTexSlot* t, const gpu_texture_desc* td)
{
    static int placed = -1;
    PS3_KNOB(placed, ps3_env_int("TEX_PLACED", 1));
    t->blk.id = 0; t->blk_class = -1;
    if (placed) {
        u64 size = 0, align = 0;
        gpu_texture_size(s_rc_dev, td, &size, &align);
        int k = 0;
        while (k < TEX_POOL_CLASSES && (TEX_POOL_MIN << k) < size) k++;
        gpu_heap heap; u64 off; int reused;
        if (size != UINT64_MAX && align <= TEX_POOL_MIN &&
            k < TEX_POOL_CLASSES && tex_pool_get(k, &heap, &off, &reused)) {
            gpu_texture_desc pd = *td;
            pd.heap = heap;
            pd.heap_offset = off;
            t->tex = gpu_texture_create(s_rc_dev, &pd);
            if (GPU_VALID(t->tex)) {
                t->blk = heap; t->blk_off = off; t->blk_class = k;
                if (reused) {   /* the memory held another texture */
                    gpu_barrier b;
                    memset(&b, 0, sizeof b);
                    b.type = GPU_BARRIER_ALIASING;
                    b.texture = t->tex;
                    gpu_cmd_barrier_n(s_rc_cmd, 1, &b);
                }
                return 1;
            }
            tex_pool_put(k, heap, off);
        }
    }
    t->tex = gpu_texture_create(s_rc_dev, td);   /* committed */
    return GPU_VALID(t->tex);
}
/* Release the slot's texture; a placed one's block goes back to its list.
 * The GPU is idle (render_frame waits before the pre-pass), and a slot
 * that is replaced was not used this frame, so it goes at once, as the
 * Release did. */
static void tex_res_release(VPTexSlot* t)
{
    if (!GPU_VALID(t->tex)) return;
    gpu_texture_destroy(s_rc_dev, t->tex, 0);
    t->tex.id = 0;
    if (GPU_VALID(t->blk)) tex_pool_put(t->blk_class, t->blk, t->blk_off);
    t->blk.id = 0; t->blk_class = -1;
}

static int vp_upload_tex_slot(u32 off, u32 w, u32 h, u32 fmt, int cube, u32 mips)
{
    /* TEX_BUDGET=<n>: cap texture uploads per frame. Off by default -- it was
     * tried against the TDR and does not help, because PERF shows the upload
     * path costing 0.01s for 775 calls. Kept because it is the right lever if a
     * title ever does hit an upload-bound frame. */
    { static int budget = -1;
      PS3_KNOB(budget, ps3_env_int("TEX_BUDGET", 0));
      if (budget > 0) {
          static u64 last_frame = ~0ull; static int used = 0;
          if (last_frame != (u64)s_d3d.frame_count) { last_frame = (u64)s_d3d.frame_count; used = 0; }
          if (++used > budget) return -1;   /* try again next frame */
      } }
    extern uint8_t* vm_base;

    if (!off || !w || !h || !vm_base || !s_d3d.srv_heap) return -1;
    /* Format classes (base = fmt & 0x9F). The LBP loading screen uses:
     * 0x85 A8R8G8B8 (swizzled UI art), 0x8B G8B8 (the 1024x2048 linear FONT
     * atlas -- without it no text renders at all), 0x86/87/88 DXT1/23/45
     * (512x512 detail/LUT layers bound at t1/t3 on every draw). */
    /* Format class, row sizes and swizzling are RSX semantics: rsx_texture_
     * layout.c owns them so the Metal backend can read a guest texture without
     * re-deriving any of it. Only the DXGI mapping below is ours. */
    rsx_tex_layout tl;
    rsx_texture_layout(fmt, w, h, &tl);
    u32 basef = fmt & 0x9F;              /* still needed for the SRV remap */
    int argb = (tl.fmt == RSX_TEXFMT_R8G8B8A8);
    int dxt  = tl.compressed;
    /* Compressed rows are counted in blocks, so keep bpp at 1 for the byte
     * arithmetic the diagnostics below do. */
    u32 bpp = tl.compressed ? 1u : tl.bytes_per_texel;
    /* The DECODED image's format, which is not always the guest's: the packed
     * 16-bit colour formats arrive here unpacked to RGBA8, and DEPTH24_D8 as a
     * float. rsx_texture_layout.h says which class each format byte lands in. */
    DXGI_FORMAT dxfmt = rsx_texfmt_dxgi(tl.fmt);
    /* DXT data is stored as linear 4x4 block rows (compressed formats are
     * never Morton-swizzled on RSX). Row of blocks = (w/4)*blocksize. */
    u32 blkrow  = dxt ? tl.row_bytes : 0;
    u32 blkrows = dxt ? tl.rows      : 0;
    const u32 key_off = off;         /* lookup key: the offset as bound */
    /* Sparse checksum of a texture's source bytes -- enough to notice an
     * animated surface changing, cheap enough to run on every bind. */
    /* (portable helper tex_csum() -- see above; a statement-expression macro
     * here was a GCC/Clang extension MSVC rejects.) */
    #define TEX_CSUM(base, nbytes) tex_csum((base), (nbytes))
    int slot = -1, freeslot = -1;
    for (int i = vp_tex_find(key_off, w, h, fmt, cube); i >= 0; i = -1) {
        VPTexSlot* c = &s_d3d.vp_tex[i];
        {
            if (c->used) return i;                /* already bound this frame */
            { static int nocache = -1;            /* TEX_NOCACHE=1 (live): always re-upload */
              PS3_KNOB(nocache, ps3_env_int("TEX_NOCACHE", 0));
              if (nocache) { slot = i; break; } }
            /* Held from an earlier frame: re-upload only if the guest bytes
             * changed. Most of this scene's textures are static, and converting
             * every one of them every frame was ~70% of the frame's CPU time. */
            u32 nb = dxt ? (blkrow * blkrows) : (w * h * bpp);
            u32 cs = tcj_sum(i, c->off, nb);
            /* TEX_VERIFY=1 (live): hash every byte of each cache hit as well,
             * and re-upload one the sparse checksum called unchanged. Slow; it
             * tells whether a held texture is ever stale. */
            { static int verify = -1;
              PS3_KNOB(verify, ps3_env_int("TEX_VERIFY", 0));
              if (verify && cs == c->csum) {
                  u32 fs = tex_csum_full(vm_base + c->off, nb);
                  if (!c->fsum) c->fsum = fs;   /* uploaded before TEX_VERIFY went on */
                  if (fs != c->fsum) {
                      static int n = 0;
                      if (n < 40 && ++n) fprintf(stderr, "[TEX] stale: 0x%X %ux%u fmt 0x%02X (%u bytes) "
                                            "changed, sparse checksum did not (frame %llu)%c",
                                            c->off, w, h, fmt, nb,
                                            (unsigned long long)s_d3d.frame_count, 10);
                      slot = i; break;
                  }
              } }
            if (cs == c->csum) { c->used = 1; tcj_order_add(i); return i; }
            slot = i; break;                      /* stale: fall through and redo */
        }
    }
    if (slot < 0) freeslot = vp_tex_free_slot();
    /* TEX_OFF_BIAS=1: sample the image one whole level-0 BELOW the bound offset.
     * Measured on the Rubber Ducky demo: at the bind offset every texture reads
     * as all-zero, while [off - w*h*bpp, off) holds the image (3800-4160 of 4298
     * sampled bytes non-zero, for four different textures at three sizes). So
     * the offset the guest programs points PAST level 0 rather than at it. */
    /* TEX_OFF_BIAS: 1 = one level-0 below the bound offset, 2 = one full MIP
     * CHAIN above it. The measured upload-to-bind deltas on the Rubber Ducky
     * demo are 0x2AAB00 for a 2MB level 0 and 0x555580 for a 4MB one -- both
     * exactly the mipmap pyramid total (L0 * 4/3), which is what the guest
     * reserves per texture. */
    { static int bias = -1;
      PS3_KNOB(bias, ps3_env_int("TEX_OFF_BIAS", 0));
      if (bias == 1) { u32 sz = w * h * bpp; if (off > sz) off -= sz; }
      else if (bias >= 2) {
          u32 chain = 0;
          for (u32 mw = w, mh = h; mw && mh; mw >>= 1, mh >>= 1) {
              chain += mw * mh * bpp;
              if (mw == 1 && mh == 1) break;
          }
          /* bias 3: only shift when the bound offset really is empty, so a
           * texture that IS bound correctly keeps its own data. Applying the
           * chain unconditionally fixes the mis-bound majority but breaks any
           * correctly-bound minority, which shows up as objects going black. */
          int shift = 1;
          if (bias >= 3) {
              u32 nz = 0, sz = w * h * bpp;
              for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
              shift = (nz == 0);
          }
          if (shift) off += chain;
      } }
    /* TEX_REMAP=1: if the bound offset holds nothing, look the image up by size
     * in the VRAM upload registry (cellGcmSys). For a title whose upload address
     * and SET_TEXTURE_OFFSET disagree this puts the real bytes under the sampler
     * without guessing a delta. */
    { static int remap = -1;
      PS3_KNOB(remap, ps3_env_int("TEX_REMAP", 0));
      if (remap) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
        if (!nz) {
            extern u32 rsx_find_vram_upload(u32);
            u32 alt = rsx_find_vram_upload(sz);
            if (alt) {
                static int _n = 0;
                if (_n < 8 && ++_n) fprintf(stderr, "[TEXREMAP] 0x%08X (%ux%u, %u bytes) -> 0x%08X%c",
                                      off, w, h, sz, alt, 10);
                off = alt;
            }
        } } }
    /* A stale cache entry (slot >= 0) re-uses its own slot; otherwise take a
     * free one. Without this the stale path fell through to freeslot and either
     * duplicated the texture into a second slot or bailed when none was free. */
    if (slot < 0 && freeslot < 0) {               /* out of slots this frame */
        static int warned;
        if (!warned) { warned = 1;
            fprintf(stderr, "[TEX] all %d texture slots in use this frame: later textures "
                            "draw unbound%c", VP_TEX_SLOTS, 10); }
        return -1;
    }
    /* TEX_SRCDBG=<N>: is the guest memory this slot uploads FROM actually
     * populated? An empty source and a broken sampler both render flat. */
    { static int cap = -1, n = 0;
      PS3_KNOB(cap, ps3_env_int("TEX_SRCDBG", 0));
      if (cap && n < cap) { n++;
        u32 nz = 0, tot = 0;
        for (u32 i = 0; i < w * h * bpp && i < 0x40000u; i += 61) { tot++; if (vm_base[off + i]) nz++; }
        /* Several of this title's textures are bound at EXACTLY the end address
         * of an upload, so also sample the block immediately BELOW the bind
         * offset: if that holds the image, the bind is one whole texture high. */
        u32 nzb = 0, totb = 0;
        { u32 sz = w * h * bpp; u32 base = (off > sz) ? off - sz : 0;
          for (u32 i = 0; i < sz && i < 0x40000u; i += 61) { totb++; if (vm_base[base + i]) nzb++; } }
        long nearest = 0; u32 found = 0;
        if (!nz) {   /* empty: where IS the data? scan out from the bind offset */
            for (long d = 0x1000; d <= 0x1000000 && !found; d += 0x1000) {
                for (int s = 0; s < 2 && !found; s++) {
                    long a = (long)off + (s ? -d : d);
                    if (a < 0x1000) continue;
                    u32 c = 0;
                    for (u32 i = 0; i < 0x1000u; i += 61) if (vm_base[a + i]) c++;
                    if (c > 8) { nearest = (s ? -d : d); found = c; }
                }
            }
        }
        fprintf(stderr, "[TEXSRC] off=0x%08X %ux%u fmt=0x%02X bpp=%u nonzero=%u/%u"
                        "%s%+ld (page nonzero=%u)\n",
                off, w, h, fmt, bpp, nz, tot,
                nz ? "" : "  nearest data at ", nz ? 0L : nearest, found);
        fprintf(stderr, "[TEXSRC]    block BELOW bind (off-%u): nonzero=%u/%u%c",
                w * h * bpp, nzb, totb, 10);
        /* Walk DOWN in 4KB steps to the first populated page whose predecessor
         * is empty: that is where this texture's data actually begins, and the
         * delta from the bound offset is the rule we are missing. */
        { u32 step = 0x1000, prev_pop = 0; long start_delta = 0;
          for (long d = 0; d >= -(long)(w * h * bpp) * 2; d -= step) {
              u32 a = (u32)((long)off + d); u32 pop = 0;
              if (a < 0x1000) break;
              for (u32 i = 0; i < step; i += 53) if (vm_base[a + i]) pop++;
              if (!pop && prev_pop) { start_delta = d + step; break; }
              prev_pop = pop;
          }
          fprintf(stderr, "[TEXSRC]    data starts at off%+ld (level0=%u bytes)%c",
                  start_delta, w * h * bpp, 10); } } }
    if (slot < 0) slot = freeslot;
    VPTexSlot* t = &s_d3d.vp_tex[slot];
    /* The upload buffer's rows hold the DECODED image, so they are sized from
     * dst_row_bytes -- w * bpp is the source row, which is narrower for any
     * format rsx_texture_decode unpacks. */
    u32 pitch = (tl.dst_row_bytes + 255) & ~255u;
    int fresh = 0;

    /* Also recreate when the CUBE-ness changes: a slot holding a 2D texture of
     * the same dimensions would otherwise be reused with 1 array slice while the
     * cube path copies 6 subresources and writes a 6x upload buffer -- which
     * crashes. */
    if (GPU_VALID(t->tex) && (t->w != w || t->h != h || t->fmt != fmt || t->cube != cube))
        tex_res_release(t);
    if (!GPU_VALID(t->tex)) {
        gpu_texture_desc td;
        memset(&td, 0, sizeof td);
        td.width = w; td.height = h;
        td.layers = cube ? 6 : 1;   /* cube = 6 array slices */
        td.mips = 1;
        td.format = (gpu_format)dxfmt; td.samples = 1;
        td.use = GPU_USE_SAMPLED | GPU_USE_COPY_DST;
        td.state = GPU_STATE_COPY_DEST;
        if (!tex_res_create(t, &td))
            return -1;
        fresh = 1;
    }

    gpu_buffer up = {0}; u64 up_at = 0;
    void* mapped = tex_stage_alloc_buf((u64)pitch * (dxt ? blkrows : h) * (cube ? 6 : 1), &up, &up_at);
    if (!mapped) {
        /* A fresh resource has never been through the copy below, so it
         * would be left in COPY_DEST: drop it and try again next frame. */
        if (fresh) tex_res_release(t);
        return -1;
    }
    { static int _tp=0; if (getenv("RTT_DUMP") && _tp < 6 && ++_tp) {
        const u8* sp = vm_base + off;
        fprintf(stderr, "[TEXUP] off=0x%X fmt=0x%X row0:", off, fmt);
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[_b]);
        fprintf(stderr, "  row240:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+_b]);
        fprintf(stderr, "  mid:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+320*bpp+_b]);
        fprintf(stderr, "%s", "\n");
    } }
    /* Swizzled (LN bit 0x20 clear): texels live in Morton/Z-order, not rows.
     * Uploading them as linear rows produced the diagonal-stripe garbage on
     * LBP's loading screen (every texture there is 0x85 swizzled). POT dims
     * only -- the hardware requires that for swizzled textures anyway. */
    int swz = tl.swizzled;
    /* Cube textures store their 6 faces consecutively. Convert each into its own
     * slice of the upload buffer; the conversion below is unchanged and simply
     * runs once per face with off/mapped rebased. */
    const u32 _face_rows  = dxt ? blkrows : h;
    /* The face stride is NOT one mip-0 image: RSX stores each cube face as its
     * own complete mip pyramid, so face f starts a whole pyramid (128-byte
     * aligned) in. Assuming mip-0-sized strides made face 1 land inside face 0's
     * mip chain -- which is exactly what the dumps showed, every face after the
     * first a progressively smaller copy of the first. */
    u32 _face_bytes = dxt ? (blkrow * blkrows) : (w * h * bpp);
    if (cube) {
        u32 total = 0;
        for (u32 lw = w, lh = h, l = 0; l < (mips ? mips : 1u); l++) {
            total += dxt ? (((lw + 3) / 4) * (bpp == 8 ? 16u : 8u) * ((lh + 3) / 4))
                         : (lw * lh * bpp);
            if (lw > 1) lw >>= 1;
            if (lh > 1) lh >>= 1;
        }
        _face_bytes = (total + 127u) & ~127u;
    }
    const u32 _nfaces     = cube ? 6u : 1u;
    const u32 _off0 = off; u8* const _map0 = (u8*)mapped;
    for (u32 _f = 0; _f < _nfaces; _f++) {
    off    = _off0 + _f * _face_bytes;
    if (cube && getenv("CUBEDBG")) { u32 _s=0; const u8* _p = vm_base + off;
        for (u32 _i = 0; _i < _face_bytes; _i += 97) _s = _s*131 + _p[_i];
        fprintf(stderr, "[CUBEFACE] f=%u off=0x%X bytes=%u bpp=%u swz=%d csum=%08X%c",
                _f, off, _face_bytes, bpp, swz, _s, 10); }
    mapped = _map0 + (u64)_f * pitch * _face_rows;
    /* Pixel conversion -- deswizzle, channel order, block copy -- is RSX
     * semantics and lives in rsx_texture_layout.c, so a second backend gets
     * it by calling this rather than porting the loops again. */
    rsx_texture_decode(mapped, pitch, vm_base + off, w, h, &tl,
                       rsx_texture_argb_is_rgba());

    /* The diagnostics below only apply to the linear 8-bit case (Bink video
     * planes); they used to sit inside that branch of the conversion. */
    if (!tl.compressed && tl.bytes_per_texel == 1 && !tl.swizzled) {
        /* MOVIE_FIND=1: when a 640x360 movie plane uploads ZERO, scan a wide
         * guest window for the REAL frame (a 640-wide region with content) so
         * we learn where the video decode actually wrote vs where the texture
         * points -- ends the frame-buffer-address guessing. */
        if (getenv("MOVIE_FIND") && w == 640 && h == 360) {
            extern uint8_t* vm_base;
            const u8* here = vm_base + off;
            u32 hz = 0; for (u32 i=0;i<w*h;i+=137) if (here[i]) { hz=1; break; }
            static int _mf = 0;
            if (!hz && _mf < 3 && ++_mf) {
                fprintf(stderr, "[movie-find] bound plane ea=0x%X is ZERO; full scan...\n", off);
                /* Scan FULL VRAM + main heap in 0x8000 steps for a 640x360
                 * content block -- NO early cap (the previous found<6 stopped in
                 * the low-VRAM display buffers before ever reaching the movie
                 * region ~0x40E80000). Report the near-plane hits explicitly so
                 * ring-desync (frame present at a DIFFERENT slot) is separable
                 * from decode-never-ran (region entirely zero). */
                struct { u32 lo, hi; const char* tag; } rng[] = {
                    {0x40000000u, 0x41000000u, "VRAM"}, {0x00100000u, 0x10000000u, "MAIN"},
                    {0x48000000u, 0x4A000000u, "BINK"} };  /* Bink working set (handle ~0x4849E760, SPU I/O 0x4847/0x4945) */
                u32 plane_lo = (off > 0x400000u) ? off - 0x400000u : 0;
                u32 plane_hi = off + 0x400000u;
                int found = 0, nearc = 0;
                for (int r = 0; r < (int)(sizeof(rng)/sizeof(rng[0])); r++) {
                    u32 best_a = 0, best_nz = 0; u32 best_span = 0;
                    for (u32 a = rng[r].lo; a + w*h < rng[r].hi; a += 0x8000) {
                        const u8* p = vm_base + a;
                        u32 nz = 0; u8 mn2 = 255, mx2 = 0;
                        for (u32 i = 0; i < w*h; i += 257) { u8 v = p[i]; if (v) nz++; if (v<mn2) mn2=v; if (v>mx2) mx2=v; }
                        u32 span = mx2 - mn2;
                        if (nz > best_nz) { best_nz = nz; best_a = a; best_span = span; }
                        if (nz > 400 && span > 60) {  /* looks like image content */
                            int isnear = (a >= plane_lo && a <= plane_hi);
                            if (isnear) nearc++;
                            if (found < 40)
                                fprintf(stderr, "[movie-find]   CONTENT @0x%08X (%s) nz=%u span=%u%s\n",
                                        a, rng[r].tag, nz, span, isnear ? " <== NEAR bound plane" : "");
                            found++;
                        }
                    }
                    /* Always report the densest block in this range even below the
                     * content threshold -- the MM intro fades in from black, so the
                     * real first frames are low-span and would otherwise be invisible. */
                    fprintf(stderr, "[movie-find]   %s best @0x%08X nz=%u span=%u%s\n",
                            rng[r].tag, best_a, best_nz, best_span,
                            (best_a >= plane_lo && best_a <= plane_hi) ? " <== NEAR bound plane" : "");
                    /* Dump the densest block as a 640x360 grayscale BMP so the
                     * "is this the decoded movie frame" question is answered by
                     * eye. One dump per range per process. */
                    if (best_nz > 400 && best_span > 40) {
                        static int _bd[4] = {0,0,0,0};
                        if (r < 4 && !_bd[r]) { _bd[r] = 1;
                            const u8* sp = vm_base + best_a;
                            char pn[128];
                            snprintf(pn, sizeof(pn), "binkscan_%s_%08X.bmp", rng[r].tag, best_a);
                            FILE* f = fopen(pn, "wb");
                            if (f) {
                                u32 rowb = w*3, rowp = (rowb+3)&~3u, fsz = 54 + rowp*h;
                                u8 hd[54] = {'B','M'};
                                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                                hd[10]=54; hd[14]=40;
                                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                                hd[26]=1; hd[28]=24;
                                fwrite(hd,1,54,f);
                                for (int y=(int)h-1; y>=0; y--) {
                                    const u8* srow = sp + (u64)y*w;
                                    for (u32 x=0;x<w;x++){ u8 px[3]; px[0]=px[1]=px[2]=srow[x]; fwrite(px,1,3,f); }
                                    { u8 z[3]={0,0,0}; fwrite(z,1,rowp-rowb,f); }
                                }
                                fclose(f);
                                fprintf(stderr, "[movie-find]   dumped %s\n", pn);
                            }
                        }
                    }
                }
                fprintf(stderr, "[movie-find]   total=%d near-plane=%d (window 0x%X..0x%X)\n",
                        found, nearc, plane_lo, plane_hi);
                if (!found) fprintf(stderr, "[movie-find]   no 640x360 content block (>thresh) found anywhere\n");
            }
        }
        /* TEX_SAVE=1: also dump wide B8 uploads (Bink video planes are B8 --
         * the Y plane IS the movie frame in grayscale) + a content stat, so
         * "is the decoder producing pixels" is answerable by looking at a BMP. */
        if (getenv("TEX_SAVE") && w >= 256) {
            static int _bs = 0;
            const u8* sp = vm_base + off;
            u32 nz = 0; u8 mn = 255, mx = 0;
            for (u32 i = 0; i < w * h; i += 17) {
                u8 v = sp[i]; if (v) nz++; if (v < mn) mn = v; if (v > mx) mx = v;
            }
            fprintf(stderr, "[TEXB8] off=0x%X %ux%u nz=%u/%u min=%u max=%u\n",
                    off, w, h, nz, (w * h) / 17, mn, mx);
            if (_bs < 8 && mx > mn) { _bs++;
                char pn[128];
                snprintf(pn, sizeof(pn), "texb8_%08X_%ux%u_%d.bmp", off, w, h, _bs);
                FILE* f = fopen(pn, "wb");
                if (f) {
                    u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                    u32 fsz = 54 + rowp * h;
                    u8 hd[54] = {'B','M'};
                    hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                    hd[10]=54; hd[14]=40;
                    hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                    hd[26]=1; hd[28]=24;
                    fwrite(hd, 1, 54, f);
                    for (int y = (int)h - 1; y >= 0; y--) {
                        const u8* srow = sp + (u64)y * w;
                        for (u32 x = 0; x < w; x++) {
                            u8 px[3]; px[0]=px[1]=px[2]=srow[x];
                            fwrite(px, 1, 3, f);
                        }
                        { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                    }
                    fclose(f);
                    fprintf(stderr, "[TEX_SAVE] %s\n", pn);
                }
            }
        }
    }
    /* TEX_SAVE=1: dump the first few converted ARGB uploads as BMPs (rgb +
     * alpha channel separately) -- ground-truth for "is the guest texture
     * wrong or is the sampling wrong" questions (wave's hue palette). */
    if (argb && _f + 1 == _nfaces && getenv("TEX_SAVE")) { static int _ts = 0; if (_ts < 8) { _ts++;
        /* For a cube, write every face: the point of the dump is to check the
         * assumed 6-face layout against the game's cubemap art. */
        for (u32 _face = 0; _face < _nfaces; _face++)
        for (int pass = 0; pass < 2; pass++) {
            char pn[128];
            if (_nfaces > 1)
                snprintf(pn, sizeof(pn), "cube_%08X_%ux%u_f%u_%s.bmp", _off0, w, h,
                         _face, pass ? "a" : "rgb");
            else
                snprintf(pn, sizeof(pn), "tex_%08X_%ux%u_%s.bmp", off, w, h,
                     pass ? "a" : "rgb");
            FILE* f = fopen(pn, "wb");
            if (f) {
                u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                u32 fsz = 54 + rowp * h;
                u8 hd[54] = {'B','M'};
                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                hd[10]=54; hd[14]=40;
                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                hd[26]=1; hd[28]=24;
                fwrite(hd, 1, 54, f);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = _map0 + (u64)_face * pitch * _face_rows
                                     + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        u8 px[3];
                        if (pass) { px[0]=px[1]=px[2]=srow[x*4+3]; }
                        else { px[0]=srow[x*4+2]; px[1]=srow[x*4+1]; px[2]=srow[x*4+0]; }
                        fwrite(px, 1, 3, f);
                    }
                    { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                }
                fclose(f);
                fprintf(stderr, "[TEX_SAVE] %s\n", pn);
            }
        }
    } }
    /* TEX_AVGDBG=1: one line per distinct bound texture offset with its average
     * source colour. duck.tga is overwhelmingly yellow (avg ~243,191,23), so this
     * says whether the duck's image is bound at all -- and if it is, the duck is
     * being drawn somewhere and the question is where, not whether. */
    { static int avgd = -1;
      PS3_KNOB(avgd, ps3_env_int("TEX_AVGDBG", 0));
      if (avgd && !dxt && bpp == 4) {
        static u32 seen[64]; static int nseen = 0;
        int known = 0;
        for (int i2 = 0; i2 < nseen; i2++) if (seen[i2] == off) { known = 1; break; }
        if (!known && nseen < 64) {
            seen[nseen++] = off;
            /* Report the source as R,G,B,A -- the layout TEX_RGBA uploads (PSGL
             * writes GL_RGBA/UNSIGNED_INT_8_8_8_8, so bytes land R,G,B,A). Alpha
             * matters as much as colour here: a blended surface whose alpha is a
             * flat 255 renders opaque no matter how correct the blend state is. */
            u64 sr = 0, sg = 0, sb = 0, sa = 0; u32 cnt = 0, amin = 255, amax = 0;
            for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
                const u8* q = vm_base + off + i2;
                sr += q[0]; sg += q[1]; sb += q[2]; sa += q[3]; cnt++;
                if (q[3] < amin) amin = q[3];
                if (q[3] > amax) amax = q[3];
            }
            if (cnt) fprintf(stderr, "[TEXAVG] raw=0x%08X %4ux%-4u fmt=0x%02X"
                                     " avgRGBA=(%3u,%3u,%3u,%3u) alpha[%u..%u]%c",
                             key_off, w, h, fmt, (u32)(sr/cnt), (u32)(sg/cnt),
                             (u32)(sb/cnt), (u32)(sa/cnt), amin, amax, 10);
        }
      } }
    /* Content-identify the duck's texture so filters can say "duck". */
    if (!dxt && bpp == 4 && w == 512 && h == 512 && s_duck_raw != key_off) {
        u64 ar = 0, ag = 0, ab = 0; u32 n = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 211) {
            const u8* q = vm_base + off + i2;
            ar += q[0]; ag += q[1]; ab += q[2]; n++;
        }
        if (n && ar/n > 200 && ag/n > 140 && ab/n < 90) {
            s_duck_off = key_off;
            { static u32 last = 0; if (last != key_off) { last = key_off;
                fprintf(stderr, "[DUCKTEX] resolved=0x%08X identified by content%c", key_off, 10); } }
        }
    }
    /* TEX_HILITE=1: paint the duck's texture pure red. duck.tga is overwhelmingly
     * yellow (avg 243,191,23); with the R,G,B,A source order that is bytes
     * s[0] high, s[1] mid, s[2] low. Flooding it with an unmistakable colour
     * answers "where on screen is the duck drawn" directly, which neither the
     * vertex data nor a pixel count can. Diagnostic only. */
    { static int hil = -1;
      PS3_KNOB(hil, ps3_env_int("TEX_HILITE", 0));
      if (hil && !dxt && bpp == 4 && w == 512 && h == 512) {
        u64 sr = 0, sg = 0, sb = 0; u32 cnt = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
            const u8* q = vm_base + off + i2;
            sr += q[0]; sg += q[1]; sb += q[2]; cnt++;
        }
        if (cnt) {
            u32 ar = (u32)(sr/cnt), ag = (u32)(sg/cnt), ab = (u32)(sb/cnt);
            if (ar > 200 && ag > 140 && ab < 90) {
                for (u32 y = 0; y < h; y++) {
                    u8* drow = (u8*)mapped + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                        drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                    }
                }
                { static int _n = 0; if (_n < 4 && ++_n)
                    fprintf(stderr, "[TEXHILITE] duck texture off=0x%08X avg=(%u,%u,%u) -> red%c",
                            off, ar, ag, ab, 10); }
            }
        }
      } }
    /* TEX_MARKEMPTY=1: paint any texture whose guest source is entirely zero a
     * flat magenta instead of uploading the empty bytes. An unresolved texture
     * and a legitimately black surface are indistinguishable on screen, so this
     * says WHICH geometry is missing its image -- diagnostic only. */
    { static int mark = -1;
      PS3_KNOB(mark, ps3_env_int("TEX_MARKEMPTY", 0));
      if (mark && !dxt) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i2 = 0; i2 < sz && i2 < 0x40000u; i2 += 61) if (vm_base[off + i2]) nz++;
        if (!nz) {
            for (u32 y = 0; y < h; y++) {
                u8* drow = (u8*)mapped + (u64)y * pitch;
                for (u32 x = 0; x < w; x++) {
                    /* Pure RED. Magenta collided with this title's wall tint,
                     * so the marker was indistinguishable from real geometry. */
                    drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                    drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                }
            }
            { static int _n = 0; if (_n < 8 && ++_n)
                fprintf(stderr, "[TEXEMPTY] off=0x%08X %ux%u fmt=0x%02X -> magenta%c",
                        off, w, h, fmt, 10); }
        }
      } }
    }   /* end per-face conversion */
    off = _off0; mapped = _map0;

    /* On the frame's recorder (step C4): the barriers, one copy per face,
     * the barriers back. */
    if (!fresh)     /* reused resource: PSR -> COPY_DEST first */
        gpu_cmd_barrier(s_rc_cmd, t->tex, GPU_STATE_PIXEL_SHADER_RESOURCE, GPU_STATE_COPY_DEST);
    gpu_footprint fp;
    fp.format    = (gpu_format)dxfmt;
    fp.width     = w;
    fp.height    = h;
    fp.depth     = 1;
    fp.row_pitch = pitch;
    for (u32 _f = 0; _f < _nfaces; _f++) {    /* cube face = array slice */
        fp.offset = up_at + (u64)_f * pitch * _face_rows;
        gpu_cmd_copy_buf_to_tex(s_rc_cmd, t->tex, _f, 0, 0, 0, up, &fp);
    }
    gpu_cmd_barrier(s_rc_cmd, t->tex, GPU_STATE_COPY_DEST, GPU_STATE_PIXEL_SHADER_RESOURCE);
    /* SRV at heap slot VP_TEX_SRV_BASE+slot */
    gpu_view_desc sv;
    memset(&sv, 0, sizeof sv);
    sv.texture = t->tex;
    sv.kind = GPU_VIEW_SRV;
    sv.dim = cube ? GPU_DIM_CUBE : GPU_DIM_2D;   /* a cube must match the TextureCube declaration in the HLSL */
    sv.mips = 1;
    sv.format = (gpu_format)dxfmt;
    /* RSX B8 replicates the byte into all four channels (dbgfont's FP reads
     * coverage from .w); DXGI R8 defaults to (r,0,0,1), so swizzle (R,R,R,R)
     * = encoded 0x1000 (component 0 in all lanes + always-set bit). ARGB8
     * keeps the identity mapping. */
    sv.swizzle = (basef == 0x81) ? 0x1000 : GPU_SWIZZLE_IDENTITY;
    gpu_table_write_tex(s_rc_dev, s_rc_srv_pool, VP_TEX_SRV_BASE + (u32)slot, &sv);

    if (t->key != key_off || t->w != w || t->h != h || t->fmt != fmt || t->cube != cube)
        s_vp_tex_index_dirty = 1;
    t->off = off; t->key = key_off; t->w = w; t->h = h; t->fmt = fmt;
    t->cube = cube; t->used = 1; t->face_bytes = _face_bytes;
    tcj_order_add(slot);
    { u32 nb = dxt ? (blkrow * blkrows) : (w * h * bpp);
      t->csum = TEX_CSUM(vm_base + off, nb);
      t->fsum = ps3_env_int("TEX_VERIFY", 0) ? tex_csum_full(vm_base + off, nb) : 0; }
    #undef TEX_CSUM
    return slot;
}

