/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"
#include "esp_check.h"
#include "example_config.h"

static const char *TAG = "i2s_es8311";
static const char err_reason[][30] = {"input param is invalid",
                                      "operation timeout"
                                     };
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;

/* Import music file as buffer */
#if CONFIG_EXAMPLE_MODE_MUSIC
extern const uint8_t music_pcm_start[] asm("_binary_canon_pcm_start");
extern const uint8_t music_pcm_end[]   asm("_binary_canon_pcm_end");
#endif

/* M5PM1 Power Setup Function (self-contained New API)
 * Creates its own temporary I2C bus on GPIO 47/48, sends all power commands,
 * then fully tears down the bus so the ES8311 driver can safely claim the pins.
 * Must be called FIRST in app_main(), before I2S or ES8311 initialisation.
 */
static esp_err_t m5pm1_new_api_power_setup(void) {
    ESP_LOGI("M5PM1", "Configuring M5PM1 using NEW I2C API...");

    /* 1. Create a temporary master bus on the shared pins */
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = 47,
        .scl_io_num = 48,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    /* Recover bus in case SDA/SCL are stuck low after a prior crash */
    esp_err_t rst_ret = i2c_master_bus_reset(bus_handle);
    if (rst_ret != ESP_OK) {
        ESP_LOGW("M5PM1", "Bus reset returned 0x%x (non-fatal)", rst_ret);
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 2. Probe for the M5PM1 at its confirmed 7-bit address 0x6E */
    uint8_t target_addr = 0x6E;
    if (i2c_master_probe(bus_handle, target_addr, 200) != ESP_OK) {
        ESP_LOGE("M5PM1", "M5PM1 not at 0x6E. Check scan output above for the correct address.");
        i2c_del_master_bus(bus_handle);
        return ESP_FAIL;
    }
    ESP_LOGI("M5PM1", "M5PM1 confirmed at 0x%02X", target_addr);

    /* 3. Register M5PM1 device */
    i2c_device_config_t m5pm1_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = target_addr,
        .scl_speed_hz    = 100000,
    };
    i2c_master_dev_handle_t m5pm1_handle;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &m5pm1_cfg, &m5pm1_handle));

    /* 4. Send power-on command sequence (timeout -1 = wait until complete) */
    uint8_t pwr_data[]  = {0x06, 0x1F};   /* Enable 5V boost output */
    ESP_ERROR_CHECK(i2c_master_transmit(m5pm1_handle, pwr_data,  sizeof(pwr_data),  -1));

    /* Configure G2 AND G3 as push-pull (clear bits 2+3 from 0x1F default).
     * G3 must be push-pull so the 0x53 pulse register can drive it without floating. */
    uint8_t drv_data[]  = {0x13, 0x13};
    ESP_ERROR_CHECK(i2c_master_transmit(m5pm1_handle, drv_data,  sizeof(drv_data),  -1));

    /* Set ONLY G2 as a manual output - G3 direction is taken over by register 0x53 */
    uint8_t mode_data[] = {0x10, 0x04};
    ESP_ERROR_CHECK(i2c_master_transmit(m5pm1_handle, mode_data, sizeof(mode_data), -1));

    /* Drive G2 HIGH to power the ES8311 codec */
    uint8_t out_data[]  = {0x11, 0x04};
    ESP_ERROR_CHECK(i2c_master_transmit(m5pm1_handle, out_data,  sizeof(out_data),  -1));

    /* Let the 5V and 3.3V rails stabilize before waking the amplifier */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Wake the AW8737 using the M5PM1 hardware pulse macro (register 0x53).
     * 0xA3 = Refresh + 1 pulse + GPIO3. The M5PM1 sends the required 1-wire
     * pulse sequence then automatically holds G3 HIGH to keep the amp active. */
    uint8_t pulse_data[] = {0x53, 0xA3};
    ESP_ERROR_CHECK(i2c_master_transmit(m5pm1_handle, pulse_data, sizeof(pulse_data), -1));
    ESP_LOGI("M5PM1", "AW8737 wake pulse sent via register 0x53");

    /* 5. Tear down the temporary bus so the ES8311 driver can claim the pins */
    ESP_ERROR_CHECK(i2c_master_bus_rm_device(m5pm1_handle));
    ESP_ERROR_CHECK(i2c_del_master_bus(bus_handle));

    ESP_LOGI("M5PM1", "M5PM1 power setup complete! Bus released.");
    return ESP_OK;
}

static esp_err_t es8311_codec_init(void)
{
    /* Initialize the ES8311's own I2C bus (pins now free after M5PM1 teardown) */
    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    i2c_master_bus_config_t i2c_mst_cfg = {
        .i2c_port = I2C_NUM,
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_cfg, &i2c_bus_handle));

    /* Create control interface with I2C bus handle */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus_handle,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if);

    /* Create data interface with I2S bus handle */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM,
        .rx_handle = rx_handle,
        .tx_handle = tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if);

    /* Create ES8311 interface handle */
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    assert(gpio_if);
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false,
        .use_mclk = false,       /* M5StickS3: ES8311 derives clock from BCLK, no MCLK pin */
        .pa_pin = EXAMPLE_PA_CTRL_IO,
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = EXAMPLE_MCLK_MULTIPLE,
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    assert(es8311_if);

    /* Create the top codec handle with ES8311 interface handle and data interface */
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = es8311_if,
        .data_if = data_if,
    };
    esp_codec_dev_handle_t codec_handle = esp_codec_dev_new(&dev_cfg);
    assert(codec_handle);

    /* Specify the sample configurations and open the device */
    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = EXAMPLE_SAMPLE_RATE,
    };
    if (esp_codec_dev_open(codec_handle, &sample_cfg) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Open codec device failed");
        return ESP_FAIL;
    }

    /* Set the initial volume and gain */
    if (esp_codec_dev_set_out_vol(codec_handle, EXAMPLE_VOICE_VOLUME) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "set output volume failed");
        return ESP_FAIL;
    }
