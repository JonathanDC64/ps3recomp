/*
 * ps3recomp - the renderer's front: the route switch (see rsx_out.h)
 *
 * Built with PS3RECOMP_RSX_HAL wherever today's D3D12 renderer is (Windows,
 * or vkd3d off it). The route is picked once, on the first rsx_out call,
 * from RSX_BACKEND; until then nothing of either renderer has run. On the
 * d3d12 route each rsx_out_X calls rsx_d3d12_X; on hal-d3d12 and null it
 * calls the new path's table for that backend, rsxc_route_d3d12 or
 * rsxc_route_null (Windows only for now).
 */
#if defined(PS3RECOMP_RSX_HAL) && (defined(_WIN32) || defined(PS3RECOMP_RSX_D3D12_VKD3D))

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   /* getenv */
#endif
#include "rsx_out.h"
#include "rsx_d3d12_backend.h"
#include <stdio.h>
#include <stdlib.h>

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

enum { ROUTE_NONE, ROUTE_D3D12, ROUTE_HAL_D3D12, ROUTE_NULL };
static volatile long s_route;   /* ROUTE_*: none until the first call */

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
    int other = 0;
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
    } else {
        other = 1;
    }
    const long had = RSX_OUT_CAS(&s_route, (long)ROUTE_NONE, want);
    if (had != ROUTE_NONE) return had;
    if (want == ROUTE_HAL_D3D12)
        printf("[rsx_out] route hal-d3d12 (the new path on its own D3D12 backend)\n");
    else if (want == ROUTE_NULL)
        printf("[rsx_out] route null (the new path on the null backend: checks every command, draws nothing)\n");
    else if (other)
        printf("[rsx_out] route d3d12 (RSX_BACKEND=%.32s is not in this build)\n", e);
    else
        printf("[rsx_out] route d3d12\n");
    fflush(stdout);
    return want;
}

#if defined(_WIN32)
/* The new path's table on hal-d3d12 and null, NULL on d3d12. One load and
 * two compares once the route is picked. */
static const rsx_route* route_table(void)
{
    long r = s_route;
    if (r == ROUTE_NONE) r = route_pick();
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
RSX_OUT_FUNCS(RSX_OUT_V_, RSX_OUT_R_)

#endif /* PS3RECOMP_RSX_HAL && (_WIN32 || PS3RECOMP_RSX_D3D12_VKD3D) */
