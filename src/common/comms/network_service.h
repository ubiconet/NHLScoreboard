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
  const char* soundSettingLabel;  // portal sound-picker heading, e.g.
                                  // "Goal Horn" ("" hides the section)
};

// One selectable team in the portal's preferred-team dropdowns.
struct NetworkTeamOption {
  int id;
  const char* label;
};

struct NetworkBoardOption {
  int id;
  const char* label;
};

// One selectable sound in the portal's sound dropdown (e.g. alternate
// goal horns). The sport supplies the ids/labels and maps ids to its own
// audio clips; an empty list hides the portal section entirely.
struct NetworkSoundOption {
  int id;
  const char* label;
};

void startNetworkServices(const NetworkBranding& branding,
                          const NetworkTeamOption* teamOptions,
                          size_t teamOptionCount,
                          const int defaultPreferredTeams[3],
                          const NetworkBoardOption* boardOptions,
                          size_t boardOptionCount,
                          int defaultBoardVersion,
                          const NetworkSoundOption* soundOptions,
                          size_t soundOptionCount,
                          int defaultSoundId,
                          String (*statusExtraJson)() = nullptr);
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
// Stops or restarts the portal web server (hibernate shuts it down for
// the dark period — power-cycle the board to configure; a reboot brings
// the portal back with the usual post-boot idle window).
void setPortalEnabled(bool enabled);
// One-shot display-test trigger from the portal's "Run Display Test"
// button (POST /display/test). Returns true exactly once per request;
// the sport layer runs its display test cycle on the render core.
bool consumeDisplayTestRequest();

// One-shot audio-test trigger from the portal's "Play Test Audio" button
// (POST /audio/test) or a sound-picker preview (POST /audio/test?id=N).
// Returns the sound id to play — the request's id, falling back to the
// saved selection — exactly once per request; -1 when nothing is pending.
int consumeAudioTestRequest();

// Selected sound id from the portal's sound picker (NVS "sound_id"; the
// injected option list's default when never saved). The sport layer maps
// the id to one of its clips.
int getSelectedSoundId();

// ---- Manual mode (portal "Manual Mode" page) ----
// Full manual override of the scoreboard outputs: the clock counts down
// on the network task; every value below is set from the manual-control
// web page and read by the render core. Entering /manual engages the
// mode; the page's exit button (POST /manual/exit) leaves it.
bool isManualMode();
void setManualMode(bool on);
// Portal-triggered demo display mode.
bool isGameDemoMode();

// True while any portal-triggered display override (game demo, manual
// mode, trivia mode, or a pending display/audio test) should own the
// screen even with no network connection — lets the diagnostics page's
// hardware checks run while the board is still provisioning (no Wi-Fi),
// so new builds can be bench-tested offline.
bool portalDisplayOverrideActive();

// Re-fire the last network boot screen (the AP instructions while
// provisioning) after such an override releases the display while the
// board is still offline.
void requestNetworkScreenRepaint();

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
// User preference: hibernate between games (NVS "hibernate", default
// off) — display dark, portal offline, CPU throttled; a silent game
// check every few minutes wakes it for puck drop. Power-cycle to
// configure. Timing/power constants live in sport_config.h.
bool isHibernateEnabled();
// Effective display timezone as a POSIX TZ string, selected in the setup
// portal (NVS "tz"); used for game times, countdowns, and the idle clock.
const char* getTzString();
// Selected physical board version (NVS "board_version"). Readable before
// network services start so sport hardware can use it during initialization.
int getBoardVersion();
