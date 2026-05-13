#pragma once
#include "esp_err.h"
#include "esp_codec_dev.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * ES8311 mono audio codec wrapper around Espressif esp_codec_dev component.
 * Sample rate : AUDIO_SAMPLE_RATE (16 kHz)
 * Bit depth   : 16-bit, I2S MSB (Left-Justified) format
 * MCLK ratio  : 256 × Fs = 4.096 MHz
 */

esp_err_t es8311_init(void);
esp_err_t es8311_start_dac(void);
esp_err_t es8311_set_volume(uint8_t vol_pct);
esp_err_t es8311_set_mic_gain(uint8_t db);
esp_err_t es8311_mute(bool mute);
esp_err_t es8311_deinit(void);

/* Used by audio.c to get the codec handle directly */
esp_codec_dev_handle_t es8311_codec_dev_get(void);
