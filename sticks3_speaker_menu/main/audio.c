#include "audio.h"
#include "pin_config.h"
#include "es8311.h"
#include "sticks_pm1.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include "voice/recording_done_voice.h"
#include "voice/welcome_stackup_voice.h"
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
#define REC_MAX_SECONDS  9
#define BYTES_PER_SAMPLE 2
#define REC_BUF_BYTES    (AUDIO_SAMPLE_RATE * BYTES_PER_SAMPLE * REC_MAX_SECONDS)
#define REC_WARMUP_MS    120
#define REC_MIC_GAIN_DB  16.0f
#define REC_TARGET_PEAK  8500
#define REC_PLAY_FADE_IN_MS   350
#define REC_PLAY_FADE_OUT_MS  550
#define REC_PLAY_ZERO_HEAD_MS 40
#define REC_PLAY_ZERO_TAIL_MS 80
#define RECORD_DONE_VOL_PCT  65

static int16_t *s_rec_buf     = NULL;
static size_t   s_rec_samples = 0;
static bool     s_has_rec     = false;
static volatile bool s_record_busy = false;
static audio_record_tick_cb_t s_record_tick_cb = NULL;
static audio_record_done_cb_t s_record_done_cb = NULL;
static TaskHandle_t s_record_task = NULL;

/* ── Tone synthesis buffer ──────────────────────────────────────────────── */
#define TONE_BUF_SAMPLES   512
#define TONE_ATTACK_MIN_MS     50
#define TONE_ATTACK_PCT        12
#define TONE_RELEASE_MIN_MS    80
#define TONE_RELEASE_PCT       15
#define TONE_ZERO_HEAD_MS      20
#define TONE_ZERO_TAIL_MS      40
#define SCALE_NOTE_MS          250
#define SCALE_GAP_MS           20
#define PLAYBACK_PRIME_MS      60
#define PLAYBACK_BOOT_PRIME_MS 200
#define PLAYBACK_TAIL_MS       60
#define PLAYBACK_AMP_SETTLE_MS 10

static int16_t s_tone_buf[TONE_BUF_SAMPLES];
static int16_t s_rec_chunk[TONE_BUF_SAMPLES];
static bool s_playback_open = false;

static esp_err_t write_silence(esp_codec_dev_handle_t dev, uint32_t samples);
static void playback_shutdown(esp_codec_dev_handle_t dev);

static uint32_t tone_fade_samples(uint32_t duration_ms, uint32_t pct, uint32_t min_ms)
{
    uint32_t fade_ms = (duration_ms * pct) / 100;
    if (fade_ms < min_ms) {
        fade_ms = min_ms;
    }
    if (fade_ms >= duration_ms) {
        fade_ms = duration_ms / 2;
    }
    if (fade_ms == 0) {
        fade_ms = 1;
    }
    return (AUDIO_SAMPLE_RATE * fade_ms) / 1000;
}

static uint32_t tone_attack_samples(uint32_t duration_ms)
{
    return tone_fade_samples(duration_ms, TONE_ATTACK_PCT, TONE_ATTACK_MIN_MS);
}

static uint32_t tone_release_samples(uint32_t duration_ms)
{
    return tone_fade_samples(duration_ms, TONE_RELEASE_PCT, TONE_RELEASE_MIN_MS);
}

static float tone_envelope(uint32_t idx, uint32_t attack, uint32_t release_start, uint32_t total)
{
    const float pi = 3.14159265f;

    if (attack > 1 && idx < attack) {
        float t = (float)idx / (float)(attack - 1);
        return 0.5f * (1.0f - cosf(pi * t));
    }
    if (attack == 1 && idx == 0) {
        return 0.0f;
    }
    if (idx >= release_start) {
        uint32_t rel = total - release_start;
        if (rel <= 1) {
            return 0.0f;
        }
        float t = (float)(idx - release_start) / (float)(rel - 1);
        if (t > 1.0f) {
            t = 1.0f;
        }
        return 0.5f * (1.0f + cosf(pi * t));
    }
    return 1.0f;
}

