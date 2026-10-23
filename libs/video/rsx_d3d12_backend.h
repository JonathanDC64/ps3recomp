/*
 * ps3recomp - D3D12 RSX Backend
 *
 * Translates RSX GPU commands to Direct3D 12 rendering.
 * Implements the rsx_backend callback interface.
 *
 * Architecture:
 *   rsx_commands.c (state tracking) → rsx_backend callbacks → this file
 *                                                               ↓
 *                                                         D3D12 device
 *                                                         swap chain
 *                                                         command lists
 *                                                         pipeline states
 *
 * Current scope (Phase 1):
 *   - Device/swap chain creation
 *   - Clear to RSX clear color
 *   - Present with vsync
 *   - Basic triangle rendering (vertex position + color)
 *
 * Future phases:
 *   - Texture upload and sampling
 *   - RSX vertex/fragment program → HLSL translation
 *   - Framebuffer resolve and display buffer management
 *   - Depth/stencil buffer creation
 */

#ifndef PS3RECOMP_RSX_D3D12_BACKEND_H
#define PS3RECOMP_RSX_D3D12_BACKEND_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the D3D12 backend.
 * Creates a Win32 window, D3D12 device, swap chain, and registers
 * as the active rsx_backend. RSX_MAXIMIZED=1 (default 0): the window
 * opens maximized (create_window in rsx_d3d12_backend.c).
 * Returns 0 on success, -1 on failure. */
int rsx_d3d12_backend_init(u32 width, u32 height, const char* title);

/* Shut down the D3D12 backend and release all resources. */
void rsx_d3d12_backend_shutdown(void);

/* Process Win32 messages. Returns 0 normally, -1 if window closed. */
int rsx_d3d12_backend_pump_messages(void);

/* Force a present (useful for debugging). */
void rsx_d3d12_backend_present(void);

/* An overlay drawn over the game, such as a wrapper UI. Both hooks are
 * optional; set_overlay(NULL) removes it. With an overlay installed, Esc no
 * longer closes the window (the overlay decides what Esc does).
 *
 * wndproc: called first for every message the game window receives, on the
 *   window's own thread (RSX_WINDOW_THREAD=0: the thread that pumps
 *   messages). Return nonzero to consume it (*result is then the window
 *   procedure's return value). Consuming WM_CLOSE keeps the window open (to
 *   ask first); the backend's own close at shutdown is not passed to it.
 *   The window is per-monitor DPI aware: an overlay sizes its text by the
 *   window's DPI (GetDpiForWindow), not by the window's size.
 * frame: called on the render thread once per presented frame, after the
 *   frame's command list has been submitted and before Present. back_buffer
 *   is the ID3D12Resource about to be presented, in the PRESENT state, at
 *   the window's client size (width x height); the game's frame is already
 *   scaled into it. The hook leaves it in that state, with its own work
 *   submitted to the queue from rsx_d3d12_queue(). Screenshots and captures
 *   are taken from the game's frame, so they never contain the overlay.
 * release: called on the render thread before the window's buffers are
 *   resized; the hook drops every reference it holds to them (and finishes
 *   any work using them). Later frames pass the new buffers. */
/* Off Windows (the backend on vkd3d) the window is SDL's and the window
 * thread hands wndproc the Win32 messages Windows would, made from SDL's
 * events, with Windows' values: WM_KEYDOWN / WM_KEYUP (WM_SYSKEY* with Alt,
 * and for Alt and F10) with virtual-key codes and the repeat bit, WM_CHAR,
 * the mouse's button messages and WM_MOUSEMOVE (window coordinates, in
 * SDL's points), WM_MOUSEWHEEL, WM_SETFOCUS, WM_KILLFOCUS, WM_MOUSELEAVE,
 * WM_SIZE and WM_CLOSE, and messages posted with rsx_d3d12_post_message.
 * hwnd is the SDL_Window. Raw mouse input is WM_RSX_MOUSE_MOTION instead
 * of WM_INPUT: wp and lp are the motion in x and y as signed counts (SDL's
 * relative motion, with no acceleration in relative mouse mode). */
