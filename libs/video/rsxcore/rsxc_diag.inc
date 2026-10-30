/* Per-draw fingerprints (live and replay): one line per record with the
 * inputs the batch actually renders from -- the emitted vertices, the VP and
 * FP constant slots, the pipeline keys, the vertex program's ucode hash, and
 * per texture unit what it resolved to (T = uploaded texture, with the
 * checksum of the bytes uploaded; R = offscreen target; Z = depth surface;
 * - = nothing). A live frame and the replay of its F9 capture should print
 * the same lines; the first that differs names the state a replay cannot
 * see. F9 writes them next to the live presents as cap_NNN_draws_XX_d<n>.txt;
 * RSX_DRAWPRINT=<prefix> (live, replay) writes the next RSX_DRAWPRINT_N
 * batches (default 4) as <prefix>_XX_d<n>.txt. */
static char s_dp_prefix[480];
static int s_dp_left, s_dp_n;
static u32 dp_hash(const void* p, size_t n)
{
    const u8* b = (const u8*)p;
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static void drawprint_write(const char* path, const char* hdr);
static u32 s_batch_seq, s_cap_mark_seq[2], s_cap_mark_at[2];
static volatile int s_cap_mark_ok[2];
static void cap_mark(int end)
{
    s_cap_mark_seq[end] = s_batch_seq;
    s_cap_mark_at[end] = s_d3d.draw_count;
    s_cap_mark_ok[end] = 1;
}
static void drawprint_batch(void)
{
    { static u64 gen = (u64)-1;
      if (gen != ps3_env_gen) { gen = ps3_env_gen;
          const char* e = getenv("RSX_DRAWPRINT");
          static char last[480];
          if (e && *e && strcmp(e, last)) {
              snprintf(last, sizeof last, "%s", e);
              snprintf(s_dp_prefix, sizeof s_dp_prefix, "%s", e);
              s_dp_left = ps3_env_int("RSX_DRAWPRINT_N", 4); s_dp_n = 0;
          } } }
    char path[512];
    /* A capture's start and end, as (batch, record) marks: the batches that
     * hold them are written as <capture>_cap<k>_d<draws>.txt with "# capture
     * starts/ends at record N" lines, so the live records line up one for
     * one with the replay's. */
    { extern char g_rcap_fin_name[512];
      int in0 = s_cap_mark_ok[0] && s_cap_mark_seq[0] == s_batch_seq;
      int in1 = s_cap_mark_ok[1] && s_cap_mark_seq[1] == s_batch_seq;
      if (in0 || in1) {
          snprintf(path, sizeof path, "%s_cap%d_d%u.txt", g_rcap_fin_name, in0 ? 0 : 1,
                   s_d3d.draw_count);
          char hdr[96]; hdr[0] = 0;
          if (in0) snprintf(hdr, sizeof hdr, "# capture starts at record %u%c", s_cap_mark_at[0], 10);
          if (in1) snprintf(hdr + strlen(hdr), sizeof hdr - strlen(hdr),
                            "# capture ends at record %u%c", s_cap_mark_at[1], 10);
          drawprint_write(path, hdr);
          if (in1) s_cap_mark_ok[0] = s_cap_mark_ok[1] = 0;
      } }
    s_batch_seq++;
    if (s_dp_left <= 0 || !s_dp_prefix[0]) return;
    s_dp_left--;
    snprintf(path, sizeof path, "%s_%02d_d%u.txt", s_dp_prefix, s_dp_n++, s_d3d.draw_count);
    drawprint_write(path, "");
}
/* FP_WATCH (live, diagnostic): catch a flicker without timing a capture.
 *   FP_WATCH=<hex>[,<hex>...]  log each change in how many draws a full batch
 *                              makes with these fragment programs, and each
 *                              of their draws that samples the depth surface
 *                              it is bound to as writable (it reads nothing);
 *   FP_WATCH=all               log any program a full batch drew, the next one
 *                              did not, and the one after drew again.
 * A full batch has at least FP_WATCH_MIN draws (default 100), which leaves out
 * the skip batches that only copy the last frame to the display. At most
 * FP_WATCH_MAX lines (default 400). Lines start [fpwatch]. */
typedef struct { u32 fp, n; } FpCount;
#define FPW_MAX 1024
static int fpw_cmp(const void* a, const void* b)
{
    u32 x = *(const u32*)a, y = *(const u32*)b;
    return x < y ? -1 : x > y;
}
static u32 s_fpw_list[16]; static int s_fpw_list_n, s_fpw_all, s_fpw_lines;
static u32 s_fpw_unread[16]; static int s_fpw_unread_n;
static void fpwatch_parse(void)
{
    static unsigned gen; if (gen == ps3_env_gen) return; gen = ps3_env_gen;
    const char* e = getenv("FP_WATCH");
    s_fpw_list_n = 0; s_fpw_all = e && !strcmp(e, "all");
    if (e && !s_fpw_all)
        for (const char* p = e; *p && s_fpw_list_n < 16; ) {
            char* q; u32 v = (u32)strtoul(p, &q, 16);
            if (q == p) break;
            s_fpw_list[s_fpw_list_n++] = v;
            p = *q == ',' ? q + 1 : q;
        }
}
static int fpwatch_listed(u32 fp)
{
    for (int i = 0; i < s_fpw_list_n; i++) if (s_fpw_list[i] == fp) return 1;
    return 0;
}
/* A watched draw sampling its own writable depth surface (bound as NULL). */
static void fpwatch_unreadable(u32 fp)
{
    if (!s_fpw_list_n || !fpwatch_listed(fp)) return;
    for (int i = 0; i < s_fpw_unread_n; i++) if (s_fpw_unread[i] == fp) return;
    if (s_fpw_unread_n < 16) s_fpw_unread[s_fpw_unread_n++] = fp;
}
static u32 fpwatch_count(const FpCount* c, int n, u32 fp)
{
    for (int i = 0; i < n; i++) if (c[i].fp == fp) return c[i].n;
    return 0;
}
static void fpwatch_batch(void)
{
    fpwatch_parse();
    if (!s_fpw_all && !s_fpw_list_n) { s_fpw_unread_n = 0; return; }
    static FpCount hist[3][FPW_MAX]; static int hn[3]; static u32 hframe[3];
    static u32* fps;
    const int maxl = ps3_env_int("FP_WATCH_MAX", 400);
    const u32 full = (u32)ps3_env_int("FP_WATCH_MIN", 100);
    if (!fps) fps = (u32*)malloc(MAX_DRAWS * sizeof *fps);
    u32 n = 0;
    for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS && fps; d++)
        if (s_d3d.draws[d].is_vp && !s_d3d.draws[d].is_clear) fps[n++] = s_d3d.draws[d].fp_addr;
    for (int i = 0; i < s_fpw_unread_n && s_fpw_lines < maxl; i++, s_fpw_lines++)
        printf("[fpwatch] frame %u fp %X samples its own writable depth surface (reads 0)\n",
               (unsigned)s_d3d.frame_count, s_fpw_unread[i]);
    s_fpw_unread_n = 0;
    if (n < full) return;
    /* Shift the history and run-length the sorted program addresses. */
    memmove(&hist[0], &hist[1], 2 * sizeof hist[0]);
    hn[0] = hn[1]; hn[1] = hn[2]; hframe[0] = hframe[1]; hframe[1] = hframe[2];
    qsort(fps, n, sizeof *fps, fpw_cmp);
    int k = 0;
    for (u32 i = 0; i < n && k < FPW_MAX; ) {
        u32 j = i; while (j < n && fps[j] == fps[i]) j++;
        hist[2][k].fp = fps[i]; hist[2][k].n = j - i; k++; i = j;
    }
    hn[2] = k; hframe[2] = (u32)s_d3d.frame_count;
    if (s_fpw_list_n) {
        for (int i = 0; i < s_fpw_list_n && s_fpw_lines < maxl; i++) {
            u32 was = fpwatch_count(hist[1], hn[1], s_fpw_list[i]);
            u32 now = fpwatch_count(hist[2], hn[2], s_fpw_list[i]);
            if (was != now) {
                s_fpw_lines++;
                printf("[fpwatch] frame %u fp %X draws %u -> %u (batch %u draws)\n",
                       hframe[2], s_fpw_list[i], was, now, n);
            }
        }
        return;
    }
    for (int i = 0; i < hn[2] && s_fpw_lines < maxl; i++) {
        u32 fp = hist[2][i].fp;
        u32 a = fpwatch_count(hist[0], hn[0], fp);
        if (a && !fpwatch_count(hist[1], hn[1], fp)) {
            s_fpw_lines++;
            printf("[fpwatch] frame %u fp %X missing: %u draws at frame %u, 0 at %u, %u at %u\n",
                   hframe[2], fp, a, hframe[0], hframe[1], hist[2][i].n, hframe[2]);
        }
    }
}
/* A texture slot's guest image decoded into a scratch buffer, hashed. */
static u32 vp_tex_decoded_hash(const VPTexSlot* t)
{
    extern uint8_t* vm_base;
    if (!t->res || !vm_base) return 0;
    rsx_tex_layout tl;
    rsx_texture_layout(t->fmt, t->w, t->h, &tl);
    const u32 pitch = (tl.dst_row_bytes + 255) & ~255u;
    const u32 rows  = tl.compressed ? tl.rows : t->h;
    const u32 nf    = t->cube ? 6u : 1u;
    const size_t n  = (size_t)pitch * rows * nf;
    u8* buf = (u8*)calloc(1, n);        /* zeroed: the row padding is hashed too */
    if (!buf) return 0;
    for (u32 f = 0; f < nf; f++)
        rsx_texture_decode(buf + (size_t)f * pitch * rows, pitch,
                           vm_base + t->off + f * t->face_bytes, t->w, t->h, &tl,
                           rsx_texture_argb_is_rgba());
    const u32 h = dp_hash(buf, n);
    free(buf);
    return h;
}
static void drawprint_write(const char* path, const char* hdr)
{
    FILE* f = fopen(path, "w");
    if (!f) return;
    fputs(hdr, f);
    static u32 uh[VP_TEX_SLOTS], uh_file[VP_TEX_SLOTS], uh_gen;
    uh_gen++;
    const u32 par = (u32)s_d3d.vp_parity;
    for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++) {
        const D3D12DrawRecord* dr = &s_d3d.draws[d];
        if (dr->is_clear) {
            fprintf(f, "d%u CLEAR rt=%X flags=%X z=%X\n", d, dr->rt_off, dr->clr_flags, dr->zeta);
            continue;
        }
        if (!dr->is_vp) { fprintf(f, "d%u legacy n=%u\n", d, dr->vertex_count); continue; }
        u32 vsh = (dr->vs_idx >= 0 && dr->vs_idx < s_d3d.vp_vs_n) ? s_d3d.vp_vs[dr->vs_idx].hash : 0;
        /* Every 4th vertex: the upload heap is write-combined, slow to read. */
        u32 vb = 0;
        if (s_d3d.vp_vb_mapped) {
            const u8* v0 = (const u8*)s_d3d.vp_vb_mapped
                + (u64)par * s_vp_cap * 256u /* VP_VERT_STRIDE */ + dr->vb_byte_offset;
            vb = 2166136261u;
            for (u32 k = 0; k < dr->vertex_count; k += 4)
                vb = (vb ^ dp_hash(v0 + (u64)k * 256u, 256u)) * 16777619u;
        }
        u32 cb = (s_d3d.vp_cb_mapped && dr->vcb_slot < MAX_DRAWS) ? dp_hash((const char*)s_d3d.vp_cb_mapped
                     + ((u64)par * MAX_DRAWS + dr->vcb_slot) * VP_CB_STRIDE, VP_CB_STRIDE) : 0;
        u32 fcb = (s_d3d.vp_fpcb_mapped && dr->cb_slot < MAX_DRAWS) ? dp_hash((const char*)s_d3d.vp_fpcb_mapped
                     + ((u64)par * MAX_DRAWS + dr->cb_slot) * VP_FPCB_STRIDE,
                     (size_t)(FPCB_K + s_fpk_n[dr->cb_slot]) * 16u) : 0;
        fprintf(f, "d%u fp=%X vs=%08X n=%u bk=%X dep=%X cull=%X cm=%X rt=%X z=%X d2c=%X vb=%08X cb=%08X fcb=%08X",
                d, dr->fp_addr, vsh, dr->vertex_count, dr->blend_key, dr->depth, dr->cull,
                dr->cmask, dr->rt_off, dr->zeta, dr->d2c_mask, vb, cb, fcb);
        for (int u = 0; u < RSX_FP_UNITS; u++) {
            if (!dr->tex[u].set) continue;
            if (dr->tex_rt[u] >= 0) { fprintf(f, " t%d=R%X", u, s_d3d.off_rt[dr->tex_rt[u]].off); continue; }
            if (dr->tex_z[u] >= 0)  { fprintf(f, " t%d=Z%X", u, s_zeta[dr->tex_z[u]].off); continue; }
            int ts = vp_tex_find(dr->tex[u].off, dr->tex[u].w, dr->tex[u].h, dr->tex[u].fmt,
                                 dr->tex[u].cube && (dr_cube_mask(dr) & (1u << u)));
            /* /<hash>: the slot's texture decoded as an upload decodes it,
             * from guest memory now; the checksum is of guest memory at the
             * last upload, so the two together say whether the GPU's copy is
             * current. */
            if (ts >= 0) {
                const VPTexSlot* t = &s_d3d.vp_tex[ts];
                if (uh_file[ts] != uh_gen) {   /* once per slot per file */
                    uh[ts] = vp_tex_decoded_hash(t); uh_file[ts] = uh_gen;
                }
                fprintf(f, " t%d=T%X:%08X/%08X", u, dr->tex[u].raw, t->csum, uh[ts]);
            }
            else fprintf(f, " t%d=-%X", u, dr->tex[u].raw);
        }
        /* RSX_DRAWPRINT_K=1: the inline FP constants and texture scales too. */
        { static int pk = -1; PS3_KNOB(pk, ps3_env_int("RSX_DRAWPRINT_K", 0));
          if (pk && s_d3d.vp_fpcb_mapped && dr->cb_slot < MAX_DRAWS) {
              const float* c = (const float*)((const char*)s_d3d.vp_fpcb_mapped
                  + ((u64)par * MAX_DRAWS + dr->cb_slot) * VP_FPCB_STRIDE);
              fprintf(f, " at=(%g,%g,%g)", c[FPCB_ALPHA*4], c[FPCB_ALPHA*4+1], c[FPCB_ALPHA*4+2]);
              for (int u = 0; u < 16; u++)
                  if (c[u*4] != 1.0f || c[u*4+1] != 1.0f) fprintf(f, " ts%d=(%g,%g)", u, c[u*4], c[u*4+1]);
              for (int k = 0; k < s_fpk_n[dr->cb_slot]; k++)
                  fprintf(f, " k%d=(%g,%g,%g,%g)", k, c[(FPCB_K+k)*4], c[(FPCB_K+k)*4+1],
                          c[(FPCB_K+k)*4+2], c[(FPCB_K+k)*4+3]);
          } }
        fputc('\n', f);
    }
    fclose(f);
}

