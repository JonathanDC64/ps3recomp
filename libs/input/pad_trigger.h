/* L2 and R2 as buttons, from how far each trigger is in. In a header of its own, with no pad, OS or SDL in it, so
 * tests/test_pad_trigger.c can run the rules.
 *
 * Every reader presses L2 or R2 only past a threshold, the same on every kind of pad:
 *   - XInput, a trigger's 0-255 over 30 (about 12% of its travel);
 *   - SDL3, its 0-32767 over 3000 (about 9%);
 *   - Sony's pads over HID (pad_hid_sony.c), the 0-255 over 30, as XInput. Not the pad's own L2/R2 bit: a DualShock 4
 *     or DualSense sets that at the first movement, so a trigger that rests a little in (a worn one, or a finger on
 *     it) holds the button down, and a shake that moves it presses it (a player's report: R2 down by itself, and a
 *     strong attack when the pad was shaken).
 *
 * PAD_TRIGGER_THRESHOLD (0-254, of 255) sets it for every reader, for a trigger that rests further in. Unset or empty:
 * the numbers above. Live. */
#ifndef PS3RECOMP_PAD_TRIGGER_H
#define PS3RECOMP_PAD_TRIGGER_H

#include <stdlib.h>

#define PAD_TRIGGER_DEFAULT_255   30     /* XInput's and Sony's, of 255 */
#define PAD_TRIGGER_DEFAULT_32767 3000   /* SDL3's, of 32767 */

/* PAD_TRIGGER_THRESHOLD's text as 0-254; -1 for unset, empty or not a number (the defaults above). */
static inline int pad_trigger_knob(const char* s)
{
    if (!s || !*s) return -1;
    char* end = NULL;
    const long v = strtol(s, &end, 10);
    if (end == s) return -1;
    while (*end == ' ') end++;
    if (*end) return -1;
    return v < 0 ? 0 : v > 254 ? 254 : (int)v;
}

/* Down: v (0-255) over the knob's threshold (pad_trigger_knob), or the default. */
static inline int pad_trigger_down_255(unsigned v, int knob)
{
    return (int)v > (knob >= 0 ? knob : PAD_TRIGGER_DEFAULT_255);
}

/* Down: v (SDL3's 0-32767) over the knob's threshold taken to that range, or SDL3's default. */
static inline int pad_trigger_down_32767(int v, int knob)
{
    return v > (knob >= 0 ? (knob * 32767 + 127) / 255 : PAD_TRIGGER_DEFAULT_32767);
}

#endif
