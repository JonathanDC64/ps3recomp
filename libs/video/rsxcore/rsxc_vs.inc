/* Hash + compile the CURRENT rsx_state vertex program into the VS cache;
 * returns the cache slot (or -1). Called at draw-record time so each draw
 * carries the VP that was loaded when it was submitted. The program's key
 * and text are rsxc_shader.c's (rsxc_vs_locate, rsxc_vs_text, step C3a); the
 * cache, its memo and the dumps are here. */

/* Append a program to the per-draw VS cache. vs none records a program that
 * failed to decompile or compile, so it is not retried on every draw. */
static int vp_vs_insert(u32 hash, gpu_shader vs, int uses_c03)
{
    if (s_d3d.vp_vs_n >= s_d3d.vp_vs_cap) {
        const int cap = s_d3d.vp_vs_cap ? s_d3d.vp_vs_cap * 2 : VP_VS_CACHE_INIT;
        VPVSEntry* grown = (VPVSEntry*)realloc(s_d3d.vp_vs, sizeof(VPVSEntry) * (size_t)cap);
        if (!grown) { sc_shader_drop(&vs); return -1; }
        s_d3d.vp_vs = grown; s_d3d.vp_vs_cap = cap;
    }
    const int slot = s_d3d.vp_vs_n++;
    s_d3d.vp_vs[slot].hash     = hash;
    s_d3d.vp_vs[slot].vs       = vs;
    s_d3d.vp_vs[slot].uses_c03 = uses_c03;
    s_d3d.vp_vs[slot].fetch    = 0;
    s_d3d.vp_vs[slot].code64   = 0;
    s_d3d.vp_vs[slot].start    = 0;
    return GPU_VALID(vs) ? slot : -1;
}

static int vp_get_vs_mode(const rsx_state* st, int fetch);
static int vp_get_vs(const rsx_state* st) { return vp_get_vs_mode(st, 0); }
/* The same program decompiled to read its vertex arrays itself (fetch mode):
 * its own cache slot, keyed by the ucode hash with a fetch salt. */
