#include "gui.h"
#include "font.h"
#include "st7789.h"
#include "pin_config.h"
#include <string.h>
#include <stdio.h>

/* ── Layout constants (portrait 135 × 240) ───────────────────────────────── */
#define HDR_H       20    /* header bar height      */
#define FOOTER_H    20    /* footer bar height       */
#define STATUS_H    16    /* status line below footer*/
#define ITEM_H      20    /* height per menu row     */
#define ITEM_PAD_X   6    /* left text indent        */

#define ITEMS_Y     (HDR_H + 1)
#define FOOTER_Y    (LCD_HEIGHT - FOOTER_H - STATUS_H)
#define STATUS_Y    (LCD_HEIGHT - STATUS_H)

/* ── Colours ─────────────────────────────────────────────────────────────── */
#define CLR_HDR_BG    COLOR_NAVY
#define CLR_HDR_FG    COLOR_WHITE
#define CLR_ITEM_BG   COLOR_BLACK
#define CLR_SEL_BG    RGB565(20, 60, 80)
#define CLR_SEL_FG    COLOR_WHITE
#define CLR_ITEM_FG   COLOR_LTGRAY
#define CLR_FOOTER_BG COLOR_DARKGRAY
#define CLR_FOOTER_FG COLOR_CYAN
#define CLR_STATUS_BG COLOR_DARKGRAY
#define CLR_STATUS_FG COLOR_YELLOW
#define CLR_SEP       COLOR_TEAL
#define CLR_VOL_FILL  COLOR_GREEN
#define CLR_VOL_BG    RGB565(30,30,30)

/* ── Text rendering ──────────────────────────────────────────────────────── */

void gui_draw_char(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale)
{
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7F) c = ' ';
    const uint8_t *glyph = font5x7[(uint8_t)c - 0x20];

    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            uint16_t color = (bits & (1 << row)) ? fg : bg;
            if (scale == 1) {
                st7789_draw_pixel(x + col, y + row, color);
            } else {
                st7789_fill_rect(x + col * scale, y + row * scale,
                                 scale, scale, color);
            }
        }
        /* column gap */
        if (scale == 1) {
            for (int row = 0; row < 7; row++)
                st7789_draw_pixel(x + 5, y + row, bg);
        } else {
            st7789_fill_rect(x + 5 * scale, y, scale, 7 * scale, bg);
        }
    }
    /* bottom gap row */
    st7789_fill_rect(x, y + 7 * scale, 6 * scale, scale, bg);
}

void gui_draw_string(int16_t x, int16_t y, const char *str,
                     uint16_t fg, uint16_t bg, uint8_t scale)
{
    while (*str) {
        gui_draw_char(x, y, *str++, fg, bg, scale);
        x += FONT_W * scale;
        if (x + FONT_W * scale > LCD_WIDTH) break;
    }
}

/* ── Volume bar ──────────────────────────────────────────────────────────── */
void gui_draw_vol_bar(int16_t x, int16_t y, int16_t w, int16_t h,
                      uint8_t pct, uint16_t fill_color, uint16_t bg_color)
{
    int16_t filled = (int16_t)((int32_t)pct * w / 100);
    if (filled > w) filled = w;
    st7789_fill_rect(x,          y, filled,      h, fill_color);
    st7789_fill_rect(x + filled, y, w - filled,  h, bg_color);
    st7789_draw_rect(x - 1,      y - 1, w + 2,  h + 2, COLOR_GRAY);
}

/* ── Menu ────────────────────────────────────────────────────────────────── */

static void draw_header(const gui_menu_t *m)
{
    st7789_fill_rect(0, 0, LCD_WIDTH, HDR_H, CLR_HDR_BG);
    /* title centred */
    int16_t tx = (LCD_WIDTH - (int16_t)(strlen(m->title) * FONT_W * 2)) / 2;
    if (tx < 2) tx = 2;
    gui_draw_string(tx, (HDR_H - FONT_H * 2) / 2, m->title,
                    CLR_HDR_FG, CLR_HDR_BG, 2);
    st7789_draw_hline(0, HDR_H, LCD_WIDTH, CLR_SEP);
}

