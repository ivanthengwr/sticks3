#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include "pin_config.h"
#include "watering.h"
#include "sticks_pm1.h"
#include "st7789.h"
#include "es8311.h"
#include "audio.h"
#include "voice/demo_done_voice.h"

static const char *TAG = "watering";

static adc_oneshot_unit_handle_t s_adc_handle;

#define WATERING_ADC_UNIT    ADC_UNIT_1
#define WATERING_ADC_CHANNEL ADC_CHANNEL_9

#define PUMP_LEDC_TIMER      LEDC_TIMER_0
#define PUMP_LEDC_CHANNEL    LEDC_CHANNEL_0
#define PUMP_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define PUMP_PWM_FREQ_HZ     2000
#define PUMP_RAMP_STEPS      4
#define PUMP_RAMP_STEP_MS    5
#define PUMP_GROVE_SETTLE_MS 150
#define PUMP_POWER_SETTLE_MS 80
#define WATERING_DONE_VOL_PCT  60

static bool s_pwm_ready   = false;
static bool s_power_shed  = false;
static bool s_grove_power = false;

static volatile bool        s_demo_running = false;
static volatile bool        s_demo_stop    = false;
static watering_status_cb_t s_demo_cb      = NULL;
static TaskHandle_t         s_demo_task    = NULL;

static esp_err_t pump_pwm_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = PUMP_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = PUMP_LEDC_TIMER,
        .freq_hz         = PUMP_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = WATERING_PUMP_GPIO,
        .speed_mode = PUMP_LEDC_MODE,
        .channel    = PUMP_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = PUMP_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ret = ledc_channel_config(&ch_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    s_pwm_ready = true;
    return ESP_OK;
}

static void pump_set_duty_pct(uint8_t pct)
{
    if (!s_pwm_ready || pct > 100) {
        return;
    }
    uint32_t duty = (uint32_t)pct * 255U / 100U;
    ledc_set_duty(PUMP_LEDC_MODE, PUMP_LEDC_CHANNEL, duty);
    ledc_update_duty(PUMP_LEDC_MODE, PUMP_LEDC_CHANNEL);
}

static bool demo_delay(uint32_t ms)
{
    uint32_t elapsed = 0;
    while (elapsed < ms) {
        if (s_demo_stop) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }
    return false;
}

static void watering_power_shed(void)
{
    if (s_power_shed) {
        return;
    }
    audio_stop();
    es8311_mute(true);
    m5pm1_speaker_enable(false);
    st7789_set_backlight(false);
    vTaskDelay(pdMS_TO_TICKS(PUMP_POWER_SETTLE_MS));
    s_power_shed = true;
}

static void watering_power_restore(void)
{
    if (!s_power_shed) {
        return;
    }
    st7789_set_backlight(true);
    audio_warmup_output();
    s_power_shed = false;
}

static esp_err_t watering_grove_power_on(void)
{
    if (s_grove_power) {
        return ESP_OK;
    }
    esp_err_t ret = m5pm1_grove_5v_enable(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Grove 5V enable failed");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(PUMP_GROVE_SETTLE_MS));
    s_grove_power = true;
    return ESP_OK;
}

static void watering_grove_power_off(void)
{
    if (!s_grove_power) {
        return;
    }
    m5pm1_grove_5v_enable(false);
    s_grove_power = false;
}

esp_err_t watering_init(void)
{
    esp_err_t ret = pump_pwm_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Pump PWM init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    pump_set_duty_pct(0);

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = WATERING_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ret = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_oneshot_config_channel(s_adc_handle, WATERING_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Watering unit ready (pump GPIO%d PWM, ADC GPIO%d)",
             WATERING_PUMP_GPIO, WATERING_ADC_GPIO);
    return ESP_OK;
}

int watering_read_raw(void)
{
    int raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc_handle, WATERING_ADC_CHANNEL, &raw);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(ret));
        return -1;
    }
    return raw;
}

void watering_pump_on(void)
{
    for (int step = 1; step <= PUMP_RAMP_STEPS; step++) {
        uint8_t pct = (uint8_t)((step * WATERING_PUMP_RUN_DUTY_PCT) / PUMP_RAMP_STEPS);
        pump_set_duty_pct(pct);
        vTaskDelay(pdMS_TO_TICKS(PUMP_RAMP_STEP_MS));
    }
}

void watering_pump_off(void)
{
    pump_set_duty_pct(0);
    vTaskDelay(pdMS_TO_TICKS(15));
}

