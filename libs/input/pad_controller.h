/* PAD_CONTROLLER's rules (cellPad.c): which pad is port 0. In a header of
 * their own, with no pad, OS or SDL in them, so tests/test_pad_controller.c
 * can run them.
 *
 * A device is what cellPad keeps per port (s_poll_dev, and the vibration
 * thread's s_rumble_dev): RUMBLE_NONE for an empty port, an XInput or SDL
 * pad with its XInput user or SDL controller slot (PAD_DEV), or RUMBLE_HID |
 * a PlayStation pad's HID slot. */
#ifndef PS3RECOMP_PAD_CONTROLLER_H
#define PS3RECOMP_PAD_CONTROLLER_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { RUMBLE_NONE = 0, RUMBLE_XINPUT = 1, RUMBLE_SDL = 2, RUMBLE_HID = 0x10 /* | the HID pad's slot */ };
#define PAD_DEV(kind, index) ((long)(kind) | ((long)(index) << 8))
static int pad_dev_index(long dev) { return (int)((dev >> 8) & 0xF); }

/* The device a PAD_CONTROLLER value names, or RUMBLE_NONE ("auto", empty, or
 * not a name): "controller1".."controller4" an XInput user (sdl: an SDL
 * controller slot), "playstation1".. a PlayStation pad's HID slot, up to
 * hid_max (0 off Windows, where SDL has every pad). */
static long pad_controller_parse(const char* e, int sdl, int hid_max)
{
    const size_t n = e ? strlen(e) : 0;
    if (!n) return RUMBLE_NONE;
    const char last = e[n - 1];
    if (n == 11 && !strncmp(e, "controller", 10) && last >= '1' && last <= '4')
        return PAD_DEV(sdl ? RUMBLE_SDL : RUMBLE_XINPUT, last - '1');
    if (n == 12 && !strncmp(e, "playstation", 11) && last >= '1' && last < '1' + hid_max)
        return RUMBLE_HID | (last - '1');
    return RUMBLE_NONE;
}

/* The port that has device want this sweep, or -1. */
static int pad_controller_find(const long* devs, int ports, long want)
{
    if (want == RUMBLE_NONE) return -1;
    for (int p = 0; p < ports; p++)
        if (devs[p] == want) return p;
    return -1;
}

/* A device's PAD_CONTROLLER name, into out (n bytes). 0 for an empty port. */
static int pad_controller_name(long dev, char* out, size_t n)
{
    if (dev & RUMBLE_HID) return snprintf(out, n, "playstation%d", (int)(dev & 0xF) + 1) > 0;
    if ((dev & 0xFF) == RUMBLE_XINPUT || (dev & 0xFF) == RUMBLE_SDL)
        return snprintf(out, n, "controller%d", pad_dev_index(dev) + 1) > 0;
    return 0;
}

#endif
