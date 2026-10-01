#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <qrcode.h>

#include <stdlib.h>
#include <time.h>

#include "config.h"
#include "common/ui/ota_screen.h"
#include "ota_update.h"

#include "common/hal/tft_panel.h"
#include "network_service.h"

namespace {
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

// PROVISIONING: no network, portal + setup screen locked.
// CONNECTING:    associating / probing for internet (portal stays available).
// ONLINE:        internet confirmed; AP torn down, setup screen releases.
enum NetworkState { PROVISIONING, CONNECTING, ONLINE };
NetworkState state = PROVISIONING;

const char DEFAULT_CONFIG[] = R"json({"version":1,"teams":[]})json";

bool dnsRunning = false;
bool apUp = false;
bool otaStarted = false;
// Branding + team options, injected at startNetworkServices() by the app
// shell (from the sport's config/tables). Empty until then.
NetworkBranding netBranding = {"Scoreboard", "SCOREBOARD", "scoreboard"};
const NetworkTeamOption* netTeamOptions = nullptr;
size_t netTeamOptionCount = 0;
int netDefaultTeams[3] = {0, 0, 0};
String deviceHostname;
// Per-device setup AP SSID (set once MAC is known) so several
// unprovisioned boards can be powered at once without SSID collisions.
String apSsid;

// Credentials in NVS (last known good) and the pair being tried from the portal.
String savedSsid;
String savedPassword;
String pendingSsid;
String pendingPassword;
bool hasPending = false;
bool clockDisplayEnabled = true;
bool audioEnabled = true;
// Display-test request from the portal (see consumeDisplayTestRequest()).
// Volatile: set by the network task's web handler, consumed on the render
// core's tick.
volatile bool displayTestRequested = false;

// Audio-test request from the portal (see consumeAudioTestRequest()).
volatile bool audioTestRequested = false;

// ---- Manual mode state (set from the manual web page on this task,
// read by the render core; small ints/bools, benign races at worst) ----
volatile bool manualMode = false;
volatile bool manualClockRunning = false;
volatile int  manualClockSec = 1200;
volatile int  manualHomeScore = 0;
volatile int  manualGuestScore = 0;
volatile int  manualPeriod = 1;
volatile int  manualPenaltyMask = 0;
volatile int  manualHomeShots = 0;
volatile int  manualGuestShots = 0;
// Period length the clock reloads to when a period ends (page "period
// length" field, default 20:00).
volatile int manualPeriodLenSec = 1200;
// millis() when the clock hit zero and the end-of-period sequence began
// (buzzer -> settle -> advance + reload); 0 = no sequence pending.
volatile uint32_t manualPeriodEndAtMs = 0;
// One-shot buzzer request for the sport layer (it owns the PCM clips).
volatile bool manualPeriodEndBuzzer = false;

// After 0:00 the buzzer sounds immediately (sport layer), then this long
// later the period advances and the clock reloads to the configured
// period length. The buzzer clip itself runs ~1.8 s, so 3 s total reads
// as "buzzer, a beat, next period".
const uint32_t MANUAL_PERIOD_ADVANCE_MS = 3000;

// 1 Hz countdown tick, driven from this task's loop.
void tickManualClock() {
  static uint32_t lastTick = 0;
  uint32_t now = millis();
  if (now - lastTick < 1000) return;
  lastTick = now;
  if (!manualMode) {
    manualPeriodEndAtMs = 0;
    return;
  }
  if (manualPeriodEndAtMs != 0) {
    // End-of-period settle: hold at 0:00 until the window passes, then
    // advance the period and reload the clock. The clock stays stopped —
    // the operator starts the next period when play actually resumes.
    if (now - manualPeriodEndAtMs >= MANUAL_PERIOD_ADVANCE_MS) {
      manualPeriodEndAtMs = 0;
      if (manualPeriod < 9) manualPeriod++;
      manualClockSec = manualPeriodLenSec;
      manualClockRunning = false;
    }
    return;
  }
  if (!manualClockRunning) return;
  if (manualClockSec > 0) {
    manualClockSec--;
    if (manualClockSec == 0) {
      manualClockRunning = false;
      manualPeriodEndAtMs = now;
      manualPeriodEndBuzzer = true;
    }
  }
}

int clampScore(int v) { return v < 0 ? 0 : (v > 99 ? 99 : v); }

// Display timezone, persisted in NVS ("tz") and selected in the portal.
// POSIX TZ strings (not IANA names): they are self-contained — DST rules and
// all — so no tz database needs to ship in the firmware. FACTORY_DEFAULT_TIMEZONE
// (src/config.h) only covers the very first boot before a selection is saved.
String tzString;

struct TzOption {
  const char* label;
  const char* posix;
};
const TzOption TZ_OPTIONS[] = {
  {"UTC",                                    "UTC0"},
  {"US Eastern (New York)",                  "EST5EDT,M3.2.0,M11.1.0"},
  {"US Central (Chicago)",                   "CST6CDT,M3.2.0,M11.1.0"},
  {"US Mountain (Denver)",                   "MST7MDT,M3.2.0,M11.1.0"},
  {"US Arizona (no DST)",                    "MST7"},
  {"US Pacific (Los Angeles)",               "PST8PDT,M3.2.0,M11.1.0"},
  {"Alaska (Anchorage)",                     "AKST9AKDT,M3.2.0,M11.1.0"},
  {"Hawaii (Honolulu)",                      "HST10"},
  {"Canada Atlantic (Halifax)",              "AST4ADT,M3.2.0,M11.1.0"},
  {"Canada Newfoundland",                    "NST3:30NDT,M3.2.0,M11.1.0"},
  {"UK & Ireland",                           "GMT0BST,M3.5.0/1,M10.5.0"},
  {"Central Europe (Paris/Berlin)",          "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Eastern Europe (Athens/Helsinki)",       "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"India",                                  "IST-5:30"},
  {"Japan (Tokyo)",                          "JST-9"},
  {"Australia Eastern (Sydney)",             "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"Australia Western (Perth)",              "AWST-8"},
};
const size_t TZ_OPTIONS_COUNT = sizeof(TZ_OPTIONS) / sizeof(TZ_OPTIONS[0]);

// Only strings from TZ_OPTIONS are accepted from the portal — never feed the
// TZ environment variable arbitrary user input.
bool isValidTzString(const char* posix) {
  if (posix == nullptr) return false;
  for (size_t i = 0; i < TZ_OPTIONS_COUNT; ++i) {
    if (strcmp(TZ_OPTIONS[i].posix, posix) == 0) return true;
  }
  return false;
}

// Applies the stored timezone to the C library (localtime/mktime everywhere —
// idle matrix clock, upcoming-game times, the schedule-day window — follows
// it). Time itself was already NTP-synced by the shell's one-shot
// configTzTime(); a mid-session change only needs the TZ update, not a
// re-sync. Runs on the network task; a concurrent localtime on the render
// core can at worst produce one odd frame during the switch.
void applyTimezone() {
  setenv("TZ", tzString.c_str(), 1);
  tzset();
  DBG_PRINTF("[NET] timezone applied: %s\n", tzString.c_str());
}

String buildTzOptionsHtml(const char* selected) {
  String html = "";
  for (size_t i = 0; i < TZ_OPTIONS_COUNT; ++i) {
    html += "<option value=\"";
    html += TZ_OPTIONS[i].posix;
    html += "\"";
    if (strcmp(TZ_OPTIONS[i].posix, selected) == 0) html += " selected";
    html += ">";
    html += TZ_OPTIONS[i].label;
    html += "</option>";
  }
  return html;
}

uint32_t stateStartedAt = 0;
uint32_t lastProbeAt = 0;
uint32_t disconnectStartedAt = 0;
uint32_t onlineAt = 0;
uint32_t lastReconnectAt = 0;
uint32_t lastDebugAt = 0;
wl_status_t lastLoggedWiFiStatus = WL_NO_SHIELD;
bool setupScreenVisible = false;
// Portal priority mode: while someone is actively using the setup pages,
// background work pauses so the web server gets the core and the radio to
// itself (page loads over this device's marginal Wi-Fi stall otherwise).
// Every request refreshes the window.
uint32_t portalActiveUntil = 0;
void markPortalActivity() {
  portalActiveUntil = millis() + PORTAL_ACTIVITY_WINDOW_MS;
}

// Only delays the very first boot's CONNECTING->ONLINE transition (see runNetworkStateMachine),
// so the Wi-Fi setup/connecting screen isn't just a flash; later reconnects skip this.
uint32_t networkTaskStartedAt = 0;
bool firstBootConnectHeld = true;

// Cross-task handoff to the main loop, which owns the display. The network
// task only sets these; handleNetworkDisplay() reads and clears them.
enum SetupDisplayMode { SETUP_CONNECTING, SETUP_AP_INSTRUCTIONS, SETUP_ONLINE_PORTAL };
volatile bool redrawSetupPending = false;
volatile SetupDisplayMode redrawModeV = SETUP_CONNECTING;
volatile uint32_t setupIpV = 0;
volatile bool releasePending = false;

// Async scan cache, only touched from the network task (server handlers run there).
String scanOptionsHtml;
bool scanActive = false;
uint32_t lastScanAt = 0;

void requestSetupRedraw(SetupDisplayMode mode, IPAddress ip) {
  redrawModeV = mode;
  setupIpV = ip;
  redrawSetupPending = true;
}

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_CONNECTED: return "CONNECTED";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    case WL_IDLE_STATUS: return "IDLE";
    default: return "OTHER";
  }
}

void logWiFiStatus(bool force = false) {
  wl_status_t status = WiFi.status();
  if (!force && status == lastLoggedWiFiStatus && millis() - lastDebugAt < NETWORK_DEBUG_INTERVAL_MS) {
    return;
  }
  lastLoggedWiFiStatus = status;
  lastDebugAt = millis();
  Serial.printf("[NET %lu] state=%s wifi=%s rssi=%d ip=%s ap=%s\n",
                millis(),
                state == ONLINE ? "ONLINE" : (state == CONNECTING ? "CONNECTING" : "PROVISIONING"),
                wifiStatusName(status),
                WiFi.RSSI(),
                WiFi.localIP().toString().c_str(),
                WiFi.softAPIP().toString().c_str());
}

void startPortalInfrastructure() {
  if (!dnsRunning) {
    dnsServer.start(53, "*", WiFi.softAPIP());
    dnsRunning = true;
  }
}

void stopPortalInfrastructure() {
  if (dnsRunning) {
    dnsServer.stop();
    dnsRunning = false;
  }
  if (apUp) {
    WiFi.softAPdisconnect(true);
    apUp = false;
  }
}

bool probeInternet() {
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  http.begin(NETWORK_PROBE_ANCHOR_URL);
  int code = http.GET();
  http.end();
  return code > 0;
}

void startArduinoOTA() {
  if (otaStarted) {
    return;
  }
  ArduinoOTA.setHostname(deviceHostname.c_str());
  ArduinoOTA.setPassword(NETWORK_OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Network update starting");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] Network update complete");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progress: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]\n", error);
  });
  ArduinoOTA.begin();
  otaStarted = true;
  Serial.println("[OTA] Arduino network OTA ready");
}