static bool pump_run_pulsed(uint32_t total_ms)
{
    uint32_t hold_ms = WATERING_PUMP_HOLD_MS;
    if (hold_ms >= total_ms) {
        hold_ms = 0;
    }
    uint32_t pulse_ms = total_ms - hold_ms;

    ESP_LOGI(TAG, "Pump pulse %u%% / %u ms on, %u ms off (%u ms) + hold %u%% / %u ms",
             WATERING_PUMP_RUN_DUTY_PCT, WATERING_PULSE_ON_MS,
             WATERING_PULSE_OFF_MS, (unsigned)pulse_ms,
             WATERING_PUMP_HOLD_DUTY_PCT, (unsigned)hold_ms);

    uint32_t elapsed = 0;
    while (elapsed < pulse_ms) {
        if (s_demo_stop) {
            watering_pump_off();
            return true;
        }

        watering_pump_on();
        if (demo_delay(WATERING_PULSE_ON_MS)) {
            watering_pump_off();
            return true;
        }
        elapsed += WATERING_PULSE_ON_MS;

        watering_pump_off();
        if (elapsed >= pulse_ms) {
            break;
        }
        if (demo_delay(WATERING_PULSE_OFF_MS)) {
            return true;
        }
        elapsed += WATERING_PULSE_OFF_MS;
    }

    if (hold_ms > 0 && !s_demo_stop) {
        for (int step = 1; step <= PUMP_RAMP_STEPS && !s_demo_stop; step++) {
            uint8_t pct = (uint8_t)((step * WATERING_PUMP_HOLD_DUTY_PCT) / PUMP_RAMP_STEPS);
            pump_set_duty_pct(pct);
            vTaskDelay(pdMS_TO_TICKS(PUMP_RAMP_STEP_MS));
        }
        if (demo_delay(hold_ms)) {
            watering_pump_off();
            return true;
        }
    }

    watering_pump_off();
    return false;
}

static void demo_cleanup(void)
{
    pump_set_duty_pct(0);
    watering_grove_power_off();
    watering_power_restore();
}

static void watering_play_done_sound(void)
{
    if (s_demo_stop) {
        return;
    }
    es8311_set_volume(WATERING_DONE_VOL_PCT);
    audio_play_pcm(DEMO_DONE_VOICE_PCM, DEMO_DONE_VOICE_SAMPLES, WATERING_DONE_VOL_PCT);
}

static void demo_task(void *arg)
{
    char msg[32];
    ESP_LOGI(TAG, "Watering demo start (%d cycles, USB-safe pulse mode)",
             WATERING_DEMO_CYCLES);

    watering_power_shed();

    /* Latch Grove 5 V once — toggling DCDC mid-demo resets the board on USB power. */
    if (watering_grove_power_on() != ESP_OK) {
        if (s_demo_cb) s_demo_cb("Grove 5V fail");
        demo_cleanup();
        s_demo_running = false;
        s_demo_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    for (int cycle = 1; cycle <= WATERING_DEMO_CYCLES; cycle++) {
        if (s_demo_stop) break;

        int raw = watering_read_raw();

        snprintf(msg, sizeof(msg), "%d/%d raw:%d", cycle, WATERING_DEMO_CYCLES, raw);
        if (s_demo_cb) s_demo_cb(msg);
        ESP_LOGI(TAG, "Cycle %d/%d  raw=%d", cycle, WATERING_DEMO_CYCLES, raw);
        if (demo_delay(WATERING_MEASURE_MS)) break;

        snprintf(msg, sizeof(msg), "%d/%d Pump pulse", cycle, WATERING_DEMO_CYCLES);
        if (s_demo_cb) s_demo_cb(msg);

        if (pump_run_pulsed(WATERING_PUMP_ON_MS)) {
            break;
        }

        snprintf(msg, sizeof(msg), "%d/%d Pump done", cycle, WATERING_DEMO_CYCLES);
        if (s_demo_cb) s_demo_cb(msg);

        if (cycle < WATERING_DEMO_CYCLES && demo_delay(WATERING_RECOVERY_MS)) {
            break;
        }
    }

    demo_cleanup();
    if (!s_demo_stop) {
        watering_play_done_sound();
    }
    if (s_demo_cb) s_demo_cb(s_demo_stop ? "Demo stopped" : "Demo done");
    ESP_LOGI(TAG, "Watering demo %s", s_demo_stop ? "stopped" : "complete");

    s_demo_running = false;
    s_demo_task    = NULL;
    vTaskDelete(NULL);
}

void watering_demo_start(watering_status_cb_t cb)
{
    if (s_demo_running) return;
    s_demo_cb      = cb;
    s_demo_stop    = false;
    s_demo_running = true;
    xTaskCreate(demo_task, "watering_demo", 4096, NULL, 5, &s_demo_task);
}

void watering_demo_stop(void)
{
    if (!s_demo_running) return;
    s_demo_stop = true;
    pump_set_duty_pct(0);
    demo_cleanup();
}

bool watering_demo_running(void)
{
    return s_demo_running;
}
