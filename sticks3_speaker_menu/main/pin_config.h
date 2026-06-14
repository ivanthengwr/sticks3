#pragma once

/* ── LCD (ST7789P3, 135×240, SPI) ─────────────────────────────────────────── */
#define LCD_MOSI_GPIO   39
#define LCD_SCK_GPIO    40
#define LCD_CS_GPIO     41
#define LCD_DC_GPIO     45
#define LCD_RST_GPIO    21
#define LCD_BL_GPIO     38
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_SPI_FREQ_HZ (40 * 1000 * 1000)
#define LCD_WIDTH       135
#define LCD_HEIGHT      240
#define LCD_COL_OFFSET  52      /* ST7789 RAM column start offset for 135px */
#define LCD_ROW_OFFSET  40      /* ST7789 RAM row start offset for 240px    */

/* ── I2C (shared: ES8311 codec, BMI270 IMU, M5PM1 PMIC) ──────────────────── */
#define I2C_SDA_GPIO    47
#define I2C_SCL_GPIO    48
#define I2C_PORT        I2C_NUM_0
#define I2C_FREQ_HZ     400000

/* ── ES8311 mono audio codec (I2C + I2S) ─────────────────────────────────── */
#define ES8311_I2C_ADDR 0x18    /* CE pin pulled low → 0011 000x            */
#define I2S_MCLK_GPIO   18
#define I2S_BCLK_GPIO   17
#define I2S_LRCK_GPIO   15
#define I2S_DOUT_GPIO   14      /* ESP32 TX  → ES8311 DSDIN  (DAC playback) */
#define I2S_DIN_GPIO    16      /* ES8311 ASDOUT → ESP32 RX  (ADC record)   */
#define I2S_PORT        I2S_NUM_0
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_BITS_PER_SAMPLE 16
#define STARTUP_SPLASH_MS     3000
#define RECORD_DURATION_MS    9000

/* ── AW8737 power amplifier (enabled via M5PM1 PMIC) ─────────────────────── */
#define M5PM1_I2C_ADDR  0x6E
/* PYG3_SPK_Pulse (SHDN of AW8737) is driven by PMIC – see m5pm1.c          */

/* ── Buttons ──────────────────────────────────────────────────────────────── */
#define BTN_KEY1_GPIO   12      /* 1x prev / 2x next    (active-low)        */
#define BTN_KEY2_GPIO   11      /* select / confirm     (active-low)        */

/* ── IR (not used in this demo, defined for reference) ───────────────────── */
#define IR_TX_GPIO      46
#define IR_RX_GPIO      42

/* ── Unit Watering (M5Stack SKU U101, Grove HY2.0-4P) ────────────────────── */
/* Grove port wiring:  GND=black, 5V=red, PUMP_EN=yellow, ADC_OUT=white       */
/* Connect Grove cable to the StickS3 Grove header (Port A or breakout pins)  */
#define WATERING_ADC_GPIO   10       /* white wire – capacitive moisture ADC   */
#define WATERING_PUMP_GPIO  9        /* yellow wire – pump enable (active-high) */
#define WATERING_DRY_THRESHOLD  3000  /* raw ADC counts: above = soil dry      */
/* USB-safe pulse mode — long enough to move water on laptop USB power       */
#define WATERING_DEMO_CYCLES        3   /* sense→pump cycles in demo           */
#define WATERING_PUMP_ON_MS      6000   /* total pump window per cycle (ms)    */
#define WATERING_PUMP_RUN_DUTY_PCT  55  /* peak PWM % during pump pulses       */
#define WATERING_PULSE_ON_MS      500   /* pump ON slice within each pulse     */
#define WATERING_PULSE_OFF_MS     200   /* pump OFF slice (USB recovery)       */
#define WATERING_PUMP_HOLD_MS    2000   /* steady pull after pulses (ms)       */
#define WATERING_PUMP_HOLD_DUTY_PCT 48  /* PWM % during final hold phase       */
#define WATERING_MEASURE_MS       400   /* delay after sensor read (ms)        */
#define WATERING_RECOVERY_MS     1500   /* rest after pump OFF before next cycle */
