#pragma once

#include <Arduino.h>   // uint32_t & friends — config headers are self-sufficient

// ============================================================================
// NHL scoreboard — sport + device profile.
// ============================================================================
// This file (named sport_config.h in every sport folder) is what the generic
// framework in src/common/ composes in via the include path each PlatformIO
// env sets up. It carries everything that changes between sports AND between
// physical builds: display pins/geometry, branding strings, feed poll
// cadences, and UI layout constants.
//
// Starting a new sport from this template: copy src/sports/nhl to
// src/sports/<sport>, edit this file, and point the env's -I flag and
// build_src_filter at the new folder (see README "new sport" checklist).

// ---- 2.0-inch ST7789V TFT (320x240, software SPI) --------------------------
// The 5-arg software-SPI constructor is deliberate: on ESP32-S3 the 3-arg
// hardware-SPI variant binds SPI.begin() to the variant's default VSPI pins
// instead of these, leaving a blank screen. See main.cpp's display comment.
// Backplane-PCB rev map (v3.44): SCL=8, SDA=3, CS=10, DC=9, RES=46. GPIO 3
// and 46 are strapping pins — fine as outputs after boot (46's reset-time
// state only gates ROM log output), but the TFT must not drive them during
// power-up (its SDA/RES lines are inputs, so it can't).
static const int TFT_SCLK_PIN = 8;
static const int TFT_MOSI_PIN = 3;
static const int TFT_CS_PIN = 10;
static const int TFT_DC_PIN = 9;
static const int TFT_RESET_PIN = 46;
static const int TFT_BACKLIGHT_PIN = -1;
static const int TFT_NATIVE_WIDTH = 240;
static const int TFT_NATIVE_HEIGHT = 320;

// ---- MAX7219 8x8 LED Matrix displays (daisy chain: away, home, period) -----
// Target physical map (docs/hardware.md): right header pos 8-10. The
// led_matrix HAL currently drives the first two modules; the period module
// joins when the HAL extends to three devices.
static const int MAX7219_DIN_PIN = 40;
static const int MAX7219_CLK_PIN = 39;
static const int MAX7219_CS_PIN = 38;

// ---- TM1637 4-digit 7-segment (period/game clock; wall clock when idle) ----
// V0.1 wiring: CLK/DIO on GPIO16/17. V1.0 swaps to GPIO15/16. V1.1 keeps
// the V1.0 pin map and adds the battery-sense divider (battery section
// below); the portal's board-version picker selects the board.
static const int BOARD_VERSION_V0_1 = 1;
static const int BOARD_VERSION_V1_0 = 10;
static const int BOARD_VERSION_V1_1 = 11;
static const int TM1637_CLK_V0_1_PIN = 16;
static const int TM1637_DIO_V0_1_PIN = 17;
static const int TM1637_CLK_V1_0_PIN = 15;
static const int TM1637_DIO_V1_0_PIN = 16;
static const int TM1637_BRIGHTNESS = 3;   // 0..7

// ---- Penalty LEDs (home P1/P2, guest P1/P2) --------------------------------
// Backplane-PCB rev map (v3.44): home 4/5, guest 11/12 (guest moved off
// 6/7 for the new routing). count_leds drives a 3/2/2 pin grouping;
// penalties need 2+2, so the unused slots point at free pins. FILL_A must
// stay off GPIO 8 (now the TFT SCL — initCountLeds would claim it) and sits
// on 13, the freed old TFT SCK.
static const int PENALTY_HOME1_PIN = 4;
static const int PENALTY_HOME2_PIN = 5;
static const int PENALTY_AWAY1_PIN = 11;
static const int PENALTY_AWAY2_PIN = 12;
static const int PENALTY_FILL_A  = 13;    // group A 3rd slot (never lit)
static const int PENALTY_FILL_B1 = 6;    // group C slot (never lit)
static const int PENALTY_FILL_B2 = 18;

