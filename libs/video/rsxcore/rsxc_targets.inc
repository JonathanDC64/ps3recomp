/* ---------------------------------------------------------------------------
 * Render-to-texture: offscreen RT pool.
 *
 * Draws/clears targeting a non-display surface (SET_SURFACE_COLOR_xOFFSET
 * not registered via cellGcmSetDisplayBuffer) render into a pooled RGBA8
 * texture keyed by the surface's resolved vm offset; a later draw binding a
 * texture at that offset samples the RT directly instead of guest memory.
 * RGBA8 for every RT (even half-float guest surfaces) keeps the existing
 * PSO/RTV formats -- values clamp to [0,1], which demosaic's RGB data fits.
 * -----------------------------------------------------------------------*/

/* Snapshot the VP constant bank + viewport epilogue for one draw into its
 * vp_cb slot. Runs at record time so every draw keeps the constants that
 * were live when the guest issued it. */
/* RSX_LATE_CHECK=1 (live): hash each draw's textures and fragment program
 * when the draw is recorded, and again when the batch renders, and report
 * what changed in between. Textures and program code are read from guest
 * memory only at render time, which is later than the RSX would read them:
 * if the title has reused that memory by then, the draw shows the new bytes.
 * A replay has one consistent snapshot, so this kind of error shows live only. */
static int off_rt_find(u32 off);
static u32 (*s_late_tex)[RSX_FP_UNITS];
static u32* s_late_fp;
static int late_on(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("RSX_LATE_CHECK", 0));
    return on;
}
/* One hash per texture per frame and pass (record, render): a frame binds
 * the same few hundred textures thousands of times. */
#define LATE_MEMO 8192
static struct { u32 off, w, h, fmt; u64 gen; u32 sum; } s_late_memo[LATE_MEMO];
static u64 s_late_gen = 1;
static u32 late_tex_hash(const D3D12DrawRecord* dr, int u);
static u32 late_tex_sum(const D3D12DrawRecord* dr, int u)
{
    if (!dr->tex[u].set || !dr->tex[u].off || !dr->tex[u].w || !dr->tex[u].h) return 0;
    const u32 off = dr->tex[u].off, w = dr->tex[u].w, h = dr->tex[u].h, fmt = dr->tex[u].fmt;
    u32 b = (off * 0x9E3779B1u ^ (w << 16 | h) ^ fmt * 0x85EBCA77u) & (LATE_MEMO - 1);
    for (int n = 0; n < 16; n++, b = (b + 1) & (LATE_MEMO - 1)) {
        if (s_late_memo[b].gen == s_late_gen) {
            if (s_late_memo[b].off == off && s_late_memo[b].w == w &&
                s_late_memo[b].h == h && s_late_memo[b].fmt == fmt) return s_late_memo[b].sum;
            continue;
        }
        s_late_memo[b].off = off; s_late_memo[b].w = w; s_late_memo[b].h = h;
        s_late_memo[b].fmt = fmt; s_late_memo[b].gen = s_late_gen;
        return s_late_memo[b].sum = late_tex_hash(dr, u);
    }
    return late_tex_hash(dr, u);
}
static u32 late_tex_hash(const D3D12DrawRecord* dr, int u)
{
    extern uint8_t* vm_base;
    rsx_tex_layout tl;
    rsx_texture_layout(dr->tex[u].fmt, dr->tex[u].w, dr->tex[u].h, &tl);
    u32 nb = tl.compressed ? tl.row_bytes * tl.rows
                           : dr->tex[u].w * dr->tex[u].h * tl.bytes_per_texel;
    return nb ? tex_csum(vm_base + dr->tex[u].off, nb) | 1u : 0;
}
static u32 late_fp_sum(const D3D12DrawRecord* dr)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int, u32);
    if (!dr->fp_addr) return 0;
    u32 foff = cellGcmResolveLocated((dr->fp_addr & 0x3u) == 1, dr->fp_addr & ~0x3u);
    if (foff == 0xFFFFFFFFu) return 0;
    u32 usz;
    return fp_hash_at(foff, &usz) | 1u;
}

#ifdef _WIN32
/* D3D12_DRED=1: a marker before each draw. DRED 1.2 keeps its text with the
 * command history, so dred_report can name the draw the GPU stopped on: its
 * record, its two programs (the pipeline index's fingerprints, which match
 * the game's own shader files), its target, depth surface and size, and its
 * first texture. */
static u64 dred_fp64(u32 fp_addr)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int, u32);
    extern u64 rsx_fp_code_hash64(const u8*, u32);
    static struct { u32 off, h; u64 h64; } memo[256];
    if (!fp_addr) return 0;
    const u32 off = cellGcmResolveLocated((fp_addr & 0x3u) == 1, fp_addr & ~0x3u);
    if (off == 0xFFFFFFFFu) return 0;
    u32 usz;
    const u32 h = fp_hash_at(off, &usz);   /* memoized; the 64-bit hash is not */
    const u32 b = ((off >> 4) * 2654435761u) >> 24;
    if (memo[b].off != off || memo[b].h != h) {
        memo[b].off = off; memo[b].h = h;
        memo[b].h64 = rsx_fp_code_hash64(vm_base + off, RSX_FP_MAX_BYTES);
    }
    return memo[b].h64;
}
static void dred_mark(u32 d, const D3D12DrawRecord* dr)   /* on the frame's recorder (step C7) */
{
    static int on = -1;
    if (on < 0) on = getenv("D3D12_DRED") ? 1 : 0;
    if (!on) return;
    const u64 vp = (dr->vs_idx >= 0 && dr->vs_idx < s_d3d.vp_vs_n) ? s_d3d.vp_vs[dr->vs_idx].code64 : 0;
    char a[256];
    int n = snprintf(a, sizeof a,
        "draw %u vp %016llx fp %016llx rt %08X %ux%u z %08X %s %u%s tex0 %08X %ux%u fmt %02X",
        d, (unsigned long long)vp, (unsigned long long)dred_fp64(dr->fp_addr),
        dr->rt_off, dr->rt_w, dr->rt_h, dr->zeta,
        dr->index_count ? "indices" : "vertices",
        dr->index_count ? dr->index_count : dr->vertex_count, dr->fetch ? " fetch" : "",
        dr->tex[0].off, dr->tex[0].w, dr->tex[0].h, dr->tex[0].fmt);
    if (n < 0) return;
    if (n >= (int)sizeof a) n = (int)sizeof a - 1;
    /* gpu_cmd_marker packs the text as DRED keeps it (today's words: the PIX
     * SetMarker form, metadata 2) and cuts it at 255 bytes, as above. */
    gpu_cmd_marker(s_rc_cmd, a, (u32)n);
}
#else
static void dred_mark(u32 d, const D3D12DrawRecord* dr)
{
    (void)d; (void)dr;
}
#endif
static void late_note(u32 slot, const D3D12DrawRecord* dr)
{
    if (!late_on() || !dr || slot >= MAX_DRAWS) return;
    if (!s_late_tex) {
        s_late_tex = calloc(MAX_DRAWS, sizeof *s_late_tex);
        s_late_fp  = calloc(MAX_DRAWS, sizeof *s_late_fp);
        if (!s_late_tex || !s_late_fp) return;
    }
    for (int u = 0; u < RSX_FP_UNITS; u++) s_late_tex[slot][u] = late_tex_sum(dr, u);
    s_late_fp[slot] = late_fp_sum(dr);
}
static void late_vtx_check(void);
static void late_check(void)
{
    late_vtx_check();
    if (!late_on() || !s_late_tex) return;
    s_late_gen++;   /* render-time hashes */
    static int said = 0;
    u32 ntex = 0, nfp = 0;
    for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++) {
        const D3D12DrawRecord* dr = &s_d3d.draws[d];
        if (!dr->is_vp || dr->is_clear || dr->cb_slot >= MAX_DRAWS) continue;
        const u32 k = dr->cb_slot;
        for (int u = 0; u < RSX_FP_UNITS; u++) {
            if (!s_late_tex[k][u] || off_rt_find(dr->tex[u].raw) >= 0) continue;
            if (late_tex_sum(dr, u) == s_late_tex[k][u]) continue;
            ntex++;
            if (said < 60) { said++;
                fprintf(stderr, "[LATE] frame %llu draw %u fp 0x%X t%d 0x%X %ux%u fmt 0x%02X "
                        "changed after the draw%c", (unsigned long long)s_d3d.frame_count, d,
                        dr->fp_addr, u, dr->tex[u].raw, dr->tex[u].w, dr->tex[u].h,
                        dr->tex[u].fmt, 10); }
        }
        if (s_late_fp[k] && late_fp_sum(dr) != s_late_fp[k]) {
            nfp++;
            if (said < 60) { said++;
                fprintf(stderr, "[LATE] frame %llu draw %u fp 0x%X: program code changed "
                        "after the draw%c", (unsigned long long)s_d3d.frame_count, d,
                        dr->fp_addr, 10); }
        }
    }
    if (ntex || nfp) {
        static int n = 0;
        if (n < 200 && ++n) fprintf(stderr, "[LATE] frame %llu: %u texture binds, %u programs "
                               "changed between draw and render%c",
                               (unsigned long long)s_d3d.frame_count, ntex, nfp, 10);
    }
    s_late_gen++;   /* the next batch's record-time hashes */
}

