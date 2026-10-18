// ============================================
// File Pilot Clone - Windows Platform Layer
// Real Win32 window, input events and file system helpers.
// SAFETY: read-only file manager. No delete / copy / rename / write
//         operations are exposed to the UI.
// ============================================
#include "platform.h"
#include "core.h"
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------
// Window / input state
// ---------------------------------------------------------------
static HWND g_hwnd = NULL;
static void (*g_paint_callback)(void) = NULL;
static InputState g_input;
static bool g_quit_requested = false;

// Per-frame edge flags (set while dispatching messages, copied to g_input
// at the end of platform_poll_events, then cleared again next frame).
static int g_key_frame = 0;
static int g_key_rep_frame = 0;   // auto-repeated key this frame (0 = none)
static int g_key_up_frame = 0;      // key released this frame (0 = none)
static unsigned int g_char_frame = 0;
static bool g_left_click_frame = false;
static bool g_right_click_frame = false;
static bool g_double_click_frame = false;
static bool g_middle_click_frame = false;   // one frame per wheel-button press
static int g_wheel_raw = 0;

// Held state as OUR QUEUE sees it. The messages below are exactly what a
// real press/release produces, so the OR with GetKeyState() in
// platform_poll_events() leaves human behaviour untouched - while input
// that is POSTED to this window (the background GUI smoke never touches
// your real mouse) drives the same state machine. Without it left_down
// was pure hardware state and a posted press looked like a plain click.
static bool g_lbtn = false;
static bool g_ctrl = false;
static bool g_shift = false;
static bool g_alt = false;
static bool g_win = false;    // Windows key (LWIN/RWIN) held as our queue sees it

// Display scale: 1.0 at 96 DPI, 2.0 at 192 DPI (4K at 200%).
static float g_dpi_scale = 1.0f;

// What a client point IS for WM_NCHITTEST (see platform.h): the app owns
// the caption strip, tabs and window buttons, the platform owns the
// invisible resize border and the drag region.
static int (*g_hit_cb)(int x, int y) = NULL;

// ---- cursor hint (see NOVA_CURSOR_* in platform.h) ------------------
// The app state machine (hover over a 3-dot resize handle / drag) asks
// for a cursor; Windows re-sends WM_SETCURSOR on every mouse move and we
// answer with the last request, so the hint tracks the pointer without
// polling.
static int g_cursor_req = NOVA_CURSOR_ARROW;
static HCURSOR g_cur_arrow = NULL;
static HCURSOR g_cur_col = NULL;   // IDC_SIZEWE - col-resize (vertical divider)
static HCURSOR g_cur_row = NULL;   // IDC_SIZENS - row-resize (horizontal one)
static HCURSOR g_cur_ibeam = NULL; // IDC_IBEAM - editable text field
static HCURSOR g_cur_hand = NULL;  // IDC_HAND - clickable item row (sidebar)

static HCURSOR cursor_of(int c) {
    if (c == NOVA_CURSOR_HAND) {
        if (!g_cur_hand) {
            g_cur_hand = LoadCursor(NULL, IDC_HAND);
            printf("[cur] hand handle=%p\n", (void*)g_cur_hand); fflush(stdout);
        }
        return g_cur_hand;
    }
    if (c == NOVA_CURSOR_COL) {
        if (!g_cur_col) {
            g_cur_col = LoadCursor(NULL, IDC_SIZEWE);
            printf("[cur] col handle=%p\n", (void*)g_cur_col); fflush(stdout);
        }
        return g_cur_col;
    }
    if (c == NOVA_CURSOR_ROW) {
        if (!g_cur_row) {
            g_cur_row = LoadCursor(NULL, IDC_SIZENS);
            printf("[cur] row handle=%p\n", (void*)g_cur_row); fflush(stdout);
        }
        return g_cur_row;
    }
    if (c == NOVA_CURSOR_IBEAM) {
        if (!g_cur_ibeam) {
            g_cur_ibeam = LoadCursor(NULL, IDC_IBEAM);
            printf("[cur] ibeam handle=%p\n", (void*)g_cur_ibeam); fflush(stdout);
        }
        return g_cur_ibeam;
    }
    if (!g_cur_arrow) {
        g_cur_arrow = LoadCursor(NULL, IDC_ARROW);
        printf("[cur] arrow handle=%p\n", (void*)g_cur_arrow); fflush(stdout);
    }
    return g_cur_arrow;
}

