/*
 * ps3recomp - the renderer's front: the route switch (see rsx_out.h)
 *
 * Built with PS3RECOMP_RSX_HAL wherever the legacy D3D12 renderer is
 * (Windows, or vkd3d off it). The route is picked once, on the first rsx_out
 * call, from RSX_BACKEND; until then nothing of any renderer has run. On
 * Windows the default (d3d12, or RSX_BACKEND unset) is the new path on its
 * D3D12 backend, rsxc_route_d3d12; null and vulkan call rsxc_route_null and
 * rsxc_route_vk (vulkan in builds with PS3RECOMP_RSX_HAL_VULKAN); on legacy
 * each rsx_out_X calls rsx_d3d12_X, the renderer before the new path
 * (rsx_d3d12_backend.c), retired on Windows and frozen everywhere: kept as
 * it was to fall back on, with its fixes going to the new path. The new path's
 * D3D12 is Windows' only; off Windows the build is vkd3d's, which
 * CMakeLists.txt allows on Linux only, where the default is the legacy
 * renderer on vkd3d and the core builds null and vulkan too.
 *
 * A start that fails runs another renderer in the same process (the section
 * "Starts that fall back" below says when and how): Vulkan's the default,
 * the new path's D3D12 the legacy renderer.
 */
#if defined(PS3RECOMP_RSX_HAL) && (defined(_WIN32) || defined(PS3RECOMP_RSX_D3D12_VKD3D))

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   /* getenv */
#endif
#include "rsx_out.h"
#include "rsx_d3d12_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the legacy renderer exports beyond its header. */
void rsx_d3d12_composite_from_offscreen(u32 raw_off);
void rsx_prof_begin(const char* prefix);
int  rsx_prof_end(char* digest, size_t cap);
void rsx_prof_title(const char* note);

#if defined(_WIN32)
#include <windows.h>
#include <intrin.h>
#define RSX_OUT_CAS(p, old, val) _InterlockedCompareExchange((p), (val), (old))
#else
#define RSX_OUT_CAS(p, old, val) __sync_val_compare_and_swap((p), (old), (val))
/* (as Windows spells it, for the section "Starts that fall back") */
#define InterlockedExchange(p, val) __atomic_exchange_n((p), (val), __ATOMIC_SEQ_CST)
#endif

/* A route here can fail its start into another (the section "Starts that
 * fall back"): the new path's D3D12, or Vulkan. */
#if defined(_WIN32) || defined(PS3RECOMP_RSX_HAL_VULKAN)
#define RSX_OUT_FALLS_BACK 1
#endif

enum { ROUTE_NONE, ROUTE_LEGACY, ROUTE_D3D12, ROUTE_NULL, ROUTE_VULKAN };
#if defined(_WIN32)
#define ROUTE_DEFAULT ROUTE_D3D12    /* the new path on D3D12 */
#else
#define ROUTE_DEFAULT ROUTE_LEGACY   /* (no D3D12 of the new path off Windows: the legacy renderer on vkd3d) */
#endif
static volatile long s_route;   /* ROUTE_*: none until the first call */

/* Why the route is not the one RSX_BACKEND asked for, for
 * rsx_out_renderer_info; NULL when it is. */
static const char* volatile s_why;
static char s_why_text[160];

#if defined(PS3RECOMP_RSX_HAL_VULKAN)
static int mark_left(void);   /* Vulkan's start, below */
#endif

/* a and b the same, ignoring ASCII case. */
static int same_name(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        const char x = (char)(*a >= 'A' && *a <= 'Z' ? *a + 32 : *a);
        const char y = (char)(*b >= 'A' && *b <= 'Z' ? *b + 32 : *b);
        if (x != y) return 0;
    }
    return *a == *b;
}

/* The route's name without picking one. */
static const char* route_name(long r)
{
    switch (r) {
    case ROUTE_LEGACY: return "legacy";
    case ROUTE_D3D12:  return "d3d12";
    case ROUTE_NULL:   return "null";
    case ROUTE_VULKAN: return "vulkan";
    default:           return NULL;
    }
}

