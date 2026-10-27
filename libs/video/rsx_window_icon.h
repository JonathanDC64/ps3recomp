/*
 * ps3recomp - the window's icon from the game's own icon, PS3_GAME/ICON0.PNG,
 * read when the window opens.
 *
 * A port's exe may carry an icon of its own (icon resource 1 on Windows). When
 * it has none, as an exe built without the game's files has not, the window
 * shows the game's icon. The disc's ICON0.PNG is 320x176: it is centred on a
 * transparent square, then scaled down (area average, premultiplied alpha).
 *
 * RSX_ICON=<png> takes another picture, RSX_ICON=0 none. The default is
 * $PS3_VFS_ROOT/PS3_GAME/ICON0.PNG.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The icon as size x size RGBA, straight alpha, into out (size * size * 4
 * bytes). 1 when there is one, 0 otherwise (no file, or not a picture). The
 * file is read and decoded once; each size is scaled from that copy. */
int rsx_window_icon(int size, uint8_t* out);

#ifdef __cplusplus
}
#endif

#ifdef SDL_h_   /* (with SDL.h included first) an SDL window's icon, 64 pixels square */
static inline void rsx_window_icon_sdl(SDL_Window* w)
{
    uint8_t rgba[64 * 64 * 4];
    if (!w || !rsx_window_icon(64, rgba)) return;
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(rgba, 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_RGBA32);
    if (!s) return;
    SDL_SetWindowIcon(w, s);   /* SDL copies the pixels */
    SDL_FreeSurface(s);
}
#endif
