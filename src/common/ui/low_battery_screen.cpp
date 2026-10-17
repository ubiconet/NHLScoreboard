#include <Arduino.h>
#include <Adafruit_GFX.h>

#include "config.h"
#include "common/hal/tft_panel.h"
#include "common/ui/gfx.h"
#include "common/ui/low_battery_screen.h"

namespace {

// Battery icon geometry (body + terminal nub), centered on the 320x240
// canvas. Only the charge segment inside the outline blinks.
constexpr int kBattX = 108;
constexpr int kBattY = 26;
constexpr int kBattW = 96;
constexpr int kBattH = 46;
constexpr int kBattNubW = 8;
constexpr int kBattNubH = 18;

void drawBatteryIcon(uint16_t chargeColor) {
  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.drawRoundRect(kBattX, kBattY, kBattW, kBattH, 6, COLOR_GOLD);
  canvas.drawRoundRect(kBattX + 1, kBattY + 1, kBattW - 2, kBattH - 2, 6, COLOR_GOLD);
  canvas.fillRoundRect(kBattX + kBattW, kBattY + (kBattH - kBattNubH) / 2,
                       kBattNubW, kBattNubH, 2, COLOR_GOLD);
  // Low-charge segment at the bottom of the body.
  canvas.fillRoundRect(kBattX + 5, kBattY + kBattH - 16, kBattW - 10, 11, 3,
                       chargeColor);
}

}  // namespace

void renderLowBatteryWarning(float volts, uint32_t holdMs) {
  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillScreen(COLOR_BG);
  drawBatteryIcon(COLOR_LED_RED);

  canvas.setTextColor(COLOR_LED_RED);
  canvas.setTextSize(2);
  drawCenteredText(canvas, "LOW BATTERY", 160, 96);

  canvas.setTextColor(COLOR_MUTED);
  canvas.setTextSize(1);
  if (volts > 0.5f) {
    char line[40];
    snprintf(line, sizeof(line), "Battery: %.2f V", volts);
    drawCenteredText(canvas, line, 160, 128);
  }
  drawCenteredText(canvas, "Shutting down to protect the battery.", 160, 158);
  drawCenteredText(canvas, "Charge the scoreboard to resume.", 160, 172);
  tftPanel.pushFull();

  // Flash the charge segment so the warning reads at a glance, ending lit.
  // Only the icon rect is repainted — a full canvas push costs over a
  // second on software SPI.
  const uint32_t start = millis();
  bool lit = true;
  while (millis() - start < holdMs) {
    delay(300);
    lit = !lit;
    drawBatteryIcon(lit ? COLOR_LED_RED : COLOR_BG);
    tftPanel.pushRows(kBattX - 2, kBattY - 2,
                      kBattW + kBattNubW + 6, kBattH + 4);
  }
  if (!lit) {
    drawBatteryIcon(COLOR_LED_RED);
    tftPanel.pushRows(kBattX - 2, kBattY - 2,
                      kBattW + kBattNubW + 6, kBattH + 4);
  }
}
