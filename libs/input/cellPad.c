/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend selection:
 *   - Windows default: XInput (no extra dependencies)
 *   - Everywhere else / if PS3RECOMP_PAD_USE_SDL2 is defined: SDL2 GameController
 *
 * Define PS3RECOMP_PAD_USE_SDL2 to force SDL2 backend on Windows.
 */

#include <time.h>
#include "ps3emu/ps3_knob.h"
#include "cellPad.h"
#include "pad_stick.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../runtime/platform/win32_compat.h"   /* GetTickCount64 on POSIX */
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host */

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_PAD_USE_SDL2)
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#elif defined(_WIN32)
  #define PAD_BACKEND_SDL2  0
  #define PAD_BACKEND_XINPUT 1
#else
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#endif

#if PAD_BACKEND_XINPUT
  #include "pad_hid_sony.h"
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <xinput.h>
  #pragma comment(lib, "xinput.lib")
#endif

#if PAD_BACKEND_SDL2
  #include <SDL2/SDL.h>
  #include "pad_hid_sony.h"   /* (pad_hid_sony_poll off Windows: the SDL pads, below) */
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PAD_MAX_HOST_PORTS  4  /* XInput supports max 4; SDL may support more */

typedef struct {
    int  connected;
    u16  buttons;           /* CELL_PAD_CTRL_* bitmask */
    u8   analog_lx;         /* 0-255, center=128 */
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;        /* 0-255 */
    u8   trigger_r2;        /* 0-255 */
    /* Pressure-sensitive face buttons (0-255) */
    u8   press_right;
    u8   press_left;
    u8   press_up;
    u8   press_down;
    u8   press_triangle;
    u8   press_circle;
    u8   press_cross;
    u8   press_square;
    u8   press_l1;
    u8   press_r1;
} PadHostState;

static int           s_pad_initialized = 0;
static u32           s_max_connect = 0;
static u32           s_port_setting[CELL_PAD_MAX_PORT_NUM];
static PadHostState  s_host_state[PAD_MAX_HOST_PORTS];
/* Per-port "a report has arrived since your last read" flag; see cellPadGetData. */
static int s_data_fresh[PAD_MAX_HOST_PORTS];
static volatile long s_host_hold;   /* cellPad_set_host_hold */
/* Where each host port's vibration goes (the vibration thread, below): what
 * the poll found on the port. s_poll_dev is the poll's own copy, published
 * once a sweep is done, so the thread never sees a port empty mid-sweep. */
enum { RUMBLE_NONE = 0, RUMBLE_XINPUT = 1, RUMBLE_SDL = 2, RUMBLE_HID = 0x10 /* | the HID pad's slot */ };
static long          s_poll_dev[PAD_MAX_HOST_PORTS];
static volatile long s_rumble_dev[PAD_MAX_HOST_PORTS];

/* Host mouse input and the menu layer for the keyboard pad (cellPad_host_mouse_buttons, _mouse_wheel, _set_menu),
 * and the buttons a wheel notch presses now, up and down (pad_poll_keyboard works them out from PAD_KEYS). */
static volatile unsigned s_mouse_buttons;
static volatile int      s_menu_layer;
static volatile unsigned s_wheel_bits[2];

/* Pressure values for hosts with digital buttons: 0 or 255. */
static void pad_press_from_buttons(PadHostState* hs)
{
    const u16 btns = hs->buttons;
    hs->press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
    hs->press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
    hs->press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
    hs->press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
    hs->press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
    hs->press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
    hs->press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
    hs->press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
    hs->press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
    hs->press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
}

/* The PS3 pad's buttons by name, for PAD_MAP and PAD_KEYS: cross circle
 * square triangle l1 r1 l2 r2 l3 r3 start select up down left right. */
static const struct { const char* name; u16 bit; } s_pad_names[] = {
    { "select", CELL_PAD_CTRL_SELECT }, { "l3", CELL_PAD_CTRL_L3 },
    { "r3", CELL_PAD_CTRL_R3 },         { "start", CELL_PAD_CTRL_START },
    { "up", CELL_PAD_CTRL_UP },         { "right", CELL_PAD_CTRL_RIGHT },
    { "down", CELL_PAD_CTRL_DOWN },     { "left", CELL_PAD_CTRL_LEFT },
    { "l2", CELL_PAD_CTRL_L2 },         { "r2", CELL_PAD_CTRL_R2 },
    { "l1", CELL_PAD_CTRL_L1 },         { "r1", CELL_PAD_CTRL_R1 },
    { "triangle", CELL_PAD_CTRL_TRIANGLE }, { "circle", CELL_PAD_CTRL_CIRCLE },
    { "cross", CELL_PAD_CTRL_CROSS },   { "square", CELL_PAD_CTRL_SQUARE },
};

/* Case-insensitive: the n characters at s spell name (lower case). */
static int pad_name_is(const char* s, size_t n, const char* name)
{
    size_t k = 0;
    while (k < n && name[k] && ((s[k] >= 'A' && s[k] <= 'Z') ? (s[k] | 0x20) : s[k]) == name[k]) k++;
    return k == n && !name[k];
}

static int pad_name_index(const char* s, size_t n)
{
    for (int i = 0; i < 16; i++)
        if (pad_name_is(s, n, s_pad_names[i].name)) return i;
    return -1;
}

/* PAD_KEYS's targets after the 16 buttons (see the keyboard pad). */
#define PAD_TARGETS  25
#define PAD_T_LS     16   /* ls_up ls_down ls_left ls_right */
#define PAD_T_RS     20   /* rs_up rs_down rs_left rs_right */
#define PAD_T_WALK   24
static const char* const s_pad_key_targets[PAD_TARGETS - 16] = {
    "ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left", "rs_right", "walk",
};

static int pad_target_index(const char* s, size_t n)
{
    const int i = pad_name_index(s, n);
    if (i >= 0) return i;
    for (int t = 16; t < PAD_TARGETS; t++)
        if (pad_name_is(s, n, s_pad_key_targets[t - 16])) return t;
    return -1;
}

/* The next game=value pair of a PAD_MAP / PAD_KEYS list. Returns where the
 * pair ends (NULL at the end of the list); *dst is the game button's index
 * into s_pad_names (keys: or a PAD_KEYS target past them), or -1 for a
 * malformed pair. */
static const char* pad_next_pair(const char* e, int* dst, const char** v, size_t* vn, int keys)
{
    while (*e == ',' || *e == ' ' || *e == ';') e++;
    if (!*e) return NULL;
    const char* end = e;
    while (*end && *end != ',' && *end != ' ' && *end != ';') end++;
    const char* eq = e;
    while (eq < end && *eq != '=') eq++;
    *dst = eq < end ? (keys ? pad_target_index : pad_name_index)(e, (size_t)(eq - e)) : -1;
    *v = eq < end ? eq + 1 : end;
    *vn = (size_t)(end - *v);
    if (*dst < 0 || !*vn)
        fprintf(stderr, "[cellPad] ignoring \"%.*s\" in a button list\n", (int)(end - e), e);
    return end;
}

#if PAD_BACKEND_SDL2
static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];
static int s_sdl_inited = 0;
#endif

/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_XINPUT

#define PAD_TRIGGER_THRESHOLD 30
#endif

#if PAD_BACKEND_XINPUT || PAD_BACKEND_SDL2
/* One stick, -32768..32767 per axis with down positive, through the radial
 * deadzone (pad_stick.h). PAD_STICK_DEADZONE (of 32767, default 7849, as
 * XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE and RPCS3) and PAD_STICK_ANTI_DEADZONE
 * (percent, default 13, RPCS3's). */
static void pad_host_stick_to_u8(int x, int y, u8* out_x, u8* out_y)
{
    static int d = -1, a = -1;
    PS3_KNOB(d, ps3_env_int("PAD_STICK_DEADZONE", 7849));
    PS3_KNOB(a, ps3_env_int("PAD_STICK_ANTI_DEADZONE", 13));
    pad_stick_to_u8(x / 32767.0f, y / 32767.0f, d / 32767.0f, a / 100.0f, out_x, out_y);
}
#endif

#if PAD_BACKEND_XINPUT || PAD_BACKEND_SDL2

/* Keyboard fallback for port 0.
 *
 * The only backend here is XInput, so on a machine with no controller plugged
 * in a title gets a pad that is reported present and never presses anything --
 * it can be watched but not played. That is what The Simpsons Arcade Game hit:
 * it reached its attract loop and no input existed to start a game.
 *
 * Drives port 0. With no pad there it stands in for one; with a pad there
 * (XInput, or a Sony pad over HID) it is merged in -- keys OR into the
 * buttons, and keys that move a stick take over that axis only while held --
 * so the keyboard keeps working with a controller plugged in. Keys are
 * read only while a window of THIS process is in the foreground, so typing in
 * another application does not drive the game. Set PAD_NO_KEYBOARD=1 to
 * disable it entirely.
 *
 * Arrows = d-pad, Z/X/A/S = cross/circle/square/triangle, Q/W = L1/R1,
 * 1/2 = L2/R2, Enter = START, Tab = SELECT. With no keys bound to the left
 * stick (below), the left stick mirrors the d-pad so a title that reads the
 * stick instead is playable too.
 *
 * Off Windows the keys come from SDL's keyboard state, which the game window's
 * thread keeps (the D3D12 backend on vkd3d), and "this process's window" is
 * the window SDL says has the keyboard: none in a headless run, so the keyboard
 * stays off there. The bindings keep their Windows virtual-key codes. */
#ifdef _WIN32
static int pad_host_window_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static int pad_host_key_down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}
#else
#define VK_BACK     0x08
#define VK_TAB      0x09
#define VK_RETURN   0x0D
#define VK_SHIFT    0x10
#define VK_CONTROL  0x11
#define VK_MENU     0x12
#define VK_CAPITAL  0x14
#define VK_ESCAPE   0x1B
#define VK_SPACE    0x20
#define VK_PRIOR    0x21
#define VK_NEXT     0x22
#define VK_END      0x23
#define VK_HOME     0x24
#define VK_LEFT     0x25
#define VK_UP       0x26
#define VK_RIGHT    0x27
#define VK_DOWN     0x28
#define VK_INSERT   0x2D
#define VK_DELETE   0x2E
#define VK_NUMPAD0  0x60
#define VK_F1       0x70
#define VK_LSHIFT   0xA0
#define VK_RSHIFT   0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU    0xA4
#define VK_RMENU    0xA5

static int pad_host_window_focused(void)
{
    return SDL_GetKeyboardFocus() != NULL;
}

/* A virtual-key code's keys: letters and digits through the keyboard layout,
 * as Windows' own codes follow it, and the rest by position. */