/* The route RSX_BACKEND asks for, said once. Every thread that gets here
 * before a route is stored reads the same variable, so they all pick the
 * same; the one that stores it prints the line. */
static long route_pick(void)
{
    const char* e = getenv("RSX_BACKEND");
    long want = ROUTE_DEFAULT;
    int other = 0, left = 0;
    if (!e || !*e || same_name(e, "d3d12")) {
        want = ROUTE_DEFAULT;
    } else if (same_name(e, "hal-d3d12")) {
#if defined(_WIN32)
        want = ROUTE_D3D12;   /* (its name while the legacy renderer was the default) */
#else
        other = 1;   /* (the new path off Windows is null and Vulkan) */
#endif
    } else if (same_name(e, "legacy")) {
        want = ROUTE_LEGACY;
    } else if (same_name(e, "null")) {
        want = ROUTE_NULL;   /* (developers and CI only: never in Settings) */
    } else if (same_name(e, "vulkan")) {
#if defined(PS3RECOMP_RSX_HAL_VULKAN)
        want = ROUTE_VULKAN;   /* (Settings > Display > Renderer in the port) */
        if (mark_left()) {     /* (the last Vulkan start never finished) */
            want = ROUTE_DEFAULT;
            left = 1;
        }
#else
        other = 1;
#endif
    } else {
        other = 1;
    }
    const long had = RSX_OUT_CAS(&s_route, (long)ROUTE_NONE, want);
    if (had != ROUTE_NONE) return had;
    if (want == ROUTE_NULL)
        printf("[rsx_out] route null (the new path on the null backend: checks every command, draws nothing)\n");
    else if (want == ROUTE_VULKAN)
        printf("[rsx_out] route vulkan (the new path on its Vulkan backend)\n");
    else if (left) {
        s_why = "the last start on Vulkan did not finish, so this one uses D3D12";
        printf("[rsx_out] route %s (RSX_BACKEND=vulkan, but %s; the next start tries Vulkan again)\n",
               route_name(want), s_why);
    } else if (other) {
        snprintf(s_why_text, sizeof s_why_text, "RSX_BACKEND=%.32s is not in this build", e);
        s_why = s_why_text;
        printf("[rsx_out] route %s (%s)\n", route_name(want), s_why);
    } else if (want == ROUTE_LEGACY)
#if defined(_WIN32)
        printf("[rsx_out] route legacy (retired: the renderer before the new path, rsx_d3d12_backend.c, kept to fall"
               " back on)\n");
#else
        printf("[rsx_out] route legacy (the default off Windows: the renderer before the new path, on vkd3d; frozen)\n");
#endif
    else
        printf("[rsx_out] route d3d12 (the new path on its D3D12 backend)\n");
    fflush(stdout);
    return want;
}

const char* rsx_out_renderer_info(const char** why)
{
    const long r = s_route;
    if (why) *why = r != ROUTE_NONE ? s_why : NULL;
    return route_name(r);
}

/* The new path's table on d3d12, null and vulkan, NULL on legacy. One load
 * and up to three compares once the route is picked. */
static const rsx_route* route_table(void)
{
    long r = s_route;
    if (r == ROUTE_NONE) r = route_pick();
#if defined(_WIN32)
    if (r == ROUTE_D3D12) return &rsxc_route_d3d12;
#endif
#if defined(PS3RECOMP_RSX_HAL_VULKAN)
    if (r == ROUTE_VULKAN) return &rsxc_route_vk;
#endif
    return r == ROUTE_NULL ? &rsxc_route_null : NULL;
}

const char* rsx_out_renderer(void)
{
    const rsx_route* const t = route_table();
    return t ? t->name : "legacy";
}

#define RSX_OUT_V_(name, legacy, params, args) \
    void rsx_out_##name params { const rsx_route* const t_ = route_table(); if (t_) t_->name args; else legacy args; }
