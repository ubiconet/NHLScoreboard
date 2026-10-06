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
#include "common/ui/boot_status.h"
#include "common/ui/ota_screen.h"

void setup() {
  Serial.begin(SERIAL_BAUD_RATE);

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
    NetworkBranding branding = sport::branding();
    size_t teamOptionCount = 0;
    startNetworkServices(branding, sport::teamOptions(teamOptionCount),
                         teamOptionCount, sport::defaultPreferredTeams());
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
