#pragma once

#include <Arduino.h>

#include "common/comms/network_service.h"

// ============================================================================
// The sport contract — the ONE seam between the generic framework
// (src/common/ + src/main.cpp) and a sport implementation
// (src/sports/<sport>/).
// ============================================================================
// The app shell (main.cpp) is fully generic: it owns the boot sequence,
// firmware self-update UI, and network bring-up, then delegates everything
// sport-specific to the `sport::` namespace implemented in the selected
// sport folder. No virtual dispatch, no heap — plain linker-level functions,
// resolved once per build via the include path / src filter in
// platformio.ini.
//
// A new sport implements every function below. Anything not listed here is
// sport-internal and free to vary.

// Per-loop-pass snapshot of shell state handed to sport::tick().
struct SportTickContext {
  uint32_t nowMs;         // millis()
  bool isOnline;          // network confirmed online
  bool isProvisioning;    // device is running its setup AP
};

namespace sport {

// ---- Identity ----

// Human-facing device name, e.g. "MLB Scoreboard" — drawn on the boot
// status page and used for portal branding.
const char* name();

// Network branding (AP SSID, base hostname, portal title) injected into the
// generic network service.
NetworkBranding branding();

// ---- Setup-portal team options ----

// The selectable team list for the portal's preferred-team dropdowns
// (first entry should be the {0, "-- None --"} sentinel).
const NetworkTeamOption* teamOptions(size_t& count);

// Preferred-team ids preloaded when NVS has none saved yet.
const int* defaultPreferredTeams();

// ---- Lifecycle ----

// One-time init: hardware bring-up with this sport's pins (TFT panel, LED
// matrices, count LEDs), any boot tests, and the boot splash. Called from
// setup() before network services start (they connect behind the splash).
void setup();

// Create the core-0 feed task. Called after network services start.
void startDataTask();

// True once the sport has published its first data (the boot status page
// holds until then, capped by BOOT_MAX_WAIT_FOR_DATA_MS).
bool hasInitialData();

// Steady-state body, called every loop() pass after the boot/update/network
// screens have had their chance. Owns game state, rendering, and hardware
// updates for sport content.
void tick(const SportTickContext& ctx);

}  // namespace sport
