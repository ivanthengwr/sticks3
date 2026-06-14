#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * StickS3 PMIC wrapper (FY32L020F15U6 via m5stack/m5pm1 component).
 * I2C address : 0x6E
 * Shared bus  : SDA=GPIO47, SCL=GPIO48
 *
 * Named sticks_pm1.h to avoid case-collision with M5PM1.h on macOS.
 */

esp_err_t m5pm1_init(void);
esp_err_t m5pm1_speaker_enable(bool en);
esp_err_t m5pm1_grove_5v_enable(bool en);
esp_err_t m5pm1_read_reg(uint8_t reg, uint8_t *val);
esp_err_t m5pm1_write_reg(uint8_t reg, uint8_t val);
i2c_master_bus_handle_t m5pm1_get_i2c_bus(void);

#ifdef __cplusplus
}
#endif
