/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define FRAME_COUNT         2   /* double buffering */
#define RC_SWAP_MAX         8   /* the swap chain's images: Vulkan may make more than FRAME_COUNT (step VC1) */
#define MAX_VERTICES    393216  /* per-frame vertex buffer (dbgfont submits
                                 * ~7.5k verts/frame; leave generous headroom) */
/* Fragment texture units a draw records and binds: all sixteen the RSX has.
 * Four were tracked; units 4-15 were dropped at bind time, and a program that
 * samples one (Demon's Souls's level shaders use t4 and t7) declares a
 * register outside the root signature's range, so its pipeline failed to
 * build (E_INVALIDARG) and those surfaces never drew. */
#define RSX_FP_UNITS      16
#define MAX_DRAWS         16384 /* per-frame draw records. The per-draw
                                 * constants were VP_CB_STRIDE * MAX_DRAWS
                                 * * 2 bytes (264 MB of upload heap) in one
                                 * buffer; now chunks as frames need them
                                 * (s_rc_vpcb, s_rc_fpcb). 2048,
                                 * sized from the spawn area's ~1841, was too
                                 * few for a dark room in the first level:
                                 * every draw past it was dropped, the display
                                 * copy at the end of the frame with them, and
                                 * the frame presented black. Boletarian
                                 * Palace reaches 7529, too close to 8192.
                                 * draw_room() reports running out. */
/* Per-draw VP constant-buffer slot: vp_c[512] + posscale + posoffset
 * (514 vec4 = 8224 B) rounded up to D3D12's 256-byte CBV alignment.
 * Constants are snapshotted at RECORD time -- wave's passes each set their
 * own texScale/offset uniforms, so one per-frame snapshot ran every pass
 * with the LAST pass's constants. */
#define VP_CB_STRIDE      8448
/* vp_cb banks a chunk (s_rc_vpcb): 1 << 10, 8.25 MB a parity half. A power
 * of two, so a draw finds its chunk with a shift, not a divide. */
#define VP_CB_CHUNK_SHIFT 10
#define VP_CB_CHUNK_MASK  ((1u << VP_CB_CHUNK_SHIFT) - 1u)
/* b1 per-draw FP slot, in float4s: rsx_texscale[16], rsx_alphatest, then the
 * program's inline constants fp_constants[FPCB_KMAX]. (17 + 128) * 16 = 2320 B,
 * rounded up to D3D12's 256-byte CBV alignment. */
#define FPCB_ALPHA        16
#define FPCB_K            17
#define FPCB_KMAX         128
#define VP_FPCB_STRIDE    2560
/* vp_fpcb draws a chunk (s_rc_fpcb): 1 << 11, 5 MB a parity half. */
#define VP_FPCB_CHUNK_SHIFT 11
#define VP_FPCB_CHUNK_MASK  ((1u << VP_FPCB_CHUNK_SHIFT) - 1u)
#define VERTEX_STRIDE       36  /* bytes per host vertex: pos3 + col4 + uv2 */

/* One texture unit as a draw takes it: the draw record's tex[] and the
 * current units it copies them from (cur_texs) share the struct, so a draw
 * takes all sixteen with one copy. */
typedef struct {
    u32 off;        /* resolved vm offset (guest upload source), 0 = none */
    u32 raw;        /* raw RSX offset (offscreen-RT matching) */
    u32 w, h, fmt;  /* dims + RSX base format */
    u32 ctrl1;      /* NV4097 TEXTURE_CONTROL1: component remap crossbar */
    u32 addr, filt; /* NV4097 TEXTURE_ADDRESS / TEXTURE_FILTER: sampler state;
                     * filt bits 20-22 (unused by the register) hold
                     * TEXTURE_CONTROL0's max anisotropy */
    u32 border;     /* NV4097 TEXTURE_BORDER_COLOR (ARGB8): what a border
                     * address mode reads outside the texture */
    u32 mips;       /* SET_TEXTURE_FORMAT bits 16..31: mipmap level count.
                     * Cube faces sit one whole mip pyramid apart, so this
                     * is what sets the face stride. */
    int cube;       /* SET_TEXTURE_FORMAT bit 2: a cube texture. Sampled
                     * with a 3-component direction, not a 2D uv -- treating
                     * one as 2D is what makes an environment-mapped chrome
                     * surface come out with black patches and banding. */
    int set;
} RcTexUnit;

typedef struct {
    u32 vb_byte_offset; /* offset into vb_mapped where this draw's verts live */
    u32 vertex_count;
    u32 index_count;    /* VP: > 0 = indexed, index_count indices from vp_ib at
                         * ib_first, numbered from this draw's first vertex */
    u32 ib_first;
    u32 topology;       /* rsx_topology (NOT a D3D value) */
    int is_vp;          /* 1 = real VP path: vb_byte_offset indexes vp_vb (float4).
                         * 0: not drawn (that pass is gone, step C8a) */
    /* VP path per-draw shader/texture state, captured at draw_arrays time. */
    u32 fp_addr;        /* SET_SHADER_PROGRAM value (guest FP ucode location)   */
    int fp_exp32;       /* SET_SHADER_CONTROL 32-bit-exports bit at draw time   */
    u32 alpha_ctl;      /* alpha test: enable<<16 | (func&0xFF)<<8 | ref */
    u32 begin_epoch;    /* SET_BEGIN_END generation, for batch concatenation */
    u32 cull;           /* packed face culling: bit0 enable, bit1 cull FRONT
                         * (else BACK), bit2 front face is CCW. RSX culls back
                         * faces on most solid geometry; rendering everything
                         * double-sided lets a shell's interior faces show
                         * through and shade unlit (black). */
    u32 cmask;          /* D3D write mask from SET_COLOR_MASK at draw time
                         * (wave's sim passes write single lanes of the height
                         * maps; ignoring the mask stomped persistent state) */
    /* Per-unit textures (t0-t3): decompiled FPs sample up to 4 units
     * (demosaic's interpolation passes read 3). Captured at draw time. */
    RcTexUnit tex[RSX_FP_UNITS];
    int tex_rt[RSX_FP_UNITS];   /* pre-pass: OffRT index sampled by unit, -1 = none */
    int tex_z[RSX_FP_UNITS];    /* pre-pass: depth surface (s_zeta) sampled by unit, -1 = none */
    u32 d2c_mask;       /* pre-pass: units reading a depth surface through a colour
                         * format (the decompiler decodes them; PSO key) */
    int tex_slot;       /* legacy single-slot path (atlas); -1 = none */
    int vs_idx;         /* VPVSEntry slot for this draw's vertex program (-1 = primary) */
    int blend;          /* guest blend enable at draw time */
    u32 blend_key;      /* packed guest blend state (factors+equation), PSO key */
    /* Render-to-texture: which colour surface this op targets. 0 = a display
     * buffer (the swap-chain backbuffer); else the resolved vm offset of an
     * offscreen surface (demosaic chains its effect passes through local-
     * memory buffers and composites from them). */
    u32 rt_off;
    /* Which per-draw constant slot this record's constants were written to.
     * vp_record_cb writes them keyed by the RECORD index, so any pass that
     * reorders or compacts s_d3d.draws (DRAW_KEEP_TEX, DRAW_LAST_TEX,
     * DRAW_LIMIT) would otherwise make every surviving draw read some other
     * object's MVP -- silently transforming it somewhere else entirely. */
    u32 cb_slot;        /* (also the draw's vp_fpcb block) */
    u32 vcb_slot;       /* vp_cb bank bound: the frame's next bank, or an earlier
                         * draw's this frame whose vertex constants are the same
                         * (vp_record_cb numbers them as it writes them) */
    u32 rt_mrt[3];      /* colour targets B,C,D (MRT1/2/3), 0 = none. Deferred
                         * shading G-buffers write 3-4 targets in one pass. */
    u32 rt_w, rt_h;     /* surface clip dims at record time (offscreen RT size) */
    u32 rt_fmt;         /* RSX surface colour format (SET_SURFACE_FORMAT [4:0]) */
    u32 rt_pitch[4];    /* guest pitch of rt_off, rt_mrt[0..2] (colour write-back) */
    u32 rt_dma[4];      /* their context DMA: 0xFEED0001 = main memory, else local */
    /* Guest viewport rect at draw time (target pixels). Sub-viewport layouts
     * (wave's debug tiles) position quads with this, not with constants --
     * forcing full-target viewports drew every tile window-sized. */
    u32 vp_x, vp_y, vp_w, vp_h;
    /* Ordered clear op (offscreen surfaces only; display clears stay the
     * frame-start backbuffer clear). is_clear records also set is_vp so the
     * legacy replay pass skips them. */
    int   is_clear;
    float cc[4];
    u32   clr_flags;    /* CLEAR_SURFACE mask: bit0 Z, bit1 S, bits 4-7 RGBA */
    float clr_z;        /* depth clear value, 0..1 */
    u32   clr_s;        /* stencil clear value */
    /* Depth surface (SET_SURFACE_ZETA_OFFSET, raw) and its SET_SURFACE_FORMAT
     * depth field [7:5] (1 = Z16, 2 = Z24S8) at record time. */
    u32   zeta, zfmt;
    u32   ctgt;         /* SET_SURFACE_COLOR_TARGET at record time (0 = none) */
    u32   depth;        /* packed guest depth state (rsx_depth_key, PSO key) */
    /* GPU vertex fetch (VP_GPU_FETCH): the VS reads the guest's vertex arrays
     * from vf_buf, and the indices are the guest's own. fdesc is the virtual
     * offset (rc_arena) of the draw's attribute records in vf_buf: the draw
     * binds the chunk that holds it and passes its offset in that chunk.
     * fetch is 1 for a triangle list, 2 for a strip with cuts
     * (vf_strip_native). */
    u32   fetch, fdesc;
    /* Step V5: 1 when the pre-pass drew it in the clamp's vertex-shader form
     * (kind 4): uvc_n vertices from the table at uvc_tab (uvc_table). */
    u32   uvc, uvc_tab, uvc_n;
    /* The pre-pass's vp_get_fp_pso result, and the render_frame it was made
     * in (s_rf_serial). The VP pass binds it instead of looking the pipeline
     * up a second time (RSX_PSO_IN_REC); a draw the pre-pass skipped keeps an
     * older serial. Pipelines are only released at shutdown, so the handle
     * stays good. */
    gpu_pipeline pso;
    u32   pso_serial;
} D3D12DrawRecord;

