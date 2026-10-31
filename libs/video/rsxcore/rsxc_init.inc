/* ---------------------------------------------------------------------------
 * D3D12 initialization
 * -----------------------------------------------------------------------*/

static void pso_lib_open(void);   /* pipeline library, defined with the shader cache */
static void shader_cache_warm(void);   /* the shader cache read ahead, defined with it */
static u64 s_vp_root_hash;        /* vp_layout's serialized bytes, part of every pipeline's name */
static u64 fnv_bytes(u64 h, const void* p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= ((const u8*)p)[i]; h *= 1099511628211ull; }
    return h;
}

/* Until the GPU has reached a frame slot's point on the timeline (2 s at
 * most, then the device's state is looked at). */
static void frame_slot_wait(u32 slot)
{
    const gpu_fence want = s_rc_frame_fence[slot];
    const u64 done = gpu_fence_completed(s_rc_dev);
    if (done == UINT64_MAX) check_device("frame fence");   /* (what a lost device's fences read) */
    if (done < want && gpu_fence_wait(s_rc_dev, want, 2000)) {
        const u32 rr = gpu_lost(s_rc_dev);
        printf("[D3D12] FENCE STUCK 2s: want %llu got %llu removed=0x%08lX\n",
               (unsigned long long)want,
               (unsigned long long)gpu_fence_completed(s_rc_dev),
               (unsigned long)rr);
        if (rr != S_OK) check_device("frame fence wait");
    }
}

/* Recording on a frame slot's recorder (the GPU layer's, one allocator and
 * list per slot): today's allocator and list Reset. s_rc_cmd is the recorder
 * every command goes to (step C8a: there is no native list beside it). The
 * slot's last frame is done first: after render_frame's opening wait_for_gpu
 * that is a read of the fence, but a repaint opens the slot right after a
 * present, and a slot the present did not move on from is still in flight
 * (move_to_next_frame waits only for its use before). */
static void frame_cmd_begin(u32 slot)
{
    frame_slot_wait(slot);
    s_rc_cmd = gpu_cmd_begin(s_rc_dev, slot);
}

/* Today's Close and ExecuteCommandLists of the open recorder. Every submit
 * may touch the acquired image (the frame's blit, the repaint's, a
 * capture's readback), so each waits for it and lets the present wait for
 * it, as gpu.h asks (step C8b: D3D12 needs neither; the null backend
 * checks both, as Vulkan needs them). */
static void frame_cmd_submit(void)
{
    gpu_submit_desc how;
    how.swapchain = s_rc_swap;
    how.flags = GPU_SUBMIT_ACQUIRE_WAIT | GPU_SUBMIT_PRESENT_SIGNAL;
    gpu_cmd_end(s_rc_dev, s_rc_cmd);
    gpu_submit(s_rc_dev, &s_rc_cmd, 1, &how);
}

/* The display buffers and the shared depth buffer, at RSX_SCALE's size: made
 * by init_d3d12, and again by rsx_scale_apply when the scale changes. They
 * are the GPU layer's textures, with their views (step C5a). */
static int display_targets_make(void)
{
    /* The game's frame: textures of their own (see render_targets), in the
     * PRESENT (= COMMON) state between frames as the swap chain's were, so
     * every transition and readback of them is unchanged. No clear value;
     * the RTV is today's NULL description. */
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        gpu_texture_desc td;
        memset(&td, 0, sizeof td);
        td.width   = s_d3d.host_w;
        td.height  = s_d3d.host_h;
        td.layers  = 1;
        td.mips    = 1;
        td.format  = GPU_FORMAT_R8G8B8A8_UNORM;
        td.samples = 1;
        td.use     = GPU_USE_RT | GPU_USE_SAMPLED | GPU_USE_COPY_SRC | GPU_USE_COPY_DST;
        td.state   = GPU_STATE_PRESENT;
        s_rc_rt_tex[i] = gpu_texture_create(s_rc_dev, &td);
        if (!GPU_VALID(s_rc_rt_tex[i])) return -1;
        s_rc_rt_rtv[i] = rc_target_view(s_rc_rt_tex[i], GPU_VIEW_RTV, 0, 0);
    }
    return 0;
}

