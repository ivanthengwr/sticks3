#include "gui.h"
#include "font.h"
#include "st7789.h"
#include "stackup_logo.h"
#include "pin_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

/* ── Layout (portrait 135 × 240) ─────────────────────────────────────────── */
#define HDR_H           26
#define CAROUSEL_TOP      (HDR_H + 6)
#define CAROUSEL_H        142
#define CENTER_ICON_SZ    52
#define SIDE_ICON_SZ      26
#define LABEL_AREA_Y      (CAROUSEL_TOP + CAROUSEL_H + 2)
#define LABEL_AREA_H      18
#define VOL_ROW_Y         (LABEL_AREA_Y + LABEL_AREA_H + 2)
#define FOOTER_Y          (LCD_HEIGHT - 34)
#define STATUS_Y          (LCD_HEIGHT - 16)

/* ── Colours (iPod-inspired dark theme) ───────────────────────────────────── */
#define CLR_BG          RGB565(10, 10, 14)
#define CLR_HDR_BG      RGB565(22, 24, 32)
#define CLR_HDR_FG      RGB565(230, 230, 235)
#define CLR_CARD_BG     RGB565(32, 36, 48)
#define CLR_RING        RGB565(72, 130, 210)
#define CLR_RING_DIM    RGB565(40, 70, 110)
#define CLR_LABEL_FG    COLOR_WHITE
#define CLR_LABEL_DIM   RGB565(90, 95, 105)
#define CLR_FOOTER_BG   RGB565(18, 18, 24)
#define CLR_FOOTER_FG   RGB565(160, 175, 195)
#define CLR_STATUS_FG   RGB565(255, 210, 80)
#define CLR_SEP         RGB565(45, 50, 65)
#define CLR_VOL_FILL    RGB565(90, 180, 255)
#define CLR_VOL_BG      RGB565(28, 30, 38)
#define CLR_CHEVRON     RGB565(100, 110, 130)
#define CAROUSEL_CENTER_Y (CAROUSEL_TOP + CAROUSEL_H / 2 - 6)
#define CAROUSEL_SIDE_Y   (CAROUSEL_CENTER_Y + 4)
#define CAROUSEL_LEFT_X   22
#define CAROUSEL_RIGHT_X  (LCD_WIDTH - 22)
#define CAROUSEL_CENTER_X (LCD_WIDTH / 2)
#define NAV_FRAMES        2
#define EASE_RES          1024
#define TRANS_ICON_SZ     CENTER_ICON_SZ

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
        if (scale == 1) {
            for (int row = 0; row < 7; row++)
                st7789_draw_pixel(x + 5, y + row, bg);
        } else {
            st7789_fill_rect(x + 5 * scale, y, scale, 7 * scale, bg);
        }
    }
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

void gui_draw_vol_bar(int16_t x, int16_t y, int16_t w, int16_t h,
                      uint8_t pct, uint16_t fill_color, uint16_t bg_color)
{
    int16_t filled = (int16_t)((int32_t)pct * w / 100);
    if (filled > w) filled = w;
    st7789_fill_rect(x,          y, filled,     h, fill_color);
    st7789_fill_rect(x + filled, y, w - filled, h, bg_color);
    st7789_draw_rect(x - 1, y - 1, w + 2, h + 2, CLR_SEP);
}

/* ── Colour helpers ────────────────────────────────────────────────────────── */

static uint16_t dim_color(uint16_t c, uint8_t pct)
{
    uint8_t r = ((c >> 11) & 0x1F) << 3;
    uint8_t g = ((c >> 5) & 0x3F) << 2;
    uint8_t b = (c & 0x1F) << 3;
    r = (uint8_t)((r * pct) / 100);
    g = (uint8_t)((g * pct) / 100);
    b = (uint8_t)((b * pct) / 100);
    return RGB565(r, g, b);
}

static int16_t str_width(const char *s, uint8_t scale)
{
    return (int16_t)(strlen(s) * FONT_W * scale);
}

/* Fast snap (ease-out quad) — position */
static int32_t ease_out_quad(int32_t t)
{
    if (t <= 0) return 0;
    if (t >= EASE_RES) return EASE_RES;
    int64_t u = EASE_RES - t;
    return (int32_t)(EASE_RES - (u * u) / EASE_RES);
}

