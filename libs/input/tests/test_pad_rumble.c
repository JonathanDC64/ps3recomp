/* Vibration: cellPadSetActDirect's motors through the vibration thread to XInput, and the Sony pads' output
 * reports. XInput is stubbed (one pad on port 0), so the result does not depend on the machine.
 *
 *   clang-cl -std:c17 -I include -I libs/input libs/input/tests/test_pad_rumble.c -Fe:test_pad_rumble.exe
 *   test_pad_rumble.exe
 */
#include <windows.h>
#include <xinput.h>
#pragma comment(lib, "user32.lib")   /* GetDesktopWindow */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG s_sets;               /* XInputSetState calls */
static volatile LONG s_left, s_right;      /* the last vibration port 0 got */
static SHORT test_key_state(int vk) { (void)vk; return 0; }
static HWND test_foreground(void) { return GetDesktopWindow(); }
static DWORD test_xinput_get(DWORD i, XINPUT_STATE* s)
{
    if (i != 0) return ERROR_DEVICE_NOT_CONNECTED;
    memset(s, 0, sizeof *s);
    return ERROR_SUCCESS;
}
static DWORD test_xinput_set(DWORD i, XINPUT_VIBRATION* v)
{
    if (i != 0) return ERROR_DEVICE_NOT_CONNECTED;
    InterlockedExchange(&s_left, v->wLeftMotorSpeed);
    InterlockedExchange(&s_right, v->wRightMotorSpeed);
    InterlockedIncrement(&s_sets);
    return ERROR_SUCCESS;
}
static DWORD test_window_pid(HWND w, LPDWORD pid) { (void)w; *pid = GetCurrentProcessId(); return 0; }
#define GetAsyncKeyState         test_key_state
#define GetForegroundWindow      test_foreground
#define GetWindowThreadProcessId test_window_pid
#define XInputGetState           test_xinput_get
#define XInputSetState           test_xinput_set

#include "../cellPad.c"
#include "../pad_hid_sony.c"   /* (the reports; PAD_NO_HID=1 keeps it off the machine's devices) */

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
void spu_lockline_unlock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }

#define EA 0x1000u
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static void set_env(const char* name, const char* value)
{
    _putenv_s(name, value ? value : "");
    ps3_env_gen++;
}

/* The game sets the motors; a poll first, as a game reads its pad (the poll finds the pads). */
static void act(unsigned weak, unsigned strong)
{
    Sleep(20);
    assert(cellPadGetData(0, (CellPadData*)(uintptr_t)(EA + 0x100)) == CELL_OK);
    vm_base[EA + CELL_PAD_ACTUATOR_PARAM_SMALL] = (uint8_t)weak;
    vm_base[EA + CELL_PAD_ACTUATOR_PARAM_LARGE] = (uint8_t)strong;
    assert(cellPadSetActDirect(0, (CellPadActParam*)(uintptr_t)EA) == CELL_OK);
}

/* Waits up to ms for port 0's motors to read left/right. */
static int motors(long left, long right, int ms)
{
    for (int t = 0; t < ms; t += 5) {
        if (s_left == left && s_right == right) return 1;
        Sleep(5);
    }
    fprintf(stderr, "  motors %ld %ld, wanted %ld %ld\n", (long)s_left, (long)s_right, left, right);
    return 0;
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 0x10000);
    set_env("PAD_RUMBLE", NULL);
    set_env("PAD_NO_HID", "1");
    assert(cellPadInit(1) == CELL_OK);

    /* the small motor: any value is full speed */
    act(1, 0);
    CHECK(motors(0, 65535, 200));
    /* the large motor: 63 and under is still, 64-255 spreads over 0-255 */
    act(0, 63);
    CHECK(motors(0, 0, 200));
    act(0, 255);
    CHECK(motors(65535, 0, 200));
    act(0, 159);   /* (159 - 63) * 255 / 192 = 127 */
    CHECK(motors(127 * 257, 0, 200));
    act(1, 255);
    CHECK(motors(65535, 65535, 200));

    /* resent while running, every 300 ms */
    const LONG n0 = s_sets;
    Sleep(700);
    CHECK(s_sets - n0 >= 2);

    /* PAD_RUMBLE=0 stops it, live; 1 brings it back */
    set_env("PAD_RUMBLE", "0");
    CHECK(motors(0, 0, 200));
    set_env("PAD_RUMBLE", "1");
    act(1, 255);
    CHECK(motors(65535, 65535, 200));

    /* the game stops setting it: off after 3 s */
    CHECK(motors(0, 0, 3500));

    /* cellPadEnd stops it */
    act(1, 200);
    CHECK(motors((200 - 63) * 255 / 192 * 257, 65535, 200));
    assert(cellPadEnd() == CELL_OK);
    CHECK(motors(0, 0, 200));

    /* The Sony output reports. CRC-32 of "123456789" is 0xCBF43926 (the check value). */
    CHECK(sony_crc32(0, (const uint8_t*)"123456789", 9) == 0xCBF43926u);
    uint8_t r[80];
    CHECK(pad_hid_sony_rumble_report(0, 0, 0, 200, 255, r, 31) == 0);   /* too short */
    CHECK(pad_hid_sony_rumble_report(0, 0, 0, 200, 255, r, 32) == 32);  /* DualShock 4 USB */
    CHECK(r[0] == 0x05 && r[1] == 0x01 && r[4] == 255 && r[5] == 200 && r[6] == 0);
    CHECK(pad_hid_sony_rumble_report(1, 0, 0, 200, 255, r, 48) == 48);  /* DualSense USB */
    CHECK(r[0] == 0x02 && r[1] == 0x03 && r[2] == 0 && r[3] == 255 && r[4] == 200 && r[39] == 0x04);
    CHECK(pad_hid_sony_rumble_report(0, 1, 0, 200, 255, r, 78) == 78);  /* DualShock 4 Bluetooth */
    CHECK(r[0] == 0x11 && r[1] == 0xC4 && r[3] == 0x01 && r[6] == 255 && r[7] == 200);
    {
        uint8_t seed = 0xA2;
        const uint32_t crc = sony_crc32(sony_crc32(0, &seed, 1), r, 74);
        CHECK(r[74] == (uint8_t)crc && r[75] == (uint8_t)(crc >> 8) && r[76] == (uint8_t)(crc >> 16) &&
              r[77] == (uint8_t)(crc >> 24));
    }
    CHECK(pad_hid_sony_rumble_report(1, 1, 5, 200, 255, r, 78) == 78);  /* DualSense Bluetooth */
    CHECK(r[0] == 0x31 && r[1] == 0x50 && r[2] == 0x10 && r[3] == 0x03 && r[5] == 255 && r[6] == 200 &&
          r[41] == 0x04);

    printf("test_pad_rumble: ok\n");
    return 0;
}
