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
// Target physical map: left header pos 9-10.
static const int TM1637_CLK_PIN = 16;
static const int TM1637_DIO_PIN = 17;
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
static const int PENALTY_FILL_B1 = 15;   // group C slots (never lit)
static const int PENALTY_FILL_B2 = 18;

// ---- MAX98357 I2S audio amp (goal horn) ------------------------------------
// Target physical map: right header pos 4-6.
static const int I2S_BCLK_PIN = 1;
static const int I2S_LRC_PIN = 2;
static const int I2S_DIN_PIN = 42;

// ---- Branding (AP network name, hostname, portal title) ---------------------
static const char* NETWORK_AP_SSID = "NHL_SCOREBOARD";
static const char* NETWORK_HOSTNAME = "nhl-scoreboard";

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
// Week look-ahead (/v1/schedule, Mon-Sun): the upcoming-games carousel
// spans a full week. The week cache refreshes at most 4x/day — future-day
// game states never change (today's half of the upcoming doc always comes
// from the fresh day-score poll) — and failed refreshes back off before
// retrying into a possibly-flaky TLS window.
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