/* Inline constants each record's program has (drawprint hashes only those:
 * the rest of the slot holds whatever an earlier frame left there). */
static u8 s_fpk_n[MAX_DRAWS];
/* rsx_fp_extract_consts of the program at guest offset `off`, memoised as
 * fp_hash_at memoises the hash: the walk ran for every draw, ~2.6% of the
 * render thread at the Palace, and a program whose bytes (constants
 * included, up to its END) equal the copy kept with the last walk has the
 * same constants. A re-patched constant is a byte that differs, so a fresh
 * walk. RSX_FPK_MEMO=0 walks every time. */
typedef struct { u32 off, size; int n; u8* bytes; float k[FPCB_KMAX * 4]; } FpkMemo;
static FpkMemo s_fpkm[1024];
static int fpk_extract_at(u32 off, float* out)
{
    extern uint8_t* vm_base;
    const u8* up = vm_base + off;
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("RSX_FPK_MEMO", 1));
    if (!en) return rsx_fp_extract_consts(up, RSX_FP_MAX_BYTES, out, FPCB_KMAX);
    FpkMemo* m = &s_fpkm[((off >> 4) * 2654435761u) >> 22];
    if (m->bytes && m->off == off && !memcmp(m->bytes, up, m->size)) {
        memcpy(out, m->k, sizeof(float) * 4 * (size_t)m->n);
        return m->n;
    }
    /* Into a cached buffer first: `out` is the upload heap, write-combined,
     * and reading it back for the memo would be the slow part. */
    float k[FPCB_KMAX * 4];
    const int n = rsx_fp_extract_consts(up, RSX_FP_MAX_BYTES, k, FPCB_KMAX);
    memcpy(out, k, sizeof(float) * 4 * (size_t)n);
    const u32 usz = rsx_fp_program_size(up, RSX_FP_MAX_BYTES);
    if (usz) {   /* no END: nothing bounds what the walk read */
        u8* b = (m->bytes && m->size >= usz) ? m->bytes : (u8*)realloc(m->bytes, usz);
        if (b) {
            memcpy(b, up, usz); m->bytes = b; m->off = off; m->size = usz; m->n = n;
            memcpy(m->k, k, sizeof(float) * 4 * (size_t)n);
        }
    }
    return n;
}
/* The last vp_cb slot written this frame, and what it was written from. */
static u32 s_vcb_last = ~0u, s_vcb_gen;
static const rsx_state* s_vcb_st;
static float s_vcb_z[2];

