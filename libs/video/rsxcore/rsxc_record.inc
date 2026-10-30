/* ---------------------------------------------------------------------------
 * RSX backend callbacks
 * -----------------------------------------------------------------------*/

static int d3d12_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    printf("[D3D12] Backend init(%ux%u)\n", width, height);
    return 0;
}

static void d3d12_shutdown(void* ud)
{
    (void)ud;
    printf("[D3D12] Backend shutdown\n");
}

static void d3d12_begin_frame(void* ud)
{
    (void)ud;
}

static void d3d12_end_frame(void* ud)
{
    (void)ud;
}

static u32 s_dbg_clears_since_present = 0;   /* CELLMARK_BLINKDBG */
static u32 s_clear_presents = 0;   /* presents issued at clear (frame boundary) */

/* FRAME_BUDGET=1: how much geometry a guest frame actually asks for, versus
 * what the per-frame vertex buffer can hold. A frame that overflows is silently
 * truncated -- the tail of the scene never renders -- which reads as "the object
 * is missing" rather than "the batch is too small". */

static int blink_dbg(void)
{
    static int v = -1;
    PS3_KNOB(v, getenv("CELLMARK_BLINKDBG") ? 1 : 0);
    return v;
}

/* A flip whose batch has no display draw keeps the batch until the composite
 * that ends it arrives (below). A batch that has run out of room never gets
 * it: the full draw table or vertex buffer drops the display copy as well,
 * render_frame (the only place that empties the batch and grows the buffer)
 * never runs again, and the picture stops for good while the game plays on
 * (the symptom of DemonsSoulsRecomp#205). Such a batch is drawn at the flip
 * without a present, as the ticker draws offscreen-only work, so the next
 * frame starts empty. */
static u32 s_nodisp_run;   /* flips in a row with draws but no display draw */
static void flip_without_display(void)
{
    if (!s_d3d.initialized || !s_d3d.draw_count) return;
    { static u32 said = 0;
      if (++s_nodisp_run == 300 && said < 10 && ++said)
          fprintf(stderr, "[D3D12] 300 flips in a row with no display draw (%u draws, %llu of %u vertices)%c",
                  s_d3d.draw_count, (unsigned long long)s_vp_want, s_vp_cap, 10); }
    if (s_d3d.draw_count < MAX_DRAWS && s_vp_want <= s_vp_cap
        && s_d3d.vb_offset + 6 * VERTEX_STRIDE <= MAX_VERTICES * VERTEX_STRIDE) return;
    { static int n = 0;
      if (n < 20 && ++n)
          fprintf(stderr, "[D3D12] a batch with no display draw is full (%u draws, %llu of %u vertices):"
                  " drawn without a present%c", s_d3d.draw_count, (unsigned long long)s_vp_want, s_vp_cap, 10); }
    s_present_this_frame = 0;
    render_frame();
    s_present_this_frame = 1;
}

static void d3d12_present(void* ud, u32 buffer_id)
{
    (void)ud;
    (void)buffer_id;

    if (blink_dbg())
        printf("[PRESENT] draws=%u clears_since_last=%u\n",
               s_d3d.draw_count, s_dbg_clears_since_present);
    s_dbg_clears_since_present = 0;

    /* An accumulated batch with draws but NONE targeting a display buffer is
     * offscreen pass work only (demosaic flips once per effect pass): showing
     * it would strobe the bare backbuffer clear. Keep accumulating -- the
     * composite draw that targets the display presents the whole chain, in
     * order, in one command list. Empty batches still present (boot/idle). */
    int flip_has_display = (s_d3d.draw_count == 0);
    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++)
        if (!s_d3d.draws[_i].is_clear && s_d3d.draws[_i].rt_off == 0) {
            flip_has_display = 1;
            break;
        }

    if (s_d3d.initialized && flip_has_display) {
        s_nodisp_run = 0;
        render_frame();
    }
    else if (!flip_has_display) flip_without_display();
    if (flip_has_display) s_pc_flip_render++; else s_pc_flip_nodisp++;

    /* FPS tracking */
    ULONGLONG now = GetTickCount64();
    if (now - s_d3d.last_fps_time >= 1000) {
        s_d3d.fps = (u32)s_d3d.frame_count; /* rough estimate */
        s_d3d.last_fps_time = now;
        s_d3d.frame_count = 0;
    }
}

