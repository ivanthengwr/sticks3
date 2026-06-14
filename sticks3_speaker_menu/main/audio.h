#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * I2S audio engine for StickS3.
 *
 * ESP32-S3 acts as I2S MASTER:
 *   MCLK → GPIO18  (ES8311 master clock, 256×Fs)
 *   BCLK → GPIO17
 *   LRCK → GPIO15
 *   DOUT → GPIO14  (ESP32 TX → ES8311 DAC, playback)
 *   DIN  → GPIO16  (ES8311 ADC → ESP32 RX, recording)
 *
 * Format: 16 kHz, 16-bit, Mono (I2S standard / Philips).
 * Buffer : allocated from PSRAM (8 MB available on StickS3).
 */

/* ── Tone playback ───────────────────────────────────────────────────────── */
esp_err_t audio_init(void);
esp_err_t audio_warmup_output(void);
esp_err_t audio_play_tone(uint16_t freq_hz, uint32_t duration_ms, uint8_t vol_pct);
esp_err_t audio_play_scale(uint8_t vol_pct);   /* C-major ascending scale   */
esp_err_t audio_play_pcm(const int16_t *samples, size_t sample_count, uint8_t vol_pct);
esp_err_t audio_stop(void);

/* ── Mic recording / playback ────────────────────────────────────────────── */
typedef void (*audio_record_tick_cb_t)(uint8_t seconds_left);
typedef void (*audio_record_done_cb_t)(bool ok);

esp_err_t audio_record_start(uint32_t duration_ms);   /* blocking capture    */
void      audio_record_start_async(uint32_t duration_ms,
                                   audio_record_tick_cb_t tick_cb,
                                   audio_record_done_cb_t done_cb);
bool      audio_record_busy(void);
void      audio_play_recording_done_voice(uint8_t vol_pct);
void      audio_play_welcome_stackup_voice(uint8_t vol_pct);
esp_err_t audio_playback_recorded(uint8_t vol_pct);   /* plays back buffer  */

bool      audio_is_playing(void);
bool      audio_has_recording(void);
esp_err_t audio_deinit(void);