static inline int homePenalty1Pin(int boardVersion) {
  return boardVersion >= BOARD_VERSION_V1_0 ? PENALTY_AWAY2_PIN
                                            : PENALTY_HOME1_PIN;
}
static inline int homePenalty2Pin(int boardVersion) {
  return boardVersion >= BOARD_VERSION_V1_0 ? PENALTY_AWAY1_PIN
                                            : PENALTY_HOME2_PIN;
}
static inline int awayPenalty1Pin(int boardVersion) {
  return boardVersion >= BOARD_VERSION_V1_0 ? PENALTY_HOME2_PIN
                                            : PENALTY_AWAY1_PIN;
}
static inline int awayPenalty2Pin(int boardVersion) {
  return boardVersion >= BOARD_VERSION_V1_0 ? PENALTY_HOME1_PIN
                                            : PENALTY_AWAY2_PIN;
}

// ---- MAX98357 I2S audio amp (goal horn) ------------------------------------
// Target physical map: right header pos 4-6.
static const int I2S_BCLK_PIN = 1;
static const int I2S_LRC_PIN = 2;
static const int I2S_DIN_PIN = 42;

// ---- Branding (AP network name, hostname, portal title) ---------------------
static const char* NETWORK_AP_SSID = "NHL_SCOREBOARD";
static const char* NETWORK_HOSTNAME = "nhl-scoreboard";

// ---- Battery power (LiPo + boost converter supply) --------------------------
// The board runs off a 3.7 V LiPo through a 5 V boost converter. When the
// pack runs low the boost rail collapses under load and the board brown-out
// cycles on/off with no indication. The power gate (common/app/power_monitor,
// driven from main.cpp's boot sequence) is a V1.1 HARDWARE FEATURE and is
// DISABLED ENTIRELY on other board versions (v3.63): V0.1/V1.0 boards do no
// voltage check and keep no boot-storm journal — they boot and run
// unconditionally. On V1.1 boards (divider wired):
//   * the pack voltage is read at boot and at/below BATTERY_LOW_VOLTS the
//     board shows the low-battery screen and deep-sleeps immediately (no
//     cycling first). While asleep it silently re-checks every
//     BATTERY_RECHECK_MS and resumes on its own once the charger lifts the
//     pack past BATTERY_RESUME_VOLTS.
//   * a backstop counts rapid bad boots (brownout resets / boots that died
//     before BATTERY_HEALTHY_BOOT_MS of uptime); BATTERY_BAD_BOOT_STREAK
//     trips in a row trigger the low-battery screen + deep sleep. Recovery
//     needs a power cycle after charging.
// The divider is a V1.1 hardware feature: batterySenseEnabled() keys off
// the portal's board-version setting (NVS), so ONE firmware serves every
// board and only boards actually set to V1.1 read the pin. Don't select
// V1.1 on a board without the divider — a floating ADC pin can read like
// a dead pack (the gate's >0.5 V plausibility guard only rejects a dead
// short, not noise).
// Sense pin: GPIO 7 = ADC1_CH6, left header pos 7 — spare since the v3.44
// PCB rev moved the guest penalty LEDs to 11/12. It sits on ADC1, which
// Wi-Fi never claims, so the divider stays readable at RUNTIME (the
// on-screen battery gauge samples it while the board runs), not only at
// the boot check. Note GPIO 21 (near the power rails) has no ADC channel
// on ESP32-S3 and GPIO 20 is native USB D−, so neither can sense.
// Divider wiring: BAT+ (after the switch / boost input +) --[470k]--
// BATTERY_SENSE_PIN --[470k]-- GND, with 100 nF across the lower
// resistor (wiring diagram in docs/assembly.md).
static const int BATTERY_SENSE_PIN = 7;            // ADC1_CH6, left header pos 7
static const float BATTERY_SENSE_DIVIDER = 2.0f;   // (R1 + R2) / R2
static const float BATTERY_LOW_VOLTS = 3.40f;      // boot reading that trips shutdown;
                                                   //   also the gauge's EMPTY floor