#define RSX_OUT_R_(ret, name, legacy, params, args) \
    ret rsx_out_##name params { const rsx_route* const t_ = route_table(); return t_ ? t_->name args : legacy args; }

#if defined(RSX_OUT_FALLS_BACK)
/* Three of the functions are written out in the section below (a start, and
 * what the renderer a failed start falls back to needs again). The table
 * makes them under these names, file-local, for those to call. */
static int  rsx_out_plain_backend_init(uint32_t width, uint32_t height, const char* title);
static void rsx_out_plain_set_overlay(const struct rsx_d3d12_overlay* overlay);
static void rsx_out_plain_pso_index_game(const char* game);
#define rsx_out_backend_init   rsx_out_plain_backend_init
#define rsx_out_set_overlay    rsx_out_plain_set_overlay
#define rsx_out_pso_index_game rsx_out_plain_pso_index_game
#endif
RSX_OUT_FUNCS(RSX_OUT_V_, RSX_OUT_R_)
#if defined(RSX_OUT_FALLS_BACK)
#undef rsx_out_backend_init
#undef rsx_out_set_overlay
#undef rsx_out_pso_index_game
#endif

/* ---- step VM ---- */
/* The overlay v2 (rsx_out.h): the route's own calls on the new path, none
 * on legacy. */
static const rsx_route_gpu* route_gpu(void)
{
    const rsx_route* const t = route_table();
#if defined(_WIN32)
    if (t == &rsxc_route_d3d12) return &rsxc_route_gpu_d3d12;
#endif
    if (t == &rsxc_route_null) return &rsxc_route_gpu_null;
#if defined(PS3RECOMP_RSX_HAL_VULKAN)
    if (t == &rsxc_route_vk) return &rsxc_route_gpu_vk;
#endif
    return NULL;
}

/* What the host gives the renderer before its start, kept for the renderer
 * a failed start falls back to: the overlay, the menus' overlay v2 and the
 * game's tag for the pipeline index. (Every route; only a fallback reads
 * them.) */
static rsx_out_overlay_gpu s_pre_overlay_gpu;
static volatile long s_pre_overlay_gpu_set;

void rsx_out_set_overlay_gpu(const rsx_out_overlay_gpu* overlay)
{
    if (overlay) s_pre_overlay_gpu = *overlay;
    InterlockedExchange(&s_pre_overlay_gpu_set, overlay ? 1 : 0);
    const rsx_route_gpu* const g = route_gpu();
    if (g) g->set_overlay_gpu(overlay);
}

struct gpu_device* rsx_out_gpu_device(void)
{
    const rsx_route_gpu* const g = route_gpu();
    return g ? g->gpu_device() : NULL;
}

struct gpu_cmd* rsx_out_overlay_gpu_flush(void)
{
    const rsx_route_gpu* const g = route_gpu();
    return g ? g->overlay_gpu_flush() : NULL;
}

#if defined(RSX_OUT_FALLS_BACK)
/* ---------------------------------------------------------------------------
 * Starts that fall back
 *
 * A start that fails at a step the core sees (rsxc_init.inc's start_step:
 * loader, instance, adapter, features, device, window, swap chain, targets,
 * shaders, rings) is undone by the core (rc_abort_init), and another
 * renderer starts in the same process with what the host handed over before
 * it, saying why (the line below, and rsx_out_renderer_info for a host's
 * report):
 *   [rsx_out] route d3d12 (Vulkan failed to start: no Vulkan 1.3 driver (loader); this start uses D3D12)
 *   [rsx_out] route legacy (D3D12 failed to start: the GPU device could not be made (device); this start uses the legacy renderer)
 * Vulkan falls back to the default (the new path's D3D12 on Windows, the
 * legacy renderer on Linux), the new path's D3D12 to the legacy renderer.
 *
 * Vulkan's start has one more: a start that never finished (the process
 * died in the driver). With RSX_STATE_DIR set (a folder of the host's that
 * setup does not clear; the port's data folder), a Vulkan start writes
 * RSX_STATE_DIR/renderer_starting first and removes it at its first present
 * (rsx_out_route_presented). A start that finds it there uses the default
 * (route_pick's line), and any start not on Vulkan removes it, so the next
 * one tries Vulkan again. A process that never starts the renderer leaves
 * it. Without RSX_STATE_DIR (test runs) there is no mark. On Linux the same,
 * with the file calls POSIX's (RSX_STATE_DIR is a path as the system takes
 * it).
 * -------------------------------------------------------------------------*/
