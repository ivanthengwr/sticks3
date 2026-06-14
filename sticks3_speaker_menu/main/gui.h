#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "st7789.h"

/* ── Text rendering ──────────────────────────────────────────────────────── */
void gui_draw_char(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale);
void gui_draw_string(int16_t x, int16_t y, const char *str,
                     uint16_t fg, uint16_t bg, uint8_t scale);

/* ── Volume bar ──────────────────────────────────────────────────────────── */
void gui_draw_vol_bar(int16_t x, int16_t y, int16_t w, int16_t h,
                      uint8_t pct, uint16_t fill_color, uint16_t bg_color);

/* ── iPod-style carousel menu ────────────────────────────────────────────── */
#define GUI_MAX_ITEMS 9

typedef enum {
    GUI_ICON_NOTE,
    GUI_ICON_SCALE,
    GUI_ICON_RECORD,
    GUI_ICON_COUNTDOWN,
    GUI_ICON_PLAY,
    GUI_ICON_VOL_UP,
    GUI_ICON_VOL_DOWN,
    GUI_ICON_STOP,
    GUI_ICON_WATER,
} gui_icon_t;

typedef struct {
    const char *label;
    uint16_t    accent;
    gui_icon_t  icon;
} gui_item_t;

typedef struct {
    const char   *title;
    gui_item_t    items[GUI_MAX_ITEMS];
    uint8_t       count;
    uint8_t       selected;
    uint8_t       vol_pct;
    const char   *status_str;
    bool          record_countdown_active;
    uint8_t       record_countdown_sec;
} gui_menu_t;

void gui_menu_draw(const gui_menu_t *m);
void gui_menu_navigate(gui_menu_t *m, int8_t direction);
void gui_menu_update_row(const gui_menu_t *m, uint8_t idx, bool full_redraw);
void gui_menu_set_status(gui_menu_t *m, const char *status);
void gui_record_countdown_update(gui_menu_t *m, uint8_t seconds_left);
void gui_record_countdown_clear(gui_menu_t *m);
void gui_show_startup_splash(void);