static void draw_footer(const gui_menu_t *m)
{
    /* footer bar */
    st7789_fill_rect(0, FOOTER_Y, LCD_WIDTH, FOOTER_H, CLR_FOOTER_BG);

    /* KEY hints */
    gui_draw_string(2,  FOOTER_Y + 4, "K1:Next", CLR_FOOTER_FG, CLR_FOOTER_BG, 1);

    /* Volume bar on the right */
    char vbuf[8];
    snprintf(vbuf, sizeof(vbuf), "%3d%%", m->vol_pct);
    int16_t bar_x = LCD_WIDTH - 44;
    gui_draw_vol_bar(bar_x, FOOTER_Y + 6, 36, 8,
                     m->vol_pct, CLR_VOL_FILL, CLR_VOL_BG);
    gui_draw_string(bar_x - 25, FOOTER_Y + 4, vbuf, CLR_FOOTER_FG, CLR_FOOTER_BG, 1);

    /* status line */
    st7789_fill_rect(0, STATUS_Y, LCD_WIDTH, STATUS_H, CLR_STATUS_BG);
    st7789_draw_hline(0, STATUS_Y, LCD_WIDTH, CLR_SEP);
    gui_draw_string(2, STATUS_Y + 3,
                    m->status_str ? m->status_str : "Ready",
                    CLR_STATUS_FG, CLR_STATUS_BG, 1);
    /* K2 hint */
    gui_draw_string(LCD_WIDTH - 7 * FONT_W - 2, STATUS_Y + 3,
                    "K2:Sel", CLR_FOOTER_FG, CLR_STATUS_BG, 1);
}

static void draw_item(const gui_menu_t *m, uint8_t idx)
{
    bool sel = (idx == m->selected);
    int16_t iy = ITEMS_Y + idx * ITEM_H;

    uint16_t bg = sel ? CLR_SEL_BG  : CLR_ITEM_BG;
    uint16_t fg = sel ? CLR_SEL_FG  : m->items[idx].accent;

    st7789_fill_rect(0, iy, LCD_WIDTH, ITEM_H, bg);

    /* selector arrow */
    if (sel) {
        gui_draw_string(1, iy + (ITEM_H - FONT_H * 2) / 2, ">", COLOR_ORANGE, bg, 2);
    }

    /* label (scale 2 = 12×16 chars, up to 10 chars in 135px) */
    gui_draw_string(ITEM_PAD_X + FONT_W * 2, iy + (ITEM_H - FONT_H * 2) / 2,
                    m->items[idx].label, fg, bg, 2);

    /* bottom divider */
    st7789_draw_hline(0, iy + ITEM_H - 1, LCD_WIDTH, CLR_SEP);
}

void gui_menu_draw(const gui_menu_t *m)
{
    st7789_fill(COLOR_BLACK);
    draw_header(m);
    for (uint8_t i = 0; i < m->count; i++) draw_item(m, i);
    draw_footer(m);
}

void gui_menu_update_row(const gui_menu_t *m, uint8_t idx, bool full_redraw)
{
    if (full_redraw) {
        gui_menu_draw(m);
        return;
    }
    draw_item(m, idx);
    draw_footer(m);
}

void gui_menu_set_status(gui_menu_t *m, const char *status)
{
    m->status_str = status;
    st7789_fill_rect(0, STATUS_Y, LCD_WIDTH, STATUS_H, CLR_STATUS_BG);
    st7789_draw_hline(0, STATUS_Y, LCD_WIDTH, CLR_SEP);
    gui_draw_string(2, STATUS_Y + 3, status, CLR_STATUS_FG, CLR_STATUS_BG, 1);
    gui_draw_string(LCD_WIDTH - 7 * FONT_W - 2, STATUS_Y + 3,
                    "K2:Sel", CLR_FOOTER_FG, CLR_STATUS_BG, 1);
}
