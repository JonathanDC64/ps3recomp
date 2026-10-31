/*
 * ps3recomp - the renderer's front: the route switch (see rsx_out.h)
 *
 * Built with PS3RECOMP_RSX_HAL wherever today's D3D12 renderer is (Windows,
 * or vkd3d off it). The route is picked once, on the first rsx_out call,
 * from RSX_BACKEND; until then nothing of either renderer has run. On the
 * d3d12 route each rsx_out_X calls rsx_d3d12_X; on hal-d3d12, null and
 * vulkan it calls the new path's table for that backend, rsxc_route_d3d12,
 * rsxc_route_null or rsxc_route_vk (Windows only for now; vulkan in builds
 * with PS3RECOMP_RSX_HAL_VULKAN).
 *
 * Step V6: a Vulkan start that fails runs today's renderer instead (the
 * section "Vulkan's start" below says when and how).
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

/* What today's renderer exports beyond its header. */
void rsx_d3d12_composite_from_offscreen(u32 raw_off);
void rsx_prof_begin(const char* prefix);
int  rsx_prof_end(char* digest, size_t cap);
void rsx_prof_title(const char* note);

#if defined(_WIN32)
#include <intrin.h>
#define RSX_OUT_CAS(p, old, val) _InterlockedCompareExchange((p), (val), (old))
#else
#define RSX_OUT_CAS(p, old, val) __sync_val_compare_and_swap((p), (old), (val))
#endif

enum { ROUTE_NONE, ROUTE_D3D12, ROUTE_HAL_D3D12, ROUTE_NULL, ROUTE_VULKAN };
static volatile long s_route;   /* ROUTE_*: none until the first call */

/* Step V6: why the route is d3d12 although RSX_BACKEND asked for another,
 * for rsx_out_renderer_info; NULL when it is the one asked for. */
static const char* volatile s_why;
static char s_why_text[160];

#if defined(_WIN32) && defined(PS3RECOMP_RSX_HAL_VULKAN)
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

/* The route RSX_BACKEND asks for, said once. Every thread that gets here
 * before a route is stored reads the same variable, so they all pick the
 * same; the one that stores it prints the line. */
