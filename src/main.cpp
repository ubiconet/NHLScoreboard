// ============================================================================
// Generic scoreboard app shell.
// ============================================================================
// This file is sport-agnostic: it owns the boot sequence (splash hold,
// firmware-update screen, boot status page), network bring-up, and the
// one-shot NTP sync, then hands every loop pass to the sport implementation
// through the contract in common/app/sport_api.h. Which sport is compiled in
// is decided by platformio.ini (build_src_filter + include path pointing at
// src/sports/<sport>).

#include <Arduino.h>
#include <time.h>

#include "config.h"
#include "common/app/sport_api.h"
#include "common/comms/network_service.h"
#include "common/trivia/trivia_engine.h"
#include "common/ui/boot_status.h"
#include "common/ui/ota_screen.h"

void setup() {
  Serial.begin(SERIAL_BAUD_RATE);

  // No power-on battery gate (removed v3.79): every board version boots
  // and runs unconditionally — power control is the physical switch. The
  // battery is still sensed at runtime, but only on boards with the V1.1
  // divider (see power_monitor.h).

  // Devkit's onboard RGB LED is unused — pin its data line low before
  // anything else so the floating WS2812 can't glow through the session.
  pinMode(ONBOARD_RGB_LED_PIN, OUTPUT);
  digitalWrite(ONBOARD_RGB_LED_PIN, LOW);

  // Sport bring-up FIRST: hardware init with the sport's pins, boot
  // tests, and the boot splash (its logo artwork belongs to the sport).
  // The splash paints within moments of power-on — nothing in front of
  // it, not even the USB CDC grace period below. In bare-metal mode
  // (bench test) this is also the LAST shell step — no network, no boot
  // UI; the sport's tick owns the display from the first loop pass.
  sport::setup();

  if (!sport::skipBootUi()) {
    // No blocking hold here: network services start immediately and connect
    // behind the logo. loop() enforces the minimum splash time instead, so
    // a fast Wi-Fi handshake can't cut the logo short.
    trivia::begin(sport::triviaBank(), sport::name());
    NetworkBranding branding = sport::branding();
    size_t teamOptionCount = 0;
    size_t boardOptionCount = 0;
    size_t soundOptionCount = 0;
    startNetworkServices(branding, sport::teamOptions(teamOptionCount),
                         teamOptionCount, sport::defaultPreferredTeams(),
                         sport::boardOptions(boardOptionCount),
                         boardOptionCount, sport::defaultBoardVersion(),
                         sport::soundOptions(soundOptionCount),
                         soundOptionCount, sport::defaultSoundId(),
                         sport::feedDiagnosticsJson);
    startNetworkTask();
    sport::startDataTask();  // core-0 feed fetches
  }

  // Native USB CDC enumeration grace period — placed AFTER the splash and
  // network start (it used to sit first and delayed both by up to 3 s);
  // now it only guards the boot banner below. UART builds skip it
  // instantly (Serial is ready immediately there).
  uint32_t start = millis();
  while (!Serial && (millis() - start < 3000)) {
    delay(10);
  }

  // Boot banner — visible over Serial whenever SB_DEBUG=1 so a freshly
  // uploaded firmware can be confirmed at a glance.
  Serial.println();
  Serial.printf("[BOOT] FW=%s build=%s %s\n",
                FIRMWARE_VERSION, __DATE__, __TIME__);
  Serial.printf("[BOOT] heap_free=%u heap_min=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap());
}

void loop() {
  if (sport::skipBootUi()) {
    SportTickContext ctx = {millis(), false, false};
    sport::tick(ctx);
    return;
  }

  // Boot sequence: (1) the logo splash holds BOOT_SPLASH_HOLD_MS while
  // the network task connects Wi-Fi behind it — a handshake still in
  // flight at the end of the window EXTENDS the splash (capped by
  // BOOT_SPLASH_CONNECT_MAX_MS, which matches the network service's own
  // connect budget) so the next screen shows the outcome, never a
  // half-state; (2) an in-progress firmware update owns the screen
  // whenever it runs; (3) once the connection is made, the status/setup
  // page shows for BOOT_SETUP_PAGE_MS; (4) if the connection cannot be
  // made, the AP provisioning page ("connect to the scoreboard") shows
  // instead and holds until the portal establishes the connection — then
  // the status page runs its window. The sport UI owns every frame after
  // that; network/update/game-data work runs on core 0 throughout.
  uint32_t bootNow = millis();
  if (bootNow < BOOT_SPLASH_HOLD_MS) {
    return;
  }
  if (handleOtaUpdateScreen()) {
    return;
  }
  // Anchor the setup page to the moment the network actually came up, so
  // a slow handshake eats into the splash instead of the page.
  static uint32_t onlineSinceMs = 0;
  if (onlineSinceMs == 0 && isOnline()) {
    onlineSinceMs = bootNow;
  }
  if (!isProvisioning()) {
    if (onlineSinceMs == 0 && bootNow < BOOT_SPLASH_CONNECT_MAX_MS) {
      return;  // still connecting: the splash keeps covering the boot
    }
    if (onlineSinceMs != 0 &&
        bootNow - onlineSinceMs < BOOT_SETUP_PAGE_MS) {
      renderBootStatusPage(sport::name());
      return;
    }
  }

  handleNetworkDisplay();

  // No usable connection: the network configuration screen owns the
  // display until a connection exists. Without this gate the waiting
  // carousel repaints over the provisioning instructions on every dwell,
  // hiding the one screen the device can't work without. ONE exception:
  // the portal's display overrides (game demo, manual/trivia mode, the
  // display/audio tests) run WITHOUT a network so new builds can be
  // bench-tested offline — while one is active the sport owns the
  // display, and the provisioning instructions repaint once it releases.
  // When a connection lands, consumeScoreboardRelease() (inside
  // sport::tick) repaints the scoreboard.
  if (isProvisioning()) {
    static bool sOverrideOwnedDisplay = false;
    if (portalDisplayOverrideActive()) {
      sOverrideOwnedDisplay = true;
      SportTickContext ctx = {millis(), false, true};
      sport::tick(ctx);
    } else if (sOverrideOwnedDisplay) {
      sOverrideOwnedDisplay = false;
      requestNetworkScreenRepaint();  // back to the AP instructions
    }
    return;
  }

  // One-shot NTP sync once the network is up. The display timezone is a
  // portal setting (NVS "tz"); the factory default in src/config.h only
  // covers the very first boot, and a mid-session portal change applies
  // itself via setenv/tzset in the network service.
  static bool timeSyncRequested = false;
  if (!timeSyncRequested && isOnline()) {
    configTzTime(getTzString(), "pool.ntp.org", "time.nist.gov");
    timeSyncRequested = true;
    DBG_PRINTF("[TIME] NTP sync requested; timezone=%s\n", getTzString());
  }

  SportTickContext ctx = {millis(), isOnline(), isProvisioning()};
  sport::tick(ctx);
}
