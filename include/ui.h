// ============================================
// File Pilot Clone - UI Header
// ============================================

#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "renderer.h"
#include "platform.h"

// ---- Widget Types ----
typedef enum {
    WIDGET_PANEL,
    WIDGET_BUTTON,
    WIDGET_LABEL,
    WIDGET_LIST,
    WIDGET_TREE,
    WIDGET_SPLIT_VIEW,
    WIDGET_COMMAND_PALETTE,
    WIDGET_STATUS_BAR,
    WIDGET_TOOLBAR,
    WIDGET_CONTEXT_MENU,
    WIDGET_MAX
} WidgetType;

typedef enum {
    STATE_NORMAL,
    STATE_HOVERED,
    STATE_PRESSED,
    STATE_FOCUSED
} WidgetState;

typedef struct Widget {
    int x, y;
    int width, height;
    WidgetType type;
    WidgetState state;
    bool visible;
    bool enabled;
    bool focused;
    char id[64];
    Color bg_color;
    Color text_color;
    Color border_color;
    float border_width;
    struct Widget* parent;
    struct Widget** children;
    size_t child_count;
    size_t child_capacity;
    void (*on_click)(struct Widget* self, int x, int y);
} Widget;

// ---- List Widget ----
typedef struct {
    Widget base;
    char** items;
    size_t item_count;
    int selected_index;
    int item_height;
} ListWidget;

// ---- Split View ----
typedef struct {
    Widget base;
    Widget* left_panel;
    Widget* right_panel;
    float split_ratio;
    char orientation; // 'h' or 'v'
} SplitViewWidget;

// ---- Create Functions ----
Widget* widget_create(WidgetType type, int x, int y, int width, int height);
void widget_destroy(Widget* widget);
void widget_add_child(Widget* parent, Widget* child);
void widget_draw(RendererContext* ctx, Widget* widget);
bool widget_handle_input(Widget* widget, const InputState* input);
void widget_focus(Widget* w);
Widget* widget_find_at(Widget* root, int x, int y);

// ---- Helper Creators ----
Widget* create_panel(Widget* parent, int x, int y, int width, int height);
Widget* create_button(Widget* parent, int x, int y, int width, int height, const char* label);
Widget* create_label(Widget* parent, int x, int y, int width, int height, const char* text);
ListWidget* create_list(Widget* parent, int x, int y, int width, int height);
Widget* create_split_view(Widget* parent, int x, int y, int width, int height);
Widget* create_toolbar(Widget* parent, int y, int width, int height);
Widget* create_status_bar(Widget* parent, int y, int width, int height);

// ---- Layout ----
void widget_layout_horizontal(Widget* parent, int spacing);
void widget_layout_vertical(Widget* parent, int spacing);

// ---- Drawing Shortcuts ----
void draw_filled_rect(RendererContext* ctx, int x, int y, int w, int h, Color color);
void draw_border_rect(RendererContext* ctx, int x, int y, int w, int h, Color color);
void draw_text(RendererContext* ctx, const char* text, int x, int y, Color color);
void draw_separator(RendererContext* ctx, int x, int y, int w, int h, Color color);

#endif
