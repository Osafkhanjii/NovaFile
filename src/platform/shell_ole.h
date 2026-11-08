// shell_ole.h - Windows shell integration (src/platform/shell_ole.c).
// Native context menu ("Show more options") and OLE drag-out.
// Requires OleInitialize() on the calling (UI) thread.
#ifndef NOVA_SHELL_OLE_H
#define NOVA_SHELL_OLE_H

#include <stdbool.h>
#include <stdint.h>

// Results of the native menu.
#define SHELL_MENU_NONE     0   // dismissed
#define SHELL_MENU_INVOKED  1   // a shell verb ran (refresh the folder)
#define SHELL_MENU_RENAME   2   // the user chose "Rename" - app does it inline

// The real Explorer menu for `n` items of ONE folder, at SCREEN (sx, sy).
// extended = Shift held (shows the extended verbs, like Explorer).
int shell_context_menu(void* hwnd, char** paths, int n, int sx, int sy,
                       bool extended);
// The folder BACKGROUND menu (New, Paste, ... plus every registered verb).
int shell_background_menu(void* hwnd, const char* folder, int sx, int sy,
                          bool extended);

// While a native menu is open, owner-drawn submenus ("Open with", "Send
// to") need these messages; the window procedure forwards them here.
bool shell_menu_handle_msg(unsigned msg, uintptr_t wp, intptr_t lp,
                           intptr_t* result);

// Drag the items out of the app (modal until dropped / cancelled).
// Returns 0 when nothing was dropped, else 0x100 | DROPEFFECT_*.
int shell_drag_files(void* hwnd, char** paths, int n);

// ---- the shell menu as DATA (the app draws it in its own style) ----
typedef struct ShellMenu ShellMenu;
typedef struct {
    unsigned id;        // command id (pass to shm_invoke)
    char text[256];     // label, mnemonic '&' removed (active code page)
    char hint[64];      // accelerator text the handler put after a TAB
    char verb[64];      // canonical verb: "open", "cut", "delete", ... or ""
    bool sep, disabled, checked, def, ownerdraw;
    void* sub;          // HMENU of a submenu (NULL = a plain command)
    void* icon;         // HBITMAP, 32bpp premultiplied, owned by the ShellMenu
    int icon_w, icon_h;
} ShmItem;
ShellMenu* shm_for_items(void* hwnd, char** paths, int n, bool extended);
ShellMenu* shm_for_folder(void* hwnd, const char* folder, bool extended);
void* shm_root(ShellMenu* m);
int   shm_count(void* hmenu);
void  shm_prepare_sub(ShellMenu* m, void* hsub, int index);
bool  shm_get(ShellMenu* m, void* hmenu, int index, ShmItem* out, int icon_px);
int   shm_invoke(ShellMenu* m, unsigned id, void* hwnd, int sx, int sy);
unsigned shm_track_native(ShellMenu* m, void* hsub, void* hwnd, int sx, int sy);
void  shm_free(ShellMenu* m);

// ---- background service (the app's right-click uses this) ----
enum { SHM_ITEM = 0, SHM_SUB = 1, SHM_SEP = 2 };
typedef struct {
    char text[128];
    char hint[48];
    char verb[64];
    unsigned id;        // shell command id (SHM_ITEM)
    int kind;           // SHM_ITEM / SHM_SUB / SHM_SEP
    bool disabled, def, filled;   // filled: a submenu's children are known
    int parent;         // node index, -1 = top level
    int first, count;   // children (SHM_SUB, once filled)
    void* icon;         // HBITMAP owned by the service until released
    int icon_w, icon_h;
    void* hsub;         // (service internal)
    int index;          // (service internal)
} ShmNode;
bool shmsvc_start(void* hwnd, void (*wake)(void), int icon_px);
void shmsvc_stop(void);
// Ask for the menu of n items (folder=false) or of a folder background
// (folder=true, paths[0] = the folder).  Returns a request id (0 = failed).
int  shmsvc_request(char** paths, int n, bool folder, bool extended);
int  shmsvc_version(int gen);          // 0 pending, 1 top, 2 complete, -1 failed
int  shmsvc_snapshot(int gen, ShmNode** out, int* n);   // caller frees *out
void shmsvc_invoke(int gen, unsigned id, int sx, int sy);
void shmsvc_release(int gen);

#endif