/* Offscreen render target (render-to-texture). Persistent across frames --
 * pass N's output is sampled by pass N+1 and possibly by later frames. */
typedef struct {
    gpu_texture           tex;         /* the GPU layer's texture (step C5a) */
    gpu_view              rtv;         /* its RTV (step C5a) */
    gpu_buffer            up_buf;      /* init-upload staging: the GPU layer's (step C5a); into the grave
                                        * once its copy is recorded, so none after off_rt_get */
    u32                   off, w, h;   /* raw RSX offset + guest dims */
    u32                   hw, hh;      /* the resource's dims (RSX_SCALE) */
    u32                   dxgi;        /* gpu_format of the resource */
    u32                   gdxgi;       /* the guest format's (refill, write-back): FP16 for an
                                        * RGBA8 target HDR promotes (rt_target_format) */
    gpu_state             st;          /* tracked resource state */
    int                   used;        /* referenced this frame */
    gpu_buffer            rb_buf[2];   /* colour write-back readback buffers: the GPU layer's (step C5b),
                                        * one per parity half (a batch's copy waits there for its store);
                                        * only [0] without frames in flight (s_rf_buf) */
    u64                   rb_size[2];
    u32                   gfmt, gpitch;/* guest SET_SURFACE_FORMAT word + pitch */
    u32                   gdma;        /* guest context DMA (local / main) */
    gpu_texture           snap_tex;    /* copy sampled by draws that also render here (step C5b) */
    u32                   snap_w, snap_h, snap_dxgi;
    gpu_state             snap_st;
    gpu_texture           down_tex;    /* guest-size copy of a scaled target (write-back, step C5b) */
    u32                   down_w, down_h, down_dxgi;
    gpu_state             down_st;
    /* Guest bytes after our last write-back (tex_csum over [gaddr, +glen)).
     * If they differ later, the title put something else there. */
    u32                   gaddr, glen, gsum;
    int                   gsum_ok, gsum_new;
    u32                   gen;         /* bumped when the target is dropped: a queued write-back of the old one
                                        * is not stored (WbCopy) */
    int                   wb_queued;   /* write-back copies of this target recorded, not stored yet */
} OffRT;
#define MAX_OFF_RTS  16  /* demosaic double-buffers its 6-surface pass chain */
#define RT_SRV_BASE  5   /* SRV heap slots 5..20 hold offscreen-RT SRVs */
/* Per-draw SRV windows: each VP draw gets RSX_FP_UNITS consecutive descriptors
 * (t0-t15) so multi-unit fragment programs see all their textures. One set
 * of MAX_DRAWS windows per parity half (draw_srv): a batch rewrites its own
 * while the GPU may still read the last batch's (RSX_FRAMES_IN_FLIGHT).
 * Without frames in flight, one set (s_rf_buf): s_draw_srv_sets, set by the
 * init before it makes the pool, so the heap's layout below is decided then. */
#define DRAW_SRV_BASE 32
static int frames_in_flight_on(void);   /* (rsxc_sync.inc) */
static u32 s_draw_srv_sets = 2;
#define DRAW_SRV_SETS s_draw_srv_sets
#define VP_SAMP_TABLES (2048 / RSX_FP_UNITS)
                             /* per-draw sampler tables (D3D12 caps a shader-
                              * visible sampler heap at 2048 samplers). When all
                              * are taken, the least recently used one the GPU
                              * has finished with is rewritten (vp_sampler_table). */
/* Each texture slot's own SRV, after the windows. It used to sit at heap
 * index 1+slot, inside the windows from slot 31 on: once a session had
 * cached that many textures, each new upload rewrote a descriptor of one of
 * the frame's first 63 draws. */
#define VP_TEX_SRV_BASE (DRAW_SRV_BASE + DRAW_SRV_SETS * MAX_DRAWS * RSX_FP_UNITS)
/* The post passes' descriptors (rsxc_post.inc), a set per parity half as
 * the windows (RSX_FRAMES_IN_FLIGHT: the last batch's AA pass may still read
 * its set): SMAA's six at POST_SRV_SMAA + 6 * half (its two 3-wide windows),
 * the AA source at POST_SRV_AA + half. (They were slots 21-27, one set.) */
#define POST_SRV_SMAA (VP_TEX_SRV_BASE + VP_TEX_SLOTS)
#define POST_SRV_AA   (POST_SRV_SMAA + 12)
#define SRV_HEAP_SIZE (POST_SRV_AA + 2)

/* Counts render_frame calls. What one call's pre-pass leaves for its later
 * passes carries this stamp (D3D12DrawRecord.pso, the TEX_CSUM_THREADS sums),
 * so nothing an earlier frame left passes for this one's. */
static u32 s_rf_serial = 0;

/* Per-frame VP texture slot: a guest texture uploaded for this frame's VP
 * draws (re-uploaded every frame -- gcm/cube's plasma animates in guest
 * memory). SRV lives at heap index VP_TEX_SRV_BASE+slot. */
typedef struct {
    gpu_texture tex;    /* the GPU layer's texture (step C4) */
    u32 off, w, h, fmt; /* current contents (resource reused when dims match) */
    u32 csum;           /* sparse checksum of the source bytes last uploaded */
    u32 fsum;           /* full checksum of them, kept while TEX_VERIFY is on */
    u32 face_bytes;     /* guest bytes from one cube face to the next */
    gpu_heap blk;       /* tex is placed here (tex_pool), at blk_off; none = committed (step C4) */
    u64 blk_off;
    int blk_class;
    int cube;           /* resource is a 6-face cube, sampled as TextureCube */
    u32 key;            /* the ORIGINAL bound offset -- the cache lookup key.
                         * off is the RESOLVED source after TEX_OFF_BIAS/TEX_REMAP
                         * shift it, so keying on off never matches the raw offset
                         * a later draw arrives with, and every draw re-uploads. */
    int used;           /* referenced this frame */
} VPTexSlot;
/* Distinct textures uploadable per frame. Four was enough for a UI/atlas-style
 * frame but not for a scene: this title binds ~20 distinct textures per frame,
 * so every draw past the fourth got slot -1 from vp_upload_tex_slot ("out of
 * slots") and drew UNTEXTURED. That reads as a correctly-lit but solid black
 * object, which is indistinguishable from a missing texture upload. */