static int16_t lerpi(int16_t a, int16_t b, int32_t ease)
{
    return (int16_t)(a + ((int32_t)(b - a) * ease) / EASE_RES);
}

static uint8_t lerpu8(uint8_t a, uint8_t b, int32_t ease)
{
    return (uint8_t)(a + ((int32_t)(b - a) * ease) / EASE_RES);
}

static uint8_t menu_idx_add(uint8_t idx, int8_t delta, uint8_t count)
{
    int32_t v = (int32_t)idx + delta;
    v %= (int32_t)count;
    if (v < 0) {
        v += count;
    }
    return (uint8_t)v;
}

static void draw_centered_string(int16_t y, const char *str,
                                 uint16_t fg, uint16_t bg, uint8_t scale)
{
    int16_t x = (LCD_WIDTH - str_width(str, scale)) / 2;
    if (x < 0) x = 0;
    gui_draw_string(x, y, str, fg, bg, scale);
}

/* ── Procedural icons (drawn into a square tile) ─────────────────────────── */

static void icon_clear_tile(int16_t x, int16_t y, int16_t sz, uint16_t bg)
{
    st7789_fill_rect(x, y, sz, sz, bg);
}

static void icon_note(int16_t x, int16_t y, int16_t sz, uint16_t c, uint16_t bg)
{
    icon_clear_tile(x, y, sz, bg);
    int stem = sz / 5;
    int head = sz / 4;
    int cx = x + sz / 2 - head / 2;
    int cy = y + sz / 2;
    st7789_fill_rect(cx + head, cy - sz / 3, stem, sz / 2 + head / 2, c);
    st7789_fill_rect(cx, cy + head / 4, head, head, c);
    st7789_fill_rect(cx - head / 4, cy + head / 2, head + head / 2, head / 2, c);
}

static void icon_scale(int16_t x, int16_t y, int16_t sz, uint16_t c, uint16_t bg)
{
    icon_clear_tile(x, y, sz, bg);
    int bar_w = sz / 6;
    int gap = sz / 8;
    int base = y + sz - sz / 5;
    for (int i = 0; i < 4; i++) {
        int h = sz / 5 + i * (sz / 6);
        int bx = x + gap + i * (bar_w + gap);
        st7789_fill_rect(bx, base - h, bar_w, h, c);
    }
}

static void icon_record(int16_t x, int16_t y, int16_t sz, uint16_t c, uint16_t bg)
{
    icon_clear_tile(x, y, sz, bg);
    int pad = sz / 6;
    st7789_draw_rect(x + pad, y + pad, sz - 2 * pad, sz - 2 * pad, c);
    int dot = sz / 5;
    st7789_fill_rect(x + (sz - dot) / 2, y + (sz - dot) / 2, dot, dot, c);
}

static void icon_countdown(int16_t x, int16_t y, int16_t sz, uint16_t c, uint16_t bg, uint8_t sec)
{
    char buf[4];

    icon_clear_tile(x, y, sz, bg);
    int pad = sz / 8;
    st7789_draw_rect(x + pad, y + pad, sz - 2 * pad, sz - 2 * pad, c);
    st7789_draw_rect(x + pad + 2, y + pad + 2, sz - 2 * pad - 4, sz - 2 * pad - 4,
                     dim_color(c, 35));

    snprintf(buf, sizeof(buf), "%u", (unsigned)sec);
    gui_draw_string(x + (sz - str_width(buf, 3)) / 2,
                    y + (sz - FONT_H * 3) / 2,
                    buf, c, bg, 3);
}

static void icon_play(int16_t x, int16_t y, int16_t sz, uint16_t c, uint16_t bg)
{
    icon_clear_tile(x, y, sz, bg);
    int m = sz / 5;
    for (int row = 0; row < sz - 2 * m; row++) {
        int half = (sz - 2 * m) / 2;
        int w = m + (row < half ? row : (sz - 2 * m - 1 - row));
        st7789_fill_rect(x + m + m / 2, y + m + row, w, 1, c);
    }
}

