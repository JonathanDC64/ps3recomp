/*
 * ps3recomp - cellOskDialog HLE
 *
 * On-screen keyboard dialog: load/unload, get input text.
 */

#ifndef PS3RECOMP_CELL_OSK_DIALOG_H
#define PS3RECOMP_CELL_OSK_DIALOG_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
/* As the SDK numbers them. The names below used to have values of their own
 * in 0x8002B3xx, which is cellMsgDialog's range; they are kept as aliases. */
#define CELL_OSKDIALOG_ERROR_IME_ALREADY_IN_USE 0x8002B501
#define CELL_OSKDIALOG_ERROR_GET_SIZE_ERROR     0x8002B502
#define CELL_OSKDIALOG_ERROR_UNKNOWN            0x8002B503
#define CELL_OSKDIALOG_ERROR_PARAM              0x8002B504
/* A second LoadAsync while one dialog is up is a sysutil BUSY. */
#define CELL_OSKDIALOG_SYSUTIL_ERROR_BUSY       0x8002B105

#define CELL_OSK_ERROR_NOT_INITIALIZED          CELL_OSKDIALOG_ERROR_UNKNOWN
#define CELL_OSK_ERROR_ALREADY_INITIALIZED      CELL_OSKDIALOG_ERROR_UNKNOWN
#define CELL_OSK_ERROR_INVALID_PARAMETER        CELL_OSKDIALOG_ERROR_PARAM
#define CELL_OSK_ERROR_DIALOG_NOT_LOADED        CELL_OSKDIALOG_ERROR_UNKNOWN
#define CELL_OSK_ERROR_DIALOG_ALREADY_LOADED    CELL_OSKDIALOG_SYSUTIL_ERROR_BUSY
#define CELL_OSK_ERROR_ABORT                    CELL_OSKDIALOG_ERROR_UNKNOWN
#define CELL_OSK_ERROR_UNKNOWN                  CELL_OSKDIALOG_ERROR_UNKNOWN

/* Sysutil callback status codes the dialog posts. */
#define CELL_SYSUTIL_OSKDIALOG_LOADED           0x0502
#define CELL_SYSUTIL_OSKDIALOG_FINISHED         0x0503
#define CELL_SYSUTIL_OSKDIALOG_UNLOADED         0x0504
#define CELL_SYSUTIL_OSKDIALOG_INPUT_ENTERED    0x0505
#define CELL_SYSUTIL_OSKDIALOG_INPUT_CANCELED   0x0506

/* ---------------------------------------------------------------------------
 * Dialog result codes
 * -----------------------------------------------------------------------*/
#define CELL_OSK_DIALOG_RESULT_OK               0
#define CELL_OSK_DIALOG_RESULT_CANCELED         1
#define CELL_OSK_DIALOG_RESULT_ABORT            2

/* ---------------------------------------------------------------------------
 * Input field types
 * -----------------------------------------------------------------------*/
#define CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK           0
#define CELL_OSK_DIALOG_INPUT_FIELD_RESULT_CANCELED     1
#define CELL_OSK_DIALOG_INPUT_FIELD_RESULT_ABORT        2
#define CELL_OSK_DIALOG_INPUT_FIELD_RESULT_NO_INPUT_TEXT 3

/* ---------------------------------------------------------------------------
 * Layout modes
 * -----------------------------------------------------------------------*/
#define CELL_OSK_PANEL_MODE_DEFAULT             0
#define CELL_OSK_PANEL_MODE_DEFAULT_NO_JP       1
#define CELL_OSK_PANEL_MODE_ALPHABET_FULL       2
#define CELL_OSK_PANEL_MODE_NUMERAL_FULL        3
#define CELL_OSK_PANEL_MODE_NUMERAL             4
#define CELL_OSK_PANEL_MODE_URL                 5
#define CELL_OSK_PANEL_MODE_EMAIL               6
#define CELL_OSK_PANEL_MODE_PASSWORD            7

/* ---------------------------------------------------------------------------
 * Continuous mode
 * -----------------------------------------------------------------------*/
#define CELL_OSK_CONTINUOUS_MODE_NONE           0
#define CELL_OSK_CONTINUOUS_MODE_SEND           1
#define CELL_OSK_CONTINUOUS_MODE_NEWLINE        2

/* ---------------------------------------------------------------------------
 * Max sizes
 * -----------------------------------------------------------------------*/
#define CELL_OSK_MAX_TEXT_LENGTH                 512
#define CELL_OSK_MAX_TITLE_LENGTH               64

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/* UTF-16LE character (PS3 uses big-endian u16, but for HLE we use host) */
typedef u16 CellOskDialogChar;

typedef struct CellOskDialogInputFieldInfo {
    CellOskDialogChar*  message;        /* prompt text (UTF-16) */
    CellOskDialogChar*  init_text;      /* initial text (UTF-16) */
    u32                 limit_length;   /* max input chars */
} CellOskDialogInputFieldInfo;

typedef struct CellOskDialogCallbackReturnParam {
    s32                 result;         /* CELL_OSK_DIALOG_INPUT_FIELD_RESULT_* */
    u32                 numCharsResultString;   /* IN: capacity of pResultString */
    CellOskDialogChar*  pResultString;
} CellOskDialogCallbackReturnParam;

typedef struct CellOskDialogPoint {
    float x;
    float y;
} CellOskDialogPoint;

typedef struct CellOskDialogParam {
    u32                 allowOskPanelFlg;
    u32                 firstViewPanel;
    CellOskDialogPoint  controlPoint;
    s32                 prohibitFlgs;
} CellOskDialogParam;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellOskDialogInit(u32 container);
s32 cellOskDialogEnd(void);

s32 cellOskDialogLoadAsync(u32 container,
                           const CellOskDialogParam* dialogParam,
                           const CellOskDialogInputFieldInfo* inputFieldInfo);
s32 cellOskDialogUnloadAsync(CellOskDialogCallbackReturnParam* result);

s32 cellOskDialogGetSize(u32* width, u32* height);
s32 cellOskDialogAbort(void);

s32 cellOskDialogSetInitialInputDevice(u32 device);
s32 cellOskDialogSetInitialKeyLayout(u32 layout);
s32 cellOskDialogSetLayoutMode(u32 mode);
s32 cellOskDialogSetSeparateWindowOption(u32 option);

s32 cellOskDialogGetInputText(CellOskDialogCallbackReturnParam* result);

/* Set a default response string (call before LoadAsync if desired). With
 * one set, or PS3_OSK_TEXT in the environment, the dialog answers at once
 * instead of asking on the host. */
void cellOskDialogSetDefaultResponse(const char* text);

/* A host interface for the dialog, such as a port's own menus, in place of
 * the Windows text-input window. open runs on the title's thread at LoadAsync
 * with the prompt and the initial text (host-order UTF-16, NUL-terminated)
 * and the most characters the title takes. close runs when the title stops
 * the dialog first (Abort, or UnloadAsync while it is still up). The host
 * answers once with cellOskDialog_host_finish, from any thread: ok = 0 is
 * Cancel. NULL removes the host. Not SDK functions. */
typedef struct CellOskDialogHost {
    void (*open)(const CellOskDialogChar* prompt, const CellOskDialogChar* text, u32 limit);
    void (*close)(void);
} CellOskDialogHost;
void cellOskDialog_set_host(const CellOskDialogHost* host);
void cellOskDialog_host_finish(int ok, const CellOskDialogChar* text, u32 n);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_OSK_DIALOG_H */