/* 96 was not enough either: a Demon's Souls level frame binds up to ~130
 * distinct textures, depending on what the camera sees. Every texture first
 * bound after the slots ran out drew with nothing in its unit, so whatever
 * came last in the frame -- the HUD, always -- vanished whenever the view
 * was busy. Lookups go through a hash index (vp_tex_find), so the count can
 * be generous; a slot only holds a resource once a texture lands in it. */
#define VP_TEX_SLOTS 1024
#define VP_TEX_HASH  4096   /* power of two, > 2 x VP_TEX_SLOTS */

/* Decompiled-VS cache: one entry per distinct vertex-program ucode seen at
 * draw time (hashed). Apps switch VPs between draws (gcm/cube: its MVP cube VP
 * vs dbgfont-gcm's text VP); compiling only the first left later draws
 * transformed by the wrong program (text offscreen). */
typedef struct {
    u32 hash;               /* FNV-1a of the ucode */
    gpu_shader vs;          /* the compiled program (step C3b: the GPU layer's; never freed) */
    gpu_shader vs4;         /* step V5: its clamp form (kind 4), made at its first clamp draw */
    u8* vs4_code;           /* step V5: a copy of the program's bytes for that (s_uvc_vs); freed once made */
    u32 vs4_len;
    int uses_c03;
    int fetch;              /* reads vf_buf (rsx_vp_decompile_fetch): no input layout */
    u64 code64;             /* the pipeline index's fingerprint: FNV-1a 64 of the words up to END */
    u32 start;              /* the transform program start it was decompiled at */
    u64 vs_key, vs4_key;    /* step P3S: K of vs's and vs4's texts, for the neutral manifest (0: none) */
} VPVSEntry;
/* The per-draw VS cache grows and never evicts. Draws record a slot index
 * (vs_idx) and PSO keys carry it, so a slot must keep meaning the same
 * program for the life of the run. A fixed 16-entry cache that evicted by
 * shifting both broke that (draws picked up the WRONG program) and, in a 3D
 * scene with more than 16 programs, recompiled vertex programs every frame.
 * Entries are one blob per distinct guest program, which the title bounds. */
#define VP_VS_CACHE_INIT 64

/* Compiled guest-FP pipeline cache: decompiled VS (by cache slot) + decompiled
 * PS (fragment ucode at fp_addr). */
typedef struct {
    u32 fp_addr;
    int vs_idx;             /* VPVSEntry slot this PSO's VS came from */
    u32 vs_hash;            /* validates the slot hasn't been evicted */
    u32 gen;
    u32 blend;              /* packed guest blend key (enable+factors+eq, PSO key) */
    int nrt;                /* bound colour target count (PSO key)       */
    u32 rtfmt;              /* DXGI format of the colour targets (PSO key) */
    int exp32;              /* 32-bit-exports control bit (PSO key)       */
    u32 ucode_hash;         /* FNV-1a of the FP ucode: apps re-patch inline
                             * constants per frame (wave's stamp position/
                             * amplitude) -- address-only keying served the
                             * stale compile forever. */
    u32 cmask;              /* colour write mask (PSO key) */
    u32 cull;               /* packed face culling (PSO key) */
    u32 depth;              /* packed depth test/func/write (PSO key) */
    u32 cube_mask;          /* which units are cube textures (PSO key): the HLSL
                             * declares those samplers as TextureCube, so a cube
                             * and a 2D variant of the same program are different
                             * pipelines and must not share a cache entry. */
    u32 d2c_mask;           /* units reading depth as colour (PSO key) */
    u32 uvclamp;            /* 1 with the sprite-edge clamp (dr_uvclamp), else 0 (PSO key) */
    u32 vs4;                /* step V5: built in the clamp's vertex-shader form (kind 4) */
    u32 hdrv;               /* HDR: a scene program's clamp limit (dr_hdrv), float bits, or 0 (PSO key) */
    gpu_pipeline pso;       /* none: this key failed to build; not retried */
    int pending;            /* psj: in this frame's batch, pso not built yet */
    int next;               /* vp_fp_find: next entry in this fp_addr's bucket */
} VPFPEntry;
/* Guest-FP PSO cache. 16 entries thrashed badly: this title needs far more
 * distinct (fp, vs, blend, rt, cmask, ucode-hash) combinations than that in a
 * single frame, so the FIFO evicted entries that were needed again immediately
 * and ~17 shaders were recompiled EVERY frame -- about half the frame's CPU
 * time. Entries are small (a PSO pointer plus key fields).
 * It grows and never evicts: a 3D scene outgrew 256 entries and recompiled
 * every frame, and an evicted PSO was released while recorded command lists
 * could still reference it. Keys that fail to build are cached too (pso
 * NULL), so a bad program costs one compile, not one per draw. */
#define VP_FP_CACHE_INIT 256

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    /* Window */
    HWND hwnd;
    u32  width;
    u32  height;
    u32  host_w, host_h;   /* the display buffers' size (RSX_SCALE); width/height are the guest's */
    int  window_closed;

    /* D3D12 core (step C8a: the device, the queue, the swap chain and its
     * buffers, the recorders and the fence are the GPU layer's, s_rc_dev and
     * the statics after s_d3d; nothing here points at them) */
    int                        tearing;   /* swap chain made with ALLOW_TEARING */
    /* The game's frame, width x height (the title's display size): its own
     * textures (s_rc_rt_tex), not the swap chain's, so the window can be any
     * size. At the present the frame is scaled into the swap chain's buffer
     * (s_rc_swap_image, win_w x win_h: blit_to_window) and an overlay draws
     * over that at the window's own resolution. */
    u32                        win_w, win_h;
    gpu_layout                 blit_layout;   /* (step C3b) */
    gpu_pipeline               blit_pso;

    /* Synchronization */
    u32          frame_index;

    /* Dynamic vertex buffer (upload heap: s_rc_vertex_buf) */
    void*                 vb_mapped;      /* persistently mapped */
    u32                   vb_offset;      /* current write position */

    int                   pipeline_ready; /* 1 once init is far enough for draws to record */

    /* Debug: copy the presented backbuffer to disk as BMP for the first N
     * frames (enabled by env CELLMARK_DUMP). Lets us verify what actually
     * rendered without racing the window/process lifetime. (The readback
     * buffer is s_rc_readback_buf.) */
    u32                   readback_pitch;
    int                   dump_frames_left;
    int                   dump_skip_left;   /* CELLMARK_DUMP_SKIP: presents to ignore first */

    /* (The font atlas's pipeline, texture and state are gone with the pass of
     * the draws without a vertex program: step C8a.) The shader-visible
     * heaps are the table pools s_rc_srv_pool and s_rc_samp_pool (step C4). */

    /* Real RSX vertex-program path: the captured VP is decompiled to HLSL and
     * used as the vertex shader, fed the raw float4 attrib0 + the vp_c[]
     * constant bank. This produces exact position + texcoord (vs. the frac
     * approximation). Used for the 2D/dbgfont quad draws. */
    gpu_layout            vp_layout;            /* CBV(b0) + SRV table + sampler (step C3b) */
    gpu_pipeline          pipeline_state_vp;    /* decompiled VS + atlas PS      */
    gpu_pipeline          pipeline_state_vp_color; /* decompiled VS + colour PS (untextured 3D) */
    void*                 vp_vb_mapped;         /* set while vp_vb's chunks are there (s_vb); not an address */
    u32                   vp_vb_offset;
    void*                 vp_ib_mapped;         /* u32 indices of the VP draws, per-frame: s_rc_vp_ib */
    u32                   vp_ib_offset;         /* in indices */
    int                   vp_ready;             /* VS+PSO compiled ok            */
    u32                   vp_compiled_bytes;    /* ucode size when last compiled */
    gpu_shader            vp_vs_shader;         /* kept for guest-FP PSO builds (step C3b: a shader) */
    u32                   vp_gen;               /* bumped per VP recompile       */
    int                   vp_uses_c03;          /* VS references vp_c[0..3]      */
    VPTexSlot             vp_tex[VP_TEX_SLOTS]; /* per-frame VP texture slots    */
    VPVSEntry*            vp_vs;                /* per-draw decompiled VS cache  */
    int                   vp_vs_n, vp_vs_cap;
    VPFPEntry*            vp_fp;                /* guest-FP PSO cache            */
    int                   vp_fp_n, vp_fp_cap;
    /* VP path: latest texture bound per unit (t0-t3). */
    RcTexUnit             cur_texs[RSX_FP_UNITS];
    u32                   samp_n;               /* 4-sampler tables created so far */

    /* Render-to-texture: offscreen RT pool (their RTVs are views). */
    OffRT                 off_rt[MAX_OFF_RTS];
    /* DRAW_ARRAYS batch merging (see d3d12_draw_arrays): the vertex index one
     * past the last batch, and whether the current BEGIN/END may still be
     * extended. */
    u32                   merge_first_end;
    int                   merge_prev_draw;
    /* Indexed strip/fan merging (see d3d12_draw_indexed): the primitive of
     * the last draw when it was one, else 0. merge_first_end is its end. */
    int                   merge_strip;

    /* Frame-parity double buffering for the per-draw upload streams (vp_vb
     * vertices, vp_cb constants, vp_fpcb texscales): records for frame N+1
     * are written while frame N's GPU work may still be reading -- writes go
     * to the other half. Toggled at the end of render_frame. */
    int                   vp_parity;

    /* Per-frame draw recording */
    int                   frame_recording; /* 1 if cmd list is open for recording */
    u32                   draw_count;      /* draws this frame */
    /* MAX_DRAWS records (17.75 MB), made by rc_backend_init and kept for a
     * start after it. (As an array here they were in the exe's static data
     * once for each backend's copy of this file, d3d12, vulkan and null,
     * though only the running one's is used. docs/124.) */
    D3D12DrawRecord*      draws;

    /* Pointer to current RSX state (set before draw calls) */
    const rsx_state*      current_rsx_state;

    /* Current frame state */
    float clear_color[4];  /* RGBA float */

    /* Stats */
    u64 frame_count;
    u64 last_fps_time;
    u32 fps;

    int initialized;
} D3D12State;

