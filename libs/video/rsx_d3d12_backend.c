/*
 * ps3recomp - D3D12 RSX Backend
 *
 * Translates RSX GPU state to D3D12 rendering commands.
 *
 * Phase 1 implementation:
 *   - Win32 window + D3D12 device + swap chain
 *   - Clear render target to RSX clear color
 *   - Present with vsync
 *   - Basic vertex-colored triangle rendering
 *
 * This file is C with COM calls (D3D12 is a COM API).
 * We use the C interface (__uuidof not available in C, so we
 * define GUIDs manually).
 */

#ifdef _WIN32

#include <time.h>
#include "ps3emu/ps3_knob.h"

/* Debug gates on the per-draw and per-texture-bind paths. A getenv on each
 * call was ~3% of the FIFO thread in a heavy view (~200k lookups a second). */
static int knob_vtx_dump(void)   { static int v = -1; PS3_KNOB(v, getenv("VTX_DUMP") ? 1 : 0); return v; }
static int dbg_movie_bind(void) { static int v = -1; PS3_KNOB(v, getenv("MOVIE_BIND") ? 1 : 0); return v; }
static int dbg_texfmt(void)     { static int v = -1; PS3_KNOB(v, getenv("TEXFMTDBG") ? 1 : 0); return v; }
/* RSX_PRESENT=0: render every frame but never Present it. Behind the lock
 * screen DWM holds each back buffer 250-500 ms without reporting occlusion,
 * so an unattended test run measures DWM instead of the game; this makes it
 * measure the game. Live (console setenv). */
static int knob_present(void)   { static int v = -1; PS3_KNOB(v, ps3_env_int("RSX_PRESENT", 1)); return v; }
#include "rsx_d3d12_backend.h"
#include "rsx_primitives.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* RSX_SCALE=<percent> (default 100, 50-800): render the offscreen colour and
 * depth targets at that multiple of their guest size (docs/37). Read once,
 * since the targets live across frames. A target whose larger side is
 * RSX_SCALE_MIN (default 64) or less stays at its guest size: Demon's Souls's
 * exposure chain (64x64, then 16x16 targets the title reads back) must keep
 * its texel counts. Each target keeps its guest size (w, h; what matching and
 * the guest's viewport use) and its host size (hw, hh; the resource). */
static u32 rsx_scale_pct(void)
{
    static u32 v = 0;
    if (!v) {
        int p = ps3_env_int("RSX_SCALE", 100);
        v = (u32)(p < 50 ? 50 : p > 800 ? 800 : p);
        if (v != 100) printf("[D3D12] RSX_SCALE: offscreen targets at %u%%\n", v);
    }
    return v;
}
static u32 rt_host_dim(u32 w, u32 h, u32 n)
{
    static int lo = -1;
    if (lo < 0) lo = ps3_env_int("RSX_SCALE_MIN", 64);
    const u32 pct = rsx_scale_pct();
    if (pct == 100 || (w > h ? w : h) <= (u32)lo) return n;
    u64 v = ((u64)n * pct + 50) / 100;
    return (u32)(v < 1 ? 1 : v > 16384 ? 16384 : v);
}

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wtsapi32.h>         /* session lock notifications (s_session_locked) */

/* D3D12 headers */
#include <d3d12.h>
#include <d3d12sdklayers.h>   /* ID3D12Debug / ID3D12InfoQueue */
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

/* We need these GUIDs — define them here to avoid uuid.lib dependency */
#include <initguid.h>

/* Link libraries */
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "wtsapi32.lib")

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
    u32 topology;       /* rsx_topology (NOT a D3D value; see topo_to_d3d) */
    int textured;       /* 1 = sample the bound font/atlas texture (dbgfont) */
    int is_vp;          /* 1 = real VP path: vb_byte_offset indexes vp_vb (float4) */
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
        u32 addr, filt; /* NV4097 TEXTURE_ADDRESS / TEXTURE_FILTER: sampler state */
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
} D3D12DrawRecord;

/* Offscreen render target (render-to-texture). Persistent across frames --
 * pass N's output is sampled by pass N+1 and possibly by later frames. */
typedef struct {
    ID3D12Resource*       res;
    ID3D12Resource*       up;          /* init-upload staging (kept alive) */
    u32                   off, w, h;   /* raw RSX offset + guest dims */
    u32                   hw, hh;      /* the resource's dims (RSX_SCALE) */
    u32                   dxgi;        /* DXGI_FORMAT of the resource */
    D3D12_RESOURCE_STATES st;          /* tracked resource state */
    int                   used;        /* referenced this frame */
    ID3D12Resource*       rb;          /* colour write-back readback buffer */
    u64                   rb_size;
    u32                   gfmt, gpitch;/* guest SET_SURFACE_FORMAT word + pitch */
    u32                   gdma;        /* guest context DMA (local / main) */
    ID3D12Resource*       snap;        /* copy sampled by draws that also render here */
    u32                   snap_w, snap_h, snap_dxgi;
    D3D12_RESOURCE_STATES snap_st;
    /* Guest bytes after our last write-back (tex_csum over [gaddr, +glen)).
     * If they differ later, the title put something else there. */
    u32                   gaddr, glen, gsum;
    int                   gsum_ok, gsum_new;
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

/* Bumped on every resource creation. A cached descriptor (srv_write_ex) names
 * its resource by pointer, and a released resource's address can come back
 * for a new one: the stamp tells the two apart. */
static u32 s_res_gen = 1;
static HRESULT d3d_create_committed(ID3D12Device* dev, const D3D12_HEAP_PROPERTIES* hp,
                                    D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* rd,
                                    D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv,
                                    REFIID riid, void** out)
{
    s_res_gen++;
    return dev->lpVtbl->CreateCommittedResource(dev, hp, hf, rd, st, cv, riid, out);
}

/* Per-frame VP texture slot: a guest texture uploaded for this frame's VP
 * draws (re-uploaded every frame -- gcm/cube's plasma animates in guest
 * memory). SRV lives at heap index VP_TEX_SRV_BASE+slot. */
typedef struct {
    ID3D12Resource* res;
    u32 off, w, h, fmt; /* current contents (resource reused when dims match) */
    u32 csum;           /* sparse checksum of the source bytes last uploaded */
    u32 fsum;           /* full checksum of them, kept while TEX_VERIFY is on */
    u32 face_bytes;     /* guest bytes from one cube face to the next */
    ID3D12Heap* blk_heap; /* res is placed here (tex_pool), at blk_off; NULL = committed */
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
    ID3DBlob* vs;
    int uses_c03;
    int fetch;              /* reads vf_buf (rsx_vp_decompile_fetch): no input layout */
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
    ID3D12PipelineState* pso;   /* NULL: this key failed to build; not retried */
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
    int  window_closed;

    /* D3D12 core */
    ID3D12Device*              device;
    ID3D12CommandQueue*        cmd_queue;
    IDXGISwapChain3*           swap_chain;
    int                        tearing;   /* swap chain made with ALLOW_TEARING */
    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_descriptor_size;
    /* The game's frame, width x height (the title's display size): its own
     * textures, not the swap chain's, so the window can be any size. At the
     * present the frame is scaled into the swap chain's buffer (blit_to_window)
     * and an overlay draws over that at the window's own resolution. */
    ID3D12Resource*            render_targets[FRAME_COUNT];
    ID3D12Resource*            swap_buffers[FRAME_COUNT];   /* the window's, win_w x win_h */
    ID3D12DescriptorHeap*      swap_rtv_heap;
    u32                        win_w, win_h;
    ID3D12RootSignature*       blit_rs;
    ID3D12PipelineState*       blit_pso;
    ID3D12DescriptorHeap*      blit_srv_heap;   /* shader-visible: one SRV per render_targets[i] */
    ID3D12CommandAllocator*    cmd_allocators[FRAME_COUNT];
    ID3D12GraphicsCommandList* cmd_list;

    /* Synchronization */
    ID3D12Fence* fence;
    HANDLE       fence_event;
    u64          fence_values[FRAME_COUNT];
    u32          frame_index;

    /* Pipeline */
    ID3D12RootSignature*  root_signature;
    ID3D12PipelineState*  pipeline_state;         /* triangle class — default */
    ID3D12PipelineState*  pipeline_state_lines;   /* line class */
    ID3D12PipelineState*  pipeline_state_points;  /* point class */

    /* Depth/stencil */
    ID3D12DescriptorHeap* dsv_heap;
    ID3D12Resource*       depth_buffer;

    /* Dynamic vertex buffer (upload heap) */
    ID3D12Resource*       vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW vb_view;
    void*                 vb_mapped;      /* persistently mapped */
    u32                   vb_offset;      /* current write position */

    int                   pipeline_ready; /* 1 if root sig + PSO created */

    /* Debug: copy the presented backbuffer to disk as BMP for the first N
     * frames (enabled by env CELLMARK_DUMP). Lets us verify what actually
     * rendered without racing the window/process lifetime. */
    ID3D12Resource*       readback_buf;
    u32                   readback_pitch;
    int                   dump_frames_left;
    int                   dump_skip_left;   /* CELLMARK_DUMP_SKIP: presents to ignore first */

    /* Textured pipeline (dbgfont / 2D atlas quads). The font atlas is an 8-bit
     * coverage texture uploaded as R8_UNORM and sampled in the pixel shader. */
    ID3D12PipelineState*  pipeline_state_tex;   /* triangle + texture + blend */
    ID3D12DescriptorHeap* srv_heap;             /* shader-visible, 1 SRV slot  */
    ID3D12Resource*       tex_resource;         /* current atlas texture       */
    ID3D12Resource*       tex_upload;           /* staging buffer for uploads  */
    u32                   tex_w, tex_h;         /* dims of tex_resource         */
    int                   tex_ready;            /* 1 once an atlas is uploaded  */
    u32                   tex_src_offset;       /* guest RSX offset of atlas    */
    int                   tex_bound;            /* a texture is bound for draws */
    int                   tex_dirty;            /* re-upload needed this frame  */

    /* Real RSX vertex-program path: the captured VP is decompiled to HLSL and
     * used as the vertex shader, fed the raw float4 attrib0 + the vp_c[]
     * constant bank. This produces exact position + texcoord (vs. the frac
     * approximation). Used for the 2D/dbgfont quad draws. */
    ID3D12RootSignature*  vp_root_sig;          /* CBV(b0) + SRV table + sampler */
    ID3D12Resource*       vf_buf;               /* GPU vertex fetch ring (x2 parity) */
    void*                 vf_mapped;
    ID3D12PipelineState*  pipeline_state_vp;    /* decompiled VS + atlas PS      */
    ID3D12PipelineState*  pipeline_state_vp_color; /* decompiled VS + colour PS (untextured 3D) */
    ID3D12Resource*       vp_vb;                /* raw float4 attrib0, per-frame */
    void*                 vp_vb_mapped;
    u32                   vp_vb_offset;
    ID3D12Resource*       vp_ib;                /* u32 indices of the VP draws, per-frame */
    void*                 vp_ib_mapped;
    u32                   vp_ib_offset;         /* in indices */
    ID3D12Resource*       vp_cb;                /* per-draw VP constant slots    */
    void*                 vp_cb_mapped;
    ID3D12Resource*       vp_fpcb;              /* per-draw FP texscale (b1)     */
    void*                 vp_fpcb_mapped;
    int                   vp_ready;             /* VS+PSO compiled ok            */
    u32                   vp_compiled_bytes;    /* ucode size when last compiled */
    ID3DBlob*             vp_vs_blob;           /* kept for guest-FP PSO builds  */
    u32                   vp_gen;               /* bumped per VP recompile       */
    int                   vp_uses_c03;          /* VS references vp_c[0..3]      */
    VPTexSlot             vp_tex[VP_TEX_SLOTS]; /* per-frame VP texture slots    */
    VPVSEntry*            vp_vs;                /* per-draw decompiled VS cache  */
    int                   vp_vs_n, vp_vs_cap;
    VPFPEntry*            vp_fp;                /* guest-FP PSO cache            */
    int                   vp_fp_n, vp_fp_cap;
    u32                   srv_inc;              /* CBV_SRV_UAV descriptor size   */
    /* VP path: latest texture bound per unit (t0-t3). */
    struct { u32 off, raw, w, h, fmt, ctrl1, mips, addr, filt; int cube; int set; } cur_texs[RSX_FP_UNITS];
    ID3D12DescriptorHeap* samp_heap;            /* shader-visible guest samplers */
    u32                   samp_inc;
    u32                   samp_n;               /* 4-sampler tables created so far */

    /* Render-to-texture: offscreen RT pool + their RTV heap. */
    OffRT                 off_rt[MAX_OFF_RTS];
    /* DRAW_ARRAYS batch merging (see d3d12_draw_arrays): the vertex index one
     * past the last batch, and whether the current BEGIN/END may still be
     * extended. */
    u32                   merge_first_end;
    int                   merge_prev_draw;
    /* Indexed strip/fan merging (see d3d12_draw_indexed): the primitive of
     * the last draw when it was one, else 0. merge_first_end is its end. */
    int                   merge_strip;
    ID3D12DescriptorHeap* rt_rtv_heap;          /* MAX_OFF_RTS RTVs (CPU only)   */

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
static int draw_room(void)
{
    if (s_d3d.draw_count < MAX_DRAWS) return 1;
    static u64 said = ~0ull; static int n = 0;
    if (said != s_d3d.frame_count && n < 20) { said = s_d3d.frame_count; n++;
        fprintf(stderr, "[D3D12] frame has more than %d draws: the rest are dropped%c",
                MAX_DRAWS, 10); }
    return 0;
}
char g_rsx_title_base[128] = "ps3recomp";
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
 * reflection is not visible. */
static ID3D12Resource* s_screen_copy = NULL;
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
 * 256x256 regions captured and handed to fp 0x5DB81/0x5EA81/0x5EF81/0x5E381. */
#define SUBVP_SLOTS 4
static struct { ID3D12Resource* res; u32 x, y, w, h; } s_subvp[SUBVP_SLOTS];
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
    s_subvp[s_subvp_n].res = NULL;
    s_subvp_n++;
}

/* Copy each noted region out of the backbuffer. Called at the same moment as
 * screen_copy_capture -- after the reduced-viewport passes and before the
 * full-screen scene overwrites them. */
static void subvp_capture(u32 fi)
{
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("SUBVP_RTT", 0));
    if (!en || !s_d3d.device || !s_subvp_n) return;
    for (int i = 0; i < s_subvp_n; i++) {
        if (!s_subvp[i].res) {
            D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td = {0};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = s_subvp[i].w; td.Height = s_subvp[i].h;
            td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (FAILED(d3d_create_committed(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                    &IID_ID3D12Resource, (void**)&s_subvp[i].res)))
                { s_subvp[i].res = NULL; continue; }
        }
        D3D12_RESOURCE_BARRIER bb[2] = {0};
        bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bb[0].Transition.pResource   = s_d3d.render_targets[fi];
        bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bb[1] = bb[0];
        bb[1].Transition.pResource   = s_subvp[i].res;
        bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

        D3D12_TEXTURE_COPY_LOCATION dstl = {0}, srcl = {0};
        dstl.pResource = s_subvp[i].res;
        dstl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstl.SubresourceIndex = 0;
        srcl.pResource = s_d3d.render_targets[fi];
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcl.SubresourceIndex = 0;
        D3D12_BOX box; box.left = s_subvp[i].x; box.top = s_subvp[i].y;
        box.front = 0; box.right = s_subvp[i].x + s_subvp[i].w;
        box.bottom = s_subvp[i].y + s_subvp[i].h; box.back = 1;
        if (box.right > s_d3d.width)  box.right  = s_d3d.width;
        if (box.bottom > s_d3d.height) box.bottom = s_d3d.height;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dstl, 0, 0, 0, &srcl, &box);

        bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);
        { static int _n = 0; if (getenv("SUBVP_DBG") && _n++ < 8)
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
static volatile long s_prof_on;   /* a profile session is recording (rsx_prof_begin) */
volatile long g_rsx_prof_toggle;   /* bumped per F11 press; boot_main starts/stops */
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

/* ---------------------------------------------------------------------------
 * Win32 window
 * -----------------------------------------------------------------------*/

/* The Windows session is locked. Behind the lock screen DWM holds each back
 * buffer for 250-500 ms while Present still returns S_OK (not
 * DXGI_STATUS_OCCLUDED), and the frame rate fell to 4 fps waiting for them.
 * Frames are rendered but not presented while this is set. Written by the
 * window thread (WM_WTSSESSION_CHANGE, and once at window creation), read
 * at the flip. */
static volatile LONG s_session_locked;

static void session_lock_init(HWND hwnd)
{
    WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
    /* A run started behind the lock screen gets no LOCK message. */
    WTSINFOEXW* info = NULL;
    DWORD n = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                    WTSSessionInfoEx, (LPWSTR*)&info, &n) && info) {
        if (info->Level == 1)
            s_session_locked = info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
        WTSFreeMemory(info);
    }
    if (s_session_locked)
        fprintf(stderr, "[D3D12] session locked: rendering without presenting until unlock%c", 10);
}

/* rsx_d3d12_set_overlay. Set from one thread, read by the window and render
 * threads: the hooks are swapped in whole and then flagged. */
static rsx_d3d12_overlay s_overlay;
static volatile long     s_overlay_on;

/* The window's client size as of its last WM_SIZE, (w << 16) | h, written by
 * the window thread; the render thread resizes the swap chain to it between
 * frames (swap_resize). 0 until the first WM_SIZE. */
static volatile LONG s_win_req;

/* Borderless fullscreen (Alt+Enter, rsx_d3d12_set_borderless): the window
 * covers its monitor, no frame. A window, not exclusive fullscreen, so alt-tab
 * and the overlay behave as windowed. Window-thread state. */
#define WM_RSX_BORDERLESS (WM_APP + 0x52)
static int   s_borderless;
static RECT  s_windowed_rect;
static LONG  s_windowed_style;

static void set_borderless(HWND hwnd, int on)
{
    if (!!on == s_borderless) return;
    if (on) {
        MONITORINFO mi = {sizeof mi};
        GetWindowRect(hwnd, &s_windowed_rect);
        s_windowed_style = GetWindowLongA(hwnd, GWL_STYLE);
        if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        SetWindowLongA(hwnd, GWL_STYLE, (s_windowed_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, s_windowed_style);
        SetWindowPos(hwnd, NULL, s_windowed_rect.left, s_windowed_rect.top,
                     s_windowed_rect.right - s_windowed_rect.left, s_windowed_rect.bottom - s_windowed_rect.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOZORDER);
    }
    s_borderless = !!on;
}

static int window_dpi(HWND hwnd);

static LRESULT CALLBACK d3d12_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {   /* before the overlay: the frame's own business */
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && LOWORD(lp) && HIWORD(lp))
            InterlockedExchange(&s_win_req, (LONG)(((u32)LOWORD(lp) << 16) | HIWORD(lp)));
        break;
    case WM_RSX_BORDERLESS:
        set_borderless(hwnd, (int)wp);
        return 0;
    case WM_GETMINMAXINFO: {   /* a client area of at least 640x360 (at the display's scale): below that an overlay's text overlaps */
        const int dpi = window_dpi(hwnd);
        RECT r = {0, 0, MulDiv(640, dpi, 96), MulDiv(360, dpi, 96)};
        AdjustWindowRect(&r, (DWORD)GetWindowLongA(hwnd, GWL_STYLE), FALSE);
        ((MINMAXINFO*)lp)->ptMinTrackSize.x = r.right - r.left;
        ((MINMAXINFO*)lp)->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_DPICHANGED: {      /* moved to a display of another scale: take the size Windows suggests */
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29)) && !(lp & (1 << 30))) {   /* Alt+Enter, not repeat */
            set_borderless(hwnd, !s_borderless);
            return 0;
        }
        break;
    }
    if (s_overlay_on && s_overlay.wndproc) {
        long long r = 0;
        if (s_overlay.wndproc(s_overlay.user, hwnd, msg, (unsigned long long)wp, (long long)lp, &r))
            return (LRESULT)r;
    }
    switch (msg) {
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK)   InterlockedExchange(&s_session_locked, 1);
        if (wp == WTS_SESSION_UNLOCK) InterlockedExchange(&s_session_locked, 0);
        return 0;
    case WM_CLOSE:
        s_d3d.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && !s_overlay_on) {
            s_d3d.window_closed = 1;
            DestroyWindow(hwnd);
        }
        if (wp == VK_F9 && !(lp & (1 << 30))) {   /* not auto-repeat */
            extern void cellGcm_replay_hotkey(void);
            cellGcm_replay_hotkey();
        }
        if (wp == VK_F11 && !(lp & (1 << 30)))    /* profile start/stop */
            InterlockedIncrement(&g_rsx_prof_toggle);
        return 0;
    case WM_SYSKEYDOWN:
        /* F10 is the menu key: Windows sends it as WM_SYSKEYDOWN, never
         * WM_KEYDOWN. Handled here, it also stops the window entering menu
         * mode. */
        if (wp == VK_F10) {
            if (!(lp & (1 << 30))) s_hotshot_arm = 1;   /* see s_hotshot_left */
            return 0;
        }
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* The window is per-monitor DPI aware, so it is drawn at the display's own
 * resolution (not stretched by Windows on a display scaled above 100%) and an
 * overlay can size its text by the display's scale (rsx_d3d12_overlay). Its
 * first size is the title's at that scale. Loaded by name: these arrived in
 * Windows 10 1607/1703, and dwmapi is not otherwise linked. */
typedef BOOL (WINAPI *set_dpi_ctx_fn)(HANDLE);
typedef UINT (WINAPI *dpi_fn)(HWND);
typedef UINT (WINAPI *sys_dpi_fn)(void);
typedef HRESULT (WINAPI *dwm_attr_fn)(HWND, DWORD, LPCVOID, DWORD);

static int window_dpi(HWND hwnd)
{
    static dpi_fn get_dpi;
    static int looked;
    if (!looked) { get_dpi = (dpi_fn)(void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow"); looked = 1; }
    const UINT dpi = get_dpi && hwnd ? get_dpi(hwnd) : 0;
    return dpi ? (int)dpi : 96;
}

static int dpi_aware(void)
{
    HMODULE u = GetModuleHandleA("user32.dll");
    set_dpi_ctx_fn set_ctx = (set_dpi_ctx_fn)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    sys_dpi_fn sys_dpi = (sys_dpi_fn)(void*)GetProcAddress(u, "GetDpiForSystem");
    if (set_ctx) set_ctx((HANDLE)(intptr_t)-4);   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    const UINT dpi = sys_dpi ? sys_dpi() : 0;
    return dpi ? (int)dpi : 96;
}

/* A dark title bar, as dark-themed programs have (RSX_DARK_TITLE=0: the
 * system's). DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Windows 10 20H1 and later). */
static void dark_title(HWND hwnd)
{
    if (!ps3_env_int("RSX_DARK_TITLE", 1)) return;
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    dwm_attr_fn set_attr = dwm ? (dwm_attr_fn)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
    const BOOL on = TRUE;
    if (set_attr) set_attr(hwnd, 20, &on, sizeof on);
}

static HWND create_window(u32 width, u32 height, const char* title)
{
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = d3d12_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    /* The title's icon when its exe carries one as icon resource 1 (NULL, the
     * system's default, otherwise). */
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.hIconSm = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = "ps3recomp_d3d12";
    RegisterClassExA(&wc);

    const int dpi = dpi_aware();
    int cw = MulDiv((int)width, dpi, 96), ch = MulDiv((int)height, dpi, 96);
    /* Centred in the primary display's work area, and made smaller (same
     * shape) if it would not fit there. */
    MONITORINFO mi = {sizeof mi};
    const POINT origin = {0, 0};
    const int have_area = GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT wr = {0, 0, cw, ch};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    if (have_area) {
        const int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
        const int fw = (wr.right - wr.left) - cw, fh = (wr.bottom - wr.top) - ch;   /* the frame */
        if (cw + fw > aw || ch + fh > ah) {
            const int sw = aw - fw, sh = ah - fh;
            if ((long long)sw * ch < (long long)sh * cw) { ch = MulDiv(ch, sw, cw); cw = sw; }
            else { cw = MulDiv(cw, sh, ch); ch = sh; }
            wr.left = 0; wr.top = 0; wr.right = cw; wr.bottom = ch;
            AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        }
    }
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (have_area) {
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (wr.right - wr.left)) / 2;
        y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - (wr.bottom - wr.top)) / 2;
    }

    /* Keep the display awake (see rsx_null_backend.c). */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    HWND hwnd = CreateWindowExA(
        0, "ps3recomp_d3d12",
        title ? title : "ps3recomp (D3D12)",
        WS_OVERLAPPEDWINDOW,
        x, y,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    if (hwnd) {   /* the title bar's colour before the window first shows */
        dark_title(hwnd);
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }
    return hwnd;
}

/* The window lives on a thread of its own that does nothing but pump its
 * messages. Moving or resizing a window runs a modal loop inside
 * DispatchMessage until the mouse is released; when the vblank ticker pumped
 * the messages, that loop stopped the ticks, the FIFO drain and the presents,
 * and the game froze for as long as the window was held. Rendering and
 * presenting stay where they were: DXGI's flip model presents to a window
 * owned by another thread. RSX_WINDOW_THREAD=0 pumps on the ticker as before. */
static int s_window_thread;

typedef struct { u32 w, h; const char* title; HANDLE ready; } window_thread_args;

static DWORD WINAPI window_thread(LPVOID p)
{
    window_thread_args* a = (window_thread_args*)p;
    HWND hwnd = create_window(a->w, a->h, a->title);
    s_d3d.hwnd = hwnd;
    SetEvent(a->ready);   /* `a` is the creator's: not touched after this */
    if (!hwnd) return 0;
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    *(volatile int*)&s_d3d.window_closed = 1;
    return 0;
}

static HWND open_window(u32 width, u32 height, const char* title)
{
    s_window_thread = ps3_env_int("RSX_WINDOW_THREAD", 1) != 0;
    if (!s_window_thread) return create_window(width, height, title);
    window_thread_args a = {width, height, title, CreateEventA(NULL, TRUE, FALSE, NULL)};
    HANDLE t = a.ready ? CreateThread(NULL, 0, window_thread, &a, 0, NULL) : NULL;
    if (!t) {
        if (a.ready) CloseHandle(a.ready);
        s_window_thread = 0;
        return create_window(width, height, title);
    }
    WaitForSingleObject(a.ready, INFINITE);
    CloseHandle(a.ready);
    CloseHandle(t);
    return s_d3d.hwnd;
}

/* The swap chain's buffers and their RTVs (init and after ResizeBuffers). */
static int swap_buffers_get(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.swap_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.swap_rtv_heap, &h);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        HRESULT hr = s_d3d.swap_chain->lpVtbl->GetBuffer(
            s_d3d.swap_chain, i, &IID_ID3D12Resource, (void**)&s_d3d.swap_buffers[i]);
        if (FAILED(hr)) return -1;
        s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_d3d.swap_buffers[i], NULL, h);
        h.ptr += s_d3d.rtv_descriptor_size;
    }
    return 0;
}

/* The scale pass: a full-screen triangle sampling the game's frame with
 * linear filtering, into a viewport that keeps its aspect ratio. */
static int blit_init(void)
{
    static const char vs[] =
        "struct O { float4 p : SV_POSITION; float2 uv : TEXCOORD; };\n"
        "O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";
    static const char ps[] =
        "Texture2D t : register(t0); SamplerState s : register(s0);\n"
        "float4 main(float4 p : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {\n"
        "  return float4(t.Sample(s, uv).rgb, 1); }\n";
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL, *rsb = NULL;
    HRESULT hr = D3DCompile(vs, sizeof vs - 1, "blit_vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &err);
    if (SUCCEEDED(hr)) hr = D3DCompile(ps, sizeof ps - 1, "blit_ps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &err);
    if (FAILED(hr)) {
        printf("[D3D12] blit shader compile failed: %s\n", err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
        return -1;
    }
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12_ROOT_PARAMETER param = {0};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC smp = {0};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {1, &param, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err);
    if (SUCCEEDED(hr))
        hr = s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0, rsb->lpVtbl->GetBufferPointer(rsb),
                                                        rsb->lpVtbl->GetBufferSize(rsb),
                                                        &IID_ID3D12RootSignature, (void**)&s_d3d.blit_rs);
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = s_d3d.blit_rs;
        pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb);
        pd.VS.BytecodeLength  = vb->lpVtbl->GetBufferSize(vb);
        pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb);
        pd.PS.BytecodeLength  = pb->lpVtbl->GetBufferSize(pb);
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState,
                                                               (void**)&s_d3d.blit_pso);
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = FRAME_COUNT;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd, &IID_ID3D12DescriptorHeap,
                                                        (void**)&s_d3d.blit_srv_heap);
    }
    if (SUCCEEDED(hr)) {
        const u32 inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
            s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        s_d3d.blit_srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.blit_srv_heap, &h);
        for (u32 i = 0; i < FRAME_COUNT; i++, h.ptr += inc)
            s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.render_targets[i], NULL, h);
    }
    if (vb) vb->lpVtbl->Release(vb);
    if (pb) pb->lpVtbl->Release(pb);
    if (rsb) rsb->lpVtbl->Release(rsb);
    if (err) err->lpVtbl->Release(err);
    if (FAILED(hr)) printf("[D3D12] blit pipeline failed (0x%08lX)\n", (long)hr);
    return FAILED(hr) ? -1 : 0;
}

/* Records the scale of the game's frame fi into the window's buffer fi: the
 * largest rectangle with the frame's aspect ratio, centred, black around it.
 * Both resources are in PRESENT before and after. */
static void blit_to_window(u32 fi)
{
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.Subresource = b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.pResource   = s_d3d.render_targets[fi];
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[1].Transition.pResource   = s_d3d.swap_buffers[fi];
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cl->lpVtbl->ResourceBarrier(cl, 2, b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    s_d3d.swap_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.swap_rtv_heap, &rtv);
    rtv.ptr += (u64)fi * s_d3d.rtv_descriptor_size;
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);

    const float ww = (float)s_d3d.win_w, wh = (float)s_d3d.win_h;
    const float gw = (float)s_d3d.width, gh = (float)s_d3d.height;
    const float k = ww / gw < wh / gh ? ww / gw : wh / gh;
    const float vw = floorf(gw * k + 0.5f), vh = floorf(gh * k + 0.5f);
    D3D12_VIEWPORT vp = {floorf((ww - vw) * 0.5f), floorf((wh - vh) * 0.5f), vw, vh, 0.0f, 1.0f};
    if (vw != ww || vh != wh) {
        static const float black[4] = {0, 0, 0, 1};
        cl->lpVtbl->ClearRenderTargetView(cl, rtv, black, 0, NULL);
    }
    D3D12_RECT sc = {0, 0, (LONG)s_d3d.win_w, (LONG)s_d3d.win_h};
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.blit_rs);
    cl->lpVtbl->SetPipelineState(cl, s_d3d.blit_pso);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_d3d.blit_srv_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    s_d3d.blit_srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.blit_srv_heap, &g);
    g.ptr += (u64)fi * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, g);
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);

    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    cl->lpVtbl->ResourceBarrier(cl, 2, b);
}

static void wait_for_gpu(void);

/* Between frames, on the render thread: the swap chain follows the window's
 * client size (s_win_req). The GPU is idled first and the overlay drops its
 * views of the old buffers, or ResizeBuffers fails on the references. */
static void swap_resize(void)
{
    const LONG req = s_win_req;
    const u32 w = (u32)req >> 16, h = (u32)req & 0xFFFF;
    if (!w || !h || (w == s_d3d.win_w && h == s_d3d.win_h)) return;
    wait_for_gpu();
    if (s_overlay_on && s_overlay.release) s_overlay.release(s_overlay.user);
    for (u32 i = 0; i < FRAME_COUNT; i++)
        if (s_d3d.swap_buffers[i]) {
            s_d3d.swap_buffers[i]->lpVtbl->Release(s_d3d.swap_buffers[i]);
            s_d3d.swap_buffers[i] = NULL;
        }
    HRESULT hr = s_d3d.swap_chain->lpVtbl->ResizeBuffers(
        s_d3d.swap_chain, FRAME_COUNT, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
        s_d3d.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (SUCCEEDED(hr)) {
        s_d3d.win_w = w;
        s_d3d.win_h = h;
    } else {
        printf("[D3D12] ResizeBuffers %ux%u failed (0x%08lX); keeping %ux%u\n", w, h, (long)hr,
               s_d3d.win_w, s_d3d.win_h);
        InterlockedExchange(&s_win_req, (LONG)((s_d3d.win_w << 16) | s_d3d.win_h));
    }
    if (swap_buffers_get() != 0) printf("[D3D12] swap chain buffers lost after a resize\n");
    /* The back buffer index starts over; the GPU is idle, so every frame's
     * fence value moves to the highest one and the fence never steps back. */
    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);
    u64 fv = 0;
    for (u32 i = 0; i < FRAME_COUNT; i++) if (s_d3d.fence_values[i] > fv) fv = s_d3d.fence_values[i];
    for (u32 i = 0; i < FRAME_COUNT; i++) s_d3d.fence_values[i] = fv;
}

/* ---------------------------------------------------------------------------
 * D3D12 initialization
 * -----------------------------------------------------------------------*/

static void pso_lib_open(void);   /* pipeline library, defined with the shader cache */
static u64 s_vp_root_hash;        /* vp_root_sig's serialized bytes, part of every pipeline's name */
static u64 fnv_bytes(u64 h, const void* p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= ((const u8*)p)[i]; h *= 1099511628211ull; }
    return h;
}

static int init_d3d12(u32 width, u32 height)
{
    HRESULT hr;

    /* Enable debug layer in debug builds */
    /* Debug layer: on in debug builds, or in any build when D3D12_DBG is set
     * (so a Release run can capture exact PSO/validation errors). */
    if (
#ifdef NDEBUG
        getenv("D3D12_DBG")
#else
        1
#endif
    ) {
        ID3D12Debug* debug_controller = NULL;
        hr = D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&debug_controller);
        if (SUCCEEDED(hr) && debug_controller) {
            debug_controller->lpVtbl->EnableDebugLayer(debug_controller);
            debug_controller->lpVtbl->Release(debug_controller);
            printf("[D3D12] Debug layer enabled\n");
        } else {
            printf("[D3D12] Debug layer requested but unavailable (0x%08lX) -- "
                   "install 'Graphics Tools' optional feature\n", hr);
        }
    }

    /* D3D12_DRED=1: Device Removed Extended Data (auto-breadcrumbs + page
     * faults). Part of the D3D12 runtime, so it works without the Graphics
     * Tools debug layer; dred_report() prints it once when the device is lost. */
    if (getenv("D3D12_DRED")) {
        ID3D12DeviceRemovedExtendedDataSettings* ds = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings,
                                             (void**)&ds)) && ds) {
            ds->lpVtbl->SetAutoBreadcrumbsEnablement(ds, D3D12_DRED_ENABLEMENT_FORCED_ON);
            ds->lpVtbl->SetPageFaultEnablement(ds, D3D12_DRED_ENABLEMENT_FORCED_ON);
            ds->lpVtbl->Release(ds);
            printf("[D3D12] DRED enabled\n");
        } else printf("[D3D12] DRED unavailable\n");
    }

    /* Create DXGI factory */
    IDXGIFactory4* factory = NULL;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateDXGIFactory1 failed (0x%08lX)\n", hr);
        return -1;
    }

    /* Create D3D12 device. On a dual-GPU laptop a NULL adapter usually lands on
     * the integrated GPU; explicitly pick the high-performance (discrete)
     * adapter via IDXGIFactory6. Set CELLMARK_IGPU to force the low-power one
     * (for A/B testing a suspected iGPU-driver stall). */
    {
        IDXGIFactory6* factory6 = NULL;
        if (SUCCEEDED(factory->lpVtbl->QueryInterface(
                factory, &IID_IDXGIFactory6, (void**)&factory6))) {
            DXGI_GPU_PREFERENCE pref = getenv("CELLMARK_IGPU")
                ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
            IDXGIAdapter1* adapter = NULL;
            for (UINT ai = 0; factory6->lpVtbl->EnumAdapterByGpuPreference(
                     factory6, ai, pref, &IID_IDXGIAdapter1, (void**)&adapter)
                     != DXGI_ERROR_NOT_FOUND; ai++) {
                DXGI_ADAPTER_DESC1 ad;
                adapter->lpVtbl->GetDesc1(adapter, &ad);
                if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(D3D12CreateDevice((IUnknown*)adapter,
                        D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                        (void**)&s_d3d.device))) {
                    printf("[D3D12] adapter: %ls (VRAM %llu MB)\n", ad.Description,
                           (unsigned long long)(ad.DedicatedVideoMemory >> 20));
                    adapter->lpVtbl->Release(adapter);
                    break;
                }
                adapter->lpVtbl->Release(adapter);
                adapter = NULL;
            }
            factory6->lpVtbl->Release(factory6);
        }
    }
    if (!s_d3d.device) {
        hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                               &IID_ID3D12Device, (void**)&s_d3d.device);
        if (FAILED(hr)) {
            printf("[D3D12] ERROR: D3D12CreateDevice failed (0x%08lX)\n", hr);
            factory->lpVtbl->Release(factory);
            return -1;
        }
        printf("[D3D12] Device created on default adapter (feature level 11.0)\n");
    }

    pso_lib_open();

    /* Create command queue */
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s_d3d.device->lpVtbl->CreateCommandQueue(
        s_d3d.device, &queue_desc, &IID_ID3D12CommandQueue, (void**)&s_d3d.cmd_queue);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateCommandQueue failed\n");
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Create swap chain */
    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = width;
    sc_desc.Height = height;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = FRAME_COUNT;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    /* A windowed flip-model swap chain presenting with sync interval 0 is
     * still paced by the compositor at the display's refresh unless it may
     * tear: the swap chain has to be made with ALLOW_TEARING and each such
     * Present pass DXGI_PRESENT_ALLOW_TEARING (RSX_VSYNC=0 below). Asked once;
     * a system without it (no variable refresh support in the driver or OS)
     * presents with vsync. */
    {
        IDXGIFactory5* f5 = NULL;
        BOOL allow = FALSE;
        if (SUCCEEDED(factory->lpVtbl->QueryInterface(factory, &IID_IDXGIFactory5, (void**)&f5))) {
            if (FAILED(f5->lpVtbl->CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                       &allow, sizeof allow)))
                allow = FALSE;
            f5->lpVtbl->Release(f5);
        }
        s_d3d.tearing = allow ? 1 : 0;
        if (allow) sc_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    IDXGISwapChain1* swap_chain1 = NULL;
    hr = factory->lpVtbl->CreateSwapChainForHwnd(
        factory, (IUnknown*)s_d3d.cmd_queue,
        s_d3d.hwnd, &sc_desc, NULL, NULL, &swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateSwapChainForHwnd failed (0x%08lX)\n", hr);
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Disable Alt+Enter fullscreen toggle */
    factory->lpVtbl->MakeWindowAssociation(factory, s_d3d.hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->lpVtbl->Release(factory);

    /* Query SwapChain3 interface */
    hr = swap_chain1->lpVtbl->QueryInterface(
        swap_chain1, &IID_IDXGISwapChain3, (void**)&s_d3d.swap_chain);
    swap_chain1->lpVtbl->Release(swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: QueryInterface for SwapChain3 failed\n");
        return -1;
    }

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

    /* Create RTV descriptor heap */
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
    rtv_heap_desc.NumDescriptors = FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
        s_d3d.device, &rtv_heap_desc, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rtv_heap);
    if (FAILED(hr)) return -1;

    s_d3d.rtv_descriptor_size = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    /* Create RTVs for each frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);

    /* The game's frame: textures of their own (see render_targets), in the
     * PRESENT (= COMMON) state between frames as the swap chain's were, so
     * every transition and readback of them is unchanged. */
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = width;
        rd.Height           = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PRESENT, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.render_targets[i]);
        if (FAILED(hr)) return -1;

        s_d3d.device->lpVtbl->CreateRenderTargetView(
            s_d3d.device, s_d3d.render_targets[i], NULL, rtv_handle);
        rtv_handle.ptr += s_d3d.rtv_descriptor_size;
    }

    /* The window's buffers, with RTVs of their own. */
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.NumDescriptors = FRAME_COUNT;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
            s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.swap_rtv_heap);
        if (FAILED(hr)) return -1;
    }
    s_d3d.win_w = width;
    s_d3d.win_h = height;
    if (swap_buffers_get() != 0) return -1;
    if (blit_init() != 0) return -1;

    /* ---------------------------------------------------------------
     * Depth/stencil buffer
     * 24-bit depth + 8-bit stencil (DXGI_FORMAT_D24_UNORM_S8_UINT).
     * One shared depth texture across both frames — RSX games on PS3
     * typically use a single zeta surface.
     * ---------------------------------------------------------------*/
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {0};
        dsv_heap_desc.NumDescriptors = 1;
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
            s_d3d.device, &dsv_heap_desc, &IID_ID3D12DescriptorHeap,
            (void**)&s_d3d.dsv_heap);
        if (FAILED(hr)) {
            printf("[D3D12] DSV heap creation failed\n");
            return -1;
        }

        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC depth_desc = {0};
        depth_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        /* Sized to cover the LARGEST render target, not just the window:
         * D3D12 clips rasterization to the smallest bound attachment, and
         * this DSV is shared by every pass -- with a window-sized depth
         * buffer, draws into larger offscreen RTs (wave's 1920x1080 input
         * image) were silently cropped to the window rect. */
        depth_desc.Width              = rt_host_dim(2048, 2048, 2048);
        depth_desc.Height             = rt_host_dim(2048, 2048, 2048);
        depth_desc.DepthOrArraySize   = 1;
        depth_desc.MipLevels          = 1;
        depth_desc.Format             = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count   = 1;
        depth_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depth_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depth_clear = {0};
        depth_clear.Format                       = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_clear.DepthStencil.Depth           = 1.0f;
        depth_clear.DepthStencil.Stencil         = 0;

        hr = d3d_create_committed(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
            &IID_ID3D12Resource, (void**)&s_d3d.depth_buffer);
        if (FAILED(hr)) {
            printf("[D3D12] Depth buffer creation failed (0x%08lX)\n", hr);
            return -1;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
        dsv_desc.Format         = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_desc.ViewDimension  = D3D12_DSV_DIMENSION_TEXTURE2D;

        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
        s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);
        s_d3d.device->lpVtbl->CreateDepthStencilView(
            s_d3d.device, s_d3d.depth_buffer, &dsv_desc, dsv_handle);

        printf("[D3D12] Depth buffer created (%ux%u D24S8)\n", width, height);
    }

    /* Create command allocators and command list */
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        hr = s_d3d.device->lpVtbl->CreateCommandAllocator(
            s_d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&s_d3d.cmd_allocators[i]);
        if (FAILED(hr)) return -1;
    }

    hr = s_d3d.device->lpVtbl->CreateCommandList(
        s_d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        s_d3d.cmd_allocators[0], NULL,
        &IID_ID3D12GraphicsCommandList, (void**)&s_d3d.cmd_list);
    if (FAILED(hr)) return -1;

    /* Close the command list (it starts in recording state) */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);

    /* Create fence */
    hr = s_d3d.device->lpVtbl->CreateFence(
        s_d3d.device, 0, D3D12_FENCE_FLAG_NONE,
        &IID_ID3D12Fence, (void**)&s_d3d.fence);
    if (FAILED(hr)) return -1;

    s_d3d.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    memset(s_d3d.fence_values, 0, sizeof(s_d3d.fence_values));

    /* ---------------------------------------------------------------
     * Create root signature with 16 root constants (one mat4 MVP at b0).
     * Visible only to the vertex shader — pixel shader doesn't need it.
     * ---------------------------------------------------------------*/
    {
        /* param 0: mat4 MVP as 16 root constants at b0 (vertex shader).
         * param 1: one SRV (t0) descriptor table for the atlas texture (pixel).
         * static sampler s0: linear clamp. The plain (untextured) PSO simply
         * never references t0/s0, so it ignores them. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors     = 1;
        srv_range.BaseShaderRegister = 0;   /* t0 */
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER root_params[2] = {0};
        root_params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_params[0].Constants.ShaderRegister = 0;   /* b0 */
        root_params[0].Constants.RegisterSpace  = 0;
        root_params[0].Constants.Num32BitValues = 16;  /* mat4 */
        root_params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
        root_params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_params[1].DescriptorTable.NumDescriptorRanges = 1;
        root_params[1].DescriptorTable.pDescriptorRanges   = &srv_range;
        root_params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = {0};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;   /* s0 */
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
        rs_desc.NumParameters     = 2;
        rs_desc.pParameters       = root_params;
        rs_desc.NumStaticSamplers = 1;
        rs_desc.pStaticSamplers   = &samp;
        rs_desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ID3DBlob* signature_blob = NULL;
        ID3DBlob* error_blob = NULL;
        hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &signature_blob, &error_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature serialization failed: %s\n",
                   error_blob ? (const char*)error_blob->lpVtbl->GetBufferPointer(error_blob) : "?");
            if (error_blob) error_blob->lpVtbl->Release(error_blob);
            return -1;
        }

        hr = s_d3d.device->lpVtbl->CreateRootSignature(
            s_d3d.device, 0,
            signature_blob->lpVtbl->GetBufferPointer(signature_blob),
            signature_blob->lpVtbl->GetBufferSize(signature_blob),
            &IID_ID3D12RootSignature, (void**)&s_d3d.root_signature);
        signature_blob->lpVtbl->Release(signature_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature creation failed\n");
            return -1;
        }
    }

    /* ---------------------------------------------------------------
     * Compile shaders and create PSO
     * ---------------------------------------------------------------*/
    {
        /* Basic vertex-colored shader.
         * The MVP matrix arrives via root constants as 4 vec4 columns
         * (PS3/OpenGL column-major convention). We multiply explicitly so
         * we don't depend on HLSL's matrix packing — matches PS3 semantics
         * `gl_Position = MVP * vec4(pos, 1.0)`. */
        static const char vs_hlsl[] =
            "cbuffer cb0 : register(b0) {\n"
            "    float4 mvp_col0;\n"
            "    float4 mvp_col1;\n"
            "    float4 mvp_col2;\n"
            "    float4 mvp_col3;\n"
            "};\n"
            "struct VSInput  { float3 pos : POSITION; float4 col : COLOR; };\n"
            "struct VSOutput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "VSOutput main(VSInput i) {\n"
            "    VSOutput o;\n"
            "    float4 p = float4(i.pos, 1.0);\n"
            "    o.pos = mvp_col0 * p.x + mvp_col1 * p.y + mvp_col2 * p.z + mvp_col3 * p.w;\n"
            "    o.col = i.col;\n"
            "    return o;\n"
            "}\n";
        static const char ps_hlsl[] =
            "struct PSInput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "float4 main(PSInput i) : SV_TARGET { return i.col; }\n";

        ID3DBlob* vs_blob = NULL;
        ID3DBlob* ps_blob = NULL;
        ID3DBlob* err = NULL;

        hr = D3DCompile(vs_hlsl, sizeof(vs_hlsl) - 1, "vs_basic", NULL, NULL,
                        "main", "vs_5_0", 0, 0, &vs_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] VS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        hr = D3DCompile(ps_hlsl, sizeof(ps_hlsl) - 1, "ps_basic", NULL, NULL,
                        "main", "ps_5_0", 0, 0, &ps_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] PS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        if (vs_blob && ps_blob) {
            D3D12_INPUT_ELEMENT_DESC input_layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
            pso_desc.pRootSignature = s_d3d.root_signature;
            pso_desc.VS.pShaderBytecode = vs_blob->lpVtbl->GetBufferPointer(vs_blob);
            pso_desc.VS.BytecodeLength = vs_blob->lpVtbl->GetBufferSize(vs_blob);
            pso_desc.PS.pShaderBytecode = ps_blob->lpVtbl->GetBufferPointer(ps_blob);
            pso_desc.PS.BytecodeLength = ps_blob->lpVtbl->GetBufferSize(ps_blob);
            pso_desc.InputLayout.pInputElementDescs = input_layout;
            pso_desc.InputLayout.NumElements = 3;
            pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;
            pso_desc.SampleMask = UINT_MAX;
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso_desc.NumRenderTargets = 1;
            pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            /* Depth test enabled, write enabled, LESS func.
             * Games with depth_test_enable=0 in RSX state can still render —
             * LESS just means new-z must be less than existing — but future
             * work should mirror RSX depth state into a PSO cache. */
            pso_desc.DepthStencilState.DepthEnable    = TRUE;
            pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pso_desc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            pso_desc.DepthStencilState.StencilEnable  = FALSE;
            pso_desc.SampleDesc.Count = 1;

            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state);
            if (SUCCEEDED(hr)) {
                s_d3d.pipeline_ready = 1;
                printf("[D3D12] Pipeline state created (triangle class)\n");
            } else {
                printf("[D3D12] PSO TRIANGLE creation failed (0x%08lX)\n", hr);
            }

            /* Line-class PSO — same shader, LINE topology type. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_lines);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (line class)\n");
            else printf("[D3D12] PSO LINE creation failed (0x%08lX)\n", hr);

            /* Point-class PSO. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_points);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (point class)\n");
            else printf("[D3D12] PSO POINT creation failed (0x%08lX)\n", hr);

            vs_blob->lpVtbl->Release(vs_blob);
            ps_blob->lpVtbl->Release(ps_blob);
        }

        /* -----------------------------------------------------------------
         * Textured triangle PSO (dbgfont / 2D atlas quads). Same MVP VS but
         * carrying UV; the PS samples the single-channel atlas as coverage
         * and modulates the vertex color, with straight alpha blending so
         * glyph edges composite over the framebuffer. Depth off (2D overlay).
         * ----------------------------------------------------------------*/
        {
            static const char vs_tex[] =
                "cbuffer cb0 : register(b0){float4 c0;float4 c1;float4 c2;float4 c3;};\n"
                "struct VSIn { float3 pos:POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "struct VSOut{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "VSOut main(VSIn i){ VSOut o; float4 p=float4(i.pos,1.0);\n"
                "  o.pos=c0*p.x+c1*p.y+c2*p.z+c3*p.w; o.col=i.col; o.uv=i.uv; return o; }\n";
            static const char ps_tex[] =
                "Texture2D tex : register(t0);\n"
                "SamplerState smp : register(s0);\n"
                "struct PSIn{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "float4 main(PSIn i):SV_TARGET{\n"
                /* dbgfont texcoords are compressed ~10x (U) / ~8x (V) vs. the
                 * atlas; scale to recover glyph cells. Exact per-glyph offset
                 * still being calibrated. */
                "  float2 uv2 = float2(i.uv.x*10.0, i.uv.y*8.0 + 0.59);\n"
                "  float cov = tex.Sample(smp, uv2).r;\n"
                "  return float4(i.col.rgb, i.col.a * cov); }\n";

            ID3DBlob *vtb = NULL, *ptb = NULL, *e2 = NULL;
            hr = D3DCompile(vs_tex, sizeof(vs_tex) - 1, "vs_tex", NULL, NULL, "main", "vs_5_0", 0, 0, &vtb, &e2);
            if (FAILED(hr)) printf("[D3D12] VS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");
            hr = D3DCompile(ps_tex, sizeof(ps_tex) - 1, "ps_tex", NULL, NULL, "main", "ps_5_0", 0, 0, &ptb, &e2);
            if (FAILED(hr)) printf("[D3D12] PS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");

            if (vtb && ptb) {
                D3D12_INPUT_ELEMENT_DESC il[] = {
                    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                };
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
                pd.pRootSignature = s_d3d.root_signature;
                pd.VS.pShaderBytecode = vtb->lpVtbl->GetBufferPointer(vtb);
                pd.VS.BytecodeLength  = vtb->lpVtbl->GetBufferSize(vtb);
                pd.PS.pShaderBytecode = ptb->lpVtbl->GetBufferPointer(ptb);
                pd.PS.BytecodeLength  = ptb->lpVtbl->GetBufferSize(ptb);
                pd.InputLayout.pInputElementDescs = il;
                pd.InputLayout.NumElements = 3;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
                pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
                pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = UINT_MAX;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
                pd.DepthStencilState.DepthEnable = FALSE;
                pd.DepthStencilState.StencilEnable = FALSE;
                pd.SampleDesc.Count = 1;
                hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                    s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_tex);
                if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (textured class)\n");
                else printf("[D3D12] PSO TEX creation failed (0x%08lX)\n", hr);
            }
            if (vtb) vtb->lpVtbl->Release(vtb);
            if (ptb) ptb->lpVtbl->Release(ptb);
        }

        /* SRV descriptor heap (shader-visible). Layout: slot 0 = legacy atlas
         * (dbgfont / textured 2D path), slots 1-4 = per-draw VP textures,
         * 5-20 = offscreen render targets (render-to-texture), rest spare.
         * All slots start as null SRVs so any 4-wide table window (the
         * root signature binds t0-t3) is valid on tier-1 hardware. */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = SRV_HEAP_SIZE;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.srv_heap);
            if (FAILED(hr)) printf("[D3D12] SRV heap creation failed (0x%08lX)\n", hr);
            else {
                s_d3d.srv_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                    s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                D3D12_SHADER_RESOURCE_VIEW_DESC nv = {0};
                nv.Format = DXGI_FORMAT_R8_UNORM;
                nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                nv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE hh;
                s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &hh);
                for (int _i = 0; _i < SRV_HEAP_SIZE; _i++) {
                    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, NULL, &nv, hh);
                    hh.ptr += s_d3d.srv_inc;
                }
                s_res_gen++;   /* a new heap: forget srv_write_ex's memo */
            }
        }

        /* RTV heap for offscreen render targets (CPU-visible only). */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = MAX_OFF_RTS;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rt_rtv_heap);
            if (FAILED(hr)) printf("[D3D12] offscreen RTV heap creation failed (0x%08lX)\n", hr);
        }
    }

    /* ---------------------------------------------------------------
     * Create dynamic vertex buffer (upload heap, 4MB)
     * ---------------------------------------------------------------*/
    {
        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC buf_desc = {0};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = MAX_VERTICES * VERTEX_STRIDE;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = d3d_create_committed(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.vertex_buffer);
        if (SUCCEEDED(hr)) {
            D3D12_RANGE read_range = {0, 0};
            s_d3d.vertex_buffer->lpVtbl->Map(
                s_d3d.vertex_buffer, 0, &read_range, &s_d3d.vb_mapped);
            s_d3d.vb_view.BufferLocation =
                s_d3d.vertex_buffer->lpVtbl->GetGPUVirtualAddress(s_d3d.vertex_buffer);
            s_d3d.vb_view.StrideInBytes = VERTEX_STRIDE;
            s_d3d.vb_view.SizeInBytes = MAX_VERTICES * VERTEX_STRIDE;
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
         * unaffected. Matching sampler table s0-s15. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = RSX_FP_UNITS;
        srv_range.BaseShaderRegister = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rp[6] = {0};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b0 = vp_c bank */
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &srv_range;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b1 = FP texscale */
        rp[2].Descriptor.ShaderRegister = 1;
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        /* s0-s15 come from a per-draw sampler table built from the guest's
         * TEXTURE_ADDRESS/TEXTURE_FILTER (vp_sampler_table). Fixed static
         * samplers sampled a 256x1 gamma LUT with WRAP, so u=0 blended texel 0
         * with texel 255 and black came out mid-grey. */
        D3D12_DESCRIPTOR_RANGE samp_range = {0};
        samp_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samp_range.NumDescriptors = RSX_FP_UNITS;
        samp_range.BaseShaderRegister = 0;
        samp_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[3].DescriptorTable.NumDescriptorRanges = 1;
        rp[3].DescriptorTable.pDescriptorRanges = &samp_range;
        rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        {
            D3D12_DESCRIPTOR_HEAP_DESC sh = {0};
            sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            sh.NumDescriptors = VP_SAMP_TABLES * RSX_FP_UNITS;
            sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &sh,
                    &IID_ID3D12DescriptorHeap, (void**)&s_d3d.samp_heap)))
                printf("[D3D12] sampler heap creation failed\n");
            s_d3d.samp_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }

        /* GPU vertex fetch: t20 = the frame's raw guest vertex bytes (vf_buf),
         * b2 = one constant, the draw's attribute records' offset in it. */
        rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[4].Descriptor.ShaderRegister = 20;
        rp[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[5].Constants.ShaderRegister = 2;
        rp[5].Constants.Num32BitValues = 1;
        rp[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rd = {0};
        rd.NumParameters = 6; rd.pParameters = rp;
        rd.NumStaticSamplers = 0; rd.pStaticSamplers = NULL;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (SUCCEEDED(hr)) {
            s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0,
                sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                &IID_ID3D12RootSignature, (void**)&s_d3d.vp_root_sig);
            s_vp_root_hash = fnv_bytes(14695981039346656037ull, sig->lpVtbl->GetBufferPointer(sig),
                                       sig->lpVtbl->GetBufferSize(sig));
            sig->lpVtbl->Release(sig);
        } else if (err) { printf("[D3D12] VP root sig: %s\n", (const char*)err->lpVtbl->GetBufferPointer(err)); err->lpVtbl->Release(err); }

        /* raw float4 vertex buffer + constant bank (both UPLOAD, persistently mapped) */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE nr = {0,0};

        /* VP_VB_VERTS=<n>: start the buffer at n vertices rather than
         * MAX_VERTICES, for a title known to need more (it grows on demand,
         * but the growth costs a frame; see vp_vb_grow). */
        { u64 v = (u64)ps3_env_int("VP_VB_VERTS", 0);
          if (v > MAX_VERTICES && v <= (64ull << 20)) s_vp_cap = (u32)v; }
        bd.Width = (u64)s_vp_cap * 256 * 2;  /* generic VP vertex: 16 float4 slots,
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
                (unsigned long long)(bd.Width >> 20), (unsigned)s_vp_cap, 10);
        if (SUCCEEDED(d3d_create_committed(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_vb)))
            s_d3d.vp_vb->lpVtbl->Map(s_d3d.vp_vb, 0, &nr, &s_d3d.vp_vb_mapped);
        else
            fprintf(stderr, "[VPVB] ALLOCATION FAILED -- no VP geometry will render%c", 10);

        bd.Width = (u64)VP_CB_STRIDE * MAX_DRAWS * 2;   /* per-draw constant slots, x2 parity */
        /* A failure here is silent and catastrophic: vp_record_cb returns early,
         * every draw loses its constants, and the whole scene transforms by
         * zeros -- indistinguishable from a broken vertex program. Say so. */
        fprintf(stderr, "[VPCB] per-draw constant buffer: %llu MB (%d draws)%c",
                (unsigned long long)(bd.Width >> 20), MAX_DRAWS, 10);
        if (SUCCEEDED(d3d_create_committed(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_cb)))
            s_d3d.vp_cb->lpVtbl->Map(s_d3d.vp_cb, 0, &nr, &s_d3d.vp_cb_mapped);

        bd.Width = (u64)VP_FPCB_STRIDE * MAX_DRAWS * 2;  /* per-draw FP slots (b1), x2 parity */
        if (SUCCEEDED(d3d_create_committed(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_fpcb)))
            s_d3d.vp_fpcb->lpVtbl->Map(s_d3d.vp_fpcb, 0, &nr, &s_d3d.vp_fpcb_mapped);

        /* GPU vertex fetch ring: each fetch draw's guest vertex bytes and
         * attribute records, x2 parity like vp_vb. VP_FETCH_RING_MB (read
         * once, default 64) per half; a draw that does not fit takes the CPU
         * path. */
        { int mb = ps3_env_int("VP_FETCH_RING_MB", 64);
          if (mb < 4) mb = 4; if (mb > 1024) mb = 1024;
          s_vf_cap = (u32)mb << 20;
          bd.Width = (u64)s_vf_cap * 2;
          if (SUCCEEDED(d3d_create_committed(s_d3d.device, &hp,
                  D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                  &IID_ID3D12Resource, (void**)&s_d3d.vf_buf)))
              s_d3d.vf_buf->lpVtbl->Map(s_d3d.vf_buf, 0, &nr, &s_d3d.vf_mapped);
          else s_vf_cap = 0; }

        printf("[D3D12] VP path resources: rootsig=%p vb=%p cb=%p\n",
               (void*)s_d3d.vp_root_sig, (void*)s_d3d.vp_vb, (void*)s_d3d.vp_cb);
    }

    printf("[D3D12] Initialization complete (%ux%u, %u buffers, pipeline=%s)\n",
           width, height, FRAME_COUNT,
           s_d3d.pipeline_ready ? "ready" : "NOT ready");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Frame sync helpers
 * -----------------------------------------------------------------------*/

/* D3D12_DRED=1: on the first device removal, print the reason, the command
 * history around the last completed breadcrumb, and the page-fault address. */
static void dred_report(const char* where)
{
    static int done = 0;
    if (done || !getenv("D3D12_DRED") || !s_d3d.device) return;
    HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
    if (rr == S_OK) return;
    done = 1;
    fprintf(stderr, "[DRED] device removed at %s: reason 0x%08lX%c", where, (long)rr, 10);
    ID3D12DeviceRemovedExtendedData* d = NULL;
    if (FAILED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device,
            &IID_ID3D12DeviceRemovedExtendedData, (void**)&d)) || !d) {
        fprintf(stderr, "[DRED] no DRED interface%c", 10); return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bo = {0};
    if (SUCCEEDED(d->lpVtbl->GetAutoBreadcrumbsOutput(d, &bo))) {
        static const char* nm[] = { "SETMARKER","BEGINEVENT","ENDEVENT","DRAWINSTANCED",
            "DRAWINDEXEDINSTANCED","EXECUTEINDIRECT","DISPATCH","COPYBUFFERREGION",
            "COPYTEXTUREREGION","COPYRESOURCE","COPYTILES","RESOLVESUBRESOURCE",
            "CLEARRENDERTARGETVIEW","CLEARUNORDEREDACCESSVIEW","CLEARDEPTHSTENCILVIEW",
            "RESOURCEBARRIER" };
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bo.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
            u32 last = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            fprintf(stderr, "[DRED] list %p: %u ops, last completed %u%c",
                    (void*)n->pCommandList, n->BreadcrumbCount, last, 10);
            if (last >= n->BreadcrumbCount) continue;   /* list finished */
            u32 a = last > 12 ? last - 12 : 0, b = last + 12 < n->BreadcrumbCount ? last + 12 : n->BreadcrumbCount;
            for (u32 k = a; k < b; k++) {
                u32 op = (u32)n->pCommandHistory[k];
                fprintf(stderr, "[DRED]   %s%5u %s (%u)%c", k == last ? ">>" : "  ", k,
                        op < sizeof nm / sizeof nm[0] ? nm[op] : "op", op, 10);
            }
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT po = {0};
    if (SUCCEEDED(d->lpVtbl->GetPageFaultAllocationOutput(d, &po))) {
        fprintf(stderr, "[DRED] page fault VA 0x%llX%c", (unsigned long long)po.PageFaultVA, 10);
        for (const D3D12_DRED_ALLOCATION_NODE* a = po.pHeadExistingAllocationNode; a; a = a->pNext)
            fprintf(stderr, "[DRED]   existing alloc type %d %s%c", (int)a->AllocationType,
                    a->ObjectNameA ? a->ObjectNameA : "", 10);
        for (const D3D12_DRED_ALLOCATION_NODE* a = po.pHeadRecentFreedAllocationNode; a; a = a->pNext)
            fprintf(stderr, "[DRED]   freed alloc type %d %s%c", (int)a->AllocationType,
                    a->ObjectNameA ? a->ObjectNameA : "", 10);
    }
    d->lpVtbl->Release(d);
    fflush(stderr);
}

static void wait_for_gpu(void)
{
    u32 fi = s_d3d.frame_index;
    s_d3d.fence_values[fi]++;
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, s_d3d.fence_values[fi]);

    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[fi]) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, s_d3d.fence_values[fi], s_d3d.fence_event);
        /* NEVER wait unbounded here: this runs on the vblank ticker, which also
         * drives the guest's vblank handlers, the FIFO drain, and the fence
         * publication. A device removal (TDR) leaves the D3D12 fence unsignaled
         * forever and an INFINITE wait silently froze the ENTIRE emulation --
         * observed on LBP as a boot that died the moment the 2048x2048
         * offscreen RT was created (last log line), every guest thread parked.
         * Degrade loudly instead: bounded waits + the removal reason, then
         * carry on (subsequent D3D calls fail visibly but the game keeps
         * running headless). */
        for (int tries = 0; tries < 3; tries++) {
            if (WaitForSingleObject(s_d3d.fence_event, 2000) != WAIT_TIMEOUT)
                return;
            HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
            printf("[D3D12] wait_for_gpu STUCK %ds: want %llu got %llu removed=0x%08lX\n",
                   2 * (tries + 1),
                   (unsigned long long)s_d3d.fence_values[fi],
                   (unsigned long long)s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence),
                   (long)rr);
            fflush(stdout);
            if (rr != 0 /* S_OK: device still alive, keep waiting a bit */)
                break;
        }
    }
}

static void move_to_next_frame(void)
{
    u64 current_fence = s_d3d.fence_values[s_d3d.frame_index];
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, current_fence);

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[s_d3d.frame_index]) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, s_d3d.fence_values[s_d3d.frame_index], s_d3d.fence_event);
        if (WaitForSingleObject(s_d3d.fence_event, 2000) == WAIT_TIMEOUT) {
            HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
            printf("[D3D12] FENCE STUCK 2s: want %llu got %llu removed=0x%08lX\n",
                   (unsigned long long)s_d3d.fence_values[s_d3d.frame_index],
                   (unsigned long long)s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence),
                   (long)rr);
        }
    }

    s_d3d.fence_values[s_d3d.frame_index] = current_fence + 1;
}

/* ---------------------------------------------------------------------------
 * Render a frame (clear + present)
 * -----------------------------------------------------------------------*/

/* Write the mapped readback buffer (R8G8B8A8, row pitch = readback_pitch) out
 * as a 24-bit bottom-up BMP. Debug-only. */
/* Name prefix for the next readback dump: lets the same writer emit the
 * backbuffer and the screen-copy texture under different filenames. */
static const char* s_dump_name = NULL;

static void dump_backbuffer_bmp(void)
{
    if (!s_d3d.readback_buf) return;
    void* mapped = NULL;
    D3D12_RANGE rr = {0, (SIZE_T)s_d3d.readback_pitch * s_d3d.height};
    if (FAILED(s_d3d.readback_buf->lpVtbl->Map(s_d3d.readback_buf, 0, &rr, &mapped)) || !mapped)
        return;

    /* CELLMARK_DUMP_CHANGED=1: write a frame only when it differs from the
     * last one written. A sampled hash (every 8th row, every 16th pixel) is
     * enough to tell a logo or a menu change from a re-presented frame, and a
     * long static screen then costs one file instead of hundreds. */
    { static int chg = -1; static unsigned long long last = 0;
      PS3_KNOB(chg, ps3_env_int("CELLMARK_DUMP_CHANGED", 0));
      if (chg && s_hotshot_left <= 0) {   /* an F10 burst keeps every present */
          unsigned long long hsh = 1469598103934665603ull;
          for (u32 y = 0; y < s_d3d.height; y += 8) {
              const u32* row = (const u32*)((unsigned char*)mapped + (u64)y * s_d3d.readback_pitch);
              for (u32 x = 0; x < s_d3d.width; x += 16) hsh = (hsh ^ row[x]) * 1099511628211ull;
          }
          if (hsh == last) {
              D3D12_RANGE wr0 = {0, 0};
              s_d3d.readback_buf->lpVtbl->Unmap(s_d3d.readback_buf, 0, &wr0);
              return;
          }
          last = hsh;
      } }

    static int idx = 0;
    char path[512];
    const char* dir = getenv("CELLMARK_DUMP_DIR");   /* default: current dir */
    if (s_hotshot_left > 0 && !s_dump_name) {
        const char* hd = getenv("RSX_HOTKEY_DIR");
        if (!hd || !*hd) hd = "rsx_hotkey";   /* as F9 (cellGcmSys.c) */
        if (s_hotshot_name[0])
            snprintf(path, sizeof(path), "%s/%s_live_%02d_d%u.bmp", hd, s_hotshot_name,
                     s_hotshot_n++, s_dbg_last_draws);
        else
        snprintf(path, sizeof(path), "%s%sshot_%03d_%02d_d%u.bmp", hd ? hd : "", hd ? "/" : "",
                 s_hotshot_burst, s_hotshot_n++, s_dbg_last_draws);
        s_hotshot_left--;
    } else
    snprintf(path, sizeof(path), "%s%s%s_%03d.bmp",
             dir ? dir : "", dir ? "/" : "", s_dump_name ? s_dump_name : "frame", idx++);
    FILE* f = fopen(path, "wb");
    if (!f) printf("[D3D12] cannot write %s%c", path, 10);
    if (f) {
        u32 w = s_d3d.width, h = s_d3d.height;
        u32 padded = (w * 3 + 3) & ~3u;
        u32 imgsz  = padded * h;
        u32 filesz = 54 + imgsz;
        unsigned char hdr[54] = {0};
        hdr[0] = 'B'; hdr[1] = 'M';
        hdr[2]=filesz&0xFF; hdr[3]=(filesz>>8)&0xFF; hdr[4]=(filesz>>16)&0xFF; hdr[5]=(filesz>>24)&0xFF;
        hdr[10]=54; hdr[14]=40;
        hdr[18]=w&0xFF; hdr[19]=(w>>8)&0xFF; hdr[20]=(w>>16)&0xFF; hdr[21]=(w>>24)&0xFF;
        hdr[22]=h&0xFF; hdr[23]=(h>>8)&0xFF; hdr[24]=(h>>16)&0xFF; hdr[25]=(h>>24)&0xFF;
        hdr[26]=1; hdr[28]=24;
        hdr[34]=imgsz&0xFF; hdr[35]=(imgsz>>8)&0xFF; hdr[36]=(imgsz>>16)&0xFF; hdr[37]=(imgsz>>24)&0xFF;
        fwrite(hdr, 1, 54, f);
        unsigned char* row = (unsigned char*)malloc(padded);
        if (row) {
            memset(row, 0, padded);
            for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
                unsigned char* src = (unsigned char*)mapped + (u32)y * s_d3d.readback_pitch;
                for (u32 x = 0; x < w; x++) {
                    row[x*3+0] = src[x*4+2]; /* B */
                    row[x*3+1] = src[x*4+1]; /* G */
                    row[x*3+2] = src[x*4+0]; /* R */
                }
                fwrite(row, 1, padded, f);
            }
            free(row);
        }
        fclose(f);
        printf("[D3D12] dumped %s (%ux%u, %u draws)\n", path, s_d3d.width, s_d3d.height,
               s_dbg_last_draws);
    }
    D3D12_RANGE wr = {0, 0};
    s_d3d.readback_buf->lpVtbl->Unmap(s_d3d.readback_buf, 0, &wr);
}

/* ---------------------------------------------------------------------------
 * RSX frame capture ("rsxcap") -- a purpose-built GPU debugger. RSX_CAP=start
 * [:count[:stride]] snapshots whole frames to <RSX_CAP_DIR>/frame_NN/: a text
 * manifest of every op (in submission order, with full surface/shader/texture
 * state) plus a BMP of the backbuffer and every offscreen colour RT used that
 * frame. One frame, captured atomically -- no cross-frame guessing.
 * -----------------------------------------------------------------------*/

/* Shared 24-bit BMP writer for readbacks. R8G8B8A8_UNORM straight; the half/
 * float RT formats are |v|-tonemapped so signed/HDR data is visible. */
static void cap_write_bmp(const char* path, const void* mapped, u32 w, u32 h,
                          u32 pitch, u32 dxgi)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    u32 rowb = (w * 3 + 3) & ~3u, datasz = rowb * h;
    u8 hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(u32*)(hdr+2) = 54 + datasz; *(u32*)(hdr+10) = 54; *(u32*)(hdr+14) = 40;
    *(int*)(hdr+18) = (int)w; *(int*)(hdr+22) = (int)h;
    *(u16*)(hdr+26) = 1; *(u16*)(hdr+28) = 24; *(u32*)(hdr+34) = datasz;
    fwrite(hdr, 1, 54, f);
    u8* line = (u8*)malloc(rowb);
    if (line) for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
        const u8* srow = (const u8*)mapped + (u64)y * pitch;
        memset(line, 0, rowb);
        for (u32 x = 0; x < w; x++) {
            float rv, gv, bv;
            if (dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                const u16* hp = (const u16*)(srow + (u64)x * 8);
                float v[3];
                for (int c = 0; c < 3; c++) {
                    u16 hv = hp[c]; u32 s = (hv>>15)&1, e = (hv>>10)&0x1F, m = hv&0x3FF; float fv;
                    if (e == 0) fv = (float)m / 16777216.0f;
                    else { u32 fb = (s<<31)|((e-15+127)<<23)|(m<<13); memcpy(&fv,&fb,4); }
                    v[c] = fv;
                }
                rv = v[0]; gv = v[1]; bv = v[2];
            } else if (dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                const float* fp = (const float*)(srow + (u64)x * 16);
                rv = fp[0]; gv = fp[1]; bv = fp[2];
            } else {
                const u8* p = srow + (u64)x * 4;
                rv = p[0]/255.0f; gv = p[1]/255.0f; bv = p[2]/255.0f;
            }
            float ar = rv<0?-rv:rv, ag = gv<0?-gv:gv, ab = bv<0?-bv:bv;
            if (ar>1) ar=1; if (ag>1) ag=1; if (ab>1) ab=1;
            line[x*3+0] = (u8)(ab*255.0f);
            line[x*3+1] = (u8)(ag*255.0f);
            line[x*3+2] = (u8)(ar*255.0f);
        }
        fwrite(line, 1, rowb, f);
    }
    free(line);
    fclose(f);
}

/* Copy one GPU resource into a fresh readback buffer and write it as a BMP.
 * Self-contained: waits, resets the frame command list, copies, executes,
 * waits, maps. `prior` is the resource's current tracked state (restored after
 * the copy so the caller's state tracking stays valid). */
static void cap_readback_write(ID3D12Resource* res, u32 w, u32 h, u32 dxgi,
                               D3D12_RESOURCE_STATES prior, u32 fi, const char* path)
{
    if (!res || !w || !h) return;
    u32 bpp = (dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
              (dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 : 4;
    u32 pitch = (w * bpp + 255) & ~255u;

    ID3D12Resource* rb = NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (u64)pitch * h; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(d3d_create_committed(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&rb)) || !rb)
        return;

    wait_for_gpu();
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = prior;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)dxgi;
    dst.PlacedFootprint.Footprint.Width = w; dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = pitch;
    src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
        /* TEXUP_DBG=1: texture uploads per second. A cache that misses every
         * draw re-uploads megabytes per frame, which is the kind of GPU work
         * that trips a 2-second TDR while looking like nothing in a draw count. */
        { static int td = -1; PS3_KNOB(td, getenv("TEXUP_DBG") ? 1 : 0);
          if (td) { static long long n = 0, bytes = 0; static clock_t t0 = 0;
              n++; bytes += (long long)pitch * h;
              const clock_t now = clock();
              if (!t0) t0 = now;
              if (now - t0 >= CLOCKS_PER_SEC) {
                  fprintf(stderr, "[TEXUP] %lld uploads/s, %.1f MB/s\n",
                          n, bytes / 1048576.0);
                  n = 0; bytes = 0; t0 = now; } } }

    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = prior;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }

    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cl[] = { (ID3D12CommandList*)s_d3d.cmd_list };
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cl);
    wait_for_gpu();

    void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)pitch * h};
    if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, &mp)) && mp) {
        cap_write_bmp(path, mp, w, h, pitch, dxgi);
        D3D12_RANGE wr = {0, 0};
        rb->lpVtbl->Unmap(rb, 0, &wr);
    }
    rb->lpVtbl->Release(rb);
}

static void rsx_capture_frame(u32 fi, u32 ndraws, u32 capidx)
{
    char dir[300]; const char* base = getenv("RSX_CAP_DIR");
    if (!base || !*base) base = "rsxcap";
    CreateDirectoryA(base, NULL);
    snprintf(dir, sizeof dir, "%s/frame_%02u", base, capidx);
    CreateDirectoryA(dir, NULL);
    char path[512];

    /* Manifest: every op in submission order with full state. */
    snprintf(path, sizeof path, "%s/manifest.txt", dir);
    FILE* mf = fopen(path, "w");
    if (mf) {
        fprintf(mf, "frame_count=%llu draws=%u backbuffer=%ux%u\n",
                (unsigned long long)s_d3d.frame_count, ndraws, s_d3d.width, s_d3d.height);
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].used)
                fprintf(mf, "offrt[%d] off=0x%08X %ux%u dxgi=%u\n", i, s_d3d.off_rt[i].off,
                        s_d3d.off_rt[i].w, s_d3d.off_rt[i].h, s_d3d.off_rt[i].dxgi);
        fprintf(mf, "--- ops ---\n");
        for (u32 d = 0; d < ndraws && d < MAX_DRAWS; d++) {
            const D3D12DrawRecord* r = &s_d3d.draws[d];
            if (r->is_clear)
                fprintf(mf, "op%03u CLEAR rt=0x%08X mrt=0x%X/0x%X/0x%X cc=(%.3f,%.3f,%.3f,%.3f) "
                            "mask=0x%X z=%.3f ct=0x%X zeta=0x%08X\n",
                        d, r->rt_off, r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                        r->cc[0], r->cc[1], r->cc[2], r->cc[3],
                        r->clr_flags, r->clr_z, r->ctgt, r->zeta);
            else {
                fprintf(mf, "op%03u DRAW  rt=0x%08X mrt=0x%X/0x%X/0x%X vp=%u,%u,%ux%u fp=0x%08X vs=%d "
                            "n=%u cmask=%X blend=%d bk=0x%X t0=0x%08X t1=0x%08X t2=0x%08X t3=0x%08X "
                            "ct=0x%X z=0x%08X zf=%u depth=0x%X cull=%u\n",
                        d, r->rt_off, r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                        r->vp_x, r->vp_y, r->vp_w, r->vp_h,
                        r->fp_addr, r->vs_idx, r->vertex_count, r->cmask, r->blend, r->blend_key,
                        r->tex[0].raw, r->tex[1].raw, r->tex[2].raw, r->tex[3].raw,
                        r->ctgt, r->zeta, r->zfmt, r->depth, r->cull);
                for (int u = 0; u < RSX_FP_UNITS; u++)
                    if (r->tex[u].set)
                        fprintf(mf, "      t%d off=0x%08X fmt=0x%02X (%s%s) %ux%u remap=0x%04X\n", u,
                                r->tex[u].off, r->tex[u].fmt,
                                (r->tex[u].fmt & 0x20) ? "LN" : "SZ",
                                (r->tex[u].fmt & 0x40) ? ",NR" : "",
                                r->tex[u].w, r->tex[u].h, r->tex[u].ctrl1 & 0xFFFF);
            }
        }
        /* Per-draw VP constant banks for the first few offscreen-RT geometry
         * draws: c[0..3] is the projection row-major for most CG VPs; also scan
         * for the largest non-zero slot so a mis-placed MVP is obvious. Records
         * live at slot==draw-index; parity is post-^1 here (capture is after the
         * frame's vp_parity flip), so read the other half. */
        if (s_d3d.vp_cb_mapped) {
            u32 par = (u32)(s_d3d.vp_parity ^ 1);
            fprintf(mf, "--- vp constants (first offscreen geometry draws) ---\n");
            int shown = 0;
            for (u32 d = 0; d < ndraws && d < MAX_DRAWS && shown < 4; d++) {
                const D3D12DrawRecord* r = &s_d3d.draws[d];
                if (r->is_clear || !r->rt_off) continue;
                const float* c = (const float*)((const char*)s_d3d.vp_cb_mapped
                    + ((u64)par * MAX_DRAWS + d) * VP_CB_STRIDE);
                int last_nz = -1; float maxabs = 0.0f;
                for (int i = 0; i < RSX_MAX_VERTEX_CONSTANTS * 4; i++) {
                    float a = c[i] < 0 ? -c[i] : c[i];
                    if (a > 1e-9f) last_nz = i;
                    if (a > maxabs) maxabs = a;
                }
                fprintf(mf, "op%03u vs=%d lastNZslot=%d maxabs=%.3f\n",
                        d, r->vs_idx, last_nz / 4, maxabs);
                if (shown == 0) {   /* full non-zero slot dump for the 1st draw */
                    for (int s = 0; s <= last_nz / 4; s++) {
                        const float* v = c + s * 4;
                        if ((v[0]||v[1]||v[2]||v[3]))
                            fprintf(mf, "   c[%3d] = %9.4f %9.4f %9.4f %9.4f\n",
                                    s, v[0], v[1], v[2], v[3]);
                    }
                }
                shown++;
            }
        }
        fclose(mf);
    }

    /* Backbuffer (in PRESENT state at capture time) + every used offscreen RT. */
    snprintf(path, sizeof path, "%s/backbuffer.bmp", dir);
    cap_readback_write(s_d3d.render_targets[fi], s_d3d.width, s_d3d.height,
                       DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PRESENT, fi, path);
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!r->res || !r->used) continue;
        snprintf(path, sizeof path, "%s/rt_%08X.bmp", dir, r->off);
        cap_readback_write(r->res, r->hw, r->hh, r->dxgi, r->st, fi, path);
    }
    printf("[RSXCAP] frame %llu -> %s (%u ops)\n",
           (unsigned long long)s_d3d.frame_count, dir, ndraws);
}

/* Decompile the captured RSX vertex program to HLSL and build the VP PSO
 * (decompiled VS + atlas alpha-test PS). One-shot per program. */
static void compile_vp(void)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st || st->vp_ucode_bytes < 16 || !s_d3d.vp_root_sig) return;

    static char hlsl[64 * 1024];
    int ni = rsx_vp_decompile(st->vp_ucode, st->vp_ucode_bytes, hlsl, sizeof hlsl);
    /* Does this VP read vp_c[0..3]? Gates the garbage-projection fallback:
     * programs keeping their MVP elsewhere (gcm/cube: c[256..259]) must not
     * have c[0..3] stomped nor the viewport z-lane overridden. */
    s_d3d.vp_uses_c03 = (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
                         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    if (ni <= 0) { printf("[VP] decompile failed (%d)\n", ni); s_d3d.vp_compiled_bytes = st->vp_ucode_bytes; return; }
    if (getenv("VP_DUMP")) {
        FILE* vf = fopen("vp_dump.hlsl", "w");
        if (vf) { fwrite(hlsl, 1, strlen(hlsl), vf); fclose(vf);
                  printf("[VP] dumped vp_dump.hlsl (%d instrs)\n", ni); }
        printf("[VPRAW] first 3 instrs (%u ucode bytes):\n", st->vp_ucode_bytes);
        for (u32 _q = 0; _q < 48 && _q < st->vp_ucode_bytes; _q += 16)
            printf("[VPRAW]  d0=%02X%02X%02X%02X d1=%02X%02X%02X%02X d2=%02X%02X%02X%02X d3=%02X%02X%02X%02X\n",
                st->vp_ucode[_q+0],st->vp_ucode[_q+1],st->vp_ucode[_q+2],st->vp_ucode[_q+3],
                st->vp_ucode[_q+4],st->vp_ucode[_q+5],st->vp_ucode[_q+6],st->vp_ucode[_q+7],
                st->vp_ucode[_q+8],st->vp_ucode[_q+9],st->vp_ucode[_q+10],st->vp_ucode[_q+11],
                st->vp_ucode[_q+12],st->vp_ucode[_q+13],st->vp_ucode[_q+14],st->vp_ucode[_q+15]);
    }

    /* Pixel shader mirrors dbgfont's FP: sample the atlas coverage at TEXCOORD0
     * (VP output o[7]), alpha-test at 0.5, output the vertex color (o[1]). */
    static const char ps[] =
        "Texture2D tex : register(t0); SamplerState smp : register(s0);\n"
        "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
        "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
        "float4 main(PSIn i):SV_TARGET{ float cov = tex.Sample(smp, i.t0.xy).r;\n"
        "  if (cov <= 0.5) discard; return float4(i.col0.rgb, 1); }\n";

    ID3DBlob *vb=NULL,*pb=NULL,*e=NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "vp", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &e);
    if (FAILED(hr)) { printf("[VP] VS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }
    hr = D3DCompile(ps, sizeof(ps)-1, "vpps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &e);
    if (FAILED(hr)) { printf("[VP] PS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); if(vb)vb->lpVtbl->Release(vb); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }

    /* The decompiled VS declares inputs a0:ATTR0 .. a15:ATTR15 and the HLSL
     * compiler keeps whichever the program body reads. D3D12 requires EVERY VS
     * input to have a matching input-layout element (by semantic name+index),
     * so declare all 16 ATTR slots. (The old single "POSITION" element matched
     * NO VS input once the decompiler switched to ATTR semantics -> PSO failed
     * with E_INVALIDARG for every VP, blanking cellmark's text and vkcube.)
     * The VP path uploads only attrib0 to vp_vb (one float4/vertex), so every
     * slot reads that same float4 at offset 0; attributes other than position
     * therefore alias attrib0 for now (colours/uv wrong until the VP path
     * uploads multiple attributes), but geometry is correct and the PSO is
     * valid. */
    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int _e = 0; _e < 16; _e++) {
        il[_e].SemanticName = "ATTR";
        il[_e].SemanticIndex = _e;
        il[_e].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        il[_e].InputSlot = 0;
        /* 256-byte generic VP vertex: every ATTRi is a float4 slot at i*16
         * (read_vp_vertex converts each enabled RSX attrib by type; disabled
         * slots hold (0,0,0,1)). Covers tiny3d (a0/a3/a8), SDK gcm samples
         * (a0/a1/a2), dbgfont -- no aliasing. */
        il[_e].AlignedByteOffset = (UINT)(_e * 16);
        il[_e].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_e].InstanceDataStepRate = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    /* Strip draws (vf_strip_native) cut at 0xFFFFFFFF; list draws never hold it. */
    pd.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    /* Depth test ON (LESS_EQUAL, matching the guest's rsxtiny_DepthTestFunc 515 /
     * CELL_GCM_LEQUAL). Without it the cube's faces drew in submission order and
     * back faces overwrote the front -> you saw through the near face to the
     * darker interior. The depth buffer is bound + cleared to 1.0 each frame. */
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleDesc.Count = 1;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_vp);

    /* Second PSO: same VS but a COLOUR-only PS (no texture sample, no alpha
     * discard) for untextured 3D geometry (vkcube's cube). The atlas PS above
     * samples a texture and discards where coverage<=0.5, which blanks any draw
     * with no atlas bound. */
    {
        static const char ps_col[] =
            "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
            "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
            "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
            "float4 main(PSIn i):SV_TARGET{ return float4(i.col0.rgb, 1); }\n";
        ID3DBlob *pcb=NULL,*ec=NULL;
        if (SUCCEEDED(D3DCompile(ps_col, sizeof(ps_col)-1, "vppsc", NULL, NULL,
                                 "main", "ps_5_0", 0, 0, &pcb, &ec)) && pcb) {
            pd.PS.pShaderBytecode = pcb->lpVtbl->GetBufferPointer(pcb);
            pd.PS.BytecodeLength  = pcb->lpVtbl->GetBufferSize(pcb);
            HRESULT hr2 = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pd, &IID_ID3D12PipelineState,
                (void**)&s_d3d.pipeline_state_vp_color);
            printf(SUCCEEDED(hr2) ? "[VP] colour pipeline ready\n"
                                  : "[VP] colour PSO FAIL (0x%08lX)\n", hr2);
            pcb->lpVtbl->Release(pcb);
        }
        if (ec) ec->lpVtbl->Release(ec);
    }
    /* Keep the VS bytecode for guest-FP PSO builds (vp_get_fp_pso); bump the
     * generation so cached FP PSOs built against the old VS are rebuilt. */
    if (s_d3d.vp_vs_blob) s_d3d.vp_vs_blob->lpVtbl->Release(s_d3d.vp_vs_blob);
    s_d3d.vp_vs_blob = vb;
    s_d3d.vp_gen++;
    pb->lpVtbl->Release(pb);
    s_d3d.vp_compiled_bytes = st->vp_ucode_bytes;
    if (SUCCEEDED(hr)) { s_d3d.vp_ready = 1; printf("[VP] pipeline ready (%d instrs)\n", ni); }
    else {
        printf("[VP] PSO creation FAIL (0x%08lX)\n", hr);
        /* Drain the debug layer's message queue to get the EXACT validation
         * reason (E_INVALIDARG is otherwise opaque). One-shot. */
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            for (UINT64 mi = 0; mi < n; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len)))
                    printf("[VP][DBG] %s\n", m->pDescription);
                free(m);
            }
            iq->lpVtbl->Release(iq);
        }
    }
}

/* Build (or fetch) a PSO pairing the current decompiled VS with the guest's
 * FRAGMENT program at fp_addr: read the FP ucode from guest memory, decompile
 * to HLSL (rsx_fp_decompiler), compile, and cache. This replaces the two
 * hardcoded pixel shaders for draws whose FP we can translate -- e.g.
 * gcm/cube's plasma FP `c = tex2D(t0, uv).x; out = (c, 0, c, 1)`. */
#include "rsx_fp_decompiler.h"

/* Compiled guest shaders on disk, so a program FXC has seen once never costs a
 * frame again. FXC takes ~100 ms for one of this title's large vertex
 * programs; a new area used to stall on dozens of them (PERF=1 "vs compile").
 * Keyed by the HLSL text itself plus the target profile, so any decompiler
 * change or diagnostic patch (VP_BYPASS, FP_SUB, ...) simply misses: there is
 * nothing to invalidate. One file per program:
 *   <dir>/<profile>_<fnv64 of profile+hlsl>.dxbc
 * <dir> is PS3_SHADER_CACHE, default "shader_cache" next to the executable;
 * PS3_SHADER_CACHE=0 turns the cache off. */
static double s_perf_fxc = 0.0;            /* time inside D3DCompile proper */
static int    s_perf_fxc_n = 0, s_perf_sc_hits = 0;
static double s_perf_cpso = 0.0;           /* CreateGraphicsPipelineState, guest-FP PSOs */
static int    s_perf_cpso_n = 0;
static double s_perf_pmiss = 0.0;          /* whole vp_get_fp_pso calls that missed */
static const char* shader_cache_dir(void)
{
    static char dir[MAX_PATH]; static int init = 0;
    if (!init) {
        init = 1;
        const char* e = getenv("PS3_SHADER_CACHE");
        if (e && !strcmp(e, "0")) dir[0] = 0;
        else if (e && *e) snprintf(dir, sizeof dir, "%s", e);
        else {
            char exe[MAX_PATH];
            DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
            char* slash = (n && n < sizeof exe) ? strrchr(exe, '\\') : NULL;
            if (slash) { *slash = 0; snprintf(dir, sizeof dir, "%s\\shader_cache", exe); }
            else snprintf(dir, sizeof dir, "shader_cache");
        }
        if (dir[0]) CreateDirectoryA(dir, NULL);
    }
    return dir;
}
static u64 shader_key(const char* src, size_t n, const char* profile)
{
    u64 h = 14695981039346656037ull;
    for (const char* p = profile; *p; p++) { h ^= (u8)*p; h *= 1099511628211ull; }
    h ^= 0xFF; h *= 1099511628211ull;
    for (size_t i = 0; i < n; i++) { h ^= (u8)src[i]; h *= 1099511628211ull; }
    return h;
}
/* A cached file is used only if it is a whole DXBC container: the header's
 * total-size field (offset 24) must match the file length. */
static ID3DBlob* shader_cache_load(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    ID3DBlob* b = NULL;
    if (n > 32 && SUCCEEDED(D3DCreateBlob((SIZE_T)n, &b))) {
        u8* p = (u8*)b->lpVtbl->GetBufferPointer(b);
        if (fread(p, 1, (size_t)n, f) != (size_t)n || memcmp(p, "DXBC", 4) ||
            ((u32)p[24] | ((u32)p[25] << 8) | ((u32)p[26] << 16) | ((u32)p[27] << 24)) != (u32)n) {
            b->lpVtbl->Release(b); b = NULL;
        }
    }
    fclose(f);
    return b;
}
/* Written to a temporary name and renamed, so a crash or a second process
 * never leaves a half-written program under the real name. */
static void shader_cache_store(const char* path, ID3DBlob* b)
{
    char tmp[MAX_PATH + 32];
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentThreadId());
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    const size_t n = b->lpVtbl->GetBufferSize(b);
    const int ok = fwrite(b->lpVtbl->GetBufferPointer(b), 1, n, f) == n;
    if (fclose(f) || !ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
}
static HRESULT shader_compile_cached(const char* src, size_t n, const char* name,
                                     const char* profile, ID3DBlob** out, ID3DBlob** err)
{
    char path[MAX_PATH + 64]; path[0] = 0;
    const char* dir = shader_cache_dir();
    if (dir[0]) {
        snprintf(path, sizeof path, "%s\\%s_%016llx.dxbc", dir, profile,
                 (unsigned long long)shader_key(src, n, profile));
        if ((*out = shader_cache_load(path)) != NULL) { if (perf_on()) s_perf_sc_hits++; return S_OK; }
    }
    const double t0 = perf_on() ? perf_now() : 0.0;
    HRESULT hr = D3DCompile(src, n, name, NULL, NULL, "main", profile, 0, 0, out, err);
    if (perf_on()) { s_perf_fxc += perf_now() - t0; s_perf_fxc_n++; }
    if (SUCCEEDED(hr) && *out && path[0]) {
        shader_cache_store(path, *out);
        /* PS3_SHADER_CACHE_HLSL=1: keep the source beside each program, to
         * see why a precompiled entry and a draw's lookup differ. */
        static int keep = -1;
        PS3_KNOB(keep, ps3_env_int("PS3_SHADER_CACHE_HLSL", 0));
        if (keep) {
            char hp[MAX_PATH + 72];
            snprintf(hp, sizeof hp, "%s.hlsl", path);
            FILE* f = fopen(hp, "wb");
            if (f) { fwrite(src, 1, n, f); fclose(f); }
        }
    }
    return hr;
}

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
static SRWLOCK       s_pso_lib_lock = SRWLOCK_INIT;   /* Load/Store; the manifest */
static volatile LONG s_pso_lib_new;         /* pipelines stored since the last save */
static volatile LONG s_pso_lib_saving;
static u32           s_pso_lib_quiet;       /* presents since the last store */
static u32           s_pso_lib_hits, s_pso_lib_live;   /* draws: loaded / compiled */

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
static volatile LONG s_pso_pre_next, s_pso_pre_live, s_pso_pre_built, s_pso_pre_abort;
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
    snprintf(out, n, "%s\\%s", shader_cache_dir(), leaf);
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
    printf("[D3D12] pipeline library: %s (%ld KB), %u known pipelines, %u to build\n",
           SUCCEEDED(hr) ? "loaded" : "new", n >> 10, cnt, cnt - s_pso_lib_have);
}
/* Serialize can fail when a pipeline is stored between sizing and writing;
 * the next attempt sizes again. pso_library.n follows the library, so after a
 * crash between the two it only undercounts, and the next startup rebuilds a
 * few pipelines the library already had. */
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
                       (unsigned long)(n >> 10), recs, s_pso_lib_hits, s_pso_lib_live);
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
    InterlockedExchange(&s_pso_lib_saving, 0);
    return 0;
}
/* Keep a pipeline's description and shaders so a later startup can build it. */
static void pso_manifest_add(u64 name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd)
{
    if (pd->DS.pShaderBytecode || pd->HS.pShaderBytecode || pd->GS.pShaderBytecode ||
        !pd->VS.pShaderBytecode || !pd->PS.pShaderBytecode || pd->InputLayout.NumElements > 16 ||
        pd->StreamOutput.NumEntries)
        return;
    pso_rec r;
    memset(&r, 0, sizeof r);
    r.magic = PSO_REC_MAGIC; r.size = sizeof r; r.name = name;
    memcpy(&r.desc, pd, sizeof r.desc);
    r.desc.pRootSignature = NULL;
    r.desc.VS.pShaderBytecode = r.desc.PS.pShaderBytecode = NULL;
    r.desc.InputLayout.pInputElementDescs = NULL;
    r.desc.CachedPSO.pCachedBlob = NULL; r.desc.CachedPSO.CachedBlobSizeInBytes = 0;
    r.nel = pd->InputLayout.NumElements;
    for (u32 i = 0; i < r.nel; i++) {
        r.el[i].e = pd->InputLayout.pInputElementDescs[i];
        snprintf(r.el[i].sem, sizeof r.el[i].sem, "%s", r.el[i].e.SemanticName);
        r.el[i].e.SemanticName = NULL;
    }
    const D3D12_SHADER_BYTECODE* sh[2] = { &pd->VS, &pd->PS };
    u64* hs[2] = { &r.vs_hash, &r.ps_hash };
    u32* ls[2] = { &r.vs_len, &r.ps_len };
    char dir[MAX_PATH + 32];
    pso_path(dir, sizeof dir, "pso_blobs");
    CreateDirectoryA(dir, NULL);
    for (int i = 0; i < 2; i++) {
        *hs[i] = fnv_bytes(14695981039346656037ull, sh[i]->pShaderBytecode, sh[i]->BytecodeLength);
        *ls[i] = (u32)sh[i]->BytecodeLength;
        char bp[MAX_PATH + 64];
        snprintf(bp, sizeof bp, "%s\\%016llx.dxbc", dir, (unsigned long long)*hs[i]);
        if (GetFileAttributesA(bp) == INVALID_FILE_ATTRIBUTES &&
            !pso_write_file(bp, sh[i]->pShaderBytecode, sh[i]->BytecodeLength))
            return;
    }
    char mp[MAX_PATH + 32];
    pso_path(mp, sizeof mp, "pso_manifest.bin");
    AcquireSRWLockExclusive(&s_pso_lib_lock);
    if (!pso_names_has(name)) {
        FILE* f = fopen(mp, "ab");
        if (f) {
            if (fwrite(&r, sizeof r, 1, f) == 1) { pso_names_add(name); s_pso_man_n++; }
            fclose(f);
        }
    }
    ReleaseSRWLockExclusive(&s_pso_lib_lock);
}
/* One startup build: the record's shaders from pso_blobs, its description
 * back together, and its name checked against the rebuilt description. */
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
        snprintf(bp, sizeof bp, "%s\\%016llx.dxbc", dir, (unsigned long long)hs[i]);
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
    ID3D12PipelineState* pso = NULL;
    if (SUCCEEDED(s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
            s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso))) {
        WCHAR name[20];
        _snwprintf(name, 20, L"%016llx", (unsigned long long)r->name);
        AcquireSRWLockExclusive(&s_pso_lib_lock);
        if (SUCCEEDED(s_pso_lib->lpVtbl->StorePipeline(s_pso_lib, name, pso)))
            InterlockedIncrement(&s_pso_lib_new);
        ReleaseSRWLockExclusive(&s_pso_lib_lock);
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
        if (pso_prebuild_one(&s_pso_man[i])) InterlockedIncrement(&s_pso_pre_built);
    }
    if (InterlockedDecrement(&s_pso_pre_live) == 0)
        printf("[D3D12] pipeline prebuild: %ld of %u built in %.0f ms\n", s_pso_pre_built,
               s_pso_man_read - s_pso_lib_have, 1000.0 * (perf_now() - s_pso_pre_t0));
    return 0;
}
/* Once per present, on the render thread. Starts the startup build once the
 * root signature exists, and saves in the background once that build is done
 * and a burst of new pipelines has settled (120 presents with none, or 256
 * pending). */
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
    if (!s_pso_lib_new || s_pso_lib_saving || s_pso_pre_live) return;
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
    _snwprintf(name, 20, L"%016llx", (unsigned long long)key);
    AcquireSRWLockExclusive(&s_pso_lib_lock);
    HRESULT hr = s_pso_lib->lpVtbl->LoadGraphicsPipeline(s_pso_lib, name, pd,
                     &IID_ID3D12PipelineState, (void**)out);
    ReleaseSRWLockExclusive(&s_pso_lib_lock);
    if (SUCCEEDED(hr)) { s_pso_lib_hits++; return S_OK; }
    *out = NULL;
    s_pso_lib_live++;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
        s_d3d.device, pd, &IID_ID3D12PipelineState, (void**)out);
    if (SUCCEEDED(hr)) {
        AcquireSRWLockExclusive(&s_pso_lib_lock);
        const HRESULT st = s_pso_lib->lpVtbl->StorePipeline(s_pso_lib, name, *out);
        ReleaseSRWLockExclusive(&s_pso_lib_lock);
        if (SUCCEEDED(st)) { InterlockedIncrement(&s_pso_lib_new); s_pso_lib_quiet = 0; }
        pso_manifest_add(key, pd);
    }
    return hr;
}

/* Hash + compile the CURRENT rsx_state vertex program into the VS cache;
 * returns the cache slot (or -1). Called at draw-record time so each draw
 * carries the VP that was loaded when it was submitted. */
static u32 vp_hash_ucode(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1u;
}

/* Append a program to the per-draw VS cache. vs NULL records a program that
 * failed to decompile or compile, so it is not retried on every draw. */
static int vp_vs_insert(u32 hash, ID3DBlob* vs, int uses_c03)
{
    if (s_d3d.vp_vs_n >= s_d3d.vp_vs_cap) {
        const int cap = s_d3d.vp_vs_cap ? s_d3d.vp_vs_cap * 2 : VP_VS_CACHE_INIT;
        VPVSEntry* grown = (VPVSEntry*)realloc(s_d3d.vp_vs, sizeof(VPVSEntry) * (size_t)cap);
        if (!grown) { if (vs) vs->lpVtbl->Release(vs); return -1; }
        s_d3d.vp_vs = grown; s_d3d.vp_vs_cap = cap;
    }
    const int slot = s_d3d.vp_vs_n++;
    s_d3d.vp_vs[slot].hash     = hash;
    s_d3d.vp_vs[slot].vs       = vs;
    s_d3d.vp_vs[slot].uses_c03 = uses_c03;
    s_d3d.vp_vs[slot].fetch    = 0;
    return vs ? slot : -1;
}

static int vp_get_vs_mode(const rsx_state* st, int fetch);
static int vp_get_vs(const rsx_state* st) { return vp_get_vs_mode(st, 0); }
/* The same program decompiled to read its vertex arrays itself (fetch mode):
 * its own cache slot, keyed by the ucode hash with a fetch salt. */
static int vp_get_vs_fetch(const rsx_state* st) { return vp_get_vs_mode(st, 1); }
static int vp_get_vs_mode(const rsx_state* st, int fetch)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    extern int rsx_vp_decompile_fetch(const uint8_t*, u32, char*, u32);
    extern u32 rsx_vp_program_size_instrs(const u8*, u32);
    if (!st || st->vp_ucode_bytes < 16) return -1;
    /* Start at the instruction SET_TRANSFORM_PROGRAM_START selects, not at 0.
     * The microcode store holds every resident program -- this title keeps its
     * scene, fluid, caustics and droplet programs in it at once -- so
     * decompiling from 0 ran the scene's transform for every draw. That is why
     * the fluid's 114300 vertices rasterized nothing: they were being pushed
     * through the wrong program.
     * VP_START_OFF=1 restores the old always-from-zero behaviour. */
    u32 vstart = 0;
    { static int off = -1; PS3_KNOB(off, getenv("VP_START_OFF") ? 1 : 0);
      if (!off) vstart = st->transform_program_start * 16u; }
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    /* Sizing and hashing the program, then scanning the cache, ran for every
     * draw. While the store has not been written since, a program start
     * resolves to the slot it resolved to before (if that slot still holds
     * the same program). RSX_VP_MEMO=0 looks it up every time. */
    extern u32 g_rsx_vp_ucode_ver;
    typedef struct { const rsx_state* st; u32 ver, vstart, hash; int idx; } VsMemo;
    static VsMemo memo[2][64];
    static int memo_en = -1;
    PS3_KNOB(memo_en, ps3_env_int("RSX_VP_MEMO", 1));
    VsMemo* m = &memo[fetch ? 1 : 0][(vstart >> 4) & 63];
    if (memo_en && m->st == st && m->ver == g_rsx_vp_ucode_ver && m->vstart == vstart &&
        m->idx >= 0 && m->idx < s_d3d.vp_vs_n && s_d3d.vp_vs[m->idx].hash == m->hash)
        return s_d3d.vp_vs[m->idx].vs ? m->idx : -1;
    const u8* vuc = st->vp_ucode + vstart;
    u32 vlen = st->vp_ucode_bytes - vstart;
    /* Key the program by its own instructions, up to its end bit. The store
     * holds every resident program, so hashing to the end of the store made
     * each program's key change whenever any program after it was uploaded. */
    { const u32 instrs = rsx_vp_program_size_instrs(vuc, vlen);
      if (instrs) vlen = instrs * 16u; }
    u32 hash = vp_hash_ucode(vuc, vlen);
    if (fetch) hash ^= 0x9E3779B9u;
    for (int i = 0; i < s_d3d.vp_vs_n; i++)
        if (s_d3d.vp_vs[i].hash == hash) {
            m->st = st; m->ver = g_rsx_vp_ucode_ver; m->vstart = vstart;
            m->hash = hash; m->idx = i;
            return s_d3d.vp_vs[i].vs ? i : -1;
        }

    static char hlsl[262144];
    const double vs_t0 = perf_on() ? perf_now() : 0.0;
    s_perf_vs_compiles++;
    int ni = fetch ? rsx_vp_decompile_fetch(vuc, vlen, hlsl, sizeof hlsl)
                   : rsx_vp_decompile(vuc, vlen, hlsl, sizeof hlsl);
    if (ni <= 0) return vp_vs_insert(hash, NULL, 0);
    if (getenv("VP_DUMP")) { static int _d=0; if (_d++ < 4) {
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
    /* VP_BYPASS=1: replace the guest transform with a direct map of attribute 0
     * into clip space. If geometry appears with this on, everything from the
     * input layout through raster/depth/present is sound and the fault is the
     * transform (constants or the decompiled program); if it stays blank, the
     * fault is upstream of the shader -- the attribute binding itself. Patched
     * in place, space-padded to the original length so offsets stay valid. */
    /* VP_W_ONE=1: keep the guest transform but force the clip-space w to 1.
     * Different from VP_BYPASS, which replaces the whole position: this tests
     * only the hypothesis that w is wrong. This title's VP ends
     *   { float4 _v = (float4)((vp_c[467]).xxxx); o[0].w = _v.w; }
     * with c467 = (-1, 1, 0, 0), so w = -1 and the clipper discards every
     * vertex. Patched in place, space-padded so offsets stay valid. */
    if (getenv("VP_W_ONE")) {
        static const char anchor[] = "o[0].w = _v.w;";
        static const char repl[]   = "o[0].w = 1.0;";
        char* at = hlsl;
        while ((at = strstr(at, anchor)) != NULL) {
            size_t n = sizeof(anchor) - 1, m = sizeof(repl) - 1;
            memcpy(at, repl, m);
            memset(at + m, ' ', n - m);
            at += n;
        }
    }
    if (getenv("VP_BYPASS")) {
        static const char anchor[] =
            "Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);";
        static const char repl[] = "Out.pos = float4(v[0].xy * 0.5, 0.5, 1.0);";
        char* at = hlsl;
        while ((at = strstr(at, anchor)) != NULL) {
            size_t n = sizeof(anchor) - 1, m = sizeof(repl) - 1;
            memcpy(at, repl, m);
            memset(at + m, ' ', n - m);
            at += n;
        }
    }
    ID3DBlob* vb = NULL; ID3DBlob* e = NULL;
    HRESULT hr = shader_compile_cached(hlsl, strlen(hlsl), "guest_vp2", "vs_5_0", &vb, &e);
    if (e) e->lpVtbl->Release(e);
    if (perf_on()) s_perf_vs += perf_now() - vs_t0;
    if (FAILED(hr) || !vb) {
        static int _e=0; if (_e++<4) printf("[VP] per-draw VS compile FAIL (hash=0x%08X)\n", hash);
        return vp_vs_insert(hash, NULL, 0);
    }
    const int slot = vp_vs_insert(hash, vb,
        (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0);
    if (slot < 0) return -1;
    s_d3d.vp_vs[slot].fetch = fetch;
    { static int _n=0; if (_n++<6) printf("[VP] per-draw VS cached (hash=0x%08X, %d instrs, slot %d)\n", hash, ni, slot); }
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

/* --- Guest blend state -> packed PSO key ----------------------------------
 * RSX blend factors/equation carry GL enums; each SFACTOR/DFACTOR/EQUATION
 * word packs (alpha<<16)|colour. The key packs D3D12 enums so the PSO cache
 * distinguishes real blend modes: bit0 enable, [5:1] srcC, [10:6] dstC,
 * [15:11] srcA, [20:16] dstA, [23:21] opC, [26:24] opA. DeferredShading's
 * light accumulation is additive (ONE,ONE) -- the old hardcoded straight-
 * alpha PSO turned each light pass into a screen-blanking flash. */
static u8 gl_blend_factor_d3d(u32 f, int is_alpha)
{
    switch (f & 0xFFFF) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return is_alpha ? D3D12_BLEND_SRC_ALPHA     : D3D12_BLEND_SRC_COLOR;
    case 0x0301: return is_alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return is_alpha ? D3D12_BLEND_DEST_ALPHA     : D3D12_BLEND_DEST_COLOR;
    case 0x0307: return is_alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_COLOR   */
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_ALPHA ~ */
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return D3D12_BLEND_ONE;
    }
}
static u8 gl_blend_op_d3d(u32 e)
{
    switch (e & 0xFFFF) {
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    default:     return D3D12_BLEND_OP_ADD;   /* 0x8006 FUNC_ADD / unset */
    }
}
/* Pack the guest's face-culling state for the PSO key.
 * bit0 = cull enabled, bit1 = cull FRONT (else BACK), bit2 = front face is CCW.
 * RSX/GL enums: CULL_FACE FRONT=0x0404 BACK=0x0405 FRONT_AND_BACK=0x0408;
 * FRONT_FACE CW=0x0900 CCW=0x0901. rsx_commands seeds the reset state, BACK
 * and CCW. */
static u32 rsx_cull_key(const rsx_state* st)
{
    if (!st || !st->cull_face_enable) return 0;
    u32 k = 1u;
    if (st->cull_face == 0x0404u || st->cull_face == 0x0408u) k |= 2u;  /* FRONT */
    if (st->front_face == 0x0901u) k |= 4u;                             /* CCW */
    return k;
}

/* Pack the guest's depth state for the PSO key: bit0 test enabled, bit1
 * writes on, bits 2-4 the compare function (GL enum 0x0200 NEVER .. 0x0207
 * ALWAYS, in the same order as D3D12_COMPARISON_FUNC minus one). */
static u32 rsx_depth_key(const rsx_state* st)
{
    if (!st || !st->depth_test_enable) return 0;
    return 1u | (st->depth_mask ? 2u : 0u) | ((st->depth_func & 7u) << 2);
}

static u32 rsx_blend_key(const rsx_state* st, int enable)
{
    { static int _bd = -1; PS3_KNOB(_bd, getenv("BLENDDBG") ? 1 : 0);
      if (_bd) { static u32 seen[32]; static int ns=0;
        u32 sk = (enable?0x80000000u:0u) | (st ? (st->blend_sfactor & 0xFFFFu) : 0u);
        int f=0; for (int k2=0;k2<ns;k2++) if (seen[k2]==sk) f=1;
        if (!f && ns<32) { seen[ns++]=sk;
            fprintf(stderr, "[BLEND] enable=%d sfactor=0x%X dfactor=0x%X%c", enable,
                    st?st->blend_sfactor:0, st?st->blend_dfactor:0, 10); } } }
    if (!enable) return 0;
    /* Factors never programmed: keep the legacy straight-alpha behaviour
     * (dbgfont-style text enables blending without setting factors). */
    if (!st || (st->blend_sfactor == 0 && st->blend_dfactor == 0))
        return 1u
             | ((u32)D3D12_BLEND_SRC_ALPHA     << 1)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 6)
             | ((u32)D3D12_BLEND_ONE           << 11)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 16)
             | ((u32)D3D12_BLEND_OP_ADD << 21) | ((u32)D3D12_BLEND_OP_ADD << 24);
    return 1u
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor,       0) << 1)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor,       0) << 6)
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor >> 16, 1) << 11)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor >> 16, 1) << 16)
         | ((u32)gl_blend_op_d3d(st->blend_equation)       << 21)
         | ((u32)gl_blend_op_d3d(st->blend_equation >> 16) << 24);
}

/* Bound colour target count for a draw record: 1 (target A) + the contiguous
 * prefix of MRT B/C/D offsets. */
/* RSX depth texture formats (base, LN/UN stripped): DEPTH24_D8, its float
 * variant, DEPTH16, DEPTH16_FLOAT. They sample a depth surface. */
static int rsx_fmt_is_depth(u32 basef) { return basef >= 0x90 && basef <= 0x93; }

/* A unit samples with a depth comparison (a shadow map) when it holds a depth
 * format and its TEXTURE_ADDRESS zfunc [31:28] is a real test -- NEVER (0)
 * and ALWAYS (7) give constant results, so RSX reads the depth instead. */
static int dr_unit_shadow(const D3D12DrawRecord* dr, int u)
{
    if (!dr || !dr->tex[u].set || !rsx_fmt_is_depth(dr->tex[u].fmt & 0x9F)) return 0;
    const u32 zf = (dr->tex[u].addr >> 28) & 0xF;
    return zf > 0 && zf < 7;
}

static u32 dr_shadow_mask(const D3D12DrawRecord* dr)
{
    u32 m = 0;
    for (int u = 0; u < RSX_FP_UNITS; u++) if (dr_unit_shadow(dr, u)) m |= 1u << u;
    return m;
}

/* Colour targets bound for a draw: 0 for a depth-only pass (colour target
 * NONE, as Demon's Souls's shadow cascades), else A plus any MRTs. */
static int dr_num_rts(const D3D12DrawRecord* dr)
{
    if (dr->ctgt == 0) return 0;
    int n = 1;
    while (n < 4 && dr->rt_mrt[n - 1]) n++;
    return n;
}

/* Which of a draw's texture units are cube textures, as a bit mask; bits
 * 16-31 mark the units sampled with a depth comparison (dr_shadow_mask). The
 * decompiler takes both from this one word, and it is part of the PSO key. */
static u32 dr_cube_mask(const D3D12DrawRecord* dr)
{
    /* ON by default (RSX_CUBE_TEX=0 to disable). This was off while the cube path
     * cost 2262 PSO misses per 20 frames and 0.38 fps -- both caused by inline
     * fragment-program constants baking into the shader source. With those
     * constants hoisted into b1 the cube path costs 0 PSO misses, and with the
     * per-face stride fixed (each face is a whole mip pyramid, not one mip-0
     * image) all six faces decode as clean environment art. */
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("RSX_CUBE_TEX", 1));
    const u32 sh = dr_shadow_mask(dr) << 16;
    if (!en) return sh;
    u32 m = 0;
    for (int u = 0; u < RSX_FP_UNITS; u++) if (dr->tex[u].set && dr->tex[u].cube) m |= 1u << u;
    return m | sh;
}

/* The guest-FP PSO cache is looked up for every draw, and holds thousands of
 * keys once a level has been played: a scan of the whole array per draw was a
 * measurable part of the render thread's frame at 6k draws. Bucketed by
 * fp_addr. */
#define VP_FP_BUCKETS 4096
static int s_vp_fp_head[VP_FP_BUCKETS];
static u32 vp_fp_bucket(u32 fp_addr) { return ((fp_addr >> 4) * 2654435761u) >> 20; }

/* Append a finished (or failed: pso NULL) key to the guest-FP PSO cache. */
static ID3D12PipelineState* vp_fp_insert(const VPFPEntry* e)
{
    if (s_d3d.vp_fp_n >= s_d3d.vp_fp_cap) {
        const int cap = s_d3d.vp_fp_cap ? s_d3d.vp_fp_cap * 2 : VP_FP_CACHE_INIT;
        VPFPEntry* grown = (VPFPEntry*)realloc(s_d3d.vp_fp, sizeof(VPFPEntry) * (size_t)cap);
        if (!grown) return e->pso;              /* uncached, still usable */
        s_d3d.vp_fp = grown; s_d3d.vp_fp_cap = cap;
    }
    const int idx = s_d3d.vp_fp_n++;
    s_d3d.vp_fp[idx] = *e;
    VPFPEntry* n = &s_d3d.vp_fp[idx];
    int* head = &s_vp_fp_head[vp_fp_bucket(e->fp_addr)];
    n->next = *head - 1;     /* heads are stored +1 so 0 means empty */
    *head = idx + 1;
    return e->pso;
}

/* The entry with exactly this key, or NULL. Entries are only appended, so a
 * bucket's chain stays valid as the array grows. */
static VPFPEntry* vp_fp_find(const VPFPEntry* k)
{
    for (int i = s_vp_fp_head[vp_fp_bucket(k->fp_addr)] - 1; i >= 0; i = s_d3d.vp_fp[i].next) {
        VPFPEntry* q = &s_d3d.vp_fp[i];
        if (q->fp_addr == k->fp_addr && q->vs_idx == k->vs_idx &&
            q->vs_hash == k->vs_hash && q->gen == k->gen &&
            q->blend == k->blend && q->nrt == k->nrt &&
            q->rtfmt == k->rtfmt && q->exp32 == k->exp32 &&
            q->ucode_hash == k->ucode_hash && q->cmask == k->cmask &&
            q->cull == k->cull && q->depth == k->depth &&
            q->cube_mask == k->cube_mask && q->d2c_mask == k->d2c_mask)
            return q;
    }
    return NULL;
}

/* The debug rewrites below (FP_ONE, FP_SHOW, FP_IDCOLOR, FP_TEX, FP_KILL) edit
 * the colour-0 output of a translated program. Its spelling depends on the
 * decompiler version: "_po.c0 = X;" (older), "float4 _o = X;" (one target) or
 * "float4 _o0 = X;" (several). They anchored on the first only and silently did
 * nothing with the current decompiler. Returns the start of X (or of the whole
 * statement with want_stmt) and sets *end to its ';', or NULL. */
static char* fp_out_anchor(char* hlsl, char** end, int want_stmt)
{
    static const char* const pats[] = { "_po.c0 = ", "float4 _o = ", "float4 _o0 = " };
    for (int i = 0; i < 3; i++) {
        char* rp = strstr(hlsl, pats[i]);
        char* semi = rp ? strchr(rp, ';') : NULL;
        if (semi) { *end = semi; return want_stmt ? rp : rp + strlen(pats[i]); }
    }
    return NULL;
}

/* Replace the colour-0 expression with expr. Returns 1 if it was replaced. */
static int fp_set_out(char* hlsl, size_t cap, const char* expr)
{
    char* semi; char* rp = fp_out_anchor(hlsl, &semi, 0);
    if (!rp) return 0;
    size_t newlen = strlen(expr), tail = strlen(semi) + 1;
    if ((size_t)(rp - hlsl) + newlen + tail >= cap) return 0;
    memmove(rp + newlen, semi, tail);
    memcpy(rp, expr, newlen);
    return 1;
}

/* Insert `ins` in front of the ')' that closes the call whose '(' is at open.
 * Returns the position just past the inserted text, or NULL. */
static char* hlsl_insert_before_close(char* hlsl, size_t cap, char* open, const char* ins)
{
    int depth = 0;
    char* p = open;
    for (; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') { if (--depth == 0) break; }
    }
    if (*p != ')') return NULL;
    const size_t il = strlen(ins), used = strlen(hlsl);
    if (used + il + 1 > cap) return NULL;
    memmove(p + il, p, used - (size_t)(p - hlsl) + 1);
    memcpy(p, ins, il);
    return p + il;
}

/* Wire a decompiled program to this backend's per-draw b1 slot (see
 * VP_FPCB_STRIDE and vp_record_cb):
 *  - its cbuffer declaration becomes the b1 layout (texel scales, alpha test,
 *    inline constants);
 *  - each 2D Sample() coordinate is multiplied by rsx_texscale[unit], which is
 *    1/size for a texture addressed in texels (format bit 0x40) and 1 otherwise;
 *  - the alpha test runs on colour target 0 before returning (D3D12 has no
 *    fixed-function one). Its state is per draw, so the pipeline key does not
 *    need it.
 * The previous decompiler emitted the scale and test itself; the current one
 * leaves them to the backend, and this path had lost both. Returns 0 on
 * failure (buffer too small or an unexpected program shape). */
static int fp_bind_b1(char* hlsl, size_t cap, u32 cube_mask, int buffered)
{
    static const char decl[] =
        "cbuffer PSConstants : register(b1) {\n"
        "    float4 rsx_texscale[16];\n"
        "    float4 rsx_alphatest;   /* x enable, y reference, z function - 0x200 */\n"
        "    float4 fp_constants[128];\n"
        "};\n"
        "bool rsx_alpha_pass(float a) {\n"
        "    const float r = rsx_alphatest.y; const int f = (int)rsx_alphatest.z;\n"
        "    return f == 1 ? a < r : f == 2 ? a == r : f == 3 ? a <= r :\n"
        "           f == 4 ? a > r : f == 5 ? a != r : f == 6 ? a >= r : f == 7;\n"
        "}\n";
    /* Replace the decompiler's declaration, or add ours in front of main. */
    char* at = strstr(hlsl, "cbuffer PSConstants : register(b1) {");
    char* end = at ? strstr(at, "};\n") : NULL;
    if (at && !end) return 0;
    if (!buffered && at) return 0;
    if (!at) {
        at = strstr(hlsl, "float4 main(");
        if (!at) at = strstr(hlsl, "struct PSOutput {");
        if (!at) return 0;
        end = at;
    } else {
        end += 3;
    }
    {
        const size_t dl = sizeof(decl) - 1, used = strlen(hlsl);
        const size_t old = (size_t)(end - at);
        if (used - old + dl + 1 > cap) return 0;
        memmove(at + dl, end, used - (size_t)(end - hlsl) + 1);
        memcpy(at, decl, dl);
    }
    /* Texel scale on every 2D Sample() of each unit. */
    for (u32 u = 0; u < RSX_FP_UNITS; u++) {
        if ((cube_mask >> u) & 1u) continue;
        /* Every spelling the decompiler emits for a 2D unit: a program with a
         * shadow unit declares its samplers one by one (rsx_sampN). */
        char needle[3][48], ins[32];
        snprintf(needle[0], sizeof needle[0], "rsx_tex[%u].Sample(rsx_samp[%u],", u, u);
        snprintf(needle[1], sizeof needle[1], "rsx_tex%u.Sample(rsx_samp[%u],", u, u);
        snprintf(needle[2], sizeof needle[2], "rsx_tex%u.Sample(rsx_samp%u,", u, u);
        snprintf(ins, sizeof ins, " * rsx_texscale[%u].xy", u);
        for (int k = 0; k < 3; k++) {
            char* from = hlsl;
            char* hit;
            while ((hit = strstr(from, needle[k])) != NULL) {
                char* open = strchr(hit, '(');
                from = open ? hlsl_insert_before_close(hlsl, cap, open, ins) : NULL;
                if (!from) return 0;
            }
        }
    }
    /* Alpha test on target 0, just before the final return. */
    {
        char* ret = NULL;
        for (char* p = strstr(hlsl, "    return "); p; p = strstr(p + 1, "    return "))
            ret = p;
        if (!ret) return 0;
        const char* test = strstr(hlsl, "struct PSOutput")
            ? "    if (rsx_alphatest.x > 0.5 && !rsx_alpha_pass(_out.t0.a)) discard;\n"
            : "    if (rsx_alphatest.x > 0.5 && !rsx_alpha_pass(_o.a)) discard;\n";
        if (!strstr(hlsl, "float4 _o = ") && !strstr(hlsl, "PSOutput _out;")) return 0;
        const size_t tl = strlen(test), used = strlen(hlsl);
        if (used + tl + 1 > cap) return 0;
        memmove(ret + tl, ret, used - (size_t)(ret - hlsl) + 1);
        memcpy(ret, test, tl);
    }
    return 1;
}

/* Knobs baked into a pipeline when it is built. The cache keys don't hold
 * them, so after the console changes one of them every pipeline is rebuilt
 * (vp_gen); other setenvs leave the cache alone. */
static void vp_pso_knobs_check(void)
{
    static unsigned seen_gen = 0, sig = 0;
    if (seen_gen == ps3_env_gen) return;
    seen_gen = ps3_env_gen;
    static const char* const names[] = {
        "CULL_OFF", "CULL_FLIP", "DEPTH_OFF", "CMASK_FORCE", "FP_CONSTBUF", "FP_FORCE",
        "FP_IDCOLOR", "FP_KILL", "FP_OFF", "FP_ONE", "FP_PICK", "FP_SHOW", "FP_SUB",
        "FP_TEX", "FP_TEX_FP", "FP_TEX_UV", NULL };
    unsigned h = 2166136261u;
    for (int i = 0; names[i]; i++) {
        const char* v = getenv(names[i]);
        for (const char* c = v ? v : ""; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
        h = (h ^ 0xFFu) * 16777619u;
    }
    if (sig && h != sig) {
        s_d3d.vp_gen++;
        fprintf(stderr, "[D3D12] pipeline knobs changed: rebuilding pipelines%c", 10);
    }
    sig = h;
}

/* The HLSL the draw path compiles for a fragment program: decompiled with its
 * inline constants read from b1 (baked in when there are more than b1 holds),
 * then given the b1 layout. The offline precompiler builds the same text, so
 * its disk-cache entries are the ones a draw looks up. ucode must be readable
 * for RSX_FP_MAX_BYTES. Returns the instruction count, <= 0 on failure;
 * fp_addr is only for the log. */
/* Fragment-program variants this title has compiled, by code hash, in
 * <shader cache>/fp_variants.txt ("hash exp32 cube d2c" per line, hex). The
 * texture kinds a program is bound with (cube, shadow, depth-as-colour) are
 * draw state that a Cg binary does not record, so the precompiler reads this
 * list to rebuild every variant a run has needed: a cache emptied by a
 * decompiler or driver update is refilled whole before the next boot. */
typedef struct { u32 hash, exp32, cube, d2c; } FPVariant;
static FPVariant* s_fpv = NULL;
static int s_fpv_n = 0, s_fpv_cap = 0, s_fpv_loaded = 0;
static int fpv_find(u32 hash, u32 exp32, u32 cube, u32 d2c)
{
    for (int i = 0; i < s_fpv_n; i++)
        if (s_fpv[i].hash == hash && s_fpv[i].exp32 == exp32 && s_fpv[i].cube == cube &&
            s_fpv[i].d2c == d2c) return 1;
    return 0;
}
static void fpv_add(u32 hash, u32 exp32, u32 cube, u32 d2c)
{
    if (s_fpv_n == s_fpv_cap) {
        const int cap = s_fpv_cap ? s_fpv_cap * 2 : 256;
        FPVariant* g = (FPVariant*)realloc(s_fpv, sizeof(FPVariant) * (size_t)cap);
        if (!g) return;
        s_fpv = g; s_fpv_cap = cap;
    }
    s_fpv[s_fpv_n].hash = hash; s_fpv[s_fpv_n].exp32 = exp32;
    s_fpv[s_fpv_n].cube = cube; s_fpv[s_fpv_n].d2c = d2c; s_fpv_n++;
}
static void fpv_load(void)
{
    if (s_fpv_loaded) return;
    s_fpv_loaded = 1;
    if (!shader_cache_dir()[0]) return;
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s\\fp_variants.txt", shader_cache_dir());
    FILE* f = fopen(path, "r");
    if (!f) return;
    unsigned h, e, c, d;
    while (fscanf(f, "%x %x %x %x", &h, &e, &c, &d) == 4)
        if (!fpv_find(h, e, c, d)) fpv_add(h, e, c, d);
    fclose(f);
}
static void fpv_note(const u8* ucode, u32 exp32, u32 cube, u32 d2c)
{
    fpv_load();
    const u32 hash = rsx_fp_code_hash(ucode, RSX_FP_MAX_BYTES);
    if (!shader_cache_dir()[0] || fpv_find(hash, exp32, cube, d2c)) return;
    fpv_add(hash, exp32, cube, d2c);
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s\\fp_variants.txt", shader_cache_dir());
    FILE* f = fopen(path, "a");
    if (f) { fprintf(f, "%08x %x %x %x\n", hash, exp32, cube, d2c); fclose(f); }
}

static int fp_build_hlsl(const u8* ucode, int exp32, u32 cube_mask, u32 d2c_mask,
                         char* hlsl, size_t cap, u32 fp_addr, int note)
{
    /* Only the units the program samples: the masks come from whatever the
     * draw has bound, and a cube or depth texture left on an unused unit
     * would otherwise change the text (and the cache entry) for nothing. */
    { const u32 used = rsx_fp_texture_units(ucode, RSX_FP_MAX_BYTES);
      cube_mask &= used | (used << 16); d2c_mask &= used; }
    if (note) fpv_note(ucode, exp32 ? 1u : 0u, cube_mask, d2c_mask);
    u32 kcount = 0;
    int n = rsx_fp_decompile_buffered_ex2(ucode, RSX_FP_MAX_BYTES, exp32 ? 0x40u : 0u, cube_mask,
                                          d2c_mask, hlsl, (u32)cap, &kcount);
    int buffered = n > 0 && kcount <= FPCB_KMAX;
    if (n > 0 && !buffered) {
        /* More constants than b1 holds: bake them, as before. */
        static int said = 0;
        if (said++ < 4)
            fprintf(stderr, "[FP] fp=0x%X has %u inline constants (b1 holds %d); "
                            "compiled with them baked in%c", fp_addr, kcount, FPCB_KMAX, 10);
        n = rsx_fp_decompile_ex2(ucode, RSX_FP_MAX_BYTES, exp32 ? 0x40u : 0u, cube_mask,
                                 d2c_mask, hlsl, (u32)cap);
    }
    if (n <= 0) { static int _e=0; if(_e++<16) printf("[FP] decompile fail (fp=0x%08X)\n", fp_addr); return n; }
    if (!fp_bind_b1(hlsl, cap, cube_mask, buffered)) {
        static int _e=0; if(_e++<16) printf("[FP] b1 binding failed (fp=0x%08X)\n", fp_addr);
        return 0;
    }
    return n;
}

static void fp_seen_note(u32 ea, u32 len);

/* rsx_fp_code_hash of the program at guest offset `off`, and its size. The
 * PSO lookup needs it for every draw, and hashing (a byte-serial FNV) plus
 * sizing the program was ~12% of the render thread at Boletarian Palace's
 * 6k draws. A program is re-hashed only when its bytes differ from the copy
 * kept with the last hash: equal bytes up to the old size mean the same END
 * instruction, so the same size and the same hash. RSX_FP_HASH_MEMO=0 hashes
 * every time. */
typedef struct { u32 off, size, hash, units; u8* bytes; } FpHashMemo;
static FpHashMemo s_fph[1024];
static u32 fp_hash_at(u32 off, u32* size_out)
{
    extern uint8_t* vm_base;
    const u8* up = vm_base + off;
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("RSX_FP_HASH_MEMO", 1));
    FpHashMemo* m = &s_fph[((off >> 4) * 2654435761u) >> 22];
    if (en && m->bytes && m->off == off && !memcmp(m->bytes, up, m->size)) {
        *size_out = m->size;
        return m->hash;
    }
    u32 usz = rsx_fp_program_size(up, RSX_FP_MAX_BYTES);
    if (usz == 0) usz = 64;
    const u32 h = rsx_fp_code_hash(up, RSX_FP_MAX_BYTES);
    fp_seen_note(off, usz);
    if (en) {
        u8* b = (m->bytes && m->size >= usz) ? m->bytes : (u8*)realloc(m->bytes, usz);
        if (b) { memcpy(b, up, usz); m->bytes = b; m->off = off; m->size = usz; m->hash = h;
                 m->units = rsx_fp_texture_units(up, RSX_FP_MAX_BYTES); }
    }
    *size_out = usz;
    return h;
}
/* The texture units the program at guest offset `off` samples, from
 * fp_hash_at's memo: render_frame binds only those. Every draw looked up all
 * sixteen units' textures (and render targets, and depth surfaces), ~14 a
 * draw at the Palace where its programs sample two or three, and that was
 * most of the frame's SRV time. */
static u32 fp_units_at(u32 off)
{
    extern uint8_t* vm_base;
    u32 usz;
    (void)fp_hash_at(off, &usz);
    const FpHashMemo* m = &s_fph[((off >> 4) * 2654435761u) >> 22];
    if (m->bytes && m->off == off && !memcmp(m->bytes, vm_base + off, m->size))
        return m->units;
    return rsx_fp_texture_units(vm_base + off, RSX_FP_MAX_BYTES);
}
static ID3D12PipelineState* vp_get_fp_pso(int vs_idx, u32 fp_addr, u32 blend, int nrt,
                                          DXGI_FORMAT rtfmt, int exp32, u32 cmask, u32 cull,
                                          u32 depth, u32 cube_mask, u32 d2c_mask)
{
    vp_pso_knobs_check();
    if (nrt < 0) nrt = 0; if (nrt > 4) nrt = 4;
    if (rtfmt == 0) rtfmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);
    if (!fp_addr || !vm_base) return NULL;
    { static int _off=-1; if(_off<0)_off=getenv("FP_OFF")?1:0; if(_off) return NULL; }
    /* Resolve the VS: the draw's cached per-VP blob, else the primary. */
    ID3DBlob* vsb = NULL; u32 vs_hash = 0;
    if (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) {
        vsb = s_d3d.vp_vs[vs_idx].vs; vs_hash = s_d3d.vp_vs[vs_idx].hash;
    }
    if (!vsb) { vsb = s_d3d.vp_vs_blob; vs_idx = -1; }
    if (!vsb) return NULL;

    /* SET_SHADER_PROGRAM low bits = location+1 (same as textures): 1 = LOCAL
     * (VRAM), 2 = MAIN (gcm/cube emits 0x00B90001 for its VRAM-resident FP). */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    u32 off = cellGcmResolveLocated((fp_addr & 0x3u) == 1, fp_addr & ~0x3u);
    if (off == 0xFFFFFFFFu) return NULL;

    /* Content hash: inline constants are patched in place per frame (wave's
     * stamp), so identity is the BYTES, not the address. */
    /* Hash the CODE, not the constants: with fp_k[] hoisted into the per-draw
     * constant buffer the compiled shader is invariant under constant changes,
     * so hashing them made the pipeline key move every draw. */
    u32 uhash;
    {
        static int cb = -1;
        PS3_KNOB(cb, ps3_env_int("FP_CONSTBUF", 1));
        u32 usz;
        uhash = fp_hash_at(off, &usz);
        s_perf_pso_hashbytes += (int)usz;
        if (!cb) {
            uhash = 2166136261u;
            const u8* up = vm_base + off;
            for (u32 i = 0; i < usz; i++) { uhash ^= up[i]; uhash *= 16777619u; }
        }
    }

    s_perf_pso_calls++;
    VPFPEntry key;
    memset(&key, 0, sizeof key);
    key.fp_addr = fp_addr; key.vs_idx = vs_idx; key.vs_hash = vs_hash;
    key.gen = s_d3d.vp_gen; key.blend = blend; key.nrt = nrt;
    key.rtfmt = (u32)rtfmt; key.exp32 = exp32; key.ucode_hash = uhash;
    key.cmask = cmask; key.cull = cull; key.depth = depth; key.cube_mask = cube_mask;
    key.d2c_mask = d2c_mask;
    key.pso = NULL;
    { const VPFPEntry* hit = vp_fp_find(&key); if (hit) return hit->pso; }
    s_perf_pso_miss++;      /* falls through to a full decompile + D3DCompile */
    /* PSOMISSDBG=1: on a miss, name the key field that differs from an existing
     * entry for the same program. "the cache misses" is not actionable; "it
     * misses on ucode_hash" is. */
    /* HASHDBG=1: how many DISTINCT ucode hashes each program produces. Inline
     * constants are patched into the shader bytes, so a program whose constants
     * animate yields a new hash -- and a new pipeline -- every time. */
    { static int hd = -1;
      PS3_KNOB(hd, ps3_env_int("HASHDBG", 0));
      if (hd) { static u32 fps[32]; static u32 cnt[32]; static int nf = 0;
        int idx = -1;
        for (int i = 0; i < nf; i++) if (fps[i] == fp_addr) idx = i;
        if (idx < 0 && nf < 32) { idx = nf++; fps[idx] = fp_addr; cnt[idx] = 0; }
        if (idx >= 0) { cnt[idx]++;
          if ((cnt[idx] % 200) == 0)
            fprintf(stderr, "[HASHDBG] fp=0x%X has produced %u distinct compiles%c",
                    fp_addr, cnt[idx], 10); } } }
    { static int md = -1;
      PS3_KNOB(md, ps3_env_int("PSOMISSDBG", 0));
      if (md) { static int n = 0;
        for (int i = 0; i < s_d3d.vp_fp_n && n < 12; i++) {
            VPFPEntry* q = &s_d3d.vp_fp[i];
            if (q->fp_addr != fp_addr) continue;
            n++;
            fprintf(stderr, "[PSOMISS] fp=0x%X differs:%s%s%s%s%s%s%s%s%s%c", fp_addr,
                    q->vs_idx != vs_idx        ? " vs_idx"     : "",
                    q->vs_hash != vs_hash      ? " vs_hash"    : "",
                    q->gen != s_d3d.vp_gen     ? " gen"        : "",
                    q->blend != blend          ? " blend"      : "",
                    q->nrt != nrt              ? " nrt"        : "",
                    q->rtfmt != (u32)rtfmt     ? " rtfmt"      : "",
                    q->ucode_hash != uhash     ? " ucode_hash" : "",
                    q->cube_mask != cube_mask  ? " cube_mask"  : "",
                    q->d2c_mask != d2c_mask    ? " d2c_mask"   : "", 10);
            break;
        } } }
    /* CUBEKEYDBG=1: which (program, cube mask) pairs are being compiled. If one
     * program shows several masks, the mask is unstable and is the reason the
     * pipeline cache thrashes with cube support on. */
    { static int ck = -1;
      PS3_KNOB(ck, ps3_env_int("CUBEKEYDBG", 0));
      if (ck) { static u32 seen[64][2]; static int ns = 0; int known = 0;
        for (int i = 0; i < ns; i++)
            if (seen[i][0] == fp_addr && seen[i][1] == cube_mask) known = 1;
        if (!known && ns < 64) { seen[ns][0] = fp_addr; seen[ns][1] = cube_mask; ns++;
            fprintf(stderr, "[CUBEKEY] fp=0x%X cube_mask=0x%X (distinct pairs=%d)%c",
                    fp_addr, cube_mask, ns, 10); } } }
    static char hlsl[RSX_FP_HLSL_BUFFER_SIZE];
    /* The third parameter is SET_SHADER_CONTROL. Only its 32_BITS_EXPORTS bit
     * matters to the decompiler, and the draw records it (exp32, also part of
     * the cache key). It decides whether the colour comes from r0 or h0; the
     * old inference ("r0 was written, so r0") picked a stale temp for programs
     * that use r0 as scratch and finish in h0 (Demon's Souls's level shaders).
     *
     * Buffered: inline constants become fp_constants[i] reads from b1, which
     * vp_record_cb refills from the guest ucode for every draw. The cache key
     * hashes the code without its constants, so a program compiled with its
     * constants baked in kept the values of its first draw for the rest of the
     * run while the title re-patched them per material (Demon's Souls's level
     * surfaces drew black or saturated). The cube mask goes to the decompiler
     * so cube units are declared and sampled as cubes. */
    int n = fp_build_hlsl(vm_base + off, exp32, cube_mask, d2c_mask, hlsl, sizeof hlsl, fp_addr, 1);
    if (n <= 0) return vp_fp_insert(&key);
    /* FP_LIST=1: every program the title actually compiles, with its size. A
     * fragment program that hangs the GPU shows up here as an implausible
     * instruction count long before it shows up as a TDR. */
    { static int fl = -1; PS3_KNOB(fl, getenv("FP_LIST") ? 1 : 0);
      if (fl) fprintf(stderr, "[FPLIST] fp=0x%08X instrs=%d hlsl=%u bytes%c",
                      fp_addr, n, (unsigned)strlen(hlsl), 10); }
    if (getenv("FP_DUMP")) { static int _d=0; if (_d++ < 4) {
        FILE* f = fopen("fp_dump.hlsl", _d==1 ? "w" : "a");
        if (f) {
            const u8* uc = vm_base + off;
            fprintf(f, "/* fp_addr=0x%08X vmoff=0x%08X raw:", fp_addr, off);
            for (int _b = 0; _b < 32; _b++) fprintf(f, " %02X", uc[_b]);
            fprintf(f, " */\n%s\n", hlsl); fclose(f);
        } } }

    /* FP_PICK=<hex fp_addr>: write just that program, whatever order it compiles
     * in. fp_dump.hlsl keeps only the first four, same trap as vp2_dump.hlsl. */
    { static const char* fpk = (const char*)1; static u32 wantf = 0; static int done = 0;
      if (fpk == (const char*)1) { fpk = getenv("FP_PICK");
                                   wantf = fpk ? (u32)strtoul(fpk, NULL, 16) : 0; }
      if (wantf && !done && fp_addr == wantf) { done = 1;
          FILE* f = fopen("duck_fp.hlsl", "w");
          if (f) { fprintf(f, "/* fp_addr=0x%08X, %d instrs */%s", fp_addr, n, hlsl);
                   fclose(f); }
          fprintf(stderr, "[DUCKFP] wrote duck_fp.hlsl (fp=0x%08X, %d instrs)%c",
                  fp_addr, n, 10);
      } }

    /* Debug FP_ONE=<hex fp_addr>: force that program's colour output to
     * all-ones (e.g. wave's colour-detect -> full mask -> island borders
     * everywhere -> the water sim must visibly radiate if it works). */
    { const char* f1 = getenv("FP_ONE");
      if (f1 && (u32)strtoul(f1, NULL, 16) == fp_addr) {
          /* Match the assignment, not one exact spelling of it: the final
           * register is r[N] for some programs and h[N] for others, and the old
           * fixed "= r[0];" anchor silently did nothing for every h[] shader --
           * so this switch appeared to have no effect and was useless for
           * identifying which program draws what. Rewrite the expression with a
           * proper length-changing splice. */
          fp_set_out(hlsl, sizeof hlsl, "(1.0).xxxx");
      } }
    /* FP_TEX=1: make every program output its unit-0 texture sample verbatim.
     * Separates "the texture is wrong" from "the shading tints it" in one run,
     * without having to identify which program draws which surface first. */
    /* FP_SHOW=<hlsl expr>: replace a program's colour output with an arbitrary
     * expression (e.g. FP_SHOW=h[1]) so an intermediate register can be looked
     * at directly. Restrict with FP_TEX_FP=<hex>. Reading a 69-instruction
     * shader to guess which term carries a bad value is slower and less certain
     * than displaying the terms. */
    /* FP_KILL=<hex fp_addr>: make that program discard every fragment, so what
     * it covers becomes visible. Distinguishes "this surface shades black" from
     * "this surface is correct and something else is missing behind it". */
    { const char* kf = getenv("FP_KILL");
      if (kf && (u32)strtoul(kf, NULL, 16) == fp_addr) {
          char* semi; char* rp = strstr(hlsl, "PSOut _po;");
          if (!rp) rp = strstr(hlsl, "PSOutput _out;");
          if (!rp) rp = fp_out_anchor(hlsl, &semi, 1);
          if (rp) {
              const char* ins = "discard; ";
              size_t nl = strlen(ins), tail = strlen(rp) + 1;
              if ((size_t)(rp - hlsl) + nl + tail < sizeof(hlsl)) {
                  memmove(rp + nl, rp, tail);
                  memcpy(rp, ins, nl);
                  fprintf(stderr, "[FPKILL] fp=0x%X discards%c", fp_addr, 10);
              }
          }
      } }
    { const char* sh = getenv("FP_SHOW");
      const char* only = getenv("FP_TEX_FP");
      if (sh && (!only || (u32)strtoul(only, NULL, 16) == fp_addr)) {
          char rep[256];
          snprintf(rep, sizeof rep, "float4((%s).xyz, 1.0)", sh);
          if (fp_set_out(hlsl, sizeof hlsl, rep))
              fprintf(stderr, "[FPSHOW] fp=0x%X output replaced with %s%c",
                      fp_addr, sh, 10);
      } }
    /* Cube units are declared and sampled as cubes by the decompiler itself
     * (it is given the mask). */
    if (cube_mask) {
        { static int _n = 0; if (_n++ < 4)
            fprintf(stderr, "[CUBE] fp=0x%X compiled with cube units mask 0x%X%c",
                    fp_addr, cube_mask, 10); }
    }
    /* FP_IDCOLOR=1: give every fragment program a distinct flat colour derived
     * from its address. One frame then shows which program paints which surface,
     * instead of one run per candidate to test them by elimination.
     * FP_IDCOLOR=h colours only half-precision-export programs: Demon's Souls
     * draws its scene with those and its full-screen post passes with 32-bit
     * exports, so the scene's colours survive the final composite. */
    if (getenv("FP_IDCOLOR") && (strcmp(getenv("FP_IDCOLOR"), "h") || !exp32)) {
        /* Mix properly: every fp address in a title tends to share low
         * bits (they all end 0x01/0x81 here), so a single multiply left
         * one channel constant across all programs and made distinct
         * shaders look like the same colour. */
        u32 hsh = fp_addr * 2654435761u;
        hsh ^= hsh >> 13; hsh *= 0x5BD1E995u; hsh ^= hsh >> 15;
        /* Force the top bit on in each channel: a hash that happens to
         * land near black is indistinguishable from "this surface was
         * never painted", which is the exact question the mode exists
         * to answer. */
        float cr = (float)(((hsh >> 16) & 0x7F) | 0x80) / 255.0f;
        float cg = (float)(((hsh >>  8) & 0x7F) | 0x80) / 255.0f;
        float cb = (float)(((hsh      ) & 0x7F) | 0x80) / 255.0f;
        char rep[128];
        snprintf(rep, sizeof rep, "float4(%.3f,%.3f,%.3f,1.0)", cr, cg, cb);
        if (fp_set_out(hlsl, sizeof hlsl, rep))
            fprintf(stderr, "[FPID] fp=0x%X -> rgb(%.0f,%.0f,%.0f)%c",
                    fp_addr, cr*255, cg*255, cb*255, 10);
    }
    /* FP_TEX=<unit>: output that unit's sample verbatim. FP_TEX_FP=<hex> limits
     * it to one program, so one surface can be inspected without repainting the
     * whole scene. */
    if (getenv("FP_TEX") && (!getenv("FP_TEX_FP") ||
        (u32)strtoul(getenv("FP_TEX_FP"), NULL, 16) == fp_addr)) {
        int _un = atoi(getenv("FP_TEX"));
        if (_un < 0 || _un >= RSX_FP_UNITS) _un = 0;
        /* The bank is an array unless a cube unit made the decompiler declare
         * each unit by name. */
        const int arr = strstr(hlsl, "rsx_tex[16]") != NULL;
        char tex[32];
        const int sarr = strstr(hlsl, "rsx_samp[16]") != NULL;
        if (arr) snprintf(tex, sizeof tex, "rsx_tex[%d].Sample(rsx_samp[%d]", _un, _un);
        else if (sarr) snprintf(tex, sizeof tex, "rsx_tex%d.Sample(rsx_samp[%d]", _un, _un);
        else     snprintf(tex, sizeof tex, "rsx_tex%d.Sample(rsx_samp%d", _un, _un);
        char rep[160];
        /* FP_TEX_UV=1: sample at a fixed centre UV instead of the
         * interpolated texcoord. If the texture is populated but the
         * texcoords never arrive, the normal path samples texel (0,0)
         * -- usually black -- and looks exactly like an empty texture.
         * This tells the two apart. */
        snprintf(rep, sizeof rep, "%s, %s)", tex,
                 getenv("FP_TEX_UV") ? "float2(0.5, 0.5)" : "input.tc0.xy");
        if (fp_set_out(hlsl, sizeof hlsl, rep))
            fprintf(stderr, "[FP_TEX] program 0x%X rewritten to sample unit %d%c",
                    fp_addr, _un, 10);
    }
    /* FP_FORCE=1: replace the translated body with solid magenta -- isolates
     * geometry/transform problems from texture/blend problems. */
    if (getenv("FP_FORCE")) {
        char* rp = strstr(hlsl, "return r[0];");
        if (rp) memcpy(rp, "return float4(1,0,1,1)", 23);
    }
    /* FP_SUB=<hex fp_addr>:<file>: compile that file instead of the translated
     * program. The decompiler's output shape changes between versions, so the
     * string-anchored rewrites above (FP_ONE/FP_TEX) silently miss; a whole
     * replacement always applies. The file must keep the PSInput/rsx_tex[]
     * declarations of the translated program (start from FP_PICK's dump). */
    { const char* fs = getenv("FP_SUB");
      if (fs && (u32)strtoul(fs, NULL, 16) == fp_addr && strchr(fs, ':')) {
          FILE* f = fopen(strchr(fs, ':') + 1, "rb");
          if (f) {
              size_t n2 = fread(hlsl, 1, sizeof(hlsl) - 1, f); hlsl[n2] = 0; fclose(f);
              fprintf(stderr, "[FP_SUB] fp=0x%X <- %s (%u bytes)%c", fp_addr,
                      strchr(fs, ':') + 1, (unsigned)n2, 10);
          }
      } }
    ID3DBlob* pb = NULL; ID3DBlob* e = NULL;
    HRESULT hr = shader_compile_cached(hlsl, strlen(hlsl), "guest_fp", "ps_5_0", &pb, &e);
    if (FAILED(hr) || !pb) {
        static int _e2=0; if (_e2++<16)
            printf("[FP] PS compile FAIL (fp=0x%08X): %s\n", fp_addr,
                   e ? (const char*)e->lpVtbl->GetBufferPointer(e) : "?");
        if (e) e->lpVtbl->Release(e);
        return vp_fp_insert(&key);
    }
    if (e) e->lpVtbl->Release(e);

    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int _i = 0; _i < 16; _i++) {
        il[_i].SemanticName = "ATTR"; il[_i].SemanticIndex = _i;
        il[_i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; il[_i].InputSlot = 0;
        il[_i].AlignedByteOffset = (UINT)(_i * 16);
        il[_i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_i].InstanceDataStepRate = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;   /* zeroed whole: pso_desc_key hashes its bytes */
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb);
    pd.VS.BytecodeLength  = vsb->lpVtbl->GetBufferSize(vsb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb);
    pd.PS.BytecodeLength  = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    if (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n && s_d3d.vp_vs[vs_idx].fetch)
        pd.InputLayout.NumElements = 0;   /* SV_VertexID only */
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    /* Guest face culling. CULL_OFF=1 forces double-sided, the old
     * unconditional behaviour -- geometry that vanishes under culling is a
     * winding/front-face problem, not a missing draw. */
    { static int _co = -1; PS3_KNOB(_co, getenv("CULL_OFF") ? 1 : 0);
      pd.RasterizerState.CullMode = (!_co && (cull & 1u))
          ? ((cull & 2u) ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK)
          : D3D12_CULL_MODE_NONE;
      /* CULL_FLIP=1: invert the front-face convention. Our VP path may hand
       * D3D the opposite winding from the guest's, in which case honouring
       * CULL_FACE culls exactly the faces it should keep. */
      static int _cfl = -1; PS3_KNOB(_cfl, getenv("CULL_FLIP") ? 1 : 0);
      pd.RasterizerState.FrontCounterClockwise =
          (((cull & 4u) ? 1 : 0) ^ _cfl) ? TRUE : FALSE; }
    /* Blend per the guest's packed key (see rsx_blend_key): dbgfont text needs
     * straight alpha; demosaic's effect passes blend OFF; DeferredShading's
     * light accumulation is additive ONE,ONE. */
    pd.BlendState.RenderTarget[0].BlendEnable    = (blend & 1) ? TRUE : FALSE;
    pd.BlendState.RenderTarget[0].SrcBlend       = (blend & 1) ? (D3D12_BLEND)((blend >> 1)  & 0x1F) : D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlend      = (blend & 1) ? (D3D12_BLEND)((blend >> 6)  & 0x1F) : D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOp        = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 21) & 0x7) : D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha  = (blend & 1) ? (D3D12_BLEND)((blend >> 11) & 0x1F) : D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = (blend & 1) ? (D3D12_BLEND)((blend >> 16) & 0x1F) : D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOpAlpha   = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 24) & 0x7) : D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    /* Strip draws (vf_strip_native) cut at 0xFFFFFFFF; list draws never hold it. */
    pd.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
    pd.NumRenderTargets = (UINT)nrt;
    for (int _r = 0; _r < nrt; _r++) {
        pd.RTVFormats[_r] = rtfmt;
        /* Zero-init leaves RenderTargetWriteMask 0 on secondary targets --
         * every MRT-B write would be masked off. Mirror RT0's blend state. */
        pd.BlendState.RenderTarget[_r] = pd.BlendState.RenderTarget[0];
    }
    /* Guest colour write mask (RGBA nibble, already D3D-ordered). cmask is 0xF
     * by default (unset colour_mask register decodes to all-on); a literal 0
     * therefore means the guest EXPLICITLY masked every channel -- a depth-only
     * pass (e.g. DeferredShading's shadow-map generation). Honour it: forcing 0
     * back to 0xF splatters the depth pass's fragment colour onto the target. */
    /* CMASK_FORCE=1: ignore the guest colour mask (write RGBA on every draw).
     * A mis-decoded mask makes geometry rasterize correctly and write nothing,
     * which is indistinguishable from "the draw never happened". */
    { static int _cf = -1; PS3_KNOB(_cf, getenv("CMASK_FORCE") ? 1 : 0);
      if (_cf) cmask = 0xF; }
    for (int _r = 0; _r < nrt; _r++)
        pd.BlendState.RenderTarget[_r].RenderTargetWriteMask = (UINT8)(cmask & 0xF);
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    /* Guest depth state (rsx_depth_key). With the test off RSX neither tests
     * nor writes depth, which is D3D's DepthEnable FALSE as well. */
    pd.DepthStencilState.DepthEnable = (depth & 1u) ? TRUE : FALSE;
    pd.DepthStencilState.DepthWriteMask = (depth & 2u) ? D3D12_DEPTH_WRITE_MASK_ALL
                                                       : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = (D3D12_COMPARISON_FUNC)(((depth >> 2) & 7u) + 1u);
    pd.DepthStencilState.StencilEnable = FALSE;
    /* DEPTH_OFF=1: drop the depth test for guest-FP draws, so submission order
     * alone decides what is on top. Paired with DRAW_LAST_TEX this puts one
     * object in front of everything and answers "is it merely occluded?" --
     * which reordering alone cannot, because a relocated draw still fails the
     * depth test against whatever is already in the buffer. Diagnostic. */
    { static int doff = -1;
      PS3_KNOB(doff, ps3_env_int("DEPTH_OFF", 0));
      if (doff) { pd.DepthStencilState.DepthEnable = FALSE;
                  pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; } }
    pd.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = NULL;
    const double cp_t0 = perf_on() ? perf_now() : 0.0;
    hr = pso_create_cached(&pd, &pso);
    if (perf_on()) { s_perf_cpso += perf_now() - cp_t0; s_perf_cpso_n++; }
    pb->lpVtbl->Release(pb);
    if (FAILED(hr)) {
        static int _e3=0; if (_e3++<16) printf("[FP] PSO FAIL (fp=0x%08X, 0x%08lX)\n", fp_addr, hr);
        return vp_fp_insert(&key);
    }
    { static int _ok=0; if (_ok++<32) printf("[FP] guest FP pipeline ready (fp=0x%08X)\n", fp_addr); }

    key.pso = pso;
    return vp_fp_insert(&key);
}

/* Upload the guest texture for a VP draw into a per-frame texture slot
 * (re-uploaded every frame: gcm/cube's plasma animates in guest memory).
 * Returns the slot index (SRV at heap VP_TEX_SRV_BASE+slot) or -1. Must run
 * while the command list is open, before the draw passes. */
/* NV4097 TEXTURE_CONTROL1 component remap -> D3D12 Shader4ComponentMapping.
 * The crossbar decode itself is RSX semantics and lives in
 * rsx_texture_layout.c; all that is left here is packing the four selectors
 * into D3D12's word. Its force-zero/force-one encodings are 4 and 5, which is
 * what RSX_REMAP_ZERO/ONE already are, so the selectors pass straight through.
 *
 * out[] arrives in the crossbar's field order A,R,G,B; D3D12 wants
 * destR | destG<<3 | destB<<6 | destA<<9 | valid<<12. */
static u32 rsx_remap_to_d3d(u32 c1, u32 basef)
{
    u8 out[4];
    rsx_texture_component_remap(c1, basef, out);
    /* TEX_REMAP_ID=<n>: override the derived crossbar with a fixed mapping, to
     * test channel order directly. The resource holds the guest's A8R8G8B8
     * bytes straight through, so its components are (R=A, G=R, B=G, A=B) and
     * the shader needs destR=1, destG=2, destB=3, destA=0 to see real RGBA.
     *   1 = identity   2 = rotate (the ARGB fix)   3 = BGRA swap */
    { static int fixed = -1;
      PS3_KNOB(fixed, ps3_env_int("TEX_REMAP_ID", 0));
      if (fixed == 1) return 0u | (1u<<3) | (2u<<6) | (3u<<9) | (1u<<12);
      if (fixed == 2) return 1u | (2u<<3) | (3u<<6) | (0u<<9) | (1u<<12);
      if (fixed == 3) return 2u | (1u<<3) | (0u<<6) | (3u<<9) | (1u<<12); }
    /* D3D12 mapping: destR | destG<<3 | destB<<6 | destA<<9 | valid bit */
    { static int _rd = -1; PS3_KNOB(_rd, getenv("REMAPDBG") ? 1 : 0);
      if (_rd) { static u32 seen[32]; static int ns=0;
        u32 k = (basef << 16) | (c1 & 0xFFFFu); int f=0;
        for (int i2=0;i2<ns;i2++) if (seen[i2]==k) f=1;
        if (!f && ns<32) { seen[ns++]=k;
            fprintf(stderr, "[REMAP] basef=0x%02X c1=0x%04X -> destR=%u destG=%u destB=%u destA=%u%c",
                    basef, c1 & 0xFFFFu, out[1], out[2], out[3], out[0], 10); } } }
    return out[1] | (out[2] << 3) | (out[3] << 6) | (out[0] << 9) | (1u << 12);
}

/* Morton/Z-order texel offset for RSX swizzled textures (LN bit clear).
 * Interleaves x (even bit positions) and y (odd) until the smaller dimension's
 * bits run out, then the larger dimension's remaining bits ride above -- the
 * NV40 layout (matches RPCS3 convert_linear_swizzle). POT dims only, which is
 * what the hardware requires for swizzled textures. */

/* Sparse FNV-1a over a texture's source bytes -- enough to notice an animated
 * surface changing, cheap enough to run on every bind. Kept a function rather
 * than a statement-expression macro so the file still compiles under MSVC;
 * ps3recomp builds with clang-cl, but titles like Tokyo Jungle drive the build
 * through the Visual Studio generator. */
static u32 tex_csum(const u8* base, u32 nbytes)
{
    u32 h = 2166136261u;
    u32 step = nbytes > 4096u ? nbytes / 1024u : 4u;
    /* The word read moves through each step (k*28 mod step): texture sizes
     * are powers of two, so a fixed stride always landed on the same bytes
     * of every compressed block -- a 128x128 DXT5 read only its alpha
     * endpoints, a DXT1 only its colour endpoints, never the indices. */
    u32 k = 0;
    for (u32 i = 0; i + 3 < nbytes; i += step, k++) {
        u32 at = step > 4u ? i + ((k * 28u) % step & ~3u) : i;
        if (at + 3 >= nbytes) at = i;
        u32 w32; memcpy(&w32, base + at, sizeof w32);
        h ^= w32; h *= 16777619u;
    }
    return h;
}

/* FNV-1a over every word, for TEX_VERIFY. */
static u32 tex_csum_full(const u8* base, u32 nbytes)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i + 3 < nbytes; i += 4) {
        u32 w32; memcpy(&w32, base + i, sizeof w32);
        h ^= w32; h *= 16777619u;
    }
    return h;
}

/* Does guest memory hold anything at [off, off+nbytes)? Sparse, about 1,000
 * reads spread over the whole range. A surface only the GPU rendered reads as
 * all zero here, because our backend never writes render targets back. */
static int tex_guest_written(u32 off, u32 nbytes)
{
    extern uint8_t* vm_base;
    if (!vm_base || !off) return 0;
    const u32 step = nbytes > 4096u ? (nbytes / 1024u) | 1u : 1u;
    for (u32 i = 0; i < nbytes; i += step)
        if (vm_base[off + i]) return 1;
    return 0;
}

/* The DXGI format of a decoded guest texture (rsx_texture_layout's class). The
 * upload creates the resource with it and the SRV views it with it. */
static DXGI_FORMAT rsx_texfmt_dxgi(rsx_texfmt f)
{
    switch (f) {
    case RSX_TEXFMT_R8G8B8A8:      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case RSX_TEXFMT_R8G8:          return DXGI_FORMAT_R8G8_UNORM;
    case RSX_TEXFMT_BC1:           return DXGI_FORMAT_BC1_UNORM;
    case RSX_TEXFMT_BC2:           return DXGI_FORMAT_BC2_UNORM;
    case RSX_TEXFMT_BC3:           return DXGI_FORMAT_BC3_UNORM;
    case RSX_TEXFMT_R16:           return DXGI_FORMAT_R16_UNORM;
    case RSX_TEXFMT_R16G16:        return DXGI_FORMAT_R16G16_UNORM;
    case RSX_TEXFMT_R16G16F:       return DXGI_FORMAT_R16G16_FLOAT;
    case RSX_TEXFMT_R16G16B16A16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case RSX_TEXFMT_R32F:          return DXGI_FORMAT_R32_FLOAT;
    case RSX_TEXFMT_R32G32B32A32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:                       return DXGI_FORMAT_R8_UNORM;
    }
}

/* Guest texture formats a VP draw samples from guest memory: every format
 * rsx_texture_layout decodes, except the depth formats. Without depth
 * write-back their guest bytes are not the depth the title rendered, and
 * they want a depth-compare sampler besides. Anything else gets a null SRV.
 * This used to be B8, A8R8G8B8, G8B8 and DXT only, so Demon's Souls's
 * D8R8G8B8 environment-light cube maps (units 11-14 of every level shader)
 * sampled 0 and the whole level rendered unlit. */
static int vp_tex_fmt_ok(u32 basef)
{
    switch (basef) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x8B: case 0x8C: case 0x8D: case 0x94: case 0x95: case 0x97:
    case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        return 1;
    default:
        return 0;
    }
}

/* Slot lookup by (bound offset, size, format, cube) through an open-
 * addressed index, rebuilt whenever a slot takes a different texture (new
 * uploads only, which cost far more than the rebuild). */
static short s_vp_tex_index[VP_TEX_HASH];   /* slot + 1, 0 = empty */
static int   s_vp_tex_index_dirty = 1;

static u32 vp_tex_hash(u32 key, u32 w, u32 h, u32 fmt, int cube)
{
    u32 x = key * 0x9E3779B1u ^ (w << 16 | h) * 0x85EBCA77u ^ (fmt << 1 | (u32)cube) * 0xC2B2AE3Du;
    return (x ^ (x >> 15)) & (VP_TEX_HASH - 1);
}

static int vp_tex_find(u32 key, u32 w, u32 h, u32 fmt, int cube)
{
    if (s_vp_tex_index_dirty) {
        memset(s_vp_tex_index, 0, sizeof s_vp_tex_index);
        for (int i = 0; i < VP_TEX_SLOTS; i++) {
            const VPTexSlot* c = &s_d3d.vp_tex[i];
            if (!c->res) continue;
            u32 b = vp_tex_hash(c->key, c->w, c->h, c->fmt, c->cube);
            while (s_vp_tex_index[b]) b = (b + 1) & (VP_TEX_HASH - 1);
            s_vp_tex_index[b] = (short)(i + 1);
        }
        s_vp_tex_index_dirty = 0;
    }
    for (u32 b = vp_tex_hash(key, w, h, fmt, cube); s_vp_tex_index[b]; b = (b + 1) & (VP_TEX_HASH - 1)) {
        const int i = s_vp_tex_index[b] - 1;
        const VPTexSlot* c = &s_d3d.vp_tex[i];
        if (c->res && c->key == key && c->w == w && c->h == h && c->fmt == fmt && c->cube == cube)
            return i;
    }
    return -1;
}

/* A slot not used this frame, round robin, so the textures a frame stops
 * using are the ones evicted, oldest first. -1 when all are in use. */
static int vp_tex_free_slot(void)
{
    static int cursor;
    for (int n = 0; n < VP_TEX_SLOTS; n++) {
        const int i = (cursor + n) % VP_TEX_SLOTS;
        if (!s_d3d.vp_tex[i].used) { cursor = (i + 1) % VP_TEX_SLOTS; return i; }
    }
    return -1;
}

/* Texture staging: one upload buffer, persistently mapped, that every
 * texture upload of a frame decodes into, from the start again each frame
 * (render_frame waits for the GPU before the pre-pass uploads anything).
 *
 * Each texture slot used to own an upload buffer of its own. Creating it
 * was half of a texture's first upload: a camera turn in Demon's Souls'
 * Tutorial that brought 67 new textures into view spent 30 ms creating
 * resources and 6 ms decoding, and the frame missed the game's 36.3 ms
 * limit. The buffers also stayed allocated for as long as their slots.
 *
 * A frame that needs more than the buffer holds puts the rest into one-off
 * buffers (released the next frame), and the next frame starts with the
 * buffer grown to what that frame used, up to TEX_STAGE_MAX_MB (default
 * 512). TEX_STAGE_MB sets the starting size (default 32). */
static void off_rt_defer_release(ID3D12Resource* res);
static ID3D12Resource* s_ts_buf;
static u8*             s_ts_map;
static u64             s_ts_cap, s_ts_off;
static ID3D12Resource* tex_stage_create(u64 bytes, u8** map)
{
    D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes;
    bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = NULL; void* mp = NULL; D3D12_RANGE nr = {0, 0};
    if (FAILED(d3d_create_committed(s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                    &IID_ID3D12Resource, (void**)&r)))
        return NULL;
    if (FAILED(r->lpVtbl->Map(r, 0, &nr, &mp)) || !mp) { r->lpVtbl->Release(r); return NULL; }
    *map = (u8*)mp;
    return r;
}
/* Space for `bytes` of decoded texture, 512-byte aligned (a placed
 * footprint's offset must be). NULL if no buffer could be had. */
static u8* tex_stage_alloc(u64 bytes, ID3D12Resource** res, u64* at)
{
    const u64 a = (s_ts_off + 511u) & ~511ull;
    s_ts_off = a + bytes;               /* counted either way: next frame's size */
    if (s_ts_map && a + bytes <= s_ts_cap) { *res = s_ts_buf; *at = a; return s_ts_map + a; }
    u8* mp = NULL;
    ID3D12Resource* r = tex_stage_create(bytes, &mp);
    if (!r) return NULL;
    off_rt_defer_release(r);            /* read by this frame's copy; gone next frame */
    *res = r; *at = 0;
    return mp;
}
/* Start of a frame's uploads; the GPU is idle. */
static void tex_stage_frame(void)
{
    const u64 used = s_ts_off;
    s_ts_off = 0;
    static u64 limit = 0;
    if (!limit) limit = (u64)ps3_env_int("TEX_STAGE_MAX_MB", 512) << 20;
    u64 cap = s_ts_cap ? s_ts_cap : (u64)ps3_env_int("TEX_STAGE_MB", 32) << 20;
    while (cap < used + used / 4 && cap < limit) cap *= 2;
    if (cap > limit) cap = limit;
    if (s_ts_buf && cap <= s_ts_cap) return;
    u8* mp = NULL;
    ID3D12Resource* r = tex_stage_create(cap, &mp);
    if (!r) {
        static int warned = 0;
        if (!warned++) fprintf(stderr, "[TEXSTAGE] no %llu MB staging buffer; staying at %llu MB%c",
                               (unsigned long long)(cap >> 20), (unsigned long long)(s_ts_cap >> 20), 10);
        limit = s_ts_cap ? s_ts_cap : 1;
        return;
    }
    if (s_ts_buf) {
        fprintf(stderr, "[TEXSTAGE] a frame staged %llu MB of textures: buffer grown to %llu MB%c",
                (unsigned long long)(used >> 20), (unsigned long long)(cap >> 20), 10);
        off_rt_defer_release(s_ts_buf);
    }
    s_ts_buf = r; s_ts_map = mp; s_ts_cap = cap;
}

/* Texture memory: placed resources in 64 MB heaps, in power-of-two blocks
 * from 64 KB to 64 MB with a free list per size, so a slot's texture
 * memory is reused when the slot takes another texture. A committed
 * resource per texture cost ~0.24 ms to create; 67 new textures on one
 * Tutorial camera turn were 16 ms of a 38 ms frame. A texture over 64 MB
 * (or TEX_PLACED=0) is still committed. Heaps are never released. */
#define TEX_POOL_CHUNK   (64ull << 20)
#define TEX_POOL_MIN     (64ull << 10)
#define TEX_POOL_CLASSES 11                  /* 64 KB << 0..10 = 64 MB */
typedef struct { ID3D12Heap* heap; u64 off; } TexBlock;
static TexBlock*   s_tp_free[TEX_POOL_CLASSES];
static int         s_tp_free_n[TEX_POOL_CLASSES], s_tp_free_cap[TEX_POOL_CLASSES];
static ID3D12Heap* s_tp_chunk;
static u64         s_tp_chunk_off;
static void tex_pool_put(int k, ID3D12Heap* heap, u64 off)
{
    if (s_tp_free_n[k] == s_tp_free_cap[k]) {
        const int nc = s_tp_free_cap[k] ? s_tp_free_cap[k] * 2 : 64;
        TexBlock* nb = (TexBlock*)realloc(s_tp_free[k], (size_t)nc * sizeof *nb);
        if (!nb) return;                     /* the block is lost, not reused */
        s_tp_free[k] = nb; s_tp_free_cap[k] = nc;
    }
    s_tp_free[k][s_tp_free_n[k]].heap = heap;
    s_tp_free[k][s_tp_free_n[k]++].off = off;
}
/* A block of class k: a freed one, or carved from the current heap. *reused
 * says the memory held another texture before. */
static int tex_pool_get(int k, ID3D12Heap** heap, u64* off, int* reused)
{
    if (s_tp_free_n[k]) {
        const TexBlock b = s_tp_free[k][--s_tp_free_n[k]];
        *heap = b.heap; *off = b.off; *reused = 1;
        return 1;
    }
    const u64 sz = TEX_POOL_MIN << k;
    if (!s_tp_chunk || s_tp_chunk_off + sz > TEX_POOL_CHUNK) {
        D3D12_HEAP_DESC hd = {0};
        hd.SizeInBytes = TEX_POOL_CHUNK;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;   /* any heap tier */
        ID3D12Heap* nh = NULL;
        if (FAILED(s_d3d.device->lpVtbl->CreateHeap(s_d3d.device, &hd, &IID_ID3D12Heap,
                                                    (void**)&nh)))
            return 0;
        /* The old heap's tail goes to the free lists. Blocks are carved in
         * any order, so align each piece to its own size. */
        if (s_tp_chunk) {
            u64 o = s_tp_chunk_off;
            while (o + TEX_POOL_MIN <= TEX_POOL_CHUNK) {
                int c = 0;
                while (c + 1 < TEX_POOL_CLASSES && !(o & ((TEX_POOL_MIN << (c + 1)) - 1)) &&
                       o + (TEX_POOL_MIN << (c + 1)) <= TEX_POOL_CHUNK) c++;
                tex_pool_put(c, s_tp_chunk, o);
                o += TEX_POOL_MIN << c;
            }
        }
        s_tp_chunk = nh; s_tp_chunk_off = 0;
    }
    /* Align to the block's own size (a 64 KB block may have left the
     * offset off a larger block's boundary); the gap goes to the lists. */
    while (s_tp_chunk_off & (sz - 1)) {
        int c = 0;
        while (TEX_POOL_MIN << (c + 1) <= sz &&
               !(s_tp_chunk_off & ((TEX_POOL_MIN << (c + 1)) - 1))) c++;
        tex_pool_put(c, s_tp_chunk, s_tp_chunk_off);
        s_tp_chunk_off += TEX_POOL_MIN << c;
    }
    if (s_tp_chunk_off + sz > TEX_POOL_CHUNK) return tex_pool_get(k, heap, off, reused);
    *heap = s_tp_chunk; *off = s_tp_chunk_off; *reused = 0;
    s_tp_chunk_off += sz;
    return 1;
}
/* The slot's texture resource, in COPY_DEST. */
static HRESULT tex_res_create(VPTexSlot* t, const D3D12_RESOURCE_DESC* td)
{
    static int placed = -1;
    PS3_KNOB(placed, ps3_env_int("TEX_PLACED", 1));
    t->blk_heap = NULL; t->blk_class = -1;
    if (placed) {
        D3D12_RESOURCE_ALLOCATION_INFO ai;
        s_d3d.device->lpVtbl->GetResourceAllocationInfo(s_d3d.device, &ai, 0, 1, td);
        int k = 0;
        while (k < TEX_POOL_CLASSES && (TEX_POOL_MIN << k) < ai.SizeInBytes) k++;
        ID3D12Heap* heap; u64 off; int reused;
        if (ai.SizeInBytes != UINT64_MAX && ai.Alignment <= TEX_POOL_MIN &&
            k < TEX_POOL_CLASSES && tex_pool_get(k, &heap, &off, &reused)) {
            s_res_gen++;
            if (SUCCEEDED(s_d3d.device->lpVtbl->CreatePlacedResource(
                    s_d3d.device, heap, off, td, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                    &IID_ID3D12Resource, (void**)&t->res))) {
                t->blk_heap = heap; t->blk_off = off; t->blk_class = k;
                if (reused) {   /* the memory held another texture */
                    D3D12_RESOURCE_BARRIER b = {0};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                    b.Aliasing.pResourceAfter = t->res;
                    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
                }
                return S_OK;
            }
            tex_pool_put(k, heap, off);
        }
    }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    return d3d_create_committed(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, td,
                                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                &IID_ID3D12Resource, (void**)&t->res);
}
/* Release the slot's texture; a placed one's block goes back to its list.
 * The GPU is idle (render_frame waits before the pre-pass), and a slot
 * that is replaced was not used this frame. */
static void tex_res_release(VPTexSlot* t)
{
    if (!t->res) return;
    t->res->lpVtbl->Release(t->res); t->res = NULL;
    if (t->blk_heap) tex_pool_put(t->blk_class, t->blk_heap, t->blk_off);
    t->blk_heap = NULL; t->blk_class = -1;
}

static int vp_upload_tex_slot(u32 off, u32 w, u32 h, u32 fmt, int cube, u32 mips)
{
    /* TEX_BUDGET=<n>: cap texture uploads per frame. Off by default -- it was
     * tried against the TDR and does not help, because PERF shows the upload
     * path costing 0.01s for 775 calls. Kept because it is the right lever if a
     * title ever does hit an upload-bound frame. */
    { static int budget = -1;
      PS3_KNOB(budget, ps3_env_int("TEX_BUDGET", 0));
      if (budget > 0) {
          static u64 last_frame = ~0ull; static int used = 0;
          if (last_frame != (u64)s_d3d.frame_count) { last_frame = (u64)s_d3d.frame_count; used = 0; }
          if (++used > budget) return -1;   /* try again next frame */
      } }
    extern uint8_t* vm_base;

    if (!off || !w || !h || !vm_base || !s_d3d.srv_heap) return -1;
    /* Format classes (base = fmt & 0x9F). The LBP loading screen uses:
     * 0x85 A8R8G8B8 (swizzled UI art), 0x8B G8B8 (the 1024x2048 linear FONT
     * atlas -- without it no text renders at all), 0x86/87/88 DXT1/23/45
     * (512x512 detail/LUT layers bound at t1/t3 on every draw). */
    /* Format class, row sizes and swizzling are RSX semantics: rsx_texture_
     * layout.c owns them so the Metal backend can read a guest texture without
     * re-deriving any of it. Only the DXGI mapping below is ours. */
    rsx_tex_layout tl;
    rsx_texture_layout(fmt, w, h, &tl);
    u32 basef = fmt & 0x9F;              /* still needed for the SRV remap */
    int argb = (tl.fmt == RSX_TEXFMT_R8G8B8A8);
    int dxt  = tl.compressed;
    /* Compressed rows are counted in blocks, so keep bpp at 1 for the byte
     * arithmetic the diagnostics below do. */
    u32 bpp = tl.compressed ? 1u : tl.bytes_per_texel;
    /* The DECODED image's format, which is not always the guest's: the packed
     * 16-bit colour formats arrive here unpacked to RGBA8, and DEPTH24_D8 as a
     * float. rsx_texture_layout.h says which class each format byte lands in. */
    DXGI_FORMAT dxfmt = rsx_texfmt_dxgi(tl.fmt);
    /* DXT data is stored as linear 4x4 block rows (compressed formats are
     * never Morton-swizzled on RSX). Row of blocks = (w/4)*blocksize. */
    u32 blkrow  = dxt ? tl.row_bytes : 0;
    u32 blkrows = dxt ? tl.rows      : 0;
    const u32 key_off = off;         /* lookup key: the offset as bound */
    /* Sparse checksum of a texture's source bytes -- enough to notice an
     * animated surface changing, cheap enough to run on every bind. */
    /* (portable helper tex_csum() -- see above; a statement-expression macro
     * here was a GCC/Clang extension MSVC rejects.) */
    #define TEX_CSUM(base, nbytes) tex_csum((base), (nbytes))
    int slot = -1, freeslot = -1;
    for (int i = vp_tex_find(key_off, w, h, fmt, cube); i >= 0; i = -1) {
        VPTexSlot* c = &s_d3d.vp_tex[i];
        {
            if (c->used) return i;                /* already bound this frame */
            { static int nocache = -1;            /* TEX_NOCACHE=1 (live): always re-upload */
              PS3_KNOB(nocache, ps3_env_int("TEX_NOCACHE", 0));
              if (nocache) { slot = i; break; } }
            /* Held from an earlier frame: re-upload only if the guest bytes
             * changed. Most of this scene's textures are static, and converting
             * every one of them every frame was ~70% of the frame's CPU time. */
            u32 nb = dxt ? (blkrow * blkrows) : (w * h * bpp);
            u32 cs = TEX_CSUM(vm_base + c->off, nb);
            /* TEX_VERIFY=1 (live): hash every byte of each cache hit as well,
             * and re-upload one the sparse checksum called unchanged. Slow; it
             * tells whether a held texture is ever stale. */
            { static int verify = -1;
              PS3_KNOB(verify, ps3_env_int("TEX_VERIFY", 0));
              if (verify && cs == c->csum) {
                  u32 fs = tex_csum_full(vm_base + c->off, nb);
                  if (!c->fsum) c->fsum = fs;   /* uploaded before TEX_VERIFY went on */
                  if (fs != c->fsum) {
                      static int n = 0;
                      if (n++ < 40) fprintf(stderr, "[TEX] stale: 0x%X %ux%u fmt 0x%02X (%u bytes) "
                                            "changed, sparse checksum did not (frame %llu)%c",
                                            c->off, w, h, fmt, nb,
                                            (unsigned long long)s_d3d.frame_count, 10);
                      slot = i; break;
                  }
              } }
            if (cs == c->csum) { c->used = 1; return i; }
            slot = i; break;                      /* stale: fall through and redo */
        }
    }
    if (slot < 0) freeslot = vp_tex_free_slot();
    /* TEX_OFF_BIAS=1: sample the image one whole level-0 BELOW the bound offset.
     * Measured on the Rubber Ducky demo: at the bind offset every texture reads
     * as all-zero, while [off - w*h*bpp, off) holds the image (3800-4160 of 4298
     * sampled bytes non-zero, for four different textures at three sizes). So
     * the offset the guest programs points PAST level 0 rather than at it. */
    /* TEX_OFF_BIAS: 1 = one level-0 below the bound offset, 2 = one full MIP
     * CHAIN above it. The measured upload-to-bind deltas on the Rubber Ducky
     * demo are 0x2AAB00 for a 2MB level 0 and 0x555580 for a 4MB one -- both
     * exactly the mipmap pyramid total (L0 * 4/3), which is what the guest
     * reserves per texture. */
    { static int bias = -1;
      PS3_KNOB(bias, ps3_env_int("TEX_OFF_BIAS", 0));
      if (bias == 1) { u32 sz = w * h * bpp; if (off > sz) off -= sz; }
      else if (bias >= 2) {
          u32 chain = 0;
          for (u32 mw = w, mh = h; mw && mh; mw >>= 1, mh >>= 1) {
              chain += mw * mh * bpp;
              if (mw == 1 && mh == 1) break;
          }
          /* bias 3: only shift when the bound offset really is empty, so a
           * texture that IS bound correctly keeps its own data. Applying the
           * chain unconditionally fixes the mis-bound majority but breaks any
           * correctly-bound minority, which shows up as objects going black. */
          int shift = 1;
          if (bias >= 3) {
              u32 nz = 0, sz = w * h * bpp;
              for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
              shift = (nz == 0);
          }
          if (shift) off += chain;
      } }
    /* TEX_REMAP=1: if the bound offset holds nothing, look the image up by size
     * in the VRAM upload registry (cellGcmSys). For a title whose upload address
     * and SET_TEXTURE_OFFSET disagree this puts the real bytes under the sampler
     * without guessing a delta. */
    { static int remap = -1;
      PS3_KNOB(remap, ps3_env_int("TEX_REMAP", 0));
      if (remap) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
        if (!nz) {
            extern u32 rsx_find_vram_upload(u32);
            u32 alt = rsx_find_vram_upload(sz);
            if (alt) {
                static int _n = 0;
                if (_n++ < 8) fprintf(stderr, "[TEXREMAP] 0x%08X (%ux%u, %u bytes) -> 0x%08X%c",
                                      off, w, h, sz, alt, 10);
                off = alt;
            }
        } } }
    /* A stale cache entry (slot >= 0) re-uses its own slot; otherwise take a
     * free one. Without this the stale path fell through to freeslot and either
     * duplicated the texture into a second slot or bailed when none was free. */
    if (slot < 0 && freeslot < 0) {               /* out of slots this frame */
        static int warned;
        if (!warned) { warned = 1;
            fprintf(stderr, "[TEX] all %d texture slots in use this frame: later textures "
                            "draw unbound%c", VP_TEX_SLOTS, 10); }
        return -1;
    }
    /* TEX_SRCDBG=<N>: is the guest memory this slot uploads FROM actually
     * populated? An empty source and a broken sampler both render flat. */
    { static int cap = -1, n = 0;
      PS3_KNOB(cap, ps3_env_int("TEX_SRCDBG", 0));
      if (cap && n < cap) { n++;
        u32 nz = 0, tot = 0;
        for (u32 i = 0; i < w * h * bpp && i < 0x40000u; i += 61) { tot++; if (vm_base[off + i]) nz++; }
        /* Several of this title's textures are bound at EXACTLY the end address
         * of an upload, so also sample the block immediately BELOW the bind
         * offset: if that holds the image, the bind is one whole texture high. */
        u32 nzb = 0, totb = 0;
        { u32 sz = w * h * bpp; u32 base = (off > sz) ? off - sz : 0;
          for (u32 i = 0; i < sz && i < 0x40000u; i += 61) { totb++; if (vm_base[base + i]) nzb++; } }
        long nearest = 0; u32 found = 0;
        if (!nz) {   /* empty: where IS the data? scan out from the bind offset */
            for (long d = 0x1000; d <= 0x1000000 && !found; d += 0x1000) {
                for (int s = 0; s < 2 && !found; s++) {
                    long a = (long)off + (s ? -d : d);
                    if (a < 0x1000) continue;
                    u32 c = 0;
                    for (u32 i = 0; i < 0x1000u; i += 61) if (vm_base[a + i]) c++;
                    if (c > 8) { nearest = (s ? -d : d); found = c; }
                }
            }
        }
        fprintf(stderr, "[TEXSRC] off=0x%08X %ux%u fmt=0x%02X bpp=%u nonzero=%u/%u"
                        "%s%+ld (page nonzero=%u)\n",
                off, w, h, fmt, bpp, nz, tot,
                nz ? "" : "  nearest data at ", nz ? 0L : nearest, found);
        fprintf(stderr, "[TEXSRC]    block BELOW bind (off-%u): nonzero=%u/%u%c",
                w * h * bpp, nzb, totb, 10);
        /* Walk DOWN in 4KB steps to the first populated page whose predecessor
         * is empty: that is where this texture's data actually begins, and the
         * delta from the bound offset is the rule we are missing. */
        { u32 step = 0x1000, prev_pop = 0; long start_delta = 0;
          for (long d = 0; d >= -(long)(w * h * bpp) * 2; d -= step) {
              u32 a = (u32)((long)off + d); u32 pop = 0;
              if (a < 0x1000) break;
              for (u32 i = 0; i < step; i += 53) if (vm_base[a + i]) pop++;
              if (!pop && prev_pop) { start_delta = d + step; break; }
              prev_pop = pop;
          }
          fprintf(stderr, "[TEXSRC]    data starts at off%+ld (level0=%u bytes)%c",
                  start_delta, w * h * bpp, 10); } } }
    if (slot < 0) slot = freeslot;
    VPTexSlot* t = &s_d3d.vp_tex[slot];
    /* The upload buffer's rows hold the DECODED image, so they are sized from
     * dst_row_bytes -- w * bpp is the source row, which is narrower for any
     * format rsx_texture_decode unpacks. */
    u32 pitch = (tl.dst_row_bytes + 255) & ~255u;
    int fresh = 0;

    /* Also recreate when the CUBE-ness changes: a slot holding a 2D texture of
     * the same dimensions would otherwise be reused with 1 array slice while the
     * cube path copies 6 subresources and writes a 6x upload buffer -- which
     * crashes. */
    if (t->res && (t->w != w || t->h != h || t->fmt != fmt || t->cube != cube))
        tex_res_release(t);
    if (!t->res) {
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h;
        td.DepthOrArraySize = cube ? 6 : 1;   /* cube = 6 array slices */
        td.MipLevels = 1;
        td.Format = dxfmt; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(tex_res_create(t, &td)))
            return -1;
        fresh = 1;
    }

    ID3D12Resource* up = NULL; u64 up_at = 0;
    void* mapped = tex_stage_alloc((u64)pitch * (dxt ? blkrows : h) * (cube ? 6 : 1), &up, &up_at);
    if (!mapped) {
        /* A fresh resource has never been through the copy below, so it
         * would be left in COPY_DEST: drop it and try again next frame. */
        if (fresh) tex_res_release(t);
        return -1;
    }
    { static int _tp=0; if (getenv("RTT_DUMP") && _tp++ < 6) {
        const u8* sp = vm_base + off;
        fprintf(stderr, "[TEXUP] off=0x%X fmt=0x%X row0:", off, fmt);
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[_b]);
        fprintf(stderr, "  row240:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+_b]);
        fprintf(stderr, "  mid:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+320*bpp+_b]);
        fprintf(stderr, "%s", "\n");
    } }
    /* Swizzled (LN bit 0x20 clear): texels live in Morton/Z-order, not rows.
     * Uploading them as linear rows produced the diagonal-stripe garbage on
     * LBP's loading screen (every texture there is 0x85 swizzled). POT dims
     * only -- the hardware requires that for swizzled textures anyway. */
    int swz = tl.swizzled;
    /* Cube textures store their 6 faces consecutively. Convert each into its own
     * slice of the upload buffer; the conversion below is unchanged and simply
     * runs once per face with off/mapped rebased. */
    const u32 _face_rows  = dxt ? blkrows : h;
    /* The face stride is NOT one mip-0 image: RSX stores each cube face as its
     * own complete mip pyramid, so face f starts a whole pyramid (128-byte
     * aligned) in. Assuming mip-0-sized strides made face 1 land inside face 0's
     * mip chain -- which is exactly what the dumps showed, every face after the
     * first a progressively smaller copy of the first. */
    u32 _face_bytes = dxt ? (blkrow * blkrows) : (w * h * bpp);
    if (cube) {
        u32 total = 0;
        for (u32 lw = w, lh = h, l = 0; l < (mips ? mips : 1u); l++) {
            total += dxt ? (((lw + 3) / 4) * (bpp == 8 ? 16u : 8u) * ((lh + 3) / 4))
                         : (lw * lh * bpp);
            if (lw > 1) lw >>= 1;
            if (lh > 1) lh >>= 1;
        }
        _face_bytes = (total + 127u) & ~127u;
    }
    const u32 _nfaces     = cube ? 6u : 1u;
    const u32 _off0 = off; u8* const _map0 = (u8*)mapped;
    for (u32 _f = 0; _f < _nfaces; _f++) {
    off    = _off0 + _f * _face_bytes;
    if (cube && getenv("CUBEDBG")) { u32 _s=0; const u8* _p = vm_base + off;
        for (u32 _i = 0; _i < _face_bytes; _i += 97) _s = _s*131 + _p[_i];
        fprintf(stderr, "[CUBEFACE] f=%u off=0x%X bytes=%u bpp=%u swz=%d csum=%08X%c",
                _f, off, _face_bytes, bpp, swz, _s, 10); }
    mapped = _map0 + (u64)_f * pitch * _face_rows;
    /* Pixel conversion -- deswizzle, channel order, block copy -- is RSX
     * semantics and lives in rsx_texture_layout.c, so a second backend gets
     * it by calling this rather than porting the loops again. */
    rsx_texture_decode(mapped, pitch, vm_base + off, w, h, &tl,
                       rsx_texture_argb_is_rgba());

    /* The diagnostics below only apply to the linear 8-bit case (Bink video
     * planes); they used to sit inside that branch of the conversion. */
    if (!tl.compressed && tl.bytes_per_texel == 1 && !tl.swizzled) {
        /* MOVIE_FIND=1: when a 640x360 movie plane uploads ZERO, scan a wide
         * guest window for the REAL frame (a 640-wide region with content) so
         * we learn where the video decode actually wrote vs where the texture
         * points -- ends the frame-buffer-address guessing. */
        if (getenv("MOVIE_FIND") && w == 640 && h == 360) {
            extern uint8_t* vm_base;
            const u8* here = vm_base + off;
            u32 hz = 0; for (u32 i=0;i<w*h;i+=137) if (here[i]) { hz=1; break; }
            static int _mf = 0;
            if (!hz && _mf++ < 3) {
                fprintf(stderr, "[movie-find] bound plane ea=0x%X is ZERO; full scan...\n", off);
                /* Scan FULL VRAM + main heap in 0x8000 steps for a 640x360
                 * content block -- NO early cap (the previous found<6 stopped in
                 * the low-VRAM display buffers before ever reaching the movie
                 * region ~0x40E80000). Report the near-plane hits explicitly so
                 * ring-desync (frame present at a DIFFERENT slot) is separable
                 * from decode-never-ran (region entirely zero). */
                struct { u32 lo, hi; const char* tag; } rng[] = {
                    {0x40000000u, 0x41000000u, "VRAM"}, {0x00100000u, 0x10000000u, "MAIN"},
                    {0x48000000u, 0x4A000000u, "BINK"} };  /* Bink working set (handle ~0x4849E760, SPU I/O 0x4847/0x4945) */
                u32 plane_lo = (off > 0x400000u) ? off - 0x400000u : 0;
                u32 plane_hi = off + 0x400000u;
                int found = 0, nearc = 0;
                for (int r = 0; r < (int)(sizeof(rng)/sizeof(rng[0])); r++) {
                    u32 best_a = 0, best_nz = 0; u32 best_span = 0;
                    for (u32 a = rng[r].lo; a + w*h < rng[r].hi; a += 0x8000) {
                        const u8* p = vm_base + a;
                        u32 nz = 0; u8 mn2 = 255, mx2 = 0;
                        for (u32 i = 0; i < w*h; i += 257) { u8 v = p[i]; if (v) nz++; if (v<mn2) mn2=v; if (v>mx2) mx2=v; }
                        u32 span = mx2 - mn2;
                        if (nz > best_nz) { best_nz = nz; best_a = a; best_span = span; }
                        if (nz > 400 && span > 60) {  /* looks like image content */
                            int isnear = (a >= plane_lo && a <= plane_hi);
                            if (isnear) nearc++;
                            if (found < 40)
                                fprintf(stderr, "[movie-find]   CONTENT @0x%08X (%s) nz=%u span=%u%s\n",
                                        a, rng[r].tag, nz, span, isnear ? " <== NEAR bound plane" : "");
                            found++;
                        }
                    }
                    /* Always report the densest block in this range even below the
                     * content threshold -- the MM intro fades in from black, so the
                     * real first frames are low-span and would otherwise be invisible. */
                    fprintf(stderr, "[movie-find]   %s best @0x%08X nz=%u span=%u%s\n",
                            rng[r].tag, best_a, best_nz, best_span,
                            (best_a >= plane_lo && best_a <= plane_hi) ? " <== NEAR bound plane" : "");
                    /* Dump the densest block as a 640x360 grayscale BMP so the
                     * "is this the decoded movie frame" question is answered by
                     * eye. One dump per range per process. */
                    if (best_nz > 400 && best_span > 40) {
                        static int _bd[4] = {0,0,0,0};
                        if (r < 4 && !_bd[r]) { _bd[r] = 1;
                            const u8* sp = vm_base + best_a;
                            char pn[128];
                            snprintf(pn, sizeof(pn), "binkscan_%s_%08X.bmp", rng[r].tag, best_a);
                            FILE* f = fopen(pn, "wb");
                            if (f) {
                                u32 rowb = w*3, rowp = (rowb+3)&~3u, fsz = 54 + rowp*h;
                                u8 hd[54] = {'B','M'};
                                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                                hd[10]=54; hd[14]=40;
                                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                                hd[26]=1; hd[28]=24;
                                fwrite(hd,1,54,f);
                                for (int y=(int)h-1; y>=0; y--) {
                                    const u8* srow = sp + (u64)y*w;
                                    for (u32 x=0;x<w;x++){ u8 px[3]; px[0]=px[1]=px[2]=srow[x]; fwrite(px,1,3,f); }
                                    { u8 z[3]={0,0,0}; fwrite(z,1,rowp-rowb,f); }
                                }
                                fclose(f);
                                fprintf(stderr, "[movie-find]   dumped %s\n", pn);
                            }
                        }
                    }
                }
                fprintf(stderr, "[movie-find]   total=%d near-plane=%d (window 0x%X..0x%X)\n",
                        found, nearc, plane_lo, plane_hi);
                if (!found) fprintf(stderr, "[movie-find]   no 640x360 content block (>thresh) found anywhere\n");
            }
        }
        /* TEX_SAVE=1: also dump wide B8 uploads (Bink video planes are B8 --
         * the Y plane IS the movie frame in grayscale) + a content stat, so
         * "is the decoder producing pixels" is answerable by looking at a BMP. */
        if (getenv("TEX_SAVE") && w >= 256) {
            static int _bs = 0;
            const u8* sp = vm_base + off;
            u32 nz = 0; u8 mn = 255, mx = 0;
            for (u32 i = 0; i < w * h; i += 17) {
                u8 v = sp[i]; if (v) nz++; if (v < mn) mn = v; if (v > mx) mx = v;
            }
            fprintf(stderr, "[TEXB8] off=0x%X %ux%u nz=%u/%u min=%u max=%u\n",
                    off, w, h, nz, (w * h) / 17, mn, mx);
            if (_bs < 8 && mx > mn) { _bs++;
                char pn[128];
                snprintf(pn, sizeof(pn), "texb8_%08X_%ux%u_%d.bmp", off, w, h, _bs);
                FILE* f = fopen(pn, "wb");
                if (f) {
                    u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                    u32 fsz = 54 + rowp * h;
                    u8 hd[54] = {'B','M'};
                    hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                    hd[10]=54; hd[14]=40;
                    hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                    hd[26]=1; hd[28]=24;
                    fwrite(hd, 1, 54, f);
                    for (int y = (int)h - 1; y >= 0; y--) {
                        const u8* srow = sp + (u64)y * w;
                        for (u32 x = 0; x < w; x++) {
                            u8 px[3]; px[0]=px[1]=px[2]=srow[x];
                            fwrite(px, 1, 3, f);
                        }
                        { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                    }
                    fclose(f);
                    fprintf(stderr, "[TEX_SAVE] %s\n", pn);
                }
            }
        }
    }
    /* TEX_SAVE=1: dump the first few converted ARGB uploads as BMPs (rgb +
     * alpha channel separately) -- ground-truth for "is the guest texture
     * wrong or is the sampling wrong" questions (wave's hue palette). */
    if (argb && _f + 1 == _nfaces && getenv("TEX_SAVE")) { static int _ts = 0; if (_ts < 8) { _ts++;
        /* For a cube, write every face: the point of the dump is to check the
         * assumed 6-face layout against the game's cubemap art. */
        for (u32 _face = 0; _face < _nfaces; _face++)
        for (int pass = 0; pass < 2; pass++) {
            char pn[128];
            if (_nfaces > 1)
                snprintf(pn, sizeof(pn), "cube_%08X_%ux%u_f%u_%s.bmp", _off0, w, h,
                         _face, pass ? "a" : "rgb");
            else
                snprintf(pn, sizeof(pn), "tex_%08X_%ux%u_%s.bmp", off, w, h,
                     pass ? "a" : "rgb");
            FILE* f = fopen(pn, "wb");
            if (f) {
                u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                u32 fsz = 54 + rowp * h;
                u8 hd[54] = {'B','M'};
                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                hd[10]=54; hd[14]=40;
                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                hd[26]=1; hd[28]=24;
                fwrite(hd, 1, 54, f);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = _map0 + (u64)_face * pitch * _face_rows
                                     + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        u8 px[3];
                        if (pass) { px[0]=px[1]=px[2]=srow[x*4+3]; }
                        else { px[0]=srow[x*4+2]; px[1]=srow[x*4+1]; px[2]=srow[x*4+0]; }
                        fwrite(px, 1, 3, f);
                    }
                    { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                }
                fclose(f);
                fprintf(stderr, "[TEX_SAVE] %s\n", pn);
            }
        }
    } }
    /* TEX_AVGDBG=1: one line per distinct bound texture offset with its average
     * source colour. duck.tga is overwhelmingly yellow (avg ~243,191,23), so this
     * says whether the duck's image is bound at all -- and if it is, the duck is
     * being drawn somewhere and the question is where, not whether. */
    { static int avgd = -1;
      PS3_KNOB(avgd, ps3_env_int("TEX_AVGDBG", 0));
      if (avgd && !dxt && bpp == 4) {
        static u32 seen[64]; static int nseen = 0;
        int known = 0;
        for (int i2 = 0; i2 < nseen; i2++) if (seen[i2] == off) { known = 1; break; }
        if (!known && nseen < 64) {
            seen[nseen++] = off;
            /* Report the source as R,G,B,A -- the layout TEX_RGBA uploads (PSGL
             * writes GL_RGBA/UNSIGNED_INT_8_8_8_8, so bytes land R,G,B,A). Alpha
             * matters as much as colour here: a blended surface whose alpha is a
             * flat 255 renders opaque no matter how correct the blend state is. */
            u64 sr = 0, sg = 0, sb = 0, sa = 0; u32 cnt = 0, amin = 255, amax = 0;
            for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
                const u8* q = vm_base + off + i2;
                sr += q[0]; sg += q[1]; sb += q[2]; sa += q[3]; cnt++;
                if (q[3] < amin) amin = q[3];
                if (q[3] > amax) amax = q[3];
            }
            if (cnt) fprintf(stderr, "[TEXAVG] raw=0x%08X %4ux%-4u fmt=0x%02X"
                                     " avgRGBA=(%3u,%3u,%3u,%3u) alpha[%u..%u]%c",
                             key_off, w, h, fmt, (u32)(sr/cnt), (u32)(sg/cnt),
                             (u32)(sb/cnt), (u32)(sa/cnt), amin, amax, 10);
        }
      } }
    /* Content-identify the duck's texture so filters can say "duck". */
    if (!dxt && bpp == 4 && w == 512 && h == 512 && s_duck_raw != key_off) {
        u64 ar = 0, ag = 0, ab = 0; u32 n = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 211) {
            const u8* q = vm_base + off + i2;
            ar += q[0]; ag += q[1]; ab += q[2]; n++;
        }
        if (n && ar/n > 200 && ag/n > 140 && ab/n < 90) {
            s_duck_off = key_off;
            { static u32 last = 0; if (last != key_off) { last = key_off;
                fprintf(stderr, "[DUCKTEX] resolved=0x%08X identified by content%c", key_off, 10); } }
        }
    }
    /* TEX_HILITE=1: paint the duck's texture pure red. duck.tga is overwhelmingly
     * yellow (avg 243,191,23); with the R,G,B,A source order that is bytes
     * s[0] high, s[1] mid, s[2] low. Flooding it with an unmistakable colour
     * answers "where on screen is the duck drawn" directly, which neither the
     * vertex data nor a pixel count can. Diagnostic only. */
    { static int hil = -1;
      PS3_KNOB(hil, ps3_env_int("TEX_HILITE", 0));
      if (hil && !dxt && bpp == 4 && w == 512 && h == 512) {
        u64 sr = 0, sg = 0, sb = 0; u32 cnt = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
            const u8* q = vm_base + off + i2;
            sr += q[0]; sg += q[1]; sb += q[2]; cnt++;
        }
        if (cnt) {
            u32 ar = (u32)(sr/cnt), ag = (u32)(sg/cnt), ab = (u32)(sb/cnt);
            if (ar > 200 && ag > 140 && ab < 90) {
                for (u32 y = 0; y < h; y++) {
                    u8* drow = (u8*)mapped + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                        drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                    }
                }
                { static int _n = 0; if (_n++ < 4)
                    fprintf(stderr, "[TEXHILITE] duck texture off=0x%08X avg=(%u,%u,%u) -> red%c",
                            off, ar, ag, ab, 10); }
            }
        }
      } }
    /* TEX_MARKEMPTY=1: paint any texture whose guest source is entirely zero a
     * flat magenta instead of uploading the empty bytes. An unresolved texture
     * and a legitimately black surface are indistinguishable on screen, so this
     * says WHICH geometry is missing its image -- diagnostic only. */
    { static int mark = -1;
      PS3_KNOB(mark, ps3_env_int("TEX_MARKEMPTY", 0));
      if (mark && !dxt) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i2 = 0; i2 < sz && i2 < 0x40000u; i2 += 61) if (vm_base[off + i2]) nz++;
        if (!nz) {
            for (u32 y = 0; y < h; y++) {
                u8* drow = (u8*)mapped + (u64)y * pitch;
                for (u32 x = 0; x < w; x++) {
                    /* Pure RED. Magenta collided with this title's wall tint,
                     * so the marker was indistinguishable from real geometry. */
                    drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                    drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                }
            }
            { static int _n = 0; if (_n++ < 8)
                fprintf(stderr, "[TEXEMPTY] off=0x%08X %ux%u fmt=0x%02X -> magenta%c",
                        off, w, h, fmt, 10); }
        }
      } }
    }   /* end per-face conversion */
    off = _off0; mapped = _map0;

    if (!fresh) {   /* reused resource: PSR -> COPY_DEST first */
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = t->res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = up;     src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format   = dxfmt;
    src.PlacedFootprint.Footprint.Width    = w;
    src.PlacedFootprint.Footprint.Height   = h;
    src.PlacedFootprint.Footprint.Depth    = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    for (u32 _f = 0; _f < _nfaces; _f++) {
        dst.SubresourceIndex        = _f;      /* cube face = array slice */
        src.PlacedFootprint.Offset  = up_at + (u64)_f * pitch * _face_rows;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
    }
    {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    /* SRV at heap slot VP_TEX_SRV_BASE+slot */
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = dxfmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    /* RSX B8 replicates the byte into all four channels (dbgfont's FP reads
     * coverage from .w); DXGI R8 defaults to (r,0,0,1), so swizzle (R,R,R,R)
     * = encoded 0x1000 (component 0 in all lanes + always-set bit). ARGB8
     * keeps the identity mapping. */
    sv.Shader4ComponentMapping = (basef == 0x81) ? 0x1000 : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(VP_TEX_SRV_BASE + slot) * s_d3d.srv_inc;
    if (cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = 1;
        sv.TextureCube.MostDetailedMip = 0;
        sv.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, t->res, &sv, sh);

    if (t->key != key_off || t->w != w || t->h != h || t->fmt != fmt || t->cube != cube)
        s_vp_tex_index_dirty = 1;
    t->off = off; t->key = key_off; t->w = w; t->h = h; t->fmt = fmt;
    t->cube = cube; t->used = 1; t->face_bytes = _face_bytes;
    { u32 nb = dxt ? (blkrow * blkrows) : (w * h * bpp);
      t->csum = TEX_CSUM(vm_base + off, nb);
      t->fsum = ps3_env_int("TEX_VERIFY", 0) ? tex_csum_full(vm_base + off, nb) : 0; }
    #undef TEX_CSUM
    return slot;
}

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
        if (n++ < 200) fprintf(stderr, "[LATE] frame %llu: %u texture binds, %u programs "
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
         * palette drew as an opaque square). */
        static int noat = -1;   /* knobs: this runs for every draw */
        PS3_KNOB(noat, getenv("NO_ALPHATEST") ? 1 : 0);
        if (dr && !noat) {
            { static int _at = 0;
              if (((dr->alpha_ctl >> 16) & 1u) && _at++ < 4)
                  printf("[ALPHATEST] enable func=%u ref=%u\n",
                         (dr->alpha_ctl >> 8) & 0xFFu, dr->alpha_ctl & 0xFFu); }
            float* at = &ts[FPCB_ALPHA * 4];
            at[0] = (float)((dr->alpha_ctl >> 16) & 1u);
            at[1] = (float)(dr->alpha_ctl & 0xFFu) / 255.0f;
            at[2] = (float)((dr->alpha_ctl >> 8) & 0x07u);
            at[3] = 0.0f;
        } else {
            float* at = &ts[FPCB_ALPHA * 4];
            at[0] = 0.0f; at[1] = 0.0f; at[2] = 7.0f; at[3] = 0.0f;
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
    float c00 = c[0];
    static int s_guess = -1;
    PS3_KNOB(s_guess, getenv("VP_GUESSPROJ") ? 1 : 0);
    int garbage = s_guess && !(c00 > 0.0f && c00 < 8.0f);
    static int nofix = -1, fix = -1;
    PS3_KNOB(nofix, getenv("VP_NOFIXPROJ") ? 1 : 0);
    PS3_KNOB(fix, getenv("VP_FIXPROJ") ? 1 : 0);
    if (nofix) garbage = 0;
    if ((garbage && uses_c03) || fix) {
        { static int _fb = 0; if (_fb++ < 6)
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

/* RSX surface colour format (SET_SURFACE_FORMAT bits [4:0]) -> DXGI. Float
 * targets matter: wave's water height maps store SIGNED values (F_W16..),
 * demosaic's differential planes likewise -- RGBA8 clamps them to zero. */
static DXGI_FORMAT rsx_surface_dxgi(u32 fmt)
{
    switch (fmt & 0x1F) {
    case 0x0B: return DXGI_FORMAT_R16G16B16A16_FLOAT; /* F_W16Z16Y16X16 */
    case 0x0C: return DXGI_FORMAT_R32G32B32A32_FLOAT; /* F_W32Z32Y32X32 */
    case 0x0D: return DXGI_FORMAT_R32_FLOAT;          /* F_X32          */
    default:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

/* NV4097 TEXTURE_ADDRESS wrap field (S: bits 0-3, T: 8-11, R: 16-19). */
static D3D12_TEXTURE_ADDRESS_MODE rsx_wrap_to_d3d(u32 m)
{
    switch (m) {
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;        /* CLAMP_TO_EDGE */
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;        /* GL CLAMP */
    case 6: case 7: case 8: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;        /* 1 = WRAP; 0 = never set */
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
static u32 vp_sampler_table(const D3D12DrawRecord* dr)
{
    static u32 keys[VP_SAMP_TABLES][RSX_FP_UNITS * 2];
    static u64 last[VP_SAMP_TABLES];   /* frame_count of the last draw using it */
    u32 k[RSX_FP_UNITS * 2];
    for (int u = 0; u < RSX_FP_UNITS; u++) {
        k[u * 2]     = dr && dr->tex[u].set ? dr->tex[u].addr : (u ? 0x10101u : 0x30303u);
        k[u * 2 + 1] = dr && dr->tex[u].set ? (dr->tex[u].filt & 0x07070000u)
                                            : (u ? 0x02060000u : 0x01010000u);
        if (dr_unit_shadow(dr, u)) k[u * 2 + 1] |= 0x80000000u;   /* comparison */
    }
    const u64 now = s_d3d.frame_count;
    /* RSX_SAMP_TABLES=<n> (2..128): use only n tables, to exercise the reuse
     * below without a two-hour session. */
    static u32 cap = 0;
    if (!cap) { int c = ps3_env_int("RSX_SAMP_TABLES", VP_SAMP_TABLES);
                cap = (u32)(c < 2 ? 2 : c > VP_SAMP_TABLES ? VP_SAMP_TABLES : c); }
    for (u32 i = 0; i < s_d3d.samp_n; i++)
        if (!memcmp(keys[i], k, sizeof k)) { last[i] = now; return i; }
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
          if (n++ < 5) fprintf(stderr, "[D3D12] sampler tables full (%u): reusing table %u, "
                               "last used in frame %llu%c", cap, t,
                               (unsigned long long)last[t], 10); }
    }
    last[t] = now;
    memcpy(keys[t], k, sizeof k);
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.samp_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.samp_heap, &h);
    for (int u = 0; u < RSX_FP_UNITS; u++) {
        const u32 a = k[u * 2], f = k[u * 2 + 1];
        const u32 minf = (f >> 16) & 7, magf = (f >> 24) & 7;
        /* min: 1 NEAREST, 2 LINEAR, 3 N_MIP_N, 4 L_MIP_N, 5 N_MIP_L, 6 L_MIP_L,
         * 7 convolution; mag: 1 NEAREST, 2 LINEAR, 4 convolution. */
        const int min_lin = (minf == 2 || minf == 4 || minf == 6 || minf == 7);
        const int mip_lin = (minf == 5 || minf == 6);
        const int mag_lin = (magf == 2 || magf == 4);
        D3D12_SAMPLER_DESC sd = {0};
        sd.Filter = D3D12_ENCODE_BASIC_FILTER(
            min_lin ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            mag_lin ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            mip_lin ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            (f & 0x80000000u) ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON
                              : D3D12_FILTER_REDUCTION_TYPE_STANDARD);
        sd.AddressU = rsx_wrap_to_d3d(a & 0xF);
        sd.AddressV = rsx_wrap_to_d3d((a >> 8) & 0xF);
        sd.AddressW = rsx_wrap_to_d3d((a >> 16) & 0xF);
        sd.MaxAnisotropy = 1;
        /* zfunc uses the GL order (D3D12_COMPARISON_FUNC minus one), but the
         * stored function is the reverse of the sampling compare: LESS means
         * "texel < reference" (RPCS3 swaps LESS/GREATER and LEQUAL/GEQUAL the
         * same way in its GL sampler). */
        static const u8 zrev[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
        sd.ComparisonFunc = (f & 0x80000000u) ? (D3D12_COMPARISON_FUNC)(zrev[(a >> 28) & 7u] + 1u)
                                              : D3D12_COMPARISON_FUNC_NEVER;
        sd.MinLOD = 0.0f;
        sd.MaxLOD = (minf == 1 || minf == 2) ? 0.0f : D3D12_FLOAT32_MAX;  /* no mipmapping */
        D3D12_CPU_DESCRIPTOR_HANDLE hs = h;
        hs.ptr += (SIZE_T)(t * RSX_FP_UNITS + (u32)u) * s_d3d.samp_inc;
        s_d3d.device->lpVtbl->CreateSampler(s_d3d.device, &sd, hs);
    }
    return t;
}

static void vp_bind_sampler_table(const D3D12DrawRecord* dr)
{
    if (!s_d3d.samp_heap) return;
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    s_d3d.samp_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.samp_heap, &g);
    g.ptr += (u64)vp_sampler_table(dr) * RSX_FP_UNITS * s_d3d.samp_inc;
    s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 3, g);
}

/* Resources dropped while a frame is being recorded, released once the GPU
 * has finished that frame (off_rt_release_deferred, after render_frame's
 * opening wait). Grows as needed: a frame that flips a surface between two
 * formats draw by draw drops a resource each time. */
static ID3D12Resource** s_rt_grave;
static int s_rt_grave_n, s_rt_grave_cap;

static void off_rt_release_deferred(void)
{
    for (int i = 0; i < s_rt_grave_n; i++) s_rt_grave[i]->lpVtbl->Release(s_rt_grave[i]);
    s_rt_grave_n = 0;
}

static void off_rt_defer_release(ID3D12Resource* res)
{
    if (!res) return;
    if (s_rt_grave_n == s_rt_grave_cap) {
        int nc = s_rt_grave_cap ? s_rt_grave_cap * 2 : 64;
        ID3D12Resource** g = (ID3D12Resource**)realloc(s_rt_grave, (size_t)nc * sizeof *g);
        if (!g) { wait_for_gpu(); res->lpVtbl->Release(res); return; }   /* best effort */
        s_rt_grave = g; s_rt_grave_cap = nc;
    }
    s_rt_grave[s_rt_grave_n++] = res;
}

static int off_rt_find(u32 off)
{
    if (!off) return -1;
    for (int i = 0; i < MAX_OFF_RTS; i++)
        if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].off == off) return i;
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
    ID3D12Resource* res;
    u32 off, w, h;      /* guest dims */
    u32 hw, hh;         /* the resource's dims (RSX_SCALE) */
    int used;
    D3D12_RESOURCE_STATES st;   /* DEPTH_WRITE as a DSV, PIXEL_SHADER_RESOURCE sampled */
} ZetaRT;
#define MAX_ZETA 8
static ZetaRT s_zeta[MAX_ZETA];
static ID3D12DescriptorHeap* s_zeta_dsv_heap;

static D3D12_CPU_DESCRIPTOR_HANDLE zeta_dsv(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_zeta_dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_zeta_dsv_heap, &h);
    h.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return h;
}

/* The same surface as a read-only depth-stencil view: depth-tested and
 * sampled by one draw. The resource is then in ZETA_READ_STATE. */
#define ZETA_READ_STATE ((D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_DEPTH_READ | \
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
static D3D12_CPU_DESCRIPTOR_HANDLE zeta_dsv_ro(int slot) { return zeta_dsv(MAX_ZETA + slot); }

static int zeta_find(u32 off)
{
    for (int i = 0; i < MAX_ZETA; i++)
        if (s_zeta[i].res && s_zeta[i].off == off) return i;
    return -1;
}

static void zeta_transition(int slot, D3D12_RESOURCE_STATES to)
{
    ZetaRT* z = &s_zeta[slot];
    if (z->st == to) return;
    D3D12_RESOURCE_BARRIER br = {0};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource   = z->res;
    br.Transition.StateBefore = z->st;
    br.Transition.StateAfter  = to;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &br);
    z->st = to;
}

/* Ensure a depth surface for this zeta offset at least w x h. A larger clip
 * recreates it (content lost once); a smaller one reuses it, since a pass may
 * render a sub-rectangle of the same surface. New surfaces start cleared. */
static int zeta_get(u32 off, u32 w, u32 h)
{
    if (!w) w = s_d3d.width;
    if (!h) h = s_d3d.height;
    if (!s_zeta_dsv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.NumDescriptors = MAX_ZETA * 2;   /* writable, then read-only */
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_zeta_dsv_heap)))
            return -1;
    }
    int slot = zeta_find(off);
    if (slot >= 0) {
        ZetaRT* z = &s_zeta[slot];
        if (z->w >= w && z->h >= h) { z->used = 1; return slot; }
        if (z->w > w) w = z->w;
        if (z->h > h) h = z->h;
        off_rt_defer_release(z->res); z->res = NULL;
    } else {
        for (int i = 0; i < MAX_ZETA; i++)
            if (!s_zeta[i].res) { slot = i; break; }
        if (slot < 0) {
            for (int i = 0; i < MAX_ZETA; i++)
                if (!s_zeta[i].used) { slot = i; break; }
            if (slot < 0) return -1;
            off_rt_defer_release(s_zeta[slot].res); s_zeta[slot].res = NULL;
        }
    }
    ZetaRT* z = &s_zeta[slot];
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = rt_host_dim(w, h, w); td.Height = rt_host_dim(w, h, h);
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R24G8_TYPELESS; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    cv.DepthStencil.Depth = 1.0f;
    if (FAILED(d3d_create_committed(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
            &IID_ID3D12Resource, (void**)&z->res)))
        return -1;
    z->off = off; z->w = w; z->h = h; z->used = 1;
    z->hw = (u32)td.Width; z->hh = td.Height;
    z->st = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_DEPTH_STENCIL_VIEW_DESC dd = {0};
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    s_d3d.device->lpVtbl->CreateDepthStencilView(s_d3d.device, z->res, &dd, zeta_dsv(slot));
    dd.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    s_d3d.device->lpVtbl->CreateDepthStencilView(s_d3d.device, z->res, &dd, zeta_dsv_ro(slot));
    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, zeta_dsv(slot),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
    printf("[ZETA] depth surface 0x%08X %ux%u (host %ux%u, slot %d)\n", off, w, h, z->hw, z->hh, slot);
    return slot;
}

/* Ensure an RT resource exists for this surface; (re)creates the RTV at
 * rt_rtv_heap[i] and the SRV at srv_heap[RT_SRV_BASE+i]. */
static int off_rt_get(u32 off, u32 w, u32 h, u32 rsx_fmt)
{
    if (!off || !s_d3d.rt_rtv_heap || !s_d3d.srv_heap) return -1;
    if (!w) w = s_d3d.width;
    if (!h) h = s_d3d.height;
    DXGI_FORMAT want_fmt = rsx_surface_dxgi(rsx_fmt);
    /* RT_FORCE_RGBA8=1: allocate every offscreen target as R8G8B8A8 rather than
     * its native format. A title whose final composite reads a half-float
     * surface cannot be shown with a plain CopyTextureRegion, and giving it an
     * 8-bit target costs HDR precision but makes the composite a same-format
     * copy. A stopgap for looking at the frame, not a rendering fix. */
    { static int f8 = -1;
      PS3_KNOB(f8, ps3_env_int("RT_FORCE_RGBA8", 0));
      if (f8) want_fmt = DXGI_FORMAT_R8G8B8A8_UNORM; }
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
        off_rt_defer_release(r->res); r->res = NULL;     /* dims/format changed */
        off_rt_defer_release(r->up);  r->up = NULL;
    } else {
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (!s_d3d.off_rt[i].res) { slot = i; break; }
        if (slot < 0) {   /* evict an entry not used this frame */
            for (int i = 0; i < MAX_OFF_RTS; i++)
                if (!s_d3d.off_rt[i].used) { slot = i; break; }
            if (slot < 0) {
                /* Every target is in use this frame: the draws into this one
                 * are dropped, and whatever samples it reads nothing. */
                static u64 said = ~0ull;
                if (said != s_d3d.frame_count) { said = s_d3d.frame_count;
                    static int n = 0;
                    if (n++ < 20) fprintf(stderr, "[RT] all %d offscreen targets in use this "
                                          "frame: 0x%X (%ux%u) not created%c",
                                          MAX_OFF_RTS, off, w, h, 10); }
                return -1;
            }
            { static int n = 0;   /* a target a later frame reads loses its content */
              if (n++ < 20) fprintf(stderr, "[RT] evicted 0x%X (%ux%u) for 0x%X (%ux%u)%c",
                                    s_d3d.off_rt[slot].off, s_d3d.off_rt[slot].w,
                                    s_d3d.off_rt[slot].h, off, w, h, 10); }
            /* same hazard as above: the open list may still use it */
            off_rt_defer_release(s_d3d.off_rt[slot].res); s_d3d.off_rt[slot].res = NULL;
            off_rt_defer_release(s_d3d.off_rt[slot].up);  s_d3d.off_rt[slot].up = NULL;
        }
    }
    OffRT* r = &s_d3d.off_rt[slot];
    const u32 hw = rt_host_dim(w, h, w), hh = rt_host_dim(w, h, h);
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = hw; td.Height = hh; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = want_fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = td.Format;
    if (FAILED(d3d_create_committed(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, &cv,
            &IID_ID3D12Resource, (void**)&r->res)))
        return -1;
    r->off = off; r->w = w; r->h = h; r->hw = hw; r->hh = hh; r->dxgi = (u32)want_fmt;
    r->st = D3D12_RESOURCE_STATE_COPY_DEST;
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
        u32 bpp = (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8u :
                  (want_fmt == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16u :
                  (want_fmt == DXGI_FORMAT_R32_FLOAT) ? 4u : 4u;
        /* A scaled target gets each guest texel repeated (nearest). */
        u32 pitch = (hw * bpp + 255) & ~255u;
        u32 src = cellGcmResolveOffset(off);
        u8* wide = (hw != w) ? (u8*)malloc((size_t)w * bpp) : NULL;
        if (vm_base && src != 0xFFFFFFFFu) {
            D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {0};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = (u64)pitch * hh; bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if ((hw == w || wide) && SUCCEEDED(d3d_create_committed(
                    s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void**)&r->up))) {
                void* mp = NULL; D3D12_RANGE nr = {0,0};
                if (SUCCEEDED(r->up->lpVtbl->Map(r->up, 0, &nr, &mp)) && mp) {
                    const u8* sp = vm_base + src;
                    for (u32 y = 0; y < hh; y++) {
                        const u8* srow = sp + (u64)((u64)y * h / hh) * w * bpp;
                        u8* hrow = (u8*)mp + (u64)y * pitch;
                        u8* drow = wide ? wide : hrow;
                        if (want_fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
                            /* guest A8R8G8B8 (bytes A,R,G,B) -> R,G,B,A */
                            for (u32 x = 0; x < w; x++) {
                                drow[x*4+0] = srow[x*4+1];
                                drow[x*4+1] = srow[x*4+2];
                                drow[x*4+2] = srow[x*4+3];
                                drow[x*4+3] = srow[x*4+0];
                            }
                        } else if (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
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
                    r->up->lpVtbl->Unmap(r->up, 0, NULL);
                    D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                    cdst.pResource = r->res;
                    cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    csrc.pResource = r->up;
                    csrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    csrc.PlacedFootprint.Footprint.Format   = want_fmt;
                    csrc.PlacedFootprint.Footprint.Width    = hw;
                    csrc.PlacedFootprint.Footprint.Height   = hh;
                    csrc.PlacedFootprint.Footprint.Depth    = 1;
                    csrc.PlacedFootprint.Footprint.RowPitch = pitch;
                    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                }
            }
        }
        free(wide);
        {
            D3D12_RESOURCE_BARRIER b = {0};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = r->res;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
            r->st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    s_d3d.rt_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, r->res, NULL, rh);

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = td.Format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(RT_SRV_BASE + slot) * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, r->res, &sv, sh);

    static int _log = 0;
    if (_log++ < 8)
        printf("[D3D12] offscreen RT %d: off=0x%X %ux%u host %ux%u (render-to-texture)\n",
               slot, off, w, h, hw, hh);
    return slot;
}

/* ---------------------------------------------------------------------------
 * Colour write-back: offscreen RTs -> guest memory.
 *
 * On the RSX a render target IS guest memory, and titles read it back with
 * the CPU, SPUs or the RSX's own transfer engines. Demon's Souls computes its
 * exposure on the GPU into the first texel of rotating 16x16 F_X32 targets and
 * feeds it to every level shader's tone-map constant (global parameter 0x87).
 * Rendering only into D3D12 resources left those bytes as stale heap data
 * (fp16 NaN, old fragment-program words) and the constant at 0; with the
 * write-back it carries the value the luminance chain computed (0.2018).
 *
 * Every RT targeted by a frame is copied to a per-slot readback buffer at the
 * end of that frame and, once the GPU is done, converted to the guest surface
 * layout (big-endian, linear pitch or swizzled) and stored at its offset.
 * RPCS3's equivalent is "Write Color Buffers". RSX_WRITE_COLOR=0 turns it off
 * (it costs a GPU wait plus a CPU conversion per frame).
 * -----------------------------------------------------------------------*/
static void off_rt_transition(int slot, D3D12_RESOURCE_STATES to);
/* RSX_WRITE_COLOR_LOG: guest ranges of the fragment programs drawn with,
 * so a write-back that lands on live ucode is reported. */
static u32 s_fp_seen[512][2]; static int s_fp_seen_n;
static void fp_seen_note(u32 ea, u32 len)
{
    for (int i = 0; i < s_fp_seen_n; i++) if (s_fp_seen[i][0] == ea) return;
    if (s_fp_seen_n < 512) { s_fp_seen[s_fp_seen_n][0] = ea; s_fp_seen[s_fp_seen_n][1] = len; s_fp_seen_n++; }
}

static int write_color_on(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("RSX_WRITE_COLOR", 1));
    return on;
}

/* RSX_WRITE_COLOR_DEFER (default 1): store the frame's write-back copies at the
 * start of the next frame, after the GPU wait that frame makes anyway, instead
 * of waiting for the GPU between submit and Present. That wait held up both the
 * Present and the FIFO drain on this thread for most of the GPU's frame time.
 * Guest reads of render targets (Demon's Souls's exposure) see the values one
 * frame later. 0 restores the store before Present. */
static int write_color_defer(void)
{
    static int on = -1;
    PS3_KNOB(on, ps3_env_int("RSX_WRITE_COLOR_DEFER", 1));
    return on;
}
static int s_wb_pending;   /* copies recorded, store not yet done */
static double s_fl_wait_ms, s_fl_store_ms;   /* FRAME_LOG: frame-start wait, store */

static void rt_pitch_record(D3D12DrawRecord* dr)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st) return;
    int a = st->color_target == 2 ? 1 : 0;
    dr->rt_pitch[0] = st->surface_color_pitch[a];
    dr->rt_dma[0]   = st->surface_color_dma[a];
    for (int i = 1; i < 4; i++) {
        dr->rt_pitch[i] = st->surface_color_pitch[i];
        dr->rt_dma[i]   = st->surface_color_dma[i];
    }
    dr->zeta = st->surface_zeta_offset;
    dr->ctgt = st->color_target;
    dr->zfmt = (st->surface_format >> 5) & 7u;
}

static u32 rt_guest_bpp(u32 dxgi)
{
    return dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u :
           dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16u : 4u;
}

/* Record the copies (before the frame's command list closes). */
static void rt_writeback_record(void)
{
    if (!write_color_on()) return;
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!r->res || !r->used) continue;
        /* A scaled target is not written back: the copy would have to be
         * scaled down first, and the targets the title reads (its exposure)
         * are below RSX_SCALE_MIN and stay at guest size. */
        if (r->hw != r->w || r->hh != r->h) continue;
        u32 pitch = (r->w * rt_guest_bpp(r->dxgi) + 255) & ~255u;
        u64 need = (u64)pitch * r->h;
        if (r->rb && r->rb_size < need) { r->rb->lpVtbl->Release(r->rb); r->rb = NULL; }
        if (!r->rb) {
            D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd = {0};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = need; rd.Height = 1; rd.DepthOrArraySize = 1;
            rd.MipLevels = 1; rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(d3d_create_committed(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                    &IID_ID3D12Resource, (void**)&r->rb)) || !r->rb) {
                r->rb = NULL; continue;
            }
            r->rb_size = need;
        }
        D3D12_RESOURCE_STATES prior = r->st;
        off_rt_transition(i, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = r->rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format   = (DXGI_FORMAT)r->dxgi;
        dst.PlacedFootprint.Footprint.Width    = r->w;
        dst.PlacedFootprint.Footprint.Height   = r->h;
        dst.PlacedFootprint.Footprint.Depth    = 1;
        dst.PlacedFootprint.Footprint.RowPitch = pitch;
        src.pResource = r->res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
        off_rt_transition(i, prior);
        r->used = 2;   /* copy pending */
    }
}

/* Morton index for a swizzled RSX surface of 2^lw x 2^lh texels: x and y bits
 * interleave (x lowest) until the shorter side runs out. */
static u32 rsx_swz_index(u32 x, u32 y, u32 lw, u32 lh)
{
    u32 o = 0, b = 0;
    for (u32 i = 0; i < 16 && (i < lw || i < lh); i++) {
        if (i < lw) o |= ((x >> i) & 1u) << b++;
        if (i < lh) o |= ((y >> i) & 1u) << b++;
    }
    return o;
}

/* n words from a readback row to guest memory. kind 0: RGBA8 to A,R,G,B
 * (a rotate), 1: FP16 (swap each half's bytes), 2: 32-bit lanes (bswap). */
static void wb_conv_row(u8* d, const u8* s, u32 n, int kind)
{
    /* One loop per kind, so each vectorises. */
    #define WB_LOOP(EXPR) for (u32 i = 0; i < n; i++) { \
        u32 v; memcpy(&v, s + (u64)i * 4, 4); v = (EXPR); memcpy(d + (u64)i * 4, &v, 4); }
    if (kind == 0)      WB_LOOP((v << 8) | (v >> 24))
    else if (kind == 1) WB_LOOP(((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu))
    else                WB_LOOP(_byteswap_ulong(v))
    #undef WB_LOOP
}

/* Store the copies in guest memory (after the GPU finished the frame). */
static u8* s_wc_shadow; static u32 s_wc_len;   /* RSX_WRITE_COLOR_LOG */
static void rt_writeback_store(void)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (!write_color_on() || !vm_base) return;
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!r->res || !r->rb || r->used != 2) continue;
        r->used = 1;
        u32 bpp = rt_guest_bpp(r->dxgi);
        u32 pitch = (r->w * bpp + 255) & ~255u;
        u32 swz = ((r->gfmt >> 8) & 0xF) == 2;
        u32 lw = (r->gfmt >> 16) & 0xFF, lh = (r->gfmt >> 24) & 0xFF;
        u32 gp = r->gpitch;
        if (swz) { if ((1u << lw) < r->w || (1u << lh) < r->h) continue; }
        else if (gp < r->w * bpp || gp > 0x10000) continue;
        /* The surface's context DMA says which memory the offset is in: local
         * and IO offsets are overlapping number spaces, so the offset alone
         * cannot place a surface the title keeps in main memory. */
        u32 dst = cellGcmResolveLocated(r->gdma != CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER, r->off);
        if (!dst || dst == 0xFFFFFFFFu) continue;
        { static int lg = -1; PS3_KNOB(lg, ps3_env_int("RSX_WRITE_COLOR_LOG", 0));
          if (lg > 0) { lg--;
            /* Did anything else write the surface since our last store? */
            static u8* shadow[MAX_OFF_RTS]; static u32 shoff[MAX_OFF_RTS], shlen[MAX_OFF_RTS];
            u32 len = swz ? ((1u << lw) << lh) * bpp : gp * r->h;
            u32 ndiff = 0, first = 0;
            if (shadow[i] && shoff[i] == r->off && shlen[i] == len)
                for (u32 k = 0; k < len; k++)
                    if (shadow[i][k] != vm_base[dst + k]) { if (!ndiff) first = k; ndiff++; }
            fprintf(stderr, "[WRITE-COLOR] off=0x%X -> 0x%08X %ux%u fmt=0x%08X pitch=%u dma=0x%X%s foreign=%u first=+0x%X%c",
                    r->off, dst, r->w, r->h, r->gfmt, gp, r->gdma, swz ? " swz" : "", ndiff, first, 10);
            for (int q = 0; q < s_fp_seen_n; q++) {
                u32 fa = s_fp_seen[q][0], fl = s_fp_seen[q][1];
                if (fa < dst + len && dst < fa + fl)
                    fprintf(stderr, "[WRITE-COLOR] !! surface 0x%X [0x%08X,+0x%X) overlaps FP ucode 0x%08X+0x%X%c",
                            r->off, dst, len, fa, fl, 10);
            }
            if (!shadow[i] || shlen[i] != len) { free(shadow[i]); shadow[i] = (u8*)malloc(len); shlen[i] = len; }
            shoff[i] = r->off;
            s_wc_shadow = shadow[i]; s_wc_len = len;
          } }
        void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)pitch * r->h};
        if (FAILED(r->rb->lpVtbl->Map(r->rb, 0, &rr, &mp)) || !mp) continue;
        /* A word at a time: every format here is whole 32-bit words per
         * texel, converted the same way. RGBA8 R,G,B,A -> A,R,G,B is a rotate,
         * FP16 swaps the bytes of each half, 32-bit lanes swap all four.
         * Byte by byte, this was 3.5-6 ms of each Demon's Souls frame (about
         * 25 MB: a 1280x720 target several times per frame, and others). */
        const int kind = r->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM ? 0 :
                         r->dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1 : 2;
        const u32 wpt = bpp / 4;                     /* words per texel */
        if (!swz) {
            for (u32 y = 0; y < r->h; y++)
                wb_conv_row(vm_base + dst + (u64)y * gp, (const u8*)mp + (u64)y * pitch,
                            r->w * wpt, kind);
        } else {
            /* rsx_swz_index(x, y) is the x bits' part OR the y bits' part. */
            static u32* xpart; static u32 xpart_n;
            if (xpart_n < r->w) {
                u32* nx = (u32*)realloc(xpart, (size_t)r->w * sizeof *nx);
                if (!nx) { D3D12_RANGE nw0 = {0, 0}; r->rb->lpVtbl->Unmap(r->rb, 0, &nw0); continue; }
                xpart = nx; xpart_n = r->w;
            }
            for (u32 x = 0; x < r->w; x++) xpart[x] = rsx_swz_index(x, 0, lw, lh);
            for (u32 y = 0; y < r->h; y++) {
                const u32 ypart = rsx_swz_index(0, y, lw, lh);
                const u8* srow = (const u8*)mp + (u64)y * pitch;
                for (u32 x = 0; x < r->w; x++)
                    wb_conv_row(vm_base + dst + (u64)(xpart[x] | ypart) * bpp,
                                srow + (u64)x * bpp, wpt, kind);
            }
        }
        D3D12_RANGE nw = {0, 0};
        r->rb->lpVtbl->Unmap(r->rb, 0, &nw);
        if (s_wc_shadow) { memcpy(s_wc_shadow, vm_base + dst, s_wc_len); s_wc_shadow = NULL; }
        r->gaddr = dst;
        r->glen = swz ? ((1u << lw) << lh) * bpp : gp * r->h;
        r->gsum_new = 1;
    }
    /* Checksums after every store, so targets that share memory do not
     * count each other's write-back as a foreign write. */
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!r->gsum_new) continue;
        r->gsum_new = 0;
        r->gsum = tex_csum(vm_base + r->gaddr, r->glen);
        r->gsum_ok = 1;
    }
}

/* Drop offscreen targets whose guest memory the title rewrote after our last
 * write-back: it streamed a texture or copied an image there. A texture bound
 * at that offset then reads the guest bytes, and a later draw into it starts
 * a new target from them. Sampling the old target instead is a live-only
 * error -- a replay starts with no targets and reads the guest bytes. */
static void off_rt_drop_rewritten(void)
{
    extern uint8_t* vm_base;
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("RSX_RT_REWRITTEN", 1));
    if (!en || !vm_base) return;
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!r->res || !r->gsum_ok) continue;
        if (tex_csum(vm_base + r->gaddr, r->glen) == r->gsum) continue;
        { static int n = 0;
          if (n++ < 40) fprintf(stderr, "[RT] 0x%X (%ux%u) rewritten by the title: "
                                "dropped (frame %llu)%c", r->off, r->w, r->h,
                                (unsigned long long)s_d3d.frame_count, 10); }
        off_rt_defer_release(r->res);  r->res = NULL;
        off_rt_defer_release(r->up);   r->up = NULL;
        off_rt_defer_release(r->snap); r->snap = NULL;
        r->gsum_ok = 0;
    }
}

static D3D12_CPU_DESCRIPTOR_HANDLE off_rt_rtv(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    s_d3d.rt_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return rh;
}

/* Write a texture SRV (or a null SRV when res == NULL) at an absolute SRV
 * heap slot. Used to fill per-draw t0-t3 descriptor windows. */
/* What each per-draw window descriptor last held. A draw at the same index
 * usually binds the same textures as it did the frame before, and at 5-6k
 * draws a frame (Demon's Souls's Boletarian Palace) re-creating all 16 views
 * of every window was ~11 ms of the render thread's frame. An unchanged view
 * is left as it is. The heap is only written after the GPU has finished the
 * previous frame (render_frame waits first), so skipping a write is safe. */
typedef struct { ID3D12Resource* res; u32 fmt, mapping, gen; } SrvMemo;
static SrvMemo s_srv_memo[MAX_DRAWS * RSX_FP_UNITS];

static void srv_write_ex(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt,
                         UINT mapping, int cube)
{
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("RSX_SRV_MEMO", 1));
    SrvMemo* m = NULL;
    if (en && heap_slot >= DRAW_SRV_BASE && heap_slot < VP_TEX_SRV_BASE) {
        m = &s_srv_memo[heap_slot - DRAW_SRV_BASE];
        const u32 g = (s_res_gen << 1) | (cube ? 1u : 0u);
        if (m->gen == g && m->res == res && m->fmt == (u32)fmt && m->mapping == mapping)
            return;
        m->res = res; m->fmt = (u32)fmt; m->mapping = mapping; m->gen = g;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = mapping;
    sv.Texture2D.MipLevels = 1;
    if (cube) {   /* must match the TextureCube declaration in the HLSL */
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = 1;
        sv.TextureCube.MostDetailedMip = 0;
        sv.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &h);
    h.ptr += (u64)heap_slot * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, res, &sv, h);
}

static void srv_write(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt, UINT mapping)
{
    srv_write_ex(heap_slot, res, fmt, mapping, 0);
}

static void off_rt_transition(int slot, D3D12_RESOURCE_STATES to)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (r->st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r->res;
    b.Transition.StateBefore = r->st;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    r->st = to;
}

/* A draw that samples a surface it is also rendering to. RSX allows it, and
 * Demon's Souls relies on it: its exposure adaptation (a 16x16 F_X32 target)
 * reads the previous exposure from the target it overwrites, each pixel its
 * own texel. D3D12 cannot have one resource bound as RTV and SRV at once; that
 * read returned 0, so the exposure never moved past one adaptation step from 0
 * and the level drew almost black. Copy the target just before the draw and
 * return the copy for the draw to sample: the contents RSX would have read. */
static ID3D12Resource* off_rt_snapshot(int slot)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (!r->res) return NULL;
    if (r->snap && (r->snap_w != r->hw || r->snap_h != r->hh || r->snap_dxgi != r->dxgi)) {
        off_rt_defer_release(r->snap);   /* the open list may still use it */
        r->snap = NULL;
    }
    if (!r->snap) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = r->hw; td.Height = r->hh; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = (DXGI_FORMAT)r->dxgi; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(d3d_create_committed(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void**)&r->snap)))
            return NULL;
        r->snap_w = r->hw; r->snap_h = r->hh; r->snap_dxgi = r->dxgi;
        r->snap_st = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    const D3D12_RESOURCE_STATES back = r->st;
    off_rt_transition(slot, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (r->snap_st != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = r->snap;
        b.Transition.StateBefore = r->snap_st;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list, r->snap, r->res);
    {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = r->snap;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
        r->snap_st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    off_rt_transition(slot, back);
    return r->snap;
}

/* Set by the present entry point: 1 = this batch ends with a swapchain
 * Present, 0 = execute the recorded draws only. An OFFSCREEN-only batch (a
 * render-to-texture pass) must still run -- it is what fills the texture a
 * later batch samples -- but presenting it would show a half-built frame. */
static int s_present_this_frame = 1;

/* Resolve a DRAW_*_TEX value: a hex raw offset, or the literal "duck" for the
 * content-identified duck texture. */
static u32 draw_filter_tex(const char* e)
{
    if (!e) return 0;
    if (e[0] == 'd' && e[1] == 'u') return s_duck_raw;
    return (u32)strtoul(e, NULL, 16);
}

/* Copy the current backbuffer into s_screen_copy. Called mid-frame the moment
 * the reduced-viewport passes finish -- this title renders its reflection into
 * the TOP-LEFT REGION of the render target and the main scene then overwrites
 * it, so a snapshot taken at end of frame contains the scene, not the
 * reflection. Also called at end of frame as a fallback for frames with no
 * reduced-viewport pass. */
static void screen_copy_capture(u32 fi)
{
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("SCREEN_AS_TEX", 1));
    if (!en || !s_d3d.device) return;
            if (!s_screen_copy) {
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC td = {0};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = s_d3d.width; td.Height = s_d3d.height;
                td.DepthOrArraySize = 1; td.MipLevels = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
                td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                if (FAILED(d3d_create_committed(
                        s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                        &IID_ID3D12Resource, (void**)&s_screen_copy)))
                    s_screen_copy = NULL;
            }
            if (s_screen_copy) {
                D3D12_RESOURCE_BARRIER bb[2] = {0};
                bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb[0].Transition.pResource   = s_d3d.render_targets[fi];
                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb[1] = bb[0];
                bb[1].Transition.pResource   = s_screen_copy;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list,
                    s_screen_copy, s_d3d.render_targets[fi]);

                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                /* SCREENCOPY_DUMP=1: stage the captured image into the readback
                 * buffer so it can be written out and LOOKED AT. Claiming a
                 * capture happened is not the same as showing what is in it. */
                { static int sd = -1;
                  PS3_KNOB(sd, ps3_env_int("SCREENCOPY_DUMP", 0));
                  if (sd && s_d3d.readback_buf) {
                      D3D12_RESOURCE_BARRIER t = {0};
                      t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                      t.Transition.pResource   = s_screen_copy;
                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);

                      D3D12_TEXTURE_COPY_LOCATION cd = {0}, cs = {0};
                      cd.pResource = s_d3d.readback_buf;
                      cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                      cd.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
                      cd.PlacedFootprint.Footprint.Width    = s_d3d.width;
                      cd.PlacedFootprint.Footprint.Height   = s_d3d.height;
                      cd.PlacedFootprint.Footprint.Depth    = 1;
                      cd.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
                      cs.pResource = s_screen_copy;
                      cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                      s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cd, 0, 0, 0, &cs, NULL);

                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);
                      s_sc_dump_pending = 1;
                  } }
            }

}

/* rsx_topology -> D3D_PRIMITIVE_TOPOLOGY. Spelled out rather than relying on
 * the two enums happening to agree: the draw record carries the neutral value
 * so a second backend can read it, and this is the one place it becomes a D3D
 * one. */
static D3D12_PRIMITIVE_TOPOLOGY topo_to_d3d(u32 t)
{
    switch (t) {
    case RSX_TOPOLOGY_POINTS:         return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case RSX_TOPOLOGY_LINES:          return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case RSX_TOPOLOGY_LINE_STRIP:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case RSX_TOPOLOGY_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case RSX_TOPOLOGY_TRIANGLES:      return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:                          return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

/* GCM_GUEST_FB=<off>,<w>,<h>,<pitch> -- present the guest display buffer.
 *
 * A title that composites its final image with the RSX 2D engine assembles it
 * in GUEST memory: nv3089_blit is a CPU copy and never touches D3D12, and the
 * draw pass below only runs when a recorded draw targets the display surface,
 * which for such a title never happens. The frame is finished, in the wrong
 * memory, and the swapchain shows only the clear. Copy it across.
 *
 * Off unless the offset and geometry are given, so nothing else changes. */
/* GPU-side composite of an offscreen RT onto the backbuffer.
 *
 * A title that finishes its frame with the RSX 2D engine blits a rendered
 * surface into the display buffer. That surface lives in guest RAM, which the
 * D3D12 path never writes -- the scene is on the GPU -- so the CPU copy in
 * nv3089_blit faithfully moves zeros. The backend already holds the same
 * surface as a texture, so do the composite here instead. Triggered from
 * cellGcmSys under GCM_COMPOSITE_RT. */
static u32 s_composite_src = 0;
static int s_last_drawn_slot = -1;
static u32 s_last_drawn_off  = 0;

void rsx_d3d12_composite_from_offscreen(u32 raw_off) { s_composite_src = raw_off; }

static void composite_present(u32 fi)
{
    u32 want = s_composite_src;
    s_composite_src = 0;
    if (!want) return;
    /* want == 2 means "whatever this frame's draws targeted" -- the one thing
     * guaranteed to hold geometry, and immune to the offset collision above. */
    int i;
    if (want == 2u) { i = s_last_drawn_slot; want = s_last_drawn_off; }
    else            { i = off_rt_find(want); }
    if (i < 0 || !s_d3d.off_rt[i].res) return;
    OffRT* r = &s_d3d.off_rt[i];
    /* Same-format copy only; a half-float source would need a converting draw. */
    if (r->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM) {
        static int w1 = 0;
        if (w1++ < 2)
            fprintf(stderr, "[COMPOSITE] RT 0x%08X is dxgi=%u, not R8G8B8A8 -- needs a draw%c",
                    want, r->dxgi, 10);
        return;
    }
    u32 cw = r->w < s_d3d.width  ? r->w : s_d3d.width;
    u32 ch = r->h < s_d3d.height ? r->h : s_d3d.height;
    if (!cw || !ch) return;
    { static int n = 0;
      if (n++ < 3) fprintf(stderr, "[COMPOSITE] RT 0x%08X %ux%u -> backbuffer%c",
                           want, cw, ch, 10); }

    D3D12_RESOURCE_BARRIER b[2] = {0};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource   = r->res;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = r->st;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1] = b[0];
    b[1].Transition.pResource   = s_d3d.render_targets[fi];
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, b);

    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = s_d3d.render_targets[fi];
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
    sl.pResource = r->res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
    D3D12_BOX box = {0};
    box.left = 0; box.top = 0; box.front = 0;
    box.right = cw; box.bottom = ch; box.back = 1;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl, 0, 0, 0, &sl, &box);

    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter  = r->st;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, b);
}


static void guest_fb_present(u32 fi)
{
    static int inited = 0;
    static u32 off, gw, gh, gpitch, up_pitch;
    static ID3D12Resource* up = NULL;
    if (!inited) {
        inited = 1;
        const char* e = getenv("GCM_GUEST_FB");
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (!e || sscanf(e, "%i,%u,%u,%u", &a, &b, &c, &d) != 4 || !b || !c) return;
        off = a; gw = b; gh = c; gpitch = d;
        up_pitch = (gw * 4u + 255u) & ~255u;      /* D3D12 wants 256-byte rows */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width  = (u64)up_pitch * gh; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(d3d_create_committed(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&up)))
            up = NULL;
        fprintf(stderr, "[GUESTFB] presenting guest 0x%08X %ux%u pitch %u -> backbuffer%c",
                off, gw, gh, gpitch, 10);
    }
    if (!up) return;
    { extern u8* vm_base;
      if (!vm_base) return;
      /* RSX local memory is mapped at guest 0xC0000000. */
      const u8* srow = vm_base + 0xC0000000u + off;
      void* m = NULL; D3D12_RANGE nr = {0, 0};
      if (FAILED(up->lpVtbl->Map(up, 0, &nr, &m)) || !m) return;
      for (u32 y = 0; y < gh; y++) {
          const u8* s = srow + (size_t)y * gpitch;
          u8* dp = (u8*)m + (size_t)y * up_pitch;
          for (u32 x = 0; x < gw; x++) {   /* guest B,G,R,A -> backbuffer R,G,B,A */
              dp[x*4+0] = s[x*4+2]; dp[x*4+1] = s[x*4+1];
              dp[x*4+2] = s[x*4+0]; dp[x*4+3] = 0xFF;
          }
      }
      D3D12_RANGE wr = {0, (SIZE_T)up_pitch * gh};
      up->lpVtbl->Unmap(up, 0, &wr); }

    D3D12_RESOURCE_BARRIER gb = {0};
    gb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    gb.Transition.pResource   = s_d3d.render_targets[fi];
    gb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    gb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    gb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &gb);

    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = s_d3d.render_targets[fi];
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
    sl.pResource = up; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Offset = 0;
    sl.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    sl.PlacedFootprint.Footprint.Width    = gw;
    sl.PlacedFootprint.Footprint.Height   = gh;
    sl.PlacedFootprint.Footprint.Depth    = 1;
    sl.PlacedFootprint.Footprint.RowPitch = up_pitch;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl, 0, 0, 0, &sl, NULL);

    gb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    gb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &gb);
}


/* Grow the VP vertex buffer to hold a frame of `want` vertices (with
 * headroom). The frame that overflowed has already lost its tail; the next
 * ones fit. The old buffer may still be read by the frame in flight, so it is
 * released with the deferred RT resources. VP_VB_MAX_VERTS caps the growth
 * (default 2M vertices = 1 GB of upload heap at 256 bytes x 2 parities). */
/* Growing creates and maps a new upload buffer of hundreds of MB on the render
 * thread: 100-400 ms for Demon's Souls's level, one frame, whose overflowing
 * draws are dropped as well. A title that needs more starts bigger
 * (VP_VB_VERTS). */
static void vp_vb_grow(u64 want)
{
    static u64 limit = 0;
    if (!limit) { const char* e = getenv("VP_VB_MAX_VERTS");
                  limit = e ? strtoull(e, NULL, 0) : (2ull << 20);
                  if (limit < s_vp_cap) limit = s_vp_cap; }
    u64 cap = s_vp_cap;
    while (cap < want + want / 8 && cap < limit) cap *= 2;
    if (cap > limit) cap = limit;
    if (cap <= s_vp_cap) {
        static int warned = 0;
        if (!warned++) fprintf(stderr, "[VPVB] a frame needs %llu vertices; the buffer stays at"
                               " %u (VP_VB_MAX_VERTS); later draws are dropped%c",
                               (unsigned long long)want, s_vp_cap, 10);
        return;
    }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Width = cap * 256 * 2;          /* VP_VERT_STRIDE, two parity halves */
    ID3D12Resource* nb = NULL; void* mp = NULL; D3D12_RANGE nr = {0, 0};
    if (FAILED(d3d_create_committed(s_d3d.device, &hp,
            D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&nb)) ||
        FAILED(nb->lpVtbl->Map(nb, 0, &nr, &mp)) || !mp) {
        if (nb) nb->lpVtbl->Release(nb);
        fprintf(stderr, "[VPVB] growing to %llu vertices failed; staying at %u%c",
                (unsigned long long)cap, s_vp_cap, 10);
        limit = s_vp_cap;
        return;
    }
    off_rt_defer_release(s_d3d.vp_vb);
    s_d3d.vp_vb = nb; s_d3d.vp_vb_mapped = mp; s_vp_cap = (u32)cap;
    fprintf(stderr, "[VPVB] a frame asked for %llu vertices: buffer grown to %u (%llu MB)%c",
            (unsigned long long)want, s_vp_cap, (unsigned long long)(bd.Width >> 20), 10);
}

/* The VP index buffer (indexed strip uploads, upload_strip_vp_indexed): u32
 * indices, two parity halves like vp_vb. Created at the first frame end and
 * grown there as vp_vb is; until it exists, or once a frame fills it, the
 * uploads expand to unindexed lists instead. */
static void vp_ib_grow(u64 want)
{
    const u64 limit = 64ull << 20;          /* 512 MB of upload heap */
    u64 cap = s_vp_icap ? s_vp_icap : (1u << 20);
    while (cap < want + want / 8 && cap < limit) cap *= 2;
    if (cap > limit) cap = limit;
    if (s_d3d.vp_ib && cap <= s_vp_icap) return;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Width = cap * 4 * 2;
    ID3D12Resource* nb = NULL; void* mp = NULL; D3D12_RANGE nr = {0, 0};
    if (FAILED(d3d_create_committed(s_d3d.device, &hp,
            D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&nb)) ||
        FAILED(nb->lpVtbl->Map(nb, 0, &nr, &mp)) || !mp) {
        if (nb) nb->lpVtbl->Release(nb);
        static int said = 0;
        if (!said++) fprintf(stderr, "[VPIB] index buffer of %llu indices failed%c",
                             (unsigned long long)cap, 10);
        return;
    }
    if (s_d3d.vp_ib) off_rt_defer_release(s_d3d.vp_ib);
    s_d3d.vp_ib = nb; s_d3d.vp_ib_mapped = mp; s_vp_icap = (u32)cap;
    fprintf(stderr, "[VPIB] index buffer: %u indices (%llu MB)%c",
            s_vp_icap, (unsigned long long)(bd.Width >> 20), 10);
}

/* -------------------------------------------------------------------------
 * Occlusion queries (NV4097 ZPASS pixel counts)
 *
 * A title counts the samples an object would cover -- the sun's probe quad,
 * say -- between CLEAR_REPORT_VALUE and GET_REPORT, with
 * SET_ZPASS_PIXEL_COUNT_ENABLE on, and reads the count back with
 * cellGcmGetReport. Answering "visible" every time drew Demon's Souls's sun
 * flare through walls. Each draw made while counting is on gets a D3D12
 * occlusion query; once the GPU has finished the frame the counts are summed
 * between the clear and the report, in stream order, and stored in the report.
 * The answer lands a frame later than on hardware, which a flare fading in
 * and out does not notice.
 *
 * Events are recorded with the record index they precede (draw_count at the
 * time), and draws are matched by cb_slot, their record index, so the debug
 * passes that reorder records do not move a query. RSX_ZPASS=0 goes back to
 * reporting "visible" (read at init).
 * -----------------------------------------------------------------------*/
extern void (*g_rsx_report_hook)(u32 method, u32 data);
#define ZQ_MAX     256    /* counted draws per batch */
#define ZQ_EV_MAX  256    /* report events per batch */
enum { ZQ_OFF = 0, ZQ_ON = 1, ZQ_CLEAR = 2, ZQ_REPORT = 3 };
static ID3D12QueryHeap* s_zq_heap;
static ID3D12Resource*  s_zq_rb;
static int  s_zq_on;                       /* counting enabled, as of the stream */
static int  s_zq_on_start;                 /* ... when the current batch began */
static struct { u32 at, kind, idx; } s_zq_ev[ZQ_EV_MAX];
static int  s_zq_nev;
static u32  s_zq_rec[ZQ_MAX];              /* cb_slot of each query issued */
static int  s_zq_n;                        /* queries issued this batch */
static u64  s_zq_acc;                      /* samples since the last clear */

static void zq_hook(u32 method, u32 data)
{
    u32 kind, idx = 0;
    if (method == 0x17CC) { s_zq_on = data != 0; kind = s_zq_on ? ZQ_ON : ZQ_OFF; }
    else if (method == 0x17C8) { if (data != 1) return; kind = ZQ_CLEAR; }
    else { if ((data >> 24) != 1) return; kind = ZQ_REPORT; idx = (data & 0xFFFFFFu) / 16u; }
    if (s_zq_nev < ZQ_EV_MAX) {
        s_zq_ev[s_zq_nev].at = s_d3d.draw_count;
        s_zq_ev[s_zq_nev].kind = kind;
        s_zq_ev[s_zq_nev].idx = idx;
        s_zq_nev++;
    }
}

/* Was counting on when record `slot` was made? */
static int zq_counted(u32 slot)
{
    int on = s_zq_on_start;
    for (int i = 0; i < s_zq_nev && s_zq_ev[i].at <= slot; i++)
        if (s_zq_ev[i].kind <= ZQ_ON) on = (int)s_zq_ev[i].kind;
    return on;
}

static int zq_ready(void)
{
    if (s_zq_heap) return 1;
    D3D12_QUERY_HEAP_DESC qd = {0};
    qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    qd.Count = ZQ_MAX;
    if (FAILED(s_d3d.device->lpVtbl->CreateQueryHeap(s_d3d.device, &qd,
            &IID_ID3D12QueryHeap, (void**)&s_zq_heap))) { s_zq_heap = NULL; return 0; }
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = ZQ_MAX * sizeof(u64);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(d3d_create_committed(s_d3d.device, &hp,
            D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_zq_rb))) {
        s_zq_heap->lpVtbl->Release(s_zq_heap); s_zq_heap = NULL; s_zq_rb = NULL; return 0;
    }
    return 1;
}

/* After the batch's GPU work is complete: sum the counts in stream order and
 * store each report. ZPASS_LOG=<n> prints the first n stored values. */
static void zq_finish(void)
{
    if (!s_zq_nev && !s_zq_n) { s_zq_on_start = s_zq_on; return; }
    u64 cnt[ZQ_MAX];
    memset(cnt, 0, sizeof cnt);
    if (s_zq_n) {
        void* mp = NULL;
        D3D12_RANGE rr = {0, (SIZE_T)s_zq_n * sizeof(u64)};
        if (SUCCEEDED(s_zq_rb->lpVtbl->Map(s_zq_rb, 0, &rr, &mp)) && mp) {
            memcpy(cnt, mp, (size_t)s_zq_n * sizeof(u64));
            D3D12_RANGE wr = {0, 0};
            s_zq_rb->lpVtbl->Unmap(s_zq_rb, 0, &wr);
        }
    }
    extern void cellGcm_report_store(u32 index, u32 value);
    static int logn = -1, logged = 0;
    PS3_KNOB(logn, ps3_env_int("ZPASS_LOG", 0));
    int qi = 0;
    for (int e = 0; e < s_zq_nev; e++) {
        for (; qi < s_zq_n && s_zq_rec[qi] < s_zq_ev[e].at; qi++) s_zq_acc += cnt[qi];
        if (s_zq_ev[e].kind == ZQ_CLEAR) s_zq_acc = 0;
        else if (s_zq_ev[e].kind == ZQ_REPORT) {
            const u32 v = s_zq_acc > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)s_zq_acc;
            cellGcm_report_store(s_zq_ev[e].idx, v);
            if (logged < logn) { logged++;
                fprintf(stderr, "[ZPASS] report %u = %u (%d queries this batch)%c",
                        s_zq_ev[e].idx, v, s_zq_n, 10); }
        }
    }
    for (; qi < s_zq_n; qi++) s_zq_acc += cnt[qi];
    s_zq_nev = 0;
    s_zq_n = 0;
    s_zq_on_start = s_zq_on;
}

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
 * thread calls rsx_prof_begin/rsx_prof_end; in between, every present is
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

void rsx_prof_begin(const char* prefix)
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
int rsx_prof_end(char* digest, size_t cap)
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
void rsx_prof_title(const char* note)
{
    static char base[256];
    if (!s_d3d.hwnd) return;
    if (!base[0]) GetWindowTextA(s_d3d.hwnd, base, sizeof base);
    char t[320];
    snprintf(t, sizeof t, "%s%s", base, note);
    SetWindowTextA(s_d3d.hwnd, t);
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

static int s_vp_topo_strip;   /* the VP pass's topology is TRIANGLESTRIP */
static void render_frame(void)
{
    vpj_sync();   /* the batch's vertices, before anything reads vp_vb */
    double _rf0 = perf_on() ? perf_now() : 0.0;
    { LARGE_INTEGER _q; QueryPerformanceCounter(&_q); s_rf_entry_qpc = _q.QuadPart; }
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
        s_d3d.readback_pitch = (s_d3d.width * 4 + 255) & ~255u;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (u64)s_d3d.readback_pitch * s_d3d.height;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d3d_create_committed(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.readback_buf);
    }

    /* Reset command allocator and list */
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);

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
                { static int _au = 0; if (_au++ < 3)
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
    if (getenv("RTT_DUMP") && s_d3d.draw_count > 0) { static int _f=0; int _cap = atoi(getenv("RTT_DUMP")); if (_cap < 2) _cap = 14; if (_f++ < _cap) {
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
                    { static int _n = 0; if (_n++ < 3)
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
                        { static int _n = 0; if (getenv("SUBVP_DBG") && _n++ < 8)
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
        if (dr->fp_addr) vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key,
                                       dr_num_rts(dr),
                                       dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                  : DXGI_FORMAT_R8G8B8A8_UNORM,
                                       dr->fp_exp32, dr->cmask, dr->cull, dr->depth,
                                       _cmask, dr->d2c_mask);
        if (perf_on()) { const double _pt = perf_now() - _ps0; s_perf_pso += _pt;
                         if (s_perf_pso_miss != _pm0) s_perf_pmiss += _pt; }
    }
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
    D3D12_VIEWPORT viewport = {0, 0, (float)s_d3d.width, (float)s_d3d.height, 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, (LONG)s_d3d.width, (LONG)s_d3d.height};
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
        if (any && vpso) {
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
            float tgt_sx = 1.0f, tgt_sy = 1.0f;    /* its host size over guest size (RSX_SCALE) */
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
            for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++) {
                const D3D12DrawRecord* dr = &s_d3d.draws[d];
                if (!dr->is_vp) continue;
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
                    D3D12_VIEWPORT vp = {0, 0, (float)s_d3d.width, (float)s_d3d.height, 0.0f, 1.0f};
                    float gw = vp.Width, gh = vp.Height;   /* the guest's size of it */
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
                        vp.Width  = wz >= 0 ? (float)s_zeta[wz].hw : (float)s_d3d.width;
                        vp.Height = wz >= 0 ? (float)s_zeta[wz].hh : (float)s_d3d.height;
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
                            if (_n++ < 3) fprintf(stderr, "[TESTTRI] issued on rt=%d off=0x%08X%c",
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
                 * back to the hardcoded atlas/colour PS pair. */
                ID3D12PipelineState* dpso =
                    dr->fp_addr ? vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key,
                                                dr_num_rts(dr),
                                                dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                           : DXGI_FORMAT_R8G8B8A8_UNORM,
                                                dr->fp_exp32, dr->cmask, dr->cull, dr->depth,
                                                dr_cube_mask(dr), dr->d2c_mask) : NULL;
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
                if (dr->index_count)
                    s_d3d.cmd_list->lpVtbl->DrawIndexedInstanced(s_d3d.cmd_list,
                        dr->index_count, 1, dr->ib_first, (INT)(dr->vb_byte_offset / 256), 0);
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
                D3D12_VIEWPORT vp = {0, 0, (float)s_d3d.width, (float)s_d3d.height, 0.0f, 1.0f};
                D3D12_RECT sc = {0, 0, (LONG)s_d3d.width, (LONG)s_d3d.height};
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
        if (vm_base && addr && _n++ < 6) {
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
      if (fb && s_req_draws) { static int n = 0; if (n++ < 40)
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
                    s_perf_fxc, s_perf_fxc_n, s_perf_sc_hits, s_perf_cpso, s_perf_cpso_n, s_perf_pmiss, 10);
            s_perf_frame = 0.0; s_perf_tex = 0.0; s_perf_vtx = 0.0; s_perf_rf = 0.0;
            s_perf_gpu = 0.0; s_perf_pre = 0.0; s_perf_srv = 0.0; s_perf_pso = 0.0;
            s_perf_pso_calls = 0; s_perf_pso_miss = 0; s_perf_pso_hashbytes = 0;
            s_perf_vs_compiles = 0; s_perf_vs = 0.0;
            s_perf_fxc = 0.0; s_perf_fxc_n = 0; s_perf_sc_hits = 0;
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
            WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA(probe, &fd);
            if (fh == INVALID_HANDLE_VALUE) break;
            FindClose(fh);
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
        dst.PlacedFootprint.Footprint.Width    = s_d3d.width;
        dst.PlacedFootprint.Footprint.Height   = s_d3d.height;
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

    /* The game's frame, scaled into the window's buffer. */
    if (s_present_this_frame) blit_to_window(fi);

    /* Close and execute */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cmd_lists[] = {(ID3D12CommandList*)s_d3d.cmd_list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cmd_lists);

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

    RF_CP(8);
    /* Present */
    { LARGE_INTEGER _pq0, _pq1;
      QueryPerformanceCounter(&_pq0);
      /* RSX_VSYNC (live, default 1): 0 presents without waiting for the
       * display's refresh, with tearing, so the title's own vblank is the
       * frame limit and a game set above the display's rate is not held to
       * it. Without tearing support (swap chain creation above) a sync
       * interval of 0 would still be paced at the refresh by the compositor,
       * so it presents with vsync and says so once. */
      static int vsync = -1;
      PS3_KNOB(vsync, ps3_env_int("RSX_VSYNC", 1));
      if (!vsync && !s_d3d.tearing) {
          static int said = 0;
          if (!said++) printf("[D3D12] RSX_VSYNC=0: tearing is not supported on this system; presenting with vsync\n");
      }
      const int tear = !vsync && s_d3d.tearing;
      if (s_present_this_frame && s_overlay_on && s_overlay.frame)
          s_overlay.frame(s_overlay.user, s_d3d.swap_buffers[s_d3d.frame_index], s_d3d.frame_index,
                          s_d3d.win_w, s_d3d.win_h);
      if (s_present_this_frame && knob_present() && !s_session_locked)   /* skipped for an offscreen-only batch */
          s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, tear ? 0 : 1,
                                            tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
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
            SetWindowTextA(s_d3d.hwnd, tb);
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
                  static u32 c0, f1, f2, k1, k2, k3;
                  fprintf(stderr, "[gcm-rates] t=%llu presents=%.1f/s flipwords=%u/s flips=%u/s label255=%08X (+%u) draws=%u"
                          " | flip:render=%u nodisp=%u clear:render=%u nodisp=%u sameflip=%u small=%u%c",
                          (unsigned long long)tnow, s_tframes * 1000.0 / (double)(tnow - s_tt0),
                          fw - fw0, fr - fr0, l, l - l0, s_dbg_last_draws,
                          s_pc_flip_render - f1, s_pc_flip_nodisp - f2, s_clear_presents - c0,
                          s_pc_clear_nodisp - k1, s_pc_clear_sameflip - k2, s_pc_clear_small - k3, 10);
                  fw0 = fw; l0 = l; fr0 = fr;
                  f1 = s_pc_flip_render; f2 = s_pc_flip_nodisp; c0 = s_clear_presents;
                  k1 = s_pc_clear_nodisp; k2 = s_pc_clear_sameflip; k3 = s_pc_clear_small;
              } }
            s_tframes = 0;
            s_tt0 = tnow;
        }
    }
}

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

    if (s_d3d.initialized && flip_has_display)
        render_frame();
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
    /* Same display gate as the other presents: offscreen pass work alone
     * keeps accumulating until its composite arrives. */
    int has_display = 0;
    for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++)
        if (!s_d3d.draws[i].is_clear && s_d3d.draws[i].rt_off == 0) { has_display = 1; break; }
    if (!has_display) { s_pc_flip_nodisp++; return; }
    if (blink_dbg())
        printf("[FLIP] presenting %u draws at the title's flip%c", s_d3d.draw_count, 10);
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
    if (s_fifo_flip_present && present_at_flip_on()) return;   /* the flip ends the frame */

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

/* Our host vertex layout for the fallback path: position (xyz) + color (rgba)
 * + texcoord (uv), 36 bytes. (The real VP path feeds raw float4 attrib0.) */
typedef struct { float x, y, z; float r, g, b, a; float u, v; } BasicVertex;


/* Read one RSX vertex (by absolute vertex index) from guest memory into our
 * host layout. Position is attrib 0 (float3+), color is attrib 3 (ubyte4 or
 * float4); missing attribs default to opaque white. RSX stores each 32-bit
 * component big-endian, so every lane is byte-swapped. */
static void read_rsx_vertex(const rsx_state* state, u32 vindex, BasicVertex* out)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    out->x = out->y = out->z = 0.0f;
    out->r = out->g = out->b = out->a = 1.0f;
    out->u = out->v = 0.0f;
    if (!state || !vm_base) return;

    const rsx_vertex_attrib* pos = &state->vertex_attribs[0];
    if (pos->enabled && pos->type == 2 /* float */ && pos->size >= 2) {
        u8* src = vm_base + cellGcmResolveOffset(pos->offset + vindex * pos->stride);
        out->x = rsx_rd_bef(src);
        out->y = rsx_rd_bef(src + 4);
        if (pos->size >= 3) out->z = rsx_rd_bef(src + 8);

        /* dbgfont (and similar 2D overlays) store screen positions biased far
         * outside clip space; their vertex program folds them back to clip
         * space using transform constants we don't execute. When a position is
         * well outside NDC, recover the fractional part as a normalized [0,1]
         * screen coord and map it to NDC (screen Y-down -> clip Y-up). In this
         * layout the attribute is [posX, posY, U, V] (size 4), so the last two
         * components are the atlas texcoords, not depth.
         * TODO: execute the real vertex-program / RSX viewport transform so
         * this is not needed. */
        if (out->x > 2.0f || out->x < -2.0f || out->y > 2.0f || out->y < -2.0f) {
            float sx = out->x - floorf(out->x);
            float sy = out->y - floorf(out->y);
            out->x = sx * 2.0f - 1.0f;
            out->y = 1.0f - sy * 2.0f;
            out->z = 0.0f;
            if (pos->size >= 4) {
                out->u = rsx_rd_bef(src + 8);   /* U */
                out->v = rsx_rd_bef(src + 12);  /* V */
            }
        }
    }

    const rsx_vertex_attrib* col = &state->vertex_attribs[3];
    if (col->enabled && col->size >= 3) {
        u8* src = vm_base + cellGcmResolveOffset(col->offset + vindex * col->stride);
        if (col->type == 4 /* ubyte */) {
            out->r = src[0] / 255.0f;
            out->g = src[1] / 255.0f;
            out->b = src[2] / 255.0f;
            out->a = (col->size >= 4) ? src[3] / 255.0f : 1.0f;
        } else if (col->type == 2 /* float */) {
            u32 fr, fg, fb, fa;
            memcpy(&fr, src,     4); fr = ((fr>>24)&0xFF)|((fr>>8)&0xFF00)|((fr<<8)&0xFF0000)|((fr<<24)&0xFF000000);
            memcpy(&fg, src + 4, 4); fg = ((fg>>24)&0xFF)|((fg>>8)&0xFF00)|((fg<<8)&0xFF0000)|((fg<<24)&0xFF000000);
            memcpy(&fb, src + 8, 4); fb = ((fb>>24)&0xFF)|((fb>>8)&0xFF00)|((fb<<8)&0xFF0000)|((fb<<24)&0xFF000000);
            memcpy(&out->r, &fr, 4); memcpy(&out->g, &fg, 4); memcpy(&out->b, &fb, 4);
            if (col->size >= 4) {
                memcpy(&fa, src + 12, 4); fa = ((fa>>24)&0xFF)|((fa>>8)&0xFF00)|((fa<<8)&0xFF0000)|((fa<<24)&0xFF000000);
                memcpy(&out->a, &fa, 4);
            }
        }
    }
}

/* Upload `count` sequential vertices [first, first+count). Returns the count
 * actually written (clamped to the remaining per-frame buffer). */
static u32 upload_vertices_from_rsx(u32 first, u32 count)
{
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (count > max_verts) count = max_verts;
    const rsx_state* state = s_d3d.current_rsx_state;
    for (u32 i = 0; i < count; i++)
        read_rsx_vertex(state, first + i, &verts[i]);
    s_d3d.vb_offset += count * sizeof(BasicVertex);
    return count;
}

/* Upload RSX QUADS (prim 8) as a triangle list: each 4-vertex quad v0..v3
 * (perimeter winding) splits into triangles (v0,v1,v2) and (v0,v2,v3).
 * D3D12 has no quad topology, so this expansion is how quads render at all.
 * Returns the number of triangle-list vertices emitted (6 per quad). */
static u32 upload_quads_from_rsx(u32 first, u32 count)
{
    const rsx_state* state = s_d3d.current_rsx_state;
    u32 quads = count / 4;
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (quads * 6 > max_verts) quads = max_verts / 6;
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        BasicVertex c[4];
        for (u32 k = 0; k < 4; k++)
            read_rsx_vertex(state, first + q * 4 + k, &c[k]);
        verts[o++] = c[0]; verts[o++] = c[1]; verts[o++] = c[2];
        verts[o++] = c[0]; verts[o++] = c[2]; verts[o++] = c[3];
    }
    s_d3d.vb_offset += o * sizeof(BasicVertex);
    return o;
}

/* Upload RSX QUADS as raw float4 attrib0 (byte-swapped) into vp_vb, expanded
 * to a triangle list (6 verts/quad). The decompiled vertex shader does the
 * transform. Returns emitted vertex count. */
/* Generic VP vertex: all 16 RSX vertex attributes, each converted to a float4
 * slot -- 256 bytes/vertex, input layout ATTRi @ i*16. Apps place attributes at
 * arbitrary indices (tiny3d: pos=a0 colour=a3 tex=a8; SDK gcm samples: pos=a0
 * colour=a1 tex=a2; dbgfont: a0..a2), so a hardcoded pos+colour pair can't
 * cover them: every enabled attrib is fetched from guest memory and converted
 * by its RSX type; disabled slots read (0,0,0,1). */
typedef struct { float v[4]; } VPSlot;
#define VP_VERT_STRIDE (16 * sizeof(VPSlot))   /* 256 */


void rsx_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n);

/* Fill all 16 attribute slots for one vertex. The per-attribute work lives in
 * rsx_vertex_fetch.c so the Metal and null backends read guest vertices the
 * same way this one does, rather than each porting the logic again. */
/* VP_FETCH_FAST (live, default 1): each VP upload starts with
 * vp_plan_begin, which builds the draw's fetch plan (rsx_fetch_plan_init) and
 * opens a new generation of the vertex cache. 0 = the per-attribute
 * rsx_fetch_attrib path and no cache, for A/B. */
static rsx_fetch_plan s_vp_plan;
static float s_vp_tmp[16][4];   /* read_vp_vertex's vertex, see there */
static int s_vp_fast = 1;
#define VP_VC_N 256u
static u32 s_vc_key[VP_VC_N], s_vc_tag[VP_VC_N], s_vc_gen;
static VPSlot s_vc_v[VP_VC_N][16];
/* For indexed uploads: where this draw's vertices went in vp_vb, keyed like
 * the vertex cache (and in the same generation). */
static u32 s_vx_key[VP_VC_N], s_vx_tag[VP_VC_N], s_vx_out[VP_VC_N];
/* Vertices the draw being extended already has (d3d12_draw_indexed), 0 for
 * a new draw: the indices a merged range writes count from there, and the
 * vertex cache carries over, so the vertices two ranges share are written
 * once. */
static u32 s_strip_base;

/* RSX_LATE_CHECK, vertices: these are read when the draw is recorded, so the
 * question is the other way round from textures -- did the title write a
 * vertex array after issuing the draw that reads it? Each VP upload notes the
 * guest bytes of every enabled array over the vertex range it fetched, and
 * late_check hashes them again when the batch renders. A replay reads its
 * snapshot from the end of the frame, so a late write shows live only. */
#define LATE_VTX_MAX 16384
typedef struct { const u8* p; u32 len, sum, fp, draw, slot, off; } LateVtx;
static LateVtx* s_late_vtx;
static u32 s_late_vtx_n, s_lv_lo, s_lv_hi, s_lv_draw, s_lv_klo, s_lv_khi, s_lv_idma, s_lv_ioff;
static int s_lv_open;
static rsx_fetch_plan s_lv_plan;
static u32 lv_elem_bytes(u32 type, u32 n)
{
    switch (type) { case 4: case 7: return n; case 1: case 3: case 5: return 2 * n;
                    default: return 4 * n; }
}
static void lv_close(void)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int, u32);
    if (!s_lv_open) return;
    s_lv_open = 0;
    if (s_lv_lo > s_lv_hi || !vm_base) return;
    for (u32 k = 0; k < s_lv_plan.n_on && s_late_vtx_n < LATE_VTX_MAX; k++) {
        const rsx_fetch_slot* sl = &s_lv_plan.on[k];
        u32 lo = s_lv_lo, hi = s_lv_hi;
        if (sl->freq) { if (sl->modulo) { lo = 0; hi = sl->freq - 1; }
                        else { lo /= sl->freq; hi /= sl->freq; } }
        const u32 off = sl->off + lo * sl->stride;
        u64 len = (u64)(hi - lo) * sl->stride + lv_elem_bytes(sl->type, sl->n);
        if (len > (16u << 20)) len = 16u << 20;
        const u32 ga = sl->main ? cellGcmResolveLocated(0, off) : sl->local_addr + off;
        if (ga == 0xFFFFFFFFu) continue;
        LateVtx* e = &s_late_vtx[s_late_vtx_n++];
        e->p = vm_base + ga; e->len = (u32)len; e->off = sl->off;
        e->sum = tex_csum_full(e->p, e->len);
        e->fp = s_lv_plan.st ? s_lv_plan.st->shader_program : 0;
        e->draw = s_lv_draw; e->slot = sl->idx;
    }
    /* The index range, too. */
    if (s_lv_klo <= s_lv_khi && s_late_vtx_n < LATE_VTX_MAX) {
        const u32 isz = ((s_lv_idma >> 4) & 0xF) == 1 ? 2 : 4;
        const u32 base = cellGcmResolveLocated((s_lv_idma & 0xF) == 0, s_lv_ioff);
        if (base != 0xFFFFFFFFu) {
            LateVtx* e = &s_late_vtx[s_late_vtx_n++];
            e->p = vm_base + base + (u64)s_lv_klo * isz;
            e->len = (s_lv_khi - s_lv_klo + 1) * isz; e->off = s_lv_ioff;
            e->sum = tex_csum_full(e->p, e->len);
            e->fp = s_lv_plan.st ? s_lv_plan.st->shader_program : 0;
            e->draw = s_lv_draw; e->slot = 16;
        }
    }
}
static void lv_open(const rsx_state* state)
{
    lv_close();
    if (!late_on()) return;
    if (!s_late_vtx && !(s_late_vtx = calloc(LATE_VTX_MAX, sizeof *s_late_vtx))) return;
    rsx_fetch_plan_init(state, &s_lv_plan);
    s_lv_lo = 0xFFFFFFFFu; s_lv_hi = 0; s_lv_open = 1;
    s_lv_draw = s_d3d.draw_count;
    s_lv_klo = 0xFFFFFFFFu; s_lv_khi = 0;
    s_lv_idma = state->index_array_dma; s_lv_ioff = state->index_array_offset;
}
static void late_vtx_check(void)
{
    lv_close();
    if (!s_late_vtx || !s_late_vtx_n) return;
    static int said = 0;
    u32 nchg = 0;
    for (u32 i = 0; i < s_late_vtx_n; i++) {
        const LateVtx* e = &s_late_vtx[i];
        if (tex_csum_full(e->p, e->len) == e->sum) continue;
        nchg++;
        if (said < 60) { said++;
            fprintf(stderr, "[LATE] frame %llu draw %u fp 0x%X %s%u array 0x%X (%u bytes) "
                    "written after the draw read it%c", (unsigned long long)s_d3d.frame_count,
                    e->draw, e->fp, e->slot == 16 ? "index" : "a", e->slot == 16 ? 0 : e->slot,
                    e->off, e->len, 10); }
    }
    if (nchg) {
        static int n = 0;
        if (n++ < 200) fprintf(stderr, "[LATE] frame %llu: %u of %u vertex reads changed "
                               "between draw and render%c", (unsigned long long)s_d3d.frame_count,
                               nchg, s_late_vtx_n, 10);
    }
    s_late_vtx_n = 0;
}

static void vp_plan_begin(const rsx_state* state)
{
    lv_open(state);
    static int fast = -1;
    PS3_KNOB(fast, ps3_env_int("VP_FETCH_FAST", 1));
    s_vp_fast = fast != 0;
    if (s_vp_fast) {
        rsx_fetch_plan_init(state, &s_vp_plan);
        memcpy(s_vp_tmp, s_vp_plan.konst, sizeof s_vp_tmp);
    }
    if (s_strip_base) return;   /* continuing the last draw: its vertices stay */
    if (++s_vc_gen == 0) { memset(s_vc_tag, 0, sizeof s_vc_tag);
                           memset(s_vx_tag, 0, sizeof s_vx_tag); s_vc_gen = 1; }
}

static void read_vp_vertex(const rsx_state* state, u32 vi, VPSlot* out16)
{
    if (s_lv_open) { if (vi < s_lv_lo) s_lv_lo = vi; if (vi > s_lv_hi) s_lv_hi = vi; }
    if (s_vp_fast && s_vp_plan.st == state) {
        /* Built in s_vp_tmp, then one copy: out16 is usually the upload
         * heap, which is write-combined. s_vp_tmp holds the draw's constant
         * attributes from vp_plan_begin on, and only the components the
         * arrays supply change from vertex to vertex. */
        s_vs_fetch++; s_vs_attr += s_vp_plan.n_on;
        rsx_fetch_vertex_arrays(&s_vp_plan, vi, s_vp_tmp);
        memcpy(out16, s_vp_tmp, sizeof s_vp_tmp);
    } else {
        for (int i = 0; i < 16; i++)
            rsx_fetch_attrib(state, i, vi, out16[i].v);
    }
    { const rsx_vertex_attrib* a = &state->vertex_attribs[0];
      u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
      rsx_vtx_pos_dbg(state, out16[0].v, n); }
}

/* For uploads that emit the same vertex more than once (strips and fans
 * expanded to lists, indexed draws): a direct-mapped cache of this draw's
 * fetched vertices. A guest vertex cannot change within one draw, so a hit is
 * exactly what a fetch would return. */
static void read_vp_vertex_c(const rsx_state* state, u32 vi, VPSlot* out16)
{
    if (!s_vp_fast) { read_vp_vertex(state, vi, out16); return; }
    const u32 h = (vi ^ (vi >> 8)) & (VP_VC_N - 1);
    if (s_vc_tag[h] != s_vc_gen || s_vc_key[h] != vi) {
        read_vp_vertex(state, vi, s_vc_v[h]);
        s_vc_key[h] = vi; s_vc_tag[h] = s_vc_gen;
    }
    memcpy(out16, s_vc_v[h], VP_VERT_STRIDE);
}

/* VP_FETCH_THREADS=<n> (read once; default 1; 0 = off): convert the vertices
 * of indexed strip uploads on worker threads. The FIFO thread only walks the
 * indices, numbers the draw's vertices and queues them (vpj_queue); the
 * workers fetch and convert them straight into vp_vb. In a Demon's Souls level
 * frame the conversion was about a third of the FIFO thread, and HighGraphics
 * waits on that thread's labels before each flip.
 *
 * One worker, whatever the machine: the conversion only has to overlap the
 * FIFO walk, and one keeps up. More cost the frame instead -- Palace, 60 fps
 * mode with VBLANK_MHZ=120000: 1 worker 63-64 fps, 2 about 61, 4 (the old
 * default of a quarter of the logical CPUs) 57-59, with each of the four at
 * ~55% of a core, half of it in the kernel going to sleep and waking. Waking
 * one worker per publish instead of all of them did not change that. The
 * threads the frame waits on (HighGraphics, the image-2 SPU thread, the FIFO
 * walker) run slower beside them. 0 workers: ~46 fps at a 60 cap.
 *
 * A queued vertex is read from guest memory later than the FIFO reached the
 * draw, so everything the title can observe of the RSX's progress waits for
 * the queue first (vpj_sync, g_rsx_sync_hook): labels, semaphores, reports,
 * the reference, the 2D engine's writes, `get`, and the frame's render. */
typedef struct {
    rsx_fetch_plan plan;     /* copied: the FIFO moves on before it is used */
    const u32* vis;          /* guest vertex indices, n of them */
    u32 n;
    VPSlot* out;             /* n vertices of VP_VERT_STRIDE */
} VPJob;
#define VPJ_JOBS 4096u
#define VPJ_VIS  (1u << 20)
static VPJob* s_vpj;
static u32* s_vpj_vis;
static u32 s_vpj_vis_n;
/* The shared counters, each on its own cache line: the workers claim from
 * `next` and count into `done` while the FIFO thread publishes into `pub`,
 * and on one line every publish waited for the workers' writes. */
typedef struct { volatile LONG v; char pad[60]; } VPJCounter;
static __declspec(align(64)) VPJCounter s_vpj_ctr[4];
#define s_vpj_pub      (s_vpj_ctr[0].v)   /* jobs the workers may take */
#define s_vpj_next     (s_vpj_ctr[1].v)
#define s_vpj_done     (s_vpj_ctr[2].v)
#define s_vpj_sleepers (s_vpj_ctr[3].v)
/* FIFO thread only: jobs queued (published or not), and the vertices queued
 * since the last publish. Jobs are published in batches of VPJ_BATCH
 * vertices (read once, default 1024): one publish per draw was a locked
 * increment of a line the workers spin on, ~6% of the FIFO thread. */
static LONG s_vpj_q;
static u32 s_vpj_qv;
static SRWLOCK s_vpj_lk = SRWLOCK_INIT;
static CONDITION_VARIABLE s_vpj_cv = CONDITION_VARIABLE_INIT;

static void vpj_run(LONG j)
{
    const VPJob* J = &s_vpj[j];
    float tmp[16][4];
    memcpy(tmp, J->plan.konst, sizeof tmp);
    for (u32 i = 0; i < J->n; i++) {
        rsx_fetch_vertex_arrays(&J->plan, J->vis[i], tmp);
        memcpy(&J->out[i * 16], tmp, sizeof tmp);
    }
}
static LONG vpj_claim(void)
{
    for (;;) {
        const LONG n = s_vpj_next;
        if (n >= s_vpj_pub) return -1;
        if (InterlockedCompareExchange(&s_vpj_next, n + 1, n) == n) return n;
    }
}
static DWORD WINAPI vpj_worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        LONG j = vpj_claim();
        if (j >= 0) { vpj_run(j); InterlockedIncrement(&s_vpj_done); continue; }
        for (int k = 0; k < 2000 && s_vpj_next >= s_vpj_pub; k++) YieldProcessor();
        if (s_vpj_next < s_vpj_pub) continue;
        AcquireSRWLockExclusive(&s_vpj_lk);
        InterlockedIncrement(&s_vpj_sleepers);   /* before the check: see vpj_publish */
        while (s_vpj_next >= s_vpj_pub)
            SleepConditionVariableSRW(&s_vpj_cv, &s_vpj_lk, INFINITE, 0);
        InterlockedDecrement(&s_vpj_sleepers);
        ReleaseSRWLockExclusive(&s_vpj_lk);
    }
}
/* Let the workers take everything queued so far. The exchange is also the
 * full barrier the sleeper check needs (see vpj_worker). */
static void vpj_publish(void)
{
    s_vpj_qv = 0;
    if (s_vpj_q == s_vpj_pub) return;
    InterlockedExchange(&s_vpj_pub, s_vpj_q);
    /* A worker counts itself a sleeper before it checks for work, so either
     * it sees these jobs or this sees it. */
    if (s_vpj_sleepers) {
        AcquireSRWLockExclusive(&s_vpj_lk);
        WakeAllConditionVariable(&s_vpj_cv);
        ReleaseSRWLockExclusive(&s_vpj_lk);
    }
}
/* Wait for every queued vertex, converting what no worker has taken yet. */
static void vpj_sync(void)
{
    vpj_publish();
    if (!s_vpj_pub) return;
    LONG j;
    while ((j = vpj_claim()) >= 0) { vpj_run(j); InterlockedIncrement(&s_vpj_done); }
    while (s_vpj_done < s_vpj_pub) YieldProcessor();
    InterlockedExchange(&s_vpj_pub, 0);   /* first: a stale claim then sees nothing */
    InterlockedExchange(&s_vpj_next, 0);
    InterlockedExchange(&s_vpj_done, 0);
    s_vpj_q = 0;
    s_vpj_vis_n = 0;
}
static void vpj_sync_hook(void) { vpj_sync(); }
static int vpj_threads(void)
{
    static int n = -1;
    if (n >= 0) return n;
    n = ps3_env_int("VP_FETCH_THREADS", 1);
    if (n > 16) n = 16;
    if (n > 0) {
        s_vpj = (VPJob*)malloc(sizeof(VPJob) * VPJ_JOBS);
        s_vpj_vis = (u32*)malloc(sizeof(u32) * VPJ_VIS);
        if (!s_vpj || !s_vpj_vis) { n = 0; return n; }
        for (int i = 0; i < n; i++) {
            HANDLE h = CreateThread(NULL, 0, vpj_worker, NULL, 0, NULL);
            if (h) CloseHandle(h);
        }
        extern void (*g_rsx_sync_hook)(void);
        g_rsx_sync_hook = vpj_sync_hook;
        fprintf(stderr, "[VPJ] %d vertex fetch threads%c", n, 10);
    }
    return n;
}
/* Can this draw's vertices go to the workers? Every array type must be one
 * rsx_fetch_vertex_arrays converts itself (its fallback reads the live
 * state), and no debug knob may want to see the vertices as they upload. */
static int vpj_usable(void)
{
    if (vpj_threads() <= 0 || s_lv_open) return 0;
    static int dbg = -1;
    PS3_KNOB(dbg, (getenv("RSX_DBG_VTX") || ps3_env_int("VTX_POS", 0)) ? 1 : 0);
    if (dbg) return 0;
    for (u32 k = 0; k < s_vp_plan.n_on; k++) {
        const u32 t = s_vp_plan.on[k].type;
        if (t < 1 || t > 7 || t == 6) return 0;
    }
    return 1;
}
/* Room for a draw of up to `nv` vertices, syncing to make it. */
static int vpj_room(u32 nv)
{
    if (nv > VPJ_VIS) return 0;
    if (s_vpj_q >= (LONG)VPJ_JOBS || s_vpj_vis_n + nv > VPJ_VIS) vpj_sync();
    return 1;
}
static void vpj_queue(const u32* vis, u32 n, VPSlot* out)
{
    VPJob* J = &s_vpj[s_vpj_q];
    memcpy(&J->plan, &s_vp_plan, sizeof J->plan);
    J->vis = vis; J->n = n; J->out = out;
    s_vpj_vis_n += n;
    s_vpj_q++;
    static int batch = -1;
    if (batch < 0) batch = ps3_env_int("VPJ_BATCH", 1024);
    s_vpj_qv += n;
    if (s_vpj_qv >= (u32)batch) vpj_publish();
}

/* VTX_POS=<N>: print the first fetched position of the first N draws. A guest
 * vertex array that resolves to the wrong memory reads as garbage/denormals, and
 * that is indistinguishable from "the shader is wrong" without seeing the input. */
void rsx_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n)
{
    static int cap = -1, seen = 0;
    PS3_KNOB(cap, ps3_env_int("VTX_POS", 0));
    if (!cap || seen >= cap) return;
    seen++;
    const rsx_vertex_attrib* a = &state->vertex_attribs[0];
    fprintf(stderr, "[VTXPOS] a0 off=0x%X stride=%u size=%u type=%u -> (%g, %g, %g, %g)\n",
            a->offset, a->stride, a->size, a->type,
            v[0], v[1], v[2], v[3]);   /* all four: masking by `size` hid whether w survived */
}

static void vp_attrs_dbg(const rsx_state* state)
{
    static int en = -1;   /* once: this runs for every draw */
    PS3_KNOB(en, getenv("VP_ATTRS") ? 1 : 0);
    if (!en) return;
    /* VP_ATTRS_FP=<hex shader_program>: dump only the draws that use one
     * fragment program. Without it the first six draws are whatever the frame
     * happens to start with, which is never the mesh being investigated. */
    { static const char* e = (const char*)1; static u32 want = 0;
      if (e == (const char*)1) { e = getenv("VP_ATTRS_FP");
          want = e ? (u32)strtoul(e, NULL, 16) : 0; }   /* hex, like FP_PICK */
      if (want && (!state || state->shader_program != want)) return;
      /* With a program picked, also write the vertex program its first draw
       * runs (vp_pick.hlsl): the attribute list says what is fed, this says
       * what is read and how the outputs are formed. */
      static int wrote = 0;
      if (want && !wrote && state->vp_ucode_bytes >= 16) {
          extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
          static char vh[262144];
          u32 vs0 = state->transform_program_start * 16u;
          if (vs0 >= state->vp_ucode_bytes) vs0 = 0;
          const int ni = rsx_vp_decompile(state->vp_ucode + vs0, state->vp_ucode_bytes - vs0,
                                          vh, sizeof vh);
          FILE* f = ni > 0 ? fopen("vp_pick.hlsl", "w") : NULL;
          if (f) { fprintf(f, "/* fp=0x%X start=%u */\n%s", want, vs0 / 16u, vh); fclose(f); }
          wrote = 1;
      } }
    /* VP_CONSTS=<i,j,...>: print those vertex constants whenever their values
     * change (capped). A VS that derives colour purely from constants renders
     * black if the guest's uploads never landed, which the attrib dump can't show. */
    { const char* e = getenv("VP_CONSTS");
      if (e) {
          static float last[16][4]; static int nl = -1, nprint = 0;
          int idx[16]; int n = 0; const char* p = e;
          while (*p && n < 16) { idx[n++] = (int)strtol(p, (char**)&p, 0); if (*p == ',') p++; else break; }
          int changed = (nl != n);
          for (int k = 0; k < n && !changed; k++)
              if (memcmp(last[k], state->vertex_constants[idx[k] & 511], 16)) changed = 1;
          if (changed && nprint < 60) {
              nprint++; nl = n;
              for (int k = 0; k < n; k++) {
                  const float* c = state->vertex_constants[idx[k] & 511];
                  memcpy(last[k], c, 16);
                  fprintf(stderr, "[VPCONST] fp=0x%X c[%d] = (%g, %g, %g, %g)%c",
                          state->shader_program, idx[k], c[0], c[1], c[2], c[3], 10);
              }
          }
      } }
    /* Dedupe by the set of enabled attributes rather than printing the first
     * few draws: one fragment program can be used by meshes with different
     * vertex layouts, and a mesh missing an attribute takes the constant
     * register instead -- which is exactly the case worth seeing. */
    { u32 mask = 0;
      for (int i = 0; i < 16; i++) if (state->vertex_attribs[i].enabled) mask |= 1u << i;
      static u32 seen[16]; static int ns = 0;
      for (int i = 0; i < ns; i++) if (seen[i] == mask) return;
      if (ns >= 16) return;
      seen[ns++] = mask; }
    fprintf(stderr, "[VPATTR] --- fp=0x%X ---%c", state->shader_program, 10);
    fprintf(stderr, "[VPATTR] divider_op=0x%08X\n", state->frequency_divider_op);
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &state->vertex_attribs[i];
        if (a->enabled) fprintf(stderr, "[VPATTR] a%d off=0x%X stride=%u size=%u type=%u freq=%u fmt=0x%08X\n",
                                i, a->offset, a->stride, a->size, a->type, a->frequency, a->format);
    }
    /* VP_ATTRS_DUMP=<attr>:<n>: read n entries of that attribute straight out of
     * guest memory. Answers "did the guest write these values, or did our fetch
     * mangle them" without inferring it from the rendered image. */
    { const char* e = getenv("VP_ATTRS_DUMP"); if (!e) return;
      int ai = 0, cnt = 8; sscanf(e, "%d:%d", &ai, &cnt);
      const rsx_vertex_attrib* a = &state->vertex_attribs[ai];
      if (!a->enabled || !a->stride) return;
      extern uint8_t* vm_base;
      extern u32 cellGcmResolveLocated(int, u32);
      u32 nbad = 0, nzero = 0;
      for (int v = 0; v < cnt; v++) {
          u32 aoff = (a->offset & 0x7FFFFFFFu) + (u32)v * a->stride;
          const u8* q = vm_base + ((a->offset & 0x80000000u)
                        ? cellGcmResolveLocated(0, aoff) : cellGcmResolveLocated(1, aoff));
          /* Also read the same offset through the OTHER context-DMA. An array
           * the guest put in main memory but whose offset we resolve as LOCAL
           * reads as untouched VRAM, i.e. all zeros -- indistinguishable from
           * "the producer never ran". */
          if (v == 0) {
              u32 la = cellGcmResolveLocated(1, aoff), ma = cellGcmResolveLocated(0, aoff);
              const u8* lp = vm_base + la; const u8* mp = vm_base + ma;
              const u8* rp = vm_base + (a->offset & 0x7FFFFFFFu);   /* raw, unresolved */
              fprintf(stderr, "[VPDUMP] a%d off=0x%X  LOCAL@0x%X=(%g,%g,%g)  MAIN@0x%X=(%g,%g,%g)"
                              "  RAW@0x%X=(%g,%g,%g)%c",
                      ai, aoff, la, rsx_rd_bef(lp), rsx_rd_bef(lp+4), rsx_rd_bef(lp+8),
                      ma, rsx_rd_bef(mp), rsx_rd_bef(mp+4), rsx_rd_bef(mp+8),
                      (a->offset & 0x7FFFFFFFu), rsx_rd_bef(rp), rsx_rd_bef(rp+4), rsx_rd_bef(rp+8), 10);
          }
          u32 nc = a->size ? a->size : 3; if (nc > 3) nc = 3;
          float f[3] = {0,0,0};
          for (u32 k = 0; k < nc; k++) f[k] = rsx_rd_bef(q + k * 4);
          if (v < 12)
              fprintf(stderr, "[VPDUMP] a%d[%d] = (%g, %g, %g)%c", ai, v, f[0], f[1], f[2], 10);
          int allz = 1; for (u32 k = 0; k < nc; k++) if (f[k] != 0.f) allz = 0;
          if (allz) nzero++;
          else if (f[2] < -0.9f && f[0] > -0.1f && f[0] < 0.1f) nbad++;
      }
      fprintf(stderr, "[VPDUMP] a%d over %d verts: %u zero, %u near (0,0,-1)%c",
              ai, cnt, nzero, nbad, 10);
      /* How many entries differ from the first? An all-identical position array
       * is a degenerate mesh: it rasterizes nothing, which looks the same as
       * "the draw never happened". */
      { float f0[3]; u32 o0 = (a->offset & 0x7FFFFFFFu);
        const u8* q0 = vm_base + ((a->offset & 0x80000000u)
                       ? cellGcmResolveLocated(0, o0) : cellGcmResolveLocated(1, o0));
        for (int k = 0; k < 3; k++) f0[k] = rsx_rd_bef(q0 + k * 4);
        u32 diff = 0;
        for (int v = 1; v < cnt; v++) {
            u32 ao = o0 + (u32)v * a->stride;
            const u8* q = vm_base + ((a->offset & 0x80000000u)
                          ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
            for (int k = 0; k < 3; k++)
                if (rsx_rd_bef(q + k * 4) != f0[k]) { diff++; break; }
        }
        fprintf(stderr, "[VPDUMP] a%d: %u/%d entries differ from entry 0 (%g,%g,%g)%c",
                ai, diff, cnt, f0[0], f0[1], f0[2], 10);
        /* Extent: an isosurface with little fluid is a speck, and a speck that
         * rasterizes nothing is correct, not a bug. */
        { float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (f < lo[k]) lo[k] = f; if (f > hi[k]) hi[k] = f; } }
          u32 nnan = 0, nfin = 0;
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              int bad = 0;
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (!(f == f) || f > 1e30f || f < -1e30f) bad = 1; }
              if (bad) nnan++; else nfin++;
          }
          fprintf(stderr, "[VPDUMP] a%d extent x[%g..%g] y[%g..%g] z[%g..%g]"
                          "  finite=%u nan/inf=%u%c",
                  ai, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], nfin, nnan, 10); } } }
}

static void dbg_vtx_dump(const rsx_state* state, const VPSlot* out, u32 first, u32 nverts)
{
    /* RSX_DBG_VTX=<hex tex0 raw> (live, re-armed by every setenv): for the
     * next 8 draws that bind that texture, print every enabled vertex
     * attribute of the first 4 vertices as the VP reads them, and the
     * non-zero constant range. RSX_DBG_VTX=fp:<hex>[:n] matches the fragment
     * program instead and prints vertex 0 of the next n draws (default 80):
     * one line per quad of a HUD. fp:<hex>:<n>:<v> prints v vertices of each.
     * For "drawn but invisible" and "drawn in the wrong place" questions. */
    { static unsigned gen; static u32 want = 0, by_fp = 0, fp_verts = 1; static int n = 0, lim = 8;
      if (gen != ps3_env_gen) { const char* dv = getenv("RSX_DBG_VTX");
          gen = ps3_env_gen; n = 0; by_fp = dv && strncmp(dv, "fp:", 3) == 0;
          if (by_fp) { char* e; want = (u32)strtoul(dv + 3, &e, 16);
                       lim = *e == ':' ? (int)strtol(e + 1, &e, 10) : 80;
                       fp_verts = *e == ':' ? (u32)atoi(e + 1) : 1; }
          else { want = dv ? (u32)strtoul(dv, NULL, 16) : 0; lim = 8; } }
      if (want && (by_fp ? state->shader_program == want : s_d3d.cur_texs[0].raw == want) && n < lim) { n++;
        if (by_fp && nverts > fp_verts) nverts = fp_verts;
        if (by_fp) fprintf(stderr, "[DBGVTX] #%d t0=0x%08X", n, s_d3d.cur_texs[0].raw);
        fprintf(stderr, "[DBGVTX] fp=0x%08X first=%u verts=%u", state->shader_program, first, nverts);
        { int lo = -1, hi = -1, nz = 0;
          for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
              const float* m = state->vertex_constants[r];
              if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
          }
          fprintf(stderr, " | constants: %d non-zero, slots %d..%d%c", nz, lo, hi, 10); }
        for (u32 k = 0; k < nverts && (by_fp || k < 4); k++) {
            fprintf(stderr, "  v%u", k);
            for (int a = 0; a < 16; a++)
                if (state->vertex_attribs[a].enabled)
                    fprintf(stderr, " a%d=(%.3f,%.3f,%.3f,%.3f)", a, out[k*16+a].v[0],
                            out[k*16+a].v[1], out[k*16+a].v[2], out[k*16+a].v[3]);
            fprintf(stderr, "%c", 10);
        }
      } }
}

static u32 upload_quads_vp(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    vp_plan_begin(state);
    u32 quads = count / 4;
    u32 maxv = (s_vp_cap * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    s_vp_want += (u64)quads * 6;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * s_vp_cap * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex(state, first + q*4 + k, c[k]);
        /* quad -> two triangles (perimeter winding) */
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (knob_vtx_dump()) { static int _n=0; if (_n++ < 4) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) { for (u32 k = 0; k < 4; k++)
                fprintf(f, "q%02d v%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", _n, k,
                    c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                    c[k][8].v[0],c[k][8].v[1]);
              fclose(f); } } }
    }
    dbg_vtx_dump(state, out, first, o);
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Straight triangle-list upload through the VP path (gcm/cube's cube draws
 * TRIANGLES, prim 5 -- no expansion needed). */
static u32 upload_tris_vp(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++; s_vp_want += count;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    vp_plan_begin(state);
    u32 maxv = (s_vp_cap * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && _n++ < 12)
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * s_vp_cap * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      for (u32 k = 0; k < count; k++)
          read_vp_vertex(state, first + k, &out[k*16]);
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; } }
    /* IDXDBG=<hex shader_program>: vertex range for that program's draws. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if (_n++ < 12)
          fprintf(stderr, "[IDXDBG] tris first=%u count=%u a0off=0x%X stride=%u%c",
                  first, count, state->vertex_attribs[0].offset,
                  state->vertex_attribs[0].stride, 10); } }
    if (knob_vtx_dump() && state->surface_color_offset[0] == 0xCC0000) {
        static int _n=0; if (_n++ < 1) {
        FILE* f = fopen("vtx_dump.txt", "w");
        if (f) { fprintf(f, "G-buffer draw surf0=0x%X count=%u first=%u\n",
                         state->surface_color_offset[0], count, first);
          for (u32 k = 0; k < count && k < 30; k++)
            fprintf(f, "v%02u a0=(%.3f,%.3f,%.3f) a2=(%.3f,%.3f,%.3f) a9=(%.4f,%.4f,%.4f,%.4f)\n", k,
                out[k*16].v[0],out[k*16].v[1],out[k*16].v[2],
                out[k*16+2].v[0],out[k*16+2].v[1],out[k*16+2].v[2],
                out[k*16+9].v[0],out[k*16+9].v[1],out[k*16+9].v[2],out[k*16+9].v[3]);
          fclose(f); } } }
    dbg_vtx_dump(state, out, first, count);
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Non-indexed TRIANGLE_STRIP (fan=0) / TRIANGLE_FAN (fan=1) through the VP
 * path, expanded to a triangle list. LBP's Bink movie draws its YUV quad as a
 * non-indexed 4-vertex strip (prim 6); without this it fell to the fixed-
 * function fallback with is_vp=0 (no vertex-program transform -> the quad
 * collapsed to a line) and textured=0 (the Y/U/V planes never sampled). */
static u32 upload_strip_vp(const rsx_state* state, u32 first, u32 count, int fan)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    vp_plan_begin(state);
    if (count < 3) return 0;
    u32 tris = count - 2;
    u32 maxv = (s_vp_cap * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    s_vp_want += (u64)tris * 3;
    if (tris * 3 > maxv) tris = maxv / 3;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * s_vp_cap * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 t = 0; t < tris; t++) {
        u32 i0 = fan ? 0 : t;
        /* strip winding alternates per triangle to keep facing consistent */
        u32 i1 = t + 1, i2 = t + 2;
        if (!fan && (t & 1)) { u32 tmp = i1; i1 = i2; i2 = tmp; }
        read_vp_vertex_c(state, first + i0, &out[o*16]); o++;
        read_vp_vertex_c(state, first + i1, &out[o*16]); o++;
        read_vp_vertex_c(state, first + i2, &out[o*16]); o++;
    }
    dbg_vtx_dump(state, out, first, o);
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Fetch index k from the guest index array (SET_INDEX_ARRAY_ADDRESS/_DMA:
 * dma [3:0] = location (0 local, 1 main), [7:4] = type (0 u32, 1 u16)).
 * Indices are big-endian in guest memory. */
static u32 read_guest_index(const rsx_state* st, u32 k)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (s_lv_open) { if (k < s_lv_klo) s_lv_klo = k; if (k > s_lv_khi) s_lv_khi = k; }
    int local = ((st->index_array_dma & 0xF) == 0);
    u32 base = cellGcmResolveLocated(local, st->index_array_offset);
    const u8* p = vm_base + base;
    if (((st->index_array_dma >> 4) & 0xF) == 1) {
        p += (u64)k * 2;
        return ((u32)p[0] << 8) | p[1];
    }
    p += (u64)k * 4;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* read_guest_index for a loop over one draw's indices: the array's base is
 * resolved once, not per index (cellGcmResolveLocated is out of line, in
 * cellGcmSys.c, and the strip walk reads millions of indices a second). The
 * caller notes the range for the live-vertex window itself. */
typedef struct { const u8* p; int w16; } GuestIdx;
/* A strip or fan split over several DRAW_INDEX_ARRAY ranges of one
 * BEGIN/END carries its state into the next range (upload_strip_vp_indexed,
 * upload_strip_vp_fetch). */
static struct { int valid, fan; u32 epoch, next, m, p0, p1; } s_strip_carry;
static GuestIdx guest_index_open(const rsx_state* st)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    GuestIdx g;
    g.p = vm_base + cellGcmResolveLocated((st->index_array_dma & 0xF) == 0,
                                          st->index_array_offset);
    g.w16 = ((st->index_array_dma >> 4) & 0xF) == 1;
    return g;
}
static inline u32 guest_index_at(const GuestIdx* g, u32 k)
{
    if (g->w16) {
        const u8* p = g->p + (u64)k * 2;
        return ((u32)p[0] << 8) | p[1];
    }
    const u8* p = g->p + (u64)k * 4;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Indexed variants of the VP-path uploads. */
static u32 upload_quads_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    vp_plan_begin(state);
    u32 quads = count / 4;
    u32 maxv = (s_vp_cap * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    s_vp_want += (u64)quads * 6;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * s_vp_cap * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex_c(state, read_guest_index(state, first + q*4 + k), c[k]);
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (knob_vtx_dump()) { static int _n=0; if (_n++ < 6) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) {
                const float* vs_ = state->viewport_scale;
                const float* vo_ = state->viewport_offset;
                fprintf(f, "iq%02d vp_scale=(%.1f,%.1f,%.3f) vp_off=(%.1f,%.1f,%.3f)\n", _n,
                        vs_[0], vs_[1], vs_[2], vo_[0], vo_[1], vo_[2]);
                for (int _c = 464; _c <= 467; _c++)
                    fprintf(f, "  c[%d]=(%.3f,%.3f,%.3f,%.3f)\n", _c,
                            state->vertex_constants[_c][0], state->vertex_constants[_c][1],
                            state->vertex_constants[_c][2], state->vertex_constants[_c][3]);
                for (u32 k = 0; k < 4; k++)
                    fprintf(f, "  v%u i%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", k,
                        read_guest_index(state, first + q*4 + k),
                        c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                        c[k][8].v[0],c[k][8].v[1]);
                fclose(f);
            } } }
    }
    dbg_vtx_dump(state, out, first, o);
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

static u32 upload_tris_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++; s_vp_want += count;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    vp_plan_begin(state);
    u32 maxv = (s_vp_cap * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && _n++ < 12)
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * s_vp_cap * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      if (!state->restart_index_enable) {
          for (u32 k = 0; k < count; k++)
              read_vp_vertex_c(state, read_guest_index(state, first + k), &out[k*16]);
      } else {
          /* Primitive restart: the restart index drops the triangle in
           * progress and the list resumes after it.
           *
           * One BEGIN/END can carry several DRAW_INDEX_ARRAY ranges (at most
           * 256 indices each), and the RSX assembles them as one stream: a
           * triangle may start in one range and end in the next. Demon's
           * Souls' particles come as (0,32) (32,256) (288,12). Dropping the
           * two indices left over at the end of each range put the rest out
           * of phase, so sparks and smoke drew as slivers joining one
           * particle to the next, different every frame. The partial
           * triangle now carries into the next contiguous range of the same
           * BEGIN/END. */
          static struct { int valid; u32 epoch, next, n, tri[2]; } carry;
          u32 o = 0, n = 0, tri[3];
          if (carry.valid && carry.epoch == state->begin_epoch && carry.next == first) {
              n = carry.n; tri[0] = carry.tri[0]; tri[1] = carry.tri[1];
          }
          for (u32 k = 0; k < count; k++) {
              const u32 ix = read_guest_index(state, first + k);
              if (ix == state->restart_index) { n = 0; continue; }
              tri[n++] = ix;
              if (n == 3) {
                  for (int j = 0; j < 3; j++) read_vp_vertex_c(state, tri[j], &out[(o + j)*16]);
                  o += 3; n = 0;
              }
          }
          carry.valid = 1; carry.epoch = state->begin_epoch; carry.next = first + count;
          carry.n = n; carry.tri[0] = tri[0]; carry.tri[1] = tri[1];
          count = o;
      }
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; } }
    /* IDXDBG=<hex shader_program>: index range for that program's draws. An
     * index past the vertex array reads unmapped guest memory as zero, which
     * shows up as a spike triangle with a zero texcoord, not as a missing draw. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if (_n++ < 12) {
          u32 lo = 0xFFFFFFFFu, hi = 0;
          for (u32 k = 0; k < count; k++) { u32 ix = read_guest_index(state, first + k);
              if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
          fprintf(stderr, "[IDXDBG] first=%u count=%u idx=[%u..%u] dma=0x%X a0off=0x%X stride=%u%c",
                  first, count, lo, hi, state->index_array_dma,
                  state->vertex_attribs[0].offset, state->vertex_attribs[0].stride, 10); } } }
    /* DBG_LOCK tracking: projected centroid of this mesh, updated every draw. */
    { static const char* dl = (const char*)1; static u32 wantl = 0; static int mvpb = 256;
      if (dl == (const char*)1) { dl = getenv("RSX_DBG_VTX");
                                  wantl = dl ? (u32)strtoul(dl, NULL, 16) : 0;
                                  const char* mb = getenv("MVP_BASE");
                                  if (mb) mvpb = atoi(mb); }
      /* RSX_DBG_TRACK=1 tracks the content-identified duck without needing its
       * offset passed in (VRAM offsets move between runs). Re-read every call:
       * s_duck_raw is filled by the texture upload, which happens AFTER the
       * first draws, so caching it once left the tracker watching offset 0. */
      { static int dtrk = -1;
        PS3_KNOB(dtrk, ps3_env_int("RSX_DBG_TRACK", 0));
        if (dtrk && s_duck_raw) wantl = s_duck_raw; }
      /* RSX_DBG_PICK=<first>: track ONLY the draw with that starting index. The
       * mesh is one big vertex buffer sliced into 256-index chunks, so averaging
       * across all of them converges on the centre of the whole field -- which is
       * where there is nothing in particular. A single slice is a stable target
       * and can be magnified without drifting off it. */
      static int pick = -1;
      PS3_KNOB(pick, ps3_env_int("RSX_DBG_PICK", -2));
      if (wantl && s_d3d.cur_texs[0].raw == wantl && count
          && (pick == -2 || (int)first == pick)) {
          double sx = 0, sy = 0, sz = 0;
          for (u32 k = 0; k < count; k++) {
              sx += out[k*16].v[0]; sy += out[k*16].v[1]; sz += out[k*16].v[2];
          }
          float v[4] = { (float)(sx/count), (float)(sy/count), (float)(sz/count), 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[mvpb + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          if (clip[3] > 1e-6f) {
              float nx = clip[0]/clip[3], ny = clip[1]/clip[3];
              if (nx > -2.0f && nx < 2.0f && ny > -2.0f && ny < 2.0f) {
                  s_lock_sx += nx; s_lock_sy += ny; s_lock_n++;
                  if (!s_lock_valid) {        /* seed so frame 1 is usable */
                      s_lock_x = nx; s_lock_y = ny; s_lock_valid = 1;
                  }
              }
          }
      } }
    /* RSX_DBG_SCALE=<f>: multiply attribute-0 positions for the draws binding
     * RSX_DBG_VTX's texture. The duck's object-space bbox is ~0.2 units and its
     * draws rasterize only a few dozen pixels per frame -- sub-pixel. Growing it
     * about its own origin tests the rest of the chain (index buffer, texture,
     * per-draw transform): if a duck-shaped, duck-coloured object appears, only
     * the scale is wrong. Diagnostic. */
    { static const char* ds = (const char*)1; static float sc = 0.0f;
      if (ds == (const char*)1) { ds = getenv("RSX_DBG_SCALE");
                                  sc = ds ? (float)atof(ds) : 0.0f; }
      static const char* dv2 = (const char*)1; static u32 want2 = 0;
      if (dv2 == (const char*)1) { dv2 = getenv("RSX_DBG_VTX");
                                   want2 = dv2 ? (u32)strtoul(dv2, NULL, 16) : 0; }
      if (sc > 0.0f && want2 && s_d3d.cur_texs[0].raw == want2 && count) {
        /* Magnify about a FIXED point captured from the first tracked draw, not
         * about each chunk's own centre -- recentring per chunk would stack every
         * chunk of the mesh on top of the others. Combined with VP_BYPASS (which
         * maps attribute 0 straight to clip space) this renders the mesh's real
         * geometry and texture at a readable size. */
        /* RSX_DBG_RECENTER=1: recentre EVERY chunk on its own centroid, overlaying
         * them. If each chunk is one instance of the same mesh they align into a
         * single silhouette; if they are arbitrary slices of one big mesh they
         * smear. Either way the answer is visible at a glance. */
        static int recen = -1;
        PS3_KNOB(recen, ps3_env_int("RSX_DBG_RECENTER", 0));
        static int have_c = 0; static float ox = 0, oy = 0, oz = 0;
        if (!have_c || recen) {
            double sx = 0, sy = 0, sz = 0;
            for (u32 k = 0; k < count; k++) {
                sx += out[k*16].v[0]; sy += out[k*16].v[1]; sz += out[k*16].v[2];
            }
            ox = (float)(sx/count); oy = (float)(sy/count); oz = (float)(sz/count);
            have_c = 1;
        }
        for (u32 k = 0; k < count; k++) {
            /* RSX_DBG_SHIFT="dx,dy": nudge after scaling. VP_BYPASS applies the
             * guest's posoffset, so a recentred mesh does not land at screen
             * centre -- without this the magnified geometry sits clipped against
             * the top edge. */
            static int shf = -1; static float dx = 0.0f, dy = 0.0f;
            if (shf < 0) { const char* e = getenv("RSX_DBG_SHIFT");
                           if (e) { double a=0,b=0; sscanf(e, "%lf,%lf", &a, &b);
                                    dx = (float)a; dy = (float)b; }
                           shf = 1; }
            out[k*16].v[0] = (out[k*16].v[0] - ox) * sc + dx;
            out[k*16].v[1] = (out[k*16].v[1] - oy) * sc + dy;
            /* Leave z alone: under VP_BYPASS the object z maps straight to clip
             * z, so scaling it walks the geometry out of the depth range and
             * everything clips away. */
            (void)oz;
        }
      } }
    /* RSX_DBG_VTX=<hex tex0 offset>: attribute-0 positions for the draws binding
     * that texture (see upload_tris_vp). The duck's meshes are indexed, so this
     * is the copy that actually fires for it. */
    { static const char* dv = (const char*)1; static u32 want = 0; static int n = 0;
      if (dv == (const char*)1) { dv = getenv("RSX_DBG_VTX");
                                  want = dv ? (u32)strtoul(dv, NULL, 16) : 0; }
      if (want && s_d3d.cur_texs[0].raw == want && n < 8) { n++;
        float mnx=1e30f,mny=1e30f,mnz=1e30f,mxx=-1e30f,mxy=-1e30f,mxz=-1e30f;
        for (u32 k = 0; k < count; k++) {
            float x=out[k*16].v[0], y=out[k*16].v[1], z=out[k*16].v[2];
            if (x<mnx)mnx=x; if (y<mny)mny=y; if (z<mnz)mnz=z;
            if (x>mxx)mxx=x; if (y>mxy)mxy=y; if (z>mxz)mxz=z;
        }
        const rsx_vertex_attrib* _a3 = &state->vertex_attribs[3];
        fprintf(stderr, "[DUCKVTX] a3(en=%d type=%u size=%u stride=%u off=0x%08X freq=%u)"
                        " -> col0 v0=(%.3f %.3f %.3f %.3f)%c",
                _a3->enabled, _a3->type, _a3->size, _a3->stride, _a3->offset,
                _a3->frequency,
                out[3].v[0], out[3].v[1], out[3].v[2], out[3].v[3], 10);
        const rsx_vertex_attrib* _a0 = &state->vertex_attribs[0];
        fprintf(stderr, "[DUCKVTX] first=%u count=%u a0(type=%u size=%u stride=%u off=0x%08X)"
                        " obj-bbox x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f]%c",
                first, count, _a0->type, _a0->size, _a0->stride, _a0->offset,
                mnx,mxx, mny,mxy, mnz,mxz, 10);
        /* Project the object-space bbox corners through the first four transform
         * constants (the conventional MVP rows) and report NDC. Sub-pixel output
         * with correct vertices means the placement transform is the problem, and
         * the NDC extent says by how much. */
        { float cx=(mnx+mxx)*0.5f, cy=(mny+mxy)*0.5f, cz=(mnz+mxz)*0.5f;
          /* This title's Cg programs keep their MVP at c[256..259], not c[0..3]
           * (see the vp_uses_c03 note); projecting through c0 reported zeros. */
          static int MVP = -1;
          PS3_KNOB(MVP, ps3_env_int("MVP_BASE", 256));
          float v[4] = { cx, cy, cz, 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[MVP + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          float ex[4];
          { float e[4] = { mxx-cx, mxy-cy, mxz-cz, 0.0f };
            for (int r = 0; r < 4; r++) {
                const float* m = state->vertex_constants[MVP + r];
                ex[r] = m[0]*e[0] + m[1]*e[1] + m[2]*e[2];
            } }
          { int lo = -1, hi = -1, nz = 0;
            for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
                const float* m = state->vertex_constants[r];
                if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
            }
            fprintf(stderr, "[DUCKVTX]   constants: %d non-zero, slots %d..%d;", nz, lo, hi);
            for (int r = (lo < 0 ? 0 : lo); r < (lo < 0 ? 0 : lo) + 4 && r < RSX_MAX_VERTEX_CONSTANTS; r++)
                fprintf(stderr, " c%d=(%.3f %.3f %.3f %.3f)", r,
                        state->vertex_constants[r][0], state->vertex_constants[r][1],
                        state->vertex_constants[r][2], state->vertex_constants[r][3]);
            fprintf(stderr, "%c", 10); }
          fprintf(stderr, "[DUCKVTX]   mvp0=(%.3f %.3f %.3f %.3f) clip=(%.3f %.3f %.3f w=%.3f)",
                  state->vertex_constants[MVP][0], state->vertex_constants[MVP][1],
                  state->vertex_constants[MVP][2], state->vertex_constants[MVP][3],
                  clip[0], clip[1], clip[2], clip[3]);
          if (clip[3] > 1e-6f)
              fprintf(stderr, " ndc=(%.4f %.4f) half-extent-px=(%.2f %.2f)",
                      clip[0]/clip[3], clip[1]/clip[3],
                      ex[0]/clip[3]*640.0f, ex[1]/clip[3]*360.0f);
          fprintf(stderr, "%c", 10); }
      } }
    /* IDX_DBG=<N>: the first indices and the position they resolve to. An index
     * buffer read with the wrong element size or base yields huge indices and
     * degenerate triangles -- an INDEXED mesh vanishes while non-indexed
     * geometry in the same scene renders fine. */
    { static int cap = -1, n = 0;
      PS3_KNOB(cap, ps3_env_int("IDX_DBG", 0));
      if (cap && n < cap && count >= 3) { n++;
        extern u32 cellGcmResolveOffset(u32);
        const rsx_vertex_attrib* a0 = &state->vertex_attribs[0];
        u32 vbase = cellGcmResolveOffset(a0->offset & 0x7FFFFFFFu);
        u32 nz = 0; for (u32 q = 0; q < 0x1000u; q += 29) if (vm_base[vbase + q]) nz++;
        fprintf(stderr, "[IDX] idxoff=0x%X idx: %u %u %u | a0 off=0x%08X(->0x%08X nz=%u/142)"
                        " stride=%u size=%u type=%u  pos0=(%g %g %g)%c",
                state->index_array_offset,
                read_guest_index(state, first), read_guest_index(state, first+1),
                read_guest_index(state, first+2),
                a0->offset, vbase, nz, a0->stride, a0->size, a0->type,
                out[0].v[0], out[0].v[1], out[0].v[2], 10); } }
    dbg_vtx_dump(state, out, first, count);
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Indexed TRIANGLE_STRIP / TRIANGLE_FAN, expanded CPU-side into a triangle
 * list (the whole VP path draws lists). The guest's face culling is honoured,
 * so strip parity winding matters. DeferredShading's spline tube meshes -- the
 * octopus tentacles -- are indexed strips. */
/* The last indexed upload's indices (count 0 = it expanded to a list), for
 * d3d12_draw_indexed's record. */
static u32 s_up_icount, s_up_ifirst;

static u32 upload_strip_vp_indexed(const rsx_state* state, u32 first, u32 count, int fan)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    /* VP_SLOW_DRAW_NS=<ns> (live): spin that long per strip upload. A causal
     * test for whether the FIFO thread's draw cost sets the frame rate. */
    { static int slow = -1; PS3_KNOB(slow, ps3_env_int("VP_SLOW_DRAW_NS", 0));
      if (slow > 0) { static long long fq; LARGE_INTEGER t0, t;
          if (!fq) { QueryPerformanceFrequency(&t); fq = t.QuadPart; }
          QueryPerformanceCounter(&t0);
          do { YieldProcessor(); QueryPerformanceCounter(&t); }
          while ((t.QuadPart - t0.QuadPart) * 1000000000LL / fq < slow); } }
    vp_attrs_dbg(state);
    vp_plan_begin(state);
    u32 maxv = (s_vp_cap * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * s_vp_cap * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    /* Expand to a triangle list, one index at a time:
     *  - Strip triangles alternate winding; every odd one is emitted with its
     *    first two vertices swapped (as the non-indexed path does), or culling
     *    removes half of a culled strip.
     *  - Primitive restart (SET_RESTART_INDEX_ENABLE): an index equal to the
     *    restart index ends the strip or fan, and the next one starts after
     *    it. Without it the restart index was fetched as a vertex (past the
     *    end of the array) and stretched triangles across the screen.
     *  - One BEGIN/END can carry several DRAW_INDEX_ARRAY ranges of at most
     *    256 indices, and the RSX assembles them as one stream. Demon's Souls'
     *    level meshes are strips split that way; restarting the strip at each
     *    range lost the two triangles across every boundary (holes in the
     *    tunnel ceiling). The strip state carries into the next contiguous
     *    range of the same BEGIN/END, as for triangle lists. */
    const int restart = state->restart_index_enable != 0;
    const u32 ridx = state->restart_index;
#define carry s_strip_carry
    u32 m = 0, p0 = 0, p1 = 0;   /* m: indices into the current strip; p0/p1: last two (fan: centre, last) */
    if (carry.valid && carry.fan == fan && carry.epoch == state->begin_epoch && carry.next == first) {
        m = carry.m; p0 = carry.p0; p1 = carry.p1;
    }
    u32 o = 0, need = 0;   /* need: vertices the strip expands to, room or not */
    const GuestIdx gi = guest_index_open(state);
    if (s_lv_open && count) {
        if (first < s_lv_klo) s_lv_klo = first;
        if (first + count - 1 > s_lv_khi) s_lv_khi = first + count - 1;
    }
    /* VP_INDEXED (live, default 1): write each of the draw's vertices once and
     * the triangle list as u32 indices into them. A Demon's Souls level strip
     * expands to ~2.5 list vertices per guest vertex, and at 256 bytes each
     * the expanded list was ~350 MB a frame through the write-combined upload
     * heap, then read back by the GPU. 0 = expand to an unindexed list. */
    static int indexed = -1;
    PS3_KNOB(indexed, ps3_env_int("VP_INDEXED", 1));
    /* Room for the most indices the strip can make, or the list path. */
    const int want_ix = indexed && s_vp_fast;
    if (want_ix && s_d3d.vp_ib_mapped && s_vp_icap - s_d3d.vp_ib_offset >= (u64)count * 3) {
        u32* ib = (u32*)s_d3d.vp_ib_mapped + (u64)s_d3d.vp_parity * s_vp_icap + s_d3d.vp_ib_offset;
        const u32 iroom = s_vp_icap - s_d3d.vp_ib_offset;
        const u32 icap = iroom - iroom % 3;
        u32 ni = 0, nmiss = 0;
        u32 tri[3];
        const int par = vpj_usable() && vpj_room(count);
        u32* vis = par ? s_vpj_vis + s_vpj_vis_n : NULL;
        for (u32 k = 0; k < count; k++) {
            const u32 ix = guest_index_at(&gi, first + k);
            if (restart && ix == ridx) { m = 0; continue; }
            if (m >= 2) need += 3;
            if (m >= 2 && ni + 3 <= icap && o + 3 <= maxv) {
                tri[0] = p0; tri[1] = p1; tri[2] = ix;
                if (!fan && ((m - 2) & 1)) { tri[0] = p1; tri[1] = p0; }
                for (int t = 0; t < 3; t++) {
                    const u32 vi = tri[t];
                    const u32 h = (vi ^ (vi >> 8)) & (VP_VC_N - 1);
                    if (s_vx_tag[h] != s_vc_gen || s_vx_key[h] != vi) {
                        if (par) vis[o] = vi;
                        else     read_vp_vertex(state, vi, &out[o*16]);
                        s_vx_key[h] = vi; s_vx_tag[h] = s_vc_gen; s_vx_out[h] = s_strip_base + o++;
                        nmiss++;
                    }
                    ib[ni++] = s_vx_out[h];
                }
            }
            if (fan) { if (m == 0) p0 = ix; else p1 = ix; }
            else     { p0 = p1; p1 = ix; }
            m++;
        }
        carry.valid = 1; carry.fan = fan; carry.epoch = state->begin_epoch;
        carry.next = first + count; carry.m = m; carry.p0 = p0; carry.p1 = p1;
        s_vp_want += nmiss;
        s_vp_iwant += need;
        if (par && o) { vpj_queue(vis, o, out); s_vs_fetch += o; s_vs_attr += (u64)o * s_vp_plan.n_on; }
        if (!par) dbg_vtx_dump(state, out, first, o);
        s_up_icount = ni; s_up_ifirst = s_d3d.vp_ib_offset;
        s_d3d.vp_ib_offset += ni;
        s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
        return ni ? o : 0;
    }
    const u32 cap = maxv - maxv % 3;
    for (u32 k = 0; k < count; k++) {
        const u32 ix = guest_index_at(&gi, first + k);
        if (restart && ix == ridx) { m = 0; continue; }
        if (m >= 2) need += 3;
        if (m >= 2 && o + 3 <= cap) {
            u32 v0 = p0, v1 = p1;
            if (!fan && ((m - 2) & 1)) { v0 = p1; v1 = p0; }
            read_vp_vertex_c(state, v0, &out[o*16]); o++;
            read_vp_vertex_c(state, v1, &out[o*16]); o++;
            read_vp_vertex_c(state, ix, &out[o*16]); o++;
        }
        if (fan) { if (m == 0) p0 = ix; else p1 = ix; }
        else     { p0 = p1; p1 = ix; }
        m++;
    }
    carry.valid = 1; carry.fan = fan; carry.epoch = state->begin_epoch;
    carry.next = first + count; carry.m = m; carry.p0 = p0; carry.p1 = p1;
    if (want_ix) s_vp_iwant += need;   /* so the index buffer grows to fit */
    /* What the buffer grows to: (count-2)*3 counted the strip as unbroken,
     * twice the vertices a Demon's Souls frame emits once restarts split its
     * level strips, and grew the buffer to the 2M-vertex cap for nothing. */
    s_vp_want += need;
    { static int said = 0;
      if (restart && !said) { said = 1;
          fprintf(stderr, "[D3D12] primitive restart in use (index 0x%X)%c", ridx, 10); } }
    dbg_vtx_dump(state, out, first, o);
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
#undef carry
}
/* GPU vertex fetch (VP_GPU_FETCH, live, default 1). The CPU path above walks
 * a strip, numbers its vertices, converts each one to 16 float4s (256 bytes)
 * and writes them to vp_vb; in a Demon's Souls level frame that conversion
 * and its writes are most of the FIFO thread. Here the strip becomes a
 * triangle list of the guest's own indices, the guest bytes the indices
 * reach are copied as they are into vf_buf, and the vertex shader
 * (rsx_vp_decompile_fetch) reads and converts them itself.
 *
 * vf_buf's half for this frame holds, per descriptor: 16 attribute records
 * (32 bytes each, the layout rsx_vp_decompile_fetch documents), then the
 * guest byte spans they point into. A record's x is where vertex 0's
 * element would sit, so one descriptor serves every range of a strip whose
 * bytes it holds; a range reaching past the last span appends to it when
 * nothing was written after it. */
static u32 s_up_fdesc;
static u64 s_vf_stat[6];   /* new, reuse, append, sparse, type/addr, room */

static int vf_on(void)
{
    static int en = -1;
    PS3_KNOB(en, ps3_env_int("VP_GPU_FETCH", 1));
    return en && s_d3d.vf_mapped && s_vf_cap && s_vp_fast && !late_on();
}

/* The guest address of the main-memory bytes [a,b), or ~0 when they are not
 * one linear run (IO pages are 1 MB). */
static u32 vf_resolve(const rsx_fetch_slot* sl, u64 a, u64 b)
{
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (!sl->main) return sl->local_addr + (u32)a;
    const u32 ga = cellGcmResolveLocated(0, (u32)a);
    if (ga == 0xFFFFFFFFu) return ga;
    for (u64 x = (a | 0xFFFFFu) + 1; x < b; x += 0x100000u)
        if (cellGcmResolveLocated(0, (u32)x) != ga + (u32)(x - a)) return 0xFFFFFFFFu;
    return ga;
}

/* A strip's triangle list from the guest's big-endian indices, for
 * upload_strip_vp_fetch: the same list, carry and [lo,hi] as its general
 * loop, with the index width a constant and the winding taken from m's low
 * bit. lo/hi cover every index a triangle uses: each new index, and p0/p1 at
 * the first triangle of a run or of the call (the indices before it, or the
 * carry); every later triangle's other two indices were new indices already.
 *
 * A Palace frame converts several hundred thousand strip indices on the FIFO
 * thread, and the general loop (per-index width test, fan test, six
 * compares) was 16% of that thread. Once the FIFO thread is on the frame's
 * path, as it is at ~105 fps, this is worth 100.2 -> 106.2 fps. Checked
 * against the general loop over 6.8 million draws with no difference. */
static __forceinline u32 vf_strip_list_w(const u8* src, const int w16, u32 count,
                                         const int restart, const u32 ridx, u32* pm,
                                         u32* pp0, u32* pp1, u32* ib, u32* plo, u32* phi)
{
    u32 m = *pm, p0 = *pp0, p1 = *pp1, lo = *plo, hi = *phi, ni = 0;
    for (u32 k = 0; k < count; k++) {
        u32 ix;
        if (w16) { u16 v; memcpy(&v, src + (u64)k * 2, 2); ix = __builtin_bswap16(v); }
        else     { u32 v; memcpy(&v, src + (u64)k * 4, 4); ix = __builtin_bswap32(v); }
        if (restart && ix == ridx) { m = 0; continue; }
        if (m >= 2) {
            const u32 odd = m & 1u;   /* (m - 2) & 1 */
            ib[ni] = odd ? p1 : p0; ib[ni + 1] = odd ? p0 : p1; ib[ni + 2] = ix; ni += 3;
            if (m == 2 || ni == 3) {
                if (p0 < lo) lo = p0; if (p0 > hi) hi = p0;
                if (p1 < lo) lo = p1; if (p1 > hi) hi = p1;
            }
            lo = ix < lo ? ix : lo; hi = ix > hi ? ix : hi;
        }
        p0 = p1; p1 = ix; m++;
    }
    *pm = m; *pp0 = p0; *pp1 = p1; *plo = lo; *phi = hi;
    return ni;
}
static u32 vf_strip_list(const GuestIdx* gi, u32 first, u32 count, int restart, u32 ridx,
                         u32* pm, u32* pp0, u32* pp1, u32* ib, u32* plo, u32* phi)
{
    if (gi->w16) return vf_strip_list_w(gi->p + (u64)first * 2, 1, count, restart, ridx,
                                        pm, pp0, pp1, ib, plo, phi);
    return vf_strip_list_w(gi->p + (u64)first * 4, 0, count, restart, ridx,
                           pm, pp0, pp1, ib, plo, phi);
}

/* The strip as a strip (VF_NATIVE_STRIP, live, default 1): the draw is a
 * TRIANGLESTRIP and the guest's indices go into the index buffer as they are,
 * byte-swapped, with the restart index as D3D12's cut value (every VP PSO
 * sets IBStripCutValue to it). One index in, one out, no branches, against
 * the list's three out and a winding choice per triangle: the list walk was
 * ~15% of the FIFO thread at ~109 fps, with that thread on the frame's path.
 *
 * A range starts with a cut and then restarts the strip it continues from
 * the carry, with the winding parity the list would have used (triangle i
 * of a strip is (i, i+1, i+2) when i is even and (i+1, i, i+2) when odd):
 *   m == 1          -> p1
 *   m even          -> p0 p1
 *   m odd, m >= 3   -> p0 p0 p1   (one degenerate triangle shifts the parity)
 * That holds whether the range opens a draw or extends the last one. The
 * carry going out comes from the guest's last indices.
 *
 * Palace, same run: 108.9 -> 112.7 fps. Checked against the list walk, as
 * triangles with degenerates dropped, over 26 million ranges (Tutorial and
 * Palace) with no difference. VF_NATIVE_STRIP=0 goes back to the list. */
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif
#define VF_CUT 0xFFFFFFFFu
static __forceinline u32 vf_ix(const u8* src, const int w16, u32 k)
{
    if (w16) { u16 v; memcpy(&v, src + (u64)k * 2, 2); return __builtin_bswap16(v); }
    u32 v; memcpy(&v, src + (u64)k * 4, 4); return __builtin_bswap32(v);
}
static __forceinline u32 vf_strip_native_w(const u8* src, const int w16, u32 count,
                                           const int restart, const u32 ridx, u32* pm,
                                           u32* pp0, u32* pp1, u32* ib, u32* plo, u32* phi)
{
    u32 m = *pm, p0 = *pp0, p1 = *pp1, lo = *plo, hi = *phi, n = 0;
    ib[n++] = VF_CUT;
    if (m >= 1) {
        if (m >= 3 && (m & 1u)) ib[n++] = p0;
        if (m >= 2) { ib[n++] = p0; lo = p0 < lo ? p0 : lo; hi = p0 > hi ? p0 : hi; }
        ib[n++] = p1; lo = p1 < lo ? p1 : lo; hi = p1 > hi ? p1 : hi;
    }
    /* ib is an upload heap (write-combined): written in order, never read
     * back. A backward scan of it for the last restart was 29% of the FIFO
     * thread, so the loop keeps r, and the tail comes from the guest's bytes. */
    u32* b = ib + n;
    u32 r = 0;   /* where the strip after the range's last restart begins (0: none) */
    u32 k = 0;
#if defined(_M_X64) || defined(__x86_64__)
    /* 16-bit indices eight at a time, the same outputs as the loop below:
     * byte swap, restart lanes to the cut value, unsigned min/max over the
     * other lanes (SSE2 has only the signed ones, hence the 0x8000 bias),
     * and the last restart from the compare mask. The scalar loop was ~10%
     * of the FIFO thread, a cmov chain per index. */
    static int sse = -1;
    PS3_KNOB(sse, ps3_env_int("VF_STRIP_SSE", 1));
    if (w16 && count >= 8 && sse) {
        const __m128i bias = _mm_set1_epi16((short)0x8000);
        const __m128i rv = _mm_set1_epi16((short)ridx);
        const int rs = restart && ridx <= 0xFFFFu;
        __m128i vlo = _mm_set1_epi16(0x7FFF), vhi = _mm_set1_epi16((short)0x8000);
        __m128i real = _mm_setzero_si128();
        for (; k + 8 <= count; k += 8) {
            __m128i v = _mm_loadu_si128((const __m128i*)(src + (u64)k * 2));
            v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
            const __m128i c = rs ? _mm_cmpeq_epi16(v, rv) : _mm_setzero_si128();
            const __m128i o = _mm_or_si128(v, c);   /* restart lanes 0xFFFF */
            _mm_storeu_si128((__m128i*)(b + k), _mm_unpacklo_epi16(o, c));
            _mm_storeu_si128((__m128i*)(b + k + 4), _mm_unpackhi_epi16(o, c));
            vlo = _mm_min_epi16(vlo, _mm_xor_si128(o, bias));
            vhi = _mm_max_epi16(vhi, _mm_xor_si128(_mm_andnot_si128(c, v), bias));
            real = _mm_or_si128(real, _mm_xor_si128(c, _mm_set1_epi32(-1)));
            const int mk = _mm_movemask_epi8(c);
            if (mk) { unsigned long top; _BitScanReverse(&top, (unsigned long)mk);
                      r = k + (u32)(top >> 1) + 1; }
        }
        if (_mm_movemask_epi8(real)) {
            u16 l8[8], h8[8];
            _mm_storeu_si128((__m128i*)l8, _mm_xor_si128(vlo, bias));
            _mm_storeu_si128((__m128i*)h8, _mm_xor_si128(vhi, bias));
            for (int j = 0; j < 8; j++) {
                lo = l8[j] < lo ? l8[j] : lo; hi = h8[j] > hi ? h8[j] : hi;
            }
        }
    }
#endif
#define VF_IX(k) vf_ix(src, w16, k)
    for (; k < count; k++) {
        const u32 ix = VF_IX(k);
        const int c = restart && ix == ridx;
        b[k] = c ? VF_CUT : ix;
        const u32 vl = c ? 0xFFFFFFFFu : ix, vh = c ? 0u : ix;
        lo = vl < lo ? vl : lo; hi = vh > hi ? vh : hi;
        r = c ? k + 1 : r;
    }
    const u32 tail = count - r;
    m = r ? tail : m + count;
    if (tail >= 2)      { p0 = VF_IX(count - 2); p1 = VF_IX(count - 1); }
    else if (tail == 1) { p0 = p1; p1 = VF_IX(count - 1); }
#undef VF_IX
    *pm = m; *pp0 = p0; *pp1 = p1; *plo = lo; *phi = hi;
    return n + count;
}
static u32 vf_strip_native(const GuestIdx* gi, u32 first, u32 count, int restart, u32 ridx,
                           u32* pm, u32* pp0, u32* pp1, u32* ib, u32* plo, u32* phi)
{
    if (gi->w16) return vf_strip_native_w(gi->p + (u64)first * 2, 1, count, restart, ridx,
                                          pm, pp0, pp1, ib, plo, phi);
    return vf_strip_native_w(gi->p + (u64)first * 4, 0, count, restart, ridx,
                             pm, pp0, pp1, ib, plo, phi);
}
static int s_up_strip;   /* the last upload_strip_vp_fetch wrote a strip */

/* Returns the indices written (0 is a strip that made no triangle), or -1
 * when the draw has to take the CPU path. ext: the fetch draw this range
 * continues, or NULL. */
static int upload_strip_vp_fetch(const rsx_state* state, u32 first, u32 count, int fan,
                                 const D3D12DrawRecord* ext)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_ib_mapped || !state->vertex_attribs[0].enabled) return -1;
    if ((u64)s_vp_icap - s_d3d.vp_ib_offset < (u64)count * 3 + 4) { s_vf_stat[5]++; return -1; }
    static rsx_fetch_plan pl;
    rsx_fetch_plan_init(state, &pl);
    for (u32 k = 0; k < pl.n_on; k++) {
        const u32 t = pl.on[k].type;
        if (t < 1 || t > 7 || t == 6) { s_vf_stat[4]++; return -1; }
    }
    /* The list, straight into the index buffer; it only counts once the
     * draw is accepted (vp_ib_offset, the carry). */
    const int restart = state->restart_index_enable != 0;
    const u32 ridx = state->restart_index;
    u32 m = 0, p0 = 0, p1 = 0;
    if (s_strip_carry.valid && s_strip_carry.fan == fan &&
        s_strip_carry.epoch == state->begin_epoch && s_strip_carry.next == first) {
        m = s_strip_carry.m; p0 = s_strip_carry.p0; p1 = s_strip_carry.p1;
    }
    u32* ib = (u32*)s_d3d.vp_ib_mapped + (u64)s_d3d.vp_parity * s_vp_icap + s_d3d.vp_ib_offset;
    const GuestIdx gi = guest_index_open(state);
    u32 ni = 0, lo = 0xFFFFFFFFu, hi = 0;
    static int nat = -1;
    PS3_KNOB(nat, ps3_env_int("VF_NATIVE_STRIP", 1));
    s_up_strip = !fan && nat;
    if (s_up_strip) {
        ni = vf_strip_native(&gi, first, count, restart, ridx, &m, &p0, &p1, ib, &lo, &hi);
        if (lo > hi) ni = 0;   /* restarts only: no vertex, so no triangle */
    }
    else if (!fan) ni = vf_strip_list(&gi, first, count, restart, ridx, &m, &p0, &p1, ib, &lo, &hi);
    else for (u32 k = 0; k < count; k++) {
        const u32 ix = guest_index_at(&gi, first + k);
        if (restart && ix == ridx) { m = 0; continue; }
        if (m >= 2) {
            ib[ni] = p0; ib[ni + 1] = p1; ib[ni + 2] = ix; ni += 3;
            if (p0 < lo) lo = p0; if (p0 > hi) hi = p0;
            if (p1 < lo) lo = p1; if (p1 > hi) hi = p1;
            if (ix < lo) lo = ix; if (ix > hi) hi = ix;
        }
        if (m == 0) p0 = ix; else p1 = ix;
        m++;
    }
    if (ni) {
        /* Each array's bytes over [lo,hi]. A range that reaches far more
         * vertices than it has indices copies more than the CPU path
         * converts: leave it to that path. */
        if ((u64)hi - lo + 1 > (u64)count * 4 + 256) { s_vf_stat[3]++; return -1; }
        u32 sg0[16], sg1[16], sx[16];
        for (u32 k = 0; k < pl.n_on; k++) {
            const rsx_fetch_slot* sl = &pl.on[k];
            u32 elo = lo, ehi = hi;
            if (sl->freq) { if (sl->modulo) { elo = 0; ehi = sl->freq - 1; }
                            else { elo /= sl->freq; ehi /= sl->freq; } }
            const u64 a = (u64)sl->off + (u64)elo * sl->stride;
            const u64 b = (u64)sl->off + (u64)ehi * sl->stride + lv_elem_bytes(sl->type, sl->n);
            if (b - a > (16u << 20)) { s_vf_stat[3]++; return -1; }
            const u32 ga = vf_resolve(sl, a, b);
            if (ga == 0xFFFFFFFFu || (u64)ga + (b - a) > 0xFFFFFFFFull) { s_vf_stat[4]++; return -1; }
            sg0[k] = ga; sg1[k] = ga + (u32)(b - a);
            sx[k] = elo * sl->stride;   /* x = ring(ga) - sx */
        }
        int done = 0;
        /* The draw being extended: its descriptor holds these bytes, or
         * can take them by appending to its one span. */
        if (ext && s_fx.valid && s_fx.fdesc == ext->fdesc && s_fx.epoch == state->begin_epoch) {
            u32 ng1 = 0; int ok = 1;
            for (u32 k = 0; k < pl.n_on && ok; k++) {
                int in = 0;
                for (u32 j = 0; j < s_fx.nsp; j++) {
                    const int last = (j + 1 == s_fx.nsp) && s_fx.end == s_vf_off;
                    if (sg0[k] >= s_fx.g0[j] && (sg1[k] <= s_fx.g1[j] || (last && sg0[k] <= s_fx.g1[j]))) {
                        if (s_fx.r0[j] + (sg0[k] - s_fx.g0[j]) - sx[k] != s_fx.x[pl.on[k].idx]) break;
                        if (sg1[k] > s_fx.g1[j] && sg1[k] > ng1) ng1 = sg1[k];
                        in = 1; break;
                    }
                }
                ok = in;
            }
            if (ok && ng1) {
                const u32 j = s_fx.nsp - 1;
                const u32 g1 = (ng1 + 3u) & ~3u, add = g1 - s_fx.g1[j];
                if ((u64)s_vf_off + add <= s_vf_cap) {
                    memcpy((u8*)s_d3d.vf_mapped + (u64)s_d3d.vp_parity * s_vf_cap + s_vf_off,
                           vm_base + s_fx.g1[j], add);
                    s_vf_off += add; s_fx.end = s_vf_off; s_fx.g1[j] = g1;
                    s_vf_stat[2]++;
                } else ok = 0;
            } else if (ok) s_vf_stat[1]++;
            done = ok;
        }
        if (!done) {
            /* A new descriptor: the spans, merged where they touch (an
             * interleaved array is one span for all its attributes). */
            u32 ord[16], n = pl.n_on;
            for (u32 k = 0; k < n; k++) ord[k] = k;
            for (u32 i = 1; i < n; i++)
                for (u32 j = i; j > 0 && sg0[ord[j]] < sg0[ord[j - 1]]; j--)
                    { const u32 t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t; }
            u32 g0[16], g1[16], nsp = 0;
            for (u32 i = 0; i < n; i++) {
                const u32 a = sg0[ord[i]] & ~3u, b = (sg1[ord[i]] + 3u) & ~3u;
                if (nsp && a <= g1[nsp - 1] + 256u) { if (b > g1[nsp - 1]) g1[nsp - 1] = b; }
                else { g0[nsp] = a; g1[nsp] = b; nsp++; }
            }
            u64 total = 512;
            for (u32 j = 0; j < nsp; j++) total += g1[j] - g0[j];
            if ((u64)s_vf_off + total > s_vf_cap) { s_vf_stat[5]++; return -1; }
            u8* base = (u8*)s_d3d.vf_mapped + (u64)s_d3d.vp_parity * s_vf_cap;
            const u32 fdesc = s_vf_off;
            u32 r = fdesc + 512;
            for (u32 j = 0; j < nsp; j++) {
                memcpy(base + r, vm_base + g0[j], g1[j] - g0[j]);
                s_fx.g0[j] = g0[j]; s_fx.g1[j] = g1[j]; s_fx.r0[j] = r;
                r += g1[j] - g0[j];
            }
            struct { u32 x, y, z, w; float c[4]; } rec[16];
            for (u32 i = 0; i < 16; i++) {
                rec[i].x = rec[i].y = rec[i].z = rec[i].w = 0;
                memcpy(rec[i].c, pl.konst[i], 16);
                s_fx.x[i] = 0;
            }
            for (u32 k = 0; k < n; k++) {
                const rsx_fetch_slot* sl = &pl.on[k];
                u32 j = 0;
                while (j + 1 < nsp && !(sg0[k] >= g0[j] && sg0[k] < g1[j])) j++;
                const u32 x = s_fx.r0[j] + (sg0[k] - g0[j]) - sx[k];
                rec[sl->idx].x = x; rec[sl->idx].y = sl->stride;
                rec[sl->idx].z = sl->type | (sl->n << 4)
                               | ((sl->freq ? (sl->modulo ? 2u : 1u) : 0u) << 8);
                rec[sl->idx].w = sl->freq ? sl->freq : 1;
                s_fx.x[sl->idx] = x;
            }
            memcpy(base + fdesc, rec, sizeof rec);
            s_vf_off = r;
            s_fx.valid = 1; s_fx.epoch = state->begin_epoch; s_fx.fdesc = fdesc;
            s_fx.end = r; s_fx.nsp = nsp;
            s_vf_stat[0]++;
        }
        s_up_fdesc = s_fx.fdesc;
    }
    s_strip_carry.valid = 1; s_strip_carry.fan = fan; s_strip_carry.epoch = state->begin_epoch;
    s_strip_carry.next = first + count; s_strip_carry.m = m;
    s_strip_carry.p0 = p0; s_strip_carry.p1 = p1;
    s_vp_iwant += ni;
    s_up_icount = ni; s_up_ifirst = s_d3d.vp_ib_offset;
    s_d3d.vp_ib_offset += ni;
    { static int st = -1; static u32 calls;
      PS3_KNOB(st, ps3_env_int("VP_FETCH_STATS", 0));
      if (st && ++calls % 20000u == 0)
          fprintf(stderr, "[VF] new %llu reuse %llu append %llu | cpu: sparse %llu type/addr %llu room %llu | ring %u KB%c",
                  (unsigned long long)s_vf_stat[0], (unsigned long long)s_vf_stat[1],
                  (unsigned long long)s_vf_stat[2], (unsigned long long)s_vf_stat[3],
                  (unsigned long long)s_vf_stat[4], (unsigned long long)s_vf_stat[5],
                  s_vf_off >> 10, 10); }
    return (int)ni;
}


static void d3d12_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    /* RSX_LIVE_FEED_DBG=1: pair with the [feed] method counts so "the FIFO
     * carries no draw batches" can be checked against "the backend is told
     * to draw" in the same run. */
    { static int dbg = -1; PS3_KNOB(dbg, getenv("RSX_LIVE_FEED_DBG") ? 1 : 0);
      if (dbg) { static long long n = 0;
          if (((++n) % 20000) == 0)
              printf("[backend] arrays callbacks=%lld (prim=%u count=%u)\n", n, primitive, count), fflush(stdout); } }
    (void)ud;
    /* Log the first 20 calls in detail, then every 1000th to show liveness
     * without flooding. */
    static u64 s_total = 0;
    if (s_total < 20 || (s_total % 1000) == 0) {
        printf("[D3D12] draw_arrays #%llu prim=%u first=%u count=%u\n",
               (unsigned long long)s_total, primitive, first, count);
    }
    s_total++;

    if (!s_d3d.pipeline_ready || !s_d3d.vb_mapped) return;
    if (count == 0 || count > MAX_VERTICES) return;

    /* QUADS (prim 8) have no D3D12 topology; expand to a triangle list.
     * dbgfont draws all text as quads. Prefer the real vertex-program path
     * (exact position + texcoord): upload raw float4 attrib0 into vp_vb and
     * mark the draw is_vp; render_frame compiles the VP and draws it. Fall
     * back to the frac-approximation textured path if VP resources are absent. */
    if (primitive == RSX_PRIMITIVE_QUADS) {
        /* Prefer the real vertex-program path whenever VP resources exist --
         * NOT only when a texture is bound. Requiring tex_bound routed vkcube's
         * UNtextured cube to the fixed-function fallback, which never applies the
         * VP's MVP transform, so the cube was drawn in object space and clipped
         * off-screen. Untextured VP draws now render via the colour PSO. */
        if (s_d3d.vp_vb_mapped && s_d3d.vp_root_sig) {
            u32 rec = s_d3d.vp_vb_offset;
            u32 emitted = upload_quads_vp(s_d3d.current_rsx_state, first, count);
            if (emitted && draw_room()) {
                D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
                dr->vb_byte_offset = rec; dr->index_count = 0;
                dr->vertex_count   = emitted;
                dr->topology       = RSX_TOPOLOGY_TRIANGLES;
                dr->textured       = s_d3d.tex_bound;
                dr->is_vp          = 1;
                dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
                dr->fp_exp32 = s_d3d.current_rsx_state ?
                    ((s_d3d.current_rsx_state->shader_control & 0x40) != 0) : 1;
                dr->cull = rsx_cull_key(s_d3d.current_rsx_state);
                dr->depth = rsx_depth_key(s_d3d.current_rsx_state);
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
                dr->cmask = 0xF;
                if (s_d3d.current_rsx_state) {
                    u32 _cm = s_d3d.current_rsx_state->color_mask;
                    dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                              | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                              | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                              | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
                }
                dr->alpha_ctl = 0;
                if (s_d3d.current_rsx_state)
                    dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                                  | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                                  | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
                for (int _u = 0; _u < RSX_FP_UNITS; _u++) {
                    dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                    dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                    dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                    dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                    dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1;
                    dr->tex[_u].addr  = s_d3d.cur_texs[_u].addr;
                    dr->tex[_u].filt  = s_d3d.cur_texs[_u].filt;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
                    dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                    dr->tex_rt[_u]  = -1;
                }
                dr->tex_slot = -1;
                dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
                dr->fetch = 0; dr->fdesc = 0;
                dr->is_clear = 0;
                /* Records are reused frame to frame without a reset: a slot that
                 * held a depth clear kept its Z/S flags, and a soft-particle draw in
                 * it lost its read-only depth view (FP_WATCH: "reads 0"). */
                dr->clr_flags = 0;
                dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
                dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
                if (getenv("BLENDDBG")) { static u32 seen[32]; static int ns=0;
                    u32 k = (dr->blend ? 0x80000000u : 0u) | dr->blend_key;
                    int f=0; for (int i=0;i<ns;i++) if (seen[i]==k) f=1;
                    if (!f && ns<32) { seen[ns++]=k;
                        fprintf(stderr, "[BLEND] enable=%d key=0x%X%c",
                                dr->blend, dr->blend_key, 10); } }
                dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt);
                dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
                rt_pitch_record(dr);
                if (s_d3d.current_rsx_state) {
                    dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                    dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                    dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                    dr->vp_h = s_d3d.current_rsx_state->viewport_h;
                } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
                dr->cb_slot = s_d3d.draw_count;
                dr->vcb_slot = vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
                s_d3d.draw_count++;
            }
            return;
        }
        u32 record_offset = s_d3d.vb_offset;
        u32 emitted = upload_quads_from_rsx(first, count);
        if (emitted == 0) return;
        if (draw_room()) {
            s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
            s_d3d.draws[s_d3d.draw_count].index_count = 0;
            s_d3d.draws[s_d3d.draw_count].vertex_count   = emitted;
            s_d3d.draws[s_d3d.draw_count].topology       = RSX_TOPOLOGY_TRIANGLES;
            s_d3d.draws[s_d3d.draw_count].textured       = s_d3d.tex_bound;
            s_d3d.draws[s_d3d.draw_count].is_vp          = 0;
            s_d3d.draws[s_d3d.draw_count].is_clear       = 0;
            s_d3d.draws[s_d3d.draw_count].clr_flags      = 0;
            s_d3d.draws[s_d3d.draw_count].rt_off         = 0;
            s_d3d.draw_count++;
        }
        return;
    }

    /* TRIANGLES (prim 5): same VP-path preference as quads -- the guest's
     * vertex program does the MVP transform (gcm/cube draws its cube this way);
     * the fixed-function fallback below applies no transform, so 3D geometry
     * ends up in object space (invisible/garbage). */
    /* Route TRIANGLES/STRIP/FAN through the VP path (sagemono: LBP draws
     * non-indexed strips/fans), but ONLY when the guest ACTUALLY HAS a vertex
     * program (flOw). The VP path transforms via the guest's VP microcode; with
     * no microcode loaded it transforms nothing and the draw silently produces
     * zero pixels (it also uploads to vp_vb, leaving the fixed-function vb empty).
     * Geometry already in clip space with no VP -- flOw's injected scene -- must
     * go down the fixed-function passthrough below instead. */
    if ((primitive == RSX_PRIMITIVE_TRIANGLES || primitive == RSX_PRIMITIVE_TRIANGLE_STRIP
         || primitive == RSX_PRIMITIVE_TRIANGLE_FAN) &&
        s_d3d.vp_vb_mapped && s_d3d.vp_root_sig &&
        s_d3d.current_rsx_state && s_d3d.current_rsx_state->vp_ucode_bytes >= 16) {
        u32 rec = s_d3d.vp_vb_offset;
        u32 emitted = (primitive == RSX_PRIMITIVE_TRIANGLES)
            ? upload_tris_vp(s_d3d.current_rsx_state, first, count)
            : upload_strip_vp(s_d3d.current_rsx_state, first, count, primitive == RSX_PRIMITIVE_TRIANGLE_FAN);
        /* RSX splits one primitive stream across several DRAW_ARRAYS entries
         * inside a single BEGIN/END (each carries at most 256 vertices), and the
         * hardware concatenates them before assembling primitives. Recording a
         * draw per batch instead regroups the triangles: this title's tub mesh
         * arrives as 128 then 256,256,..., neither a multiple of 3, so every
         * batch after the first was assembled one vertex out of phase -- which
         * rendered as spike triangles with a zero texcoord along the tub rim.
         * The batches land contiguously in the vertex buffer, so extending the
         * previous record is all that is needed to restore the stream. */
        if (emitted && s_d3d.merge_prev_draw && s_d3d.draw_count > 0 &&
            s_d3d.merge_first_end == first) {
            D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
            u32 fpnow = s_d3d.current_rsx_state
                        ? s_d3d.current_rsx_state->shader_program : 0;
            if (pv->is_vp && pv->topology == RSX_TOPOLOGY_TRIANGLES &&
                pv->fp_addr == fpnow && primitive == RSX_PRIMITIVE_TRIANGLES &&
                pv->begin_epoch == (s_d3d.current_rsx_state
                                    ? s_d3d.current_rsx_state->begin_epoch : 0) &&
                pv->index_count == 0 &&
            pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec) {
                pv->vertex_count += emitted;
                s_d3d.merge_first_end = first + count;
                return;
            }
        }
        if (emitted && draw_room()) {
            D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
            dr->vb_byte_offset = rec; dr->index_count = 0;
            dr->vertex_count   = emitted;
            dr->topology       = RSX_TOPOLOGY_TRIANGLES;
            dr->textured       = s_d3d.tex_bound;
            dr->is_vp          = 1;
            dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
            dr->fp_exp32 = s_d3d.current_rsx_state ?
                ((s_d3d.current_rsx_state->shader_control & 0x40) != 0) : 1;
            dr->cull = rsx_cull_key(s_d3d.current_rsx_state);
            dr->depth = rsx_depth_key(s_d3d.current_rsx_state);
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->cmask = 0xF;
            if (s_d3d.current_rsx_state) {
                u32 _cm = s_d3d.current_rsx_state->color_mask;
                dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                          | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                          | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                          | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
            }
            dr->alpha_ctl = 0;
            if (s_d3d.current_rsx_state)
                dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                              | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                              | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
            for (int _u = 0; _u < RSX_FP_UNITS; _u++) {
                dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1;
                    dr->tex[_u].addr  = s_d3d.cur_texs[_u].addr;
                    dr->tex[_u].filt  = s_d3d.cur_texs[_u].filt;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
                dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                dr->tex_rt[_u]  = -1;
            }
            dr->tex_slot = -1;
            dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
            dr->fetch = 0; dr->fdesc = 0;
            dr->is_clear = 0;
            dr->clr_flags = 0;
            dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
            dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
            dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt);
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            rt_pitch_record(dr);
            if (s_d3d.current_rsx_state) {
                dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                dr->vp_h = s_d3d.current_rsx_state->viewport_h;
            } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
            dr->cb_slot = s_d3d.draw_count;
                dr->vcb_slot = vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
            s_d3d.draw_count++;
            /* Anchor for merging the next DRAW_ARRAYS batch of this stream. */
            s_d3d.merge_prev_draw  = (primitive == RSX_PRIMITIVE_TRIANGLES);
            s_d3d.merge_strip      = 0;
            s_d3d.merge_first_end  = first + count;
        }
        return;
    }
    s_d3d.merge_prev_draw = 0;   /* any other primitive breaks the stream */
    s_d3d.merge_strip = 0;

    u32 topo = (u32)rsx_primitive_topology(primitive);
    if (topo == RSX_TOPOLOGY_UNSUPPORTED) {
        /* Other primitives still needing index-buffer conversion
         * (line loops, triangle fans) — skip rather than draw wrong. */
        static int s_skipped_nontri = 0;
        if (s_skipped_nontri < 3) {
            printf("[D3D12] draw_arrays: skipping prim=%u (needs index conversion)\n",
                   primitive);
            s_skipped_nontri++;
        }
        return;
    }

    u32 record_offset = s_d3d.vb_offset;
    u32 actual_count  = upload_vertices_from_rsx(first, count);
    if (actual_count == 0) return;

    if (draw_room()) {
        s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
            s_d3d.draws[s_d3d.draw_count].index_count = 0;
        s_d3d.draws[s_d3d.draw_count].vertex_count   = actual_count;
        s_d3d.draws[s_d3d.draw_count].topology       = topo;
        s_d3d.draws[s_d3d.draw_count].textured       = 0;
        s_d3d.draws[s_d3d.draw_count].is_vp          = 0;
        s_d3d.draws[s_d3d.draw_count].is_clear       = 0;
        s_d3d.draws[s_d3d.draw_count].clr_flags      = 0;
        s_d3d.draws[s_d3d.draw_count].rt_off         = 0;
        s_d3d.draw_count++;
    }
}

static void d3d12_draw_indexed(void* ud, u32 primitive, u32 first, u32 count)
{
    /* RSX_LIVE_FEED_DBG=1: pair with the [feed] method counts so "the FIFO
     * carries no draw batches" can be checked against "the backend is told
     * to draw" in the same run. */
    { static int dbg = -1; PS3_KNOB(dbg, getenv("RSX_LIVE_FEED_DBG") ? 1 : 0);
      if (dbg) { static long long n = 0;
          if (((++n) % 20000) == 0)
              printf("[backend] indexed callbacks=%lld (prim=%u count=%u)\n", n, primitive, count), fflush(stdout); } }
    (void)ud;
    static int log_count = 0;
    if (log_count < 8) {
        printf("[D3D12] draw_indexed(prim=%u, first=%u, count=%u)\n",
               primitive, first, count);
        log_count++;
    }
    if (!s_d3d.pipeline_ready) return;
    if (count == 0 || count > MAX_VERTICES) return;
    if (!s_d3d.vp_vb_mapped || !s_d3d.vp_root_sig) return;

    /* Expand through the VP path (indices resolved CPU-side): QUADS -> two
     * triangles per quad, TRIANGLES straight through, STRIP/FAN -> triangle
     * list. Other primitives are skipped rather than drawn wrong. */
    u32 emitted = 0;
    u32 rec = s_d3d.vp_vb_offset;
    s_up_icount = 0;
    /* A strip split over several DRAW_INDEX_ARRAY ranges of one BEGIN/END
     * (Demon's Souls' level meshes, 256 indices each) was a draw per range,
     * each paying the whole per-draw cost here and in render_frame. A range
     * that continues the last draw's strip, with its vertices and indices
     * right after that draw's, extends it instead. RSX_STRIP_MERGE=0 draws
     * each range on its own. */
    D3D12DrawRecord* mpv = NULL;
    if ((primitive == RSX_PRIMITIVE_TRIANGLE_STRIP || primitive == RSX_PRIMITIVE_TRIANGLE_FAN) &&
        s_d3d.merge_strip == (int)primitive && s_d3d.merge_first_end == first &&
        s_d3d.draw_count > 0 && s_d3d.current_rsx_state && !late_on()) {
        static int en = -1;
        PS3_KNOB(en, ps3_env_int("RSX_STRIP_MERGE", 1));
        D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
        if (en && pv->is_vp && pv->index_count && pv->vertex_count &&
            pv->begin_epoch == s_d3d.current_rsx_state->begin_epoch &&
            pv->fp_addr == s_d3d.current_rsx_state->shader_program &&
            pv->ib_first + pv->index_count == s_d3d.vp_ib_offset &&
            (pv->fetch || pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == s_d3d.vp_vb_offset))
            mpv = pv;
    }
    /* VP_GPU_FETCH (live, default 1; 0 = the CPU path): strips and fans go
     * to the GPU as the guest's own indices and vertex bytes
     * (upload_strip_vp_fetch); the CPU path below runs when a draw does not
     * qualify. A range continues the last draw
     * only if both are fetch draws reading the same attribute records. */
    int fetch = 0;
    if ((primitive == RSX_PRIMITIVE_TRIANGLE_STRIP || primitive == RSX_PRIMITIVE_TRIANGLE_FAN) &&
        vf_on()) {
        const int ni = upload_strip_vp_fetch(s_d3d.current_rsx_state, first, count,
                                             primitive == RSX_PRIMITIVE_TRIANGLE_FAN,
                                             mpv && mpv->fetch ? mpv : NULL);
        if (ni >= 0) {
            fetch = s_up_strip ? 2 : 1;   /* 2: the indices are a strip */
            s_vs_up[primitive == RSX_PRIMITIVE_TRIANGLE_STRIP ? 2 : 3]++;
            if (mpv) s_d3d.merge_first_end = first + count;
            if (!ni) return;
            if (mpv && mpv->fetch == (u32)fetch && mpv->fdesc == s_up_fdesc) {
                mpv->index_count  += (u32)ni;
                mpv->vertex_count += (u32)ni;
                return;
            }
            emitted = (u32)ni; rec = 0;
        }
    }
    if (mpv && mpv->fetch) mpv = NULL;   /* a CPU range cannot extend a fetch draw */
    if (!fetch) {
    s_strip_base = mpv ? mpv->vertex_count : 0;
    s_vs_up[primitive == RSX_PRIMITIVE_QUADS ? 0 : primitive == RSX_PRIMITIVE_TRIANGLES ? 1
            : primitive == RSX_PRIMITIVE_TRIANGLE_STRIP ? 2 : 3]++;
    if (primitive == RSX_PRIMITIVE_QUADS)             emitted = upload_quads_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == RSX_PRIMITIVE_TRIANGLES)    emitted = upload_tris_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == RSX_PRIMITIVE_TRIANGLE_STRIP) emitted = upload_strip_vp_indexed(s_d3d.current_rsx_state, first, count, 0);
    else if (primitive == RSX_PRIMITIVE_TRIANGLE_FAN)   emitted = upload_strip_vp_indexed(s_d3d.current_rsx_state, first, count, 1);
    else {
        static int _skip = 0;
        if (_skip++ < 3)
            printf("[D3D12] draw_indexed: skipping prim=%u (not wired)\n", primitive);
        return;
    }
    s_strip_base = 0;
    if (mpv) {
        s_d3d.merge_first_end = first + count;
        if (!emitted) return;   /* nothing drawn; the strip's carry still moved on */
        if (s_up_icount) {
            mpv->vertex_count += emitted;
            mpv->index_count  += s_up_icount;
            return;
        }
        /* The unindexed fallback ran (no index room): a draw of its own. */
    }
    /* Same BEGIN/END batch concatenation as the non-indexed path: this title's
     * fluid arrives as ~3000 DRAW_INDEX_ARRAY batches of 256, and 256 is not a
     * multiple of 3, so every batch after the first assembled out of phase. */
    if (emitted && s_d3d.merge_prev_draw && s_d3d.draw_count > 0 &&
        s_d3d.merge_first_end == first && primitive == RSX_PRIMITIVE_TRIANGLES) {
        D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
        u32 fpnow = s_d3d.current_rsx_state
                    ? s_d3d.current_rsx_state->shader_program : 0;
        if (pv->is_vp && pv->topology == RSX_TOPOLOGY_TRIANGLES &&
            pv->fp_addr == fpnow &&
            pv->begin_epoch == (s_d3d.current_rsx_state
                                ? s_d3d.current_rsx_state->begin_epoch : 0) &&
            pv->index_count == 0 &&
            pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec) {
            pv->vertex_count += emitted;
            s_d3d.merge_first_end = first + count;
            return;
        }
    }
    }
    if (emitted && draw_room()) {
        D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
        dr->vb_byte_offset = rec;
        dr->index_count = s_up_icount; dr->ib_first = s_up_ifirst;
        dr->vertex_count   = emitted;
        dr->topology       = RSX_TOPOLOGY_TRIANGLES;
        dr->textured       = s_d3d.tex_bound;
        dr->is_vp          = 1;
        dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
        dr->fp_exp32 = s_d3d.current_rsx_state ?
            ((s_d3d.current_rsx_state->shader_control & 0x40) != 0) : 1;
        dr->cull = rsx_cull_key(s_d3d.current_rsx_state);
        dr->depth = rsx_depth_key(s_d3d.current_rsx_state);
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
        dr->cmask = 0xF;
        if (s_d3d.current_rsx_state) {
            u32 _cm = s_d3d.current_rsx_state->color_mask;
            dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                      | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                      | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                      | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
        }
        dr->alpha_ctl = 0;
        if (s_d3d.current_rsx_state)
            dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                          | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                          | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
        for (int _u = 0; _u < RSX_FP_UNITS; _u++) {
            dr->tex[_u].off = s_d3d.cur_texs[_u].off;
            dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
            dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
            dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
            dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1;
                    dr->tex[_u].addr  = s_d3d.cur_texs[_u].addr;
                    dr->tex[_u].filt  = s_d3d.cur_texs[_u].filt;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
            dr->tex[_u].set = s_d3d.cur_texs[_u].set;
            dr->tex_rt[_u]  = -1;
        }
        dr->tex_slot = -1;
        dr->vs_idx = fetch ? vp_get_vs_fetch(s_d3d.current_rsx_state)
                           : vp_get_vs(s_d3d.current_rsx_state);
        dr->fetch = (u32)fetch; dr->fdesc = fetch ? s_up_fdesc : 0;
        dr->is_clear = 0;
        dr->clr_flags = 0;
        dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
        dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
        dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt);
        dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
        rt_pitch_record(dr);
        if (s_d3d.current_rsx_state) {
            dr->vp_x = s_d3d.current_rsx_state->viewport_x;
            dr->vp_y = s_d3d.current_rsx_state->viewport_y;
            dr->vp_w = s_d3d.current_rsx_state->viewport_w;
            dr->vp_h = s_d3d.current_rsx_state->viewport_h;
        } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
        dr->cb_slot = s_d3d.draw_count;
                dr->vcb_slot = vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
        s_d3d.draw_count++;
        /* Anchor for merging the next batch of this indexed stream. */
        s_d3d.merge_prev_draw = (primitive == RSX_PRIMITIVE_TRIANGLES);
        s_d3d.merge_strip = (s_up_icount && (primitive == RSX_PRIMITIVE_TRIANGLE_STRIP ||
                             primitive == RSX_PRIMITIVE_TRIANGLE_FAN)) ? (int)primitive : 0;
        s_d3d.merge_first_end = first + count;
    }
}

static void d3d12_bind_texture(void* ud, u32 unit, const rsx_texture_state* tex)
{
    (void)ud;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    u32 width  = (tex->image_rect >> 16) & 0xFFFF;
    u32 height = tex->image_rect & 0xFFFF;
    u32 format = (tex->format >> 8) & 0xFF;
    u32 offset = tex->offset;
    /* CUBEDBG=1: NV4097_SET_TEXTURE_FORMAT bit 2 is the cubemap flag, and the
     * line above throws it away with the rest of the low byte. Nothing in this
     * backend handles cube textures, so a cubemap bound for an environment
     * reflection is sampled as a plain 2D image -- which is what the chrome
     * faucet's black patches and banded escutcheons look like. */
    { static int cd = -1;
      PS3_KNOB(cd, ps3_env_int("CUBEDBG", 0));
      if (cd && (tex->format & 4)) {
          static u32 seen[8]; static int ns = 0; int known = 0;
          for (int k = 0; k < ns; k++) if (seen[k] == offset) known = 1;
          if (!known && ns < 8) { seen[ns++] = offset;
              fprintf(stderr, "[CUBE] unit=%u offset=0x%08X %ux%u fmt=0x%02X"
                              " raw_format=0x%08X (cubemap bit SET)%c",
                      unit, offset, width, height, format, tex->format, 10); }
      } }

    static int log_count = 0;
    if (log_count < 10) {
        printf("[D3D12] bind_texture(unit=%u, offset=0x%X, fmt=0x%02X, %ux%u)\n",
               unit, offset, format, width, height);
        log_count++;
    }
    /* MOVIE_BIND=1: trace movie-plane binds (640x360 Y / 320x180 U/V) with the
     * resolved EA + a content probe -- used to diagnose the Bink frame-buffer
     * ring mismatch (the draw binds a cleared buffer 0x4D80 before the one the
     * decoder actually fills). */
    if (dbg_movie_bind() &&
        ((width == 640 && height == 360) || (width == 320 && height == 180))) {
        extern u32 cellGcmResolveLocated(int local, u32 offset);
        static int _mv = 0; if (_mv++ < 24) {
            /* Resolve the SAME raw offset both ways -- as LOCAL (VRAM) and as MAIN
             * (IO-mapped) -- and count nonzero bytes over the whole plane for each.
             * If the plane the game declares (loc bits fmt&3) is zero but the OTHER
             * pool has content, the decoder wrote to a different memory space than
             * our texture resolve picked (the local-vs-main mismatch). */
            u32 loc = tex->format & 3;               /* 1=LOCAL, 2=MAIN */
            u32 ea_loc  = cellGcmResolveLocated(1, offset);
            u32 ea_main = cellGcmResolveLocated(0, offset);
            u32 plane = width * height;
            u32 nz_loc = 0, nz_main = 0;
            if (vm_base) for (u32 i = 0; i < plane; i += 137) {
                if (vm_base[ea_loc  + i]) nz_loc++;
                if (vm_base[ea_main + i]) nz_main++;
            }
            fprintf(stderr, "[mv-bind] unit=%u fmt=0x%02X loc=%u %ux%u off=0x%X | LOCAL ea=0x%08X nz=%u | MAIN ea=0x%08X nz=%u\n",
                    unit, format, loc, width, height, offset,
                    ea_loc, nz_loc, ea_main, nz_main);
        }
    }

    if (!vm_base || width == 0 || height == 0) return;

    /* TEX_STAT=1: one line per distinct texture offset -- format, location, both
     * resolutions and how much of each is non-zero. Answers "is this draw black
     * because its texture is empty, unsupported, or resolved to the wrong pool"
     * for every texture a frame uses, not just the first few binds. */
    { static int ts = -1; PS3_KNOB(ts, getenv("TEX_STAT") ? 1 : 0);
      if (ts) {
        static u32 seen[128]; static int ns = 0; int known = 0;
        for (int k = 0; k < ns; k++) if (seen[k] == offset) known = 1;
        if (!known && ns < 128) { seen[ns++] = offset;
            extern u32 cellGcmResolveLocated(int local, u32 offset);
            u32 ea_l = cellGcmResolveLocated(1, offset), ea_m = cellGcmResolveLocated(0, offset);
            u32 span = width * height; if (span > 65536) span = 65536;
            u32 nz_l = 0, nz_m = 0;
            for (u32 i = 0; i < span; i += 13) {
                if (ea_l && ea_l < 0xF0000000u && vm_base[ea_l + i]) nz_l++;
                if (ea_m && ea_m < 0xF0000000u && vm_base[ea_m + i]) nz_m++;
            }
            fprintf(stderr, "[TEX_STAT] unit=%u off=0x%08X fmt=0x%02X loc=%u %ux%u raw_fmt=0x%08X"
                            " | LOCAL ea=0x%08X nz=%u | MAIN ea=0x%08X nz=%u (of %u)%c",
                    unit, offset, format, tex->format & 3, width, height, tex->format,
                    ea_l, nz_l, ea_m, nz_m, span / 13, 10);
        } } }

    /* Record the currently-bound atlas so subsequent quad draws sample it. The
     * actual GPU upload happens in render_frame (we have no open command list
     * here). Only the 8-bit single-channel font atlas (B8, RSX fmt base 0x81 /
     * as-seen 0xA1 with the LN flag) is wired up so far; other formats fall
     * back to untextured. */
    u32 base_fmt = format & 0x9F;   /* strip LN(0x20)/UN(0x40) flags */
    /* TEXFMTDBG=1 (live): one line per new unit/format/width, including the
     * formats the VP path below does not take -- those units end up sampling
     * nothing, so they are the ones worth seeing. */
    if (dbg_texfmt()) { static u32 seen[256]; static int ns=0;
        u32 k = (unit<<24) | (((tex->format>>8) & 0xFFu)<<16) | (width & 0xFFFFu);
        int f=0; for (int i=0;i<ns;i++) if (seen[i]==k) f=1;
        if (!f && ns < 256) { seen[ns++]=k;
            fprintf(stderr, "[TEXFMT] unit=%u fmt=0x%02X %ux%u mips=%u cube=%d off=0x%X%c",
                    unit, (tex->format >> 8) & 0xFFu, width, height,
                    (tex->format>>16)&0xFFFFu, (tex->format&4)?1:0, offset, 10); } }
    /* VP path: record the latest bound texture (any supported format) so draws
     * can carry it per-draw. Location bits (format[1:0]): 1 = LOCAL, 2 = MAIN. */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (unit < RSX_FP_UNITS && (vp_tex_fmt_ok(base_fmt) || rsx_fmt_is_depth(base_fmt))) {
        /* TEX_RESOLVE_AUTO=1: resolve through the page tables (local-page map
         * then IO table) instead of trusting the format's location bits. A
         * texture the guest built in main memory but tagged local resolves to
         * untouched VRAM and samples as all-zero -- geometry renders, flat. */
        { static int _ra = -1; PS3_KNOB(_ra, getenv("TEX_RESOLVE_AUTO") ? 1 : 0);
          extern u32 cellGcmResolveIO(u32);
          u32 _r = 0;
          if (_ra) _r = cellGcmResolveIO(offset);          /* IO table first */
          if (!_r) _r = cellGcmResolveLocated((tex->format & 3) == 1, offset);
          /* TEX_PROBE=1: the location bits are not always right. This title
           * binds its menu textures with a location that resolves into main
           * memory, where the bytes are all zero, while the same raw offset
           * resolved as LOCAL lands on real data -- so 100k menu draws sample
           * black and the menu never appears. When the chosen resolution is
           * empty over a sample and the other one is not, take the other. */
          { static int _tp = -1;
            PS3_KNOB(_tp, getenv("TEX_PROBE") ? 1 : 0);
            if (_tp && vm_base && _r) {
                u32 alt = cellGcmResolveLocated((tex->format & 3) != 1, offset);
                if (alt && alt != _r && alt < 0xE0000000u && _r < 0xE0000000u) {
                    u32 span = width * height / 2;
                    if (span > 4096) span = 4096;
                    u32 nz_r = 0, nz_a = 0;
                    for (u32 i = 0; i < span; i += 7) {
                        if (vm_base[_r  + i]) nz_r++;
                        if (vm_base[alt + i]) nz_a++;
                    }
                    if (nz_r == 0 && nz_a > 0) {
                        static u32 seen[32]; static int ns = 0; int known = 0;
                        for (int k = 0; k < ns; k++) if (seen[k] == offset) known = 1;
                        if (!known && ns < 32) { seen[ns++] = offset;
                            fprintf(stderr, "[TEX_PROBE] 0x%08X: %s empty, using %s"
                                            " (%u non-zero)%c", offset,
                                    ((tex->format & 3) == 1) ? "local" : "main",
                                    ((tex->format & 3) == 1) ? "main" : "local",
                                    nz_a, 10); }
                        _r = alt;
                    }
                }
            } }
          s_d3d.cur_texs[unit].off = _r; }
        s_d3d.cur_texs[unit].raw = offset;
        s_d3d.cur_texs[unit].w = width; s_d3d.cur_texs[unit].h = height;
        s_d3d.cur_texs[unit].fmt = format;   /* full byte: LN(0x20)/UN(0x40) kept */
        s_d3d.cur_texs[unit].ctrl1 = tex->control1;
        s_d3d.cur_texs[unit].addr  = tex->address;
        s_d3d.cur_texs[unit].filt  = tex->filter;
        s_d3d.cur_texs[unit].cube  = (tex->format & 4) ? 1 : 0;
        s_d3d.cur_texs[unit].mips  = (tex->format >> 16) & 0xFFFFu;
        s_d3d.cur_texs[unit].set = 1;
    }
    if (base_fmt == 0x81 /* B8 */) {
        s_d3d.tex_src_offset = cellGcmResolveLocated((tex->format & 3) == 1, offset);
        if (s_d3d.tex_w != width || s_d3d.tex_h != height) {
            /* dims changed -> resource must be (re)created in render_frame */
            s_d3d.tex_ready = 0;
        }
        s_d3d.tex_w     = width;
        s_d3d.tex_h     = height;
        s_d3d.tex_bound = 1;
        s_d3d.tex_dirty = 1;
    } else {
        s_d3d.tex_bound = 0;
    }
}

static void d3d12_set_vertex_attribs(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    /* Log enabled vertex attributes for debugging. RSX_VTXDBG=<N> raises the
     * cap: the fixed 5 were all consumed by boot-time setup, so the layouts the
     * real draws use were never printed. */
    static int log_count = 0, log_cap = -1;
    PS3_KNOB(log_cap, ps3_env_int("RSX_VTXDBG", 5));
    if (log_count < log_cap) {
        printf("[D3D12] set_vertex_attribs:\n");
        for (int i = 0; i < 16; i++) {
            const rsx_vertex_attrib* a = &state->vertex_attribs[i];
            if (a->enabled) {
                const char* type_name = "?";
                switch (a->type) {
                case 1: type_name = "snorm16"; break;
                case 2: type_name = "float32"; break;
                case 3: type_name = "float16"; break;
                case 4: type_name = "ubyte"; break;
                case 5: type_name = "s16"; break;
                case 7: type_name = "ubyte256"; break;
                }
                printf("  attrib[%d]: %s x%u, stride=%u, offset=0x%X\n",
                       i, type_name, a->size, a->stride, a->offset);
            }
        }
        log_count++;
    }
}

static void d3d12_set_shader(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_shader: fp_addr=0x%08X, vp_load=%u, output_mask=0x%08X\n",
               state->fragment_program_addr, state->transform_program_load,
               state->vertex_attrib_output_mask);
        log_count++;
    }

    /* TODO: Look up or compile a PSO matching this shader combination.
     * For now we use the basic vertex-colored PSO for everything. */
}

static void d3d12_set_blend(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: modify PSO blend state or use dynamic state.
     * D3D12 requires PSO recreation for blend state changes,
     * so we'd need a PSO cache keyed by blend configuration. */
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_blend(enable=%d, sfactor=0x%X, dfactor=0x%X)\n",
               state->blend_enable, state->blend_sfactor, state->blend_dfactor);
        log_count++;
    }
}

static void d3d12_set_depth_stencil(void* ud, const rsx_state* state)
{
    (void)ud;
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_depth_stencil(depth=%d, stencil=%d, func=0x%X)\n",
               state->depth_test_enable, state->stencil_test_enable,
               state->depth_func);
        log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_d3d12_backend = {0};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rsx_d3d12_backend_init(u32 width, u32 height, const char* title)
{
    { extern void (*g_gcm_fifo_flip_hook)(void); g_gcm_fifo_flip_hook = d3d12_fifo_flip; }
    if (ps3_env_int("RSX_ZPASS", 1)) g_rsx_report_hook = zq_hook;
    { extern void (*g_rcap_mark_hook)(int end); g_rcap_mark_hook = cap_mark; }
    memset(&s_d3d, 0, sizeof(s_d3d));
    memset(s_vp_fp_head, 0, sizeof s_vp_fp_head);   /* the PSO cache is empty again */
    s_d3d.width = width;
    s_d3d.height = height;
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

void rsx_d3d12_backend_shutdown(void)
{
    if (!s_d3d.initialized) return;

    wait_for_gpu();
    if (s_pso_lib) {
        InterlockedExchange(&s_pso_pre_abort, 1);
        while (s_pso_lib_saving || s_pso_pre_live) Sleep(1);
        if (s_pso_lib_new) pso_lib_save();
    }

    /* Release D3D12 resources */
    if (s_d3d.vertex_buffer) {
        s_d3d.vertex_buffer->lpVtbl->Unmap(s_d3d.vertex_buffer, 0, NULL);
        s_d3d.vertex_buffer->lpVtbl->Release(s_d3d.vertex_buffer);
    }
    if (s_d3d.pipeline_state)        s_d3d.pipeline_state->lpVtbl->Release(s_d3d.pipeline_state);
    if (s_d3d.pipeline_state_lines)  s_d3d.pipeline_state_lines->lpVtbl->Release(s_d3d.pipeline_state_lines);
    if (s_d3d.pipeline_state_points) s_d3d.pipeline_state_points->lpVtbl->Release(s_d3d.pipeline_state_points);
    if (s_d3d.depth_buffer) s_d3d.depth_buffer->lpVtbl->Release(s_d3d.depth_buffer);
    if (s_d3d.dsv_heap)     s_d3d.dsv_heap->lpVtbl->Release(s_d3d.dsv_heap);
    if (s_d3d.root_signature) s_d3d.root_signature->lpVtbl->Release(s_d3d.root_signature);
    if (s_d3d.fence) s_d3d.fence->lpVtbl->Release(s_d3d.fence);
    if (s_d3d.fence_event) CloseHandle(s_d3d.fence_event);
    if (s_d3d.cmd_list) s_d3d.cmd_list->lpVtbl->Release(s_d3d.cmd_list);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (s_d3d.cmd_allocators[i]) s_d3d.cmd_allocators[i]->lpVtbl->Release(s_d3d.cmd_allocators[i]);
        if (s_d3d.render_targets[i]) s_d3d.render_targets[i]->lpVtbl->Release(s_d3d.render_targets[i]);
        if (s_d3d.swap_buffers[i]) s_d3d.swap_buffers[i]->lpVtbl->Release(s_d3d.swap_buffers[i]);
    }
    if (s_d3d.rtv_heap) s_d3d.rtv_heap->lpVtbl->Release(s_d3d.rtv_heap);
    if (s_d3d.swap_rtv_heap) s_d3d.swap_rtv_heap->lpVtbl->Release(s_d3d.swap_rtv_heap);
    if (s_d3d.blit_srv_heap) s_d3d.blit_srv_heap->lpVtbl->Release(s_d3d.blit_srv_heap);
    if (s_d3d.blit_pso) s_d3d.blit_pso->lpVtbl->Release(s_d3d.blit_pso);
    if (s_d3d.blit_rs) s_d3d.blit_rs->lpVtbl->Release(s_d3d.blit_rs);
    if (s_d3d.swap_chain) s_d3d.swap_chain->lpVtbl->Release(s_d3d.swap_chain);
    if (s_d3d.cmd_queue) s_d3d.cmd_queue->lpVtbl->Release(s_d3d.cmd_queue);
    if (s_d3d.device) s_d3d.device->lpVtbl->Release(s_d3d.device);

    /* A window belongs to the thread that made it: the window thread
     * destroys its own and then leaves its message loop. */
    InterlockedExchange(&s_overlay_on, 0);   /* (its device is gone; and this close must not ask first) */
    if (s_d3d.hwnd) {
        if (s_window_thread) PostMessageA(s_d3d.hwnd, WM_CLOSE, 0, 0);
        else DestroyWindow(s_d3d.hwnd);
    }

    rsx_set_backend(NULL);
    s_d3d.initialized = 0;

    printf("[D3D12] Backend shut down\n");
}

void rsx_d3d12_set_overlay(const rsx_d3d12_overlay* overlay)
{
    InterlockedExchange(&s_overlay_on, 0);
    if (overlay) {
        s_overlay = *overlay;
        InterlockedExchange(&s_overlay_on, 1);
    }
}
void rsx_d3d12_set_borderless(int on)
{
    if (s_d3d.hwnd) {
        if (s_window_thread) PostMessageA(s_d3d.hwnd, WM_RSX_BORDERLESS, (WPARAM)!!on, 0);
        else set_borderless(s_d3d.hwnd, on);
    }
}
int      rsx_d3d12_borderless(void)   { return s_borderless; }
void*    rsx_d3d12_device(void)       { return s_d3d.device; }
void*    rsx_d3d12_queue(void)        { return s_d3d.cmd_queue; }
void*    rsx_d3d12_hwnd(void)         { return s_d3d.hwnd; }
void*    rsx_d3d12_back_buffer(unsigned i) { return i < FRAME_COUNT ? s_d3d.swap_buffers[i] : NULL; }
unsigned rsx_d3d12_back_buffer_count(void)  { return s_d3d.initialized ? FRAME_COUNT : 0; }
unsigned rsx_d3d12_back_buffer_format(void) { return (unsigned)DXGI_FORMAT_R8G8B8A8_UNORM; }

int rsx_d3d12_backend_pump_messages(void)
{
    MSG msg;
    if (s_window_thread) return s_d3d.window_closed ? -1 : 0;   /* the window thread pumps */
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return -1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return s_d3d.window_closed ? -1 : 0;
}

void rsx_d3d12_backend_present(void)
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

/* Offline warm-up of the shader disk cache (rsx_shader_precompile). */
int rsx_d3d12_shader_hlsl(int fragment, const u8* ucode, u32 ucode_len, u32 vp_start_slot,
                          int exp32, u32 cube_mask, u32 d2c_mask, char* out, u32 cap)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    extern int rsx_vp_decompile_fetch(const uint8_t*, u32, char*, u32);
    extern u32 rsx_vp_program_size_instrs(const u8*, u32);
    extern void rsx_vp_set_branch_base(u32);
    if (!ucode || !out || cap < 2) return 0;
    if (fragment == 1) {
        /* The decompiler may read up to RSX_FP_MAX_BYTES, as it does from
         * guest memory; give it a zero tail. */
        u8* buf = (u8*)calloc(1, RSX_FP_MAX_BYTES);
        if (!buf) return 0;
        memcpy(buf, ucode, ucode_len < RSX_FP_MAX_BYTES ? ucode_len : RSX_FP_MAX_BYTES);
        const int n = fp_build_hlsl(buf, exp32, cube_mask, d2c_mask, out, cap, 0, 0);
        free(buf);
        return n;
    }
    /* The binary holds big-endian words; rsx_state keeps the words
     * SET_TRANSFORM_PROGRAM delivered, little-endian. Then as vp_get_vs_mode:
     * trimmed to the END instruction, branch targets made relative to the
     * slot the program is loaded at. */
    u32 vlen = ucode_len & ~3u;
    u8* w = (u8*)malloc(vlen ? vlen : 4);
    if (!w) return 0;
    for (u32 i = 0; i < vlen; i += 4) {
        w[i] = ucode[i + 3]; w[i + 1] = ucode[i + 2]; w[i + 2] = ucode[i + 1]; w[i + 3] = ucode[i];
    }
    { const u32 instrs = rsx_vp_program_size_instrs(w, vlen);
      if (instrs) vlen = instrs * 16u; }
    rsx_vp_set_branch_base(vp_start_slot);
    const int n = fragment == 2 ? rsx_vp_decompile_fetch(w, vlen, out, cap)
                                : rsx_vp_decompile(w, vlen, out, cap);
    rsx_vp_set_branch_base(0);
    free(w);
    return n;
}

int rsx_d3d12_shader_cache_put(int fragment, const char* hlsl)
{
    const char* profile = fragment ? "ps_5_0" : "vs_5_0";
    if (!shader_cache_dir()[0] || !hlsl) return 0;
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s\\%s_%016llx.dxbc", shader_cache_dir(), profile,
             (unsigned long long)shader_key(hlsl, strlen(hlsl), profile));
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return 1;
    ID3DBlob* b = NULL; ID3DBlob* e = NULL;
    HRESULT hr = shader_compile_cached(hlsl, strlen(hlsl), fragment ? "guest_fp" : "guest_vp2",
                                       profile, &b, &e);
    if (e) e->lpVtbl->Release(e);
    if (b) b->lpVtbl->Release(b);
    return SUCCEEDED(hr) && b ? 2 : 0;
}

const char* rsx_d3d12_shader_cache_path(void) { return shader_cache_dir(); }

int rsx_d3d12_shader_fp_variants(const u8* ucode, u32 ucode_len, u32* out, int max)
{
    u8* buf = (u8*)calloc(1, RSX_FP_MAX_BYTES);
    if (!buf) return 0;
    memcpy(buf, ucode, ucode_len < RSX_FP_MAX_BYTES ? ucode_len : RSX_FP_MAX_BYTES);
    const u32 hash = rsx_fp_code_hash(buf, RSX_FP_MAX_BYTES);
    free(buf);
    fpv_load();
    int n = 0;
    for (int i = 0; i < s_fpv_n && n < max; i++)
        if (s_fpv[i].hash == hash) {
            out[n * 3 + 0] = s_fpv[i].exp32; out[n * 3 + 1] = s_fpv[i].cube;
            out[n * 3 + 2] = s_fpv[i].d2c; n++;
        }
    return n;
}

#else /* !_WIN32 */

#include <ps3emu/ps3types.h>   /* u32 (header includes above are inside the _WIN32 guard) */
#include <stdio.h>
#include "rsx_d3d12_backend.h"   /* rsx_d3d12_overlay */

/* Stub for non-Windows — D3D12 is Windows-only */
int rsx_d3d12_backend_init(u32 w, u32 h, const char* t)
{
    (void)w; (void)h; (void)t;
    printf("[D3D12] Not available on this platform (use Vulkan backend)\n");
    return -1;
}
void rsx_d3d12_backend_shutdown(void) {}
int rsx_d3d12_backend_pump_messages(void) { return 0; }
void rsx_d3d12_backend_present(void) {}
void rsx_d3d12_set_overlay(const rsx_d3d12_overlay* overlay) { (void)overlay; }
void*    rsx_d3d12_device(void) { return NULL; }
void*    rsx_d3d12_queue(void) { return NULL; }
void*    rsx_d3d12_hwnd(void) { return NULL; }
void*    rsx_d3d12_back_buffer(unsigned i) { (void)i; return NULL; }
unsigned rsx_d3d12_back_buffer_count(void) { return 0; }
unsigned rsx_d3d12_back_buffer_format(void) { return 0; }
void     rsx_d3d12_set_borderless(int on) { (void)on; }
int      rsx_d3d12_borderless(void) { return 0; }

#endif /* _WIN32 */
