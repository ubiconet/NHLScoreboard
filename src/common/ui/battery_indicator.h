#pragma once

#include <Adafruit_GFX.h>

// Small status battery icon (outline + terminal nub, four quarter
// segments) drawn with its top-left at (x, y). Footprint is 22x12 px —
// sized for the top-right corner of the 320x240 canvas, clear of the
// waiting screens' headers and the live screen's guest logo.
// quarters: 0..4 lit segments; <=1 draws red (running low), >=2 gold.
// Callers hide the icon entirely when power::batteryQuarters() is 255
// (unknown — boards without the sense divider).
void drawBatteryIndicator(Adafruit_GFX& g, int x, int y, uint8_t quarters);
