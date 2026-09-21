# ESP32-S3 Board & Configuration Reference

This document fully describes the microcontroller board, toolchain configuration,
peripheral wiring, and software libraries used in the BaseballRadar project. It is
intended to be handed to another AI agent or developer as a self-contained reference
for starting a **new** project on the exact same hardware/software base.

## 1. Board Identity

| Item | Value |
|---|---|
| Board | ESP32-S3-DevKitC-1 |
| PlatformIO board ID | `esp32-s3-devkitc-1` |
| SoC | Espressif ESP32-S3 (Xtensa LX7 dual-core) |
| Platform package | `platform = espressif32` (PlatformIO) |
| Framework | Arduino (`framework = arduino`) |
| Flash size | 4 MB |
| Partition table | `default.csv` |
| USB interface | Native USB CDC (no external USB-UART bridge) |
| Serial monitor speed | 115200 baud |

## 2. PlatformIO Project Configuration

Full `platformio.ini` environment used in this project:

```ini
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino

board_build.flash_size = 4MB
board_upload.flash_size = 4MB
board_build.partitions = default.csv

monitor_speed = 115200

build_flags =
    -DARDUINO_USB_CDC_ON_BOOT=1

lib_deps =
    adafruit/Adafruit GFX Library@^1.11.11
    adafruit/Adafruit SSD1306@^2.5.13
```

### Key build flag notes
- `-DARDUINO_USB_CDC_ON_BOOT=1` — Required so that `Serial` is routed over the
  native USB-CDC peripheral immediately at boot (instead of UART0). This is what
  allows plugging directly into a PC via the board's USB port and opening a
  serial monitor without a separate programmer/adapter.
- Because native USB CDC takes a moment to enumerate, application code should
  wait/poll on `Serial` (or a short timeout) at the start of `setup()` before
  printing, otherwise early log lines can be lost. Example pattern used in this
  project:
  ```cpp
  Serial.begin(SERIAL_BAUD_RATE);
  uint32_t start = millis();
  while (!Serial && (millis() - start < 3000)) {
    delay(10);
  }
  ```

## 3. Libraries Used

| Library | Version constraint | Purpose |
|---|---|---|
| Adafruit GFX Library | `^1.11.11` | Core graphics primitives (lines, shapes, text) used by the SSD1306 driver. |
| Adafruit SSD1306 | `^2.5.13` | Driver for the SSD1306 OLED display controller over I2C. |
| Arduino core for ESP32 (`Arduino.h`) | bundled with `espressif32` platform | Base framework: GPIO, `analogRead`, `Wire`, timing (`millis`/`micros`), `Serial`. |
| `Wire` (I2C) | bundled with Arduino core | I2C bus driver used to talk to the SSD1306 display. |

No other third-party libraries are used. All DSP (FFT, windowing, signal
statistics) and Doppler-speed math are hand-written in this project (see
`radar_dsp.*`) and are application-specific, not board/library configuration.

## 4. Peripheral Wiring / Pin Configuration

All pins are defined centrally in `src/config.h`.

### 4.1 Analog input (radar IF signal)

| Signal | GPIO | Notes |
|---|---|---|
| Radar IF ADC input | GPIO 1 (`ADC1_CHANNEL_0`) | 12-bit resolution (`analogReadResolution(12)`), read via `analogRead()`. Expects an externally conditioned signal: AC-coupled, gain-adjusted, bandpass filtered (~100 Hz–15 kHz), and biased to ~1.65 V DC (mid-scale of the 3.3 V ADC range) before entering this pin. |

ADC characteristics assumed by the software:
- Full-scale voltage: 3.3 V
- Max count: 4095 (12-bit)
- Clipping thresholds: raw counts `<= 15` (low rail) or `>= 4080` (high rail)

### 4.2 I2C OLED Display (SSD1306, 128x32)

| Signal | GPIO |
|---|---|
| SDA | GPIO 8 |
| SCL | GPIO 9 |
| I2C address | `0x3C` |
| Reset pin | `-1` (no dedicated reset pin; shares power-on reset) |
| Resolution | 128x32 |

I2C bus is initialized explicitly with the custom pins (ESP32-S3 lets you remap
I2C to arbitrary GPIOs, unlike classic boards with fixed SDA/SCL):
```cpp
Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN); // GPIO 8, GPIO 9
display.begin(SSD1306_SWITCHCAPVCC, OLED_SCREEN_ADDRESS); // 0x3C
```

## 5. Sampling Strategy (board-specific behavior worth reusing)

This project does **not** use ESP32's continuous/DMA ADC mode. Instead it uses a
software-paced blocking loop calling `analogRead()` on a microsecond timer to hit
a target sample rate (48 kHz), and measures the *actual* achieved rate each block
via `micros()` deltas (see `radar_acquisition.cpp`). This is a deliberate
simplicity/portability tradeoff — if a new project needs higher-fidelity or
higher-rate sampling, consider the ESP32 ADC continuous DMA driver instead, but
note that is a different (lower-level, ESP-IDF-flavored) API than what's used
here.

## 6. Reuse Checklist for a New Project on This Same Board

1. Copy the `[env:esp32-s3-devkitc-1]` block above into the new project's
   `platformio.ini`, keeping `-DARDUINO_USB_CDC_ON_BOOT=1` if native USB
   Serial is desired.
2. If reusing the OLED display, add the two Adafruit `lib_deps` entries and
   wire SDA/SCL to GPIO 8/9 (or update pins and re-declare `Wire.begin(sda, scl)`
   accordingly).
3. Wait on `Serial` at boot before logging, per the USB CDC note in Section 2.
4. Pick a free ADC1-capable GPIO (this project uses GPIO 1) for any analog
   input, and remember ESP32-S3 ADC is 12-bit (0–4095) at up to 3.3 V full
   scale by default.
5. No RTOS-specific, PSRAM, or WiFi/BLE configuration is used by this project;
   add those explicitly if the new project requires them.
