#include "audio.h"
#include "pin_config.h"
#include "es8311.h"
#include "m5pm1.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include <math.h>
#include <string.h>

static const char *TAG = "audio";

/* ── I2S handles (shared with es8311 wrapper) ────────────────────────────── */
static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static bool s_initialised = false;
static bool s_playing     = false;

i2s_chan_handle_t audio_get_tx_chan(void) { return s_tx_chan; }
i2s_chan_handle_t audio_get_rx_chan(void) { return s_rx_chan; }

/* ── Recording buffer (PSRAM) ────────────────────────────────────────────── */
#define REC_MAX_SECONDS  5
#define BYTES_PER_SAMPLE 2
#define REC_BUF_BYTES    (AUDIO_SAMPLE_RATE * BYTES_PER_SAMPLE * REC_MAX_SECONDS)
#define REC_WARMUP_MS    80
#define REC_MIC_GAIN_DB  24.0f
#define REC_PLAY_GAIN_Q8 512

static int16_t *s_rec_buf     = NULL;
static size_t   s_rec_samples = 0;
static bool     s_has_rec     = false;

/* ── Tone synthesis buffer ──────────────────────────────────────────────── */
#define TONE_BUF_SAMPLES 512
static int16_t s_tone_buf[TONE_BUF_SAMPLES];
static int16_t s_rec_chunk[TONE_BUF_SAMPLES];

/* ── I2S channel creation (called BEFORE es8311_init) ────────────────────── */
esp_err_t audio_init(void)
{
    if (s_initialised) return ESP_OK;

    /* PSRAM recording buffer */
    s_rec_buf = heap_caps_malloc(REC_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_rec_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed for rec buf (%d bytes)", REC_BUF_BYTES);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Recording buffer: %d bytes in PSRAM", REC_BUF_BYTES);

    /* Create I2S channels (do NOT enable — esp_codec_dev does that on open) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_LRCK_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_DIN_GPIO,
            .invert_flags = {0},
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));

    s_initialised = true;
    ESP_LOGI(TAG, "I2S channels created – 16 kHz 16-bit mono on MCLK=GPIO%d", I2S_MCLK_GPIO);
    return ESP_OK;
}

static void apply_recording_playback_gain(int16_t *buf, size_t samples, uint8_t vol_pct)
{
    if (vol_pct > 100) vol_pct = 100;
    for (size_t i = 0; i < samples; i++) {
        int32_t sample = buf[i];
        sample = (sample * REC_PLAY_GAIN_Q8 * vol_pct) / (256 * 100);
        if (sample > INT16_MAX) {
            sample = INT16_MAX;
        } else if (sample < INT16_MIN) {
            sample = INT16_MIN;
        }
        buf[i] = (int16_t)sample;
    }
}

/* ── Recording cleanup: remove DC/rumble and tame very low-level hiss ────── */
static void clean_recording_chunk(int16_t *buf, size_t samples, int32_t *prev_x, int32_t *prev_y)
{
    for (size_t i = 0; i < samples; i++) {
        int32_t x = buf[i];
        int32_t y = x - *prev_x + ((*prev_y * 995) / 1000);
        *prev_x = x;
        *prev_y = y;

        if (y > INT16_MAX) {
            y = INT16_MAX;
        } else if (y < INT16_MIN) {
            y = INT16_MIN;
        }
        if (y > -64 && y < 64) {
            y = 0;
        }
        buf[i] = (int16_t)y;
    }
}

/* ── Tone synthesis ──────────────────────────────────────────────────────── */
esp_err_t audio_play_tone(uint16_t freq_hz, uint32_t duration_ms, uint8_t vol_pct)
{
    if (!s_initialised) return ESP_ERR_INVALID_STATE;

    esp_codec_dev_handle_t dev = es8311_codec_dev_get();
    if (!dev) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "audio_play_tone: freq=%u Hz, dur=%u ms, vol=%u%%", freq_hz, duration_ms, vol_pct);

    m5pm1_speaker_enable(true);
    esp_codec_dev_set_out_vol(dev, (float)vol_pct);

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = AUDIO_SAMPLE_RATE,
        .channel         = 1,
        .channel_mask    = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
    };
    int ret = esp_codec_dev_open(dev, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", ret);
        return ESP_FAIL;
    }

    s_playing = true;

    const float two_pi = 6.283185307f;
    float phase    = 0.0f;
    float phase_inc = two_pi * freq_hz / AUDIO_SAMPLE_RATE;

    uint32_t total_samples = (uint32_t)(AUDIO_SAMPLE_RATE * duration_ms / 1000);
    uint32_t written_total = 0;

    while (written_total < total_samples && s_playing) {
        uint32_t chunk = total_samples - written_total;
        if (chunk > TONE_BUF_SAMPLES) chunk = TONE_BUF_SAMPLES;

        for (uint32_t i = 0; i < chunk; i++) {
            s_tone_buf[i] = (int16_t)(sinf(phase) * 0x7FFF);
            phase += phase_inc;
            if (phase >= two_pi) phase -= two_pi;
        }

        ret = esp_codec_dev_write(dev, s_tone_buf, chunk * BYTES_PER_SAMPLE);
        if (ret != 0) {
            ESP_LOGE(TAG, "esp_codec_dev_write failed: %d", ret);
            break;
        }
        written_total += chunk;
    }

    s_playing = false;
    esp_codec_dev_close(dev);
    return ESP_OK;
}