void platform_set_cursor(int c) {
    if (c == g_cursor_req) return;   // already showing it
    g_cursor_req = c;
    HCURSOR hc = cursor_of(c);
    // Log the REQUEST, not just the handle: the pointer is shared between
    // virtual desktops, so the OS may not be showing our shape right now -
    // this line is what the app itself decided to display.
    printf("[cur] req=%s handle=%p\n",
           (c == NOVA_CURSOR_COL) ? "col" : (c == NOVA_CURSOR_ROW) ? "row"
           : (c == NOVA_CURSOR_IBEAM) ? "ibeam"
           : (c == NOVA_CURSOR_HAND) ? "hand" : "arrow",
           (void*)hc);
    fflush(stdout);
    SetCursor(hc);          // apply at once: WM_SETCURSOR may lag
}

int platform_get_cursor(void) {
    return g_cursor_req;
}

// Make the process DPI aware BEFORE any window or DC exists, otherwise
// Windows lies to us (virtualised 96 DPI coordinates) on 4K screens.
static void enable_dpi_awareness(void) {
    typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        SetCtxFn set_ctx = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        // (HANDLE)-4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (Win10 1703+)
        if (set_ctx && set_ctx((HANDLE)-4)) {
            g_dpi_scale = 1.0f;   // filled in below from the real DPI
        }
    }
    // Vista+ fallback - looked up dynamically because old MinGW headers
    // do not declare it (guarded by _WIN32_WINNT).
    typedef BOOL (WINAPI *DpiAwareFn)(void);
    DpiAwareFn dpi_aware = (DpiAwareFn)GetProcAddress(user32, "SetProcessDPIAware");
    if (dpi_aware) dpi_aware();

    HDC dc = GetDC(NULL);
    int dpi = 96;
    if (dc) {
        dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
    }
    if (dpi < 96) dpi = 96;
    g_dpi_scale = (float)dpi / 96.0f;
}

float platform_dpi_scale(void) {
    return (g_dpi_scale > 0.5f) ? g_dpi_scale : 1.0f;
}

static void sleep_impl(int milliseconds) {
    if (milliseconds > 0) Sleep((DWORD)milliseconds);
}

// Dark title bar so the window matches the dark UI (Windows 10 2004+
// understands attribute 20, older builds attribute 19). dwmapi is loaded
// dynamically: no import table change, no link flag change.
static void enable_dark_titlebar(HWND hwnd) {
    typedef BOOL (WINAPI *DwmSetAttrFn)(HWND, DWORD, const void*, DWORD);
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    if (!dwm) return;
    DwmSetAttrFn set = (DwmSetAttrFn)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (set) {
        BOOL dark = TRUE;
        // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (2004+), 19 = pre-2004 build
        if (set(hwnd, 20, &dark, (DWORD)sizeof(dark)) != 0)
            set(hwnd, 19, &dark, (DWORD)sizeof(dark));
    }
    FreeLibrary(dwm);
}

// Forward declarations (used by platform_set_fs_funcs above their bodies).
DirectoryList list_directory_impl(const char* path);
DirEntry* get_directory_entries_impl(const char* path, size_t* count);
char* read_file_impl(const char* path, size_t* size);
uint64_t get_file_size_impl(const char* path);