static int pad_host_key_down(int vk)
{
    const Uint8* st = SDL_GetKeyboardState(NULL);
    SDL_Scancode a = SDL_SCANCODE_UNKNOWN, b = SDL_SCANCODE_UNKNOWN;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        a = SDL_GetScancodeFromKey((SDL_Keycode)(vk >= 'A' ? vk + 0x20 : vk));
    else if (vk >= VK_F1 && vk < VK_F1 + 12)
        a = (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - VK_F1));
    else if (vk >= VK_F1 + 12 && vk < VK_F1 + 24)
        a = (SDL_Scancode)(SDL_SCANCODE_F13 + (vk - VK_F1 - 12));
    else if (vk > VK_NUMPAD0 && vk <= VK_NUMPAD0 + 9)
        a = (SDL_Scancode)(SDL_SCANCODE_KP_1 + (vk - VK_NUMPAD0 - 1));
    else switch (vk) {
    case VK_NUMPAD0:  a = SDL_SCANCODE_KP_0; break;
    case VK_BACK:     a = SDL_SCANCODE_BACKSPACE; break;
    case VK_TAB:      a = SDL_SCANCODE_TAB; break;
    case VK_RETURN:   a = SDL_SCANCODE_RETURN; b = SDL_SCANCODE_KP_ENTER; break;
    case VK_SHIFT:    a = SDL_SCANCODE_LSHIFT; b = SDL_SCANCODE_RSHIFT; break;
    case VK_CONTROL:  a = SDL_SCANCODE_LCTRL; b = SDL_SCANCODE_RCTRL; break;
    case VK_MENU:     a = SDL_SCANCODE_LALT; b = SDL_SCANCODE_RALT; break;
    case VK_CAPITAL:  a = SDL_SCANCODE_CAPSLOCK; break;
    case VK_ESCAPE:   a = SDL_SCANCODE_ESCAPE; break;
    case VK_SPACE:    a = SDL_SCANCODE_SPACE; break;
    case VK_PRIOR:    a = SDL_SCANCODE_PAGEUP; break;
    case VK_NEXT:     a = SDL_SCANCODE_PAGEDOWN; break;
    case VK_END:      a = SDL_SCANCODE_END; break;
    case VK_HOME:     a = SDL_SCANCODE_HOME; break;
    case VK_LEFT:     a = SDL_SCANCODE_LEFT; break;
    case VK_UP:       a = SDL_SCANCODE_UP; break;
    case VK_RIGHT:    a = SDL_SCANCODE_RIGHT; break;
    case VK_DOWN:     a = SDL_SCANCODE_DOWN; break;
    case VK_INSERT:   a = SDL_SCANCODE_INSERT; break;
    case VK_DELETE:   a = SDL_SCANCODE_DELETE; break;
    case VK_LSHIFT:   a = SDL_SCANCODE_LSHIFT; break;
    case VK_RSHIFT:   a = SDL_SCANCODE_RSHIFT; break;
    case VK_LCONTROL: a = SDL_SCANCODE_LCTRL; break;
    case VK_RCONTROL: a = SDL_SCANCODE_RCTRL; break;
    case VK_LMENU:    a = SDL_SCANCODE_LALT; break;
    case VK_RMENU:    a = SDL_SCANCODE_RALT; break;
    case 0x6A: a = SDL_SCANCODE_KP_MULTIPLY; break;
    case 0x6B: a = SDL_SCANCODE_KP_PLUS; break;
    case 0x6D: a = SDL_SCANCODE_KP_MINUS; break;
    case 0x6E: a = SDL_SCANCODE_KP_PERIOD; break;
    case 0x6F: a = SDL_SCANCODE_KP_DIVIDE; break;
    case 0xBA: a = SDL_SCANCODE_SEMICOLON; break;      /* VK_OEM_1 */
    case 0xBB: a = SDL_SCANCODE_EQUALS; break;         /* VK_OEM_PLUS */
    case 0xBC: a = SDL_SCANCODE_COMMA; break;
    case 0xBD: a = SDL_SCANCODE_MINUS; break;
    case 0xBE: a = SDL_SCANCODE_PERIOD; break;
    case 0xBF: a = SDL_SCANCODE_SLASH; break;          /* VK_OEM_2 */
    case 0xC0: a = SDL_SCANCODE_GRAVE; break;          /* VK_OEM_3 */
    case 0xDB: a = SDL_SCANCODE_LEFTBRACKET; break;    /* VK_OEM_4 */
    case 0xDC: a = SDL_SCANCODE_BACKSLASH; break;      /* VK_OEM_5 */
    case 0xDD: a = SDL_SCANCODE_RIGHTBRACKET; break;   /* VK_OEM_6 */
    case 0xDE: a = SDL_SCANCODE_APOSTROPHE; break;     /* VK_OEM_7 */
    default: break;
    }
    return (a != SDL_SCANCODE_UNKNOWN && st[a]) || (b != SDL_SCANCODE_UNKNOWN && st[b]);
}
#endif

/* cellPad.h. Off Windows the mouse buttons' codes read SDL's mouse state. */
int cellPad_host_key_down(int vk)
{
#ifndef _WIN32
    static const struct { int vk; Uint32 mask; } kButtons[] = {
        { 0x01, SDL_BUTTON_LMASK },  { 0x02, SDL_BUTTON_RMASK },  { 0x04, SDL_BUTTON_MMASK },
        { 0x05, SDL_BUTTON_X1MASK }, { 0x06, SDL_BUTTON_X2MASK },
    };
    for (size_t i = 0; i < sizeof kButtons / sizeof kButtons[0]; i++)
        if (vk == kButtons[i].vk) return (SDL_GetMouseState(NULL, NULL) & kButtons[i].mask) != 0;
#endif
    return pad_host_key_down(vk);
}

/* The keyboard pad's bindings. PAD_KEYS changes them: target=binding pairs.
 * Targets: the 16 buttons (s_pad_names), the left stick's four directions
 * (ls_up ls_down ls_left ls_right), the right stick's (rs_up ...), and walk:
 * while it is held, keys move the left stick only PAD_WALK percent of the way
 * (default 55), so a title that picks walking or running from the stick's
 * deflection walks. Targets not listed keep the default keys below; with no
 * left-stick keys bound, the left stick follows the d-pad as before. Live.
 *
 * A target takes up to PAD_BIND_MAX bindings, separated by '|'. A binding is
 * an input, with an optional modifier input before a '+' (a chord), and an
 * optional "menu:" prefix: it then counts only while the title says one of
 * its menus is open (cellPad_host_set_menu), and the same input's other
 * bindings do not count meanwhile. While a chord is held, the bindings of its
 * two inputs alone do not count: with r1=mouse1 and r2=shift+mouse1, Shift
 * and a left click give R2, not R1 as well. A held input keeps the binding it
 * went down with until it is let go: letting go of Shift first leaves R2
 * held, and Shift pressed during a click leaves R1. A left or right Shift,
 * Ctrl or Alt holds the plain one down too, so lshift's bindings also wait
 * while a shift+ chord is held, and shift's while an lshift+ one is. "none"
 * binds nothing.
 *
 *   PAD_KEYS="r1=mouse1,r2=shift+mouse1,circle=space|menu:q,r3=q|mouse3"
 *
 * An input is a letter or digit, a key name (s_key_names, and f1 to f24), a
 * Windows virtual-key code ("0x10"), mouse1 to mouse5 (the buttons the title
 * passes in, cellPad_host_mouse_buttons), or wheelup / wheeldown (notches,
 * cellPad_host_mouse_wheel; each presses its button for PAD_WHEEL_READS
 * packets, then lets go as long, see pad_host_overlay). */
#define PAD_BIND_MAX    4
#define PAD_IN_MOUSE    0x100   /* mouse1..mouse5: 0x100..0x104 */
#define PAD_IN_WHEEL_UP 0x105
#define PAD_IN_WHEEL_DN 0x106
#define PAD_IN_SLOTS    0x107

typedef struct { u16 in, mod; u8 menu; } PadBind;
static PadBind s_binds[PAD_TARGETS][PAD_BIND_MAX];
static u8      s_nbind[PAD_TARGETS];
static u16     s_watch[PAD_IN_MOUSE];   /* the keys the bindings name, read once a poll */
static int     s_nwatch;
/* Which bindings a held input counts for, chosen when it went down and kept until it is let go: 0 its plain ones,
 * else the modifier of the chord it went down with (pad_poll_keyboard). */
static u8      s_held[PAD_IN_SLOTS];
static u16     s_held_mod[PAD_IN_SLOTS];

/* The default keys, in s_pad_names order (0 = no key). */
static const int s_pad_keys_default[16] = {
    VK_TAB, 0, 0, VK_RETURN,                  /* select l3 r3 start */
    VK_UP, VK_RIGHT, VK_DOWN, VK_LEFT,
    '1', '2', 'Q', 'W',                       /* l2 r2 l1 r1 */
    'S', 'X', 'Z', 'A',                       /* triangle circle cross square */
};

static const struct { const char* name; u16 code; } s_key_names[] = {
    { "space", VK_SPACE },   { "enter", VK_RETURN },  { "return", VK_RETURN }, { "tab", VK_TAB },
    { "esc", VK_ESCAPE },    { "escape", VK_ESCAPE }, { "backspace", VK_BACK },
    { "shift", VK_SHIFT },   { "lshift", VK_LSHIFT }, { "rshift", VK_RSHIFT },
    { "ctrl", VK_CONTROL },  { "lctrl", VK_LCONTROL }, { "rctrl", VK_RCONTROL },
    { "alt", VK_MENU },      { "lalt", VK_LMENU },    { "ralt", VK_RMENU },
    { "up", VK_UP },         { "down", VK_DOWN },     { "left", VK_LEFT },     { "right", VK_RIGHT },
    { "insert", VK_INSERT }, { "delete", VK_DELETE }, { "home", VK_HOME },     { "end", VK_END },
    { "pageup", VK_PRIOR },  { "pagedown", VK_NEXT }, { "capslock", VK_CAPITAL },
    { "mouse1", PAD_IN_MOUSE + 0 }, { "mouse2", PAD_IN_MOUSE + 1 }, { "mouse3", PAD_IN_MOUSE + 2 },
    { "mouse4", PAD_IN_MOUSE + 3 }, { "mouse5", PAD_IN_MOUSE + 4 },
    { "wheelup", PAD_IN_WHEEL_UP }, { "wheeldown", PAD_IN_WHEEL_DN },
};

/* One input's code from the n characters at s, or 0. */
static u16 pad_parse_input(const char* s, size_t n)
{
    if (n == 1 && ((s[0] >= '0' && s[0] <= '9') || ((s[0] | 0x20) >= 'a' && (s[0] | 0x20) <= 'z')))
        return (u16)(s[0] >= 'a' ? s[0] - 0x20 : s[0]);
    for (size_t i = 0; i < sizeof s_key_names / sizeof s_key_names[0]; i++)
        if (pad_name_is(s, n, s_key_names[i].name)) return s_key_names[i].code;
    if (n >= 2 && n <= 3 && (s[0] | 0x20) == 'f' && s[1] >= '1' && s[1] <= '9') {
        const int f = n == 2 ? s[1] - '0' : (s[2] >= '0' && s[2] <= '9' ? (s[1] - '0') * 10 + s[2] - '0' : 0);
        return f >= 1 && f <= 24 ? (u16)(VK_F1 + f - 1) : 0;
    }
    if (s[0] >= '0' && s[0] <= '9' && n < 16) {
        char buf[16];
        memcpy(buf, s, n); buf[n] = 0;
        return (u16)(strtol(buf, NULL, 0) & 0xFF);
    }
    return 0;
}

