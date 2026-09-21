#pragma once

#include <Arduino.h>

// Network service — Wi-Fi provisioning state machine, captive setup portal,
// NVS-backed preferences, and development (Arduino)OTA. Fully generic: the
// sport layer injects its branding and team-option list at
// startNetworkServices() time; nothing in here knows a sport.

struct NetworkBranding {
  const char* deviceName;  // e.g. "MLB Scoreboard" — portal <title>/heading
  const char* apSsid;      // fallback provisioning AP network name
  const char* hostname;    // base hostname; a per-device suffix is appended
};

// One selectable team in the portal's preferred-team dropdowns.
struct NetworkTeamOption {
  int id;
  const char* label;
};

void startNetworkServices(const NetworkBranding& branding,
                          const NetworkTeamOption* teamOptions,
                          size_t teamOptionCount,
                          const int defaultPreferredTeams[3]);
void startNetworkTask();
// Runs on the main loop; performs any pending display work owned by that core.
void handleNetworkDisplay();
// True once when the device is online and the setup screen may give way.
bool consumeScoreboardRelease();

bool isOnline();
// True while the device runs its own setup AP with no usable saved Wi-Fi —
// the boot UI shows the "connect to the scoreboard" page instead of the
// status page in that state.
bool isProvisioning();
// True while someone recently used the setup portal: background feed work
// pauses so the web server gets the core and the radio to itself.
bool portalEngaged();
// Saved Wi-Fi SSID ("" when none) and the device's current IP ("" while
// not online) — read by the boot status page.
const char* getSavedWifiSsid();
String getDeviceIp();
// The three preferred-team ids (NVS "team1..3"; 0 = slot unused).
void getPreferredTeamIds(int outTeamIds[3]);
// User preference: show the idle clock on the score matrices (NVS "show_clock").
bool isClockDisplayEnabled();
// Effective display timezone as a POSIX TZ string, selected in the setup
// portal (NVS "tz"); used for game times, countdowns, and the idle clock.
const char* getTzString();