static esp_codec_dev_sample_info_t playback_fs(void)
{
    return (esp_codec_dev_sample_info_t) {
        .sample_rate     = AUDIO_SAMPLE_RATE,
        .channel         = 1,
        .channel_mask    = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
    };
}

static esp_err_t playback_open(esp_codec_dev_handle_t dev, uint8_t vol_pct)
{
    esp_codec_dev_sample_info_t fs = playback_fs();
    uint32_t prime_ms = s_playback_open ? PLAYBACK_PRIME_MS : PLAYBACK_BOOT_PRIME_MS;

    es8311_mute(true);
    m5pm1_speaker_enable(false);

    if (!s_playback_open) {
        esp_codec_dev_set_out_vol(dev, (float)vol_pct);
        if (esp_codec_dev_open(dev, &fs) != 0) {
            es8311_mute(false);
            return ESP_FAIL;
        }
        s_playback_open = true;
    } else {
        esp_codec_dev_set_out_vol(dev, (float)vol_pct);
    }

    s_playing = true;
    if (write_silence(dev, (AUDIO_SAMPLE_RATE * prime_ms) / 1000) != ESP_OK) {
        s_playing = false;
        return ESP_FAIL;
    }
    s_playing = false;

    m5pm1_speaker_enable(true);
    vTaskDelay(pdMS_TO_TICKS(PLAYBACK_AMP_SETTLE_MS));
    es8311_mute(false);
    return ESP_OK;
}

static void playback_close(esp_codec_dev_handle_t dev)
{
    if (!s_playback_open) {
        s_playing = false;
        return;
    }

    s_playing = true;
    write_silence(dev, (AUDIO_SAMPLE_RATE * PLAYBACK_TAIL_MS) / 1000);
    s_playing = false;

    es8311_mute(true);
    vTaskDelay(pdMS_TO_TICKS(PLAYBACK_AMP_SETTLE_MS));
    m5pm1_speaker_enable(false);
}

static void playback_shutdown(esp_codec_dev_handle_t dev)
{
    if (!dev || !s_playback_open) {
        s_playing = false;
        return;
    }

    playback_close(dev);
    esp_codec_dev_close(dev);
    s_playback_open = false;
    s_playing = false;
    es8311_mute(false);
}

static esp_err_t write_pcm_chunk(esp_codec_dev_handle_t dev, int16_t *samples, uint32_t count)
{
    uint32_t offset = 0;
    while (offset < count && s_playing) {
        uint32_t chunk = count - offset;
        if (chunk > TONE_BUF_SAMPLES) {
            chunk = TONE_BUF_SAMPLES;
        }
        int ret = esp_codec_dev_write(dev, samples + offset, chunk * BYTES_PER_SAMPLE);
        if (ret != 0) {
            ESP_LOGE(TAG, "esp_codec_dev_write failed: %d", ret);
            return ESP_FAIL;
        }
        offset += chunk;
    }
    return ESP_OK;
}

static esp_err_t write_silence(esp_codec_dev_handle_t dev, uint32_t samples)
{
    memset(s_tone_buf, 0, sizeof(s_tone_buf));
    uint32_t written = 0;
    while (written < samples && s_playing) {
        uint32_t chunk = samples - written;
        if (chunk > TONE_BUF_SAMPLES) {
            chunk = TONE_BUF_SAMPLES;
        }
        esp_err_t ret = write_pcm_chunk(dev, s_tone_buf, chunk);
        if (ret != ESP_OK) {
            return ret;
        }
        written += chunk;
    }
    return ESP_OK;
}