/* Returns the vp_cb slot the draw binds (its vcb_slot). */
static u32 vp_record_cb(u32 slot, int vs_idx, const D3D12DrawRecord* dr)
{
    late_note(slot, dr);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!s_d3d.vp_cb_mapped || !st || slot >= MAX_DRAWS) return slot;
    /* FP texcoord scale (b1): 1/size for UNnormalized textures (fmt bit
     * 0x40 -- wave samples everything in texel space), 1.0 otherwise. */
    if (s_d3d.vp_fpcb_mapped) {
        float* ts = (float*)((char*)s_d3d.vp_fpcb_mapped
            + ((u64)s_d3d.vp_parity * MAX_DRAWS + slot) * VP_FPCB_STRIDE);
        for (int _u = 0; _u < RSX_FP_UNITS; _u++) {
            float sx = 1.0f, sy = 1.0f;
            if (dr && dr->tex[_u].set && (dr->tex[_u].fmt & 0x40) &&
                dr->tex[_u].w && dr->tex[_u].h) {
                sx = 1.0f / (float)dr->tex[_u].w;
                sy = 1.0f / (float)dr->tex[_u].h;
            }
            ts[_u*4+0] = sx; ts[_u*4+1] = sy; ts[_u*4+2] = 0.0f; ts[_u*4+3] = 0.0f;
        }
        /* rsx_alphatest (b1[4]): x=enable, y=ref/255, z=func-0x200. D3D12
         * has no fixed alpha test; guest FPs discard on it dynamically
         * (wave's colour wheel is a disc via alpha test -- without it the
         * palette drew as an opaque square). w: half the RSX_SCALE factor,
         * for the sprite-edge clamp (fp_inject_uvclamp); a shader without
         * the clamp never reads it. */
        const float half_scale = (float)rsx_scale_pct() / 200.0f;
        static int noat = -1;   /* knobs: this runs for every draw */
        PS3_KNOB(noat, getenv("NO_ALPHATEST") ? 1 : 0);
        if (dr && !noat) {
            { static int _at = 0;
              if (((dr->alpha_ctl >> 16) & 1u) && _at < 4 && ++_at)
                  printf("[ALPHATEST] enable func=%u ref=%u\n",
                         (dr->alpha_ctl >> 8) & 0xFFu, dr->alpha_ctl & 0xFFu); }
            float* at = &ts[FPCB_ALPHA * 4];
            at[0] = (float)((dr->alpha_ctl >> 16) & 1u);
            at[1] = (float)(dr->alpha_ctl & 0xFFu) / 255.0f;
            at[2] = (float)((dr->alpha_ctl >> 8) & 0x07u);
            at[3] = half_scale;
        } else {
            float* at = &ts[FPCB_ALPHA * 4];
            at[0] = 0.0f; at[1] = 0.0f; at[2] = 7.0f; at[3] = half_scale;
        }

        /* fp_k[]: the program's inline fragment constants, re-read from the
         * guest ucode every draw. The decompiler emits fp_k[i] lookups instead
         * of baking these in as literals, so the compiled shader no longer
         * changes when the title re-patches them -- which is what forced the
         * pipeline cache to key on a hash of the ucode bytes. */
        if (dr && dr->fp_addr) {
            extern uint8_t* vm_base;
            extern u32 cellGcmResolveLocated(int, u32);
            u32 foff = cellGcmResolveLocated((dr->fp_addr & 0x3u) == 1,
                                             dr->fp_addr & ~0x3u);
            if (vm_base && foff != 0xFFFFFFFFu)
                s_fpk_n[slot] = (u8)fpk_extract_at(foff, &ts[FPCB_K * 4]);
            /* FPK_FP=<hex fp_addr>: that program's constants, each time they
             * change (capped). Shows whether the title's per-material patches
             * reach the draw, for one surface at a time. */
            { static const char* e = (const char*)1; static u32 want = 0;
              static float last[FPCB_KMAX * 4]; static int nk = 0, np = 0;
              if (e == (const char*)1) { e = getenv("FPK_FP");
                                         want = e ? (u32)strtoul(e, NULL, 16) : 0; }
              if (want && dr->fp_addr == want && np < 40 && vm_base && foff != 0xFFFFFFFFu) {
                  const float* k = &ts[FPCB_K * 4];
                  int cnt = 0;
                  for (int q = FPCB_KMAX - 1; q >= 0; q--)
                      if (k[q*4] || k[q*4+1] || k[q*4+2] || k[q*4+3]) { cnt = q + 1; break; }
                  if (cnt != nk || memcmp(last, k, sizeof(float) * 4 * (size_t)cnt)) {
                      np++; nk = cnt; memcpy(last, k, sizeof(float) * 4 * (size_t)cnt);
                      fprintf(stderr, "[FPK_FP] fp=0x%X tex0=0x%X n=%u:", dr->fp_addr,
                              dr->tex[0].raw, dr->vertex_count);
                      for (int q = 0; q < cnt; q++)
                          fprintf(stderr, " k%d=(%g,%g,%g,%g)", q, k[q*4], k[q*4+1], k[q*4+2], k[q*4+3]);
                      fputc(10, stderr);
                  }
              } }
            /* FPK_DBG=1: the fragment constants as the shader will see them.
             * A program whose only output is fp_k[0].xxxx renders black if
             * those constants never arrive, which looks identical from the
             * outside to geometry that never rasterised. */
            { static int fk = -1; PS3_KNOB(fk, getenv("FPK_DBG") ? 1 : 0);
              static int n = 0;
              if (fk && n < 12 && dr->fp_addr != 0x00EC2B02u) { n++;
                  fprintf(stderr, "[FPK] tex0=0x%08X fp=0x%08X k0=(%g %g %g %g)"
                                  " k1=(%g %g %g %g)%c",
                          dr->tex[0].raw, dr->fp_addr,
                          ts[FPCB_K*4+0], ts[FPCB_K*4+1], ts[FPCB_K*4+2], ts[FPCB_K*4+3],
                          ts[FPCB_K*4+4], ts[FPCB_K*4+5], ts[FPCB_K*4+6], ts[FPCB_K*4+7], 10);
                  const u32 raw = dr->fp_addr & ~0x3u;
                  const u32 ea_main  = cellGcmResolveLocated(0, raw);
                  const u32 ea_local = cellGcmResolveLocated(1, raw);
                  fprintf(stderr, "[FPK]   ucode main@0x%08X:", ea_main);
                  for (int b = 0; b < 48; b++) fprintf(stderr, "%s%02X", (b%16)?" ":"  |", vm_base[ea_main + b]);
                  fputc(10, stderr); }
              /* distinct fragment programs seen -- if every draw shares one,
               * SET_SHADER_PROGRAM is not being tracked per draw. */
              if (fk) { static u32 seen[16]; static int ns = 0; int f = 0;
                  for (int q = 0; q < ns; q++) if (seen[q] == dr->fp_addr) f = 1;
                  if (!f && ns < 16) { seen[ns++] = dr->fp_addr;
                      fprintf(stderr, "[FPK] distinct fp #%d = 0x%08X%c", ns, dr->fp_addr, 10); } } }
        }
    }
    /* VP_CB_REUSE (live, default 1): a draw whose vertex constants and
     * viewport z are those of the last bank written this frame binds that
     * bank instead of copying its own. The bank is 8 KB, and a Demon's Souls
     * level frame's 6,000+ draws copied ~50 MB into the write-combined upload
     * heap, most of it unchanged from the draw before. The debug rewrites
     * below (DBG_ZOOM, the projection fallbacks) need their own copy. */
    { static int reuse = -1, dbgk = -1;
      PS3_KNOB(reuse, ps3_env_int("VP_CB_REUSE", 1));
      PS3_KNOB(dbgk, (getenv("DBG_ZOOM") || getenv("VP_GUESSPROJ") || getenv("VP_FIXPROJ") ||
                      getenv("VP_MVP") || getenv("RSX_DBG_CB")) ? 1 : 0);
      if (reuse && !dbgk && s_vcb_last < MAX_DRAWS && s_vcb_st == st &&
          s_vcb_gen == st->vertex_constants_gen &&
          s_vcb_z[0] == st->viewport_scale[2] && s_vcb_z[1] == st->viewport_offset[2])
          return s_vcb_last;
      s_vcb_last = slot; s_vcb_st = st; s_vcb_gen = st->vertex_constants_gen;
      s_vcb_z[0] = st->viewport_scale[2]; s_vcb_z[1] = st->viewport_offset[2]; }
    char* dst = (char*)s_d3d.vp_cb_mapped
        + ((u64)s_d3d.vp_parity * MAX_DRAWS + slot) * VP_CB_STRIDE;
    memcpy(dst, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16);
    /* VP_MVP=<N>: the constant bank as the shader will see it, for the first N
     * draws. A snapshot taken from a stale rsx_state looks exactly like a broken
     * vertex program from the outside -- both give zero fragments. */
    { static int cap = -1, seen = 0;
      PS3_KNOB(cap, ps3_env_int("VP_MVP", 0));
      if (cap && seen < cap) { seen++;
        const float* c = (const float*)dst;
        int last_nz = -1;
        for (int i = 0; i < RSX_MAX_VERTEX_CONSTANTS * 4; i++)
            if (c[i] > 1e-9f || c[i] < -1e-9f) last_nz = i / 4;
        fprintf(stderr, "[VPMVP] slot=%u lastNZ=c%d  c260=(%g %g %g %g) c467=(%g %g %g %g)\n",
                slot, last_nz,
                c[260*4+0], c[260*4+1], c[260*4+2], c[260*4+3],
                c[467*4+0], c[467*4+1], c[467*4+2], c[467*4+3]); } }
    /* RSX_DBG_CB=<hex tex0 offset>: the constants as they land in the per-draw
     * constant buffer for that texture's draws -- i.e. exactly what the shader
     * samples, not what the CPU-side rsx_state holds. Those two agreeing is an
     * assumption worth checking directly when a draw transforms to nothing. */
    const float* vpx_dbg = (const float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);
    { static const char* dc = (const char*)1; static u32 wantc = 0; static int n = 0;
      if (dc == (const char*)1) { dc = getenv("RSX_DBG_CB");
                                  wantc = dc ? (u32)strtoul(dc, NULL, 16) : 0; }
      if (wantc && dr && dr->tex[0].raw == wantc && n < 4) { n++;
          const float* c = (const float*)dst;
          fprintf(stderr, "[DUCKCB] slot=%u vs_idx=%d posscale=(%g %g %g) posoffset=(%g %g %g)%c",
                  slot, vs_idx, vpx_dbg[0], vpx_dbg[1], vpx_dbg[2],
                  vpx_dbg[4], vpx_dbg[5], vpx_dbg[6], 10);
          for (int r = 256; r <= 259; r++)
              fprintf(stderr, "[DUCKCB]   c%d=(%g %g %g %g)%c", r,
                      c[r*4+0], c[r*4+1], c[r*4+2], c[r*4+3], 10);
      } }

    /* Viewport epilogue (see the render_frame notes this logic came from):
     * x/y identity, z lane remaps GL clip z when the guest programs one. */
    float* vpx = (float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);
    const float* vs_ = st->viewport_scale;
    const float* vo_ = st->viewport_offset;
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (vs_[2] != 0.0f) { vpx[2] = vs_[2]; vpx[6] = vo_[2]; }
    else                { vpx[2] = 1.0f;   vpx[6] = 0.0f;   }
    /* DBG_ZOOM=<k> with DBG_CENTER="cx,cy": a debug camera applied in CLIP space,
     * not a framebuffer crop -- the geometry is re-rasterized at full resolution.
     * The shader epilogue already computes
     *     pos.xyz = _p.xyz * vp_posscale + _p.w * vp_posoffset
     * so recentring NDC point c and magnifying by k is exactly
     *     pos.xy = _p.xy * k + _p.w * (-c * k).
     * Rubber Ducky frames its ducks near the bottom edge of the viewport and its
     * camera only moves from the analog sticks, so without this there is no way
     * to look at the subject of the demo. It changes the view, nothing else:
     * same draws, same transform, same textures. */
    { static int z = -1; static float k = 1.0f, cx = 0.0f, cy = 0.0f;
      if (z < 0) { const char* e = getenv("DBG_ZOOM");
                   k = e ? (float)atof(e) : 1.0f;
                   z = (e && k > 0.0f) ? 1 : 0;
                   const char* ce = getenv("DBG_CENTER");
                   if (ce) { double a = 0, b = 0; sscanf(ce, "%lf,%lf", &a, &b);
                             cx = (float)a; cy = (float)b; } }
      if (z) { float ux = cx, uy = cy;
               static int lock = -1;
               PS3_KNOB(lock, ps3_env_int("DBG_LOCK", 0));
               if (lock && s_lock_valid) { ux = s_lock_x; uy = s_lock_y; }
               vpx[0] = k; vpx[1] = k; vpx[4] = -ux * k; vpx[5] = -uy * k; } }

    /* Garbage-projection fallback, for vkcube only, and now opt-in.
     *
     * It replaced c0-c3 with a stock perspective whenever c0.x fell outside
     * (0, 8), on the theory that c0-c3 is always a projection and a real one
     * has a small x-scale. vkcube's broken MatrixProjPerspective (a lifter
     * bug at the time) was the case it was written for. Nothing makes c0-c3 a
     * projection: Demon's Souls's level shaders keep c0.x = 300 there, and the
     * fallback rewrote their transforms on every such draw.
     *
     * VP_GUESSPROJ=1 brings the heuristic back (vkcube). VP_FIXPROJ=1 forces
     * the stock projection on every draw, as before. */
    int uses_c03 = (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n)
                       ? s_d3d.vp_vs[vs_idx].uses_c03 : s_d3d.vp_uses_c03;
    float* c = (float*)dst;
    /* c0.x from the state, not from the bank just copied: dst is the
     * write-combined upload heap, and reading it back waited for the whole
     * 8 KB copy to drain (~2% of the render thread at the Palace). The same
     * value. RSX_VCB_C00=0 reads the bank, as before. */
    static int c00_st = -1;
    PS3_KNOB(c00_st, ps3_env_int("RSX_VCB_C00", 1));
    float c00 = c00_st ? st->vertex_constants[0][0] : c[0];
    static int s_guess = -1;
    PS3_KNOB(s_guess, getenv("VP_GUESSPROJ") ? 1 : 0);
    int garbage = s_guess && !(c00 > 0.0f && c00 < 8.0f);
    static int nofix = -1, fix = -1;
    PS3_KNOB(nofix, getenv("VP_NOFIXPROJ") ? 1 : 0);
    PS3_KNOB(fix, getenv("VP_FIXPROJ") ? 1 : 0);
    if (nofix) garbage = 0;
    if ((garbage && uses_c03) || fix) {
        { static int _fb = 0; if (_fb < 6 && ++_fb)
            printf("[VPFB] fallback proj on draw slot %u (c00=%g vs_idx=%d)\n",
                   slot, c00, vs_idx); }
        for (int _i = 0; _i < 16; _i++) c[_i] = 0.0f;
        c[0]=1.358f; c[5]=2.414f; c[11]=1.0f;
        c[10]=1.0f/99.0f; c[14]=-1.0f/99.0f;
        vpx[2] = 1.0f; vpx[6] = 0.0f;
    }
    return slot;
}