static void icon_vol(int16_t x, int16_t y, int16_t sz, uint16_t c, uint16_t bg, bool up)
{
    icon_clear_tile(x, y, sz, bg);
    int spk = sz / 4;
    st7789_fill_rect(x + sz / 6, y + sz / 3, spk, spk, c);
    st7789_fill_rect(x + sz / 6 + spk, y + sz / 3 + spk / 4, spk, spk / 2, c);
    st7789_fill_rect(x + sz / 6 + spk * 2, y + sz / 3 + spk / 3, spk / 2, spk / 3, c);
    int ax = x + sz / 2 + spk / 2;
    int ay = y + sz / 4;
    int al = sz / 3;
    if (up) {
        st7789_fill_rect(ax, ay + al / 2, 2, al / 2, c);
        st7789_fill_rect(ax - al / 3, ay + al / 4, al / 2, 2, c);
        st7789_fill_rect(ax - al / 4, ay, 2, al / 4, c);
        st7789_fill_rect(ax + al / 4 - 2, ay, 2, al / 4, c);
    } else {
        st7789_fill_rect(ax, ay, 2, al / 2, c);
        st7789_fill_rect(ax - al / 3, ay + al / 4, al / 2, 2, c);
        st7789_fill_rect(ax - al / 4, ay + al / 2, 2, al / 4, c);
        st7789_fill_rect(ax + al / 4 - 2, ay + al / 2, 2, al / 4, c);
    }
}

static void icon_stop(int16_t x, int16_t y, int16_t sz, uint16_t c, uint16_t bg)
{
    icon_clear_tile(x, y, sz, bg);
    int pad = sz / 4;
    st7789_fill_rect(x + pad, y + pad, sz - 2 * pad, sz - 2 * pad, c);
}

static void icon_water(int16_t x, int16_t y, int16_t sz, uint16_t c, uint16_t bg)
{
    icon_clear_tile(x, y, sz, bg);
    int cx = x + sz / 2;
    int top = y + sz / 5;
    for (int row = 0; row < sz * 3 / 5; row++) {
        int half = (sz * 3 / 5) / 2;
        int w = row < half ? (row + 1) * 2 : (half - (row - half)) * 2;
        if (w < 2) w = 2;
        st7789_fill_rect(cx - w / 2, top + row, w, 1, c);
    }
    st7789_fill_rect(cx - sz / 5, y + sz - sz / 5, sz / 2 + 2, sz / 8, c);
}

static void draw_icon(gui_icon_t type, int16_t x, int16_t y, int16_t sz,
                      uint16_t color, uint16_t bg, uint8_t brightness)
{
    uint16_t c = brightness >= 100 ? color : dim_color(color, brightness);
    switch (type) {
    case GUI_ICON_NOTE:   icon_note(x, y, sz, c, bg); break;
    case GUI_ICON_SCALE:  icon_scale(x, y, sz, c, bg); break;
    case GUI_ICON_RECORD: icon_record(x, y, sz, c, bg); break;
    case GUI_ICON_COUNTDOWN:
        icon_countdown(x, y, sz, c, bg, 0);
        break;
    case GUI_ICON_PLAY:   icon_play(x, y, sz, c, bg); break;
    case GUI_ICON_VOL_UP: icon_vol(x, y, sz, c, bg, true); break;
    case GUI_ICON_VOL_DOWN: icon_vol(x, y, sz, c, bg, false); break;
    case GUI_ICON_STOP:   icon_stop(x, y, sz, c, bg); break;
    case GUI_ICON_WATER:  icon_water(x, y, sz, c, bg); break;
    default:              icon_clear_tile(x, y, sz, bg); break;
    }
}

/* ── Carousel chrome ───────────────────────────────────────────────────────── */

static void draw_header(const gui_menu_t *m)
{
    st7789_fill_rect(0, 0, LCD_WIDTH, HDR_H, CLR_HDR_BG);
    draw_centered_string((HDR_H - FONT_H * 2) / 2, m->title, CLR_HDR_FG, CLR_HDR_BG, 2);
    st7789_draw_hline(0, HDR_H, LCD_WIDTH, CLR_SEP);
}