static D3D12State s_d3d;
/* The GPU layer's objects (portable renderer, step C1): the device, the swap
 * chain and its images, the recorder begun last (open until frame_cmd_submit),
 * and each frame slot's fence. (Step C8a: s_d3d holds no native pointer to
 * any of them; the menu's exports borrow them in rsxc_api.inc.) */
static gpu_device*   s_rc_dev;
static gpu_swapchain s_rc_swap;
static gpu_texture   s_rc_swap_image[RC_SWAP_MAX];   /* by image index, s_rc_swap_n of them (step VC1) */
static gpu_cmd*      s_rc_cmd;
static gpu_fence     s_rc_frame_fence[FRAME_COUNT];
static gpu_texture   s_rc_rt_tex[FRAME_COUNT];   /* the display targets (step C5a) */
/* Step C3b: the root signatures, the pipelines and the shaders they are made
 * from are the GPU layer's (gpu_layout_create, gpu_pipeline_create,
 * gpu_shader_create), never released by the core (gpu_destroy frees them).
 * The core holds their handles (gpu_layout, gpu_pipeline: step C8a). The
 * pipelines made this run, from the GPU layer's library and compiled
 * (rc_pipeline): */
static volatile LONG s_rc_pipe_loaded, s_rc_pipe_made;
static int draw_room(void)
{
    if (s_d3d.draw_count < MAX_DRAWS) return 1;
    static u64 said = ~0ull; static int n = 0;
    if (said != s_d3d.frame_count && n < 20) { said = s_d3d.frame_count; n++;
        fprintf(stderr, "[D3D12] frame has more than %d draws: the rest are dropped%c",
                MAX_DRAWS, 10); }
    return 0;
}
extern char g_rsx_title_base[128];
static u32 s_dbg_last_draws = 0;
/* Room for one more draw record this frame? Says so once per frame when
 * not: those draws are dropped, and a frame cut short presents without its
 * end (Demon's Souls copies its finished image to the display last). */
static int draw_room(void);
/* Finish the vertex fetch threads' queued work (defined with them). */
static void vpj_sync(void);
/* F10: dump the next RSX_HOTKEY_SHOTS (default 12) presents, every one, to
 * RSX_HOTKEY_DIR as shot_<burst>_<n>_d<draws>.bmp. For a flicker someone
 * playing can see but a replay cannot show (a replay has no flips, so it
 * cuts frames at display clears instead): what was actually presented, one
 * file per present, with the batch's draw count in the name. */
static volatile int s_hotshot_left, s_hotshot_arm;
static int s_hotshot_burst = -1, s_hotshot_n;
static char s_hotshot_name[40];   /* F9's live presents: "cap_NNN", else "" */
/* GCM_RATES: why a frame boundary did or did not present (per-second deltas) */
static u32 s_clear_presents;   /* defined with the clear path */
static u32 s_pc_flip_render, s_pc_flip_nodisp, s_pc_clear_nodisp, s_pc_clear_sameflip, s_pc_clear_small;
static u64 s_req_verts = 0, s_req_draws = 0, s_drop_draws = 0;
/* The vertices vp_vb's chunks can hold in all (s_vb, below), for the logs and
 * flip_without_display's full test. The one buffer it was grew when a frame
 * asked for more: a Demon's Souls level frame filled all 393,216 and every
 * later draw was dropped -- the rest of the scene, the bloom/luminance/
 * tone-map chain and the final composite -- so the exposure never updated
 * and the level stayed dark. */
static u32 s_vp_cap = MAX_VERTICES;
static u64 s_vp_want = 0;   /* vertices this frame's VP draws asked for */
/* VP index buffer capacity, in indices per parity half, and what this frame's
 * indexed uploads asked for. 0 until the first frame end creates it. */
static u32 s_vp_icap = 0;
static u32 s_vf_cap = 0;    /* bytes per parity half of a vf_buf chunk (s_vf), a power of two */
static u32 s_vf_shift = 0;  /* its log2: chunk k starts at virtual offset k << s_vf_shift */
static u32 s_vf_off = 0;    /* bytes used in this frame, as a virtual offset (rc_arena) */
/* The chunk this frame writes in: virtual [s_vf_lo, s_vf_end), both 0 until
 * the frame's first descriptor, and s_vf_wp its half for this frame (the
 * CPU's pointer to s_vf_lo). Kept so a draw's upload neither divides nor
 * looks the chunk up (upload_strip_vp_fetch). */
static u32 s_vf_lo = 0, s_vf_end = 0;
static u8* s_vf_wp = NULL;
/* The last GPU-fetch descriptor (upload_strip_vp_fetch). Its offsets are
 * virtual ones, in the chunk this frame writes in. */
static struct {
    int valid; u32 epoch, fdesc, end, nsp;
    u32 g0[16], g1[16], r0[16];   /* guest [g0,g1) copied to ring offset r0 */
    u32 ve[16];                   /* each span's VF_ONCE entry if copied here, or ~0 */
    u32 x[16];                    /* the records' x, slot by slot (a record holds it less s_vf_lo) */
} s_fx;
/* The span copies made in this stretch of the stream (upload_strip_vp_fetch,
 * VF_ONCE), by guest start. A stretch ends at each point where the title can
 * see the RSX move on (g_rsx_sync_hook) and at each frame's ring reset; an
 * entry of an older stretch is empty. */
#define VFO_SETS 256u
#define VFO_WAYS 4u
typedef struct { u32 g0, g1, r0, ep; } VFOEntry;
static VFOEntry s_vfo[VFO_SETS][VFO_WAYS];
static u8 s_vfo_rr[VFO_SETS];   /* the way a set's next entry replaces when all are this stretch's */
static u32 s_vfo_ep = 1;
/* The same entries by the 64 KB pages they cover (VFO_PAGES at most each),
 * for a span that starts inside a copy: an entry index and its stretch. A
 * slot can name an entry that holds another copy since; the lookup checks
 * the entry itself. */
#define VFO_PSETS 512u
#define VFO_PWAYS 8u
#define VFO_PAGES 16u
static struct { u32 ep[VFO_PWAYS]; u16 e[VFO_PWAYS]; u8 rr; } s_vfop[VFO_PSETS];
static u64 s_vfo_copied, s_vfo_reused;   /* span bytes, for VP_FETCH_STATS */
/* Per 64 KB guest page, the stretch a span copy last read it in (vf_copy,
 * vpj_sync_write_hook). */
