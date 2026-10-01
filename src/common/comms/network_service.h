#pragma once

#include <Arduino.h>

// Network service — Wi-Fi provisioning state machine, captive setup portal,
// NVS-backed preferences, and development (Arduino)OTA. Fully generic: the
// sport layer injects its branding and team-option list at
// startNetworkServices() time; nothing in here knows a sport. The only
// sport reach-out is the display-test REQUEST flag, polled and executed by
// the sport itself.

struct NetworkBranding {
  const char* deviceName;  // e.g. "NHL Scoreboard" — portal <title>/heading
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
// One-shot display-test trigger from the portal's "Run Display Test"
// button (POST /display/test). Returns true exactly once per request;
// the sport layer runs its display test cycle on the render core.
bool consumeDisplayTestRequest();

// One-shot audio-test trigger from the portal's "Play Test Audio" button
// (POST /audio/test). Returns true exactly once per request.
bool consumeAudioTestRequest();

// ---- Manual mode (portal "Manual Mode" page) ----
// Full manual override of the scoreboard outputs: the clock counts down
// on the network task; every value below is set from the manual-control
// web page and read by the render core. Entering /manual engages the
// mode; the page's exit button (POST /manual/exit) leaves it.
bool isManualMode();
void setManualMode(bool on);
int  getManualClockSec();
bool getManualClockRunning();
int  getManualHomeScore();
int  getManualGuestScore();
int  getManualPeriod();
int  getManualPenaltyMask();   // bit0=home P1, 1=home P2, 2=guest P1, 3=guest P2
int  getManualHomeShots();
int  getManualGuestShots();
// True exactly once per manual-clock expiry (0:00): the sport layer
// answers with the end-of-period buzzer — it owns the audio clips, so
// common code only raises the request.
bool consumeManualPeriodEndBuzzer();
// Saved Wi-Fi SSID ("" when none) and the device's current IP ("" while
// not online) — read by the boot status page.
const char* getSavedWifiSsid();
String getDeviceIp();
// The three preferred-team ids (NVS "team1..3"; 0 = slot unused).
void getPreferredTeamIds(int outTeamIds[3]);
// User preference: show the idle clock on the score matrices (NVS "show_clock").
bool isClockDisplayEnabled();
// User preference: enable the audio output — sounds and alerts (NVS
// "audio_en", default on).
bool isAudioEnabled();
// Effective display timezone as a POSIX TZ string, selected in the setup
// portal (NVS "tz"); used for game times, countdowns, and the idle clock.
const char* getTzString();
