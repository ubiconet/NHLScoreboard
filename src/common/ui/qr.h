#pragma once

#include <Adafruit_GFX.h>

// Renders a QR code (ricmoo/QRCode, version 2, ECC_LOW — ~22-char URLs fit)
// pointing at `text` onto any GFX target, with a white quiet-zone border.
// Used by the boot status page (canvas) and the AP provisioning screen
// (direct panel draw). Returns the module count (25 for version 2) so
// callers can position captions relative to the code.
int drawQr(Adafruit_GFX& target, const char* text, int left, int top,
           int scale, int borderPad);