/* Which offscreen surface (if any) does the current RSX state render to?
 * Returns 0 for a display buffer (backbuffer), else the surface's RAW RSX
 * offset. RTs are keyed by raw offset -- a texture bound at the same raw
 * offset is the same buffer (surface and texture registers share the offset
 * space; location bits differ but one title doesn't alias local vs main at
 * one offset), which sidesteps guessing the surface's context DMA location. */
static u32 current_rt_off(u32* out_w, u32* out_h, u32 out_mrt[3])
{
    extern int cellGcmOffsetIsDisplay(u32 offset);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_mrt) out_mrt[0] = out_mrt[1] = out_mrt[2] = 0;
    if (!st) return 0;
    /* SET_SURFACE_COLOR_TARGET: 1 = A, 2 = B, 0x13 = MRT1 (A+B),
     * 0x17 = MRT2 (A+B+C), 0x1F = MRT3 (A+B+C+D). */
    int sel = (st->color_target == 2) ? 1 : 0;
    u32 raw = st->surface_color_offset[sel];
    if (out_mrt) {
        if (st->color_target >= 0x13) out_mrt[0] = st->surface_color_offset[1];
        if (st->color_target >= 0x17) out_mrt[1] = st->surface_color_offset[2];
        if (st->color_target >= 0x1F) out_mrt[2] = st->surface_color_offset[3];
    }
    { static int _rs = -1; PS3_KNOB(_rs, getenv("RT_SEQDBG") ? 1 : 0);
      static u32 _last = 0; static int _n2 = 0;
      if (_rs && st->surface_color_offset[0] != _last && _n2 < 400000) {
          _last = st->surface_color_offset[0]; _n2++;
          fprintf(stderr, "[RTSEQ] #%d -> 0x%X%c", _n2, _last, 10); } }
    { static int _ro = -1; PS3_KNOB(_ro, getenv("RT_OFFDBG") ? 1 : 0);
      static u32 _seen[32]; static int _ns = 0;
      if (_ro) { u32 _k = st->surface_color_offset[0] ^ (st->color_target << 24)
                        ^ (st->surface_clip_w << 8) ^ st->surface_format;
        int _f = 0; for (int _i = 0; _i < _ns; _i++) if (_seen[_i] == _k) _f = 1;
        if (!_f && _ns < 32) { _seen[_ns++] = _k;
        fprintf(stderr, "[RTOFF] fmt=0x%X tgt=0x%X off[0]=0x%X off[1]=0x%X zeta=0x%X clip=%ux%u disp=%d -> 0x%X\n",
                st->surface_format, st->color_target, st->surface_color_offset[0], st->surface_color_offset[1],
                st->surface_zeta_offset, st->surface_clip_w, st->surface_clip_h,
                cellGcmOffsetIsDisplay(raw), cellGcmOffsetIsDisplay(raw) ? 0 : raw); } } }
    if (cellGcmOffsetIsDisplay(raw)) return 0;
    /* RT_DISPLAY_BY_SIZE=1: also treat a single-target surface whose clip
     * exactly matches the display resolution as the backbuffer.
     *
     * cellGcmSetDisplayBuffer only registers the buffers the FLIP may point at;
     * a guest is free to render into a different surface of the same size and
     * flip to it later (PSGL does). Matching offsets alone then classifies every
     * draw as offscreen, has_display stays 0, and render_frame() -- which is
     * what draws ALL recorded geometry -- is never called. The backbuffer shows
     * only the clear, which looks exactly like "nothing rasterizes". */
    { static int _bs = -1; PS3_KNOB(_bs, getenv("RT_DISPLAY_BY_SIZE") ? 1 : 0);
      if (_bs && st->color_target < 0x13 &&
          st->surface_clip_w == s_d3d.width && st->surface_clip_h == s_d3d.height)
          return 0; }
    /* Surface clip dims when sane; else the window size. Any size works --
     * passes draw normalized full-surface quads -- this only picks resolution. */
    u32 w = st->surface_clip_w, h = st->surface_clip_h;
    if (w < 16 || w > 2048 || h < 16 || h > 2048) { w = 0; h = 0; }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return raw;
}

/* RSX surface colour format (SET_SURFACE_FORMAT bits [4:0]) -> gpu_format. Float
 * targets matter: wave's water height maps store SIGNED values (F_W16..),
 * demosaic's differential planes likewise -- RGBA8 clamps them to zero. */
static gpu_format rsx_surface_format(u32 fmt)
{
    switch (fmt & 0x1F) {
    case 0x0B: return GPU_FORMAT_R16G16B16A16_FLOAT; /* F_W16Z16Y16X16 */
    case 0x0C: return GPU_FORMAT_R32G32B32A32_FLOAT; /* F_W32Z32Y32X32 */
    case 0x0D: return GPU_FORMAT_R32_FLOAT;          /* F_X32          */
    default:   return GPU_FORMAT_R8G8B8A8_UNORM;
    }
}