/* Target t's bindings from a PAD_KEYS value (replacing its defaults). */
static void pad_parse_value(int t, const char* v, size_t vn)
{
    s_nbind[t] = 0;
    const char* end = v + vn;
    while (v < end) {
        const char* alt = v;
        while (alt < end && *alt != '|') alt++;
        const char* p = v;
        u8 menu = 0;
        if (alt - p > 5 && pad_name_is(p, 5, "menu:")) { menu = 1; p += 5; }
        const char* plus = p;
        while (plus < alt && *plus != '+') plus++;
        PadBind b = { 0, 0, menu };
        if (plus < alt) { b.mod = pad_parse_input(p, (size_t)(plus - p)); b.in = pad_parse_input(plus + 1, (size_t)(alt - plus - 1)); }
        else            b.in = pad_parse_input(p, (size_t)(alt - p));
        const int none = !menu && plus == alt && pad_name_is(p, (size_t)(alt - p), "none");
        if (!none && (!b.in || (plus < alt && (!b.mod || b.mod >= PAD_IN_WHEEL_UP))))
            fprintf(stderr, "[cellPad] PAD_KEYS: ignoring \"%.*s\" for %s\n", (int)(alt - v), v,
                    t < 16 ? s_pad_names[t].name : s_pad_key_targets[t - 16]);
        else if (!none && s_nbind[t] < PAD_BIND_MAX)
            s_binds[t][s_nbind[t]++] = b;
        v = alt + 1;
    }
}

/* A left or right Shift, Ctrl or Alt: the plain key it also holds down (GetAsyncKeyState(VK_SHIFT) is either
 * Shift). Any other input: itself. */
static u16 pad_mod_plain(u16 c)
{
    switch (c) {
    case VK_LSHIFT:   case VK_RSHIFT:   return VK_SHIFT;
    case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
    case VK_LMENU:    case VK_RMENU:    return VK_MENU;
    default:                            return c;
    }
}

/* c, or for Shift, Ctrl or Alt the plain key and both sides, into out; returns how many. */
static int pad_mod_family(u16 c, u16 out[3])
{
    switch (pad_mod_plain(c)) {
    case VK_SHIFT:   out[0] = VK_SHIFT;   out[1] = VK_LSHIFT;   out[2] = VK_RSHIFT;   return 3;
    case VK_CONTROL: out[0] = VK_CONTROL; out[1] = VK_LCONTROL; out[2] = VK_RCONTROL; return 3;
    case VK_MENU:    out[0] = VK_MENU;    out[1] = VK_LMENU;    out[2] = VK_RMENU;    return 3;
    default:         out[0] = c;                                                    return 1;
    }
}

static void pad_watch(u16 code)
{
    if (!code || code >= PAD_IN_MOUSE) return;
    u16 fam[3];
    const int n = pad_mod_family(code, fam);   /* a modifier's both sides are read (pad_take) */
    for (int k = 0; k < n; k++) {
        int i = 0;
        while (i < s_nwatch && s_watch[i] != fam[k]) i++;
        if (i == s_nwatch && s_nwatch < PAD_IN_MOUSE) s_watch[s_nwatch++] = fam[k];
    }
}

/* Input c is part of a chord held now: its bindings alone do not count, and for Shift, Ctrl or Alt neither do the
 * bindings of the keys holding it down: shift+ takes lshift and rshift too, lshift+ takes shift but not rshift. */
static void pad_take(u8* taken, u16 c)
{
    taken[c] = 1;
    const u16 plain = pad_mod_plain(c);
    if (plain != c) { taken[plain] = 1; return; }
    u16 fam[3];
    const int n = pad_mod_family(c, fam);
    for (int k = 0; k < n; k++) taken[fam[k]] = 1;
}

static int pad_parse_keys(const char* e)
{
    int dst;
    const char* v;
    size_t vn;
    memset(s_nbind, 0, sizeof s_nbind);
    for (int t = 0; t < 16; t++)
        if (s_pad_keys_default[t]) { s_binds[t][0] = (PadBind){ (u16)s_pad_keys_default[t], 0, 0 }; s_nbind[t] = 1; }
    while (e && (e = pad_next_pair(e, &dst, &v, &vn, 1)) != NULL)
        if (dst >= 0) pad_parse_value(dst, v, vn);
    s_nwatch = 0;
    for (int t = 0; t < PAD_TARGETS; t++)
        for (int j = 0; j < s_nbind[t]; j++) { pad_watch(s_binds[t][j].in); pad_watch(s_binds[t][j].mod); }
    return 1;
}

/* A stick axis byte: dir -1, 0 or 1, deflected r of the way. */
static u8 pad_axis_byte(int dir, float r)
{
    r = r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r;
    return (u8)(dir < 0 ? lroundf(127.5f - 127.5f * r) : dir > 0 ? lroundf(127.5f + 127.5f * r) : 128);
}

/* One keyboard poll (pad_poll_keyboard runs one at a time). */
static void pad_poll_keyboard_once(void)
{
    static int off = -1, keys = -1, walk = -1;
    PS3_KNOB(off, getenv("PAD_NO_KEYBOARD") ? 1 : 0);
    if (off) return;
    PS3_KNOB(keys, pad_parse_keys(getenv("PAD_KEYS")));
    (void)keys;
    PS3_KNOB(walk, ps3_env_int("PAD_WALK", 55));

    PadHostState* hs = &s_host_state[0];
    const int pad = hs->connected;   /* a real pad on port 0: merge into it */
    if (!pad_host_window_focused()) {
        /* Release everything on focus loss, or a key held while alt-tabbing
         * would stay down forever. */
        s_wheel_bits[0] = s_wheel_bits[1] = 0;
        memset(s_held, 0, sizeof s_held);   /* keys still held on the way back start over */
        if (!pad) {
            hs->buttons = 0;
            hs->analog_lx = hs->analog_ly = 128;
            hs->analog_rx = hs->analog_ry = 128;
            hs->trigger_l2 = hs->trigger_r2 = 0;
            pad_press_from_buttons(hs);
            hs->connected = 1;
        }
        return;
    }

    u8 down[PAD_IN_SLOTS], claimed[PAD_IN_SLOTS], chorded[PAD_IN_SLOTS], fresh[PAD_IN_SLOTS];
    memset(down, 0, sizeof down);
    memset(claimed, 0, sizeof claimed);
    memset(chorded, 0, sizeof chorded);
    for (int i = 0; i < s_nwatch; i++) down[s_watch[i]] = (u8)pad_host_key_down(s_watch[i]);
    const unsigned mouse = s_mouse_buttons;
    for (int i = 0; i < 5; i++) down[PAD_IN_MOUSE + i] = (u8)((mouse >> i) & 1);
    const int menu = s_menu_layer;
    if (menu)   /* inputs a menu binding takes over while a menu is open */
        for (int t = 0; t < PAD_TARGETS; t++)
            for (int j = 0; j < s_nbind[t]; j++)
                if (s_binds[t][j].menu) claimed[s_binds[t][j].in] = 1;
#define PAD_LAYER_OK(b) ((b).menu ? menu : !claimed[(b).in])
    for (int c = 0; c < PAD_IN_WHEEL_UP; c++) {   /* an input that just went down counts for its plain bindings... */
        fresh[c] = down[c] && !s_held[c];
        s_held[c] = down[c];
        if (fresh[c]) s_held_mod[c] = 0;
    }
    for (int t = 0; t < PAD_TARGETS; t++)   /* ...or for a chord whose modifier is held then, until it is let go */
        for (int j = 0; j < s_nbind[t]; j++) {
            const PadBind b = s_binds[t][j];
            if (b.mod && b.in < PAD_IN_WHEEL_UP && fresh[b.in] && !s_held_mod[b.in] && PAD_LAYER_OK(b) && down[b.mod])
                s_held_mod[b.in] = b.mod;
        }
    for (int t = 0; t < PAD_TARGETS; t++)   /* chords held now: their inputs alone do not count */
        for (int j = 0; j < s_nbind[t]; j++) {
            const PadBind b = s_binds[t][j];
            if (b.mod && b.in < PAD_IN_WHEEL_UP && PAD_LAYER_OK(b) && down[b.in] && s_held_mod[b.in] == b.mod) {
                pad_take(chorded, b.in);
                pad_take(chorded, b.mod);
            }
        }
    unsigned on = 0, wheel_chord[2] = { 0, 0 }, wheel_plain[2] = { 0, 0 };
    for (int t = 0; t < PAD_TARGETS; t++)
        for (int j = 0; j < s_nbind[t]; j++) {
            const PadBind b = s_binds[t][j];
            if (!PAD_LAYER_OK(b)) continue;
            if (b.in >= PAD_IN_WHEEL_UP) {   /* notches press buttons only */
                const int d = b.in - PAD_IN_WHEEL_UP;
                if (t >= 16) continue;
                if (!b.mod)          wheel_plain[d] |= s_pad_names[t].bit;
                else if (down[b.mod]) wheel_chord[d] |= s_pad_names[t].bit;
                continue;
            }
            if (b.mod ? (down[b.in] && s_held_mod[b.in] == b.mod)
                      : (down[b.in] && !s_held_mod[b.in] && !chorded[b.in])) on |= 1u << t;
        }
#undef PAD_LAYER_OK
    s_wheel_bits[0] = wheel_chord[0] ? wheel_chord[0] : wheel_plain[0];
    s_wheel_bits[1] = wheel_chord[1] ? wheel_chord[1] : wheel_plain[1];

    u16 btns = 0;
    for (int t = 0; t < 16; t++)
        if (on & (1u << t)) btns |= s_pad_names[t].bit;

#define PAD_ON(t) ((on >> (t)) & 1u)
    const int ls_keys = s_nbind[PAD_T_LS] || s_nbind[PAD_T_LS + 1] || s_nbind[PAD_T_LS + 2] || s_nbind[PAD_T_LS + 3];
    const int lx = ls_keys ? (int)PAD_ON(PAD_T_LS + 3) - (int)PAD_ON(PAD_T_LS + 2)
                           : (btns & CELL_PAD_CTRL_LEFT) ? -1 : (btns & CELL_PAD_CTRL_RIGHT) ? 1 : 0;
    const int ly = ls_keys ? (int)PAD_ON(PAD_T_LS + 1) - (int)PAD_ON(PAD_T_LS)
                           : (btns & CELL_PAD_CTRL_UP) ? -1 : (btns & CELL_PAD_CTRL_DOWN) ? 1 : 0;
    const int rx = (int)PAD_ON(PAD_T_RS + 3) - (int)PAD_ON(PAD_T_RS + 2);
    const int ry = (int)PAD_ON(PAD_T_RS + 1) - (int)PAD_ON(PAD_T_RS);
    float r = 1.0f;
    if (ls_keys && PAD_ON(PAD_T_WALK)) {   /* the same walking speed in all eight directions */
        r = (float)(walk < 0 ? 0 : walk > 100 ? 100 : walk) / 100.0f;
        if (lx && ly) r *= 0.70710678f;
    }
#undef PAD_ON

    hs->connected = 1;
    if (!pad) {
        hs->buttons   = btns;
        hs->analog_lx = pad_axis_byte(lx, r);
        hs->analog_ly = pad_axis_byte(ly, r);
        hs->analog_rx = pad_axis_byte(rx, 1.0f);
        hs->analog_ry = pad_axis_byte(ry, 1.0f);
        hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
        hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);
    } else {
        hs->buttons |= btns;
        if (lx) hs->analog_lx = pad_axis_byte(lx, r);
        if (ly) hs->analog_ly = pad_axis_byte(ly, r);
        if (rx) hs->analog_rx = pad_axis_byte(rx, 1.0f);
        if (ry) hs->analog_ry = pad_axis_byte(ry, 1.0f);
        if (btns & CELL_PAD_CTRL_L2) hs->trigger_l2 = 255;
        if (btns & CELL_PAD_CTRL_R2) hs->trigger_r2 = 255;
    }
    pad_press_from_buttons(hs);

    { static int said = 0;
      if (!said && (btns || lx || ly || rx || ry)) { said = 1;
          printf("[cellPad] keyboard input on port 0 (%s)\n", pad ? "merged with the pad" : "no pad");
          fflush(stdout); } }
}

