/* ---------------------------------------------------------------------------
 * The window: Win32, or SDL off Windows
 * -----------------------------------------------------------------------*/

/* The Windows session is locked. Behind the lock screen DWM holds each back
 * buffer for 250-500 ms while Present still returns S_OK (not the
 * occluded status), and the frame rate fell to 4 fps waiting for them.
 * Frames are rendered but not presented while this is set. Written by the
 * window thread (WM_WTSSESSION_CHANGE, and once at window creation), read
 * at the flip. */
static volatile LONG s_session_locked;

#ifdef _WIN32
static void session_lock_init(HWND hwnd)
{
    WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
    /* A run started behind the lock screen gets no LOCK message. */
    WTSINFOEXW* info = NULL;
    DWORD n = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                    WTSSessionInfoEx, (LPWSTR*)&info, &n) && info) {
        if (info->Level == 1)
            s_session_locked = info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
        WTSFreeMemory(info);
    }
    if (s_session_locked)
        fprintf(stderr, "[D3D12] session locked: rendering without presenting until unlock%c", 10);
}
#else
static void session_lock_init(HWND hwnd) { (void)hwnd; }   /* (Windows only) */
#endif

/* rc_set_overlay. Set from one thread, read by the window and render
 * threads: the hooks are swapped in whole and then flagged. */
static rsx_d3d12_overlay s_overlay;
static volatile long     s_overlay_on;
static volatile long     s_overlay_repaint;   /* rc_overlay_repaint */
static volatile long     s_overlay_wake;      /* rc_overlay_wake */

/* The window's client size as of its last WM_SIZE, (w << 16) | h, written by
 * the window thread; the render thread resizes the swap chain to it between
 * frames (swap_resize). 0 until the first WM_SIZE. */
static volatile LONG s_win_req;

/* Set by the window thread when what the window's display reports may have
 * changed (the window activated, a display change): the render thread asks
 * again (hdr_display_check). */
static volatile LONG s_hdr_requery = 1;

#ifdef _WIN32
/* Borderless fullscreen (Alt+Enter, rc_set_borderless): the window
 * covers its monitor, no frame. A window, not exclusive fullscreen, so alt-tab
 * and the overlay behave as windowed. Window-thread state.
 * A maximized window is restored before its rect and style are saved, and
 * maximized again after they are put back. So the fullscreen window is never
 * a maximized one (Windows puts a maximized window back in the work area when
 * the taskbar or the display changes), and leaving gives the window as it
 * was: maximized, or at the user's own size. (Not Get/SetWindowPlacement: the
 * window would still have to be restored first, and the saved rect and style
 * already cover the rest.) */
#define WM_RSX_BORDERLESS (WM_APP + 0x52)
static int   s_borderless;
static RECT  s_windowed_rect;
static LONG  s_windowed_style;
static int   s_windowed_maximized;

static void set_borderless(HWND hwnd, int on)
{
    if (!!on == s_borderless) return;
    if (on) {
        MONITORINFO mi = {sizeof mi};
        if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;   /* where it is now */
        s_windowed_maximized = IsZoomed(hwnd) != 0;
        if (s_windowed_maximized) ShowWindow(hwnd, ps3_no_user_input() ? SW_SHOWNOACTIVATE : SW_RESTORE);
        GetWindowRect(hwnd, &s_windowed_rect);
        s_windowed_style = GetWindowLongA(hwnd, GWL_STYLE);
        SetWindowLongA(hwnd, GWL_STYLE, (s_windowed_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | (ps3_no_user_input() ? SWP_NOACTIVATE : 0));
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, s_windowed_style);
        SetWindowPos(hwnd, NULL, s_windowed_rect.left, s_windowed_rect.top,
                     s_windowed_rect.right - s_windowed_rect.left, s_windowed_rect.bottom - s_windowed_rect.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOZORDER | (ps3_no_user_input() ? SWP_NOACTIVATE : 0));
        if (s_windowed_maximized) ShowWindow(hwnd, SW_MAXIMIZE);   /* that rect becomes its restored size */
    }
    s_borderless = !!on;
}

static int window_dpi(HWND hwnd);

