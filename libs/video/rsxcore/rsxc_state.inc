/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define FRAME_COUNT         2   /* double buffering */
#define MAX_VERTICES    393216  /* per-frame vertex buffer (dbgfont submits
                                 * ~7.5k verts/frame; leave generous headroom) */
/* Fragment texture units a draw records and binds: all sixteen the RSX has.
 * Four were tracked; units 4-15 were dropped at bind time, and a program that
 * samples one (Demon's Souls's level shaders use t4 and t7) declares a
 * register outside the root signature's range, so its pipeline failed to
 * build (E_INVALIDARG) and those surfaces never drew. */
#define RSX_FP_UNITS      16
#define MAX_DRAWS         16384 /* per-frame draw records. The per-draw
                                 * constant buffer is VP_CB_STRIDE * MAX_DRAWS
                                 * * 2 bytes (264 MB of upload heap). 2048,
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
/* b1 per-draw FP slot, in float4s: rsx_texscale[16], rsx_alphatest, then the
 * program's inline constants fp_constants[FPCB_KMAX]. (17 + 128) * 16 = 2320 B,
 * rounded up to D3D12's 256-byte CBV alignment. */
#define FPCB_ALPHA        16
#define FPCB_K            17
#define FPCB_KMAX         128
#define VP_FPCB_STRIDE    2560
#define VERTEX_STRIDE       36  /* bytes per host vertex: pos3 + col4 + uv2 */

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
    struct {
        u32 off;        /* resolved vm offset (guest upload source), 0 = none */
        u32 raw;        /* raw RSX offset (offscreen-RT matching) */
        u32 w, h, fmt;  /* dims + RSX base format */
        u32 ctrl1;      /* NV4097 TEXTURE_CONTROL1: component remap crossbar */
        u32 addr, filt; /* NV4097 TEXTURE_ADDRESS / TEXTURE_FILTER: sampler state;
                         * filt bits 20-22 (unused by the register) hold
                         * TEXTURE_CONTROL0's max anisotropy */
        u32 mips;       /* SET_TEXTURE_FORMAT bits 16..31: mipmap level count.
                         * Cube faces sit one whole mip pyramid apart, so this
                         * is what sets the face stride. */
        int cube;       /* SET_TEXTURE_FORMAT bit 2: a cube texture. Sampled
                         * with a 3-component direction, not a 2D uv -- treating
                         * one as 2D is what makes an environment-mapped chrome
                         * surface come out with black patches and banding. */
        int set;
    } tex[RSX_FP_UNITS];
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
    u32 cb_slot;
    u32 vcb_slot;       /* vp_cb slot bound: cb_slot, or an earlier draw's this
                         * frame whose vertex constants are the same (vp_record_cb) */
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
     * from vf_buf, and the indices are the guest's own. fdesc is the byte
     * offset of the draw's attribute records in vf_buf's half. fetch is 1
     * for a triangle list, 2 for a strip with cuts (vf_strip_native). */
    u32   fetch, fdesc;
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
    gpu_buffer            up_buf;      /* init-upload staging (kept alive): the GPU layer's (step C5a) */
    u32                   off, w, h;   /* raw RSX offset + guest dims */
    u32                   hw, hh;      /* the resource's dims (RSX_SCALE) */
    u32                   dxgi;        /* DXGI_FORMAT of the resource */
    u32                   gdxgi;       /* the guest format's (refill, write-back): FP16 dxgi for an
                                        * RGBA8 target HDR promotes (rt_dxgi) */
    D3D12_RESOURCE_STATES st;          /* tracked resource state */
    int                   used;        /* referenced this frame */
    gpu_buffer            rb_buf;      /* colour write-back readback buffer: the GPU layer's (step C5b) */
    u64                   rb_size;
    u32                   gfmt, gpitch;/* guest SET_SURFACE_FORMAT word + pitch */
    u32                   gdma;        /* guest context DMA (local / main) */
    gpu_texture           snap_tex;    /* copy sampled by draws that also render here (step C5b) */
    u32                   snap_w, snap_h, snap_dxgi;
    D3D12_RESOURCE_STATES snap_st;
    gpu_texture           down_tex;    /* guest-size copy of a scaled target (write-back, step C5b) */
    u32                   down_w, down_h, down_dxgi;
    D3D12_RESOURCE_STATES down_st;
    /* Guest bytes after our last write-back (tex_csum over [gaddr, +glen)).
     * If they differ later, the title put something else there. */
    u32                   gaddr, glen, gsum;
    int                   gsum_ok, gsum_new;
    u32                   gpu_dst;     /* used == 3: the GPU stored it at this guest address */
    int                   gpu_check;   /* RSX_WB_GPU=2: the CPU store goes to a scratch copy to compare */
} OffRT;
#define MAX_OFF_RTS  16  /* demosaic double-buffers its 6-surface pass chain */
#define RT_SRV_BASE  5   /* SRV heap slots 5..20 hold offscreen-RT SRVs */
/* Per-draw SRV windows: each VP draw gets RSX_FP_UNITS consecutive descriptors
 * (t0-t15) so multi-unit fragment programs see all their textures. */
#define DRAW_SRV_BASE 32
#define VP_SAMP_TABLES (2048 / RSX_FP_UNITS)
                             /* per-draw sampler tables (D3D12 caps a shader-
                              * visible sampler heap at 2048 samplers). When all
                              * are taken, the least recently used one the GPU
                              * has finished with is rewritten (vp_sampler_table). */
