/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_d3d12_backend = {0};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

static int rc_backend_init(u32 width, u32 height, const char* title)
{
    { extern void (*g_gcm_fifo_flip_hook)(void); g_gcm_fifo_flip_hook = d3d12_fifo_flip; }
    if (ps3_env_int("RSX_ZPASS", 1)) g_rsx_report_hook = zq_hook;
    { extern void (*g_rcap_mark_hook)(int end); g_rcap_mark_hook = cap_mark; }
    memset(&s_d3d, 0, sizeof(s_d3d));
    memset(s_vp_fp_head, 0, sizeof s_vp_fp_head);   /* the PSO cache is empty again */
    s_d3d.width = width;
    s_d3d.height = height;
    s_d3d.host_w = rt_host_dim(width, height, width);
    s_d3d.host_h = rt_host_dim(width, height, height);
    s_d3d.clear_color[0] = 0.0f;
    s_d3d.clear_color[1] = 0.0f;
    s_d3d.clear_color[2] = 0.1f;
    s_d3d.clear_color[3] = 1.0f;

    /* Debug: dump the first N presented frames to BMP if CELLMARK_DUMP is set
     * (its numeric value when > 1, else 24). */
    {
        const char* dv = getenv("CELLMARK_DUMP");
        int n = dv ? atoi(dv) : 0;
        s_d3d.dump_frames_left = dv ? (n > 1 ? n : 24) : 0;
        /* CELLMARK_DUMP_SKIP=N: ignore the first N presents before dumping.
         * The opening frames are the loading screen; the interesting content
         * only appears once the sim SPUs have advanced the scene. */
        { const char* sv = getenv("CELLMARK_DUMP_SKIP");
          s_d3d.dump_skip_left = sv ? atoi(sv) : 0; }
    }

    /* Create window */
    {
        extern char g_rsx_title_base[128];
        snprintf(g_rsx_title_base, sizeof(g_rsx_title_base), "%s",
                 title ? title : "ps3recomp");
    }
    s_d3d.hwnd = open_window(width, height, title);
    if (!s_d3d.hwnd) {
        printf("[D3D12] ERROR: Window creation failed\n");
        return -1;
    }
    session_lock_init(s_d3d.hwnd);

    /* Initialize D3D12 */
    if (init_d3d12(width, height) != 0) {
        printf("[D3D12] ERROR: D3D12 initialization failed\n");
        return -1;
    }

    /* Set up backend callbacks */
    s_d3d12_backend.userdata          = &s_d3d;
    s_d3d12_backend.init              = d3d12_init;
    s_d3d12_backend.shutdown          = d3d12_shutdown;
    s_d3d12_backend.begin_frame       = d3d12_begin_frame;
    s_d3d12_backend.end_frame         = d3d12_end_frame;
    s_d3d12_backend.present           = d3d12_present;
    s_d3d12_backend.clear             = d3d12_clear;
    s_d3d12_backend.set_render_target = d3d12_set_render_target;
    s_d3d12_backend.set_viewport      = d3d12_set_viewport;
    s_d3d12_backend.set_blend         = d3d12_set_blend;
    s_d3d12_backend.set_depth_stencil = d3d12_set_depth_stencil;
    s_d3d12_backend.set_shader        = d3d12_set_shader;
    s_d3d12_backend.set_vertex_attribs = d3d12_set_vertex_attribs;
    s_d3d12_backend.draw_arrays       = d3d12_draw_arrays;
    s_d3d12_backend.draw_indexed      = d3d12_draw_indexed;
    s_d3d12_backend.bind_texture      = d3d12_bind_texture;

    rsx_set_backend(&s_d3d12_backend);

    s_d3d.initialized = 1;
    s_d3d.last_fps_time = GetTickCount64();

    printf("[D3D12] Backend ready: %ux%u\n", width, height);
    return 0;
}