/* FRAME_LOG=<file>: one line per present, to see how evenly frames reach the
 * screen (the title-bar FPS can read 30 while frames are held for uneven
 * numbers of refreshes). Times in ms:
 *   t      Present call, from the first logged one
 *   dt     since the previous present
 *   rf     render_frame entry -> Present (recording the batch's GPU work)
 *   pres   inside Present
 *   flat   game's flip word decoded -> Present
 *   flips  flip words since the previous present, draws in the batch
 * then DXGI's statistics for the last frame that reached the screen:
 *   pc / prc / src  PresentCount, PresentRefreshCount, SyncRefreshCount
 * (consecutive pc and prc give how many refreshes each frame was shown). */
/* Console `trace` marks (boot_main sets this while a trace runs). */
extern void (*volatile g_rsx_trace_mark)(const char* name, u32 a, u32 b);
#define RSX_MARK(n, a, b) do { void (*_m)(const char*, u32, u32) = g_rsx_trace_mark; \
                               if (_m) _m((n), (a), (b)); } while (0)
static long long s_rf_entry_qpc = 0;
/* RF_SLOW_MS=<ms> (live): print where a render_frame longer than that spent
 * its time, phase by phase (checkpoints RF_CP below). */
#define RF_CPS 10
static long long s_rf_cp[RF_CPS];
/* With PERF=1, the draw pre-pass split: texture uploads, SRVs, pipeline
 * lookups and creation, shader compiles (ms) and counts. */