static long route_pick(void)
{
    const char* e = getenv("RSX_BACKEND");
    long want = ROUTE_D3D12;
    int other = 0, left = 0;
    if (!e || !*e || same_name(e, "d3d12")) {
        want = ROUTE_D3D12;
    } else if (same_name(e, "hal-d3d12")) {
#if defined(_WIN32)
        want = ROUTE_HAL_D3D12;
#else
        other = 1;   /* the new path off Windows is Vulkan, later */
#endif
    } else if (same_name(e, "null")) {
#if defined(_WIN32)
        want = ROUTE_NULL;   /* (developers and CI only: never in Settings) */
#else
        other = 1;
#endif
    } else if (same_name(e, "vulkan")) {
#if defined(_WIN32) && defined(PS3RECOMP_RSX_HAL_VULKAN)
        want = ROUTE_VULKAN;   /* (Settings > Display > Renderer in the port) */
        if (mark_left()) {     /* (step V6: the last Vulkan start never finished) */
            want = ROUTE_D3D12;
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
    if (want == ROUTE_HAL_D3D12)
        printf("[rsx_out] route hal-d3d12 (the new path on its own D3D12 backend)\n");
    else if (want == ROUTE_NULL)
        printf("[rsx_out] route null (the new path on the null backend: checks every command, draws nothing)\n");
    else if (want == ROUTE_VULKAN)
        printf("[rsx_out] route vulkan (the new path on its Vulkan backend)\n");
    else if (left) {
        s_why ="the last start on Vulkan did not finish, so this one uses D3D12";
        printf("[rsx_out] route d3d12 (RSX_BACKEND=vulkan, but %s; the next start tries Vulkan again)\n", s_why);
    } else if (other) {
        snprintf(s_why_text, sizeof s_why_text, "RSX_BACKEND=%.32s is not in this build", e);
        s_why = s_why_text;
        printf("[rsx_out] route d3d12 (%s)\n", s_why);
    } else
        printf("[rsx_out] route d3d12\n");
    fflush(stdout);
    return want;
}

/* Step V6: the route's name without picking one. */
static const char* route_name(long r)
{
    switch (r) {
    case ROUTE_D3D12:     return "d3d12";
    case ROUTE_HAL_D3D12: return "hal-d3d12";
    case ROUTE_NULL:      return "null";
    case ROUTE_VULKAN:    return "vulkan";
    default:              return NULL;
    }
}

const char* rsx_out_renderer_info(const char** why)
{
    const long r = s_route;
    if (why) *why = r == ROUTE_D3D12 ? s_why : NULL;
    return route_name(r);
}

#if defined(_WIN32)
/* The new path's table on hal-d3d12, null and vulkan, NULL on d3d12. One
 * load and up to three compares once the route is picked. */
static const rsx_route* route_table(void)
{
    long r = s_route;
    if (r == ROUTE_NONE) r = route_pick();
#if defined(PS3RECOMP_RSX_HAL_VULKAN)
    if (r == ROUTE_VULKAN) return &rsxc_route_vk;
#endif
    return r == ROUTE_HAL_D3D12 ? &rsxc_route_d3d12 : r == ROUTE_NULL ? &rsxc_route_null : NULL;
}

const char* rsx_out_renderer(void)
{
    const rsx_route* const t = route_table();
    return t ? t->name : "d3d12";
}

#define RSX_OUT_V_(name, legacy, params, args) \
    void rsx_out_##name params { const rsx_route* const t_ = route_table(); if (t_) t_->name args; else legacy args; }
#define RSX_OUT_R_(ret, name, legacy, params, args) \
    ret rsx_out_##name params { const rsx_route* const t_ = route_table(); return t_ ? t_->name args : legacy args; }
#else
/* Nonzero on hal-d3d12 (never here). One load and a compare once the route is picked. */
static int route_hal(void)
{
    long r = s_route;
    if (r == ROUTE_NONE) r = route_pick();
    return r == ROUTE_HAL_D3D12;
}

const char* rsx_out_renderer(void)
{
    return route_hal() ? "hal-d3d12" : "d3d12";
}

/* No new path in this build yet: every call is today's renderer's. */
#define RSX_OUT_V_(name, legacy, params, args) \
    void rsx_out_##name params { (void)route_hal(); legacy args; }
#define RSX_OUT_R_(ret, name, legacy, params, args) \
    ret rsx_out_##name params { (void)route_hal(); return legacy args; }
#endif

#if defined(_WIN32) && defined(PS3RECOMP_RSX_HAL_VULKAN)
/* Step V6: three of the functions are written out in the section below (the
 * Vulkan start, and what today's renderer needs again if it falls back).
 * The table makes them under these names, file-local, for those to call. */
static int  rsx_out_plain_backend_init(uint32_t width, uint32_t height, const char* title);
static void rsx_out_plain_set_overlay(const struct rsx_d3d12_overlay* overlay);
static void rsx_out_plain_pso_index_game(const char* game);
#define rsx_out_backend_init   rsx_out_plain_backend_init
#define rsx_out_set_overlay    rsx_out_plain_set_overlay
#define rsx_out_pso_index_game rsx_out_plain_pso_index_game
#endif
RSX_OUT_FUNCS(RSX_OUT_V_, RSX_OUT_R_)
#if defined(_WIN32) && defined(PS3RECOMP_RSX_HAL_VULKAN)
#undef rsx_out_backend_init
#undef rsx_out_set_overlay
#undef rsx_out_pso_index_game

/* ---------------------------------------------------------------------------
 * Step V6: Vulkan's start, with today's renderer to fall back on
 *
 * Two kinds of failed start run today's renderer (d3d12) in Vulkan's place,
 * and say why (the line below, and rsx_out_renderer_info for a host's
 * report):
 *   - a start that fails at a step the core sees (rsxc_init.inc's
 *     start_step: loader, instance, adapter, features, device, window, swap
 *     chain, targets, shaders, rings): the core undoes what it made
 *     (rc_abort_init) and today's renderer starts in the same process.
 *       [rsx_out] route d3d12 (Vulkan failed to start: no Vulkan 1.3 driver (loader); ...)
 *   - a start that never finished (the process died in the driver): with
 *     RSX_STATE_DIR set (a folder of the host's that setup does not clear;
 *     the port's data folder), a Vulkan start writes
 *     RSX_STATE_DIR/renderer_starting first and removes it at its first
 *     present (rsx_out_route_presented). A start that finds it there uses
 *     d3d12 (route_pick's line), and any start not on Vulkan removes it, so
 *     the next one tries Vulkan again. A process that never starts the
 *     renderer leaves it. Without RSX_STATE_DIR (test runs) there is no mark.
 * -------------------------------------------------------------------------*/
#include <windows.h>

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

static volatile long s_marked;   /* 1 while this process's mark is there */

void rsx_out_route_presented(void)
{
    if (InterlockedExchange(&s_marked, 0)) mark_take();
}

/* What the host gives the renderer before its start, kept for today's
 * renderer should the Vulkan start fall back: the overlay and the game's
 * tag for the pipeline index. (Every route; only a fallback reads them.) */
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
        {"device",     "the Vulkan device could not be made"},
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

int rsx_out_backend_init(uint32_t width, uint32_t height, const char* title)
{
    if (route_table() != &rsxc_route_vk) {
        mark_take();   /* (one the last start left: this start is not Vulkan's, so it is done with) */
        return rsx_out_plain_backend_init(width, height, title);
    }
    InterlockedExchange(&s_marked, mark_put());
    if (rsxc_route_vk.backend_init(width, height, title) == 0) return 0;   /* (the mark goes at the first present) */
    if (InterlockedExchange(&s_marked, 0)) mark_take();   /* (a failure the core saw is no crash) */
    const char* step = rsxc_vk_start_failed();
    snprintf(s_why_text, sizeof s_why_text, "Vulkan failed to start: %s (%s)", step_words(step), step ? step : "?");
    s_why = s_why_text;
    InterlockedExchange(&s_route, ROUTE_D3D12);
    printf("[rsx_out] route d3d12 (%s; this start uses D3D12)\n", s_why);
    fflush(stdout);
    if (s_pre_overlay_set) rsx_d3d12_set_overlay(&s_pre_overlay);
    if (s_pre_game[0]) rsx_d3d12_pso_index_game(s_pre_game);
    return rsx_d3d12_backend_init(width, height, title);
}
#endif /* _WIN32 && PS3RECOMP_RSX_HAL_VULKAN */

#endif /* PS3RECOMP_RSX_HAL && (_WIN32 || PS3RECOMP_RSX_D3D12_VKD3D) */
