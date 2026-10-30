/* Driver pipelines on disk. The .dxbc cache above saves FXC, but every new
 * PSO still has the driver compile DXBC to GPU code: 0.1 ms when the driver's
 * own cache has it, 7-20 ms when it does not, and a new area asks for a dozen
 * in one frame (PERF=1 "pipelines created"). The driver's cache belongs to the
 * executable, so a rebuilt exe starts cold. Ours, in the shader cache dir:
 *
 *   gpu/d3d12/pipelines.lib  the GPU layer's pipeline library
 *                    (gpu_pipeline_cache_load/save; on D3D12 an
 *                    ID3D12PipelineLibrary): the compiled pipelines, named by
 *                    the GPU layer's hash of the whole description (call map
 *                    G9), so it is not the d3d12 route's pso_library.bin,
 *                    which this route never reads or writes. Only good for the
 *                    driver and GPU that wrote it; another one fails to load.
 *   gpu/d3d12/pipelines.n    how many manifest records the saved library holds.
 *   gpu/d3d12/index.built    the pipeline index's build mark (rsxc_pipeline.inc).
 *   pso_manifest.bin every pipeline ever created, as a description that can
 *                    be rebuilt without the guest (fixed-size records): the
 *                    d3d12 route's file, with the same records (the D3D12
 *                    description the GPU layer makes of a pipeline is
 *                    today's, gpu_d3d12_pipeline_d3d_desc).
 *   pso_blobs/       the shader bytecode those records name, by content hash.
 *
 * At startup, worker threads build every manifest record the library does not
 * hold yet (all of them after a driver update), before the title asks for
 * them; a draw then only loads its pipeline from the library. A pipeline no
 * session has met yet is still compiled on first use, once.
 * PS3_PSO_LIBRARY=0 turns all of it off; PS3_PSO_PREBUILD=0 keeps the library
 * but skips the startup build. */
static int           s_pso_lib_on;          /* the GPU layer has its library (pso_lib_open) */
static volatile LONG s_pso_lib_new;         /* pipelines stored since the last save */
static volatile LONG s_pso_lib_saving;
static volatile LONG s_pso_man_pending;     /* manifest records queued or being written */
static u32           s_pso_lib_quiet;       /* presents since the last store */
static volatile LONG s_pso_lib_hits, s_pso_lib_live;   /* draws: loaded / compiled */
static SRWLOCK       s_pso_man_lock = SRWLOCK_INIT;   /* the manifest's names and file (pso_manifest_write) */

#define PSO_REC_MAGIC 0x314F5350u           /* "PSO1" */
typedef struct {
    u32 magic, size;
    u64 name, vs_hash, ps_hash;
    u32 vs_len, ps_len, nel, pad;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;   /* pointers zeroed */
    struct { char sem[16]; D3D12_INPUT_ELEMENT_DESC e; } el[16];
} pso_rec;
static pso_rec*      s_pso_man;             /* records read at startup */
static u32           s_pso_man_n;           /* records in the file (read + appended) */
static u32           s_pso_man_read;
static u64*          s_pso_names;           /* open-addressed set of record names */
static u32           s_pso_names_cap, s_pso_names_n;
static u32           s_pso_lib_have;        /* records the loaded library holds */
static volatile LONG s_pso_pre_next, s_pso_pre_live, s_pso_pre_built, s_pso_pre_had, s_pso_pre_abort;
static int           s_pso_pre_started;
static double        s_pso_pre_t0;