#define WM_RSX_MOUSE_MOTION (0x8000u + 0x53u)   /* WM_APP + 0x53 */
typedef struct rsx_d3d12_overlay {
    void* user;
    int  (*wndproc)(void* user, void* hwnd, unsigned msg, unsigned long long wp,
                    long long lp, long long* result);
    void (*frame)(void* user, void* back_buffer, unsigned index,
                  unsigned width, unsigned height);
    void (*release)(void* user);
} rsx_d3d12_overlay;
void rsx_d3d12_set_overlay(const rsx_d3d12_overlay* overlay);
/* The overlay at the display's refresh rate (on = 1), not only with the
 * game's frames: between them the render thread shows the last game frame
 * again, with the overlay's frame hook called on it as for any other frame.
 * For a menu that should move smoothly whatever the game's frame rate, and
 * through the game's hitches. The game's own frames still show as soon as
 * they are presented: a repaint is made only late in a refresh with no
 * frame of its own. Any thread; 0 (the default) is off. RSX_OVERLAY_REPAINT=0
 * (live) turns it off whatever the overlay asks. */
void rsx_d3d12_overlay_repaint(int on);
/* The overlay has input waiting (a key that opens its menu) while its
 * repaints are off. When the game has not presented for a quarter of a
 * second (it has stopped: a hang, or a long stall), the render thread calls
 * the frame hook once over the last frame shown, so the overlay can open its
 * menu and turn repaints on; a game that is presenting runs the hook with its
 * next frame anyway. Any thread (the window procedure). */
void rsx_d3d12_overlay_wake(void);
/* The render thread's idle point (the ticker, between FIFO drains): makes
 * the repaint above when one is due. Cheap when none is. */
void rsx_d3d12_overlay_tick(void);
/* For the overlay: ID3D12Device*, ID3D12CommandQueue*, HWND, the swap chain's
 * back buffers (ID3D12Resource*, count, DXGI_FORMAT). NULL / 0 before init. */
void*    rsx_d3d12_device(void);
void*    rsx_d3d12_queue(void);
void*    rsx_d3d12_hwnd(void);
/* Posts a message to the game window's thread, as PostMessage: the window
 * procedure, and so the overlay's wndproc hook, gets it there. Off Windows
 * the overlay gets it and a WM_CLOSE it does not take closes the window.
 * Any thread. */
void     rsx_d3d12_post_message(unsigned msg, unsigned long long wp, long long lp);
void*    rsx_d3d12_back_buffer(unsigned index);
unsigned rsx_d3d12_back_buffer_count(void);
unsigned rsx_d3d12_back_buffer_format(void);
/* Borderless fullscreen on the window's monitor (1) or the window as it was,
 * maximized or not (0). Alt+Enter toggles it too. Any thread. */
void     rsx_d3d12_set_borderless(int on);
int      rsx_d3d12_borderless(void);

/* Shader disk cache (PS3_SHADER_CACHE, default shader_cache beside the exe),
 * for filling it before a title runs (tools: rsx_shader_precompile).
 * rsx_d3d12_shader_hlsl builds the HLSL a draw would compile for one RSX
 * program (fragment 1: exp32 = SET_SHADER_CONTROL 32-bit exports, cube/d2c =
 * per-unit masks; fragment 3: the same with the sprite-edge clamp, as a draw
 * without a depth test on a scaled target (RSX_SCALE) has it; vertex,
 * fragment 0: vp_start_slot = the slot it is loaded at; fragment 2: the same
 * vertex program in fetch form, reading its vertex arrays itself, as the
 * draws the GPU fetch path records use) and returns its instruction count,
 * <= 0 on failure. Not thread-safe.
 * rsx_d3d12_shader_cache_put compiles that text into the cache: 1 = already
 * there, 2 = compiled, 0 = failed. Thread-safe. */
int rsx_d3d12_shader_hlsl(int fragment, const u8* ucode, u32 ucode_len, u32 vp_start_slot,
                          int exp32, u32 cube_mask, u32 d2c_mask, char* out, u32 cap);
int rsx_d3d12_shader_cache_put(int fragment, const char* hlsl);
const char* rsx_d3d12_shader_cache_path(void);
/* Variants (exp32, cube mask, d2c mask: 3 words each) that earlier runs
 * compiled this fragment program with, from the cache's fp_variants.txt.
 * Returns how many were written to out (at most max). */
int rsx_d3d12_shader_fp_variants(const u8* ucode, u32 ucode_len, u32* out, int max);

#ifdef __cplusplus
}
#endif
#endif
