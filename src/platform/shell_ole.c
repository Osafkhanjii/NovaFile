// ============================================
// Nova File - Windows shell integration (OLE / COM)
// ============================================
// Two Explorer features that only the shell itself can provide:
//
//   1. "Show more options" - the REAL Windows context menu for the
//      selection (or the folder background): every verb other programs
//      registered (7-Zip, Git, antivirus scans, "Open with", "Send to",
//      "Give access to", ...), exactly as Explorer shows it.
//   2. Drag OUT - dragging the selected items from NovaFile into Explorer,
//      the desktop, an e-mail, a chat window or any other drop target.
//
// Kept in its own file because it needs <shlobj.h>, whose VIEW_* macros
// collide with the app's own view enum in main.c.  Pure C through the
// COBJMACROS vtable helpers; only shell32 / ole32 (already linked).
#define COBJMACROS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell_ole.h"

// ---------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------
// App strings are in the active code page (UTF-8 with the manifest).
static bool to_wide(const char* s, wchar_t* out, int cap) {
    if (!s || !out || cap <= 0) return false;
    return MultiByteToWideChar(CP_ACP, 0, s, -1, out, cap) > 0;
}

static PIDLIST_ABSOLUTE pidl_of(const char* path) {
    wchar_t w[2048];
    if (!to_wide(path, w, 2048)) return NULL;
    PIDLIST_ABSOLUTE pidl = NULL;
    if (FAILED(SHParseDisplayName(w, NULL, &pidl, 0, NULL))) return NULL;
    return pidl;
}

// The parent folder + one child id per path.  Every path must live in the
// SAME folder (a selection always does).  Returns the IShellFolder of the
// parent; *abs receives the absolute pidls (caller frees with free_pidls).
static IShellFolder* items_of(char** paths, int n, PIDLIST_ABSOLUTE* abs,
                              PCUITEMID_CHILD* kids) {
    IShellFolder* parent = NULL;
    for (int i = 0; i < n; i++) {
        abs[i] = pidl_of(paths[i]);
        if (!abs[i]) return parent ? (IShellFolder_Release(parent), NULL)
                                   : NULL;
        IShellFolder* psf = NULL;
        PCUITEMID_CHILD child = NULL;
        if (FAILED(SHBindToParent(abs[i], &IID_IShellFolder, (void**)&psf,
                                  &child))) {
            if (parent) IShellFolder_Release(parent);
            return NULL;
        }
        kids[i] = child;
        if (!parent) parent = psf;
        else IShellFolder_Release(psf);
    }
    return parent;
}

static void free_pidls(PIDLIST_ABSOLUTE* abs, int n) {
    for (int i = 0; i < n; i++)
        if (abs[i]) CoTaskMemFree(abs[i]);
}

// ---------------------------------------------------------------------
// 1. the native context menu
// ---------------------------------------------------------------------
// Owner-drawn parts of the shell menu ("Open with" icons, "Send to") are
// painted by the menu's own IContextMenu2/3 - the window procedure hands
// those messages over through shell_menu_handle_msg() while it is open.
static IContextMenu2* g_cm2 = NULL;
static IContextMenu3* g_cm3 = NULL;