void enterOnline() {
  state = ONLINE;
  onlineAt = millis();
  if (hasPending) {
    preferences.begin("network", false);
    preferences.putString("ssid", pendingSsid);
    preferences.putString("password", pendingPassword);
    preferences.end();
    savedSsid = pendingSsid;
    savedPassword = pendingPassword;
    hasPending = false;
  }
  stopPortalInfrastructure();
  startArduinoOTA();
  requestSetupRedraw(SETUP_ONLINE_PORTAL, WiFi.localIP());
  Serial.printf("[NET] Online: ip=%s rssi=%d gateway=%s\n",
                WiFi.localIP().toString().c_str(),
                WiFi.RSSI(),
                WiFi.gatewayIP().toString().c_str());
  logWiFiStatus(true);
}

void enterProvisioning() {
  state = PROVISIONING;
  stateStartedAt = millis();
  WiFi.disconnect();
  if (!apUp) {
    WiFi.softAP(apSsid.c_str(), NETWORK_AP_PASSWORD);
    apUp = true;
    startPortalInfrastructure();
  }
  requestSetupRedraw(SETUP_AP_INSTRUCTIONS, WiFi.softAPIP());
  Serial.println("[NET] Provisioning: waiting for portal credentials");
  logWiFiStatus(true);
  Serial.printf("[NET] AP ready: ssid=%s ip=%s\n",
                apSsid.c_str(),
                WiFi.softAPIP().toString().c_str());
}

void enterConnecting() {
  state = CONNECTING;
  stateStartedAt = millis();
  lastProbeAt = 0;
  lastReconnectAt = millis();
  Serial.printf("[NET] Connecting to SSID '%s'\n", savedSsid.c_str());
  requestSetupRedraw(SETUP_CONNECTING, WiFi.softAPIP());
}

