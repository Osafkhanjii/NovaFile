// ============================================
// File Pilot Clone - GDI Renderer (Windows)
// Double-buffered drawing: renderer_begin_paint() opens a BeginPaint
// block and draws into an off-screen bitmap, renderer_end_paint() blits
// it to the screen. That gives flicker-free redraws with no OpenGL.
// SAFETY: every draw call is a no-op outside a begin/end paint block,
//         and every GDI object we create is deleted again.
// ============================================
#include "renderer.h"
#include "core.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Theme g_current_theme;

// ---- paint block state ----
static bool g_in_paint = false;
static HWND g_paint_hwnd = NULL;
static HDC g_paint_dc = NULL;
static HDC g_mem_dc = NULL;
static HBITMAP g_mem_bmp = NULL;
static HBITMAP g_old_bmp = NULL;
static PAINTSTRUCT g_ps;
static int g_paint_w = 0;
static int g_paint_h = 0;

// ---- fonts ----
static HFONT g_font = NULL;
static HFONT g_old_font = NULL;

// ---- cached brushes (GDI handles are a limited resource) ----
#define BRUSH_CACHE 64
static COLORREF g_brush_key[BRUSH_CACHE];
static HBRUSH g_brush_val[BRUSH_CACHE];
static int g_brush_count = 0;

static COLORREF to_colorref(Color c) {
    int r = (int)(c.r * 255.0f); if (r < 0) r = 0; if (r > 255) r = 255;
    int g = (int)(c.g * 255.0f); if (g < 0) g = 0; if (g > 255) g = 255;
    int b = (int)(c.b * 255.0f); if (b < 0) b = 0; if (b > 255) b = 255;
    return RGB(r, g, b);
}

// Returns a brush for the colour. *cached tells the caller whether the
// brush belongs to the cache (do not delete it) or is a one-off.
static HBRUSH acquire_brush(COLORREF key, bool* cached) {
    for (int i = 0; i < g_brush_count; i++) {
        if (g_brush_key[i] == key) {
            *cached = true;
            return g_brush_val[i];
        }
    }
    HBRUSH b = CreateSolidBrush(key);
    if (!b) {
        *cached = true;
        return (HBRUSH)GetStockObject(BLACK_BRUSH);
    }
    if (g_brush_count < BRUSH_CACHE) {
        g_brush_key[g_brush_count] = key;
        g_brush_val[g_brush_count] = b;
        g_brush_count++;
        *cached = true;
        return b;
    }
    *cached = false;   // cache full: caller must delete it
    return b;
}

static void free_brush_cache(void) {
    for (int i = 0; i < g_brush_count; i++) {
        if (g_brush_val[i]) DeleteObject(g_brush_val[i]);
        g_brush_val[i] = NULL;
    }
    g_brush_count = 0;
}

// ---- translucent fill state (see rect_fill_alpha below) ---------------
// One grow-only flat 32bpp DIB in a source DC, shared by every fill with
// alpha < 1; g_af_old is the DC's stock bitmap, kept to deselect before
// the DIB is deleted (NULL_BITMAP is not in the MinGW headers).
static HDC g_af_dc = NULL;        // source DC (lives with the renderer)
static HBITMAP g_af_bmp = NULL;   // flat-colour DIB, selected in g_af_dc
static HGDIOBJ g_af_old = NULL;   // stock bitmap that was in g_af_dc
static DWORD* g_af_bits = NULL;
static int g_af_w = 0, g_af_h = 0;
static COLORREF g_af_key = 0;     // colour currently stored in the DIB
static bool g_af_filled = false;

// ---- font cache: one HFONT per pixel size (DPI scaling needs this) ----
#define FONT_CACHE 8
static HFONT g_font_cache[FONT_CACHE];
static int g_font_px[FONT_CACHE];
static int g_font_cache_n = 0;

