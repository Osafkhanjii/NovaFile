// ============================================
// File Pilot Clone - UI Framework Implementation
// ============================================
#include "ui.h"
#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Widget* widget_create(WidgetType type, int x, int y, int width, int height) {
    Widget* w = (Widget*)mem_zero(sizeof(Widget));
    w->x = x;
    w->y = y;
    w->width = width;
    w->height = height;
    w->type = type;
    w->state = STATE_NORMAL;
    w->visible = true;
    w->enabled = true;
    w->bg_color = COLOR_DARK_GRAY;
    w->text_color = COLOR_TEXT;
    w->border_color = COLOR_BORDER;
    w->border_width = 1.0f;
    w->children = NULL;
    w->child_count = 0;
    w->child_capacity = 0;
    w->parent = NULL;
    w->on_click = NULL;
    w->id[0] = '\0';
    return w;
}

void widget_destroy(Widget* widget) {
    if (!widget) return;
    for (size_t i = 0; i < widget->child_count; i++) {
        widget_destroy(widget->children[i]);
    }
    // NOTE: id[] is an inline char array and on_click is a function pointer.
    // Neither is heap memory, so they must NEVER be freed.
    if (widget->children) free(widget->children);
    mem_free(widget);
}

void widget_add_child(Widget* parent, Widget* child) {
    if (!parent || !child) return;
    if (parent->child_count >= parent->child_capacity) {
        size_t new_cap = parent->child_capacity == 0 ? 8 : parent->child_capacity * 2;
        Widget** new_children = (Widget**)realloc(parent->children, new_cap * sizeof(Widget*));
        if (new_children) {
            parent->children = new_children;
            parent->child_capacity = new_cap;
        }
    }
    parent->children[parent->child_count] = child;
    child->parent = parent;
    parent->child_count++;
}

void widget_focus(Widget* w) {
    if (!w) return;
    w->focused = true;
    w->state = STATE_FOCUSED;
}

Widget* widget_find_at(Widget* root, int x, int y) {
    if (!root || !root->visible) return NULL;
    
    for (size_t i = 0; i < root->child_count; i++) {
        Widget* child = root->children[i];
        if (x >= child->x && x <= child->x + child->width &&
            y >= child->y && y <= child->y + child->height) {
            Widget* found = widget_find_at(child, x, y);
            return found ? found : child;
        }
    }
    return NULL;
}

bool widget_handle_input(Widget* widget, const InputState* input) {
    if (!widget || !widget->visible || !widget->enabled) return false;
    
    bool inside = (input->x >= widget->x && input->x <= widget->x + widget->width &&
                   input->y >= widget->y && input->y <= widget->y + widget->height);
    
    if (inside) {
        widget->state = STATE_HOVERED;
        if (input->left_click && widget->on_click) {
            widget->on_click(widget, input->x - widget->x, input->y - widget->y);
            return true;
        }
    } else {
        widget->state = STATE_NORMAL;
    }
    
    for (size_t i = 0; i < widget->child_count; i++) {
        if (widget_handle_input(widget->children[i], input)) return true;
    }
    return inside;
}

static void draw_widget(RendererContext* ctx, Widget* w) {
    if (!w || !w->visible) return;
    
    // Draw background
    if (w->bg_color.a > 0.01f) {
        Rect bg = {w->x, w->y, w->width, w->height};
        renderer_draw_rect_fill(ctx, bg, w->bg_color);
    }
    
    // Draw border
    if (w->border_width > 0) {
        renderer_draw_line(ctx, w->x, w->y, w->x + w->width, w->y, w->border_color, w->border_width);
        renderer_draw_line(ctx, w->x + w->width, w->y, w->x + w->width, w->y + w->height, w->border_color, w->border_width);
        renderer_draw_line(ctx, w->x + w->width, w->y + w->height, w->x, w->y + w->height, w->border_color, w->border_width);
        renderer_draw_line(ctx, w->x, w->y + w->height, w->x, w->y, w->border_color, w->border_width);
    }
    
    // Draw children
    for (size_t i = 0; i < w->child_count; i++) {
        draw_widget(ctx, w->children[i]);
    }
}

void widget_draw(RendererContext* ctx, Widget* widget) {
    draw_widget(ctx, widget);
}

