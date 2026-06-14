#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* RGB565 color helpers */
#define RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))
#define COLOR_BLACK    0x0000
#define COLOR_WHITE    0xFFFF
#define COLOR_RED      0xF800
#define COLOR_GREEN    0x07E0
#define COLOR_BLUE     0x001F
#define COLOR_YELLOW   0xFFE0
#define COLOR_CYAN     0x07FF
#define COLOR_ORANGE   RGB565(255, 140,  0)
#define COLOR_DARKGRAY RGB565( 40,  40, 40)
#define COLOR_GRAY     RGB565(120, 120, 120)
#define COLOR_LTGRAY   RGB565(200, 200, 200)
#define COLOR_NAVY     RGB565(  0,   0, 100)
#define COLOR_TEAL     RGB565(  0, 100, 100)

esp_err_t st7789_init(void);
void st7789_set_backlight(bool on);
void st7789_fill(uint16_t color);
void st7789_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void st7789_draw_pixel(int16_t x, int16_t y, uint16_t color);
void st7789_draw_hline(int16_t x, int16_t y, int16_t len, uint16_t color);
void st7789_draw_vline(int16_t x, int16_t y, int16_t len, uint16_t color);
void st7789_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void st7789_draw_bitmap(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels);
