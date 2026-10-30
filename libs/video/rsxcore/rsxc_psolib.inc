/* Driver pipelines on disk. The .dxbc cache above saves FXC, but every new
 * PSO still has the driver compile DXBC to GPU code: 0.1 ms when the driver's
 * own cache has it, 7-20 ms when it does not, and a new area asks for a dozen
 * in one frame (PERF=1 "pipelines created"). The driver's cache belongs to the
 * executable, so a rebuilt exe starts cold. Ours, in the shader cache dir:
 *
 *   pso_library.bin  an ID3D12PipelineLibrary: the compiled pipelines, named by
 *                    a hash of the whole description. Only good for the driver
 *                    and GPU that wrote it; another one fails to load.
 *   pso_library.n    how many manifest records the saved library holds.
 *   pso_manifest.bin every pipeline ever created, as a description that can
 *                    be rebuilt without the guest (fixed-size records).
 *   pso_blobs/       the shader bytecode those records name, by content hash.
 *
 * At startup, worker threads build every manifest record the library does not
 * hold yet (all of them after a driver update), before the title asks for
 * them; a draw then only loads its pipeline from the library. A pipeline no
 * session has met yet is still compiled on first use, once.
 * PS3_PSO_LIBRARY=0 turns all of it off; PS3_PSO_PREBUILD=0 keeps the library
 * but skips the startup build. */
static ID3D12PipelineLibrary* s_pso_lib;
static void*         s_pso_lib_blob;        /* the library reads from this while it lives */
static SRWLOCK       s_pso_lib_lock = SRWLOCK_INIT;   /* Load/Store */
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
     * vp_root_sig. Callers zero the whole description first, so padding hashes
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
static void pso_lib_open(void)
{
    if (!ps3_env_int("PS3_PSO_LIBRARY", 1) || !shader_cache_dir()[0]) return;
    ID3D12Device1* d1 = NULL;
    if (FAILED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12Device1, (void**)&d1)))
        return;
    char path[MAX_PATH + 32];
    pso_path(path, sizeof path, "pso_library.bin");
    long n = 0;
    s_pso_lib_blob = pso_read_file(path, &n);
    HRESULT hr = E_FAIL;
    if (s_pso_lib_blob) {
        hr = d1->lpVtbl->CreatePipelineLibrary(d1, s_pso_lib_blob, (SIZE_T)n,
                 &IID_ID3D12PipelineLibrary, (void**)&s_pso_lib);
        if (FAILED(hr)) {
            printf("[D3D12] pipeline library: '%s' is from another driver or GPU (0x%08lX); rebuilding it\n",
                   path, hr);
            free(s_pso_lib_blob); s_pso_lib_blob = NULL;
        }
    }
    if (!s_pso_lib && FAILED(d1->lpVtbl->CreatePipelineLibrary(d1, NULL, 0,
                                 &IID_ID3D12PipelineLibrary, (void**)&s_pso_lib)))
        s_pso_lib = NULL;
    d1->lpVtbl->Release(d1);
    if (!s_pso_lib) return;

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
    if (SUCCEEDED(hr)) {
        char np[MAX_PATH + 32];
        pso_path(np, sizeof np, "pso_library.n");
        FILE* f = fopen(np, "rb");
        unsigned have = 0;
        if (f) { if (fscanf(f, "%u", &have) != 1) have = 0; fclose(f); }
        s_pso_lib_have = have < cnt ? have : cnt;
    }
    if (FAILED(hr)) {
        /* A new library (none yet, or another driver's): the pipeline index's
         * pipelines with a geometry shader are in no manifest, so the index is
         * built again. The start's shader pass may have looked already; then
         * the next start does it. */
        char ip[MAX_PATH + 32];
        pso_path(ip, sizeof ip, "pso_index.built");
        DeleteFileA(ip);
    }
    printf("[D3D12] pipeline library: %s (%ld KB), %u known pipelines, %u to build\n",
           SUCCEEDED(hr) ? "loaded" : "new", n >> 10, cnt, cnt - s_pso_lib_have);
}
/* The pipeline index (defined with vp_fp_desc). */
static volatile LONG s_pxb_live;      /* index build threads running */
static volatile LONG s_pxr_writing;   /* a thread writing new rows */
static void pso_index_flush(void);
static void pso_index_tick(void);
static void pso_index_build_start(void);
/* Serialize can fail when a pipeline is stored between sizing and writing;
 * the next attempt sizes again. pso_library.n follows the library, so after a
 * crash between the two it only undercounts, and the next startup's build
 * finds those pipelines in the library (pso_prebuild_one). */
