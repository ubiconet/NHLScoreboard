#include <Arduino.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "config.h"
#include "common/comms/network_service.h"
#include "common/hal/tft_panel.h"
#include "common/ui/boot_status.h"
#include "common/ui/gfx.h"
#include "common/ui/qr.h"

void renderBootStatusPage(const char* deviceName) {
  // Drawn once on entry and once more if connectivity flips during the
  // window; full-screen pushes are ~1 s on this bus, so no per-loop redraw.
  static bool sDrawn = false;
  static bool sDrawnOnline = false;
  bool online = isOnline();
  if (sDrawn && sDrawnOnline == online) return;
  sDrawn = true;
  sDrawnOnline = online;

  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillScreen(COLOR_BG);

  char heading[48];
  snprintf(heading, sizeof(heading), "%s", deviceName);
  for (char* p = heading; *p != '\0'; ++p) {
    *p = toupper((unsigned char)*p);  // "MLB SCOREBOARD" heading style
  }
  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(2);
  drawCenteredText(canvas, heading, 160, 26);
  canvas.drawFastHLine(24, 50, 272, COLOR_GOLD);

  char line[48];
  snprintf(line, sizeof(line), "Firmware %s", FIRMWARE_VERSION);
  canvas.setTextColor(ST77XX_WHITE);
  canvas.setTextSize(1);
  drawCenteredText(canvas, line, 160, 72);

  const char* ssid = getSavedWifiSsid();

  if (online) {
    // Left column: connection + activity status.
    String ip = getDeviceIp();
    canvas.setCursor(16, 102);
    canvas.print("Wi-Fi: ");
    canvas.print(ssid[0] != '\0' ? ssid : "--");
    canvas.setCursor(16, 120);
    canvas.print("Connected: ");
    canvas.print(ip);
    canvas.setTextColor(COLOR_MUTED);
    canvas.setCursor(16, 148);
    canvas.print("Checking for updates");
    canvas.setCursor(16, 162);
    canvas.print("Loading game data");

    // Right: QR straight to the device's setup page (http://<lan-ip>/).
    // Same version-2 code as the AP setup screen; ~22-char URLs fit.
    String portalUrl = "http://" + ip + "/";
    const int scale = 4;
    const int left = 208;
    const int top = 88;
    int modules = drawQr(canvas, portalUrl.c_str(), left, top, scale, 6);
    canvas.setTextColor(COLOR_MUTED);
    drawCenteredText(canvas, "scan for setup page",
                     left + (modules * scale) / 2, top + modules * scale + 10);
  } else {
    snprintf(line, sizeof(line), "Wi-Fi: %s", ssid[0] != '\0' ? ssid : "--");
    drawCenteredText(canvas, line, 160, 102);
    canvas.setTextColor(COLOR_MUTED);
    drawCenteredText(canvas, "Connecting to Wi-Fi...", 160, 124);
    drawCenteredText(canvas, "Update check and game data", 160, 152);
    drawCenteredText(canvas, "load once the network is up", 160, 166);
  }

  tftPanel.pushFull();
}