static void rc_backend_shutdown(void)
{
    if (!s_d3d.initialized) return;

    wait_for_gpu();
    if (s_pso_lib) {
        InterlockedExchange(&s_pso_pre_abort, 1);
        while (s_pso_lib_saving || s_pso_pre_live || s_pxb_live) Sleep(1);
        pso_manifest_flush();
        if (s_pso_lib_new) pso_lib_save();
    }
    while (s_pxr_writing) Sleep(1);
    pso_index_flush();

    /* Release D3D12 resources */
    if (s_wbg.buf)  { s_wbg.buf->lpVtbl->Release(s_wbg.buf);   s_wbg.buf = NULL; }
    if (s_wbg.heap) { s_wbg.heap->lpVtbl->Release(s_wbg.heap); s_wbg.heap = NULL; }
    s_wbg.state = -1;
    if (s_d3d.vertex_buffer) {
        s_d3d.vertex_buffer->lpVtbl->Unmap(s_d3d.vertex_buffer, 0, NULL);
        s_d3d.vertex_buffer->lpVtbl->Release(s_d3d.vertex_buffer);
    }
    if (s_d3d.pipeline_state)        s_d3d.pipeline_state->lpVtbl->Release(s_d3d.pipeline_state);
    if (s_d3d.pipeline_state_lines)  s_d3d.pipeline_state_lines->lpVtbl->Release(s_d3d.pipeline_state_lines);
    if (s_d3d.pipeline_state_points) s_d3d.pipeline_state_points->lpVtbl->Release(s_d3d.pipeline_state_points);
    if (s_d3d.root_signature) s_d3d.root_signature->lpVtbl->Release(s_d3d.root_signature);
    if (s_of.pso) s_of.pso->lpVtbl->Release(s_of.pso);
    if (s_of.rs) s_of.rs->lpVtbl->Release(s_of.rs);
    memset(&s_of, 0, sizeof s_of);
    if (s_d3d.blit_pso) s_d3d.blit_pso->lpVtbl->Release(s_d3d.blit_pso);
    if (s_d3d.blit_rs) s_d3d.blit_rs->lpVtbl->Release(s_d3d.blit_rs);
    if (s_aa.pso) s_aa.pso->lpVtbl->Release(s_aa.pso);
    if (s_aa.pso16) s_aa.pso16->lpVtbl->Release(s_aa.pso16);
    if (s_aa.rs) s_aa.rs->lpVtbl->Release(s_aa.rs);
    if (s_aa.disp) s_aa.disp->lpVtbl->Release(s_aa.disp);
    memset(&s_aa, 0, sizeof s_aa);
    /* The swap chain and its buffers, the recorders, the fence, the queue and
     * the device are the GPU layer's: s_d3d's pointers to them were borrowed,
     * and gpu_destroy frees them once the GPU is idle. */
    s_d3d.device = NULL;
    s_d3d.cmd_queue = NULL;
    s_d3d.swap_chain = NULL;
    s_d3d.cmd_list = NULL;
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        s_d3d.swap_buffers[i] = NULL;
        s_rc_swap_image[i].id = 0;
    }
    s_rc_cmd = NULL;
    s_rc_swap.id = 0;
    /* Step C4: the shader-visible heaps are the table pools', borrowed too;
     * the pools, the texture slots, the staging buffers, the pool's heaps and
     * the adopted textures' handles go with the device. */
    s_d3d.srv_heap = s_d3d.samp_heap = s_d3d.blit_srv_heap = NULL;
    s_down_srv = NULL;
    s_rc_srv_pool.id = s_rc_samp_pool.id = s_rc_blit_pool.id = s_rc_down_pool.id = 0;
    /* Step C5a: the display targets, the depth buffer, zeta and the
     * offscreen targets with their views (no RTV or DSV heap is made) go
     * with the device too; the native pointers were borrowed. */
    s_d3d.depth_buffer = NULL;
    for (u32 i = 0; i < FRAME_COUNT; i++) s_d3d.render_targets[i] = NULL;
    if (s_rc_dev) gpu_destroy(s_rc_dev);
    s_rc_dev = NULL;

    /* A window belongs to the thread that made it: the window thread
     * destroys its own and then leaves its message loop. */
    InterlockedExchange(&s_overlay_on, 0);   /* (its device is gone; and this close must not ask first) */
#ifdef _WIN32
    if (s_d3d.hwnd) {
        if (s_window_thread) PostMessageA(s_d3d.hwnd, WM_CLOSE, 0, 0);
        else DestroyWindow(s_d3d.hwnd);
    }
#else
    if (s_d3d.hwnd) win_post(WIN_MAIL_CLOSE, 0);
#endif

    rsx_set_backend(NULL);
    s_d3d.initialized = 0;

    printf("[D3D12] Backend shut down\n");
}

static void rc_overlay_repaint(int on)
{
    InterlockedExchange(&s_overlay_repaint, on ? 1 : 0);
}