static LRESULT CALLBACK fp_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        // We double-buffer every paint, so the background never needs erasing.
        return 1;

    case WM_PAINT: {
        if (g_paint_callback) {
            g_paint_callback();   // must call BeginPaint/EndPaint itself
            return 0;
        }
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        // Minimum usable size in design pixels, scaled for 4K.
        mmi->ptMinTrackSize.x = (LONG)(640.0f * g_dpi_scale);
        mmi->ptMinTrackSize.y = (LONG)(400.0f * g_dpi_scale);
        return 0;
    }

    case WM_MOUSEMOVE:
        g_input.x = (short)LOWORD(lp);
        g_input.y = (short)HIWORD(lp);
        return 0;

    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        g_input.x = (short)LOWORD(lp);
        g_input.y = (short)HIWORD(lp);
        g_left_click_frame = true;
        g_lbtn = true;
        return 0;

    case WM_LBUTTONDBLCLK:
        // With CS_DBLCLKS this replaces the second WM_LBUTTONDOWN,
        // so report it as a click too - otherwise a double click would
        // never select anything.
        g_input.x = (short)LOWORD(lp);
        g_input.y = (short)HIWORD(lp);
        g_left_click_frame = true;
        g_double_click_frame = true;
        g_lbtn = true;
        return 0;

    case WM_LBUTTONUP:
        g_lbtn = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;

    case WM_CAPTURECHANGED:
        // A capture we do not own any more ends the interaction, so the
        // held flag must never be able to get stuck down.
        if (GetCapture() != hwnd) g_lbtn = false;
        return 0;

    case WM_KILLFOCUS:
        g_lbtn = false;
        g_ctrl = false;
        g_shift = false;
        g_alt = false;          // Alt+Tab / focus loss must not leave the
        g_win = false;          // corner gesture armed (reset in gesture_tick)
        return 0;               // or a Win+key binding held down either

    case WM_MBUTTONUP:
        // NOTE: the frame flag is deliberately NOT cleared here - it is a
        // per-frame edge (see platform_poll_events). Clearing it on UP would
        // swallow clicks whose down+up land in the same message batch.
        return 0;

    case WM_RBUTTONDOWN:
        g_right_click_frame = true;
        return 0;

    case WM_MBUTTONDOWN:
        g_middle_click_frame = true;
        return 0;

    case WM_MOUSEWHEEL:
        g_wheel_raw += GET_WHEEL_DELTA_WPARAM(wp);
        return 0;

    case WM_SETCURSOR:
        // Keep OUR cursor hint on the client area (col/row-resize over a
        // 3-dot handle, arrow otherwise) instead of the class arrow.
        // Non-client areas (borders, title bar) use the default so window
        // resizing keeps its native cursor.
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(cursor_of(g_cursor_req));
            return TRUE;
        }
        return DefWindowProcA(hwnd, msg, wp, lp);

    case WM_KEYDOWN:
        // Bit 30 of lParam = 1 means this is an auto-repeat. Repeats are
        // latched SEPARATELY (g_key_rep_frame -> InputState.key_repeat):
        // the plain key latch stays edge-only so a HELD toggle key can
        // never fire twice, while the focused text field turns the
        // repeat into continuous Backspace/Delete/caret movement at the
        // OS key-repeat rate.
        if ((lp & (1L << 30)) == 0) {
            g_key_frame = (int)wp;
        } else {
            g_key_rep_frame = (int)wp;
        }
        if (wp == VK_CONTROL) g_ctrl = true;
        if (wp == VK_SHIFT) g_shift = true;
        if (wp == VK_MENU) g_alt = true;
        if (wp == VK_LWIN || wp == VK_RWIN) g_win = true;
        return 0;

    case WM_KEYUP:
        // The RELEASE edge, for held-key gestures (Quick Access: hold C).
        // Auto-repeat never produces a KEYUP, so this fires exactly once
        // per real press, just like the press edge above.
        g_key_up_frame = (int)wp;
        if (wp == VK_CONTROL) g_ctrl = false;
        if (wp == VK_SHIFT) g_shift = false;
        if (wp == VK_MENU) g_alt = false;
        if (wp == VK_LWIN || wp == VK_RWIN) g_win = false;
        return 0;

    case WM_SYSKEYDOWN:
        // Alt is a SYSTEM key: Windows sends WM_SYSKEYDOWN (0x104), never
        // WM_KEYDOWN, so the press edge would otherwise be lost.  The
        // message is still forwarded to DefWindowProc for anything that is
        // not Alt itself (Alt+F4, menu shortcuts) so behaviour is unchanged.
        if (wp == VK_MENU) { g_alt = true; return 0; }
        return DefWindowProcA(hwnd, msg, wp, lp);

    case WM_SYSKEYUP:
        // ...and WM_SYSKEYUP (0x105) on release.  Without this case g_alt
        // could never be cleared by the message stream, and the gesture
        // overlay would stay on screen after Alt was let go.
        if (wp == VK_MENU) { g_alt = false; return 0; }
        return DefWindowProcA(hwnd, msg, wp, lp);

    case WM_CHAR:
        // Printable characters only (32..). Control keys such as Enter,
        // Backspace and Esc arrive as VK codes instead.
        if (wp >= 32 && wp < 256) {
            g_char_frame = (unsigned int)wp;
        }
        return 0;

    case WM_DESTROY:
        g_quit_requested = true;
        PostQuitMessage(0);
        return 0;

    // ---- custom title bar: no OS caption, no OS frame -------------------
    case WM_NCCALCSIZE:
        // The client area fills the whole window. Windows sends this once
        // at creation with wp == FALSE (lParam = the window RECT) and on
        // every frame change with wp == TRUE (lParam =
        // NCCALCSIZE_PARAMS); BOTH must leave the rect untouched or the
        // OS frame is silently subtracted again - the creation-time
        // message is then re-confirmed by the SWP_FRAMECHANGED call in
        // platform_window_create.
        // While maximized the proposed rect is the monitor with the
        // invisible resize frame pushed off-screen, so clamp the client
        // to the monitor's work area: the window then stops exactly at
        // the taskbar like a normal captioned window does.
        if (wp == TRUE) {
            NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
            if (IsZoomed(hwnd)) {
                HMONITOR mon = MonitorFromRect(&p->rgrc[0],
                                               MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi;
                mi.cbSize = sizeof(mi);
                if (mon && GetMonitorInfoA(mon, &mi)) p->rgrc[0] = mi.rcWork;
            }
        }
        return 0;                    // rect untouched = client = window

    case WM_NCHITTEST: {
        // Client == window, so first map the screen point into client
        // space (while maximized the window rect overhangs the monitor).
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
        ScreenToClient(hwnd, &pt);
        RECT crc;
        GetClientRect(hwnd, &crc);
        int w = crc.right - crc.left, h = crc.bottom - crc.top;
        // 1) App-owned chrome (caption buttons, tabs, ...) always wins.
        int own = g_hit_cb ? g_hit_cb(pt.x, pt.y) : 0;
        if (own > 0) return HTCLIENT;
        // 2) Invisible resize border (4 design px, DPI-aware) - off while
        //    maximized, like on any other window.
        if (w > 8 && h > 8 && !IsZoomed(hwnd)) {
            int f = (int)(4.0f * g_dpi_scale + 0.5f);
            if (f < 4) f = 4;
            bool l = pt.x < f, r = pt.x >= w - f;
            bool t = pt.y < f, b = pt.y >= h - f;
            if (t && l) return HTTOPLEFT;
            if (t && r) return HTTOPRIGHT;
            if (b && l) return HTBOTTOMLEFT;
            if (b && r) return HTBOTTOMRIGHT;
            if (t) return HTTOP;
            if (b) return HTBOTTOM;
            if (l) return HTLEFT;
            if (r) return HTRIGHT;
        }
        // 3) The empty top strip acts as the title bar: drag to move,
        //    double-click to maximize, right-click for the system menu.
        if (own < 0) return HTCAPTION;
        return HTCLIENT;
    }

    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

// ---- test-run parking (NOVA_OFFSCREEN=1) -------------------------------
// Everything below is a test seam: it exists so a background verification
// run can exercise the REAL window, the real renderer and the real system
// file picker without any of them ever appearing on your screen. It is
// read once per window creation, so a normal run pays a single failed
// GetEnvironmentVariable and nothing else.
bool platform_offscreen_mode(void) {
    char v[8];
    DWORD n = GetEnvironmentVariableA("NOVA_OFFSCREEN", v, sizeof(v));
    return n > 0 && v[0] == '1';
}

// Park a window so that NO monitor can show any of it: just past the right
// edge of the virtual screen (which already spans every display). Placing
// it beyond X rather than at a negative Y means it stays out of sight no
// matter which monitors exist or how they are arranged, and SWP_NOACTIVATE
// means it never takes focus away from whatever you are working in.
void platform_park_offscreen(void* hwnd) {
    HWND w = (HWND)hwnd;
    if (!w) return;
    int x0 = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int cw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    if (cw <= 0) { x0 = 0; cw = GetSystemMetrics(SM_CXSCREEN); }
    SetWindowPos(w, NULL, x0 + cw + 64, 0, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool platform_window_create(const char* title, int width, int height) {
    if (width < 320) width = 320;
    if (height < 240) height = 240;

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = fp_wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    // App icon (resource 1 = NovaFile.ico, embedded by windres). Stays NULL
    // when built without windres, so the window just falls back to the
    // exe default - same as before the icon existed. (WNDCLASSA has no
    // hIconSm; Windows derives the small title-bar icon from hIcon.)
    wc.hIcon = LoadImage(wc.hInstance, MAKEINTRESOURCE(1), IMAGE_ICON,
                         0, 0, LR_DEFAULTSIZE);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "NovaFileWindow";

    if (!RegisterClassA(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    // No AdjustWindowRect: WM_NCCALCSIZE below turns the whole window into
    // the client area (custom title bar), so the requested size IS the
    // client size.
    RECT rc = {0, 0, width, height};

    g_hwnd = CreateWindowExA(
        0, "NovaFileWindow",
        title ? title : "Nova File",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, GetModuleHandleA(NULL), NULL);

    if (!g_hwnd) return false;

    // Force the frame recalculation NOW: it re-sends WM_NCCALCSIZE with
    // wParam TRUE, so the client area becomes the whole window before the
    // first paint (the creation-time message has wParam FALSE).
    SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_FRAMECHANGED);

    enable_dark_titlebar(g_hwnd);   // dark caption, matches the dark UI

    // A verification run never shows this window: it is moved past the
    // last monitor while still hidden (CreateWindowExA did NOT pass
    // WS_VISIBLE), so there is not one frame of it on screen - and it is
    // then shown WITHOUT activation, so your own window keeps the focus.
    bool off = platform_offscreen_mode();
    if (off) platform_park_offscreen(g_hwnd);

    ShowWindow(g_hwnd, off ? SW_SHOWNOACTIVATE : SW_SHOW);
    UpdateWindow(g_hwnd);
    return true;
}

void platform_window_set_title(const char* title) {
    if (g_hwnd && title) SetWindowTextA(g_hwnd, title);
}

// ---- custom title bar (window controls live in the app's tab row) -------
void platform_set_drag_hit(int (*cb)(int x, int y)) {
    g_hit_cb = cb;
}

void platform_window_minimize(void) {
    if (g_hwnd) ShowWindow(g_hwnd, SW_MINIMIZE);
}

void platform_window_maximize_toggle(void) {
    if (!g_hwnd) return;
    ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
}

bool platform_window_maximized(void) {
    return g_hwnd != NULL && IsZoomed(g_hwnd) != 0;
}

void platform_window_size(int* width, int* height) {
    RECT rc = {0, 0, 0, 0};
    if (g_hwnd) GetClientRect(g_hwnd, &rc);
    if (width) *width = (rc.right > 0) ? rc.right : 0;
    if (height) *height = (rc.bottom > 0) ? rc.bottom : 0;
}

void platform_set_paint_callback(void (*cb)(void)) {
    g_paint_callback = cb;
}

void* platform_window_handle(void) {
    return (void*)g_hwnd;
}

void platform_invalidate(void) {
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

InputState* platform_get_input(void) {
    return &g_input;
}

bool platform_poll_events(void) {
    if (g_quit_requested) return false;

    // Clear this frame's edge state before dispatching new messages.
    g_key_frame = 0;
    g_key_rep_frame = 0;
    g_key_up_frame = 0;
    g_char_frame = 0;
    g_left_click_frame = false;
    g_right_click_frame = false;
    g_double_click_frame = false;
    g_middle_click_frame = false;
    g_wheel_raw = 0;

    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            g_quit_requested = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    // Publish the frame's state for the app to read.
    g_input.key_code = g_key_frame;
    g_input.key_repeat = g_key_rep_frame;
    g_input.key_up = g_key_up_frame;
    g_input.char_pressed = g_char_frame;
    g_input.left_click = g_left_click_frame;
    // held: OUR queue's view OR the hardware state - identical for a human
    // (their press generates these messages), and it also works when the
    // press was POSTED to this window instead of being physically made.
    g_input.left_down = g_lbtn || ((GetKeyState(VK_LBUTTON) & 0x8000) != 0);
    g_input.right_click = g_right_click_frame;
    g_input.double_click = g_double_click_frame;
    g_input.middle_click = g_middle_click_frame;   // true for ONE frame only

    g_input.wheel = g_wheel_raw / WHEEL_DELTA;
    if (g_wheel_raw > 0 && g_input.wheel == 0) g_input.wheel = 1;
    if (g_wheel_raw < 0 && g_input.wheel == 0) g_input.wheel = -1;

    g_input.ctrl_pressed = g_ctrl || ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
    g_input.shift_pressed = g_shift || ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
    // GetAsyncKeyState, not GetKeyState: the latter reports the state as of
    // the last message pulled from OUR queue, which goes stale while a menu
    // is tracking keys (Alt opens one) - that is exactly how the gesture
    // overlay could stay visible with Alt already released.  The async
    // call reads the hardware state directly, every frame.
    g_input.alt_pressed = g_alt || ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
    // The Windows key: our queue's edge (WM_KEYDOWN for VK_LWIN/VK_RWIN)
    // OR the hardware state.  Windows itself eats many Win+letter
    // combinations before any app sees them - the shortcut recorder
    // explains that instead of pretending they were captured.
    g_input.win_pressed = g_win ||
                          ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
                          ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);

    return !g_quit_requested;
}

// ---------------------------------------------------------------
// PlatformAPI
// ---------------------------------------------------------------
PlatformAPI* platform_init(void) {
    // Must happen before the first window/DC is created.
    enable_dpi_awareness();

    PlatformAPI* api = (PlatformAPI*)mem_zero(sizeof(PlatformAPI));
    if (!api) return NULL;

    api->list_directory = NULL;
    api->get_directory_entries = NULL;
    api->read_file = NULL;
    api->write_file = NULL;      // disabled on purpose (read-only app)
    api->create_directory = NULL;
    api->delete_file = NULL;     // disabled on purpose (read-only app)
    api->copy_file = NULL;
    api->get_file_size = NULL;

    api->create_thread = NULL;
    api->join_thread = NULL;
    api->sleep = sleep_impl;

    api->malloc = malloc;
    api->free = free;
    api->realloc = realloc;

    return api;
}

void platform_shutdown(PlatformAPI* api) {
    if (api) mem_free(api);
}

const char* platform_get_name(void) { return "Windows"; }
const char* platform_get_path_separator(void) { return "\\"; }

const char* platform_get_home_directory(void) {
    static char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, path)) && path[0])
        return path;
    return "C:\\Users\\Public";
}

// ---------------------------------------------------------------
// Directory listing (read-only)
// ---------------------------------------------------------------
// Parent + child join that never doubles the separator: "D:\" + "Cache"
// must become "D:\Cache", NOT "D:\\Cache". The shell (thumbnail factory
// and SHGetFileInfo) refuses doubled separators, so every folder lost its
// real icon and fell back to the drawn placeholder.
static const char* path_sep(const char* dir) {
    size_t n = strlen(dir);
    return (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/')) ? "" : "\\";
}

DirectoryList list_directory_impl(const char* path) {
    DirectoryList list = {0};
    if (!path) return list;

    char search[2048];
    snprintf(search, sizeof(search), "%s%s*", path, path_sep(path));

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return list;

    size_t cap = 64, count = 0;
    char** paths = (char**)mem_zero(cap * sizeof(char*));
    if (!paths) {
        FindClose(h);
        return list;
    }

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        if (count >= cap) {
            size_t new_cap = cap * 2;
            char** grown = (char**)realloc(paths, new_cap * sizeof(char*));
            if (!grown) break;
            paths = grown;
            cap = new_cap;
        }
        char full[2048];
        snprintf(full, sizeof(full), "%s%s%s", path, path_sep(path),
                 fd.cFileName);
        paths[count] = _strdup(full);
        if (!paths[count]) break;
        count++;
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    list.paths = paths;
    list.count = count;
    return list;
}

DirEntry* get_directory_entries_impl(const char* path, size_t* count) {
    if (!count) return NULL;
    *count = 0;
    if (!path || !path[0]) return NULL;

    char search[2048];
    snprintf(search, sizeof(search), "%s%s*", path, path_sep(path));

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    size_t cap = 256;
    DirEntry* entries = (DirEntry*)mem_zero(cap * sizeof(DirEntry));
    if (!entries) {
        FindClose(h);
        return NULL;
    }
    size_t ec = 0;

    // The parent may already end in a separator ("D:\"): adding another one
    // produced "D:\\Cache", and the shell (SHCreateItemFromParsingName /
    // SHGetFileInfo) REFUSES such paths - every folder then fell back to the
    // drawn placeholder (the "flat yellow folders" report).
    const char* sep = path_sep(path);

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        if (ec >= cap) {
            size_t new_cap = cap * 2;
            DirEntry* grown = (DirEntry*)realloc(entries, new_cap * sizeof(DirEntry));
            if (!grown) break;
            // Zero the new half so unused entries stay clean.
            memset(grown + cap, 0, (new_cap - cap) * sizeof(DirEntry));
            entries = grown;
            cap = new_cap;
        }

        DirEntry* e = &entries[ec];
        strncpy(e->name, fd.cFileName, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';

        snprintf(e->path, sizeof(e->path), "%s%s%s", path, sep, e->name);

        e->size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e->is_directory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e->attributes = fd.dwFileAttributes;   // HIDDEN / SYSTEM filtering
        e->modified = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
                      fd.ftLastWriteTime.dwLowDateTime;
        ec++;
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    *count = ec;
    return entries;
}

// Public function pointers - assigned at runtime
DirectoryList (*platform_list_dir)(const char*) = NULL;
DirEntry* (*platform_get_entries)(const char*, size_t*) = NULL;
char* (*platform_read_file)(const char*, size_t*) = NULL;
bool (*platform_write_file)(const char*, const void*, size_t) = NULL;

void platform_set_fs_funcs(PlatformAPI* api) {
    if (!api) return;
    api->list_directory = list_directory_impl;
    api->get_directory_entries = get_directory_entries_impl;
    api->read_file = read_file_impl;
    api->get_file_size = get_file_size_impl;
    // write / delete / copy / create intentionally left NULL (read-only).
}

// ---------------------------------------------------------------
// Read-only file helpers
// ---------------------------------------------------------------
char* read_file_impl(const char* path, size_t* size) {
    if (size) *size = 0;
    if (!path) return NULL;

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart > (LONGLONG)(64 * 1024 * 1024)) {
        // Refuse missing sizes and anything above 64 MB.
        CloseHandle(h);
        return NULL;
    }

    size_t fs = (size_t)li.QuadPart;
    char* buf = (char*)malloc(fs + 1);
    if (!buf) {
        CloseHandle(h);
        return NULL;
    }

    DWORD to_read = (DWORD)fs, got = 0;
    BOOL ok = TRUE;
    if (to_read > 0) ok = ReadFile(h, buf, to_read, &got, NULL);
    CloseHandle(h);

    if (!ok) {
        free(buf);
        return NULL;
    }

    buf[got] = '\0';
    if (size) *size = got;
    return buf;
}

uint64_t get_file_size_impl(const char* path) {
    if (!path) return 0;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &data))
        return ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    return 0;
}