/* Two threads can poll in the same tick (pad_poll_backend). The keyboard poll runs one at a time: a live PAD_KEYS
 * change rebuilds the bindings it reads, and a second poll at the same moment would only read the same keys. */
static volatile int s_kb_busy;
static void pad_poll_keyboard(void)
{
    if (__atomic_exchange_n(&s_kb_busy, 1, __ATOMIC_ACQUIRE)) return;
    pad_poll_keyboard_once();
    __atomic_store_n(&s_kb_busy, 0, __ATOMIC_RELEASE);
}
#else
int cellPad_host_key_down(int vk) { (void)vk; return 0; }   /* (no host input in this build) */
#endif /* PAD_BACKEND_XINPUT || PAD_BACKEND_SDL2 */

#if PAD_BACKEND_XINPUT
static void pad_poll_xinput(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        DWORD result = XInputGetState((DWORD)i, &state);
        if (result != ERROR_SUCCESS) {
            s_host_state[i].connected = 0;
            s_poll_dev[i] = RUMBLE_NONE;
            continue;
        }

        s_host_state[i].connected = 1;
        s_poll_dev[i] = RUMBLE_XINPUT;
        XINPUT_GAMEPAD* gp = &state.Gamepad;

        /* Map XInput buttons to PS3 CELL_PAD_CTRL_* */
        u16 btns = 0;
        if (gp->wButtons & XINPUT_GAMEPAD_BACK)           btns |= CELL_PAD_CTRL_SELECT;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     btns |= CELL_PAD_CTRL_L3;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    btns |= CELL_PAD_CTRL_R3;
        if (gp->wButtons & XINPUT_GAMEPAD_START)          btns |= CELL_PAD_CTRL_START;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_UP)        btns |= CELL_PAD_CTRL_UP;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     btns |= CELL_PAD_CTRL_RIGHT;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      btns |= CELL_PAD_CTRL_DOWN;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      btns |= CELL_PAD_CTRL_LEFT;
        if (gp->bLeftTrigger > PAD_TRIGGER_THRESHOLD)     btns |= CELL_PAD_CTRL_L2;
        if (gp->bRightTrigger > PAD_TRIGGER_THRESHOLD)    btns |= CELL_PAD_CTRL_R2;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  btns |= CELL_PAD_CTRL_L1;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btns |= CELL_PAD_CTRL_R1;
        if (gp->wButtons & XINPUT_GAMEPAD_Y)              btns |= CELL_PAD_CTRL_TRIANGLE;
        if (gp->wButtons & XINPUT_GAMEPAD_B)              btns |= CELL_PAD_CTRL_CIRCLE;
        if (gp->wButtons & XINPUT_GAMEPAD_A)              btns |= CELL_PAD_CTRL_CROSS;
        if (gp->wButtons & XINPUT_GAMEPAD_X)              btns |= CELL_PAD_CTRL_SQUARE;

        s_host_state[i].buttons = btns;

        /* Analog sticks. PS3 Y axis is inverted vs XInput (up = 0): negate it
         * before the conversion, so a centred stick stays 128. 127 would alias
         * SELECT+START (0x7F) and can make the guest self-exit.
         * (via sagemono, PR #42) */
        pad_host_stick_to_u8(gp->sThumbLX, -gp->sThumbLY,
                             &s_host_state[i].analog_lx, &s_host_state[i].analog_ly);
        pad_host_stick_to_u8(gp->sThumbRX, -gp->sThumbRY,
                             &s_host_state[i].analog_rx, &s_host_state[i].analog_ry);

        /* Triggers */
        s_host_state[i].trigger_l2 = gp->bLeftTrigger;
        s_host_state[i].trigger_r2 = gp->bRightTrigger;

        /* Pressure-sensitive buttons: XInput has digital only, so 0 or 255 */
        pad_press_from_buttons(&s_host_state[i]);
    }
}

/* Sony pads over HID (pad_hid_sony.c) take the host ports XInput left
 * empty, in connection order. */
static void pad_poll_hid(void)
{
    PadHidSonyState hid[PAD_HID_SONY_MAX];
    const int n = pad_hid_sony_poll(hid, PAD_HID_SONY_MAX);
    for (int k = 0, i = 0; k < n; k++) {
        while (i < PAD_MAX_HOST_PORTS && s_host_state[i].connected) i++;
        if (i >= PAD_MAX_HOST_PORTS) break;
        PadHostState* hs = &s_host_state[i];
        hs->connected  = 1;
        hs->buttons    = hid[k].buttons;
        hs->analog_lx  = hid[k].lx; hs->analog_ly = hid[k].ly;
        hs->analog_rx  = hid[k].rx; hs->analog_ry = hid[k].ry;
        hs->trigger_l2 = hid[k].l2; hs->trigger_r2 = hid[k].r2;
        pad_press_from_buttons(hs);
        s_poll_dev[i] = RUMBLE_HID | hid[k].slot;
    }
}

static void pad_init_backend(void)
{
    /* XInput needs no explicit init */
}

static void pad_shutdown_backend(void)
{
    /* XInput needs no explicit shutdown */
}

#endif /* PAD_BACKEND_XINPUT */

/* ---------------------------------------------------------------------------
 * SDL2 backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_SDL2

static u8 pad_sdl_trigger_to_u8(int raw)
{
    /* SDL trigger: 0..32767 -> 0..255 */
    int val = (raw * 255) / 32767;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

/* One controller's buttons, sticks and triggers, as the PS3 pad has them. */
static void pad_sdl_read(SDL_GameController* gc, PadHostState* hs)
{
    u16 btns = 0;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))          btns |= CELL_PAD_CTRL_SELECT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))     btns |= CELL_PAD_CTRL_L3;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))    btns |= CELL_PAD_CTRL_R3;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))         btns |= CELL_PAD_CTRL_START;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))       btns |= CELL_PAD_CTRL_UP;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))    btns |= CELL_PAD_CTRL_RIGHT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))     btns |= CELL_PAD_CTRL_DOWN;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))     btns |= CELL_PAD_CTRL_LEFT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  btns |= CELL_PAD_CTRL_L1;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) btns |= CELL_PAD_CTRL_R1;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))             btns |= CELL_PAD_CTRL_TRIANGLE;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))             btns |= CELL_PAD_CTRL_CIRCLE;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))             btns |= CELL_PAD_CTRL_CROSS;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))             btns |= CELL_PAD_CTRL_SQUARE;

    /* Triggers via axis */
    int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (lt > 3000) btns |= CELL_PAD_CTRL_L2;
    if (rt > 3000) btns |= CELL_PAD_CTRL_R2;

    hs->buttons = btns;

    /* Analog sticks */
    pad_host_stick_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX),
                         SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY),
                         &hs->analog_lx, &hs->analog_ly);
    pad_host_stick_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX),
                         SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY),
                         &hs->analog_rx, &hs->analog_ry);

    /* Triggers */
    hs->trigger_l2 = pad_sdl_trigger_to_u8(lt);
    hs->trigger_r2 = pad_sdl_trigger_to_u8(rt);

    /* Pressure: SDL has digital buttons, so 0 or 255 */
    pad_press_from_buttons(hs);
}

static void pad_poll_sdl2(void)
{
    SDL_GameControllerUpdate();

    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (!s_sdl_controllers[i]) {
            /* Try to open newly connected controllers */
            if (SDL_IsGameController(i)) {
                s_sdl_controllers[i] = SDL_GameControllerOpen(i);
            }
        }

        SDL_GameController* gc = s_sdl_controllers[i];
        if (!gc || !SDL_GameControllerGetAttached(gc)) {
            s_host_state[i].connected = 0;
            s_poll_dev[i] = RUMBLE_NONE;
            if (gc) {
                SDL_LockJoysticks();   /* (pad_hid_sony_poll reads the list from another thread) */
                SDL_GameControllerClose(gc);
                s_sdl_controllers[i] = NULL;
                SDL_UnlockJoysticks();
            }
            continue;
        }

        s_host_state[i].connected = 1;
        s_poll_dev[i] = RUMBLE_SDL;
        pad_sdl_read(gc, &s_host_state[i]);
    }
}

#ifndef _WIN32
/* pad_hid_sony.h, off Windows: there is no HID reader there (SDL reads every pad, Sony ones too), so this reads the
 * SDL game controllers cellPad has open. For the title's window code that reads the pads itself, such as a wrapper
 * UI's menus while the game's input is held. */
int pad_hid_sony_poll(PadHidSonyState* out, int max)
{
    int n = 0;
    SDL_LockJoysticks();
    for (int i = 0; i < PAD_MAX_HOST_PORTS && n < max; i++) {
        SDL_GameController* gc = s_sdl_controllers[i];
        if (!gc || !SDL_GameControllerGetAttached(gc)) continue;
        PadHostState hs;
        memset(&hs, 0, sizeof hs);
        pad_sdl_read(gc, &hs);
        out[n].buttons = hs.buttons;
        out[n].lx = hs.analog_lx; out[n].ly = hs.analog_ly;
        out[n].rx = hs.analog_rx; out[n].ry = hs.analog_ry;
        out[n].l2 = hs.trigger_l2; out[n].r2 = hs.trigger_r2;
        out[n].slot = (uint8_t)i;
        n++;
    }
    SDL_UnlockJoysticks();
    return n;
}
#endif

static void pad_init_backend(void)
{
    if (!s_sdl_inited) {
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        }
        s_sdl_inited = 1;
    }
    memset(s_sdl_controllers, 0, sizeof(s_sdl_controllers));

    /* Open any controllers already connected */
    int num = SDL_NumJoysticks();
    for (int i = 0; i < num && i < PAD_MAX_HOST_PORTS; i++) {
        if (SDL_IsGameController(i)) {
            s_sdl_controllers[i] = SDL_GameControllerOpen(i);
        }
    }
}

static void pad_shutdown_backend(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (s_sdl_controllers[i]) {
            SDL_GameControllerClose(s_sdl_controllers[i]);
            s_sdl_controllers[i] = NULL;
        }
    }
}

#endif /* PAD_BACKEND_SDL2 */

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

/* Re-poll the host at most every PAD_POLL_INTERVAL_MS; serve cached state in
 * between.
 *
 * XInputGetState is a driver call, and on a slot with nothing plugged in it is
 * an expensive one: measured at 97.6 us for a sweep of the 7 host ports on a
 * machine with no controller attached. cellPadGetData ran that sweep on EVERY
 * call, and a guest that polls the pad from a spin loop -- The Simpsons Arcade
 * Game does, in its input stage -- therefore spends its whole main thread
 * inside the input driver.
 *
 * 4 ms is 250 Hz, well above the 60 Hz a title can actually observe (and above
 * a real DualShock 3's own report rate), so no game can tell the difference,
 * including one sampling for button-press edges. */
