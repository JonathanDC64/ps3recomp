/* The keyboard pad's PAD_KEYS bindings and the host input calls (Windows: the keyboard pad reads GetAsyncKeyState).
 * Keys, the foreground window and XInput are stubbed, so the result does not depend on the machine.
 *
 *   clang-cl -std:c17 -I include -I libs/input libs/input/tests/test_pad_keys.c -Fe:test_pad_keys.exe
 *   test_pad_keys.exe
 */
#include <windows.h>
#include <xinput.h>
#pragma comment(lib, "user32.lib")   /* GetDesktopWindow */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char s_keys[256];   /* the stubbed keyboard: 1 = held */
/* As on Windows, a left or right Shift, Ctrl or Alt also holds the plain one down. */
static SHORT test_key_state(int vk)
{
    int held = vk > 0 && vk < 256 && s_keys[vk];
    if (vk == VK_SHIFT)   held |= s_keys[VK_LSHIFT] | s_keys[VK_RSHIFT];
    if (vk == VK_CONTROL) held |= s_keys[VK_LCONTROL] | s_keys[VK_RCONTROL];
    if (vk == VK_MENU)    held |= s_keys[VK_LMENU] | s_keys[VK_RMENU];
    return (SHORT)(held ? 0x8000 : 0);
}
static HWND test_foreground(void) { return GetDesktopWindow(); }
static DWORD test_xinput_get(DWORD i, XINPUT_STATE* s) { (void)i; (void)s; return ERROR_DEVICE_NOT_CONNECTED; }
static DWORD test_xinput_set(DWORD i, XINPUT_VIBRATION* v) { (void)i; (void)v; return ERROR_DEVICE_NOT_CONNECTED; }
static DWORD test_window_pid(HWND w, LPDWORD pid) { (void)w; *pid = GetCurrentProcessId(); return 0; }
#define GetAsyncKeyState         test_key_state
#define GetForegroundWindow      test_foreground
#define GetWindowThreadProcessId test_window_pid
#define XInputGetState           test_xinput_get
#define XInputSetState           test_xinput_set

#include "../cellPad.c"

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
int pad_hid_sony_poll(PadHidSonyState* out, int max) { (void)out; (void)max; return 0; }
int pad_hid_sony_rumble(int slot, uint8_t strong, uint8_t weak) { (void)slot; (void)strong; (void)weak; return 0; }

#define EA 0x1000u

static void set_env(const char* name, const char* value)
{
    _putenv_s(name, value ? value : "");
    ps3_env_gen++;
}

typedef struct { unsigned len, buttons; unsigned lx, ly, rx, ry; } Pkt;

/* One read of port 0, after the 4 ms poll interval, so the keys are read again and a packet is handed out. */
static Pkt rd(void)
{
    Sleep(20);   /* GetTickCount64 moves in steps of about 16 ms */
    assert(cellPadGetData(0, (CellPadData*)(uintptr_t)EA) == CELL_OK);
    Pkt p;
    p.len = vm_read32(EA);
    const uint16_t d1 = vm_read16(EA + 4 + 2 * CELL_PAD_BTN_OFFSET_DIGITAL1);
    const uint16_t d2 = vm_read16(EA + 4 + 2 * CELL_PAD_BTN_OFFSET_DIGITAL2);
    p.buttons = (unsigned)(d2 & 0xFF) << 8 | (d1 & 0xFF);
    p.rx = vm_read16(EA + 4 + 2 * CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X);
    p.ry = vm_read16(EA + 4 + 2 * CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y);
    p.lx = vm_read16(EA + 4 + 2 * CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X);
    p.ly = vm_read16(EA + 4 + 2 * CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y);
    return p;
}

static void keys(const int* vks, int n)
{
    memset(s_keys, 0, sizeof s_keys);
    for (int i = 0; i < n; i++) s_keys[vks[i]] = 1;
}
#define KEYS(...) do { const int k_[] = { 0, __VA_ARGS__ }; keys(k_ + 1, (int)(sizeof k_ / sizeof k_[0]) - 1); } while (0)

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