static int vp_get_vs_fetch(const rsx_state* st) { return vp_get_vs_mode(st, 1); }
static int vp_get_vs_mode(const rsx_state* st, int fetch)
{
    if (!st || st->vp_ucode_bytes < 16) return -1;
    /* Start at the instruction SET_TRANSFORM_PROGRAM_START selects, not at 0.
     * The microcode store holds every resident program -- this title keeps its
     * scene, fluid, caustics and droplet programs in it at once -- so
     * decompiling from 0 ran the scene's transform for every draw. That is why
     * the fluid's 114300 vertices rasterized nothing: they were being pushed
     * through the wrong program.
     * VP_START_OFF=1 restores the old always-from-zero behaviour.
     * (rsxc_vs_locate finds the same start; the memo below needs it first.) */
    u32 vstart = 0;
    { static int off = -1; PS3_KNOB(off, getenv("VP_START_OFF") ? 1 : 0);
      if (!off) vstart = st->transform_program_start * 16u; }
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    /* Sizing and hashing the program, then scanning the cache, ran for every
     * draw. While the store has not been written since, a program start
     * resolves to the slot it resolved to before (if that slot still holds
     * the same program). RSX_VP_MEMO=0 looks it up every time.
     * RSX_VP_MEMO=n (2 to 32, the default 32) keeps the n programs used last
     * at each start, the most recent first, each with a copy of its bytes:
     * when the store was written since, a program whose bytes are those of a
     * copy (up to its end bit) is still that slot. A title that switches one
     * start between programs, or re-sends a program with a word changed and
     * back, then skips the sizing, the hash and the cache scan; the least
     * recently used program makes room. Demon's Souls' deep Palace cycles one
     * start through up to 32 programs: two missed 10.6% of the lookups, each
     * miss hashing about 850 bytes a byte at a time (2.7% of the FIFO
     * thread), eight would miss 3.5% and 32 almost none. 2 is the two-way
     * memo as it was; 1 is the one-entry memo without the copies. */
    extern u32 g_rsx_vp_ucode_ver;
    enum { VP_MEMO_MAX = 32 };
    typedef struct { const rsx_state* st; u32 ver, vstart, hash; int idx;
                     u8* code; u32 len, cap; int end; } VsMemo;
    static VsMemo memo[2][64][VP_MEMO_MAX];
    static int memo_en = -1;
    PS3_KNOB(memo_en, ps3_env_int("RSX_VP_MEMO", VP_MEMO_MAX));
    const int mf = fetch ? 1 : 0, mb = (int)((vstart >> 4) & 63);
    VsMemo* mw = memo[mf][mb];
    const int ways = memo_en >= 2 ? (memo_en < VP_MEMO_MAX ? memo_en : VP_MEMO_MAX) : 1;
    const u32 avail = st->vp_ucode_bytes - vstart;
    if (memo_en) {
        for (int pass = 0; pass < (ways > 1 ? 2 : 1); pass++)
            for (int w = 0; w < ways; w++) {
                VsMemo* q = &mw[w];
                if (pass == 0 && q->ver != g_rsx_vp_ucode_ver) continue;
                if (q->st != st || q->vstart != vstart || q->idx < 0 ||
                    q->idx >= s_d3d.vp_vs_n || s_d3d.vp_vs[q->idx].hash != q->hash)
                    continue;
                if (pass == 1) {
                    /* !q->len: the one-entry memo wrote this entry and kept
                     * no copy (RSX_VP_MEMO=1, then 2 again, live). Compared
                     * over 0 bytes, it matched any program at this start. */
                    if (q->ver == g_rsx_vp_ucode_ver || !q->code || !q->len ||
                        (q->end ? q->len > avail : q->len != avail) ||
                        memcmp(q->code, st->vp_ucode + vstart, q->len))
                        continue;
                    q->ver = g_rsx_vp_ucode_ver;
                }
                const int idx = q->idx;
                if (w) {   /* to the front */
                    const VsMemo hit = *q;
                    memmove(&mw[1], &mw[0], (size_t)w * sizeof *mw);
                    mw[0] = hit;
                }
                return GPU_VALID(s_d3d.vp_vs[idx].vs) ? idx : -1;
            }
    }
    VsMemo* m = &mw[ways - 1];   /* the least recently used; its copy's buffer is reused */
    /* The program keyed by its own instructions, up to its end bit. */
    rsxc_vs_program vp;
    if (rsxc_vs_locate(st, fetch, &vp) < 0) return -1;
    const u8* vuc = vp.code;
    const u32 vlen = vp.vlen;
    const int vend = vp.end;
    const u32 hash = vp.hash;
    for (int i = 0; i < s_d3d.vp_vs_n; i++)
        if (s_d3d.vp_vs[i].hash == hash) {
            m->st = st; m->ver = g_rsx_vp_ucode_ver; m->vstart = vstart;
            m->hash = hash; m->idx = i;
            m->len = 0;
            if (ways > 1) {
                if (m->cap < vlen) {
                    u8* grown = (u8*)realloc(m->code, vlen);
                    if (grown) { m->code = grown; m->cap = vlen; }
                }
                if (m->code && m->cap >= vlen) {
                    memcpy(m->code, vuc, vlen); m->len = vlen; m->end = vend;
                    const VsMemo e = *m;   /* to the front */
                    memmove(&mw[1], &mw[0], (size_t)(ways - 1) * sizeof *mw);
                    mw[0] = e;
                } else {
                    m->st = NULL;   /* no room for the copy: drop the entry */
                }
            }
            return GPU_VALID(s_d3d.vp_vs[i].vs) ? i : -1;
        }

    static char hlsl[262144];
    const double vs_t0 = perf_on() ? perf_now() : 0.0;
    s_perf_vs_compiles++;
    /* The text, with VP_W_ONE's and VP_BYPASS's patches (so the dumps below
     * show them too). */
    int uses_c03 = 0;
    int ni = rsxc_vs_text(&vp, fetch, hlsl, sizeof hlsl, &uses_c03);
    if (ni <= 0) return vp_vs_insert(hash, (gpu_shader){0}, 0);
    if (getenv("VP_DUMP")) { static int _d=0; if (_d < 4 && ++_d) {
        FILE* f = fopen("vp2_dump.hlsl", _d==1 ? "w" : "a");
        if (f) { fprintf(f, "/* per-draw VS hash pending, %d instrs */%s%s", ni, hlsl, "\n"); fclose(f); } } }
    /* RSX_DBG_VP=<hex tex0 offset>: write the vertex program belonging to the draws
     * that bind that texture. vp2_dump.hlsl holds whichever four programs were
     * compiled first, which need not include the one under investigation -- and
     * reading the wrong program sends you chasing the wrong constants. */
    { static const char* dp = (const char*)1; static u32 wantp = 0; static int done = 0;
      if (dp == (const char*)1) { dp = getenv("RSX_DBG_VP");
                                  wantp = dp ? (u32)strtoul(dp, NULL, 16) : 0; }
      if (wantp && !done && s_d3d.cur_texs[0].raw == wantp) {
          done = 1;
          FILE* f = fopen("duck_vp.hlsl", "w");
          if (f) { fprintf(f, "/* duck VP, %d instrs, tex0=0x%X */%s", ni, wantp, hlsl);
                   fclose(f); }
          fprintf(stderr, "[DUCKVP] wrote duck_vp.hlsl (%d instrs)%c", ni, 10);
      } }
    gpu_shader vb = sc_shader(GPU_SC_VS, hlsl, strlen(hlsl), "guest_vp2", NULL, 0);
    if (perf_on()) s_perf_vs += perf_now() - vs_t0;
    if (!GPU_VALID(vb)) {
        static int _e=0; if (_e < 4 && ++_e) printf("[VP] per-draw VS compile FAIL (hash=0x%08X)\n", hash);
        return vp_vs_insert(hash, (gpu_shader){0}, 0);
    }
    const int slot = vp_vs_insert(hash, vb, uses_c03);
    if (slot < 0) return -1;
    s_d3d.vp_vs[slot].fetch = fetch;
    s_d3d.vp_vs[slot].code64 = rsxc_vs_code64(&vp);
    s_d3d.vp_vs[slot].start = vstart / 16u;
    { static int _n=0; if (_n < 6 && ++_n) printf("[VP] per-draw VS cached (hash=0x%08X, %d instrs, slot %d)\n", hash, ni, slot); }
    /* Build the base VP pipeline here if it does not exist yet. render_frame's
     * trigger reads s_d3d.current_rsx_state, which by frame end no longer has
     * the microcode -- so for a title that only ever compiles per-draw VS the
     * base pipeline was never built, s_d3d.vp_ready stayed 0, and the VP draw
     * pass it gates dropped every recorded is_vp draw. Here we are at RECORD
     * time with a live state that definitely has microcode. */
    if (!s_d3d.vp_ready && st && st->vp_ucode_bytes >= 16) {
        const rsx_state* prev = s_d3d.current_rsx_state;
        s_d3d.current_rsx_state = st;
        compile_vp();
        s_d3d.current_rsx_state = prev;
    }
    return slot;
}