static double s_rf_pp[6], s_rf_pp0[6]; static int s_rf_ppn[3], s_rf_ppn0[3];
static void rf_pp_snap(double* d, int* n)
{
    d[0] = s_perf_tex; d[1] = s_perf_srv; d[2] = s_perf_pso; d[3] = s_perf_cpso;
    d[4] = s_perf_fxc; d[5] = s_perf_pmiss;
    n[0] = s_perf_ntex; n[1] = s_perf_cpso_n; n[2] = s_perf_fxc_n;
}
#define RF_CP(i) do { LARGE_INTEGER _c; QueryPerformanceCounter(&_c); s_rf_cp[i] = _c.QuadPart; } while (0)
static void rf_slow_report(void)
{
    static int lim = -1;
    PS3_KNOB(lim, ps3_env_int("RF_SLOW_MS", 0));
    if (lim <= 0 || !s_rf_cp[0]) return;
    static long long fq = 0;
    if (!fq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); fq = f.QuadPart; }
    const double ms = 1000.0 / (double)fq;
    long long last = s_rf_cp[0];
    for (int i = 1; i < RF_CPS; i++) if (s_rf_cp[i] > last) last = s_rf_cp[i];
    if ((last - s_rf_cp[0]) * ms < (double)lim) return;
    static const char* nm[RF_CPS] = { "", "gpu-wait+store", "setup", "rt-prepass",
        "draw-prepass", "legacy-pass", "vp-pass", "post", "submit+zq", "present" };
    fprintf(stderr, "[RF-SLOW] %.1f ms, %u draws:", (last - s_rf_cp[0]) * ms, s_dbg_last_draws);
    long long prev = s_rf_cp[0];
    for (int i = 1; i < RF_CPS; i++) {
        if (!s_rf_cp[i]) continue;
        fprintf(stderr, " %s %.1f", nm[i], (s_rf_cp[i] - prev) * ms);
        prev = s_rf_cp[i];
    }
    if (perf_on())
        fprintf(stderr, " | pre-pass: tex %.1f (%d) srv %.1f pso %.1f create-pso %.1f (%d) fxc %.1f (%d) miss %.1f",
                (s_rf_pp[0] - s_rf_pp0[0]) * 1000, s_rf_ppn[0] - s_rf_ppn0[0],
                (s_rf_pp[1] - s_rf_pp0[1]) * 1000, (s_rf_pp[2] - s_rf_pp0[2]) * 1000,
                (s_rf_pp[3] - s_rf_pp0[3]) * 1000, s_rf_ppn[1] - s_rf_ppn0[1],
                (s_rf_pp[4] - s_rf_pp0[4]) * 1000, s_rf_ppn[2] - s_rf_ppn0[2],
                (s_rf_pp[5] - s_rf_pp0[5]) * 1000);
    fprintf(stderr, "%c", 10);
}