static HFONT font_for(int px) {
    if (px <= 0) px = 16;
    if (px > 256) px = 256;

    for (int i = 0; i < g_font_cache_n; i++) {
        if (g_font_px[i] == px) return g_font_cache[i];
    }

    HFONT f = CreateFontA(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          "Segoe UI");
    if (!f) return (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    if (g_font_cache_n < FONT_CACHE) {
        g_font_px[g_font_cache_n] = px;
        g_font_cache[g_font_cache_n] = f;
        g_font_cache_n++;
    } else {
        // Cache full: recycle a slot round robin. Fonts are only selected
        // inside a single draw call and deselected again, never live in
        // a DC when we delete them here.
        static int rr = 0;
        int slot = rr % FONT_CACHE;
        rr++;
        if (g_font_cache[slot] && g_font_cache[slot] != g_font)
            DeleteObject(g_font_cache[slot]);
        g_font_px[slot] = px;
        g_font_cache[slot] = f;
    }
    return f;
}

static void free_font_cache(void) {
    for (int i = 0; i < g_font_cache_n; i++) {
        if (g_font_cache[i] && g_font_cache[i] != g_font)
            DeleteObject(g_font_cache[i]);
        g_font_cache[i] = NULL;
    }
    g_font_cache_n = 0;
}

// ---------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------
RendererContext* renderer_init(void* window_handle, int width, int height) {
    RendererContext* ctx = (RendererContext*)mem_zero(sizeof(RendererContext));
    if (!ctx) return NULL;

    ctx->window_handle = window_handle;
    ctx->window_width = width;
    ctx->window_height = height;
    ctx->vsync = true;
    ctx->background_r = 0.10f;
    ctx->background_g = 0.10f;
    ctx->background_b = 0.12f;
    ctx->background_a = 1.0f;

    if (!g_font) {
        g_font = CreateFontA(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             "Segoe UI");
        if (!g_font) {
            g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        }
    }
    return ctx;
}

void renderer_destroy(RendererContext* ctx) {
    if (g_in_paint) renderer_end_paint();
    free_font_cache();
    if (g_font) {
        HFONT stock = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        if (g_font != stock) DeleteObject(g_font);
        g_font = NULL;
    }
    free_brush_cache();
    if (g_af_dc) {                    // translucent-fill source DC
        if (g_af_old) SelectObject(g_af_dc, g_af_old);
        DeleteDC(g_af_dc);
        g_af_dc = NULL;
    }
    if (g_af_bmp) { DeleteObject(g_af_bmp); g_af_bmp = NULL; }
    g_af_old = NULL;
    g_af_bits = NULL;
    g_af_w = g_af_h = 0;
    g_af_filled = false;
    if (ctx) mem_free(ctx);
}

bool renderer_is_painting(void) { return g_in_paint; }

void* renderer_paint_dc(RendererContext* ctx) {
    if (!ctx || !g_in_paint || !g_mem_dc) return NULL;
    return (void*)g_mem_dc;
}

bool renderer_probe_pixel(RendererContext* ctx, int x, int y, Color* out) {
    if (!ctx || !g_in_paint || !g_mem_dc || !out) return false;
    if (x < 0 || y < 0 || x >= g_paint_w || y >= g_paint_h) return false;
    COLORREF c = GetPixel(g_mem_dc, x, y);
    if (c == CLR_INVALID) return false;
    out->r = GetRValue(c) / 255.0f;
    out->g = GetGValue(c) / 255.0f;
    out->b = GetBValue(c) / 255.0f;
    out->a = 1.0f;
    return true;
}

void renderer_begin_paint(void* window_handle) {
    if (g_in_paint) return;   // never nest paint blocks

    HWND hwnd = (HWND)window_handle;
    if (!hwnd) return;

    RECT rc = {0, 0, 0, 0};
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    HDC paint_dc = BeginPaint(hwnd, &g_ps);
    if (!paint_dc) return;

    HDC mem_dc = CreateCompatibleDC(paint_dc);
    HBITMAP bmp = CreateCompatibleBitmap(paint_dc, w, h);
    if (!mem_dc || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (mem_dc) DeleteDC(mem_dc);
        EndPaint(hwnd, &g_ps);
        return;
    }

    g_paint_hwnd = hwnd;
    g_paint_dc = paint_dc;
    g_mem_dc = mem_dc;
    g_mem_bmp = bmp;
    g_paint_w = w;
    g_paint_h = h;
    g_old_bmp = (HBITMAP)SelectObject(g_mem_dc, g_mem_bmp);
    g_old_font = (HFONT)SelectObject(g_mem_dc, g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT));

    SetBkMode(g_mem_dc, TRANSPARENT);

    // Fill the whole surface with the theme background.
    RECT full = {0, 0, w, h};
    bool cached = false;
    HBRUSH bg = acquire_brush(to_colorref((Color)COLOR_BG), &cached);
    FillRect(g_mem_dc, &full, bg);
    if (!cached) DeleteObject(bg);

    g_in_paint = true;
}