static LRESULT CALLBACK d3d12_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* PS3_NO_USER_INPUT=1 (ps3_knob.h): no key, mouse or raw input message of
     * the user's reaches the overlay or the hot keys. The window can still be
     * moved, sized and closed from its frame. */
    if (((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
         msg == WM_INPUT) && ps3_no_user_input())
        return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {   /* before the overlay: the frame's own business */
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && LOWORD(lp) && HIWORD(lp))
            InterlockedExchange(&s_win_req, (LONG)(((u32)LOWORD(lp) << 16) | HIWORD(lp)));
        break;
    case WM_RSX_BORDERLESS:
        set_borderless(hwnd, (int)wp);
        return 0;
    case WM_ACTIVATE:
    case WM_DISPLAYCHANGE:   /* HDR switched, or the SDR white level moved meanwhile (hdr_display_check) */
        InterlockedExchange(&s_hdr_requery, 1);
        break;
    case WM_GETMINMAXINFO: {   /* a client area of at least 640x360 (at the display's scale): below that an overlay's text overlaps */
        const int dpi = window_dpi(hwnd);
        RECT r = {0, 0, MulDiv(640, dpi, 96), MulDiv(360, dpi, 96)};
        AdjustWindowRect(&r, (DWORD)GetWindowLongA(hwnd, GWL_STYLE), FALSE);
        ((MINMAXINFO*)lp)->ptMinTrackSize.x = r.right - r.left;
        ((MINMAXINFO*)lp)->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_DPICHANGED: {      /* moved to a display of another scale: take the size Windows suggests */
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29)) && !(lp & (1 << 30))) {   /* Alt+Enter, not repeat */
            set_borderless(hwnd, !s_borderless);
            return 0;
        }
        break;
    }
    if (s_overlay_on && s_overlay.wndproc) {
        long long r = 0;
        if (s_overlay.wndproc(s_overlay.user, hwnd, msg, (unsigned long long)wp, (long long)lp, &r))
            return (LRESULT)r;
    }
    switch (msg) {
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK)   InterlockedExchange(&s_session_locked, 1);
        if (wp == WTS_SESSION_UNLOCK) InterlockedExchange(&s_session_locked, 0);
        return 0;
    case WM_CLOSE:
        s_d3d.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && !s_overlay_on) {
            s_d3d.window_closed = 1;
            DestroyWindow(hwnd);
        }
        if (wp == VK_F9 && !(lp & (1 << 30)) && ps3_dev_hotkeys()) {   /* not auto-repeat */
            extern void cellGcm_replay_hotkey(void);
            cellGcm_replay_hotkey();
        }
        if (wp == VK_F11 && !(lp & (1 << 30)) && ps3_dev_hotkeys())    /* profile start/stop */
            InterlockedIncrement(&g_rsx_prof_toggle);
        return 0;
    case WM_SYSKEYDOWN:
        /* F10 is the menu key: Windows sends it as WM_SYSKEYDOWN, never
         * WM_KEYDOWN. Handled here, also with the developer keys off, it
         * stops the window entering menu mode. */
        if (wp == VK_F10) {
            if (!(lp & (1 << 30)) && ps3_dev_hotkeys()) s_hotshot_arm = 1;   /* see s_hotshot_left */
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        /* Alt on its own, or Alt+Space, from the keyboard: Windows would put
         * the window's menu up, and it would then take the keys the keyboard
         * pad reads (Left Alt walks and Space rolls in a Souls-style layout).
         * The menu still opens from the title bar with the mouse. */
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* The window is per-monitor DPI aware, so it is drawn at the display's own
 * resolution (not stretched by Windows on a display scaled above 100%) and an
 * overlay can size its text by the display's scale (rsx_d3d12_overlay). Its
 * first size is the title's at that scale. Loaded by name: these arrived in
 * Windows 10 1607/1703, and dwmapi is not otherwise linked. */
typedef BOOL (WINAPI *set_dpi_ctx_fn)(HANDLE);
typedef UINT (WINAPI *dpi_fn)(HWND);
typedef UINT (WINAPI *sys_dpi_fn)(void);
typedef HRESULT (WINAPI *dwm_attr_fn)(HWND, DWORD, LPCVOID, DWORD);

static int window_dpi(HWND hwnd)
{
    static dpi_fn get_dpi;
    static int looked;
    if (!looked) { get_dpi = (dpi_fn)(void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow"); looked = 1; }
    const UINT dpi = get_dpi && hwnd ? get_dpi(hwnd) : 0;
    return dpi ? (int)dpi : 96;
}

static int dpi_aware(void)
{
    HMODULE u = GetModuleHandleA("user32.dll");
    set_dpi_ctx_fn set_ctx = (set_dpi_ctx_fn)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    sys_dpi_fn sys_dpi = (sys_dpi_fn)(void*)GetProcAddress(u, "GetDpiForSystem");
    if (set_ctx) set_ctx((HANDLE)(intptr_t)-4);   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    const UINT dpi = sys_dpi ? sys_dpi() : 0;
    return dpi ? (int)dpi : 96;
}

/* A dark title bar, as dark-themed programs have (RSX_DARK_TITLE=0: the
 * system's). DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Windows 10 20H1 and later). */
static void dark_title(HWND hwnd)
{
    if (!ps3_env_int("RSX_DARK_TITLE", 1)) return;
    HMODULE dwm = LoadLibraryExA("dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);   /* (Windows' own only) */
    dwm_attr_fn set_attr = dwm ? (dwm_attr_fn)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
    const BOOL on = TRUE;
    if (set_attr) set_attr(hwnd, 20, &on, sizeof on);
}

/* The game's icon (rsx_window_icon.h) as a size x size icon, or NULL. */
static HICON game_icon(int size)
{
    if (size <= 0 || size > 256) return NULL;
    uint8_t* rgba = (uint8_t*)malloc((size_t)size * (size_t)size * 4);
    void* mask_bits = calloc((size_t)((size + 15) / 16 * 2), (size_t)size);   /* 1 bit a pixel, rows of whole WORDs */
    HICON icon = NULL;
    if (rgba && mask_bits && rsx_window_icon(size, rgba)) {
        BITMAPV5HEADER bh = {0};
        bh.bV5Size = sizeof bh;
        bh.bV5Width = size;
        bh.bV5Height = -size;   /* top-down */
        bh.bV5Planes = 1;
        bh.bV5BitCount = 32;
        bh.bV5Compression = BI_BITFIELDS;
        bh.bV5RedMask = 0x00FF0000;
        bh.bV5GreenMask = 0x0000FF00;
        bh.bV5BlueMask = 0x000000FF;
        bh.bV5AlphaMask = 0xFF000000;
        void* bits = NULL;
        HDC dc = GetDC(NULL);
        HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bh, DIB_RGB_COLORS, &bits, NULL, 0);
        ReleaseDC(NULL, dc);
        HBITMAP mask = CreateBitmap(size, size, 1, 1, mask_bits);   /* unused with alpha, but required */
        if (color && mask && bits) {
            uint8_t* d = (uint8_t*)bits;
            for (int k = 0; k < size * size; k++) {   /* RGBA -> BGRA, straight alpha as icons take it */
                d[4 * k + 0] = rgba[4 * k + 2];
                d[4 * k + 1] = rgba[4 * k + 1];
                d[4 * k + 2] = rgba[4 * k + 0];
                d[4 * k + 3] = rgba[4 * k + 3];
            }
            ICONINFO ii = {TRUE, 0, 0, mask, color};
            icon = CreateIconIndirect(&ii);
        }
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
    }
    free(rgba);
    free(mask_bits);
    return icon;
}

/* RSX_MAXIMIZED=1 (default 0; read once, here): the window opens maximized,
 * and the centred size below is the one it restores to. Not with
 * PS3_NO_USER_INPUT=1: a window shown maximized always takes the focus. */
static HWND create_window(u32 width, u32 height, const char* title)
{
    const int dpi = dpi_aware();
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = d3d12_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    /* The title's icon when its exe carries one as icon resource 1; when it
     * has none, the game's own from its ICON0.PNG (rsx_window_icon.h); NULL,
     * the system's default, without either. */
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.hIconSm = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIcon) {
        wc.hIcon = game_icon(MulDiv(32, dpi, 96));
        wc.hIconSm = game_icon(MulDiv(16, dpi, 96));
    }
    wc.lpszClassName = "ps3recomp_d3d12";
    RegisterClassExA(&wc);

    if (rsx_aspect() > 0.0f) width = (u32)((float)height * rsx_aspect() + 0.5f);   /* the shape the frame is shown at */
    int cw = MulDiv((int)width, dpi, 96), ch = MulDiv((int)height, dpi, 96);
    /* Centred in the primary display's work area, and made smaller (same
     * shape) if it would not fit there. */
    MONITORINFO mi = {sizeof mi};
    const POINT origin = {0, 0};
    const int have_area = GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT wr = {0, 0, cw, ch};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    if (have_area) {
        const int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
        const int fw = (wr.right - wr.left) - cw, fh = (wr.bottom - wr.top) - ch;   /* the frame */
        if (cw + fw > aw || ch + fh > ah) {
            const int sw = aw - fw, sh = ah - fh;
            if ((long long)sw * ch < (long long)sh * cw) { ch = MulDiv(ch, sw, cw); cw = sw; }
            else { cw = MulDiv(cw, sh, ch); ch = sh; }
            wr.left = 0; wr.top = 0; wr.right = cw; wr.bottom = ch;
            AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        }
    }
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (have_area) {
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (wr.right - wr.left)) / 2;
        y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - (wr.bottom - wr.top)) / 2;
    }

    /* Keep the display awake (see rsx_null_backend.c). */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    /* PS3_NO_USER_INPUT=1 (ps3_knob.h): the window never takes the focus, when
     * it opens or when it is clicked, so the user's typing stays where it was.
     * It still shows, keeps its taskbar button and presents as usual. */
    const int no_user_input = ps3_no_user_input();
    const int maximized = ps3_env_int("RSX_MAXIMIZED", 0) && !no_user_input;
    HWND hwnd = CreateWindowExA(
        no_user_input ? WS_EX_NOACTIVATE | WS_EX_APPWINDOW : 0, "ps3recomp_d3d12",
        title ? title : "ps3recomp (D3D12)",
        WS_OVERLAPPEDWINDOW,
        x, y,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    if (hwnd) {   /* the title bar's colour before the window first shows */
        dark_title(hwnd);
        ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : no_user_input ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    }
    return hwnd;
}

/* The window lives on a thread of its own that does nothing but pump its
 * messages. Moving or resizing a window runs a modal loop inside
 * DispatchMessage until the mouse is released; when the vblank ticker pumped
 * the messages, that loop stopped the ticks, the FIFO drain and the presents,
 * and the game froze for as long as the window was held. Rendering and
 * presenting stay where they were: DXGI's flip model presents to a window
 * owned by another thread. RSX_WINDOW_THREAD=0 pumps on the ticker as before. */
static int s_window_thread;

typedef struct { u32 w, h; const char* title; HANDLE ready; } window_thread_args;

static DWORD WINAPI window_thread(LPVOID p)
{
    window_thread_args* a = (window_thread_args*)p;
    HWND hwnd = create_window(a->w, a->h, a->title);
    s_d3d.hwnd = hwnd;
    SetEvent(a->ready);   /* `a` is the creator's: not touched after this */
    if (!hwnd) return 0;
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    *(volatile int*)&s_d3d.window_closed = 1;
    return 0;
}

static HWND open_window(u32 width, u32 height, const char* title)
{
    s_window_thread = ps3_env_int("RSX_WINDOW_THREAD", 1) != 0;
    if (!s_window_thread) return create_window(width, height, title);
    window_thread_args a = {width, height, title, CreateEventA(NULL, TRUE, FALSE, NULL)};
    HANDLE t = a.ready ? CreateThread(NULL, 0, window_thread, &a, 0, NULL) : NULL;
    if (!t) {
        if (a.ready) CloseHandle(a.ready);
        s_window_thread = 0;
        return create_window(width, height, title);
    }
    WaitForSingleObject(a.ready, INFINITE);
    CloseHandle(a.ready);
    CloseHandle(t);
    return s_d3d.hwnd;
}

#else /* SDL */

/* The window is SDL's, made by a thread of its own that then waits on its
 * events, as the Win32 one pumps its messages. Other threads change it by
 * posting that thread an event (win_post): a title, borderless on or off, or
 * the close at shutdown. */
static SDL_Window* s_sdl_window;
static Uint32      s_sdl_mail;            /* the event type win_post sends (SDL_RegisterEvents) */
enum { WIN_MAIL_TITLE, WIN_MAIL_BORDERLESS, WIN_MAIL_CLOSE };
static SRWLOCK     s_win_title_lock;
static char        s_win_title[320];
static volatile LONG s_refresh_hz;        /* the window's display, 0 until known */

/* Borderless fullscreen (Alt+Enter, rc_set_borderless): SDL's desktop
 * fullscreen, the window covering its display. Window-thread state. */
static int s_borderless;

static void win_post(int what, int arg)
{
    if (!s_sdl_mail) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = s_sdl_mail;
    e.user.code = what;
    e.user.data1 = (void*)(intptr_t)arg;
    SDL_PushEvent(&e);
}

static void win_set_title(const char* t)
{
    AcquireSRWLockExclusive(&s_win_title_lock);
    snprintf(s_win_title, sizeof s_win_title, "%s", t);
    ReleaseSRWLockExclusive(&s_win_title_lock);
    win_post(WIN_MAIL_TITLE, 0);
}

static void set_borderless(HWND hwnd, int on)
{
    if (!!on == s_borderless) return;
    SDL_SetWindowFullscreen((SDL_Window*)hwnd, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    s_borderless = !!on;
}

/* The drawable's size for swap_resize, as WM_SIZE gives it on Windows. */
static void win_size_note(void)
{
    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(s_sdl_window, &w, &h);
    if (w > 0 && h > 0 && w < 0x10000 && h < 0x10000)
        InterlockedExchange(&s_win_req, (LONG)(((u32)w << 16) | (u32)h));
}

static void win_refresh_note(void)
{
    SDL_DisplayMode m;
    const int d = SDL_GetWindowDisplayIndex(s_sdl_window);
    if (d >= 0 && SDL_GetCurrentDisplayMode(d, &m) == 0 && m.refresh_rate > 20 && m.refresh_rate < 1000)
        InterlockedExchange(&s_refresh_hz, (LONG)m.refresh_rate);
}

/* The overlay's messages off Windows (rsx_d3d12_overlay): the Win32 messages it would get there, made from SDL's
 * events, with Windows' values. */
#define WM_SIZE          0x0005u
#define WM_SETFOCUS      0x0007u
#define WM_KILLFOCUS     0x0008u
#define WM_CLOSE         0x0010u
#define WM_KEYDOWN       0x0100u
#define WM_KEYUP         0x0101u
#define WM_CHAR          0x0102u
#define WM_SYSKEYDOWN    0x0104u
#define WM_SYSKEYUP      0x0105u
#define WM_MOUSEMOVE     0x0200u
#define WM_LBUTTONDOWN   0x0201u
#define WM_LBUTTONUP     0x0202u
#define WM_RBUTTONDOWN   0x0204u
#define WM_RBUTTONUP     0x0205u
#define WM_MBUTTONDOWN   0x0207u
#define WM_MBUTTONUP     0x0208u
#define WM_MOUSEWHEEL    0x020Au
#define WM_XBUTTONDOWN   0x020Bu
#define WM_XBUTTONUP     0x020Cu
#define WM_MOUSELEAVE    0x02A3u

/* rc_post_message's mails carry the message in the code, above the plain mails. */
#define WIN_MAIL_MESSAGE 0x10000

/* An SDL key's Windows virtual-key code, or 0: letters, digits and punctuation by what the key types (Windows' codes
 * follow the keyboard layout too), the rest by what the key is. */
static unsigned win_vk(const SDL_Keysym* k)
{
    const SDL_Keycode c = k->sym;
    if (c >= 'a' && c <= 'z') return (unsigned)(c - 0x20);
    if (c >= '0' && c <= '9') return (unsigned)c;
    if (c >= SDLK_F1 && c <= SDLK_F12) return 0x70u + (unsigned)(c - SDLK_F1);
    if (c >= SDLK_F13 && c <= SDLK_F24) return 0x7Cu + (unsigned)(c - SDLK_F13);
    if (c >= SDLK_KP_1 && c <= SDLK_KP_9) return 0x61u + (unsigned)(c - SDLK_KP_1);
    switch (c) {
    case SDLK_KP_0:         return 0x60;
    case SDLK_BACKSPACE:    return 0x08;
    case SDLK_TAB:          return 0x09;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:     return 0x0D;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:       return 0x10;
    case SDLK_LCTRL:
    case SDLK_RCTRL:        return 0x11;
    case SDLK_LALT:
    case SDLK_RALT:         return 0x12;
    case SDLK_PAUSE:        return 0x13;
    case SDLK_CAPSLOCK:     return 0x14;
    case SDLK_ESCAPE:       return 0x1B;
    case SDLK_SPACE:        return 0x20;
    case SDLK_PAGEUP:       return 0x21;
    case SDLK_PAGEDOWN:     return 0x22;
    case SDLK_END:          return 0x23;
    case SDLK_HOME:         return 0x24;
    case SDLK_LEFT:         return 0x25;
    case SDLK_UP:           return 0x26;
    case SDLK_RIGHT:        return 0x27;
    case SDLK_DOWN:         return 0x28;
    case SDLK_PRINTSCREEN:  return 0x2C;
    case SDLK_INSERT:       return 0x2D;
    case SDLK_DELETE:       return 0x2E;
    case SDLK_LGUI:         return 0x5B;
    case SDLK_RGUI:         return 0x5C;
    case SDLK_APPLICATION:  return 0x5D;
    case SDLK_KP_MULTIPLY:  return 0x6A;
    case SDLK_KP_PLUS:      return 0x6B;
    case SDLK_KP_MINUS:     return 0x6D;
    case SDLK_KP_PERIOD:    return 0x6E;
    case SDLK_KP_DIVIDE:    return 0x6F;
    case SDLK_NUMLOCKCLEAR: return 0x90;
    case SDLK_SCROLLLOCK:   return 0x91;
    case SDLK_SEMICOLON:    return 0xBA;   /* VK_OEM_1 */
    case SDLK_EQUALS:       return 0xBB;   /* VK_OEM_PLUS */
    case SDLK_COMMA:        return 0xBC;
    case SDLK_MINUS:        return 0xBD;
    case SDLK_PERIOD:       return 0xBE;
    case SDLK_SLASH:        return 0xBF;   /* VK_OEM_2 */
    case SDLK_BACKQUOTE:    return 0xC0;   /* VK_OEM_3 */
    case SDLK_LEFTBRACKET:  return 0xDB;   /* VK_OEM_4 */
    case SDLK_BACKSLASH:    return 0xDC;   /* VK_OEM_5 */
    case SDLK_RIGHTBRACKET: return 0xDD;   /* VK_OEM_6 */
    case SDLK_QUOTE:        return 0xDE;   /* VK_OEM_7 */
    default:                return 0;
    }
}

/* Hands the overlay one message; 1 when it took it. */
static int win_overlay(unsigned msg, unsigned long long wp, long long lp)
{
    if (!s_overlay_on || !s_overlay.wndproc) return 0;
    long long r = 0;
    return s_overlay.wndproc(s_overlay.user, s_sdl_window, msg, wp, lp, &r);
}

/* A position as Win32 packs it in a message's lParam (signed 16-bit x low, y high). */
static long long win_xy(int x, int y)
{
    return (long long)(((u32)(u16)(s16)y << 16) | (u16)(s16)x);
}

/* The overlay's messages for one SDL event, as Windows would send them; PS3_NO_USER_INPUT=1 (ps3_knob.h) keeps the
 * keyboard's and the mouse's from it. Returns 1 when the overlay took the event: a key the hot keys then skip, or
 * the close (it asks first, and posts WM_CLOSE once answered). */
static int win_overlay_event(const SDL_Event* e)
{
    const int input = !ps3_no_user_input();
    switch (e->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        const unsigned vk = input ? win_vk(&e->key.keysym) : 0;
        if (!vk) return 0;
        const int down = e->type == SDL_KEYDOWN;
        const int alt = (e->key.keysym.mod & KMOD_ALT) != 0;
        const int sys = alt || vk == 0x12 || vk == 0x79;   /* Alt held, Alt itself or F10: WM_SYSKEY*, as on Windows */
        long long lp = 1 | (long long)(e->key.keysym.scancode & 0xFF) << 16;
        if (alt) lp |= 1LL << 29;
        if (!down || e->key.repeat) lp |= 1LL << 30;   /* down before this message */
        if (!down) lp |= 1LL << 31;
        const int took = win_overlay(down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP), vk, lp);
        /* What TranslateMessage adds: the control characters these keys type. */
        if (down && !sys && (vk == 0x08 || vk == 0x09 || vk == 0x0D || vk == 0x1B)) win_overlay(WM_CHAR, vk, lp);
        return took;
    }
    case SDL_TEXTINPUT: {   /* UTF-8 to code points, a WM_CHAR each */
        const unsigned char* s = (const unsigned char*)e->text.text;
        while (input && *s) {
            unsigned c = *s++;
            int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            c &= more == 3 ? 0x07u : more == 2 ? 0x0Fu : more == 1 ? 0x1Fu : 0x7Fu;
            while (more-- > 0 && (*s & 0xC0) == 0x80) c = (c << 6) | (*s++ & 0x3Fu);
            win_overlay(WM_CHAR, c, 1);
        }
        return 0;
    }
    case SDL_MOUSEMOTION:
        if (!input) return 0;
        win_overlay(WM_MOUSEMOVE, 0, win_xy(e->motion.x, e->motion.y));
        if (e->motion.xrel || e->motion.yrel)
            win_overlay(WM_RSX_MOUSE_MOTION, (unsigned long long)(long long)e->motion.xrel, (long long)e->motion.yrel);
        return 0;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        if (!input) return 0;
        const int down = e->type == SDL_MOUSEBUTTONDOWN;
        unsigned msg;
        unsigned long long wp = 0;
        switch (e->button.button) {
        case SDL_BUTTON_LEFT:   msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; break;
        case SDL_BUTTON_RIGHT:  msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; break;
        case SDL_BUTTON_MIDDLE: msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; break;
        case SDL_BUTTON_X1:
        case SDL_BUTTON_X2:     /* XBUTTON1 or XBUTTON2 in the high word */
            msg = down ? WM_XBUTTONDOWN : WM_XBUTTONUP;
            wp = (unsigned long long)(e->button.button == SDL_BUTTON_X1 ? 1 : 2) << 16;
            break;
        default: return 0;
        }
        win_overlay(msg, wp, win_xy(e->button.x, e->button.y));
        return 0;
    }
    case SDL_MOUSEWHEEL: {   /* WHEEL_DELTA (120) a notch in the high word, positive away from the user */
        const int notches = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
        if (input && notches) win_overlay(WM_MOUSEWHEEL, (unsigned long long)(u16)(s16)(notches * 120) << 16, 0);
        return 0;
    }
    case SDL_WINDOWEVENT:
        switch (e->window.event) {
        case SDL_WINDOWEVENT_CLOSE:        return win_overlay(WM_CLOSE, 0, 0);
        case SDL_WINDOWEVENT_FOCUS_GAINED: win_overlay(WM_SETFOCUS, 0, 0); break;
        case SDL_WINDOWEVENT_FOCUS_LOST:   win_overlay(WM_KILLFOCUS, 0, 0); break;
        case SDL_WINDOWEVENT_LEAVE:        win_overlay(WM_MOUSELEAVE, 0, 0); break;
        case SDL_WINDOWEVENT_MINIMIZED:    win_overlay(WM_SIZE, 1 /* SIZE_MINIMIZED */, 0); break;
        case SDL_WINDOWEVENT_RESTORED:
        case SDL_WINDOWEVENT_SIZE_CHANGED: {
            int w = 0, h = 0;
            SDL_GetWindowSize(s_sdl_window, &w, &h);
            win_overlay(WM_SIZE, 0, win_xy(w, h));
            break;
        }
        default: break;
        }
        return 0;
    default:
        return 0;
    }
}