// Gauge FULL level. The charger terminates at 4.18 V on this pack, so a
// topped-off battery reads 100% (4/4) and holds it until the smoothed
// voltage crosses back below the 75% boundary (~3.99 V).
static const float BATTERY_FULL_VOLTS = 4.18f;
static const float BATTERY_RESUME_VOLTS = 3.90f;   // re-check resume (charger present)
static const uint32_t BATTERY_RECHECK_MS = 900000; // silent re-check while asleep (15 min)
static const uint8_t BATTERY_BAD_BOOT_STREAK = 3;  // bad boots before the backstop trips
static const uint32_t BATTERY_HEALTHY_BOOT_MS = 90000; // uptime that marks a boot healthy
// Runtime gauge sampling (serviceBatteryMonitor paces itself to this).
// Quarter-step charge changes take minutes; 30 s is far finer than the
// display ever shows, and each sample is a ~10 ms median-of-5 on core 0.
static const uint32_t BATTERY_MONITOR_INTERVAL_MS = 30000;

// True on boards whose hardware carries the battery-sense divider (V1.1+).
// boardVersion comes from the portal's board-version setting (NVS); the
// power gate uses this to decide whether the pack voltage can be trusted.
static inline bool batterySenseEnabled(int boardVersion) {
  return boardVersion >= BOARD_VERSION_V1_1;
}

// ---- UI theme (RGB565) -------------------------------------------------------
// Shared by the sport renderer AND the generic boot/OTA screens (they include
// config.h). A new sport overrides these to re-skin the whole UI.
static const uint16_t COLOR_BG = 0x0821;        // Dark slate
static const uint16_t COLOR_CARD = 0x18A5;      // Card background
static const uint16_t COLOR_GOLD = 0xFD20;      // Gold accent
static const uint16_t COLOR_YELLOW = 0xFFE0;    // Count / runner yellow
static const uint16_t COLOR_MUTED = 0x7BEF;     // Gray text/icon
static const uint16_t COLOR_LED_RED = 0xF800;   // Bright red for LED dot display
static const uint16_t COLOR_LED_OFF = 0x2100;   // Dark unlit LED dot background

// ---- NHL feed polling + UI timing -------------------------------------------
static const uint32_t NHL_LIVE_POLL_INTERVAL_MS = 5000;  // 5 second live linescore tick
// Landing-fetch failure backoff cap (doubles from the 5 s poll up to
// this), and how long without a fresh snapshot before the renderer's
// clock/penalties continue on their own instead of freezing mid-play.
static const uint32_t NHL_LANDING_RETRY_MAX_MS = 60000;
static const uint32_t NHL_LANDING_STALE_MS = 90000;
static const uint32_t NHL_SCHEDULE_POLL_INTERVAL_MS = 60000; // Detect followed-game start/end within 1 min
static const uint32_t NHL_SCHEDULE_RETRY_MS = 15000;   // Base retry while the last schedule fetch failed (flaky Wi-Fi)
static const uint32_t NHL_RETRY_BACKOFF_MAX_MS = 120000; // Exponential backoff cap for failed fetch retries.

// ---- Optional hibernate mode (portal setting "hibernate") -------------------
// When enabled, a board with no live preferred-team game goes dark 3 min
// after boot (and 3 min after any game/portal session ends): TFT panel
// into sleep, MAX7219 matrices into shutdown, TM1637 blank, penalty LEDs
// off, portal web server STOPPED (power-cycle the board to configure),
// Wi-Fi modem sleep between checks, and both cores at 80 MHz — it
// appears off and sips power. While dark the data task makes ONE
// silent schedule check every 5 min; a live preferred-team game wakes
// the board to full power (240 MHz, radio awake, portal back) for puck
// drop, and it re-hibernates after the next idle period. The board
// never reboots itself into hibernate.
static const uint32_t HIBERNATE_IDLE_GRACE_MS = 180000;  // idle time before going dark
static const uint32_t HIBERNATE_CHECK_MS = 300000;       // silent game check while dark (5 min)

