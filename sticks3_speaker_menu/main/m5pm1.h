#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * M5PM1 (FY32L020F15U6) power-management IC driver.
 * I2C address : 0x6E
 * Shared bus  : SDA=GPIO47, SCL=GPIO48
 *
 * Responsibilities in this demo:
 *   • Initialise I2C bus (shared with ES8311 / BMI270)
 *   • Control AW8737 speaker-amp SHDN via PYG3_SPK_Pulse PWM output
 *
 * NOTE: The FY32L020F15U6 is a custom M5Stack PMIC. The register
 *       map below is derived from schematic analysis (K150 v0.6) and
 *       M5Stack open-source library inspection. Verify against the
 *       official FY32L020F15U6 datasheet if available.
 */

esp_err_t m5pm1_init(void);
esp_err_t m5pm1_speaker_enable(bool en);
esp_err_t m5pm1_read_reg(uint8_t reg, uint8_t *val);
esp_err_t m5pm1_write_reg(uint8_t reg, uint8_t val);
i2c_master_bus_handle_t m5pm1_get_i2c_bus(void);

#ifdef __cplusplus
}
#endif
