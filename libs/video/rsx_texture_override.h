/*
 * ps3recomp - a port's own image in place of one of the title's textures
 *
 * A port can draw its own art where the title draws one of its textures: Demon's Souls' port puts the buttons of
 * the pad the player is using (or the keys they bound) on the font page that holds the PS3's buttons. The renderer
 * (the new path's core, libs/video/rsxcore/rsxc_texture.inc) asks the port when a texture of a watched shape
 * uploads: its width, height and format class (fmt & 0x9F). The port looks at the guest's bytes and gives an image
 * in their place, or nothing to keep the title's.
 *
 * The image is RGBA8, of any size: one larger than the title's draws sharper on a scaled render. The title's own
 * filters sample it, so its smaller levels are read only where the title's sampler uses mipmaps. Its channels are
 * what the title's texture holds once decoded (R, G, B, A), so the title's channel swizzle applies as it is.
 *
 * The answer stays with the texture until the guest's bytes change or the port calls
 * rsx_texture_override_changed (its image changed: the player picked up another pad); the texture's next bind
 * asks again. The hook runs on the renderer's thread while it records a frame, so it should hand over an image
 * made beforehand. The retired legacy renderer (rsx_d3d12_backend.c) does not ask.
 */
#ifndef PS3RECOMP_RSX_TEXTURE_OVERRIDE_H
#define PS3RECOMP_RSX_TEXTURE_OVERRIDE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_texture_override_image {
    const void* rgba;   /* RGBA8: level 0, then each smaller level, rows of width * 4 bytes, no padding */
    uint32_t width;
    uint32_t height;
    uint32_t levels;    /* 1: level 0 only. Each level halves the last, down to 1 */
} rsx_texture_override_image;

/* 1 with *out filled: draw that image in place of the guest's (the renderer copies it before returning). 0: keep
 * the guest's. guest is the texture's level 0 as the title stored it (bytes long); fmt its whole format byte. */
typedef int (*rsx_texture_override_fn)(void* user, const uint8_t* guest, uint32_t bytes, uint32_t width,
                                       uint32_t height, uint32_t fmt, rsx_texture_override_image* out);

#define RSX_TEXTURE_OVERRIDE_SHAPES 8

/* Ask fn about textures of this width, height and format class (fmt & 0x9F); not cube maps. Again for the same
 * shape replaces its hook, and a NULL fn stops asking. 0 when all RSX_TEXTURE_OVERRIDE_SHAPES are taken. Any
 * thread, before or after the renderer starts. */
int rsx_texture_override_watch(uint32_t width, uint32_t height, uint32_t fmt, rsx_texture_override_fn fn,
                               void* user);

/* The port's image changed: each watched texture asks again at its next bind. Any thread. */
void rsx_texture_override_changed(void);

/* For the renderer. The count of changes (watch and changed calls), 1 at the start. */
uint32_t rsx_texture_override_epoch(void);
/* 1 when a hook is watching textures of this shape. */
int rsx_texture_override_watched(uint32_t width, uint32_t height, uint32_t fmt);
/* -1: the shape is not watched (nothing was asked); 0: keep the guest's; 1: *out holds the image to draw. */
int rsx_texture_override_ask(const uint8_t* guest, uint32_t bytes, uint32_t width, uint32_t height, uint32_t fmt,
                             rsx_texture_override_image* out);

#ifdef __cplusplus
}
#endif

#endif