/* Alt+Enter, which d3d12_wndproc takes before the overlay sees the key: 1 when this was it. */
static int win_alt_enter(const SDL_KeyboardEvent* k)
{
    if (ps3_no_user_input() || k->keysym.sym != SDLK_RETURN || !(k->keysym.mod & KMOD_ALT)) return 0;
    if (!k->repeat) set_borderless(s_sdl_window, !s_borderless);
    return 1;
}

/* The hot keys d3d12_wndproc takes after the overlay, and nothing else (the
 * keyboard pad reads the keys on its own). */
static void win_key(const SDL_KeyboardEvent* k)
{
    if (ps3_no_user_input()) return;
    const SDL_Keycode c = k->keysym.sym;
    if (c == SDLK_ESCAPE && !s_overlay_on) s_d3d.window_closed = 1;
    if (k->repeat || !ps3_dev_hotkeys()) return;   /* the developer keys below */
    if (c == SDLK_F9) {
        extern void cellGcm_replay_hotkey(void);
        cellGcm_replay_hotkey();
    }
    if (c == SDLK_F11) InterlockedIncrement(&g_rsx_prof_toggle);   /* profile start/stop */
    if (c == SDLK_F10) s_hotshot_arm = 1;                           /* see s_hotshot_left */
}

/* Its first size is the title's, made smaller (same shape) if it would not
 * fit the primary display's usable area. RSX_MAXIMIZED=1 (default 0; read
 * once, here): it opens maximized, that size the one it restores to; not with
 * PS3_NO_USER_INPUT=1, as on Windows. The window manager may maximize it only
 * after it shows: the SIZE_CHANGED that follows resizes the swap chain. */
