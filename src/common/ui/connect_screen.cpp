#include <Arduino.h>
#include <WiFi.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <math.h>

#include "config.h"
#include "common/comms/network_service.h"
#include "common/hal/tft_panel.h"
#include "common/ui/connect_screen.h"
#include "common/ui/gfx.h"

namespace {

// Layout (320x240 landscape).
const int TITLE_Y = 14;
const int RULE_Y = 38;
const int LABEL_Y = 48;
const int SSID_Y = 60;
// Wi-Fi icon: a dot with three arcs above it, inside a small box so each
// animation frame pushes only these pixels.
const int ICON_X = 116, ICON_Y = 82, ICON_W = 88, ICON_H = 54;
const int ICON_CX = 160, ICON_CY = 130;
const int ICON_RADII[3] = {13, 25, 37};
// Status block, repainted once a second: step, detail, bar, countdown.
const int STATUS_TOP = 140, STATUS_H = 82;
const int STEP_Y = 144;
const int DETAIL_Y = 166;
const int BAR_X = 20, BAR_Y = 182, BAR_W = 280, BAR_H = 12;
const int HINT_Y = 204;
const int VERSION_Y = 228;

const uint16_t ARC_DIM = 0x2945;  // unlit arc: faint slate

const uint32_t ICON_FRAME_MS = 400;
const uint32_t STATUS_REFRESH_MS = 1000;

bool sDirty = true;
uint32_t sDrawnAttempt = 0;
uint8_t sIconFrame = 0;
uint32_t sLastIconAt = 0;
uint32_t sLastStatusAt = 0;
int sLastWifiStatus = -1;

const char* titleFor(ConnectReason reason) {
  switch (reason) {
    case ConnectReason::LINK_LOST:   return "WI-FI CONNECTION LOST";
    case ConnectReason::SETUP_RETRY: return "RETRYING WI-FI";
    case ConnectReason::NEW_NETWORK: return "JOINING NEW NETWORK";
    default:                         return "CONNECTING TO WI-FI";
  }
}

// What the radio is doing, from the Wi-Fi driver's status.
void describeStep(int wifiStatus, const char*& step, const char*& detail) {
  switch (wifiStatus) {
    case WL_IDLE_STATUS:  // associated, waiting on DHCP
      step = "Getting IP address";
      detail = "Joined - waiting for the router";
      break;
    case WL_NO_SSID_AVAIL:
      step = "Network not found";
      detail = "Is the router on and in range?";
      break;
    case WL_CONNECT_FAILED:
      step = "Connection refused";
      detail = "Check the Wi-Fi password in setup";
      break;
    case WL_CONNECTION_LOST:
      step = "Signal lost";
      detail = "Weak signal - trying again";
      break;
    case WL_CONNECTED:
      step = "Connected!";
      detail = "Starting the scoreboard";
      break;
    default:  // WL_DISCONNECTED: association in progress
      step = "Joining network";
      detail = "Looking for the router";
      break;
  }
}

// Thick arc from 45 to 135 degrees, drawn as a chain of small dots.
void drawArc(GFXcanvas16& c, int radius, uint16_t color) {
  for (int deg = 45; deg <= 135; deg += 3) {
    float rad = deg * (float)M_PI / 180.0f;
    int x = ICON_CX + (int)lroundf(radius * cosf(rad));
    int y = ICON_CY - (int)lroundf(radius * sinf(rad));
    c.fillCircle(x, y, 2, color);
  }
}

// Frame 0 = dot only; each frame lights one more arc (a "searching" sweep).
void drawWifiIcon(GFXcanvas16& c, uint8_t frame) {
  c.fillRect(ICON_X, ICON_Y, ICON_W, ICON_H, COLOR_BG);
  for (int i = 0; i < 3; ++i) {
    drawArc(c, ICON_RADII[i], i < frame ? COLOR_GOLD : ARC_DIM);
  }
  c.fillCircle(ICON_CX, ICON_CY, 4, COLOR_GOLD);
}

void drawStatusBlock(GFXcanvas16& c, const ConnectProgress& p) {
  c.fillRect(0, STATUS_TOP, 320, STATUS_H, COLOR_BG);

  const char* step;
  const char* detail;
  describeStep(p.wifiStatus, step, detail);
  c.setTextColor(ST77XX_WHITE);
  c.setTextSize(2);
  drawCenteredText(c, step, 160, STEP_Y);
  c.setTextColor(COLOR_MUTED);
  c.setTextSize(1);
  drawCenteredText(c, detail, 160, DETAIL_Y);

  uint32_t elapsed = p.elapsedMs < p.budgetMs ? p.elapsedMs : p.budgetMs;
  int fillW = p.budgetMs > 0
                  ? (int)((uint64_t)elapsed * (BAR_W - 4) / p.budgetMs)
                  : 0;
  c.drawRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, 3, COLOR_MUTED);
  if (fillW > 0) {
    c.fillRoundRect(BAR_X + 2, BAR_Y + 2, fillW, BAR_H - 4, 2, COLOR_GOLD);
  }