static void pso_lib_save(void)
{
    for (int tries = 0; tries < 3; tries++) {
        const LONG pending = s_pso_lib_new;
        AcquireSRWLockExclusive(&s_pso_lib_lock);
        const u32 recs = s_pso_man_n;
        ReleaseSRWLockExclusive(&s_pso_lib_lock);
        const SIZE_T n = s_pso_lib->lpVtbl->GetSerializedSize(s_pso_lib);
        void* buf = malloc(n ? n : 1);
        if (buf && SUCCEEDED(s_pso_lib->lpVtbl->Serialize(s_pso_lib, buf, n))) {
            char path[MAX_PATH + 32];
            pso_path(path, sizeof path, "pso_library.bin");
            const int ok = pso_write_file(path, buf, n);
            free(buf);
            if (ok) {
                char np[MAX_PATH + 32], txt[16];
                pso_path(np, sizeof np, "pso_library.n");
                pso_write_file(np, txt, (size_t)snprintf(txt, sizeof txt, "%u", recs));
                InterlockedExchangeAdd(&s_pso_lib_new, -pending);
                printf("[D3D12] pipeline library saved: %lu KB, %u pipelines; draws so far: %u loaded, %u compiled\n",
                       (unsigned long)(n >> 10), recs, (unsigned)s_pso_lib_hits, (unsigned)s_pso_lib_live);
            }
            return;
        }
        free(buf);
    }
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
/* Store a pipeline under its name, unless the library has that name. Storing
 * a name the library held (from the file, or stored by another thread in the
 * meantime) made the library's next Serialize read past a buffer in
 * D3D12Core (seen 2026-10-23), so the name is looked up first, under the
 * same lock. 1 if stored. */
static int pso_lib_store(const WCHAR* name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd,
                         ID3D12PipelineState* pso)
{
    ID3D12PipelineState* had = NULL;
    int stored = 0;
    AcquireSRWLockExclusive(&s_pso_lib_lock);
    if (SUCCEEDED(s_pso_lib->lpVtbl->LoadGraphicsPipeline(s_pso_lib, name, pd,
                      &IID_ID3D12PipelineState, (void**)&had)))
        had->lpVtbl->Release(had);
    else if (SUCCEEDED(s_pso_lib->lpVtbl->StorePipeline(s_pso_lib, name, pso))) {
        InterlockedIncrement(&s_pso_lib_new);
        stored = 1;
    }
    ReleaseSRWLockExclusive(&s_pso_lib_lock);
    return stored;
}

/* One startup build: the record's shaders from pso_blobs, its description
 * back together, and its name checked against the rebuilt description.
 * 1: built, 2: the library had it, 0: not built. */
static int pso_prebuild_one(const pso_rec* r)
{
    char dir[MAX_PATH + 32];
    pso_path(dir, sizeof dir, "pso_blobs");
    void* blob[2] = { NULL, NULL }; long len[2] = { 0, 0 };
    const u64 hs[2] = { r->vs_hash, r->ps_hash };
    const u32 ls[2] = { r->vs_len, r->ps_len };
    int ok = 0;
    for (int i = 0; i < 2; i++) {
        char bp[MAX_PATH + 64];
        snprintf(bp, sizeof bp, "%s/%016llx.dxbc", dir, (unsigned long long)hs[i]);
        blob[i] = pso_read_file(bp, &len[i]);
        if (!blob[i] || (u32)len[i] != ls[i] ||
            fnv_bytes(14695981039346656037ull, blob[i], (size_t)len[i]) != hs[i])
            goto out;
    }
    D3D12_INPUT_ELEMENT_DESC il[16];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memcpy(&pd, &r->desc, sizeof pd);
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = blob[0]; pd.VS.BytecodeLength = (SIZE_T)len[0];
    pd.PS.pShaderBytecode = blob[1]; pd.PS.BytecodeLength = (SIZE_T)len[1];
    for (u32 i = 0; i < r->nel && i < 16; i++) { il[i] = r->el[i].e; il[i].SemanticName = r->el[i].sem; }
    pd.InputLayout.pInputElementDescs = r->nel ? il : NULL;
    if (pso_desc_key(&pd) != r->name) goto out;   /* another build's layout, or a stale root signature */
    WCHAR name[20];
    swprintf(name, 20, L"%016llx", (unsigned long long)r->name);
    ID3D12PipelineState* pso = NULL;
    /* A record pso_library.n does not count may be in the library already:
     * one stored while the library was being saved, or a crash between the
     * two files. It is loaded then, not built. */
    AcquireSRWLockExclusive(&s_pso_lib_lock);
    const HRESULT lh = s_pso_lib->lpVtbl->LoadGraphicsPipeline(s_pso_lib, name, &pd,
                           &IID_ID3D12PipelineState, (void**)&pso);
    ReleaseSRWLockExclusive(&s_pso_lib_lock);
    if (SUCCEEDED(lh)) { pso->lpVtbl->Release(pso); ok = 2; goto out; }
    pso = NULL;
    if (SUCCEEDED(s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
            s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso))) {
        pso_lib_store(name, &pd, pso);
        pso->lpVtbl->Release(pso);
        ok = 1;
    }
out:
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
 * queued so far (pso_library.n counts its records), and a burst of new
 * pipelines has settled (120 presents with none, or 256 pending). */
static void pso_lib_tick(void)
{
    if (!s_pso_lib) return;
    if (!s_pso_pre_started && s_d3d.vp_root_sig) {
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
static HRESULT pso_create_cached(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd,
                                 ID3D12PipelineState** out)
{
    *out = NULL;
    if (!s_pso_lib)
        return s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
            s_d3d.device, pd, &IID_ID3D12PipelineState, (void**)out);
    const u64 key = pso_desc_key(pd);
    WCHAR name[20];
    swprintf(name, 20, L"%016llx", (unsigned long long)key);
    AcquireSRWLockExclusive(&s_pso_lib_lock);
    HRESULT hr = s_pso_lib->lpVtbl->LoadGraphicsPipeline(s_pso_lib, name, pd,
                     &IID_ID3D12PipelineState, (void**)out);
    ReleaseSRWLockExclusive(&s_pso_lib_lock);
    if (SUCCEEDED(hr)) { InterlockedIncrement(&s_pso_lib_hits); return S_OK; }
    *out = NULL;
    InterlockedIncrement(&s_pso_lib_live);
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
        s_d3d.device, pd, &IID_ID3D12PipelineState, (void**)out);
    if (SUCCEEDED(hr)) {
        if (pso_lib_store(name, pd, *out)) s_pso_lib_quiet = 0;
        pso_manifest_add(key, pd);
    }
    return hr;
}