static u32* s_vpg;
static void vfo_next(void)
{
    if (++s_vfo_ep == 0) { memset(s_vfo, 0, sizeof s_vfo); memset(s_vfop, 0, sizeof s_vfop); s_vfo_ep = 1; }
}
static u64 s_vp_iwant = 0;
/* Step C2: the buffers are the GPU layer's. The fixed-function vertex buffer,
 * the per-draw rings (vp_vb, vp_ib, vp_cb, vp_fpcb, vf_buf) and the dump's
 * readback buffer are gpu_buffer_create's; upload ones are mapped once, for
 * their life (step C8a: made? is the handle's, no native field holds them).
 * Their GPU addresses are gpu_buffer_addr's (per draw
 * gpu_fast_buffer_addr's) plus today's offsets. */
static gpu_buffer s_rc_vertex_buf;     /* the fixed-function vertex buffer (vb_mapped) */
static gpu_buffer s_rc_vp_ib;          /* their indices (vp_ib_mapped) */
static gpu_buffer s_rc_readback_buf;   /* the dump's readback buffer */
/* The ring vp_ib_grow replaced ([1]; [0] was vp_vb's, before its chunks,
 * rc_arena), with the frame slot that last drew from it: destroyed with that
 * frame's fence at the next render_frame's start (rc_ring_release), where
 * today's deferred release let them go. */
static gpu_buffer s_rc_ring_old[2];
static u32        s_rc_ring_old_slot[2];

/* A committed buffer of `bytes` in GPU_MEM_UPLOAD (GENERIC_READ) or
 * GPU_MEM_READBACK (COPY_DEST) memory, as *b and, with map, its pointer for
 * life in *map (NULL if the map failed). 0 when none could be made. */
static int rc_buf_make(u64 bytes, u32 use, u32 memory, gpu_buffer* b, void** map)
{
    gpu_buffer_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = bytes;
    bd.use = use;
    bd.memory = memory;
    bd.state = memory == GPU_MEM_READBACK ? GPU_STATE_COPY_DEST : GPU_STATE_GENERIC_READ;
    *b = gpu_buffer_create(s_rc_dev, &bd);
    if (map) *map = GPU_VALID(*b) ? gpu_buffer_map(s_rc_dev, *b, 0, 0) : NULL;
    return GPU_VALID(*b);
}

/* Ring i's buffer, replaced while this frame is recorded (one growth per
 * frame): it goes once the GPU is past this frame. */
static void rc_ring_retire(int i, gpu_buffer old)
{
    if (!GPU_VALID(old)) return;
    s_rc_ring_old[i] = old;
    s_rc_ring_old_slot[i] = s_d3d.frame_index;
}

/* At render_frame's start, after its wait: the rings the last frame replaced,
 * each with the fence of the frame that drew from it (passed by now). */
static void rc_ring_release(void)
{
    for (int i = 0; i < 2; i++) {
        if (!GPU_VALID(s_rc_ring_old[i])) continue;
        gpu_buffer_destroy(s_rc_dev, s_rc_ring_old[i], s_rc_frame_fence[s_rc_ring_old_slot[i]]);
        s_rc_ring_old[i].id = 0;
    }
}

/* The per-draw upload buffers in chunks (docs/124). Each was one upload heap
 * sized for the worst frame (vp_vb 768 MB at the port's VP_VB_VERTS, vp_cb
 * 264 MB, vp_fpcb 80 MB), and an upload heap sits in RAM whole, used or not.
 * Now each is a list of chunks of one size, each with its two parity halves.
 * A frame's data sits at a virtual offset: chunk = offset / half, the rest
 * the offset in that chunk's half, and nothing lies across a chunk's end
 * (rc_arena_fit). The GPU sees the same bytes at the same offsets inside a
 * chunk; a draw binds the chunk it reads. Chunks are made as frames need them
 * and kept. When a frame gets past half of the last chunk, one more is made
 * at its end (rc_arena_frame_end), so a frame that grows a little never waits
 * for a new buffer; only a frame that jumps past that makes one in its middle.
 * One piece bigger than a chunk (a single draw's vertices) goes to the
 * arena's big buffer, at the virtual offsets from RC_ARENA_MAX chunks on. */
#define RC_ARENA_MAX 64u
typedef struct {
    const char* name;            /* the log's */
    u32 use;                     /* GPU_USE_* */
    u64 half;                    /* bytes per parity half of a chunk */
    u32 shift;                   /* log2(half) when half is a power of two (no divide per draw), else 0 */
    u32 n, most;                 /* chunks made; at most */
    u32 hi;                      /* chunks this frame reached (the highest written + 1) */
    u32 ahead;                   /* eighths of the last chunk a frame passes for one more to be made */
    u32 peak;                    /* the most any frame reached (said once per new peak) */
    u64 end;                     /* the end of what this frame placed (virtual, chunks only) */
    u64 used[RC_ARENA_MAX];      /* each chunk's end of what this frame placed (in the chunk) */
    gpu_buffer buf[RC_ARENA_MAX];
    u8* map[RC_ARENA_MAX];
    gpu_addr va[RC_ARENA_MAX];
    /* The big buffer: two halves of big_half bytes, made when a piece bigger
     * than a chunk first comes, bumped from big_off each frame. A frame that
     * outgrows it after placing something in it makes a bigger one at its end
     * (the old one goes with the fence of the frame that last read it). */
    gpu_buffer big, big_old;
    u8* big_map;
    gpu_addr big_va;
    u64 big_half, big_off, big_want;
    u32 big_old_slot;
} rc_arena;

/* The chunk of virtual offset `off` (RC_ARENA_MAX or more: the big buffer). */
static inline u64 rc_arena_k(const rc_arena* a, u64 off)
{
    return a->shift ? off >> a->shift : off / a->half;
}

/* Make chunks up to index k. 1 when chunk k is there. */
static int rc_arena_make(rc_arena* a, u32 k)
{
    while (a->n <= k) {
        if (a->n >= a->most) return 0;
        gpu_buffer b; void* mp = NULL;
        if (!rc_buf_make(a->half * 2, a->use, GPU_MEM_UPLOAD, &b, &mp) || !mp) {
            if (GPU_VALID(b)) gpu_buffer_destroy(s_rc_dev, b, 0);   /* made, not mapped: never used */
            fprintf(stderr, "[RAM] %s: chunk %u (%llu MB) could not be made; staying at %u%c",
                    a->name, a->n, (unsigned long long)(a->half * 2 >> 20), a->n, 10);
            a->most = a->n;
            return 0;
        }
        a->buf[a->n] = b;
        a->map[a->n] = (u8*)mp;
        a->va[a->n] = gpu_buffer_addr(s_rc_dev, b);
        a->n++;
    }
    return 1;
}

/* The big buffer at `half` bytes a parity half (the old one, if any, goes
 * with this frame's fence at the next render_frame). 1 when it is there. */
static int rc_arena_big_make(rc_arena* a, u64 half)
{
    gpu_buffer b; void* mp = NULL;
    if (!rc_buf_make(half * 2, a->use, GPU_MEM_UPLOAD, &b, &mp) || !mp) {
        if (GPU_VALID(b)) gpu_buffer_destroy(s_rc_dev, b, 0);
        fprintf(stderr, "[RAM] %s: a big buffer of %llu MB could not be made%c", a->name,
                (unsigned long long)(half * 2 >> 20), 10);
        return 0;
    }
    if (GPU_VALID(a->big)) {
        if (GPU_VALID(a->big_old)) gpu_buffer_destroy(s_rc_dev, a->big_old, s_rc_frame_fence[a->big_old_slot]);
        a->big_old = a->big;
        a->big_old_slot = s_d3d.frame_index;
    }
    a->big = b;
    a->big_map = (u8*)mp;
    a->big_va = gpu_buffer_addr(s_rc_dev, b);
    a->big_half = half;
    fprintf(stderr, "[RAM] %s: big buffer for pieces over a chunk: %llu MB%c", a->name,
            (unsigned long long)(half * 2 >> 20), 10);
    return 1;
}

/* An arena of `start` chunks of `half` bytes a parity half (at most `most`,
 * capped at RC_ARENA_MAX). 1 when the first chunk is there. */
