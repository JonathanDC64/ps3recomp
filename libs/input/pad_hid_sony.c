/*
 * pad_hid_sony -- DualSense / DualSense Edge / DualShock 4 over raw HID.
 *
 * XInput never enumerates Sony pads: on Windows a DualSense or DualShock 4
 * is a plain HID device, and without Steam Input or DS4Windows standing in
 * it was an empty slot, so the only way to play was the keyboard. This reads
 * the pads' input reports directly through hid.dll / setupapi (part of the
 * OS, no extra dependency), over USB and Bluetooth.
 *
 * Report layouts (as SDL's hidapi drivers and Linux hid-playstation read
 * them; offsets from the first byte after the report id):
 *   DualSense USB 0x01 and BT 0x31 (one more header byte), "full":
 *     LX LY RX RY L2 R2 counter b0 b1 b2
 *   DualSense BT 0x01 before anything asked for the full report, and
 *   DualShock 4 0x01 (USB, BT) / BT 0x11 (two more header bytes), "simple":
 *     LX LY RX RY b0 b1 b2 L2 R2
 *   b0: hat in the low nibble (0 = N, clockwise, 8 = centred), 0x10 square,
 *       0x20 cross, 0x40 circle, 0x80 triangle
 *   b1: L1 R1 L2 R2 create/share options L3 R3 (bit 0 up)
 *   b2: bit 0 PS, bit 1 touchpad click
 * The pad's sticks are already the PS3's 0-255 with up = 0.
 *
 * One thread rescans the HID devices every 2 s (hot-plug); each pad found
 * gets a reader thread blocked in ReadFile, so an idle pad costs nothing.
 * The reader parses each report into the pad's state under a lock and
 * cellPad's poll copies it out.
 *
 * Knobs: PAD_NO_HID=1 turns it off (if Steam Input already presents the pad
 * as XInput, the game would otherwise see it twice). PAD_HID_DEADZONE
 * (default 10, of 128): a stick within that of centre on an axis reads
 * centred -- a DualSense at rest sits a few counts off 128.
 */
#ifdef _WIN32

#include "pad_hid_sony.h"
#include "cellPad.h"
#include "ps3emu/ps3_knob.h"

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <process.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

enum { KIND_DS4 = 4, KIND_DS5 = 5 };

typedef struct {
    volatile LONG   used;      /* slot holds an open pad */
    HANDLE          h;
    int             kind;
    USHORT          in_len;    /* input report length: 64 USB, larger on BT */
    wchar_t         path[512];
    SRWLOCK         lock;
    PadHidSonyState st;
    int             have;      /* a report has been parsed */
} SonyDev;

static SonyDev s_dev[PAD_HID_SONY_MAX];
static volatile LONG s_started;

static int sony_kind(USHORT vid, USHORT pid)
{
    if (vid != 0x054C) return 0;
    switch (pid) {
    case 0x0CE6: case 0x0DF2: return KIND_DS5;               /* DualSense, Edge */
    case 0x05C4: case 0x09CC: case 0x0BA0: return KIND_DS4;  /* DS4 v1, v2, dongle */
    default: return 0;
    }
}

static uint8_t dz(uint8_t v)
{
    static int d = -1;
    PS3_KNOB(d, getenv("PAD_HID_DEADZONE") ? atoi(getenv("PAD_HID_DEADZONE")) : 10);
    return (v >= 128 - d && v <= 128 + d) ? 128 : v;
}

static void sony_parse(SonyDev* d, const uint8_t* r, DWORD n)
{
    const uint8_t* q;
    int full;
    if (d->kind == KIND_DS5) {
        if (r[0] == 0x31 && n >= 12)                        { q = r + 2; full = 1; }
        else if (r[0] == 0x01 && d->in_len <= 64 && n >= 11) { q = r + 1; full = 1; }
        else if (r[0] == 0x01 && n >= 10)                   { q = r + 1; full = 0; }
        else return;
    } else {
        if (r[0] == 0x01 && n >= 10)      { q = r + 1; full = 0; }
        else if (r[0] == 0x11 && n >= 12) { q = r + 3; full = 0; }
        else return;
    }
    const uint8_t b0 = full ? q[7] : q[4];
    const uint8_t b1 = full ? q[8] : q[5];
    const uint8_t b2 = full ? q[9] : q[6];
    PadHidSonyState s;
    s.lx = dz(q[0]); s.ly = dz(q[1]); s.rx = dz(q[2]); s.ry = dz(q[3]);
    s.l2 = full ? q[4] : q[7];
    s.r2 = full ? q[5] : q[8];

    uint16_t btn = 0;
    const int hat = b0 & 0x0F;
    if (hat == 0 || hat == 1 || hat == 7) btn |= CELL_PAD_CTRL_UP;
    if (hat == 1 || hat == 2 || hat == 3) btn |= CELL_PAD_CTRL_RIGHT;
    if (hat == 3 || hat == 4 || hat == 5) btn |= CELL_PAD_CTRL_DOWN;
    if (hat == 5 || hat == 6 || hat == 7) btn |= CELL_PAD_CTRL_LEFT;
    if (b0 & 0x10) btn |= CELL_PAD_CTRL_SQUARE;
    if (b0 & 0x20) btn |= CELL_PAD_CTRL_CROSS;
    if (b0 & 0x40) btn |= CELL_PAD_CTRL_CIRCLE;
    if (b0 & 0x80) btn |= CELL_PAD_CTRL_TRIANGLE;
    if (b1 & 0x01) btn |= CELL_PAD_CTRL_L1;
    if (b1 & 0x02) btn |= CELL_PAD_CTRL_R1;
    if (b1 & 0x04) btn |= CELL_PAD_CTRL_L2;
    if (b1 & 0x08) btn |= CELL_PAD_CTRL_R2;
    if (b1 & 0x10) btn |= CELL_PAD_CTRL_SELECT;   /* Create / Share */
    if (b1 & 0x20) btn |= CELL_PAD_CTRL_START;    /* Options */
    if (b1 & 0x40) btn |= CELL_PAD_CTRL_L3;
    if (b1 & 0x80) btn |= CELL_PAD_CTRL_R3;
    if (b2 & 0x02) btn |= CELL_PAD_CTRL_SELECT;   /* touchpad click */
    s.buttons = btn;

    AcquireSRWLockExclusive(&d->lock);
    d->st = s; d->have = 1;
    ReleaseSRWLockExclusive(&d->lock);
}

