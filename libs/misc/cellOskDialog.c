/*
 * ps3recomp - cellOskDialog HLE implementation
 *
 * The on-screen keyboard, as a title sees it: LoadAsync opens the dialog and
 * posts CELL_SYSUTIL_OSKDIALOG_LOADED; when the player confirms or cancels it
 * posts FINISHED; the title then calls UnloadAsync for the text, which posts
 * UNLOADED. Titles drive their name-entry screens off those three events, so
 * a dialog that never posts them leaves the title waiting for good.
 *
 * The player types on the host. On Windows LoadAsync opens a small text-input
 * window on its own thread (Enter accepts, Esc cancels). PS3_OSK_TEXT=<utf-8>
 * -- or cellOskDialogSetDefaultResponse -- answers at once instead, for
 * scripted runs and hosts without a window.
 */

#include "cellOskDialog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../guest_struct.h"   /* GUEST_EA, vm_read/vm_write: guest EA -> host */
#include "../system/cellSysutil.h"   /* cellSysutilQueueEvent */

#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#define OSK_LOCK()   AcquireSRWLockExclusive(&s_lock)
#define OSK_UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define OSK_LOCK()   pthread_mutex_lock(&s_lock)
#define OSK_UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

/* CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED: what Abort and UnloadAsync answer
 * when no dialog is loaded. */
#define OSK_ERROR_NOT_OPENED 0x8002B302

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* UNLOADED -> (LoadAsync) OPEN -> (confirm / cancel / Abort) CLOSED
 * -> (UnloadAsync) UNLOADED. */
enum { OSK_UNLOADED, OSK_OPEN, OSK_CLOSED };
static int  s_state = OSK_UNLOADED;
static s32  s_result = CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK;
static u32  s_limit = CELL_OSK_MAX_TEXT_LENGTH - 1;
static char s_default_response[CELL_OSK_MAX_TEXT_LENGTH + 1] = "";

/* The text, in host-order UTF-16 code units, NUL-terminated. */
static CellOskDialogChar s_text[CELL_OSK_MAX_TEXT_LENGTH + 1];
static CellOskDialogChar s_prompt[CELL_OSK_MAX_TEXT_LENGTH + 1];

/* Log a UTF-16 string as UTF-8 (ASCII with '?' where there is no converter). */
static void osk_log(const char* what, const CellOskDialogChar* s)
{
    char out[CELL_OSK_MAX_TEXT_LENGTH * 3 + 1];
#ifdef _WIN32
    int n = WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)s, -1, out, (int)sizeof out, NULL, NULL);
    if (n <= 0) out[0] = 0;
#else
    u32 i = 0;
    for (; s[i] && i < sizeof out - 1; i++) out[i] = s[i] < 128 ? (char)s[i] : '?';
    out[i] = 0;
#endif
    printf("[cellOskDialog] %s \"%s\"\n", what, out);
}

/* UTF-8 -> UTF-16, at most max units. Returns the unit count. */
static u32 osk_from_utf8(const char* src, CellOskDialogChar* dst, u32 max)
{
    u32 n = 0;
#ifdef _WIN32
    int w = MultiByteToWideChar(CP_UTF8, 0, src, -1, NULL, 0);
    if (w > 0) {
        WCHAR* tmp = (WCHAR*)malloc((size_t)w * sizeof(WCHAR));
        if (tmp && MultiByteToWideChar(CP_UTF8, 0, src, -1, tmp, w) > 0)
            for (; n < max && tmp[n]; n++) dst[n] = tmp[n];
        free(tmp);
    }
#else
    for (; n < max && src[n]; n++) dst[n] = (u8)src[n];
#endif
    dst[n] = 0;
    return n;
}

/* Every registered sysutil callback hears every dialog event; slots nobody
 * registered are skipped when the queue drains. */
static void osk_post(u32 status)
{
    for (int slot = 0; slot < CELL_SYSUTIL_MAX_CALLBACKS; slot++)
        cellSysutilQueueEvent(slot, status, 0);
}

/* The player is done: keep the result and text, tell the title. A second
 * close (the window going away after an Abort) changes nothing. */
static void osk_close(s32 result, const CellOskDialogChar* text, u32 n)
{
    OSK_LOCK();
    if (s_state != OSK_OPEN) { OSK_UNLOCK(); return; }
    s_result = result;
    if (text) {
        if (n > s_limit) n = s_limit;
        memcpy(s_text, text, n * sizeof *s_text);
        s_text[n] = 0;
    }
    s_state = OSK_CLOSED;
    OSK_UNLOCK();
    printf("[cellOskDialog] closed, result %d\n", result);
    if (result == CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK) osk_log("text", s_text);
    osk_post(CELL_SYSUTIL_OSKDIALOG_FINISHED);
}

