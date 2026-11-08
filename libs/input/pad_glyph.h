/* The device whose button icons a title shows: the last one used on port 0 (cellPad.c, cellPad_host_glyph_device).
 * In a header of its own, with no pad, OS or SDL in it, so tests/test_pad_glyph.c can run the rules.
 *
 * Each poll gives a sample: what the pad on port 0 is (its kind), its own buttons, sticks and triggers before the
 * keyboard is merged in, and whether a key or mouse button went down. pad_glyph_step says which device was used:
 *   - the pad, when one of its buttons goes down, or a stick or trigger goes past half way;
 *   - the keyboard and mouse, when a key or mouse button goes down and the pad did nothing new in the same poll (a
 *     tie goes to the pad);
 *   - otherwise the device stays as it was.
 * Only changes count: a button held down, a stick resting where it was (drift) or a trigger resting half down change
 * nothing, so a pad lying beside a player on the keyboard does not take the icons back. Mouse movement alone never
 * changes the device: a player with a pad may turn the camera with the mouse. Before anything is used, the device is
 * the pad on port 0, or the keyboard when there is none. */
#ifndef PS3RECOMP_PAD_GLYPH_H
#define PS3RECOMP_PAD_GLYPH_H

enum {
    PAD_GLYPH_NONE = 0,        /* nothing known yet (or no user input is read) */
    PAD_GLYPH_KEYBOARD = 1,    /* keyboard and mouse */
    PAD_GLYPH_PLAYSTATION = 2, /* DualShock 3/4, DualSense */
    PAD_GLYPH_XBOX = 3,        /* XInput pads, and any pad of no known kind */
    PAD_GLYPH_NINTENDO = 4     /* Switch Pro, Joy-Cons */
};

#define PAD_GLYPH_STICK   64   /* a stick counts from half way out (of 128) */
#define PAD_GLYPH_TRIGGER 128  /* a trigger from half way down (of 255) */

typedef struct {
    int pad_kind;              /* PAD_GLYPH_PLAYSTATION, _XBOX or _NINTENDO for port 0's pad; PAD_GLYPH_NONE: none */
    unsigned pad_buttons;      /* the pad's own buttons (CELL_PAD_CTRL_* bits), before the keyboard */
    unsigned char lx, ly, rx, ry;  /* its sticks, 128 the centre */
    unsigned char l2, r2;      /* its triggers */
    int keys_fresh;            /* a bound key or a mouse button went down this poll */
} PadGlyphSample;

typedef struct {
    int device;                /* PAD_GLYPH_*: the answer */
    unsigned prev_buttons;     /* the pad's buttons at the last poll */
    unsigned prev_far;         /* its sticks and triggers past the line at the last poll, a bit each */
} PadGlyphState;

static unsigned pad_glyph_far(const PadGlyphSample* s)
{
    const int dx[4] = {s->lx - 128, s->ly - 128, s->rx - 128, s->ry - 128};
    unsigned past = 0;
    for (int i = 0; i < 4; i++)
        if (dx[i] >= PAD_GLYPH_STICK || dx[i] <= -PAD_GLYPH_STICK) past |= 1u << i;
    if (s->l2 >= PAD_GLYPH_TRIGGER) past |= 1u << 4;
    if (s->r2 >= PAD_GLYPH_TRIGGER) past |= 1u << 5;
    return past;
}

/* One poll's sample in; the device after it out (also in st->device). */
static int pad_glyph_step(PadGlyphState* st, const PadGlyphSample* s)
{
    const int pad = s->pad_kind != PAD_GLYPH_NONE;
    const unsigned buttons = pad ? s->pad_buttons : 0;
    const unsigned past = pad ? pad_glyph_far(s) : 0;
    const int pad_new = pad && ((buttons & ~st->prev_buttons) || (past & ~st->prev_far));
    st->prev_buttons = buttons;
    st->prev_far = past;
    if (pad_new)
        st->device = s->pad_kind;
    else if (s->keys_fresh)
        st->device = PAD_GLYPH_KEYBOARD;
    else if (st->device == PAD_GLYPH_NONE)
        st->device = pad ? s->pad_kind : PAD_GLYPH_KEYBOARD;
    return st->device;
}

#endif
