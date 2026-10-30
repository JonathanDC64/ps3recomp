static int s_vp_topo_strip;   /* the VP pass's topology is TRIANGLESTRIP */
static void render_frame(void)
{
    vpj_sync();   /* the batch's vertices, before anything reads vp_vb */
    double _rf0 = perf_on() ? perf_now() : 0.0;
    { LARGE_INTEGER _q; QueryPerformanceCounter(&_q); s_rf_entry_qpc = _q.QuadPart; }
    s_rf_serial++;
    RSX_MARK("render_frame", s_dbg_last_draws, 0);
    memset(s_rf_cp, 0, sizeof s_rf_cp); RF_CP(0);
    u32 fi = s_d3d.frame_index;
    /* RSX_FRAME_SLEEP=<ms> (live, diagnostic): stall each batch this long.
     * Frames over ~36 ms put Demon's Souls's pacer into its 15 fps mode, with
     * a skip frame between rendered ones (see docs/28, Frame rate); this
     * reproduces that mode on a machine fast enough not to enter it. */
    { static int fs = -1;
      PS3_KNOB(fs, ps3_env_int("RSX_FRAME_SLEEP", 0));
      if (fs > 0) Sleep((DWORD)fs); }

    /* Drain the GPU before touching shared upload resources (vp_vb vertices,
     * vp_cb constants, per-frame texture staging): the previous frame's draws
     * may still be reading them, and overwriting mid-flight tears geometry
     * (gcm/cube: triangles mixing stale and new vertices -> missing/sliver
     * polygons). These workloads are a few draws/frame, so full serialisation
     * costs little. */
    { LARGE_INTEGER _w0, _w1, _w2, _f;
      QueryPerformanceCounter(&_w0);
      wait_for_gpu();
      QueryPerformanceCounter(&_w1);
      /* Before the pre-pass below resets `used` and reassigns slots. */
      if (s_wb_pending) { s_wb_pending = 0; rt_writeback_store(); }
      QueryPerformanceCounter(&_w2); QueryPerformanceFrequency(&_f);
      s_fl_wait_ms  = (double)(_w1.QuadPart - _w0.QuadPart) * 1000.0 / (double)_f.QuadPart;
      s_fl_store_ms = (double)(_w2.QuadPart - _w1.QuadPart) * 1000.0 / (double)_f.QuadPart; }
    rsx_scale_apply();
    hdr_batch_start();           /* the targets HDR promotes in this batch */
    off_rt_release_deferred();   /* RT resources the previous frame dropped */
    tex_stage_frame();           /* this frame's texture uploads start over */
    RF_CP(1);

    /* Compile the real vertex program once its microcode is captured, and keep
     * the constant bank uploaded for the VS. */
    /* The base VP pipeline gates the ENTIRE vertex-program draw pass below
     * (`if (s_d3d.vp_ready ...)`). The old trigger also required the microcode
     * size to differ from the last compile, so a title whose first captured
     * program has the same byte count as vp_compiled_bytes never built the base
     * pipeline -- and then every is_vp draw record was silently dropped: draws
     * recorded, constants uploaded, per-draw VS compiled and cached, and not one
     * of them submitted. While !vp_ready there is nothing to be stale against,
     * so only the "have microcode" test belongs here. */
    if (s_d3d.current_rsx_state && !s_d3d.vp_ready &&
        s_d3d.current_rsx_state->vp_ucode_bytes >= 16)
        compile_vp();
    /* Per-draw VP constants are snapshotted at record time (vp_record_cb). */

    /* Lazily create the readback buffer the first time a dump is requested. */
    if (s_d3d.dump_frames_left > 0 && !s_d3d.readback_buf) {
        s_d3d.readback_pitch = (s_d3d.host_w * 4 + 255) & ~255u;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (u64)s_d3d.readback_pitch * s_d3d.host_h;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d3d_create_committed(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.readback_buf);
    }

    /* Reset command allocator and list */
    frame_cmd_begin(fi);

    /* Upload the bound font atlas into an R8_UNORM texture and create its SRV.
     * The atlas is a linear 8-bit coverage map, so a straight row copy (no
     * deswizzle) suffices. It is a DYNAMIC glyph cache -- the game rasterizes
     * new glyphs into it over time -- so re-upload whenever it is dirty (set on
     * every bind), not just once. Uploading only on first bind left every glyph
     * added after frame 1 sampling stale texels -> torn text. The backend is
     * synchronous (wait_for_gpu each frame), so recreating the resource per
     * dirty frame is safe. */
    if (s_d3d.tex_bound && (!s_d3d.tex_ready || s_d3d.tex_dirty) && s_d3d.srv_heap && s_d3d.pipeline_state_tex) {
        extern uint8_t* vm_base;
        u32 w = s_d3d.tex_w, h = s_d3d.tex_h;
        u32 pitch = (w + 255) & ~255u;   /* D3D12 requires 256-byte row pitch */

        if (s_d3d.tex_resource) { s_d3d.tex_resource->lpVtbl->Release(s_d3d.tex_resource); s_d3d.tex_resource = NULL; }
        if (s_d3d.tex_upload)   { s_d3d.tex_upload->lpVtbl->Release(s_d3d.tex_upload);     s_d3d.tex_upload = NULL; }

        D3D12_HEAP_PROPERTIES hp_def = {0}; hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        HRESULT thr = d3d_create_committed(
            s_d3d.device, &hp_def, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.tex_resource);

        D3D12_HEAP_PROPERTIES hp_up = {0}; hp_up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ud = {0};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = (u64)pitch * h; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
        ud.SampleDesc.Count = 1; ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(thr))
            thr = d3d_create_committed(
                s_d3d.device, &hp_up, D3D12_HEAP_FLAG_NONE, &ud,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.tex_upload);

        if (SUCCEEDED(thr) && vm_base) {
            void* mapped = NULL;
            D3D12_RANGE nr = {0, 0};
            if (SUCCEEDED(s_d3d.tex_upload->lpVtbl->Map(s_d3d.tex_upload, 0, &nr, &mapped)) && mapped) {
                const u8* srcbase = vm_base + s_d3d.tex_src_offset;
                for (u32 y = 0; y < h; y++)
                    memcpy((u8*)mapped + (u64)y * pitch, srcbase + (u64)y * w, w);
                s_d3d.tex_upload->lpVtbl->Unmap(s_d3d.tex_upload, 0, NULL);

                D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
                dst.pResource = s_d3d.tex_resource;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                src.pResource = s_d3d.tex_upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset = 0;
                src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8_UNORM;
                src.PlacedFootprint.Footprint.Width    = w;
                src.PlacedFootprint.Footprint.Height   = h;
                src.PlacedFootprint.Footprint.Depth    = 1;
                src.PlacedFootprint.Footprint.RowPitch = pitch;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

                D3D12_RESOURCE_BARRIER tb = {0};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource   = s_d3d.tex_resource;
                tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &tb);

                D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
                sv.Format = DXGI_FORMAT_R8_UNORM;
                sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE sh;
                s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
                s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.tex_resource, &sv, sh);

                s_d3d.tex_ready = 1;
                s_d3d.tex_dirty = 0;   /* content now in sync with guest atlas */
                { static int _au = 0; if (_au < 3 && ++_au)
                    printf("[D3D12] atlas uploaded (%ux%u R8) -> textured\n", w, h); }
            }
        }
    }

    /* Frames with no ops (init/boot presents) don't count against the cap --
     * PNG-decode-heavy titles burn hundreds of empty presents before the first
     * real draw. */
    /* RTT_DUMP_SKIP=<n>: skip the first n frames that have ops (dump late frames). */
    { static int _sk = -1; PS3_KNOB(_sk, ps3_env_int("RTT_DUMP_SKIP", 0));
      if (_sk > 0 && s_d3d.draw_count > 0 && getenv("RTT_DUMP")) { _sk--; goto rtt_dump_done; } }
    if (getenv("RTT_DUMP") && s_d3d.draw_count > 0) { static int _f=0; int _cap = atoi(getenv("RTT_DUMP")); if (_cap < 2) _cap = 14; if (_f < _cap && ++_f) {
        fprintf(stderr, "[RTT] frame %d: %u ops\n", _f, s_d3d.draw_count);
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &s_d3d.draws[_d];
            fprintf(stderr, "[RTT]  op%02u %s rt=0x%X mrt=0x%X/0x%X/0x%X t0=0x%X fp=0x%X%s n=%u cmask=%X vp=%u,%u %ux%u blend=%d\n",
                _d, r->is_clear?"CLR ":(r->is_vp?"draw":"leg "), r->rt_off,
                r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                r->tex[0].raw, r->fp_addr, r->fp_exp32 ? "" : "/h", r->vertex_count,
                r->cmask, r->vp_x, r->vp_y, r->vp_w, r->vp_h, r->blend);
        }
    } }
