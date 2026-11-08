/* The table of a port's texture hooks (rsx_texture_override.c): which shapes are asked, what the renderer is
 * told, and the count that makes it ask again. No renderer or GPU: the renderer's side is in rsxc_texture.inc.
 *   cmake --build build --target test_rsx_texture_override && ctest --test-dir build -R rsx_texture_override
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rsx_texture_override.h"

static unsigned char s_page[4 * 4 * 4 + 2 * 2 * 4 + 4];
static int s_calls;
static unsigned s_last_fmt;
static const uint8_t* s_last_guest;

static int give(void* user, const uint8_t* guest, uint32_t bytes, uint32_t w, uint32_t h, uint32_t fmt,
                rsx_texture_override_image* out)
{
    (void)bytes; (void)w; (void)h;
    s_calls++;
    s_last_fmt = fmt;
    s_last_guest = guest;
    if (guest[0] != *(const uint8_t*)user) return 0;   /* only the page it knows */
    out->rgba = s_page;
    out->width = 4;
    out->height = 4;
    out->levels = 3;
    return 1;
}

static int keep(void* user, const uint8_t* guest, uint32_t bytes, uint32_t w, uint32_t h, uint32_t fmt,
                rsx_texture_override_image* out)
{
    (void)user; (void)guest; (void)bytes; (void)w; (void)h; (void)fmt; (void)out;
    s_calls++;
    return 0;
}

static int no_image(void* user, const uint8_t* guest, uint32_t bytes, uint32_t w, uint32_t h, uint32_t fmt,
                    rsx_texture_override_image* out)
{
    (void)user; (void)guest; (void)bytes; (void)w; (void)h; (void)fmt;
    out->width = 4;   /* says yes, gives nothing */
    return 1;
}

int main(void)
{
    rsx_texture_override_image im;
    const uint8_t mine[16] = {0xA5}, other[16] = {0x11};
    static uint8_t key = 0xA5;

    /* Nothing watched: nothing asked. */
    assert(rsx_texture_override_epoch() == 1);
    assert(!rsx_texture_override_watched(512, 512, 0x88));
    assert(rsx_texture_override_ask(mine, 16, 512, 512, 0x88, &im) == -1);
    puts("PASS: nothing watched: -1 and the hook-free count");

    /* A shape watched: its textures are asked, by the class of the format (fmt & 0x9F), with the whole byte. */
    assert(rsx_texture_override_watch(512, 512, 0x88, give, &key));
    assert(rsx_texture_override_epoch() == 2);
    assert(rsx_texture_override_watched(512, 512, 0x88));
    assert(rsx_texture_override_watched(512, 512, 0xA8));   /* the linear bit (0x20) is not the class */
    assert(!rsx_texture_override_watched(512, 256, 0x88));
    assert(!rsx_texture_override_watched(512, 512, 0x86));
    assert(rsx_texture_override_ask(mine, 16, 512, 512, 0xA8, &im) == 1);
    assert(s_calls == 1 && s_last_fmt == 0xA8 && s_last_guest == mine);
    assert(im.rgba == s_page && im.width == 4 && im.height == 4 && im.levels == 3);
    assert(rsx_texture_override_ask(other, 16, 512, 512, 0x88, &im) == 0);
    assert(rsx_texture_override_ask(mine, 16, 256, 256, 0x88, &im) == -1);
    assert(s_calls == 2);
    puts("PASS: a watched shape's textures are asked; the hook keeps or replaces");

    /* The port's image changed: the count moves. */
    rsx_texture_override_changed();
    assert(rsx_texture_override_epoch() == 3);
    puts("PASS: changed moves the count");

    /* The same shape again replaces the hook; a NULL hook stops asking. */
    assert(rsx_texture_override_watch(512, 512, 0x88, keep, NULL));
    assert(rsx_texture_override_epoch() == 4);
    assert(rsx_texture_override_ask(mine, 16, 512, 512, 0x88, &im) == 0);
    assert(s_calls == 3);
    assert(rsx_texture_override_watch(512, 512, 0x88, NULL, NULL));
    assert(!rsx_texture_override_watched(512, 512, 0x88));
    assert(rsx_texture_override_ask(mine, 16, 512, 512, 0x88, &im) == -1);
    assert(s_calls == 3);
    assert(rsx_texture_override_watch(64, 64, 0x85, NULL, NULL));   /* removing one never watched */
    puts("PASS: a second hook replaces the first; NULL stops asking");

    /* A yes without an image keeps the guest's. */
    assert(rsx_texture_override_watch(128, 128, 0x86, no_image, NULL));
    assert(rsx_texture_override_ask(mine, 16, 128, 128, 0x86, &im) == 0);
    puts("PASS: a yes with no image keeps the guest's");

    /* The table holds RSX_TEXTURE_OVERRIDE_SHAPES (the removed one keeps its place). */
    int n = 2;   /* 512x512 (removed) and 128x128 */
    for (uint32_t s = 1; n < RSX_TEXTURE_OVERRIDE_SHAPES; s++, n++)
        assert(rsx_texture_override_watch(s, s, 0x85, keep, NULL));
    assert(!rsx_texture_override_watch(1000, 1000, 0x85, keep, NULL));
    assert(rsx_texture_override_watch(512, 512, 0x88, give, &key));   /* its old place again */
    assert(rsx_texture_override_ask(mine, 16, 512, 512, 0x88, &im) == 1);
    puts("PASS: a full table says so; a shape's place is kept");
    return 0;
}