/* Each texture slot's own SRV, after the windows. It used to sit at heap
 * index 1+slot, inside the windows from slot 31 on: once a session had
 * cached that many textures, each new upload rewrote a descriptor of one of
 * the frame's first 63 draws. */
#define VP_TEX_SRV_BASE (DRAW_SRV_BASE + MAX_DRAWS * RSX_FP_UNITS)
#define SRV_HEAP_SIZE (VP_TEX_SRV_BASE + VP_TEX_SLOTS)

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
    int uses_c03;
    int fetch;              /* reads vf_buf (rsx_vp_decompile_fetch): no input layout */
    u64 code64;             /* the pipeline index's fingerprint: FNV-1a 64 of the words up to END */
    u32 start;              /* the transform program start it was decompiled at */
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
    void*                 vf_mapped;            /* GPU vertex fetch ring (x2 parity): s_rc_vf_buf */
    gpu_pipeline          pipeline_state_vp;    /* decompiled VS + atlas PS      */
    gpu_pipeline          pipeline_state_vp_color; /* decompiled VS + colour PS (untextured 3D) */
    void*                 vp_vb_mapped;         /* raw float4 attrib0, per-frame: s_rc_vp_vb */
    u32                   vp_vb_offset;
    void*                 vp_ib_mapped;         /* u32 indices of the VP draws, per-frame: s_rc_vp_ib */
    u32                   vp_ib_offset;         /* in indices */
    void*                 vp_cb_mapped;         /* per-draw VP constant slots: s_rc_vp_cb */
    void*                 vp_fpcb_mapped;       /* per-draw FP texscale (b1): s_rc_vp_fpcb */
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
    struct { u32 off, raw, w, h, fmt, ctrl1, mips, addr, filt; int cube; int set; } cur_texs[RSX_FP_UNITS];
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
    D3D12DrawRecord       draws[MAX_DRAWS];

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
static gpu_texture   s_rc_swap_image[FRAME_COUNT];
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
/* VP vertex buffer capacity, in vertices per frame-parity half. Starts at
 * MAX_VERTICES and grows when a frame asks for more (vp_vb_grow): a Demon's
 * Souls level frame filled all 393,216 and every later draw was dropped --
 * the rest of the scene, the bloom/luminance/tone-map chain and the final
 * composite -- so the exposure never updated and the level stayed dark. */
static u32 s_vp_cap = MAX_VERTICES;
static u64 s_vp_want = 0;   /* vertices this frame's VP draws asked for */
/* VP index buffer capacity, in indices per parity half, and what this frame's
 * indexed uploads asked for. 0 until the first frame end creates it. */
static u32 s_vp_icap = 0;
static u32 s_vf_cap = 0;    /* bytes per parity half of vf_buf */
static u32 s_vf_off = 0;    /* bytes used in this frame's half */
/* The last GPU-fetch descriptor (upload_strip_vp_fetch). */
static struct {
    int valid; u32 epoch, fdesc, end, nsp;
    u32 g0[16], g1[16], r0[16];   /* guest [g0,g1) copied to ring offset r0 */
    u32 x[16];                    /* the records' x, slot by slot */
} s_fx;
static u64 s_vp_iwant = 0;
/* Step C2: the buffers are the GPU layer's. The fixed-function vertex buffer,
 * the per-draw rings (vp_vb, vp_ib, vp_cb, vp_fpcb, vf_buf) and the dump's
 * readback buffer are gpu_buffer_create's; upload ones are mapped once, for
 * their life (step C8a: made? is the handle's, no native field holds them).
 * Their GPU addresses are gpu_buffer_addr's (per draw
 * gpu_fast_buffer_addr's) plus today's offsets. */
static gpu_buffer s_rc_vertex_buf;     /* the fixed-function vertex buffer (vb_mapped) */
static gpu_buffer s_rc_vp_vb;          /* the VP draws' vertices (vp_vb_mapped) */
static gpu_buffer s_rc_vp_ib;          /* their indices (vp_ib_mapped) */
static gpu_buffer s_rc_vp_cb;          /* their constants (vp_cb_mapped) */
static gpu_buffer s_rc_vp_fpcb;        /* their FP texscales (vp_fpcb_mapped) */
static gpu_buffer s_rc_vf_buf;         /* the vertex fetch ring (vf_mapped) */
static gpu_buffer s_rc_readback_buf;   /* the dump's readback buffer */
/* The rings vp_vb_grow and vp_ib_grow replaced ([0] vp_vb, [1] vp_ib), with
 * the frame slot that last drew from each: destroyed with that frame's fence
 * at the next render_frame's start (rc_ring_release), where today's deferred
 * release let them go. */
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
/* VP_VSTAT=<N> (live): every N frames, print the VP uploads' vertex traffic:
 * vertices fetched from guest memory, vertices written to the upload heap,
 * uploads by primitive, and the mean enabled-attribute count per fetch. */
static u64 s_vs_fetch, s_vs_attr, s_vs_up[4];
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
static gpu_view       s_rc_swap_rtv[FRAME_COUNT];     /* the swap chain images' RTVs */

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
 * and none is borrowed through gpu_native but the menu's (rsxc_api.inc). */
static const gpu_pipeline k_rc_no_pipe = {0};      /* none */
/* The overlay's frame callback with the window's buffer fi (the menu's,
 * rsxc_api.inc). */
static void rc_overlay_frame(u32 fi);

