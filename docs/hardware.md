# Hardware — NHL board topology & physical pin map

The NHL build's outputs, their drivers, and the GPIO plan mapped to the
**physical header positions** of the actual dev board (the logical GPIO
numbers are NOT in order on its headers, so assignments below are chosen
for physical sequence). Since v2.0 the firmware drives this target map for
the TM1637, penalty LEDs, and the MAX7219 chain; the remaining topography
items are the third (period) MAX7219 module — the `led_matrix` HAL still
drives two — and the MAX98357 I2S audio output.

For building the physical board see **[`assembly.md`](assembly.md)** —
parts list, wiring tables, the rendered wiring diagram
(`wiring-diagram.png`), and the first-boot checklist.

## Board

ESP32-S3 DevKit ("esp32s1-devkit1", a YD-ESP32-S3-family 44-pin DevKitC-1
clone), **ESP32-S3-WROOM-1 N8R8** (8 MB quad flash + 8 MB octal PSRAM).
Two USB-C ports at the bottom edge: the **native USB OTG** port (GPIO
19/20 — use this one for `Serial`/CDC and flashing) and a **USB-UART
bridge** port (CH343 → GPIO 43/44). RST + BOOT buttons on the side, and an
onboard WS2812 RGB LED on **GPIO48** (don't use 48 for I/O). On some YD
boards the `USB-OTG` solder jumper ships open — close it if the native USB
port is dead. Both headers below are confirmed against the board (note:
position 7 on the right is GPIO41 — easy to misread as 21, but 21 sits at
position 18 and a GPIO can't appear twice).

## Outputs

| # | Output | Part | Role | Interface | GPIOs |
|---|---|---|---|---|---|
| 1 | Home score | MAX7219 8×8 matrix | Home goals | chained SPI | 40/39/38 |
| 2 | Guest score | MAX7219 8×8 matrix | Guest goals | chained SPI | (same chain) |
| 3 | Period | MAX7219 8×8 matrix | 1–4, OT, SO | chained SPI | (same chain) |
| 4 | Period clock | TM1637 4-digit 7-seg | MM:SS game clock | 2-wire bit-bang | 16/17 |
| 5 | Audio | MAX98357 I2S amp + speaker | goal horn / cues | I2S | 1/2/42 |
| 6 | Home penalties | 2 discrete LEDs | penalty 1 / penalty 2 | GPIO direct | 4/5 |
| 7 | Guest penalties | 2 discrete LEDs | penalty 1 / penalty 2 | GPIO direct | 6/7 |
| 8 | Game details | 2.0" ST7789 240×320 TFT | linescore, carousel, portal | software SPI | 9/10/11/12/13 |

**Total: 17 GPIOs.** Wiring-friendly split: **left header = indicators +
displays**, **right header = serial buses** (audio + score matrices).

## Physical header map

### LEFT header (top → bottom, confirmed)

| Pos | Pin | Assigned to | Note |
|---|---|---|---|
| 1 | 3V3 | — | power for TM1637 |
| 2 | 3V3 | — | power for TFT |
| 3 | RST | — | |
| 4 | GPIO4 | Home penalty LED 1 | |
| 5 | GPIO5 | Home penalty LED 2 | |
| 6 | GPIO6 | Guest penalty LED 1 | |
| 7 | GPIO7 | Guest penalty LED 2 | |
| 8 | GPIO15 | — gap — | spare |
| 9 | GPIO16 | TM1637 CLK | |
| 10 | GPIO17 | TM1637 DIO | |
| 11 | GPIO18 | — gap — | spare |
| 12 | GPIO8 | — spare — | was MLB matrix DIN |
| 13 | GPIO3 | — spare — | strap-adjacent; proven as LED on the MLB build |
| 14 | GPIO46 | — spare — | strapping pin — avoid using |
| 15 | GPIO9 | TFT CS | proven template pins, |
| 16 | GPIO10 | TFT DC | now also physically |
| 17 | GPIO11 | TFT RESET | consecutive |
| 18 | GPIO12 | TFT MOSI | |
| 19 | GPIO13 | TFT SCK | |
| 20 | GPIO14 | — gap — | spare |
| 21 | 5V | — | power for MAX7219 chain + MAX98357 |
| 22 | GND | — | |

### RIGHT header (top → bottom, confirmed)

| Pos | Pin | Assigned to | Note |
|---|---|---|---|
| 1 | GND | — | ground for I2S/matrices |
| 2 | TX (43) | — | UART debug spare |
| 3 | RX (44) | — | UART debug spare |
| 4 | GPIO1 | MAX98357 BCLK | |
| 5 | GPIO2 | MAX98357 LRC (WS) | |
| 6 | GPIO42 | MAX98357 DIN (data) | |
| 7 | GPIO41 | — gap — | spare (JTAG MTDO) |
| 8 | GPIO40 | MAX7219 DIN (chain in) | |
| 9 | GPIO39 | MAX7219 CLK | |
| 10 | GPIO38 | MAX7219 CS / LOAD | |
| 11 | GPIO37 | ✗ | octal PSRAM (N8R8) |
| 12 | GPIO36 | ✗ | octal PSRAM (N8R8) |
| 13 | GPIO35 | ✗ | octal PSRAM (N8R8) |
| 14 | GPIO0 | ✗ | strap / boot button |
| 15 | GPIO45 | ✗ | strapping pin |
| 16 | GPIO48 | ✗ | onboard RGB LED |
| 17 | GPIO47 | — spare — | expansion |
| 18 | GPIO21 | — spare — | expansion |
| 19 | GPIO20 | ✗ | native USB D+ |
| 20 | GPIO19 | ✗ | native USB D− |
| 21 | GND | — | |
| 22 | GND | — | |

## Design rationale

1. **Physical sequence per device.** Each peripheral occupies consecutive
   header positions: LEDs 4–7, TM1637 9–10, TFT 15–19 (left); I2S 4–6,
   MAX7219 chain 8–10 (right). One-position gaps (left 8, 11, 20; right 7)
   separate devices visually and electrically.
2. **Minimal pins.** The three MAX7219 matrices share one 3-pin daisy
   chain (CLK + CS common, data cascades DOUT→DIN), so the Period matrix
   costs zero extra pins. TM1637 is 2 pins, MAX98357 is 3 (I2S), penalty
   LEDs are 4.
3. **Proven pins survive.** The TFT lands on `CS=9, DC=10, RESET=11,
   MOSI=12, SCK=13` — the exact wiring the MLB template proved — purely
   because those GPIOs happen to sit consecutively (left 15–19) on this
   board. The penalty LEDs are a subset of the old GPIO 1–7 LED harness.
4. **Avoided by design:** GPIO 35–37 (octal PSRAM on N8R8), 0/45/46
   (straps), 48 (onboard RGB), 19/20 (native USB), 43/44 (debug UART).
   3/46 sit as spares only because 46 is a strap — prefer GPIO 15/18/14
   (left) or 41/47/21 (right) when adding parts.

## Wiring notes

**MAX7219 chain** — data flows GPIO 40 → matrix 1 (Guest) DIN → DOUT →
matrix 2 (Period) DIN → DOUT → matrix 3 (Home) DIN. CLK and CS/LOAD are
shared by all three. With a shared LOAD the first bytes shifted land in
the **last** physical device (home) — the driver's `max7219Send3` orders
its slots accordingly.

**TM1637** — power from 3V3 (left pos 1/2): its input high threshold
scales with VCC, so 5 V power makes 3.3 V logic marginal. Slightly dimmer
at 3V3 but 100 % reliable.

**MAX98357** — Vin from 5V; logic inputs are 3.3 V safe. Leave SD
unconnected (default: active, 9 dB gain). Speaker 4 Ω/3 W or 8 Ω.

**Penalty LEDs** — GPIO → LED → 330–470 Ω → GND.

**Common ground** — everything shares the board GND (corners + right pos
1/19); the I2S and bit-banged SPI signals are unforgiving of ground
bounce.

## Flash / partitions (N8R8)

8 MB flash: `partitions.csv` gives the OTA app slots 3.375 MB each (the
image uses ~42%). PSRAM (8 MB octal) is NOT enabled in firmware yet —
`board_build.arduino.memory_type = qio_opi` is already set, so enabling it
later is just `-DBOARD_HAS_PSRAM`. First use: parse the big NHL payloads
(week schedule 85 KB, play-by-play 146 KB) into PSRAM instead of internal
RAM, keeping the TLS-context heap intact.

## Driver notes (for the topography rewrite)

- **TM1637** — bit-banged 2-wire driver; render MM:SS from the live
  snapshot clock fields.
- **MAX98357** — ESP32-S3 I2S TX (16-bit mono is enough), PCM clips in
  PROGMEM, DMA playback triggered from **core 0**; nothing blocking in
  `sport::tick()`.
- **Matrices** — extend `common/hal/led_matrix` to 3 devices (24 bits per
  update, one LOAD pulse); map chain positions to Home/Guest/Period
  honoring the last-device-first-shifted ordering.
- **Penalty LEDs** — reuse `common/hal/count_leds` with a 4-pin table.
- TFT rendering, dirty-rect discipline, and the core split are unchanged.
