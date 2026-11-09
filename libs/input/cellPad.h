/*
 * ps3recomp - cellPad HLE
 *
 * Gamepad input: initialization, reading button/analog state.
 * Backend: XInput on Windows, SDL2 GameController elsewhere.
 */

#ifndef PS3RECOMP_CELL_PAD_H
#define PS3RECOMP_CELL_PAD_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define CELL_PAD_MAX_PORT_NUM       7
#define CELL_PAD_MAX_CODES          64
#define CELL_MAX_PADS               CELL_PAD_MAX_PORT_NUM

/* Button bitmask constants (digital buttons, in button[2] low word) */
#define CELL_PAD_CTRL_SELECT        (1 << 0)
#define CELL_PAD_CTRL_L3            (1 << 1)
#define CELL_PAD_CTRL_R3            (1 << 2)
#define CELL_PAD_CTRL_START         (1 << 3)
#define CELL_PAD_CTRL_UP            (1 << 4)
#define CELL_PAD_CTRL_RIGHT         (1 << 5)
#define CELL_PAD_CTRL_DOWN          (1 << 6)
#define CELL_PAD_CTRL_LEFT          (1 << 7)
#define CELL_PAD_CTRL_L2            (1 << 8)
#define CELL_PAD_CTRL_R2            (1 << 9)
#define CELL_PAD_CTRL_L1            (1 << 10)
#define CELL_PAD_CTRL_R1            (1 << 11)
#define CELL_PAD_CTRL_TRIANGLE      (1 << 12)
#define CELL_PAD_CTRL_CIRCLE        (1 << 13)
#define CELL_PAD_CTRL_CROSS         (1 << 14)
#define CELL_PAD_CTRL_SQUARE        (1 << 15)

/* PS button (in button[3] high byte) */
#define CELL_PAD_CTRL_PS            (1 << 0)

/* Port setting flags */
#define CELL_PAD_SETTING_PRESS_ON       (1 << 1)
#define CELL_PAD_SETTING_SENSOR_ON      (1 << 2)
#define CELL_PAD_SETTING_PRESS_OFF      0
#define CELL_PAD_SETTING_SENSOR_OFF     0

/* Port status flags */
#define CELL_PAD_STATUS_DISCONNECTED    0
#define CELL_PAD_STATUS_CONNECTED       1
#define CELL_PAD_STATUS_ASSIGN_CHANGES  2

/* Device type */
#define CELL_PAD_DEV_TYPE_STANDARD      0
#define CELL_PAD_DEV_TYPE_BD_REMOCON    4
#define CELL_PAD_DEV_TYPE_LDD           5

/* Device capability flags */
#define CELL_PAD_CAPABILITY_PS3_CONFORMITY  (1 << 0)
#define CELL_PAD_CAPABILITY_PRESS_MODE      (1 << 1)
#define CELL_PAD_CAPABILITY_SENSOR_MODE     (1 << 2)
#define CELL_PAD_CAPABILITY_HP_ANALOG_STICK (1 << 3)
#define CELL_PAD_CAPABILITY_ACTUATOR        (1 << 4)

/* Standard data lengths */
#define CELL_PAD_LEN_NO_CHANGE          0
#define CELL_PAD_LEN_CHANGE_DEFAULT     8
#define CELL_PAD_LEN_CHANGE_PRESS_ON    24
#define CELL_PAD_LEN_CHANGE_SENSOR_ON   28

/* Pressure-sensitive button offsets in button[] array */
#define CELL_PAD_BTN_OFFSET_DIGITAL1    2
#define CELL_PAD_BTN_OFFSET_DIGITAL2    3
#define CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X  4
#define CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y  5
#define CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X   6
#define CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y   7
#define CELL_PAD_BTN_OFFSET_PRESS_RIGHT     8
#define CELL_PAD_BTN_OFFSET_PRESS_LEFT      9
#define CELL_PAD_BTN_OFFSET_PRESS_UP        10
#define CELL_PAD_BTN_OFFSET_PRESS_DOWN      11
#define CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE  12
#define CELL_PAD_BTN_OFFSET_PRESS_CIRCLE    13
#define CELL_PAD_BTN_OFFSET_PRESS_CROSS     14
#define CELL_PAD_BTN_OFFSET_PRESS_SQUARE    15
#define CELL_PAD_BTN_OFFSET_PRESS_L1        16
#define CELL_PAD_BTN_OFFSET_PRESS_R1        17
#define CELL_PAD_BTN_OFFSET_PRESS_L2        18
#define CELL_PAD_BTN_OFFSET_PRESS_R2        19
#define CELL_PAD_BTN_OFFSET_SENSOR_X        20
#define CELL_PAD_BTN_OFFSET_SENSOR_Y        21
#define CELL_PAD_BTN_OFFSET_SENSOR_Z        22
#define CELL_PAD_BTN_OFFSET_SENSOR_G        23