static u64 pso_desc_key(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC c;
    memcpy(&c, pd, sizeof c);
    c.pRootSignature = NULL;
    c.VS.pShaderBytecode = c.PS.pShaderBytecode = c.DS.pShaderBytecode = NULL;
    c.HS.pShaderBytecode = c.GS.pShaderBytecode = NULL;
    c.StreamOutput.pSODeclaration = NULL; c.StreamOutput.pBufferStrides = NULL;
    c.InputLayout.pInputElementDescs = NULL; c.CachedPSO.pCachedBlob = NULL;
    /* The root signature by its serialized bytes: every caller passes
     * vp_layout. Callers zero the whole description first, so padding hashes
     * the same every run. */
    u64 h = fnv_bytes(14695981039346656037ull, &s_vp_root_hash, sizeof s_vp_root_hash);
    h = fnv_bytes(h, &c, sizeof c);
    const D3D12_SHADER_BYTECODE* sh[5] = { &pd->VS, &pd->PS, &pd->DS, &pd->HS, &pd->GS };
    for (int i = 0; i < 5; i++)
        if (sh[i]->pShaderBytecode) h = fnv_bytes(h, sh[i]->pShaderBytecode, sh[i]->BytecodeLength);
    for (UINT i = 0; i < pd->InputLayout.NumElements; i++) {
        D3D12_INPUT_ELEMENT_DESC e = pd->InputLayout.pInputElementDescs[i];
        h = fnv_bytes(h, e.SemanticName, strlen(e.SemanticName));
        e.SemanticName = NULL;
        h = fnv_bytes(h, &e, sizeof e);
    }
    return h;
}
static void pso_path(char* out, size_t n, const char* leaf)
{
    snprintf(out, n, "%s/%s", shader_cache_dir(), leaf);
}
/* Through a temporary name and a rename, so a crash or a second process never
 * leaves a half-written file under the real name. */