#define PAD_POLL_INTERVAL_MS 4

static unsigned long long pad_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

/* PAD_MAP: gamepad button remapping, for every host port.
 * A list of game=host pairs, e.g. "cross=circle,circle=cross,l1=r2":
 * the game's cross reads the host's circle, and so on. Names: cross circle
 * square triangle l1 r1 l2 r2 l3 r3 start select up down left right.
 * Buttons not listed read themselves. L2/R2's analog values follow the
 * mapping (a digital source gives 0 or 255). Live.
 *
 * pad_parse_map fills src[i], the host button the game's button i reads (an
 * index into s_pad_names), and returns 0 when nothing is remapped. */
static int pad_parse_map(const char* e, signed char src[16])
{
    int any = 0, dst;
    const char* v;
    size_t vn;
    for (int i = 0; i < 16; i++) src[i] = (signed char)i;
    while (e && (e = pad_next_pair(e, &dst, &v, &vn, 0)) != NULL) {
        const int from = dst >= 0 && vn ? pad_name_index(v, vn) : -1;
        if (from < 0) continue;
        src[dst] = (signed char)from;
        if (from != dst) any = 1;
    }
    if (any) {
        printf("[cellPad] PAD_MAP:");
        for (int i = 0; i < 16; i++)
            if (src[i] != i) printf(" %s=%s", s_pad_names[i].name, s_pad_names[(int)src[i]].name);
        printf("\n");
        fflush(stdout);
    }
    return any;
}

static void pad_apply_map(void)
{
    static signed char src[16];
    static int on = -1;
    PS3_KNOB(on, pad_parse_map(getenv("PAD_MAP"), src));
    if (!on) return;
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++) {
        PadHostState* hs = &s_host_state[p];
        if (!hs->connected) continue;
        const u16 in = hs->buttons;
        const u8 l2 = hs->trigger_l2, r2 = hs->trigger_r2;
        u16 out = 0;
        for (int i = 0; i < 16; i++)
            if (in & s_pad_names[(int)src[i]].bit) out |= s_pad_names[i].bit;
        hs->buttons = out;
        /* index 8 = l2, 9 = r2 */
        const u8 v8 = src[8] == 8 ? l2 : src[8] == 9 ? r2 : ((out & CELL_PAD_CTRL_L2) ? 255 : 0);
        const u8 v9 = src[9] == 9 ? r2 : src[9] == 8 ? l2 : ((out & CELL_PAD_CTRL_R2) ? 255 : 0);
        hs->trigger_l2 = v8;
        hs->trigger_r2 = v9;
        pad_press_from_buttons(hs);
    }
}

/* PS3_NO_USER_INPUT=1 (ps3_knob.h): no pad or key of the user's is read. Port
 * 0 stays a pad at rest, so scripted input (PAD_FILE, PAD_STICK, PAD_SCRIPT,
 * g_pad_title_hook) still has a pad to press; the other ports are empty. */
static void pad_poll_none(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        PadHostState* hs = &s_host_state[i];
        memset(hs, 0, sizeof *hs);
        hs->connected = i == 0;
        hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
        s_poll_dev[i] = RUMBLE_NONE;
    }
    static int said = 0;
    if (!said) {
        said = 1;
        printf("[cellPad] PS3_NO_USER_INPUT: no pad or key of the user's is read; scripted input only\n");
        fflush(stdout);
    }
}

static void pad_publish_devices(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++)
        if (s_rumble_dev[i] != s_poll_dev[i]) InterlockedExchange(&s_rumble_dev[i], s_poll_dev[i]);
}

static void pad_poll_backend(void)
{
    /* Benign race: two threads may poll in the same tick. That costs one extra
     * sweep, never a missed or stale-forever read. */
    static unsigned long long s_last_ms = 0;
    unsigned long long now = pad_now_ms();
    if (s_last_ms && now - s_last_ms < PAD_POLL_INTERVAL_MS) return;
    s_last_ms = now;
    /* A real pad streams reports at roughly this rate whether or not anything
     * changed, and libpad buffers them; a fresh sweep is a fresh report for
     * every port. cellPadGetData hands each one out exactly once. */
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++) s_data_fresh[_i] = 1;

    if (ps3_no_user_input()) {   /* (nothing for the host hold to hold) */
        pad_poll_none();
        pad_publish_devices();
        return;
    }
#if PAD_BACKEND_XINPUT
    pad_poll_xinput();
    pad_poll_hid();
#elif PAD_BACKEND_SDL2
    pad_poll_sdl2();
#endif
    pad_publish_devices();
    pad_apply_map();   /* the pads only: the keyboard has its own keys */
#if PAD_BACKEND_XINPUT || PAD_BACKEND_SDL2
    pad_poll_keyboard();
#endif
    if (s_host_hold)
        for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
            PadHostState* hs = &s_host_state[i];
            const int connected = hs->connected;
            memset(hs, 0, sizeof *hs);
            hs->connected = connected;
            hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
        }
}

void cellPad_set_host_hold(int hold)
{
    s_host_hold = hold ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    /* The bound is the 127-slot pad table, not the 7 ports: larger requests
     * are accepted and clamped. Resistance: Fall of Man asks for 8; rejected,
     * the library stayed closed, every GetInfo failed and the title never
     * polled the pad -- stuck on "Press the START button". */
    if (max_connect == 0 || max_connect > 127)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    s_max_connect = max_connect > CELL_PAD_MAX_PORT_NUM ? CELL_PAD_MAX_PORT_NUM : max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    memset(s_host_state, 0, sizeof(s_host_state));

    pad_init_backend();

    /* Do an initial poll to detect connected controllers */
    pad_poll_backend();

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_shutdown_backend();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

void cellPad_poll(void)
{
    if (s_pad_initialized) {
        pad_poll_backend();
    }
}

/* HLE args are GUEST effective addresses; guest structs are BIG-ENDIAN. The
 * vm_write helpers from ppu_memory.h translate + byte-swap. (cellPad was
 * written for host pointers and was never actually invoked until its NIDs
 * were fixed.) These used to be redeclared here with a 64-bit address
 * parameter, which conflicts with the header now that it is included. */

/* PAD_TRACE=1: once a second, how many GetData / GetInfo2 calls each port got.
 * Tells "the title stopped reading the pad" apart from "it reads it and ignores
 * the press" (DeS's auto-save notice). */
static void pad_trace(int which, u32 port)
{
    static int on = -1; static unsigned long long t0 = 0;
    static unsigned n[2][8];
    PS3_KNOB(on, getenv("PAD_TRACE") ? 1 : 0);
    if (!on) return;
    if (port < 8) n[which][port]++;
    unsigned long long now = GetTickCount64();
    if (!t0) t0 = now;
    if (now - t0 >= 1000) {
        t0 = now;
        printf("[cellPad] trace GetData p0=%u p1=%u p2=%u p3=%u  GetInfo2=%u%c",
               n[0][0], n[0][1], n[0][2], n[0][3], n[1][0], 10);
        memset(n, 0, sizeof n);
    }
}

/* PAD_ALWAYS_CHANGE=1: every read is a new packet (see the end of cellPadGetData). */
static int pad_always_change(void)
{
    static int always = -1;
    PS3_KNOB(always, getenv("PAD_ALWAYS_CHANGE") ? 1 : 0);
    return always;
}

/* Stick holds for port 0 that title code asks for (cellPad_host_stick) and the mouse wheel's notches, held for a
 * number of packets the title receives rather than for a time: a title reads the pad once a frame, so a hold of two
 * packets is two of its frames at any frame rate. */
#define PAD_HOST_SLOTS 2   /* one a stick */
typedef struct { u8 stick, x, y; int left; } PadHostHold;   /* stick: 1 left, 2 right */
static PadHostHold s_hp[PAD_HOST_SLOTS];
static volatile int s_hp_lock;   /* s_hp, the wheel queue and the notch pad_host_overlay is pressing */
/* Wheel notches waiting their turn, oldest first, each with the buttons it presses (cellPad_host_mouse_wheel). */
#define PAD_WHEEL_QUEUE 4
static u16 s_wheel_q[PAD_WHEEL_QUEUE];
static int s_wheel_qn;

static void pad_hp_lock(void)   { while (__atomic_exchange_n(&s_hp_lock, 1, __ATOMIC_ACQUIRE)) { } }
static void pad_hp_unlock(void) { __atomic_store_n(&s_hp_lock, 0, __ATOMIC_RELEASE); }

static void pad_hp_add(u8 stick, u8 x, u8 y, unsigned reads)
{
    if (!reads) return;
    pad_hp_lock();
    int k = -1;
    for (int i = 0; i < PAD_HOST_SLOTS && k < 0; i++)   /* a new hold of a stick replaces the old one */
        if (s_hp[i].left > 0 && s_hp[i].stick == stick) k = i;
    for (int i = 0; i < PAD_HOST_SLOTS && k < 0; i++)
        if (s_hp[i].left <= 0) k = i;
    if (k >= 0) s_hp[k] = (PadHostHold){ stick, x, y, (int)(reads > 1000 ? 1000 : reads) };
    pad_hp_unlock();
}

void cellPad_host_stick(int stick, unsigned char x, unsigned char y, unsigned reads)
{
    if (stick == 0 || stick == 1) pad_hp_add((u8)(stick + 1), x, y, reads);
}

void cellPad_host_mouse_buttons(unsigned mask)
{
    s_mouse_buttons = mask & 0x1F;
}

/* Each notch is queued with the buttons it presses as it turns (s_wheel_bits: the modifiers and menu layer of the
 * latest keyboard poll), so it keeps Shift while it waits its turn, and up and down notches never cancel out. */
void cellPad_host_mouse_wheel(int notches)
{
    if (!notches) return;
    const u16 bits = (u16)s_wheel_bits[notches > 0 ? 0 : 1];
    if (!bits) return;   /* nothing bound to it, or the window is in the background */
    int k = notches > 0 ? notches : notches < -PAD_WHEEL_QUEUE ? PAD_WHEEL_QUEUE : -notches;
    pad_hp_lock();
    for (; k > 0 && s_wheel_qn < PAD_WHEEL_QUEUE; k--) s_wheel_q[s_wheel_qn++] = bits;
    pad_hp_unlock();
}

void cellPad_host_set_menu(int open)
{
    s_menu_layer = open ? 1 : 0;
}

/* Port 0's packet gets the stick holds and the current wheel notch. A wheel notch presses the
 * buttons it was queued with (cellPad_host_mouse_wheel) for PAD_WHEEL_READS packets, then lets go for as many, so
 * two quick notches to the same button are two presses. */
static void pad_host_overlay(CellPadData* data)
{
    static int reads = -1;
    PS3_KNOB(reads, ps3_env_int("PAD_WHEEL_READS", 2));
    static u16 w_bits;
    static int w_left, w_gap;
    pad_hp_lock();
    if (s_host_hold) {   /* a wrapper UI has the input: nothing pending reaches the title */
        for (int i = 0; i < PAD_HOST_SLOTS; i++) s_hp[i].left = 0;
        s_wheel_qn = 0;
        w_left = w_gap = 0;
        pad_hp_unlock();
        return;
    }
    const int delivers = pad_always_change() || s_data_fresh[0];   /* this read hands out a packet */
    if (!w_left && !w_gap && s_wheel_qn) {
        w_bits = s_wheel_q[0];
        s_wheel_qn--;
        memmove(s_wheel_q, s_wheel_q + 1, (size_t)s_wheel_qn * sizeof s_wheel_q[0]);
        w_left = reads > 0 ? reads : 1;
    }
    u16 bits = 0;
    if (w_left) {
        bits |= w_bits;
        if (delivers && !--w_left) w_gap = reads > 0 ? reads : 1;
    } else if (w_gap && delivers) {
        w_gap--;
    }
    for (int i = 0; i < PAD_HOST_SLOTS; i++) {
        PadHostHold* h = &s_hp[i];
        if (h->left <= 0) continue;
        if (h->stick == 1) { data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] = h->x;  data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] = h->y; }
        if (h->stick == 2) { data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = h->x; data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = h->y; }
        if (delivers) h->left--;
    }
    pad_hp_unlock();
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(bits & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((bits >> 8) & 0xFF);
}