void renderer_end_paint(void) {
    if (!g_in_paint) return;

    if (g_mem_dc && g_paint_dc) {
        BitBlt(g_paint_dc, 0, 0, g_paint_w, g_paint_h, g_mem_dc, 0, 0, SRCCOPY);
    }

    if (g_mem_dc) {
        if (g_old_bmp) SelectObject(g_mem_dc, g_old_bmp);
        if (g_old_font) SelectObject(g_mem_dc, g_old_font);
        DeleteDC(g_mem_dc);
    }
    if (g_mem_bmp) DeleteObject(g_mem_bmp);
    if (g_paint_hwnd && g_paint_dc) EndPaint(g_paint_hwnd, &g_ps);

    g_mem_dc = NULL;
    g_mem_bmp = NULL;
    g_old_bmp = NULL;
    g_old_font = NULL;
    g_paint_dc = NULL;
    g_paint_hwnd = NULL;
    g_paint_w = g_paint_h = 0;
    g_in_paint = false;
}

// ---------------------------------------------------------------
// Drawing (all no-ops outside a paint block)
// ---------------------------------------------------------------
void renderer_clear(RendererContext* ctx) {
    (void)ctx;
    // The background is already filled in renderer_begin_paint().
}

void renderer_present(RendererContext* ctx) {
    (void)ctx;
    // Painting ends in renderer_end_paint().
}

// ---- translucent solid fill ------------------------------------------
// to_colorref() drops the alpha and GDI's FillRect can only paint
// opaque, so every "translucent" fill landed as a SOLID BLACK rectangle
// - the reported "the whole background turns black when Settings opens"
// (the modal backdrops, the menu shadows and the cut-item fade were all
// affected). AlphaBlend applies the opacity through SourceConstantAlpha
// while a cached, grow-only, flat 32bpp DIB carries the colour. Only
// used when a < 1: the opaque path is bit-for-bit unchanged.
// (The g_af_* state lives with the caches at the top of the file, above
// renderer_destroy, which releases it.)
static void rect_fill_alpha(const RECT* r, Color color) {
    int w = r->right - r->left, h = r->bottom - r->top;
    if (w <= 0 || h <= 0) return;
    int sa = (int)(color.a * 255.0f + 0.5f);
    if (sa <= 0) return;                       // fully transparent

    if (!g_af_dc) g_af_dc = CreateCompatibleDC(NULL);
    if (!g_af_dc) return;

    // grow-only cache: never shrink, so a full-window backdrop and the
    // small menu shadows share one DIB without thrashing
    if (!g_af_bmp || g_af_w < w || g_af_h < h) {
        int nw = w, nh = h;
        if (nw < g_af_w) nw = g_af_w;
        if (nh < g_af_h) nh = g_af_h;
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = nw;
        bi.bmiHeader.biHeight = -nh;           // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = NULL;
        HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits,
                                       NULL, 0);
        if (!bmp || !bits) {
            if (bmp) DeleteObject(bmp);
            return;                            // no dim rather than a leak
        }
        if (g_af_bmp) {                        // deselect before deleting
            SelectObject(g_af_dc, g_af_old);
            DeleteObject(g_af_bmp);
        }
        g_af_bmp = bmp;
        g_af_bits = (DWORD*)bits;
        g_af_old = SelectObject(g_af_dc, g_af_bmp);  // keep it selected
        g_af_w = nw;
        g_af_h = nh;
        g_af_filled = false;
    }

    // flat colour into the DIB, once per colour: AlphaFormat = 0 makes
    // AlphaBlend ignore the pixel alpha, SourceConstantAlpha carries it
    COLORREF flat = to_colorref(color);
    if (!g_af_filled || flat != g_af_key) {
        DWORD px = (DWORD)flat;
        int n = g_af_w * g_af_h;
        for (int i = 0; i < n; i++) g_af_bits[i] = px;
        g_af_key = flat;
        g_af_filled = true;
    }

    BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)sa, 0};
    AlphaBlend(g_mem_dc, r->left, r->top, w, h, g_af_dc, 0, 0, w, h, bf);
}

