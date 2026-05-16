#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include "pin_config.h"
#include "watering.h"

static const char *TAG = "watering";

static adc_oneshot_unit_handle_t s_adc_handle;

/* ADC channel that maps to WATERING_ADC_GPIO (GPIO10 = ADC1_CH9 on ESP32-S3) */
#define WATERING_ADC_UNIT    ADC_UNIT_1
#define WATERING_ADC_CHANNEL ADC_CHANNEL_9   /* GPIO10 on ESP32-S3 */

esp_err_t watering_init(void)
{
    /* ── Pump GPIO ──────────────────────────────────────────────────────── */
    /* GPIO9 is a strapping pin on ESP32-S3 (JTAG_SEL). gpio_config() rejects
     * it via its validity mask in some IDF versions. Use the low-level API
     * directly — it applies no strapping-pin guard and is safe after boot. */
    gpio_set_direction(WATERING_PUMP_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(WATERING_PUMP_GPIO, GPIO_FLOATING);
    gpio_set_level(WATERING_PUMP_GPIO, 0);

    /* ── ADC ────────────────────────────────────────────────────────────── */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = WATERING_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
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

    ESP_LOGI(TAG, "Watering unit ready (pump GPIO%d, ADC GPIO%d)",
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
    gpio_set_level(WATERING_PUMP_GPIO, 1);
    ESP_LOGI(TAG, "Pump ON");
}

void watering_pump_off(void)
{
    gpio_set_level(WATERING_PUMP_GPIO, 0);
    ESP_LOGI(TAG, "Pump OFF");
}

/* ── Demo task ─────────────────────────────────────────────────────────────── */
static volatile bool          s_demo_running = false;
static volatile bool          s_demo_stop    = false;
static watering_status_cb_t   s_demo_cb      = NULL;
static TaskHandle_t           s_demo_task    = NULL;

/* Interruptible delay — returns true if stop was requested mid-wait. */
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

static void demo_task(void *arg)
{
    char msg[32];
    ESP_LOGI(TAG, "Watering demo start (%d cycles)", WATERING_DEMO_CYCLES);

    for (int cycle = 1; cycle <= WATERING_DEMO_CYCLES; cycle++) {
        if (s_demo_stop) break;

        int raw = watering_read_raw();
        snprintf(msg, sizeof(msg), "%d/%d raw:%d", cycle, WATERING_DEMO_CYCLES, raw);
        if (s_demo_cb) s_demo_cb(msg);
        ESP_LOGI(TAG, "Cycle %d/%d  raw=%d", cycle, WATERING_DEMO_CYCLES, raw);
        if (demo_delay(WATERING_MEASURE_MS)) break;

        snprintf(msg, sizeof(msg), "%d/%d Pump ON", cycle, WATERING_DEMO_CYCLES);
        if (s_demo_cb) s_demo_cb(msg);
        watering_pump_on();
        if (demo_delay(WATERING_PUMP_ON_MS)) {
            watering_pump_off();
            break;
        }
        watering_pump_off();

        snprintf(msg, sizeof(msg), "%d/%d Pump OFF", cycle, WATERING_DEMO_CYCLES);
        if (s_demo_cb) s_demo_cb(msg);
        if (demo_delay(500)) break;
    }

    watering_pump_off();
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
    /* pump and task cleanup happen inside demo_task once it sees the flag */
}

bool watering_demo_running(void)
{
    return s_demo_running;
}