/* Buttons title code holds down on port 0 (CELL_PAD_CTRL_* packed as in
 * PAD_SCRIPT), asked on every poll. Demon's Souls' DES_AUTO_CONTINUE presses
 * through its boot menus with it. NULL by default. */
unsigned (*g_pad_title_hook)(void) = NULL;

/* Port 0 as the guest last saw it, for a frame log in the title's port code:
 * the reads, how many came back empty (len = 0), and the last packet's buttons
 * ((DIGITAL2 << 8) | DIGITAL1) and left stick ((lx << 8) | ly). */
volatile unsigned g_pad0_reads, g_pad0_empty_reads, g_pad0_buttons, g_pad0_lstick;

s32 cellPadGetData(u32 port_no, CellPadData* data_guest)
{
    pad_trace(0, port_no);
    { static int _once = 0;
      if (!_once++) printf("[cellPad] GetData polling begins (port %u)\n", port_no); }
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadData _d; CellPadData* data = &_d;
    memset(data, 0, sizeof(CellPadData));

    /* Poll fresh state */
    pad_poll_backend();

    /* YDKJ_INJECT_PAD: the game's wait loop polls the pad (cellPadSetActDirect /
     * GetData) -- if it's parked on a "press button" prompt after loading, no
     * host input means it waits forever. Inject a real button PULSE (CROSS+START+
     * CIRCLE) so it advances. Legit input simulation, not forged pixels. */
    /* NOTE: cache getenv() -- this runs on every poll from multiple threads, and the
     * uncached version crashed deterministically (varying fault address = race). Every
     * other probe in this tree uses the cached `static int x=-1` pattern; match it. */
    static int  s_inj   = -1;
    static u16  s_btn   = 0x0008u;   /* default START */
    static long s_delay = 3000;
    if (s_inj < 0) {
        s_inj = getenv("YDKJ_INJECT_PAD") ? 1 : 0;
        const char* be = getenv("YDKJ_PAD_BTN");   if (be) s_btn   = (u16)strtoul(be,0,0);
        const char* de = getenv("YDKJ_PAD_DELAY"); if (de) s_delay = strtol(de,0,0);
    }
    /* Gate on WALL-CLOCK, not poll count: the game polls the pad hundreds of thousands
     * of times inside an early wait loop, so any count-based delay fires during init
     * (crash: guest ctr=0x0005F8A0 r3=0 -- button handler on an unconstructed object).
     * YDKJ_PAD_DELAY is now SECONDS to wait before the first injected press. */
    if (s_inj && port_no < PAD_MAX_HOST_PORTS) {
        static long _pc = 0; _pc++;
        /* Cheap counter gate FIRST: clock() is a slow call and this poll loop is hammered
         * by the guest from multiple threads; sampling it every poll perturbs timing. Only
         * consult the clock once every 256 polls, and never after arming. */
        static int     s_armed = 0;
        static clock_t s_t0    = 0;
        if (!s_armed) {
            if ((_pc & 0xFF) == 0) {
                if (!s_t0) s_t0 = clock();
                else if ((double)(clock() - s_t0) / (double)CLOCKS_PER_SEC >= (double)s_delay) s_armed = 1;
            }
            goto skip_inject;
        }
        u16 btn = s_btn; long delay = 0;
        /* Delay past init, then a single clean pulse every ~180 calls (press 15, release 165). */
        if (_pc > delay) {
            long ph = (_pc - delay) % 180;
            if (ph < 15) s_host_state[port_no].buttons |= btn;
            else         s_host_state[port_no].buttons &= ~btn;
            s_host_state[port_no].connected = 1;
        }
    }
skip_inject: ;

    if (port_no >= PAD_MAX_HOST_PORTS || (!s_host_state[port_no].connected && port_no != 0)) {
        goto emit;   /* data stays zeroed -> len=0 */
    }
    /* port 0 is always a valid (virtual) pad; s_host_state[0] is neutral if no
     * physical device, which yields no-buttons-pressed data below. */

    {
    PadHostState* hs = &s_host_state[port_no];
    u32 setting = s_port_setting[port_no];

    /* Determine data length based on port settings */
    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    /* button[0] is reserved and 0 on hardware; button[1] carries the pad mode
     * in its high nibble (0x7) and len/2 in its low one. GH3 drops any packet
     * whose button[0] is non-zero -- with len (8) there, every press was
     * received and thrown away, and "Press any button" never advanced. */
    data->button[0] = 0;
    data->button[1] = (u16)(0x70 | ((len / 2) & 0xF));

    /* Digital buttons. hs->buttons packs both halves into one 16-bit mask
     * (SELECT=bit0..LEFT=bit7 = DIGITAL1; L2=bit8..SQUARE=bit15 = DIGITAL2).
     * Split correctly — writing the whole value into DIGITAL1 with DIGITAL2=0
     * left every face button (cross/circle/triangle/square, L1/L2/R1/R2) dead.
     * (via sagemono, PR #42) */
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(hs->buttons & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((hs->buttons >> 8) & 0xFF);

    /* PAD_AUTOPRESS: headless bring-up input -- pulse CROSS then START every few
     * seconds of polling so boot screens that wait for input advance without a
     * human at the pad. Env-gated test scaffolding, off by default. */
    { static int s_ap = -1; static unsigned s_apn = 0;
      PS3_KNOB(s_ap, getenv("PAD_AUTOPRESS") ? 1 : 0);
      if (s_ap && port_no == 0) {
          unsigned ph = s_apn++ % 240;
          if (ph < 12)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;   /* CROSS */
          else if (ph >= 120 && ph < 132)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= 0x08;   /* START */
      } }

    /* PAD_SCRIPT="<sec>:<mask>,<sec>:<mask>,..." -- press a NAMED button at a
     * given wall-clock second, each held ~250 ms. PAD_AUTOPRESS only pulses
     * CROSS and START, which is enough to clear a "press start" screen but not
     * to navigate a menu: You Don't Know Jack needs DOWN to move off its name
     * field before CROSS means anything. Masks are CELL_PAD_CTRL_* packed as
     * (DIGITAL2 << 8) | DIGITAL1: START 0x0008, UP 0x0010, DOWN 0x0040,
     * LEFT 0x0080, RIGHT 0x0020, CROSS 0x4000, CIRCLE 0x2000, TRIANGLE 0x1000.
     * Legit input simulation on the same footing as PAD_AUTOPRESS/PAD_STICK. */
    { static int s_sc = -1;
      static struct { double t; unsigned mask; } ev[32]; static int n_ev = 0;
      static unsigned long long t0 = 0;   /* not ULONGLONG: this builds on POSIX too */
      if (s_sc < 0) {
          s_sc = 0;
          const char* e = getenv("PAD_SCRIPT");
          if (e && *e) {
              s_sc = 1; t0 = GetTickCount64();
              const char* p = e;
              while (*p && n_ev < 32) {
                  double t = strtod(p, (char**)&p);
                  if (*p == ':') p++;
                  unsigned m = (unsigned)strtoul(p, (char**)&p, 0);
                  ev[n_ev].t = t; ev[n_ev].mask = m; n_ev++;
                  while (*p == ',' || *p == ' ') p++;
              }
              printf("[cellPad] PAD_SCRIPT: %d event(s)\n", n_ev);
          }
      }
      if (s_sc && port_no == 0) {
          double now = (double)(GetTickCount64() - t0) / 1000.0;
          /* Latch, do not window. A fixed time window is missed whenever the
           * title stops polling the pad for longer than the window (it does
           * exactly that on a loading screen), so the press silently never
           * happens. Fire on the first poll at or after the event time and
           * hold it for a fixed number of polls, which is what a real press
           * looks like regardless of poll rate. */
          static int fired[32], held[32];
          for (int i = 0; i < n_ev; i++) {
              if (!fired[i] && now >= ev[i].t) { fired[i] = 1; held[i] = 40; }
              if (held[i] > 0) {
                  held[i]--;
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(ev[i].mask & 0xFF);
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((ev[i].mask >> 8) & 0xFF);
                  static int said[32];
                  if (!said[i]) { said[i] = 1;
                      printf("[cellPad] PAD_SCRIPT t=%.1fs press 0x%04X\n", ev[i].t, ev[i].mask); }
              }
          }
      } }

    if (g_pad_title_hook && port_no == 0) {
        const unsigned m = g_pad_title_hook();
        data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(m & 0xFF);
        data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((m >> 8) & 0xFF);
    }

    /* PAD_FILE=<path> -- closed-loop input. Each poll, if the file holds a
     * button mask (same encoding as PAD_SCRIPT), press it for a fixed number
     * of polls and empty the file. A driver outside the process can then look
     * at the log or a frame dump, decide which screen the title is on, and
     * write the next button -- which a fixed time schedule cannot do, because
     * a menu system with several screens wanders when driven blind.
     *
     *   echo 0x0008 > pad.txt    # START
     *   echo 0x4000 > pad.txt    # CROSS
     *   echo 0x0040 4 > pad.txt  # DOWN for 4 polls (menus auto-repeat a
     *                            # long hold; the default is 40)
     */
    { static int s_pf = -1; static char pf_path[512];
      static unsigned held_mask = 0; static int held_n = 0;
      if (s_pf < 0) { const char* e = getenv("PAD_FILE");
                      s_pf = (e && *e) ? 1 : 0;
                      if (s_pf) { strncpy(pf_path, e, sizeof pf_path - 1); pf_path[sizeof pf_path - 1] = 0;
                                  printf("[cellPad] PAD_FILE=%s\n", pf_path); fflush(stdout); } }
      if (s_pf && port_no == 0) {
          /* Heartbeat every 2000 polls: separates "not polled", "file not
           * opened" and "opened but empty" when a press seems to be ignored. */
          static unsigned hb_poll, hb_open, hb_text;
          if (++hb_poll % 2000 == 0) {
              printf("[cellPad] PAD_FILE polls=%u opened=%u text=%u held=%d\n",
                     hb_poll, hb_open, hb_text, held_n); fflush(stdout); }
          if (held_n <= 0) {
              FILE* f = fopen(pf_path, "rb");
              if (f) {
                  char buf[64]; buf[0] = 0; hb_open++;
                  if (fgets(buf, sizeof buf, f)) { hb_text++;
                      char* rest;
                      unsigned m = (unsigned)strtoul(buf, &rest, 0);
                      int n = (int)strtol(rest, 0, 0);
                      if (m) { held_mask = m; held_n = n > 0 ? n : 40;
                               printf("[cellPad] PAD_FILE press 0x%04X\n", m); fflush(stdout); }
                  }
                  fclose(f);
                  if (held_n > 0) { FILE* w = fopen(pf_path, "wb"); if (w) fclose(w); }
              }
          }
          if (held_n > 0) { held_n--;
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(held_mask & 0xFF);
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((held_mask >> 8) & 0xFF); }
      } }

    /* Analog sticks */
    /* PAD_STICK="lx,ly,rx,ry" (0-255, 128 = centred): hold the analog sticks at
     * fixed positions. Rubber Ducky drives its camera from the sticks and never
     * moves on its own, so with no host pad the view is frozen wherever it
     * started -- which can leave the subject of the demo off-screen or too far
     * away to make out. Legit input simulation, same footing as YDKJ_INJECT_PAD. */
    { static int s_st = -1;
      static unsigned char s_v[4] = {128,128,128,128};
      /* Optional 5th/6th fields: the seconds the hold starts and ends, so the menus
       * before gameplay are not driven by it (PAD_STICK="128,0,128,128,300,320"). */
      static double s_from = 0, s_until = 1e30; static unsigned long long s_t0 = 0;
      /* Live: the console's setenv PAD_STICK=... re-reads it, and the window
       * then counts from that moment (setenv PAD_STICK=128,0,128,128,0,5 walks
       * forward for five seconds). At startup it counts from the first poll.
       * Only a new value restarts the window, not a setenv of another knob. */
      static unsigned s_stg = 0; static char s_last[64] = "\1";   /* never a real value */
      if (s_stg != ps3_env_gen) { s_stg = ps3_env_gen;
        const char* e = getenv("PAD_STICK");
        if (strncmp(s_last, e ? e : "", sizeof s_last - 1)) {
          snprintf(s_last, sizeof s_last, "%s", e ? e : "");
          s_st = e ? 1 : 0; s_t0 = GetTickCount64(); s_from = 0; s_until = 1e30;
          if (e) { int a=128,b=128,c=128,d=128;
                   sscanf(e, "%d,%d,%d,%d,%lf,%lf", &a,&b,&c,&d,&s_from,&s_until);
                   s_v[0]=(unsigned char)a; s_v[1]=(unsigned char)b;
                   s_v[2]=(unsigned char)c; s_v[3]=(unsigned char)d; } } }
      if (s_st && (double)(GetTickCount64() - s_t0) / 1000.0 >= s_from && (double)(GetTickCount64() - s_t0) / 1000.0 < s_until) { hs->analog_lx = s_v[0]; hs->analog_ly = s_v[1];
                  hs->analog_rx = s_v[2]; hs->analog_ry = s_v[3];
                  hs->connected = 1; }
      /* PAD_SWEEP=<seconds per step>: walk the sticks through a fixed set of
       * deflections instead of holding one. Guessing a single stick position and
       * re-running costs ~5 minutes a try; a sweep answers "can input move the
       * camera anywhere useful" in one run, and the frame dumps say which step
       * paid off. Steps: centre, each left-stick direction, then each right. */
      { static int sw = -1; static clock_t t0 = 0;
        if (sw < 0) { const char* e = getenv("PAD_SWEEP");
                      sw = e ? atoi(e) : 0; if (sw < 0) sw = 0; }
        if (sw) {
            static const unsigned char steps[9][4] = {
                {128,128,128,128},
                {128,  0,128,128}, {128,255,128,128},
                {  0,128,128,128}, {255,128,128,128},
                {128,128,128,  0}, {128,128,128,255},
                {128,128,  0,128}, {128,128,255,128},
            };
            if (!t0) t0 = clock();
            long el = (long)((double)(clock() - t0) / (double)CLOCKS_PER_SEC);
            int k = (int)((el / (sw > 0 ? sw : 1)) % 9);
            hs->analog_lx = steps[k][0]; hs->analog_ly = steps[k][1];
            hs->analog_rx = steps[k][2]; hs->analog_ry = steps[k][3];
            hs->connected = 1;
            { static int lastk = -1;
              if (k != lastk) { lastk = k;
                fprintf(stderr, "[PADSWEEP] step %d: l=(%u,%u) r=(%u,%u)%c", k,
                        steps[k][0], steps[k][1], steps[k][2], steps[k][3], 10); } }
        } } }
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = hs->analog_rx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = hs->analog_ry;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = hs->analog_lx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = hs->analog_ly;
    if (port_no == 0) pad_host_overlay(data);

    /* Pressure-sensitive buttons (only meaningful if PRESS_ON) */
    if (setting & CELL_PAD_SETTING_PRESS_ON) {
        data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT]    = hs->press_right;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT]     = hs->press_left;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_UP]       = hs->press_up;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN]     = hs->press_down;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = hs->press_triangle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]   = hs->press_circle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]    = hs->press_cross;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]   = hs->press_square;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L1]       = hs->press_l1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R1]       = hs->press_r1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L2]       = hs->trigger_l2;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R2]       = hs->trigger_r2;
        /* A button whose digital bit is set reads full pressure. Injected
         * input (PAD_FILE/PAD_SCRIPT/PAD_AUTOPRESS, keyboard) only sets the
         * digital bits, and the host values above zeroed any pressure it had
         * written -- so a title keying its menus off the pressure words (GH3,
         * Tornado Outbreak's title screen) never saw the press. */
        static const struct { u8 word; u8 bit; u8 off; } pm[] = {
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x20, CELL_PAD_BTN_OFFSET_PRESS_RIGHT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x80, CELL_PAD_BTN_OFFSET_PRESS_LEFT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x10, CELL_PAD_BTN_OFFSET_PRESS_UP },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x40, CELL_PAD_BTN_OFFSET_PRESS_DOWN },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x10, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x20, CELL_PAD_BTN_OFFSET_PRESS_CIRCLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x40, CELL_PAD_BTN_OFFSET_PRESS_CROSS },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x80, CELL_PAD_BTN_OFFSET_PRESS_SQUARE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x04, CELL_PAD_BTN_OFFSET_PRESS_L1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x08, CELL_PAD_BTN_OFFSET_PRESS_R1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x01, CELL_PAD_BTN_OFFSET_PRESS_L2 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x02, CELL_PAD_BTN_OFFSET_PRESS_R2 } };
        for (unsigned k = 0; k < sizeof pm / sizeof pm[0]; k++)
            if ((data->button[pm[k].word] & pm[k].bit) && data->button[pm[k].off] < 255)
                data->button[pm[k].off] = 255;
    }

    /* Sensor data (only meaningful if SENSOR_ON) */
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        /* Default sensor values: accelerometer at rest (512 = 1g center) */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 399; /* gravity */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 512;
    }
    }  /* end connected block */