/* ---------------------------------------------------------------------------
 * Host input window (Windows)
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#define OSK_IDC_EDIT 100
static HWND s_hwnd;

static LRESULT CALLBACK osk_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            WCHAR buf[CELL_OSK_MAX_TEXT_LENGTH + 1] = {0};
            int n = 0;
            if (LOWORD(w) == IDOK)
                n = GetWindowTextW(GetDlgItem(h, OSK_IDC_EDIT), buf, CELL_OSK_MAX_TEXT_LENGTH);
            osk_close(LOWORD(w) == IDOK ? CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK
                                        : CELL_OSK_DIALOG_INPUT_FIELD_RESULT_CANCELED,
                      LOWORD(w) == IDOK ? (const CellOskDialogChar*)buf : NULL,
                      n > 0 ? (u32)n : 0);
            DestroyWindow(h);
        }
        return 0;
    case WM_CLOSE:
        SendMessageW(h, WM_COMMAND, IDCANCEL, 0);
        return 0;
    case WM_DESTROY:
        OSK_LOCK(); s_hwnd = NULL; OSK_UNLOCK();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

/* The title's own window, to centre the dialog over. */
static BOOL CALLBACK osk_find_game_window(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && h != GetConsoleWindow() &&
        !GetWindow(h, GW_OWNER)) {
        *(HWND*)lp = h;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI osk_window_thread(LPVOID arg)
{
    (void)arg;
    static const WCHAR cls[] = L"ps3recomp_osk";
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = osk_wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));   /* IDC_ARROW */
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);   /* fails harmlessly once registered */

    /* Unowned on purpose: a window owned across threads shares the game
     * window's input queue, and a game thread busy elsewhere would stall it. */
    const int cw = 520, ch = 132;
    RECT r = {0, 0, cw, ch};
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_TOPMOST | WS_EX_DLGMODALFRAME);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND game = NULL;
    EnumWindows(osk_find_game_window, (LPARAM)&game);
    RECT g = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (game) GetWindowRect(game, &g);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, cls, L"Enter text", style,
                             g.left + (g.right - g.left - ww) / 2, g.top + (g.bottom - g.top - wh) / 2,
                             ww, wh, NULL, NULL, inst, NULL);
    if (!h) {
        /* No window: answer with the initial text rather than hang the title. */
        osk_close(CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK, NULL, 0);
        return 0;
    }

    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    HFONT font = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)
               ? CreateFontIndirectW(&ncm.lfMessageFont) : NULL;
    OSK_LOCK();
    WCHAR prompt[CELL_OSK_MAX_TEXT_LENGTH + 1], init[CELL_OSK_MAX_TEXT_LENGTH + 1];
    memcpy(prompt, s_prompt, sizeof prompt);
    memcpy(init, s_text, sizeof init);
    const u32 limit = s_limit;
    OSK_UNLOCK();

    HWND label = CreateWindowExW(0, L"STATIC", prompt, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 12, 10, cw - 24, 24, h, NULL, inst, NULL);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", init,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                12, 40, cw - 24, 28, h, (HMENU)(INT_PTR)OSK_IDC_EDIT, inst, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              cw - 212, 86, 96, 30, h, (HMENU)(INT_PTR)IDOK, inst, NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  cw - 108, 86, 96, 30, h, (HMENU)(INT_PTR)IDCANCEL, inst, NULL);
    if (font) {
        HWND ctl[] = {label, edit, ok, cancel};
        for (int i = 0; i < 4; i++) SendMessageW(ctl[i], WM_SETFONT, (WPARAM)font, TRUE);
    }
    /* limit 0 would mean "no limit" to an edit control. */
    SendMessageW(edit, EM_SETLIMITTEXT, limit ? limit : 1, 0);
    SendMessageW(edit, EM_SETSEL, 0, -1);

    OSK_LOCK(); s_hwnd = h; const int still_open = s_state == OSK_OPEN; OSK_UNLOCK();
    if (!still_open) DestroyWindow(h);   /* aborted while the window was being built */
    else {
        ShowWindow(h, SW_SHOW);
        SetForegroundWindow(h);
        SetFocus(edit);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (font) DeleteObject(font);
    return 0;
}
#endif

