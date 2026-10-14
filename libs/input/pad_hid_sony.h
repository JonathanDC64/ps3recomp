/*
 * pad_hid_sony -- DualSense / DualSense Edge / DualShock 4 over raw HID
 * (Windows), for machines where those pads are not XInput devices.
 */
#ifndef PS3RECOMP_PAD_HID_SONY_H
#define PS3RECOMP_PAD_HID_SONY_H

#include <stdint.h>

#define PAD_HID_SONY_MAX 4

typedef struct {
    uint16_t buttons;   /* CELL_PAD_CTRL_* */
    uint8_t  lx, ly, rx, ry;   /* 0-255, centre 128, up = 0 */
    uint8_t  l2, r2;           /* analog triggers 0-255 */
} PadHidSonyState;

/* Starts the device scanner on first call. Fills up to max states, one per
 * connected Sony pad in connection order, and returns how many. Cheap: it
 * copies what the reader threads last parsed. PAD_NO_HID=1 returns 0. */
int pad_hid_sony_poll(PadHidSonyState* out, int max);

#endif