#if defined(PS3RECOMP_RSX_HAL_VULKAN)
#if defined(_WIN32)
/* RSX_STATE_DIR/renderer_starting (RSX_STATE_DIR in UTF-8): 1 with its path
 * in p, 0 without the knob. */
static int mark_path(wchar_t* p, int cap)
{
    const char* d = getenv("RSX_STATE_DIR");
    wchar_t w[MAX_PATH];
    if (!d || !*d || MultiByteToWideChar(CP_UTF8, 0, d, -1, w, MAX_PATH) <= 0) return 0;
    return _snwprintf(p, (size_t)cap, L"%ls\\renderer_starting", w) > 0;
}

static int mark_left(void)
{
    wchar_t p[MAX_PATH + 32];
    return mark_path(p, MAX_PATH + 32) && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* 1 once the mark is written. */
static int mark_put(void)
{
    static const char text[] = "A Vulkan start is under way; the renderer removes this file at its first frame.\r\n";
    wchar_t p[MAX_PATH + 32];
    if (!mark_path(p, MAX_PATH + 32)) return 0;
    HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        printf("[rsx_out] could not write the start mark in RSX_STATE_DIR (error %lu)\n", (unsigned long)GetLastError());
        return 0;
    }
    DWORD n = 0;
    const BOOL ok = WriteFile(f, text, (DWORD)(sizeof text - 1), &n, NULL);
    CloseHandle(f);
    return ok != 0;
}

static void mark_take(void)
{
    wchar_t p[MAX_PATH + 32];
    if (mark_path(p, MAX_PATH + 32)) DeleteFileW(p);
}
#else
#include <errno.h>
#include <unistd.h>

/* RSX_STATE_DIR/renderer_starting: 1 with its path in p, 0 without the knob
 * (or a path too long). */
static int mark_path(char* p, size_t cap)
{
    const char* d = getenv("RSX_STATE_DIR");
    if (!d || !*d) return 0;
    const int n = snprintf(p, cap, "%s/renderer_starting", d);
    return n > 0 && (size_t)n < cap;
}

static int mark_left(void)
{
    char p[4096];
    return mark_path(p, sizeof p) && access(p, F_OK) == 0;
}

/* 1 once the mark is written. */
static int mark_put(void)
{
    static const char text[] = "A Vulkan start is under way; the renderer removes this file at its first frame.\n";
    char p[4096];
    if (!mark_path(p, sizeof p)) return 0;
    FILE* f = fopen(p, "wb");
    if (!f) {
        printf("[rsx_out] could not write the start mark in RSX_STATE_DIR (error %d)\n", errno);
        return 0;
    }
    const int ok = fwrite(text, 1, sizeof text - 1, f) == sizeof text - 1;
    return fclose(f) == 0 && ok;
}

static void mark_take(void)
{
    char p[4096];
    if (mark_path(p, sizeof p)) unlink(p);
}
#endif

static volatile long s_marked;   /* 1 while this process's mark is there */

void rsx_out_route_presented(void)
{
    if (InterlockedExchange(&s_marked, 0)) mark_take();
}
#endif /* PS3RECOMP_RSX_HAL_VULKAN */

/* (the rest of what the host gives before the start: see s_pre_overlay_gpu) */
static rsx_d3d12_overlay s_pre_overlay;
static volatile long s_pre_overlay_set;
static char s_pre_game[64];