/* Profile sessions (F11, or the console's `profile`): boot_main's profile
 * thread calls rc_prof_begin/rc_prof_end; in between, every present is
 * recorded here, and the end writes <prefix>_frames.txt (one line per
 * present) and <prefix>_summary.txt (frame-time statistics and the worst
 * frames with where their time went). */
typedef struct ProfRec {
    double t, dt, rf, flat, wait, store;
    float ph[RF_CPS];                         /* render_frame phases, ms */
    float tex, srv, pso, cpso, fxc, miss;     /* draw pre-pass split, ms */
    int ntex, ncpso, nfxc;
    u32 draws;
} ProfRec;
#define PROF_MAX (1 << 17)                    /* ~70 min at 30 presents/s */
static ProfRec* s_prof; static volatile long s_prof_n;
static long long s_prof_t0, s_prof_prev;
static char s_prof_prefix[MAX_PATH];

static void prof_record(long long q0)
{
    if (!s_prof_on || !s_prof) return;
    static long long fq = 0;
    if (!fq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); fq = f.QuadPart; }
    const double ms = 1000.0 / (double)fq;
    const long i = s_prof_n;
    if (i >= PROF_MAX) return;
    ProfRec* r = &s_prof[i];
    extern volatile long long g_gcm_flip_word_qpc;
    if (!s_prof_t0) s_prof_t0 = q0;
    r->t = (q0 - s_prof_t0) * ms;
    r->dt = s_prof_prev ? (q0 - s_prof_prev) * ms : 0.0;
    r->rf = s_rf_cp[0] ? (q0 - s_rf_cp[0]) * ms : 0.0;
    r->flat = g_gcm_flip_word_qpc ? (q0 - g_gcm_flip_word_qpc) * ms : -1.0;
    r->wait = s_fl_wait_ms; r->store = s_fl_store_ms;
    long long prev = s_rf_cp[0];
    for (int k = 0; k < RF_CPS; k++) {
        r->ph[k] = (k && s_rf_cp[k]) ? (float)((s_rf_cp[k] - prev) * ms) : 0.f;
        if (k && s_rf_cp[k]) prev = s_rf_cp[k];
    }
    r->tex = (float)((s_rf_pp[0] - s_rf_pp0[0]) * 1000); r->srv = (float)((s_rf_pp[1] - s_rf_pp0[1]) * 1000);
    r->pso = (float)((s_rf_pp[2] - s_rf_pp0[2]) * 1000); r->cpso = (float)((s_rf_pp[3] - s_rf_pp0[3]) * 1000);
    r->fxc = (float)((s_rf_pp[4] - s_rf_pp0[4]) * 1000); r->miss = (float)((s_rf_pp[5] - s_rf_pp0[5]) * 1000);
    r->ntex = s_rf_ppn[0] - s_rf_ppn0[0]; r->ncpso = s_rf_ppn[1] - s_rf_ppn0[1]; r->nfxc = s_rf_ppn[2] - s_rf_ppn0[2];
    r->draws = s_dbg_last_draws;
    s_prof_prev = q0;
    s_prof_n = i + 1;
}