static void draw_chevron(int16_t cx, int16_t cy, bool left)
{
    int s = 5;
    uint16_t c = CLR_CHEVRON;
    if (left) {
        for (int i = 0; i < s; i++)
            st7789_fill_rect(cx + i, cy - s / 2 + i, 2, s - 2 * i, c);
    } else {
        for (int i = 0; i < s; i++)
            st7789_fill_rect(cx + s - i, cy - s / 2 + i, 2, s - 2 * i, c);
    }
}

static void draw_position_dots(const gui_menu_t *m, uint8_t highlight)
{
    const int16_t dot = 3;
    const int16_t gap = 4;
    int16_t total = (int16_t)(m->count * dot + (m->count - 1) * gap);
    int16_t start_x = (LCD_WIDTH - total) / 2;
    int16_t y = CAROUSEL_TOP + CAROUSEL_H - 6;

    for (uint8_t i = 0; i < m->count; i++) {
        int16_t x = start_x + i * (dot + gap);
        uint16_t c = (i == highlight) ? CLR_RING : CLR_LABEL_DIM;
        st7789_fill_rect(x, y, dot, dot, c);
    }
}

static void draw_carousel_item_scaled(const gui_menu_t *m, uint8_t idx,
                                      int16_t cx, int16_t cy, int16_t icon_sz,
                                      uint8_t brightness, uint8_t ring_pct)
{
    if (idx >= m->count || icon_sz < 8 || brightness < 6) return;

    const gui_item_t *item = &m->items[idx];
    int16_t card_pad = ring_pct > 30 ? 6 : 2;
    int16_t card_sz = icon_sz + card_pad * 2;
    int16_t card_x = cx - card_sz / 2;
    int16_t card_y = cy - card_sz / 2;
    uint16_t card_bg = ring_pct > 35 ? CLR_CARD_BG : CLR_BG;

    if (ring_pct > 0) {
        uint16_t ring_outer = dim_color(CLR_RING_DIM, ring_pct);
        uint16_t ring_inner = dim_color(CLR_RING, ring_pct);
        st7789_draw_rect(card_x - 1, card_y - 1, card_sz + 2, card_sz + 2, ring_outer);
        st7789_draw_rect(card_x, card_y, card_sz, card_sz, ring_inner);
    } else {
        st7789_fill_rect(card_x, card_y, card_sz, card_sz, card_bg);
    }

    if (m->record_countdown_active && idx == m->selected) {
        icon_countdown(card_x + card_pad, card_y + card_pad, icon_sz,
                       item->accent, card_bg, m->record_countdown_sec);
    } else {
        draw_icon(item->icon, card_x + card_pad, card_y + card_pad, icon_sz,
                  item->accent, card_bg, brightness);
    }
}

static void draw_carousel(const gui_menu_t *m, int16_t slide_x)
{
    (void)slide_x;
    st7789_fill_rect(0, CAROUSEL_TOP, LCD_WIDTH, CAROUSEL_H, CLR_BG);

    uint8_t prev = menu_idx_add(m->selected, -1, m->count);
    uint8_t next = menu_idx_add(m->selected, 1, m->count);

    if (m->count > 1) {
        draw_chevron(10, CAROUSEL_CENTER_Y, true);
        draw_chevron(LCD_WIDTH - 10, CAROUSEL_CENTER_Y, false);
        draw_carousel_item_scaled(m, prev, CAROUSEL_LEFT_X, CAROUSEL_SIDE_Y,
                                  SIDE_ICON_SZ, 45, 0);
        draw_carousel_item_scaled(m, next, CAROUSEL_RIGHT_X, CAROUSEL_SIDE_Y,
                                  SIDE_ICON_SZ, 45, 0);
    }

    draw_carousel_item_scaled(m, m->selected, CAROUSEL_CENTER_X, CAROUSEL_CENTER_Y,
                              CENTER_ICON_SZ, 100, 100);
    draw_position_dots(m, m->selected);
}