void tryReconnectWithSavedNetwork() {
  if (savedSsid.isEmpty()) {
    enterProvisioning();
    return;
  }
  // Keep the modem awake. Default Wi-Fi power save (min-modem sleep) makes
  // the ESP32 miss beacons on a marginal link and disassociate in storms a
  // minute or so after connect — exactly the drop/reconnect cycles that
  // killed the schedule/news fetches. The scoreboard is mains-powered, so
  // the extra ~40 mA is irrelevant.
  WiFi.setSleep(false);
  WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
  enterConnecting();
}

int prefTeam1 = 0; // populated from netDefaultTeams in loadSavedNetwork()
int prefTeam2 = 0;
int prefTeam3 = 0;

void loadSavedNetwork() {
  preferences.begin("network", true);
  savedSsid = preferences.getString("ssid", "");
  savedPassword = preferences.getString("password", "");
  prefTeam1 = preferences.getInt("team1", netDefaultTeams[0]);
  prefTeam2 = preferences.getInt("team2", netDefaultTeams[1]);
  prefTeam3 = preferences.getInt("team3", netDefaultTeams[2]);
  clockDisplayEnabled = preferences.getBool("show_clock", true);
  audioEnabled = preferences.getBool("audio_en", true);
  tzString = preferences.getString("tz", FACTORY_DEFAULT_TIMEZONE);
  if (!isValidTzString(tzString.c_str())) {
    tzString = FACTORY_DEFAULT_TIMEZONE;  // unknown/legacy value: fall back
  }
  applyTimezone();
  preferences.end();
}

String buildTeamOptionsHtml(int selectedId) {
  String html = "";
  for (size_t i = 0; i < netTeamOptionCount; i++) {
    html += "<option value=\"";
    html += String(netTeamOptions[i].id);
    html += "\"";
    if (netTeamOptions[i].id == selectedId) html += " selected";
    html += ">";
    html += netTeamOptions[i].label;
    html += "</option>";
  }
  return html;
}