/* Actuator (rumble) parameters */
#define CELL_PAD_ACTUATOR_MAX           2
#define CELL_PAD_ACTUATOR_PARAM_SMALL   0
#define CELL_PAD_ACTUATOR_PARAM_LARGE   1

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/*
 * CellPadData -- returned by cellPadGetData.
 *
 * Layout of the button[] array (each element is u16):
 *   [0] = data length (number of valid entries following)
 *   [1] = reserved
 *   [2] = digital buttons bitmask (CELL_PAD_CTRL_*)
 *   [3] = digital buttons bitmask continued (PS button, etc.)
 *   [4] = right analog stick X (0-255, center=128)
 *   [5] = right analog stick Y (0-255, center=128)
 *   [6] = left  analog stick X (0-255, center=128)
 *   [7] = left  analog stick Y (0-255, center=128)
 *   [8..19] = pressure-sensitive button values (0-255 each)
 *   [20..23] = sensor data (accel X, Y, Z, gyro)
 */
typedef struct CellPadData {
    s32 len;                          /* number of valid button entries */
    u16 button[CELL_PAD_MAX_CODES];   /* button/analog/sensor data */
} CellPadData;

/* Per-port info returned by cellPadGetInfo2 */
typedef struct CellPadInfo2 {
    u32 max_connect;
    u32 now_connect;
    u32 system_info;
    u32 port_status[CELL_PAD_MAX_PORT_NUM];
    u32 port_setting[CELL_PAD_MAX_PORT_NUM];
    u32 device_capability[CELL_PAD_MAX_PORT_NUM];
    u32 device_type[CELL_PAD_MAX_PORT_NUM];
} CellPadInfo2;

/* Peripheral-class view of the ports (cellPadPeriphGetInfo). Extends the
 * CellPadInfo2 field set with the peripheral class (guitar/drums/DJ/...);
 * a normal DUALSHOCK reports pclass_type STANDARD with no profile bits.
 * Layout must match SDK cell/pad/pad_codes.h -- all u32. */
typedef struct CellPadPeriphInfo {
    u32 max_connect;
    u32 now_connect;
    u32 system_info;
    u32 port_status[CELL_PAD_MAX_PORT_NUM];
    u32 port_setting[CELL_PAD_MAX_PORT_NUM];
    u32 device_capability[CELL_PAD_MAX_PORT_NUM];
    u32 device_type[CELL_PAD_MAX_PORT_NUM];
    u32 pclass_type[CELL_PAD_MAX_PORT_NUM];
    u32 pclass_profile[CELL_PAD_MAX_PORT_NUM];
} CellPadPeriphInfo;

/* Peripheral class types (SDK pad_codes.h) */
#define CELL_PAD_PCLASS_TYPE_STANDARD   0

/* Capability info for a specific port */
typedef struct CellPadCapabilityInfo {
    u32 info[CELL_PAD_MAX_CODES];
} CellPadCapabilityInfo;

/* Actuator (vibration) parameter */
typedef struct CellPadActParam {
    u8 motor[CELL_PAD_ACTUATOR_MAX];
} CellPadActParam;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* NID: 0x1CF98800 */
s32 cellPadInit(u32 max_connect);

/* NID: 0x4D9B75D5 */
s32 cellPadEnd(void);

/* NID: 0x8B72CBA1 */
s32 cellPadGetData(u32 port_no, CellPadData* data);

/* NID: 0x8B8013DA */
s32 cellPadGetInfo2(CellPadInfo2* info);
s32 cellPadPeriphGetInfo(CellPadPeriphInfo* info);

/* NID: 0xA703A51D */
s32 cellPadSetPortSetting(u32 port_no, u32 port_setting);
s32 cellPadInfoPressMode(u32 port_no);
s32 cellPadSetPressMode(u32 port_no, u32 mode);

/* NID: 0x578E3C98 */
s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info);

/* NID: 0xBE5C3E81 */
s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param);

/* NID: 0x3733EA3C */
s32 cellPadClearBuf(u32 port_no);