void renderer_draw_rect_fill(RendererContext* ctx, Rect rect, Color color) {
    if (!ctx || !g_in_paint || !g_mem_dc) return;

    RECT r;
    r.left = (LONG)rect.x;
    r.top = (LONG)rect.y;
    r.right = (LONG)(rect.x + rect.width);
    r.bottom = (LONG)(rect.y + rect.height);
    if (r.right < r.left) { LONG t = r.left; r.left = r.right; r.right = t; }
    if (r.bottom < r.top) { LONG t = r.top; r.top = r.bottom; r.bottom = t; }

    if (color.a <= 0.0f) return;              // nothing to paint
    if (color.a < 0.999f) {                   // translucent: blend, not fill
        rect_fill_alpha(&r, color);
        return;
    }

    bool cached = false;
    HBRUSH b = acquire_brush(to_colorref(color), &cached);
    FillRect(g_mem_dc, &r, b);
    if (!cached) DeleteObject(b);
}

void renderer_draw_round_rect(RendererContext* ctx, Rect rect, float radius,
                              Color fill, Color stroke, float stroke_width) {
    if (!ctx || !g_in_paint || !g_mem_dc) return;

    int x = (int)rect.x, y = (int)rect.y;
    int w = (int)rect.width, h = (int)rect.height;
    if (w <= 0 || h <= 0) return;

    int rr = (int)(radius + 0.5f);
    if (rr < 0) rr = 0;
    int ew = rr * 2;
    if (ew > w) ew = w;
    if (ew > h) ew = h;

    bool cached = false;
    HBRUSH brush = acquire_brush(to_colorref(fill), &cached);

    int pw = (int)(stroke_width + 0.5f);
    HPEN pen = NULL;
    if (pw >= 1) pen = CreatePen(PS_SOLID, pw, to_colorref(stroke));
    HGDIOBJ old_pen = SelectObject(g_mem_dc, pen ? (HGDIOBJ)pen
                                                 : GetStockObject(NULL_PEN));
    HGDIOBJ old_brush = SelectObject(g_mem_dc, brush);

    RoundRect(g_mem_dc, x, y, x + w, y + h, ew, ew);

    SelectObject(g_mem_dc, old_brush);
    if (old_pen) SelectObject(g_mem_dc, old_pen);
    if (pen) DeleteObject(pen);
    if (!cached) DeleteObject(brush);
}

void renderer_draw_line(RendererContext* ctx, float x1, float y1, float x2, float y2,
                        Color color, float thickness) {
    if (!ctx || !g_in_paint || !g_mem_dc) return;

    int w = (int)thickness;
    if (w < 1) w = 1;

    HPEN pen = CreatePen(PS_SOLID, w, to_colorref(color));
    if (!pen) return;
    HGDIOBJ old_pen = SelectObject(g_mem_dc, pen);

    MoveToEx(g_mem_dc, (int)x1, (int)y1, NULL);
    LineTo(g_mem_dc, (int)x2, (int)y2);

    SelectObject(g_mem_dc, old_pen);
    DeleteObject(pen);
}

void renderer_draw_text(RendererContext* ctx, const char* text, float x, float y,
                        float font_size, Color color) {
    if (!ctx || !g_mem_dc || !text || !text[0]) return;
    if (!g_in_paint) return;

    // Pick the font that matches the requested (DPI scaled) size, draw,
    // then put the previous font back so DC state stays clean.
    HFONT use = font_for((int)(font_size + 0.5f));
    HFONT old = (HFONT)SelectObject(g_mem_dc, use);

    SetTextColor(g_mem_dc, to_colorref(color));
    SetBkMode(g_mem_dc, TRANSPARENT);
    TextOutA(g_mem_dc, (int)x, (int)y, text, (int)strlen(text));

    if (old) SelectObject(g_mem_dc, old);
}