void rsx_out_set_overlay(const struct rsx_d3d12_overlay* overlay)
{
    if (overlay) s_pre_overlay = *overlay;
    InterlockedExchange(&s_pre_overlay_set, overlay ? 1 : 0);
    rsx_out_plain_set_overlay(overlay);
}

void rsx_out_pso_index_game(const char* game)
{
    snprintf(s_pre_game, sizeof s_pre_game, "%s", game ? game : "");
    rsx_out_plain_pso_index_game(game);
}

/* A start step's failure in plain words. */
static const char* step_words(const char* step)
{
    static const struct { const char* step; const char* words; } k[] = {
        {"loader",     "no Vulkan 1.3 driver"},
        {"instance",   "the Vulkan driver would not start"},
        {"adapter",    "no GPU with Vulkan"},
        {"features",   "the GPU lacks a Vulkan feature the renderer needs"},
        {"device",     "the GPU device could not be made"},
        {"window",     "the window could not be made"},
        {"swap chain", "the window's swap chain could not be made"},
        {"targets",    "the render targets could not be made"},
        {"shaders",    "the renderer's shaders could not be made"},
        {"rings",      "the frame's buffers could not be made"},
    };
    for (size_t i = 0; step && i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(step, k[i].step)) return k[i].words;
    return "an error the renderer did not name";
}

/* A start of api that failed at step: say why, make route the one that
 * runs, and hand it what the host gave the one that failed. */
static void fall_back(const char* api, const char* step, long route)
{
    snprintf(s_why_text, sizeof s_why_text, "%s failed to start: %s (%s)", api, step_words(step),
             step && *step ? step : "?");
    s_why = s_why_text;
    InterlockedExchange(&s_route, route);
    printf("[rsx_out] route %s (%s; this start uses %s)\n", route_name(route), s_why,
           route == ROUTE_LEGACY ? "the legacy renderer" : "D3D12");
    fflush(stdout);
    const rsx_route* const t = route_table();
    if (s_pre_overlay_set) {
        if (t) t->set_overlay(&s_pre_overlay);
        else rsx_d3d12_set_overlay(&s_pre_overlay);
    }
    const rsx_route_gpu* const g = route_gpu();
    if (s_pre_overlay_gpu_set && g) g->set_overlay_gpu(&s_pre_overlay_gpu);
    if (s_pre_game[0]) {
        if (t) t->pso_index_game(s_pre_game);
        else rsx_d3d12_pso_index_game(s_pre_game);
    }
}

int rsx_out_backend_init(uint32_t width, uint32_t height, const char* title)
{
    const rsx_route* t = route_table();
#if defined(PS3RECOMP_RSX_HAL_VULKAN)
    if (t == &rsxc_route_vk) {
        InterlockedExchange(&s_marked, mark_put());
        if (rsxc_route_vk.backend_init(width, height, title) == 0) return 0;   /* (the mark goes at the first present) */
        if (InterlockedExchange(&s_marked, 0)) mark_take();   /* (a failure the core saw is no crash) */
        fall_back("Vulkan", rsxc_vk_start_failed(), ROUTE_DEFAULT);
        t = route_table();
    } else {
        mark_take();   /* (one the last start left: this start is not Vulkan's, so it is done with) */
    }
#endif
#if defined(_WIN32)
    if (t == &rsxc_route_d3d12) {
        if (rsxc_route_d3d12.backend_init(width, height, title) == 0) return 0;
        fall_back("D3D12", rsxc_d3d12_start_failed(), ROUTE_LEGACY);
        t = NULL;
    }
#endif
    return t ? t->backend_init(width, height, title) : rsx_d3d12_backend_init(width, height, title);
}
#endif /* RSX_OUT_FALLS_BACK */

#endif /* PS3RECOMP_RSX_HAL && (_WIN32 || PS3RECOMP_RSX_D3D12_VKD3D) */