static esp_err_t write_tone(esp_codec_dev_handle_t dev, uint16_t freq_hz,
                            uint32_t duration_ms, uint8_t vol_pct, bool envelope)
{
    const float two_pi = 6.283185307f;
    float phase = 0.0f;
    float phase_inc = two_pi * freq_hz / AUDIO_SAMPLE_RATE;
    float vol_scale = (float)vol_pct / 100.0f;

    uint32_t total_samples = (uint32_t)(AUDIO_SAMPLE_RATE * duration_ms / 1000);
    uint32_t attack_samples = 0;
    uint32_t release_samples = 0;
    uint32_t release_start = total_samples;

    if (envelope) {
        attack_samples = tone_attack_samples(duration_ms);
        release_samples = tone_release_samples(duration_ms);
        if (attack_samples + release_samples >= total_samples) {
            attack_samples = total_samples / 6;
            release_samples = (total_samples * 2) / 5;
        }
        release_start = total_samples - release_samples;
    }

    if (envelope && s_playing) {
        esp_err_t head_ret = write_silence(dev, (AUDIO_SAMPLE_RATE * TONE_ZERO_HEAD_MS) / 1000);
        if (head_ret != ESP_OK) {
            return head_ret;
        }
    }

    uint32_t written_total = 0;
    while (written_total < total_samples && s_playing) {
        uint32_t chunk = total_samples - written_total;
        if (chunk > TONE_BUF_SAMPLES) {
            chunk = TONE_BUF_SAMPLES;
        }

        for (uint32_t i = 0; i < chunk; i++) {
            uint32_t sample_idx = written_total + i;
            float amp = envelope
                ? tone_envelope(sample_idx, attack_samples, release_start, total_samples)
                : 1.0f;
            s_tone_buf[i] = (int16_t)(sinf(phase) * 0x7FFF * amp * vol_scale);
            phase += phase_inc;
            if (phase >= two_pi) {
                phase -= two_pi;
            }
        }

        esp_err_t ret = write_pcm_chunk(dev, s_tone_buf, chunk);
        if (ret != ESP_OK) {
            return ret;
        }
        written_total += chunk;
    }

    if (envelope && s_playing) {
        return write_silence(dev, (AUDIO_SAMPLE_RATE * TONE_ZERO_TAIL_MS) / 1000);
    }

    return ESP_OK;
}

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

esp_err_t audio_warmup_output(void)
{
    esp_codec_dev_handle_t dev;

    if (!s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_playback_open) {
        return ESP_OK;
    }

    dev = es8311_codec_dev_get();
    if (!dev) {
        return ESP_ERR_INVALID_STATE;
    }

    es8311_mute(true);
    m5pm1_speaker_enable(false);
    esp_codec_dev_set_out_vol(dev, 0.0f);

    esp_codec_dev_sample_info_t fs = playback_fs();
    if (esp_codec_dev_open(dev, &fs) != 0) {
        es8311_mute(false);
        return ESP_FAIL;
    }

    s_playback_open = true;
    s_playing = true;
    esp_err_t ret = write_silence(dev, (AUDIO_SAMPLE_RATE * PLAYBACK_BOOT_PRIME_MS) / 1000);
    s_playing = false;
    if (ret != ESP_OK) {
        playback_shutdown(dev);
        return ret;
    }

    ESP_LOGI(TAG, "I2S playback path warmed");
    return ESP_OK;
}

static int32_t clamp_i16(int32_t v)
{
    if (v > INT16_MAX) {
        return INT16_MAX;
    }
    if (v < INT16_MIN) {
        return INT16_MIN;
    }
    return v;
}

static void process_recording_buffer(size_t samples)
{
    if (samples == 0) {
        return;
    }

    int64_t sum = 0;
    int32_t peak = 0;

    for (size_t i = 0; i < samples; i++) {
        sum += s_rec_buf[i];
    }
    int32_t mean = (int32_t)(sum / (int64_t)samples);

    int32_t prev_x = 0;
    int32_t prev_y = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t x = (int32_t)s_rec_buf[i] - mean;
        int32_t y = x - prev_x + ((prev_y * 996) / 1000);
        prev_x = x;
        prev_y = y;
        y = clamp_i16(y);
        s_rec_buf[i] = (int16_t)y;

        int32_t abs_y = y >= 0 ? y : -y;
        if (abs_y > peak) {
            peak = abs_y;
        }
    }

    if (peak < 200) {
        memset(s_rec_buf, 0, samples * BYTES_PER_SAMPLE);
        return;
    }

    for (size_t i = 0; i < samples; i++) {
        int32_t v = ((int32_t)s_rec_buf[i] * REC_TARGET_PEAK) / peak;
        s_rec_buf[i] = (int16_t)clamp_i16(v);
    }

    ESP_LOGI(TAG, "Recording processed: mean=%ld peak=%ld target=%d",
             (long)mean, (long)peak, REC_TARGET_PEAK);
}