/* The format a target is made in: its own (rsx_surface_format), or FP16 for an
 * RGBA8 target HDR promotes (see HDR output). */
static gpu_format rt_target_format(u32 off, u32 fmt)
{
    const gpu_format f = rsx_surface_format(fmt);
    if (!s_hdr.stage_b || !off || f != GPU_FORMAT_R8G8B8A8_UNORM) return f;
    if (off == s_hdr.t_off) return GPU_FORMAT_R16G16B16A16_FLOAT;
    for (int i = 0; i < 4; i++)
        if (off == s_hdr.x_off[i]) return GPU_FORMAT_R16G16B16A16_FLOAT;
    return f;
}

/* A byte's value / 255 as a half float, to fill a promoted target from guest
 * memory. */
static u16 half_of_byte(u8 b)
{
    static u16 t[256];
    static int made;
    if (!made) {
        for (int i = 1; i < 256; i++) {
            const float f = (float)i / 255.0f;
            u32 x;
            memcpy(&x, &f, 4);
            const u32 e = ((x >> 23) & 0xFFu) - 127u + 15u, m = x & 0x7FFFFFu;   /* normal for 1/255 and up */
            t[i] = (u16)(((e << 10) | (m >> 13)) + ((m >> 12) & 1u));
        }
        made = 1;
    }
    return t[b];
}

/* NV4097 TEXTURE_ADDRESS wrap field (S: bits 0-3, T: 8-11, R: 16-19). */
static u32 rsx_wrap_to_address(u32 m)
{
    switch (m) {
    case 2: return GPU_ADDRESS_MIRROR;
    case 3: return GPU_ADDRESS_CLAMP;        /* CLAMP_TO_EDGE */
    case 4: return GPU_ADDRESS_BORDER;
    case 5: return GPU_ADDRESS_CLAMP;        /* GL CLAMP */
    case 6: case 7: case 8: return GPU_ADDRESS_MIRROR_ONCE;
    default: return GPU_ADDRESS_WRAP;        /* 1 = WRAP; 0 = never set */
    }
}

/* The sampler table (s0-s15) for one draw, from each unit's guest sampler
 * state; returns the table index. Unset units keep the old static defaults
 * (s0 point/clamp, the rest linear/wrap).
 *
 * Tables are shared by every draw with the same 16 sampler states. A session
 * meets more combinations than fit: Demon's Souls filled all 128 between the
 * tutorial and the Nexus, and every later combination used table 0's
 * addressing and filtering -- the colour-grading LUT wrapped instead of
 * clamping, and bright pixels came out blue, magenta and grey. A full heap now
 * gives up the table used longest ago, once the GPU is done with it. */
/* RSX_TEXFILTER (live; 0 the game's): 1 nearest, 2 bilinear, 3 trilinear,
 * for the units the game samples with mipmaps (min filter 3-6), which are its
 * surface textures. Colour-grading LUTs, render targets and other data are
 * sampled without mipmaps and keep the game's filter: a LUT read nearest
 * bands, and one read linear where the game reads it nearest blends entries.
 *
 * RSX_ANISO (live): 0 uses the game's own max anisotropy (TEXTURE_CONTROL0,
 * as RPCS3 does), 1-16 replaces it. It applies to mipmapped units whose min
 * and mag filters are linear, and never to depth comparison. The key's low
 * five bits carry the level, so a change makes new tables and the old ones
 * age out. */
static u32 tex_filter_key(u32 filt, int shadow)
{
    static int force = -1, aniso = -1;
    PS3_KNOB(force, ps3_env_int("RSX_TEXFILTER", 0));
    PS3_KNOB(aniso, ps3_env_int("RSX_ANISO", 0));
    u32 minf = (filt >> 16) & 7, magf = (filt >> 24) & 7;
    const int mipped = minf >= 3 && minf <= 6;
    if (mipped && !shadow && force >= 1 && force <= 3) {
        static const u8 fmin[4] = {0, 3, 4, 6}, fmag[4] = {0, 1, 2, 2};
        minf = fmin[force]; magf = fmag[force];
    }
    static const u8 guest_level[8] = {1, 2, 4, 6, 8, 10, 12, 16};
    u32 level = aniso > 0 ? (u32)(aniso > 16 ? 16 : aniso) : guest_level[(filt >> 20) & 7];
    if (shadow || !(minf == 4 || minf == 6) || magf != 2) level = 1;
    return (magf << 24) | (minf << 16) | level | (shadow ? 0x80000000u : 0u);
}

/* vp_sampler_table's hint slot for a key: four multiply-xor lanes over its
 * words, folded. */
#define VP_SAMP_HINTS 1024
static u32 samp_key_slot(const u32* k)
{
    u32 a = 0x9E3779B9u, b = 0x85EBCA6Bu, c = 0xC2B2AE35u, d = 0x27D4EB2Fu;
    for (int i = 0; i < RSX_FP_UNITS * 2; i += 4) {
        a = (a ^ k[i])     * 0x01000193u;
        b = (b ^ k[i + 1]) * 0x01000193u;
        c = (c ^ k[i + 2]) * 0x01000193u;
        d = (d ^ k[i + 3]) * 0x01000193u;
    }
    u32 h = a ^ (b << 7 | b >> 25) ^ (c << 14 | c >> 18) ^ (d << 21 | d >> 11);
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15;
    return h & (VP_SAMP_HINTS - 1);
}