static int rc_arena_init(rc_arena* a, const char* name, u32 use, u64 half, u32 start, u32 most)
{
    memset(a, 0, sizeof *a);
    a->name = name; a->use = use; a->half = half; a->ahead = 4;
    if (half && !(half & (half - 1))) while ((1ull << a->shift) < half) a->shift++;
    a->most = most < RC_ARENA_MAX ? most : RC_ARENA_MAX;
    if (!start) start = 1;
    rc_arena_make(a, start - 1);
    fprintf(stderr, "[RAM] %s: %u chunk(s) of %llu MB (%llu MB a half), up to %u%c", name, a->n,
            (unsigned long long)(half * 2 >> 20), (unsigned long long)(half >> 20), a->most, 10);
    return a->n > 0;
}

/* Where `bytes` go at or after virtual offset `at` without crossing a chunk's
 * end: `at` itself, or the next chunk's start (made now if it is not there:
 * the frame jumped past the spare). More than a chunk goes to the big buffer
 * (made now if this frame has nothing in it yet). ~0 when they cannot be
 * placed: no buffer could be made, or the big buffer is in use this frame
 * and too small (it grows at the frame's end). */
static u64 rc_arena_fit(rc_arena* a, u64 at, u64 bytes)
{
    if (bytes > a->half) {
        const u64 o = (a->big_off + 255u) & ~255ull;
        if (o + bytes > a->big_want) a->big_want = o + bytes;
        if (o + bytes > a->big_half &&
            (a->big_off || !rc_arena_big_make(a, (bytes + bytes / 2 + 0xFFFFFull) & ~0xFFFFFull)))
            return ~0ull;
        a->big_off = o + bytes;
        return (u64)RC_ARENA_MAX * a->half + o;
    }
    u64 k = rc_arena_k(a, at);
    if (at - k * a->half + bytes > a->half) { k++; at = k * a->half; }
    if (k >= a->n) {
        if (k >= a->most || !rc_arena_make(a, (u32)k)) return ~0ull;
        fprintf(stderr, "[RAM] %s: a frame needed chunk %u in its middle (%u made)%c",
                a->name, (u32)k + 1, a->n, 10);
    }
    if ((u32)k + 1 > a->hi) a->hi = (u32)k + 1;
    if (at + bytes > a->end) a->end = at + bytes;
    if (at - k * a->half + bytes > a->used[k]) a->used[k] = at - k * a->half + bytes;
    return at;
}

/* Parity half `par` of the chunk (or the big buffer) that holds virtual
 * offset `off`: the CPU's pointer to the byte, and the GPU's address of it. */
static inline u8* rc_arena_ptr_par(const rc_arena* a, u64 off, u32 par)
{
    const u64 k = rc_arena_k(a, off);
    if (k >= RC_ARENA_MAX)
        return a->big_map + (u64)par * a->big_half + (off - (u64)RC_ARENA_MAX * a->half);
    return a->map[k] + (u64)par * a->half + (off - k * a->half);
}
static inline u8* rc_arena_ptr(const rc_arena* a, u64 off)
{
    return rc_arena_ptr_par(a, off, (u32)s_d3d.vp_parity);
}
static inline gpu_addr rc_arena_va(const rc_arena* a, u64 off)
{
    const u64 k = rc_arena_k(a, off);
    if (k >= RC_ARENA_MAX)
        return a->big_va + (u64)s_d3d.vp_parity * a->big_half + (off - (u64)RC_ARENA_MAX * a->half);
    return a->va[k] + (u64)s_d3d.vp_parity * a->half + (off - k * a->half);
}
/* The GPU address of chunk k's half for this frame (a whole-chunk bind; k at
 * RC_ARENA_MAX or more is the big buffer), its size, and the chunk of an
 * offset with the offset inside it. */
static inline gpu_addr rc_arena_base(const rc_arena* a, u32 k)
{
    if (k >= RC_ARENA_MAX) return a->big_va + (u64)s_d3d.vp_parity * a->big_half;
    return a->va[k] + (u64)s_d3d.vp_parity * a->half;
}
static inline u64 rc_arena_size(const rc_arena* a, u32 k)
{
    return k >= RC_ARENA_MAX ? a->big_half : a->half;
}
static inline u32 rc_arena_chunk(const rc_arena* a, u64 off)
{
    const u64 k = rc_arena_k(a, off);
    return k >= RC_ARENA_MAX ? RC_ARENA_MAX : (u32)k;
}
static inline u64 rc_arena_local(const rc_arena* a, u64 off)
{
    const u32 k = rc_arena_chunk(a, off);
    return off - (u64)k * a->half;
}
/* The frame wrote up to virtual offset `end` (chunks only): for the users that
 * place by their own counters and ask rc_arena_fit only for a chunk's first
 * piece (vf_buf, the banks), so the chunk's end (used) and the chunk ahead
 * at the frame's end see what they wrote. */
static void rc_arena_mark(rc_arena* a, u64 end)
{
    if (!end) return;
    const u64 k = rc_arena_k(a, end - 1);
    if (k >= a->n) return;
    if (end - k * a->half > a->used[k]) a->used[k] = end - k * a->half;
    if (end > a->end) a->end = end;
}

/* At the frame's end: a new peak goes to the log; when the frame got past
 * `ahead` eighths of the last chunk made (half, unless the user of the arena
 * says), one more is made ahead for the frames to come; a big buffer the
 * frame outgrew is made bigger. */
static void rc_arena_frame_end(rc_arena* a)
{
    if (a->hi > a->peak) {
        a->peak = a->hi;
        fprintf(stderr, "[RAM] %s: a frame used %u chunk(s) (%llu MB)%c", a->name, a->peak,
                (unsigned long long)(a->peak * a->half * 2 >> 20), 10);
    }
    if (a->n && a->n < a->most && a->end > (u64)(a->n - 1) * a->half + a->half / 8 * a->ahead)
        rc_arena_make(a, a->n);
    if (a->big_want > a->big_half)
        rc_arena_big_make(a, (a->big_want + a->big_want / 2 + 0xFFFFFull) & ~0xFFFFFull);
    a->hi = 0; a->end = 0; a->big_off = 0; a->big_want = 0;
    memset(a->used, 0, sizeof a->used);
}

/* At render_frame's start, after its wait (as rc_ring_release): a big buffer
 * replaced since, with the fence of the frame that last read it. */
static void rc_arena_release(rc_arena* a)
{
    if (!GPU_VALID(a->big_old)) return;
    gpu_buffer_destroy(s_rc_dev, a->big_old, s_rc_frame_fence[a->big_old_slot]);
    a->big_old.id = 0;
}

/* The VP vertex buffer (vp_vb) in chunks of s_vb_chunk vertices a parity half
 * (VP_VB_CHUNK). Draws place their vertices with vb_room (rsxc_vertex.inc); a
 * draw's vb_byte_offset is its virtual offset, and render_frame binds the
 * chunk that holds it. */
static rc_arena s_vb;
static u32 s_vb_chunk = 131072;
static u64 s_vb_big = ~0ull;   /* the big buffer's first virtual offset (RC_ARENA_MAX chunks in) */
static u32 s_vb_at;            /* where the last vb_room put its vertices */
static u64 s_vb_cursor;        /* the chunks' place while vp_vb_offset is in the big buffer */
static u32 s_vb_carry;         /* for the next vb_room: the open triangle's vertices it copies ahead (tri_carry) */
static u32 s_vb_carried;       /* the vertices the last vb_room copied ahead of the draw's (at s_vb_at) */
static u32 s_vb_most;          /* the biggest draw so far, in vertices */
/* VP_VSTAT=<N> (live): every N frames, print the VP uploads' vertex traffic:
 * vertices fetched from guest memory, vertices written to the upload heap,
 * uploads by primitive, and the mean enabled-attribute count per fetch. */
static u64 s_vs_fetch, s_vs_attr, s_vs_up[4];
/* The vertex fetch ring (vf_buf) as chunks (rc_arena; made in rsxc_init.inc's
 * "GPU vertex fetch ring"). Here rather than with the s_vf_* above, which
 * come before rc_arena's type. */
static rc_arena s_vf;
/* The per-draw constants in chunks (docs/124 steps 3 and 4). They were one
 * upload heap each for MAX_DRAWS draws (vp_cb 264 MB, vp_fpcb 80 MB), and a
 * frame uses a few hundred banks and a few thousand draws. vp_cb holds the
 * vertex-constant banks by bank number (vp_record_cb numbers a frame's banks
 * as it writes them, and VP_CB_REUSE has draws share one); vp_fpcb holds each
 * draw's b1 block by draw number (cb_slot). The pointers below are given the
 * parity half: a capture, made after the frame's flip, reads the other one. */
