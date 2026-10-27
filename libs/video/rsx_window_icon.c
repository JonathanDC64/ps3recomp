/*
 * ps3recomp - the window's icon from the game's own ICON0.PNG (rsx_window_icon.h).
 */
#include "rsx_window_icon.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "../codec/stb_image.h"   /* declarations only: cellPngDec.c holds STB_IMAGE_IMPLEMENTATION */

/* The whole file (at most 16 MB), or NULL. The path is UTF-8, so on Windows it
 * is opened by its UTF-16 name. */
static unsigned char* read_file(const char* path, size_t* size)
{
    FILE* f = NULL;
#ifdef _WIN32
    wchar_t wide[4096];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, (int)(sizeof wide / sizeof wide[0])) > 0)
        f = _wfopen(wide, L"rb");
#else
    f = fopen(path, "rb");
#endif
    if (!f) return NULL;
    unsigned char* data = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n > 0 && n <= 16L * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0 && (data = (unsigned char*)malloc((size_t)n))) {
        if (fread(data, 1, (size_t)n, f) == (size_t)n) {
            *size = (size_t)n;
        } else {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    return data;
}

/* The picture, decoded once: RGBA, s_w x s_h. Only the window's thread asks
 * for it, when the window opens. */
static int s_tried;
static unsigned char* s_rgba;
static int s_w, s_h;

static void load_once(void)
{
    if (s_tried) return;
    s_tried = 1;
    const char* knob = getenv("RSX_ICON");
    if (knob && !strcmp(knob, "0")) return;
    char path[4096];
    if (knob && knob[0]) {
        snprintf(path, sizeof path, "%s", knob);
    } else {
        const char* root = getenv("PS3_VFS_ROOT");
        if (!root || !root[0]) return;
        snprintf(path, sizeof path, "%s/PS3_GAME/ICON0.PNG", root);
    }
    size_t n = 0;
    unsigned char* file = read_file(path, &n);
    if (!file) return;
    int comp = 0;
    s_rgba = stbi_load_from_memory(file, (int)n, &s_w, &s_h, &comp, 4);
    free(file);
    if (s_rgba && (s_w <= 0 || s_h <= 0)) {
        stbi_image_free(s_rgba);
        s_rgba = NULL;
    }
    if (s_rgba) printf("[rsx] window icon: %s (%dx%d)\n", path, s_w, s_h);
    else printf("[rsx] window icon: %s is not a picture\n", path);
}

int rsx_window_icon(int size, uint8_t* out)
{
    load_once();
    if (!s_rgba || size <= 0 || !out) return 0;
    /* The square the picture is centred on, and the part of it each output
     * pixel covers: its pixels are averaged by the area each covers, weighted
     * by alpha (premultiplied), the padding counting as transparent. */
    const int side = s_w > s_h ? s_w : s_h;
    const double ox = (side - s_w) / 2.0, oy = (side - s_h) / 2.0;
    const double step = (double)side / size, area = step * step;
    for (int j = 0; j < size; j++) {
        const double y0 = j * step - oy, y1 = y0 + step;   /* in the picture's rows */
        for (int i = 0; i < size; i++) {
            const double x0 = i * step - ox, x1 = x0 + step;
            double r = 0, g = 0, b = 0, a = 0;
            for (int sy = (int)floor(y0); sy < y1; sy++) {
                if (sy < 0 || sy >= s_h) continue;
                const double wy = fmin(y1, sy + 1.0) - fmax(y0, (double)sy);
                if (wy <= 0) continue;
                for (int sx = (int)floor(x0); sx < x1; sx++) {
                    if (sx < 0 || sx >= s_w) continue;
                    const double wx = fmin(x1, sx + 1.0) - fmax(x0, (double)sx);
                    if (wx <= 0) continue;
                    const unsigned char* p = s_rgba + ((size_t)sy * (size_t)s_w + (size_t)sx) * 4;
                    const double w = wx * wy * p[3];
                    r += w * p[0];
                    g += w * p[1];
                    b += w * p[2];
                    a += w;
                }
            }
            uint8_t* o = out + ((size_t)j * (size_t)size + (size_t)i) * 4;
            if (a > 0) {
                o[0] = (uint8_t)(r / a + 0.5);
                o[1] = (uint8_t)(g / a + 0.5);
                o[2] = (uint8_t)(b / a + 0.5);
                o[3] = (uint8_t)fmin(255.0, a / area + 0.5);
            } else {
                memset(o, 0, 4);
            }
        }
    }
    return 1;
}
