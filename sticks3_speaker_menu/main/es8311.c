#include "es8311.h"
#include "pin_config.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sticks_pm1.h"

#include "esp_codec_dev.h"
#include "es8311_codec.h"
#include "audio_codec_data_if.h"
#include "audio_codec_ctrl_if.h"
#include "audio_codec_gpio_if.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "es8311";

/* These handles are shared with audio.c — exposed via helpers below */
static esp_codec_dev_handle_t s_codec_dev = NULL;
static const audio_codec_data_if_t *s_data_if = NULL;
static const audio_codec_ctrl_if_t *s_ctrl_if = NULL;
static const audio_codec_gpio_if_t *s_gpio_if = NULL;
static const audio_codec_if_t      *s_codec_if = NULL;

/* External hooks so audio.c can pass its I2S channel handles */
extern i2s_chan_handle_t audio_get_tx_chan(void);
extern i2s_chan_handle_t audio_get_rx_chan(void);

esp_codec_dev_handle_t es8311_codec_dev_get(void)
{
    return s_codec_dev;
}

static uint8_t es8311_detect_addr(void)
{
    const uint8_t addr_7bit[] = {
        ES8311_I2C_ADDR,
        ES8311_I2C_ADDR | 0x01,
    };
    i2c_master_bus_handle_t bus = m5pm1_get_i2c_bus();
    for (size_t i = 0; i < sizeof(addr_7bit); i++) {
        if (i2c_master_probe(bus, addr_7bit[i], 100) == ESP_OK) {
            ESP_LOGI(TAG, "ES8311 detected at 7-bit I2C address 0x%02X", addr_7bit[i]);
            return addr_7bit[i] << 1;
        }
    }
    ESP_LOGW(TAG, "ES8311 did not ACK 0x%02X or 0x%02X; trying default 0x%02X",
             addr_7bit[0], addr_7bit[1], addr_7bit[0]);
    return addr_7bit[0] << 1;
}

esp_err_t es8311_init(void)
{
    /* I2S data interface — auto picks up TX/RX from default I2S port */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_PORT,
        .tx_handle = audio_get_tx_chan(),
        .rx_handle = audio_get_rx_chan(),
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!s_data_if) {
        ESP_LOGE(TAG, "Failed to create I2S data interface");
        return ESP_FAIL;
    }

    /* I2C control interface — pass the bus handle created by m5pm1_init() */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr       = es8311_detect_addr(),  /* esp_codec_dev expects 8-bit addr */
        .bus_handle = m5pm1_get_i2c_bus(),
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!s_ctrl_if) {
        ESP_LOGE(TAG, "Failed to create I2C ctrl interface");
        return ESP_FAIL;
    }

    /* GPIO interface (not used here, but required) */
    s_gpio_if = audio_codec_new_gpio();

    /* ES8311 codec configuration — slave, MCLK from ESP32, no PA pin */
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if    = s_ctrl_if,
        .gpio_if    = s_gpio_if,
        .pa_pin     = -1,
        .use_mclk   = true,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
    };
    s_codec_if = es8311_codec_new(&es_cfg);
    if (!s_codec_if) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec");
        return ESP_FAIL;
    }

    /* esp_codec_dev handle for both record + playback */
    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = s_codec_if,
        .data_if  = s_data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_codec_dev) {
        ESP_LOGE(TAG, "Failed to create codec dev");
        return ESP_FAIL;
    }
    esp_codec_set_disable_when_closed(s_codec_dev, false);

    ESP_LOGI(TAG, "ES8311 init OK via esp_codec_dev (16 kHz, 16-bit, slave)");
    return ESP_OK;
}

esp_err_t es8311_start_dac(void)
{
    /* No-op: esp_codec_dev handles start in esp_codec_dev_open */
    return ESP_OK;
}

esp_err_t es8311_set_volume(uint8_t vol_pct)
{
    if (!s_codec_dev) return ESP_ERR_INVALID_STATE;
    if (vol_pct > 100) vol_pct = 100;
    /* esp_codec_dev_set_out_vol takes 0-100 directly */
    int ret = esp_codec_dev_set_out_vol(s_codec_dev, (float)vol_pct);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t es8311_set_mic_gain(uint8_t db)
{
    if (!s_codec_dev) return ESP_ERR_INVALID_STATE;
    int ret = esp_codec_dev_set_in_gain(s_codec_dev, (float)db);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t es8311_mute(bool mute)
{
    if (!s_codec_dev) return ESP_ERR_INVALID_STATE;
    int ret = esp_codec_dev_set_out_mute(s_codec_dev, mute);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t es8311_deinit(void)
{
    if (s_codec_dev) {
        esp_codec_dev_close(s_codec_dev);
        esp_codec_dev_delete(s_codec_dev);
        s_codec_dev = NULL;
    }
    return ESP_OK;
}
