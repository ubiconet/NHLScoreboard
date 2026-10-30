#pragma once

#include <Arduino.h>

#include "common/comms/network_service.h"
#include "common/trivia/trivia_engine.h"

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

// Human-facing device name, e.g. "NHL Scoreboard" — drawn on the boot
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

// Physical board revisions selectable in the setup portal. The default is
// used on first boot and when NVS contains an unsupported version.
const NetworkBoardOption* boardOptions(size_t& count);
int defaultBoardVersion();

// ---- Setup-portal sound options ----

// Selectable sounds (e.g. alternate goal horns) for the portal's sound
// picker; the sport maps each id to one of its audio clips. An empty list
// hides the picker.
const NetworkSoundOption* soundOptions(size_t& count);
int defaultSoundId();

// ---- Trivia mode ----

// The sport's trivia question bank (three difficulty pools). The trivia
// engine + web page + on-device screen are generic (common/trivia); this
// supplies the sport's content.
TriviaBank triviaBank();

// ---- Lifecycle ----

// One-time init: hardware bring-up with this sport's pins (TFT panel, LED
// matrices, count LEDs), any boot tests, and the boot splash. Called from
// setup() before network services start (they connect behind the splash).
void setup();

// True when the sport wants a bare-metal boot: no splash hold, no network
// bring-up, no boot/OTA/portal screens — setup() drew the first frame and
// tick() owns the display from the first loop pass. Used by bench test
// modes that exercise display wiring without any network dependency.
bool skipBootUi();

// Create the core-0 feed task. Called after network services start.
void startDataTask();

// Steady-state body, called every loop() pass after the boot/update/network
// screens have had their chance. Owns game state, rendering, and hardware
// updates for sport content.
void tick(const SportTickContext& ctx);

// ---- Feed diagnostics ----

// JSON fragment (no outer braces) describing the sport's feed state for
// the portal's /status endpoint: schedule fetch attempts/successes, the
// last error and HTTP code, the followed game id, Wi-Fi signal, and the
// device clock. Wired into the status JSON by main.cpp via the
// network-service status hook. Empty string is fine for sports without
// feeds.
String feedDiagnosticsJson();

}  // namespace sport