/* Internal: call once per frame to update pad state from host input */
void cellPad_poll(void);

/* Host input hold, for a wrapper UI that has the input: while set, every host
 * port reads as connected with nothing pressed and the sticks centred.
 * Scripted input (PAD_FILE, PAD_STICK, ...) still applies. */
void cellPad_set_host_hold(int hold);

/* The mouse, for the keyboard pad's bindings (PAD_KEYS names mouse1 to mouse5,
 * wheelup and wheeldown). The title's window code passes them while the
 * game has its window's mouse, so a click that brings the window to the
 * front, a drag of its title bar or a click on the title's own menus never
 * presses anything.
 * Buttons held: bit 0 = mouse1 (left), 1 = right, 2 = middle, 3 and 4 = the
 * side buttons. Wheel: notches, positive away from the user (up); each is
 * queued with the buttons it presses as it turns (Shift held then counts
 * when its turn comes), at most 4 waiting. */
void cellPad_host_mouse_buttons(unsigned mask);
/* 1 while keys and the mouse stay out of port 0, which has a pad (PAD_KEYBOARD_WITH_PAD, on by default on a Steam Deck):
 * the mouse's other uses for the game, such as looking around, should stay out too. */
int cellPad_host_keys_out(void);
void cellPad_host_mouse_wheel(int notches);

/* A key or mouse button is held now, by its Windows virtual-key code (VK_LBUTTON to VK_XBUTTON2 for the mouse), read
 * the way the keyboard pad reads keys: for the title's window code, such as a wrapper UI that waits for every key to
 * be let go. 0 in a build without host input. */
int cellPad_host_key_down(int vk);

/* The user's pads count now: 0 with PS3_NO_USER_INPUT, or with PAD_FOCUS_ONLY=1
 * while no window of this process is in front. For the title's own UI, which
 * reads the pads itself, so it ignores them when the title does. */
int cellPad_host_pads_on(void);

/* The controllers connected at the last poll, by their PAD_CONTROLLER names
 * ("controller1", "playstation2", ...) in port order, separated by commas,
 * into out (n bytes, always terminated). Returns how many. For a title's
 * settings, to show which controller is which. */
int cellPad_host_controllers(char* out, int n);

/* The i-th of those controllers (0 first): its PAD_CONTROLLER name into id
 * (idn bytes), and what it is ("Xbox controller (wireless)", "DualSense
 * (USB)", SDL's name for it) into name (namen bytes; empty when unknown).
 * Both are always terminated. 0 when there is no i-th. */
int cellPad_host_controller(int i, char* id, int idn, char* name, int namen);

/* One stick (0 left, 1 right) of the pad on port 0, as the pad reports it: x and y -1 to 1 with down positive, before
 * any deadzone. Reads the devices when called, so it works before the title polls and under the host hold; for a
 * settings screen, once a frame at most. Returns which deadzone the pad goes through (for cellPad_host_stick_stage):
 * 1 XInput's or SDL's, 2 the HID reader's for a Sony pad, 0 no pad (x and y 0). */
int cellPad_host_stick_raw(int stick, float* x, float* y);

/* What the title would read for that stick position: x and y through the deadzones in force on that path, to the
 * pad's 0-255 bytes. stick -1 gives the default deadzone whatever PAD_DEADZONE_LEFT and _RIGHT say. */
void cellPad_host_stick_stage(int path, int stick, float x, float y, unsigned char* out_x, unsigned char* out_y);

/* The device whose button icons the title shows: the last one used on port 0, by pad_glyph.h's rules. 0 before
 * the first poll and with PS3_NO_USER_INPUT, 1 the keyboard and mouse, 2 a PlayStation pad, 3 an Xbox (or other
 * XInput, or unknown) pad, 4 a Nintendo pad (off Windows, where SDL reads it; Steam Input shows one as Xbox).
 * PAD_GLYPH_LOG=1 logs each change. Any thread. */
int cellPad_host_glyph_device(void);

/* One of the title's menus is open: PAD_KEYS bindings marked "menu:" count,
 * and the other bindings of the same inputs do not. */
void cellPad_host_set_menu(int open);

/* Hold a stick on port 0 (0 = left, 1 = right; 0-255, 128 = centred) for the
 * next `reads` packets the title receives, over whatever else drives it. For
 * title code that turns host input into stick moves, such as a stick flick
 * from the mouse. Dropped while the host hold is set. */
void cellPad_host_stick(int stick, unsigned char x, unsigned char y, unsigned reads);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_PAD_H */
