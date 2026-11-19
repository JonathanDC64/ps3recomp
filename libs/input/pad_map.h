/* PAD_MAP's rules (cellPad.c, pad_apply_map): which of a host pad's buttons press each of the game's 16. In a header
 * of their own, with no pad, OS or SDL in them, so tests/test_pad_map.c can run them.
 *
 * A host pad's buttons are one word here: bit i (0-15) the PS3 button i, in CELL_PAD_CTRL_* order, and above them,
 * from bit 16, the buttons some pads have beyond the PS3's (a pad's `extra`, PAD_EXTRA_*). The names are cellPad's
 * (s_pad_names) and then these:
 *   paddle1 paddle2 paddle3 paddle4   SDL's paddles: upper right, upper left, lower right, lower left as the pad is
 *                                     held (Xbox Elite P1 P3 P2 P4; the DualSense Edge's right and left back buttons,
 *                                     then its right and left Fn buttons, as SDL has them)
 *   misc                              SDL's misc1: the Xbox Series Share button, the DualSense's mute button, the
 *                                     Switch Pro's Capture button
 *   touchpad                          a PlayStation pad's touchpad click, where the pad reader doesn't make it Select
 *                                     (SDL; the Windows reader of Sony pads makes it Select)
 * A pad that doesn't have one, or whose reader can't see it (XInput sees none), never presses it.
 *
 * A PAD_MAP value is one name, several joined by '|' (any of them presses the game's button), or "none" (nothing
 * does). (Inline: pad_hid_sony.c takes only the PAD_EXTRA_* bits.) */
#ifndef PS3RECOMP_PAD_MAP_H
#define PS3RECOMP_PAD_MAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PAD_EXTRA_PADDLE1  0x01u
#define PAD_EXTRA_PADDLE2  0x02u
#define PAD_EXTRA_PADDLE3  0x04u
#define PAD_EXTRA_PADDLE4  0x08u
#define PAD_EXTRA_MISC     0x10u
#define PAD_EXTRA_TOUCHPAD 0x20u

#define PAD_MAP_GAME  16   /* the game's buttons */
#define PAD_MAP_NAMES 22   /* the host's: the 16, then the extra ones */

static const char* const pad_map_names[PAD_MAP_NAMES] = {
    "select", "l3", "r3", "start", "up", "right", "down", "left",
    "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square",
    "paddle1", "paddle2", "paddle3", "paddle4", "misc", "touchpad",
};

/* Case-insensitive: the n characters at s spell name (lower case). */
static inline int pad_map_is(const char* s, size_t n, const char* name)
{
    size_t k = 0;
    while (k < n && name[k] && ((s[k] >= 'A' && s[k] <= 'Z') ? (s[k] | 0x20) : s[k]) == name[k]) k++;
    return k == n && !name[k];
}

/* The host button the n characters at s name (an index into pad_map_names, and its bit in a host word), or -1. */
static inline int pad_map_name(const char* s, size_t n)
{
    for (int i = 0; i < PAD_MAP_NAMES; i++)
        if (pad_map_is(s, n, pad_map_names[i])) return i;
    return -1;
}

/* A PAD_MAP value's host buttons, a host word; *ok 0 when any part of it isn't a name (the value is then left out). */
static inline uint32_t pad_map_value(const char* v, size_t n, int* ok)
{
    uint32_t mask = 0;
    *ok = n > 0;
    if (pad_map_is(v, n, "none")) return 0;
    const char* end = v + n;
    const char* bar;
    do {   /* (each part a name: "r1|" and "r1||l1" are not values) */
        bar = v;
        while (bar < end && *bar != '|') bar++;
        const int i = pad_map_name(v, (size_t)(bar - v));
        if (i < 0) *ok = 0;
        else mask |= 1u << i;
        v = bar + 1;
    } while (bar < end);
    return *ok ? mask : 0;
}

/* The identity: each game button pressed by the host's own. */
static inline void pad_map_identity(uint32_t src[PAD_MAP_GAME])
{
    for (int i = 0; i < PAD_MAP_GAME; i++) src[i] = 1u << i;
}

/* A value as PAD_MAP writes it, into out (n bytes): "r1", "r1|paddle1", "none". */
static inline void pad_map_text(uint32_t mask, char* out, size_t n)
{
    size_t used = 0;
    if (!n) return;
    out[0] = '\0';
    if (!mask) { snprintf(out, n, "none"); return; }
    for (int i = 0; i < PAD_MAP_NAMES; i++) {
        if (!(mask & (1u << i))) continue;
        const int w = snprintf(out + used, n - used, "%s%s", used ? "|" : "", pad_map_names[i]);
        if (w < 0 || (size_t)w >= n - used) break;
        used += (size_t)w;
    }
}

/* The game's buttons (CELL_PAD_CTRL_*) from a host word `in`: game button i is down when any of src[i] is. L2 and R2's
 * analog values (l2, r2 the host's) follow: the most of the host triggers among its sources, and 255 while another of
 * them is down, so a game trigger read from one host trigger keeps its travel. */
static inline uint16_t pad_map_apply(const uint32_t src[PAD_MAP_GAME], uint32_t in, uint8_t l2, uint8_t r2,
                                     uint8_t* out_l2, uint8_t* out_r2)
{
    uint16_t out = 0;
    for (int i = 0; i < PAD_MAP_GAME; i++)
        if (in & src[i]) out |= (uint16_t)(1u << i);
    for (int t = 8; t <= 9; t++) {   /* 8 = l2, 9 = r2 */
        const uint32_t s = src[t];
        uint8_t v = 0;
        if ((s & (1u << 8)) && l2 > v) v = l2;
        if ((s & (1u << 9)) && r2 > v) v = r2;
        if (in & s & ~((1u << 8) | (1u << 9))) v = 255;
        *(t == 8 ? out_l2 : out_r2) = v;
    }
    return out;
}

#endif
