#pragma once
#include <stdint.h>
#include "esp_err.h"

/*
 * M5Stack Unit Watering (SKU U101)
 * Capacitive soil moisture sensor + 5 W pump.
 *
 * Grove wiring (HY2.0-4P):
 *   black  = GND
 *   red    = 5 V
 *   yellow = PUMP_EN  → WATERING_PUMP_GPIO  (digital output, active-high)
 *   white  = ADC_OUT  → WATERING_ADC_GPIO   (analog input)
 *
 * ADC note (ESP32-S3):
 *   Raw counts are 12-bit (0-4095). Higher value = drier soil (less capacitance).
 *   Threshold is configurable via WATERING_DRY_THRESHOLD in pin_config.h.
 */

esp_err_t watering_init(void);

/* Read the moisture sensor.  Returns raw 12-bit ADC count (0–4095). */
int       watering_read_raw(void);

/* Pump control. */
void      watering_pump_on(void);
void      watering_pump_off(void);

/*
 * Non-blocking demo — runs in its own FreeRTOS task.
 * watering_demo_start() spawns the task; watering_demo_stop() signals it to
 * quit (pump is turned off immediately on stop).  The status callback is
 * called from the demo task so keep it lightweight (just update a string and
 * redraw the status bar — no blocking calls).
 */
typedef void (*watering_status_cb_t)(const char *msg);
void watering_demo_start(watering_status_cb_t cb);
void watering_demo_stop(void);
bool watering_demo_running(void);