rtt_dump_done:;

    /* Debug: RTT_PASS=N shows pass N's output directly on screen (drops later
     * ops, retargets pass N to the backbuffer). */
    { const char* rp = getenv("RTT_PASS");
      if (rp) {
        int keep = atoi(rp), seen = 0; u32 cut = s_d3d.draw_count;
        int viewrt = getenv("RTT_VIEWRT") != NULL;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &s_d3d.draws[_d];
            if (r->is_clear || !r->is_vp) continue;
            if (seen == keep) {
                /* Alone: retarget op N to the backbuffer. With RTT_VIEWRT:
                 * keep op N intact and pull the display composite forward so
                 * the chosen RT is shown as of this point in the chain. */
                if (!viewrt) { r->rt_off = 0; r->rt_mrt[0] = r->rt_mrt[1] = r->rt_mrt[2] = 0; }
                cut = _d + 1;
                break;
            }
            seen++;
        }
        if (viewrt) {
            for (u32 _j = cut; _j < s_d3d.draw_count && _j < MAX_DRAWS; _j++) {
                D3D12DrawRecord* r = &s_d3d.draws[_j];
                if (r->is_vp && !r->is_clear && r->rt_off == 0) {
                    if (cut < MAX_DRAWS) s_d3d.draws[cut++] = *r;
                    break;
                }
            }
        }
        s_d3d.draw_count = (cut < s_d3d.draw_count) ? cut : s_d3d.draw_count;
      } }

    /* DRAW_LIMIT=<N>: keep only the first N recorded ops this frame. The duck's
     * draws are ops 00..~03 of every frame, so a small limit shows the ducks
     * alone -- separating "the ducks never rasterize" from "later geometry is
     * painted over them". Diagnostic only. */
    { static int lim = -1;
      PS3_KNOB(lim, ps3_env_int("DRAW_LIMIT", 0));
      if (lim > 0 && s_d3d.draw_count > (u32)lim) s_d3d.draw_count = (u32)lim; }

    /* DRAW_SKIP_TEX=<hex raw offset>: drop every op binding that texture on
     * unit 0. The ducks sit behind the water's slab geometry, so removing the
     * occluder is the way to see whether the duck itself renders. Diagnostic. */
    { static const char* sk = (const char*)1; static u32 want = 0;
      if (sk == (const char*)1) sk = getenv("DRAW_SKIP_TEX");
      want = draw_filter_tex(sk);
      if (want) {
        u32 keep = 0;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            if (!s_d3d.draws[_d].is_clear && s_d3d.draws[_d].tex[0].raw == want) continue;
            if (keep != _d) s_d3d.draws[keep] = s_d3d.draws[_d];
            keep++;
        }
        s_d3d.draw_count = keep;
      } }

    /* DRAW_KEEP_TEX=<hex raw offset>: the inverse -- keep only the ops binding
     * that texture (plus clears). Isolates one object from everything drawn over
     * it, which is what finally shows the duck on its own. Diagnostic. */
    { static const char* kp = (const char*)1; static u32 want = 0;
      if (kp == (const char*)1) kp = getenv("DRAW_KEEP_TEX");
      want = draw_filter_tex(kp);
      /* DRAW_KEEP_NOCLEAR=1: also drop the clears. Keeping them preserves their
       * ORIGINAL position in the batch, so a clear that originally ran after the
       * kept draws still runs after them -- and wipes the very geometry being
       * isolated. That makes an object that renders fine look like it renders
       * nothing. */
      static int noclr = -1;
      PS3_KNOB(noclr, ps3_env_int("DRAW_KEEP_NOCLEAR", 0));
      /* DRAW_KEEP_BLEND=0|1: further restrict DRAW_KEEP_TEX to draws with that
       * blend flag. A mesh drawn twice -- once opaque, once blended -- is two
       * different passes (body vs reflection shell) that a texture filter alone
       * cannot separate. */
      static int kblend = -2;
      if (kblend == -2) { const char* e = getenv("DRAW_KEEP_BLEND");
                          kblend = e ? atoi(e) : -1; }
      if (want) {
        /* Emit the CLEARS FIRST, then the kept draws. Preserving the original
         * order means a clear that ran after them still runs after them and
         * wipes the isolated geometry; dropping the clears entirely (noclr)
         * leaves stale DEPTH that rejects most of its fragments. Neither shows
         * the object properly -- clear, then draw, does. */
        static D3D12DrawRecord kept[MAX_DRAWS];
        u32 nk = 0;
        if (!noclr)
            for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++)
                if (s_d3d.draws[_d].is_clear && nk < MAX_DRAWS) kept[nk++] = s_d3d.draws[_d];
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++)
            if (!s_d3d.draws[_d].is_clear && s_d3d.draws[_d].tex[0].raw == want
                && (kblend < 0 || s_d3d.draws[_d].blend == kblend)
                && nk < MAX_DRAWS)
                kept[nk++] = s_d3d.draws[_d];
        for (u32 _k = 0; _k < nk; _k++) s_d3d.draws[_k] = kept[_k];
        s_d3d.draw_count = nk;
      } }

    /* DRAW_LAST_TEX=<hex raw offset>: move the ops binding that texture to the
     * END of the batch so nothing is drawn over them. The duck's draws are ops
     * 00..03 of every frame and the tub wall is drawn afterwards across the same
     * screen area, so anything that defeats the depth sort hides the duck
     * completely. Reordering separates "occluded" from "not rendered". */
    { static const char* lt = (const char*)1; static u32 want = 0;
      if (lt == (const char*)1) lt = getenv("DRAW_LAST_TEX");
      want = draw_filter_tex(lt);
      if (want && s_d3d.draw_count > 1) {
        static D3D12DrawRecord moved[MAX_DRAWS];
        u32 nm = 0, keep = 0;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            if (!s_d3d.draws[_d].is_clear && s_d3d.draws[_d].tex[0].raw == want) {
                if (nm < MAX_DRAWS) moved[nm++] = s_d3d.draws[_d];
                continue;
            }
            if (keep != _d) s_d3d.draws[keep] = s_d3d.draws[_d];
            keep++;
        }
        for (u32 _m = 0; _m < nm && keep < MAX_DRAWS; _m++)
            s_d3d.draws[keep++] = moved[_m];
        s_d3d.draw_count = keep;
      } }

    RF_CP(2);
    /* Render-to-texture pre-pass: make sure an offscreen RT resource exists for
     * every non-display surface targeted this frame (so draws binding it as a
     * texture can resolve to it below, whatever the op order). */
    for (int _i = 0; _i < MAX_OFF_RTS; _i++) s_d3d.off_rt[_i].used = 0;
    for (int _i = 0; _i < MAX_ZETA; _i++) s_zeta[_i].used = 0;
    off_rt_drop_rewritten();
    for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &s_d3d.draws[_d];
        if (dr->is_vp && dr->zfmt && ((dr->depth & 1u) || (dr->clr_flags & 3u)))
            zeta_get(dr->zeta, dr->rt_w, dr->rt_h);
        u32 _offs[4] = { dr->rt_off, dr->rt_mrt[0], dr->rt_mrt[1], dr->rt_mrt[2] };
        for (int _m = 0; _m < 4; _m++) {
            if (!dr->is_vp || !_offs[_m]) continue;
            int _s = off_rt_get(_offs[_m], dr->rt_w, dr->rt_h, dr->rt_fmt);
            if (_s >= 0) { s_d3d.off_rt[_s].gfmt = dr->rt_fmt;
                           s_d3d.off_rt[_s].gpitch = dr->rt_pitch[_m];
                           s_d3d.off_rt[_s].gdma = dr->rt_dma[_m]; }
        }
    }

    /* Per-frame VP textures + guest-FP pipelines: for each VP draw, upload the
     * texture it had bound at submit time into a slot (SRV heap
     * VP_TEX_SRV_BASE+slot; plasma animates so contents re-upload every
     * frame) and pre-build its FP PSO.
     * A texture whose offset matches an offscreen RT samples the RT directly
     * (tex_slot 1000+idx) -- no guest-memory upload. */
    RF_CP(3); rf_pp_snap(s_rf_pp0, s_rf_ppn0);
    late_check();
    double _pre0 = perf_on() ? perf_now() : 0.0;
    tcj_dispatch();
    s_psj_defer = psj_threads() > 0;
    for (int _i = 0; _i < VP_TEX_SLOTS; _i++) s_d3d.vp_tex[_i].used = 0;
    for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &s_d3d.draws[_d];
        if (!dr->is_vp || dr->is_clear) continue;
        /* Debug: RTT_VIEWRT=<hex raw offset> makes display draws sample that
         * offscreen RT at t0 (the composite blit then shows it fullscreen). */
        { static const char* vr = (const char*)1;     /* hoisted: this runs per DRAW */
          if (vr == (const char*)1) vr = getenv("RTT_VIEWRT");
          if (vr && dr->rt_off == 0) {
              dr->tex[0].raw = (u32)strtoul(vr, NULL, 16);
              dr->tex[0].off = 0;
              dr->tex[0].set = 1;
          } }
        /* SCREENTEX=1: report draws that sample a DISPLAY-SIZED texture. On RSX
         * a title renders a reflection/refraction into a surface and then
         * samples it; our backend renders into a D3D backbuffer, so the guest
         * memory that sampler reads is never written and the texture comes back
         * empty. Finding which geometry does it is the first step to feeding it
         * the rendered image instead. */
        /* EMPTYTEX=1: which geometry binds a texture whose source is all zero.
         * An unresolved texture renders untextured, so this names the surfaces
         * affected instead of leaving it to guesswork. */
        { static int ed = -1;
          PS3_KNOB(ed, ps3_env_int("EMPTYTEX", 0));
          extern uint8_t* vm_base;
          if (ed && vm_base) for (int _u = 0; _u < RSX_FP_UNITS; _u++) {
              if (!dr->tex[_u].set || !dr->tex[_u].off) continue;
              u32 nz = 0, sz = dr->tex[_u].w * dr->tex[_u].h * 4u;
              for (u32 i2 = 0; i2 < sz && i2 < 0x20000u; i2 += 997)
                  if (vm_base[dr->tex[_u].off + i2]) { nz = 1; break; }
              if (!nz) {
                  static u32 seen[24]; static int ns = 0; int known = 0;
                  for (int k = 0; k < ns; k++) if (seen[k] == dr->fp_addr) known = 1;
                  if (!known && ns < 24) { seen[ns++] = dr->fp_addr;
                      /* Scan a window around the resolved address: if the pass
                       * that fills this surface ran but landed elsewhere, the
                       * data is nearby; if the whole window is zero it was never
                       * produced. */
                      /* Same offset through BOTH context DMAs: a texture the
                       * guest put in main memory but that we resolve as LOCAL
                       * reads as untouched VRAM, i.e. all zeros. */
                      { extern u32 cellGcmResolveLocated(int, u32);
                        u32 la = cellGcmResolveLocated(1, dr->tex[_u].raw);
                        u32 ma = cellGcmResolveLocated(0, dr->tex[_u].raw);
                        u32 ln = 0, mn = 0;
                        for (u32 i4 = 0; i4 < 0x20000u; i4 += 997) {
                            if (vm_base[la + i4]) ln++;
                            if (vm_base[ma + i4]) mn++;
                        }
                        fprintf(stderr, "[TEXLOC] raw=0x%08X LOCAL@0x%08X nz=%u  MAIN@0x%08X nz=%u%c",
                                dr->tex[_u].raw, la, ln, ma, mn, 10); }
                      { u32 base = dr->tex[_u].off & ~0xFFFFFu;
                        for (u32 w = 0; w < 0x400000u; w += 0x100000u) {
                            u32 nzc = 0;
                            for (u32 i3 = 0; i3 < 0x100000u; i3 += 1021)
                                if (vm_base[base + w + i3]) nzc++;
                            fprintf(stderr, "[EMPTYSCAN] 0x%08X..+1MB nonzero %u/1027%c",
                                    base + w, nzc, 10);
                        } }
                      fprintf(stderr, "[EMPTYTEX] fp=0x%X unit=%d raw=0x%08X res=0x%08X %ux%u"
                                      " fmt=0x%02X verts=%u vp=%u,%u %ux%u%c",
                              dr->fp_addr, _u, dr->tex[_u].raw, dr->tex[_u].off, dr->tex[_u].w,
                              dr->tex[_u].h, dr->tex[_u].fmt, dr->vertex_count,
                              dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, 10); }
              }
          } }
        { static int sd = -1;
          PS3_KNOB(sd, ps3_env_int("SCREENTEX", 0));
          if (sd) for (int _u = 0; _u < RSX_FP_UNITS; _u++)
              if (dr->tex[_u].set && dr->tex[_u].w == s_d3d.width &&
                  dr->tex[_u].h == s_d3d.height) {
                  static u32 seen[16]; static int ns = 0; int known = 0;
                  for (int k = 0; k < ns; k++) if (seen[k] == dr->fp_addr) known = 1;
                  if (!known && ns < 16) { seen[ns++] = dr->fp_addr;
                      fprintf(stderr, "[SCREENTEX] unit=%d raw=0x%08X %ux%u fmt=0x%02X"
                                      " fp=0x%X verts=%u vp=%u,%u %ux%u%c",
                              _u, dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h,
                              dr->tex[_u].fmt, dr->fp_addr, dr->vertex_count,
                              dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, 10); }
              } }
        /* Fill this draw's SRV window (DRAW_SRV_BASE + d*RSX_FP_UNITS): each unit
         * resolves to an offscreen RT (sampled directly), an uploaded guest
         * texture, or a null SRV. */
        double _sv0 = perf_on() ? perf_now() : 0.0;
        dr->d2c_mask = 0;
        /* Once per draw: it walks all sixteen units, and the unit loop below
         * and the PSO lookup after it each wanted it. */
        const u32 _cmask = dr_cube_mask(dr);
        /* The units the program samples; the others get a null SRV.
         * RSX_SRV_USED=0 binds all sixteen. */
        u32 _used = 0xFFFFFFFFu;
        { static int en = -1;
          PS3_KNOB(en, ps3_env_int("RSX_SRV_USED", 1));
          if (en && dr->is_vp && dr->fp_addr) {
              extern u32 cellGcmResolveLocated(int, u32);
              const u32 foff = cellGcmResolveLocated((dr->fp_addr & 0x3u) == 1,
                                                     dr->fp_addr & ~0x3u);
              if (foff != 0xFFFFFFFFu) _used = fp_units_at(foff);
          } }
        for (int _u = 0; _u < RSX_FP_UNITS; _u++) {
            u32 wslot = DRAW_SRV_BASE + _d * RSX_FP_UNITS + (u32)_u;
            dr->tex_rt[_u] = -1;
            dr->tex_z[_u] = -1;
            if (!(_used & (1u << _u))) {
                srv_write(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                          D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                continue;
            }
            /* An A8R8G8B8 / D8R8G8B8 unit over a depth surface reads the depth
             * buffer's bytes as colour (RPCS3 RSXThread.cpp "redirected"
             * textures; Demon's Souls's light shafts and depth of field read
             * the scene depth this way). Bind the depth view; the fragment
             * program decodes it (rsx_d2c). The unit's remap is not applied:
             * the titles seen use the identity (0xAAE4), as RPCS3 notes. A
             * colour target at the same offset wins: that is what was drawn. */
            if (dr->tex[_u].set && ((dr->tex[_u].fmt & 0x9F) == 0x85 ||
                                    (dr->tex[_u].fmt & 0x9F) == 0x9E)) {
                static int en = -1;
                PS3_KNOB(en, ps3_env_int("RSX_DEPTH_AS_COLOR", 1));
                int zs = en && off_rt_find(dr->tex[_u].raw) < 0 ? zeta_find(dr->tex[_u].raw) : -1;
                if (zs >= 0) {
                    dr->tex_z[_u] = zs;
                    dr->d2c_mask |= 1u << _u;
                    srv_write(wslot, s_zeta[zs].res, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
            }
            /* A depth-format unit reads the depth surface rendered at its offset
             * (a shadow map). With nothing rendered there it samples nothing. */
            if (dr->tex[_u].set && rsx_fmt_is_depth(dr->tex[_u].fmt & 0x9F)) {
                int zs = zeta_find(dr->tex[_u].raw);
                if (zs >= 0) {
                    dr->tex_z[_u] = zs;
                    srv_write(wslot, s_zeta[zs].res, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                } else
                    srv_write(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                continue;
            }
            /* Display-sized sampler source -> the rendered frame. Only when no
             * offscreen RT lives at that offset: Demon's Souls renders its
             * loading screen into a 1280x720 RT and composites it through a
             * gamma LUT, and the frame copy (last frame's backbuffer) here
             * replaced the RT it had just drawn -- a feedback loop to flat grey.
             * And only when the guest memory there is empty, which is what a
             * surface the GPU rendered looks like: a display-sized image the
             * title wrote itself (a cellSail movie frame) is the texture. */
            if (dr->tex[_u].set && s_screen_copy &&
                dr->tex[_u].w == s_d3d.width && dr->tex[_u].h == s_d3d.height &&
                off_rt_find(dr->tex[_u].raw) < 0 &&
                !tex_guest_written(dr->tex[_u].off, dr->tex[_u].w * dr->tex[_u].h * 4u)) {
                static int en = -1;
                PS3_KNOB(en, ps3_env_int("SCREEN_AS_TEX", 1));
                if (en) {
                    { static int _n = 0; if (_n < 3 && ++_n)
                        fprintf(stderr, "[SCREENTEX] bound frame copy at unit %d for"
                                        " fp=0x%X (%ux%u)%c", _u, dr->fp_addr,
                                dr->tex[_u].w, dr->tex[_u].h, 10); }
                    srv_write(wslot, s_screen_copy, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
            }
            /* A sampler whose guest buffer was never written, at the size of a
             * reduced-viewport pass we captured: that pass IS its producer. */
            if (dr->tex[_u].set && dr->tex[_u].off && s_subvp_n) {
                int hit = -1;
                for (int i = 0; i < s_subvp_n; i++)
                    if (s_subvp[i].res && s_subvp[i].w == dr->tex[_u].w &&
                        s_subvp[i].h == dr->tex[_u].h) { hit = i; break; }
                if (hit >= 0) {
                    extern uint8_t* vm_base;
                    u32 nz = 0, sz = dr->tex[_u].w * dr->tex[_u].h * 4u;
                    for (u32 i5 = 0; i5 < sz && i5 < 0x20000u; i5 += 997)
                        if (vm_base[dr->tex[_u].off + i5]) { nz = 1; break; }
                    if (!nz) {
                        { static int _n = 0; if (getenv("SUBVP_DBG") && _n < 8 && ++_n)
                            fprintf(stderr, "[SUBVP] unit %d fp=0x%X <- captured %ux%u%c",
                                    _u, dr->fp_addr, dr->tex[_u].w, dr->tex[_u].h, 10); }
                        srv_write(wslot, s_subvp[hit].res, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                        continue;
                    }
                }
            }
            if (dr->tex[_u].set) {
                int rt = off_rt_find(dr->tex[_u].raw);
                if (rt >= 0) {
                    dr->tex_rt[_u] = rt;
                    srv_write(wslot, s_d3d.off_rt[rt].res, (DXGI_FORMAT)s_d3d.off_rt[rt].dxgi,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
                u32 _bf = dr->tex[_u].fmt & 0x9F;
                if (dr->tex[_u].off && vp_tex_fmt_ok(_bf)) {
                    double _tt = perf_on() ? perf_now() : 0.0;
                    int _cube = dr->tex[_u].cube && (_cmask & (1u << _u));
                    int ts = vp_upload_tex_slot(dr->tex[_u].off, dr->tex[_u].w,
                                                dr->tex[_u].h, dr->tex[_u].fmt,
                                                _cube, dr->tex[_u].mips);
                    if (perf_on()) { s_perf_tex += perf_now() - _tt; s_perf_ntex++;
                        s_perf_texbytes += (u64)dr->tex[_u].w * dr->tex[_u].h * 4u; }
                    /* Both forms of the offset are in hand here; the filters
                     * compare the BOUND one. */
                    if (s_duck_off && dr->tex[_u].off == s_duck_off)
                        s_duck_raw = dr->tex[_u].raw;
                    if (ts >= 0) {
                        rsx_tex_layout _tl;
                        rsx_texture_layout(dr->tex[_u].fmt, dr->tex[_u].w, dr->tex[_u].h, &_tl);
                        /* The crossbar is defined for the 8-bit colour classes
                         * (rsx_texture_component_remap); the 16- and 32-bit
                         * ones are viewed as stored, like an offscreen RT. */
                        const int _x8 = _tl.fmt == RSX_TEXFMT_R8G8B8A8 ||
                                        _tl.fmt == RSX_TEXFMT_R8G8 || _tl.compressed;
                        srv_write_ex(wslot, s_d3d.vp_tex[ts].res, rsx_texfmt_dxgi(_tl.fmt),
                                  (_bf == 0x81) ? 0x1000
                                  : _x8 ? rsx_remap_to_d3d(dr->tex[_u].ctrl1, _bf)
                                        : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                  _cube);
                        continue;
                    }
                }
            }
            /* TEX_BINDDBG=<N>: a unit that ends up with a NULL SRV. The draw is
             * still recorded, lit and transformed -- it just samples nothing,
             * which renders as a solid black object. */
            { static int cap = -1, n = 0;
              PS3_KNOB(cap, ps3_env_int("TEX_BINDDBG", 0));
              if (cap && n < cap && dr->tex[_u].set) { n++;
                fprintf(stderr, "[TEXBIND] null SRV unit=%d raw=0x%X off=0x%X %ux%u fmt=0x%02X%c",
                        _u, dr->tex[_u].raw, dr->tex[_u].off, dr->tex[_u].w,
                        dr->tex[_u].h, dr->tex[_u].fmt, 10); } }
            srv_write(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        }
        if (perf_on()) s_perf_srv += perf_now() - _sv0;
        double _ps0 = perf_on() ? perf_now() : 0.0;
        const int _pm0 = s_perf_pso_miss;
        if (dr->fp_addr) {
            dr->pso = vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key,
                                    dr_num_rts(dr),
                                    dr->rt_off ? rt_dxgi(dr->rt_off, dr->rt_fmt)
                                               : DXGI_FORMAT_R8G8B8A8_UNORM,
                                    dr->fp_exp32, dr->cmask, dr->cull, dr->depth,
                                    _cmask, dr->d2c_mask, dr_uvclamp(dr), dr_hdrv(dr));
            dr->pso_serial = s_rf_serial;
            if (s_psj_last >= 0) {   /* filled in by psj_finish */
                s_psj_fix[s_psj_fix_n].draw = _d;
                s_psj_fix[s_psj_fix_n].entry = s_psj_last;
                s_psj_fix_n++;
            }
        }
        if (perf_on()) { const double _pt = perf_now() - _ps0; s_perf_pso += _pt;
                         if (s_perf_pso_miss != _pm0) s_perf_pmiss += _pt; }
    }
    s_psj_defer = 0;
    psj_finish();
    drawprint_batch();
    RF_CP(4); rf_pp_snap(s_rf_pp, s_rf_ppn);

    /* Transition render target to RENDER_TARGET state */
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = s_d3d.render_targets[fi];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

    /* Get RTV handle for current frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);
    rtv_handle.ptr += fi * s_d3d.rtv_descriptor_size;

    /* Get DSV handle (single depth buffer shared across frames) */
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);

    if (perf_on()) s_perf_pre += perf_now() - _pre0;
    /* Set render target + depth */
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);

    /* Clear color and depth */
    s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(
        s_d3d.cmd_list, rtv_handle, s_d3d.clear_color, 0, NULL);
    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(
        s_d3d.cmd_list, dsv_handle,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, NULL);

    /* Set viewport and scissor */
    D3D12_VIEWPORT viewport = {0, 0, (float)s_d3d.host_w, (float)s_d3d.host_h, 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, (LONG)s_d3d.host_w, (LONG)s_d3d.host_h};
    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &viewport);
    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &scissor);

    /* Bind pipeline state and push MVP if anything to draw */
    if (s_d3d.pipeline_ready && s_d3d.draw_count > 0) {
        s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.root_signature);
        s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &s_d3d.vb_view);

        /* Make the atlas SRV heap current and bind its table (t0) for textured
         * draws. Safe to set even if no draw is textured. */
        if (s_d3d.tex_ready && s_d3d.srv_heap) {
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 1, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
        }

        /* Push the MVP matrix from RSX vertex constants slots 0..3.
         * If the game hasn't written any constants (e.g. placeholder data
         * already in clip space), fall back to identity. */
        float mvp[16];
        const rsx_state* st = s_d3d.current_rsx_state;
        int have_mvp = 0;
        if (st) {
            for (u32 r = 0; r < 4; r++) {
                for (u32 c = 0; c < 4; c++) {
                    float v = st->vertex_constants[r][c];
                    mvp[r * 4 + c] = v;
                    if (v != 0.0f) have_mvp = 1;
                }
            }
        }
        if (!have_mvp) {
            memset(mvp, 0, sizeof(mvp));
            mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f; /* identity */
        }
        s_d3d.cmd_list->lpVtbl->SetGraphicsRoot32BitConstants(
            s_d3d.cmd_list, 0 /*root param 0*/, 16, mvp, 0);

        /* D3D12_IQ=1: dump exactly what the GPU is about to see -- the MVP root
         * constants and the first uploaded host vertices. The legacy path draws
         * with no validation errors yet produces zero pixels, so the geometry
         * must be degenerate/off-screen post-VS. */
        { static int _vd = -1;
          if (_vd < 0) { const char* e = getenv("D3D12_IQ"); _vd = e ? 1 : 0; }
          static int _n = 0;
          if (_vd && _n < 3) {
            _n++;
            fprintf(stderr, "[D3D12-DBG] have_mvp=%d draw_count=%u vb_offset=%u\n",
                    have_mvp, s_d3d.draw_count, s_d3d.vb_offset);
            fprintf(stderr, "[D3D12-DBG] mvp rows: [%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]"
                            "[%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]\n",
                    mvp[0],mvp[1],mvp[2],mvp[3], mvp[4],mvp[5],mvp[6],mvp[7],
                    mvp[8],mvp[9],mvp[10],mvp[11], mvp[12],mvp[13],mvp[14],mvp[15]);
            if (s_d3d.vb_mapped) {
                /* host vertex = 9 floats (pos3 + col4 + uv2), VERTEX_STRIDE=36 */
                const float* bv = (const float*)s_d3d.vb_mapped;
                for (int k = 0; k < 3; k++) {
                    const float* v = bv + k * 9;
                    fprintf(stderr, "[D3D12-DBG]  hostvert[%d] pos=(%.3f,%.3f,%.3f) col=(%.2f,%.2f,%.2f,%.2f)\n",
                            k, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
                }
            }
            for (u32 d = 0; d < s_d3d.draw_count && d < 3; d++) {
                const D3D12DrawRecord* dr = &s_d3d.draws[d];
                fprintf(stderr, "[D3D12-DBG]  draw[%u] is_vp=%d is_clear=%d topo=%u cnt=%u vbofs=%u startv=%u rt=%u\n",
                        d, dr->is_vp, dr->is_clear, dr->topology, dr->vertex_count,
                        dr->vb_byte_offset, dr->vb_byte_offset / VERTEX_STRIDE, dr->rt_off);
            }
            fflush(stderr);
          } }

        /* Replay each recorded draw with its own primitive topology and
         * the matching PSO class (triangle / line / point). The PSO class
         * must match the topology or D3D12 rejects the draw. */
        u32 last_topo = 0xFFFFFFFFu;
        ID3D12PipelineState* last_pso = NULL;
        u32 draws = s_d3d.draw_count;
        if (draws > MAX_DRAWS) draws = MAX_DRAWS;
        for (u32 d = 0; d < draws; d++) {
            const D3D12DrawRecord* dr = &s_d3d.draws[d];
            if (dr->is_vp) continue; /* drawn by the VP pass below */

            /* Select PSO: textured triangles (dbgfont) use the atlas PSO;
             * otherwise pick by topology class. */
            ID3D12PipelineState* target_pso = s_d3d.pipeline_state; /* default triangle */
            if (dr->textured && s_d3d.tex_ready && s_d3d.pipeline_state_tex) {
                target_pso = s_d3d.pipeline_state_tex;
            } else if (dr->topology == RSX_TOPOLOGY_POINTS) {
                target_pso = s_d3d.pipeline_state_points
                             ? s_d3d.pipeline_state_points : s_d3d.pipeline_state;
            } else if (dr->topology == RSX_TOPOLOGY_LINES ||
                       dr->topology == RSX_TOPOLOGY_LINE_STRIP) {
                target_pso = s_d3d.pipeline_state_lines
                             ? s_d3d.pipeline_state_lines : s_d3d.pipeline_state;
            }
            if (target_pso != last_pso) {
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, target_pso);
                last_pso = target_pso;
            }
            if (dr->topology != last_topo) {
                s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, topo_to_d3d(dr->topology));
                last_topo = dr->topology;
            }
            u32 start_vert = dr->vb_byte_offset / VERTEX_STRIDE;
            s_d3d.cmd_list->lpVtbl->DrawInstanced(
                s_d3d.cmd_list, dr->vertex_count, 1, start_vert, 0);
        }
    }

    RF_CP(5);
    /* VP pass: real decompiled vertex program + atlas alpha-test PS. Feeds raw
     * float4 attrib0 from vp_vb and the vp_c[] constant bank. */
    if (s_d3d.vp_ready && s_d3d.draw_count > 0) {
        int any = 0;
        for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++)
            if (s_d3d.draws[d].is_vp) { any = 1; break; }
        /* Textured geometry (dbgfont atlas) uses the sampling PS; untextured 3D
         * (vkcube) uses the colour-only PS. Fall back to whichever exists. */
        ID3D12PipelineState* vpso =
            (s_d3d.tex_ready && s_d3d.pipeline_state_vp) ? s_d3d.pipeline_state_vp
                                                         : s_d3d.pipeline_state_vp_color;
        if (!vpso) vpso = s_d3d.pipeline_state_vp;
        { static int cap = -1, n = 0;
          PS3_KNOB(cap, ps3_env_int("VP_SUBMIT", 0));
          if (cap && n < cap) { n++;
            u32 nvp = 0, nclr = 0;
            for (u32 d2 = 0; d2 < s_d3d.draw_count && d2 < MAX_DRAWS; d2++) {
                if (s_d3d.draws[d2].is_vp)    nvp++;
                if (s_d3d.draws[d2].is_clear) nclr++;
            }
            fprintf(stderr, "[VPPASS] records=%u is_vp=%u clears=%u any=%d vpso=%p rootsig=%p\n",
                    s_d3d.draw_count, nvp, nclr, any,
                    (void*)vpso, (void*)s_d3d.vp_root_sig); } }
        /* (No vertex buffer or vertex constants: RSX_VP_BUFFERS=0, or their
         * allocation failed. The pass is skipped rather than faulting.) */
        if (any && vpso && s_d3d.vp_vb && s_d3d.vp_cb) {
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.vp_root_sig);
            s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, vpso);
            s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            s_vp_topo_strip = 0;
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb));
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap, s_d3d.samp_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, s_d3d.samp_heap ? 2 : 1, heaps);
            vp_bind_sampler_table(NULL);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
            D3D12_VERTEX_BUFFER_VIEW vbv;
            vbv.BufferLocation = s_d3d.vp_vb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_vb)
                               + (u64)s_d3d.vp_parity * s_vp_cap * 256;
            vbv.SizeInBytes    = s_vp_cap * 256;
            vbv.StrideInBytes  = 256;   /* 16 float4 attrib slots */
            s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &vbv);
            if (s_d3d.vp_ib) {
                D3D12_INDEX_BUFFER_VIEW ibv;
                ibv.BufferLocation = s_d3d.vp_ib->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_ib)
                                   + (u64)s_d3d.vp_parity * s_vp_icap * 4;
                ibv.SizeInBytes    = s_vp_icap * 4;
                ibv.Format         = DXGI_FORMAT_R32_UINT;
                s_d3d.cmd_list->lpVtbl->IASetIndexBuffer(s_d3d.cmd_list, &ibv);
            }
            if (s_d3d.vf_buf)
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootShaderResourceView(s_d3d.cmd_list, 4,
                    s_d3d.vf_buf->lpVtbl->GetGPUVirtualAddress(s_d3d.vf_buf)
                    + (u64)s_d3d.vp_parity * s_vf_cap);
            D3D12_GPU_DESCRIPTOR_HANDLE gh_base;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh_base);
            int cur_rt = -1;                       /* target A: -1 = backbuffer */
            float vp_last_w = 0, vp_last_h = 0;    /* bound target's guest size; DRAWARGS */
            /* its host size over guest size (RSX_SCALE). The batch starts on the display
             * buffer, already bound: a batch that never changes target (a loading
             * screen) draws with this ratio, and at 1.0 it drew at the guest size, in
             * the top-left quarter of every other frame. */
            float tgt_sx = (float)s_d3d.host_w / (float)s_d3d.width;
            float tgt_sy = (float)s_d3d.host_h / (float)s_d3d.height;
            float dvp_x = 0, dvp_y = 0, dvp_w = 0, dvp_h = 0;  /* per-draw, for DRAWARGS */
            int cur_m[3] = {-1, -1, -1};           /* MRT B/C/D: -1 = unbound   */
            int cur_z = -1;                        /* zeta: -1 = shared buffer  */
            int cur_zro = 0;                       /* cur_z bound read-only     */
            int cur_nocol = 0;                     /* depth-only pass bound     */
            double _rec0 = perf_on() ? perf_now() : 0.0;
            int seen_small_vp = 0, captured = 0;
            /* DRAW_SKIP=a-b (live, diagnostic): leave out the frame's ops a..b
             * (manifest op numbers), to bisect which op hides or breaks a
             * later one. Mostly for rsx_replay. */
            static unsigned skip_gen; static u32 skip_a = 1, skip_b = 0;
            if (skip_gen != ps3_env_gen) { const char* e = getenv("DRAW_SKIP");
                skip_gen = ps3_env_gen; skip_a = 1; skip_b = 0;
                if (e && sscanf(e, "%u-%u", &skip_a, &skip_b) == 1) skip_b = skip_a; }
            /* RSX_AA: this batch's anti-aliasing pass, on target aa_t before
             * op aa_before (aa_pick). A batch with fewer draws than a 3D
             * frame needs is left alone and not logged: Demon's Souls shows
             * every other present from a one-op batch that copies the last
             * frame out again, which would make the log alternate. */
            hdr_mark_scan();
            int aa_t = -1;
            u32 aa_before = 0;
            if (!aa_mode() || knob_aa_at_present())
                s_aa.log_valid = 0;   /* when it comes back on, log the first choice */
            else if (s_present_this_frame && s_d3d.draw_count >= AA_MIN_SCENE) {
                int why = AA_RUN;
                aa_t = aa_pick(&aa_before, &why);
                aa_log(aa_t, aa_before, why);
            }
            for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++) {
                const D3D12DrawRecord* dr = &s_d3d.draws[d];
                if (!dr->is_vp) continue;
                /* The pass binds its own pipeline. Then the VP pass's state
                 * goes back: its root signature, which drops every root
                 * argument (vf_buf's is set again here, the rest per draw
                 * below), its topology, and the targets bound now. */
                if (aa_t >= 0 && d >= aa_before) {
                    if (aa_run_rt(aa_t)) {
                        ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
                        cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.vp_root_sig);
                        if (s_d3d.vf_buf)
                            cl->lpVtbl->SetGraphicsRootShaderResourceView(cl, 4,
                                s_d3d.vf_buf->lpVtbl->GetGPUVirtualAddress(s_d3d.vf_buf)
                                + (u64)s_d3d.vp_parity * s_vf_cap);
                        cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        s_vp_topo_strip = 0;
                        D3D12_CPU_DESCRIPTOR_HANDLE rh[4];
                        UINT nrt = 0;
                        if (!cur_nocol) {
                            rh[nrt++] = cur_rt >= 0 ? off_rt_rtv(cur_rt) : rtv_handle;
                            for (int _m = 0; _m < 3 && cur_m[_m] >= 0; _m++)
                                rh[nrt++] = off_rt_rtv(cur_m[_m]);
                        }
                        D3D12_CPU_DESCRIPTOR_HANDLE dz = cur_z < 0 ? dsv_handle
                                                       : cur_zro ? zeta_dsv_ro(cur_z) : zeta_dsv(cur_z);
                        cl->lpVtbl->OMSetRenderTargets(cl, nrt, nrt ? rh : NULL, FALSE, &dz);
                    }
                    aa_t = -1;
                }
                if (d >= skip_a && d <= skip_b && !dr->is_clear) continue;
                /* The reduced-viewport passes (this title's reflection, drawn
                 * into a region of the same target) finish here -- snapshot
                 * before the full-screen scene overwrites them. */
                if (!dr->is_clear && dr->ctgt) {
                    if (dr->vp_w && dr->vp_w < s_d3d.width) {
                        seen_small_vp = 1;
                        subvp_note(dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h);
                    }
                    else if (seen_small_vp && !captured &&
                             dr->vp_w == s_d3d.width && dr->rt_off == 0) {
                        captured = 1;
                        screen_copy_capture(fi);
                        subvp_capture(fi);
                        s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(
                            s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
                    }
                }
                /* Render-to-texture: retarget when this op's surfaces differ.
                 * Depth is a single shared buffer, so clear it per switch. */
                int want  = dr->rt_off  ? off_rt_find(dr->rt_off)  : -1;
                /* Remember where geometry actually went: off_rt_find() matches
                 * on offset alone and slots share offsets at different sizes,
                 * so the composite cannot just look the source up by address. */
                if (want >= 0) { s_last_drawn_slot = want; s_last_drawn_off = dr->rt_off; }
                int wantm[3];
                for (int _m = 0; _m < 3; _m++)
                    wantm[_m] = dr->rt_mrt[_m] ? off_rt_find(dr->rt_mrt[_m]) : -1;
                /* Depth follows the op's zeta when it has one; ops without a
                 * depth surface (or not using depth) keep the shared buffer. */
                int wz = dr->zfmt && ((dr->depth & 1u) || (dr->clr_flags & 3u))
                       ? zeta_find(dr->zeta) : -1;
                if (wz < 0 && cur_z >= 0 && !(dr->depth & 1u) && !(dr->clr_flags & 3u))
                    wz = cur_z;   /* depth unused: don't rebind just to drop it */
                /* A draw that samples the depth surface it is bound to and does
                 * not write depth (Demon's Souls's light shafts test against
                 * the scene depth and fade by it) binds it read-only, so it can
                 * be a depth target and a texture at once, as on RSX. */
                int wro = 0;
                if (wz >= 0 && !(dr->depth & 2u) && !(dr->clr_flags & 3u))
                    for (int _u = 0; _u < RSX_FP_UNITS; _u++)
                        if (dr->tex_z[_u] == wz) { wro = 1; break; }
                /* Colour target NONE: a depth-only pass. It binds no colour
                 * target and takes the depth surface's size -- bound to the
                 * backbuffer, the 2048x2048 shadow atlas was clipped to the
                 * window and its cascade viewports fell back to full-target. */
                const int nocol = dr->ctgt == 0;
                if (want != cur_rt || wantm[0] != cur_m[0] ||
                    wantm[1] != cur_m[1] || wantm[2] != cur_m[2] || wz != cur_z ||
                    wro != cur_zro || nocol != cur_nocol) {
                    D3D12_CPU_DESCRIPTOR_HANDLE dsv_op = wz < 0 ? dsv_handle
                                                       : wro ? zeta_dsv_ro(wz) : zeta_dsv(wz);
                    if (wz >= 0) zeta_transition(wz, wro ? ZETA_READ_STATE
                                                         : D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    D3D12_CPU_DESCRIPTOR_HANDLE rh[4];
                    UINT nrt = 1;
                    rh[0] = rtv_handle;
                    D3D12_VIEWPORT vp = {0, 0, (float)s_d3d.host_w, (float)s_d3d.host_h, 0.0f, 1.0f};
                    float gw = (float)s_d3d.width, gh = (float)s_d3d.height;   /* the guest's size of it */
                    if (want >= 0) {
                        off_rt_transition(want, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        rh[0] = off_rt_rtv(want);
                        vp.Width  = (float)s_d3d.off_rt[want].hw;
                        vp.Height = (float)s_d3d.off_rt[want].hh;
                        gw = (float)s_d3d.off_rt[want].w;
                        gh = (float)s_d3d.off_rt[want].h;
                    }
                    for (int _m = 0; _m < 3 && wantm[_m] >= 0; _m++) {
                        off_rt_transition(wantm[_m], D3D12_RESOURCE_STATE_RENDER_TARGET);
                        rh[nrt++] = off_rt_rtv(wantm[_m]);
                    }
                    if (nocol) {
                        nrt = 0;
                        vp.Width  = wz >= 0 ? (float)s_zeta[wz].hw : (float)s_d3d.host_w;
                        vp.Height = wz >= 0 ? (float)s_zeta[wz].hh : (float)s_d3d.host_h;
                        gw = wz >= 0 ? (float)s_zeta[wz].w : (float)s_d3d.width;
                        gh = wz >= 0 ? (float)s_zeta[wz].h : (float)s_d3d.height;
                    }
                    D3D12_RECT sc = {0, 0, (LONG)vp.Width, (LONG)vp.Height};
                    vp_last_w = gw; vp_last_h = gh;
                    tgt_sx = vp.Width / gw; tgt_sy = vp.Height / gh;
                    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, nrt,
                                                               nrt ? rh : NULL, FALSE, &dsv_op);
                    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &vp);
                    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &sc);
                    /* The shared buffer stands in for "no guest depth surface",
                     * so it starts each target fresh; a zeta keeps its content. */
                    if (wz < 0)
                        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, dsv_handle,
                            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
                    cur_z = wz;
                    cur_zro = wro;
                    cur_nocol = nocol;
                    /* RT_CLEARDBG=2: magenta-clear the surface the DRAWS bind,
                     * at the moment they bind it. The guest's own clears may go
                     * to a different surface entirely (they do here: clears to
                     * 0xAB0000, draws to 0x1BE0000), so clearing on the clear
                     * path tests the wrong target. Anything not magenta
                     * afterwards was rasterised. */
                    { static int cd2 = -1;
                      PS3_KNOB(cd2, ps3_env_int("RT_CLEARDBG", 0));
                      if (cd2 >= 2 && want >= 0) {
                          const float mag[4] = {1.0f, 0.0f, 1.0f, 1.0f};
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(
                              s_d3d.cmd_list, off_rt_rtv(want), mag, 0, NULL);
                      } }
                    /* VP_TESTTRI=1: our own triangle, through the game's exact
                     * pipeline -- same vpso, same root signature, same vertex
                     * buffer, same RTV just bound. Written at vertex 1000 so it
                     * cannot collide with the guest's data. Pair with
                     * VP_BYPASS=1 so the shader is a trivial passthrough.
                     *
                     * If this renders, the GPU does read vp_vb and the fault is
                     * in the guest draws' own state. If it does not, nothing
                     * this backend draws reaches a render target at all. */
                    { static int tt = -1;
                      PS3_KNOB(tt, ps3_env_int("VP_TESTTRI", 0));
                      if (tt && want >= 0 && s_d3d.vp_vb_mapped) {
                          float* vtx = (float*)((char*)s_d3d.vp_vb_mapped
                              + (u64)s_d3d.vp_parity * s_vp_cap * 256u
                              + (u64)1000 * 256u);
                          static const float tri[3][4] = {
                              {-2.0f, -2.0f, 0.0f, 1.0f},
                              { 2.0f, -2.0f, 0.0f, 1.0f},
                              { 0.0f,  2.0f, 0.0f, 1.0f} };
                          for (int _v = 0; _v < 3; _v++) {
                              float* dstv = vtx + (size_t)_v * (256u / 4u);
                              dstv[0] = tri[_v][0]; dstv[1] = tri[_v][1];
                              dstv[2] = tri[_v][2]; dstv[3] = tri[_v][3];
                          }
                          s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list, 3, 1, 1000, 0);
                          { static int _n = 0;
                            if (_n < 3 && ++_n) fprintf(stderr, "[TESTTRI] issued on rt=%d off=0x%08X%c",
                                                  want, s_d3d.off_rt[want].off, 10); }
                      } }
                    cur_rt = want;
                    for (int _m = 0; _m < 3; _m++) cur_m[_m] = wantm[_m];
                }
                if (dr->is_clear && (dr->clr_flags & 3u) && cur_z >= 0) {
                    D3D12_CLEAR_FLAGS zf = (D3D12_CLEAR_FLAGS)(
                        ((dr->clr_flags & 1u) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                        ((dr->clr_flags & 2u) ? D3D12_CLEAR_FLAG_STENCIL : 0));
                    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, zeta_dsv(cur_z),
                        zf, dr->clr_z, (UINT8)dr->clr_s, 0, NULL);
                }
                if (dr->is_clear && (!(dr->clr_flags & 0xF0u) || nocol)) continue;   /* depth/stencil only */
                if (dr->is_clear) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rh =
                        (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                    /* RT_CLEARDBG=1: clear offscreen targets to magenta rather
                     * than the guest colour. "0.000% nonzero" cannot tell an
                     * untouched surface from one written black, and that
                     * ambiguity invalidates every read of an offscreen target.
                     * If the surface comes back magenta the clears land and the
                     * draws are writing black; if it stays black, nothing
                     * reaches it at all. */
                    { static int cd = -1;
                      PS3_KNOB(cd, ps3_env_int("RT_CLEARDBG", 0));
                      if (cd && cur_rt >= 0) {
                          const float mag[4] = {1.0f, 0.0f, 1.0f, 1.0f};
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rh, mag, 0, NULL);
                      } else
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rh, dr->cc, 0, NULL); }
                    for (int _m = 0; _m < 3; _m++)
                        if (cur_m[_m] >= 0) {
                            D3D12_CPU_DESCRIPTOR_HANDLE rhm = off_rt_rtv(cur_m[_m]);
                            s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rhm, dr->cc, 0, NULL);
                        }
                    continue;
                }
                /* Per-draw pipeline: prefer the guest's own compiled FP; fall
                 * back to the hardcoded atlas/colour PS pair. The pre-pass
                 * looked it up already and kept it in the record.
                 * RSX_PSO_IN_REC (live): 1 (default) binds the record's,
                 * 0 looks it up again here, 2 does both and reports a
                 * difference. A draw whose pre-pass lookup failed looks
                 * again, as before. */
                static int pso_in_rec = -1;
                PS3_KNOB(pso_in_rec, ps3_env_int("RSX_PSO_IN_REC", 1));
                ID3D12PipelineState* dpso = NULL;
                if (dr->fp_addr) {
                    if (pso_in_rec && dr->pso && dr->pso_serial == s_rf_serial) dpso = dr->pso;
                    if (!dpso || pso_in_rec == 2) {
                        ID3D12PipelineState* look =
                            vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key,
                                          dr_num_rts(dr),
                                          dr->rt_off ? rt_dxgi(dr->rt_off, dr->rt_fmt)
                                                     : DXGI_FORMAT_R8G8B8A8_UNORM,
                                          dr->fp_exp32, dr->cmask, dr->cull, dr->depth,
                                          dr_cube_mask(dr), dr->d2c_mask, dr_uvclamp(dr), dr_hdrv(dr));
                        if (dpso && look != dpso) {
                            static int n = 0;
                            if (n < 8) {
                                n++;
                                fprintf(stderr, "[PSO] draw %u: the record's pipeline differs "
                                                "from a new lookup (frame %llu)\n",
                                        d, (unsigned long long)s_d3d.frame_count);
                            }
                        }
                        dpso = look;
                    }
                }
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list,
                                                         dpso ? dpso : vpso);
                /* Per-draw viewport: the guest rect when sane, else the
                 * full target. Scissor tracks the same rect. */
                {
                    float tw = cur_nocol ? vp_last_w
                             : (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].w : (float)s_d3d.width;
                    float th = cur_nocol ? vp_last_h
                             : (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].h : (float)s_d3d.height;
                    D3D12_VIEWPORT dvp = {0, 0, tw, th, 0.0f, 1.0f};
                    if (dr->vp_w >= 2 && dr->vp_h >= 2 &&
                        (float)(dr->vp_x + dr->vp_w) <= tw + 0.5f &&
                        (float)(dr->vp_y + dr->vp_h) <= th + 0.5f) {
                        dvp.TopLeftX = (float)dr->vp_x;
                        dvp.TopLeftY = (float)dr->vp_y;
                        dvp.Width    = (float)dr->vp_w;
                        dvp.Height   = (float)dr->vp_h;
                    }
                    D3D12_RECT dsc = {(LONG)dvp.TopLeftX, (LONG)dvp.TopLeftY,
                                      (LONG)(dvp.TopLeftX + dvp.Width),
                                      (LONG)(dvp.TopLeftY + dvp.Height)};
                    dvp_x = dvp.TopLeftX; dvp_y = dvp.TopLeftY;
                    dvp_w = dvp.Width;    dvp_h = dvp.Height;
                    if (tgt_sx != 1.0f || tgt_sy != 1.0f) {   /* RSX_SCALE: the guest rect, scaled */
                        dvp.TopLeftX *= tgt_sx; dvp.Width  *= tgt_sx;
                        dvp.TopLeftY *= tgt_sy; dvp.Height *= tgt_sy;
                        dsc.left   = lroundf(dvp.TopLeftX);
                        dsc.top    = lroundf(dvp.TopLeftY);
                        dsc.right  = lroundf(dvp.TopLeftX + dvp.Width);
                        dsc.bottom = lroundf(dvp.TopLeftY + dvp.Height);
                    }
                    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &dvp);
                    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &dsc);
                }
                /* Per-draw textures: bind this draw's SRV window.
                 * Any sampled offscreen RT transitions to PSR first (never
                 * one of the currently-bound colour targets). */
                for (int _u = 0; _u < RSX_FP_UNITS; _u++) {
                    int tz = dr->tex_z[_u];
                    if (tz >= 0) {
                        if (tz == cur_z && cur_zro)   /* bound read-only: readable */
                            ;
                        else if (tz == cur_z) { /* bound as this draw's depth: unreadable */
                            srv_write(DRAW_SRV_BASE + d * RSX_FP_UNITS + (u32)_u, NULL,
                                      DXGI_FORMAT_R8G8B8A8_UNORM,
                                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                            fpwatch_unreadable(dr->fp_addr);
                        }
                        else
                            zeta_transition(tz, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        continue;
                    }
                    int rt = dr->tex_rt[_u];
                    if (rt < 0) continue;
                    if (rt != cur_rt &&
                        rt != cur_m[0] && rt != cur_m[1] && rt != cur_m[2]) {
                        off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        continue;
                    }
                    /* Sampling a bound colour target: read a copy (above). */
                    ID3D12Resource* snap = off_rt_snapshot(rt);
                    srv_write(DRAW_SRV_BASE + d * RSX_FP_UNITS + (u32)_u, snap,
                              (DXGI_FORMAT)s_d3d.off_rt[rt].dxgi,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                }
                D3D12_GPU_DESCRIPTOR_HANDLE gh = gh_base;
                gh.ptr += (u64)(DRAW_SRV_BASE + d * RSX_FP_UNITS) * s_d3d.srv_inc;
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
                vp_bind_sampler_table(dr);
                /* Per-draw constants: this draw's vp_cb + FP texscale slots. */
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                    s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb)
                    + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->vcb_slot) * VP_CB_STRIDE);
                if (s_d3d.vp_fpcb)
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 2,
                        s_d3d.vp_fpcb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_fpcb)
                        + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->cb_slot) * VP_FPCB_STRIDE);
                /* DRAWARGS=<N>: the arguments actually handed to the GPU, plus
                 * the target they are aimed at. A zero vertex count, or a start
                 * vertex past the buffer, rasterises nothing while every other
                 * check upstream still says the draw is fine. */
                { static int dcap = -1, dn = 0;
                  PS3_KNOB(dcap, ps3_env_int("DRAWARGS", 0));
                  if (dcap && dn < dcap) { dn++;
                      D3D12_CPU_DESCRIPTOR_HANDLE _rh =
                          (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                      fprintf(stderr, "[DRAWARGS] verts=%u start=%u rt=%d rec=0x%08X bound=0x%08X"
                              " tex0=0x%08X texrt=%d dvp=%.0f,%.0f %.0fx%.0f%c",
                              dr->vertex_count, dr->vb_byte_offset / 256, cur_rt,
                              dr->rt_off,
                              (cur_rt >= 0) ? s_d3d.off_rt[cur_rt].off : 0u,
                              dr->tex[0].raw, dr->tex_rt[0],
                              (double)dvp_x, (double)dvp_y,
                              (double)dvp_w, (double)dvp_h, 10); } }
                /* RT_REBIND=1: re-issue OMSetRenderTargets immediately before
                 * every draw instead of only when the target changes. A clear
                 * proves the RTV descriptor is good but not that the binding is
                 * still live at draw time -- ClearRenderTargetView writes
                 * through the descriptor and never consults the OM. If anything
                 * between the retarget and here drops the binding, this restores
                 * it. */
                { static int rb = -1;
                  PS3_KNOB(rb, ps3_env_int("RT_REBIND", 0));
                  if (rb) {
                      D3D12_CPU_DESCRIPTOR_HANDLE brh[4];
                      UINT bn = 1;
                      brh[0] = (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                      for (int _m = 0; _m < 3; _m++)
                          if (cur_m[_m] >= 0) brh[bn++] = off_rt_rtv(cur_m[_m]);
                      s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(
                          s_d3d.cmd_list, bn, brh, FALSE, &dsv_handle);
                  } }
                { int zq = -1;
                  if (g_rsx_report_hook && (s_zq_nev || s_zq_on_start) &&
                      s_zq_n < ZQ_MAX && zq_counted(dr->cb_slot) && zq_ready()) {
                      zq = s_zq_n++;
                      s_zq_rec[zq] = dr->cb_slot;
                      s_zq_area[zq] = tgt_sx * tgt_sy;
                      s_d3d.cmd_list->lpVtbl->BeginQuery(s_d3d.cmd_list, s_zq_heap,
                          D3D12_QUERY_TYPE_OCCLUSION, (UINT)zq);
                  }
                if (dr->fetch)
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRoot32BitConstant(s_d3d.cmd_list, 5,
                        dr->fdesc, 0);
                /* A strip draw's indices are a strip (vf_strip_native). */
                if ((dr->fetch == 2) != s_vp_topo_strip) {
                    s_vp_topo_strip = dr->fetch == 2;
                    s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list,
                        s_vp_topo_strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                        : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                }
                dred_mark(s_d3d.cmd_list, d, dr);
                if (dr->index_count)
                    s_d3d.cmd_list->lpVtbl->DrawIndexedInstanced(s_d3d.cmd_list,
                        dr->index_count, hang_test_instances(dr->index_count), dr->ib_first,
                        (INT)(dr->vb_byte_offset / 256), 0);
                else
                    s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list,
                        dr->vertex_count, 1, dr->vb_byte_offset / 256, 0);
                  if (zq >= 0)
                      s_d3d.cmd_list->lpVtbl->EndQuery(s_d3d.cmd_list, s_zq_heap,
                          D3D12_QUERY_TYPE_OCCLUSION, (UINT)zq); }
                /* VP_SUBMIT=<N>: prove the VP pass actually reaches the GPU.
                 * "records exist" and "draws were submitted" are different
                 * claims, and every blank-output investigation conflates them. */
                { static int cap = -1, n = 0;
                  PS3_KNOB(cap, ps3_env_int("VP_SUBMIT", 0));
                  if (cap && n < cap) { n++;
                    fprintf(stderr, "[VPSUBMIT] draw[%u] verts=%u pso=%s vp=%ux%u tex0=0x%X(set=%d) tex1=0x%X slot=%d fp=0x%X%c",
                            d, dr->vertex_count, dpso ? "guest-fp" : "fallback",
                            dr->vp_w, dr->vp_h, dr->tex[0].raw, dr->tex[0].set,
                            dr->tex[1].raw, dr->tex_slot, dr->fp_addr, 10);
                    fprintf(stderr, "            vs_idx=%d cb_slot=%u vbofs=%u cull=%u blend=%d%c",
                            dr->vs_idx, dr->cb_slot, dr->vb_byte_offset / 256,
                            dr->cull, dr->blend, 10); } }
            }
            if (perf_on()) s_perf_gpu += perf_now() - _rec0;   /* reuse: record time */
            fpwatch_batch();
            /* Leave the backbuffer bound for the dump/present epilogue. */
            if (cur_rt >= 0) {
                s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
                D3D12_VIEWPORT vp = {0, 0, (float)s_d3d.host_w, (float)s_d3d.host_h, 0.0f, 1.0f};
                D3D12_RECT sc = {0, 0, (LONG)s_d3d.host_w, (LONG)s_d3d.host_h};
                s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &vp);
                s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &sc);
            }
        }
    }

    RF_CP(6);
    if (perf_on()) s_perf_rf += perf_now() - _rf0;
    /* MEMPEEK=<hex addr>:<n>: n big-endian floats at a guest address each frame,
     * with a min/max so a buffer of constants is distinguishable from real data. */
    { const char* mp = getenv("MEMPEEK");
      if (mp) { extern uint8_t* vm_base; static int _n = 0;
        u32 addr = 0; int cnt = 8; sscanf(mp, "%x:%d", &addr, &cnt);
        if (vm_base && addr && _n < 6 && ++_n) {
            float lo = 1e30f, hi = -1e30f; u32 nz = 0;
            for (int i = 0; i < cnt; i++) {
                const u8* q = vm_base + addr + i * 4;
                u32 bits = ((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3];
                float f; memcpy(&f, &bits, 4);
                if (f == f && f < 1e30f && f > -1e30f) { if (f < lo) lo = f; if (f > hi) hi = f; }
                if (bits) nz++;
            }
            fprintf(stderr, "[MEMPEEK] 0x%08X n=%d nonzero=%u range[%g..%g]%c",
                    addr, cnt, nz, lo, hi, 10);
        } } }
    /* VRAMSCAN=1: which 64 KB blocks of guest memory change between frames.
     * "nothing writes the buffer we read" is only half an answer -- this says
     * where the producer IS writing, so the two can be matched up. */
    { static int vs = -1; PS3_KNOB(vs, getenv("VRAMSCAN") ? 1 : 0);
      if (vs) { extern uint8_t* vm_base;
        static u32 prev[512]; static int have = 0; static int reported = 0;
        u32 base = 0xC6000000u; u32 nblk = 512;          /* 32 MB window */
        const char* bs = getenv("VRAMSCAN_BASE");
        if (bs) base = (u32)strtoul(bs, NULL, 0);
        if (vm_base && reported < 6) {
            u32 changed = 0; char list[512]; int ln = 0;
            for (u32 b = 0; b < nblk; b++) {
                const u8* p = vm_base + base + b * 0x10000u;
                u32 h = 2166136261u;
                for (u32 i = 0; i < 0x10000u; i += 61) h = (h ^ p[i]) * 16777619u;
                if (have && h != prev[b]) { changed++;
                    if (ln < 400) ln += snprintf(list + ln, sizeof(list) - ln,
                                                 " +0x%X", b * 0x10000u); }
                prev[b] = h;
            }
            if (have && changed) { reported++;
                fprintf(stderr, "[VRAMSCAN] base=0x%08X %u/512 blocks changed:%s%c",
                        base, changed, list, 10); }
            have = 1;
        } } }
    /* FRAME_BUDGET=1: geometry a frame asked for vs what the batch can hold.
     * Reported from render_frame so it works under RSX_ACCUM_FRAME too -- it
     * used to live in the clear-boundary present path, which accum bypasses, so
     * it silently produced nothing in exactly the configuration being used. */
    { static int fb = -1;
      PS3_KNOB(fb, ps3_env_int("FRAME_BUDGET", 0));
      if (fb && s_req_draws) { static int n = 0; if (n < 40 && ++n)
        fprintf(stderr, "[BUDGET] frame: requested %llu verts / %llu draws, "
                        "buffer holds %u verts, %llu draws truncated%c",
                (unsigned long long)s_req_verts, (unsigned long long)s_req_draws,
                s_vp_cap, (unsigned long long)s_drop_draws, 10); }
      s_req_verts = 0; s_req_draws = 0; s_drop_draws = 0; }
    if (perf_on()) {
        static double s_t_prev = 0.0; static int s_n = 0;
        double now = perf_now();
        if (s_t_prev > 0.0) s_perf_frame += now - s_t_prev;
        s_t_prev = now;
        if (++s_n % 20 == 0) {
            double fr = s_perf_frame > 0 ? s_perf_frame : 1;
            gpu_sc_stats sc;   /* the shader compiles: the compile service's counters */
            gpu_sc_stats_take(&sc, 1);
            if (perf_print()) fprintf(stderr, "[PERF] %.2f fps | tex %.2fs (%.0f%%, %d calls) | vtx %.2fs"
                            " (%.0f%%, %.0fk verts) | render_frame %.2fs (%.0f%%)"
                            " | prepass %.2fs (%.0f%%) [srv %.2fs %.0f%% | pso %.2fs %.0f%% %d calls %d MISS %dKB hashed] | guest %.2fs (%.0f%%)"
                            " [vs compile %.2fs, %d] [fxc %.2fs %d, disk %d | create pso %.2fs %d | misses %.2fs]%c",
                    20.0 / fr,
                    s_perf_tex, 100.0 * s_perf_tex / fr, s_perf_ntex,
                    s_perf_vtx, 100.0 * s_perf_vtx / fr,
                    (double)s_perf_nverts / 1000.0,
                    s_perf_rf, 100.0 * s_perf_rf / fr,
                    s_perf_pre, 100.0 * s_perf_pre / fr,
                    s_perf_srv, 100.0 * s_perf_srv / fr,
                    s_perf_pso, 100.0 * s_perf_pso / fr,
                    s_perf_pso_calls, s_perf_pso_miss, s_perf_pso_hashbytes / 1024,
                    fr - s_perf_rf, 100.0 * (fr - s_perf_rf) / fr,
                    s_perf_vs, s_perf_vs_compiles,
                    sc.compile_s, sc.compiles, sc.cache_hits, s_perf_cpso, s_perf_cpso_n, s_perf_pmiss, 10);
            s_perf_frame = 0.0; s_perf_tex = 0.0; s_perf_vtx = 0.0; s_perf_rf = 0.0;
            s_perf_gpu = 0.0; s_perf_pre = 0.0; s_perf_srv = 0.0; s_perf_pso = 0.0;
            s_perf_pso_calls = 0; s_perf_pso_miss = 0; s_perf_pso_hashbytes = 0;
            s_perf_vs_compiles = 0; s_perf_vs = 0.0;
            s_perf_cpso = 0.0; s_perf_cpso_n = 0; s_perf_pmiss = 0.0;
            s_perf_ntex = 0; s_perf_texbytes = 0; s_perf_nverts = 0;
        }
    }
    { static int dt = -1;
      PS3_KNOB(dt, ps3_env_int("RSX_DBG_TRACK", 0));
      if (dt) { static int fr = 0; fr++;
        if (s_lock_n)
            fprintf(stderr, "[RSX_DBG_TRACK] frame %d: %u duck draws, ndc=(%.3f,%.3f)%c",
                    fr, s_lock_n, (float)(s_lock_sx/s_lock_n), (float)(s_lock_sy/s_lock_n), 10);
        else if (fr % 20 == 0)
            fprintf(stderr, "[RSX_DBG_TRACK] frame %d: NO duck draws%c", fr, 10);
      } }
    if (s_lock_n) {                       /* publish this frame's centroid */
        s_lock_x = (float)(s_lock_sx / s_lock_n);
        s_lock_y = (float)(s_lock_sy / s_lock_n);
        s_lock_valid = 1;
        s_lock_sx = s_lock_sy = 0.0; s_lock_n = 0;
    }
    s_dbg_last_draws = s_d3d.draw_count;
    vpj_sync();           /* nothing may still write the half being reset */
    s_d3d.vb_offset  = 0; /* reset for next frame */
    s_d3d.vp_vb_offset = 0;
    s_d3d.draw_count = 0;
    s_d3d.vp_parity ^= 1;  /* next frame's records go to the other half */
    s_vf_off = 0;
    s_fx.valid = 0;
    { static int vst = -1; static u32 fr = 0; static u64 emit = 0;
      PS3_KNOB(vst, ps3_env_int("VP_VSTAT", 0));
      emit += s_vp_want;
      if (vst > 0 && ++fr >= (u32)vst) {
          fprintf(stderr, "[VSTAT] per frame: fetched %llu emitted %llu (%.1f MB) attrs/fetch %.2f"
                  " | uploads quad %llu tri %llu strip %llu fan %llu%c",
                  (unsigned long long)(s_vs_fetch / fr), (unsigned long long)(emit / fr),
                  (double)emit / fr * 256.0 / 1048576.0,
                  s_vs_fetch ? (double)s_vs_attr / s_vs_fetch : 0.0,
                  (unsigned long long)(s_vs_up[0] / fr), (unsigned long long)(s_vs_up[1] / fr),
                  (unsigned long long)(s_vs_up[2] / fr), (unsigned long long)(s_vs_up[3] / fr), 10);
          fr = 0; emit = 0; s_vs_fetch = s_vs_attr = 0; memset(s_vs_up, 0, sizeof s_vs_up);
      } }
    if (s_vp_want > s_vp_cap) vp_vb_grow(s_vp_want);
    s_vp_want = 0;
    s_d3d.vp_ib_offset = 0;
    s_vcb_last = ~0u;   /* the next frame's banks go to the other half */
    if (!s_d3d.vp_ib || s_vp_iwant > s_vp_icap) vp_ib_grow(s_vp_iwant);
    s_vp_iwant = 0;

    /* The fragment-program debug rewrites (FP_SHOW, FP_TEX, FP_ONE, FP_KILL,
     * FP_IDCOLOR, FP_TEX_FP) apply when a program is compiled, and a program
     * compiles once. When any of them changes (the console's setenv), bump the
     * pipeline generation so every program rebuilds with the new value: one
     * live run can then look at register after register. */
    { static char last[512] = ""; static int init = 0;
      static const char* const knobs[] = { "FP_SHOW", "FP_TEX", "FP_ONE", "FP_KILL",
                                           "FP_IDCOLOR", "FP_TEX_FP" };
      char cur[512]; size_t at = 0;
      cur[0] = 0;
      for (int k = 0; k < 6; k++) {
          const char* v = getenv(knobs[k]);
          int w = snprintf(cur + at, sizeof cur - at, "%s=%s;", knobs[k], v ? v : "");
          if (w > 0 && (size_t)w < sizeof cur - at) at += (size_t)w;
      }
      if (init && strcmp(cur, last)) {
          s_d3d.vp_gen++;
          fprintf(stderr, "[FP] debug knobs changed (%s): rebuilding pipelines%c", cur, 10);
      }
      init = 1;
      memcpy(last, cur, sizeof last);
    }

    /* Debug: RTT_SAVERT=<hex raw offset>[:frame] copies that offscreen RT
     * into a readback buffer this frame and writes rt_save_<offset>.bmp
     * (half-float RTs are tonemapped |v| -> byte). A new value re-arms it, so
     * the console's setenv can save one target after another in a live run. */
    static ID3D12Resource* s_rtsave_buf = NULL;
    static u64 s_rtsave_cap = 0;
    static u32 s_rtsave_state = 0;   /* 1 = copy queued this frame */
    static u32 s_rtsave_w, s_rtsave_h, s_rtsave_pitch, s_rtsave_dxgi, s_rtsave_off;
    { const char* sv = getenv("RTT_SAVERT");
      static int _done = 0;
      static char _last[32] = "";
      if (sv && strncmp(sv, _last, sizeof _last - 1) != 0) {
          strncpy(_last, sv, sizeof _last - 1); _done = 0; }
      static u32 _skip = 0xFFFFFFFFu;
      if (_skip == 0xFFFFFFFFu) { const char* sk = getenv("RTT_SAVERT_SKIP");
          _skip = sk ? (u32)strtoul(sk, NULL, 0) : 0; }   /* frame 60 is far too
          early for a title that takes a minute to reach its menu */
      /* RTT_SAVERT_AFTER=<seconds> triggers on wall time instead of a frame
       * count. This title runs its menu at a few frames a second, so a frame
       * number is not a usable way to say "once it has settled". */
      static u32 _after = 0xFFFFFFFFu;
      if (_after == 0xFFFFFFFFu) { const char* sa = getenv("RTT_SAVERT_AFTER");
          _after = sa ? (u32)strtoul(sa, NULL, 0) : 0; }
      static clock_t _t0 = 0;
      if (!_t0) _t0 = clock();
      const int _due = _after
          ? ((u32)((clock() - _t0) / CLOCKS_PER_SEC) >= _after)
          : (s_d3d.frame_count > 60 + _skip);
      { static int _p1 = 0;
        if (sv && _due && !_p1) { _p1 = 1;
            printf("[RTT_SAVERT] due: want=0x%lX found_rt=%d frame=%u\n",
                   strtoul(sv, NULL, 16), off_rt_find((u32)strtoul(sv, NULL, 16)),
                   (unsigned)s_d3d.frame_count); fflush(stdout); } }
      if (sv && !_done && _due) {
        int rt = off_rt_find((u32)strtoul(sv, NULL, 16));
        fprintf(stderr, "[RTT_SAVERT] rt=%d res=%p\n", rt, rt >= 0 ? (void*)s_d3d.off_rt[rt].res : NULL);
        if (rt >= 0 && s_d3d.off_rt[rt].res) {
            OffRT* r = &s_d3d.off_rt[rt];
            u32 bpp = (r->dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
                      (r->dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 : 4;
            u32 pitch = (r->hw * bpp + 255) & ~255u;
            fprintf(stderr, "[RTT_SAVERT] rt=%d w=%u h=%u bpp=%u pitch=%u dxgi=%d bytes=%llu\n",
                    rt, r->hw, r->hh, bpp, pitch, (int)r->dxgi,
                    (unsigned long long)((u64)pitch * r->hh));
            if (s_rtsave_buf && s_rtsave_cap < (u64)pitch * r->hh) {
                s_rtsave_buf->lpVtbl->Release(s_rtsave_buf); s_rtsave_buf = NULL; }
            if (!s_rtsave_buf) {
                s_rtsave_cap = (u64)pitch * r->hh;
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC rd = {0};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = (u64)pitch * r->hh; rd.Height = 1; rd.DepthOrArraySize = 1;
                rd.MipLevels = 1; rd.SampleDesc.Count = 1;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                d3d_create_committed(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                    &IID_ID3D12Resource, (void**)&s_rtsave_buf);
            }
            if (!s_rtsave_buf) fprintf(stderr, "[RTT_SAVERT] readback buffer alloc FAILED\n");
            if (s_rtsave_buf) {
                off_rt_transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                cdst.pResource = s_rtsave_buf;
                cdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                cdst.PlacedFootprint.Footprint.Format   = (DXGI_FORMAT)r->dxgi;
                cdst.PlacedFootprint.Footprint.Width    = r->hw;
                cdst.PlacedFootprint.Footprint.Height   = r->hh;
                cdst.PlacedFootprint.Footprint.Depth    = 1;
                cdst.PlacedFootprint.Footprint.RowPitch = pitch;
                csrc.pResource = r->res;
                csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                s_rtsave_state = 1;
                s_rtsave_w = r->hw; s_rtsave_h = r->hh;
                s_rtsave_pitch = pitch; s_rtsave_dxgi = r->dxgi;
                s_rtsave_off = (u32)strtoul(sv, NULL, 16);
                fprintf(stderr, "[RTT_SAVERT] copy queued: rt=%d %ux%u pitch=%u\n",
                        rt, r->hw, r->hh, pitch);
                _done = 1;
            }
        }
      } }

    /* CELLMARK_DUMP_MINDRAWS=<N>: only consider frames carrying at least N draw
     * records. Most render_frame calls come from the guest flip with a batch the
     * clear boundary already drained, so they present an empty backbuffer -- and
     * sampling those wastes every dump on a black image while the frames that do
     * hold the scene go uncaptured. */
    { static int mind = -1;
      if (mind < 0) { const char* e = getenv("CELLMARK_DUMP_MINDRAWS");
                      mind = e ? atoi(e) : 0; }
      /* draw_count is already reset by here; s_dbg_last_draws holds the
       * count this frame actually rendered. */
      if (mind > 0 && s_dbg_last_draws < (u32)mind) goto skip_dump_consider; }
    if (s_d3d.dump_skip_left > 0 && s_d3d.dump_frames_left > 0) s_d3d.dump_skip_left--;
skip_dump_consider: ;
    /* CELLMARK_DUMP_NOW=<n> (live): dump the next n presents, every one, from
     * the console -- the frames around a flicker, once the game is showing it.
     * CELLMARK_DUMP is read at init, and its skip count cannot know when a
     * hand-driven run reaches the spot. Each dump line gives the batch's draws. */
    /* An F9 capture started: save the presents that follow it as
     * cap_NNN_live_XX_d<draws>.bmp, RSX_HOTKEY_LIVE of them (default 4, 0 =
     * off). A replay rebuilds one batch from guest memory with no targets
     * from earlier frames and no flips; these show what was on screen. */
    { extern volatile long g_rcap_live_arm; extern char g_rcap_live_name[32];
      static long seen;
      if (g_rcap_live_arm != seen) {
          seen = g_rcap_live_arm;
          const int n = ps3_env_int("RSX_HOTKEY_LIVE", 4);
          if (n > 0 && s_hotshot_left <= 0) {
              snprintf(s_hotshot_name, sizeof s_hotshot_name, "%s", g_rcap_live_name);
              s_hotshot_n = 0;
              s_hotshot_left = n;
              s_d3d.dump_frames_left = n; s_d3d.dump_skip_left = 0;
              const char* hd = getenv("RSX_HOTKEY_DIR");
              if (!hd || !*hd) hd = "rsx_hotkey";   /* as F9 (cellGcmSys.c) */
              snprintf(s_dp_prefix, sizeof s_dp_prefix, "%s/%s_draws", hd, g_rcap_live_name);
              s_dp_left = n; s_dp_n = 0;
          }
      } }
    if (s_hotshot_arm) {   /* F10, armed from the window procedure */
        s_hotshot_arm = 0;
        s_hotshot_name[0] = 0;
        const int n = ps3_env_int("RSX_HOTKEY_SHOTS", 12);
        const char* hd = getenv("RSX_HOTKEY_DIR");
        if (!hd || !*hd) hd = "rsx_hotkey";   /* as F9 (cellGcmSys.c) */
        CreateDirectoryA(hd, NULL);
        char probe[512];
        do {   /* the first burst number not already on disk */
            s_hotshot_burst++;
            snprintf(probe, sizeof probe, "%s%sshot_%03d_00_d*.bmp", hd ? hd : "", hd ? "/" : "",
                     s_hotshot_burst);
#ifdef _WIN32
            WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA(probe, &fd);
            if (fh == INVALID_HANDLE_VALUE) break;
            FindClose(fh);
#else
            glob_t g;
            const int found = glob(probe, 0, NULL, &g) == 0 && g.gl_pathc > 0;
            globfree(&g);
            if (!found) break;
#endif
        } while (s_hotshot_burst < 999);
        s_hotshot_n = 0;
        s_hotshot_left = n > 0 ? n : 12;
        s_d3d.dump_frames_left = s_hotshot_left; s_d3d.dump_skip_left = 0;
        printf("[F10] dumping the next %d presents as shot_%03d_*%c", s_hotshot_left,
               s_hotshot_burst, 10);
    }
    { static unsigned gen; static int last;
      if (gen != ps3_env_gen) { gen = ps3_env_gen;
          const int v = ps3_env_int("CELLMARK_DUMP_NOW", 0);
          if (v != last) { last = v;
              if (v > 0) { s_d3d.dump_frames_left = v; s_d3d.dump_skip_left = 0; } } } }
    guest_fb_present(fi);
    composite_present(fi);
    if (!aa_mode() || !knob_aa_at_present())
        s_aa.present_logged = 0;
    else if (s_present_this_frame) {
        if (s_aa.present_logged != aa_mode() && knob_aa_log() > 0)
            printf("[AA] %s on the finished frame, HUD included (RSX_AA_AT=present)\n",
                   aa_mode() == 2 ? "SMAA" : "FXAA");
        s_aa.present_logged = aa_mode();
        aa_run_display(fi, rtv_handle);
    }
    int dumping = (s_d3d.dump_frames_left > 0 && s_d3d.readback_buf
                   && s_d3d.dump_skip_left == 0);
    { static int mind2 = -1;
      if (mind2 < 0) { const char* e = getenv("CELLMARK_DUMP_MINDRAWS");
                       mind2 = e ? atoi(e) : 0; }
      if (mind2 > 0 && s_dbg_last_draws < (u32)mind2) dumping = 0; }
    /* CELLMARK_DUMP_EVERY=N: after each dump, skip N-1 frames -- samples the whole
     * run instead of one contiguous window, so a short burst of real content
     * can't fall between the dumped frames. */
    if (dumping) {
        static int s_every = -1;
        if (s_every < 0) { const char* e = getenv("CELLMARK_DUMP_EVERY");
                           s_every = e ? atoi(e) : 0; }
        if (s_every > 1) s_d3d.dump_skip_left = s_every - 1;
    }
    if (dumping) {
        /* RT -> COPY_SOURCE, copy into the readback buffer, then -> PRESENT. */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_d3d.readback_buf;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width    = s_d3d.host_w;
        dst.PlacedFootprint.Footprint.Height   = s_d3d.host_h;
        dst.PlacedFootprint.Footprint.Depth    = 1;
        dst.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
        src.pResource = s_d3d.render_targets[fi];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
    } else {
        /* End-of-frame fallback snapshot: used when the frame never contained a
         * reduced-viewport pass to capture mid-frame. */
        screen_copy_capture(fi);
        /* Transition render target to PRESENT state */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
    }

    RF_CP(7);
    rt_writeback_record();
    if (s_zq_n)
        s_d3d.cmd_list->lpVtbl->ResolveQueryData(s_d3d.cmd_list, s_zq_heap,
            D3D12_QUERY_TYPE_OCCLUSION, 0, (UINT)s_zq_n, s_zq_rb, 0);

    /* The game's frame, scaled into the window's buffer (and held for the
     * overlay's repaints). */
    if (s_present_this_frame) {
        hold_frame(fi);
        blit_to_window(fi);
    }

    /* Close and execute */
    frame_cmd_submit();

    dred_report("frame submit");
    if (write_color_on() && write_color_defer()) {
        s_wb_pending = 1;
    } else if (write_color_on()) {
        double _g = perf_on() ? perf_now() : 0.0;
        wait_for_gpu();
        dred_report("write-back wait");
        if (perf_on()) s_perf_gpu += perf_now() - _g;
        rt_writeback_store();
    }
    if (s_zq_n && (!write_color_on() || write_color_defer())) wait_for_gpu();
    zq_finish();

    /* D3D12_IQ=1: drain the debug layer's message queue after submitting the
     * frame. The legacy (is_vp=0) DrawInstanced path silently produced ZERO
     * pixels in flOw's render injection while ClearRenderTargetView worked, and
     * every input (verts, offsets, layout, MVP, PSO, root sig, depth, cull) was
     * verified correct -- so the only remaining explanation is a validation
     * error the debug layer is swallowing. Off unless the env var is set. */
    { static int _iq = -1;
      if (_iq < 0) { const char* e = getenv("D3D12_IQ"); _iq = e ? 1 : 0; }
      if (_iq && s_d3d.device) {
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            static int _printed = 0;
            for (UINT64 mi = 0; mi < n && _printed < 80; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len))) {
                    fprintf(stderr, "[D3D12-IQ][sev=%d id=%d] %s\n",
                            (int)m->Severity, (int)m->ID, m->pDescription);
                    _printed++;
                }
                free(m);
            }
            if (n) iq->lpVtbl->ClearStoredMessages(iq);
            iq->lpVtbl->Release(iq);
            fflush(stderr);
        }
      } }

    if (s_sc_dump_pending) {
        s_sc_dump_pending = 0;
        wait_for_gpu();
        s_dump_name = "screencopy";
        dump_backbuffer_bmp();
        s_dump_name = NULL;
    }
    if (dumping) {
        wait_for_gpu();            /* ensure the copy finished before mapping */
        dump_backbuffer_bmp();
        s_d3d.dump_frames_left--;
    }

    if (s_rtsave_state) {
        char _rtname[64];
        snprintf(_rtname, sizeof _rtname, "rt_save_%08X.bmp", s_rtsave_off);
        fprintf(stderr, "[RTT_SAVERT] writing %s %ux%u\n", _rtname, s_rtsave_w, s_rtsave_h);
        if (!dumping) { double _g = perf_on() ? perf_now() : 0.0;
                        wait_for_gpu();
                        if (perf_on()) s_perf_gpu += perf_now() - _g; }
        void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)s_rtsave_pitch * s_rtsave_h};
        if (SUCCEEDED(s_rtsave_buf->lpVtbl->Map(s_rtsave_buf, 0, &rr, &mp)) && mp) {
            FILE* f = fopen(_rtname, "wb");
            if (f) {
                u32 w = s_rtsave_w, h = s_rtsave_h;
                u32 rowb = (w * 3 + 3) & ~3u;
                u32 datasz = rowb * h;
                u8 hdr[54] = {0};
                hdr[0]='B'; hdr[1]='M';
                *(u32*)(hdr+2) = 54 + datasz; *(u32*)(hdr+10) = 54;
                *(u32*)(hdr+14) = 40; *(int*)(hdr+18) = (int)w; *(int*)(hdr+22) = (int)h;
                *(u16*)(hdr+26) = 1; *(u16*)(hdr+28) = 24; *(u32*)(hdr+34) = datasz;
                fwrite(hdr, 1, 54, f);
                u8* line = (u8*)malloc(rowb);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = (const u8*)mp + (u64)y * s_rtsave_pitch;
                    memset(line, 0, rowb);
                    for (u32 x = 0; x < w; x++) {
                        float rv, gv, bv;
                        if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                            /* crude half->float: sign|exp|mant */
                            float v[3];
                            for (int c2 = 0; c2 < 3; c2++) {
                                u16 hv = hp16[c2];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                float fv;
                                if (exp == 0) fv = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&fv, &fb, 4); }
                                v[c2] = fv;
                            }
                            rv = v[0]; gv = v[1]; bv = v[2];
                        } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                            const float* fp32 = (const float*)(srow + (u64)x * 16);
                            rv = fp32[0]; gv = fp32[1]; bv = fp32[2];
                        } else {
                            const u8* p8 = srow + (u64)x * 4;
                            rv = p8[0] / 255.0f; gv = p8[1] / 255.0f; bv = p8[2] / 255.0f;
                        }
                        /* RTT_SAVEA=1: show the alpha lane in RED (gates
                         * like wave's mask.w live there). */
                        if (getenv("RTT_SAVEA")) {
                            float av;
                            if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                                const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                                u16 hv = hp16[3];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                if (exp == 0) av = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&av, &fb, 4); }
                            } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                                av = ((const float*)(srow + (u64)x * 16))[3];
                            } else {
                                av = (srow + (u64)x * 4)[3] / 255.0f;
                            }
                            rv = av;
                        }
                        /* |v| tonemap so signed heights are visible */
                        float ar = rv < 0 ? -rv : rv, ag = gv < 0 ? -gv : gv, ab = bv < 0 ? -bv : bv;
                        if (ar > 1) ar = 1; if (ag > 1) ag = 1; if (ab > 1) ab = 1;
                        line[x*3+0] = (u8)(ab * 255.0f);
                        line[x*3+1] = (u8)(ag * 255.0f);
                        line[x*3+2] = (u8)(ar * 255.0f);
                    }
                    fwrite(line, 1, rowb, f);
                }
                free(line);
                fclose(f);
                printf("[D3D12] wrote %s (%ux%u dxgi=%u)\n", _rtname, w, h, s_rtsave_dxgi);
            }
            s_rtsave_buf->lpVtbl->Unmap(s_rtsave_buf, 0, NULL);
        }
        s_rtsave_state = 0;
    }

    /* rsxcap: snapshot whole frames (backbuffer + every offscreen RT + a draw
     * manifest) at RSX_CAP=start[:count[:stride]] frame_counts. Runs before the
     * present transition would matter -- the backbuffer is in PRESENT here and
     * is restored to PRESENT after each readback. */
    { const char* cap = getenv("RSX_CAP");
      if (cap) {
        /* Re-parsed whenever the value changes, so the console can arm one
         * capture after another in a running game (`setenv RSX_CAP=1:3`,
         * then a different RSX_CAP_DIR and `RSX_CAP=2:3`); a start below the
         * current frame count captures at once. */
        static char s_cap_last[64];
        static u32 s_cap_start = 0, s_cap_count = 1, s_cap_stride = 1, s_cap_done = 0;
        if (strncmp(cap, s_cap_last, sizeof s_cap_last - 1) != 0) {
            strncpy(s_cap_last, cap, sizeof s_cap_last - 1);
            s_cap_done = 0;
            u32 a = 0, b = 1, c = 1;
            int n = sscanf(cap, "%u:%u:%u", &a, &b, &c);
            s_cap_start  = (n >= 1 && a) ? a : 200;
            s_cap_count  = (n >= 2 && b) ? b : 1;
            s_cap_stride = (n >= 3 && c) ? c : 1;
        }
        if (s_cap_done < s_cap_count) {
            u32 target = s_cap_start + s_cap_done * s_cap_stride;
            if (s_d3d.frame_count >= target && s_dbg_last_draws > 0) {
                rsx_capture_frame(fi, s_dbg_last_draws, s_cap_done);
                s_cap_done++;
            }
        }
      } }

    { static char last[400];   /* RSX_HDR_SNAP (see HDR output) */
      const char* e = getenv("RSX_HDR_SNAP");
      if (e && *e && strcmp(e, last) && s_present_this_frame && s_hdr.active) {
          snprintf(last, sizeof last, "%s", e);
          s_cap_raw = 1;
          cap_readback_write(s_d3d.swap_buffers[fi], s_d3d.win_w, s_d3d.win_h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             D3D12_RESOURCE_STATE_PRESENT, fi, e);
          s_cap_raw = 0;
      } }

    RF_CP(8);
    /* Present */
    { LARGE_INTEGER _pq0, _pq1;
      QueryPerformanceCounter(&_pq0);
      if (s_present_this_frame && s_overlay_on && s_overlay.frame) {
          if (s_overlay_wake) InterlockedExchange(&s_overlay_wake, 0);   /* (this call is the wake) */
          s_overlay.frame(s_overlay.user, s_d3d.swap_buffers[s_d3d.frame_index], s_d3d.frame_index,
                          s_d3d.win_w, s_d3d.win_h);
      }
      if (s_present_this_frame && knob_present() && !s_session_locked)   /* skipped for an offscreen-only batch */
          swap_present(1);
      QueryPerformanceCounter(&_pq1);
      if (s_present_this_frame) frame_log(_pq0.QuadPart, _pq1.QuadPart);
      if (s_present_this_frame) pso_lib_tick();
      RSX_MARK("present", s_present_this_frame, 0); }
    RF_CP(9); rf_slow_report();
    if (s_present_this_frame) prof_record(s_rf_cp[9]);

    move_to_next_frame();
    swap_resize();

    s_d3d.frame_count++;

    /* RPCS3-style titlebar stats, refreshed once a second: presented FPS,
     * draw count of the last frame, and the backbuffer size. */
    {
        static ULONGLONG s_tt0 = 0;
        static u64 s_tframes = 0;
        s_tframes++;
        ULONGLONG tnow = GetTickCount64();
        if (s_tt0 == 0) s_tt0 = tnow;
        if (tnow - s_tt0 >= 1000 && s_d3d.hwnd) {
            extern char g_rsx_title_base[128];
            char tb[256];
            snprintf(tb, sizeof(tb), "%s | FPS: %.2f | draws: %u | %ux%u",
                     g_rsx_title_base,
                     s_tframes * 1000.0 / (double)(tnow - s_tt0),
                     s_dbg_last_draws, s_d3d.width, s_d3d.height);
#ifdef _WIN32
            SetWindowTextA(s_d3d.hwnd, tb);
#else
            win_set_title(tb);
#endif
            /* GCM_RATES=1: the same once-a-second sample on stderr, beside
             * the FIFO flip words decoded and how far RSX label 255 moved --
             * the fence a title like Demon's Souls paces its frames on. */
            { static int on = -1; static u32 fw0, l0, fr0;
              PS3_KNOB(on, getenv("GCM_RATES") ? 1 : 0);
              extern uint8_t* vm_base;
              if (on && vm_base) {
                  extern volatile u32 g_gcm_flip_words;
                  extern unsigned cellGcm_flip_request_count(void);
                  const u8* lp = vm_base + 0x20000FF0u;
                  u32 fw = g_gcm_flip_words, l = ((u32)lp[0] << 24) | ((u32)lp[1] << 16) | ((u32)lp[2] << 8) | lp[3];
                  u32 fr = cellGcm_flip_request_count();
                  /* flips: all flip requests, in the FIFO or not (Demon's
                   * Souls flips twice per frame it renders). */
                  /* boundaries: at a flip (render / no display draws), at a
                   * display clear (render / no display draws / no flip since
                   * the last present / batch too small) */
                  /* repaints: the overlay's frames between the game's
                   * (rc_overlay_repaint), not counted in presents. */
                  static u32 c0, f1, f2, k1, k2, k3, rp0;
                  fprintf(stderr, "[gcm-rates] t=%llu presents=%.1f/s flipwords=%u/s flips=%u/s label255=%08X (+%u) draws=%u"
                          " | flip:render=%u nodisp=%u clear:render=%u nodisp=%u sameflip=%u small=%u repaints=%u%c",
                          (unsigned long long)tnow, s_tframes * 1000.0 / (double)(tnow - s_tt0),
                          fw - fw0, fr - fr0, l, l - l0, s_dbg_last_draws,
                          s_pc_flip_render - f1, s_pc_flip_nodisp - f2, s_clear_presents - c0,
                          s_pc_clear_nodisp - k1, s_pc_clear_sameflip - k2, s_pc_clear_small - k3,
                          s_repaints - rp0, 10);
                  rp0 = s_repaints;
                  fw0 = fw; l0 = l; fr0 = fr;
                  f1 = s_pc_flip_render; f2 = s_pc_flip_nodisp; c0 = s_clear_presents;
                  k1 = s_pc_clear_nodisp; k2 = s_pc_clear_sameflip; k3 = s_pc_clear_small;
              } }
            s_tframes = 0;
            s_tt0 = tnow;
        }
    }
}

