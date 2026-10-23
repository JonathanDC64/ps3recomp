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
 * Vibration (pad_hid_sony_rumble, from cellPad's vibration thread) writes
 * the pad's output report on a second, overlapped handle: the reader's is
 * synchronous and stays in ReadFile, and a write on it would wait behind
 * that read. The reports set the motors alone (as Linux hid-playstation and
 * SDL lay them out; offsets from the report id):
 *   DualShock 4 USB 0x05: [1] flags 0x01 (motors), [4] small (right, weak),
 *     [5] large (left, strong)
 *   DualShock 4 BT 0x11: [1] 0xC4 (HID, CRC, 4 ms reports), then as USB
 *     two bytes on: [3] flags, [6] small, [7] large
 *   DualSense USB 0x02: [1] 0x03 (rumble the old way, haptics off), [3]
 *     small, [4] large, [39] 0x04 (the rumble emulation newer firmware
 *     wants)
 *   DualSense BT 0x31: [1] sequence << 4, [2] 0x10, then as USB two bytes
 *     on
 *   Bluetooth: a CRC-32 of 0xA2 and bytes 0-73 at 74-77, little-endian.
 * Over Bluetooth the first such report also switches the pad to its full
 * input reports (DualShock 4 0x11, DualSense 0x31), which sony_parse reads.
 *
 * Knobs: PAD_NO_HID=1 turns it off (if Steam Input already presents the pad
 * as XInput, the game would otherwise see it twice). PAD_HID_DEADZONE
 * (default 20, of 128, RPCS3's 40/255) is the sticks' radial deadzone -- a
 * DualSense at rest sits a few counts off 128 -- and PAD_STICK_ANTI_DEADZONE
 * (percent, default 13) the output at its edge; see pad_stick.h.
 */
#ifdef _WIN32

#include "pad_hid_sony.h"
#include "cellPad.h"
#include "pad_stick.h"
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
    /* Vibration: the write handle (NULL when the pad takes none) and its
     * event, under wlock; the output report's length; the DualSense's
     * Bluetooth sequence number. */
    SRWLOCK         wlock;
    HANDLE          wh, wev;
    USHORT          out_len;
    unsigned        seq;
    int             said;      /* a failed write was logged */
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

/* One stick's raw bytes (centre 128) through the radial deadzone. */
static void stick(uint8_t rx, uint8_t ry, uint8_t* x, uint8_t* y)
{
    static int d = -1, a = -1;
    PS3_KNOB(d, ps3_env_int("PAD_HID_DEADZONE", 20));
    PS3_KNOB(a, ps3_env_int("PAD_STICK_ANTI_DEADZONE", 13));
    pad_stick_to_u8((rx - 128) / 127.0f, (ry - 128) / 127.0f, d / 128.0f, a / 100.0f, x, y);
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
    stick(q[0], q[1], &s.lx, &s.ly);
    stick(q[2], q[3], &s.rx, &s.ry);
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
    AcquireSRWLockExclusive(&d->wlock);
    if (d->wh) CloseHandle(d->wh);
    if (d->wev) CloseHandle(d->wev);
    d->wh = d->wev = NULL;
    ReleaseSRWLockExclusive(&d->wlock);
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
        USHORT in_len = 0, out_len = 0;
        PHIDP_PREPARSED_DATA pp;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps;
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                in_len = caps.InputReportByteLength;
                out_len = caps.OutputReportByteLength;
            }
            HidD_FreePreparsedData(pp);
        }
        int slot = -1;
        for (int i = 0; i < PAD_HID_SONY_MAX && slot < 0; i++)
            if (InterlockedCompareExchange(&s_dev[i].used, 1, 0) == 0) slot = i;
        if (in_len < 10 || slot < 0) { if (slot >= 0) s_dev[slot].used = 0; CloseHandle(h); continue; }

        SonyDev* d = &s_dev[slot];
        d->h = h; d->kind = kind; d->in_len = in_len; d->have = 0;
        wcscpy(d->path, path);
        /* The write handle, if the pad takes output reports this long. */
        uint8_t probe[1024];
        d->out_len = out_len;
        d->seq = 0;
        d->said = 0;
        d->wh = d->wev = NULL;
        if (out_len <= sizeof probe &&
            pad_hid_sony_rumble_report(kind == KIND_DS5, in_len > 64, 0, 0, 0, probe, out_len)) {
            HANDLE w = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                   FILE_FLAG_OVERLAPPED, NULL);
            HANDLE e = w != INVALID_HANDLE_VALUE ? CreateEventW(NULL, TRUE, FALSE, NULL) : NULL;
            if (e) { d->wh = w; d->wev = e; }
            else if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
        }
        HANDLE t = (HANDLE)_beginthreadex(NULL, 0, sony_reader, d, 0, NULL);
        if (!t) {
            if (d->wh) { CloseHandle(d->wh); CloseHandle(d->wev); d->wh = d->wev = NULL; }
            CloseHandle(h); d->path[0] = 0; InterlockedExchange(&d->used, 0); continue;
        }
        SetThreadDescription(t, L"cellPad HID reader");
        CloseHandle(t);
        fprintf(stderr, "[cellPad] %s connected (HID %s, PID %04X) as HID pad %d%s\n",
                kind == KIND_DS5 ? "DualSense" : "DualShock 4",
                in_len > 64 ? "Bluetooth" : "USB", at.ProductID, slot, d->wh ? "" : ", no vibration");
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
        for (int i = 0; i < PAD_HID_SONY_MAX; i++) {
            InitializeSRWLock(&s_dev[i].lock);
            InitializeSRWLock(&s_dev[i].wlock);
        }
        HANDLE t = (HANDLE)_beginthreadex(NULL, 0, sony_scanner, NULL, 0, NULL);
        if (t) { SetThreadDescription(t, L"cellPad HID scan"); CloseHandle(t); }
    }
    int n = 0;
    for (int i = 0; i < PAD_HID_SONY_MAX && n < max; i++) {
        if (!s_dev[i].used) continue;
        AcquireSRWLockShared(&s_dev[i].lock);
        if (s_dev[i].have) {
            out[n] = s_dev[i].st;
            out[n++].slot = (uint8_t)i;
        }
        ReleaseSRWLockShared(&s_dev[i].lock);
    }
    return n;
}