static float recording_playback_envelope(uint32_t idx, uint32_t total)
{
    const float pi = 3.14159265f;
    uint32_t fade_in = (AUDIO_SAMPLE_RATE * REC_PLAY_FADE_IN_MS) / 1000;
    uint32_t fade_out = (AUDIO_SAMPLE_RATE * REC_PLAY_FADE_OUT_MS) / 1000;

    if (fade_in + fade_out >= total) {
        fade_in = total / 5;
        fade_out = total / 5;
    }
    uint32_t fade_out_start = total - fade_out;

    if (fade_in > 1 && idx < fade_in) {
        float t = (float)idx / (float)(fade_in - 1);
        return 0.5f * (1.0f - cosf(pi * t));
    }
    if (fade_out > 1 && idx >= fade_out_start) {
        float t = (float)(idx - fade_out_start) / (float)(fade_out - 1);
        return 0.5f * (1.0f + cosf(pi * t));
    }
    return 1.0f;
}

static void apply_recording_envelope(int16_t *buf, size_t chunk, size_t offset, size_t total)
{
    for (size_t i = 0; i < chunk; i++) {
        float amp = recording_playback_envelope((uint32_t)(offset + i), (uint32_t)total);
        int32_t v = (int32_t)((float)buf[i] * amp);
        buf[i] = (int16_t)clamp_i16(v);
    }
}

/* ── Tone synthesis ──────────────────────────────────────────────────────── */
esp_err_t audio_play_tone(uint16_t freq_hz, uint32_t duration_ms, uint8_t vol_pct)
{
    if (!s_initialised) return ESP_ERR_INVALID_STATE;

    esp_codec_dev_handle_t dev = es8311_codec_dev_get();
    if (!dev) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "audio_play_tone: freq=%u Hz, dur=%u ms, vol=%u%%", freq_hz, duration_ms, vol_pct);

    if (playback_open(dev, vol_pct) != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        return ESP_FAIL;
    }

    s_playing = true;
    esp_err_t ret = write_tone(dev, freq_hz, duration_ms, vol_pct, true);
    playback_close(dev);
    return ret;
}

/* ── C-major scale ───────────────────────────────────────────────────────── */
esp_err_t audio_play_scale(uint8_t vol_pct)
{
    if (!s_initialised) return ESP_ERR_INVALID_STATE;

    esp_codec_dev_handle_t dev = es8311_codec_dev_get();
    if (!dev) return ESP_ERR_INVALID_STATE;

    static const uint16_t notes[] = {262, 294, 330, 349, 392, 440, 494, 523};
    const uint32_t gap_samples = (AUDIO_SAMPLE_RATE * SCALE_GAP_MS) / 1000;

    ESP_LOGI(TAG, "audio_play_scale: %u notes, vol=%u%%", (unsigned)(sizeof(notes) / sizeof(notes[0])), vol_pct);

    if (playback_open(dev, vol_pct) != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed for scale");
        return ESP_FAIL;
    }

    s_playing = true;
    esp_err_t ret = ESP_OK;

    for (int i = 0; i < 8 && s_playing; i++) {
        ret = write_tone(dev, notes[i], SCALE_NOTE_MS, vol_pct, true);
        if (ret != ESP_OK) {
            break;
        }
        if (i < 7) {
            ret = write_silence(dev, gap_samples);
            if (ret != ESP_OK) {
                break;
            }
        }
    }

    playback_close(dev);
    return ret;
}

/* ── Stop playback ───────────────────────────────────────────────────────── */
esp_err_t audio_stop(void)
{
    s_playing = false;
    playback_shutdown(es8311_codec_dev_get());
    return ESP_OK;
}