/* ---------------------------------------------------------------------------
 * Guest struct access
 *
 * CellOskDialogInputFieldInfo is { CellOskDialogChar* message;
 * CellOskDialogChar* init_text; u32 limit_length; } and
 * CellOskDialogCallbackReturnParam is { s32 result; s32 numCharsResultString;
 * CellOskDialogChar* pResultString; }. Both lead with (or contain) pointers,
 * which the guest stores in four bytes and we in eight, so a cast misplaces
 * every field after the first. The strings are UTF-16 and big-endian, so the
 * code units come across one at a time too.
 * -----------------------------------------------------------------------*/
enum {
    OSKIF_O_MESSAGE = 0, OSKIF_O_INIT_TEXT = 4, OSKIF_O_LIMIT_LENGTH = 8,
    OSKRET_O_RESULT = 0, OSKRET_O_NUMCHARS = 4, OSKRET_O_PRESULTSTRING = 8
};

/* Copy a NUL-terminated big-endian UTF-16 string out of guest memory. */
static u32 osk_guest_utf16(u32 ea, CellOskDialogChar* dst, u32 max)
{
    u32 n = 0;
    if (!ea)
        return 0;
    while (n < max) {
        u16 c = vm_read16(ea + n * 2);
        if (!c)
            break;
        dst[n++] = c;
    }
    dst[n] = 0;
    return n;
}

/* Hand the text to the title: result, then the string into pResultString,
 * which holds numCharsResultString units plus a terminator. */
static s32 osk_write_result(u32 res_ea, s32 result)
{
    const s32 cap = (s32)vm_read32(res_ea + OSKRET_O_NUMCHARS);
    if (cap < 0)
        return (s32)CELL_OSKDIALOG_ERROR_PARAM;
    vm_write32(res_ea + OSKRET_O_RESULT, (u32)result);
    const u32 str_ea = vm_read32(res_ea + OSKRET_O_PRESULTSTRING);
    if (!str_ea || (result != CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK &&
                    result != CELL_OSK_DIALOG_INPUT_FIELD_RESULT_NO_INPUT_TEXT))
        return CELL_OK;
    for (s32 i = 0; i < CELL_OSK_MAX_TEXT_LENGTH - 1; i++) {
        if (i >= cap) { vm_write16(str_ea + (u32)i * 2, 0); break; }
        vm_write16(str_ea + (u32)i * 2, s_text[i]);
        if (!s_text[i]) break;
    }
    return CELL_OK;
}