float renderer_text_width(RendererContext* ctx, const char* text, float font_size) {
    if (!ctx || !text) return 0.0f;
    size_t len = strlen(text);
    if (g_in_paint && g_mem_dc && len > 0) {
        HFONT use = font_for((int)(font_size + 0.5f));
        HFONT old = (HFONT)SelectObject(g_mem_dc, use);

        SIZE sz = {0, 0};
        GetTextExtentPoint32A(g_mem_dc, text, (int)len, &sz);

        if (old) SelectObject(g_mem_dc, old);
        return (float)sz.cx;
    }
    return (float)(len * 8);   // safe fallback outside a paint block
}

void renderer_set_scissor(RendererContext* ctx, Rect clip) {
    if (!ctx || !g_in_paint || !g_mem_dc) return;
    RECT r;
    r.left = (LONG)clip.x;
    r.top = (LONG)clip.y;
    r.right = (LONG)(clip.x + clip.width);
    r.bottom = (LONG)(clip.y + clip.height);
    // INTERSECT, never Exclude: ExcludeClipRect would hide everything
    // INSIDE the rect (that is what made the sidebar rows disappear).
    IntersectClipRect(g_mem_dc, r.left, r.top, r.right, r.bottom);
}

void renderer_reset_scissor(RendererContext* ctx) {
    if (!ctx || !g_in_paint || !g_mem_dc) return;
    SelectClipRgn(g_mem_dc, NULL);
}

void renderer_begin_batch(RendererContext* ctx) { (void)ctx; }
void renderer_end_batch(RendererContext* ctx) { (void)ctx; }

// ---------------------------------------------------------------
// Theme
// ---------------------------------------------------------------
Theme* theme_create_dark(void) {
    Theme* t = (Theme*)mem_zero(sizeof(Theme));
    if (!t) return NULL;

    t->bg_primary = COLOR_DARK_GRAY;
    t->bg_secondary = COLOR_PANEL;
    t->bg_panel = COLOR_PANEL;
    t->bg_input = (Color){0.2f, 0.2f, 0.22f, 1.0f};
    t->text_primary = COLOR_TEXT;
    t->text_secondary = COLOR_LIGHT_GRAY;
    t->accent = COLOR_ACCENT;
    t->border = COLOR_BORDER;
    t->selected = COLOR_SELECTED;
    t->hover = (Color){0.2f, 0.25f, 0.3f, 1.0f};
    t->active = COLOR_ACCENT;
    t->border_radius = 4.0f;
    t->padding = 8.0f;
    g_current_theme = *t;
    return t;
}

Theme* theme_create_light(void) {
    Theme* t = (Theme*)mem_zero(sizeof(Theme));
    if (!t) return NULL;

    t->bg_primary = (Color){0.95f, 0.95f, 0.96f, 1.0f};
    t->bg_secondary = (Color){0.9f, 0.9f, 0.92f, 1.0f};
    t->bg_panel = (Color){0.92f, 0.92f, 0.94f, 1.0f};
    t->bg_input = (Color){1.0f, 1.0f, 1.0f, 1.0f};
    t->text_primary = (Color){0.15f, 0.15f, 0.15f, 1.0f};
    t->text_secondary = (Color){0.4f, 0.4f, 0.4f, 1.0f};
    t->accent = COLOR_ACCENT;
    t->border = (Color){0.8f, 0.8f, 0.85f, 1.0f};
    t->selected = COLOR_SELECTED;
    t->hover = (Color){0.85f, 0.9f, 0.95f, 1.0f};
    t->active = COLOR_ACCENT;
    t->border_radius = 4.0f;
    t->padding = 8.0f;
    return t;
}

void theme_apply(Theme* theme, RendererContext* ctx) {
    if (!theme || !ctx) return;
    ctx->background_r = theme->bg_primary.r;
    ctx->background_g = theme->bg_primary.g;
    ctx->background_b = theme->bg_primary.b;
    ctx->background_a = theme->bg_primary.a;
}