// ---- Helper Creators ----
Widget* create_panel(Widget* parent, int x, int y, int width, int height) {
    Widget* panel = widget_create(WIDGET_PANEL, x, y, width, height);
    panel->bg_color = COLOR_PANEL;
    if (parent) widget_add_child(parent, panel);
    return panel;
}

Widget* create_button(Widget* parent, int x, int y, int width, int height, const char* label) {
    Widget* btn = widget_create(WIDGET_BUTTON, x, y, width, height);
    btn->bg_color = COLOR_DARK_GRAY;
    if (label) strncpy(btn->id, label, 63);
    if (parent) widget_add_child(parent, btn);
    return btn;
}

Widget* create_label(Widget* parent, int x, int y, int width, int height, const char* text) {
    Widget* lbl = widget_create(WIDGET_LABEL, x, y, width, height);
    if (text) strncpy(lbl->id, text, 63);
    lbl->text_color = COLOR_TEXT;
    if (parent) widget_add_child(parent, lbl);
    return lbl;
}

ListWidget* create_list(Widget* parent, int x, int y, int width, int height) {
    ListWidget* list = (ListWidget*)widget_create(WIDGET_LIST, x, y, width, height);
    list->base.bg_color = COLOR_BG;
    list->item_height = 28;
    list->selected_index = -1;
    if (parent) widget_add_child(parent, (Widget*)list);
    return list;
}

Widget* create_split_view(Widget* parent, int x, int y, int width, int height) {
    SplitViewWidget* sv = (SplitViewWidget*)widget_create(WIDGET_SPLIT_VIEW, x, y, width, height);
    sv->split_ratio = 0.25f;
    sv->orientation = 'v';
    if (parent) widget_add_child(parent, (Widget*)sv);
    return (Widget*)sv;
}

Widget* create_toolbar(Widget* parent, int y, int width, int height) {
    Widget* toolbar = widget_create(WIDGET_TOOLBAR, 0, y, width, height);
    toolbar->bg_color = COLOR_PANEL;
    toolbar->border_color = COLOR_BORDER;
    if (parent) widget_add_child(parent, toolbar);
    return toolbar;
}

Widget* create_status_bar(Widget* parent, int y, int width, int height) {
    Widget* bar = widget_create(WIDGET_STATUS_BAR, 0, y, width, height);
    bar->bg_color = COLOR_DARK_GRAY;
    bar->text_color = COLOR_LIGHT_GRAY;
    if (parent) widget_add_child(parent, bar);
    return bar;
}

// ---- Layout ----
void widget_layout_horizontal(Widget* parent, int spacing) {
    if (!parent) return;
    int cx = parent->x;
    for (size_t i = 0; i < parent->child_count; i++) {
        Widget* c = parent->children[i];
        c->x = cx;
        c->y = parent->y;
        cx += c->width + spacing;
    }
}

void widget_layout_vertical(Widget* parent, int spacing) {
    if (!parent) return;
    int cy = parent->y;
    for (size_t i = 0; i < parent->child_count; i++) {
        Widget* c = parent->children[i];
        c->x = parent->x;
        c->y = cy;
        cy += c->height + spacing;
    }
}

// ---- Drawing Shortcuts ----
void draw_filled_rect(RendererContext* ctx, int x, int y, int w, int h, Color color) {
    Rect r = {(float)x, (float)y, (float)w, (float)h};
    renderer_draw_rect_fill(ctx, r, color);
}

void draw_border_rect(RendererContext* ctx, int x, int y, int w, int h, Color color) {
    renderer_draw_line(ctx, x, y, x+w, y, color, 1.0f);
    renderer_draw_line(ctx, x+w, y, x+w, y+h, color, 1.0f);
    renderer_draw_line(ctx, x+w, y+h, x, y+h, color, 1.0f);
    renderer_draw_line(ctx, x, y+h, x, y, color, 1.0f);
}

void draw_text(RendererContext* ctx, const char* text, int x, int y, Color color) {
    renderer_draw_text(ctx, text, (float)x, (float)y, 14.0f, color);
}

void draw_separator(RendererContext* ctx, int x, int y, int w, int h, Color color) {
    renderer_draw_line(ctx, x, y, x + w, y, color, 1.0f);
}