static u32 vp_sampler_table(const D3D12DrawRecord* dr)
{
    static u32 keys[VP_SAMP_TABLES][RSX_FP_UNITS * 2];
    static u64 last[VP_SAMP_TABLES];   /* frame_count of the last draw using it */
    u32 k[RSX_FP_UNITS * 2];
    for (int u = 0; u < RSX_FP_UNITS; u++) {
        k[u * 2]     = dr && dr->tex[u].set ? dr->tex[u].addr : (u ? 0x10101u : 0x30303u);
        k[u * 2 + 1] = dr && dr->tex[u].set ? tex_filter_key(dr->tex[u].filt, dr_unit_shadow(dr, u))
                                            : (u ? 0x02060001u : 0x01010001u);
    }
    const u64 now = s_d3d.frame_count;
    /* RSX_SAMP_TABLES=<n> (2..128): use only n tables, to exercise the reuse
     * below without a two-hour session. */
    static u32 cap = 0;
    if (!cap) { int c = ps3_env_int("RSX_SAMP_TABLES", VP_SAMP_TABLES);
                cap = (u32)(c < 2 ? 2 : c > VP_SAMP_TABLES ? VP_SAMP_TABLES : c); }
    /* RSX_SAMP_HASH (live, default 1; 0 = compare with every table): a hash
     * of the key names the table to try first, checked against its stored
     * key. The scan below compares up to 128 keys of 128 bytes a draw. A
     * slot is only a hint: a miss, a collision or a table rewritten since
     * falls back to the scan, which sets the slot again. */
    static int hashed = 1;
    PS3_KNOB(hashed, ps3_env_int("RSX_SAMP_HASH", 1) != 0);
    static u16 hint[VP_SAMP_HINTS];   /* table + 1; 0 = none */
    u32 hs = 0;
    if (hashed) {
        hs = samp_key_slot(k);
        const u32 i = hint[hs];
        if (i && i <= s_d3d.samp_n && !memcmp(keys[i - 1], k, sizeof k)) { last[i - 1] = now; return i - 1; }
    }
    for (u32 i = 0; i < s_d3d.samp_n; i++)
        if (!memcmp(keys[i], k, sizeof k)) { last[i] = now; hint[hs] = (u16)(i + 1); return i; }
    u32 t;
    if (s_d3d.samp_n < cap) {
        t = s_d3d.samp_n++;
    } else {
        /* The frame being recorded may use any table it has bound, so only a
         * table it has not touched can be rewritten. One last used FRAME_COUNT
         * frames ago or more is done (move_to_next_frame waited for it);
         * a more recent one needs the queue drained first. */
        t = cap;
        for (u32 i = 0; i < cap; i++)
            if (last[i] != now && (t == cap || last[i] < last[t])) t = i;
        if (t == cap) {
            static int said = 0;
            if (!said) { said = 1;
                fprintf(stderr, "[D3D12] one frame uses more than %u sampler "
                                "combinations; the rest share table 0%c", cap, 10); }
            return 0;
        }
        if (last[t] + FRAME_COUNT > now) wait_for_gpu();
        { static int n = 0;
          if (n < 5 && ++n) fprintf(stderr, "[D3D12] sampler tables full (%u): reusing table %u, "
                               "last used in frame %llu%c", cap, t,
                               (unsigned long long)last[t], 10); }
    }
    last[t] = now;
    memcpy(keys[t], k, sizeof k);
    hint[hs] = (u16)(t + 1);
    /* Each unit's sampler is the GPU layer's, made once per description
     * (gpu_sampler_get), written into table t of the sampler pool; an entry
     * that already holds it is left as it is (step C4). */
    for (int u = 0; u < RSX_FP_UNITS; u++) {
        const u32 a = k[u * 2], f = k[u * 2 + 1];
        const u32 minf = (f >> 16) & 7, magf = (f >> 24) & 7;
        /* min: 1 NEAREST, 2 LINEAR, 3 N_MIP_N, 4 L_MIP_N, 5 N_MIP_L, 6 L_MIP_L,
         * 7 convolution; mag: 1 NEAREST, 2 LINEAR, 4 convolution. */
        const int min_lin = (minf == 2 || minf == 4 || minf == 6 || minf == 7);
        const int mip_lin = (minf == 5 || minf == 6);
        const int mag_lin = (magf == 2 || magf == 4);
        const u32 au = rsx_wrap_to_address(a & 0xF),
                  av = rsx_wrap_to_address((a >> 8) & 0xF),
                  aw = rsx_wrap_to_address((a >> 16) & 0xF);
        gpu_sampler_desc sd;
        memset(&sd, 0, sizeof sd);
        sd.filter = GPU_FILTER(
            min_lin ? GPU_FILTER_LINEAR : GPU_FILTER_POINT,
            mag_lin ? GPU_FILTER_LINEAR : GPU_FILTER_POINT,
            mip_lin ? GPU_FILTER_LINEAR : GPU_FILTER_POINT,
            (f & 0x80000000u) ? GPU_REDUCE_COMPARE : GPU_REDUCE_STANDARD);
        sd.address_u = (u32)au;
        sd.address_v = (u32)av;
        sd.address_w = (u32)aw;
        sd.max_aniso = 1;
        if ((f & 0x1Fu) > 1) {   /* tex_filter_key's anisotropy level */
            sd.filter = GPU_FILTER_ANISO(GPU_REDUCE_STANDARD);
            sd.max_aniso = f & 0x1Fu;
        }
        /* zfunc uses the GL order (GPU_CMP_* minus one), but the
         * stored function is the reverse of the sampling compare: LESS means
         * "texel < reference" (RPCS3 swaps LESS/GREATER and LEQUAL/GEQUAL the
         * same way in its GL sampler). */
        static const u8 zrev[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
        sd.compare = (f & 0x80000000u) ? (u32)(zrev[(a >> 28) & 7u] + 1u) : (u32)GPU_CMP_NEVER;
        sd.min_lod = 0.0f;
        sd.max_lod = (minf == 1 || minf == 2) ? 0.0f : GPU_LOD_MAX;  /* no mipmapping */
        gpu_fast_table_write_sampler(s_rc_dev, s_rc_samp_pool, t * RSX_FP_UNITS + (u32)u,
                                     gpu_sampler_get(s_rc_dev, &sd));
    }
    return t;
}

/* Table t of the sampler pool (gpu_fast_table_ref: today's heap start +
 * t * 16 * increment), on the frame's recorder (step C4). */
static void vp_bind_sampler_table(const D3D12DrawRecord* dr)
{
    if (!GPU_VALID(s_rc_samp_pool)) return;
    const u32 t = vp_sampler_table(dr);
    gpu_cmd_bind_table(s_rc_cmd, 3, gpu_fast_table_ref(s_rc_dev, s_rc_samp_pool, t * RSX_FP_UNITS));
}

/* The grave (step C8a: the one deferred-destroy list; step C5a's and step
 * C5b's write-back grave were two). Textures and buffers dropped while a
 * frame is being recorded may still be used by that frame's list: they wait
 * here, and at the next render_frame's start, after its opening wait
 * (rc_grave_release), each goes to gpu_*_destroy with the fence of the last
 * frame submitted, which the GPU layer frees it after. One dropped while a
 * frame is recorded was last used by that frame, and one dropped before
 * (rsx_scale_apply) by an earlier one. A texture's views go with it. Grows
 * as needed: a frame that flips a surface between two formats draw by draw
 * drops a target each time. In it: the offscreen targets and their staging,
 * zeta, the display and depth targets, SMAA's targets, the AA display copy,
 * the sub-viewport captures, and the write-back's downscale copies,
 * snapshots and screen copy (which waited for a later render_frame's
 * pre-pass, wb_tex_bury: the same point, after the same wait). */
typedef struct { gpu_texture tex; gpu_buffer buf; } RcGrave;
static RcGrave* s_rc_grave;
static int s_rc_grave_n, s_rc_grave_cap;

/* The last frame's fence: the highest of the frame slots' (one timeline). */
static gpu_fence rc_last_frame_fence(void)
{
    gpu_fence f = 0;
    for (u32 i = 0; i < FRAME_COUNT; i++)
        if (s_rc_frame_fence[i] > f) f = s_rc_frame_fence[i];
    return f;
}

static void rc_grave_release(void)
{
    if (s_rc_grave_n) {
        const gpu_fence after = rc_last_frame_fence();
        for (int i = 0; i < s_rc_grave_n; i++) {
            if (GPU_VALID(s_rc_grave[i].tex)) gpu_texture_destroy(s_rc_dev, s_rc_grave[i].tex, after);
            if (GPU_VALID(s_rc_grave[i].buf)) gpu_buffer_destroy(s_rc_dev, s_rc_grave[i].buf, after);
        }
        s_rc_grave_n = 0;
    }
}

/* t and b (either may be none) into the grave. */
static void rc_grave_put(gpu_texture t, gpu_buffer b)
{
    if (!GPU_VALID(t) && !GPU_VALID(b)) return;
    if (s_rc_grave_n == s_rc_grave_cap) {
        int nc = s_rc_grave_cap ? s_rc_grave_cap * 2 : 64;
        RcGrave* g = (RcGrave*)realloc(s_rc_grave, (size_t)nc * sizeof *g);
        if (!g) {   /* best effort: at once, once the GPU is idle */
            wait_for_gpu();
            if (GPU_VALID(t)) gpu_texture_destroy(s_rc_dev, t, 0);
            if (GPU_VALID(b)) gpu_buffer_destroy(s_rc_dev, b, 0);
            return;
        }
        s_rc_grave = g; s_rc_grave_cap = nc;
    }
    s_rc_grave[s_rc_grave_n].tex = t;
    s_rc_grave[s_rc_grave_n].buf = b;
    s_rc_grave_n++;
}

/* A texture dropped (a target, a copy, a capture): its handle (and views)
 * into the grave; *t is none after. */
static void rc_grave_tex(gpu_texture* t)
{
    const gpu_buffer none = {0};
    rc_grave_put(*t, none);
    t->id = 0;
}

/* An offscreen target and its init staging, dropped (step C5a): the handles
 * go into the grave, and the RTV is cleared. */
static void off_rt_drop_target(OffRT* r)
{
    rc_grave_put(r->tex, r->up_buf);
    r->tex.id = 0;
    r->up_buf.id = 0;
    r->rtv.id = 0;
}

static int off_rt_find(u32 off)
{
    if (!off) return -1;
    for (int i = 0; i < MAX_OFF_RTS; i++)
        if (GPU_VALID(s_d3d.off_rt[i].tex) && s_d3d.off_rt[i].off == off) return i;
    return -1;
}

/* Depth surfaces, one per guest zeta offset. RSX keeps depth in memory like
 * any other surface: a pass that switches to another target (a shadow
 * cascade, a luminance step) and comes back finds its depth where it left it,
 * and only the guest's own CLEAR_SURFACE Z/S bits reset it. A single shared
 * buffer cleared on every switch lost the scene's depth halfway through
 * Demon's Souls's frame, so later scene draws covered the player.
 * Created typeless (R24G8) so a later pass can sample one as a texture. */
typedef struct {
    gpu_texture tex;    /* the GPU layer's texture (step C5a) */
    gpu_view dsv, dsv_ro;   /* its writable and read-only DSVs (step C5a) */
    u32 off, w, h;      /* guest dims */
    u32 hw, hh;         /* the resource's dims (RSX_SCALE) */
    int used;
    gpu_state st;   /* DEPTH_WRITE as a DSV, PIXEL_SHADER_RESOURCE sampled */
} ZetaRT;
#define MAX_ZETA 8
static ZetaRT s_zeta[MAX_ZETA];

/* The surface's DSV, as zeta_get made it (a view: step C5a; the view itself
 * for the frame's inline commands, step C7). */
static gpu_view zeta_dsv(int slot) { return s_zeta[slot].dsv; }

/* The same surface as a read-only depth-stencil view: depth-tested and
 * sampled by one draw. The resource is then in ZETA_READ_STATE. */
#define ZETA_READ_STATE ((gpu_state)(GPU_STATE_DEPTH_READ | GPU_STATE_PIXEL_SHADER_RESOURCE))
static gpu_view zeta_dsv_ro(int slot) { return s_zeta[slot].dsv_ro; }

/* A depth surface dropped (step C5a): its handle and views into the grave. */
static void zeta_drop(ZetaRT* z)
{
    rc_grave_tex(&z->tex);
    z->dsv.id = z->dsv_ro.id = 0;
}

static int zeta_find(u32 off)
{
    for (int i = 0; i < MAX_ZETA; i++)
        if (GPU_VALID(s_zeta[i].tex) && s_zeta[i].off == off) return i;
    return -1;
}

static void zeta_transition(int slot, gpu_state to)
{
    ZetaRT* z = &s_zeta[slot];
    if (z->st == to) return;
    gpu_cmd_barrier(s_rc_cmd, z->tex, z->st, to);   /* (step C5a) */
    z->st = to;
}

/* Ensure a depth surface for this zeta offset at least w x h. A larger clip
 * recreates it (content lost once); a smaller one reuses it, since a pass may
 * render a sub-rectangle of the same surface. New surfaces start cleared. */
static int zeta_get(u32 off, u32 w, u32 h)
{
    if (!w) w = s_d3d.width;
    if (!h) h = s_d3d.height;
    int slot = zeta_find(off);
    if (slot >= 0) {
        ZetaRT* z = &s_zeta[slot];
        if (z->w >= w && z->h >= h) { z->used = 1; return slot; }
        if (z->w > w) w = z->w;
        if (z->h > h) h = z->h;
        zeta_drop(z);
    } else {
        for (int i = 0; i < MAX_ZETA; i++)
            if (!GPU_VALID(s_zeta[i].tex)) { slot = i; break; }
        if (slot < 0) {
            for (int i = 0; i < MAX_ZETA; i++)
                if (!s_zeta[i].used) { slot = i; break; }
            if (slot < 0) return -1;
            zeta_drop(&s_zeta[slot]);
        }
    }
    ZetaRT* z = &s_zeta[slot];
    /* The GPU layer's texture (step C5a): typeless R24G8,
     * cleared to depth 1.0 fastest, sampled as depth (the SRV writes). Its
     * DSVs are D24S8 views, writable and read-only. */
    gpu_texture_desc td;
    memset(&td, 0, sizeof td);
    td.width = rt_host_dim(w, h, w); td.height = rt_host_dim(w, h, h);
    td.layers = 1; td.mips = 1;
    td.format = GPU_FORMAT_R24G8_TYPELESS; td.samples = 1;
    td.use = GPU_USE_DS | GPU_USE_SAMPLED;
    td.state = GPU_STATE_DEPTH_WRITE;
    td.clear_format = GPU_FORMAT_D24_UNORM_S8_UINT;
    td.clear_depth = 1.0f;
    z->tex = gpu_texture_create(s_rc_dev, &td);
    if (!GPU_VALID(z->tex)) return -1;
    z->off = off; z->w = w; z->h = h; z->used = 1;
    z->hw = td.width; z->hh = td.height;
    z->st = GPU_STATE_DEPTH_WRITE;
    z->dsv = rc_target_view(z->tex, GPU_VIEW_DSV, 0, GPU_FORMAT_D24_UNORM_S8_UINT);
    z->dsv_ro = rc_target_view(z->tex, GPU_VIEW_DSV, GPU_VIEW_READ_ONLY_DEPTH | GPU_VIEW_READ_ONLY_STENCIL,
                               GPU_FORMAT_D24_UNORM_S8_UINT);
    gpu_cmd_clear_depth(s_rc_cmd, z->dsv, GPU_CLEAR_DEPTH | GPU_CLEAR_STENCIL, 1.0f, 0, NULL);
    printf("[ZETA] depth surface 0x%08X %ux%u (host %ux%u, slot %d)\n", off, w, h, z->hw, z->hh, slot);
    return slot;
}

/* Ensure an RT resource exists for this surface; (re)creates its RTV (a view
 * of the texture, step C5a) and the SRV at the SRV pool's RT_SRV_BASE+i. */
static int off_rt_get(u32 off, u32 w, u32 h, u32 rsx_fmt)
{
    if (!off || !GPU_VALID(s_rc_srv_pool)) return -1;
    if (!w) w = s_d3d.width;
    if (!h) h = s_d3d.height;
    gpu_format want_fmt = rt_target_format(off, rsx_fmt);
    gpu_format gfmt = rsx_surface_format(rsx_fmt);   /* the guest's, for refill and write-back */
    /* RT_FORCE_RGBA8=1: allocate every offscreen target as R8G8B8A8 rather than
     * its native format. A title whose final composite reads a half-float
     * surface cannot be shown with a plain CopyTextureRegion, and giving it an
     * 8-bit target costs HDR precision but makes the composite a same-format
     * copy. A stopgap for looking at the frame, not a rendering fix. */
    { static int f8 = -1;
      PS3_KNOB(f8, ps3_env_int("RT_FORCE_RGBA8", 0));
      if (f8) want_fmt = gfmt = GPU_FORMAT_R8G8B8A8_UNORM; }
    int slot = off_rt_find(off);
    if (slot >= 0) {
        OffRT* r = &s_d3d.off_rt[slot];
        if (r->w == w && r->h == h && r->dxgi == (u32)want_fmt) { r->used = 1; return slot; }
        /* The frame being recorded may already reference this resource (its
         * init upload, its barriers, or draws from earlier in the pre-pass), and
         * releasing it before that list runs is "deleted prior to closing the
         * command list" (D3D12 message 921). wait_for_gpu() only drains lists
         * already submitted, not the open one, so the release is deferred to
         * after the GPU finishes this frame. Demon's Souls declares its
         * luminance targets as A8R8G8B8 and then as F_X32 at the same offsets;
         * releasing at once removed the device about one run in two. */
        off_rt_drop_target(r);     /* dims/format changed */
    } else {
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (!GPU_VALID(s_d3d.off_rt[i].tex)) { slot = i; break; }
        if (slot < 0) {   /* evict an entry not used this frame */
            for (int i = 0; i < MAX_OFF_RTS; i++)
                if (!s_d3d.off_rt[i].used) { slot = i; break; }
            if (slot < 0) {
                /* Every target is in use this frame: the draws into this one
                 * are dropped, and whatever samples it reads nothing. */
                static u64 said = ~0ull;
                if (said != s_d3d.frame_count) { said = s_d3d.frame_count;
                    static int n = 0;
                    if (n < 20 && ++n) fprintf(stderr, "[RT] all %d offscreen targets in use this "
                                          "frame: 0x%X (%ux%u) not created%c",
                                          MAX_OFF_RTS, off, w, h, 10); }
                return -1;
            }
            { static int n = 0;   /* a target a later frame reads loses its content */
              if (n < 20 && ++n) fprintf(stderr, "[RT] evicted 0x%X (%ux%u) for 0x%X (%ux%u)%c",
                                    s_d3d.off_rt[slot].off, s_d3d.off_rt[slot].w,
                                    s_d3d.off_rt[slot].h, off, w, h, 10); }
            /* same hazard as above: the open list may still use it */
            off_rt_drop_target(&s_d3d.off_rt[slot]);
        }
    }
    OffRT* r = &s_d3d.off_rt[slot];
    const u32 hw = rt_host_dim(w, h, w), hh = rt_host_dim(w, h, h);
    /* The GPU layer's texture (step C5a): made in COPY_DEST
     * for the refill below, cleared fastest to zero in its own format. */
    gpu_texture_desc td;
    memset(&td, 0, sizeof td);
    td.width = hw; td.height = hh; td.layers = 1; td.mips = 1;
    td.format = (gpu_format)want_fmt; td.samples = 1;
    td.use = GPU_USE_RT | GPU_USE_SAMPLED | GPU_USE_COPY_SRC | GPU_USE_COPY_DST;
    td.state = GPU_STATE_COPY_DEST;
    td.clear_format = td.format;
    r->tex = gpu_texture_create(s_rc_dev, &td);
    if (!GPU_VALID(r->tex)) return -1;
    r->off = off; r->w = w; r->h = h; r->hw = hw; r->hh = hh; r->dxgi = (u32)want_fmt;
    r->gdxgi = (u32)gfmt;
    r->st = GPU_STATE_COPY_DEST;
    r->used = 1;
    r->gsum_ok = 0;

    /* Populate the new RT from guest memory: titles CPU-initialise their
     * render-target buffers (wave fills the height fields with texels of
     * (0,0,0, 1.0f) -- the .w is 'inverse of mass'; from an all-zero GPU
     * resource the water simulation can never boot). Guest data is
     * big-endian and tightly packed. */
    {
        extern uint8_t* vm_base;
        extern u32 cellGcmResolveOffset(u32);
        u32 bpp = (want_fmt == GPU_FORMAT_R16G16B16A16_FLOAT) ? 8u :
                  (want_fmt == GPU_FORMAT_R32G32B32A32_FLOAT) ? 16u :
                  (want_fmt == GPU_FORMAT_R32_FLOAT) ? 4u : 4u;
        const u32 sbpp = want_fmt == gfmt ? bpp : 4u;   /* a promoted target's guest texels: RGBA8 */
        /* A scaled target gets each guest texel repeated (nearest). */
        u32 pitch = (hw * bpp + 255) & ~255u;
        u32 src = cellGcmResolveOffset(off);
        u8* wide = (hw != w) ? (u8*)malloc((size_t)w * bpp) : NULL;
        if (vm_base && src != 0xFFFFFFFFu) {
            /* The staging: a GPU layer upload buffer, mapped for its life
             * (step C5a). */
            gpu_buffer_desc bd;
            memset(&bd, 0, sizeof bd);
            bd.size = (u64)pitch * hh;
            bd.use = GPU_USE_COPY_SRC;
            bd.memory = GPU_MEM_UPLOAD;
            bd.state = GPU_STATE_GENERIC_READ;
            if (hw == w || wide) r->up_buf = gpu_buffer_create(s_rc_dev, &bd);
            if (GPU_VALID(r->up_buf)) {
                void* mp = gpu_buffer_map(s_rc_dev, r->up_buf, 0, 0);
                if (mp) {
                    const u8* sp = vm_base + src;
                    for (u32 y = 0; y < hh; y++) {
                        const u8* srow = sp + (u64)((u64)y * h / hh) * w * sbpp;
                        u8* hrow = (u8*)mp + (u64)y * pitch;
                        u8* drow = wide ? wide : hrow;
                        if (want_fmt != gfmt) {
                            /* promoted (HDR): guest A8R8G8B8 -> R,G,B,A halves */
                            for (u32 x = 0; x < w; x++) {
                                const u16 hv[4] = {half_of_byte(srow[x*4+1]), half_of_byte(srow[x*4+2]),
                                                   half_of_byte(srow[x*4+3]), half_of_byte(srow[x*4+0])};
                                memcpy(drow + (u64)x * 8, hv, 8);
                            }
                        } else if (want_fmt == GPU_FORMAT_R8G8B8A8_UNORM) {
                            /* guest A8R8G8B8 (bytes A,R,G,B) -> R,G,B,A */
                            for (u32 x = 0; x < w; x++) {
                                drow[x*4+0] = srow[x*4+1];
                                drow[x*4+1] = srow[x*4+2];
                                drow[x*4+2] = srow[x*4+3];
                                drow[x*4+3] = srow[x*4+0];
                            }
                        } else if (want_fmt == GPU_FORMAT_R16G16B16A16_FLOAT) {
                            for (u32 x = 0; x < w * 4; x++) {   /* u16 halves, BE */
                                drow[x*2+0] = srow[x*2+1];
                                drow[x*2+1] = srow[x*2+0];
                            }
                        } else {                                 /* u32 floats, BE */
                            u32 nw = (w * bpp) / 4;
                            for (u32 x = 0; x < nw; x++) {
                                drow[x*4+0] = srow[x*4+3];
                                drow[x*4+1] = srow[x*4+2];
                                drow[x*4+2] = srow[x*4+1];
                                drow[x*4+3] = srow[x*4+0];
                            }
                        }
                        if (wide)
                            for (u32 x = 0; x < hw; x++)
                                memcpy(hrow + (u64)x * bpp, wide + (u64)((u64)x * w / hw) * bpp, bpp);
                    }
                    /* (an upload buffer stays mapped: the unmap writes nothing) */
                    gpu_buffer_unmap(s_rc_dev, r->up_buf, 0, bd.size);
                    gpu_footprint fp;   /* the whole buffer, from offset 0 */
                    memset(&fp, 0, sizeof fp);
                    fp.format    = (gpu_format)want_fmt;
                    fp.width     = hw;
                    fp.height    = hh;
                    fp.depth     = 1;
                    fp.row_pitch = pitch;
                    gpu_cmd_copy_buf_to_tex(s_rc_cmd, r->tex, 0, 0, 0, 0, r->up_buf, &fp);
                }
            }
        }
        free(wide);
        gpu_cmd_barrier(s_rc_cmd, r->tex, GPU_STATE_COPY_DEST, GPU_STATE_PIXEL_SHADER_RESOURCE);
        r->st = GPU_STATE_PIXEL_SHADER_RESOURCE;
    }

    r->rtv = rc_target_view(r->tex, GPU_VIEW_RTV, 0, 0);   /* today's NULL description */

    gpu_view_desc sv;   /* in the SRV pool (step C4) */
    memset(&sv, 0, sizeof sv);
    sv.texture = r->tex;
    sv.kind = GPU_VIEW_SRV;
    sv.dim = GPU_DIM_2D;
    sv.mips = 1;
    sv.format = td.format;
    sv.swizzle = GPU_SWIZZLE_IDENTITY;
    gpu_table_write_tex(s_rc_dev, s_rc_srv_pool, RT_SRV_BASE + (u32)slot, &sv);

    static int _log = 0;
    if (_log < 8 && ++_log)
        printf("[D3D12] offscreen RT %d: off=0x%X %ux%u host %ux%u (render-to-texture)\n",
               slot, off, w, h, hw, hh);
    return slot;
}