emit:
    /* A non-zero len means "here is a CHANGE packet" -- the constant is literally
     * named CELL_PAD_LEN_CHANGE_DEFAULT. Hardware reports len = 0 once the port
     * has nothing new, and titles DRAIN on that: The Simpsons Arcade Game's
     * input stage is
     *
     *     while (cellPadGetData(port, &d) == CELL_OK && d.len > 0) { ... }
     *
     * (func_0013CFA8, loop 0x13D648 -> 0x13D040). Reporting a change on every
     * call made that loop infinite: the main thread pegged one core inside
     * libpad and the title never got past its first rendered screen.
     *
     * So hand out one packet per host report and report len = 0 for any read
     * after that until the next one arrives (pad_poll_backend sets the flag when
     * it actually re-polls, every PAD_POLL_INTERVAL_MS). That is what the
     * hardware does: a pad streams reports at a fixed rate whether or not
     * anything changed, and libpad buffers them.
     *
     * Reporting only on CHANGE instead is wrong and was tried: a HELD button
     * produces exactly one packet, so a title that reads button[] only when
     * len > 0 sees the press for a single frame and then sees nothing. In this
     * game that came out as jump being the only control that seemed to work and
     * attack firing once.
     *
     * PAD_ALWAYS_CHANGE=1 restores the old always-a-packet behaviour. */
    {
        if (!pad_always_change() && port_no < PAD_MAX_HOST_PORTS && data->len) {
            if (!s_data_fresh[port_no]) data->len = 0;   /* nothing new: end the drain */
            else                        s_data_fresh[port_no] = 0;
        }
    }
    if (port_no == 0) {
        g_pad0_reads++;
        if (!data->len) g_pad0_empty_reads++;
        else {
            g_pad0_buttons = (unsigned)(data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] & 0xFF) << 8 |
                             (data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] & 0xFF);
            g_pad0_lstick  = (unsigned)(data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] & 0xFF) << 8 |
                             (data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] & 0xFF);
        }
    }
    {
        unsigned int ea = (unsigned int)(uintptr_t)data_guest;
        vm_write32((unsigned long long)ea + 0, (unsigned int)data->len);   /* len (s32) */
        for (int _i = 0; _i < CELL_PAD_MAX_CODES; _i++)                    /* button[] u16 */
            vm_write16((unsigned long long)ea + 4 + (unsigned int)_i * 2, data->button[_i]);
    }
    return CELL_OK;
}

/* cellPadGetInfo (v1 API, NID 0x3AAAD464) — used by PSL1GHT's ioPadGetInfo.
 * Layout (RPCS3 cellPad.h CellPadInfo, all big-endian):
 *   +0x00 u32 max_connect        +0x04 u32 now_connect   +0x08 u32 system_info
 *   +0x0C u16 vendor_id[127]     +0x10A u16 product_id[127] +0x208 u8 status[127]
 *   (CELL_MAX_PADS / PSL1GHT MAX_PADS = 127 -- NOT 7; see below)
 * Leaving this unimplemented let the guest read stack garbage as pad state and
 * run off into the weeds before ever reaching gcm init. */
s32 cellPadGetInfo(CellPadInfo2* info)
{
    uint32_t gaddr = (uint32_t)(uintptr_t)info;
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!gaddr)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    /* Present a virtual pad on port 0 when nothing physical is attached, the
     * same as cellPadGetInfo2 does.
     *
     * The two entry points disagreed: GetInfo2 carried the fallback and GetInfo
     * did not, so which one a title imports decided whether it believed a
     * controller existed. The Orange Box's chooser imports GetInfo (v1) and
     * never GetInfo2, so it saw zero pads, opened "SIXAXIS wireless controller
     * not detected", took the auto-OK, and opened it again -- forever, before
     * it ever finished booting. Both entry points describe the same hardware
     * and have to answer the same way. */
    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected || i == 0) connected++;

    vm_write32(gaddr + 0x00, s_max_connect);
    vm_write32(gaddr + 0x04, connected);
    vm_write32(gaddr + 0x08, 0);                       /* system_info */
    /* CellPadInfo (v1) sizes its arrays by CELL_MAX_PADS = 127, not by the 7
     * ports: vendor_id[127] @0x0C, product_id[127] @0x10A, status[127] @0x208.
     * Packed for 7 ports, status landed at 0x28 and GH3 -- which reads
     * info+0x208+port -- saw port 0 disconnected and never polled the pad. */
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; i++) {
        int on = (i < s_max_connect && i < PAD_MAX_HOST_PORTS &&
                  (s_host_state[i].connected || i == 0));
        vm_write16(gaddr + 0x00C + i * 2, on ? 0x054C : 0);   /* vendor: Sony  */
        vm_write16(gaddr + 0x10A + i * 2, on ? 0x0268 : 0);   /* product: DS3  */
        vm_write8(gaddr + 0x208 + i, on ? CELL_PAD_STATUS_CONNECTED : 0);
    }
    return CELL_OK;
}

