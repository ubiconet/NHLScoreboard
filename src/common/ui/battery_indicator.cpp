#include <Adafruit_GFX.h>

#include "config.h"
#include "common/ui/battery_indicator.h"

void drawBatteryIndicator(Adafruit_GFX& g, int x, int y, uint8_t quarters) {
  if (quarters > 4) return;  // unknown (255) — the caller normally hides
  const uint16_t shell = quarters <= 1 ? COLOR_LED_RED : COLOR_MUTED;
  const uint16_t fill = quarters <= 1 ? COLOR_LED_RED : COLOR_GOLD;

  // Body 19x10 + terminal nub on the right.
  g.drawRoundRect(x, y, 19, 10, 2, shell);
  g.fillRect(x + 19, y + 3, 2, 4, shell);

  // Four quarter segments in the interior (3 px wide, 1 px apart).
  for (int q = 0; q < 4; ++q) {
    int sx = x + 2 + q * 4;
    if (q < quarters) {
      g.fillRect(sx, y + 2, 3, 6, fill);
    } else {
      g.fillRect(sx, y + 2, 3, 6, COLOR_BG);
    }
  }
}
