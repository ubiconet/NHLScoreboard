#pragma once

#include <Arduino.h>

// One-time splash shown at boot before Wi-Fi/game state is known. The logo
// artwork is supplied by the sport (a PROGMEM RGB565 bitmap); the firmware
// version from src/config.h is drawn beneath it.
void renderBootSplash(int logoWidth, int logoHeight, const uint16_t* logoPixels);