  unsigned elapsedSec = (unsigned)(elapsed / 1000);
  unsigned leftSec = (unsigned)((p.budgetMs - elapsed + 999) / 1000);
  char hint[64];
  snprintf(hint, sizeof(hint), "%u s, try %u - %s in %u s if it fails",
           elapsedSec, (unsigned)p.retries + 1,
           p.reason == ConnectReason::SETUP_RETRY ? "back to setup"
                                                  : "setup mode",
           leftSec);
  c.setTextColor(COLOR_MUTED);
  drawCenteredText(c, hint, 160, HINT_Y);
}

void paintFull(const ConnectProgress& p) {
  GFXcanvas16& c = tftPanel.canvas();
  c.fillScreen(COLOR_BG);

  c.setTextColor(COLOR_GOLD);
  c.setTextSize(2);
  drawCenteredText(c, titleFor(p.reason), 160, TITLE_Y);
  c.drawFastHLine(24, RULE_Y, 272, COLOR_GOLD);

  c.setTextColor(COLOR_MUTED);
  c.setTextSize(1);
  drawCenteredText(c, "Network", 160, LABEL_Y);
  // Size 2 fits 25 characters across the panel; longer names drop to 1.
  bool big = strlen(p.ssid) <= 25;
  c.setTextColor(ST77XX_WHITE);
  c.setTextSize(big ? 2 : 1);
  drawCenteredText(c, p.ssid[0] != '\0' ? p.ssid : "(no network saved)", 160,
                   big ? SSID_Y : SSID_Y + 4);

  drawWifiIcon(c, sIconFrame);
  drawStatusBlock(c, p);

  c.setTextColor(COLOR_GOLD);
  c.setTextSize(1);
  drawCenteredText(c, FIRMWARE_VERSION, 160, VERSION_Y);

  tftPanel.pushFull();
}

}  // namespace

void invalidateConnectScreen() { sDirty = true; }

void renderConnectScreen() {
  ConnectProgress p;
  if (!getConnectProgress(p)) return;
  uint32_t now = millis();

  if (sDirty || p.attemptId != sDrawnAttempt) {
    sDirty = false;
    sDrawnAttempt = p.attemptId;
    sIconFrame = 0;
    sLastIconAt = now;
    sLastStatusAt = now;
    sLastWifiStatus = p.wifiStatus;
    paintFull(p);
    return;
  }

  GFXcanvas16& c = tftPanel.canvas();
  if (now - sLastIconAt >= ICON_FRAME_MS) {
    sLastIconAt = now;
    sIconFrame = (sIconFrame + 1) % 4;
    drawWifiIcon(c, sIconFrame);
    tftPanel.pushRows(ICON_X, ICON_Y, ICON_W, ICON_H);
  }
  if (now - sLastStatusAt >= STATUS_REFRESH_MS ||
      p.wifiStatus != sLastWifiStatus) {
    sLastStatusAt = now;
    sLastWifiStatus = p.wifiStatus;
    drawStatusBlock(c, p);
    tftPanel.pushBand(STATUS_TOP, STATUS_H);
  }
}
