#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "pin_config.h"
#include "st7789.h"
#include "gui.h"
#include "es8311.h"
#include "m5pm1.h"
#include "audio.h"

static const char *TAG = "main";

/* ── Menu definition ─────────────────────────────────────────────────────── */
#define ITEM_TONE_A4    0
#define ITEM_TONE_C5    1
#define ITEM_SCALE      2
#define ITEM_RECORD     3
#define ITEM_PLAYBACK   4
#define ITEM_VOL_UP     5
#define ITEM_VOL_DOWN   6
#define ITEM_STOP       7

static gui_menu_t s_menu = {
    .title    = "StickS3",
    .count    = 8,
    .selected = 0,
    .vol_pct  = 60,
    .status_str = "Ready",
    .items = {
        [ITEM_TONE_A4]  = {"Tone A4",  COLOR_CYAN   },
        [ITEM_TONE_C5]  = {"Tone C5",  COLOR_CYAN   },
        [ITEM_SCALE]    = {"Scale",    COLOR_GREEN  },
        [ITEM_RECORD]   = {"Record",   COLOR_ORANGE },
        [ITEM_PLAYBACK] = {"Playback", COLOR_YELLOW },
        [ITEM_VOL_UP]   = {"Vol  +",   COLOR_LTGRAY },
        [ITEM_VOL_DOWN] = {"Vol  -",   COLOR_LTGRAY },
        [ITEM_STOP]     = {"Stop",     COLOR_RED    },
    },
};

/* ── Button debounce ─────────────────────────────────────────────────────── */
#define DEBOUNCE_MS 50

static bool btn_pressed(int gpio)
{
    if (gpio_get_level(gpio) != 0) return false;
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
    return gpio_get_level(gpio) == 0;
}

static void wait_release(int gpio)
{
    while (gpio_get_level(gpio) == 0) vTaskDelay(pdMS_TO_TICKS(10));
}

/* ── Button GPIO init ────────────────────────────────────────────────────── */
static void buttons_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BTN_KEY1_GPIO) | (1ULL << BTN_KEY2_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

/* ── Action handler ──────────────────────────────────────────────────────── */
static void handle_select(gui_menu_t *m)
{
    uint8_t sel = m->selected;
    char    status[32];

    switch (sel) {

    case ITEM_TONE_A4:
        snprintf(status, sizeof(status), "Playing 440 Hz…");
        gui_menu_set_status(m, status);
        es8311_set_volume(m->vol_pct);
        audio_play_tone(440, 1000, m->vol_pct);   /* 1 s A4 tone */
        gui_menu_set_status(m, "Done");
        break;

    case ITEM_TONE_C5:
        snprintf(status, sizeof(status), "Playing 523 Hz…");
        gui_menu_set_status(m, status);
        es8311_set_volume(m->vol_pct);
        audio_play_tone(523, 1000, m->vol_pct);   /* 1 s C5 tone */
        gui_menu_set_status(m, "Done");
        break;

    case ITEM_SCALE:
        gui_menu_set_status(m, "Playing scale…");
        es8311_set_volume(m->vol_pct);
        audio_play_scale(m->vol_pct);
        gui_menu_set_status(m, "Done");
        break;

    case ITEM_RECORD:
        gui_menu_set_status(m, "Recording 3s…");
        audio_record_start(3000);
        gui_menu_set_status(m, "Rec done");
        break;

    case ITEM_PLAYBACK:
        if (!audio_has_recording()) {
            gui_menu_set_status(m, "No recording!");
        } else {
            gui_menu_set_status(m, "Playing back…");
            es8311_set_volume(m->vol_pct);
            audio_playback_recorded(m->vol_pct);
            gui_menu_set_status(m, "Done");
        }
        break;

    case ITEM_VOL_UP:
        if (m->vol_pct < 75) m->vol_pct += 5;  /* cap at 75% on battery */
        snprintf(status, sizeof(status), "Vol: %d%%", m->vol_pct);
        gui_menu_set_status(m, status);
        es8311_set_volume(m->vol_pct);
        /* Redraw footer to update bar */
        gui_menu_draw(m);
        break;

    case ITEM_VOL_DOWN:
        if (m->vol_pct > 5) m->vol_pct -= 5;
        snprintf(status, sizeof(status), "Vol: %d%%", m->vol_pct);
        gui_menu_set_status(m, status);
        es8311_set_volume(m->vol_pct);
        gui_menu_draw(m);
        break;

    case ITEM_STOP:
        audio_stop();
        gui_menu_set_status(m, "Stopped");
        break;
    }
}

/* ── Main ────────────────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "StickS3 Speaker + Menu Demo");

    /* 1. PMIC – initialises I2C bus and enables 3V3_L3B rail */
    ESP_ERROR_CHECK(m5pm1_init());

    /* 2. I2S channels (must exist before ES8311 codec init) */
    ESP_ERROR_CHECK(audio_init());

    /* 3. ES8311 codec — opens esp_codec_dev which enables I2S channels */
    ESP_ERROR_CHECK(es8311_init());
    ESP_ERROR_CHECK(es8311_set_volume(60));

    /* 4. SPI LCD */
    ESP_ERROR_CHECK(st7789_init());

    /* 5. Buttons */
    buttons_init();

    /* 6. Draw initial menu */
    gui_menu_draw(&s_menu);

    ESP_LOGI(TAG, "Init complete – entering UI loop");

    /* Startup beep – two short tones to confirm audio path is alive */
    es8311_set_volume(100);
    audio_play_tone(880, 200, 100);
    vTaskDelay(pdMS_TO_TICKS(50));
    audio_play_tone(1046, 200, 100);

    gui_menu_set_status(&s_menu, "Ready");

    /* ── Main loop ───────────────────────────────────────────────────────── */
    for (;;) {
        /* KEY1 – navigate to next menu item */
        if (btn_pressed(BTN_KEY1_GPIO)) {
            wait_release(BTN_KEY1_GPIO);
            uint8_t old = s_menu.selected;
            s_menu.selected = (s_menu.selected + 1) % s_menu.count;

            /* Redraw just the two changed rows for speed */
            gui_menu_update_row(&s_menu, old, false);
            gui_menu_update_row(&s_menu, s_menu.selected, false);
        }

        /* KEY2 – select / execute */
        if (btn_pressed(BTN_KEY2_GPIO)) {
            wait_release(BTN_KEY2_GPIO);
            handle_select(&s_menu);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