static void rc_prof_begin(const char* prefix)
{
    if (!s_prof) s_prof = (ProfRec*)malloc(sizeof(ProfRec) * PROF_MAX);
    if (!s_prof) return;
    snprintf(s_prof_prefix, sizeof s_prof_prefix, "%s", prefix);
    s_prof_n = 0; s_prof_t0 = 0; s_prof_prev = 0;
    InterlockedExchange(&s_prof_on, 1);
}

static int prof_cmp_dbl(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* Returns the number of presents recorded; fills a one-line digest. */
static int rc_prof_end(char* digest, size_t cap)
{
    InterlockedExchange(&s_prof_on, 0);
    Sleep(150);                       /* the frame being recorded finishes */
    const long n = s_prof_n;
    if (digest && cap) digest[0] = 0;
    if (!s_prof || n < 3) return (int)n;
    static const char* ph[RF_CPS] = { "", "wait+store", "setup", "rt-prepass",
        "draw-prepass", "legacy", "vp-pass", "post", "submit", "present" };
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s_frames.txt", s_prof_prefix);
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "# t_ms dt rf flip->present wait store draws |");
        for (int k = 1; k < RF_CPS; k++) fprintf(f, " %s", ph[k]);
        fprintf(f, " | tex ntex srv pso create-pso ncreate fxc nfxc miss%c", 10);
        for (long i = 0; i < n; i++) {
            const ProfRec* r = &s_prof[i];
            fprintf(f, "%.1f %.2f %.2f %.2f %.2f %.2f %u |", r->t, r->dt, r->rf, r->flat,
                    r->wait, r->store, r->draws);
            for (int k = 1; k < RF_CPS; k++) fprintf(f, " %.2f", r->ph[k]);
            fprintf(f, " | %.2f %d %.2f %.2f %.2f %d %.2f %d %.2f%c", r->tex, r->ntex, r->srv,
                    r->pso, r->cpso, r->ncpso, r->fxc, r->nfxc, r->miss, 10);
        }
        fclose(f);
    }
    /* statistics over intervals (skip the first: no previous present) */
    double* dt = (double*)malloc(sizeof(double) * n);
    double* rf = (double*)malloc(sizeof(double) * n);
    if (!dt || !rf) { free(dt); free(rf); return (int)n; }
    long m = 0, skips = 0, over36 = 0, over50 = 0, over100 = 0;
    double sum = 0, phsum[RF_CPS] = {0};
    for (long i = 1; i < n; i++) {
        const ProfRec* r = &s_prof[i];
        dt[m] = r->dt; rf[m] = r->rf; sum += r->dt; m++;
        if (r->draws < 50) skips++;
        if (r->dt > 36.3) over36++;
        if (r->dt > 50) over50++;
        if (r->dt > 100) over100++;
        for (int k = 1; k < RF_CPS; k++) phsum[k] += r->ph[k];
    }
    qsort(dt, (size_t)m, sizeof *dt, prof_cmp_dbl);
    qsort(rf, (size_t)m, sizeof *rf, prof_cmp_dbl);
    const double secs = s_prof[n - 1].t / 1000.0;
    const double fps = sum > 0 ? 1000.0 * m / sum : 0;
    /* 1% low: the frame rate of the slowest 1% of presents (mean interval) */
    long k1 = m / 100; if (k1 < 1) k1 = 1;
    double low = 0; for (long i = m - k1; i < m; i++) low += dt[i]; low /= k1;
    snprintf(path, sizeof path, "%s_summary.txt", s_prof_prefix);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%.1f s, %ld presents: %.1f fps average, 1%% low %.1f fps%c", secs, n, fps,
                low > 0 ? 1000.0 / low : 0, 10);
        fprintf(f, "present interval ms: median %.1f  p90 %.1f  p99 %.1f  max %.1f%c",
                dt[m / 2], dt[m * 9 / 10], dt[m * 99 / 100], dt[m - 1], 10);
        fprintf(f, "  over 36.3 ms (the game's limit for 30 fps): %ld   over 50: %ld   over 100: %ld%c",
                over36, over50, over100, 10);
        fprintf(f, "skip frames (under 50 draws, the game's 15 fps mode): %ld (%.1f%%)%c",
                skips, m ? 100.0 * skips / m : 0, 10);
        fprintf(f, "render_frame (frame start -> Present) ms: median %.1f  p90 %.1f  max %.1f%c",
                rf[m / 2], rf[m * 9 / 10], rf[m - 1], 10);
        fprintf(f, "  mean per phase:");
        for (int k = 1; k < RF_CPS; k++) fprintf(f, " %s %.2f", ph[k], m ? phsum[k] / m : 0);
        fprintf(f, "%c%c", 10, 10);
        fprintf(f, "An interval much longer than its render_frame means the game itself was%c"
                   "late with the frame (CPU, SPU or waits: see _threads.txt); a long%c"
                   "render_frame is this renderer.%c%c", 10, 10, 10, 10);
        fprintf(f, "worst presents (t = seconds into the session):%c", 10);
        char* used = (char*)calloc((size_t)n, 1);
        for (int w = 0; w < 15 && used; w++) {
            long bi = -1;
            for (long i = 1; i < n; i++)
                if (!used[i] && (bi < 0 || s_prof[i].dt > s_prof[bi].dt)) bi = i;
            if (bi < 0) break;
            used[bi] = 1;
            const ProfRec* r = &s_prof[bi];
            fprintf(f, "  t=%7.2f  interval %6.1f  render_frame %6.1f  draws %5u  flip->present %5.1f |",
                    r->t / 1000.0, r->dt, r->rf, r->draws, r->flat);
            for (int k = 1; k < RF_CPS; k++) if (r->ph[k] >= 1.0f) fprintf(f, " %s %.1f", ph[k], r->ph[k]);
            if (r->tex >= 1.0f) fprintf(f, " | tex uploads %.1f (%d)", r->tex, r->ntex);
            if (r->ncpso) fprintf(f, " | %d pipelines created %.1f", r->ncpso, r->cpso);
            if (r->nfxc) fprintf(f, " | %d shaders compiled %.1f", r->nfxc, r->fxc);
            fprintf(f, "%c", 10);
        }
        free(used);
        fclose(f);
    }
    if (digest && cap)
        snprintf(digest, cap, "%.1f s, %.1f fps, 1%% low %.1f, %ld over 36.3 ms, %ld over 100 ms, %.1f%% skip frames",
                 secs, fps, low > 0 ? 1000.0 / low : 0, over36, over100, m ? 100.0 * skips / m : 0);
    free(dt); free(rf);
    return (int)n;
}