/* Step V5: the forms for what the device lacks (rsxc_state.inc), from its
 * feature bits and RSX_CLAMP_FORM / RSX_DEPTH_FORMAT; a line says which when
 * it is not today's. */
static void rc_forms_pick(void)
{
    const u32 caps = s_rc_dev->caps.bits;
    const char* cf = getenv("RSX_CLAMP_FORM");
    s_uvc_vs = !(caps & GPU_CAP_GS) || (cf && !strcmp(cf, "vs"));
    if (s_uvc_vs)
        printf("[D3D12] sprite-edge clamp: in the vertex shader (%s)\n",
               (caps & GPU_CAP_GS) ? "RSX_CLAMP_FORM=vs" : "no geometry shaders");
    const char* df = getenv("RSX_DEPTH_FORMAT");
    s_rc_d32 = !(caps & GPU_CAP_D24S8) || (df && !strcmp(df, "d32s8"));
    if (s_rc_d32)
        printf("[D3D12] depth surfaces: D32S8 (%s)\n",
               (caps & GPU_CAP_D24S8) ? "RSX_DEPTH_FORMAT=d32s8" : "no D24S8");
}

static int depth_buffer_make(void)
{
    gpu_texture_desc td;
    memset(&td, 0, sizeof td);
    /* Sized to cover the LARGEST render target, not just the window:
     * D3D12 clips rasterization to the smallest bound attachment, and
     * this DSV is shared by every pass -- with a window-sized depth
     * buffer, draws into larger offscreen RTs (wave's 1920x1080 input
     * image) were silently cropped to the window rect. */
    td.width         = rt_host_dim(2048, 2048, 2048);
    td.height        = rt_host_dim(2048, 2048, 2048);
    td.layers        = 1;
    td.mips          = 1;
    td.format        = rc_ds_format();   /* (step V5: D24S8, or D32S8) */
    td.samples       = 1;
    td.use           = GPU_USE_DS;
    td.state         = GPU_STATE_DEPTH_WRITE;
    td.clear_format  = rc_ds_format();
    td.clear_depth   = 1.0f;
    td.clear_stencil = 0;
    s_rc_depth_tex = gpu_texture_create(s_rc_dev, &td);
    if (!GPU_VALID(s_rc_depth_tex)) {
        printf("[D3D12] Depth buffer creation failed\n");
        return -1;
    }

    /* The DSV: D24S8 (or D32S8), 2D, mip 0, no flags. */
    s_rc_depth_dsv = rc_target_view(s_rc_depth_tex, GPU_VIEW_DSV, 0, rc_ds_format());
    return 0;
}

/* Step V6: the start's steps, by name: loader, instance, adapter, features
 * and device (the instance file's rc_be_create says which it can tell
 * apart), window, swap chain, targets, shaders (the backend's own) and rings
 * (the recorders and the frame's buffers). ok 0 is a step that failed; the
 * first one is kept, for the route switch's line and the host's report
 * (rsx_out.c falls back to today's renderer when the Vulkan start fails).
 * RSX_FAIL_START_AT=<step> fails the start there on purpose, for tests, as
 * RSX_LOSE_DEVICE_AT_FRAME loses the device. */
static const char* s_rc_start_failed;   /* the step a failed start stopped at, NULL */
static int start_step(const char* step, int ok)
{
    const char* f = getenv("RSX_FAIL_START_AT");
    if (ok && f && !strcmp(f, step)) {
        fprintf(stderr, "[rsx] RSX_FAIL_START_AT=%s: the start fails here (test)%c", step, 10);
        ok = 0;
    }
    if (!ok && !s_rc_start_failed) s_rc_start_failed = step;
    return ok;
}