static SDL_Window* create_window(u32 width, u32 height, const char* title)
{
    if (rsx_aspect() > 0.0f) width = (u32)((float)height * rsx_aspect() + 0.5f);   /* the shape the frame is shown at */
    int cw = (int)width, ch = (int)height;
    SDL_Rect area;
    if (SDL_GetDisplayUsableBounds(0, &area) == 0 && area.w > 0 && area.h > 0 && (cw > area.w || ch > area.h)) {
        if ((long long)area.w * ch < (long long)area.h * cw) { ch = (int)((long long)ch * area.w / cw); cw = area.w; }
        else { cw = (int)((long long)cw * area.h / ch); ch = area.h; }
    }
    /* PS3_NO_USER_INPUT=1 (ps3_knob.h): the window does not take the focus
     * when it opens, so the user's typing stays where it was. */
    if (ps3_no_user_input()) SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
    /* The close button is the window's close event alone (the overlay can ask first); SDL_QUIT is then only a
     * signal (Ctrl+C, SIGTERM), which closes at once. */
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    const Uint32 maximized = ps3_env_int("RSX_MAXIMIZED", 0) && !ps3_no_user_input() ? SDL_WINDOW_MAXIMIZED : 0;
    SDL_Window* w = SDL_CreateWindow(title ? title : "ps3recomp (D3D12)", SDL_WINDOWPOS_CENTERED,
                                     SDL_WINDOWPOS_CENTERED, cw, ch,
                                     SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | maximized);
    if (w) SDL_SetWindowMinimumSize(w, 640, 360);   /* below that an overlay's text overlaps */
    rsx_window_icon_sdl(w);   /* the game's own icon (rsx_window_icon.h) */
    return w;
}