/* Window caption note while a session records ("" clears it). */
static void rc_prof_title(const char* note)
{
    static char base[256];
    if (!s_d3d.hwnd) return;
#ifdef _WIN32
    if (!base[0]) GetWindowTextA(s_d3d.hwnd, base, sizeof base);
#else
    if (!base[0]) {
        extern char g_rsx_title_base[128];
        snprintf(base, sizeof base, "%s", g_rsx_title_base);
    }
#endif
    char t[320];
    snprintf(t, sizeof t, "%s%s", base, note);
#ifdef _WIN32
    SetWindowTextA(s_d3d.hwnd, t);
#else
    win_set_title(t);
#endif
}
static void frame_log(long long q0, long long q1)
{
    static FILE* f = NULL; static int st = -1;
    static long long first = 0, prev = 0, freq = 0; static u32 fw_prev = 0;
    static char path[MAX_PATH];
    if (st < 0) {
        const char* e = getenv("FRAME_LOG");
        st = (e && *e) ? 1 : 0;
        if (st) {
            snprintf(path, sizeof path, "%s", e);
            f = fopen(path, "w");
            if (!f) st = 0;
            else fprintf(f, "t dt rf pres flat flips draws pc prc src wait store\n");
            LARGE_INTEGER fr; QueryPerformanceFrequency(&fr); freq = fr.QuadPart;
        }
    }
    if (!st) return;
    extern volatile u32 g_gcm_flip_words;
    extern volatile long long g_gcm_flip_word_qpc;
    if (!first) first = q0;
    const double ms = 1000.0 / (double)freq;
    DXGI_FRAME_STATISTICS fs; memset(&fs, 0, sizeof fs);
    if (FAILED(s_d3d.swap_chain->lpVtbl->GetFrameStatistics(s_d3d.swap_chain, &fs)))
        memset(&fs, 0, sizeof fs);
    const u32 fw = g_gcm_flip_words;
    const long long fq = g_gcm_flip_word_qpc;
    fprintf(f, "%.2f %.2f %.2f %.2f %.2f %u %u %u %u %u %.2f %.2f\n",
            (q0 - first) * ms, prev ? (q0 - prev) * ms : 0.0,
            s_rf_entry_qpc ? (q0 - s_rf_entry_qpc) * ms : 0.0, (q1 - q0) * ms,
            fq ? (q0 - fq) * ms : -1.0, fw - fw_prev, s_dbg_last_draws,
            fs.PresentCount, fs.PresentRefreshCount, fs.SyncRefreshCount,
            s_fl_wait_ms, s_fl_store_ms);
    prev = q0; fw_prev = fw;
    { static int n = 0; if (++n % 64 == 0) fflush(f); }
}