static void draw_label_for(const gui_menu_t *m, uint8_t idx)
{
    st7789_fill_rect(0, LABEL_AREA_Y, LCD_WIDTH, LABEL_AREA_H, CLR_BG);

    if (m->record_countdown_active && idx == m->selected) {
        draw_centered_string(LABEL_AREA_Y + 4, "Recording", COLOR_ORANGE, CLR_BG, 2);
        return;
    }

    draw_centered_string(LABEL_AREA_Y + 2, m->items[idx].label, CLR_LABEL_FG, CLR_BG, 2);

    char idx_buf[8];
    snprintf(idx_buf, sizeof(idx_buf), "%u/%u", idx + 1, m->count);
    gui_draw_string(LCD_WIDTH - str_width(idx_buf, 1) - 4,
                    LABEL_AREA_Y + 10, idx_buf, CLR_LABEL_DIM, CLR_BG, 1);
}

static void draw_label(const gui_menu_t *m)
{
    draw_label_for(m, m->selected);
}

static void draw_volume_row(const gui_menu_t *m)
{
    char vbuf[8];
    const int16_t bar_w = 78;
    const int16_t bar_h = 6;
    const int16_t row_h = FOOTER_Y - VOL_ROW_Y;

    snprintf(vbuf, sizeof(vbuf), "%3d%%", m->vol_pct);
    int16_t pct_w = str_width(vbuf, 1);
    int16_t label_w = str_width("Vol", 1);
    int16_t total_w = label_w + 4 + bar_w + 6 + pct_w;
    int16_t x = (LCD_WIDTH - total_w) / 2;
    int16_t y = VOL_ROW_Y + (row_h - bar_h) / 2;

    st7789_fill_rect(0, VOL_ROW_Y, LCD_WIDTH, row_h, CLR_BG);
    gui_draw_string(x, y - 1, "Vol", CLR_LABEL_DIM, CLR_BG, 1);
    x += label_w + 4;
    gui_draw_vol_bar(x, y, bar_w, bar_h, m->vol_pct, CLR_VOL_FILL, CLR_VOL_BG);
    gui_draw_string(x + bar_w + 6, y - 1, vbuf, CLR_FOOTER_FG, CLR_BG, 1);
}

static void draw_carousel_transition(const gui_menu_t *m, uint8_t from_idx, uint8_t to_idx,
                                     int32_t pos_ease, int32_t fade_ease, int8_t dir,
                                     bool final_frame)
{
    st7789_fill_rect(0, CAROUSEL_TOP, LCD_WIDTH, CAROUSEL_H, CLR_BG);

    if (m->count > 1) {
        draw_chevron(10, CAROUSEL_CENTER_Y, true);
        draw_chevron(LCD_WIDTH - 10, CAROUSEL_CENTER_Y, false);
    }

    int16_t from_x, from_y, to_x, to_y;
    if (dir > 0) {
        from_x = lerpi(CAROUSEL_CENTER_X, CAROUSEL_LEFT_X, pos_ease);
        from_y = lerpi(CAROUSEL_CENTER_Y, CAROUSEL_SIDE_Y, pos_ease);
        to_x = lerpi(CAROUSEL_RIGHT_X, CAROUSEL_CENTER_X, pos_ease);
        to_y = lerpi(CAROUSEL_SIDE_Y, CAROUSEL_CENTER_Y, pos_ease);
    } else {
        from_x = lerpi(CAROUSEL_CENTER_X, CAROUSEL_RIGHT_X, pos_ease);
        from_y = lerpi(CAROUSEL_CENTER_Y, CAROUSEL_SIDE_Y, pos_ease);
        to_x = lerpi(CAROUSEL_LEFT_X, CAROUSEL_CENTER_X, pos_ease);
        to_y = lerpi(CAROUSEL_SIDE_Y, CAROUSEL_CENTER_Y, pos_ease);
    }

    uint8_t from_bright = lerpu8(100, 0, fade_ease);
    uint8_t to_bright = lerpu8(0, 100, fade_ease);
    uint8_t from_ring = lerpu8(100, 0, fade_ease);
    uint8_t to_ring = lerpu8(0, 100, fade_ease);

    if (from_bright > 15) {
        draw_carousel_item_scaled(m, from_idx, from_x, from_y, TRANS_ICON_SZ,
                                  from_bright, from_ring);
    }
    if (to_bright > 15) {
        draw_carousel_item_scaled(m, to_idx, to_x, to_y, TRANS_ICON_SZ,
                                  to_bright, to_ring);
    }

    if (final_frame) {
        draw_position_dots(m, to_idx);
    }
}