static void d3d12_clear_display(const float cc[4]);

/* Guest depth clear value in 0..1 for the current zeta format: Z24S8 keeps
 * depth in the top 24 bits, Z16 in the low 16 (rsx_commands assumes Z24S8). */
static float clear_depth_value(const rsx_state* st, float z24)
{
    if (st && ((st->surface_format >> 5) & 7u) == 1u)
        return (float)(st->zstencil_clear_value & 0xFFFFu) / 65535.0f;
    return z24;
}

static void d3d12_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;

    /* Convert RSX ARGB u32 to float[4] RGBA */
    float cc[4];
    cc[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    cc[1] = ((color >> 8) & 0xFF) / 255.0f;  /* G */
    cc[2] = (color & 0xFF) / 255.0f;          /* B */
    cc[3] = ((color >> 24) & 0xFF) / 255.0f;  /* A */
    /* CLEAR_RGB=r,g,b: override the guest clear colour. Black holes (geometry
     * that never rasterized) and black pixels a shader really wrote look
     * identical against a black clear; this separates them in one run. */
    { static int _cinit = 0; static float _co[3]; static int _con = 0;
      if (!_cinit) { _cinit = 1; const char* e = getenv("CLEAR_RGB");
          if (e && sscanf(e, "%f,%f,%f", &_co[0], &_co[1], &_co[2]) == 3) _con = 1; }
      if (_con) { cc[0] = _co[0]; cc[1] = _co[1]; cc[2] = _co[2]; } }

    u32 rt_w = 0, rt_h = 0, mrt[3] = {0, 0, 0};
    u32 rt = current_rt_off(&rt_w, &rt_h, mrt);

    /* An OFFSCREEN clear is just an ordered op in the current frame's pass
     * chain (demosaic clears each effect pass's surface) -- record it, don't
     * touch the frame boundary. */
    if (rt != 0) {
        if (draw_room()) {
            D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
            memset(dr, 0, sizeof(*dr));
            dr->is_vp = 1; dr->is_clear = 1; dr->tex_slot = -1;
            dr->rt_off = rt; memcpy(dr->rt_mrt, mrt, sizeof(mrt));
            dr->rt_w = rt_w; dr->rt_h = rt_h;
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            rt_pitch_record(dr);
            memcpy(dr->cc, cc, sizeof(cc));
            dr->clr_flags = flags;
            dr->clr_z = clear_depth_value(s_d3d.current_rsx_state, depth);
            dr->clr_s = stencil;
        }
        return;
    }

    d3d12_clear_display(cc);
    /* The display path above may close the frame; a Z/S clear belongs to
     * whatever frame follows it, so it is recorded after. */
    if ((flags & 3u) && draw_room()) {
        D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
        memset(dr, 0, sizeof(*dr));
        dr->is_vp = 1; dr->is_clear = 1; dr->tex_slot = -1;
        rt_pitch_record(dr);
        dr->clr_flags = flags & 3u;
        dr->clr_z = clear_depth_value(s_d3d.current_rsx_state, depth);
        dr->clr_s = stencil;
    }
}

/* Frames end at the title's flips once the command stream carries them
 * (cellGcmSys calls d3d12_fifo_flip when the walker reaches one), which is
 * where the RSX ends a frame. Until then -- and for a title that flips
 * outside the stream, or with RSX_PRESENT_AT_FLIP=0 -- display clears are
 * the boundary, with the heuristics in d3d12_clear_display. Those depended
 * on when the PPU's immediate flip landed relative to the walker: whenever
 * Demon's Souls hitched, a frame went out without its post chain and HUD. */
static int s_fifo_flip_present;
static u32 s_fifo_flip_req;   /* cellGcm_flip_request_count() at the last flip in the stream */
static int s_flips_outside;   /* display clears present: the flips are outside the stream */

static int present_at_flip_on(void)
{
    static int v = -1;
    PS3_KNOB(v, ps3_env_int("RSX_PRESENT_AT_FLIP", 1));
    return v;
}