enum {
    SELECT = 0x0001, L3 = 0x0002, R3 = 0x0004, START = 0x0008, UP = 0x0010, RIGHT = 0x0020, DOWN = 0x0040, LEFT = 0x0080,
    L2 = 0x0100, R2 = 0x0200, L1 = 0x0400, R1 = 0x0800, TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000,
    SQUARE = 0x8000,
};

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 0x10000);
    set_env("PAD_KEYS", NULL);
    assert(cellPadInit(1) == CELL_OK);
    Pkt p;

    /* defaults: Z = cross; an arrow presses the d-pad and moves the left stick */
    KEYS('Z');
    p = rd(); CHECK(p.len && p.buttons == CROSS && p.lx == 128 && p.ly == 128);
    KEYS(VK_LEFT);
    p = rd(); CHECK(p.buttons == LEFT && p.lx == 0 && p.ly == 128);
    KEYS();
    p = rd(); CHECK(p.buttons == 0 && p.lx == 128);

    /* a Souls-style layout */
    set_env("PAD_KEYS", "ls_up=w,ls_down=s,ls_left=a,ls_right=d,walk=lalt,circle=space|menu:q,cross=e|menu:mouse1,"
                        "square=r,triangle=f,r1=mouse1,r2=shift+mouse1,l1=mouse2,l2=lctrl|shift+mouse2,"
                        "r3=q|mouse3,l3=c,start=esc,select=g|tab,up=up|wheelup,down=down|wheeldown,"
                        "right=right|shift+wheelup,left=left|shift+wheeldown,bogus=x,cross=e|menu:mouse1|nokey");
    KEYS('W');
    p = rd(); CHECK(p.buttons == 0 && p.lx == 128 && p.ly == 0);          /* W moves the stick, no d-pad */
    KEYS(VK_UP);
    p = rd(); CHECK(p.buttons == UP && p.lx == 128 && p.ly == 128);       /* the arrow is the d-pad only */
    KEYS('W', 'S');
    p = rd(); CHECK(p.ly == 128);                                          /* opposite keys cancel */
    KEYS('W', 'D');
    p = rd(); CHECK(p.lx == 255 && p.ly == 0);
    KEYS('W', VK_LMENU);
    p = rd(); CHECK(p.ly == 57 && p.lx == 128);                            /* walk: 55 % */
    KEYS('W', 'D', VK_LMENU);
    p = rd(); CHECK(p.ly == 78 && p.lx == 177);                            /* walk, diagonal: 55 % / sqrt 2 */
    KEYS('S', VK_LMENU);
    p = rd(); CHECK(p.ly == 198);
    KEYS(VK_SPACE, 'E', 'R', 'F', 'C', VK_ESCAPE, 'G');
    p = rd(); CHECK(p.buttons == (CIRCLE | CROSS | SQUARE | TRIANGLE | L3 | START | SELECT));
    KEYS(VK_TAB, 'Q', VK_LCONTROL);
    p = rd(); CHECK(p.buttons == (SELECT | R3 | L2));

    /* mouse buttons come from the host, chords take over their inputs */
    KEYS();
    cellPad_host_mouse_buttons(1);
    p = rd(); CHECK(p.buttons == R1);
    cellPad_host_mouse_buttons(0);
    KEYS(VK_SHIFT);
    p = rd(); CHECK(p.buttons == 0);
    cellPad_host_mouse_buttons(1);
    p = rd(); CHECK(p.buttons == R2);                                      /* Shift + left: R2, not R1 too */
    cellPad_host_mouse_buttons(3);
    p = rd(); CHECK(p.buttons == (R2 | L2));                               /* Shift + right: L2, not L1 */
    cellPad_host_mouse_buttons(2);
    p = rd(); CHECK(p.buttons == L2);
    KEYS();
    p = rd(); CHECK(p.buttons == L2);                                      /* Shift let go first: L2 stays held */
    cellPad_host_mouse_buttons(0);
    p = rd(); CHECK(p.buttons == 0);
    cellPad_host_mouse_buttons(3);
    p = rd(); CHECK(p.buttons == (R1 | L1));
    KEYS(VK_SHIFT);
    p = rd(); CHECK(p.buttons == (R1 | L1));                               /* Shift during a click: still R1 and L1 */
    KEYS();
    cellPad_host_mouse_buttons(4);
    p = rd(); CHECK(p.buttons == R3);
    cellPad_host_mouse_buttons(0);

    /* the menu layer: Q cancels and a left click confirms, instead of lock-on and attack */
    cellPad_host_set_menu(1);
    KEYS('Q');
    p = rd(); CHECK(p.buttons == CIRCLE);
    KEYS();
    cellPad_host_mouse_buttons(1);
    p = rd(); CHECK(p.buttons == CROSS);
    KEYS(VK_SPACE, 'E');
    cellPad_host_mouse_buttons(0);
    p = rd(); CHECK(p.buttons == (CIRCLE | CROSS));                        /* the other bindings still count */
    cellPad_host_set_menu(0);
    KEYS('Q');
    p = rd(); CHECK(p.buttons == R3);

    /* wheel notches: two packets pressed, two let go; Shift picks the weapon switch */
    KEYS();
    p = rd();
    cellPad_host_mouse_wheel(2);
    unsigned seq[8];
    for (int i = 0; i < 8; i++) seq[i] = rd().buttons;
    CHECK(seq[0] == UP && seq[1] == UP && seq[2] == 0 && seq[3] == 0 && seq[4] == UP && seq[5] == UP && seq[6] == 0);
    KEYS(VK_SHIFT);
    p = rd();
    cellPad_host_mouse_wheel(1);
    p = rd(); CHECK(p.buttons == RIGHT);
    p = rd(); CHECK(p.buttons == RIGHT);
    p = rd(); CHECK(p.buttons == 0);
    p = rd(); p = rd();
    KEYS();
    p = rd();
    cellPad_host_mouse_wheel(-9);                                         /* queued at most 4 */
    int n = 0;
    for (int i = 0; i < 24; i++) if (rd().buttons == DOWN) n++;
    CHECK(n == 8);

    /* stick holds, counted in packets */
    cellPad_host_stick(1, 255, 128, 3);
    cellPad_host_stick(0, 0, 128, 2);                                      /* both sticks at once */
    p = rd(); CHECK(p.buttons == 0 && p.rx == 255 && p.ry == 128 && p.lx == 0);
    p = rd(); CHECK(p.rx == 255 && p.lx == 0);
    p = rd(); CHECK(p.rx == 255 && p.lx == 128);
    p = rd(); CHECK(p.rx == 128);
    cellPad_host_stick(1, 0, 128, 5);
    cellPad_host_stick(1, 128, 0, 1);                                     /* a new hold replaces the old */
    p = rd(); CHECK(p.rx == 128 && p.ry == 0);
    p = rd(); CHECK(p.rx == 128 && p.ry == 128);
    cellPad_set_host_hold(1);
    cellPad_host_stick(1, 255, 128, 3);
    p = rd(); CHECK(p.rx == 128);                                          /* dropped while the UI has the input */
    cellPad_set_host_hold(0);
    p = rd(); CHECK(p.rx == 128);

    /* none unbinds; with no left-stick keys the arrows move the stick again */
    set_env("PAD_KEYS", "cross=none,r1=mouse1");
    KEYS('Z', VK_RIGHT);
    p = rd(); CHECK(p.buttons == RIGHT && p.lx == 255);

    /* up and down notches queue in turn instead of cancelling out, and each keeps the Shift it was turned with */
    set_env("PAD_KEYS", "up=wheelup,down=wheeldown,right=shift+wheelup");
    KEYS();
    p = rd();
    cellPad_host_mouse_wheel(1);
    cellPad_host_mouse_wheel(-1);
    for (int i = 0; i < 8; i++) seq[i] = rd().buttons;
    CHECK(seq[0] == UP && seq[1] == UP && seq[2] == 0 && seq[3] == 0 && seq[4] == DOWN && seq[5] == DOWN && seq[6] == 0 && seq[7] == 0);
    KEYS(VK_LSHIFT);                                                      /* a left Shift holds Shift down */
    p = rd();
    cellPad_host_mouse_wheel(2);
    KEYS();                                                               /* let go before the second notch's turn */
    for (int i = 0; i < 8; i++) seq[i] = rd().buttons;
    CHECK(seq[0] == RIGHT && seq[1] == RIGHT && seq[2] == 0 && seq[3] == 0 && seq[4] == RIGHT && seq[5] == RIGHT && seq[6] == 0);

    /* left and right Shift: a shift+ chord takes lshift's bindings too, an lshift+ chord takes shift's */
    set_env("PAD_KEYS", "r1=mouse1,r2=shift+mouse1,triangle=lshift");
    KEYS(VK_LSHIFT);
    p = rd(); CHECK(p.buttons == TRIANGLE);
    cellPad_host_mouse_buttons(1);
    p = rd(); CHECK(p.buttons == R2);                                      /* not TRIANGLE as well */
    cellPad_host_mouse_buttons(0);
    p = rd(); CHECK(p.buttons == TRIANGLE);
    set_env("PAD_KEYS", "r1=mouse1,r2=lshift+mouse1,triangle=shift");
    cellPad_host_mouse_buttons(1);
    p = rd(); CHECK(p.buttons == R2);                                      /* not TRIANGLE as well */
    cellPad_host_mouse_buttons(0);
    KEYS(VK_RSHIFT);
    p = rd(); CHECK(p.buttons == TRIANGLE);
    cellPad_host_mouse_buttons(1);
    p = rd(); CHECK(p.buttons == (R1 | TRIANGLE));                         /* the right Shift is not the chord's */
    cellPad_host_mouse_buttons(0);
    KEYS();

    /* PAD_WALK outside 0-100 is held to it */
    set_env("PAD_KEYS", "ls_up=w,walk=lalt");
    set_env("PAD_WALK", "250");
    KEYS('W', VK_LMENU);
    p = rd(); CHECK(p.ly == 0);
    set_env("PAD_WALK", "-5");
    p = rd(); CHECK(p.ly == 128);
    set_env("PAD_WALK", NULL);
    p = rd(); CHECK(p.ly == 57);

    /* one keyboard poll at a time: a poll that finds another one running leaves the keys to it */
    set_env("PAD_KEYS", NULL);
    KEYS('Z');
    p = rd(); CHECK(p.buttons == CROSS);
    s_kb_busy = 1;
    KEYS();
    p = rd(); CHECK(p.buttons == CROSS);
    s_kb_busy = 0;
    p = rd(); CHECK(p.buttons == 0);

    printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
