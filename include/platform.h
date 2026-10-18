// ============================================
// File Pilot Clone - Platform Header
// ============================================

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void* handle;
    int width;
    int height;
    int x;
    int y;
    bool focused;
    bool visible;
    const char* title;
} Window;

typedef struct {
    int x, y;
    int delta_x, delta_y;
    bool left_click;      // true ONLY on the frame the button went down
    bool left_down;       // true while the button is HELD (slider dragging)
    bool right_click;     // true ONLY on the frame the button went down
    bool middle_click;    // true ONLY on the frame the button went down
    bool double_click;    // true ONLY on the frame of a double click
    bool ctrl_pressed;
    bool shift_pressed;
    bool alt_pressed;     // Alt held: arms the corner Back/Forward gesture
    bool win_pressed;     // Windows key held (recorder display + Win+key bindings)
    int wheel;            // scroll notches this frame (+up / -down), 0 = none
    unsigned int char_pressed; // printable character typed THIS frame (0 = none)
    char key_text[16];
    int key_code;         // virtual key pressed THIS frame, 0 = none
    // Auto-REPEATED virtual key this frame, 0 = none.  The OS keyboard
    // repeat generates fresh WM_KEYDOWNs whose "previous key state" bit
    // is set; those never touch key_code (a HELD toggle must fire once),
    // but a focused text field turns them into continuous Backspace,
    // Delete and caret movement at the natural repeat rate.
    int key_repeat;
    // Key RELEASED this frame, 0 = none.  The press edge alone cannot say
    // "the key is still held": the radial Quick Access menu needs both
    // edges of the C key (press = open, hold = edit mode, release = leave
    // edit mode) and auto-repeat is filtered out on purpose, so this is
    // the only place a release can be observed.
    int key_up;
} InputState;

typedef struct {
    char** paths;
    size_t count;
} DirectoryList;

typedef struct {
    char name[256];
    char path[1024];
    uint64_t size;
    bool is_directory;
    uint64_t modified;
    uint32_t attributes;     // FILE_ATTRIBUTE_* (hidden / system filtering)
    uint16_t depth;          // tree view indent level (0 = top level row)
    int icon_idx;            // memo: icon cache slot this resolved to (-1 = none)
    int tbmp_idx;            // memo: thumbnail cache slot (-1 = none)
    int ct_tbmp_idx;         // memo: custom folder PREVIEW slot (-1 = none)
} DirEntry;

typedef struct {
    Window* (*create_window)(const char* title, int width, int height);
    void (*destroy_window)(Window* win);
    void (*resize_window)(Window* win, int width, int height);
    void (*set_title)(Window* win, const char* title);
    void (*show_window)(Window* win);
    void (*close_window)(Window* win);
    
    InputState (*poll_input)(void);
    bool (*is_key_pressed)(int key_code);
    
    DirectoryList (*list_directory)(const char* path);
    DirEntry* (*get_directory_entries)(const char* path, size_t* count);
    char* (*read_file)(const char* path, size_t* size);
    bool (*write_file)(const char* path, const void* data, size_t size);
    bool (*create_directory)(const char* path);
    bool (*delete_file)(const char* path);
    bool (*copy_file)(const char* src, const char* dst);
    uint64_t (*get_file_size)(const char* path);
    
    void* (*create_thread)(void* (*func)(void*), void* arg);
    void (*join_thread)(void* thread);
    void (*sleep)(int milliseconds);
    
    void* (*malloc)(size_t size);
    void (*free)(void* ptr);
    void* (*realloc)(void* ptr, size_t size);
} PlatformAPI;

PlatformAPI* platform_init(void);
void platform_shutdown(PlatformAPI* api);
void platform_set_fs_funcs(PlatformAPI* api);
const char* platform_get_name(void);
const char* platform_get_path_separator(void);
const char* platform_get_home_directory(void);

// ---- DPI / 4K ----
// Returns the display scale (1.0 = 96 DPI, 2.0 = 192 DPI / 4K).
// platform_init() must run first: it makes the process DPI aware so
// every pixel value we get from Windows is a real physical pixel.
float platform_dpi_scale(void);

// ---- Window / event API (implemented by the platform layer) ----
bool platform_window_create(const char* title, int width, int height);
void platform_window_set_title(const char* title);
void platform_window_size(int* width, int* height);
bool platform_poll_events(void);                  // false => quit requested
InputState* platform_get_input(void);
void* platform_window_handle(void);               // HWND on Windows
void platform_set_paint_callback(void (*cb)(void));
void platform_invalidate(void);                   // request a repaint

// ---- Custom title bar (the client area IS the whole window) -------------
// There is no OS caption: WM_NCHITTEST asks the app what a client point
// IS, and the platform maps the answer onto the invisible resize border
// and the title-bar drag region:
//   1  = interactive chrome (caption buttons, tabs, ...) - a plain client
//        point that must NEVER become the resize border
//  -1  = the empty top strip - title-bar drag area (HTCAPTION; while a
//        resize border is available, the border wins, so the top edge of
//        a normal window stays resizable)
//   0  = no opinion - platform decides (resize border, else plain client)
void platform_set_drag_hit(int (*cb)(int x, int y));
void platform_window_minimize(void);
void platform_window_maximize_toggle(void);       // maximize <-> restore
bool platform_window_maximized(void);             // caption glyph state

// ---- Test-run parking (NOVA_OFFSCREEN=1) -------------------------------
// The background verification harness must never put a window on your
// screen. It used to do that by moving you to a spare virtual desktop for
// the whole run - which took your screen over and let your own clicks land
// in the test. Instead, in this mode every window the app creates (its own
// main window and the system file picker it opens) is moved ENTIRELY past
// the last monitor before it is first shown: nothing is ever visible, the
// geometry and posted input are untouched, and not a single input-injection
// call is made anywhere. Purely a test seam - normal runs never set the
// variable, and these two calls then cost one getenv-style read.
bool platform_offscreen_mode(void);
void platform_park_offscreen(void* hwnd);         // HWND, may be NULL

// ---- Mouse cursor hints (the resize handle must SAY it resizes) ----
// The app asks for a cursor; the Win32 layer applies it from WM_SETCURSOR
// (and immediately when the request changes). NOVA_CURSOR_COL is the
// col-resize arrows shown over a vertical divider's 3-dot handle,
// NOVA_CURSOR_ROW the row-resize arrows over a horizontal one,
// NOVA_CURSOR_HAND the pointing hand over clickable rows (sidebar).
#define NOVA_CURSOR_ARROW 0
#define NOVA_CURSOR_COL   1
#define NOVA_CURSOR_ROW   2
#define NOVA_CURSOR_IBEAM 3   // over an editable text field (search/address)
#define NOVA_CURSOR_HAND  4   // over a clickable item row (sidebar)
void platform_set_cursor(int cursor);             // request (NOVA_CURSOR_*)
int  platform_get_cursor(void);                   // last request (selftest)

#endif
