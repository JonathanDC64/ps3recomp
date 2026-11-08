/* The rules for which device's button icons to show (pad_glyph.h); no pad, OS or SDL needed.
 * cc -std=c17 -I libs/input libs/input/tests/test_pad_glyph.c -o test_pad_glyph
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>   /* before the header, as cellPad.c has it: its old macros (far, near) must not clash */
#endif
#include "pad_glyph.h"

#define CROSS 0x4000u
#define CIRCLE 0x2000u

static PadGlyphSample rest(int kind)
{
    PadGlyphSample s;
    memset(&s, 0, sizeof s);
    s.pad_kind = kind;
    s.lx = s.ly = s.rx = s.ry = 128;
    return s;
}

int main(void) {
    PadGlyphState st;
    PadGlyphSample s;

    /* Before anything is used: port 0's pad, or the keyboard without one. */
    memset(&st, 0, sizeof st);
    s = rest(PAD_GLYPH_XBOX);
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_XBOX);
    memset(&st, 0, sizeof st);
    s = rest(PAD_GLYPH_NONE);
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_KEYBOARD);
    puts("PASS: the start: the pad on port 0, else the keyboard");

    /* A key makes it the keyboard; a pad button takes it back; holding either changes nothing. */
    memset(&st, 0, sizeof st);
    s = rest(PAD_GLYPH_PLAYSTATION);
    pad_glyph_step(&st, &s);
    s.keys_fresh = 1;
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_KEYBOARD);
    s.keys_fresh = 0;
    s.pad_buttons = CROSS;
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_PLAYSTATION);
    s.keys_fresh = 1;   /* Cross still held: not new, so the key wins */
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_KEYBOARD);
    s.keys_fresh = 0;
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_KEYBOARD);   /* still held, nothing new */
    s.pad_buttons = CROSS | CIRCLE;   /* another button goes down while Cross is held */
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_PLAYSTATION);
    puts("PASS: keys and buttons that go down decide; held ones do not");

    /* A tie in one poll goes to the pad. */
    memset(&st, 0, sizeof st);
    s = rest(PAD_GLYPH_XBOX);
    s.keys_fresh = 1;
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_KEYBOARD);
    s.pad_buttons = CROSS;
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_XBOX);
    puts("PASS: a key and a button in the same poll: the pad");

    /* Sticks and triggers count from half way, once; drift and a resting trigger do not. */
    memset(&st, 0, sizeof st);
    s = rest(PAD_GLYPH_XBOX);
    pad_glyph_step(&st, &s);
    s.keys_fresh = 1;
    pad_glyph_step(&st, &s);
    s.keys_fresh = 0;
    s.lx = 128 + 40;   /* drift */
    s.l2 = 100;        /* a trigger resting part way */
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_KEYBOARD);
    s.ry = 128 - 64;   /* the right stick pushed up half way */
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_XBOX);
    s.keys_fresh = 1;
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_KEYBOARD);   /* the stick held there: not new */
    s.keys_fresh = 0;
    s.r2 = 200;        /* a trigger pulled */
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_XBOX);
    s.keys_fresh = 1;
    pad_glyph_step(&st, &s);
    s.keys_fresh = 0;
    s.ry = 128;        /* the stick let go, then pushed again */
    pad_glyph_step(&st, &s);
    s.ry = 255;
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_XBOX);
    puts("PASS: sticks and triggers count from half way, and only on the way out");

    /* Another pad on port 0: its kind once it is used, not before. */
    memset(&st, 0, sizeof st);
    s = rest(PAD_GLYPH_XBOX);
    s.pad_buttons = CROSS;
    pad_glyph_step(&st, &s);
    s = rest(PAD_GLYPH_NINTENDO);
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_XBOX);
    s.pad_buttons = CIRCLE;
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_NINTENDO);
    puts("PASS: a new pad's kind once it is pressed");

    /* The pad unplugged while it was the device: it stays until a key goes down. */
    s = rest(PAD_GLYPH_NONE);
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_NINTENDO);
    s.keys_fresh = 1;
    assert(pad_glyph_step(&st, &s) == PAD_GLYPH_KEYBOARD);
    puts("PASS: no pad: the keyboard once a key is pressed");
    return 0;
}
