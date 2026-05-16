#include "m5pm1.h"
#include "pin_config.h"

#include "M5PM1.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "m5pm1";

static i2c_master_bus_handle_t s_bus_handle = nullptr;
static M5PM1 s_pm1;
static bool s_pm1_ready = false;

static esp_err_t pm1_to_esp(m5pm1_err_t err)
{
    return err == M5PM1_OK ? ESP_OK : ESP_FAIL;
}

extern "C" i2c_master_bus_handle_t m5pm1_get_i2c_bus(void)
{
    return s_bus_handle;
}

extern "C" esp_err_t m5pm1_speaker_enable(bool en)
{
    if (!s_pm1_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = pm1_to_esp(s_pm1.gpioSetFunc(M5PM1_GPIO_NUM_3, M5PM1_GPIO_FUNC_GPIO));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = pm1_to_esp(s_pm1.gpioSetMode(M5PM1_GPIO_NUM_3, M5PM1_GPIO_MODE_OUTPUT));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = pm1_to_esp(s_pm1.gpioSetDrive(M5PM1_GPIO_NUM_3, M5PM1_GPIO_DRIVE_PUSHPULL));
    if (ret != ESP_OK) {
        return ret;
    }
    return pm1_to_esp(s_pm1.gpioSetOutput(M5PM1_GPIO_NUM_3, en));
}

extern "C" esp_err_t m5pm1_init(void)
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_PORT;
    bus_cfg.sda_io_num = (gpio_num_t)I2C_SDA_GPIO;
    bus_cfg.scl_io_num = (gpio_num_t)I2C_SCL_GPIO;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus_handle), TAG, "create I2C bus failed");

    ESP_RETURN_ON_ERROR(pm1_to_esp(s_pm1.begin(s_bus_handle, M5PM1_I2C_ADDR, I2C_FREQ_HZ)), TAG, "M5PM1 begin failed");
    s_pm1_ready = true;

    ESP_RETURN_ON_ERROR(pm1_to_esp(s_pm1.setLdoEnable(true)), TAG, "enable PMIC LDO rail failed");
    ESP_RETURN_ON_ERROR(pm1_to_esp(s_pm1.gpioSetFunc(M5PM1_GPIO_NUM_2, M5PM1_GPIO_FUNC_GPIO)), TAG, "configure L3B func failed");
    ESP_RETURN_ON_ERROR(pm1_to_esp(s_pm1.gpioSetMode(M5PM1_GPIO_NUM_2, M5PM1_GPIO_MODE_OUTPUT)), TAG, "configure L3B mode failed");
    ESP_RETURN_ON_ERROR(pm1_to_esp(s_pm1.gpioSetDrive(M5PM1_GPIO_NUM_2, M5PM1_GPIO_DRIVE_PUSHPULL)), TAG, "configure L3B driver failed");
    ESP_RETURN_ON_ERROR(pm1_to_esp(s_pm1.gpioSetOutput(M5PM1_GPIO_NUM_2, true)), TAG, "enable L3B failed");
    ESP_RETURN_ON_ERROR(m5pm1_speaker_enable(true), TAG, "enable speaker amp failed");

    /* Enable the Grove 5 V rail. On the StickS3 the PMIC routes Grove power
     * through the DCDC converter; BOOST_EN and DCDC_EN are both "hardware
     * dependent" per the register map so we enable all three paths. */
    ESP_RETURN_ON_ERROR(pm1_to_esp(s_pm1.setDcdcEnable(true)),     TAG, "enable Grove 5V DCDC failed");
    ESP_RETURN_ON_ERROR(pm1_to_esp(s_pm1.setBoostEnable(true)),    TAG, "enable Grove 5V boost failed");
    ESP_RETURN_ON_ERROR(pm1_to_esp(s_pm1.boostSetPowerHold(true)), TAG, "latch Grove 5V boost failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "I2C bus ready, speaker amp enabled, Grove 5V ON");
    return ESP_OK;
}

extern "C" esp_err_t m5pm1_write_reg(uint8_t reg, uint8_t val)
{
    (void)reg;
    (void)val;
    return ESP_OK;
}

extern "C" esp_err_t m5pm1_read_reg(uint8_t reg, uint8_t *val)
{
    (void)reg;
    if (val) {
        *val = 0;
    }
    return ESP_OK;
}