/* ── PCM sample playback ─────────────────────────────────────────────────── */
esp_err_t audio_play_pcm(const int16_t *samples, size_t sample_count, uint8_t vol_pct)
{
    if (!s_initialised || !samples || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_codec_dev_handle_t dev = es8311_codec_dev_get();
    if (!dev) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "audio_play_pcm: %u samples (~%u ms), vol=%u%%",
             (unsigned)sample_count,
             (unsigned)(sample_count * 1000 / AUDIO_SAMPLE_RATE),
             vol_pct);

    if (playback_open(dev, vol_pct) != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open for PCM failed");
        return ESP_FAIL;
    }

    s_playing = true;

    int16_t tmp[TONE_BUF_SAMPLES];
    size_t remaining = sample_count;
    size_t offset = 0;
    int ret = 0;

    while (remaining > 0 && s_playing) {
        size_t chunk = remaining < TONE_BUF_SAMPLES ? remaining : TONE_BUF_SAMPLES;
        memcpy(tmp, samples + offset, chunk * BYTES_PER_SAMPLE);
        ret = esp_codec_dev_write(dev, tmp, chunk * BYTES_PER_SAMPLE);
        if (ret != 0) break;
        offset += chunk;
        remaining -= chunk;
    }

    playback_close(dev);
    return ESP_OK;
}