/* ── C-major scale ───────────────────────────────────────────────────────── */
esp_err_t audio_play_scale(uint8_t vol_pct)
{
    static const uint16_t notes[] = {262,294,330,349,392,440,494,523};
    for (int i = 0; i < 8; i++) {
        audio_play_tone(notes[i], 250, vol_pct);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return ESP_OK;
}

/* ── Stop playback ───────────────────────────────────────────────────────── */
esp_err_t audio_stop(void)
{
    s_playing = false;
    return ESP_OK;
}

/* ── Microphone recording ────────────────────────────────────────────────── */
esp_err_t audio_record_start(uint32_t duration_ms)
{
    if (!s_initialised || !s_rec_buf) return ESP_ERR_INVALID_STATE;
    esp_codec_dev_handle_t dev = es8311_codec_dev_get();
    if (!dev) return ESP_ERR_INVALID_STATE;

    uint32_t total_samples = (uint32_t)(AUDIO_SAMPLE_RATE * duration_ms / 1000);
    if (total_samples * BYTES_PER_SAMPLE > REC_BUF_BYTES) {
        total_samples = REC_BUF_BYTES / BYTES_PER_SAMPLE;
    }
    ESP_LOGI(TAG, "Recording %lu samples (~%lu ms)…",
             (unsigned long)total_samples,
             (unsigned long)(total_samples * 1000 / AUDIO_SAMPLE_RATE));

    s_has_rec = false;
    s_rec_samples = 0;
    m5pm1_speaker_enable(false);
    esp_codec_dev_set_in_gain(dev, REC_MIC_GAIN_DB);
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = AUDIO_SAMPLE_RATE,
        .channel         = 1,
        .channel_mask    = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
    };
    int ret = esp_codec_dev_open(dev, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_open for record failed: %d", ret);
        m5pm1_speaker_enable(true);
        return ESP_FAIL;
    }

    uint32_t warmup_samples = (AUDIO_SAMPLE_RATE * REC_WARMUP_MS) / 1000;
    while (warmup_samples > 0) {
        uint32_t chunk = warmup_samples > TONE_BUF_SAMPLES ? TONE_BUF_SAMPLES : warmup_samples;
        ret = esp_codec_dev_read(dev, s_rec_chunk, chunk * BYTES_PER_SAMPLE);
        if (ret != 0) {
            ESP_LOGW(TAG, "Warmup read failed: %d", ret);
            break;
        }
        warmup_samples -= chunk;
    }

    int32_t prev_x = 0;
    int32_t prev_y = 0;
    while (s_rec_samples < total_samples) {
        uint32_t remaining = total_samples - s_rec_samples;
        uint32_t chunk = remaining > TONE_BUF_SAMPLES ? TONE_BUF_SAMPLES : remaining;
        ret = esp_codec_dev_read(dev, s_rec_chunk, chunk * BYTES_PER_SAMPLE);
        if (ret != 0) {
            ESP_LOGE(TAG, "esp_codec_dev_read failed at sample %zu: %d", s_rec_samples, ret);
            break;
        }
        clean_recording_chunk(s_rec_chunk, chunk, &prev_x, &prev_y);
        memcpy(s_rec_buf + s_rec_samples, s_rec_chunk, chunk * BYTES_PER_SAMPLE);
        s_rec_samples += chunk;
    }

    s_has_rec = (s_rec_samples > 0 && ret == 0);
    ESP_LOGI(TAG, "Recorded %zu/%lu samples (ret=%d)", s_rec_samples, (unsigned long)total_samples, ret);
    esp_codec_dev_close(dev);
    m5pm1_speaker_enable(true);
    return s_has_rec ? ESP_OK : ESP_FAIL;
}

/* ── Playback of recorded buffer ─────────────────────────────────────────── */
esp_err_t audio_playback_recorded(uint8_t vol_pct)
{
    if (!s_has_rec || !s_rec_buf || s_rec_samples == 0) {
        ESP_LOGW(TAG, "No recording available");
        return ESP_ERR_INVALID_STATE;
    }
    esp_codec_dev_handle_t dev = es8311_codec_dev_get();
    if (!dev) return ESP_ERR_INVALID_STATE;

    m5pm1_speaker_enable(true);
    esp_codec_dev_set_out_vol(dev, (float)vol_pct);

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = AUDIO_SAMPLE_RATE,
        .channel         = 1,
        .channel_mask    = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
    };
    int ret = esp_codec_dev_open(dev, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_open for playback failed: %d", ret);
        return ESP_FAIL;
    }

    s_playing = true;

    int16_t tmp[TONE_BUF_SAMPLES];
    size_t  remaining = s_rec_samples;
    size_t  offset    = 0;

    while (remaining > 0 && s_playing) {
        size_t chunk = remaining < TONE_BUF_SAMPLES ? remaining : TONE_BUF_SAMPLES;
        memcpy(tmp, s_rec_buf + offset, chunk * BYTES_PER_SAMPLE);
        apply_recording_playback_gain(tmp, chunk, vol_pct);
        ret = esp_codec_dev_write(dev, tmp, chunk * BYTES_PER_SAMPLE);
        if (ret != 0) break;
        offset    += chunk;
        remaining -= chunk;
    }

    s_playing = false;
    esp_codec_dev_close(dev);
    return ESP_OK;
}

/* ── Status queries ──────────────────────────────────────────────────────── */
bool audio_is_playing(void)    { return s_playing;  }
bool audio_has_recording(void) { return s_has_rec;  }

/* ── Deinit ──────────────────────────────────────────────────────────────── */
esp_err_t audio_deinit(void)
{
    if (!s_initialised) return ESP_OK;
    audio_stop();
    if (s_tx_chan) { i2s_del_channel(s_tx_chan); s_tx_chan = NULL; }
    if (s_rx_chan) { i2s_del_channel(s_rx_chan); s_rx_chan = NULL; }
    if (s_rec_buf) { heap_caps_free(s_rec_buf); s_rec_buf = NULL; }
    s_initialised = false;
    return ESP_OK;
}
