// ============================================
// File Pilot Clone - Renderer Header
// ============================================

#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    float r, g, b, a;
} Color;

typedef struct {
    float x, y;
    float width, height;
} Rect;

typedef enum {
    RENDER_QUAD,
    RENDER_TEXT,
    RENDER_LINE,
    RENDER_RECT_FILL
} RenderType;

typedef struct {
    unsigned int id;
    int width;
    int height;
    bool loaded;
} Texture;

typedef struct {
    unsigned int program_id;
    unsigned int vertex_shader;
    unsigned int fragment_shader;
    int uniform_model;
    int uniform_view;
    int uniform_projection;
    int uniform_color;
} ShaderProgram;

typedef struct {
    void* window_handle;
    unsigned int vao;
    unsigned int vbo;
    unsigned int ebo;
    ShaderProgram shader;
    int window_width;
    int window_height;
    bool vsync;
    float background_r, background_g, background_b, background_a;
} RendererContext;

typedef struct {
    RenderType type;
    Rect rect;
    Color color;
    const char* text;
    Texture* texture;
    float thickness;
} DrawCommand;

// Colors (C99 compound literals so they work as expressions AND initializers)
#define COLOR_WHITE      ((Color){1.0f, 1.0f, 1.0f, 1.0f})
#define COLOR_BLACK      ((Color){0.0f, 0.0f, 0.0f, 1.0f})
#define COLOR_DARK_GRAY  ((Color){0.15f, 0.15f, 0.15f, 1.0f})
#define COLOR_MED_GRAY   ((Color){0.3f, 0.3f, 0.3f, 1.0f})
#define COLOR_LIGHT_GRAY ((Color){0.5f, 0.5f, 0.5f, 1.0f})
#define COLOR_ACCENT     ((Color){0.0f, 0.6f, 0.9f, 1.0f})
#define COLOR_SELECTED   ((Color){0.0f, 0.5f, 0.8f, 0.8f})
#define COLOR_BG         ((Color){0.1f, 0.1f, 0.12f, 1.0f})
#define COLOR_PANEL      ((Color){0.15f, 0.15f, 0.18f, 1.0f})
#define COLOR_TEXT       ((Color){0.9f, 0.9f, 0.9f, 1.0f})
#define COLOR_BORDER     ((Color){0.3f, 0.3f, 0.35f, 1.0f})
#define COLOR_SUCCESS    ((Color){0.2f, 0.7f, 0.3f, 1.0f})
#define COLOR_WARNING    ((Color){0.9f, 0.6f, 0.1f, 1.0f})
#define COLOR_ERROR      ((Color){0.9f, 0.2f, 0.2f, 1.0f})

RendererContext* renderer_init(void* window_handle, int width, int height);
void renderer_destroy(RendererContext* ctx);

// Painting: call begin at the start of a frame, end when all drawing is done.
// All draw_* functions only work between begin and end.
void renderer_begin_paint(void* window_handle);
void renderer_end_paint(void);
bool renderer_is_painting(void);
void* renderer_paint_dc(RendererContext* ctx);   // current draw DC, NULL outside paint

void renderer_clear(RendererContext* ctx);
void renderer_present(RendererContext* ctx);
// TEST SEAM: read one pixel of the frame being drawn. Only valid INSIDE
// a paint block - the offscreen DC dies in renderer_end_paint().
bool renderer_probe_pixel(RendererContext* ctx, int x, int y, Color* out);
void renderer_draw_rect_fill(RendererContext* ctx, Rect rect, Color color);
// Rounded rectangle: fill + optional outline (stroke_width <= 0 = no outline).
void renderer_draw_round_rect(RendererContext* ctx, Rect rect, float radius,
                              Color fill, Color stroke, float stroke_width);
void renderer_draw_line(RendererContext* ctx, float x1, float y1, float x2, float y2, Color color, float thickness);
void renderer_draw_text(RendererContext* ctx, const char* text, float x, float y, float font_size, Color color);
float renderer_text_width(RendererContext* ctx, const char* text, float font_size);
void renderer_set_scissor(RendererContext* ctx, Rect clip);
void renderer_reset_scissor(RendererContext* ctx);
void renderer_begin_batch(RendererContext* ctx);
void renderer_end_batch(RendererContext* ctx);

// Theme
typedef struct {
    Color bg_primary;
    Color bg_secondary;
    Color bg_panel;
    Color bg_input;
    Color text_primary;
    Color text_secondary;
    Color accent;
    Color border;
    Color selected;
    Color hover;
    Color active;
    float border_radius;
    float padding;
} Theme;

Theme* theme_create_dark(void);
Theme* theme_create_light(void);
void theme_apply(Theme* theme, RendererContext* ctx);

#endif
