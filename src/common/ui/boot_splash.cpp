#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "config.h"
#include "common/hal/tft_panel.h"
#include "common/ui/boot_splash.h"
#include "common/ui/gfx.h"

void renderBootSplash(int logoWidth, int logoHeight, const uint16_t* logoPixels) {
  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillScreen(COLOR_BG);

  int x = (320 - logoWidth) / 2;
  int y = (240 - logoHeight) / 2;
  for (int py = 0; py < logoHeight; py++) {
    for (int px = 0; px < logoWidth; px++) {
      uint16_t color = pgm_read_word(logoPixels + (py * logoWidth) + px);
      canvas.drawPixel(x + px, y + py, color);
    }
  }

  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(1);
  drawCenteredText(canvas, FIRMWARE_VERSION, 160, 226);

  tftPanel.pushFull();
}