void cellOskDialogSetDefaultResponse(const char* text)
{
    if (text) {
        strncpy(s_default_response, GUEST_PTR(text, const char*),
                CELL_OSK_MAX_TEXT_LENGTH);
        s_default_response[CELL_OSK_MAX_TEXT_LENGTH] = '\0';
    }
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* Not SDK functions: LoadAsync is the entry point and needs no set-up. Kept
 * so existing callers link; they only log. */
s32 cellOskDialogInit(u32 container)
{
    (void)container;
    printf("[cellOskDialog] Init()\n");
    return CELL_OK;
}

s32 cellOskDialogEnd(void)
{
    printf("[cellOskDialog] End()\n");
    return CELL_OK;
}

s32 cellOskDialogLoadAsync(u32 container,
                           const CellOskDialogParam* dialogParam,
                           const CellOskDialogInputFieldInfo* inputFieldInfo)
{
    (void)container;
    const u32 info_ea    = GUEST_EA(inputFieldInfo);
    const u32 message_ea = info_ea ? vm_read32(info_ea + OSKIF_O_MESSAGE)      : 0;
    const u32 init_ea    = info_ea ? vm_read32(info_ea + OSKIF_O_INIT_TEXT)    : 0;
    const u32 limit      = info_ea ? vm_read32(info_ea + OSKIF_O_LIMIT_LENGTH) : 0;
    if (!GUEST_EA(dialogParam) || !info_ea || !message_ea || !init_ea ||
        limit > CELL_OSK_MAX_TEXT_LENGTH)
        return (s32)CELL_OSKDIALOG_ERROR_PARAM;

    OSK_LOCK();
    if (s_state != OSK_UNLOADED) {
        OSK_UNLOCK();
        return (s32)CELL_OSKDIALOG_SYSUTIL_ERROR_BUSY;
    }
    s_limit = limit >= CELL_OSK_MAX_TEXT_LENGTH ? CELL_OSK_MAX_TEXT_LENGTH - 1 : limit;
    osk_guest_utf16(message_ea, s_prompt, CELL_OSK_MAX_TEXT_LENGTH);
    osk_guest_utf16(init_ea, s_text, s_limit);
    s_result = CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK;
    s_state = OSK_OPEN;
    OSK_UNLOCK();

    osk_log("LoadAsync, prompt", s_prompt);
    osk_log("initial text", s_text);
    printf("[cellOskDialog] limit %u chars\n", s_limit);
    osk_post(CELL_SYSUTIL_OSKDIALOG_LOADED);

    /* A scripted answer closes the dialog straight away. */
    const char* scripted = getenv("PS3_OSK_TEXT");
    if (!scripted && s_default_response[0]) scripted = s_default_response;
    if (scripted) {
        CellOskDialogChar text[CELL_OSK_MAX_TEXT_LENGTH + 1];
        const u32 n = osk_from_utf8(scripted, text, CELL_OSK_MAX_TEXT_LENGTH);
        osk_close(CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK, text, n);
        return CELL_OK;
    }
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, osk_window_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else osk_close(CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK, NULL, 0);
#else
    /* No host window here: accept the initial text. */
    osk_close(CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK, NULL, 0);
#endif
    return CELL_OK;
}

s32 cellOskDialogUnloadAsync(CellOskDialogCallbackReturnParam* result)
{
    const u32 res_ea = GUEST_EA(result);
    if (!res_ea)
        return (s32)CELL_OSKDIALOG_ERROR_PARAM;

    OSK_LOCK();
    if (s_state == OSK_UNLOADED) {
        OSK_UNLOCK();
        return (s32)OSK_ERROR_NOT_OPENED;
    }
    /* Unloading a dialog still on screen takes what it holds. */
#ifdef _WIN32
    if (s_state == OSK_OPEN && s_hwnd) PostMessageW(s_hwnd, WM_CLOSE, 0, 0);
#endif
    const s32 rc = osk_write_result(res_ea, s_result);
    if (rc == CELL_OK) s_state = OSK_UNLOADED;
    OSK_UNLOCK();
    if (rc != CELL_OK)
        return rc;
    printf("[cellOskDialog] UnloadAsync, result %d\n", s_result);
    osk_post(CELL_SYSUTIL_OSKDIALOG_UNLOADED);
    return CELL_OK;
}

s32 cellOskDialogGetSize(u32* width, u32* height)
{
    if (!width || !height)
        return CELL_OSK_ERROR_INVALID_PARAMETER;

    /* Standard PS3 OSK dimensions */
    vm_write32((u32)(uintptr_t)width, (u32)640);
    vm_write32((u32)(uintptr_t)height, (u32)240);
    return CELL_OK;
}

s32 cellOskDialogAbort(void)
{
    printf("[cellOskDialog] Abort()\n");
    OSK_LOCK();
    if (s_state == OSK_UNLOADED) {
        OSK_UNLOCK();
        return (s32)OSK_ERROR_NOT_OPENED;
    }
    const int was_open = s_state == OSK_OPEN;
    if (was_open) {
        s_result = CELL_OSK_DIALOG_INPUT_FIELD_RESULT_ABORT;
        s_state = OSK_CLOSED;
    }
#ifdef _WIN32
    if (s_hwnd) PostMessageW(s_hwnd, WM_CLOSE, 0, 0);
#endif
    OSK_UNLOCK();
    if (was_open) osk_post(CELL_SYSUTIL_OSKDIALOG_FINISHED);
    return CELL_OK;
}

s32 cellOskDialogSetInitialInputDevice(u32 device)
{
    (void)device;
    return CELL_OK;
}

s32 cellOskDialogSetInitialKeyLayout(u32 layout)
{
    (void)layout;
    printf("[cellOskDialog] SetInitialKeyLayout(%u)\n", layout);
    return CELL_OK;
}

s32 cellOskDialogSetLayoutMode(u32 mode)
{
    (void)mode;
    printf("[cellOskDialog] SetLayoutMode(%u)\n", mode);
    return CELL_OK;
}

s32 cellOskDialogSetSeparateWindowOption(u32 option)
{
    (void)option;
    return CELL_OK;
}

/* The text so far, without closing anything. */
s32 cellOskDialogGetInputText(CellOskDialogCallbackReturnParam* result)
{
    const u32 res_ea = GUEST_EA(result);
    if (!res_ea)
        return (s32)CELL_OSKDIALOG_ERROR_PARAM;
    OSK_LOCK();
    const s32 rc = osk_write_result(res_ea, s_text[0] ? CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK
                                                      : CELL_OSK_DIALOG_INPUT_FIELD_RESULT_NO_INPUT_TEXT);
    OSK_UNLOCK();
    return rc;
}