int pad_hid_sony_rumble(int slot, uint8_t strong, uint8_t weak)
{
    if (slot < 0 || slot >= PAD_HID_SONY_MAX || !s_started) return 0;
    SonyDev* d = &s_dev[slot];
    uint8_t r[1024];
    int ok = 0;
    AcquireSRWLockExclusive(&d->wlock);
    if (d->used && d->wh && d->out_len <= sizeof r &&
        pad_hid_sony_rumble_report(d->kind == KIND_DS5, d->in_len > 64, d->seq, strong, weak, r, d->out_len)) {
        d->seq = (d->seq + 1) & 15;
        /* (overlapped, so a pad that stops answering costs 200 ms, not the thread) */
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.hEvent = d->wev;
        ResetEvent(d->wev);
        DWORD put = 0, err = 0;
        if (WriteFile(d->wh, r, d->out_len, NULL, &ov)) {
            ok = GetOverlappedResult(d->wh, &ov, &put, FALSE);
        } else if ((err = GetLastError()) == ERROR_IO_PENDING) {
            if (WaitForSingleObject(d->wev, 200) != WAIT_OBJECT_0) CancelIoEx(d->wh, &ov);
            ok = GetOverlappedResult(d->wh, &ov, &put, TRUE);
        }
        if (!ok && !d->said++) {
            if (!err) err = GetLastError();
            fprintf(stderr, "[cellPad] HID pad %d: vibration not sent (error %lu)\n", slot, (unsigned long)err);
        }
    }
    ReleaseSRWLockExclusive(&d->wlock);
    return ok;
}

#endif /* _WIN32 */

/* The reports, on every host (the tests build them anywhere). */
#include "pad_hid_sony.h"
#include <string.h>

/* CRC-32 (IEEE, reflected), as zlib's crc32: crc starts at 0. */
static uint32_t sony_crc32(uint32_t crc, const uint8_t* p, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

size_t pad_hid_sony_rumble_report(int ds5, int bt, unsigned seq, uint8_t strong, uint8_t weak, uint8_t* out,
                                  size_t n)
{
    const size_t need = bt ? 78 : ds5 ? 48 : 32;
    if (n < need) return 0;
    memset(out, 0, n);
    const size_t c = bt ? 3 : 1;   /* where the common part starts */
    if (ds5) {
        out[0] = bt ? 0x31 : 0x02;
        if (bt) {
            out[1] = (uint8_t)((seq & 15u) << 4);
            out[2] = 0x10;
        }
        out[c + 0] = 0x03;    /* rumble the old way, haptics off */
        out[c + 2] = weak;
        out[c + 3] = strong;
        out[c + 38] = 0x04;   /* the newer rumble emulation */
    } else {
        out[0] = bt ? 0x11 : 0x05;
        if (bt) out[1] = 0xC4;   /* HID, CRC, input reports every 4 ms */
        out[c + 0] = 0x01;    /* the motors only */
        out[c + 3] = weak;
        out[c + 4] = strong;
    }
    if (bt) {
        static const uint8_t seed = 0xA2;
        const uint32_t crc = sony_crc32(sony_crc32(0, &seed, 1), out, 74);
        out[74] = (uint8_t)crc;
        out[75] = (uint8_t)(crc >> 8);
        out[76] = (uint8_t)(crc >> 16);
        out[77] = (uint8_t)(crc >> 24);
    }
    return need;
}