static void d3d12_fifo_flip(void)
{
    if (!present_at_flip_on() || !s_d3d.initialized) { s_fifo_flip_present = 0; return; }
    s_fifo_flip_present = 1;
    { extern unsigned cellGcm_flip_request_count(void); s_fifo_flip_req = cellGcm_flip_request_count(); }
    if (s_flips_outside) {
        s_flips_outside = 0;
        static int n = 0;
        if (n < 10 && ++n) fprintf(stderr, "[D3D12] flips are in the command stream again%c", 10);
    }
    /* Same display gate as the other presents: offscreen pass work alone
     * keeps accumulating until its composite arrives, unless the batch is
     * full (flip_without_display). */
    int has_display = 0;
    for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++)
        if (!s_d3d.draws[i].is_clear && s_d3d.draws[i].rt_off == 0) { has_display = 1; break; }
    if (!has_display) { s_pc_flip_nodisp++; flip_without_display(); return; }
    if (blink_dbg())
        printf("[FLIP] presenting %u draws at the title's flip%c", s_d3d.draw_count, 10);
    s_nodisp_run = 0;
    render_frame();
    s_pc_flip_render++;
    s_clear_presents++;   /* framed presents are on: the ticker stays out */
    { extern void cellGcm_rcap_cut(void); cellGcm_rcap_cut(); }
    s_dbg_clears_since_present = 0;
    s_d3d.draw_count   = 0;
    s_d3d.vb_offset    = 0;
    s_d3d.vp_vb_offset = 0;
}