// Devkit onboard RGB LED (WS2812 on GPIO 48): unused by the app. Pinned
// low right after boot so its floating data line can't make it glow.
static const int ONBOARD_RGB_LED_PIN = 48;
// After the first couple of connections following boot, new TLS connections
// start failing instantly (start_ssl_client: -1) with the radio still
// associated and heap healthy — consistent with the AP's flood protection
// and/or leaked lwIP PCBs from failed handshakes. Backing off exponentially
// (per ADR-0002) instead of retrying every 15 s lets those windows expire.
static const uint32_t NHL_NTP_READY_RETRY_MS = 5000;     // short retry while awaiting first time sync
static const uint32_t NHL_POSTGAME_GRACE_MS = 120000;    // Keep final followed game visible for 2 min
static const uint32_t NHL_AT_BAT_RESULT_DISPLAY_MS = 5000; // Full-screen result card duration
static const uint32_t NHL_GOAL_ANNOUNCEMENT_MS = 5000; // Full-screen GOAL! duration
static const uint32_t NHL_GOAL_DETAILS_MS = 5000; // Scorer details before returning live
static const uint32_t NHL_BOTTOM_VIEW_FLIP_MS = 8000; // Penalty-free bottom strip: linescore <-> league scores
static const uint32_t NHL_CAROUSEL_ROTATE_MS = 5000;      // Rotate live-game stat ticker every 5s
static const uint32_t NHL_UPCOMING_GAMES_ROTATE_MS = 8000;  // Show each upcoming-game card for 8s
// Goal blink: a score increase flashes that team's NEW score on its
// matrix this many times, each flash NHL_SCORE_BLINK_HALF_MS on and
// NHL_SCORE_BLINK_HALF_MS off, before returning to the steady display.
static const uint8_t NHL_SCORE_BLINK_FLASHES = 3;
static const uint32_t NHL_SCORE_BLINK_HALF_MS = 250;
// Week look-ahead (/v1/schedule, Mon-Sun): TWO week caches — the one
// containing today and the next — keep the upcoming-games horizon at
// >= 7 days out (a single Mon-Sun cache empties late in the week). Each
// cache refreshes on its own TTL/rollover — future-day states never
// change (today's half of the upcoming doc always comes from the fresh
// day-score poll) — and failed refreshes back off before retrying into
// a possibly-flaky TLS window.
static const uint32_t NHL_WEEK_SCHEDULE_TTL_MS = 6UL * 60UL * 60UL * 1000UL;
static const uint32_t NHL_WEEK_SCHEDULE_RETRY_MS = 15UL * 60UL * 1000UL;
// Division standings: rows only change after games — a few refreshes a
// day is plenty; failed fetches back off before retrying.
static const uint32_t NHL_STANDINGS_TTL_MS = 6UL * 60UL * 60UL * 1000UL;
static const uint32_t NHL_STANDINGS_RETRY_MS = 30UL * 60UL * 1000UL;
// News ticker pacing — LED-marquee style. The bit-banged bus can't push
// the window fast enough for clean continuous motion (any continuous
// scroll tears by speed x push time, ~14 px at best), so the ticker
// advances one whole character cell (24 px) per step and holds between
// steps, like a physical LED sign: the display is perfectly static except
// for a brief tick every NHL_NEWS_TICKER_STEP_MS. 200 ms/step averages
// ~80 px/s. Smaller = faster, larger = slower.
static const uint32_t NHL_NEWS_TICKER_STEP_MS = 200;   // ms per 24-px character step
static const uint32_t NHL_NEWS_CACHE_TTL_MS = 30UL * 60UL * 1000UL; // Refresh ESPN news every 30 min
static const uint32_t NHL_NEWS_RETRY_MS     = 60UL * 1000UL;       // Retry failed news fetch every 60s until first success. Keep this gentle: ESPN's edge starts rejecting TLS handshakes (fatal alerts) from clients that retry every few seconds.
// Waiting-carousel news pages (MLB look): one story per page, white
// headline with the story details scrolling beneath it; stories dwell
// longer than the other slides so the scroll has time to run.
static const uint32_t NHL_NEWS_STORY_DWELL_MS = 10000;   // story page dwell
// Detail scroll speed (px/s). The strip advances one whole ticker
// character cell (TICKER_CHAR_W, 15 px) per redraw: a stepped LED-sign
// look that is perfectly static between pushes — continuous sub-character
// sweeping tears on the bit-banged bus (speed x push time per frame).
static const uint32_t NHL_NEWS_SCROLL_PX_PER_SEC = 56;   // detail scroll speed
// How far past its end the detail scroll holds before the story slide
// advances (~1.3 s at the default scroll speed), so the tail of the story
// is readable before the carousel moves on.
static const int NHL_NEWS_SCROLL_END_HOLD_PX = 60;