/* ── Microphone recording ────────────────────────────────────────────────── */
static esp_err_t record_capture(uint32_t duration_ms, audio_record_tick_cb_t tick_cb)
{
    esp_codec_dev_handle_t dev = es8311_codec_dev_get();
    if (!s_initialised || !s_rec_buf || !dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t total_samples = (uint32_t)(AUDIO_SAMPLE_RATE * duration_ms / 1000);
    uint32_t samples_per_sec = AUDIO_SAMPLE_RATE;
    uint32_t next_tick_sample = samples_per_sec;
    uint8_t seconds_left = (uint8_t)(duration_ms / 1000);

    if (total_samples * BYTES_PER_SAMPLE > REC_BUF_BYTES) {
        total_samples = REC_BUF_BYTES / BYTES_PER_SAMPLE;
    }

    ESP_LOGI(TAG, "Recording %lu samples (~%lu ms)…",
             (unsigned long)total_samples,
             (unsigned long)(total_samples * 1000 / AUDIO_SAMPLE_RATE));

    s_has_rec = false;
    s_rec_samples = 0;
    playback_shutdown(dev);
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

    if (tick_cb) {
        tick_cb(seconds_left);
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

    while (s_rec_samples < total_samples) {
        uint32_t remaining = total_samples - s_rec_samples;
        uint32_t chunk = remaining > TONE_BUF_SAMPLES ? TONE_BUF_SAMPLES : remaining;
        int read_ret = -1;

        for (int attempt = 0; attempt < 3; attempt++) {
            read_ret = esp_codec_dev_read(dev, s_rec_chunk, chunk * BYTES_PER_SAMPLE);
            if (read_ret == 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        ret = read_ret;
        if (ret != 0) {
            ESP_LOGE(TAG, "esp_codec_dev_read failed at sample %zu: %d", s_rec_samples, ret);
            break;
        }
        memcpy(s_rec_buf + s_rec_samples, s_rec_chunk, chunk * BYTES_PER_SAMPLE);
        s_rec_samples += chunk;

        if (tick_cb) {
            while (s_rec_samples >= next_tick_sample && next_tick_sample <= total_samples) {
                if (seconds_left > 0) {
                    seconds_left--;
                    tick_cb(seconds_left);
                }
                next_tick_sample += samples_per_sec;
            }
        }
    }

    if (s_rec_samples > 0 && ret == 0) {
        process_recording_buffer(s_rec_samples);
    }

    s_has_rec = (s_rec_samples > 0 && ret == 0);
    ESP_LOGI(TAG, "Recorded %zu/%lu samples (ret=%d)", s_rec_samples, (unsigned long)total_samples, ret);
    esp_codec_dev_close(dev);
    s_playback_open = false;

    if (tick_cb) {
        tick_cb(0);
    }

    return s_has_rec ? ESP_OK : ESP_FAIL;
}

static void record_task(void *arg)
{
    uint32_t duration_ms = (uint32_t)(uintptr_t)arg;
    esp_err_t ret = record_capture(duration_ms, s_record_tick_cb);

    audio_warmup_output();

    if (s_record_done_cb) {
        s_record_done_cb(ret == ESP_OK);
    }

    s_record_busy = false;
    s_record_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_record_start(uint32_t duration_ms)
{
    if (s_record_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = record_capture(duration_ms, NULL);
    audio_warmup_output();
    return ret;
}

void audio_record_start_async(uint32_t duration_ms,
                              audio_record_tick_cb_t tick_cb,
                              audio_record_done_cb_t done_cb)
{
    if (s_record_busy || !s_initialised) {
        return;
    }

    s_record_busy = true;
    s_record_tick_cb = tick_cb;
    s_record_done_cb = done_cb;
    xTaskCreate(record_task, "audio_rec", 4096, (void *)(uintptr_t)duration_ms, 7, &s_record_task);
}

bool audio_record_busy(void)
{
    return s_record_busy;
}

void audio_play_recording_done_voice(uint8_t vol_pct)
{
    es8311_set_volume(vol_pct);
    audio_play_pcm(RECORDING_DONE_VOICE_PCM, RECORDING_DONE_VOICE_SAMPLES, vol_pct);
}

void audio_play_welcome_stackup_voice(uint8_t vol_pct)
{
    es8311_set_volume(vol_pct);
    audio_play_pcm(WELCOME_STACKUP_VOICE_PCM, WELCOME_STACKUP_VOICE_SAMPLES, vol_pct);
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

    if (playback_open(dev, vol_pct) != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open for playback failed");
        return ESP_FAIL;
    }

    s_playing = true;
    if (write_silence(dev, (AUDIO_SAMPLE_RATE * REC_PLAY_ZERO_HEAD_MS) / 1000) != ESP_OK) {
        playback_close(dev);
        return ESP_FAIL;
    }

    int16_t tmp[TONE_BUF_SAMPLES];
    size_t  remaining = s_rec_samples;
    size_t  offset    = 0;
    esp_err_t ret = ESP_OK;

    while (remaining > 0 && s_playing) {
        size_t chunk = remaining < TONE_BUF_SAMPLES ? remaining : TONE_BUF_SAMPLES;
        memcpy(tmp, s_rec_buf + offset, chunk * BYTES_PER_SAMPLE);
        apply_recording_envelope(tmp, chunk, offset, s_rec_samples);
        if (write_pcm_chunk(dev, tmp, chunk) != ESP_OK) {
            ESP_LOGE(TAG, "Recorded playback write failed at sample %zu", offset);
            ret = ESP_FAIL;
            break;
        }
        offset    += chunk;
        remaining -= chunk;
    }

    if (s_playing && ret == ESP_OK) {
        if (write_silence(dev, (AUDIO_SAMPLE_RATE * REC_PLAY_ZERO_TAIL_MS) / 1000) != ESP_OK) {
            ret = ESP_FAIL;
        }
    }

    playback_close(dev);
    return ret;
}

/* ── Status queries ──────────────────────────────────────────────────────── */
bool audio_is_playing(void)    { return s_playing;  }
bool audio_has_recording(void) { return s_has_rec;  }

/* ── Deinit ──────────────────────────────────────────────────────────────── */
esp_err_t audio_deinit(void)
{
    if (!s_initialised) return ESP_OK;
    playback_shutdown(es8311_codec_dev_get());
    if (s_tx_chan) { i2s_del_channel(s_tx_chan); s_tx_chan = NULL; }
    if (s_rx_chan) { i2s_del_channel(s_rx_chan); s_rx_chan = NULL; }
    if (s_rec_buf) { heap_caps_free(s_rec_buf); s_rec_buf = NULL; }
    s_initialised = false;
    return ESP_OK;
}