static rc_arena s_rc_vpcb, s_rc_fpcb;
static inline int vpcb_has(u32 bank) { return (bank >> VP_CB_CHUNK_SHIFT) < s_rc_vpcb.n; }
static inline int fpcb_has(u32 slot) { return (slot >> VP_FPCB_CHUNK_SHIFT) < s_rc_fpcb.n; }
static inline u8* vpcb_ptr(u32 bank, u32 par)
{
    return s_rc_vpcb.map[bank >> VP_CB_CHUNK_SHIFT] + (u64)par * s_rc_vpcb.half
         + (u64)(bank & VP_CB_CHUNK_MASK) * VP_CB_STRIDE;
}
static inline u8* fpcb_ptr(u32 slot, u32 par)
{
    return s_rc_fpcb.map[slot >> VP_FPCB_CHUNK_SHIFT] + (u64)par * s_rc_fpcb.half
         + (u64)(slot & VP_FPCB_CHUNK_MASK) * VP_FPCB_STRIDE;
}
/* Raw bind offset of the duck's texture, identified by CONTENT (duck.tga is
 * overwhelmingly yellow, avg 243,191,23). VRAM offsets move between runs, so a
 * hard-coded offset in a filter silently matches nothing and the run looks like
 * "renders nothing" -- which cost real time. DRAW_KEEP_TEX=duck resolves here. */
/* Copy of the last completed frame, bound wherever a draw samples a
 * DISPLAY-SIZED texture. On RSX this title renders its reflection into a corner
 * of the render surface and then samples that surface as a texture; our backend
 * renders into a D3D backbuffer, so the guest memory behind that sampler is
 * never written and it reads empty -- which is why the water had no reflection.
 * Feeding it the previous frame costs one frame of latency, which for a water
 * reflection is not visible. (The copy is s_rc_screen_copy_tex, below.) */
/* Sub-viewport render-to-texture. This title renders its reflection/refraction
 * pre-pass into a REGION of the same surface (vp 0,208 512x512 and 0,208
 * 1024x512, cmask=F) and then samples a texture of exactly that size. On
 * hardware the pass lands in the buffer that texture points at; our backend
 * renders into D3D resources, so that guest buffer is never written and every
 * fluid sampler reads zero. Capture each region into a matching resource and
 * bind it for samplers whose guest source is empty and whose size matches.
 *
 * SUBVP_RTT=1 to enable. OFF by default because it cannot be validated on this
 * title yet: the fluid draws that would consume these captures never rasterize
 * (see below), so binding them changes nothing observable. The capture and
 * binding themselves are confirmed working -- SUBVP_DBG shows the 512x512 and
 * 256x256 regions captured and handed to fp 0x5DB81/0x5EA81/0x5EF81/0x5E381.
 * Each capture is a texture of the GPU layer's (step C8a). */
#define SUBVP_SLOTS 4
static struct { gpu_texture tex;
                u32 x, y, w, h; } s_subvp[SUBVP_SLOTS];
static int s_subvp_n = 0;

static void subvp_note(u32 x, u32 y, u32 w, u32 h)
{
    if (!w || !h || w > s_d3d.width || h > s_d3d.height) return;
    if (w == s_d3d.width && h == s_d3d.height) return;      /* full-screen pass */
    for (int i = 0; i < s_subvp_n; i++)
        if (s_subvp[i].w == w && s_subvp[i].h == h) { s_subvp[i].x = x; s_subvp[i].y = y; return; }
    if (s_subvp_n >= SUBVP_SLOTS) return;
    s_subvp[s_subvp_n].x = x; s_subvp[s_subvp_n].y = y;
    s_subvp[s_subvp_n].w = w; s_subvp[s_subvp_n].h = h;
    s_subvp[s_subvp_n].tex.id = 0;
    s_subvp_n++;
}

/* Copy each noted region out of the backbuffer. Called at the same moment as
 * screen_copy_capture -- after the reduced-viewport passes and before the
 * full-screen scene overwrites them. The capture is made, and the region
 * copied into it, on the GPU layer (step C8a): today's committed RGBA8
 * texture with no flags, in PIXEL_SHADER_RESOURCE, and today's barriers and
 * copy on the frame's recorder (s_rc_rt_tex[fi] is the display target). */
static void subvp_capture(u32 fi)
{
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("SUBVP_RTT", 0));
    if (!en || !s_rc_dev || !s_subvp_n) return;
    for (int i = 0; i < s_subvp_n; i++) {
        if (!GPU_VALID(s_subvp[i].tex)) {
            gpu_texture_desc td;
            memset(&td, 0, sizeof td);
            td.width  = (u32)((u64)s_subvp[i].w * s_d3d.host_w / s_d3d.width);
            td.height = s_subvp[i].h * s_d3d.host_h / s_d3d.height;
            td.layers = 1; td.mips = 1;
            td.format = GPU_FORMAT_R8G8B8A8_UNORM; td.samples = 1;
            td.use = GPU_USE_SAMPLED | GPU_USE_COPY_DST;
            td.state = GPU_STATE_PIXEL_SHADER_RESOURCE;
            s_subvp[i].tex = gpu_texture_create(s_rc_dev, &td);
            if (!GPU_VALID(s_subvp[i].tex)) continue;
        }
        gpu_barrier bb[2];
        memset(bb, 0, sizeof bb);
        bb[0].type    = GPU_BARRIER_TRANSITION;
        bb[0].texture = s_rc_rt_tex[fi];
        bb[0].sub     = GPU_SUB_ALL;
        bb[0].before  = GPU_STATE_RENDER_TARGET;
        bb[0].after   = GPU_STATE_COPY_SOURCE;
        bb[1] = bb[0];
        bb[1].texture = s_subvp[i].tex;
        bb[1].before  = GPU_STATE_PIXEL_SHADER_RESOURCE;
        bb[1].after   = GPU_STATE_COPY_DEST;
        gpu_cmd_barrier_n(s_rc_cmd, 2, bb);

        const u32 hx = s_subvp[i].x * s_d3d.host_w / s_d3d.width;
        const u32 hy = s_subvp[i].y * s_d3d.host_h / s_d3d.height;
        gpu_box box; box.left = hx; box.top = hy;
        box.front = 0; box.right = hx + s_subvp[i].w * s_d3d.host_w / s_d3d.width;
        box.bottom = hy + s_subvp[i].h * s_d3d.host_h / s_d3d.height; box.back = 1;
        if (box.right > s_d3d.host_w)  box.right  = s_d3d.host_w;
        if (box.bottom > s_d3d.host_h) box.bottom = s_d3d.host_h;
        gpu_cmd_copy_texture(s_rc_cmd, s_subvp[i].tex, 0, 0, 0, 0, s_rc_rt_tex[fi], 0, &box);

        bb[0].before = GPU_STATE_COPY_SOURCE;
        bb[0].after  = GPU_STATE_RENDER_TARGET;
        bb[1].before = GPU_STATE_COPY_DEST;
        bb[1].after  = GPU_STATE_PIXEL_SHADER_RESOURCE;
        gpu_cmd_barrier_n(s_rc_cmd, 2, bb);
        { static int _n = 0; if (getenv("SUBVP_DBG") && _n < 8 && ++_n)
            fprintf(stderr, "[SUBVP] captured %ux%u from (%u,%u)%c",
                    s_subvp[i].w, s_subvp[i].h, s_subvp[i].x, s_subvp[i].y, 10); }
    }
}
static void screen_copy_capture(u32 fi);   /* fwd */
static int  s_sc_dump_pending = 0;
static u32 s_duck_raw = 0;   /* as BOUND (what draw records carry) */
static u32 s_duck_off = 0;   /* as RESOLVED (what the uploader sees)   */
/* PERF=1: where a frame's CPU time actually goes. Guessing at this is how you
 * spend an afternoon optimising the wrong loop. */
