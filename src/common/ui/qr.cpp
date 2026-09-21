#include <Arduino.h>

#include <Adafruit_ST7789.h>
#include <qrcode.h>

#include "common/ui/qr.h"

int drawQr(Adafruit_GFX& target, const char* text, int left, int top,
           int scale, int borderPad) {
  uint8_t qrData[qrcode_getBufferSize(2)];
  QRCode qr;
  qrcode_initText(&qr, qrData, 2, ECC_LOW, text);

  target.fillRect(left - borderPad, top - borderPad,
                  qr.size * scale + borderPad * 2,
                  qr.size * scale + borderPad * 2, ST77XX_WHITE);
  for (uint8_t y = 0; y < qr.size; y++) {
    for (uint8_t x = 0; x < qr.size; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        target.fillRect(left + x * scale, top + y * scale, scale, scale,
                        ST77XX_BLACK);
      }
    }
  }
  return qr.size;
}
