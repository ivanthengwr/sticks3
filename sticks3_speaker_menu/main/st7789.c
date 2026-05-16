#include "st7789.h"
#include "pin_config.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "st7789";
static spi_device_handle_t s_spi;
static SemaphoreHandle_t   s_lcd_mutex;

#define LCD_LOCK()   xSemaphoreTake(s_lcd_mutex, portMAX_DELAY)
#define LCD_UNLOCK() xSemaphoreGive(s_lcd_mutex)

static void st7789_fill_rect_unsafe(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

/* ── ST7789 command set ──────────────────────────────────────────────────── */
#define ST7789_NOP      0x00
#define ST7789_SWRESET  0x01
#define ST7789_SLPOUT   0x11
#define ST7789_NORON    0x13
#define ST7789_INVON    0x21
#define ST7789_DISPON   0x29
#define ST7789_CASET    0x2A
#define ST7789_RASET    0x2B
#define ST7789_RAMWR    0x2C
#define ST7789_MADCTL   0x36
#define ST7789_COLMOD   0x3A
#define ST7789_PORCTRL  0xB2
#define ST7789_GCTRL    0xB7
#define ST7789_VCOMS    0xBB
#define ST7789_LCMCTRL  0xC0
#define ST7789_VDVVRHEN 0xC2
#define ST7789_VRHS     0xC3
#define ST7789_VDVS     0xC4
#define ST7789_FRCTRL2  0xC6
#define ST7789_PWCTRL1  0xD0
#define ST7789_PVGAMCTRL 0xE0
#define ST7789_NVGAMCTRL 0xE1

/* ── Low-level SPI helpers ───────────────────────────────────────────────── */
static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(LCD_DC_GPIO, 0);
    spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_data(const uint8_t *data, size_t len)
{
    if (!len) return;
    gpio_set_level(LCD_DC_GPIO, 1);
    spi_transaction_t t = {.length = len * 8, .tx_buffer = data};
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_databyte(uint8_t d)
{
    lcd_data(&d, 1);
}

/* ── Window / address setting ────────────────────────────────────────────── */
static void set_window(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    uint8_t buf[4];

    x0 += LCD_COL_OFFSET; x1 += LCD_COL_OFFSET;
    y0 += LCD_ROW_OFFSET; y1 += LCD_ROW_OFFSET;

    lcd_cmd(ST7789_CASET);
    buf[0] = x0 >> 8; buf[1] = x0 & 0xFF;
    buf[2] = x1 >> 8; buf[3] = x1 & 0xFF;
    lcd_data(buf, 4);

    lcd_cmd(ST7789_RASET);
    buf[0] = y0 >> 8; buf[1] = y0 & 0xFF;
    buf[2] = y1 >> 8; buf[3] = y1 & 0xFF;
    lcd_data(buf, 4);

    lcd_cmd(ST7789_RAMWR);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t st7789_init(void)
{
    /* GPIO for DC and RST */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LCD_DC_GPIO) | (1ULL << LCD_RST_GPIO) | (1ULL << LCD_BL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(LCD_BL_GPIO, 0);

    /* SPI bus */
    spi_bus_config_t bus = {
        .mosi_io_num     = LCD_MOSI_GPIO,
        .miso_io_num     = -1,
        .sclk_io_num     = LCD_SCK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_SPI_FREQ_HZ,
        .mode           = 0,
        .spics_io_num   = LCD_CS_GPIO,
        .queue_size     = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_SPI_HOST, &dev, &s_spi));

    s_lcd_mutex = xSemaphoreCreateMutex();

    /* Hardware reset */
    gpio_set_level(LCD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Init sequence for ST7789P3, portrait 135×240 */
    lcd_cmd(ST7789_SWRESET);  vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(ST7789_SLPOUT);   vTaskDelay(pdMS_TO_TICKS(10));

    lcd_cmd(ST7789_COLMOD);   lcd_databyte(0x55);  /* 16-bit RGB565 */
    lcd_cmd(ST7789_MADCTL);   lcd_databyte(0x00);  /* portrait, RGB order */

    lcd_cmd(ST7789_PORCTRL);
    lcd_databyte(0x0C); lcd_databyte(0x0C);
    lcd_databyte(0x00); lcd_databyte(0x33); lcd_databyte(0x33);

    lcd_cmd(ST7789_GCTRL);    lcd_databyte(0x35);
    lcd_cmd(ST7789_VCOMS);    lcd_databyte(0x19);
    lcd_cmd(ST7789_LCMCTRL);  lcd_databyte(0x2C);
    lcd_cmd(ST7789_VDVVRHEN); lcd_databyte(0x01);
    lcd_cmd(ST7789_VRHS);     lcd_databyte(0x12);
    lcd_cmd(ST7789_VDVS);     lcd_databyte(0x20);
    lcd_cmd(ST7789_FRCTRL2);  lcd_databyte(0x0F);

    lcd_cmd(ST7789_PWCTRL1);
    lcd_databyte(0xA4); lcd_databyte(0xA1);

    /* Positive gamma */
    lcd_cmd(ST7789_PVGAMCTRL);
    const uint8_t pvgam[] = {0xD0,0x04,0x0D,0x11,0x13,0x2B,0x3F,0x54,0x4C,0x18,0x0D,0x0B,0x1F,0x23};
    lcd_data(pvgam, sizeof(pvgam));

    /* Negative gamma */
    lcd_cmd(ST7789_NVGAMCTRL);
    const uint8_t nvgam[] = {0xD0,0x04,0x0C,0x11,0x13,0x2C,0x3F,0x44,0x51,0x2F,0x1F,0x1F,0x20,0x23};
    lcd_data(nvgam, sizeof(nvgam));

    lcd_cmd(ST7789_INVON);    /* inversion needed for correct colours on P3 */
    lcd_cmd(ST7789_NORON);
    lcd_cmd(ST7789_DISPON);   vTaskDelay(pdMS_TO_TICKS(10));

    st7789_fill(COLOR_BLACK);
    gpio_set_level(LCD_BL_GPIO, 1);   /* backlight on */

    ESP_LOGI(TAG, "ST7789P3 135x240 initialised");
    return ESP_OK;
}

void st7789_set_backlight(bool on)
{
    gpio_set_level(LCD_BL_GPIO, on ? 1 : 0);
}

void st7789_fill(uint16_t color)
{
    LCD_LOCK();
    st7789_fill_rect_unsafe(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
    LCD_UNLOCK();
}

/* Internal unlocked fill used by fill() and fill_rect() under the same lock */
static void st7789_fill_rect_unsafe(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    set_window(x, y, x + w - 1, y + h - 1);

    static uint16_t line[LCD_WIDTH];
    uint16_t swapped = (color >> 8) | (color << 8);
    for (int i = 0; i < w && i < LCD_WIDTH; i++) line[i] = swapped;

    gpio_set_level(LCD_DC_GPIO, 1);
    spi_transaction_t t = {.length = (size_t)w * 2 * 8, .tx_buffer = line};
    for (int row = 0; row < h; row++) {
        spi_device_polling_transmit(s_spi, &t);
    }
}

void st7789_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    LCD_LOCK();
    st7789_fill_rect_unsafe(x, y, w, h, color);
    LCD_UNLOCK();
}

void st7789_draw_pixel(int16_t x, int16_t y, uint16_t color)
{
    LCD_LOCK();
    set_window(x, y, x, y);
    uint8_t buf[2] = {color >> 8, color & 0xFF};
    lcd_data(buf, 2);
    LCD_UNLOCK();
}

void st7789_draw_hline(int16_t x, int16_t y, int16_t len, uint16_t color)
{
    LCD_LOCK();
    st7789_fill_rect_unsafe(x, y, len, 1, color);
    LCD_UNLOCK();
}

void st7789_draw_vline(int16_t x, int16_t y, int16_t len, uint16_t color)
{
    LCD_LOCK();
    st7789_fill_rect_unsafe(x, y, 1, len, color);
    LCD_UNLOCK();
}

void st7789_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    LCD_LOCK();
    st7789_fill_rect_unsafe(x,         y,         w, 1, color);
    st7789_fill_rect_unsafe(x,         y + h - 1, w, 1, color);
    st7789_fill_rect_unsafe(x,         y,         1, h, color);
    st7789_fill_rect_unsafe(x + w - 1, y,         1, h, color);
    LCD_UNLOCK();
}