s32 cellPadGetInfo2(CellPadInfo2* info_guest)
{
    pad_trace(1, 0);
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_backend();

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadInfo2 _in; CellPadInfo2* info = &_in;
    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
        /* Always present a virtual pad on port 0 (standard emulator behavior) so
         * boot flows that block until a controller is connected can proceed even
         * with no physical device attached. */
        if (s_host_state[i].connected || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    /* copy to guest big-endian (CellPadInfo2 is all u32 fields) */
    {
        unsigned int ea = (unsigned int)(uintptr_t)info_guest;
        for (unsigned int _o = 0; _o < sizeof(CellPadInfo2); _o += 4)
            vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o));
    }
    return CELL_OK;
}

/* cellPadPeriphGetInfo -- the peripheral-class view of the pad ports (NID
 * 0x4CC9B68D). LBP polls this during boot; leaving it unresolved returned
 * CELL_OK with the caller's CellPadPeriphInfo left as uninitialised stack
 * garbage, so the game read junk port_status/pclass_type for all 7 ports.
 *
 * Layout (SDK cell/pad/pad_codes.h) -- all u32, so the whole struct marshals
 * big-endian in one sweep like cellPadGetInfo2:
 *   max_connect, now_connect, system_info,
 *   port_status[7], port_setting[7], device_capability[7],
 *   device_type[7], pclass_type[7], pclass_profile[7]
 * We report a standard DUALSHOCK-class pad (pclass_type STANDARD = 0, no
 * profile bits) mirroring the ports cellPadGetInfo2 already reports. */
s32 cellPadPeriphGetInfo(CellPadPeriphInfo* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    CellPadPeriphInfo _in; CellPadPeriphInfo* info = &_in;
    memset(info, 0, sizeof(CellPadPeriphInfo));
    info->max_connect = s_max_connect;
    info->system_info = 0;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < CELL_PAD_MAX_PORT_NUM; i++) {
        /* Same virtual-pad-on-port-0 rule as cellPadGetInfo2, so both views of
         * the ports agree (a game that cross-checks them must see one truth). */
        if ((i < PAD_MAX_HOST_PORTS && s_host_state[i].connected) || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            info->pclass_type[i]       = CELL_PAD_PCLASS_TYPE_STANDARD;
            info->pclass_profile[i]    = 0;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    { unsigned int ea = (unsigned int)(uintptr_t)info_guest;
      for (unsigned int _o = 0; _o < sizeof(CellPadPeriphInfo); _o += 4)
          vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o)); }

    { static int _n = 0;
      if (_n < 2 && ++_n)
          printf("[cellPad] PeriphGetInfo(max=%u now=%u) -> STANDARD class\n",
                 info->max_connect, connected); }
    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

/* cellPadInfoPressMode (NID 0x0E2DFAAD) / cellPadSetPressMode (NID 0xF83F8182)
 *
 * Press mode makes the digital buttons report an analog 0..255 pressure in the
 * extended part of CellPadData. A DS3 supports it, and the virtual pad reported
 * by cellPadGetInfo/GetInfo2 already advertises CELL_PAD_CAPABILITY_PRESS_MODE,
 * so these have to agree with that claim rather than fall through to the
 * unresolved-NID path.
 *
 * Info returns 1 when the port supports press mode; Set toggles it, recorded in
 * the same s_port_setting word cellPadSetPortSetting uses so a title that reads
 * the setting back sees what it asked for. */
s32 cellPadInfoPressMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return 1;                                  /* supported */
}

s32 cellPadSetPressMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    if (mode)
        s_port_setting[port_no] |= CELL_PAD_SETTING_PRESS_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_PRESS_ON;
    return CELL_OK;
}

s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    u32 info_ea = (u32)(uintptr_t)info;
    for (u32 i = 0; i < CELL_PAD_MAX_CODES; i++)
        vm_write32(info_ea + i * 4, 0);

    /* Report standard DualShock 3 capabilities */
    vm_write32(info_ea, CELL_PAD_CAPABILITY_PS3_CONFORMITY
                        | CELL_PAD_CAPABILITY_PRESS_MODE
                        | CELL_PAD_CAPABILITY_SENSOR_MODE
                        | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                        | CELL_PAD_CAPABILITY_ACTUATOR);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Vibration (cellPadSetActDirect)
 *
 * A game sets a pad's two motors: motor[0] the small one, on (anything but 0)
 * or off, and motor[1] the large one, 0-255. A DualShock 3's large motor does
 * not turn at 63 or less, so a game tuned on one sends low values for still:
 * the large value goes through that threshold and is spread over the rest of
 * the range, and the small motor runs at full speed while it is on (as RPCS3
 * maps them). Demon's Souls sends the small motor 1 for on, which as 1/255 of
 * full speed was never felt.
 *
 * A thread of its own sends the motors to the pads, so a slow write (a Sony
 * pad's Bluetooth report) never holds a game thread: on a change, at most
 * every 20 ms; again every 300 ms while they run (an SDL rumble has a length,
 * and a pad that lost a report gets the next); off when the game has not set
 * them for 3 s (a game that stopped calling), when cellPad ends and at exit.
 * Port i goes to XInput user i, else the Sony HID pad on that port
 * (pad_poll_hid); with SDL, to controller i.
 *
 * PAD_RUMBLE=0 (live) keeps every pad still. PS3_NO_USER_INPUT=1 opens no pad
 * of the user's, so nothing is sent; PAD_RUMBLE_TRACE=1 logs each change the
 * game asks for either way.
 * -----------------------------------------------------------------------*/
static volatile long s_rumble_want[PAD_MAX_HOST_PORTS];   /* large << 8 | small, as sent */
static volatile unsigned long long s_rumble_at[PAD_MAX_HOST_PORTS];   /* the game's last set (pad_now_ms) */
static long s_rumble_sent[PAD_MAX_HOST_PORTS], s_rumble_sent_dev[PAD_MAX_HOST_PORTS];   /* the thread's */
static HANDLE s_rumble_wake;
static volatile long s_rumble_started, s_rumble_exit;

static unsigned rumble_large(unsigned v)
{
    return v <= 63 ? 0 : (v - 63) * 255 / 192;
}

static int rumble_on(void)
{
    static int on = 1;
    PS3_KNOB(on, ps3_env_int("PAD_RUMBLE", 1));
    return on != 0;
}

static void rumble_send(long dev, int port, long want)
{
    const unsigned strong = (unsigned)(want >> 8) & 0xFFu, weak = (unsigned)want & 0xFFu;
#if PAD_BACKEND_XINPUT
    if (dev == RUMBLE_XINPUT) {
        XINPUT_VIBRATION v;
        v.wLeftMotorSpeed = (WORD)(strong * 257);   /* the low-frequency, strong motor */
        v.wRightMotorSpeed = (WORD)(weak * 257);
        XInputSetState((DWORD)port, &v);
    } else if (dev & RUMBLE_HID) {
        pad_hid_sony_rumble((int)(dev & 0xF), (uint8_t)strong, (uint8_t)weak);
    }
#elif PAD_BACKEND_SDL2
    if (dev == RUMBLE_SDL) {
        SDL_LockJoysticks();   /* (the poll closes a controller under it) */
        if (s_sdl_controllers[port])
            SDL_GameControllerRumble(s_sdl_controllers[port], (Uint16)(strong * 257), (Uint16)(weak * 257), 500);
        SDL_UnlockJoysticks();
    }
#else
    (void)dev; (void)port; (void)strong; (void)weak;
#endif
}

static DWORD WINAPI rumble_thread(LPVOID arg)
{
    (void)arg;
    unsigned long long sent_at[PAD_MAX_HOST_PORTS] = {0};
    for (;;) {
        const unsigned long long now = pad_now_ms();
        const int on = rumble_on() && s_pad_initialized && !s_rumble_exit;
        int busy = 0;
        for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
            const long dev = s_rumble_dev[i];
            long want = s_rumble_want[i];
            if (!on || now - s_rumble_at[i] > 3000) want = 0;
            if (dev != s_rumble_sent_dev[i]) {   /* another pad on the port, or none */
                if (s_rumble_sent[i] && s_rumble_sent_dev[i]) rumble_send(s_rumble_sent_dev[i], i, 0);
                s_rumble_sent[i] = 0;
                s_rumble_sent_dev[i] = dev;
                sent_at[i] = 0;
            }
            if (!dev) continue;
            const unsigned long long since = now - sent_at[i];
            if (want != s_rumble_sent[i] ? since >= 20 : want && since >= 300) {
                rumble_send(dev, i, want);
                s_rumble_sent[i] = want;
                sent_at[i] = now;
            }
            /* (the game's ask counts while it is fresh, so PAD_RUMBLE=1 brings it back at once) */
            if (s_rumble_sent[i] || (s_rumble_want[i] && now - s_rumble_at[i] <= 3000)) busy = 1;
        }
        WaitForSingleObject(s_rumble_wake, busy ? 10 : 1000);
    }
    return 0;
}

/* At exit: the motors off (a pad keeps the last report it got). */
static void rumble_atexit(void)
{
    InterlockedExchange(&s_rumble_exit, 1);
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++)
        if (s_rumble_sent[i] && s_rumble_sent_dev[i]) rumble_send(s_rumble_sent_dev[i], i, 0);
}

static void rumble_set(u32 port, unsigned weak, unsigned strong)
{
    const long want = (long)(rumble_large(strong) << 8 | (weak ? 255u : 0u));
    s_rumble_at[port] = pad_now_ms();
    static int trace = -1, calls = 0;
    PS3_KNOB(trace, ps3_env_int("PAD_RUMBLE_TRACE", 0));
    if (trace && !calls++) {   /* (the first call, the motors still or not) */
        printf("[cellPad] vibration: the game's first set, port %u: small %u large %u\n", port, weak, strong);
        fflush(stdout);
    }
    if (want == s_rumble_want[port]) return;
    if (trace) {
        printf("[cellPad] vibration, port %u: small %u large %u -> small %ld large %ld%s\n", port, weak, strong,
               want & 0xFF, want >> 8, !rumble_on() ? " (PAD_RUMBLE=0)" : ps3_no_user_input() ? " (no pads open)" : "");
        fflush(stdout);
    }
    InterlockedExchange(&s_rumble_want[port], want);
    if (InterlockedCompareExchange(&s_rumble_started, 1, 0) == 0) {
        s_rumble_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        atexit(rumble_atexit);
        HANDLE t = CreateThread(NULL, 0, rumble_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    if (s_rumble_wake) SetEvent(s_rumble_wake);
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    param = GUEST_PTR(param, CellPadActParam*);
    if (port_no < PAD_MAX_HOST_PORTS)
        rumble_set(port_no, param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL], param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE]);
    return CELL_OK;
}

s32 cellPadClearBuf(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Nothing to clear in our implementation -- state is polled fresh */
    return CELL_OK;
}