typedef struct { u32 w, h; const char* title; HANDLE ready; } window_thread_args;

static DWORD WINAPI window_thread(LPVOID p)
{
    window_thread_args* a = (window_thread_args*)p;
    SDL_Window* w = NULL;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        printf("[D3D12] SDL video init failed: %s\n", SDL_GetError());
    } else {
        s_sdl_mail = SDL_RegisterEvents(1);
        if (s_sdl_mail == (Uint32)-1) s_sdl_mail = 0;
        w = create_window(a->w, a->h, a->title);
        if (!w) printf("[D3D12] SDL_CreateWindow failed: %s\n", SDL_GetError());
    }
    s_sdl_window = w;
    s_d3d.hwnd = w;
    if (w) {
        win_size_note();
        win_refresh_note();
    }
    SetEvent(a->ready);   /* `a` is the creator's: not touched after this */
    if (!w) return 0;
    /* Closing (the window's close button, or Escape) only flags it: the swap
     * chain still presents to this window until the backend shuts down and
     * posts WIN_MAIL_CLOSE, so the window is hidden until then. The overlay
     * sees each event first, as its messages (win_overlay_event); a close it
     * takes is not one. */
    SDL_Event e;
    while (SDL_WaitEvent(&e)) {
        if (e.type == SDL_QUIT) {
            s_d3d.window_closed = 1;
        } else if (s_sdl_mail && e.type == s_sdl_mail) {
            if (e.user.code >= WIN_MAIL_MESSAGE) {   /* rc_post_message */
                const unsigned msg = (unsigned)(e.user.code - WIN_MAIL_MESSAGE);
                const int took = win_overlay(msg, (unsigned long long)(uintptr_t)e.user.data1,
                                             (long long)(intptr_t)e.user.data2);
                if (msg == WM_CLOSE && !took) s_d3d.window_closed = 1;
            }
            if (e.user.code == WIN_MAIL_CLOSE) break;
            if (e.user.code == WIN_MAIL_BORDERLESS) set_borderless(w, (int)(intptr_t)e.user.data1);
            if (e.user.code == WIN_MAIL_TITLE) {
                char t[sizeof s_win_title];
                AcquireSRWLockExclusive(&s_win_title_lock);
                memcpy(t, s_win_title, sizeof t);
                ReleaseSRWLockExclusive(&s_win_title_lock);
                SDL_SetWindowTitle(w, t);
            }
        } else {
            if (e.type == SDL_WINDOWEVENT) {
                const Uint8 ev = e.window.event;
                if (ev == SDL_WINDOWEVENT_SIZE_CHANGED) win_size_note();
                else if (ev == SDL_WINDOWEVENT_MOVED || ev == SDL_WINDOWEVENT_DISPLAY_CHANGED) win_refresh_note();
            }
            const int alt_enter = e.type == SDL_KEYDOWN && win_alt_enter(&e.key);
            const int took = !alt_enter && win_overlay_event(&e);
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE && !took)
                s_d3d.window_closed = 1;
            if (e.type == SDL_KEYDOWN && !alt_enter && !took) win_key(&e.key);
        }
        if (s_d3d.window_closed && (SDL_GetWindowFlags(w) & SDL_WINDOW_SHOWN)) SDL_HideWindow(w);
    }
    *(volatile int*)&s_d3d.window_closed = 1;
    s_sdl_window = NULL;
    SDL_DestroyWindow(w);
    return 0;
}

static HWND open_window(u32 width, u32 height, const char* title)
{
    window_thread_args a = {width, height, title, CreateEventA(NULL, TRUE, FALSE, NULL)};
    HANDLE t = a.ready ? CreateThread(NULL, 0, window_thread, &a, 0, NULL) : NULL;
    if (!t) {
        if (a.ready) CloseHandle(a.ready);
        return NULL;
    }
    WaitForSingleObject(a.ready, INFINITE);
    CloseHandle(a.ready);
    CloseHandle(t);
    return s_d3d.hwnd;
}

#endif /* _WIN32 */