/* RSX_SCALE changed (the port's Settings window sets it while the game runs):
 * at the start of a batch, with the GPU idle and the last write-back stored,
 * everything sized from it is dropped and made again at the new size. Colour
 * targets refill from guest memory, which the write-back keeps current at any
 * scale; depth surfaces start cleared, as a new one does. */
static void rsx_scale_apply(void)
{
    const u32 want = rsx_scale_wanted();
    if (want == rsx_scale_pct()) return;
    s_scale_pct = want;
    printf("[D3D12] RSX_SCALE: render targets at %u%%%c", want, 10);
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        off_rt_defer_release(r->res);  r->res = NULL;
        off_rt_defer_release(r->up);   r->up = NULL;
        off_rt_defer_release(r->snap); r->snap = NULL;
        off_rt_defer_release(r->down); r->down = NULL;
        r->gsum_ok = 0; r->used = 0;
    }
    for (int i = 0; i < MAX_ZETA; i++) {
        off_rt_defer_release(s_zeta[i].res); s_zeta[i].res = NULL; s_zeta[i].used = 0;
    }
    for (int i = 0; i < SUBVP_SLOTS; i++) { off_rt_defer_release(s_subvp[i].res); s_subvp[i].res = NULL; }
    off_rt_defer_release(s_screen_copy);      s_screen_copy = NULL;
    off_rt_defer_release(s_d3d.readback_buf); s_d3d.readback_buf = NULL;
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        off_rt_defer_release(s_d3d.render_targets[i]); s_d3d.render_targets[i] = NULL;
    }
    off_rt_defer_release(s_d3d.depth_buffer); s_d3d.depth_buffer = NULL;
    s_d3d.host_w = rt_host_dim(s_d3d.width, s_d3d.height, s_d3d.width);
    s_d3d.host_h = rt_host_dim(s_d3d.width, s_d3d.height, s_d3d.height);
    if (display_targets_make() != 0 || depth_buffer_make() != 0) {
        fprintf(stderr, "[D3D12] RSX_SCALE: the targets at %u%% could not be made%c", want, 10);
        abort();
    }
    blit_srvs_write();
}

