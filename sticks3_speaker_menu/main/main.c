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
#include "sticks_pm1.h"
#include "audio.h"
#include "watering.h"

static const char *TAG = "main";

/* ── Menu definition ─────────────────────────────────────────────────────── */
#define ITEM_TONE_A4    0
#define ITEM_TONE_C5    1
#define ITEM_SCALE      2
#define ITEM_RECORD     3
#define ITEM_PLAYBACK   4
#define ITEM_VOL_UP     5
#define ITEM_VOL_DOWN   6
#define ITEM_WATERING   7

static gui_menu_t s_menu = {
    .title    = "StickS3",
    .count    = 8,
    .selected = 0,
    .vol_pct  = 80,
    .status_str = "Ready",
    .items = {
        [ITEM_TONE_A4]  = {"Tone A4",  COLOR_CYAN,   GUI_ICON_NOTE   },
        [ITEM_TONE_C5]  = {"Tone C5",  COLOR_CYAN,   GUI_ICON_NOTE   },
        [ITEM_SCALE]    = {"Scale",    COLOR_GREEN,  GUI_ICON_SCALE  },
        [ITEM_RECORD]   = {"Record",   COLOR_ORANGE, GUI_ICON_RECORD },
        [ITEM_PLAYBACK] = {"Playback", COLOR_YELLOW, GUI_ICON_PLAY   },
        [ITEM_VOL_UP]   = {"Vol  +",   COLOR_LTGRAY, GUI_ICON_VOL_UP },
        [ITEM_VOL_DOWN] = {"Vol  -",   COLOR_LTGRAY, GUI_ICON_VOL_DOWN },
        [ITEM_WATERING] = {"Watering", COLOR_GREEN,  GUI_ICON_WATER  },
    },
};

/* ── Button debounce ─────────────────────────────────────────────────────── */
#define DEBOUNCE_MS      50
#define DOUBLE_TAP_MS   300   /* window to detect a second KEY1 press */

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

/* Returns 1 for single tap (previous), 2 for double-tap (next item). */
static int key1_tap_type(void)
{
    wait_release(BTN_KEY1_GPIO);

    int elapsed = 0;
    while (elapsed < DOUBLE_TAP_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
        if (btn_pressed(BTN_KEY1_GPIO)) {
            wait_release(BTN_KEY1_GPIO);
            return 2;   /* double-tap → next */
        }
    }
    return 1;         /* single tap → previous */
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

/* ── Watering demo status callback ──────────────────────────────────────── */
static gui_menu_t *s_watering_menu_ref;   /* set before demo runs */
static gui_menu_t *s_record_menu_ref;

static volatile uint8_t s_rec_countdown_sec   = 0;
static volatile bool    s_rec_countdown_dirty = false;
static volatile bool    s_rec_done_pending    = false;
static volatile bool    s_rec_done_ok           = false;
static uint8_t            s_rec_countdown_drawn = 255;

static void watering_status_update(const char *msg)
{
    if (s_watering_menu_ref) {
        gui_menu_set_status(s_watering_menu_ref, msg);
    }
}

static void record_countdown_tick(uint8_t seconds_left)
{
    s_rec_countdown_sec   = seconds_left;
    s_rec_countdown_dirty = true;
}

static void record_finished(bool ok)
{
    s_rec_done_ok       = ok;
    s_rec_done_pending  = true;
}

static void record_poll_ui(void)
{
    if (s_rec_countdown_dirty && s_rec_countdown_sec != s_rec_countdown_drawn) {
        s_rec_countdown_dirty = false;
        s_rec_countdown_drawn = s_rec_countdown_sec;
        if (s_record_menu_ref) {
            gui_record_countdown_update(s_record_menu_ref, s_rec_countdown_sec);
        }
    }
}

static void record_handle_done(void)
{
    gui_menu_t *m = s_record_menu_ref;
    if (!m) {
        return;
    }

    s_record_menu_ref       = NULL;
    s_rec_countdown_drawn   = 255;
    gui_record_countdown_clear(m);
    if (s_rec_done_ok) {
        audio_play_recording_done_voice(m->vol_pct);
        gui_menu_set_status(m, "Rec done");
    } else {
        gui_menu_set_status(m, "Rec failed");
    }
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
        if (audio_record_busy()) {
            break;
        }
        s_record_menu_ref       = m;
        s_rec_countdown_sec     = RECORD_DURATION_MS / 1000;
        s_rec_countdown_drawn   = 255;
        s_rec_countdown_dirty   = true;
        s_rec_done_pending      = false;
        gui_record_countdown_update(m, s_rec_countdown_sec);
        s_rec_countdown_drawn = s_rec_countdown_sec;
        audio_record_start_async(RECORD_DURATION_MS, record_countdown_tick, record_finished);
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
        if (m->vol_pct < 100) m->vol_pct += 5;
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

    case ITEM_WATERING:
        if (watering_demo_running()) {
            watering_demo_stop();
            gui_menu_set_status(m, "Demo stopped");
        } else {
            s_watering_menu_ref = m;
            watering_demo_start(watering_status_update);
            gui_menu_set_status(m, "Demo running");
        }
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
    ESP_ERROR_CHECK(es8311_set_volume(80));
    ESP_ERROR_CHECK(audio_warmup_output());

    /* 4. SPI LCD */
    ESP_ERROR_CHECK(st7789_init());

    /* 5. Startup splash — STACKUP logo + welcome voice */
    gui_show_startup_splash();
    audio_play_welcome_stackup_voice(s_menu.vol_pct);
    vTaskDelay(pdMS_TO_TICKS(STARTUP_SPLASH_MS));

    /* 6. Buttons */
    buttons_init();

    /* 7. Watering unit (ADC + pump GPIO) */
    ESP_ERROR_CHECK(watering_init());

    /* 8. Draw menu */
    gui_menu_draw(&s_menu);

    ESP_LOGI(TAG, "Init complete – entering UI loop");

    gui_menu_set_status(&s_menu, "Ready");

    /* ── Main loop ───────────────────────────────────────────────────────── */
    for (;;) {
        if (audio_record_busy()) {
            record_poll_ui();
        } else if (s_rec_done_pending) {
            s_rec_done_pending = false;
            record_handle_done();
        } else {
            /* KEY1 – single tap: previous, double-tap: next */
            if (btn_pressed(BTN_KEY1_GPIO)) {
                int action = key1_tap_type();
                if (action == 2) {
                    gui_menu_navigate(&s_menu, 1);
                } else {
                    gui_menu_navigate(&s_menu, -1);
                }
            }

            /* KEY2 – single tap: enter / start selected option */
            if (btn_pressed(BTN_KEY2_GPIO)) {
                wait_release(BTN_KEY2_GPIO);
                handle_select(&s_menu);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