bool shell_menu_handle_msg(unsigned msg, uintptr_t wp, intptr_t lp,
                           intptr_t* result) {
    if (!g_cm2 && !g_cm3) return false;
    switch (msg) {
    case WM_INITMENUPOPUP:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_MENUCHAR: {
        LRESULT r = 0;
        if (g_cm3) {
            if (SUCCEEDED(IContextMenu3_HandleMenuMsg2(g_cm3, msg, (WPARAM)wp,
                                                       (LPARAM)lp, &r))) {
                if (result) *result = (intptr_t)r;
                return true;
            }
        } else if (g_cm2) {
            if (SUCCEEDED(IContextMenu2_HandleMenuMsg(g_cm2, msg, (WPARAM)wp,
                                                      (LPARAM)lp))) {
                if (result) *result = 0;
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

#define SHM_FIRST 1
#define SHM_LAST  0x7FFF

static int run_menu(IContextMenu* cm, void* hwnd_v, int sx, int sy,
                    bool extended) {
    HWND hwnd = (HWND)hwnd_v;
    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;
    UINT flags = CMF_NORMAL | CMF_CANRENAME;
    if (extended) flags |= CMF_EXTENDEDVERBS;     // Shift held: all verbs
    int result = 0;
    if (SUCCEEDED(IContextMenu_QueryContextMenu(cm, menu, 0, SHM_FIRST,
                                                SHM_LAST, flags))) {
        IContextMenu_QueryInterface(cm, &IID_IContextMenu3, (void**)&g_cm3);
        if (!g_cm3)
            IContextMenu_QueryInterface(cm, &IID_IContextMenu2, (void**)&g_cm2);
        UINT cmd = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                          sx, sy, hwnd, NULL);
        if (g_cm3) { IContextMenu3_Release(g_cm3); g_cm3 = NULL; }
        if (g_cm2) { IContextMenu2_Release(g_cm2); g_cm2 = NULL; }
        if (cmd >= SHM_FIRST) {
            // "rename" is the one verb the shell wants the VIEW to do: hand
            // it back so the app opens its own inline rename field.
            char verb[64] = "";
            IContextMenu_GetCommandString(cm, cmd - SHM_FIRST, GCS_VERBA,
                                          NULL, verb, sizeof verb);
            if (_stricmp(verb, "rename") == 0) {
                result = SHELL_MENU_RENAME;
            } else {
                CMINVOKECOMMANDINFOEX ici;
                memset(&ici, 0, sizeof ici);
                ici.cbSize = sizeof ici;
                ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE |
                            CMIC_MASK_ASYNCOK;
                if (GetKeyState(VK_CONTROL) < 0) ici.fMask |= CMIC_MASK_CONTROL_DOWN;
                if (GetKeyState(VK_SHIFT) < 0) ici.fMask |= CMIC_MASK_SHIFT_DOWN;
                ici.hwnd = hwnd;
                ici.lpVerb = MAKEINTRESOURCEA(cmd - SHM_FIRST);
                ici.lpVerbW = MAKEINTRESOURCEW(cmd - SHM_FIRST);
                ici.nShow = SW_SHOWNORMAL;
                ici.ptInvoke.x = sx;
                ici.ptInvoke.y = sy;
                if (SUCCEEDED(IContextMenu_InvokeCommand(
                        cm, (LPCMINVOKECOMMANDINFO)&ici)))
                    result = SHELL_MENU_INVOKED;
            }
        }
    }
    DestroyMenu(menu);
    return result;
}

int shell_context_menu(void* hwnd, char** paths, int n, int sx, int sy,
                       bool extended) {
    if (!paths || n <= 0) return 0;
    if (n > 512) n = 512;
    PIDLIST_ABSOLUTE* abs = (PIDLIST_ABSOLUTE*)calloc((size_t)n, sizeof *abs);
    PCUITEMID_CHILD* kids = (PCUITEMID_CHILD*)calloc((size_t)n, sizeof *kids);
    int r = 0;
    if (abs && kids) {
        IShellFolder* parent = items_of(paths, n, abs, kids);
        if (parent) {
            IContextMenu* cm = NULL;
            if (SUCCEEDED(IShellFolder_GetUIObjectOf(parent, (HWND)hwnd,
                                                     (UINT)n, kids,
                                                     &IID_IContextMenu, NULL,
                                                     (void**)&cm)) && cm) {
                r = run_menu(cm, hwnd, sx, sy, extended);
                IContextMenu_Release(cm);
            }
            IShellFolder_Release(parent);
        }
        free_pidls(abs, n);
    }
    free(abs);
    free(kids);
    return r;
}

int shell_background_menu(void* hwnd, const char* folder, int sx, int sy,
                          bool extended) {
    PIDLIST_ABSOLUTE pidl = pidl_of(folder);
    if (!pidl) return 0;
    int r = 0;
    IShellFolder* desk = NULL;
    if (SUCCEEDED(SHGetDesktopFolder(&desk)) && desk) {
        IShellFolder* fold = NULL;
        HRESULT hr;
        if (ILIsEmpty((PCUIDLIST_RELATIVE)pidl)) {   // the desktop itself
            fold = desk;
            IShellFolder_AddRef(fold);
            hr = S_OK;
        } else {
            hr = IShellFolder_BindToObject(desk, (PCUIDLIST_RELATIVE)pidl,
                                           NULL, &IID_IShellFolder,
                                           (void**)&fold);
        }
        if (SUCCEEDED(hr) && fold) {
            IContextMenu* cm = NULL;
            if (SUCCEEDED(IShellFolder_CreateViewObject(fold, (HWND)hwnd,
                                                        &IID_IContextMenu,
                                                        (void**)&cm)) && cm) {
                r = run_menu(cm, hwnd, sx, sy, extended);
                IContextMenu_Release(cm);
            }
            IShellFolder_Release(fold);
        }
        IShellFolder_Release(desk);
    }
    CoTaskMemFree(pidl);
    return r;
}

// ---------------------------------------------------------------------
// 2. drag OUT (OLE drag source)
// ---------------------------------------------------------------------
// The data object comes from the shell (GetUIObjectOf IID_IDataObject), so
// every target sees exactly what an Explorer drag carries.  Only the
// IDropSource - "keep going / drop / cancel" - is ours.
typedef struct {
    IDropSourceVtbl* lpVtbl;
    LONG ref;
} NovaDropSource;

static HRESULT STDMETHODCALLTYPE ds_qi(IDropSource* This, REFIID riid,
                                       void** out) {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropSource)) {
        *out = This;
        IDropSource_AddRef(This);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ds_addref(IDropSource* This) {
    return (ULONG)InterlockedIncrement(&((NovaDropSource*)This)->ref);
}
static ULONG STDMETHODCALLTYPE ds_release(IDropSource* This) {
    LONG r = InterlockedDecrement(&((NovaDropSource*)This)->ref);
    return (ULONG)(r < 0 ? 0 : r);          // lives on the caller's stack
}
static HRESULT STDMETHODCALLTYPE ds_continue(IDropSource* This, BOOL esc,
                                             DWORD keys) {
    (void)This;
    if (esc) return DRAGDROP_S_CANCEL;
    if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ds_feedback(IDropSource* This, DWORD eff) {
    (void)This;
    (void)eff;
    return DRAGDROP_S_USEDEFAULTCURSORS;
}
static IDropSourceVtbl g_ds_vtbl = {ds_qi, ds_addref, ds_release, ds_continue,
                                    ds_feedback};

int shell_drag_files(void* hwnd, char** paths, int n) {
    if (!paths || n <= 0) return 0;
    if (n > 4096) n = 4096;
    PIDLIST_ABSOLUTE* abs = (PIDLIST_ABSOLUTE*)calloc((size_t)n, sizeof *abs);
    PCUITEMID_CHILD* kids = (PCUITEMID_CHILD*)calloc((size_t)n, sizeof *kids);
    int effect_out = 0;
    if (abs && kids) {
        IShellFolder* parent = items_of(paths, n, abs, kids);
        if (parent) {
            IDataObject* data = NULL;
            if (SUCCEEDED(IShellFolder_GetUIObjectOf(parent, (HWND)hwnd,
                                                     (UINT)n, kids,
                                                     &IID_IDataObject, NULL,
                                                     (void**)&data)) && data) {
                // The drag image Explorer shows (thumbnail + count badge).
                IDragSourceHelper* helper = NULL;
                if (SUCCEEDED(CoCreateInstance(&CLSID_DragDropHelper, NULL,
                                               CLSCTX_INPROC_SERVER,
                                               &IID_IDragSourceHelper,
                                               (void**)&helper)) && helper) {
                    POINT pt;
                    GetCursorPos(&pt);
                    IDragSourceHelper_InitializeFromWindow(helper, NULL, &pt,
                                                           data);
                    IDragSourceHelper_Release(helper);
                }
                NovaDropSource ds = {&g_ds_vtbl, 1};
                DWORD eff = 0;
                HRESULT hr = DoDragDrop(data, (IDropSource*)&ds,
                                        DROPEFFECT_COPY | DROPEFFECT_MOVE |
                                            DROPEFFECT_LINK,
                                        &eff);
                if (hr == DRAGDROP_S_DROP) effect_out = (int)eff | 0x100;
                IDataObject_Release(data);
            }
            IShellFolder_Release(parent);
        }
        free_pidls(abs, n);
    }
    free(abs);
    free(kids);
    return effect_out;
}

// ---------------------------------------------------------------------
// 3. the shell menu as DATA - for the app's own (dark, searchable) menu
// ---------------------------------------------------------------------
// File Pilot style: instead of popping the classic white Win32 menu, the
// app asks the shell to FILL a menu, walks it item by item (text, icon,
// submenus, state) and draws it in its own style.  The command ids stay
// valid while the ShellMenu lives, so a click maps straight back to
// IContextMenu::InvokeCommand - every verb works exactly as in Explorer.
#define SHM_ICONS_MAX 512
struct ShellMenu {
    IContextMenu* cm;
    IContextMenu2* cm2;
    IContextMenu3* cm3;
    HMENU menu;
    HBITMAP icons[SHM_ICONS_MAX];     // owned copies handed out by shm_get
    int icon_n;
};

static ShellMenu* shm_from_cm(IContextMenu* cm, bool extended) {
    ShellMenu* m = (ShellMenu*)calloc(1, sizeof *m);
    if (!m) { IContextMenu_Release(cm); return NULL; }
    m->cm = cm;
    m->menu = CreatePopupMenu();
    UINT flags = CMF_NORMAL | CMF_CANRENAME;
    if (extended) flags |= CMF_EXTENDEDVERBS;
    if (!m->menu ||
        FAILED(IContextMenu_QueryContextMenu(cm, m->menu, 0, SHM_FIRST,
                                             SHM_LAST, flags))) {
        shm_free(m);
        return NULL;
    }
    IContextMenu_QueryInterface(cm, &IID_IContextMenu3, (void**)&m->cm3);
    if (!m->cm3)
        IContextMenu_QueryInterface(cm, &IID_IContextMenu2, (void**)&m->cm2);
    return m;
}

ShellMenu* shm_for_items(void* hwnd, char** paths, int n, bool extended) {
    if (!paths || n <= 0) return NULL;
    if (n > 512) n = 512;
    PIDLIST_ABSOLUTE* abs = (PIDLIST_ABSOLUTE*)calloc((size_t)n, sizeof *abs);
    PCUITEMID_CHILD* kids = (PCUITEMID_CHILD*)calloc((size_t)n, sizeof *kids);
    ShellMenu* m = NULL;
    if (abs && kids) {
        IShellFolder* parent = items_of(paths, n, abs, kids);
        if (parent) {
            IContextMenu* cm = NULL;
            if (SUCCEEDED(IShellFolder_GetUIObjectOf(parent, (HWND)hwnd,
                                                     (UINT)n, kids,
                                                     &IID_IContextMenu, NULL,
                                                     (void**)&cm)) && cm)
                m = shm_from_cm(cm, extended);
            IShellFolder_Release(parent);
        }
        free_pidls(abs, n);
    }
    free(abs);
    free(kids);
    return m;
}

ShellMenu* shm_for_folder(void* hwnd, const char* folder, bool extended) {
    PIDLIST_ABSOLUTE pidl = pidl_of(folder);
    if (!pidl) return NULL;
    ShellMenu* m = NULL;
    IShellFolder* desk = NULL;
    if (SUCCEEDED(SHGetDesktopFolder(&desk)) && desk) {
        IShellFolder* fold = NULL;
        HRESULT hr;
        if (ILIsEmpty((PCUIDLIST_RELATIVE)pidl)) {
            fold = desk;
            IShellFolder_AddRef(fold);
            hr = S_OK;
        } else {
            hr = IShellFolder_BindToObject(desk, (PCUIDLIST_RELATIVE)pidl, NULL,
                                           &IID_IShellFolder, (void**)&fold);
        }
        if (SUCCEEDED(hr) && fold) {
            IContextMenu* cm = NULL;
            if (SUCCEEDED(IShellFolder_CreateViewObject(fold, (HWND)hwnd,
                                                        &IID_IContextMenu,
                                                        (void**)&cm)) && cm)
                m = shm_from_cm(cm, extended);
            IShellFolder_Release(fold);
        }
        IShellFolder_Release(desk);
    }
    CoTaskMemFree(pidl);
    return m;
}

void* shm_root(ShellMenu* m) { return m ? (void*)m->menu : NULL; }

int shm_count(void* hmenu) {
    int n = hmenu ? GetMenuItemCount((HMENU)hmenu) : 0;
    return n < 0 ? 0 : n;
}

static LRESULT shm_msg(ShellMenu* m, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT r = 0;
    if (m->cm3) IContextMenu3_HandleMenuMsg2(m->cm3, msg, wp, lp, &r);
    else if (m->cm2) IContextMenu2_HandleMenuMsg(m->cm2, msg, wp, lp);
    return r;
}

void shm_prepare_sub(ShellMenu* m, void* hsub, int index) {
    // Lazily filled submenus ("Send to", "Open with", some archivers)
    // populate themselves on WM_INITMENUPOPUP - exactly what Windows sends
    // right before a submenu appears.
    if (!m || !hsub) return;
    shm_msg(m, WM_INITMENUPOPUP, (WPARAM)hsub, MAKELPARAM(index, FALSE));
}

static HBITMAP shm_dib(int w, int h, unsigned** bits) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                 // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* p = NULL;
    HBITMAP b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &p, NULL, 0);
    if (bits) *bits = (unsigned*)p;
    return b;
}

// Copy a bitmap the menu owns into a premultiplied 32bpp DIB of our own.
static HBITMAP shm_copy_bitmap(HBITMAP src) {
    BITMAP bm;
    if (!GetObject(src, sizeof bm, &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0)
        return NULL;
    int w = bm.bmWidth, h = bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight;
    if (w > 256 || h > 256) return NULL;
    unsigned* dst = NULL;
    HBITMAP out = shm_dib(w, h, &dst);
    if (!out) return NULL;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(NULL);
    int got = GetDIBits(dc, src, 0, (UINT)h, dst, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    if (got <= 0) { DeleteObject(out); return NULL; }
    // A bitmap without any alpha is opaque art: make it so.
    bool any_a = false;
    for (int i = 0; i < w * h; i++) if (dst[i] >> 24) { any_a = true; break; }
    if (!any_a) for (int i = 0; i < w * h; i++) dst[i] |= 0xFF000000u;
    return out;
}

// HBMMENU_CALLBACK: the handler DRAWS its icon on WM_DRAWITEM.  It is
// rendered twice - over black and over white - and the difference gives
// back the true per-pixel alpha (premultiplied colour = the black pass).
static HBITMAP shm_render_callback(ShellMenu* m, HMENU hm, UINT id,
                                   ULONG_PTR data, int px) {
    if (!m->cm3 && !m->cm2) return NULL;
    MEASUREITEMSTRUCT mis;
    memset(&mis, 0, sizeof mis);
    mis.CtlType = ODT_MENU;
    mis.itemID = id;
    mis.itemData = data;
    shm_msg(m, WM_MEASUREITEM, 0, (LPARAM)&mis);
    int w = px, h = px;
    unsigned *b0 = NULL, *b1 = NULL;
    HBITMAP k = shm_dib(w, h, &b0), wh = shm_dib(w, h, &b1);
    HBITMAP out = NULL;
    if (k && wh) {
        HDC dc = CreateCompatibleDC(NULL);
        for (int pass = 0; pass < 2; pass++) {
            unsigned* bits = pass ? b1 : b0;
            for (int i = 0; i < w * h; i++)
                bits[i] = pass ? 0xFFFFFFFFu : 0xFF000000u;
            HGDIOBJ old = SelectObject(dc, pass ? wh : k);
            DRAWITEMSTRUCT dis;
            memset(&dis, 0, sizeof dis);
            dis.CtlType = ODT_MENU;
            dis.itemID = id;
            dis.itemAction = ODA_DRAWENTIRE;
            dis.hwndItem = (HWND)hm;
            dis.hDC = dc;
            dis.rcItem.right = w;
            dis.rcItem.bottom = h;
            dis.itemData = data;
            shm_msg(m, WM_DRAWITEM, 0, (LPARAM)&dis);
            GdiFlush();
            SelectObject(dc, old);
        }
        DeleteDC(dc);
        unsigned* o = NULL;
        out = shm_dib(w, h, &o);
        bool drew = false;
        if (out) {
            for (int i = 0; i < w * h; i++) {
                unsigned c0 = b0[i], c1 = b1[i];
                int a = 255 - (int)((c1 & 0xFF) - (c0 & 0xFF));
                if (a < 0) a = 0;
                if (a > 255) a = 255;
                if (a) drew = true;
                o[i] = ((unsigned)a << 24) | (c0 & 0x00FFFFFFu);
            }
            if (!drew) { DeleteObject(out); out = NULL; }
        }
    }
    if (k) DeleteObject(k);
    if (wh) DeleteObject(wh);
    return out;
}

static void w_to_app(const wchar_t* w, char* out, int cap) {
    if (!WideCharToMultiByte(CP_ACP, 0, w, -1, out, cap, NULL, NULL)) out[0] = 0;
}

bool shm_get(ShellMenu* m, void* hmenu_v, int index, ShmItem* it, int icon_px) {
    if (!m || !hmenu_v || !it) return false;
    HMENU hm = (HMENU)hmenu_v;
    memset(it, 0, sizeof *it);
    wchar_t text[300];
    text[0] = 0;
    MENUITEMINFOW mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING |
               MIIM_BITMAP | MIIM_DATA;
    mi.dwTypeData = text;
    mi.cch = 299;
    if (!GetMenuItemInfoW(hm, (UINT)index, TRUE, &mi)) return false;
    if (mi.fType & MFT_SEPARATOR) { it->sep = true; return true; }
    it->id = mi.wID;
    it->disabled = (mi.fState & (MFS_DISABLED | MFS_GRAYED)) != 0;
    it->checked = (mi.fState & MFS_CHECKED) != 0;
    it->def = (mi.fState & MFS_DEFAULT) != 0;
    it->sub = mi.hSubMenu;
    it->ownerdraw = (mi.fType & MFT_OWNERDRAW) != 0;
    // label: mnemonics out ("&Open" -> "Open", "&&" -> "&"); the part after
    // a TAB is the accelerator hint the handler wants shown on the right.
    char raw[600];
    w_to_app(text, raw, sizeof raw);
    char* tab = strchr(raw, 9);
    if (tab) {
        *tab = 0;
        snprintf(it->hint, sizeof it->hint, "%s", tab + 1);
    }
    int o = 0;
    for (int i = 0; raw[i] && o < (int)sizeof it->text - 1; i++) {
        if (raw[i] == '&') {
            if (raw[i + 1] == '&') { it->text[o++] = '&'; i++; }
            continue;
        }
        it->text[o++] = raw[i];
    }
    it->text[o] = 0;
    // canonical verb (cut / copy / delete / rename / properties / open...)
    if (!it->sub && mi.wID >= SHM_FIRST && mi.wID <= SHM_LAST) {
        char v[64] = "";
        if (SUCCEEDED(IContextMenu_GetCommandString(m->cm, mi.wID - SHM_FIRST,
                                                    GCS_VERBA, NULL, v,
                                                    sizeof v)))
            snprintf(it->verb, sizeof it->verb, "%s", v);
    }
    // icon
    HBITMAP ib = NULL;
    if (mi.hbmpItem == HBMMENU_CALLBACK)
        ib = shm_render_callback(m, hm, mi.wID, mi.dwItemData,
                                 icon_px > 0 ? icon_px : 16);
    else if (mi.hbmpItem && (ULONG_PTR)mi.hbmpItem > 11)
        ib = shm_copy_bitmap(mi.hbmpItem);
    if (ib) {
        if (m->icon_n < SHM_ICONS_MAX) {
            m->icons[m->icon_n++] = ib;
            BITMAP bm;
            GetObject(ib, sizeof bm, &bm);
            it->icon = ib;
            it->icon_w = bm.bmWidth;
            it->icon_h = bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight;
        } else {
            DeleteObject(ib);
        }
    }
    return true;
}

int shm_invoke(ShellMenu* m, unsigned id, void* hwnd, int sx, int sy) {
    if (!m || id < SHM_FIRST || id > SHM_LAST) return SHELL_MENU_NONE;
    CMINVOKECOMMANDINFOEX ici;
    memset(&ici, 0, sizeof ici);
    ici.cbSize = sizeof ici;
    ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_ASYNCOK;
    if (GetKeyState(VK_CONTROL) < 0) ici.fMask |= CMIC_MASK_CONTROL_DOWN;
    if (GetKeyState(VK_SHIFT) < 0) ici.fMask |= CMIC_MASK_SHIFT_DOWN;
    ici.hwnd = (HWND)hwnd;
    ici.lpVerb = MAKEINTRESOURCEA(id - SHM_FIRST);
    ici.lpVerbW = MAKEINTRESOURCEW(id - SHM_FIRST);
    ici.nShow = SW_SHOWNORMAL;
    ici.ptInvoke.x = sx;
    ici.ptInvoke.y = sy;
    return SUCCEEDED(IContextMenu_InvokeCommand(m->cm,
                                                (LPCMINVOKECOMMANDINFO)&ici))
               ? SHELL_MENU_INVOKED
               : SHELL_MENU_NONE;
}

// Show ONE submenu of the shell menu natively (fallback for owner-drawn
// entries whose text the shell keeps to itself).  Returns the picked id.
unsigned shm_track_native(ShellMenu* m, void* hsub, void* hwnd, int sx, int sy) {
    if (!m || !hsub) return 0;
    g_cm3 = m->cm3;
    g_cm2 = m->cm2;
    UINT cmd = (UINT)TrackPopupMenuEx((HMENU)hsub, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      sx, sy, (HWND)hwnd, NULL);
    g_cm3 = NULL;
    g_cm2 = NULL;
    return cmd;
}

void shm_free(ShellMenu* m) {
    if (!m) return;
    for (int i = 0; i < m->icon_n; i++) if (m->icons[i]) DeleteObject(m->icons[i]);
    if (m->menu) DestroyMenu(m->menu);
    if (m->cm3) IContextMenu3_Release(m->cm3);
    if (m->cm2) IContextMenu2_Release(m->cm2);
    if (m->cm) IContextMenu_Release(m->cm);
    free(m);
}

// ---------------------------------------------------------------------
// 4. the shell-menu SERVICE - a background STA thread
// ---------------------------------------------------------------------
// Asking the shell for a context menu loads every registered extension
// (archivers, cloud clients, editors...): ~1.4 s cold, 50-400 ms warm.
// Doing that on the UI thread froze the window on every right-click, so
// it runs here instead.  The app's menu opens INSTANTLY with its own rows;
// the shell's rows stream in when this thread has them:
//   phase 1 - the top level        (ver 1: shows almost at once)
//   phase 2 - every submenu filled (ver 2: Send to, Open with, 7-Zip...)
// Invoking a command is sent back here too: the IContextMenu belongs to
// this thread's apartment.  Icons stay owned by the ShellMenu until the
// app releases the request (it never draws a released request's nodes).
#define SVC_NODES_MAX 1500
#define WM_SVC_REQ     (WM_APP + 40)
#define WM_SVC_INVOKE  (WM_APP + 41)
#define WM_SVC_RELEASE (WM_APP + 42)
#define WM_SVC_QUIT    (WM_APP + 43)

typedef struct {
    int gen;
    bool folder;
    bool extended;
    char** paths;       // malloc'd copies (freed by the worker)
    int n;
} SvcReq;

typedef struct {
    int gen;
    unsigned id;
    int sx, sy;
} SvcInvoke;

static HANDLE g_svc_thread = NULL;
static DWORD g_svc_tid = 0;
static HANDLE g_svc_ready = NULL;
static void* g_svc_hwnd = NULL;
static void (*g_svc_wake)(void) = NULL;
static CRITICAL_SECTION g_svc_cs;
static volatile LONG g_svc_gen = 0;         // newest request id
static int g_svc_done_gen = 0;              // request the snapshot belongs to
static int g_svc_ver = 0;                   // 0 none, 1 top level, 2 complete
static bool g_svc_failed = false;
static ShmNode* g_svc_nodes = NULL;         // published snapshot
static int g_svc_n = 0;
static int g_svc_icon_px = 16;

// worker-side state
static ShellMenu* g_svc_menu = NULL;
static int g_svc_menu_gen = 0;

static void svc_publish(int gen, ShmNode* nodes, int n, int ver, bool failed) {
    EnterCriticalSection(&g_svc_cs);
    if (gen == (int)g_svc_gen) {
        ShmNode* copy = NULL;
        if (n > 0) {
            copy = (ShmNode*)malloc(sizeof(ShmNode) * (size_t)n);
            if (copy) memcpy(copy, nodes, sizeof(ShmNode) * (size_t)n);
            else n = 0;
        }
        free(g_svc_nodes);
        g_svc_nodes = copy;
        g_svc_n = n;
        g_svc_ver = ver;
        g_svc_done_gen = gen;
        g_svc_failed = failed;
    }
    LeaveCriticalSection(&g_svc_cs);
    if (g_svc_wake) g_svc_wake();
}

// Append the items of one HMENU as children of `parent` (-1 = root).
static int svc_walk(ShmNode* nodes, int n, void* hm, int parent, int depth,
                    bool expand, int gen) {
    int cnt = shm_count(hm);
    int first = n;
    for (int i = 0; i < cnt && n < SVC_NODES_MAX; i++) {
        if (gen != (int)g_svc_gen) return n;          // superseded: stop
        ShmItem it;
        if (!shm_get(g_svc_menu, hm, i, &it, g_svc_icon_px)) continue;
        ShmNode* d = &nodes[n];
        memset(d, 0, sizeof *d);
        d->parent = parent;
        d->first = -1;
        d->kind = it.sep ? SHM_SEP : (it.sub ? SHM_SUB : SHM_ITEM);
        d->id = it.id;
        d->disabled = it.disabled;
        d->def = it.def;
        d->icon = it.icon;
        d->icon_w = it.icon_w;
        d->icon_h = it.icon_h;
        snprintf(d->text, sizeof d->text, "%s", it.text);
        snprintf(d->hint, sizeof d->hint, "%s", it.hint);
        snprintf(d->verb, sizeof d->verb, "%s", it.verb);
        d->hsub = it.sub;
        d->index = i;
        n++;
    }
    int last = n;
    if (parent >= 0) {
        nodes[parent].first = first;
        nodes[parent].count = last - first;
        nodes[parent].filled = true;
    }
    if (expand && depth < 5) {
        for (int k = first; k < last && n < SVC_NODES_MAX; k++) {
            if (nodes[k].kind != SHM_SUB || !nodes[k].hsub) continue;
            shm_prepare_sub(g_svc_menu, nodes[k].hsub, nodes[k].index);
            n = svc_walk(nodes, n, nodes[k].hsub, k, depth + 1, true, gen);
        }
    }
    return n;
}

static void svc_handle_req(SvcReq* r) {
    if (g_svc_menu) { shm_free(g_svc_menu); g_svc_menu = NULL; }
    g_svc_menu_gen = r->gen;
    if (r->gen == (int)g_svc_gen) {
        g_svc_menu = r->folder
                         ? shm_for_folder(g_svc_hwnd, r->paths[0], r->extended)
                         : shm_for_items(g_svc_hwnd, r->paths, r->n, r->extended);
        if (!g_svc_menu) {
            svc_publish(r->gen, NULL, 0, 2, true);
        } else {
            ShmNode* nodes = (ShmNode*)malloc(sizeof(ShmNode) * SVC_NODES_MAX);
            if (nodes) {
                int n = svc_walk(nodes, 0, shm_root(g_svc_menu), -1, 0, false,
                                 r->gen);
                svc_publish(r->gen, nodes, n, 1, false);
                // phase 2: fill every submenu, then republish (same order,
                // so phase-1 indices stay valid for the open panels)
                int top = n;
                for (int k = 0; k < top && n < SVC_NODES_MAX; k++) {
                    if (r->gen != (int)g_svc_gen) break;
                    if (nodes[k].kind != SHM_SUB || !nodes[k].hsub) continue;
                    shm_prepare_sub(g_svc_menu, nodes[k].hsub, nodes[k].index);
                    n = svc_walk(nodes, n, nodes[k].hsub, k, 1, true, r->gen);
                }
                if (r->gen == (int)g_svc_gen) svc_publish(r->gen, nodes, n, 2, false);
                free(nodes);
            } else {
                svc_publish(r->gen, NULL, 0, 2, true);
            }
        }
    }
    for (int i = 0; i < r->n; i++) free(r->paths[i]);
    free(r->paths);
    free(r);
}

static DWORD WINAPI svc_main(LPVOID arg) {
    (void)arg;
    OleInitialize(NULL);
    MSG msg;
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);  // make the queue
    SetEvent(g_svc_ready);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.hwnd == NULL) {
            if (msg.message == WM_SVC_QUIT) break;
            if (msg.message == WM_SVC_REQ) {
                svc_handle_req((SvcReq*)msg.lParam);
                continue;
            }
            if (msg.message == WM_SVC_INVOKE) {
                SvcInvoke* iv = (SvcInvoke*)msg.lParam;
                if (g_svc_menu && iv->gen == g_svc_menu_gen)
                    shm_invoke(g_svc_menu, iv->id, g_svc_hwnd, iv->sx, iv->sy);
                free(iv);
                if (g_svc_wake) g_svc_wake();
                continue;
            }
            if (msg.message == WM_SVC_RELEASE) {
                if (g_svc_menu && (int)msg.wParam == g_svc_menu_gen) {
                    shm_free(g_svc_menu);
                    g_svc_menu = NULL;
                }
                continue;
            }
        }
        TranslateMessage(&msg);       // dialogs a verb opened on this thread
        DispatchMessageW(&msg);
    }
    if (g_svc_menu) { shm_free(g_svc_menu); g_svc_menu = NULL; }
    OleUninitialize();
    return 0;
}

bool shmsvc_start(void* hwnd, void (*wake)(void), int icon_px) {
    if (g_svc_thread) return true;
    g_svc_hwnd = hwnd;
    g_svc_wake = wake;
    g_svc_icon_px = icon_px > 0 ? icon_px : 16;
    InitializeCriticalSection(&g_svc_cs);
    g_svc_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_svc_thread = CreateThread(NULL, 0, svc_main, NULL, 0, &g_svc_tid);
    if (!g_svc_thread) return false;
    WaitForSingleObject(g_svc_ready, 3000);
    return true;
}

void shmsvc_stop(void) {
    if (!g_svc_thread) return;
    InterlockedIncrement(&g_svc_gen);           // any walk in flight stops
    PostThreadMessageW(g_svc_tid, WM_SVC_QUIT, 0, 0);
    if (WaitForSingleObject(g_svc_thread, 2000) != WAIT_OBJECT_0) {
        // a hung shell extension must never keep the app from closing
    }
    CloseHandle(g_svc_thread);
    g_svc_thread = NULL;
    if (g_svc_ready) { CloseHandle(g_svc_ready); g_svc_ready = NULL; }
    free(g_svc_nodes);
    g_svc_nodes = NULL;
    g_svc_n = 0;
}

int shmsvc_request(char** paths, int n, bool folder, bool extended) {
    if (!g_svc_thread || !paths || n <= 0) return 0;
    SvcReq* r = (SvcReq*)calloc(1, sizeof *r);
    if (!r) return 0;
    r->paths = (char**)calloc((size_t)n, sizeof(char*));
    if (!r->paths) { free(r); return 0; }
    for (int i = 0; i < n; i++) {
        r->paths[r->n] = _strdup(paths[i]);
        if (r->paths[r->n]) r->n++;
    }
    r->folder = folder;
    r->extended = extended;
    r->gen = (int)InterlockedIncrement(&g_svc_gen);
    EnterCriticalSection(&g_svc_cs);
    g_svc_ver = 0;
    g_svc_failed = false;
    LeaveCriticalSection(&g_svc_cs);
    if (!PostThreadMessageW(g_svc_tid, WM_SVC_REQ, 0, (LPARAM)r)) {
        for (int i = 0; i < r->n; i++) free(r->paths[i]);
        free(r->paths);
        free(r);
        return 0;
    }
    return r->gen;
}

// Copy the snapshot for `gen` (caller frees *out).  Returns its version:
// 0 = not ready yet, 1 = top level only, 2 = complete; -1 = failed.
int shmsvc_snapshot(int gen, ShmNode** out, int* n) {
    if (out) *out = NULL;
    if (n) *n = 0;
    int ver = 0;
    EnterCriticalSection(&g_svc_cs);
    if (gen != 0 && gen == g_svc_done_gen && g_svc_ver > 0) {
        ver = g_svc_failed ? -1 : g_svc_ver;
        if (!g_svc_failed && g_svc_n > 0 && out) {
            *out = (ShmNode*)malloc(sizeof(ShmNode) * (size_t)g_svc_n);
            if (*out) {
                memcpy(*out, g_svc_nodes, sizeof(ShmNode) * (size_t)g_svc_n);
                if (n) *n = g_svc_n;
            }
        }
    }
    LeaveCriticalSection(&g_svc_cs);
    return ver;
}

int shmsvc_version(int gen) {
    int v = 0;
    EnterCriticalSection(&g_svc_cs);
    if (gen != 0 && gen == g_svc_done_gen) v = g_svc_failed ? -1 : g_svc_ver;
    LeaveCriticalSection(&g_svc_cs);
    return v;
}

void shmsvc_invoke(int gen, unsigned id, int sx, int sy) {
    if (!g_svc_thread) return;
    SvcInvoke* iv = (SvcInvoke*)calloc(1, sizeof *iv);
    if (!iv) return;
    iv->gen = gen;
    iv->id = id;
    iv->sx = sx;
    iv->sy = sy;
    if (!PostThreadMessageW(g_svc_tid, WM_SVC_INVOKE, 0, (LPARAM)iv)) free(iv);
}

void shmsvc_release(int gen) {
    if (!g_svc_thread || gen == 0) return;
    PostThreadMessageW(g_svc_tid, WM_SVC_RELEASE, (WPARAM)gen, 0);
}