String htmlEscape(const String& text) {
  String out;
  out.reserve(text.length());
  for (size_t i = 0; i < text.length(); ++i) {
    char c = text[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

void refreshScanCache() {
  int8_t result = WiFi.scanComplete();
  if (scanActive && result >= 0) {
    scanOptionsHtml = "";
    for (int index = 0; index < result; ++index) {
      String ssid = WiFi.SSID(index);
      if (!ssid.isEmpty()) {
        String escaped = htmlEscape(ssid);
        scanOptionsHtml += "<option value=\"" + escaped + "\">" + escaped + " (" +
                           String(WiFi.RSSI(index)) + " dBm)</option>";
      }
    }
    WiFi.scanDelete();
    scanActive = false;
    lastScanAt = millis();
  } else if (result == WIFI_SCAN_FAILED) {
    scanActive = false;
    lastScanAt = millis();
  }
  if (!scanActive && state != ONLINE && millis() - lastScanAt > NETWORK_SCAN_REFRESH_MS) {
    WiFi.scanNetworks(true, true);
    scanActive = true;
  }
}

void renderAccessPointInstructions(const String& portalAddress, bool stationConnected) {
  // Direct panel draw (no canvas): this screen replaces everything during
  // provisioning and nothing else renders while it is up.
  Adafruit_ST7789& display = tftPanel.raw();
  String portalUrl = "http://" + portalAddress + "/";

  display.fillScreen(0x012B);
  display.setTextColor(ST77XX_WHITE);
  display.setTextSize(2);
  display.setCursor(8, 8);
  // Wi-Fi is already established once online; only the AP-mode screen is initial setup.
  display.print(stationConnected ? "SCOREBOARD SETUP" : "SETUP WI-FI");
  display.setTextSize(1);
  display.setCursor(8, 38);
  if (stationConnected) {
    display.print("1. Wi-Fi Connected!");
    display.setCursor(8, 55);
    display.print("Device IP Address:");
    display.setTextSize(2);
    display.setTextColor(0xFD20); // Gold
    display.setCursor(8, 72);
    display.print(portalAddress);

    display.setTextSize(1);
    display.setTextColor(ST77XX_WHITE);
    display.setCursor(8, 105);
    display.print("2. Scan QR or open URL");
    display.setCursor(8, 120);
    display.print("to adjust team settings.");
  } else {
    display.print("1. Connect to:");
    display.setTextSize(2);
    display.setCursor(8, 51);
    display.print(apSsid);
    display.setTextSize(1);
    display.setCursor(8, 78);
    display.print("Password:");
    display.setTextSize(2);
    display.setCursor(8, 91);
    display.print(NETWORK_AP_PASSWORD);

    display.setTextSize(1);
    display.setCursor(8, 125);
    display.print("2. Scan QR or open:");
    display.setCursor(8, 139);
    display.print(portalAddress);
    display.setCursor(8, 165);
    display.print("3. Enter Wi-Fi settings");
  }

  // QR always links to this screen's portal address (AP portal or device LAN IP).
  uint8_t qrData[qrcode_getBufferSize(2)];
  QRCode qrCode;
  qrcode_initText(&qrCode, qrData, 2, ECC_LOW, portalUrl.c_str());

  const int scale = 4;
  const int left = 208;
  const int top = 65;
  display.fillRect(left - 4, top - 4, (qrCode.size * scale) + 8, (qrCode.size * scale) + 8, ST77XX_WHITE);
  for (uint8_t y = 0; y < qrCode.size; y++) {
    for (uint8_t x = 0; x < qrCode.size; x++) {
      if (qrcode_getModule(&qrCode, x, y)) {
        display.fillRect(left + (x * scale), top + (y * scale), scale, scale, ST77XX_BLACK);
      }
    }
  }
}

String readConfig() {
  File file = LittleFS.open("/config.json", "r");
  if (!file) {
    return DEFAULT_CONFIG;
  }
  String config = file.readString();
  file.close();
  return config;
}

void redirectToPortal() {
  markPortalActivity();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void servePortal() {
  markPortalActivity();
  server.sendHeader("Cache-Control", "max-age=300");
  String page = R"html(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>@@NAME@@ Setup</title><style>
body{margin:0;background:#061b46;color:#fff;font:16px system-ui,sans-serif}
main{max-width:440px;margin:5vh auto;padding:24px;background:#0b2b62;border:2px solid #dfe9ff;border-radius:8px}
h1{margin-top:0;font-size:24px}label{display:block;margin:14px 0 4px;font-weight:600}input,select{box-sizing:border-box;width:100%;padding:10px;border:0;border-radius:4px;font-size:15px}
button{margin-top:20px;width:100%;padding:12px;background:#f5c400;border:0;border-radius:4px;font-weight:700;font-size:16px;color:#000;cursor:pointer}.hint{color:#c5d3ee;font-size:14px;line-height:1.4}
hr{border:0;border-top:1px solid #1c4587;margin:20px 0}
</style></head><body><main><h1>@@NAME@@ Setup</h1>
<p class="hint">Configure Wi-Fi connection and select your favorite teams in order of priority.</p>
<form method="post" action="/save">
<label for="network">Nearby Wi-Fi Networks</label>
<select id="network" onchange="ssid.value=this.value"><option value="">Enter network manually</option>)html";
  page += scanOptionsHtml;
  page += R"html(</select>
<label for="ssid">Wi-Fi Network Name</label><input id="ssid" name="ssid" value=")html";
  page += htmlEscape(savedSsid);
  page += R"html(" required maxlength="32" autocomplete="off">
<label for="password">Wi-Fi Password</label><input id="password" name="password" type="password" maxlength="63" autocomplete="off">
<p class="hint">Leave the password blank and the network unchanged to save team preferences without touching the connection.</p>
<hr>
<h3>Favorite Team Priorities</h3>
<label for="team1">Priority 1 Team (Primary)</label><select id="team1" name="team1">)html";
  page += buildTeamOptionsHtml(prefTeam1);
  page += R"html(</select>
<label for="team2">Priority 2 Team</label><select id="team2" name="team2">)html";
  page += buildTeamOptionsHtml(prefTeam2);
  page += R"html(</select>
<label for="team3">Priority 3 Team</label><select id="team3" name="team3">)html";
  page += buildTeamOptionsHtml(prefTeam3);
  page += R"html(</select>
<hr><h3>Display Time Zone</h3>
<label for="tz">Used for game times, countdowns, and the idle clock</label>
<select id="tz" name="tz">)html";
  page += buildTzOptionsHtml(tzString.c_str());
  page += R"html(</select>
<hr><label style="display:flex;align-items:center;gap:10px" for="show-clock"><input style="width:auto" id="show-clock" name="show_clock" type="checkbox" value="1")html";
  if (clockDisplayEnabled) page += " checked";
  page += R"html(>Display current time on score boards when no game is live</label>
<label style="display:flex;align-items:center;gap:10px;margin-top:12px" for="audio-en"><input style="width:auto" id="audio-en" name="audio_en" type="checkbox" value="1")html";
  if (audioEnabled) page += " checked";
  page += R"html(>Enable audio output (game sounds and alerts)</label>
<button type="submit">Save & Connect Scoreboard</button></form>
<hr><h3>Display Test</h3>
<p class="hint">Cycles every display once (matrices, clock, penalty LEDs, screen), holds everything on for 2 seconds, then returns to what was showing.</p>
<button type="button" style="margin-top:8px" onclick="dtest()">Run Display Test</button>
<button type="button" style="margin-top:8px;background:#2a5daf;color:#fff" onclick="atest()">Play Test Audio</button>
<p class="hint" id="dtestStatus">&nbsp;</p>
<hr><h3>Manual Mode</h3>
<p class="hint">Full manual control: clock countdown, scores, period, penalty LEDs, shots — from a dedicated page.</p>
<button type="button" onclick="location.href='/manual'">Open Manual Mode</button>
<hr><h3>Firmware Update</h3>
<p class="hint">Installed: )html" + String(FIRMWARE_VERSION) + R"html(. Automatic checks run at boot.</p>
<button type="button" style="margin-top:8px" onclick="otaCheck()">Check for Update Now</button>
<p class="hint" id="otaStatus">&nbsp;</p>
<p class="hint">Latest binary (for manual updates):<br>
<a style="color:#f5c400;word-break:break-all" href=")html" + String(OTA_LATEST_BIN_URL) + R"html(">)html" + String(OTA_LATEST_BIN_URL) + R"html(</a></p>
<p><a style="color:#f5c400" href="/update">Upload a firmware file manually</a></p>
<script>
var otaWaiting=false;
function dtest(){document.getElementById('dtestStatus').textContent='Running - watch the scoreboard...';fetch('/display/test',{method:'POST'}).then(function(){setTimeout(function(){document.getElementById('dtestStatus').textContent='Done.'},18000)}).catch(function(){document.getElementById('dtestStatus').textContent='Request failed.'})}
function atest(){fetch('/audio/test',{method:'POST'})}
function otaCheck(){otaWaiting=true;document.getElementById('otaStatus').textContent='Checking...';fetch('/ota/check',{method:'POST'})}
setInterval(function(){fetch('/ota/status').then(function(r){return r.json()}).then(function(s){var e=document.getElementById('otaStatus');
if(s.stage==='DOWNLOADING'){otaWaiting=false;e.textContent='Downloading update '+s.progress+'% - watch the scoreboard; do not power off.'}
else if(s.stage==='REBOOTING'){otaWaiting=false;e.textContent='Update installed - rebooting...'}
else if(s.stage==='FAILED'){otaWaiting=false;e.textContent='Update failed (network may block GitHub) - use the manual upload below.'}
else if(otaWaiting&&s.checked&&!s.ok){otaWaiting=false;e.textContent='Check failed - this network may block GitHub. Use the manual upload below.'}
else if(otaWaiting&&s.checked&&s.ok){otaWaiting=false;e.textContent='No update available - firmware is current.'}
})},2000);
</script>
<hr><p class="hint"><a style="color:#f5c400" href="/update">Upload new firmware (.bin)</a></p></main></body></html>)html";
  page.replace("@@NAME@@", netBranding.deviceName);
  server.send(200, "text/html", page);
}

void serveStatus() {
  markPortalActivity();
  const char* name = state == ONLINE ? "online" : (state == CONNECTING ? "connecting" : "provisioning");
  server.send(200, "application/json", String("{\"state\":\"") + name + "\"}");
}

void handleDisplayTest() {
  // Fire-and-forget trigger: no markPortalActivity() on purpose — the
  // display test runs on the render core while feeds keep flowing.
  displayTestRequested = true;
  server.send(200, "text/plain", "ok");
}

void handleAudioTest() {
  audioTestRequested = true;
  server.send(200, "text/plain", "ok");
}

