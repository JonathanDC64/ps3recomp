/*
 * pad_hid_sony -- DualSense / DualSense Edge / DualShock 4 over raw HID
 * (Windows), for machines where those pads are not XInput devices.
 */
#ifndef PS3RECOMP_PAD_HID_SONY_H
#define PS3RECOMP_PAD_HID_SONY_H

#include <stddef.h>
#include <stdint.h>

#define PAD_HID_SONY_MAX 4

typedef struct {
    uint16_t buttons;   /* CELL_PAD_CTRL_* */
    uint8_t  lx, ly, rx, ry;   /* 0-255, centre 128, up = 0 */
    uint8_t  l2, r2;           /* analog triggers 0-255 */
    uint8_t  slot;             /* the pad, for pad_hid_sony_rumble */
} PadHidSonyState;

/* Starts the device scanner on first call. Fills up to max states, one per
 * connected Sony pad in connection order, and returns how many. Cheap: it
 * copies what the reader threads last parsed. PAD_NO_HID=1 returns 0. */
int pad_hid_sony_poll(PadHidSonyState* out, int max);

/* Sets a pad's motors (0-255: strong the large, left one; weak the small,
 * right one; "small" is a macro in Windows' headers) with its output
 * report. Blocks while the report is written (a few ms over Bluetooth), so it
 * is for cellPad's vibration thread. 0 when the pad is gone, has no output
 * report or the write failed. Windows only: elsewhere SDL drives every pad's
 * motors. */
int pad_hid_sony_rumble(int slot, uint8_t strong, uint8_t weak);

/* The output report that sets a pad's motors and nothing else (the light bar
 * and the player lights stay as they are), into out, n bytes (the report's
 * length as the device gives it; the rest stays 0): ds5 0 a DualShock 4, 1 a
 * DualSense; bt 1 over Bluetooth, which adds a CRC-32 at bytes 74-77; seq the
 * DualSense's Bluetooth sequence number, 0-15. Returns the bytes the report
 * needs, or 0 when n is shorter. */
size_t pad_hid_sony_rumble_report(int ds5, int bt, unsigned seq, uint8_t strong, uint8_t weak, uint8_t* out,
                                  size_t n);

#endif