static int init_d3d12(u32 width, u32 height)
{
    /* The device, its queue, a recorder per frame slot and the fence: the GPU
     * layer's backend, with today's choices. Its breadcrumbs knob
     * (rc_be_breadcrumbs; on D3D12 Device Removed Extended Data:
     * auto-breadcrumbs, page faults, the draws' markers), which
     * gpu_fault_report prints once the device is lost.
     * CELLMARK_IGPU: the low-power adapter (for A/B testing a suspected
     * iGPU-driver stall); else the high-performance one, software adapters
     * skipped, the default adapter last. The debug layer is never enabled on
     * this route (its debug knob and debug builds included). The backend's
     * device is made by the instance file (rc_be_create). */
    {
        gpu_device_desc dd;
        memset(&dd, 0, sizeof dd);
        if (rc_be_breadcrumbs()) dd.flags |= GPU_DEVICE_BREADCRUMBS;
        if (getenv("CELLMARK_IGPU")) dd.flags |= GPU_DEVICE_LOW_POWER;
        dd.cmd_slots = 2 * FRAME_COUNT;   /* (step VM: the overlay's, RC_OVERLAY_SLOT on) */
        s_rc_dev = rc_be_create(&dd);
        if (!start_step("device", s_rc_dev != NULL)) return -1;
    }
    rc_forms_pick();

    pso_lib_open();
    shader_cache_warm();

    /* The swap chain: flip model, FRAME_COUNT buffers, Alt+Enter left to the
     * window. A windowed flip-model swap chain presenting with sync interval 0
     * is still paced by the compositor at the display's refresh unless it may
     * tear: it is made with ALLOW_TEARING when the system has it, and each such
     * Present passes GPU_PRESENT_TEARING (RSX_VSYNC=0, swap_present). A
     * system without it (no variable refresh support in the driver or OS)
     * presents with vsync. */
    s_d3d.tearing = (s_rc_dev->caps.bits & GPU_CAP_TEARING) ? 1 : 0;
    {
        gpu_swapchain_desc sd;
        memset(&sd, 0, sizeof sd);
        sd.window = s_d3d.hwnd;
        sd.width = width;
        sd.height = height;
        sd.format = GPU_FORMAT_R8G8B8A8_UNORM;
        sd.buffers = FRAME_COUNT;
        sd.colour_space = GPU_COLOR_SRGB;
        sd.flags = s_d3d.tearing ? GPU_SWAP_TEARING : 0;
        s_rc_swap = gpu_swap_create(s_rc_dev, &sd);
        if (!start_step("swap chain", GPU_VALID(s_rc_swap))) return -1;
        /* Step VC2: read again, as Vulkan knows only from the window's
         * surface whether it has the IMMEDIATE mode (the same on D3D12). */
        s_d3d.tearing = (s_rc_dev->caps.bits & GPU_CAP_TEARING) ? 1 : 0;
    }

    rc_acquire();   /* (step VC1: the image, and the slot it falls in) */

    /* The display targets' and the swap chain images' RTVs and the depth
     * DSV are the GPU layer's views (step C5a): no RTV or DSV heap here, nor
     * anywhere else now, so no descriptor size is read (step C8a). */

    if (!start_step("targets", display_targets_make() == 0)) return -1;

    /* The window's buffers, with RTVs of their own (swap_buffers_get). */
    s_d3d.win_w = width;
    s_d3d.win_h = height;
    if (!start_step("swap chain", swap_buffers_get() == 0)) return -1;
    if (!start_step("shaders", blit_init() == 0)) return -1;

    /* ---------------------------------------------------------------
     * Depth/stencil buffer
     * 24-bit depth + 8-bit stencil (GPU_FORMAT_D24_UNORM_S8_UINT), or
     * D32S8 with RSX_DEPTH_FORMAT=d32s8 (rc_ds_format, step V5).
     * One shared depth texture across both frames — RSX games on PS3
     * typically use a single zeta surface. Its DSV is a view of it.
     * ---------------------------------------------------------------*/
    {
        if (!start_step("targets", depth_buffer_make() == 0)) return -1;

        printf("[D3D12] Depth buffer created (%ux%u %s)\n", width, height, s_rc_d32 ? "D32S8" : "D24S8");
    }

    /* The recorders and the fence are the device's (one fence, the device's
     * timeline: s_rc_frame_fence holds each frame slot's point on it). Until
     * the first frame, slot 0's list is closed, as today's one list is after
     * init. (The slots' points first: a device made again starts its
     * timeline over.) */
    memset(s_rc_frame_fence, 0, sizeof s_rc_frame_fence);
    frame_cmd_begin(0);
    if (!start_step("rings", s_rc_cmd != NULL)) return -1;
    gpu_cmd_end(s_rc_dev, s_rc_cmd);

    /* Draws record from here on (pipeline_ready). The root signature and the
     * fixed-function pipelines this made for the draws without a vertex
     * program (triangle, line, point and the font atlas's) are gone with
     * their pass (step C8a): no capture draws through them. */
    s_d3d.pipeline_ready = 1;
    {
        /* SRV descriptor heap (shader-visible). Layout: slot 0 = legacy atlas
         * (dbgfont / textured 2D path), slots 1-4 = per-draw VP textures,
         * 5-20 = offscreen render targets (render-to-texture), rest spare.
         * All slots start as null SRVs so any 4-wide table window (the
         * root signature binds t0-t3) is valid on tier-1 hardware. */
        /* The GPU layer's table pool, its entries made as these R8_UNORM
         * null SRVs (step C4). The regions name its tables for Vulkan: the
         * per-draw windows, the AA source (AA_SRV_SLOT, rsxc_post.inc) and
         * SMAA's two 3-wide windows (SMAA_SRV_SLOT + 0 and + 3). */
        {
            gpu_table_pool_desc pd;
            memset(&pd, 0, sizeof pd);
            pd.kind = GPU_TABLE_TEX;
            pd.count = SRV_HEAP_SIZE;
            pd.nregions = 4;
            pd.region[0].first = DRAW_SRV_BASE; pd.region[0].tables = MAX_DRAWS; pd.region[0].size = RSX_FP_UNITS;
            pd.region[1].first = 21; pd.region[1].tables = 1; pd.region[1].size = 1;
            pd.region[2].first = 22; pd.region[2].tables = 1; pd.region[2].size = 3;
            pd.region[3].first = 25; pd.region[3].tables = 1; pd.region[3].size = 3;
            s_rc_srv_pool = gpu_table_pool_create(s_rc_dev, &pd);
            if (!GPU_VALID(s_rc_srv_pool)) printf("[D3D12] SRV heap creation failed\n");
            else s_rc_srv_w = gpu_table_writer_get(s_rc_dev, s_rc_srv_pool);
        }

        /* The offscreen targets' RTVs are views of their textures (off_rt_get,
         * step C5a): no RTV heap for them. */
    }

    /* ---------------------------------------------------------------
     * Create dynamic vertex buffer (upload heap, 4MB)
     * ---------------------------------------------------------------*/
    {
        /* The GPU layer's upload buffer, mapped for its life (step C2). */
        if (rc_buf_make((u64)(MAX_VERTICES * VERTEX_STRIDE), GPU_USE_VERTEX, GPU_MEM_UPLOAD,
                        &s_rc_vertex_buf, &s_d3d.vb_mapped)) {
            printf("[D3D12] Vertex buffer created (%u KB)\n",
                   (MAX_VERTICES * VERTEX_STRIDE) / 1024);
        }
    }

    /* ---------------------------------------------------------------
     * Real vertex-program path resources: root signature (CBV b0 for the
     * vp_c[] bank + SRV t0 + static sampler s0), a raw-float4 vertex buffer,
     * and the constant-bank buffer. The PSO itself is built lazily once the
     * game uploads its VP microcode (render_frame).
     * ---------------------------------------------------------------*/
    {
        /* SRV table t0-t15 so decompiled fragment programs can sample every
         * RSX texture unit; the hardcoded atlas/colour PSs use only t0 and are
         * unaffected. Matching sampler table s0-s15. The root signature is the
         * GPU layer's layout (step C3b), slot for slot today's parameters. */
        gpu_layout_desc ld;
        memset(&ld, 0, sizeof ld);
        ld.flags = GPU_LAYOUT_VERTEX_INPUT;
        ld.nslots = 6;
        ld.slot[0] = lay_slot(GPU_SLOT_CB, 0, 0, GPU_VIS_VERTEX);                    /* b0 = vp_c bank */
        ld.slot[1] = lay_slot(GPU_SLOT_TABLE_TEX, 0, RSX_FP_UNITS, GPU_VIS_PIXEL);
        ld.slot[2] = lay_slot(GPU_SLOT_CB, 1, 0, GPU_VIS_PIXEL);                     /* b1 = FP texscale */

        /* s0-s15 come from a per-draw sampler table built from the guest's
         * TEXTURE_ADDRESS/TEXTURE_FILTER (vp_sampler_table). Fixed static
         * samplers sampled a 256x1 gamma LUT with WRAP, so u=0 blended texel 0
         * with texel 255 and black came out mid-grey. */
        ld.slot[3] = lay_slot(GPU_SLOT_TABLE_SAMPLER, 0, RSX_FP_UNITS, GPU_VIS_PIXEL);
        {
            /* The GPU layer's table pool: table t at t*16 (step C4). */
            gpu_table_pool_desc sh;
            memset(&sh, 0, sizeof sh);
            sh.kind = GPU_TABLE_SAMPLER;
            sh.count = VP_SAMP_TABLES * RSX_FP_UNITS;
            sh.nregions = 1;
            sh.region[0].first = 0; sh.region[0].tables = VP_SAMP_TABLES; sh.region[0].size = RSX_FP_UNITS;
            s_rc_samp_pool = gpu_table_pool_create(s_rc_dev, &sh);
            if (!GPU_VALID(s_rc_samp_pool))
                printf("[D3D12] sampler heap creation failed\n");
        }

        /* GPU vertex fetch: t20 = the frame's raw guest vertex bytes (vf_buf),
         * b2 = one constant, the draw's attribute records' offset in it. */
        ld.slot[4] = lay_slot(GPU_SLOT_RAW, 20, 0, GPU_VIS_VERTEX);
        ld.slot[5] = lay_slot(GPU_SLOT_CONST32, 2, 1, GPU_VIS_VERTEX);
        /* No static samplers. The layout's serialized bytes are today's, so
         * s_vp_root_hash, and with it every pipeline's name in the manifest
         * (pso_desc_key), is today's too. */
        s_d3d.vp_layout = lay_make(&ld);
        s_vp_root_hash = rc_be_layout_hash(s_rc_dev, s_d3d.vp_layout);   /* (the instance file's) */

        /* raw float4 vertex buffer + constant bank (both UPLOAD, persistently
         * mapped): the GPU layer's upload buffers (step C2, rc_buf_make) */
        u64 bytes;

        /* RSX_VP_BUFFERS=0: none of the four per-frame buffers below (about
         * 1.2 GB of upload memory at the defaults), for a process that runs no
         * game and draws only its overlay, such as a title's setup screens
         * while a compile beside them needs the memory. Only a frame with
         * guest draws uses them: a game started this way skips render_frame's
         * vertex-program pass, so it draws none of its geometry. */
        if (!ps3_env_int("RSX_VP_BUFFERS", 1)) {
            fprintf(stderr, "[VPVB] no vertex-program buffers (RSX_VP_BUFFERS=0)%c", 10);
            goto vp_buffers_done;
        }

        /* VP_VB_VERTS=<n>: start the buffer at n vertices rather than
         * MAX_VERTICES, for a title known to need more (it grows on demand,
         * but the growth costs a frame; see vp_vb_grow). */
        { u64 v = (u64)ps3_env_int("VP_VB_VERTS", 0);
          if (v > MAX_VERTICES && v <= (64ull << 20)) s_vp_cap = (u32)v; }
        bytes = (u64)s_vp_cap * 256 * 2;     /* generic VP vertex: 16 float4 slots,
                                          * DOUBLE-buffered by frame parity so a
                                          * new frame's upload never overwrites
                                          * vertices the in-flight GPU frame is
                                          * still reading (was: torn/missing
                                          * triangles mixing stale+new verts) */
        /* Failure here leaves vp_vb_mapped NULL and every VP draw silently
         * declines to upload -- the scene just does not render, with no error
         * anywhere. Report the size too: this buffer is
         * MAX_VERTICES * 256 * 2 bytes and grows fast. */
        fprintf(stderr, "[VPVB] per-frame vertex buffer: %llu MB (%u verts, x2 parity)%c",
                (unsigned long long)(bytes >> 20), (unsigned)s_vp_cap, 10);
        /* (Step V5: the clamp's vertex-shader form reads it at t20 too.) */
        if (!rc_buf_make(bytes, GPU_USE_VERTEX | (s_uvc_vs ? GPU_USE_RAW : 0u), GPU_MEM_UPLOAD,
                         &s_rc_vp_vb, &s_d3d.vp_vb_mapped))
            fprintf(stderr, "[VPVB] ALLOCATION FAILED -- no VP geometry will render%c", 10);

        bytes = (u64)VP_CB_STRIDE * MAX_DRAWS * 2;      /* per-draw constant slots, x2 parity */
        /* A failure here is silent and catastrophic: vp_record_cb returns early,
         * every draw loses its constants, and the whole scene transforms by
         * zeros -- indistinguishable from a broken vertex program. Say so. */
        fprintf(stderr, "[VPCB] per-draw constant buffer: %llu MB (%d draws)%c",
                (unsigned long long)(bytes >> 20), MAX_DRAWS, 10);
        rc_buf_make(bytes, GPU_USE_CONSTANT, GPU_MEM_UPLOAD, &s_rc_vp_cb, &s_d3d.vp_cb_mapped);

        bytes = (u64)VP_FPCB_STRIDE * MAX_DRAWS * 2;     /* per-draw FP slots (b1), x2 parity */
        rc_buf_make(bytes, GPU_USE_CONSTANT, GPU_MEM_UPLOAD, &s_rc_vp_fpcb, &s_d3d.vp_fpcb_mapped);

        /* GPU vertex fetch ring: each fetch draw's guest vertex bytes and
         * attribute records, x2 parity like vp_vb. VP_FETCH_RING_MB (read
         * once, default 64) per half; a draw that does not fit takes the CPU
         * path. */
        { int mb = ps3_env_int("VP_FETCH_RING_MB", 64);
          if (mb < 4) mb = 4; if (mb > 1024) mb = 1024;
          s_vf_cap = (u32)mb << 20;
          if (!rc_buf_make((u64)s_vf_cap * 2, GPU_USE_RAW, GPU_MEM_UPLOAD, &s_rc_vf_buf, &s_d3d.vf_mapped))
              s_vf_cap = 0; }

    vp_buffers_done:
        printf("[D3D12] VP path resources: rootsig=%p vb=%p cb=%p\n",
               (void*)(uintptr_t)s_d3d.vp_layout.id, (void*)(uintptr_t)s_rc_vp_vb.id,
               (void*)(uintptr_t)s_rc_vp_cb.id);
    }
    /* Step V6: on a backend that starts strictly (rc_be_start_strict: Vulkan,
     * whose route then starts today's renderer instead), a frame buffer that
     * could not be made fails the start; elsewhere the start goes on without
     * it, as today's does. */
    if (!start_step("rings", !rc_be_start_strict || !ps3_env_int("RSX_VP_BUFFERS", 1) ||
                                 (s_d3d.vb_mapped && s_d3d.vp_vb_mapped && s_d3d.vp_cb_mapped && s_d3d.vp_fpcb_mapped)))
        return -1;

    printf("[D3D12] Initialization complete (%ux%u, %u buffers, pipeline=%s)\n",
           width, height, FRAME_COUNT,
           s_d3d.pipeline_ready ? "ready" : "NOT ready");
    return 0;
}