void serveManualPage() {
  markPortalActivity();
  manualMode = true;  // arriving at the page engages manual mode
  server.sendHeader("Cache-Control", "no-store");
  String page = R"html(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>@@NAME@@ Manual</title><style>
body{margin:0;background:#061b46;color:#fff;font:14px system-ui,sans-serif}
main{max-width:430px;margin:0 auto;padding:8px}
h1{font-size:16px;margin:0 0 2px}.muted{color:#c5d3ee;font-size:12px}
button{padding:8px;border:0;border-radius:6px;font-weight:700;font-size:15px;cursor:pointer;touch-action:manipulation}
.big{width:100%;padding:12px 0;font-size:19px}
.dec,.inc{width:46px;height:46px;background:#2a5daf;color:#fff;font-size:23px}
.val{display:inline-block;min-width:52px;text-align:center;font-size:21px;font-weight:800;margin:0 6px}
.row{display:flex;align-items:center;justify-content:center;margin:6px 0}
.cols{display:flex;gap:8px}.col{flex:1;background:#0b2b62;border:1px solid #1c4587;border-radius:8px;padding:8px}
.col h2{text-align:center;font-size:14px;margin:1px 0 6px}
.pen{width:100%;margin:3px 0;background:#31415f;color:#fff;padding:9px}
.pen.on{background:#c62828}
input{width:54px;padding:7px;font-size:16px;border:0;border-radius:6px;text-align:center}
hr{border:0;border-top:1px solid #1c4587;margin:8px 0}
.go{background:#2e7d32;color:#fff}.stop{background:#c62828;color:#fff}
.set{background:#f5c400;color:#000}.horn{background:#f5c400;color:#000;width:100%;padding:12px;font-size:17px}
.exit{background:#777;color:#fff;width:100%;padding:10px}
#clk{font-size:26px;font-weight:800;text-align:center;margin:4px 0;letter-spacing:2px}
h2.ctr{text-align:center;font-size:14px;margin:2px 0}
</style></head><body><main>
<h1>@@NAME@@ &mdash; Manual Mode</h1>
<hr><h2 class="ctr">Clock</h2>
<div id="clk">--:--</div>
<div class="row"><input id="mm" inputmode="numeric" maxlength="2" value="20"> : <input id="ss" inputmode="numeric" maxlength="2" value="00">
<button class="set" onclick="setClock()">Set</button></div>
<div class="row"><span class="muted">period length</span>
<input id="plm" inputmode="numeric" maxlength="2" value="@@PLM@@"> : <input id="pls" inputmode="numeric" maxlength="2" value="@@PLS@@">
<button class="set" onclick="setLen()">Set</button></div>
<p class="muted" style="text-align:center;margin:2px 0">at 0:00: buzzer &rarr; next period reloads at this length</p>
<div class="row"><button id="runBtn" class="big go" onclick="toggleRun()">Start</button></div>
<div class="cols">
<div class="col"><h2>HOME</h2>
<p class="muted" style="text-align:center;margin:1px 0">score</p>
<div class="row"><button class="dec" onclick="adj('hs',-1)">&minus;</button><span class="val" id="hs">0</span><button class="inc" onclick="adj('hs',1)">+</button></div>
<p class="muted" style="text-align:center;margin:1px 0">shots</p>
<div class="row"><button class="dec" onclick="adj('hsh',-1)">&minus;</button><span class="val" id="hsh">0</span><button class="inc" onclick="adj('hsh',1)">+</button></div>
<button class="pen" id="pen0" onclick="togPen(0)">Penalty 1</button>
<button class="pen" id="pen1" onclick="togPen(1)">Penalty 2</button>
</div>
<div class="col"><h2>GUEST</h2>
<p class="muted" style="text-align:center;margin:1px 0">score</p>
<div class="row"><button class="dec" onclick="adj('gs',-1)">&minus;</button><span class="val" id="gs">0</span><button class="inc" onclick="adj('gs',1)">+</button></div>
<p class="muted" style="text-align:center;margin:1px 0">shots</p>
<div class="row"><button class="dec" onclick="adj('gsh',-1)">&minus;</button><span class="val" id="gsh">0</span><button class="inc" onclick="adj('gsh',1)">+</button></div>
<button class="pen" id="pen2" onclick="togPen(2)">Penalty 1</button>
<button class="pen" id="pen3" onclick="togPen(3)">Penalty 2</button>
</div>
</div>
<div class="row"><span class="muted">period</span>
<button class="dec" onclick="adj('per',-1)">&minus;</button><span class="val" id="per">1</span><button class="inc" onclick="adj('per',1)">+</button></div>
<button class="horn" onclick="fetch('/audio/test',{method:'POST'})">&#128383; Goal Horn</button>
<hr><button class="exit" onclick="location.href='/manual/exit'">Exit Manual Mode</button>
</main></body></html>
<script>
var s={run:false,hs:0,gs:0,per:1,hsh:0,gsh:0,pen:0,sec:1200};
var lastSend=0;
function fmt(t){var m=Math.floor(t/60),x=t%60;return (m<10?'0':'')+m+':'+(x<10?'0':'')+x}
function paint(){document.getElementById('clk').textContent=fmt(s.sec);
for(var k of ['hs','gs','per','hsh','gsh'])document.getElementById(k).textContent=s[k];
for(var i=0;i<4;i++){var b=document.getElementById('pen'+i);b.className='pen'+((s.pen>>i)&1?' on':'')}
var r=document.getElementById('runBtn');r.textContent=s.run?'Stop':'Start';r.className='big '+(s.run?'stop':'go')}
function send(){var p=new URLSearchParams();p.set('sec',s.sec);p.set('run',s.run?1:0);
p.set('hs',s.hs);p.set('gs',s.gs);p.set('per',s.per);p.set('hsh',s.hsh);p.set('gsh',s.gsh);p.set('pen',s.pen);
lastSend=Date.now();
fetch('/manual/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()});paint()}
function adj(k,d){s[k]=Math.max(k=='per'?1:0,s[k]+d);if(k=='per'&&s[k]>9)s[k]=9;if(k!='per'&&s[k]>99)s[k]=99;send()}
function togPen(i){s.pen^=(1<<i);send()}
function setClock(){var m=parseInt(document.getElementById('mm').value||'0'),x=parseInt(document.getElementById('ss').value||'0');
if(isNaN(m)||isNaN(x)||m<0||m>99||x<0||x>59)return;s.sec=m*60+x;s.run=false;send()}
function setLen(){var m=parseInt(document.getElementById('plm').value||'0'),x=parseInt(document.getElementById('pls').value||'0');
if(isNaN(m)||isNaN(x)||m<0||m>99||x<0||x>59||m*60+x==0)return;var p=new URLSearchParams();p.set('plen',m*60+x);
fetch('/manual/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()})}
function toggleRun(){s.run=!s.run;send()}
setInterval(function(){if(Date.now()-lastSend<1500)return;
fetch('/manual/state').then(function(r){return r.json()}).then(function(d){
s.sec=d.sec;s.run=d.run;s.per=d.per;s.hs=d.hs;s.gs=d.gs;s.hsh=d.hsh;s.gsh=d.gsh;s.pen=d.pen;paint()})},1000);
paint();
</script>)html";
  page.replace("@@NAME@@", netBranding.deviceName);
  char plm[4], pls[4];
  snprintf(plm, sizeof(plm), "%d", (int)(manualPeriodLenSec / 60));
  snprintf(pls, sizeof(pls), "%02d", (int)(manualPeriodLenSec % 60));
  page.replace("@@PLM@@", plm);
  page.replace("@@PLS@@", pls);
  server.send(200, "text/html", page);
}

void handleManualSet() {
  // The page sends every value on every button tap, so only a CHANGING
  // sec/per cancels a pending end-of-period advance — score/penalty taps
  // during the 0:00 settle window must not.
  if (server.hasArg("sec")) {
    int sec = server.arg("sec").toInt();
    if (sec >= 0 && sec <= 99 * 60 + 59 && sec != manualClockSec) {
      manualClockSec = sec;
      manualPeriodEndAtMs = 0;
    }
  }
  if (server.hasArg("plen")) {
    int len = server.arg("plen").toInt();
    if (len > 0 && len <= 99 * 60 + 59) manualPeriodLenSec = len;
  }
  if (server.hasArg("run")) {
    manualClockRunning = server.arg("run").toInt() != 0 &&
                         manualClockSec > 0;
  }
  if (server.hasArg("hs")) manualHomeScore = clampScore(server.arg("hs").toInt());
  if (server.hasArg("gs")) manualGuestScore = clampScore(server.arg("gs").toInt());
  if (server.hasArg("per")) {
    int p = server.arg("per").toInt();
    p = (p >= 1 && p <= 9) ? p : 1;
    if (p != manualPeriod) {
      manualPeriod = p;
      manualPeriodEndAtMs = 0;
    }
  }
  if (server.hasArg("hsh")) manualHomeShots = clampScore(server.arg("hsh").toInt());
  if (server.hasArg("gsh")) manualGuestShots = clampScore(server.arg("gsh").toInt());
  if (server.hasArg("pen")) {
    int m = server.arg("pen").toInt();
    manualPenaltyMask = (m >= 0 && m <= 15) ? m : 0;
  }
  server.send(200, "text/plain", "ok");
}

void handleManualState() {
  // 1 Hz poll from the manual page so it follows device-side changes
  // (countdown, end-of-period advance). Deliberately no
  // markPortalActivity(): a read shouldn't pause the feeds.
  String json = String("{\"sec\":") + String((int)manualClockSec) +
      ",\"run\":" + String(manualClockRunning ? 1 : 0) +
      ",\"per\":" + String((int)manualPeriod) +
      ",\"hs\":" + String((int)manualHomeScore) +
      ",\"gs\":" + String((int)manualGuestScore) +
      ",\"hsh\":" + String((int)manualHomeShots) +
      ",\"gsh\":" + String((int)manualGuestShots) +
      ",\"pen\":" + String((int)manualPenaltyMask) + "}";
  server.send(200, "application/json", json);
}

void handleManualExit() {
  markPortalActivity();
  manualMode = false;
  manualClockRunning = false;
  manualPeriodEndAtMs = 0;
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void saveNetwork() {
  markPortalActivity();
  pendingSsid = server.arg("ssid");
  pendingPassword = server.arg("password");
  if (server.hasArg("team1")) prefTeam1 = server.arg("team1").toInt();
  if (server.hasArg("team2")) prefTeam2 = server.arg("team2").toInt();
  if (server.hasArg("team3")) prefTeam3 = server.arg("team3").toInt();
  clockDisplayEnabled = server.hasArg("show_clock");
  audioEnabled = server.hasArg("audio_en");
  // Timezone is validated against the option table before it is trusted.
  bool tzChanged = false;
  if (server.hasArg("tz")) {
    String requestedTz = server.arg("tz");
    if (isValidTzString(requestedTz.c_str()) &&
        !requestedTz.equals(tzString)) {
      tzString = requestedTz;
      tzChanged = true;
    }
  }

  // Network settings only count as changed when a new SSID is supplied or a
  // password is (re-)entered; otherwise this is a team-preferences-only save
  // and the Wi-Fi connection must be left alone.
  bool networkChanging = !pendingSsid.isEmpty() &&
                         (pendingSsid != savedSsid || !pendingPassword.isEmpty());

  if (savedSsid.isEmpty() && !networkChanging) {
    server.send(400, "text/plain", "Wi-Fi network name is required");
    return;
  }

  // Team/timezone preferences always persist; credentials only when they changed.
  preferences.begin("network", false);
  preferences.putInt("team1", prefTeam1);
  preferences.putInt("team2", prefTeam2);
  preferences.putInt("team3", prefTeam3);
  preferences.putBool("show_clock", clockDisplayEnabled);
  preferences.putBool("audio_en", audioEnabled);
  preferences.putString("tz", tzString);
  if (networkChanging) {
    preferences.putString("ssid", pendingSsid);
    preferences.putString("password", pendingPassword);
  }
  preferences.end();

  if (tzChanged) {
    applyTimezone();  // game times + idle clock follow on the next render
  }

  if (!networkChanging) {
    Serial.printf("[NET] Preferences-only save: teams=[%d, %d, %d] tz=%s (network untouched)\n",
                  prefTeam1, prefTeam2, prefTeam3, tzString.c_str());
    server.send(200, "text/html", R"html(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved</title><style>body{margin:0;background:#061b46;color:#fff;font:16px system-ui,sans-serif}
main{max-width:420px;margin:8vh auto;padding:28px;background:#0b2b62;border:2px solid #dfe9ff;border-radius:8px;text-align:center}
</style></head><body><main><h1>Settings Saved</h1>
<p>Team priorities and time zone updated — the scoreboard picks them up within a minute.</p>
<p>Wi-Fi settings were not changed.</p>
<p><a style="color:#f5c400" href="/">Back to configuration</a></p></main></body></html>)html");
    return;
  }

  savedSsid = pendingSsid;
  savedPassword = pendingPassword;
  hasPending = true;

  WiFi.disconnect();
  WiFi.begin(pendingSsid.c_str(), pendingPassword.c_str());
  enterConnecting();

  server.send(200, "text/html", R"html(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Connecting</title><style>body{margin:0;background:#061b46;color:#fff;font:16px system-ui,sans-serif}
main{max-width:420px;margin:8vh auto;padding:28px;background:#0b2b62;border:2px solid #dfe9ff;border-radius:8px;text-align:center}
#status{font-size:20px;font-weight:700}</style></head><body><main><h1>Connecting...</h1>
<p id="status">Trying the network and checking internet access.</p>
<p><a style="color:#f5c400" href="/">Back to configuration</a></p>
<script>setInterval(function(){fetch('/status').then(function(r){return r.json()}).then(function(s){
if(s.state==='online'){document.getElementById('status').textContent='Connected! The scoreboard is going online.'}
else if(s.state==='provisioning'){document.getElementById('status').textContent='Could not connect or no internet. Check the password and try again.'}
})},2000)</script></main></body></html>)html");
  Serial.printf("Saved network '%s' and Teams [%d, %d, %d]\n", pendingSsid.c_str(), prefTeam1, prefTeam2, prefTeam3);
}


void saveConfig() {
  markPortalActivity();
  if (server.arg("portal") != NETWORK_PORTAL_PASSWORD) {
    server.send(401, "text/plain", "Invalid portal password");
    return;
  }
  JsonDocument document;
  if (deserializeJson(document, server.arg("config"))) {
    server.send(400, "text/plain", "Invalid JSON configuration");
    return;
  }
  File file = LittleFS.open("/config.tmp", "w");
  if (!file) {
    server.send(500, "text/plain", "Unable to write configuration");
    return;
  }
  serializeJson(document, file);
  file.close();
  LittleFS.remove("/config.json");
  LittleFS.rename("/config.tmp", "/config.json");
  server.send(200, "text/html", "<h1>Saved</h1><p>Runtime settings updated.</p>");
}

void serveConfig() {
  markPortalActivity();
  server.send(200, "application/json", readConfig());
}

void handleOtaCheckNow() {
  markPortalActivity();
  if (!isOnline()) {
    server.send(503, "text/plain", "Not online");
    return;
  }
  requestOtaCheckNow();
  server.send(200, "application/json", "{\"ok\":true}");
}

void serveOtaStatus() {
  markPortalActivity();
  const char* stage = "NONE";
  switch (getOtaStage()) {
    case OtaStage::DOWNLOADING: stage = "DOWNLOADING"; break;
    case OtaStage::REBOOTING:   stage = "REBOOTING";   break;
    case OtaStage::FAILED:      stage = "FAILED";      break;
    default: break;
  }
  char body[96];
  snprintf(body, sizeof(body),
           "{\"stage\":\"%s\",\"progress\":%d,\"checked\":%s,\"ok\":%s}",
           stage, getOtaProgress(),
           otaEverChecked() ? "true" : "false",
           otaLastCheckOk() ? "true" : "false");
  server.send(200, "application/json", body);
}

void serveUpdatePage() {
  markPortalActivity();
  server.send(200, "text/html", R"html(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Firmware Update</title><style>
body{margin:0;background:#061b46;color:#fff;font:16px system-ui,sans-serif}
main{max-width:440px;margin:8vh auto;padding:24px;background:#0b2b62;border:2px solid #dfe9ff;border-radius:8px}
h1{margin-top:0;font-size:24px}input{box-sizing:border-box;width:100%;padding:10px;border:0;border-radius:4px;font-size:15px;background:#fff}
button{margin-top:16px;width:100%;padding:12px;background:#f5c400;border:0;border-radius:4px;font-weight:700;font-size:16px;color:#000;cursor:pointer}
.hint{color:#c5d3ee;font-size:14px;line-height:1.4}
</style></head><body><main><h1>Firmware Update</h1>
<p class="hint">Upload a new compiled .bin firmware image. The scoreboard will reboot automatically once the update finishes.</p>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="update" accept=".bin" required>
<button type="submit">Upload & Flash</button>
</form></main></body></html>)html");
}

void handleUpdateUpload() {
  markPortalActivity();
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[UPDATE] Receiving firmware: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[UPDATE] Success: %u bytes. Rebooting...\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
  }
}

void handleUpdateResult() {
  markPortalActivity();
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", Update.hasError() ? "Update FAILED" : "Update OK, rebooting...");
  delay(500);
  ESP.restart();
}

void registerPortalRoutes() {
  server.on("/", HTTP_GET, servePortal);
  server.on("/status", HTTP_GET, serveStatus);
  server.on("/config", HTTP_GET, serveConfig);
  server.on("/save", HTTP_POST, saveNetwork);
  server.on("/config", HTTP_POST, saveConfig);
  server.on("/update", HTTP_GET, serveUpdatePage);
  server.on("/ota/check", HTTP_POST, handleOtaCheckNow);
  server.on("/display/test", HTTP_POST, handleDisplayTest);
  server.on("/audio/test", HTTP_POST, handleAudioTest);
  server.on("/manual", HTTP_GET, serveManualPage);
  server.on("/manual/set", HTTP_POST, handleManualSet);
  server.on("/manual/state", HTTP_GET, handleManualState);
  server.on("/manual/exit", HTTP_GET, handleManualExit);
  server.on("/ota/status", HTTP_GET, serveOtaStatus);
  server.on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);
  server.onNotFound(redirectToPortal);
}

void runNetworkStateMachine() {
  logWiFiStatus();
  if (state == CONNECTING) {
    if (WiFi.status() != WL_CONNECTED && millis() - lastReconnectAt >= NETWORK_RECONNECT_RETRY_MS) {
      lastReconnectAt = millis();
      Serial.println("[NET] Retrying saved Wi-Fi connection");
      WiFi.reconnect();
    }
    bool holdForFirstBoot = firstBootConnectHeld &&
                            millis() - networkTaskStartedAt < NETWORK_FIRST_CONNECT_MIN_MS;
    if (WiFi.status() == WL_CONNECTED && !holdForFirstBoot) {
      firstBootConnectHeld = false;
      enterOnline();
    } else if (millis() - stateStartedAt >= NETWORK_CONNECT_AND_PROBE_TIMEOUT_MS) {
      enterProvisioning();
    }
  } else if (state == ONLINE) {
    if (WiFi.status() == WL_CONNECTED) {
      disconnectStartedAt = 0;
    } else {
      if (disconnectStartedAt == 0) {
        disconnectStartedAt = millis();
      } else if (millis() - disconnectStartedAt >= NETWORK_RECONNECT_GRACE_MS) {
        Serial.println("[NET] Connection lost; retrying saved network");
        setupScreenVisible = true;
        tryReconnectWithSavedNetwork();
      }
    }
    if (setupScreenVisible && millis() - onlineAt >= NETWORK_SETUP_SCREEN_MS) {
      setupScreenVisible = false;
      releasePending = true;
    }
  } else if (state == PROVISIONING) {
    if (!savedSsid.isEmpty() && millis() - stateStartedAt >= NETWORK_PROVISIONING_RETRY_MS) {
      Serial.println("[NET] Retrying saved Wi-Fi after provisioning timeout");
      tryReconnectWithSavedNetwork();
    }
  }
}
} // namespace

bool isOnline() {
  return state == ONLINE;
}

bool isProvisioning() {
  return state == PROVISIONING;
}

bool portalEngaged() {
  return millis() < portalActiveUntil;
}

bool consumeDisplayTestRequest() {
  if (!displayTestRequested) return false;
  displayTestRequested = false;
  return true;
}

bool consumeAudioTestRequest() {
  if (!audioTestRequested) return false;
  audioTestRequested = false;
  return true;
}

bool isManualMode() { return manualMode; }
void setManualMode(bool on) { manualMode = on; }
int  getManualClockSec() { return manualClockSec; }
bool getManualClockRunning() { return manualClockRunning; }
int  getManualHomeScore() { return manualHomeScore; }
int  getManualGuestScore() { return manualGuestScore; }
int  getManualPeriod() { return manualPeriod; }
int  getManualPenaltyMask() { return manualPenaltyMask; }
int  getManualHomeShots() { return manualHomeShots; }
int  getManualGuestShots() { return manualGuestShots; }

bool consumeManualPeriodEndBuzzer() {
  if (!manualPeriodEndBuzzer) return false;
  manualPeriodEndBuzzer = false;
  return true;
}

const char* getSavedWifiSsid() {
  return savedSsid.c_str();
}

String getDeviceIp() {
  return (state == ONLINE) ? WiFi.localIP().toString() : String("");
}

bool isClockDisplayEnabled() {
  return clockDisplayEnabled;
}

// Audio output toggle (NVS "audio_en", default on). Consumed by the sport
// layer's sound playback (goal horn and cues) once the audio HAL lands.
bool isAudioEnabled() {
  return audioEnabled;
}

// Effective display timezone as a POSIX TZ string ("EST5EDT,M3.2.0,M11.1.0").
// Selected in the setup portal, persisted in NVS ("tz"); the factory default
// only applies before the first selection is saved.
const char* getTzString() {
  return tzString.c_str();
}

void getPreferredTeamIds(int outTeamIds[3]) {
  outTeamIds[0] = prefTeam1;
  outTeamIds[1] = prefTeam2;
  outTeamIds[2] = prefTeam3;
}


void startNetworkServices(const NetworkBranding& branding,
                          const NetworkTeamOption* teamOptions,
                          size_t teamOptionCount,
                          const int defaultPreferredTeams[3]) {
  netBranding = branding;
  netTeamOptions = teamOptions;
  netTeamOptionCount = teamOptionCount;
  netDefaultTeams[0] = defaultPreferredTeams[0];
  netDefaultTeams[1] = defaultPreferredTeams[1];
  netDefaultTeams[2] = defaultPreferredTeams[2];
  deviceHostname = branding.hostname;
  apSsid = branding.apSsid;
  networkTaskStartedAt = millis();
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS unavailable; runtime settings disabled");
  }
  registerPortalRoutes();
  WiFi.mode(WIFI_AP_STA);
  // Keep the modem awake from the very first connection (previously only
  // reconnects did this): default power save makes the radio sleep between
  // beacons, which stalls inbound portal page loads from phones.
  WiFi.setSleep(false);
  // Unique per-board hostname built on the sport's branding base
  // ("nhl-scoreboard-a68d0") so mDNS/OTA targets and router client lists
  // name the right device; the suffix disambiguates multiple boards.
  String macAddress = WiFi.macAddress();
  macAddress.replace(":", "");
  if (macAddress.length() >= 5) {
    deviceHostname = String(branding.hostname) + "-" +
                     macAddress.substring(macAddress.length() - 5);
    deviceHostname.toLowerCase();
  }
  WiFi.setHostname(deviceHostname.c_str());
  Serial.printf("[NET] Device hostname: %s\n", deviceHostname.c_str());
  // The web server starts now and serves over whichever interface is up,
  // but the setup AP only comes up in enterProvisioning() — i.e. when
  // there's genuinely no saved network or connecting failed. Starting it
  // eagerly at every boot broadcast the setup AP for the whole connect
  // window, letting phones that remember it auto-join and lose their
  // route the moment the device went online (setup page then "hung").
  server.begin();
  setupScreenVisible = true;

  loadSavedNetwork();
  if (savedSsid.isEmpty()) {
    enterProvisioning();
  } else {
    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    enterConnecting();
  }
}

void handleNetworkServices() {
  if (dnsRunning) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
  if (otaStarted) {
    ArduinoOTA.handle();
  }
  refreshScanCache();
  runNetworkStateMachine();
}

void handleNetworkDisplay() {
  if (redrawSetupPending) {
    redrawSetupPending = false;
    IPAddress ip(setupIpV);
    switch (redrawModeV) {
      case SETUP_CONNECTING:
      case SETUP_ONLINE_PORTAL:
        // Connecting and online transitions draw nothing: the boot logo
        // stays on screen while Wi-Fi connects behind it (loop() also
        // holds the logo for BOOT_SPLASH_HOLD_MS). Only the AP
        // provisioning instructions ever replace the logo — they're the
        // one screen the device can't work without.
        break;
      case SETUP_AP_INSTRUCTIONS:
        renderAccessPointInstructions(ip.toString(), false);
        break;
    }
  }
}

bool consumeScoreboardRelease() {
  if (!releasePending) {
    return false;
  }
  releasePending = false;
  return true;
}

void startNetworkTask() {
  xTaskCreatePinnedToCore(
    [](void*) {
      while (true) {
        handleNetworkServices();
        tickManualClock();
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    },
    "NetworkTask",
    8192,
    nullptr,
    2,  // above the NHL data task: the portal must preempt feed fetches
    nullptr,
    0
  );
}
