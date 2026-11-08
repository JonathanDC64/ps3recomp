/*
 * ps3recomp - a port's own image in place of one of the title's textures (rsx_texture_override.h)
 *
 * The watched shapes, their hooks and the count of changes. One copy for every route: the core is built once per
 * backend (rsxc_d3d12.c, rsxc_vk.c, rsxc_null.c each include rsxc_core.inc), and each instance asks here.
 */
#include "rsx_texture_override.h"

#include <stddef.h>

/* Swap and add, full barriers: the compiler's own (this file needs nothing else of the platform's). */
#if defined(_MSC_VER)
#include <intrin.h>
#define TOV_XCHG(p, v) _InterlockedExchange((p), (v))
#define TOV_INC(p) _InterlockedIncrement(p)
#else
#define TOV_XCHG(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define TOV_INC(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#endif

typedef struct {
    uint32_t width, height, fmt;
    rsx_texture_override_fn fn;
    void* user;
} Shape;

static Shape s_shapes[RSX_TEXTURE_OVERRIDE_SHAPES];
static volatile long s_count;       /* shapes in use (a removed one stays, with no hook) */
static volatile long s_lock;        /* the table's writers and the renderer's ask */
static volatile long s_epoch = 1;

static void lock(void)
{
    while (TOV_XCHG(&s_lock, 1))
        ;
}

static void unlock(void)
{
    TOV_XCHG(&s_lock, 0);
}

int rsx_texture_override_watch(uint32_t width, uint32_t height, uint32_t fmt, rsx_texture_override_fn fn,
                               void* user)
{
    fmt &= 0x9Fu;
    int ok = 0;
    lock();
    for (long i = 0; i < s_count; i++) {
        Shape* s = &s_shapes[i];
        if (s->width == width && s->height == height && s->fmt == fmt) {
            s->fn = fn;
            s->user = user;
            ok = 1;
            break;
        }
    }
    if (!ok && fn && s_count < RSX_TEXTURE_OVERRIDE_SHAPES) {
        Shape* s = &s_shapes[s_count];
        s->width = width;
        s->height = height;
        s->fmt = fmt;
        s->fn = fn;
        s->user = user;
        TOV_INC(&s_count);
        ok = 1;
    }
    unlock();
    if (ok) rsx_texture_override_changed();   /* a texture of the shape asked the old hook (or none) */
    return ok || !fn;
}

void rsx_texture_override_changed(void)
{
    TOV_INC(&s_epoch);
}

uint32_t rsx_texture_override_epoch(void)
{
    return (uint32_t)s_epoch;
}

/* The hook for a shape, and its user; NULL fn when none. */
static rsx_texture_override_fn find(uint32_t width, uint32_t height, uint32_t fmt, void** user)
{
    rsx_texture_override_fn fn = NULL;
    *user = NULL;
    if (!s_count) return NULL;   /* nothing watched: the usual case, no lock */
    fmt &= 0x9Fu;
    lock();
    for (long i = 0; i < s_count; i++) {
        const Shape* s = &s_shapes[i];
        if (s->width == width && s->height == height && s->fmt == fmt) {
            fn = s->fn;
            *user = s->user;
            break;
        }
    }
    unlock();
    return fn;
}

int rsx_texture_override_watched(uint32_t width, uint32_t height, uint32_t fmt)
{
    void* user;
    return find(width, height, fmt, &user) != NULL;
}

int rsx_texture_override_ask(const uint8_t* guest, uint32_t bytes, uint32_t width, uint32_t height, uint32_t fmt,
                             rsx_texture_override_image* out)
{
    void* user;
    const rsx_texture_override_fn fn = find(width, height, fmt, &user);
    if (!fn) return -1;
    out->rgba = NULL;
    out->width = out->height = out->levels = 0;
    if (!fn(user, guest, bytes, width, height, fmt, out)) return 0;
    if (!out->rgba || !out->width || !out->height) return 0;
    if (!out->levels) out->levels = 1;
    return 1;
}