static double s_perf_tex = 0.0, s_perf_frame = 0.0, s_perf_vtx = 0.0, s_perf_rf = 0.0;
static double s_perf_gpu = 0.0, s_perf_pre = 0.0, s_perf_srv = 0.0, s_perf_pso = 0.0;
static int s_perf_pso_calls = 0, s_perf_pso_miss = 0, s_perf_pso_hashbytes = 0;
/* Vertex programs decompiled + compiled at draw-record time (vp_get_vs).
 * Booked under "guest", not render_frame, since it runs while recording. */
static int s_perf_vs_compiles = 0; static double s_perf_vs = 0.0;
static u64    s_perf_nverts = 0;
static u64    s_perf_texbytes = 0;
static int    s_perf_ntex = 0;
static double perf_now(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static volatile long s_prof_on;   /* a profile session is recording (rc_prof_begin) */
extern volatile long g_rsx_prof_toggle;   /* bumped per F11 press; boot_main starts/stops */
static int perf_print(void)
{
    static int v = -1;
    PS3_KNOB(v, ps3_env_int("PERF", 0));
    return v;
}
/* PERF=1 prints the [PERF] lines; a profile session turns the counters on
 * without them. */
static int perf_on(void) { return perf_print() || s_prof_on; }
/* DBG_LOCK: running clip-space centroid of the tracked mesh (RSX_DBG_VTX's
 * texture), so the debug camera can follow it. The ducks are driven by the
 * physics sim and drift every frame, so a fixed DBG_CENTER loses them as soon
 * as the magnification is high enough to make one recognisable. */
static float s_lock_x = 0.0f, s_lock_y = 0.0f;
static int   s_lock_valid = 0;
/* Accumulated over one frame, published at the frame boundary. Averaging as the
 * frame is recorded makes the aim wander while it is being used, which at high
 * magnification walks the target off-screen entirely. */
static double s_lock_sx = 0.0, s_lock_sy = 0.0;
static u32    s_lock_n  = 0;

/* Step C4: the shader-visible descriptor heaps are the GPU layer's table
 * pools, and every SRV and sampler write goes through them, the memo
 * included (gpu_table_write_tex; per draw gpu_fast_table_writer_tex), and
 * every bind names a pool (step C8a: no heap pointer is borrowed; made? is
 * the pool's handle). Every resource an SRV names
 * is a texture of the GPU layer's, by its handle (step C8a: the last ones
 * adopted, the sub-viewport captures, are its own now), so no write looks a
 * handle up. */
static gpu_table_pool s_rc_srv_pool;    /* the textures' (the draws, the passes) */
static gpu_table_writer s_rc_srv_w;     /* s_rc_srv_pool found once, for srv_write_ex (step C8a) */
static gpu_table_pool s_rc_samp_pool;   /* the guest's samplers */
static gpu_table_pool s_rc_blit_pool;   /* the blit's: one SRV per display target */
static gpu_table_pool s_rc_down_pool;   /* the write-back downscale's (rsxc_writeback.inc) */
static gpu_texture    s_rc_screen_copy_tex;       /* the screen copy: the GPU layer's (step C5b) */
static const gpu_texture k_rc_no_tex = {0};       /* none: an empty entry */

/* Step C5a: the colour and depth targets are the GPU layer's textures (the
 * display targets s_rc_rt_tex above, the depth buffer, zeta, the offscreen
 * pool), held by their handles (step C8a), and their RTVs and
 * DSVs are its views (gpu_view_get): no RTV or DSV heap is made. Every bind
 * and clear takes the view (step C8a: no CPU descriptor is kept beside it). */
static gpu_view       s_rc_rt_rtv[FRAME_COUNT];       /* the display targets' RTVs */
static gpu_texture    s_rc_depth_tex;                 /* the shared depth buffer */
static gpu_view       s_rc_depth_dsv;
static u32            s_rc_depth_w, s_rc_depth_h;     /* its size (host; depth_buffer_fit) */
static gpu_view       s_rc_swap_rtv[RC_SWAP_MAX];     /* the swap chain images' RTVs */

/* A colour (GPU_VIEW_RTV) or depth (GPU_VIEW_DSV) view of t, made once and
 * kept with t. Format 0 and flags 0 are today's NULL RTV description; DSVs
 * spell their format out, as today. */
static gpu_view rc_target_view(gpu_texture t, u32 kind, u32 flags, gpu_format format)
{
    gpu_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.texture = t;
    vd.kind = (uint8_t)kind;
    vd.dim = GPU_DIM_2D;
    vd.flags = (uint8_t)flags;
    vd.format = format;
    return gpu_view_get(s_rc_dev, &vd);
}

/* Step C6: the passes that run a few times a frame (the output filter, the
 * blit to the window, the HDR blit, the repaint hold, FXAA and SMAA, the
 * captures' readbacks) record on the GPU layer, on the frame's recorder
 * (s_rc_cmd). */

/* Step C8a: the core holds no native object. Every field that held one is
 * the handle it borrowed from (made? is GPU_VALID, or .v for a pipeline),
 * and nothing native is borrowed but the menu's objects (rsxc_api.inc,
 * through the instance file's hooks). */
static const gpu_pipeline k_rc_no_pipe = {0};      /* none */
/* The overlay's frame callback with the window's buffer fi (the menu's,
 * rsxc_api.inc). */
static void rc_overlay_frame(u32 fi);

/* Step VC1: a frame slot is not a swap chain image. The slot
 * (s_d3d.frame_index: the recorders, s_rc_frame_fence, the display targets,
 * the blit's tables) is the instance file's pick (rc_be_frame_slot); the
 * image (s_rc_image, below s_rc_swap_n) picks only the window's image and
 * its view. On D3D12 the two are the same number, as today: the swap chain
 * has FRAME_COUNT buffers and its index moves only at a present. On Vulkan
 * the driver may make more images and hands them out in any order. */
static u32 s_rc_image;    /* the image the last acquire gave */
static u32 s_rc_swap_n;   /* the swap chain's images (swap_buffers_get) */
/* Step VC2: the acquire gave an image. Always on D3D12; Vulkan gives none
 * while the window is minimized or its surface is lost, and blit_to_window
 * draws into nothing then (an image not acquired is not the core's). */
static int s_rc_have_image;
static void swap_follow(gpu_texture img);   /* (step VW, rsxc_hdr.inc) */
static void rc_acquire(void)
{
    gpu_texture img;
    s_rc_image = gpu_swap_acquire(s_rc_dev, s_rc_swap, &img);
    s_rc_have_image = GPU_VALID(img);
    if (s_rc_have_image) swap_follow(img);
    s_d3d.frame_index = rc_be_frame_slot(s_d3d.frame_index, s_rc_image);
}

/* Step V5: two forms for GPUs that lack a feature, picked once a device is
 * made (rc_forms_pick, rsxc_init.inc).
 * The sprite-edge clamp (dr_uvclamp): s_uvc_vs 0 is today's geometry shader
 * (uvclamp_gs), 1 its vertex-shader form (kind 4, rsxc_vs_inject_uvclamp),
 * where the draw becomes a list of triangles from a table (uvc_table).
 * RSX_CLAMP_FORM=gs|vs, by default gs where the device has GPU_CAP_GS.
 * The depth surfaces: s_rc_d32 0 is today's D24S8, 1 D32S8 (32-bit float
 * depth). RSX_DEPTH_FORMAT=d24s8|d32s8, by default d24s8 where the device has
 * GPU_CAP_D24S8. Nothing writes depth back to guest memory; a program that
 * reads depth as colour gets its 24 bits from rsx_d2c, which rounds the
 * float it samples to 24 bits either way. */
static int s_uvc_vs;
static int s_fp_vs4;      /* vp_get_fp_pso: the pipeline it returned is kind 4's */
static int s_rc_d32;
static gpu_format rc_ds_format(void)      /* the depth target and its views */
{
    return s_rc_d32 ? GPU_FORMAT_D32_FLOAT_S8X24_UINT : GPU_FORMAT_D24_UNORM_S8_UINT;
}
static gpu_format rc_ds_typeless(void)    /* a depth surface that is also sampled */
{
    return s_rc_d32 ? GPU_FORMAT_R32G8X24_TYPELESS : GPU_FORMAT_R24G8_TYPELESS;
}
static gpu_format rc_ds_depth_srv(void)   /* its depth, sampled */
{
    return s_rc_d32 ? GPU_FORMAT_R32_FLOAT_X8X24_TYPELESS : GPU_FORMAT_R24_UNORM_X8_TYPELESS;
}