static void rc_overlay_wake(void)
{
    InterlockedExchange(&s_overlay_wake, 1);
}

/* DWM's timing: the composition's refresh period and the time of its last
 * vblank, in performance counter ticks; 0 when it cannot say. The window's
 * display mode: its refresh in whole Hz (143.9 Hz reads 143 or 144), 0 when
 * not known. Any thread. */
#ifdef _WIN32
static long long dwm_timing(long long freq, long long* vblank)
{
    typedef HRESULT (WINAPI *timing_fn)(HWND, DWM_TIMING_INFO*);
    static timing_fn volatile timing;
    static volatile LONG state;   /* 0 = not loaded yet, 1 = loaded, -1 = missing */
    if (!state) {
        HMODULE dwm = LoadLibraryExA("dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);   /* (Windows' own only) */
        timing = dwm ? (timing_fn)(void*)GetProcAddress(dwm, "DwmGetCompositionTimingInfo") : NULL;
        InterlockedExchange(&state, timing ? 1 : -1);
    }
    *vblank = 0;
    if (state < 0) return 0;
    DWM_TIMING_INFO ti;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti;
    if (FAILED(timing(NULL, &ti)) || ti.qpcRefreshPeriod < (QPC_TIME)(freq / 1000) ||
        ti.qpcRefreshPeriod > (QPC_TIME)(freq / 20))
        return 0;
    *vblank = (long long)ti.qpcVBlank;
    return (long long)ti.qpcRefreshPeriod;
}

static int display_mode_hz(void)
{
    if (!s_d3d.hwnd) return 0;
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (GetMonitorInfoW(MonitorFromWindow(s_d3d.hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO*)&mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 20 &&
        dm.dmDisplayFrequency < 1000)
        return (int)dm.dmDisplayFrequency;
    return 0;
}
#else
static int display_mode_hz(void)
{
    return (int)s_refresh_hz;   /* (window thread, win_refresh_note) */
}
#endif

/* The display's refresh: its period and the time of a vblank, in performance
 * counter ticks. DWM's timing, read again every quarter second (the window
 * can move to another display); without it the window's display mode gives
 * the period and *vblank is 0 (unknown). Render thread. */
static long long refresh_period(long long now, long long freq, long long* vblank)
{
    static long long period, vb, read_at;
    if (!period || now - read_at >= freq / 4) {
        read_at = now;
        vb = 0;
        long long p = 0;
#ifdef _WIN32
        p = dwm_timing(freq, &vb);
#endif
        if (!p) {
            const int hz = display_mode_hz();
            if (hz) p = freq / hz;
        }
        period = p ? p : freq / 60;
    }
    *vblank = vb;
    return period;
}

/* For rc_display_timing: the window's display mode in Hz, read again
 * every half second (the window can move to another display). */
static int display_hz_cached(long long now, long long freq)
{
    static volatile long long at;
    static volatile LONG hz;
    if (!at || now - at >= freq / 2) {
        InterlockedExchange(&hz, (LONG)display_mode_hz());
        InterlockedExchange64(&at, now);
    }
    return (int)hz;
}

static int period_near(long long p, long long nominal)
{
    const long long d = p > nominal ? p - nominal : nominal - p;
    return d * 1000 <= nominal * 15;   /* 1.5%: the mode's whole Hz against the real rate */
}

static long long rc_display_timing(long long* vblank)
{
    LARGE_INTEGER f, q;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    const int hz = display_hz_cached(q.QuadPart, f.QuadPart);
    const long long nominal = hz ? f.QuadPart / hz : 0;
    const long long sp = s_disp_period, svb = s_disp_vblank, sat = s_disp_at;
    if (sp && svb && q.QuadPart - sat < 2 * f.QuadPart && (!nominal || period_near(sp, nominal))) {
        *vblank = svb;
        return sp;
    }
#ifdef _WIN32
    long long dvb = 0;
    const long long dp = dwm_timing(f.QuadPart, &dvb);
    if (dp && (!nominal || period_near(dp, nominal))) {
        *vblank = dvb;
        return dp;
    }
#endif
    *vblank = 0;
    return nominal;
}

static int rc_display_refresh_mhz(void)
{
    long long vb;
    const long long p = rc_display_timing(&vb);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return p ? (int)((f.QuadPart * 1000 + p / 2) / p) : 0;
}

static int rc_display_mode_mhz(void)
{
    LARGE_INTEGER f, q;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return display_hz_cached(q.QuadPart, f.QuadPart) * 1000;
}

static int rc_display_shown_mhz(void)
{
    LARGE_INTEGER f, q;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    const long long sp = s_disp_period, sat = s_disp_at;
    if (!sp || q.QuadPart - sat >= 2 * f.QuadPart) return 0;
    return (int)((f.QuadPart * 1000 + sp / 2) / sp);
}

/* Is a frame presented and not yet shown? 1 / 0, or -1 when the swap
 * chain's statistics cannot say. */
static int present_queued(void)
{
    gpu_present_stats st;
    memset(&st, 0, sizeof st);
    if (gpu_swap_stats(s_rc_dev, s_rc_swap, &st) || !st.present_count)
        return -1;
    const UINT ahead = st.last_present_count - st.present_count;
    if (ahead > FRAME_COUNT + 1) return -1;   /* (statistics that do not follow this swap chain) */
    return ahead != 0;
}

static void rc_overlay_tick(void)
{
    /* Woken (rc_overlay_wake) with repaints off: the hold is made here
     * from the last frame shown, once the game has stopped presenting. */
    const int wake = s_overlay_wake && !s_overlay_repaint;
    if ((!s_overlay_repaint && !wake) || !s_overlay_on || !s_overlay.frame || !s_d3d.initialized ||
        !repaint_knob() || !knob_present() || s_session_locked)
        return;
    const int held = s_hold_valid && s_hold_w == s_d3d.host_w && s_hold_h == s_d3d.host_h;
    if (!held && !wake) return;   /* (held before RSX_SCALE moved) */
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    const long long now = q.QuadPart;
    if (wake) {
        if (now - s_last_present_qpc < f.QuadPart / 4) return;   /* (its next frame runs the hook) */
        if (s_shown_fi >= FRAME_COUNT && !held) return;          /* (nothing shown yet to draw over) */
        InterlockedExchange(&s_overlay_wake, 0);
    }
    long long vblank = 0;
    const long long period = refresh_period(now, f.QuadPart, &vblank);
    if (wake) {
        /* (at once: the game has not presented for a quarter of a second) */
    } else if (vblank && vblank <= now) {
        /* Late in a refresh that has no frame yet: a frame the game presents
         * earlier in it shows instead, and this one still makes the vblank. */
        const long long last_vblank = vblank + (now - vblank) / period * period;
        if (s_last_present_qpc >= last_vblank || now - last_vblank < period / 2) return;
    } else if (now - s_last_present_qpc < period) {
        return;
    }
    /* (presenting now would wait for it, and hold up the game's next frame;
     * a game that has stopped has none, and a window the compositor is not
     * showing never takes it) */
    if (now - s_game_frame_qpc < f.QuadPart / 4 && present_queued() == 1) return;

    /* The held frame into the game's buffer for this index (cleared by the
     * next batch anyway), then into the window's as for any frame. */
    const u32 fi = s_d3d.frame_index;
    frame_cmd_begin(fi);
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    if (!held) {
        /* The last frame shown is still in its render target: a frame index
         * moves only at a Present, so every batch since has drawn into the
         * other one. */
        hold_copy(s_shown_fi);
        if (!s_hold_valid) {
            gpu_cmd_end(s_rc_dev, s_rc_cmd);
            return;
        }
    }
    D3D12_RESOURCE_BARRIER b[2] = {0};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.Subresource = b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.pResource   = s_d3d.render_targets[fi];
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.pResource   = s_hold;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->lpVtbl->ResourceBarrier(cl, 2, b);
    cl->lpVtbl->CopyResource(cl, s_d3d.render_targets[fi], s_hold);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    cl->lpVtbl->ResourceBarrier(cl, 2, b);
    s_hdr.frame_t = -1;   /* a held frame: T may hold a later one */
    blit_to_window(fi);
    s_shown_fi = fi;
    frame_cmd_submit();
    s_overlay.frame(s_overlay.user, s_d3d.swap_buffers[fi], fi, s_d3d.win_w, s_d3d.win_h);
    swap_present(0);
    s_repaints++;
    move_to_next_frame();
    swap_resize();
}

static void rc_set_overlay(const rsx_d3d12_overlay* overlay)
{
    InterlockedExchange(&s_overlay_on, 0);
    if (overlay) {
        s_overlay = *overlay;
        InterlockedExchange(&s_overlay_on, 1);
    }
}
static void rc_set_borderless(int on)
{
#ifdef _WIN32
    if (s_d3d.hwnd) {
        if (s_window_thread) PostMessageA(s_d3d.hwnd, WM_RSX_BORDERLESS, (WPARAM)!!on, 0);
        else set_borderless(s_d3d.hwnd, on);
    }
#else
    if (s_d3d.hwnd) win_post(WIN_MAIL_BORDERLESS, !!on);
#endif
}
static int      rc_borderless(void)   { return s_borderless; }
/* The device, the queue and the swap chain's buffers for the menu renderer:
 * the GPU layer's own objects (gpu_native), borrowed as today's were. */
static void*    rc_device(void)       { return s_d3d.device; }
static long     rc_device_lost(long long* qpc)
{
    if (qpc) *qpc = s_device_lost_qpc;
    return s_device_lost;
}
static void*    rc_queue(void)        { return s_d3d.cmd_queue; }
static void*    rc_hwnd(void)         { return s_d3d.hwnd; }

static void rc_post_message(unsigned msg, unsigned long long wp, long long lp)
{
    if (!s_d3d.hwnd) return;
#ifdef _WIN32
    PostMessageA((HWND)s_d3d.hwnd, msg, (WPARAM)wp, (LPARAM)lp);
#else
    if (!s_sdl_mail || msg > 0xFFFFu) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = s_sdl_mail;
    e.user.code = WIN_MAIL_MESSAGE + (Sint32)msg;
    e.user.data1 = (void*)(uintptr_t)wp;
    e.user.data2 = (void*)(intptr_t)lp;
    SDL_PushEvent(&e);
#endif
}
static void*    rc_back_buffer(unsigned i) { return i < FRAME_COUNT ? s_d3d.swap_buffers[i] : NULL; }
static unsigned rc_back_buffer_count(void)  { return s_d3d.initialized ? FRAME_COUNT : 0; }
static unsigned rc_back_buffer_format(void)
{
    return (unsigned)(s_hdr.active ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);
}
static int rc_hdr(float* ui_scale, int* srgb)
{
    if (ui_scale) *ui_scale = s_hdr.active ? hdr_white_nits() / 80.0f : 1.0f;
    if (srgb) *srgb = knob_hdr_gamma() != 0;
    return s_hdr.active;
}

static int rc_backend_pump_messages(void)
{
#ifdef _WIN32
    MSG msg;
    if (s_window_thread) return s_d3d.window_closed ? -1 : 0;   /* the window thread pumps */
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return -1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
#endif
    return s_d3d.window_closed ? -1 : 0;   /* (off Windows the window thread waits on SDL's events) */
}

static void rc_backend_present(void)
{
    if (blink_dbg())
        printf("[PRESENT] draws=%u clears_since_last=%u clear_presents=%u\n",
               s_d3d.draw_count, s_dbg_clears_since_present, s_clear_presents);
    s_dbg_clears_since_present = 0;

    /* Once frame-boundary presents are active (d3d12_clear presents each
     * completed frame as the drain crosses into the next one), the ticker
     * present would only ever show the partially-accumulated NEXT frame --
     * that partial present right after the FIFO ring recycle was the visible
     * blink. Keep the ticker present solely as the boot-time fallback (before
     * the first framed clear arrives). */
    if (s_clear_presents > 0)
        return;

    /* Same display gate as d3d12_present: a batch of offscreen pass work only
     * (render-to-texture) keeps accumulating until its composite arrives.
     * Empty batches present only until the first real frame -- after that an
     * empty present is a flip/drain race and wipes the screen for a frame
     * (wave: black flashes and layout flicker between frames). */
    static int s_seen_content = 0;
    int has_display = (s_d3d.draw_count == 0 && !s_seen_content);
    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++)
        if (!s_d3d.draws[_i].is_clear && s_d3d.draws[_i].rt_off == 0) {
            has_display = 1;
            break;
        }
    /* The guest-framebuffer present needs the pass to run at all. */
    { static int gfb = -1;
      if (gfb < 0) { const char* e = getenv("GCM_GUEST_FB"); gfb = e ? 1 : 0; }
      if (gfb) has_display = 1; }
    if (s_composite_src) has_display = 1;
    if (has_display && s_d3d.draw_count > 0) s_seen_content = 1;

    /* VP_SUBMIT=<N>: has_display gates render_frame() entirely, so a batch whose
     * records all target an OFFSCREEN rt (rt_off != 0) presents without ever
     * running the draw pass -- the backbuffer then shows only the clear, which
     * reads as "nothing rasterizes" from every downstream check. */
    { static int cap = -1, n = 0;
      PS3_KNOB(cap, ps3_env_int("VP_SUBMIT", 0));
      if (cap && n < cap && s_d3d.draw_count) { n++;
        u32 onscreen = 0, offscreen = 0, clears = 0;
        for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
            if (s_d3d.draws[_i].is_clear) { clears++; continue; }
            if (s_d3d.draws[_i].rt_off) offscreen++; else onscreen++;
        }
        { /* Which surfaces the batch's draws actually target -- "offscreen=57"
           * does not say whether they all go to one RT or scatter. */
          u32 offs[8]; u32 cnt[8]; int nd = 0;
          for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
              if (s_d3d.draws[_i].is_clear) continue;
              u32 o = s_d3d.draws[_i].rt_off; int f = -1;
              for (int _j = 0; _j < nd; _j++) if (offs[_j] == o) { f = _j; break; }
              if (f < 0) { if (nd < 8) { offs[nd] = o; cnt[nd] = 1; nd++; } }
              else cnt[f]++;
          }
          { u32 vp = 0, novp = 0;
            for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
                if (s_d3d.draws[_i].is_clear) continue;
                if (s_d3d.draws[_i].is_vp) vp++; else novp++;
            }
            /* The execution loop skips anything without is_vp, so a batch full
             * of !is_vp records is counted, gated and then never drawn. */
            fprintf(stderr, "[PRESENTGATE] is_vp=%u not_vp=%u%c", vp, novp, 10); }
          fprintf(stderr, "[PRESENTGATE] targets:");
          for (int _j = 0; _j < nd; _j++)
              fprintf(stderr, " 0x%08X x%u", offs[_j], cnt[_j]);
          fprintf(stderr, "%c", 10); }
        fprintf(stderr, "[PRESENTGATE] records=%u onscreen=%u offscreen=%u clears=%u"
                        " has_display=%d seen_content=%d -> render_frame=%s\n",
                s_d3d.draw_count, onscreen, offscreen, clears, has_display,
                s_seen_content, (s_d3d.initialized && has_display) ? "YES" : "SKIPPED"); } }

    /* Execute the batch whenever it has draws. Gating the whole call on
     * has_display meant a render-to-texture pass -- every draw targeting an
     * offscreen surface -- was DISCARDED rather than deferred, so the texture
     * it produces was never written and whatever sampled it later read an
     * empty resource. Only the Present needs onscreen content. */
    if (s_d3d.initialized && (has_display || s_d3d.draw_count > 0)) {
        { extern void rsx_reset_upload_claims(void);
          static int remap = -1;
          PS3_KNOB(remap, ps3_env_int("TEX_REMAP", 0));
          if (remap) rsx_reset_upload_claims(); }
        s_present_this_frame = has_display;
        render_frame();
        s_present_this_frame = 1;
    }
}