static void draw_footer(const gui_menu_t *m)
{
    st7789_fill_rect(0, FOOTER_Y, LCD_WIDTH, LCD_HEIGHT - FOOTER_Y, CLR_FOOTER_BG);
    st7789_draw_hline(0, FOOTER_Y, LCD_WIDTH, CLR_SEP);

    gui_draw_string(4, FOOTER_Y + 3, "1 press prev option", CLR_FOOTER_FG, CLR_FOOTER_BG, 1);
    gui_draw_string(4, FOOTER_Y + 12, "2 press next option", CLR_FOOTER_FG, CLR_FOOTER_BG, 1);

    st7789_fill_rect(0, STATUS_Y, LCD_WIDTH, LCD_HEIGHT - STATUS_Y, CLR_FOOTER_BG);
    st7789_draw_hline(0, STATUS_Y, LCD_WIDTH, CLR_SEP);
    gui_draw_string(4, STATUS_Y + 3,
                    m->status_str ? m->status_str : "Ready",
                    CLR_STATUS_FG, CLR_FOOTER_BG, 1);
}

void gui_menu_draw(const gui_menu_t *m)
{
    st7789_fill(CLR_BG);
    draw_header(m);
    draw_carousel(m, 0);
    draw_label(m);
    draw_volume_row(m);
    draw_footer(m);
}

void gui_menu_navigate(gui_menu_t *m, int8_t direction)
{
    if (m->count <= 1) {
        return;
    }

    int8_t dir = (direction >= 0) ? 1 : -1;
    uint8_t from_idx = m->selected;
    uint8_t to_idx = menu_idx_add(from_idx, dir, m->count);

    for (int frame = 0; frame <= NAV_FRAMES; frame++) {
        bool final_frame = (frame == NAV_FRAMES);
        int32_t t = (frame * EASE_RES) / NAV_FRAMES;
        int32_t pos_ease = ease_out_quad(t);
        int32_t fade_ease = t;

        draw_carousel_transition(m, from_idx, to_idx, pos_ease, fade_ease, dir,
                                 final_frame);

        if (final_frame) {
            draw_label_for(m, to_idx);
        }
    }

    m->selected = to_idx;
}

void gui_menu_update_row(const gui_menu_t *m, uint8_t idx, bool full_redraw)
{
    (void)idx;
    if (full_redraw) {
        gui_menu_draw(m);
        return;
    }
    draw_carousel(m, 0);
    draw_label(m);
    draw_footer(m);
}

void gui_menu_set_status(gui_menu_t *m, const char *status)
{
    m->status_str = status;
    st7789_fill_rect(0, STATUS_Y, LCD_WIDTH, LCD_HEIGHT - STATUS_Y, CLR_FOOTER_BG);
    st7789_draw_hline(0, STATUS_Y, LCD_WIDTH, CLR_SEP);
    gui_draw_string(4, STATUS_Y + 3, status, CLR_STATUS_FG, CLR_FOOTER_BG, 1);
}

void gui_record_countdown_update(gui_menu_t *m, uint8_t seconds_left)
{
    static char status[24];

    m->record_countdown_active = true;
    m->record_countdown_sec = seconds_left;
    snprintf(status, sizeof(status), "Recording: %u sec", seconds_left);
    m->status_str = status;

    draw_carousel(m, 0);
    draw_label_for(m, m->selected);
    gui_menu_set_status(m, status);
}

void gui_record_countdown_clear(gui_menu_t *m)
{
    m->record_countdown_active = false;
    m->record_countdown_sec = 0;
    gui_menu_draw(m);
}

void gui_show_startup_splash(void)
{
    st7789_draw_bitmap(0, 0, STACKUP_LOGO_WIDTH, STACKUP_LOGO_HEIGHT, STACKUP_LOGO_RGB565);
}