static unsigned __stdcall sony_reader(void* arg)
{
    SonyDev* d = (SonyDev*)arg;
    uint8_t buf[1024];
    const DWORD len = d->in_len < sizeof buf ? d->in_len : (DWORD)sizeof buf;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(d->h, buf, len, &got, NULL)) break;   /* unplugged */
        if (got) sony_parse(d, buf, got);
    }
    fprintf(stderr, "[cellPad] %s disconnected (HID)\n", d->kind == KIND_DS5 ? "DualSense" : "DualShock 4");
    CloseHandle(d->h);
    AcquireSRWLockExclusive(&d->lock);
    d->have = 0; d->h = NULL; d->path[0] = 0;
    ReleaseSRWLockExclusive(&d->lock);
    InterlockedExchange(&d->used, 0);
    return 0;
}

static int sony_already_open(const wchar_t* path)
{
    for (int i = 0; i < PAD_HID_SONY_MAX; i++)
        if (s_dev[i].used && _wcsicmp(s_dev[i].path, path) == 0) return 1;
    return 0;
}

static void sony_scan(void)
{
    GUID hid; HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof ifd;
    for (DWORD k = 0; SetupDiEnumDeviceInterfaces(set, NULL, &hid, k, &ifd); k++) {
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; char raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 1024]; } det;
        det.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, &det.d, sizeof det, NULL, NULL)) continue;
        const wchar_t* path = det.d.DevicePath;
        if (wcslen(path) >= 512 || sony_already_open(path)) continue;

        /* Attributes need no access rights; skip everything not Sony first. */
        HANDLE q = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (q == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at; at.Size = sizeof at;
        const int kind = HidD_GetAttributes(q, &at) ? sony_kind(at.VendorID, at.ProductID) : 0;
        CloseHandle(q);
        if (!kind) continue;

        HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        USHORT in_len = 0;
        PHIDP_PREPARSED_DATA pp;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps;
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) in_len = caps.InputReportByteLength;
            HidD_FreePreparsedData(pp);
        }
        int slot = -1;
        for (int i = 0; i < PAD_HID_SONY_MAX && slot < 0; i++)
            if (InterlockedCompareExchange(&s_dev[i].used, 1, 0) == 0) slot = i;
        if (in_len < 10 || slot < 0) { if (slot >= 0) s_dev[slot].used = 0; CloseHandle(h); continue; }

        SonyDev* d = &s_dev[slot];
        d->h = h; d->kind = kind; d->in_len = in_len; d->have = 0;
        wcscpy(d->path, path);
        HANDLE t = (HANDLE)_beginthreadex(NULL, 0, sony_reader, d, 0, NULL);
        if (!t) { CloseHandle(h); d->path[0] = 0; InterlockedExchange(&d->used, 0); continue; }
        SetThreadDescription(t, L"cellPad HID reader");
        CloseHandle(t);
        fprintf(stderr, "[cellPad] %s connected (HID %s, PID %04X) as HID pad %d\n",
                kind == KIND_DS5 ? "DualSense" : "DualShock 4",
                in_len > 64 ? "Bluetooth" : "USB", at.ProductID, slot);
    }
    SetupDiDestroyDeviceInfoList(set);
}

static unsigned __stdcall sony_scanner(void* arg)
{
    (void)arg;
    for (;;) { sony_scan(); Sleep(2000); }
    return 0;
}

int pad_hid_sony_poll(PadHidSonyState* out, int max)
{
    static int off = -1;
    PS3_KNOB(off, getenv("PAD_NO_HID") && getenv("PAD_NO_HID")[0] != '0' ? 1 : 0);
    if (off) return 0;
    if (InterlockedCompareExchange(&s_started, 1, 0) == 0) {
        for (int i = 0; i < PAD_HID_SONY_MAX; i++) InitializeSRWLock(&s_dev[i].lock);
        HANDLE t = (HANDLE)_beginthreadex(NULL, 0, sony_scanner, NULL, 0, NULL);
        if (t) { SetThreadDescription(t, L"cellPad HID scan"); CloseHandle(t); }
    }
    int n = 0;
    for (int i = 0; i < PAD_HID_SONY_MAX && n < max; i++) {
        if (!s_dev[i].used) continue;
        AcquireSRWLockShared(&s_dev[i].lock);
        if (s_dev[i].have) out[n++] = s_dev[i].st;
        ReleaseSRWLockShared(&s_dev[i].lock);
    }
    return n;
}

#endif /* _WIN32 */