/* The offline exports (rsx_shader_precompile, setup's shader pass): the
 * texts are rsxc_shader.c's and the cache is the compile service's (step
 * C3a), which is today's .dxbc cache. */
static int rc_shader_hlsl(int fragment, const u8* ucode, u32 ucode_len, u32 vp_start_slot,
                          int exp32, u32 cube_mask, u32 d2c_mask, char* out, u32 cap)
{
    return rsxc_shader_hlsl(fragment, ucode, ucode_len, vp_start_slot, exp32, cube_mask, d2c_mask, out, cap);
}
static u64 rc_program_hash64(int fragment, const u8* ucode, u32 ucode_len)
{
    return rsxc_program_hash64(fragment, ucode, ucode_len);
}
static u64 rc_shader_key(int fragment, const char* hlsl) { return rsxc_shader_key(fragment, hlsl); }
static int rc_shader_cache_put(int fragment, const char* hlsl)
{
    gpu_sc_perf(perf_on());
    return gpu_sc_put(GPU_SC_DXBC, fragment ? GPU_SC_PS : GPU_SC_VS, hlsl);
}
static const char* rc_shader_cache_path(void) { return gpu_sc_cache_dir(); }
static int rc_shader_fp_variants(const u8* ucode, u32 ucode_len, u32* out, int max)
{
    return rsxc_shader_fp_variants(ucode, ucode_len, out, max);
}