#if CONFIG_EXAMPLE_MODE_ECHO
    if (esp_codec_dev_set_in_gain(codec_handle, EXAMPLE_MIC_GAIN) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "set input gain failed");
        return ESP_FAIL;
    }
#endif
    return ESP_OK;
}

static esp_err_t i2s_driver_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 1023;
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = -1,          /* M5StickS3: no physical MCLK wire to ES8311 */
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = EXAMPLE_MCLK_MULTIPLE;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    return ESP_OK;
}

#if CONFIG_EXAMPLE_MODE_MUSIC
static void i2s_music(void *args)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_write = 0;
    uint8_t *data_ptr = (uint8_t *)music_pcm_start;

    /* (Optional) Disable TX channel and preload the data before enabling the TX channel,
     * so that the valid data can be transmitted immediately */
    ESP_ERROR_CHECK(i2s_channel_disable(tx_handle));
    ESP_ERROR_CHECK(i2s_channel_preload_data(tx_handle, data_ptr, music_pcm_end - data_ptr, &bytes_write));
    data_ptr += bytes_write;  // Move forward the data pointer

    /* Enable the TX channel */
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    while (1) {
        /* Write music to earphone */
        ret = i2s_channel_write(tx_handle, data_ptr, music_pcm_end - data_ptr, &bytes_write, portMAX_DELAY);
        if (ret != ESP_OK) {
            /* Since we set timeout to 'portMAX_DELAY' in 'i2s_channel_write'
               so you won't reach here unless you set other timeout value,
               if timeout detected, it means write operation failed. */
            ESP_LOGE(TAG, "[music] i2s write failed, %s", err_reason[ret == ESP_ERR_TIMEOUT]);
            abort();
        }
        if (bytes_write > 0) {
            ESP_LOGI(TAG, "[music] i2s music played, %d bytes are written.", bytes_write);
        } else {
            ESP_LOGE(TAG, "[music] i2s music play failed.");
            abort();
        }
        data_ptr = (uint8_t *)music_pcm_start;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

#else
static void i2s_echo(void *args)
{
    int *mic_data = malloc(EXAMPLE_RECV_BUF_SIZE);
    if (!mic_data) {
        ESP_LOGE(TAG, "[echo] No memory for read data buffer");
        abort();
    }
    esp_err_t ret = ESP_OK;
    size_t bytes_read = 0;
    size_t bytes_write = 0;
    ESP_LOGI(TAG, "[echo] Echo start");

    while (1) {
        memset(mic_data, 0, EXAMPLE_RECV_BUF_SIZE);
        /* Read sample data from mic */
        ret = i2s_channel_read(rx_handle, mic_data, EXAMPLE_RECV_BUF_SIZE, &bytes_read, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[echo] i2s read failed, %s", err_reason[ret == ESP_ERR_TIMEOUT]);
            abort();
        }
        /* Write sample data to earphone */
        ret = i2s_channel_write(tx_handle, mic_data, EXAMPLE_RECV_BUF_SIZE, &bytes_write, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[echo] i2s write failed, %s", err_reason[ret == ESP_ERR_TIMEOUT]);
            abort();
        }
        if (bytes_read != bytes_write) {
            ESP_LOGW(TAG, "[echo] %d bytes read but only %d bytes are written", bytes_read, bytes_write);
        }
    }
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
    printf("i2s es8311 codec example start\n-----------------------------\n");

    /* Step 1: MUST BE FIRST - Power up M5PM1 (5V boost + codec + amplifier wake)
     * Uses a self-contained temporary I2C bus that is fully deleted before
     * the ES8311 driver claims the same pins. */
    if (m5pm1_new_api_power_setup() != ESP_OK) {
        ESP_LOGE(TAG, "M5PM1 power setup failed; aborting to avoid speaker buzz");
        abort();
    }

    /* Step 3: Initialize I2S */
    if (i2s_driver_init() != ESP_OK) {
        ESP_LOGE(TAG, "i2s driver init failed");
        abort();
    } else {
        ESP_LOGI(TAG, "i2s driver init success");
    }

    /* Step 4: Initialize ES8311 Codec (power is already on, pins are free) */
    if (es8311_codec_init() != ESP_OK) {
        ESP_LOGE(TAG, "es8311 codec init failed");
        abort();
    } else {
        ESP_LOGI(TAG, "es8311 codec init success");
    }
    
    /* Step 4: Volume is set to 70% inside es8311_codec_init via EXAMPLE_VOICE_VOLUME
     * (keep below 75% to prevent battery crashes) */

    /* Step 5: Play Music / Start Audio Processing */
#if CONFIG_EXAMPLE_MODE_MUSIC
    /* Play a piece of music in music mode */
    xTaskCreate(i2s_music, "i2s_music", 4096, NULL, 5, NULL);
#else
    /* Echo the sound from MIC in echo mode */
    xTaskCreate(i2s_echo, "i2s_echo", 8192, NULL, 5, NULL);
#endif
}
