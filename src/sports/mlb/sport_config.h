#pragma once

#include <Arduino.h>   // uint32_t & friends — config headers are self-sufficient

// ============================================================================
// MLB scoreboard — sport + device profile.
// ============================================================================
// This file (named sport_config.h in every sport folder) is what the generic
// framework in src/common/ composes in via the include path each PlatformIO
// env sets up. It carries everything that changes between sports AND between
// physical builds: display pins/geometry, branding strings, feed poll
// cadences, and UI layout constants.
//
// Starting a new sport from this template: copy src/sports/mlb to
// src/sports/<sport>, edit this file, and point the env's -I flag and
// build_src_filter at the new folder (see README "new sport" checklist).

// ---- 2.0-inch ST7789V TFT (320x240, software SPI) --------------------------
// The 5-arg software-SPI constructor is deliberate: on ESP32-S3 the 3-arg
// hardware-SPI variant binds SPI.begin() to the variant's default VSPI pins
// instead of these, leaving a blank screen. See main.cpp's display comment.
static const int TFT_SCLK_PIN = 13;
static const int TFT_MOSI_PIN = 12;
static const int TFT_CS_PIN = 9;
static const int TFT_DC_PIN = 10;
static const int TFT_RESET_PIN = 11;
static const int TFT_BACKLIGHT_PIN = -1;
static const int TFT_NATIVE_WIDTH = 240;
static const int TFT_NATIVE_HEIGHT = 320;

// ---- MAX7219 8x8 LED Matrix displays (2 cascaded modules: 0=Away, 1=Home) --
static const int MAX7219_DIN_PIN = 14;
static const int MAX7219_CLK_PIN = 8;
static const int MAX7219_CS_PIN = 16;

// ---- Discrete Count LEDs (GPIO pins; balls/strikes/outs) --------------------
static const int BALL_3_PIN = 1;
static const int BALL_2_PIN = 2;
static const int BALL_1_PIN = 3;
static const int STRIKE_2_PIN = 4;
static const int STRIKE_1_PIN = 5;
static const int OUT_2_PIN = 6;
static const int OUT_1_PIN = 7;

// ---- Branding (AP network name, hostname, portal title) ---------------------
static const char* NETWORK_AP_SSID = "MLB_SCOREBOARD";
static const char* NETWORK_HOSTNAME = "mlb-scoreboard";

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

// ---- MLB feed polling + UI timing -------------------------------------------
static const uint32_t MLB_LIVE_POLL_INTERVAL_MS = 5000;  // 5 second live linescore tick
static const uint32_t MLB_SCHEDULE_POLL_INTERVAL_MS = 60000; // Detect followed-game start/end within 1 min
static const uint32_t MLB_SCHEDULE_RETRY_MS = 15000;   // Base retry while the last schedule fetch failed (flaky Wi-Fi)
static const uint32_t MLB_RETRY_BACKOFF_MAX_MS = 120000; // Exponential backoff cap for failed fetch retries.
// After the first couple of connections following boot, new TLS connections
// start failing instantly (start_ssl_client: -1) with the radio still
// associated and heap healthy — consistent with the AP's flood protection
// and/or leaked lwIP PCBs from failed handshakes. Backing off exponentially
// (per ADR-0002) instead of retrying every 15 s lets those windows expire.
static const uint32_t MLB_NTP_READY_RETRY_MS = 5000;     // short retry while awaiting first time sync
static const uint32_t MLB_POSTGAME_GRACE_MS = 300000;    // Keep final followed game visible for 5 min
static const uint32_t MLB_AT_BAT_RESULT_DISPLAY_MS = 5000; // Full-screen result card duration
static const uint32_t MLB_CAROUSEL_ROTATE_MS = 5000;      // Rotate live-game stat ticker every 5s
static const uint32_t MLB_UPCOMING_GAMES_ROTATE_MS = 5000;  // Show each upcoming-game card for 5s
// News ticker pacing — LED-marquee style. The bit-banged bus can't push
// the window fast enough for clean continuous motion (any continuous
// scroll tears by speed x push time, ~14 px at best), so the ticker
// advances one whole character cell (24 px) per step and holds between
// steps, like a physical LED sign: the display is perfectly static except
// for a brief tick every MLB_NEWS_TICKER_STEP_MS. 200 ms/step averages
// ~80 px/s. Smaller = faster, larger = slower.
static const uint32_t MLB_NEWS_TICKER_STEP_MS = 200;   // ms per 24-px character step
static const uint32_t MLB_NEWS_CACHE_TTL_MS = 30UL * 60UL * 1000UL; // Refresh ESPN news every 30 min
static const uint32_t MLB_NEWS_RETRY_MS     = 60UL * 1000UL;       // Retry failed news fetch every 60s until first success. Keep this gentle: ESPN's edge starts rejecting TLS handshakes (fatal alerts) from clients that retry every few seconds.
