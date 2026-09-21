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
  // Native USB CDC enumeration grace period
  uint32_t start = millis();
  while (!Serial && (millis() - start < 3000)) {
    delay(10);
  }

  // Sport bring-up: hardware init with the sport's pins, boot tests, and
  // the boot splash (its logo artwork belongs to the sport).
  sport::setup();

  // No blocking hold here: network services start immediately and connect
  // behind the logo. loop() enforces the minimum splash time instead, so
  // a fast Wi-Fi handshake can't cut the logo short.
  NetworkBranding branding = sport::branding();
  size_t teamOptionCount = 0;
  startNetworkServices(branding, sport::teamOptions(teamOptionCount),
                       teamOptionCount, sport::defaultPreferredTeams());
  startNetworkTask();
  sport::startDataTask();  // core-0 feed fetches

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
  // Boot sequence: (1) logo splash for BOOT_SPLASH_HOLD_MS while the
  // network task connects Wi-Fi behind it; (2) an in-progress firmware
  // update owns the screen whenever it runs; (3) the status/setup page
  // for BOOT_SETUP_PAGE_MS — or until the sport publishes its first data
  // (capped by BOOT_MAX_WAIT_FOR_DATA_MS) — unless there is no usable
  // saved Wi-Fi, in which case the AP provisioning page ("connect to the
  // scoreboard") shows right after the splash instead. The sport UI owns
  // every frame after that; network/update/game-data work runs on core 0
  // throughout.
  uint32_t bootNow = millis();
  if (bootNow < BOOT_SPLASH_HOLD_MS) {
    return;
  }
  if (handleOtaUpdateScreen()) {
    return;
  }
  if (!isProvisioning() &&
      bootNow < BOOT_SPLASH_HOLD_MS + BOOT_MAX_WAIT_FOR_DATA_MS &&
      (bootNow < BOOT_SPLASH_HOLD_MS + BOOT_SETUP_PAGE_MS ||
       !sport::hasInitialData())) {
    renderBootStatusPage(sport::name());
    return;
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