static void d3d12_clear_display(const float cc[4])
{
    memcpy(s_d3d.clear_color, cc, 4 * sizeof(float));
    /* The flip ends the frame, while the title's flips are in the command
     * stream. A flip made outside it (an immediate cellGcmSetFlipCommand, or
     * one with no room in the buffer) never reaches d3d12_fifo_flip, and with
     * only those nothing would present again: the picture would stop while
     * the game plays on. From three such flips in a row, display clears end
     * the frame again, until a flip in the stream arrives. */
    if (s_fifo_flip_present && present_at_flip_on()) {
        extern unsigned cellGcm_flip_request_count(void);
        if (cellGcm_flip_request_count() - s_fifo_flip_req < 3) return;
        if (!s_flips_outside) {
            s_flips_outside = 1;
            static int n = 0;
            if (n < 10 && ++n)
                fprintf(stderr, "[D3D12] %u flips outside the command stream since the last one in it:"
                        " presenting at display clears%c", cellGcm_flip_request_count() - s_fifo_flip_req, 10);
        }
    }

    /* A DISPLAY clear marks the start of a new visible frame. If a completed
     * frame is still accumulated (the drain gulped across a frame boundary --
     * guaranteed at the FIFO ring wrap, where rest-of-frame-N + clear-N+1
     * arrive in one batch), PRESENT it now instead of discarding it. Only a
     * batch that actually contains DISPLAY draws is a completed frame: a
     * batch of offscreen pass work (render-to-texture) must keep accumulating
     * until its composite draw arrives, or the screen strobes intermediates. */
    int have_display_draws = 0;
    for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++)
        if (!s_d3d.draws[i].is_clear && s_d3d.draws[i].rt_off == 0) {
            have_display_draws = 1;
            break;
        }
    if (!have_display_draws) {
        s_pc_clear_nodisp++;
        return;   /* keep accumulating the in-progress frame */
    }

    /* A title that DOUBLE-BUFFERS the display (DeferredShading clears both
     * 0x0 and 0x440000 per frame, plus a HUD pass) issues several display
     * clears per real frame -- treating each as a boundary presented the
     * 1-2-draw intermediates, strobing black between the full 146-draw
     * frames. Anchor the boundary to the FLIP instead: only present once a
     * cellGcmSetFlip has landed since the last present. Titles that flip once
     * per frame (wave/cellmark/gcmcube) are unaffected -- their single
     * display clear still follows their single flip. Fall back to the old
     * clear-only heuristic for titles that never flip (fc stays 0). */
    static u32 s_frame_draws_max = 0;   /* running max frame size (typical full frame) */
    if (s_d3d.draw_count > s_frame_draws_max) s_frame_draws_max = s_d3d.draw_count;
    {
        extern unsigned cellGcm_flip_request_count(void);
        static unsigned s_last_present_flip = 0;
        unsigned fc = cellGcm_flip_request_count();
        if (fc != 0) {
            if (fc == s_last_present_flip) {
                s_pc_clear_sameflip++;
                return;   /* no flip since last present -> not a real boundary */
            }
            /* A double-buffered title issues several display clears per flip
             * (DeferredShading: clear back-buffer, HUD pass, ...). They can
             * arrive in either order, so a small batch here may be an
             * intermediate that precedes the full frame's clear. Don't present
             * (or consume the flip) until the batch is a substantial fraction
             * of a full frame -- keep accumulating so the complete frame lands
             * in one present. Gauged against the running MAX frame size (not
             * the last, which a leaked tiny batch would poison), so it
             * self-scales per title with no fixed threshold. */
            /* A max that is stale: no present for RSX_STALE_FLIPS flips
             * (default 4; 0 = never) means the scene got lighter, not that
             * these are intermediates -- a full frame arrives at least every
             * other flip where the rule above is needed (the Demon's Souls
             * level). Start over from this batch. Without it the whole-run
             * max made every frame of a light scene "small": movie frames of
             * 5-10 draws after 1,700-draw loading or level frames were merged
             * two to a present (15 fps) or hardly presented at all (1 fps). */
            if (s_frame_draws_max > 16 && s_d3d.draw_count < s_frame_draws_max / 4) {
                static int stale = -1;
                PS3_KNOB(stale, ps3_env_int("RSX_STALE_FLIPS", 4));
                if (stale <= 0 || fc - s_last_present_flip < (unsigned)stale) {
                    s_pc_clear_small++;
                    return;   /* intermediate: keep accumulating, flip stays pending */
                }
                s_frame_draws_max = s_d3d.draw_count;
            }
            /* The other face of a stale max: every present merges two or more
             * flips and is still light (under half the max) -- a 5-draw movie
             * frame is "small" against a max of 30, two of them are not, so
             * the movie ran at 15 fps. Three such presents in a row halve the
             * max. A full level frame is never under half, so the level is
             * untouched. */
            { static int light_merges;
              if (fc - s_last_present_flip >= 2 && s_d3d.draw_count * 2 < s_frame_draws_max) {
                  if (++light_merges >= 3) { s_frame_draws_max /= 2; light_merges = 0; }
              } else light_merges = 0; }
            s_last_present_flip = fc;
        }
    }

    /* RSX_ACCUM_FRAME=1: never present at a clear boundary, so every draw in a
     * guest frame accumulates into one image. A title that clears several times
     * per frame (render-to-texture passes) otherwise gets presented mid-scene,
     * and each captured frame holds only a slice of the geometry -- the floor in
     * one, a wall in the next -- which makes a single model impossible to see. */
    { static int accum = -1;
      PS3_KNOB(accum, ps3_env_int("RSX_ACCUM_FRAME", 0));
      if (accum) return; }
    if (s_d3d.initialized) {
        if (blink_dbg())
            printf("[CLEAR] presenting %u accumulated draws at frame boundary\n",
                   s_d3d.draw_count);
        render_frame();
        s_clear_presents++;
        { extern void cellGcm_rcap_cut(void); cellGcm_rcap_cut(); }   /* frame capture boundary */
    }
    s_dbg_clears_since_present++;
    s_d3d.draw_count   = 0;
    s_d3d.vb_offset    = 0;
    s_d3d.vp_vb_offset = 0;
}

static void d3d12_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;
    /* Log only the first few; set_render_target is called every frame and
     * floods the log otherwise. */
    static int s_count = 0;
    if (s_count < 5) {
        printf("[D3D12] set_render_target(%ux%u)\n",
               state->surface_clip_w, state->surface_clip_h);
        s_count++;
    }
}

static void d3d12_set_viewport(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: update D3D12 viewport from RSX state */
    (void)state;
}

