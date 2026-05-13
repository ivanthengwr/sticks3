#pragma once
#include <stdint.h>
#include "st7789.h"

/* ── Text rendering ──────────────────────────────────────────────────────── */
void gui_draw_char(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale);
void gui_draw_string(int16_t x, int16_t y, const char *str,
                     uint16_t fg, uint16_t bg, uint8_t scale);

/* ── Volume bar ──────────────────────────────────────────────────────────── */
void gui_draw_vol_bar(int16_t x, int16_t y, int16_t w, int16_t h,
                      uint8_t pct, uint16_t fill_color, uint16_t bg_color);

/* ── Full-screen menu ────────────────────────────────────────────────────── */
#define GUI_MAX_ITEMS 8

typedef struct {
    const char *label;      /* text shown in the menu row */
    uint16_t    accent;     /* per-item colour accent      */
} gui_item_t;

typedef struct {
    const char   *title;
    gui_item_t    items[GUI_MAX_ITEMS];
    uint8_t       count;
    uint8_t       selected;     /* currently highlighted row */
    uint8_t       vol_pct;      /* 0-100 shown in footer     */
    const char   *status_str;   /* short status string       */
} gui_menu_t;

void gui_menu_draw(const gui_menu_t *m);
void gui_menu_update_row(const gui_menu_t *m, uint8_t idx, bool full_redraw);
void gui_menu_set_status(gui_menu_t *m, const char *status);
