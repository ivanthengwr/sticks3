# StickS3 Speaker Menu — Complete Beginner's Guide

This document explains **every detail** of this project: what the hardware is, how the software works, what every file does, and why things are done the way they are. No prior embedded programming experience is assumed.

---

## Table of Contents

1. [What is this project?](#1-what-is-this-project)
2. [The Hardware](#2-the-hardware)
3. [What is ESP-IDF?](#3-what-is-esp-idf)
4. [Project File Structure](#4-project-file-structure)
5. [How the Program Starts](#5-how-the-program-starts)
6. [The Menu System](#6-the-menu-system)
7. [The Display Driver — st7789.c](#7-the-display-driver--st7789c)
8. [The GUI Layer — gui.c](#8-the-gui-layer--guic)
9. [The Audio System — audio.c](#9-the-audio-system--audioc)
10. [The Audio Codec — es8311.c](#10-the-audio-codec--es8311c)
11. [The Power Management IC — m5pm1.cpp](#11-the-power-management-ic--m5pm1cpp)
12. [The Watering Unit — watering.c](#12-the-watering-unit--wateringc)
13. [Pin Configuration — pin_config.h](#13-pin-configuration--pin_configh)
14. [The Font — font.h](#14-the-font--fonth)
15. [Build System — CMakeLists.txt](#15-build-system--cmakeliststxt)
16. [Key Programming Concepts Used](#16-key-programming-concepts-used)
17. [How Everything Connects Together](#17-how-everything-connects-together)
18. [Glossary](#18-glossary)

---

## 1. What is this project?

This is firmware (the software that runs directly on a microcontroller chip) for the **M5Stack StickS3** — a small handheld device about the size of a USB drive with a colour screen, speaker, microphone, and buttons.

The firmware creates an interactive **menu** on the screen where you can:

| Menu Option | What it does |
|-------------|-------------|
| Tone A4     | Plays a 440 Hz musical note (the A above middle C) for 1 second |
| Tone C5     | Plays a 523 Hz musical note for 1 second |
| Scale       | Plays all 8 notes of the C-major musical scale |
| Record      | Records 3 seconds of audio from the built-in microphone |
| Playback    | Plays back the recorded audio through the speaker |
| Vol +       | Increases the speaker volume by 5% |
| Vol -       | Decreases the speaker volume by 5% |
| Stop        | Stops whatever audio is playing or the watering demo |
| Watering    | Runs a demonstration of the M5Stack Watering Unit (soil sensor + pump) |

**Buttons:**
- **KEY1** (single tap) — move selection **down** the menu
- **KEY1** (double tap within 300 ms) — move selection **up** the menu
- **KEY2** — execute the currently selected option

---

## 2. The Hardware

### The M5Stack StickS3

The StickS3 is a development board built around the **ESP32-S3** chip made by Espressif. Think of a development board as a chip plus all the support components (power regulation, connectors, displays) soldered onto one convenient board.

### The ESP32-S3 Chip

The brain of the device. Key facts:
- **Dual-core processor** running at 160 MHz — it has two independent CPUs that can run code simultaneously
- **512 KB of internal RAM** — fast memory for storing variables while the program runs
- **8 MB of external PSRAM** — extra RAM connected via a high-speed bus (called "Octal SPI"), used here for storing audio recordings
- **8 MB of flash storage** — where the firmware itself is stored permanently
- **45+ GPIO pins** — General Purpose Input/Output pins that can be configured as digital inputs, digital outputs, or connected to special hardware peripherals (SPI, I2C, I2S, ADC, etc.)

### GPIO — What it Means

GPIO stands for **General Purpose Input/Output**. Each GPIO pin is a physical metal pad on the chip that can:
- Be an **output**: the software sets it HIGH (3.3V) or LOW (0V) to control something (like turning on a pump)
- Be an **input**: the software reads whether it is HIGH or LOW (like detecting a button press)
- Be assigned to a **peripheral**: special hardware inside the chip (like SPI, I2C, I2S) takes control of the pin for a specific communication protocol

### Peripherals Used in this Project

A peripheral is a hardware module built into the chip. This project uses:

| Peripheral | What it is | Used for |
|------------|-----------|---------|
| **SPI** | Serial Peripheral Interface — sends data in one direction very fast | Driving the LCD display |
| **I2C** | Inter-Integrated Circuit — a 2-wire bus for controlling chips | Controlling the audio codec and power management IC |
| **I2S** | Inter-IC Sound — a protocol designed specifically for audio data | Sending/receiving audio samples to/from the ES8311 codec |
| **ADC** | Analog-to-Digital Converter — reads a voltage and gives a number | Reading the soil moisture sensor |

### The ST7789P3 Display

A 135×240 pixel colour LCD screen. It communicates over **SPI** and displays colour images using the **RGB565** colour format (16 bits per pixel: 5 bits red, 6 bits green, 5 bits blue).

### The ES8311 Audio Codec

A "codec" (coder/decoder) chip that:
- **Converts digital audio data → analog sound** (for the speaker)
- **Converts analog sound → digital audio data** (from the microphone)

The ESP32-S3 generates digital numbers representing sound waves. The ES8311 turns those numbers into electrical voltage fluctuations that make the speaker cone vibrate, creating sound.

### The AW8737 Power Amplifier

A small amplifier chip that boosts the audio signal from the ES8311 to a level strong enough to drive the speaker. It is controlled by the PMIC (see below) — the firmware enables it before playing audio and disables it when idle.

### The M5PM1 PMIC

PMIC stands for **Power Management IC**. It manages:
- Multiple power rails (different voltages for different parts of the circuit)
- The **3.3V rail** for the audio codec
- The **5V BOOST/GROVE rail** for the Grove port (needed to power the watering unit pump)
- The **speaker amplifier enable** signal

It is controlled over **I2C**.

### The M5Stack Watering Unit (SKU U101)

An external add-on connected via a Grove cable (4-wire connector: GND, 5V, signal, signal):
- **Capacitive soil moisture sensor** — measures how wet the soil is by detecting changes in electrical capacitance. More water = more capacitance = lower ADC reading
- **5W water pump** — a small motor-driven pump controlled by a digital ON/OFF signal

Grove wiring colours:
| Wire | Function |
|------|----------|
| Black | Ground (0V reference) |
| Red | 5V power supply (from the BOOST rail) |
| Yellow | Pump enable — GPIO9, set HIGH to run pump |
| White | Moisture ADC output — GPIO10, analog voltage read by ADC |

### Buttons

Two physical push buttons, both **active-low** (the pin reads 0V when pressed, 3.3V when not pressed, because of pull-up resistors connected to 3.3V):
- **KEY1** — GPIO12
- **KEY2** — GPIO11

---

## 3. What is ESP-IDF?

**ESP-IDF** (Espressif IoT Development Framework) is the official software development kit for ESP32 chips. It provides:

- **FreeRTOS** — a real-time operating system. Even though microcontrollers typically run one program, FreeRTOS lets you create multiple "tasks" (like threads) that appear to run simultaneously
- **Drivers** — ready-made code for controlling hardware peripherals (SPI, I2C, I2S, ADC, GPIO)
- **Components** — libraries for common tasks like managing memory, logging, and more
- **Build system** — tools to compile all your code into a single binary file you can flash to the chip

This project runs on **ESP-IDF v5.5.1**.

---

## 4. Project File Structure

```
sticks3_speaker_menu/
├── main/
│   ├── main.c          — Program entry point, menu logic, button handling
│   ├── gui.h / gui.c   — Menu drawing on the LCD
│   ├── st7789.h / st7789.c  — Low-level LCD display driver
│   ├── audio.h / audio.c    — I2S audio engine (tones, recording, playback)
│   ├── es8311.h / es8311.c  — ES8311 audio codec wrapper
│   ├── m5pm1.h / m5pm1.cpp  — Power management IC driver
│   ├── watering.h / watering.c  — Watering unit driver and demo
│   ├── pin_config.h    — All GPIO pin number definitions in one place
│   ├── font.h          — 5×7 pixel bitmap font for text rendering
│   └── CMakeLists.txt  — Tells the build system what files to compile
├── CMakeLists.txt      — Top-level build file
├── sdkconfig.defaults  — ESP-IDF configuration (PSRAM, flash size, etc.)
└── managed_components/ — Third-party libraries downloaded automatically
    ├── espressif__esp_codec_dev/  — Audio codec abstraction library
    ├── m5stack__m5pm1/            — M5PM1 PMIC library
    └── ...
```

**Why split into .h and .c files?**

- A `.h` file (header) is like a table of contents — it declares what functions exist and what they are called, without explaining how they work. Other files `#include` the header to know what they can call.
- A `.c` file contains the actual implementation — the full code of each function.
- This separation lets files use each other's functions without needing to see each other's full source code.

---

## 5. How the Program Starts

When the ESP32-S3 powers on, it runs `app_main()` in `main.c`. There is no `main()` function like in a desktop program — ESP-IDF calls `app_main()` for you inside a FreeRTOS task.

### Initialization Order in `app_main()`

The order matters because each step depends on the previous one:

```
1. m5pm1_init()     Creates the I2C bus, enables 3.3V and 5V rails,
                    enables the speaker amplifier

2. audio_init()     Allocates 160 KB of recording buffer in PSRAM,
                    creates the I2S TX (playback) and RX (recording)
                    channels — but does NOT enable them yet

3. es8311_init()    Connects esp_codec_dev to the I2S channels and
                    the ES8311 over I2C. This opens the codec and
                    enables the I2S channels.

4. st7789_init()    Sets up the SPI bus, creates a mutex for thread
                    safety, sends the init sequence to the LCD, fills
                    the screen black, turns on the backlight

5. buttons_init()   Configures GPIO11 and GPIO12 as inputs with
                    pull-up resistors

6. watering_init()  Configures GPIO9 as output (pump), sets up the
                    ADC for GPIO10 (moisture sensor)

7. gui_menu_draw()  Renders the full menu on screen for the first time

— Startup beep —    Plays two short tones to confirm the audio path works

— Main loop —       Polls buttons and responds to presses forever
```

### The Main Loop

After init, the program runs an infinite loop (`for (;;)`) that:
1. Checks if KEY1 is pressed
2. If yes, determines if it was a single tap (go down) or double-tap (go up), then redraws the changed menu rows
3. Checks if KEY2 is pressed
4. If yes, calls `handle_select()` to execute the chosen menu action
5. Waits 20 ms and repeats

This is called **polling** — the CPU constantly checks the state of inputs. It's simple but uses CPU time even when nothing is happening.

---

## 6. The Menu System

### Data Structure

The menu is represented by a `gui_menu_t` struct (a struct is a group of related variables bundled together):

```c
typedef struct {
    const char   *title;         // "StickS3" — shown in header
    gui_item_t    items[9];      // array of 9 menu items
    uint8_t       count;         // 9 — total number of items
    uint8_t       selected;      // index of currently highlighted item (0–8)
    uint8_t       vol_pct;       // current volume, 0–100
    const char   *status_str;    // short message shown at the bottom
} gui_menu_t;
```

Each `gui_item_t` holds:
```c
typedef struct {
    const char *label;   // text shown in the row, e.g. "Tone A4"
    uint16_t    accent;  // colour of the text in RGB565 format
} gui_item_t;
```

### Double-Tap Detection

When KEY1 is pressed, the code waits up to **300 ms** for a second press:
```
KEY1 pressed →
  wait_release()     — wait until finger lifts off
  start 300ms timer
  check every 10ms:
    if KEY1 pressed again within 300ms → double-tap → go UP
  if 300ms passes with no second press → single tap → go DOWN
```

The direction (+1 or -1) is added to the current selection. The modulo operator (`%`) makes it wrap: going down from item 8 wraps to item 0, and going up from item 0 wraps to item 8. The `+ count` before the modulo prevents underflow when going up from 0 on an unsigned integer.

### Button Debounce

Mechanical buttons don't make a clean electrical contact — they "bounce" (rapidly connect and disconnect many times in milliseconds). To ignore this noise, after detecting a press the code waits 50 ms and checks again. If it's still pressed, it counts as a real press.

---

## 7. The Display Driver — st7789.c

### SPI Communication

SPI (Serial Peripheral Interface) is a communication protocol that sends data over 4 wires:
- **MOSI** (Master Out Slave In) — data from ESP32 to LCD
- **SCK** (clock) — a timing signal so both sides agree when each bit is valid
- **CS** (Chip Select) — pulled LOW to tell the LCD "this data is for you"
- **DC** (Data/Command) — tells the LCD whether the byte being sent is a command (configure the chip) or data (pixel colours)

The LCD runs at **40 MHz** SPI clock — 40 million bits per second.

### RGB565 Colour Format

Each pixel is 16 bits (2 bytes):
- Bits 15–11: Red (5 bits, values 0–31)
- Bits 10–5: Green (6 bits, values 0–63)
- Bits 4–0: Blue (5 bits, values 0–31)

Green gets an extra bit because human eyes are most sensitive to green.

### The ST7789 Address Offset

The ST7789 chip's internal pixel memory is larger than the 135×240 screen. The actual visible pixels start at column 52 and row 40 inside the chip's RAM. Every time we set a window (a rectangular region to draw into), we add these offsets. This is why `LCD_COL_OFFSET 52` and `LCD_ROW_OFFSET 40` exist.

### Thread Safety Mutex

A **mutex** (mutual exclusion lock) is a tool that ensures only one task at a time can access a shared resource. Without it, if the main task and the watering demo task both try to write to the LCD simultaneously, they corrupt each other's SPI transactions — producing the `spi_device_polling_start: Cannot send polling transaction while the previous polling transaction is not terminated` error.

The mutex works like a bathroom key:
1. A task calls `LCD_LOCK()` — takes the key
2. It does its LCD writes
3. It calls `LCD_UNLOCK()` — puts the key back
4. If another task tries `LCD_LOCK()` while the key is taken, it waits

The mutex is created in `st7789_init()` using `xSemaphoreCreateMutex()` — a FreeRTOS function. It must be created **before** any drawing call, so it is placed at the very beginning of the init sequence.

An internal function `st7789_fill_rect_unsafe()` does the actual pixel writing without locking. Compound operations like `draw_rect()` (which draws 4 lines) take the lock once and call the unsafe version 4 times — this avoids the same task trying to take the same lock twice (which would deadlock).

---

## 8. The GUI Layer — gui.c

`gui.c` sits above `st7789.c`. While `st7789.c` knows about pixels and rectangles, `gui.c` knows about the menu layout and text.

### Screen Layout (portrait, 135×240 pixels)

```
┌─────────────────────┐  ← Y=0
│   HEADER (20px)     │  "StickS3" title, navy background
│─────────────────────│  ← Y=21
│   Item 0  (20px)    │
│   Item 1  (20px)    │
│   Item 2  (20px)    │
│   Item 3  (20px)    │  9 items × 20px = 180px
│   Item 4  (20px)    │
│   Item 5  (20px)    │
│   Item 6  (20px)    │
│   Item 7  (20px)    │
│   Item 8  (20px)    │
│─────────────────────│  ← Y=201
│   FOOTER (20px)     │  "K1:Next", volume bar
│─────────────────────│  ← Y=221
│   STATUS (16px)     │  Current action message, "K2:Sel"
└─────────────────────┘  ← Y=237
```

The 9-item layout was carefully calculated: 9 × 20px = 180px, which fits between the 20px header and the 36px footer+status area.

### Text Rendering

The font is a 5×7 bitmap font (each character is 5 columns × 7 rows of pixels). `gui_draw_char()` reads the bitmap for each character from `font.h` and draws it pixel by pixel using `st7789_draw_pixel()`. The `scale` parameter makes characters bigger by drawing each "pixel" as a `scale×scale` filled rectangle — scale 2 is used for menu items, making each character 10×14 pixels.

### Volume Bar

The volume bar is a simple horizontal rectangle. A fraction of it (`pct * width / 100`) is filled green, the rest is dark grey. A thin outline is drawn around it.

### Selective Redraw

Redrawing the entire screen on every button press would be slow (filling 135×240 pixels over SPI takes time). Instead, only the two affected menu rows are redrawn when navigating — the old selection row (to deselect it) and the new selection row (to highlight it).

---

## 9. The Audio System — audio.c

### What is I2S?

I2S (Inter-IC Sound) is a serial bus protocol designed for audio. It carries three signals:
- **MCLK** (Master Clock) — a high-frequency reference clock. Here it's 256 × 16000 Hz = 4.096 MHz
- **BCLK** (Bit Clock) — clocks each individual audio bit
- **LRCK** (Left-Right Clock, also called WS/Word Select) — toggles at the sample rate (16000 Hz) to indicate left or right channel. Since this is mono, it's just the frame sync.
- **DOUT** — digital audio data OUT from ESP32 to codec (playback)
- **DIN** — digital audio data IN from codec to ESP32 (recording)

### Audio Format

All audio in this project uses:
- **Sample rate**: 16,000 samples per second (16 kHz) — good enough for voice
- **Bit depth**: 16 bits per sample — each sample is a signed integer from −32768 to +32767 representing the amplitude (loudness) of the sound wave at that moment
- **Channels**: 1 (mono — one channel, not stereo)

### Sine Wave Tone Synthesis (DDS)

To generate a pure tone (like 440 Hz), the code uses **Direct Digital Synthesis**:

1. Calculate how much phase to advance per sample: `phase_inc = 2π × freq / sample_rate`
2. For each sample, compute `sin(phase) × 32767` and store as a 16-bit integer
3. Advance phase by `phase_inc`
4. When phase exceeds 2π, subtract 2π (wraps back to start of cycle)
5. Send the buffer of samples to the codec

The sine function produces a smooth wave that, when converted to sound, is a pure musical tone. The frequency of the tone equals how many complete cycles occur per second.

### Recording

Recording reverses the I2S direction:
1. Disable the speaker amplifier (prevents feedback)
2. Set the microphone gain to 24 dB (amplifies the quiet mic signal)
3. Discard the first 80 ms of audio (warmup — the codec needs time to stabilize)
4. Read chunks of 512 samples at a time into a buffer in PSRAM
5. Apply a **DC removal filter** (high-pass filter) to each chunk

### DC Removal Filter

Microphones often have a small constant offset (DC bias) in their output. This appears as a flat horizontal shift of the waveform. The high-pass filter removes it:

```c
y = x - prev_x + (prev_y × 0.995)
```

This is a **single-pole IIR (Infinite Impulse Response) high-pass filter**. It passes frequencies above ~0.5 Hz (all audible sound) while blocking the 0 Hz DC component. The coefficient 0.995 controls the cutoff frequency.

### Noise Gate

After filtering, any sample with absolute value less than 64 is set to zero:
```c
if (y > -64 && y < 64) y = 0;
```
This suppresses background hiss that the microphone picks up when the room is quiet.

### PSRAM Recording Buffer

The recording buffer is 160,000 bytes (160 KB) — enough for 5 seconds of 16-bit 16 kHz mono audio. This is allocated from **PSRAM** using `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` because it's too large to fit in the chip's internal RAM. PSRAM is slightly slower than internal RAM but has 8 MB of capacity.

### Playback Gain

When playing back a recording, a gain is applied to make it louder:
```c
sample = (sample × 512 × vol_pct) / (256 × 100)
```
The 512 is a fixed Q8 (fixed-point) gain of 2× amplification. Q8 means the value is scaled by 256 (2^8) to avoid using floating-point arithmetic, which is slower on embedded systems.

---

## 10. The Audio Codec — es8311.c

The ES8311 is controlled through the **esp_codec_dev** abstraction library from Espressif. This library provides a uniform interface for many different codec chips — you configure it once and then use generic `open`, `read`, `write`, `close` calls regardless of which chip is underneath.

### Layers of Abstraction

```
audio.c            ← your code calls: esp_codec_dev_open(), write(), read()
    ↓
esp_codec_dev      ← abstraction layer (handles codec-specific details)
    ↓
es8311_codec       ← ES8311-specific driver (register writes over I2C)
    ↓
audio_codec_i2c    ← I2C transport layer
    ↓
I2C bus            ← physical wires to ES8311 chip
```

### I2C Address Detection

The ES8311 can respond to two different I2C addresses (0x18 or 0x19) depending on how a pin is wired. During init, the code probes both addresses with `i2c_master_probe()` and uses whichever one responds. This makes the code robust even if the hardware wiring changes.

### Slave Mode

The ES8311 operates in **I2S slave mode** — it receives its clock signals (MCLK, BCLK, LRCK) from the ESP32, rather than generating them itself. The ESP32 is the I2S master, producing the clocks.

---

## 11. The Power Management IC — m5pm1.cpp

The file extension `.cpp` means this file is written in **C++**, not C. This is because the M5PM1 library from M5Stack is a C++ class. The `extern "C"` wrappers around the functions make them callable from C code (like `main.c`).

### What the PMIC Does in init

```
1. Creates the I2C master bus (shared by ES8311 and PMIC)
2. Initializes the M5PM1 chip at address 0x6E
3. Enables the 3.3V LDO rail (powers the audio codec)
4. Configures PMIC GPIO2 to HIGH (enables 3.3V_L3B rail for codec)
5. Configures PMIC GPIO3 to HIGH (enables AW8737 speaker amplifier)
6. Calls setDcdcEnable(true)    — enables 5V DCDC converter
7. Calls setBoostEnable(true)   — enables BOOST/GROVE 5V rail
8. Calls boostSetPowerHold(true)— latches the 5V rail ON permanently
```

### Why Three Calls for the 5V Rail?

The M5PM1 PMIC has two registers involved in powering the Grove port:
- **PWR_CFG register (0x06), bit 3** — `BOOST_EN`: momentarily enables the boost converter. Written by `setBoostEnable()`
- **PWR_CFG register (0x06), bit 1** — `DCDC_EN`: enables the 5V DCDC. Written by `setDcdcEnable()`
- **HOLD_CFG register (0x07), bit 6** — power hold: **latches** the boost ON so it stays on continuously. Written by `boostSetPowerHold()`

Without `boostSetPowerHold(true)`, the boost converter turns on briefly then the PMIC turns it off again when it re-evaluates its state. The hold bit tells it "keep this on permanently."

---

## 12. The Watering Unit — watering.c

### GPIO9 — The Strapping Pin Problem

GPIO9 on the ESP32-S3 is a **strapping pin** — it is sampled at power-on to configure boot behaviour (specifically, JTAG interface selection). Because of this, the ESP-IDF's `gpio_config()` function rejects it with an error in some versions.

The workaround: use the lower-level API that bypasses the validity check:
```c
gpio_set_direction(WATERING_PUMP_GPIO, GPIO_MODE_OUTPUT);
gpio_set_pull_mode(WATERING_PUMP_GPIO, GPIO_FLOATING);
gpio_set_level(WATERING_PUMP_GPIO, 0);
```
These functions write directly to the IO MUX (pin multiplexer) registers. By the time the firmware runs, the strapping values have already been latched at reset, so it's safe.

### ADC Channel Mapping

GPIO10 is connected to ADC1 **channel 9** on the ESP32-S3. This mapping is fixed in hardware — you cannot choose which ADC channel a GPIO uses. Using the wrong channel number (like channel 0 = GPIO1) would read a completely unrelated pin.

The ADC is configured for:
- **12-bit resolution**: outputs values 0–4095
- **12 dB attenuation**: allows reading voltages up to ~3.3V (full supply voltage range)

### The Demo Task (FreeRTOS Task)

The demo runs in its own **FreeRTOS task** — a separate thread of execution. This is critical because if the demo ran blocking (pausing the whole program) in the main loop, you couldn't press buttons to stop it mid-run.

```
Main task                    Demo task
─────────────────────        ──────────────────────────
polls KEY1, KEY2             reads moisture sensor
draws menu updates           waits 500ms (checking stop flag every 10ms)
runs handle_select()         turns pump ON
                             waits 3000ms (checking stop flag every 10ms)
                             turns pump OFF
                             repeats for 5 cycles
```

The **stop flag** (`s_demo_stop`) is a `volatile bool` shared between tasks. `volatile` tells the compiler not to cache this variable in a CPU register — always read it fresh from memory — because it can change from another task at any time.

The `demo_delay()` function checks `s_demo_stop` every 10 ms during waits, so pressing Stop responds within 10 ms.

### Demo Cycle Behaviour

Each of the 5 demo cycles:
1. Reads the moisture ADC value and shows it on the LCD status bar
2. Waits 500 ms
3. Turns the pump on and shows "Pump ON" on the LCD
4. Runs the pump for 3000 ms (3 seconds)
5. Turns the pump off and shows "Pump OFF"
6. Waits 500 ms before the next cycle

---

## 13. Pin Configuration — pin_config.h

All GPIO numbers are defined in one place so they can be changed without hunting through every file:

```c
// Display (SPI)
LCD_MOSI_GPIO   39   // Data to LCD
LCD_SCK_GPIO    40   // Clock
LCD_CS_GPIO     41   // Chip select
LCD_DC_GPIO     45   // Data/Command select
LCD_RST_GPIO    21   // Hardware reset
LCD_BL_GPIO     38   // Backlight enable

// I2C (shared bus)
I2C_SDA_GPIO    47   // Data line
I2C_SCL_GPIO    48   // Clock line

// I2S (audio)
I2S_MCLK_GPIO   18   // Master clock (256 × sample rate)
I2S_BCLK_GPIO   17   // Bit clock
I2S_LRCK_GPIO   15   // Left/right frame sync
I2S_DOUT_GPIO   14   // Data out → codec (playback)
I2S_DIN_GPIO    16   // Data in ← codec (recording)

// Buttons
BTN_KEY1_GPIO   12   // Navigate (active-low)
BTN_KEY2_GPIO   11   // Select (active-low)

// Watering unit (Grove port)
WATERING_ADC_GPIO    10   // Moisture sensor (ADC1_CH9)
WATERING_PUMP_GPIO    9   // Pump enable (active-high)
```

**Active-low** means the signal is 0V (logic LOW) when active (button pressed) and 3.3V (logic HIGH) when inactive (button not pressed). Pull-up resistors connected to 3.3V make the pin read HIGH by default; pressing the button connects it to GND (0V).

**Active-high** means the signal is 3.3V when active. The pump enable pin is HIGH to run the pump, LOW to stop it.

---

## 14. The Font — font.h

`font.h` contains a 5×7 bitmap font covering all 96 printable ASCII characters (space through tilde).

Each character is stored as 5 bytes — one byte per column. Each bit in a byte represents one pixel row (7 pixels high, 1 bit unused). For example, the letter 'A':

```
Column:  0    1    2    3    4
         .    .    X    .    .    bit 0 (top)
         .    X    .    X    .    bit 1
         X    .    .    .    X    bit 2
         X    X    X    X    X    bit 3
         X    .    .    .    X    bit 4
         X    .    .    .    X    bit 5
         .    .    .    .    .    bit 6
```

The rendering code iterates over columns and rows, drawing foreground colour where a bit is 1 and background colour where it is 0.

---

## 15. Build System — CMakeLists.txt

CMake is a build system — it figures out which files need to be compiled and in what order. ESP-IDF uses CMake.

### main/CMakeLists.txt

```cmake
idf_component_register(
    SRCS
        "main.c"       # all .c and .cpp source files to compile
        "st7789.c"
        "gui.c"
        "es8311.c"
        "m5pm1.cpp"
        "audio.c"
        "watering.c"
    INCLUDE_DIRS "."   # look for .h files in this directory
    REQUIRES
        driver             # ESP-IDF GPIO, SPI, I2C, I2S drivers
        esp_adc            # ESP-IDF ADC driver (for moisture sensor)
        esp_driver_i2c     # I2C master driver
        esp_timer          # timer functions
        freertos           # FreeRTOS tasks, mutexes, delays
        log                # ESP_LOGI, ESP_LOGE logging macros
        m5stack__m5pm1     # M5Stack PMIC library
        spi_flash          # flash storage
        espressif__esp_codec_dev  # audio codec abstraction
)
target_link_libraries(${COMPONENT_LIB} PRIVATE m)  # links the math library (for sinf())
```

### sdkconfig.defaults

This file sets ESP-IDF configuration options without requiring manual menuconfig:
- Enables the 8 MB PSRAM
- Sets the flash size to 8 MB
- Configures the FreeRTOS tick rate and stack sizes

---

## 16. Key Programming Concepts Used

### Structs
A way to group related variables under one name:
```c
typedef struct {
    const char *label;
    uint16_t    accent;
} gui_item_t;
```
You can then create variables of this type: `gui_item_t item = {"Tone A4", COLOR_CYAN};`

### Pointers
A variable that stores the memory address of another variable. Used extensively:
- `const char *label` — pointer to a string (array of characters)
- `int16_t *s_rec_buf` — pointer to the recording buffer in PSRAM
- Function pointers like `watering_status_cb_t cb` — a pointer to a function, allowing `watering.c` to call back into `main.c` to update the display

### `volatile` Keyword
Tells the compiler this variable can change at any time (from another task or interrupt) and must always be read from memory, not from a CPU register cache:
```c
static volatile bool s_demo_stop = false;
```

### `static` Keyword (in C files)
When used on a variable or function at file scope, `static` means "private to this file" — other files cannot access it. This prevents accidental name collisions across files.

### Fixed-Point Arithmetic
Floating-point (decimal) math is slower on some processors. Fixed-point uses integers with an implied scale factor. In this project, Q8 format multiplies values by 256 before storing:
```c
#define REC_PLAY_GAIN_Q8 512   // = 2.0 × 256
sample = (sample × 512 × vol_pct) / (256 × 100);
```
Dividing by 256 at the end converts back from Q8 to real values.

### FreeRTOS Tasks
Creating a task is like spawning a new thread:
```c
xTaskCreate(demo_task,      // function to run
            "watering_demo",// name (for debugging)
            4096,           // stack size in bytes
            NULL,           // argument passed to function
            5,              // priority (higher = runs first)
            &s_demo_task);  // handle to reference this task later
```
The task function runs until it calls `vTaskDelete(NULL)` (deletes itself).

### ESP Error Handling
Most ESP-IDF functions return `esp_err_t` — an integer where `ESP_OK` (0) means success and any other value is an error code. `ESP_ERROR_CHECK()` is a macro that checks the return value and crashes (with a log message) if it's not `ESP_OK`. This catches hardware failures early during init.

---

## 17. How Everything Connects Together

```
                         ┌──────────────┐
                         │   app_main   │
                         │   (main.c)   │
                         └──────┬───────┘
              ┌─────────────────┼──────────────────┐
              │                 │                   │
       ┌──────▼──────┐   ┌──────▼──────┐   ┌───────▼──────┐
       │  m5pm1.cpp  │   │  st7789.c   │   │  watering.c  │
       │  (PMIC/I2C) │   │  (LCD/SPI)  │   │ (pump + ADC) │
       └──────┬───────┘   └──────┬──────┘   └──────────────┘
              │                  │
       ┌──────▼──────┐   ┌───────▼─────┐
       │  es8311.c   │   │   gui.c     │
       │  (codec)    │   │ (menu draw) │
       └──────┬───────┘   └─────────────┘
              │
       ┌──────▼──────┐
       │   audio.c   │
       │ (I2S tones/ │
       │ rec/play)   │
       └─────────────┘
```

**Data flow for playing a tone:**
1. User presses KEY2 with "Tone A4" selected
2. `main.c → handle_select()` calls `audio_play_tone(440, 1000, vol_pct)`
3. `audio.c` generates sine wave samples into `s_tone_buf[]`
4. `audio.c` calls `esp_codec_dev_write()` to send samples over **I2S (DOUT, GPIO14)**
5. **ES8311 chip** receives the digital samples and converts them to an analog voltage
6. **AW8737 amplifier** amplifies the voltage
7. **Speaker** vibrates, producing the 440 Hz tone

**Data flow for the watering demo:**
1. User presses KEY2 with "Watering" selected
2. `main.c → handle_select()` calls `watering_demo_start(callback)`
3. `watering.c` creates a FreeRTOS task running `demo_task()`
4. `main.c` returns immediately — buttons keep working
5. Inside `demo_task()`:
   - Calls `adc_oneshot_read()` to read moisture from **ADC1_CH9 (GPIO10)**
   - Calls `s_demo_cb("1/5 raw:2048")` → `watering_status_update()` in `main.c` → `gui_menu_set_status()` → LCD updates
   - Calls `gpio_set_level(GPIO9, 1)` → pump turns ON
   - After 3 seconds, `gpio_set_level(GPIO9, 0)` → pump turns OFF
6. If user presses KEY2 on "Stop": `watering_demo_stop()` sets `s_demo_stop = true` → `demo_task` sees it within 10 ms and stops

---

## 18. Glossary

| Term | Meaning |
|------|---------|
| **ADC** | Analog-to-Digital Converter — reads a voltage and gives a number |
| **Attenuation** | Reducing signal strength. ADC attenuation sets the input voltage range |
| **Bitmap font** | A font where each character is stored as a grid of pixels |
| **BCLK** | Bit Clock — the I2S clock that times each individual audio bit |
| **Callback** | A function passed as an argument to another function, to be called later |
| **Capacitance** | A measure of how much electric charge a component can store; used by the soil moisture sensor |
| **Codec** | Coder/decoder — a chip that converts between digital data and analog signals |
| **DC offset** | A constant voltage shift in a signal that sits on top of the actual varying signal |
| **DDS** | Direct Digital Synthesis — generating waveforms by computing mathematical functions |
| **Debounce** | Ignoring the rapid on/off transitions of a mechanical button press |
| **Firmware** | Software that runs directly on a microcontroller chip |
| **Fixed-point** | Representing decimal numbers using integers with a fixed implied scale factor |
| **Flash** | Non-volatile storage (like a hard drive for microcontrollers) — keeps data after power off |
| **FreeRTOS** | Free Real-Time Operating System — provides tasks, mutexes, queues for microcontrollers |
| **GPIO** | General Purpose Input/Output — a configurable pin on a chip |
| **Grove** | M5Stack's standardised 4-pin connector system for accessories |
| **Header file (.h)** | Declares function signatures so other files know what's available |
| **I2C** | Two-wire serial communication protocol for low-speed chip-to-chip communication |
| **I2S** | Inter-IC Sound — serial protocol designed specifically for audio data |
| **IIR filter** | Infinite Impulse Response — a type of digital filter that feeds its output back into its input |
| **IO MUX** | IO Multiplexer — hardware inside the chip that routes internal peripherals to GPIO pins |
| **ISR** | Interrupt Service Routine — code that runs immediately when hardware signals an event |
| **LRCK** | Left-Right Clock — I2S frame sync signal, toggles at the audio sample rate |
| **MCLK** | Master Clock — a high-frequency clock reference for audio, typically 256× sample rate |
| **Microcontroller** | A chip that contains a CPU, RAM, flash, and peripherals all in one package |
| **Mono** | Single audio channel (as opposed to stereo which has left and right channels) |
| **MOSI** | Master Out Slave In — the SPI data wire from controller to peripheral |
| **Mutex** | Mutual exclusion lock — ensures only one task at a time accesses a shared resource |
| **Peripheral** | A hardware module built into the chip (SPI, I2C, I2S, ADC, etc.) |
| **Polling** | Repeatedly checking the state of something in a loop |
| **PMIC** | Power Management IC — a chip that controls power rails and battery charging |
| **PSRAM** | Pseudo-Static RAM — external RAM chip connected via high-speed SPI |
| **Pull-up resistor** | A resistor connecting a GPIO to 3.3V, making it read HIGH when nothing else drives it |
| **PWM** | Pulse Width Modulation — rapidly toggling a pin to simulate an analog output |
| **Q8 format** | Fixed-point number with 8 fractional bits (scale factor of 256) |
| **RGB565** | 16-bit colour format: 5 bits red, 6 bits green, 5 bits blue |
| **Sample rate** | How many audio measurements per second — 16,000 Hz means 16,000 samples/second |
| **SPI** | Serial Peripheral Interface — fast serial communication, typically for displays and flash |
| **Strapping pin** | A GPIO sampled at boot to configure chip behaviour; can be used normally after boot |
| **struct** | A C data type that groups related variables together |
| **Task** | A FreeRTOS thread — an independent sequence of code that appears to run concurrently |
| **`volatile`** | C keyword telling the compiler to always read a variable from memory, not a register cache |
| **WROVER** | ESP32 module with built-in PSRAM |
