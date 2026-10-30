
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

/* RSX_SCALE=<percent> (default 100, 50-800): render the display buffers and
 * the offscreen colour and depth targets at that multiple of their guest size
 * (docs/37); blit_to_window fits the display buffer to the window. The knob is
 * live: render_frame remakes the targets at the next batch (rsx_scale_apply),
 * and until then rsx_scale_pct stays at the scale they were made at. A target
 * whose larger side is
 * RSX_SCALE_MIN (default 64) or less stays at its guest size: Demon's Souls's
 * exposure chain (64x64, then 16x16 targets the title reads back) must keep
 * its texel counts. Each target keeps its guest size (w, h; what matching and
 * the guest's viewport use) and its host size (hw, hh; the resource). */
static u32 rsx_scale_wanted(void)
{
    static int p = -1;
    PS3_KNOB(p, ps3_env_int("RSX_SCALE", 100));
    return (u32)(p < 50 ? 50 : p > 800 ? 800 : p);
}
static u32 s_scale_pct;   /* the scale the targets are at */
static u32 rsx_scale_pct(void)
{
    if (!s_scale_pct) {
        s_scale_pct = rsx_scale_wanted();
        if (s_scale_pct != 100) printf("[D3D12] RSX_SCALE: render targets at %u%%\n", s_scale_pct);
    }
    return s_scale_pct;
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

/* RSX_ASPECT=<w:h, WxH or a number> (default unset: the display buffer's own
 * shape, 1280x720): the shape the frame is shown at. blit_to_window stretches
 * the frame to the largest rectangle of that shape in the window, so a title
 * that draws a wider view into its 1280x720 buffers (Demon's Souls's
 * DES_ASPECT) fills a 21:9 or 32:9 window, as RPCS3's "Stretch To Display
 * Area" does. The window's first size takes the same shape. Live. A value
 * outside 0.5-8, or one that does not read as a number, is the buffer's
 * shape. */
static float rsx_aspect_parse(const char* s)
{
    char* end = NULL;
    double a;
    if (!s || !*s) return 0.0f;
    a = strtod(s, &end);
    if (end && (*end == ':' || *end == 'x' || *end == 'X' || *end == '/')) {
        const double b = strtod(end + 1, NULL);
        a = b > 0.0 ? a / b : 0.0;
    }
    return a >= 0.5 && a <= 8.0 ? (float)a : 0.0f;
}
static float rsx_aspect(void)
{
    static float a = -1.0f, said = 0.0f;
    PS3_KNOB(a, rsx_aspect_parse(getenv("RSX_ASPECT")));
    if (a != said) {
        said = a;
        if (a > 0.0f) printf("[D3D12] RSX_ASPECT: the frame is shown at %.4f:1\n", (double)a);
        else printf("[D3D12] RSX_ASPECT off: the frame keeps its own shape\n");
    }
    return a;
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wtsapi32.h>         /* session lock notifications (s_session_locked) */
#include <dwmapi.h>           /* DWM_TIMING_INFO (dwmapi.dll is loaded, not linked) */

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
#else
#define RSX_VKD3D_DEFINE_GUIDS   /* the interface GUIDs live in this file */
#include "rsx_vkd3d_host.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <glob.h>
#endif
#include "rsx_window_icon.h"   /* (after SDL.h: rsx_window_icon_sdl) */