static int pso_write_file(const char* path, const void* p, size_t n)
{
    char tmp[MAX_PATH + 64];
    snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetCurrentThreadId());
    FILE* f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(p, 1, n, f) == n;
    if (fclose(f)) ok = 0;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp); return 0; }
    return 1;
}
static void* pso_read_file(const char* path, long* n)
{
    *n = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    void* p = sz > 0 ? malloc((size_t)sz) : NULL;
    if (p && fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); p = NULL; }
    fclose(f);
    if (p) *n = sz;
    return p;
}
static int pso_names_has(u64 k)
{
    if (!s_pso_names_cap) return 0;
    for (u32 i = (u32)k & (s_pso_names_cap - 1); s_pso_names[i]; i = (i + 1) & (s_pso_names_cap - 1))
        if (s_pso_names[i] == k) return 1;
    return 0;
}
static void pso_names_add(u64 k)
{
    if (!k || pso_names_has(k)) return;
    if ((s_pso_names_n + 1) * 2 > s_pso_names_cap) {
        const u32 cap = s_pso_names_cap ? s_pso_names_cap * 2 : 4096;
        u64* old = s_pso_names; const u32 ocap = s_pso_names_cap;
        u64* nw = (u64*)calloc(cap, sizeof(u64));
        if (!nw) return;
        s_pso_names = nw; s_pso_names_cap = cap;
        for (u32 i = 0; i < ocap; i++)
            if (old[i])
                for (u32 j = (u32)old[i] & (cap - 1);; j = (j + 1) & (cap - 1))
                    if (!nw[j]) { nw[j] = old[i]; break; }
        free(old);
    }
    for (u32 i = (u32)k & (s_pso_names_cap - 1);; i = (i + 1) & (s_pso_names_cap - 1))
        if (!s_pso_names[i]) { s_pso_names[i] = k; s_pso_names_n++; return; }
}
/* The GPU layer's library's files (step C3b): <shader cache>/gpu/d3d12/. */
static void pso_lib_path(char* out, size_t n, const char* leaf)
{
    snprintf(out, n, "%s/gpu/d3d12/%s", shader_cache_dir(), leaf);
}
static long pso_file_size(const char* path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return 0;
    return (long)(((u64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
}
/* The library is the GPU layer's (gpu_pipeline_create loads from it and
 * stores in it): its file read here, before any pipeline is made, or a new
 * one when the file is another driver's or GPU's, or there is none. */
static void pso_lib_open(void)
{
    if (!ps3_env_int("PS3_PSO_LIBRARY", 1) || !shader_cache_dir()[0]) return;
    if (!(s_rc_dev->caps.bits & GPU_CAP_PIPELINE_CACHE)) return;   /* (no ID3D12Device1) */
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s/gpu", shader_cache_dir());
    CreateDirectoryA(path, NULL);
    snprintf(path, sizeof path, "%s/gpu/d3d12", shader_cache_dir());
    CreateDirectoryA(path, NULL);
    pso_lib_path(path, sizeof path, "pipelines.lib");
    const long n = pso_file_size(path);
    /* Step C3c: no library possible (today's s_pso_lib NULL) leaves it all
     * off, the manifest unread; else the file's library, or a new one. */
    const int lib = gpu_pipeline_cache_load(s_rc_dev, path);
    if (lib == GPU_PIPELINE_CACHE_NONE) return;
    const int loaded = lib == GPU_PIPELINE_CACHE_LOADED;
    s_pso_lib_on = 1;

    /* The manifest: whole records only; a torn tail is cut off. */
    char mp[MAX_PATH + 32];
    pso_path(mp, sizeof mp, "pso_manifest.bin");
    long mn = 0;
    pso_rec* m = (pso_rec*)pso_read_file(mp, &mn);
    u32 cnt = 0;
    if (m) {
        const u32 whole = (u32)(mn / (long)sizeof(pso_rec));
        while (cnt < whole && m[cnt].magic == PSO_REC_MAGIC && m[cnt].size == sizeof(pso_rec)) cnt++;
        if ((long)cnt * (long)sizeof(pso_rec) != mn) {
            printf("[D3D12] pipeline manifest: keeping the first %u whole records\n", cnt);
            pso_write_file(mp, m, (size_t)cnt * sizeof(pso_rec));
        }
        for (u32 i = 0; i < cnt; i++) pso_names_add(m[i].name);
    }
    s_pso_man = m; s_pso_man_n = s_pso_man_read = cnt;
    if (loaded) {
        char np[MAX_PATH + 32];
        pso_lib_path(np, sizeof np, "pipelines.n");
        FILE* f = fopen(np, "rb");
        unsigned have = 0;
        if (f) { if (fscanf(f, "%u", &have) != 1) have = 0; fclose(f); }
        s_pso_lib_have = have < cnt ? have : cnt;
    } else {
        /* A new library (none yet, or another driver's): the pipeline index's
         * pipelines with a geometry shader are in no manifest, so the index is
         * built again. The start's shader pass may have looked already; then
         * the next start does it. (The pass reads the d3d12 route's
         * pso_index.built, rsx_shader_precompile_index_built; this route's
         * mark is its own and today's file is left alone.) */
        char ip[MAX_PATH + 32];
        pso_lib_path(ip, sizeof ip, "index.built");
        DeleteFileA(ip);
    }
    printf("[D3D12] pipeline library: %s (%ld KB), %u known pipelines, %u to build\n",
           loaded ? "loaded" : "new", n >> 10, cnt, cnt - s_pso_lib_have);
}
/* The pipeline index (defined with vp_fp_desc). */
static volatile LONG s_pxb_live;      /* index build threads running */
static volatile LONG s_pxr_writing;   /* a thread writing new rows */
static void pso_index_flush(void);
static void pso_index_tick(void);
static void pso_index_build_start(void);
/* gpu_pipeline_cache_save writes the library (Serialize can fail when a
 * pipeline is stored between sizing and writing; it tries again, sizing
 * again). pipelines.n follows the library, so after a crash between the two
 * it only undercounts, and the next startup's build finds those pipelines in
 * the library (pso_prebuild_one). */
static void pso_lib_save(void)
{
    const LONG pending = s_pso_lib_new;
    AcquireSRWLockShared(&s_pso_man_lock);
    const u32 recs = s_pso_man_n;
    ReleaseSRWLockShared(&s_pso_man_lock);
    char path[MAX_PATH + 32];
    pso_lib_path(path, sizeof path, "pipelines.lib");
    if (!gpu_pipeline_cache_save(s_rc_dev, path)) return;
    char np[MAX_PATH + 32], txt[16];
    pso_lib_path(np, sizeof np, "pipelines.n");
    pso_write_file(np, txt, (size_t)snprintf(txt, sizeof txt, "%u", recs));
    InterlockedExchangeAdd(&s_pso_lib_new, -pending);
    printf("[D3D12] pipeline library saved: %lu KB, %u pipelines; draws so far: %u loaded, %u compiled\n",
           (unsigned long)(pso_file_size(path) >> 10), recs, (unsigned)s_pso_lib_hits, (unsigned)s_pso_lib_live);
}
static DWORD WINAPI pso_lib_save_thread(LPVOID unused)
{
    (void)unused;
    pso_lib_save();
    pso_index_flush();
    InterlockedExchange(&s_pso_lib_saving, 0);
    return 0;
}
/* Keep a pipeline's description and shaders so a later startup can build it.
 * The record and copies of its two shaders go to a thread of its own, which
 * writes them (pso_blobs/, pso_manifest.bin): where the pipeline was built,
 * on the render thread or one of psj's workers, no file is touched, and
 * psj's workers do not wait on each other's writes. A record still queued
 * when the game closes is written by rc_backend_shutdown
 * (pso_manifest_flush). One that is lost only means a later startup does
 * not build that pipeline ahead: the library still has it. */
typedef struct PsoManItem {
    struct PsoManItem* next;
    pso_rec r;
    void* sh[2];                        /* VS, PS */
} PsoManItem;
static struct {
    SRWLOCK            lk;
    CONDITION_VARIABLE cv;
    PsoManItem        *head, *tail;
    int                started;
} s_pso_manq = { SRWLOCK_INIT, CONDITION_VARIABLE_INIT };

static void pso_manifest_write(PsoManItem* it)
{
    pso_rec* r = &it->r;
    const u64 hs[2] = { r->vs_hash, r->ps_hash };
    const u32 ls[2] = { r->vs_len, r->ps_len };
    char dir[MAX_PATH + 32];
    pso_path(dir, sizeof dir, "pso_blobs");
    CreateDirectoryA(dir, NULL);
    for (int i = 0; i < 2; i++) {
        char bp[MAX_PATH + 64];
        snprintf(bp, sizeof bp, "%s/%016llx.dxbc", dir, (unsigned long long)hs[i]);
        if (GetFileAttributesA(bp) == INVALID_FILE_ATTRIBUTES && !pso_write_file(bp, it->sh[i], ls[i]))
            return;
    }
    char mp[MAX_PATH + 32];
    pso_path(mp, sizeof mp, "pso_manifest.bin");
    AcquireSRWLockExclusive(&s_pso_man_lock);
    if (!pso_names_has(r->name)) {
        FILE* f = fopen(mp, "ab");
        if (f) {
            if (fwrite(r, sizeof *r, 1, f) == 1) { pso_names_add(r->name); s_pso_man_n++; }
            fclose(f);
        }
    }
    ReleaseSRWLockExclusive(&s_pso_man_lock);
}

static PsoManItem* pso_manifest_pop(int wait)
{
    AcquireSRWLockExclusive(&s_pso_manq.lk);
    while (wait && !s_pso_manq.head)
        SleepConditionVariableSRW(&s_pso_manq.cv, &s_pso_manq.lk, INFINITE, 0);
    PsoManItem* it = s_pso_manq.head;
    if (it) {
        s_pso_manq.head = it->next;
        if (!s_pso_manq.head) s_pso_manq.tail = NULL;
    }
    ReleaseSRWLockExclusive(&s_pso_manq.lk);
    return it;
}

static void pso_manifest_done(PsoManItem* it)
{
    pso_manifest_write(it);
    free(it->sh[0]); free(it->sh[1]); free(it);
    InterlockedDecrement(&s_pso_man_pending);
}

static DWORD WINAPI pso_manifest_thread(LPVOID unused)
{
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) pso_manifest_done(pso_manifest_pop(1));
}

static void pso_manifest_flush(void)
{
    for (PsoManItem* it; (it = pso_manifest_pop(0)) != NULL; ) pso_manifest_done(it);
}

static void pso_manifest_add(u64 name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd)
{
    if (pd->DS.pShaderBytecode || pd->HS.pShaderBytecode || pd->GS.pShaderBytecode ||
        !pd->VS.pShaderBytecode || !pd->PS.pShaderBytecode || pd->InputLayout.NumElements > 16 ||
        pd->StreamOutput.NumEntries)
        return;
    PsoManItem* it = (PsoManItem*)calloc(1, sizeof *it);
    if (!it) return;
    pso_rec* r = &it->r;
    r->magic = PSO_REC_MAGIC; r->size = sizeof *r; r->name = name;
    memcpy(&r->desc, pd, sizeof r->desc);
    r->desc.pRootSignature = NULL;
    r->desc.VS.pShaderBytecode = r->desc.PS.pShaderBytecode = NULL;
    r->desc.InputLayout.pInputElementDescs = NULL;
    r->desc.CachedPSO.pCachedBlob = NULL; r->desc.CachedPSO.CachedBlobSizeInBytes = 0;
    r->nel = pd->InputLayout.NumElements;
    for (u32 i = 0; i < r->nel; i++) {
        r->el[i].e = pd->InputLayout.pInputElementDescs[i];
        snprintf(r->el[i].sem, sizeof r->el[i].sem, "%s", r->el[i].e.SemanticName);
        r->el[i].e.SemanticName = NULL;
    }
    const D3D12_SHADER_BYTECODE* sh[2] = { &pd->VS, &pd->PS };
    u64* hs[2] = { &r->vs_hash, &r->ps_hash };
    u32* ls[2] = { &r->vs_len, &r->ps_len };
    for (int i = 0; i < 2; i++) {
        *hs[i] = fnv_bytes(14695981039346656037ull, sh[i]->pShaderBytecode, sh[i]->BytecodeLength);
        *ls[i] = (u32)sh[i]->BytecodeLength;
        it->sh[i] = malloc(sh[i]->BytecodeLength ? sh[i]->BytecodeLength : 1);
        if (!it->sh[i]) { free(it->sh[0]); free(it); return; }
        memcpy(it->sh[i], sh[i]->pShaderBytecode, sh[i]->BytecodeLength);
    }
    InterlockedIncrement(&s_pso_man_pending);
    AcquireSRWLockExclusive(&s_pso_manq.lk);
    if (s_pso_manq.tail) s_pso_manq.tail->next = it; else s_pso_manq.head = it;
    s_pso_manq.tail = it;
    int start = 0;
    if (!s_pso_manq.started) { s_pso_manq.started = 1; start = 1; }
    ReleaseSRWLockExclusive(&s_pso_manq.lk);
    WakeConditionVariable(&s_pso_manq.cv);
    if (start) {
        HANDLE h = CreateThread(NULL, 0, pso_manifest_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        else {   /* no thread: write here, as before */
            AcquireSRWLockExclusive(&s_pso_manq.lk);
            s_pso_manq.started = 0;
            ReleaseSRWLockExclusive(&s_pso_manq.lk);
            pso_manifest_flush();
        }
    }
}
/* A manifest record as the GPU layer's description: the inverse of the D3D12
 * description the GPU layer makes of vp_fp_desc's (gpu_d3d12_pipe_d3d_desc),
 * so the record's pipeline gets the name a draw's gets. 0 for a record no
 * vp_fp_desc makes. */
static int pso_rec_desc(const pso_rec* r, gpu_shader vs, gpu_shader ps, gpu_pipeline_desc* pd)
{
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d = &r->desc;
    const D3D12_RENDER_TARGET_BLEND_DESC* rt = &d->BlendState.RenderTarget[0];
    if (r->nel > GPU_MAX_VERTEX_ATTRS || d->NumRenderTargets > 4) return 0;
    memset(pd, 0, sizeof *pd);   /* zeroed whole: the GPU layer names the pipeline by its bytes */
    pd->layout = s_d3d.vp_layout;
    pd->vs = vs;
    pd->ps = ps;
    pd->vertex.count = r->nel;
    pd->vertex.stride = r->nel ? 256 : 0;
    for (u32 i = 0; i < r->nel; i++) {
        snprintf(pd->vertex.attr[i].semantic, sizeof pd->vertex.attr[i].semantic, "%s", r->el[i].sem);
        pd->vertex.attr[i].semantic_index = r->el[i].e.SemanticIndex;
        pd->vertex.attr[i].format = (gpu_format)r->el[i].e.Format;
        pd->vertex.attr[i].offset = r->el[i].e.AlignedByteOffset;
    }
    pd->topology_type = (u8)d->PrimitiveTopologyType;
    pd->cull = (u8)d->RasterizerState.CullMode;
    pd->front_ccw = d->RasterizerState.FrontCounterClockwise ? 1 : 0;
    pd->depth_clip = d->RasterizerState.DepthClipEnable ? 1 : 0;
    pd->multisample = d->RasterizerState.MultisampleEnable ? 1 : 0;
    pd->depth_test = d->DepthStencilState.DepthEnable ? 1 : 0;
    pd->depth_write = d->DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL ? 1 : 0;
    pd->depth_func = (u8)d->DepthStencilState.DepthFunc;
    pd->blend.enable = rt->BlendEnable ? 1 : 0;
    pd->blend.src = (u8)rt->SrcBlend;
    pd->blend.dst = (u8)rt->DestBlend;
    pd->blend.op = (u8)rt->BlendOp;
    pd->blend.src_alpha = (u8)rt->SrcBlendAlpha;
    pd->blend.dst_alpha = (u8)rt->DestBlendAlpha;
    pd->blend.op_alpha = (u8)rt->BlendOpAlpha;
    pd->blend.write_mask = (u8)rt->RenderTargetWriteMask;
    pd->nrt = d->NumRenderTargets;
    for (u32 i = 0; i < pd->nrt; i++) pd->rt_format[i] = (gpu_format)d->RTVFormats[i];
    pd->ds_format = (gpu_format)d->DSVFormat;
    pd->samples = d->SampleDesc.Count;
    pd->sample_mask = d->SampleMask;
    pd->strip_cut = (u32)d->IBStripCutValue;
    return 1;
}

/* Step C3c: a pipeline made only to be stored in the library
 * (GPU_PIPELINE_STORE_ONLY), as today's startup and index builds release
 * theirs once stored (pso_prebuild_one, pxi_build_one): the GPU layer loads
 * it, or compiles and stores it, then frees it, so it is not resident beside
 * the one a draw later loads. GPU_PIPELINE_LOADED (the library had it),
 * _STORED (new in it), _COMPILED (another thread stored it first) or
 * _FAILED. Thread-safe. */
static volatile LONG s_rc_pipe_store_only;   /* those made (LOADED, STORED or COMPILED) */
static uint32_t rc_pipeline_store(const gpu_pipeline_desc* d)
{
    uint32_t how = GPU_PIPELINE_FAILED;
    (void)gpu_pipeline_create(s_rc_dev, d, GPU_PIPELINE_STORE_ONLY, &how);
    if (how != GPU_PIPELINE_FAILED) InterlockedIncrement(&s_rc_pipe_store_only);
    return how;
}

/* One startup build: the record's shaders from pso_blobs, its description
 * back together, and its name checked against the rebuilt description (the
 * D3D12 one the GPU layer makes of it). 1: built, 2: the library had it, 0:
 * not built. */
static int pso_prebuild_one(const pso_rec* r)
{
    char dir[MAX_PATH + 32];
    pso_path(dir, sizeof dir, "pso_blobs");
    void* blob[2] = { NULL, NULL }; long len[2] = { 0, 0 };
    const u64 hs[2] = { r->vs_hash, r->ps_hash };
    const u32 ls[2] = { r->vs_len, r->ps_len };
    gpu_shader sh[2] = { {0}, {0} };
    uint32_t how = GPU_PIPELINE_FAILED;
    int ok = 0;
    for (int i = 0; i < 2; i++) {
        char bp[MAX_PATH + 64];
        snprintf(bp, sizeof bp, "%s/%016llx.dxbc", dir, (unsigned long long)hs[i]);
        blob[i] = pso_read_file(bp, &len[i]);
        if (!blob[i] || (u32)len[i] != ls[i] ||
            fnv_bytes(14695981039346656037ull, blob[i], (size_t)len[i]) != hs[i])
            goto out;
    }
    for (int i = 0; i < 2; i++) {
        gpu_shader_desc sd;
        memset(&sd, 0, sizeof sd);
        sd.stage = i ? GPU_STAGE_PS : GPU_STAGE_VS;
        sd.code = GPU_CODE_DXBC;
        sd.bytes = blob[i];
        sd.size = (size_t)len[i];
        sh[i] = gpu_shader_create(s_rc_dev, &sd);
        if (!GPU_VALID(sh[i])) goto out;
    }
    gpu_pipeline_desc pd;
    if (!pso_rec_desc(r, sh[0], sh[1], &pd)) goto out;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC dd;
    D3D12_INPUT_ELEMENT_DESC il[GPU_MAX_VERTEX_ATTRS];
    gpu_d3d12_pipeline_d3d_desc(s_rc_dev, &pd, &dd, il);
    if (pso_desc_key(&dd) != r->name) goto out;   /* another build's layout, or a stale root signature */
    /* A record pipelines.n does not count may be in the library already:
     * one stored while the library was being saved, or a crash between the
     * two files. It is loaded then, not built. Either way only the library
     * keeps it (step C3c). */
    how = rc_pipeline_store(&pd);
    if (how == GPU_PIPELINE_LOADED) ok = 2;
    else if (how != GPU_PIPELINE_FAILED) {
        if (how == GPU_PIPELINE_STORED) InterlockedIncrement(&s_pso_lib_new);   /* stored in the library */
        ok = 1;
    }
out:
    sc_shader_drop(&sh[0]);
    sc_shader_drop(&sh[1]);
    free(blob[0]); free(blob[1]);
    return ok;
}
static DWORD WINAPI pso_prebuild_thread(LPVOID unused)
{
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        const u32 i = s_pso_lib_have + (u32)(InterlockedIncrement(&s_pso_pre_next) - 1);
        if (i >= s_pso_man_read || s_pso_pre_abort) break;
        const int r = pso_prebuild_one(&s_pso_man[i]);
        if (r) InterlockedIncrement(r == 2 ? &s_pso_pre_had : &s_pso_pre_built);
    }
    if (InterlockedDecrement(&s_pso_pre_live) == 0)
        printf("[D3D12] pipeline prebuild: %ld of %u built, %ld there already, in %.0f ms\n",
               s_pso_pre_built, s_pso_man_read - s_pso_lib_have, s_pso_pre_had,
               1000.0 * (perf_now() - s_pso_pre_t0));
    return 0;
}
/* Once per present, on the render thread. Starts the startup build once the
 * root signature exists, then the pipeline index's, and saves in the
 * background once those builds are done, the manifest has every record
 * queued so far (pipelines.n counts its records), and a burst of new
 * pipelines has settled (120 presents with none, or 256 pending). */
/* The pipelines the GPU layer made this run (rc_pipeline), from its library
 * or compiled, and the guest-FP ones' share (pso_create_cached). Step C3c:
 * those are the ones it keeps (resident: today's renderer keeps the same);
 * the startup and index builds' only went into the library
 * (rc_pipeline_store). */
static void pso_count_print(const char* when)
{
    printf("[D3D12] pipelines %s: %ld loaded from the library, %ld compiled; guest FP: %ld loaded, %ld compiled\n",
           when, (long)s_rc_pipe_loaded, (long)s_rc_pipe_made, (long)s_pso_lib_hits, (long)s_pso_lib_live);
    printf("[D3D12] pipelines resident: %ld; only stored in the library: %ld\n",
           (long)(s_rc_pipe_loaded + s_rc_pipe_made), (long)s_rc_pipe_store_only);
}
static void pso_lib_tick(void)
{
    /* PS3_PSO_LOG=1: those counts after each present that made pipelines (a
     * replay ends without rc_backend_shutdown's line). */
    { static int lg = -1; PS3_KNOB(lg, ps3_env_int("PS3_PSO_LOG", 0));
      static LONG seen = -1;
      const LONG t = s_rc_pipe_loaded + s_rc_pipe_made + s_rc_pipe_store_only;
      if (lg && t != seen) { seen = t; pso_count_print("so far"); } }
    if (!s_pso_lib_on) return;
    if (!s_pso_pre_started && GPU_VALID(s_d3d.vp_layout)) {
        s_pso_pre_started = 1;
        if (s_pso_man_read > s_pso_lib_have && ps3_env_int("PS3_PSO_PREBUILD", 1)) {
            SYSTEM_INFO si; GetSystemInfo(&si);
            int nt = (int)si.dwNumberOfProcessors / 4;
            if (nt < 1) nt = 1; if (nt > 4) nt = 4;
            s_pso_pre_t0 = perf_now();
            s_pso_pre_live = nt;
            for (int t = 0; t < nt; t++) {
                HANDLE h = CreateThread(NULL, 0, pso_prebuild_thread, NULL, 0, NULL);
                if (h) CloseHandle(h); else InterlockedDecrement(&s_pso_pre_live);
            }
        }
    }
    /* The index after the manifest: what the manifest holds is in the
     * library by then, and the index builds only what is not. */
    if (s_pso_pre_started && !s_pso_pre_live) pso_index_build_start();
    pso_index_tick();
    if (!s_pso_lib_new || s_pso_lib_saving || s_pso_pre_live || s_pxb_live || s_pso_man_pending) return;
    if (++s_pso_lib_quiet < 120 && s_pso_lib_new < 256) return;
    if (InterlockedCompareExchange(&s_pso_lib_saving, 1, 0) == 0) {
        HANDLE t = CreateThread(NULL, 0, pso_lib_save_thread, NULL, 0, NULL);
        if (t) CloseHandle(t); else InterlockedExchange(&s_pso_lib_saving, 0);
    }
}
/* A guest-FP pipeline (vp_get_fp_pso, psj's workers): the GPU layer's, which
 * loads it from its library or compiles and stores it. One it compiled goes
 * into the manifest under today's name, pso_desc_key of the D3D12
 * description the GPU layer made of it (the bridge, on this miss path
 * only). None on failure. Thread-safe. */
static gpu_pipeline pso_create_cached(const gpu_pipeline_desc* pd)
{
    uint32_t how = GPU_PIPELINE_FAILED;
    const gpu_pipeline pso = rc_pipeline(pd, &how);
    if (!s_pso_lib_on || !pso.v) return pso;
    if (how == GPU_PIPELINE_LOADED) { InterlockedIncrement(&s_pso_lib_hits); return pso; }
    InterlockedIncrement(&s_pso_lib_live);
    if (how == GPU_PIPELINE_STORED) {   /* new in the library (step C3c: only then, as today's) */
        InterlockedIncrement(&s_pso_lib_new);
        s_pso_lib_quiet = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC dd;
    D3D12_INPUT_ELEMENT_DESC il[GPU_MAX_VERTEX_ATTRS];
    gpu_d3d12_pipeline_d3d_desc(s_rc_dev, pd, &dd, il);
    pso_manifest_add(pso_desc_key(&dd), &dd);
    return pso;
}

